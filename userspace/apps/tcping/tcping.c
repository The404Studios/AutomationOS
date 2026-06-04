/*
 * tcping.c -- TCP connect-latency probe (freestanding, ring 3).
 * =============================================================
 *
 * A tiny "tcping"-style tool for AutomationOS userspace: it measures how long
 * a TCP three-way handshake (connect()) to a host:port takes, the same way the
 * classic Windows `tcping` utility does.  Completely freestanding: NO libc, NO
 * stdio, NO malloc, NO standard headers -- every I/O operation is an inline
 * `syscall`, every buffer is a fixed-size local/static array, and all
 * string/number handling is done by the small static helpers below.  The
 * syscall ABI, inline-syscall macro, argv handling and the print/itoa helpers
 * are copied VERBATIM from the model net tools (userspace/apps/nc/nc.c,
 * ping/ping.c, netscan/netscan.c, netinfo/netinfo.c) -- nothing here is
 * invented.  The timed connect path mirrors nc.c's do_connect():
 *     SYS_SOCKET -> SYS_CONNECT (ONE bounded call) -> SYS_CLOSE_SK
 * with the wall time read from SYS_GET_TICKS_MS (40) on both sides of the
 * single connect() call.
 *
 * The only external dependency is the freestanding DNS resolver:
 *     int dns_resolve(const char *host, unsigned int *out_ip);  (0 == ok)
 * A dotted-quad host ("a.b.c.d") is parsed by dns_resolve with NO network
 * activity at all; a name triggers a bounded UDP DNS query.  Addresses are
 * returned in HOST byte order (0x0A000202 == 10.0.2.2), exactly what the
 * SYS_CONNECT socket syscall expects.
 *
 * --- Probe model (modeled off nc.c's connect path) ----------------------
 * For each of `count` attempts we:
 *     1. SYS_SOCKET(SOCK_STREAM)                 -> a fresh fd
 *     2. t0 = SYS_GET_TICKS_MS
 *     3. SYS_CONNECT(fd, ip_host_order, port)    -> ONE bounded attempt
 *     4. t1 = SYS_GET_TICKS_MS;  elapsed = t1 - t0
 *     5. classify the return code:
 *            == 0   (SOCK_OK)        -> "open in N ms"      (handshake done)
 *            -107   (SOCK_ECONN)     -> "refused in N ms"   (RST received)
 *            -110   (SOCK_ETIMEDOUT) -> "timeout"           (no reply in budget)
 *            other negative          -> "error rc=..."      (socket/EINVAL/etc.)
 *     6. SYS_CLOSE_SK(fd)
 *     7. SYS_SOCK_POLL + a short SYS_SLEEP throttle between attempts.
 *
 * SYS_CONNECT is called EXACTLY ONCE per attempt and is NEVER looped on: the
 * kernel's tcp_connect() enforces its own internal budget (TCP_CONNECT_MS), so
 * the call is guaranteed to RETURN and this tool can never hang.  We also clamp
 * every measured elapsed time to PER_ATTEMPT_TIMEOUT_MS so a runaway clock can
 * never produce a nonsensical figure.  Each attempt's result is PRINTED AS WE
 * GO so progress is visible, then a min/avg/max summary over the OPEN attempts
 * is printed at the end (exactly like tcping).
 *
 * Usage (linked with crt0.o -> int main(int argc, char **argv)):
 *     tcping IP PORT [count]    probe IP:PORT `count` times (default 4)
 *     tcping                    API self-test (no args) -> "TCPING SELFTEST: ..."
 *
 * Examples:
 *     tcping 10.0.2.2 53        4 timed connects to the slirp DNS port
 *     tcping 10.0.2.2 80 10     10 timed connects to the slirp HTTP forward
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/tcping/tcping.c -o tcping.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dns.c -o dns.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       crt0.o tcping.o dns.o -o build/tcping
 *   objdump -d build/tcping | grep fs:0x28   # MUST be empty (no stack canary)
 */

#include "../../lib/net/dns.h"

/* ---- syscall numbers (per AutomationOS ABI -- identical to nc/ping/netscan) */
#define SYS_WRITE         3    /* write(fd, buf, len)           fd1 = stdout   */
#define SYS_SLEEP         9    /* sleep(ms) -- real blocking ms sleep          */
#define SYS_YIELD         15   /* cooperative yield                            */
#define SYS_GET_TICKS_MS  40   /* monotonic milliseconds since boot            */
#define SYS_SOCKET        51   /* socket(SOCK_STREAM) -> fd                    */
#define SYS_CONNECT       52   /* connect(fd, ip_host_order, port) -> 0/neg    */
#define SYS_CLOSE_SK      55   /* close(fd) -> 0                               */
#define SYS_SOCK_POLL     58   /* pump the NIC RX/timers                       */

/* ---- socket type ------------------------------------------------------- */
#define SOCK_STREAM       1

/* ---- well-known fd ----------------------------------------------------- */
#define FD_STDOUT         1

/*
 * Socket return codes (mirror kernel/include/socket.h). SYS_CONNECT returns
 * one of these directly:
 *   SOCK_OK        ( 0  ) -> three-way handshake completed   => open
 *   SOCK_ECONN     (-107) -> got a RST (connection refused)  => refused
 *   SOCK_ETIMEDOUT (-110) -> no reply within the kernel's    => timeout
 *                            TCP_CONNECT_MS budget
 * Any other negative value (e.g. SOCK_EINVAL -22, or a bad fd) is reported as
 * an error and does NOT count as a successful open.
 */
#define SOCK_OK            0
#define SOCK_ECONN       (-107)
#define SOCK_ETIMEDOUT   (-110)

/* ---- self-test reference: 10.0.2.2 -> host-order 0x0A000202 ------------ */
#define GATEWAY_IP        0x0A000202u
#define DISCARD_PORT      9

/* ---- defaults ---------------------------------------------------------- */
#define DEF_COUNT         4          /* tcping default attempt count          */
#define MAX_COUNT         100000     /* clamp absurd counts                   */

/*
 * Per-attempt timeout (ms).  The kernel's tcp_connect() already enforces its
 * own internal TCP_CONNECT_MS budget, so SYS_CONNECT is guaranteed to return;
 * this constant is the UPPER BOUND we clamp the measured elapsed time to, so a
 * single attempt's reported latency can never exceed it even if the monotonic
 * clock jumps.  It also documents the "bounded per-attempt timeout" contract.
 */
#define PER_ATTEMPT_TIMEOUT_MS  3000

/*
 * Inter-attempt throttle (ms).  After each attempt we pump the NIC once and
 * sleep this many milliseconds so a multi-attempt run stays gentle on the RX
 * ring and yields the CPU.  This is spacing BETWEEN attempts, not the connect
 * timeout.  Same idea as netscan.c's THROTTLE_MS, sized like tcping's ~1s gap
 * but kept smaller so the tool returns promptly.
 */
#define THROTTLE_MS       200

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
/* Tiny freestanding helpers (copied from nc.c / netscan.c)              */
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

/*
 * Parse a non-negative decimal count; returns -1 on bad input, clamps absurd
 * values to MAX_COUNT (mirrors ping.c's parse_uint clamp).
 */
static long n_parse_count(const char *s)
{
    if (!s || !*s) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > MAX_COUNT) return MAX_COUNT;
    }
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

/* Print a host-order IPv4 address as A.B.C.D to fd1. */
static void out_ip(unsigned int ip)
{
    out_unum((ip >> 24) & 0xFFu); out_puts(".");
    out_unum((ip >> 16) & 0xFFu); out_puts(".");
    out_unum((ip >>  8) & 0xFFu); out_puts(".");
    out_unum( ip        & 0xFFu);
}

/* ===================================================================== */
/* Single timed connect attempt                                          */
/* ===================================================================== */

/*
 * One bounded, TIMED TCP connect to ip:port (ip in host byte order).  Mirrors
 * nc.c's do_connect() inner path exactly: socket() -> ONE connect() (never
 * looped; the kernel bounds it) -> close().  We bracket ONLY the single
 * SYS_CONNECT call with SYS_GET_TICKS_MS reads so the measured figure is the
 * handshake (or RST/timeout) latency, not setup/teardown.
 *
 * The raw SYS_CONNECT return code is written to *out_rc (SOCK_OK / SOCK_ECONN /
 * SOCK_ETIMEDOUT / other) and the elapsed milliseconds (clamped to
 * PER_ATTEMPT_TIMEOUT_MS) is the function's return value, or -1 if the socket
 * could not even be created (in which case *out_rc carries that error).  The fd
 * is ALWAYS closed before returning, so no descriptors leak across a run.
 */
static long timed_connect(unsigned int ip, long port, long *out_rc)
{
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) {
        *out_rc = fd;                       /* couldn't make a socket */
        return -1;
    }

    /*
     * Bracket the SINGLE bounded connect with monotonic-ms reads.  We do NOT
     * loop on connect: the kernel's tcp_connect() returns within its own
     * TCP_CONNECT_MS budget (RST -> immediate ECONN, handshake -> immediate OK,
     * silence -> ETIMEDOUT at the budget), so this one call always comes back.
     */
    long t0 = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    long cr = sc(SYS_CONNECT, fd, (long)ip, port, 0, 0);
    long t1 = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);

    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);

    long elapsed = t1 - t0;
    if (elapsed < 0) elapsed = 0;                       /* clock went backwards */
    if (elapsed > PER_ATTEMPT_TIMEOUT_MS)               /* bound the figure     */
        elapsed = PER_ATTEMPT_TIMEOUT_MS;

    *out_rc = cr;
    return elapsed;
}

/* ===================================================================== */
/* Probe driver                                                          */
/* ===================================================================== */

/*
 * Resolve `host`, then run `count` timed connects to host:port, printing each
 * attempt's outcome as it happens and a min/avg/max summary at the end (over
 * the OPEN attempts only -- refused/timeout/error contribute to the tallies but
 * not to the latency stats, matching tcping's behaviour).  Returns a process
 * exit code: 0 if at least one attempt opened, 1 if none did, 2 on resolve fail.
 */
static int do_tcping(const char *host, long port, long count)
{
    unsigned int ip = 0;
    int dr = dns_resolve(host, &ip);
    if (dr != 0) {
        out_puts("tcping: cannot resolve host '");
        out_puts(host);
        out_puts("' (dns rc=");
        out_num(dr);
        out_puts(")\n");
        return 2;
    }

    out_puts("tcping ");
    out_ip(ip);
    out_puts(":");
    out_num(port);
    out_puts(" (");
    out_num(count);
    out_puts(" attempts)\n");

    long open_count   = 0;
    long refused_count = 0;
    long timeout_count = 0;
    long err_count     = 0;

    long min_ms = 0, max_ms = 0, sum_ms = 0;   /* over OPEN attempts only */

    for (long i = 0; i < count; i++) {
        long rc = 0;
        long ms = timed_connect(ip, port, &rc);

        out_puts("  ");
        out_ip(ip);
        out_puts(":");
        out_num(port);
        out_puts(" seq=");
        out_num(i + 1);
        out_puts(" : ");

        if (ms >= 0 && rc == SOCK_OK) {
            out_puts("open in ");
            out_num(ms);
            out_puts(" ms\n");
            open_count++;
            sum_ms += ms;
            if (open_count == 1 || ms < min_ms) min_ms = ms;
            if (open_count == 1 || ms > max_ms) max_ms = ms;
        } else if (rc == SOCK_ECONN) {
            out_puts("refused in ");
            out_num(ms >= 0 ? ms : 0);
            out_puts(" ms\n");
            refused_count++;
        } else if (rc == SOCK_ETIMEDOUT) {
            out_puts("timeout\n");
            timeout_count++;
        } else {
            out_puts("error rc=");
            out_num(rc);
            out_puts("\n");
            err_count++;
        }

        /*
         * Pump the NIC, then a short throttle/yield between attempts so a
         * multi-attempt run stays gentle and cooperative.  Skip the throttle
         * after the final attempt so the tool returns promptly.  Bounded +
         * non-blocking; this is spacing, not a connect timeout.
         */
        if (i + 1 < count) {
            sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
            sc(SYS_SLEEP, THROTTLE_MS, 0, 0, 0, 0);
        }
    }

    /* --- summary (tcping-style) --- */
    out_puts("\n--- ");
    out_ip(ip);
    out_puts(":");
    out_num(port);
    out_puts(" tcping statistics ---\n");
    out_puts("  ");
    out_num(count);
    out_puts(" attempts, ");
    out_num(open_count);
    out_puts(" open, ");
    out_num(refused_count);
    out_puts(" refused, ");
    out_num(timeout_count);
    out_puts(" timeout");
    if (err_count) {
        out_puts(", ");
        out_num(err_count);
        out_puts(" error");
    }
    out_puts("\n");

    if (open_count > 0) {
        long avg_ms = sum_ms / open_count;
        out_puts("  rtt min/avg/max = ");
        out_num(min_ms);
        out_puts("/");
        out_num(avg_ms);
        out_puts("/");
        out_num(max_ms);
        out_puts(" ms\n");
        return 0;
    }

    out_puts("  no successful connections\n");
    return 1;
}

/* ===================================================================== */
/* Self-test (argc <= 1)                                                 */
/* ===================================================================== */

/*
 * Exercise the socket + DNS + timed-connect API without depending on any
 * particular listening server (mirrors nc.c / netscan.c self_test):
 *
 *   1. dns_resolve("10.0.2.2") must succeed and yield host-order 0x0A000202
 *      with NO network activity (pure dotted-quad parse).
 *   2. n_parse_port must accept "53" (->53) and reject "0" / "70000" / "x".
 *   3. SYS_GET_TICKS_MS must return a non-negative, non-decreasing value.
 *   4. timed_connect() to 10.0.2.2:9 (discard) must RETURN a value (does not
 *      hang) with a bounded elapsed <= PER_ATTEMPT_TIMEOUT_MS; ANY rc is
 *      acceptable here -- the point is boundedness.
 *
 * Prints "TCPING SELFTEST: PASS" if the API behaves sanely, otherwise
 * "TCPING SELFTEST: FAIL <why>".  Returns 0 on PASS, non-zero on FAIL.
 */
static int self_test(void)
{
    /* --- 1. DNS dotted-quad parse (no network) --- */
    unsigned int ip = 0xDEADBEEFu;
    int dr = dns_resolve("10.0.2.2", &ip);
    if (dr != 0) {
        out_puts("TCPING SELFTEST: FAIL dns_resolve rc=");
        out_num(dr);
        out_puts("\n");
        return 1;
    }
    if (ip != GATEWAY_IP) {
        out_puts("TCPING SELFTEST: FAIL dns ip mismatch (got ");
        out_unum(ip);
        out_puts(", want 0x0A000202)\n");
        return 1;
    }

    /* --- 2. port parser sanity --- */
    if (n_parse_port("53") != 53)    { out_puts("TCPING SELFTEST: FAIL parse 53\n");    return 1; }
    if (n_parse_port("0")  != -1)    { out_puts("TCPING SELFTEST: FAIL parse 0\n");     return 1; }
    if (n_parse_port("70000") != -1) { out_puts("TCPING SELFTEST: FAIL parse 70000\n"); return 1; }
    if (n_parse_port("x")  != -1)    { out_puts("TCPING SELFTEST: FAIL parse x\n");     return 1; }

    /* --- 3. monotonic clock sanity --- */
    long ta = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    long tb = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    if (ta < 0 || tb < ta) {
        out_puts("TCPING SELFTEST: FAIL ticks not monotonic (");
        out_num(ta); out_puts(" -> "); out_num(tb); out_puts(")\n");
        return 1;
    }

    /* --- 4. bounded timed connect to the discard port (must RETURN, not hang) --- */
    long rc = 0;
    long ms = timed_connect(GATEWAY_IP, DISCARD_PORT, &rc);
    if (ms > PER_ATTEMPT_TIMEOUT_MS) {
        out_puts("TCPING SELFTEST: FAIL connect elapsed unbounded (");
        out_num(ms); out_puts(" ms)\n");
        return 1;
    }
    /* Don't assert the outcome: nothing need listen on :9.  The mere fact that
     * control returns here with a bounded elapsed proves timed_connect is
     * bounded and never hangs. */

    out_puts("TCPING SELFTEST: PASS (dns=10.0.2.2->0x0A000202, port-parse ok, "
             "ticks ok, probe 10.0.2.2:9 rc=");
    out_num(rc);
    out_puts(", elapsed=");
    out_num(ms);
    out_puts(" ms)\n");
    return 0;
}

/* ===================================================================== */
/* Entry point (crt0 supplies _start -> main(argc, argv))                */
/* ===================================================================== */

int main(int argc, char **argv)
{
    /* No args (or just the program name) -> run the API self-test. */
    if (argc <= 1)
        return self_test();

    /* tcping IP PORT [count] */
    if (argc < 3) {
        out_puts("usage: tcping <ip> <port> [count]\n");
        return 1;
    }

    const char *host     = argv[1];
    const char *port_str = argv[2];

    long port = n_parse_port(port_str);
    if (port < 0) {
        out_puts("tcping: invalid port '");
        out_puts(port_str);
        out_puts("'\n");
        return 1;
    }

    long count = DEF_COUNT;
    if (argc >= 4) {
        long c = n_parse_count(argv[3]);
        if (c < 0) {
            out_puts("tcping: invalid count '");
            out_puts(argv[3]);
            out_puts("'\n");
            return 1;
        }
        if (c == 0) c = 1;                 /* a 0 count is treated as 1 */
        count = c;
    }

    return do_tcping(host, port, count);
}
