/*
 * netscan.c -- TCP connect() port scanner (freestanding, ring 3).
 * ===============================================================
 *
 * A tiny "nmap -sT"-style TCP connect scanner for AutomationOS userspace.
 * Completely freestanding: NO libc, NO stdio, NO malloc, NO standard headers --
 * every I/O operation is an inline `syscall`, every buffer is a fixed-size local
 * or static array, and all string/number handling is done by the small static
 * helpers below.  The syscall ABI, inline-syscall macro, argv handling and the
 * print/itoa helpers are copied verbatim from the model net tools
 * (userspace/apps/nc/nc.c, ping/ping.c, wget/wget.c) -- nothing here is invented.
 *
 * The only external dependency is the freestanding DNS resolver:
 *     int dns_resolve(const char *host, unsigned int *out_ip);  (0 == ok)
 * A dotted-quad host ("a.b.c.d") is parsed by dns_resolve with NO network
 * activity at all; a name triggers a bounded UDP DNS query.  Addresses are
 * returned in HOST byte order (0x0A000202 == 10.0.2.2), exactly what the
 * SYS_CONNECT socket syscall expects.
 *
 * --- Scan model (modeled off nc's connect path) -------------------------
 * For each port in [startPort, endPort] we:
 *     1. SYS_SOCKET(SOCK_STREAM)                 -> a fresh fd
 *     2. SYS_CONNECT(fd, ip_host_order, port)    -> ONE bounded attempt
 *     3. classify the return code:
 *            == 0   (SOCK_OK)        -> OPEN     (handshake completed)
 *            -107   (SOCK_ECONN)     -> closed   (RST: connection refused)
 *            -110   (SOCK_ETIMEDOUT) -> filtered (no reply within budget)
 *            other negative          -> error    (socket/EINVAL/etc.)
 *     4. SYS_CLOSE_SK(fd)
 *     5. SYS_SOCK_POLL + a small SYS_SLEEP throttle so a quiet host can't
 *        starve the run and so we don't hammer the NIC ring back-to-back.
 *
 * SYS_CONNECT is called EXACTLY ONCE per port and is never looped on: the
 * kernel's tcp_connect() enforces its own internal budget (TCP_CONNECT_MS),
 * so the call is guaranteed to RETURN and this tool can never hang.  On a
 * reachable host (e.g. QEMU slirp 10.0.2.2) an OPEN port completes its
 * handshake immediately and a closed port gets an immediate RST, so the
 * common cases are fast; only a genuinely filtered/black-holed port pays the
 * kernel's full connect budget.  We therefore PRINT EACH PORT'S RESULT AS WE
 * GO so progress is visible even on a slow target.
 *
 * Usage (linked with crt0.o -> int main(int argc, char **argv)):
 *     netscan HOST                 scan default port range 1..1024
 *     netscan HOST START END       scan the inclusive range [START, END]
 *     netscan HOST PORT            scan a single port (START == END)
 *     netscan                      API self-test (no args) -> "NETSCAN SELFTEST: ..."
 *
 * Examples:
 *     netscan 10.0.2.2             scan slirp gateway ports 1..1024
 *     netscan 10.0.2.2 1 100       scan ports 1..100
 *     netscan 10.0.2.3 53 53       probe just the DNS port on the slirp resolver
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/netscan/netscan.c -o netscan.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dns.c -o dns.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       crt0.o netscan.o dns.o -o build/netscan
 *   objdump -d build/netscan | grep fs:0x28   # MUST be empty (no stack canary)
 */

#include "../../lib/net/dns.h"

/* ---- syscall numbers (per AutomationOS ABI -- identical to nc/ping/wget) -- */
#define SYS_WRITE       3    /* write(fd, buf, len)           fd1 = stdout   */
#define SYS_SLEEP       9    /* sleep(ms) -- real blocking ms sleep          */
#define SYS_YIELD       15   /* cooperative yield                            */
#define SYS_SOCKET      51   /* socket(SOCK_STREAM) -> fd                    */
#define SYS_CONNECT     52   /* connect(fd, ip_host_order, port) -> 0/neg    */
#define SYS_CLOSE_SK    55   /* close(fd) -> 0                               */
#define SYS_SOCK_POLL   58   /* pump the NIC RX/timers                       */

/* ---- socket type ------------------------------------------------------- */
#define SOCK_STREAM     1

/* ---- well-known fd ------------------------------------------------------ */
#define FD_STDOUT       1

/*
 * Socket return codes (mirror kernel/include/socket.h). SYS_CONNECT returns
 * one of these directly:
 *   SOCK_OK        ( 0  ) -> three-way handshake completed   => OPEN
 *   SOCK_ECONN     (-107) -> got a RST (connection refused)  => closed
 *   SOCK_ETIMEDOUT (-110) -> no reply within the kernel's    => filtered
 *                            TCP_CONNECT_MS budget
 * Any other negative value (e.g. SOCK_EINVAL -22, or a bad fd) is reported as
 * an error and does NOT count as open.
 */
#define SOCK_OK          0
#define SOCK_ECONN     (-107)
#define SOCK_ETIMEDOUT (-110)

/* ---- self-test reference: 10.0.2.2 -> host-order 0x0A000202 ------------ */
#define GATEWAY_IP      0x0A000202u
#define DISCARD_PORT    9

/* ---- default scan range ------------------------------------------------ */
#define DEF_START_PORT  1
#define DEF_END_PORT    1024

/*
 * Inter-port throttle.  After each port we pump the NIC once and sleep this
 * many milliseconds so a long scan stays gentle on the RX ring and yields the
 * CPU to other processes.  The per-port TIMEOUT itself is enforced by the
 * kernel's tcp_connect() budget (see header comment); this constant is only
 * the spacing BETWEEN ports, not the connect timeout.
 */
#define THROTTLE_MS     5

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
/* Tiny freestanding helpers (copied from nc.c)                          */
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
/* Single-port probe                                                     */
/* ===================================================================== */

/*
 * Attempt one bounded TCP connect to ip:port (ip in host byte order).
 * Returns the raw SYS_CONNECT result, or the negative socket()-failure code
 * if the socket could not even be created.  The fd is always closed before
 * returning, so no descriptors leak across a long scan.
 *
 * This is the heart of the scanner and mirrors nc.c's do_connect() path:
 * socket() -> ONE connect() (kernel-bounded, never looped) -> close().
 */
static long probe_port(unsigned int ip, long port)
{
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0)
        return fd;                          /* couldn't make a socket */

    /*
     * Single bounded connect.  We deliberately do NOT loop here: the kernel's
     * tcp_connect() returns within TCP_CONNECT_MS (RST -> immediate ECONN,
     * handshake -> immediate OK, silence -> ETIMEDOUT at the budget), so this
     * one call is guaranteed to come back and the scan can never hang.
     */
    long cr = sc(SYS_CONNECT, fd, (long)ip, port, 0, 0);

    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    return cr;
}

/*
 * Classify + print a single port's outcome.  Returns 1 if the port was OPEN,
 * 0 otherwise.  Only OPEN ports get a line by default (closed/filtered are
 * summarised by the caller); errors are surfaced so a wedged NIC is visible.
 */
static int report_port(unsigned int ip, long port, long cr)
{
    if (cr == SOCK_OK) {
        out_puts("  ");
        out_ip(ip);
        out_puts(":");
        out_num(port);
        out_puts("  OPEN\n");
        return 1;
    }
    /* closed (RST) and filtered (timeout) are the common, expected cases --
     * we don't spam a line per closed port, but a hard/unexpected error is
     * worth showing (e.g. socket() exhaustion, EINVAL). */
    if (cr != SOCK_ECONN && cr != SOCK_ETIMEDOUT) {
        out_puts("  ");
        out_ip(ip);
        out_puts(":");
        out_num(port);
        out_puts("  error rc=");
        out_num(cr);
        out_puts("\n");
    }
    return 0;
}

/* ===================================================================== */
/* Full-range scan driver                                                */
/* ===================================================================== */

/*
 * Resolve `host`, then sweep ports [start, end] inclusive, printing each OPEN
 * port as it is found and a final summary.  Returns a process exit code
 * (0 == scan ran to completion, non-zero == setup failure).
 */
static int do_scan(const char *host, long start, long end)
{
    unsigned int ip = 0;
    int dr = dns_resolve(host, &ip);
    if (dr != 0) {
        out_puts("netscan: cannot resolve host '");
        out_puts(host);
        out_puts("' (dns rc=");
        out_num(dr);
        out_puts(")\n");
        return 2;
    }

    out_puts("netscan: scanning ");
    out_ip(ip);
    out_puts(" ports ");
    out_num(start);
    out_puts("-");
    out_num(end);
    out_puts("\n");

    long open_count   = 0;
    long closed_count = 0;
    long filt_count   = 0;
    long err_count    = 0;

    for (long p = start; p <= end; p++) {
        long cr = probe_port(ip, p);

        if (cr == SOCK_OK)              open_count++;
        else if (cr == SOCK_ECONN)     closed_count++;
        else if (cr == SOCK_ETIMEDOUT) filt_count++;
        else                           err_count++;

        report_port(ip, p, cr);

        /* Pump the NIC, then a short throttle/yield between ports so a long
         * sweep stays gentle and cooperative.  Bounded + non-blocking; this
         * is spacing, not a connect timeout. */
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
        sc(SYS_SLEEP, THROTTLE_MS, 0, 0, 0, 0);
    }

    out_puts("netscan: done -- ");
    out_num(open_count);
    out_puts(" open, ");
    out_num(closed_count);
    out_puts(" closed, ");
    out_num(filt_count);
    out_puts(" filtered");
    if (err_count) {
        out_puts(", ");
        out_num(err_count);
        out_puts(" error");
    }
    out_puts("\n");

    return 0;
}

/* ===================================================================== */
/* Self-test (argc <= 1)                                                 */
/* ===================================================================== */

/*
 * Exercise the socket + DNS + connect API without depending on any particular
 * listening server (mirrors nc.c's self_test):
 *
 *   1. dns_resolve("10.0.2.2") must succeed and yield host-order 0x0A000202
 *      with NO network activity (pure dotted-quad parse).
 *   2. n_parse_port must accept "80" (->80) and reject "0" / "70000" / "x".
 *   3. probe_port() to 10.0.2.2:9 (discard) must RETURN a value (does not
 *      hang); ANY return is acceptable here -- the point is boundedness.
 *
 * Prints "NETSCAN SELFTEST: PASS" if the API behaves sanely, otherwise
 * "NETSCAN SELFTEST: FAIL <why>".  Returns 0 on PASS, non-zero on FAIL.
 */
static int self_test(void)
{
    /* --- 1. DNS dotted-quad parse (no network) --- */
    unsigned int ip = 0xDEADBEEFu;
    int dr = dns_resolve("10.0.2.2", &ip);
    if (dr != 0) {
        out_puts("NETSCAN SELFTEST: FAIL dns_resolve rc=");
        out_num(dr);
        out_puts("\n");
        return 1;
    }
    if (ip != GATEWAY_IP) {
        out_puts("NETSCAN SELFTEST: FAIL dns ip mismatch (got ");
        out_unum(ip);
        out_puts(", want 0x0A000202)\n");
        return 1;
    }

    /* --- 2. port parser sanity --- */
    if (n_parse_port("80") != 80) { out_puts("NETSCAN SELFTEST: FAIL parse 80\n"); return 1; }
    if (n_parse_port("0")  != -1) { out_puts("NETSCAN SELFTEST: FAIL parse 0\n");  return 1; }
    if (n_parse_port("70000") != -1) { out_puts("NETSCAN SELFTEST: FAIL parse 70000\n"); return 1; }
    if (n_parse_port("x")  != -1) { out_puts("NETSCAN SELFTEST: FAIL parse x\n");  return 1; }

    /* --- 3. bounded probe to the discard port (must RETURN, not hang) --- */
    long cr = probe_port(GATEWAY_IP, DISCARD_PORT);
    /* Don't assert the outcome: nothing need listen on :9.  The mere fact
     * that control returns here proves probe_port is bounded. */

    out_puts("NETSCAN SELFTEST: PASS (dns=10.0.2.2->0x0A000202, port-parse ok, "
             "probe 10.0.2.2:9 rc=");
    out_num(cr);
    out_puts(")\n");
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

    const char *host = argv[1];

    long start = DEF_START_PORT;
    long end   = DEF_END_PORT;

    if (argc == 2) {
        /* netscan HOST -> default range 1..1024 */
        /* start/end already set */
    } else if (argc == 3) {
        /* netscan HOST PORT -> single port */
        long p = n_parse_port(argv[2]);
        if (p < 0) {
            out_puts("netscan: invalid port '");
            out_puts(argv[2]);
            out_puts("'\n");
            return 1;
        }
        start = p;
        end   = p;
    } else {
        /* netscan HOST START END -> inclusive range */
        long ps = n_parse_port(argv[2]);
        long pe = n_parse_port(argv[3]);
        if (ps < 0 || pe < 0) {
            out_puts("netscan: invalid port range '");
            out_puts(argv[2]);
            out_puts("' '");
            out_puts(argv[3]);
            out_puts("'\n");
            return 1;
        }
        if (ps > pe) { long t = ps; ps = pe; pe = t; }   /* tolerate reversed range */
        start = ps;
        end   = pe;
    }

    return do_scan(host, start, end);
}
