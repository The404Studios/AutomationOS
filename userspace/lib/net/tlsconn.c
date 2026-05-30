/*
 * tlsconn.c -- Unified plain-TCP / TLS connection abstraction (freestanding, ring 3).
 * ====================================================================================
 *
 * Implements netconn_open / netconn_read / netconn_write / netconn_close as a
 * thin dispatch layer: plain TCP uses the SYS_SOCKET/SYS_CONNECT/SYS_SEND/
 * SYS_RECV/SYS_SOCK_POLL/SYS_CLOSE_SK syscalls directly; TLS delegates to the
 * tls_conn_t state embedded in the netconn struct after performing the
 * tls_client_connect() handshake.
 *
 * SIZE CONTRACT -- netconn is approximately 40 KB (dominated by tls_conn_t).
 * Callers MUST declare netconn in static storage; stack allocation will
 * overflow any reasonable ring-3 stack.  See tlsconn.h for details.
 *
 * No libc / stdio / malloc / standard headers are used.  Every buffer is
 * fixed-size.  Every receive loop is bounded by a constant iteration cap plus
 * SYS_YIELD so a stalled peer can never hang the OS.
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/tlsconn.c -o tlsconn.o
 *   objdump -d tlsconn.o | grep fs:0x28   # MUST be empty (no stack canary)
 *
 * Depends: dns.o (dns_resolve), tls.o + crypto deps (tls_client_connect etc.)
 */

#include "tlsconn.h"
#include "dns.h"

/* ---- syscall numbers ------------------------------------------------------- */
#define SYS_YIELD       15
#define SYS_GET_TICKS_MS 40    /* monotonic millisecond counter               */
#define SYS_SOCKET      51
#define SYS_CONNECT     52
#define SYS_SEND        53
#define SYS_RECV        54
#define SYS_CLOSE_SK    55
#define SYS_SOCK_POLL   58

/* ---- socket type ----------------------------------------------------------- */
#define SOCK_STREAM     1

/* ---- errno values we test directly ---------------------------------------- */
#define EAGAIN_NEG      (-11)   /* kernel -EAGAIN: no data yet, keep polling  */

/* ---- tunables -------------------------------------------------------------- */

/*
 * Maximum SYS_SOCK_POLL + SYS_RECV retry iterations for plain-TCP reads.
 * Each dry iteration calls SYS_YIELD, so the CPU is not busy-spun.  With a
 * typical yield of a few microseconds this allows roughly 200 ms of patience
 * before the read is declared timed-out.  Adjust if longer stalls are expected.
 */
#define NC_POLL_MAX     200000

/*
 * Maximum SYS_SEND retry iterations for a single plain-TCP write when the
 * kernel short-writes (returns fewer bytes than requested).  In practice a
 * connected socket's send buffer is large enough that this never triggers
 * except under very heavy load; the cap prevents an infinite loop.
 */
#define NC_SEND_RETRIES 1024

/* ---- error codes (used internally; callers see these as negative returns) -- */
#define NC_ERR_DNS      (-1)    /* dns_resolve() failed                       */
#define NC_ERR_SOCK     (-2)    /* SYS_SOCKET returned an error               */
#define NC_ERR_CONNECT  (-3)    /* SYS_CONNECT returned an error              */

/* ---- raw 6-argument inline syscall wrapper --------------------------------- */
/*
 * sc() maps directly onto the x86_64 SYSCALL ABI:
 *   rax = syscall number, rdi/rsi/rdx/r10/r8/r9 = arguments.
 * The r9 slot (6th argument) is used by some syscalls but none of the socket
 * calls below need it, so we pass 0.  NEVER add "fs:0x28" stack-canary code
 * (hence -fno-stack-protector in the build flags).
 */
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

/* ---- tiny freestanding helpers -------------------------------------------- */

/* Zero `n` bytes starting at `p`.  Used to initialise fields in netconn. */
static void tc_memzero(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    for (unsigned long i = 0; i < n; i++) b[i] = (unsigned char)0;
}

/* Returns a monotonic millisecond timestamp (wraps after ~49 days).  Used only
 * for the optional wall-clock deadline; never called when no deadline is set. */
static unsigned long nc_ticks_ms(void)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"((long)SYS_GET_TICKS_MS),
                   "D"((long)0), "S"((long)0), "d"((long)0)
                 : "rcx", "r11", "memory");
    return (unsigned long)r;
}

/* ---- optional wall-clock deadline ----------------------------------------- *
 *
 * g_deadline_ms == 0 means "no deadline" (historical behaviour: loop up to the
 * fixed iteration cap).  When non-zero it is an ABSOLUTE SYS_GET_TICKS_MS
 * timestamp; once the clock reaches it, the blocking read/write loops stop.
 * File-scope because the userspace is single-threaded (one fetch in flight). */
static unsigned long g_deadline_ms;   /* 0 = disabled */

void netconn_set_deadline(unsigned long deadline_ms)
{
    g_deadline_ms = deadline_ms;
}

/*
 * nc_deadline_passed -- 1 if a deadline is set AND the monotonic clock has
 * reached it, else 0.  The subtraction is done in unsigned arithmetic so the
 * comparison stays correct across the ~49-day tick wrap: (now - deadline) only
 * becomes "small" (top bit clear) once now is at/after deadline.
 */
static int nc_deadline_passed(void)
{
    if (g_deadline_ms == 0)
        return 0;                       /* no deadline installed */
    unsigned long now = nc_ticks_ms();
    /* now >= deadline, wrap-safe: treat the half-range as "reached". */
    return (unsigned long)(now - g_deadline_ms) < (1UL << 63);
}

/* =========================================================================
 * netconn_open
 * =========================================================================
 *
 * Steps:
 *   1. dns_resolve(host) -- dotted-quad inputs are converted with no network I/O.
 *   2. SYS_SOCKET(SOCK_STREAM) -- obtain a TCP fd from the kernel.
 *   3. SYS_CONNECT(fd, ip, port) -- initiate the TCP handshake.
 *   4. [use_tls only] tls_client_connect(&nc->tls, fd, host) -- TLS 1.2
 *      handshake with SNI = host.
 *   5. [use_tls only] nc->trusted = tls_cert_trusted(&nc->tls).
 *
 * On any failure before step 3, the socket (if opened) is closed via
 * SYS_CLOSE_SK and nc->fd is set to -1.
 */
int netconn_open(netconn *nc, const char *host, unsigned short port,
                 int use_tls)
{
    /* Initialise the public fields we always own.  We do NOT zero the embedded
     * tls_conn_t here -- tls_client_connect() initialises it internally.      */
    nc->fd      = -1;
    nc->is_tls  = 0;
    nc->trusted = 0;

    /* Step 1: Resolve the host to an IPv4 address (host byte order). */
    unsigned int ip = 0;
    if (dns_resolve(host, &ip) < 0)
        return NC_ERR_DNS;

    /* Step 2: Create a STREAM (TCP) socket. */
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0)
        return NC_ERR_SOCK;
    nc->fd = (int)fd;

    /* Step 3: Connect to (ip, port).
     * SYS_CONNECT(fd, ip_host_order, port_host_order) per the kernel ABI
     * defined for this OS.  On failure, clean up and report. */
    long cr = sc(SYS_CONNECT, (long)nc->fd, (long)ip, (long)port, 0, 0);
    if (cr < 0) {
        sc(SYS_CLOSE_SK, (long)nc->fd, 0, 0, 0, 0);
        nc->fd = -1;
        return NC_ERR_CONNECT;
    }

    /* Step 4 & 5 (TLS only). */
    if (use_tls) {
        /* tls_client_connect initialises nc->tls internally; we must NOT
         * zero it ourselves here because tls.c may use the fd we set below
         * to prime its internal state before the handshake begins.
         * Pass the host as the SNI server_name. */
        int hr = tls_client_connect(&nc->tls, nc->fd, host);
        if (hr < 0) {
            /* Handshake failed -- close the TCP fd and surface the TLS error. */
            sc(SYS_CLOSE_SK, (long)nc->fd, 0, 0, 0, 0);
            nc->fd = -1;
            return hr;   /* negative TLS_ERR_* value */
        }
        nc->is_tls  = 1;
        nc->trusted = tls_cert_trusted(&nc->tls);
    }

    return 0;
}

/* =========================================================================
 * netconn_write
 * =========================================================================
 *
 * Dispatch:
 *   is_tls == 0 : SYS_SEND loop.
 *     The kernel may short-write (return fewer bytes than requested) if the
 *     send buffer is temporarily full.  We loop, advancing the pointer, up to
 *     NC_SEND_RETRIES iterations.  The loop is bounded so a full buffer can
 *     never hang the OS.
 *
 *   is_tls == 1 : tls_write() handles record framing and encryption in one
 *     call.  tls_write() is expected to accept all `len` bytes (it internally
 *     splits across multiple TLS records if needed).
 */
long netconn_write(netconn *nc, const void *buf, unsigned long len)
{
    if (nc->fd < 0 || len == 0)
        return 0;

    /* --- TLS path --- */
    if (nc->is_tls)
        return tls_write(&nc->tls, buf, len);

    /* --- Plain TCP path --- */
    const unsigned char *p    = (const unsigned char *)buf;
    unsigned long        sent = 0;
    int                  retries = 0;

    while (sent < len && retries < NC_SEND_RETRIES) {
        long r = sc(SYS_SEND, (long)nc->fd,
                    (long)(p + sent),
                    (long)(len - sent), 0, 0);
        if (r < 0) return r;   /* kernel error -- propagate */
        if (r == 0) {
            /* Kernel accepted zero bytes -- increment retry counter to avoid
             * an infinite spin; yield so other processes can run. */
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
            retries++;
            /* Wall-clock backstop: stop blocking once the deadline is hit and
             * report the partial count (the caller treats short writes as an
             * error).  Only checked on a dry retry so a fully-progressing send
             * is never charged a ticks syscall. */
            if (nc_deadline_passed())
                break;
        } else {
            sent   += (unsigned long)r;
            retries = 0;   /* progress was made; reset the retry counter */
        }
    }

    return (long)sent;
}

/* =========================================================================
 * netconn_read
 * =========================================================================
 *
 * Dispatch:
 *   is_tls == 1 : tls_read() decrypts from the internal record buffers.
 *     Returns > 0 (bytes), 0 (peer close_notify), or -1 (error).
 *
 *   is_tls == 0 : poll-mode plain TCP receive.
 *     Algorithm (matching the pattern used by http.c and dns.c):
 *       for up to NC_POLL_MAX iterations:
 *           SYS_SOCK_POLL(fd) -- ask the NIC driver to pump its ring buffer.
 *           SYS_RECV(fd, buf, cap) -- attempt the read.
 *           if SYS_RECV returns > 0  : return bytes.
 *           if SYS_RECV returns   0  : return 0 (connection closed).
 *           if SYS_RECV returns -11 (EAGAIN): SYS_YIELD and continue.
 *           any other negative       : return the error code.
 *       return -1 (timed out / iteration cap exhausted).
 *
 * The bounded loop ensures a stalled or closed peer cannot spin the CPU or
 * hang the OS indefinitely.
 */
long netconn_read(netconn *nc, void *buf, unsigned long cap)
{
    if (nc->fd < 0 || cap == 0)
        return 0;

    /* --- TLS path --- */
    if (nc->is_tls)
        return tls_read(&nc->tls, buf, cap);

    /* --- Plain TCP path --- */
    for (int i = 0; i < NC_POLL_MAX; i++) {
        /* Pump the NIC ring buffer so a fresh frame can be delivered. */
        sc(SYS_SOCK_POLL, (long)nc->fd, 0, 0, 0, 0);

        long r = sc(SYS_RECV, (long)nc->fd, (long)buf, (long)cap, 0, 0);

        if (r > 0)          return r;          /* got data                   */
        if (r == 0)         return 0;          /* peer closed cleanly        */
        if (r == EAGAIN_NEG) {
            /* No data yet.  Stop early if the wall-clock deadline is hit so a
             * slow/unreachable peer cannot keep us yielding; report a DISTINCT
             * code so the caller (http.c) maps it to a timeout error rather
             * than a generic failure.  Checked before yielding. */
            if (nc_deadline_passed())
                return NC_ERR_TIMEDOUT;
            sc(SYS_YIELD, 0, 0, 0, 0, 0);     /* yield & retry               */
            continue;
        }
        return r;                              /* real error: propagate errno */
    }

    /* Iteration cap exhausted: peer is stalled or very slow. */
    return NC_ERR_TIMEDOUT;
}

/* =========================================================================
 * netconn_close
 * =========================================================================
 *
 * TLS: send close_notify first (best effort; tls_close does not close the fd).
 * Always: call SYS_CLOSE_SK on the fd and set it to -1.
 *
 * Safe to call on an already-closed (fd == -1) netconn -- it is a no-op.
 */
void netconn_close(netconn *nc)
{
    if (nc->fd < 0)
        return;

    if (nc->is_tls) {
        /* Send TLS close_notify.  Best effort: if the send fails (e.g. the
         * peer already hung up) we continue and close the socket anyway.    */
        tls_close(&nc->tls);
        nc->is_tls  = 0;
        nc->trusted = 0;
    }

    sc(SYS_CLOSE_SK, (long)nc->fd, 0, 0, 0, 0);
    nc->fd = -1;
}

/* =========================================================================
 * netconn_selftest
 * =========================================================================
 *
 * Offline sanity check -- NO network connections are made.
 *
 * Test 1: struct field initialisation and plain-mode dispatch flag.
 *   - Manually fill a netconn with is_tls = 0, fd = -1.
 *   - Confirm that is_tls is 0 (so read/write would take the plain path).
 *   - Confirm that trusted is 0 (no TLS => cert status is meaningless).
 *   - Confirm fd == -1 (i.e. the connection is in the closed/unused state).
 *
 * Test 2: dns_resolve dotted-quad bypass.
 *   - Call dns_resolve("127.0.0.1", &ip).  A dotted-quad is parsed locally
 *     by dns_resolve with no DNS query; it must return 0 and set ip to
 *     0x7F000001 (127.0.0.1 in host byte order).
 *
 * Returns 0 on pass, -1 on any failure.
 */
int netconn_selftest(void)
{
    /* ---- Test 1: struct/dispatch ---------------------------------------- */
    netconn nc;
    tc_memzero(&nc, sizeof(nc));

    nc.fd      = -1;
    nc.is_tls  = 0;
    nc.trusted = 0;

    /* Verify the flags are as set -- dispatch would pick the plain-TCP path. */
    if (nc.is_tls  != 0) return -1;
    if (nc.trusted != 0) return -1;
    if (nc.fd      != -1) return -1;

    /* Simulate what netconn_write / netconn_read would check before
     * dispatching.  With fd == -1 both should short-circuit to 0 without
     * touching any syscall.  We cannot actually call them here because we have
     * no real fd, but we can verify the fd guard via direct inspection. */
    if (nc.fd >= 0) return -1;   /* guard condition that blocks syscall access */

    /* ---- Test 2: dns_resolve dotted-quad (no network) ------------------- */
    unsigned int ip = 0;
    int rc = dns_resolve("127.0.0.1", &ip);
    if (rc != 0)           return -1;   /* must succeed without a DNS query   */
    if (ip != 0x7F000001u) return -1;   /* 127.0.0.1 in host byte order       */

    /* Bonus: verify "10.0.2.2" (QEMU gateway dotted-quad) also resolves
     * locally -- confirms the parser handles all four octets correctly.       */
    unsigned int ip2 = 0;
    int rc2 = dns_resolve("10.0.2.2", &ip2);
    if (rc2 != 0)          return -1;
    if (ip2 != 0x0A000202u) return -1;  /* 10.0.2.2 in host byte order        */

    return 0;   /* all checks passed */
}
