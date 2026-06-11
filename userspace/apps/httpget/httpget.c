/*
 * httpget.c -- minimal HTTP/1.0 client (freestanding, ring 3).
 * ============================================================
 *
 * A tiny HTTP/1.0 GET client for AutomationOS userspace.  Completely
 * freestanding: NO libc, NO stdio, NO malloc, NO standard headers -- every
 * I/O operation is an inline `syscall` (the EXACT sc() macro + syscall numbers
 * used by userspace/apps/nc/nc.c), every buffer is a fixed-size static array,
 * and all string handling is done by the small static helpers below.  The
 * TCP flow (socket -> connect -> send GET -> poll/recv until close) is modeled
 * directly on nc.c's relay loop and wget.c's status-line/header/body split.
 *
 * Argv handling: linked with userspace/crt0.asm, which reads argc/argv off the
 * kernel-provided stack and calls int main(int argc, char **argv) -- exactly
 * like nc / ping / wget.
 *
 * The only external dependency is the freestanding DNS resolver (same one nc.c
 * links against):
 *     int dns_resolve(const char *host, unsigned int *out_ip);   (0 == ok)
 * A dotted-quad "<ip>" ("a.b.c.d") is parsed by dns_resolve with NO network
 * activity at all and yields a HOST-byte-order address (0x0A000202 == 10.0.2.2),
 * exactly what SYS_CONNECT expects.  (A bare name would trigger a bounded UDP
 * DNS query, but the documented usage passes a dotted-quad IP.)
 *
 * Usage (crt0 -> main(argc, argv)):
 *     httpget <ip> <path> [port]      GET <path> from <ip>:port (default 80)
 *     httpget                         (argc <= 1) -> built-in API self-test
 *
 * Example (reachable from QEMU user-net):
 *     httpget 10.0.2.2 /              # the slirp gateway / host-side server
 *
 * Behavior: opens a TCP socket to <ip>:port, sends exactly
 *     "GET <path> HTTP/1.0\r\nHost: <ip>\r\n\r\n"
 * reads the whole response into a bounded static buffer, prints the status line
 * + all response headers, then the first ~2KB of the body.  Every connect/recv
 * is bounded by the kernel's own connect timeout + a finite poll iteration cap
 * plus SYS_YIELD, so a silent peer can never hang the tool.
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/httpget/httpget.c -o /tmp/httpget.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/crt0.o /tmp/httpget.o /tmp/dns.o -o /tmp/httpget.elf
 *   objdump -d /tmp/httpget.o | grep fs:0x28   # MUST be empty (no stack canary)
 */

#include "../../lib/net/dns.h"

/* ---- syscall numbers (per AutomationOS ABI -- identical to nc.c) -------- */
#define SYS_READ        2    /* read(fd, buf, len)            fd0 = stdin    */
#define SYS_WRITE       3    /* write(fd, buf, len)           fd1 = stdout   */
#define SYS_YIELD       15   /* cooperative yield                            */
#define SYS_SOCKET      51   /* socket(SOCK_STREAM) -> fd                    */
#define SYS_CONNECT     52   /* connect(fd, ip_host_order, port) -> 0/neg    */
#define SYS_SEND        53   /* send(fd, buf, len) -> bytes                  */
#define SYS_RECV        54   /* recv(fd, buf, len) -> bytes/0(closed)/-11    */
#define SYS_CLOSE_SK    55   /* close(fd) -> 0                               */
#define SYS_SOCK_POLL   58   /* pump the NIC RX/timers; call before each RECV */

/* ---- socket type ------------------------------------------------------- */
#define SOCK_STREAM     1

/* ---- well-known fds ---------------------------------------------------- */
#define FD_STDOUT       1

/* ---- error code returned by RECV when no data yet (would-block) -------- */
#define EAGAIN_NEG      (-11)

/* ---- self-test reference: 10.0.2.2 -> host-order 0x0A000202 ------------ */
#define GATEWAY_IP      0x0A000202u
#define DISCARD_PORT    9
#define HTTP_PORT       80

/*
 * Iteration cap for the RECV drain loop.  Each iteration is one SYS_SOCK_POLL
 * + one SYS_RECV (with a SYS_YIELD on a dry read), so this is purely a safety
 * bound to guarantee the tool always returns even if a peer goes silent
 * without closing.  Same magnitude nc.c uses for its relay.
 */
#define RECV_MAX        400000

/* ===================================================================== */
/* Raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8) -- copied verbatim from   */
/* nc.c so the ABI is byte-for-byte identical.                            */
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
/* Tiny freestanding helpers (same style as nc.c)                        */
/* ===================================================================== */

static unsigned int n_strlen(const char *s)
{
    unsigned int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/*
 * Parse a base-10 port string.  Returns the value in 1..65535, or -1 on any
 * malformed / out-of-range input (non-digits, overflow, empty).
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
    /* Best-effort: write once; partial writes are tolerated for diagnostics. */
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

/* Print a host-order IPv4 address as A.B.C.D to fd1. */
static void out_ip(unsigned int ip)
{
    out_unum((ip >> 24) & 0xFFu); out_puts(".");
    out_unum((ip >> 16) & 0xFFu); out_puts(".");
    out_unum((ip >>  8) & 0xFFu); out_puts(".");
    out_unum( ip        & 0xFFu);
}

/* Translate a negative errno to a short reason string (freestanding). */
static const char *rc_str(long e) {
    if (e >= 0)   return "ok";
    long v = -e;
    switch (v) {
        case  11: return "would block";
        case  12: return "out of memory";
        case  22: return "invalid argument";
        case 104: return "connection reset by peer";
        case 110: return "connection timed out";
        case 111: return "connection refused";
        case 113: return "no route to host";
        default:  return "error";
    }
}

/* ===================================================================== */
/* Buffers (static -> no big stack frames in -mno-red-zone code)         */
/* ===================================================================== */

/* Request line + Host header build buffer. */
#define REQ_BUF_SZ   2304     /* room for "GET <path> HTTP/1.0\r\nHost: <ip>\r\n\r\n" */
static char g_req[REQ_BUF_SZ];

/* Per-recv chunk buffer. */
#define RX_CHUNK_SZ  2048
static char g_rx[RX_CHUNK_SZ];

/*
 * Whole-response accumulation buffer.  Large enough to hold a typical headers
 * block plus well past the first 2KB of body we print.  Bounded; extra bytes
 * beyond capacity are drained from the socket but not stored.
 */
#define RESP_BUF_SZ  65536
static char g_resp[RESP_BUF_SZ];

/* How many body bytes to print after the headers. */
#define BODY_PRINT   2048

/* ===================================================================== */
/* send_all -- loop over short SYS_SEND results (copied from nc.c)        */
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
        if (n == EAGAIN_NEG) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
            if (++guard > 100000) break;    /* socket wedged -> give up */
            continue;
        }
        /* Hard error (e.g. connection reset). */
        return n;
    }
    return off;
}

/* ===================================================================== */
/* Build the fixed HTTP/1.0 request into g_req.                          */
/* Produces: "GET <path> HTTP/1.0\r\nHost: <host>\r\n\r\n"               */
/* Returns the request length, or -1 if it would not fit.               */
/* ===================================================================== */
static long build_request(const char *path, const char *host)
{
    int pos = 0;
    const char *const m = "GET ";
    const char *const v = " HTTP/1.0\r\nHost: ";
    const char *const tail = "\r\n\r\n";

    unsigned int ml = n_strlen(m);
    unsigned int pl = n_strlen(path);
    unsigned int vl = n_strlen(v);
    unsigned int hl = n_strlen(host);
    unsigned int tl = n_strlen(tail);

    /* Bounds check: everything + NUL must fit in g_req. */
    if (ml + pl + vl + hl + tl + 1 > (unsigned int)REQ_BUF_SZ) return -1;

    for (unsigned int i = 0; i < ml; i++) g_req[pos++] = m[i];
    for (unsigned int i = 0; i < pl; i++) g_req[pos++] = path[i];
    for (unsigned int i = 0; i < vl; i++) g_req[pos++] = v[i];
    for (unsigned int i = 0; i < hl; i++) g_req[pos++] = host[i];
    for (unsigned int i = 0; i < tl; i++) g_req[pos++] = tail[i];
    g_req[pos] = '\0';
    return pos;
}

/* ===================================================================== */
/* Drain the socket into g_resp until the peer closes (RECV == 0) or the */
/* iteration cap is hit.  Returns total bytes STORED (<= RESP_BUF_SZ);   */
/* bytes past capacity are recv'd and discarded so the peer can finish.  */
/* Modeled on nc.c's relay() loop (SYS_SOCK_POLL + SYS_RECV + SYS_YIELD).*/
/* ===================================================================== */
static long drain_response(long fd)
{
    long stored = 0;

    for (long it = 0; it < RECV_MAX; it++) {
        /* Always pump the NIC before attempting a receive. */
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);

        long rn = sc(SYS_RECV, fd, (long)g_rx, RX_CHUNK_SZ, 0, 0);
        if (rn > 0) {
            /* Copy as much as fits into g_resp; discard the rest. */
            long room = (long)RESP_BUF_SZ - stored;
            long cpy = (rn < room) ? rn : room;
            for (long i = 0; i < cpy; i++) g_resp[stored + i] = g_rx[i];
            stored += cpy;
        } else if (rn == 0) {
            /* Peer closed the connection -> we are done. */
            break;
        } else if (rn == EAGAIN_NEG) {
            /* No data yet; yield so the NIC/timers can make progress. */
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
        } else {
            /* Hard socket error -> stop draining. */
            break;
        }
    }

    return stored;
}

/* ===================================================================== */
/* Print the status line + all headers, then the first ~2KB of body.     */
/* `n` is the number of stored response bytes in g_resp.                 */
/* ===================================================================== */
static void print_response(long n)
{
    if (n <= 0) {
        out_puts("httpget: empty response\n");
        return;
    }

    /*
     * Find the header/body boundary: the first "\r\n\r\n".  Everything up to
     * and including the first "\r\n" of that pair is the status line + headers;
     * the body starts just after the blank line.  If no boundary is found the
     * whole thing is treated as headers (e.g. a truncated HTTP/1.0 reply).
     */
    long hdr_end = -1;      /* index of the first byte of "\r\n\r\n"          */
    for (long i = 0; i + 3 < n; i++) {
        if (g_resp[i] == '\r' && g_resp[i + 1] == '\n' &&
            g_resp[i + 2] == '\r' && g_resp[i + 3] == '\n') {
            hdr_end = i;
            break;
        }
    }

    out_puts("---- status + headers ----\n");
    if (hdr_end >= 0) {
        /* Print status line + headers up to (and including) the final CRLF. */
        out_write(g_resp, (unsigned int)(hdr_end + 2));
        out_puts("\n---- body (first ");

        long body_start = hdr_end + 4;             /* skip "\r\n\r\n" */
        long body_len = n - body_start;
        if (body_len < 0) body_len = 0;
        long show = (body_len < BODY_PRINT) ? body_len : BODY_PRINT;

        out_unum((unsigned long)show);
        out_puts(" of ");
        out_num(body_len);
        out_puts(" bytes) ----\n");
        if (show > 0) out_write(g_resp + body_start, (unsigned int)show);
        out_puts("\n");
    } else {
        /* No blank-line boundary: dump what we have as headers, no body. */
        out_write(g_resp, (unsigned int)n);
        out_puts("\n(no header/body boundary found)\n");
    }
}

/* ===================================================================== */
/* Connected-mode driver                                                 */
/* ===================================================================== */
static int do_get(const char *ip_str, const char *path, long port)
{
    /* Resolve the dotted-quad <ip> with NO network (dns_resolve fast path). */
    unsigned int ip = 0;
    int dr = dns_resolve(ip_str, &ip);
    if (dr != 0) {
        out_puts("httpget: cannot resolve '");
        out_puts(ip_str);
        out_puts("' (dns rc=");
        out_num(dr);
        out_puts(")\n");
        return 2;
    }

    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) {
        out_puts("httpget: cannot create socket: ");
        out_puts(rc_str(fd));
        out_puts(" (rc=");
        out_num(fd);
        out_puts(")\n");
        return 3;
    }

    out_puts("httpget: connecting to ");
    out_ip(ip);
    out_puts(":");
    out_num(port);
    out_puts(" ...\n");

    /* Single bounded connect; the kernel's tcp_connect has its own timeout. */
    long cr = sc(SYS_CONNECT, fd, (long)ip, port, 0, 0);
    if (cr < 0) {
        out_puts("httpget: cannot connect to ");
        out_puts(ip_str);
        out_puts(":");
        out_num(port);
        out_puts(": ");
        out_puts(rc_str(cr));
        out_puts(" (rc=");
        out_num(cr);
        out_puts(")\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 4;
    }
    out_puts("httpget: connected\n");

    /* Build + send the fixed HTTP/1.0 GET request. */
    long want = build_request(path, ip_str);
    if (want < 0) {
        out_puts("httpget: request too long for ");
        out_puts(path);
        out_puts("\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 5;
    }
    out_puts("httpget: GET ");
    out_puts(path);
    out_puts(" HTTP/1.0\n");

    long s = send_all(fd, g_req, want);
    if (s < 0) {
        out_puts("httpget: send to ");
        out_puts(ip_str);
        out_puts(" failed: ");
        out_puts(rc_str(s));
        out_puts(" (rc=");
        out_num(s);
        out_puts(")\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 6;
    }

    /* Read the whole response (bounded) and print status+headers+body. */
    long got = drain_response(fd);
    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);

    print_response(got);

    out_puts("\nhttpget: done (");
    out_num(got);
    out_puts(" bytes received)\n");
    return (got > 0) ? 0 : 7;
}

/* ===================================================================== */
/* Self-test (argc <= 1) -- no listening server required.                */
/*                                                                       */
/*   1. dns_resolve("10.0.2.2") must yield host-order 0x0A000202 with NO  */
/*      network activity (pure dotted-quad parse).                       */
/*   2. build_request() must produce the exact expected bytes.           */
/*   3. SYS_SOCKET must return a non-negative fd, and a SINGLE bounded    */
/*      SYS_CONNECT to 10.0.2.2:9 (discard) must RETURN (not hang);       */
/*      success is not required since nothing listens there.             */
/*   4. SYS_CLOSE_SK must succeed.                                       */
/*                                                                       */
/* Prints "HTTPGET SELFTEST: PASS" / "... FAIL <why>"; returns 0 / non-0. */
/* ===================================================================== */
static int n_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int self_test(void)
{
    /* --- 1. DNS dotted-quad parse (no network) --- */
    unsigned int ip = 0xDEADBEEFu;
    int dr = dns_resolve("10.0.2.2", &ip);
    if (dr != 0 || ip != GATEWAY_IP) {
        out_puts("HTTPGET SELFTEST: FAIL dns (rc=");
        out_num(dr);
        out_puts(")\n");
        return 1;
    }

    /* --- 2. request builder produces the exact bytes --- */
    long rl = build_request("/", "10.0.2.2");
    const char *expect = "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    if (rl < 0 || !n_streq(g_req, expect)) {
        out_puts("HTTPGET SELFTEST: FAIL request-build\n");
        return 1;
    }

    /* --- 3. socket() + single bounded connect (expected to fail/return) --- */
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) {
        out_puts("HTTPGET SELFTEST: FAIL socket rc=");
        out_num(fd);
        out_puts("\n");
        return 1;
    }
    long cr = sc(SYS_CONNECT, fd, (long)GATEWAY_IP, DISCARD_PORT, 0, 0);
    (void)cr;   /* nothing listens on :9; the point is the call returns */

    /* --- 4. close() --- */
    long clr = sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    if (clr < 0) {
        out_puts("HTTPGET SELFTEST: FAIL close rc=");
        out_num(clr);
        out_puts("\n");
        return 1;
    }

    out_puts("HTTPGET SELFTEST: PASS (dns ok, request ok, socket fd=");
    out_num(fd);
    out_puts(", connect rc=");
    out_num(cr);
    out_puts(")\n");
    return 0;
}

/* ===================================================================== */
/* Entry point -- crt0 turns the return value into SYS_EXIT.             */
/* ===================================================================== */
int main(int argc, char **argv)
{
    /* No args (or just the program name) -> run the API self-test. */
    if (argc <= 1)
        return self_test();

    /* httpget <ip> <path> [port] */
    if (argc < 3) {
        out_puts("usage: httpget <ip> <path> [port]\n");
        return 1;
    }

    const char *ip_str = argv[1];
    const char *path   = argv[2];

    long port = HTTP_PORT;
    if (argc >= 4) {
        port = n_parse_port(argv[3]);
        if (port < 0) {
            out_puts("httpget: invalid port '");
            out_puts(argv[3]);
            out_puts("'\n");
            return 1;
        }
    }

    return do_get(ip_str, path, port);
}
