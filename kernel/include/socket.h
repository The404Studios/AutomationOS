#ifndef SOCKET_H
#define SOCKET_H

/*
 * Minimal BSD-ish socket layer + UDP + TCP over the existing IPv4 stack.
 * =====================================================================
 *
 * Built strictly on top of kernel/net/net.c (Ethernet + ARP + IPv4 + ICMP):
 *
 *   socket.c  -- the socket table, the central RX poll/demux loop, the BSD-ish
 *                API (sock_socket/connect/send/recv/close, sendto/recvfrom).
 *   udp.c     -- UDP datagram TX (IP+UDP checksums) and inbound demux into the
 *                owning socket's datagram queue.
 *   tcp.c     -- a deliberately small active-open TCP: 3-way handshake
 *                (SYN / SYN-ACK / ACK), in-order stream send/recv with seq/ack
 *                tracking and retransmit-once, fixed window, FIN close.
 *
 * Everything is poll-mode and single-threaded, mirroring net.c. The caller (a
 * userspace test via syscalls, or in-kernel selftest) drives progress by
 * calling sock_poll() in a loop -- it pulls frames off the NIC via net_recv()
 * and dispatches IPv4/UDP/TCP into per-socket queues. net.c still gets to learn
 * ARP and answer ICMP from the same frames (net_recv does that internally).
 *
 * Byte order: on-wire fields big-endian via net_htons/net_htonl; IPs and ports
 * in this API are HOST byte order unless noted.
 *
 * Scope (this engineer): kernel/net/{tcp,udp,socket}.c + this header.
 */

#include "types.h"
#include "net.h"

/* ------------------------------------------------------------------ */
/* Socket types (the `type` arg to sock_socket).                       */
/* ------------------------------------------------------------------ */
#define SOCK_STREAM   1   /* TCP  */
#define SOCK_DGRAM    2   /* UDP  */

/* Limits. */
/* NET-P1-E: 16 -> 32. sock_t is ~45 KB (rxbuf-dominated), so 32 is ~1.4 MB
 * of ONE kmalloc in sock_init -- sized deliberately (64 would be ~2.9 MB of
 * mostly-wasted rings; half-opens scale through the NET-P1-A SYN side-table,
 * not this table). sock_init asserts the allocation loudly. */
#define SOCK_MAX            32     /* concurrent sockets                  */
#define SOCK_RXBUF_SIZE     32768  /* per-TCP-socket stream RX ring       */
/* NET-P1-D: 8 -> 16 (1472 B/slot: +~12 KB per socket; 32 would be waste). */
#define UDP_QUEUE_DEPTH     16     /* per-UDP-socket queued datagrams     */
#define UDP_DGRAM_MAX       1472   /* max UDP payload over 1500 MTU       */
#define TCP_MSS             1460   /* standard MSS for 1500-byte MTU      */

/* Socket errors (negative; match syscall.h conventions where they overlap). */
#define SOCK_OK         0
#define SOCK_EINVAL    -22
#define SOCK_ENOMEM    -12
#define SOCK_EBADF     -9
#define SOCK_ECONN     -107   /* not connected / connection failed       */
#define SOCK_EAGAIN    -11    /* would block / nothing available         */
#define SOCK_ETIMEDOUT -110

/* ------------------------------------------------------------------ */
/* A4 (SOCKET-PARITY-0): setsockopt/getsockopt + shutdown surface.     */
/* ------------------------------------------------------------------ */
/* Option level (only SOL_SOCKET is supported). */
#define SOL_SOCKET      1

/* Option names (SOL_SOCKET). All are int-valued; timeouts are in ms. */
#define SO_REUSEADDR    2   /* relax the bind() duplicate-port check     */
#define SO_TYPE         3   /* read-only: SOCK_STREAM | SOCK_DGRAM       */
#define SO_ERROR        4   /* read-only: 0 or a negative SOCK_E*        */
#define SO_BROADCAST    6   /* permit sendto() to a broadcast address    */
#define SO_RCVTIMEO     20  /* receive timeout in milliseconds (int)     */
#define SO_SNDTIMEO     21  /* send timeout in milliseconds (int)        */
#define SO_KEEPALIVE    9   /* enable TCP keepalive (stored; advisory)   */

/* shutdown(2) "how". */
#define SHUT_RD         0   /* disable further receives                  */
#define SHUT_WR         1   /* disable further sends (TCP: emit FIN)     */
#define SHUT_RDWR       2   /* both                                      */

/* ------------------------------------------------------------------ */
/* Wire-format transport headers (big-endian fields).                  */
/* ------------------------------------------------------------------ */
#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

typedef struct {
    uint16_t src_port;    /* big-endian */
    uint16_t dst_port;    /* big-endian */
    uint16_t length;      /* big-endian: UDP header + payload            */
    uint16_t checksum;    /* big-endian: 0 = none                        */
} PACKED udp_hdr_t;

typedef struct {
    uint16_t src_port;    /* big-endian */
    uint16_t dst_port;    /* big-endian */
    uint32_t seq;         /* big-endian */
    uint32_t ack;         /* big-endian */
    uint8_t  data_off;    /* high nibble = header length in 32-bit words */
    uint8_t  flags;       /* TCP flag bits                               */
    uint16_t window;      /* big-endian */
    uint16_t checksum;    /* big-endian */
    uint16_t urgent;      /* big-endian */
} PACKED tcp_hdr_t;

/* TCP flag bits. */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* TCP connection states (subset of RFC 793). */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,        /* passive open: awaiting incoming connections    */
    TCP_SYN_SENT,
    TCP_SYN_RCVD,      /* server: SYN received, SYN-ACK sent             */
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,      /* we sent FIN, awaiting ACK / peer FIN           */
    TCP_CLOSE_WAIT,    /* peer sent FIN; we may still send, must FIN     */
    TCP_TIME_WAIT,
    TCP_LAST_ACK       /* NET-HARDENING-2: passive close, awaiting ACK of our FIN */
} tcp_state_t;

/* ------------------------------------------------------------------ */
/* Pseudo-header checksum helper (shared by udp.c / tcp.c).            */
/* ------------------------------------------------------------------ */
/* One's-complement Internet checksum over an arbitrary buffer. */
uint16_t net_inet_checksum(const void* data, uint32_t len);

/*
 * Transport-layer checksum over the IPv4 pseudo-header + the given segment.
 * src_ip/dst_ip are HOST byte order; proto is IPPROTO_UDP/IPPROTO_TCP; seg/len
 * is the UDP/TCP header+payload as it sits on the wire. Returns the 16-bit
 * checksum already in host order (store it via net_htons()).
 */
uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip,
                                uint8_t proto, const void* seg, uint32_t len);

/* ------------------------------------------------------------------ */
/* Socket descriptor structure (opaque to callers; defined here so the */
/* three .c files in this subsystem can share it).                     */
/* ------------------------------------------------------------------ */
typedef struct sock sock_t;  /* forward declaration for self-reference */

struct sock {
    bool        used;
    int         type;          /* SOCK_STREAM | SOCK_DGRAM              */
    uint32_t    owner_pid;     /* PID that created this socket           */

    /* Connection 4-tuple (host byte order). */
    uint32_t    local_ip;
    uint16_t    local_port;
    uint32_t    remote_ip;
    uint16_t    remote_port;

    /* Server-side fields (for TCP LISTEN sockets). */
    int         backlog;       /* max pending connections               */
    sock_t*     accept_queue;  /* linked list of completed connections  */
    sock_t*     accept_next;   /* next socket in accept queue           */
    sock_t*     parent;        /* listening socket (for child sockets)  */

    /* --- UDP datagram queue (ring of fixed-size datagrams) --- */
    struct {
        uint32_t src_ip;
        uint16_t src_port;
        uint16_t len;
        uint8_t  data[UDP_DGRAM_MAX];
    } dgram[UDP_QUEUE_DEPTH];
    uint8_t     dq_head, dq_tail, dq_count;

    /* --- TCP state --- */
    tcp_state_t state;
    uint32_t    snd_nxt;       /* next seq we'll send                   */
    uint32_t    snd_una;       /* oldest unacked seq                    */
    uint32_t    rcv_nxt;       /* next seq we expect from peer          */
    uint16_t    snd_wnd;       /* peer's advertised window              */

    /* TCP stream receive ring. */
    uint8_t     rxbuf[SOCK_RXBUF_SIZE];
    uint32_t    rx_head, rx_tail, rx_used;

    /* Retransmit-once bookkeeping for the last data/SYN/FIN segment. */
    bool        rt_pending;    /* a segment is outstanding (unacked)    */
    bool        rt_done;       /* already retransmitted once            */
    uint64_t    rt_time_ms;    /* when the outstanding seg was sent     */
    uint32_t    rt_seq;        /* seq of the outstanding segment        */
    uint8_t     rt_flags;      /* flags of the outstanding segment      */
    uint16_t    rt_len;        /* payload length of outstanding segment */
    uint8_t     rt_data[TCP_MSS];

    bool        reset;         /* peer sent RST / fatal error           */
    bool        orphaned;      /* NET-HARDENING-2: app closed but our FIN is    */
                               /* un-acked; background tcp_tick retransmits +   */
                               /* reaps it (no owning fd)                       */

    /* --- A4 (SOCKET-PARITY-0): socket options + half-close state --- */
    uint8_t     so_reuseaddr;  /* SO_REUSEADDR: relax bind dup check     */
    uint8_t     so_broadcast;  /* SO_BROADCAST: allow broadcast sendto   */
    uint8_t     so_keepalive;  /* SO_KEEPALIVE: stored (advisory)        */
    uint8_t     shut_rd;       /* shutdown(SHUT_RD): recv returns EOF    */
    uint8_t     shut_wr;       /* shutdown(SHUT_WR): send is rejected    */
    uint32_t    so_rcvtimeo_ms;/* SO_RCVTIMEO (ms; 0 = none)             */
    uint32_t    so_sndtimeo_ms;/* SO_SNDTIMEO (ms; 0 = none)             */

    /* --- TCP-ROBUST 2MSL: when this socket entered TIME_WAIT (ms). --- */
    uint64_t    time_wait_ms;

    /* --- TCP-ROBUST adaptive RTO (RFC 6298, integer ms) --- */
    uint32_t    srtt_ms;       /* smoothed RTT (0 = no sample yet)        */
    uint32_t    rttvar_ms;     /* RTT variance                            */
    uint32_t    base_rto_ms;   /* current un-backed-off RTO (0 = none)    */

    /* --- TCP-ROBUST options negotiated from the peer's SYN/SYN-ACK --- */
    uint16_t    peer_mss;      /* peer's advertised MSS (0 = unknown)     */
    uint8_t     snd_wscale;    /* peer's window-scale shift (0..14)       */
    uint8_t     peer_sack_ok;  /* peer sent SACK-permitted                */
    uint8_t     peer_ts_ok;    /* peer sent the timestamp option          */
};

/* ------------------------------------------------------------------ */
/* Internal hooks used by socket.c <-> udp.c/tcp.c (not for callers).  */
/* ------------------------------------------------------------------ */

/* socket.c: build+transmit one IPv4 packet carrying an already-built UDP/TCP
 * segment (transport header + checksum included). dst_ip host order, proto
 * IPPROTO_UDP/TCP. 0 on success. Shared by udp.c / tcp.c. */
int ip_tx(uint32_t dst_ip, uint8_t proto, const void* seg, uint16_t seg_len);

/* socket.c: allocate an ephemeral local port (host order). */
uint16_t sock_alloc_port(void);

/* socket.c: locate the socket bound to a (local_port[,remote tuple]). */
sock_t* sock_find_udp(uint16_t local_port);
sock_t* sock_find_tcp(uint32_t remote_ip, uint16_t remote_port,
                      uint16_t local_port);

/* udp.c */
int  udp_send_to(sock_t* s, uint32_t dst_ip, uint16_t dst_port,
                 const void* data, uint16_t len);
void udp_input(uint32_t src_ip, uint32_t dst_ip,
               const uint8_t* seg, uint16_t seg_len);

/* tcp.c */
int  tcp_connect(sock_t* s, uint32_t dst_ip, uint16_t dst_port);
int  tcp_send(sock_t* s, const void* data, uint16_t len);
int  tcp_recv(sock_t* s, void* buf, uint16_t len);
int  tcp_close(sock_t* s);
void tcp_input(uint32_t src_ip, uint32_t dst_ip,
               const uint8_t* seg, uint16_t seg_len);
void tcp_tick(sock_t* s);   /* called from sock_poll for retransmit/timers */
/* NET-P1-A: half-open SYN side-table timers (SYN-ACK retransmit, TTL expiry,
 * orphan cleanup). Called once per sock_poll(). */
void tcp_synq_tick(void);
/* NET-P1-B: allocate + zero the heap-backed per-socket OOO buffers
 * ([SOCK_MAX][4] reassembly slots). Idempotent; called from sock_init(). */
void tcp_buffers_init(void);

/* ------------------------------------------------------------------ */
/* Public BSD-ish socket API (socket.c).                               */
/* ------------------------------------------------------------------ */

/* Reset the socket subsystem. Safe to call multiple times. */
void sock_init(void);

/*
 * Pump the NIC once: pull pending frames off net_recv() and demux IPv4/UDP/TCP
 * into socket queues, run TCP timers/retransmits. Returns the number of frames
 * processed this call. CALL THIS REPEATEDLY to make progress (it is the only
 * thing that drives RX and TCP state). Safe to call when net is down (no-op).
 */
int sock_poll(void);

/* POLL-SELECT-0: non-destructive readiness probe for poll()/select()/epoll.
 * Returns -1 if `fd` is not a live socket; otherwise a bitmask of the bits
 * below describing the socket's CURRENT state (no data is consumed). Caller is
 * expected to have pumped the stack (sock_poll) first so RX state is current. */
#define SOCKPOLL_READ   0x1   /* RX data queued, or EOF/peer-FIN (a read won't block) */
#define SOCKPOLL_WRITE  0x2   /* send window open (a write won't block)               */
#define SOCKPOLL_HUP    0x4   /* connection closing/closed (peer FIN seen)            */
#define SOCKPOLL_ERR    0x8   /* connection reset / error                             */
int sock_poll_bits(int fd);

/* Create a socket. type = SOCK_STREAM (TCP) or SOCK_DGRAM (UDP). Returns a
 * non-negative descriptor, or a negative SOCK_E* error. */
int sock_socket(int type);

/* Active-open a TCP connection (blocking-ish: pumps sock_poll internally up to
 * an internal timeout). For UDP this just records the default peer for send().
 * ip/port are host byte order. 0 on success, negative on error. */
int sock_connect(int s, uint32_t ip, uint16_t port);

/* Bind a UDP socket to a local port (host order) so it can recvfrom. For TCP
 * this sets the ephemeral local port to use on connect. 0 / negative. */
int sock_bind(int s, uint16_t local_port);

/* Mark a TCP socket as passive (listening for incoming connections).
 * backlog = max queued connections. Returns 0 / negative. */
int sock_listen(int s, int backlog);

/* Accept an incoming TCP connection. Returns new socket descriptor for the
 * connected client, or negative on error (SOCK_EAGAIN if none pending). */
int sock_accept(int s);

/* Send on a connected socket (TCP stream or UDP to the connected peer).
 * Returns bytes accepted (>=0) or negative error. */
int sock_send(int s, const void* buf, uint32_t len);

/* Receive on a connected socket. For TCP returns up to len stream bytes
 * available (0 if peer closed and drained); for UDP returns one datagram's
 * bytes. SOCK_EAGAIN if nothing is available yet. */
int sock_recv(int s, void* buf, uint32_t len);

/* UDP sendto: send a datagram to (ip,port) (host order). bytes / negative. */
int sock_sendto(int s, const void* buf, uint32_t len,
                uint32_t ip, uint16_t port);

/*
 * UDP recvfrom: copy one queued datagram into buf and, if non-NULL, the sender
 * (ip,port) into out_ip / out_port (host order). Returns datagram length,
 * SOCK_EAGAIN if none queued, or negative error.
 */
int sock_recvfrom(int s, void* buf, uint32_t len,
                  uint32_t* out_ip, uint16_t* out_port);

/* Close (TCP: send FIN if connected). 0 / negative. */
int sock_close(int s);

/* A4: half-close. how = SHUT_RD | SHUT_WR | SHUT_RDWR. After SHUT_RD a recv
 * returns 0 (EOF); after SHUT_WR a send is rejected (TCP also emits a FIN).
 * The socket stays open until sock_close. 0 / negative. */
int sock_shutdown(int s, int how);

/* A4: set/get a socket option. level must be SOL_SOCKET. Values are int
 * (timeouts in ms). Returns 0 / negative SOCK_E*. */
int sock_setsockopt(int s, int level, int optname, int value);
int sock_getsockopt(int s, int level, int optname, int* out_value);

/* Close all sockets owned by a dying process. Called from process_unref(). */
void sock_cleanup_process(uint32_t pid);

/* ------------------------------------------------------------------ */
/* In-kernel self test (optional; safe after net_init()).              */
/* ------------------------------------------------------------------ */
void sock_selftest(void);

/* ------------------------------------------------------------------ */
/* Proposed userspace syscall surface (REPORTED to the integrator).    */
/* ------------------------------------------------------------------ */
/*
 * Syscall numbers (AUTHORITATIVE -- see kernel/include/syscall.h):
 *
 *     #define SYS_SOCKET    51   sys_sock_socket(type)
 *     #define SYS_CONNECT   52   sys_sock_connect(s, ip, port)
 *     #define SYS_SEND      53   sys_sock_send(s, buf, len)
 *     #define SYS_RECV      54   sys_sock_recv(s, buf, len)
 *     #define SYS_CLOSE_SK  55   sys_sock_close(s)   (distinct from SYS_CLOSE=5)
 *     #define SYS_SENDTO    56   sys_sock_sendto(s, buf, len, ip, port)
 *     #define SYS_RECVFROM  57   sys_sock_recvfrom(s, buf, len, out_addr)
 *     #define SYS_SOCK_POLL 58   sys_sock_poll()  (optional explicit pump)
 *     #define SYS_BIND      76   sys_sock_bind(s, port)
 *     #define SYS_LISTEN    77   sys_sock_listen(s, backlog)
 *     #define SYS_ACCEPT    78   sys_sock_accept(s)
 *
 * Routed to the handlers below (kernel/net/socket.c). The handlers
 * match syscall_handler_t and use copy_from_user/copy_to_user.
 */

/* recvfrom out-address payload (filled by kernel). */
typedef struct {
    uint32_t ip;     /* host byte order */
    uint16_t port;   /* host byte order */
    uint16_t _pad;
} sock_addr_t;

/* NET-RESILIENCE-OBS: one row of the live socket table for netstat/ss
 * (SYS_SOCK_LIST=131). All host byte order. type=SOCK_STREAM|SOCK_DGRAM;
 * state=tcp_state_t (0 for UDP); owner_pid is the creating process. */
typedef struct {
    uint8_t  type;
    uint8_t  state;
    uint16_t local_port;
    uint16_t remote_port;
    uint16_t reserved;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint32_t owner_pid;
} sock_info_t;
_Static_assert(sizeof(sock_info_t) == 20, "sock_info_t ABI must be 20 bytes");

/* Fill `out` (up to `max` rows) with the live socket table; returns row count. */
int sock_get_list(sock_info_t* out, int max);

int64_t sys_sock_socket(uint64_t type, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_connect(uint64_t s, uint64_t ip, uint64_t port,
                         uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_send(uint64_t s, uint64_t buf, uint64_t len,
                      uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_recv(uint64_t s, uint64_t buf, uint64_t len,
                      uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_close(uint64_t s, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_sendto(uint64_t s, uint64_t buf, uint64_t len,
                        uint64_t ip, uint64_t port, uint64_t a6);
int64_t sys_sock_recvfrom(uint64_t s, uint64_t buf, uint64_t len,
                          uint64_t out_addr, uint64_t a5, uint64_t a6);
int64_t sys_sock_poll(uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_bind(uint64_t s, uint64_t port, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_listen(uint64_t s, uint64_t backlog, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_sock_accept(uint64_t s, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);
/* A4 (SOCKET-PARITY-0): SYS_SETSOCKOPT=125, SYS_GETSOCKOPT=126, SYS_SHUTDOWN=127. */
int64_t sys_sock_setsockopt(uint64_t s, uint64_t level, uint64_t optname,
                            uint64_t optval, uint64_t optlen, uint64_t a6);
int64_t sys_sock_getsockopt(uint64_t s, uint64_t level, uint64_t optname,
                            uint64_t optval, uint64_t optlen, uint64_t a6);
int64_t sys_sock_shutdown(uint64_t s, uint64_t how, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);
/* NET-RESILIENCE-OBS: SYS_SOCK_LIST=131 -- copy the socket table to userspace. */
int64_t sys_sock_list(uint64_t out_ptr, uint64_t max_entries, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* SOCKET_H */
