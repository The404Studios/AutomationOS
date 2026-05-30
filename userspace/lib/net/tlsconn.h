/*
 * tlsconn.h -- Unified plain-TCP / TLS connection abstraction (freestanding, ring 3).
 * ====================================================================================
 *
 * Provides a single read/write API (netconn_read / netconn_write) that works
 * identically whether the connection is plain HTTP or HTTPS, hiding the
 * plain-SYS_RECV vs tls_read dispatch from all callers.
 *
 * Usage:
 *   Declare a `static netconn nc;` (NOT a stack local -- see size note below).
 *   Call netconn_open() to resolve, connect, and optionally handshake.
 *   Then use netconn_read() / netconn_write() as if the transport were opaque.
 *   Call netconn_close() when done.
 *
 * SIZE WARNING
 * ------------
 *   netconn embeds a tls_conn_t, which is approximately 40 KB (dominated by the
 *   two 16640-byte record buffers inside tls_conn_t).  You MUST declare netconn
 *   objects as `static` or place them in BSS/data -- NEVER as automatic (stack)
 *   variables, and NEVER inside a struct that is itself stack-allocated.  Violating
 *   this will silently corrupt the stack on any platform with less than ~48 KB of
 *   stack headroom.
 *
 *     static netconn nc;               // OK: BSS
 *     static netconn nc_buf[4];        // OK: BSS array
 *     netconn nc;  (inside a function) // DANGER: stack overflow
 *
 * Build (flags passed DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/tlsconn.c -o tlsconn.o
 *   objdump -d tlsconn.o | grep fs:0x28   # MUST be empty (no stack canary)
 *
 * Link against dns.o and tls.o (and their transitive crypto deps).
 */

#ifndef AUTOMATIONOS_TLSCONN_H
#define AUTOMATIONOS_TLSCONN_H

#include "../tls/tls.h"   /* tls_conn_t, tls_client_connect, tls_read/write,
                             tls_close, tls_cert_trusted                      */

/*
 * netconn -- unified connection handle.
 *
 * ALWAYS declare in static storage (see SIZE WARNING above).
 *
 * Fields:
 *   fd       : the kernel TCP socket file descriptor.  -1 means closed/unused.
 *   is_tls   : 1 if a TLS handshake was performed; 0 for plain TCP.
 *   trusted  : 1 if tls_cert_trusted() reported the server cert chain was
 *              validated against the CA store.  Always 0 for plain connections.
 *              Callers that display a security indicator MUST gate it on this.
 *   tls      : embedded TLS state (~40 KB).  Only valid when is_tls == 1.
 *              Do not touch this field directly.
 */
typedef struct {
    int        fd;       /* TCP socket fd; -1 = closed/unused                */
    int        is_tls;   /* 1 = TLS active, 0 = plain TCP                    */
    int        trusted;  /* 1 = cert chain trusted (only meaningful if is_tls)*/
    tls_conn_t tls;      /* embedded TLS state (large; see SIZE WARNING)      */
} netconn;

/*
 * netconn_open -- resolve `host`, open a TCP socket to (host, port), and
 *                 optionally perform a TLS 1.2 handshake.
 *
 *   nc       : caller-provided netconn in static storage (zeroed or not -- we
 *              always initialise it before use).  MUST NOT be on the stack.
 *   host     : NUL-terminated hostname or dotted-quad IPv4 string.
 *              Passed as SNI server_name when use_tls is non-zero.
 *   port     : TCP port in host byte order (e.g. 80 or 443).
 *   use_tls  : 0 = plain TCP, non-zero = TLS (SNI = host).
 *
 * On success returns 0; nc->fd holds the socket, nc->is_tls is set, and
 * nc->trusted reflects the chain validation outcome (TLS only).
 * On failure returns a negative value; nc->fd is set to -1.
 *
 * Errors:
 *   -1  : dns_resolve() failed (host not found / no network)
 *   -2  : SYS_SOCKET failed (kernel could not create a socket)
 *   -3  : SYS_CONNECT failed (TCP connection refused / timed out)
 *   negative TLS_ERR_* : TLS handshake failed (see tls.h)
 */
int  netconn_open(netconn *nc, const char *host, unsigned short port,
                  int use_tls);

/*
 * netconn_write -- send `len` bytes from `buf` over the connection.
 *
 *   Plain TCP : calls SYS_SEND in a loop, retrying short writes until all
 *               bytes are accepted or an error occurs.
 *   TLS       : delegates to tls_write() which handles record framing.
 *
 * Returns `len` on complete success, or a negative value on error.
 * A return value in (0, len) means a partial write occurred on plain TCP after
 * all retries (kernel buffer exhausted unexpectedly -- treat as error).
 */
long netconn_write(netconn *nc, const void *buf, unsigned long len);

/*
 * netconn_read -- receive up to `cap` bytes into `buf`.
 *
 *   Plain TCP : calls SYS_SOCK_POLL then SYS_RECV.  If SYS_RECV returns
 *               -EAGAIN (-11), the loop yields (SYS_YIELD) and retries, up to
 *               a bounded iteration cap so a stalled peer cannot hang the OS.
 *   TLS       : delegates to tls_read() which handles record reassembly and
 *               decryption.
 *
 * Returns:
 *   > 0  : bytes placed into buf.
 *     0  : connection closed cleanly by the peer.
 *   < 0  : error (plain: kernel errno; TLS: TLS_ERR_* from tls.h, or -1).
 */
long netconn_read(netconn *nc, void *buf, unsigned long cap);

/*
 * netconn_close -- tear down the connection.
 *
 *   If is_tls: sends a TLS close_notify (best effort via tls_close()) before
 *              closing the socket, giving the peer a clean shutdown indication.
 *   Always calls SYS_CLOSE_SK on nc->fd (even on plain TCP).
 *   Sets nc->fd to -1 after closing.
 */
void netconn_close(netconn *nc);

/*
 * netconn_set_deadline -- install an ABSOLUTE wall-clock deadline (in
 *                         SYS_GET_TICKS_MS milliseconds) that bounds the time
 *                         spent in the blocking poll/retry loops of
 *                         netconn_read() and netconn_write().
 *
 *   deadline_ms : absolute millisecond timestamp (as returned by the
 *                 SYS_GET_TICKS_MS=40 syscall) past which the read/write loops
 *                 stop yielding and return early.  Pass 0 to DISABLE the
 *                 deadline (the default state), restoring the historical
 *                 behaviour of looping up to the fixed iteration cap.
 *
 * The deadline is file-scope, process-wide, and persists until changed; the
 * AutomationOS userspace is single-threaded so there is exactly one in-flight
 * connection at a time.  Callers that want a bounded total fetch (e.g. http.c)
 * set the deadline once before the request and clear it (pass 0) afterwards.
 *
 * When the deadline is hit:
 *   - netconn_read()  returns NC_ERR_TIMEDOUT (-110) instead of -1, so the
 *     caller can distinguish "deadline exceeded" from "iteration cap / other".
 *   - netconn_write() returns whatever partial byte count it managed (the
 *     caller treats a short write as an error exactly as before).
 *
 * The fixed iteration caps (NC_POLL_MAX / NC_SEND_RETRIES) remain in force as
 * an unconditional backstop even when no deadline is set, so a stalled peer can
 * never hang the OS regardless of whether a deadline was installed.
 */
void netconn_set_deadline(unsigned long deadline_ms);

/* Distinct return code from netconn_read() when the installed deadline (see
 * netconn_set_deadline) is exceeded before any data arrives.  Negative so it
 * is indistinguishable-as-an-error from other negatives to legacy callers, but
 * distinct enough for deadline-aware callers (http.c) to map to HTTP_ERR_TIMEO. */
#define NC_ERR_TIMEDOUT  (-110)

/*
 * netconn_selftest -- offline dispatch-logic sanity check (no network needed).
 *
 * Verifies:
 *   1. That the is_tls / plain dispatch flags work correctly on a netconn with
 *      is_tls=0 (plain mode) -- exercises struct initialisation and field logic.
 *   2. That dns_resolve() with a dotted-quad argument ("127.0.0.1") returns 0
 *      and fills a non-zero IP without sending any DNS query.
 *
 * Prints nothing.  Returns 0 on pass, -1 on any assertion failure.
 * Call this at startup before the first real connection to catch build/link
 * regressions without needing a live network.
 */
int  netconn_selftest(void);

#endif /* AUTOMATIONOS_TLSCONN_H */
