/*
 * httpd.c -- tiny HTTP/1.0 static-file SERVER (freestanding, ring 3).
 * ==================================================================
 *
 * A minimal "host a directory over HTTP" server for AutomationOS userspace.
 * Completely freestanding: NO libc, NO stdio, NO malloc, NO standard headers --
 * every I/O operation is an inline `syscall`, every buffer is a fixed-size
 * static array, and all string/number handling is done by the small static
 * helpers below.  The syscall ABI, inline-syscall macro, argv handling and the
 * print/itoa helpers are copied VERBATIM from the model net tools
 * (userspace/apps/nc/nc.c, ping/ping.c, wget/wget.c, netinfo, netscan) plus the
 * file-read pattern from userspace/apps/cut/cut.c -- nothing here is invented.
 *
 * Unlike nc/wget/netscan (which are CLIENTS: socket -> connect), httpd is a
 * SERVER.  It models the passive-open path off the kernel socket layer that
 * nc's connect path also uses (kernel/net/socket.c):
 *     1. SYS_SOCKET(SOCK_STREAM)            -> a listening fd
 *     2. SYS_BIND(fd, port)                 -> claim 0.0.0.0:port   (port is the
 *                                              bare port number, host order)
 *     3. SYS_LISTEN(fd, backlog)            -> mark it passive (TCP_LISTEN)
 *     4. accept loop: SYS_SOCK_POLL + SYS_ACCEPT(fd) until a child fd appears
 *        (SYS_ACCEPT is NON-BLOCKING -- it returns SOCK_EAGAIN (-11) when no
 *         connection is pending, so we pump the NIC and yield, never block).
 *     5. per connection: bounded recv of the request, parse "GET <path> ...",
 *        open+read the file, send "HTTP/1.0 200 OK" + Content-Length + bytes
 *        (or a 404 / 400 / 405 error page), then SYS_CLOSE_SK -- one request
 *        per connection (HTTP/1.0, "Connection: close").
 *
 * DNS: NONE.  A server never resolves a name, so this tool does NOT link
 * userspace/lib/net/dns.c and does NOT need /tmp/dns.o (it links exactly like
 * netinfo: crt0.o + httpd.o only).
 *
 * BOUNDEDNESS (never hang): every blocking-ish wait is iteration-capped and
 * interleaved with SYS_YIELD so a quiet client or a silent NIC can NEVER wedge
 * the server.  The request recv has a finite spin budget; the accept loop runs
 * for a bounded number of connections then returns; each connection is closed
 * after exactly one request.  The tool always returns control to the OS.
 *
 * Usage (linked with crt0.o -> int main(int argc, char **argv)):
 *     httpd                     serve ./ ... default: port 8080, dir "/usr/src"
 *     httpd PORT                serve "/usr/src" on PORT
 *     httpd PORT DIR            serve DIR on PORT
 *     httpd -t                  API self-test (bind/listen/accept on a port,
 *                               no client required) -> "HTTPD SELFTEST: ..."
 *
 * Examples:
 *     httpd                     # http://10.0.2.15:8080/  -> /usr/src/index.html
 *     httpd 80 /                # serve the whole FS root on the default HTTP port
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable;
 *        identical CF/LD to netinfo -- NO dns.o):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/httpd/httpd.c -o /tmp/httpd.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/crt0.o /tmp/httpd.o -o /tmp/httpd.elf
 *   objdump -d /tmp/httpd.elf | grep fs:0x28   # MUST be empty (no stack canary)
 */

/* ---- syscall numbers (per AutomationOS ABI -- see kernel/include/syscall.h) */
#define SYS_EXIT        0
#define SYS_READ        2    /* read(fd, buf, len)  -- FILE read              */
#define SYS_WRITE       3    /* write(fd, buf, len)           fd1 = stdout    */
#define SYS_OPEN        4    /* open(path, flags, mode) -> fd  -- FILE open    */
#define SYS_CLOSE       5    /* close(fd)               -- FILE close          */
#define SYS_SLEEP       9    /* sleep(ms) -- real blocking ms sleep            */
#define SYS_YIELD       15   /* cooperative yield                             */
#define SYS_SOCKET      51   /* socket(SOCK_STREAM) -> fd                     */
#define SYS_SEND        53   /* send(fd, buf, len) -> bytes                   */
#define SYS_RECV        54   /* recv(fd, buf, len) -> bytes/0(closed)/-11     */
#define SYS_CLOSE_SK    55   /* close(socket fd) -> 0  (distinct from SYS_CLOSE)*/
#define SYS_SOCK_POLL   58   /* pump the NIC RX/timers; call before each accept/recv */
#define SYS_BIND        76   /* bind(fd, port) -> 0/neg   (port = bare number) */
#define SYS_LISTEN      77   /* listen(fd, backlog) -> 0/neg                  */
#define SYS_ACCEPT      78   /* accept(fd) -> child fd / SOCK_EAGAIN(-11)     */

/* ---- socket type ------------------------------------------------------- */
#define SOCK_STREAM     1

/* ---- file open flags (mirror userspace/libc/syscall.h) ------------------ */
#define O_RDONLY        0x0000

/* ---- well-known fd ------------------------------------------------------ */
#define FD_STDOUT       1

/*
 * Socket return codes (mirror kernel/include/socket.h). The server-path calls
 * return one of these directly:
 *   SOCK_OK     ( 0  ) -> success
 *   SOCK_EAGAIN (-11) -> would block / nothing pending (accept/recv)
 *   SOCK_EBADF  (-9 ) -> bad descriptor
 *   SOCK_EINVAL (-22) -> bad argument / wrong state
 */
#define SOCK_OK          0
#define SOCK_EBADF     (-9)
#define SOCK_EAGAIN    (-11)
#define SOCK_EINVAL    (-22)

/* ---- defaults ---------------------------------------------------------- */
#define DEFAULT_PORT     8080
#define DEFAULT_DIR      "/usr/src"
#define LISTEN_BACKLOG   8
#define KPATH_MAX        1024

/*
 * Accept-loop bound: total number of connections this server will handle before
 * returning to the OS.  Large enough to be useful, finite so the process always
 * terminates (it is a foreground CLI tool, not a daemon).  Each connection is
 * one accept + one request + one close.
 */
#define MAX_CONNS        100000

/*
 * Per-accept spin budget.  SYS_ACCEPT is non-blocking (returns SOCK_EAGAIN when
 * nothing is pending), so between connections we pump the NIC and yield for at
 * most this many iterations before giving up the wait and returning.  This is
 * the bound that guarantees a quiet network never wedges the server.
 */
#define ACCEPT_SPIN_MAX  4000000

/*
 * Per-request recv spin budget.  After a connection is accepted we read the
 * request headers with a bounded number of SYS_SOCK_POLL + SYS_RECV spins.  A
 * client that connects but never sends is dropped after this budget so it can
 * never stall the accept loop.
 */
#define RECV_SPIN_MAX    400000

/* ===================================================================== */
/* Raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8) -- verbatim from nc.c    */
/* ===================================================================== */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* ===================================================================== */
/* Tiny freestanding helpers (copied from nc.c / netscan.c / cut.c)      */
/* ===================================================================== */

static unsigned int n_strlen(const char *s)
{
    unsigned int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int n_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Append src to dst (bounded by cap incl. NUL); returns new length. */
static unsigned int n_strlcat(char *dst, const char *src, unsigned int cap)
{
    unsigned int d = n_strlen(dst);
    unsigned int i = 0;
    if (cap == 0) return d;
    while (src[i] && d + 1 < cap) { dst[d++] = src[i++]; }
    dst[d] = '\0';
    return d;
}

/*
 * Parse a base-10 port string.  Returns the value in 1..65535, or -1 on any
 * malformed / out-of-range input (leading sign, non-digits, overflow, empty).
 * Verbatim from nc.c / netscan.c.
 */
static long n_parse_port(const char *s)
{
    if (!s || !*s) return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;          /* overflow / out of range */
    }
    if (v <= 0) return -1;
    return v;
}

/* ---- stdout (fd1) print helpers (serial / console) -------------------- */

static void out_write(const char *buf, unsigned int len)
{
    if (len == 0) return;
    sc(SYS_WRITE, FD_STDOUT, (long)buf, (long)len, 0, 0);
}

static void out_puts(const char *s)
{
    out_write(s, n_strlen(s));
}

/* Print an unsigned decimal number to fd1. */
static void out_unum(unsigned long v)
{
    char b[24];
    int i = 0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    char rev[24];
    int j = 0;
    while (i > 0) rev[j++] = b[--i];
    out_write(rev, (unsigned int)j);
}

/* Print a signed decimal number to fd1. */
static void out_num(long v)
{
    if (v < 0) { out_puts("-"); out_unum((unsigned long)(-v)); }
    else        out_unum((unsigned long)v);
}

/* ===================================================================== */
/* Static buffers (no big stack frames in -mno-red-zone code)            */
/* ===================================================================== */
#define REQ_BUF_SZ   4096
#define HDR_BUF_SZ   512
#define FILE_BUF_SZ  (256 * 1024)   /* max served file size in one shot       */

static char g_req[REQ_BUF_SZ];      /* raw request bytes                      */
static char g_path[KPATH_MAX];      /* resolved filesystem path               */
static char g_hdr[HDR_BUF_SZ];      /* response header                        */
static char g_file[FILE_BUF_SZ];    /* file contents                          */

/* Built-in directory-index / fallback body (served when no index file). */
static const char g_default_index[] =
    "<!doctype html><html><head><title>AutomationOS httpd</title></head>"
    "<body><h1>AutomationOS httpd</h1>"
    "<p>It works. This is the built-in default page.</p>"
    "</body></html>\n";

/* ===================================================================== */
/* Bounded socket send: loop over short SYS_SEND results (verbatim model  */
/* from nc.c send_all). Returns total sent (>=0) or the last negative err. */
/* ===================================================================== */
static long send_all(long fd, const char *buf, long len)
{
    long off = 0;
    int guard = 0;
    while (off < len) {
        long n = sc(SYS_SEND, fd, (long)(buf + off), len - off, 0, 0);
        if (n > 0) {
            off += n;
            guard = 0;
            continue;
        }
        if (n == SOCK_EAGAIN) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
            if (++guard > 100000) break;     /* socket wedged -> give up */
            continue;
        }
        return n;                            /* hard error (e.g. reset) */
    }
    return off;
}

/* ===================================================================== */
/* HTTP helpers                                                          */
/* ===================================================================== */

/*
 * URL-decode a path component in place ("%20" -> ' ', '+' is left as-is since
 * '+' is only space in query strings, not paths).  Bounded by the NUL.
 */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; ) {
        if (r[0] == '%' && r[1] && r[2]) {
            int hi = hexval(r[1]), lo = hexval(r[2]);
            if (hi >= 0 && lo >= 0) {
                int ch = (hi << 4) | lo;
                if (ch == 0) return -1;  /* reject %00 (NUL injection) */
                *w++ = (char)ch;
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
    return 0;
}

/*
 * Reject paths that try to escape the served directory ("..") or contain NUL-ish
 * tricks. Returns 1 if the (already-decoded) path is safe, 0 otherwise.
 * We scan for a ".." path segment (start-of-string or after '/').
 */
static int path_is_safe(const char *p)
{
    for (const char *c = p; *c; c++) {
        /* Reject control characters (0x01..0x1F, 0x7F) in paths */
        if ((unsigned char)*c < 0x20 || (unsigned char)*c == 0x7F)
            return 0;
        if (c[0] == '.' && c[1] == '.') {
            char before = (c == p) ? '/' : c[-1];
            char after  = c[2];
            if (before == '/' && (after == '/' || after == '\0'))
                return 0;                    /* a real ".." segment */
        }
    }
    return 1;
}

/* Guess a Content-Type from the file extension (small, common set). */
static const char *content_type_for(const char *path)
{
    /* find last '.' */
    const char *dot = (void *)0;
    for (const char *c = path; *c; c++) if (*c == '.') dot = c;
    if (!dot) return "application/octet-stream";
    const char *e = dot + 1;
    if (n_streq(e, "html") || n_streq(e, "htm")) return "text/html";
    if (n_streq(e, "txt")  || n_streq(e, "c") ||
        n_streq(e, "h")    || n_streq(e, "md"))  return "text/plain";
    if (n_streq(e, "css"))  return "text/css";
    if (n_streq(e, "js"))   return "application/javascript";
    if (n_streq(e, "json")) return "application/json";
    if (n_streq(e, "png"))  return "image/png";
    if (n_streq(e, "jpg") || n_streq(e, "jpeg")) return "image/jpeg";
    if (n_streq(e, "gif"))  return "image/gif";
    return "application/octet-stream";
}

/*
 * Send an HTTP/1.0 response: status line + a few headers + body.
 * `status` is the numeric code (200/400/404/405), `reason` the text, `ctype`
 * the Content-Type, and (body,body_len) the payload.  All header bytes are
 * built into g_hdr with our own bounded itoa -- no snprintf.
 */
static void send_response(long cfd, int status, const char *reason,
                          const char *ctype, const char *body, long body_len)
{
    /* Build the header into g_hdr by hand (bounded concatenation). */
    g_hdr[0] = '\0';
    n_strlcat(g_hdr, "HTTP/1.0 ", HDR_BUF_SZ);
    {   /* status code */
        char num[8]; int i = 0; int v = status;
        char rev[8]; int j = 0;
        do { num[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
        while (i > 0) rev[j++] = num[--i];
        rev[j] = '\0';
        n_strlcat(g_hdr, rev, HDR_BUF_SZ);
    }
    n_strlcat(g_hdr, " ", HDR_BUF_SZ);
    n_strlcat(g_hdr, reason, HDR_BUF_SZ);
    n_strlcat(g_hdr, "\r\nServer: AutomationOS-httpd/1.0\r\nConnection: close\r\n", HDR_BUF_SZ);
    n_strlcat(g_hdr, "Content-Type: ", HDR_BUF_SZ);
    n_strlcat(g_hdr, ctype, HDR_BUF_SZ);
    n_strlcat(g_hdr, "\r\nContent-Length: ", HDR_BUF_SZ);
    {   /* content length */
        char num[24]; int i = 0; long v = body_len;
        char rev[24]; int j = 0;
        do { num[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
        while (i > 0) rev[j++] = num[--i];
        rev[j] = '\0';
        n_strlcat(g_hdr, rev, HDR_BUF_SZ);
    }
    n_strlcat(g_hdr, "\r\n\r\n", HDR_BUF_SZ);

    send_all(cfd, g_hdr, (long)n_strlen(g_hdr));
    if (body && body_len > 0)
        send_all(cfd, body, body_len);
}

/* Convenience: send a small text/html error page. */
static void send_error(long cfd, int status, const char *reason)
{
    /* Build a tiny body in g_file (it's free between requests). */
    g_file[0] = '\0';
    n_strlcat(g_file, "<!doctype html><html><body><h1>", FILE_BUF_SZ);
    {   char num[8]; int i = 0; int v = status; char rev[8]; int j = 0;
        do { num[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
        while (i > 0) rev[j++] = num[--i];
        rev[j] = '\0';
        n_strlcat(g_file, rev, FILE_BUF_SZ);
    }
    n_strlcat(g_file, " ", FILE_BUF_SZ);
    n_strlcat(g_file, reason, FILE_BUF_SZ);
    n_strlcat(g_file, "</h1></body></html>\n", FILE_BUF_SZ);
    send_response(cfd, status, reason, "text/html",
                  g_file, (long)n_strlen(g_file));
}

/* ===================================================================== */
/* File read: open(path, O_RDONLY) -> read into g_file (model: cut.c)    */
/* Returns bytes read (>=0), or a negative open() error code.            */
/* ===================================================================== */
static long read_file(const char *path, long cap)
{
    long fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return fd;                   /* ENOENT etc. */

    long total = 0;
    int guard = 0;
    while (total < cap) {
        long n = sc(SYS_READ, fd, (long)(g_file + total), cap - total, 0, 0);
        if (n > 0) { total += n; guard = 0; continue; }
        if (n == 0) break;                   /* EOF */
        /* transient/short -> a few bounded retries, then stop */
        if (++guard > 1000) break;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return total;
}

/* ===================================================================== */
/* Request handling                                                      */
/* ===================================================================== */

/*
 * Read the request from the connection (bounded), parse the request line, map
 * the path under `root`, read+serve the file (or default index / 404 / error),
 * then return.  The caller closes the socket.  Returns 0 always (per-request
 * errors are sent as HTTP responses, not propagated).
 */
static int handle_conn(long cfd, const char *root)
{
    /* --- bounded recv of the request (need at least the request line) --- */
    long rlen = 0;
    for (long it = 0; it < RECV_SPIN_MAX && rlen < REQ_BUF_SZ - 1; it++) {
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
        long n = sc(SYS_RECV, cfd, (long)(g_req + rlen), REQ_BUF_SZ - 1 - rlen, 0, 0);
        if (n > 0) {
            rlen += n;
            g_req[rlen] = '\0';
            /* Stop once we have the end of the request headers (\r\n\r\n) or
             * at least a full request line (\n) -- HTTP/1.0 GET has no body. */
            for (long k = 0; k < rlen; k++) {       /* k<rlen: also check the last byte */
                if (g_req[k] == '\n') { it = RECV_SPIN_MAX; break; }
            }
        } else if (n == 0) {
            break;                           /* peer closed before sending */
        } else if (n == SOCK_EAGAIN) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
        } else {
            break;                           /* hard socket error */
        }
    }

    if (rlen <= 0) {
        /* Nothing arrived -> nothing to answer. */
        return 0;
    }
    g_req[rlen] = '\0';

    /* --- parse the request line: "METHOD SP PATH SP VERSION" --- */
    /* method */
    char *p = g_req;
    char *method = p;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;
    if (*p != ' ') { send_error(cfd, 400, "Bad Request"); return 0; }
    *p++ = '\0';

    /* path */
    char *url = p;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;
    *p = '\0';                               /* terminate the URL token */

    /* Only GET is supported. */
    if (!n_streq(method, "GET")) {
        send_error(cfd, 405, "Method Not Allowed");
        return 0;
    }

    /* Strip a query string. */
    for (char *q = url; *q; q++) { if (*q == '?') { *q = '\0'; break; } }

    /* Decode %xx escapes (reject %00), then reject directory traversal. */
    if (url_decode(url) < 0 || url[0] != '/' || !path_is_safe(url)) {
        send_error(cfd, 400, "Bad Request");
        return 0;
    }

    /* Map "/" to the default index file under root. */
    int is_root = (url[0] == '/' && url[1] == '\0');

    /* Build the filesystem path: root + url  (root has no trailing slash). */
    g_path[0] = '\0';
    n_strlcat(g_path, root, KPATH_MAX);
    /* avoid a double slash if root == "/" */
    if (!(root[0] == '/' && root[1] == '\0'))
        n_strlcat(g_path, url, KPATH_MAX);
    else
        n_strlcat(g_path, url + 0, KPATH_MAX);   /* root "/" + "/..." */
    if (is_root)
        n_strlcat(g_path, "/index.html", KPATH_MAX);

    /* Collapse any accidental "//" left by root=="/" + url=="/...". */
    {
        char *w = g_path;
        for (char *r = g_path; *r; ) {
            if (r[0] == '/' && r[1] == '/') { r++; continue; }
            *w++ = *r++;
        }
        *w = '\0';
    }

    long flen = read_file(g_path, FILE_BUF_SZ);

    if (flen < 0) {
        /* On "/" with no index.html, fall back to the built-in default page. */
        if (is_root) {
            send_response(cfd, 200, "OK", "text/html",
                          g_default_index, (long)n_strlen(g_default_index));
            out_puts("httpd: 200 ");
            out_puts(url);
            out_puts(" (default index)\n");
            return 0;
        }
        send_error(cfd, 404, "Not Found");
        out_puts("httpd: 404 ");
        out_puts(url);
        out_puts("\n");
        return 0;
    }

    send_response(cfd, 200, "OK", content_type_for(g_path), g_file, flen);
    out_puts("httpd: 200 ");
    out_puts(url);
    out_puts(" (");
    out_num(flen);
    out_puts(" bytes)\n");
    return 0;
}

/* ===================================================================== */
/* Server driver                                                         */
/* ===================================================================== */

/*
 * Open a listening socket, bind to `port`, listen, then run the bounded accept
 * loop, handling one request per accepted connection.  Returns a process exit
 * code (0 == clean shutdown after the connection cap, non-zero == setup error).
 */
static int do_serve(long port, const char *root)
{
    long lfd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (lfd < 0) {
        out_puts("httpd: socket() failed rc=");
        out_num(lfd);
        out_puts("\n");
        return 3;
    }

    long br = sc(SYS_BIND, lfd, port, 0, 0, 0);
    if (br < 0) {
        out_puts("httpd: bind(:");
        out_num(port);
        out_puts(") failed rc=");
        out_num(br);
        out_puts("\n");
        sc(SYS_CLOSE_SK, lfd, 0, 0, 0, 0);
        return 4;
    }

    long lr = sc(SYS_LISTEN, lfd, LISTEN_BACKLOG, 0, 0, 0);
    if (lr < 0) {
        out_puts("httpd: listen() failed rc=");
        out_num(lr);
        out_puts("\n");
        sc(SYS_CLOSE_SK, lfd, 0, 0, 0, 0);
        return 5;
    }

    out_puts("httpd: serving '");
    out_puts(root);
    out_puts("' on 0.0.0.0:");
    out_num(port);
    out_puts(" (HTTP/1.0)\n");

    long served = 0;
    for (long conn = 0; conn < MAX_CONNS; conn++) {
        /* --- bounded accept: poll the NIC, try accept, yield on EAGAIN --- */
        long cfd = SOCK_EAGAIN;
        for (long spin = 0; spin < ACCEPT_SPIN_MAX; spin++) {
            sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
            cfd = sc(SYS_ACCEPT, lfd, 0, 0, 0, 0);
            if (cfd >= 0) break;             /* got a connection */
            if (cfd == SOCK_EAGAIN) {        /* nothing pending yet */
                sc(SYS_YIELD, 0, 0, 0, 0, 0);
                continue;
            }
            /* hard accept error -> stop the server */
            break;
        }

        if (cfd < 0) {
            /* No connection within the spin budget (quiet network) OR a hard
             * error -> stop. The server is bounded; it returns to the OS. */
            if (cfd != SOCK_EAGAIN) {
                out_puts("httpd: accept() error rc=");
                out_num(cfd);
                out_puts("\n");
            } else {
                out_puts("httpd: idle timeout reached -- shutting down\n");
            }
            break;
        }

        handle_conn(cfd, root);
        sc(SYS_CLOSE_SK, cfd, 0, 0, 0, 0);   /* HTTP/1.0: close after each */
        served++;
    }

    out_puts("httpd: closed (");
    out_num(served);
    out_puts(" requests served)\n");

    sc(SYS_CLOSE_SK, lfd, 0, 0, 0, 0);
    return 0;
}

/* ===================================================================== */
/* Self-test (httpd -t)                                                  */
/* ===================================================================== */

/*
 * Exercise the SERVER socket API without needing a client (mirrors nc.c's
 * self_test, but for the passive path):
 *
 *   1. SYS_SOCKET must return a non-negative fd.
 *   2. SYS_BIND to a high test port must succeed (SOCK_OK).
 *   3. SYS_LISTEN must succeed (socket enters TCP_LISTEN).
 *   4. A SINGLE, bounded SYS_ACCEPT must RETURN -- with nothing connecting it
 *      returns SOCK_EAGAIN (would-block), which is the EXPECTED, correct result
 *      and proves the call is non-blocking and the server can never hang.
 *   5. SYS_CLOSE_SK must succeed.
 *
 * Prints "HTTPD SELFTEST: PASS" if the API behaves sanely, otherwise
 * "HTTPD SELFTEST: FAIL <why>".  Returns 0 on PASS, non-zero on FAIL.
 */
#define SELFTEST_PORT 18080
static int self_test(void)
{
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) {
        out_puts("HTTPD SELFTEST: FAIL socket rc=");
        out_num(fd);
        out_puts("\n");
        return 1;
    }

    long br = sc(SYS_BIND, fd, SELFTEST_PORT, 0, 0, 0);
    if (br != SOCK_OK) {
        out_puts("HTTPD SELFTEST: FAIL bind rc=");
        out_num(br);
        out_puts("\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 1;
    }

    long lr = sc(SYS_LISTEN, fd, LISTEN_BACKLOG, 0, 0, 0);
    if (lr != SOCK_OK) {
        out_puts("HTTPD SELFTEST: FAIL listen rc=");
        out_num(lr);
        out_puts("\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 1;
    }

    /* Single bounded accept: nothing is connecting, so SOCK_EAGAIN is the
     * correct outcome.  The point is that the call RETURNS (does not block). */
    sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
    long ar = sc(SYS_ACCEPT, fd, 0, 0, 0, 0);
    if (ar != SOCK_EAGAIN && ar < 0) {
        out_puts("HTTPD SELFTEST: FAIL accept rc=");
        out_num(ar);
        out_puts(" (expected EAGAIN -11)\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 1;
    }

    long clr = sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    if (clr < 0) {
        out_puts("HTTPD SELFTEST: FAIL close rc=");
        out_num(clr);
        out_puts("\n");
        return 1;
    }

    out_puts("HTTPD SELFTEST: PASS (socket fd=");
    out_num(fd);
    out_puts(", bind :");
    out_num(SELFTEST_PORT);
    out_puts(" ok, listen ok, accept rc=");
    out_num(ar);
    out_puts(" [EAGAIN expected], close ok)\n");
    return 0;
}

/* ===================================================================== */
/* Entry point (crt0 supplies _start -> main(argc, argv))                */
/* ===================================================================== */

int main(int argc, char **argv)
{
    /* "httpd -t" -> run the API self-test (no client required). */
    if (argc >= 2 && n_streq(argv[1], "-t"))
        return self_test();

    long port = DEFAULT_PORT;
    const char *root = DEFAULT_DIR;

    if (argc >= 2) {
        long p = n_parse_port(argv[1]);
        if (p < 0) {
            out_puts("httpd: invalid port '");
            out_puts(argv[1]);
            out_puts("'\n");
            out_puts("usage: httpd [PORT] [DIR]   (default 8080 /usr/src) | httpd -t\n");
            return 1;
        }
        port = p;
    }
    if (argc >= 3)
        root = argv[2];

    return do_serve(port, root);
}
