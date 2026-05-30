/*
 * livenet.c -- Live HTTPS fetch tool (freestanding, ring 3).
 * ===========================================================
 *
 * Manual proof that real HTTPS works end-to-end in AutomationOS userspace.
 * NO libc, NO stdio, NO malloc, NO standard headers.  Everything is built
 * on inline syscalls, fixed static buffers and own helpers.
 *
 * Linked with userspace/crt0.asm (_start -> main) + the lib objects below.
 *
 * Build (flags DIRECT on cmdline; objdump grep for fs:0x28 MUST be empty):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/livenet/livenet.c -o livenet.o
 *   objdump -d livenet.o | grep fs:0x28   # MUST be empty
 *
 * Usage:
 *   livenet                       -- self-test (no network required)
 *   livenet URL                   -- fetch URL, print status + body excerpt
 *   livenet --trust-info          -- dump CA bundle count + first few subjects
 *
 * URL syntax: http://HOST[:PORT]/PATH  or  https://HOST[:PORT]/PATH
 * Default ports: http -> 80, https -> 443.
 *
 * Self-test markers (argc <= 1):
 *   LIVENET SELFTEST: PASS
 *   LIVENET SELFTEST: FAIL <reason>
 *
 * Boot with QEMU slirp for live internet access, then:
 *   livenet https://example.com
 */

#include "../../lib/net/http.h"   /* https_get / http_get / http_get_ex */
#include "../../lib/net/dns.h"    /* dns_resolve / dns_selftest          */
#include "../../lib/tls/ca_bundle.h" /* ca_get_count / ca_get_name       */

/* ---- syscall numbers ---- */
#define SYS_EXIT    0
#define SYS_WRITE   3
#define SYS_YIELD   15

/* ---- fixed-width types (freestanding) ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed long long   i64;

/*
 * 6-argument inline syscall wrapper.
 * Encoding: rax=n, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6.
 */
static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- response buffer (static -- off the small user stack) ---- */
static char body[262144];

/* ================================================================
 * Freestanding string / print helpers
 * ================================================================ */

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

static void print_char(char c)
{
    sc(SYS_WRITE, 1, (long)&c, 1, 0, 0, 0);
}

static void print_dec(long n)
{
    if (n < 0) { print_char('-'); n = -n; }
    char b[24];
    int i = 0;
    unsigned long u = (unsigned long)n;
    do { b[i++] = (char)('0' + (u % 10)); u /= 10; } while (u > 0);
    while (i > 0) { print_char(b[--i]); }
}

/* print at most `len` bytes of buf (stops at NUL or len) */
static void print_n(const char *buf, long len)
{
    if (len <= 0) return;
    sc(SYS_WRITE, 1, (long)buf, (long)len, 0, 0, 0);
}

static int k_strncmp(const char *a, const char *b, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++) {
        if ((unsigned char)a[i] != (unsigned char)b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static int k_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* copy at most cap-1 bytes, always NUL-terminate */
static void k_strlcpy(char *dst, const char *src, unsigned long cap)
{
    if (!cap) return;
    unsigned long i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static unsigned short k_atou16(const char *s)
{
    unsigned short v = 0;
    while (*s >= '0' && *s <= '9') { v = (unsigned short)(v * 10 + (*s - '0')); s++; }
    return v;
}

/* ================================================================
 * Tiny URL parser
 *
 * Accepts:  http://HOST[:PORT]/PATH
 *           https://HOST[:PORT]/PATH
 *
 * On success returns 1 and fills scheme (0=http,1=https),
 * host[], port, path[].
 * On malformed input returns 0.
 * ================================================================ */
#define URL_HOST_MAX  256
#define URL_PATH_MAX  1024

static int parse_url(const char *url,
                     int *out_scheme,          /* 0=http, 1=https */
                     char  host[URL_HOST_MAX],
                     unsigned short *out_port,
                     char  path[URL_PATH_MAX])
{
    const char *p = url;

    /* scheme */
    if (k_strncmp(p, "https://", 8) == 0) {
        *out_scheme = 1;
        p += 8;
    } else if (k_strncmp(p, "http://", 7) == 0) {
        *out_scheme = 0;
        p += 7;
    } else {
        return 0; /* unknown scheme */
    }

    /* host (up to ':' or '/') */
    unsigned long hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < URL_HOST_MAX - 1) {
        host[hi++] = *p++;
    }
    host[hi] = '\0';
    if (hi == 0) return 0; /* empty host */

    /* optional :PORT */
    unsigned short port = (*out_scheme == 1) ? 443 : 80;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') return 0; /* colon but no digits */
        port = k_atou16(p);
        while (*p >= '0' && *p <= '9') p++;
    }
    *out_port = port;

    /* path (must start with '/' or be empty -> default "/") */
    if (*p == '/') {
        k_strlcpy(path, p, URL_PATH_MAX);
    } else if (*p == '\0') {
        path[0] = '/'; path[1] = '\0';
    } else {
        return 0; /* garbage after host:port */
    }

    return 1;
}

/* ================================================================
 * --trust-info subcommand
 * Dumps CA bundle count + first few cert subject labels.
 * ================================================================ */
static int cmd_trust_info(void)
{
    int count = ca_get_count();
    print("[LIVENET] CA bundle count: ");
    print_dec(count);
    print("\n");

    if (count == 0) {
        print("[LIVENET] trust store is EMPTY -- TLS connections will be "
              "encrypted but UNAUTHENTICATED.\n");
        return 0;
    }

    int show = (count < 5) ? count : 5;
    print("[LIVENET] First ");
    print_dec(show);
    print(" CA root(s):\n");
    for (int i = 0; i < show; i++) {
        const char *name = ca_get_name(i);
        print("  [");
        print_dec(i);
        print("] ");
        print(name ? name : "(null)");
        print("\n");
    }
    if (count > show) {
        print("  ... (");
        print_dec(count - show);
        print(" more)\n");
    }
    return 0;
}

/* ================================================================
 * Self-test (argc <= 1, no network required)
 *
 * Tests:
 *  1. dns_resolve("1.1.1.1") dotted-quad fast-path -> 0x01010101
 *  2. URL parser handles http://
 *  3. URL parser handles https://
 *  4. URL parser rejects malformed input
 * ================================================================ */
static int cmd_selftest(void)
{
    int pass = 1;
    char fail_why[128];
    fail_why[0] = '\0';

    /* --- test 1: dns_resolve dotted-quad fast path --- */
    {
        unsigned int ip = 0;
        int rc = dns_resolve("1.1.1.1", &ip);
        if (rc != 0) {
            pass = 0;
            k_strlcpy(fail_why, "dns_resolve('1.1.1.1') returned error", 128);
        } else if (ip != 0x01010101u) {
            pass = 0;
            k_strlcpy(fail_why, "dns_resolve('1.1.1.1') wrong address", 128);
        }
    }

    /* --- test 2: URL parser - http:// --- */
    if (pass) {
        int scheme; char host[URL_HOST_MAX]; unsigned short port;
        char path[URL_PATH_MAX];
        int ok = parse_url("http://example.com/index.html",
                           &scheme, host, &port, path);
        if (!ok) {
            pass = 0;
            k_strlcpy(fail_why, "parser rejected http://example.com/index.html", 128);
        } else if (scheme != 0) {
            pass = 0;
            k_strlcpy(fail_why, "http:// scheme parsed as https", 128);
        } else if (!k_streq(host, "example.com")) {
            pass = 0;
            k_strlcpy(fail_why, "http host mismatch", 128);
        } else if (port != 80) {
            pass = 0;
            k_strlcpy(fail_why, "http default port != 80", 128);
        } else if (!k_streq(path, "/index.html")) {
            pass = 0;
            k_strlcpy(fail_why, "http path mismatch", 128);
        }
    }

    /* --- test 3: URL parser - https:// with explicit port --- */
    if (pass) {
        int scheme; char host[URL_HOST_MAX]; unsigned short port;
        char path[URL_PATH_MAX];
        int ok = parse_url("https://secure.example.com:8443/api/v1",
                           &scheme, host, &port, path);
        if (!ok) {
            pass = 0;
            k_strlcpy(fail_why, "parser rejected https with explicit port", 128);
        } else if (scheme != 1) {
            pass = 0;
            k_strlcpy(fail_why, "https:// scheme parsed as http", 128);
        } else if (!k_streq(host, "secure.example.com")) {
            pass = 0;
            k_strlcpy(fail_why, "https host mismatch", 128);
        } else if (port != 8443) {
            pass = 0;
            k_strlcpy(fail_why, "https explicit port mismatch", 128);
        } else if (!k_streq(path, "/api/v1")) {
            pass = 0;
            k_strlcpy(fail_why, "https path mismatch", 128);
        }
    }

    /* --- test 4: URL parser - https:// bare host, no path -> "/" --- */
    if (pass) {
        int scheme; char host[URL_HOST_MAX]; unsigned short port;
        char path[URL_PATH_MAX];
        int ok = parse_url("https://example.com",
                           &scheme, host, &port, path);
        if (!ok) {
            pass = 0;
            k_strlcpy(fail_why, "parser rejected bare https://example.com", 128);
        } else if (!k_streq(path, "/")) {
            pass = 0;
            k_strlcpy(fail_why, "bare host path should default to '/'", 128);
        }
    }

    /* --- test 5: URL parser rejects malformed URLs --- */
    if (pass) {
        int scheme; char host[URL_HOST_MAX]; unsigned short port;
        char path[URL_PATH_MAX];

        /* no scheme */
        if (parse_url("example.com/page", &scheme, host, &port, path)) {
            pass = 0;
            k_strlcpy(fail_why, "parser accepted URL with no scheme", 128);
        }
        /* ftp:// */
        else if (parse_url("ftp://example.com/", &scheme, host, &port, path)) {
            pass = 0;
            k_strlcpy(fail_why, "parser accepted ftp:// scheme", 128);
        }
        /* colon but no port digits */
        else if (parse_url("http://host:/path", &scheme, host, &port, path)) {
            pass = 0;
            k_strlcpy(fail_why, "parser accepted ':' with no port digits", 128);
        }
    }

    if (pass) {
        print("LIVENET SELFTEST: PASS\n");
        return 0;
    } else {
        print("LIVENET SELFTEST: FAIL ");
        print(fail_why);
        print("\n");
        return 1;
    }
}

/* ================================================================
 * Fetch subcommand: livenet URL
 * ================================================================ */
static int cmd_fetch(const char *url)
{
    int scheme;
    char host[URL_HOST_MAX];
    unsigned short port;
    char path[URL_PATH_MAX];

    if (!parse_url(url, &scheme, host, &port, path)) {
        print("[LIVENET] error: malformed URL: ");
        print(url);
        print("\n");
        print("[LIVENET] expected: http://HOST[:PORT]/PATH  or"
              "  https://HOST[:PORT]/PATH\n");
        return 1;
    }

    /* Show what we parsed */
    print("[LIVENET] fetching ");
    print(scheme ? "https://" : "http://");
    print(host);
    print(":");
    print_dec(port);
    print(path);
    print("\n");

    int status = 0;
    long nbytes;

    if (scheme == 1) {
        nbytes = https_get(host, port, path,
                           body, (unsigned long)sizeof(body), &status);
    } else {
        nbytes = http_get(host, port, path,
                         body, (unsigned long)sizeof(body), &status);
    }

    if (nbytes < 0) {
        print("[LIVENET] error: fetch failed (rc=");
        print_dec(nbytes);
        print(")\n");
        return 1;
    }

    /* Summary line */
    print("HTTP ");
    print_dec(status);
    print(", ");
    print_dec(nbytes);
    print(" bytes\n");

    /* First ~500 bytes of body */
    print("--- body excerpt ---\n");
    long show = (nbytes < 500) ? nbytes : 500;
    print_n(body, show);
    if (nbytes > 500) {
        print("\n... (");
        print_dec(nbytes - 500);
        print(" more bytes)\n");
    } else {
        print("\n--- end ---\n");
    }

    return 0;
}

/* ================================================================
 * main
 * ================================================================ */
int main(int argc, char **argv)
{
    (void)argc; /* suppress unused-parameter warning in -ffreestanding */

    /* argc <= 1 -> self-test */
    if (argc <= 1) {
        return cmd_selftest();
    }

    /* --trust-info */
    if (k_streq(argv[1], "--trust-info")) {
        return cmd_trust_info();
    }

    /* URL fetch */
    return cmd_fetch(argv[1]);
}
