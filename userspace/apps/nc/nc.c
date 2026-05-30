/*
 * nc.c -- netcat-lite TCP client (freestanding, ring 3).
 * ======================================================
 *
 * A tiny "nc"-style TCP client for AutomationOS userspace.  Completely
 * freestanding: NO libc, NO stdio, NO malloc, NO standard headers -- every
 * I/O operation is an inline `syscall`, every buffer is a fixed-size local
 * or static array, and all string handling is done by the small static
 * helpers below.
 *
 * The only external dependency is the freestanding DNS resolver:
 *     int dns_resolve(const char *host, unsigned int *out_ip);  (0 == ok)
 * A dotted-quad host ("a.b.c.d") is parsed by dns_resolve with NO network
 * activity at all; a name triggers a bounded UDP DNS query.  Addresses are
 * returned in HOST byte order (0x0A000202 == 10.0.2.2), exactly what the
 * SYS_CONNECT socket syscall expects.
 *
 * Usage (linked with crt0.o -> int main(int argc, char **argv)):
 *     nc HOST PORT                connect; relay socket<->stdout (+stdin if any)
 *     nc -e "TEXT" HOST PORT      connect, send TEXT, print the response
 *     nc HOST PORT "TEXT"         same, trailing-text form
 *     nc                          API self-test (no args) -> "NC SELFTEST: ..."
 *
 * The "-e TEXT" / trailing-TEXT forms are handy because interactive stdin
 * may not be wired in this environment; e.g.
 *     nc -e "GET / HTTP/1.0\r\n\r\n" 10.0.2.2 80
 * (note: the TEXT is sent verbatim -- the shell/caller is responsible for any
 *  escape expansion; this tool does NOT interpret backslash escapes).
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/nc/nc.c -o nc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dns.c -o dns.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       crt0.o nc.o dns.o -o build/nc
 *   objdump -d build/nc | grep fs:0x28   # MUST be empty (no stack canary)
 */

#include "../../lib/net/dns.h"

/* ---- syscall numbers (per AutomationOS ABI) ---------------------------- */
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
#define FD_STDIN        0
#define FD_STDOUT       1

/* ---- error code returned by RECV when no data yet (would-block) -------- */
#define EAGAIN_NEG      (-11)

/* ---- self-test reference: 10.0.2.2 -> host-order 0x0A000202 ------------ */
#define GATEWAY_IP      0x0A000202u
#define DISCARD_PORT    9

/*
 * Iteration cap for the RECV relay loop.  Each iteration is one SYS_SOCK_POLL
 * + one SYS_RECV (with a SYS_YIELD on a dry read), so this is purely a safety
 * bound to guarantee the tool always returns even if a peer goes silent
 * without closing.  Large enough to drain real responses, small enough to be
 * finite.  (dns.c uses 200000 for its UDP wait; we keep the same magnitude.)
 */
#define RELAY_MAX       400000

/*
 * Connect self-test cap.  The kernel's tcp_connect() has its own internal
 * timeout (it returns within TCP_CONNECT_MS), so we call SYS_CONNECT exactly
 * ONCE and never loop on it -- this constant just documents that intent.
 */
#define CONNECT_ATTEMPTS 1

/* ===================================================================== */
/* Raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8)                         */
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
/* Tiny freestanding helpers                                             */
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

/*
 * Parse a base-10 port string.  Returns the value in 1..65535, or -1 on any
 * malformed / out-of-range input (leading sign, non-digits, overflow, empty).
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

/* ===================================================================== */
/* Socket relay                                                          */
/* ===================================================================== */

/* Shared I/O buffers (static -> no big stack frames in -mno-red-zone code). */
#define IO_BUF_SZ  2048
static char g_rx_buf[IO_BUF_SZ];
static char g_tx_buf[IO_BUF_SZ];

/*
 * Send the entire buffer over the socket, looping over short SYS_SEND results.
 * Returns total bytes sent (>=0) or the last negative error.  Bounded by a
 * fixed number of attempts so a stuck socket can never spin forever.
 */
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

/*
 * Relay loop: pump the NIC, drain the socket to fd1, and (if stdin is wired)
 * forward stdin bytes to the socket.  Runs until the peer closes the
 * connection (RECV == 0) or the iteration cap is hit.
 *
 * stdin is treated as optional/non-blocking: a return of 0 or a negative
 * result is simply ignored (interactive stdin may not exist here).  We never
 * block waiting on stdin.
 *
 * Returns the total number of bytes written to fd1.
 */
static long relay(long fd)
{
    long total_out = 0;
    int  dry = 0;          /* consecutive iterations with no socket data */

    for (long it = 0; it < RELAY_MAX; it++) {
        /* Always pump the NIC before attempting a receive. */
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);

        long rn = sc(SYS_RECV, fd, (long)g_rx_buf, IO_BUF_SZ, 0, 0);
        if (rn > 0) {
            out_write(g_rx_buf, (unsigned int)rn);
            total_out += rn;
            dry = 0;
        } else if (rn == 0) {
            /* Peer closed the connection -> we are done. */
            break;
        } else if (rn == EAGAIN_NEG) {
            /* No data yet; yield so the NIC/timers can make progress. */
            dry++;
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
        } else {
            /* Hard socket error -> stop relaying. */
            break;
        }

        /*
         * Opportunistically forward any stdin bytes to the socket.  SYS_READ
         * on fd0 may legitimately return 0 (no stdin) or a negative value;
         * either way we just skip forwarding this round and keep relaying RX.
         */
        long in = sc(SYS_READ, FD_STDIN, (long)g_tx_buf, IO_BUF_SZ, 0, 0);
        if (in > 0) {
            long s = send_all(fd, g_tx_buf, in);
            if (s < 0) break;               /* socket gone -> stop */
            dry = 0;
        }

        (void)dry;   /* tracked for clarity; the iteration cap is the bound */
    }

    return total_out;
}

/* ===================================================================== */
/* Connected-mode driver                                                 */
/* ===================================================================== */

/*
 * Resolve `host`, open a TCP socket, connect to host:port, optionally send a
 * fixed `text` line, then relay until the peer closes or the cap is hit.
 * Prints a short status to fd1 throughout.  Returns a process exit code
 * (0 == success, non-zero == failure stage).
 */
static int do_connect(const char *host, long port, const char *text)
{
    unsigned int ip = 0;
    int dr = dns_resolve(host, &ip);
    if (dr != 0) {
        out_puts("nc: cannot resolve host '");
        out_puts(host);
        out_puts("' (dns rc=");
        out_num(dr);
        out_puts(")\n");
        return 2;
    }

    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) {
        out_puts("nc: socket() failed rc=");
        out_num(fd);
        out_puts("\n");
        return 3;
    }

    out_puts("nc: connecting to ");
    out_ip(ip);
    out_puts(":");
    out_num(port);
    out_puts(" ...\n");

    /* Single bounded connect; the kernel's tcp_connect has its own timeout. */
    long cr = sc(SYS_CONNECT, fd, (long)ip, port, 0, 0);
    if (cr < 0) {
        out_puts("nc: connect failed rc=");
        out_num(cr);
        out_puts("\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 4;
    }

    out_puts("nc: connected\n");

    /* Optional one-shot payload (e.g. an HTTP request line). */
    if (text && text[0]) {
        long want = (long)n_strlen(text);
        long s = send_all(fd, text, want);
        if (s < 0) {
            out_puts("nc: send failed rc=");
            out_num(s);
            out_puts("\n");
            sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
            return 5;
        }
        out_puts("nc: sent ");
        out_num(s);
        out_puts(" bytes\n");
    }

    long got = relay(fd);

    out_puts("\nnc: connection closed (");
    out_num(got);
    out_puts(" bytes received)\n");

    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    return 0;
}

/* ===================================================================== */
/* Self-test (argc <= 1)                                                 */
/* ===================================================================== */

/*
 * Exercise the socket + DNS API without depending on any listening server:
 *
 *   1. dns_resolve("10.0.2.2") must succeed and yield host-order 0x0A000202
 *      with NO network activity (pure dotted-quad parse).
 *   2. SYS_SOCKET must return a non-negative fd.
 *   3. A SINGLE, bounded SYS_CONNECT to 10.0.2.2:9 (discard) is attempted.
 *      Nothing listens there, so this will fail/timeout -- that is EXPECTED
 *      and fine.  The point is that the call RETURNS (does not hang); the
 *      kernel's tcp_connect enforces its own timeout, so we never loop on it.
 *   4. SYS_CLOSE_SK must succeed.
 *
 * Prints "NC SELFTEST: PASS" if the API behaves sanely, otherwise
 * "NC SELFTEST: FAIL <why>".  Returns 0 on PASS, non-zero on FAIL.
 */
static int self_test(void)
{
    /* --- 1. DNS dotted-quad parse (no network) --- */
    unsigned int ip = 0xDEADBEEFu;
    int dr = dns_resolve("10.0.2.2", &ip);
    if (dr != 0) {
        out_puts("NC SELFTEST: FAIL dns_resolve rc=");
        out_num(dr);
        out_puts("\n");
        return 1;
    }
    if (ip != GATEWAY_IP) {
        out_puts("NC SELFTEST: FAIL dns ip=0x");
        /* print as decimal for simplicity; value mismatch is the signal */
        out_unum(ip);
        out_puts(" expected 0x0A000202\n");
        return 1;
    }

    /* --- 2. socket() --- */
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) {
        out_puts("NC SELFTEST: FAIL socket rc=");
        out_num(fd);
        out_puts("\n");
        return 1;
    }

    /*
     * --- 3. bounded connect to the discard port (expected to fail) ---
     * We call connect exactly once.  Any return value is acceptable as long
     * as the call comes back (success is unlikely since nothing listens; a
     * negative errno is the normal outcome).  The mere fact that control
     * returns here proves the call is bounded and does not hang the OS.
     */
    long cr = sc(SYS_CONNECT, fd, (long)GATEWAY_IP, DISCARD_PORT, 0, 0);
    (void)cr;   /* don't assert success: nothing is listening on :9 */

    /* --- 4. close() --- */
    long clr = sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    if (clr < 0) {
        out_puts("NC SELFTEST: FAIL close rc=");
        out_num(clr);
        out_puts("\n");
        return 1;
    }

    /* All API calls behaved: fd>=0, dns correct, connect returned, close ok. */
    out_puts("NC SELFTEST: PASS (socket fd=");
    out_num(fd);
    out_puts(", dns=10.0.2.2->0x0A000202, connect rc=");
    out_num(cr);
    out_puts(")\n");
    return 0;
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(int argc, char **argv)
{
    /* No args (or just the program name) -> run the API self-test. */
    if (argc <= 1)
        return self_test();

    /*
     * Argument forms:
     *   nc -e "TEXT" HOST PORT      argv: [-e][TEXT][HOST][PORT]  (argc 5)
     *   nc HOST PORT "TEXT"         argv: [HOST][PORT][TEXT]      (argc 4)
     *   nc HOST PORT                argv: [HOST][PORT]            (argc 3)
     */
    const char *host = (void *)0;
    const char *port_str = (void *)0;
    const char *text = (void *)0;

    if (argc >= 2 && n_streq(argv[1], "-e")) {
        /* nc -e TEXT HOST PORT */
        if (argc < 5) {
            out_puts("usage: nc -e \"TEXT\" HOST PORT\n");
            return 1;
        }
        text     = argv[2];
        host     = argv[3];
        port_str = argv[4];
    } else {
        /* nc HOST PORT [TEXT] */
        if (argc < 3) {
            out_puts("usage: nc HOST PORT  |  nc HOST PORT \"TEXT\"  |  "
                     "nc -e \"TEXT\" HOST PORT\n");
            return 1;
        }
        host     = argv[1];
        port_str = argv[2];
        if (argc >= 4) text = argv[3];   /* trailing-text form */
    }

    long port = n_parse_port(port_str);
    if (port < 0) {
        out_puts("nc: invalid port '");
        out_puts(port_str);
        out_puts("'\n");
        return 1;
    }

    return do_connect(host, port, text);
}
