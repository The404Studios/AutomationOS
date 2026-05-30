/*
 * wget.c -- minimal freestanding HTTP/HTTPS downloader for AutomationOS (ring 3).
 * ================================================================================
 *
 * FREESTANDING userspace: NO libc, NO stdio, NO malloc, NO standard headers.
 * Everything is inline syscalls + tiny self-contained helpers + fixed static
 * buffers. Linked with crt0 (provides _start -> main) and the net libraries
 * dns (dns_resolve), http (http_get / https_get), compiled separately.
 *
 * Usage:
 *   wget URL                   fetch URL, print the response body to fd 1 (stdout)
 *   wget -O FILE URL           fetch URL, write the body to FILE instead of stdout
 *   wget -k URL                fetch https:// URL silencing the CA-not-validated warning
 *   wget --insecure URL        same as -k
 *   wget -k -O FILE URL        combine -k and -O (in either order before URL)
 *   wget                       (argc<=1) run the built-in DETERMINISTIC self-test
 *                              (no network), printing "WGET SELFTEST: PASS" or
 *                              "WGET SELFTEST: FAIL <why>".
 *
 * URL forms:
 *   http://HOST[:PORT]/PATH    plain TCP, default port 80
 *   https://HOST[:PORT]/PATH   TLS, default port 443
 *   HOST[:PORT]/PATH           no scheme -> plain HTTP, default port 80
 *   HOST[:PORT]                bare host -> path "/"
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/wget/wget.c -o /tmp/wget.o
 *   objdump -d /tmp/wget.o | grep 'fs:0x28'   # must produce no output
 */

#include "../../lib/net/http.h"
#include "../../lib/net/dns.h"

#define SYS_EXIT  0
#define SYS_WRITE 3
#define SYS_OPEN  4
#define SYS_CLOSE 5
#define SYS_YIELD 15

#define O_WRONLY 0x0001
#define O_CREAT  0x0040
#define O_TRUNC  0x0200

/* 6-arg inline syscall wrapper (nr in rax; args rdi,rsi,rdx,r10,r8,r9). */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* ----------------------------------------------------------------------
 * Tiny string helpers (no libc).
 * -------------------------------------------------------------------- */
static unsigned long w_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void w_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int w_streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* Case-insensitive prefix match: does `s` start with (lowercased) `pfx`? */
static int w_starts_ci(const char *s, const char *pfx) {
    int i = 0;
    for (; pfx[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != pfx[i]) return 0;
    }
    return 1;
}

/* Write a NUL-terminated string to fd 1 (stdout/console/serial). */
static void out(const char *s) {
    sc(SYS_WRITE, 1, (long)s, (long)w_strlen(s), 0, 0);
}

/* Append decimal representation of an unsigned value into buf at *pos. */
static void put_udec(char *buf, int cap, int *pos, unsigned long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && *pos < cap - 1) buf[(*pos)++] = tmp[--n];
    buf[*pos] = '\0';
}

/* Write exactly n bytes to fd, looping over short writes. 0 ok / -1 err. */
static int write_all(long fd, const char *buf, long n) {
    long off = 0;
    while (off < n) {
        long w = sc(SYS_WRITE, fd, (long)(buf + off), (long)(n - off), 0, 0);
        if (w < 0) return -1;
        if (w == 0) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        off += w;
    }
    return 0;
}

/* ----------------------------------------------------------------------
 * URL parser.
 *
 * Accepts:
 *   https://HOST[:PORT]/PATH   (TLS, default port 443) -> *is_https = 1
 *   http://HOST[:PORT]/PATH    (plain, default port 80) -> *is_https = 0
 *   HOST[:PORT]/PATH           (no scheme -> HTTP)
 *   HOST[:PORT]                (bare host -> path "/")
 *
 * Fills host[hostcap], *port, path[pathcap], *is_https.
 * Returns 0 on success, -1 on parse error.
 * -------------------------------------------------------------------- */
static int url_parse(const char *url,
                     char *host, int hostcap,
                     unsigned short *port,
                     char *path, int pathcap,
                     int *is_https) {
    const char *p = url;

    if (!url || !url[0]) return -1;

    *is_https = 0;
    unsigned short default_port = 80;

    if (w_starts_ci(p, "https://")) {
        *is_https = 1;
        default_port = 443;
        p += 8;  /* skip "https://" */
    } else if (w_starts_ci(p, "http://")) {
        p += 7;  /* skip "http://" */
    }

    /* HOST runs until ':' (port), '/' (path), or end. */
    int hi = 0;
    while (*p && *p != ':' && *p != '/') {
        if (hi < hostcap - 1) host[hi++] = *p;
        p++;
    }
    host[hi] = '\0';
    if (hi == 0) return -1;  /* empty host */

    /* Optional :PORT */
    unsigned short pt = default_port;
    if (*p == ':') {
        p++;
        unsigned long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned long)(*p - '0');
            digits++;
            p++;
        }
        if (digits == 0 || v == 0 || v > 65535) return -1;
        pt = (unsigned short)v;
    }
    *port = pt;

    /* PATH: everything from '/' onward, or "/" if none. */
    if (*p == '/') {
        int pi = 0;
        while (*p) {
            if (pi < pathcap - 1) path[pi++] = *p;
            p++;
        }
        path[pi] = '\0';
    } else if (*p == '\0') {
        w_strlcpy(path, "/", pathcap);
    } else {
        /* Unexpected leftover (e.g. stray junk after port w/o '/'). */
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------------------
 * Body buffer (large static; off the small user stack).
 * -------------------------------------------------------------------- */
static char body[262144];
static char g_host[256];
static char g_path[2048];

/* ----------------------------------------------------------------------
 * fetch -- parse URL, do http_get or https_get, then emit a status line +
 * body either to fd 1 or to an output file.
 *
 * insecure: if non-zero the CA-not-validated warning is suppressed.
 *
 * Returns 0 on success, 1 on error.
 * -------------------------------------------------------------------- */
static int fetch(const char *url, const char *outfile, int insecure) {
    unsigned short port = 80;
    int is_https = 0;
    int pr = url_parse(url, g_host, (int)sizeof(g_host), &port,
                       g_path, (int)sizeof(g_path), &is_https);
    if (pr != 0) {
        out("wget: bad URL: ");
        out(url);
        out("\n");
        return 1;
    }

    /* For HTTPS: warn about unauthenticated connections unless -k/--insecure. */
    if (is_https && !insecure) {
        out("wget: warning: server certificate not validated (no CA roots installed)\n");
    }

    int status = 0;
    long n;
    if (is_https) {
        n = https_get(g_host, port, g_path, body, sizeof(body), &status);
    } else {
        n = http_get(g_host, port, g_path, body, sizeof(body), &status);
    }

    if (n < 0) {
        out("wget: request failed for ");
        out(is_https ? "https://" : "http://");
        out(g_host);
        out(g_path);
        out("\n");
        return 1;
    }

    /* Status line to fd 1, e.g. "HTTP 200, 1234 bytes\n". */
    {
        char line[64];
        int pos = 0;
        w_strlcpy(line, "HTTP ", (int)sizeof(line));
        pos = (int)w_strlen(line);
        put_udec(line, (int)sizeof(line), &pos, (unsigned long)status);
        if (pos < (int)sizeof(line) - 2) { line[pos++] = ','; line[pos++] = ' '; line[pos] = '\0'; }
        put_udec(line, (int)sizeof(line), &pos, (unsigned long)n);
        w_strlcpy(line + pos, " bytes\n", (int)sizeof(line) - pos);
        out(line);
    }

    if (outfile) {
        w_strlcpy(g_path, outfile, (int)sizeof(g_path));
        long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
        if (fd < 0) {
            out("wget: cannot create '");
            out(outfile);
            out("'\n");
            return 1;
        }
        int rc = write_all(fd, body, n);
        sc(SYS_CLOSE, fd, 0, 0, 0, 0);
        if (rc != 0) { out("wget: write error\n"); return 1; }
    } else {
        if (write_all(1, body, n) != 0) { out("wget: write error\n"); return 1; }
    }

    return 0;
}

/* ----------------------------------------------------------------------
 * SELF-TEST -- deterministic, no network. Verifies the URL parser and the
 * dotted-quad (no-network) path of dns_resolve. Never performs a live
 * http_get or https_get (could block with nothing listening).
 * -------------------------------------------------------------------- */
static void selftest(void) {
    char host[256];
    char path[2048];
    unsigned short port = 0;
    int is_https = 0;

    /* Check 1: http://10.0.2.2:80/index.html */
    if (url_parse("http://10.0.2.2:80/index.html",
                  host, (int)sizeof(host), &port, path, (int)sizeof(path),
                  &is_https) != 0) {
        out("WGET SELFTEST: FAIL parse1-rc\n");
        return;
    }
    if (!w_streq(host, "10.0.2.2"))    { out("WGET SELFTEST: FAIL parse1-host\n"); return; }
    if (port != 80)                    { out("WGET SELFTEST: FAIL parse1-port\n"); return; }
    if (!w_streq(path, "/index.html")) { out("WGET SELFTEST: FAIL parse1-path\n"); return; }
    if (is_https != 0)                 { out("WGET SELFTEST: FAIL parse1-https\n"); return; }

    /* Check 2: http://example.com/  (default port 80, root path) */
    port = 0; is_https = -1;
    if (url_parse("http://example.com/",
                  host, (int)sizeof(host), &port, path, (int)sizeof(path),
                  &is_https) != 0) {
        out("WGET SELFTEST: FAIL parse2-rc\n");
        return;
    }
    if (!w_streq(host, "example.com")) { out("WGET SELFTEST: FAIL parse2-host\n"); return; }
    if (port != 80)                    { out("WGET SELFTEST: FAIL parse2-port\n"); return; }
    if (!w_streq(path, "/"))           { out("WGET SELFTEST: FAIL parse2-path\n"); return; }
    if (is_https != 0)                 { out("WGET SELFTEST: FAIL parse2-https\n"); return; }

    /* Check 3: bare host with no scheme/path -> HTTP, path "/" */
    port = 0; is_https = -1;
    if (url_parse("example.org",
                  host, (int)sizeof(host), &port, path, (int)sizeof(path),
                  &is_https) != 0) {
        out("WGET SELFTEST: FAIL parse3-rc\n");
        return;
    }
    if (!w_streq(host, "example.org")) { out("WGET SELFTEST: FAIL parse3-host\n"); return; }
    if (port != 80)                    { out("WGET SELFTEST: FAIL parse3-port\n"); return; }
    if (!w_streq(path, "/"))           { out("WGET SELFTEST: FAIL parse3-path\n"); return; }
    if (is_https != 0)                 { out("WGET SELFTEST: FAIL parse3-https\n"); return; }

    /* Check 4: https://example.com/ -> default port 443, is_https=1, succeeds. */
    port = 0; is_https = 0;
    if (url_parse("https://example.com/",
                  host, (int)sizeof(host), &port, path, (int)sizeof(path),
                  &is_https) != 0) {
        out("WGET SELFTEST: FAIL parse4-rc\n");
        return;
    }
    if (!w_streq(host, "example.com")) { out("WGET SELFTEST: FAIL parse4-host\n"); return; }
    if (port != 443)                   { out("WGET SELFTEST: FAIL parse4-port\n"); return; }
    if (!w_streq(path, "/"))           { out("WGET SELFTEST: FAIL parse4-path\n"); return; }
    if (is_https != 1)                 { out("WGET SELFTEST: FAIL parse4-https\n"); return; }

    /* Check 5: https://secure.example.org:8443/api -> custom port, is_https=1. */
    port = 0; is_https = 0;
    if (url_parse("https://secure.example.org:8443/api",
                  host, (int)sizeof(host), &port, path, (int)sizeof(path),
                  &is_https) != 0) {
        out("WGET SELFTEST: FAIL parse5-rc\n");
        return;
    }
    if (!w_streq(host, "secure.example.org")) { out("WGET SELFTEST: FAIL parse5-host\n"); return; }
    if (port != 8443)                          { out("WGET SELFTEST: FAIL parse5-port\n"); return; }
    if (!w_streq(path, "/api"))                { out("WGET SELFTEST: FAIL parse5-path\n"); return; }
    if (is_https != 1)                         { out("WGET SELFTEST: FAIL parse5-https\n"); return; }

    /* Check 6: dotted-quad DNS resolves with NO network: 10.0.2.2 -> 0x0A000202. */
    {
        unsigned int ip = 0;
        if (dns_resolve("10.0.2.2", &ip) != 0) {
            out("WGET SELFTEST: FAIL dns-rc\n");
            return;
        }
        if (ip != 0x0A000202u) {
            out("WGET SELFTEST: FAIL dns-ip\n");
            return;
        }
    }

    out("WGET SELFTEST: PASS\n");
}

/* ----------------------------------------------------------------------
 * main -- crt0 turns the return value into SYS_EXIT.
 * -------------------------------------------------------------------- */
int main(int argc, char **argv) {
    if (argc <= 1) {
        selftest();
        return 0;
    }

    /*
     * Argument parsing.
     *
     * Supported forms (flags before the URL):
     *   wget URL
     *   wget -O FILE URL
     *   wget -k URL
     *   wget --insecure URL
     *   wget -k -O FILE URL
     *   wget -O FILE -k URL
     *   wget --insecure -O FILE URL
     *   wget -O FILE --insecure URL
     *
     * We walk argv[1..] consuming flags until we hit something that is not
     * a recognised flag, which we treat as the URL.
     */
    const char *outfile = (void *)0;
    int insecure = 0;
    int i = 1;

    while (i < argc && argv[i]) {
        if (w_streq(argv[i], "-k") || w_streq(argv[i], "--insecure")) {
            insecure = 1;
            i++;
        } else if (w_streq(argv[i], "-O")) {
            if (i + 1 >= argc || !argv[i + 1]) {
                out("wget: -O requires a filename argument\n");
                return 1;
            }
            outfile = argv[i + 1];
            i += 2;
        } else {
            /* Not a recognised flag -- treat as URL. */
            break;
        }
    }

    if (i >= argc || !argv[i]) {
        out("usage: wget [-k|--insecure] [-O FILE] URL\n");
        return 1;
    }

    return fetch(argv[i], outfile, insecure);
}
