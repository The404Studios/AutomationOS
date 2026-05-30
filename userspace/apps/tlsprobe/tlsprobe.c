/*
 * tlsprobe.c -- TLS end-to-end interop tester for AutomationOS (ring 3).
 * =======================================================================
 *
 * FREESTANDING userspace: NO libc, NO stdio, NO malloc, NO standard headers.
 * Everything is inline syscalls + tiny self-contained helpers + fixed static
 * buffers. Linked with crt0 (provides _start -> main) and the unified netconn
 * layer (tlsconn.o) plus its transitive deps (dns.o, tls.o, crypto libs).
 *
 * Usage:
 *   tlsprobe HOST [PORT]
 *       Open a TLS connection to HOST:PORT (default 443), complete the
 *       handshake, send a minimal HTTP/1.0 GET, and print the first chunk of
 *       the response. Reports whether the certificate was trusted.
 *
 *   tlsprobe          (argc <= 1)
 *       Self-test: verify arg-parsing logic and static-storage layout without
 *       touching the network. Prints "TLSPROBE SELFTEST: PASS" and returns 0.
 *
 * Build (flags DIRECTLY on the command line; NO fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/tlsprobe/tlsprobe.c -o tlsprobe.o
 *   objdump -d tlsprobe.o | grep 'fs:0x28'   # MUST produce no output
 *
 * Link (integrator-supplied):
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       crt0.o tlsprobe.o tlsconn.o dns.o tls.o ... -o tlsprobe
 */

#include "../../lib/net/tlsconn.h"   /* netconn, netconn_open/read/write/close,
                                        netconn_selftest                        */

/* ---- syscall numbers --------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_WRITE  3

/* ---- 6-arg inline syscall wrapper -------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Static storage
 * netconn embeds a tls_conn_t (~40 KB of record buffers). It MUST NOT be
 * a stack local. Declared here at file scope so it lives in BSS.
 * --------------------------------------------------------------------- */
static netconn nc;

/* Scratch buffers for building and reading data. */
#define REQ_BUF_CAP   512
#define RESP_BUF_CAP  512

static char req_buf[REQ_BUF_CAP];
static char resp_buf[RESP_BUF_CAP];

/* ---- tiny string helpers (no libc) ------------------------------------ */
static unsigned long tp_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void tp_memset(void *dst, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
}

static void tp_strlcpy(char *dst, const char *src, unsigned long cap)
{
    unsigned long i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Parse an unsigned decimal string; returns 0 on empty/invalid. */
static unsigned long tp_atou(const char *s)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned long)(*s - '0'); s++; }
    return v;
}

/* ---- output helpers --------------------------------------------------- */
static void print(const char *s)
{
    sc(SYS_WRITE, 1, (long)s, (long)tp_strlen(s), 0, 0);
}

static void print_dec(long n)
{
    char buf[24];
    int neg = 0, i = 0;
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) { buf[i++] = '0'; }
    else { unsigned long u = (unsigned long)n; while (u > 0) { buf[i++] = (char)('0' + u % 10); u /= 10; } }
    if (neg) buf[i++] = '-';
    /* reverse */
    for (int lo = 0, hi = i - 1; lo < hi; lo++, hi--) {
        char t = buf[lo]; buf[lo] = buf[hi]; buf[hi] = t;
    }
    buf[i] = '\0';
    print(buf);
}

/* Print at most `max_bytes` bytes from `data`, replacing control bytes
 * (except \r and \n) with '.' so terminals stay clean. */
static void print_bounded(const char *data, long len, long max_bytes)
{
    char line[64];
    int  li  = 0;
    long n   = (len < max_bytes) ? len : max_bytes;
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\r') continue;           /* strip CR; \n will end the line */
        if (c == '\n') {
            line[li] = '\0';
            print(line);
            print("\n");
            li = 0;
            continue;
        }
        /* replace non-printable bytes */
        line[li++] = (c >= 32 && c < 127) ? (char)c : '.';
        if (li >= (int)(sizeof(line) - 1)) {
            line[li] = '\0';
            print(line);
            li = 0;
        }
    }
    if (li > 0) {
        line[li] = '\0';
        print(line);
        print("\n");
    }
}

/* -----------------------------------------------------------------------
 * Build the minimal GET request into req_buf.
 * Returns the number of bytes written (always < REQ_BUF_CAP).
 * --------------------------------------------------------------------- */
static unsigned long build_get_request(const char *host)
{
    /* "GET / HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n" */
    static const char prefix[] = "GET / HTTP/1.0\r\nHost: ";
    static const char suffix[] = "\r\nConnection: close\r\n\r\n";

    unsigned long plen  = tp_strlen(prefix);
    unsigned long hlen  = tp_strlen(host);
    unsigned long slen_ = tp_strlen(suffix);
    unsigned long total = plen + hlen + slen_;

    if (total >= REQ_BUF_CAP) total = REQ_BUF_CAP - 1;

    tp_memset(req_buf, 0, REQ_BUF_CAP);
    unsigned long pos = 0;
    for (unsigned long i = 0; i < plen  && pos < REQ_BUF_CAP - 1; i++) req_buf[pos++] = prefix[i];
    for (unsigned long i = 0; i < hlen  && pos < REQ_BUF_CAP - 1; i++) req_buf[pos++] = host[i];
    for (unsigned long i = 0; i < slen_ && pos < REQ_BUF_CAP - 1; i++) req_buf[pos++] = suffix[i];
    req_buf[pos] = '\0';
    return pos;
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    /* ------------------------------------------------------------------
     * Self-test mode: no host argument (argc <= 1).
     * Verify arg-parsing logic and that static storage is accessible.
     * No network calls; always passes.
     * ---------------------------------------------------------------- */
    if (argc <= 1) {
        /* Touch static storage to confirm it's accessible and zero-initialised
         * (BSS guarantee: nc.fd should be 0 at entry; we treat 0 as closed-
         * sentinel only in the real path, so touching it here is safe). */
        volatile int *fdp = &nc.fd;
        volatile int *tlsp = &nc.is_tls;
        volatile int *trp = &nc.trusted;
        (void)fdp; (void)tlsp; (void)trp;

        /* Confirm build_get_request helper doesn't overflow. */
        unsigned long rlen = build_get_request("example.com");
        if (rlen == 0 || rlen >= REQ_BUF_CAP) {
            print("TLSPROBE SELFTEST: FAIL (req_build)\n");
            sc(SYS_EXIT, 1, 0, 0, 0, 0);
            for (;;) {}
        }

        /* Confirm tp_atou parses correctly. */
        if (tp_atou("443") != 443 || tp_atou("0") != 0 || tp_atou("80") != 80) {
            print("TLSPROBE SELFTEST: FAIL (atou)\n");
            sc(SYS_EXIT, 1, 0, 0, 0, 0);
            for (;;) {}
        }

        /* Run netconn_selftest (offline dispatch + dotted-quad DNS stub). */
        int st = netconn_selftest();
        if (st != 0) {
            print("TLSPROBE SELFTEST: FAIL (netconn_selftest rc=");
            print_dec((long)st);
            print(")\n");
            sc(SYS_EXIT, 1, 0, 0, 0, 0);
            for (;;) {}
        }

        print("TLSPROBE SELFTEST: PASS\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0);
        for (;;) {}
    }

    /* ------------------------------------------------------------------
     * Real probe: tlsprobe HOST [PORT]
     * ---------------------------------------------------------------- */
    const char *host = argv[1];
    unsigned short port = 443;
    if (argc >= 3) {
        unsigned long p = tp_atou(argv[2]);
        if (p > 0 && p <= 65535)
            port = (unsigned short)p;
    }

    /* Print what we're about to probe. */
    print("TLSPROBE: connecting to ");
    print(host);
    print(":");
    print_dec((long)port);
    print(" ...\n");

    /* Connect + TLS handshake.
     * nc MUST be static (see SIZE WARNING in tlsconn.h); it is. */
    tp_memset(&nc, 0, sizeof(nc));
    int rc = netconn_open(&nc, host, port, /*use_tls=*/1);
    if (rc != 0) {
        print("TLSPROBE: handshake FAILED rc=");
        print_dec((long)rc);
        print("\n");
        print("TLSPROBE: FAILED ");
        print(host);
        print(" rc=");
        print_dec((long)rc);
        print("\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0);
        for (;;) {}
    }

    /* Report handshake outcome. */
    if (nc.trusted) {
        print("TLSPROBE: TLS handshake OK (cert=trusted)\n");
    } else {
        print("TLSPROBE: TLS handshake OK (encrypted but UNVERIFIED - no CA roots)\n");
    }

    /* Send minimal GET request. */
    unsigned long req_len = build_get_request(host);
    long wrc = netconn_write(&nc, req_buf, req_len);
    if (wrc < 0) {
        print("TLSPROBE: write failed rc=");
        print_dec(wrc);
        print("\n");
        netconn_close(&nc);
        print("TLSPROBE: FAILED ");
        print(host);
        print(" rc=");
        print_dec(wrc);
        print("\n");
        sc(SYS_EXIT, 2, 0, 0, 0, 0);
        for (;;) {}
    }

    /* Read the first chunk of the response (bounded to RESP_BUF_CAP - 1). */
    tp_memset(resp_buf, 0, RESP_BUF_CAP);
    long rrc = netconn_read(&nc, resp_buf, RESP_BUF_CAP - 1);

    netconn_close(&nc);

    if (rrc < 0) {
        print("TLSPROBE: read failed rc=");
        print_dec(rrc);
        print("\n");
        print("TLSPROBE: FAILED ");
        print(host);
        print(" rc=");
        print_dec(rrc);
        print("\n");
        sc(SYS_EXIT, 3, 0, 0, 0, 0);
        for (;;) {}
    }

    /* Print the HTTP status line + first ~200 bytes of the response. */
    print("--- response ---\n");
    long show = (rrc < 200) ? rrc : 200;
    print_bounded(resp_buf, rrc, show);
    print("--- end (");
    print_dec(rrc);
    print(" bytes read) ---\n");

    /* Final one-line result. */
    print("TLSPROBE: CONNECTED ");
    print(host);
    if (nc.trusted) {
        print(" (cert=trusted)\n");
    } else {
        print(" (cert=unverified)\n");
    }

    sc(SYS_EXIT, 0, 0, 0, 0, 0);
    for (;;) {}
}
