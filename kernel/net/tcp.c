/*
 * tcp.c -- hardened TCP (active open only), rev-2.
 * =================================================
 *
 * Implements RFC 793 active-open TCP robust enough to talk to real internet
 * servers (large HTTPS responses, Google's stack, out-of-order/retransmitted
 * segments, proper window management):
 *
 *   - Active open: SYN -> (SYN|ACK) -> ACK 3-way handshake.
 *   - Segmentation: tcp_send() breaks large payloads into TCP_MSS chunks.
 *   - Reliable send: multi-retransmit with exponential back-off (up to
 *     TCP_RTO_MAX_RETRIES), respects peer's advertised window.
 *   - Receive: in-order bytes queued into socket rx ring; out-of-order
 *     segments saved in a static side-table OOO buffer and merged when
 *     the hole is filled.  Duplicate/already-acked data silently dropped
 *     with a fresh ACK.
 *   - Window advertisement: we advertise the free space in our rx ring so
 *     the server knows when it can keep sending (dynamic receive window).
 *   - ACKing: every data segment is acknowledged promptly (no delayed ACK).
 *   - Close: send FIN, handle peer FIN (CLOSE_WAIT -> TIME_WAIT), drain.
 *   - RST: marks socket reset; tcp_recv() returns SOCK_ECONN.
 *   - Correct TCP checksums and all byte-order conversions via net_htons/l.
 *
 * CHANGED from rev-1 (deliberate-minimal):
 *   1. tcp_xmit() advertises the real free space in our rx ring (rcv_wnd),
 *      not the peer's window in snd_wnd.
 *   2. tcp_send() segments payloads > TCP_MSS into multiple back-to-back
 *      segments instead of truncating; returns total bytes accepted.
 *   3. tcp_tick() implements exponential back-off retransmit up to
 *      TCP_RTO_MAX_RETRIES (was retransmit-once then give-up).
 *   4. tcp_input() ESTABLISHED: out-of-order data saved in per-socket OOO
 *      side-table (tcp_ooo[]) and merged when gap fills, so multi-segment
 *      responses are fully delivered.  Duplicate data (seq < rcv_nxt) is
 *      dropped with a fresh ACK.  ACK comparisons use SEQ32_GEQ() for
 *      correct wraparound.
 *   5. RST in ESTABLISHED causes s->reset=true but does NOT close the state
 *      machine until tcp_recv() is called -- callers see SOCK_ECONN rather
 *      than getting stuck in a half-open state.
 *   6. SYN-ACK ack check uses SEQ32_GEQ() for wraparound safety.
 *   7. Timeouts scaled up: TCP_CONNECT_MS=8000, TCP_CLOSE_MS=4000,
 *      initial RTO=1000 ms (Internet-realistic RTTs can exceed 500 ms).
 *
 * LIMITATIONS (documented, not in scope) -- REFRESHED by NET-GAPS (several
 * older entries here were fixed by later bricks and are now features above):
 *   - No SACK (selective acknowledgement) -- parsed from the peer, unused.
 *   - No window scaling on our side (deliberate; see tcp_parse_options).
 *   - No Nagle / delayed-ACK.
 *   - Passive open does not record the peer's MSS from its SYN (the synq
 *     entry doesn't store it), so servers segment at our own TCP_MSS.
 *   - Simultaneous open not hardened (rare in active-open clients).
 *   - Retransmission is go-back-N from snd_una (no out-of-order TX repair).
 *   - No IPv6.
 *
 * Driven by socket.c: tcp_input() is called for inbound TCP segments and
 * tcp_tick() is called every sock_poll() to run the retransmit timer.
 */

#include "../include/socket.h"
#include "../include/net.h"
#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/string.h"
#include "../include/drivers.h"   /* timer_get_ticks_ms */
#include "../include/mem.h"       /* kmalloc (heap-backed OOO side-table)  */
#include "../include/rng.h"       /* rng_u64 (unpredictable ISN)           */

/* ------------------------------------------------------------------ */
/* Timeouts and retransmit parameters                                  */
/* ------------------------------------------------------------------ */
#define TCP_RTO_INIT_MS       1000    /* initial retransmit timeout (ms)    */
#define TCP_RTO_MAX_MS        30000   /* maximum RTO after back-off         */
#define TCP_RTO_MAX_RETRIES   8       /* give up after this many retransmits*/
#define TCP_CONNECT_MS        8000    /* total connect budget (ms)          */
#define TCP_CLOSE_MS          4000    /* wait for FIN/ACK on close          */
#define TCP_CLOSE_DRAIN_ITERS 4096    /* NET-HARDENING-2: short close-drain spin --
                                       * loopback completes in a few iterations; a
                                       * lossy close defers to background tcp_tick */
/* Frozen-tick backstop: any wall-clock-bounded spin INSIDE a syscall must also
 * cap consecutive iterations where now_ms() did not advance, because the PIT IRQ
 * (the only thing that bumps the ms clock) cannot fire while the syscall runs
 * with IF=0 -- so a pure wall-clock guard can never trip. Same value + pattern as
 * resolve_mac's iter_cap (socket.c) and the zero-window persist loop below. */
#define TCP_FROZEN_SPIN_CAP   200000
#define TCP_TIMEWAIT_MS       60000   /* 2MSL linger before TIME_WAIT->CLOSED*/
#define TCP_RTO_MIN_MS        200     /* RFC 6298 RTO floor (ms)            */

/* sock_poll lives in socket.c. */
int sock_poll(void);

/* ------------------------------------------------------------------ */
/* Sequence-number arithmetic (32-bit wraparound-safe)                 */
/* ------------------------------------------------------------------ */
/* Returns true if a >= b in TCP sequence space (handles wrap). */
static inline bool SEQ32_GEQ(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) >= 0;
}
/* Returns true if a > b in TCP sequence space. */
static inline bool SEQ32_GT(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

/* ------------------------------------------------------------------ */
/* Out-of-order segment side-table                                      */
/* ------------------------------------------------------------------ */
/*
 * NET-P1-B: a 4-DEEP out-of-order buffer per socket (was a single slot, so
 * any reordering of 2+ segments stalled RX until the peer retransmitted).
 * sock_t stays untouched; the buffer is a side-table indexed by socket slot.
 *
 * HEAP-BACKED, not .bss: 16 sockets x 4 slots x ~1.5 KB is ~94 KB -- a
 * static array that size risks pushing kernel .bss into the GRUB-placed
 * initrd (the exact reason the socket table itself moved to kmalloc; see
 * socket.c). Allocated once via tcp_buffers_init() from sock_init(). If the
 * allocation ever fails the stack degrades CORRECTLY: OOO segments are
 * simply dropped and the peer retransmits them in order.
 */
#define TCP_OOO_SLOTS   SOCK_MAX      /* rows: one per possible socket      */
#define TCP_OOO_DEPTH   4             /* slots per socket (NET-P1-B)        */

typedef struct {
    bool     valid;
    uint32_t seq;                     /* sequence number of first byte      */
    uint16_t len;                     /* payload length                     */
    uint8_t  data[TCP_MSS];           /* payload (capped at MSS)            */
} tcp_ooo_slot_t;

static tcp_ooo_slot_t (*tcp_ooo)[TCP_OOO_DEPTH] = NULL;   /* [SOCK_MAX][4] */

/* ------------------------------------------------------------------ */
/* NET-GAPS N1: per-socket TX retransmit queue                          */
/* ------------------------------------------------------------------ */
/*
 * The old design kept ONE retransmittable segment (rt_data): with segments
 * pipelined, loss of anything but the tail was unrepairable (stall -> RTO
 * give-up -> reset). This ring remembers every UNACKED payload byte in
 * [snd_una, snd_nxt); retransmission always resumes at snd_una (go-back-N
 * head, resegmented to the effective MSS -- legal TCP). The rt_* single slot
 * REMAINS for control segments (SYN via tcp_connect, FIN via tcp_close) and
 * as the degrade path if this heap allocation ever fails.
 *
 * HEAP-BACKED like the OOO table: 32 sockets x ~16.4 KB = ~525 KB.
 * (Types + globals live up here because tcp_buffers_init just below
 * allocates them; the ring helpers follow now_ms further down.)
 */
#define TCP_TXQ_SIZE 16384
typedef struct {
    uint8_t  buf[TCP_TXQ_SIZE];  /* ring of unacked payload bytes            */
    uint32_t head;               /* ring offset of the byte at seq snd_una   */
    uint32_t used;               /* payload bytes in [snd_una, snd_nxt)      */
    uint64_t sent_ms;            /* when the current oldest byte was (re)sent*/
    uint8_t  rtx_count;          /* RTO retries for the current oldest       */
    uint32_t rto_ms;             /* current (backed-off) queue RTO           */
    /* NET-GAPS N4: congestion control (RFC 5681-lite, byte-counted). */
    uint32_t cwnd;               /* congestion window (bytes)                */
    uint32_t ssthresh;           /* slow-start threshold (bytes)             */
} tcp_txq_t;

static tcp_txq_t* tcp_txq = NULL;          /* [SOCK_MAX], kmalloc'd         */
static uint8_t    tcp_rtx_buf[TCP_MSS];    /* single-threaded linearize buf */

#define TCP_CWND_INIT_B   (4u * TCP_MSS)   /* IW: parity with the old fixed */
                                           /* 4-segment pipeline            */
#define TCP_SSTHRESH_INIT 65535u           /* start in slow start; cap cwnd */
                                           /* at the 16-bit window world    */

/* Allocate + zero the OOO side-table (idempotent; re-call re-zeroes). */
void tcp_buffers_init(void) {
    if (tcp_ooo == NULL) {
        tcp_ooo = (tcp_ooo_slot_t(*)[TCP_OOO_DEPTH])
            kmalloc(sizeof(tcp_ooo_slot_t) * TCP_OOO_SLOTS * TCP_OOO_DEPTH);
        if (tcp_ooo == NULL) {
            kprintf("[TCP] ooo buffer alloc failed: degrading to no-OOO\n");
        }
    }
    if (tcp_ooo)
        memset(tcp_ooo, 0,
               sizeof(tcp_ooo_slot_t) * TCP_OOO_SLOTS * TCP_OOO_DEPTH);

    /* NET-GAPS N1: the TX retransmit queues (heap, ~525 KB). Alloc failure
     * degrades CORRECTLY: tcp_send falls back to the single-slot stop-and-
     * wait path (rt_data), exactly the pre-N1 behavior. */
    if (tcp_txq == NULL) {
        tcp_txq = (tcp_txq_t*)kmalloc(sizeof(tcp_txq_t) * SOCK_MAX);
        if (tcp_txq == NULL)
            kprintf("[TCP] txq alloc failed: degrading to tail-only retransmit\n");
    }
    if (tcp_txq)
        memset(tcp_txq, 0, sizeof(tcp_txq_t) * SOCK_MAX);
}

/* Invalidate every OOO slot belonging to socket index `idx`. */
static void tcp_ooo_clear(int idx) {
    if (!tcp_ooo) return;
    for (int k = 0; k < TCP_OOO_DEPTH; k++) tcp_ooo[idx][k].valid = false;
}

/*
 * Per-socket retransmit retry counter.  sock_t only has rt_done (bool).
 * We extend it with a separate count table so we can do multi-retry
 * exponential back-off without touching sock_t.
 */
static uint8_t  tcp_rt_count[TCP_OOO_SLOTS];   /* retransmit attempt count */
static uint32_t tcp_rt_rto_ms[TCP_OOO_SLOTS];  /* current RTO for backoff  */
/* TCP-ROBUST (RFC 5681 fast retransmit): consecutive duplicate-ACK count per
 * socket. Reset on every fresh arm/disarm and whenever snd_una advances. */
static uint8_t  tcp_dupack[TCP_OOO_SLOTS];

/*
 * Resolve a sock_t* to its stable slot index by scanning the socket
 * table.  We declare sock_table as extern so tcp.c can walk it; the
 * table lives in socket.c.  If the layout ever changes, update this.
 *
 * Alternatively, if the socket.c environment does not export sock_table,
 * fall back to a pointer-comparison scan.  We use a small helper that
 * tries the known symbol and falls back gracefully.
 */
/* socket.c keeps its table as a kmalloc'd array exposed via this accessor. */
extern sock_t* sock_table_base(void);

static int sock_index(const sock_t* s) {
    sock_t* base = sock_table_base();
    if (!base) return 0;
    int idx = (int)(s - base);
    if (idx >= 0 && idx < SOCK_MAX) return idx;
    return 0;   /* safe fallback: share slot 0 if pointer math fails */
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
#ifdef NET_SELFTEST
/* RIG-TIME-HOOK: the boot test rig never calls sock_poll(), so timer-driven
 * features (2MSL TIME_WAIT, RTO, ARP aging) cannot fire by inject->assert. This
 * offset lets the rig fast-forward the clock deterministically (no wall-clock
 * wait) so a directly-invoked tcp_tick() sees a deadline as expired. It is 0 in
 * normal NET_SELFTEST runs (existing NETP1 tests do not advance it). Compiled
 * out entirely without NET_SELFTEST -> the default kernel is byte-identical. */
static uint64_t g_rig_clock_offset_ms = 0;
void     tcp_rig_advance_ms(uint64_t ms) { g_rig_clock_offset_ms += ms; }
void     tcp_rig_clock_reset(void)       { g_rig_clock_offset_ms = 0; }
static uint64_t now_ms(void) { return timer_get_ticks_ms() + g_rig_clock_offset_ms; }
uint64_t tcp_rig_now(void)   { return now_ms(); }
#else
static uint64_t now_ms(void) { return timer_get_ticks_ms(); }
#endif

/* ------------------------------------------------------------------ */
/* NET-GAPS N1: TX-queue ring helpers (type + globals above, with the  */
/* OOO table; these live below sock_index()/now_ms() which they use).  */
/* ------------------------------------------------------------------ */
static inline tcp_txq_t* txq_of(sock_t* s) {
    return tcp_txq ? &tcp_txq[sock_index(s)] : NULL;
}

/* Reset per-connection TX/congestion state (active connect + passive promote). */
static void txq_reset(sock_t* s) {
    tcp_txq_t* q = txq_of(s);
    if (!q) return;
    q->head = 0;  q->used = 0;
    q->sent_ms = 0;
    q->rtx_count = 0;
    q->rto_ms = TCP_RTO_INIT_MS;
    q->cwnd = TCP_CWND_INIT_B;
    q->ssthresh = TCP_SSTHRESH_INIT;
}

static uint32_t txq_free_bytes(sock_t* s) {
    tcp_txq_t* q = txq_of(s);
    return q ? (TCP_TXQ_SIZE - q->used) : 0;
}

/* Append payload (caller checked space). Arms the queue timer if it was empty. */
static void txq_append(sock_t* s, const uint8_t* p, uint16_t n) {
    tcp_txq_t* q = txq_of(s);
    if (!q || n == 0) return;
    uint32_t tail = (q->head + q->used) % TCP_TXQ_SIZE;
    uint32_t c1 = TCP_TXQ_SIZE - tail;
    if (c1 > n) c1 = n;
    memcpy(q->buf + tail, p, c1);
    if (n - c1) memcpy(q->buf, p + c1, (uint32_t)(n - c1));
    if (q->used == 0) {
        q->sent_ms   = now_ms();
        q->rtx_count = 0;
        q->rto_ms    = s->base_rto_ms ? s->base_rto_ms : TCP_RTO_INIT_MS;
    }
    q->used += n;
}

/* Drop the NEWEST n bytes (rollback right after an append whose xmit failed). */
static void txq_rollback(sock_t* s, uint16_t n) {
    tcp_txq_t* q = txq_of(s);
    if (q && q->used >= n) q->used -= n;
}

/* Copy the oldest min(used, max_n) bytes (the bytes at snd_una) into dst. */
static uint16_t txq_peek_oldest(sock_t* s, uint8_t* dst, uint16_t max_n) {
    tcp_txq_t* q = txq_of(s);
    if (!q || q->used == 0) return 0;
    uint16_t n = (q->used < max_n) ? (uint16_t)q->used : max_n;
    uint32_t c1 = TCP_TXQ_SIZE - q->head;
    if (c1 > n) c1 = n;
    memcpy(dst, q->buf + q->head, c1);
    if (n - c1) memcpy(dst + c1, q->buf, (uint32_t)(n - c1));
    return n;
}

/* N4: an ACK advanced snd_una -> slow start below ssthresh (+MSS per ACK),
 * congestion avoidance above it (+MSS*MSS/cwnd per ACK ~= +MSS per RTT). */
static void cwnd_on_ack(sock_t* s) {
    tcp_txq_t* q = txq_of(s);
    if (!q) return;
    if (q->cwnd < q->ssthresh) {
        q->cwnd += TCP_MSS;
    } else {
        uint32_t inc = ((uint32_t)TCP_MSS * TCP_MSS) / (q->cwnd ? q->cwnd : 1);
        q->cwnd += inc ? inc : 1;
    }
    if (q->cwnd > TCP_SSTHRESH_INIT) q->cwnd = TCP_SSTHRESH_INIT;
}

/* N4: multiplicative decrease. hard_rto=1 -> collapse to 1*MSS (RFC 5681
 * RTO); hard_rto=0 -> fast-retransmit halving (cwnd = new ssthresh). */
static void cwnd_on_loss(sock_t* s, int hard_rto) {
    tcp_txq_t* q = txq_of(s);
    if (!q) return;
    uint32_t flight = s->snd_nxt - s->snd_una;
    uint32_t half   = flight / 2;
    if (half < 2u * TCP_MSS) half = 2u * TCP_MSS;
    q->ssthresh = half;
    q->cwnd     = hard_rto ? TCP_MSS : q->ssthresh;
}

/* SipHash-2-4 (Aumasson/Bernstein) -- a fast keyed PRF. Used here as the keyed
 * hash for the RFC 6528 ISN below; the only requirement for that use is that an
 * attacker observing outputs cannot recover the key, which SipHash provides. */
static inline uint64_t sip_rotl(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}
static uint64_t siphash24(const uint8_t* in, uint32_t inlen,
                          uint64_t k0, uint64_t k1) {
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    uint64_t b  = (uint64_t)inlen << 56;
    uint32_t blocks = inlen & ~7u;
#define SIP_ROUND() do {                                                  \
        v0 += v1; v1 = sip_rotl(v1,13); v1 ^= v0; v0 = sip_rotl(v0,32);    \
        v2 += v3; v3 = sip_rotl(v3,16); v3 ^= v2;                          \
        v0 += v3; v3 = sip_rotl(v3,21); v3 ^= v0;                          \
        v2 += v1; v1 = sip_rotl(v1,17); v1 ^= v2; v2 = sip_rotl(v2,32);    \
    } while (0)
    for (uint32_t off = 0; off < blocks; off += 8) {
        uint64_t m = 0;
        for (int i = 0; i < 8; i++) m |= (uint64_t)in[off + i] << (8 * i);
        v3 ^= m; SIP_ROUND(); SIP_ROUND(); v0 ^= m;
    }
    for (uint32_t i = blocks; i < inlen; i++)
        b |= (uint64_t)in[i] << (8 * (i - blocks));
    v3 ^= b; SIP_ROUND(); SIP_ROUND(); v0 ^= b;
    v2 ^= 0xff; SIP_ROUND(); SIP_ROUND(); SIP_ROUND(); SIP_ROUND();
#undef SIP_ROUND
    return v0 ^ v1 ^ v2 ^ v3;
}

/* NET-HARDENING-2 (RFC 6528 keyed ISN): ISN = M + SipHash(secret, 4-tuple). The
 * 128-bit secret is drawn ONCE at boot from the RNG and never re-emitted, so an
 * off-path attacker observing our ISNs cannot recover it (SipHash is a keyed PRF)
 * and therefore cannot predict the ISN for a 4-tuple it wishes to spoof -- closing
 * the synq_on_ack ack==isn+1 gate EVEN on RDRAND-less CPUs (T410 / QEMU default)
 * where rng_u64() is a non-crypto xorshift128+: the remote attacker never sees the
 * PRNG state, only ISNs, and SipHash does not leak the key through its outputs.
 * (The prior gen_isn took the RNG's high bits directly, which a /dev/random reader
 * who recovered the xorshift state could still predict -- a keyed hash removes that
 * dependence on PRNG quality.) M is a coarse (~4us) timer so successive ISNs for
 * the SAME 4-tuple advance, giving RFC 6528 old-incarnation safety after TIME_WAIT. */
static uint32_t gen_isn(uint32_t l_ip, uint16_t l_port,
                        uint32_t r_ip, uint16_t r_port) {
    static uint64_t key0 = 0, key1 = 0;
    static int      keyed = 0;
    if (!keyed) { key0 = rng_u64(); key1 = rng_u64(); keyed = 1; }
    uint8_t t[12];
    t[0]=(uint8_t)(l_ip>>24); t[1]=(uint8_t)(l_ip>>16); t[2]=(uint8_t)(l_ip>>8); t[3]=(uint8_t)l_ip;
    t[4]=(uint8_t)(l_port>>8); t[5]=(uint8_t)l_port;
    t[6]=(uint8_t)(r_ip>>24); t[7]=(uint8_t)(r_ip>>16); t[8]=(uint8_t)(r_ip>>8); t[9]=(uint8_t)r_ip;
    t[10]=(uint8_t)(r_port>>8); t[11]=(uint8_t)r_port;
    uint64_t h = siphash24(t, 12, key0, key1);
    uint32_t M = (uint32_t)(now_ms() * 250u);   /* ~4us tick (RFC 6528) */
    return (uint32_t)h + M;
}

/* Free space left in the receive ring (= what we advertise as rcv_wnd). */
static uint16_t rcv_wnd(const sock_t* s) {
    uint32_t free_bytes = SOCK_RXBUF_SIZE - s->rx_used;
    /* Cap to 16-bit field; never advertise 0 unless truly full. */
    if (free_bytes > 65535) free_bytes = 65535;
    return (uint16_t)free_bytes;
}

/*
 * Build + transmit one TCP segment.  data/len is optional payload.  flags
 * is the TCP flag set.  seq/ack are host order.  Stores nothing about
 * retransmit -- the caller arranges that via tcp_arm_retransmit().
 *
 * CHANGED: advertises rcv_wnd(s) (our free rx ring space) instead of
 * s->snd_wnd (peer's window), so the server knows when it can keep sending.
 */
/* The IP source address for a TCP segment to `dst`. A loopback destination
 * (127/8) must use a loopback source so the peer's replies are themselves
 * loopback-routed by ip_tx -- otherwise a SYN-ACK addressed to net_get_ip()
 * escapes to the NIC and the in-OS (client+server in one kernel) handshake never
 * completes. Non-loopback returns net_get_ip() exactly as before (no behavior
 * change for real-NIC TCP). The pseudo-header checksum is computed with this same
 * source so it verifies against the source ip_tx actually stamps. */
static inline uint32_t tcp_src_ip(uint32_t dst) {
    return ((dst >> 24) == 127u) ? dst : net_get_ip();
}

static int tcp_xmit(sock_t* s, uint8_t flags, uint32_t seq, uint32_t ack,
                    const void* data, uint16_t len) {
    if (len > TCP_MSS) len = TCP_MSS;

    uint8_t seg[sizeof(tcp_hdr_t) + 4 + TCP_MSS];
    tcp_hdr_t* th = (tcp_hdr_t*)seg;
    memset(th, 0, sizeof(*th));
    th->src_port = net_htons(s->local_port);
    th->dst_port = net_htons(s->remote_port);
    th->seq      = net_htonl(seq);
    th->ack      = net_htonl(ack);
    th->flags    = flags;
    /* CHANGED: advertise our free rx buffer space as the receive window. */
    th->window   = net_htons(rcv_wnd(s));
    th->urgent   = 0;
    th->checksum = 0;

    /* NET-GAPS N2: every SYN we emit advertises our real MSS (kind 2). Without
     * it an RFC-conservative peer assumes the 536-byte default and caps our RX
     * throughput. Done here (not at the call sites) so SYN RETRANSMITS via
     * tcp_tick()'s rt slot carry the same option as the original. */
    uint16_t hlen = sizeof(tcp_hdr_t);
    if (flags & TCP_SYN) {
        uint8_t* opt = seg + hlen;
        opt[0] = 2; opt[1] = 4;
        opt[2] = (uint8_t)(TCP_MSS >> 8);
        opt[3] = (uint8_t)(TCP_MSS & 0xFF);
        hlen += 4;
    }
    th->data_off = (uint8_t)((hlen / 4) << 4);

    if (len && data) memcpy(seg + hlen, data, len);
    uint16_t seg_len = (uint16_t)(hlen + len);

    uint16_t ck = net_transport_checksum(tcp_src_ip(s->remote_ip), s->remote_ip,
                                         IPPROTO_TCP, seg, seg_len);
    th->checksum = net_htons(ck);

    return ip_tx(s->remote_ip, IPPROTO_TCP, seg, seg_len);
}

/*
 * Record an outstanding segment so tcp_tick() can retransmit it.
 * CHANGED: resets the per-socket retry count and RTO on each fresh arm.
 */
static void tcp_arm_retransmit(sock_t* s, uint8_t flags, uint32_t seq,
                               const void* data, uint16_t len) {
    int idx = sock_index(s);
    s->rt_pending    = true;
    s->rt_done       = false;
    s->rt_time_ms    = now_ms();
    s->rt_seq        = seq;
    s->rt_flags      = flags;
    s->rt_len        = (len > TCP_MSS) ? TCP_MSS : len;
    if (s->rt_len && data) memcpy(s->rt_data, data, s->rt_len);
    tcp_rt_count[idx] = 0;
    /* TCP-ROBUST adaptive RTO: seed from the smoothed estimate once we have one,
     * else the conservative initial RTO. tcp_tick still doubles this on loss. */
    tcp_rt_rto_ms[idx] = s->base_rto_ms ? s->base_rto_ms : TCP_RTO_INIT_MS;
    tcp_dupack[idx] = 0;
}

/* Clear the retransmit machinery for this socket. */
static void tcp_disarm_retransmit(sock_t* s) {
    int idx = sock_index(s);
    s->rt_pending       = false;
    s->rt_done          = false;
    tcp_rt_count[idx]   = 0;
    tcp_rt_rto_ms[idx]  = TCP_RTO_INIT_MS;
    tcp_dupack[idx]     = 0;
}

/* Send a pure ACK for the current rcv_nxt (no retransmit tracking). */
static void tcp_send_ack(sock_t* s) {
    tcp_xmit(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
}

/*
 * TCP-ROBUST adaptive RTO (RFC 6298): fold one RTT sample into the smoothed
 * estimator and recompute the base (un-backed-off) RTO. Integer fixed point;
 * alpha=1/8, beta=1/4; clamped to [TCP_RTO_MIN_MS, TCP_RTO_MAX_MS]. Karn's
 * rule (skip retransmitted segments) is enforced by the caller.
 */
static void tcp_rtt_update(sock_t* s, uint32_t r_ms) {
    if (s->srtt_ms == 0) {
        s->srtt_ms   = r_ms;          /* first sample */
        s->rttvar_ms = r_ms / 2;
    } else {
        uint32_t srtt  = s->srtt_ms;
        uint32_t delta = (srtt > r_ms) ? (srtt - r_ms) : (r_ms - srtt);
        s->rttvar_ms = (3 * s->rttvar_ms + delta) / 4;   /* (1-1/4)var + 1/4|d| */
        s->srtt_ms   = (7 * srtt + r_ms) / 8;            /* (1-1/8)srtt + 1/8 R  */
    }
    uint32_t rto = s->srtt_ms + 4 * s->rttvar_ms;
    if (rto < TCP_RTO_MIN_MS) rto = TCP_RTO_MIN_MS;
    if (rto > TCP_RTO_MAX_MS) rto = TCP_RTO_MAX_MS;
    s->base_rto_ms = rto;
}

/*
 * TCP-ROBUST: parse the TCP options that sit between the 20-byte fixed header
 * and the data offset (ihl). Extracts MSS, window-scale, SACK-permitted, and
 * timestamps. Walks kind/length defensively (bounded by ihl, malformed -> stop)
 * since the segment is untrusted input. Stores onto the socket.
 */
static void tcp_parse_options(sock_t* s, const uint8_t* seg, uint16_t ihl) {
    uint16_t i = sizeof(tcp_hdr_t);
    while (i < ihl) {
        uint8_t kind = seg[i];
        if (kind == 0) break;               /* End-of-options */
        if (kind == 1) { i++; continue; }   /* NOP */
        if (i + 1 >= ihl) break;
        uint8_t len = seg[i + 1];
        if (len < 2 || i + len > ihl) break; /* malformed -> stop */
        switch (kind) {
            case 2:  if (len == 4)  s->peer_mss = (uint16_t)((seg[i+2] << 8) | seg[i+3]); break;
            /* NET-HARDENING-2 (window-scaling decision): record the peer's shift
             * count for diagnostics but DELIBERATELY do NOT apply it. RFC 7323
             * window scaling is active only if BOTH peers send the WSCALE option
             * in their SYN; this stack never EMITS one (our SYN/SYN-ACK carry no
             * options), so the peer keeps its window UNSCALED and treating
             * th->window as the literal window is correct. Applying snd_wscale
             * without also advertising it would mis-scale a window the peer never
             * scaled -- so it stays parsed-but-unused by design (not half-done). */
            case 3:  if (len == 3)  { uint8_t w = seg[i+2]; s->snd_wscale = (w > 14) ? 14 : w; } break;
            case 4:  if (len == 2)  s->peer_sack_ok = 1; break;
            case 8:  if (len == 10) s->peer_ts_ok = 1; break;
            default: break;
        }
        i += len;
    }
}

/*
 * TCP-ROBUST (RFC 5681 fast retransmit): resend the oldest unacked segment
 * IMMEDIATELY on the 3rd duplicate ACK, instead of waiting a full RTO. Resets
 * the RTO clock so the timer does not also fire right after. Does not touch the
 * back-off count (that stays owned by tcp_tick).
 */
static void tcp_fast_retransmit(sock_t* s) {
    /* NET-GAPS N1: with a TX queue, loss recovery restarts at the OLDEST
     * unacked byte (snd_una), resegmented to the effective MSS. */
    tcp_txq_t* q = txq_of(s);
    if (q && q->used) {
        uint16_t eff_mss = (s->peer_mss && s->peer_mss < TCP_MSS)
                               ? s->peer_mss : TCP_MSS;
        uint16_t n = txq_peek_oldest(s, tcp_rtx_buf, eff_mss);
        if (n) {
            tcp_xmit(s, TCP_ACK | TCP_PSH, s->snd_una, s->rcv_nxt,
                     tcp_rtx_buf, n);
            q->sent_ms = now_ms();
        }
        return;
    }
    if (!s->rt_pending) return;
    uint32_t ack_val = (s->state == TCP_SYN_SENT) ? 0 : s->rcv_nxt;
    tcp_xmit(s, s->rt_flags, s->rt_seq, ack_val,
             s->rt_len ? s->rt_data : NULL, s->rt_len);
    s->rt_time_ms = now_ms();
}

/* ------------------------------------------------------------------ */
/* NET-P1-A: half-open SYN side-table                                   */
/* ------------------------------------------------------------------ */
/*
 * Passive open used to allocate a FULL sock_t (~45 KB: 32 KB rx ring + UDP
 * queue + retransmit slot) for every incoming SYN, so a burst of half-opens
 * drained the 16-slot shared table and starved ALL networking; SYNs past the
 * backlog were silently dropped. Half-open state is tiny -- hold it in this
 * compact side-table instead and promote to a real socket only when the
 * final ACK of the handshake arrives (straight to ESTABLISHED + the accept
 * queue). The listener's backlog now bounds ESTABLISHED-but-unaccepted
 * children at promote time; half-opens are bounded by the table itself
 * (evict-oldest on overflow -- the evicted peer just retransmits its SYN).
 * SYN-ACK retransmit runs from the table via tcp_synq_tick() (sock_poll).
 */
#define TCP_SYNQ_MAX        32       /* half-open capacity (~1 KB total)    */
#define TCP_SYNQ_RETRY_MS   1000     /* SYN-ACK retransmit cadence          */
#define TCP_SYNQ_MAX_RETRY  4        /* give up re-SYN-ACKing after this    */
#define TCP_SYNQ_TTL_MS     16000    /* drop half-opens older than this     */
#define TCP_SYNQ_WND        32768    /* window advertised in the SYN-ACK    */

typedef struct {
    bool     valid;
    uint32_t src_ip;       /* the peer                                     */
    uint16_t src_port;
    uint16_t local_port;   /* the listener's port                          */
    uint32_t isn;          /* OUR initial seq (the SYN-ACK's seq)          */
    uint32_t rcv_nxt;      /* peer's SYN seq + 1                           */
    uint16_t peer_wnd;     /* peer's window from the SYN                   */
    uint64_t born_ms;      /* age (TTL + evict-oldest)                     */
    uint64_t last_tx_ms;   /* last SYN-ACK transmit                        */
    uint8_t  retries;
} tcp_synq_ent_t;

static tcp_synq_ent_t tcp_synq[TCP_SYNQ_MAX];

/* Build + send one socket-less TCP control segment (the side-table has no
 * sock_t to hang tcp_xmit() off). No payload, no retransmit arming here. */
static int tcp_xmit_raw(uint32_t remote_ip, uint16_t local_port,
                        uint16_t remote_port, uint8_t flags,
                        uint32_t seq, uint32_t ack, uint16_t window) {
    uint8_t seg[sizeof(tcp_hdr_t) + 4];
    tcp_hdr_t* th = (tcp_hdr_t*)seg;
    memset(th, 0, sizeof(*th));
    th->src_port = net_htons(local_port);
    th->dst_port = net_htons(remote_port);
    th->seq      = net_htonl(seq);
    th->ack      = net_htonl(ack);
    th->flags    = flags;
    th->window   = net_htons(window);
    /* NET-GAPS N2: a SYN-ACK from the side-table advertises our MSS too
     * (synq_on_syn + the tcp_synq_tick retransmit both route through here). */
    uint16_t hlen = sizeof(tcp_hdr_t);
    if (flags & TCP_SYN) {
        uint8_t* opt = seg + hlen;
        opt[0] = 2; opt[1] = 4;
        opt[2] = (uint8_t)(TCP_MSS >> 8);
        opt[3] = (uint8_t)(TCP_MSS & 0xFF);
        hlen += 4;
    }
    th->data_off = (uint8_t)((hlen / 4) << 4);
    th->checksum = net_htons(net_transport_checksum(tcp_src_ip(remote_ip), remote_ip,
                                                    IPPROTO_TCP, seg, hlen));
    return ip_tx(remote_ip, IPPROTO_TCP, seg, hlen);
}

static tcp_synq_ent_t* synq_find(uint32_t src_ip, uint16_t src_port,
                                 uint16_t local_port) {
    for (int i = 0; i < TCP_SYNQ_MAX; i++) {
        tcp_synq_ent_t* e = &tcp_synq[i];
        if (e->valid && e->src_ip == src_ip && e->src_port == src_port &&
            e->local_port == local_port)
            return e;
    }
    return NULL;
}

/* A SYN arrived at a LISTEN socket: record (or refresh) the half-open in the
 * side-table and answer with a SYN-ACK. No sock_t is consumed. */
static void synq_on_syn(sock_t* listener, uint32_t src_ip, uint16_t src_port,
                        uint32_t seq, uint16_t peer_wnd) {
    /* Retransmitted SYN for an existing half-open: re-answer with the SAME
     * ISN (the peer may have missed our first SYN-ACK). */
    tcp_synq_ent_t* e = synq_find(src_ip, src_port, listener->local_port);
    if (!e) {
        /* New half-open: free slot, else evict the OLDEST (that peer simply
         * retransmits its SYN later -- correct TCP behavior under load). */
        tcp_synq_ent_t* oldest = &tcp_synq[0];
        for (int i = 0; i < TCP_SYNQ_MAX; i++) {
            if (!tcp_synq[i].valid) { e = &tcp_synq[i]; break; }
            if (tcp_synq[i].born_ms < oldest->born_ms) oldest = &tcp_synq[i];
        }
        if (!e) e = oldest;
        e->valid      = true;
        e->src_ip     = src_ip;
        e->src_port   = src_port;
        e->local_port = listener->local_port;
        e->isn        = gen_isn(listener->local_ip, listener->local_port,
                                src_ip, src_port);
        e->rcv_nxt    = seq + 1;          /* their SYN consumes one seq */
        e->born_ms    = now_ms();
    }
    e->peer_wnd   = peer_wnd;
    e->last_tx_ms = now_ms();
    /* NET-HARDENING F5: a (re)transmitted SYN is fresh proof the peer is alive
     * and still trying, so reset the SYN-ACK retransmit budget -- for BOTH a new
     * half-open and a dup-SYN against an existing one. The old code reset it only
     * on the new-entry path, so a peer that kept retransmitting its SYN (because
     * it never saw our SYN-ACK) could still be evicted once tcp_synq_tick() hit
     * TCP_SYNQ_MAX_RETRY. born_ms is left untouched so the TTL still caps the
     * half-open's total lifetime -- a SYN flood can't pin an entry forever. */
    e->retries    = 0;
    tcp_xmit_raw(src_ip, e->local_port, src_port, TCP_SYN | TCP_ACK,
                 e->isn, e->rcv_nxt, TCP_SYNQ_WND);
}

/* The final handshake ACK arrived at the LISTEN socket: promote the matching
 * half-open into a real ESTABLISHED child on the accept queue. This is the
 * ONLY place passive open consumes a sock_t. */
static void synq_on_ack(sock_t* listener, uint32_t src_ip, uint16_t src_port,
                        uint32_t ack, uint16_t peer_wnd) {
    tcp_synq_ent_t* e = synq_find(src_ip, src_port, listener->local_port);
    if (!e) return;                              /* stray ACK: ignore       */
    if (ack != e->isn + 1) return;               /* doesn't ack our SYN-ACK */

    sock_t* base = sock_table_base();
    if (!base) { e->valid = false; return; }

    /* The backlog bounds ESTABLISHED-but-unaccepted children (half-opens no
     * longer consume sockets, so this is now its true RFC meaning). */
    int pending = 0;
    for (int i = 0; i < SOCK_MAX; i++)
        if (base[i].used && base[i].parent == listener) pending++;
    int blcap = listener->backlog;
    if (blcap <= 0) blcap = 1;
    if (blcap > SOCK_MAX) blcap = SOCK_MAX;

    sock_t* child = NULL;
    int child_idx = -1;
    if (pending < blcap) {
        for (int i = 0; i < SOCK_MAX; i++) {
            if (!base[i].used) { child = &base[i]; child_idx = i; break; }
        }
    }
    if (!child) {
        /* Backlog full or socket table exhausted: the peer believes it is
         * ESTABLISHED, so an honest RST beats silently eating its data. */
        tcp_xmit_raw(src_ip, e->local_port, src_port, TCP_RST,
                     e->isn + 1, e->rcv_nxt, 0);
        e->valid = false;
        return;
    }

    memset(child, 0, sizeof(*child));
    child->used        = true;
    child->type        = SOCK_STREAM;
    child->local_ip    = listener->local_ip;
    child->local_port  = listener->local_port;
    child->remote_ip   = src_ip;
    child->remote_port = src_port;
    child->parent      = listener;
    child->owner_pid   = listener->owner_pid;
    child->snd_wnd     = peer_wnd;
    child->rcv_nxt     = e->rcv_nxt;
    child->snd_una     = e->isn + 1;             /* our SYN-ACK is acked    */
    child->snd_nxt     = e->isn + 1;
    child->state       = TCP_ESTABLISHED;
    child->reset       = false;

    tcp_ooo_clear(child_idx);
    tcp_rt_count[child_idx]   = 0;
    tcp_rt_rto_ms[child_idx]  = TCP_RTO_INIT_MS;
    txq_reset(child);         /* NET-GAPS N1+N4: fresh TX queue + cwnd */

    /* Accept queue (same push the old SYN_RCVD promotion used). */
    child->accept_next      = listener->accept_queue;
    listener->accept_queue  = child;

    e->valid = false;
}

/* Locate a live listener for a side-table entry's port (NULL if it closed). */
static sock_t* synq_listener(uint16_t local_port) {
    sock_t* base = sock_table_base();
    if (!base) return NULL;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (base[i].used && base[i].type == SOCK_STREAM &&
            base[i].state == TCP_LISTEN && base[i].local_port == local_port)
            return &base[i];
    }
    return NULL;
}

/* Run the side-table timers: SYN-ACK retransmit (bounded), TTL expiry, and
 * orphan cleanup when a listener closes. Called once per sock_poll(). */
void tcp_synq_tick(void) {
    uint64_t now = now_ms();
    for (int i = 0; i < TCP_SYNQ_MAX; i++) {
        tcp_synq_ent_t* e = &tcp_synq[i];
        if (!e->valid) continue;
        if (now - e->born_ms >= TCP_SYNQ_TTL_MS ||
            e->retries >= TCP_SYNQ_MAX_RETRY ||
            synq_listener(e->local_port) == NULL) {
            e->valid = false;
            continue;
        }
        if (now - e->last_tx_ms >= TCP_SYNQ_RETRY_MS) {
            e->retries++;
            e->last_tx_ms = now;
            tcp_xmit_raw(e->src_ip, e->local_port, e->src_port,
                         TCP_SYN | TCP_ACK, e->isn, e->rcv_nxt, TCP_SYNQ_WND);
        }
    }
}

/* Append in-order stream bytes into the socket RX ring. Returns the number of
 * bytes actually stored (< n if the ring filled). Callers MUST advance rcv_nxt
 * by the returned count only -- advancing by the full n would ACK bytes we
 * dropped, so the peer frees them and never retransmits (silent stream loss).
 *
 * Uses memcpy (up to two chunks for the wrap case) instead of byte-at-a-time. */
static uint16_t rx_ring_push(sock_t* s, const uint8_t* p, uint16_t n) {
    uint32_t free_space = SOCK_RXBUF_SIZE - s->rx_used;
    uint16_t to_push = (n <= free_space) ? n : (uint16_t)free_space;
    if (to_push == 0) return 0;

    /* First chunk: from rx_tail to end of buffer (or to_push, whichever smaller). */
    uint32_t chunk1 = SOCK_RXBUF_SIZE - s->rx_tail;
    if (chunk1 > to_push) chunk1 = to_push;
    memcpy(s->rxbuf + s->rx_tail, p, chunk1);

    /* Second chunk: wrap around to the beginning of the buffer. */
    uint32_t chunk2 = to_push - chunk1;
    if (chunk2 > 0) {
        memcpy(s->rxbuf, p + chunk1, chunk2);
    }

    s->rx_tail = (s->rx_tail + to_push) % SOCK_RXBUF_SIZE;
    s->rx_used += to_push;
    return to_push;
}

static uint32_t rx_ring_pop(sock_t* s, uint8_t* dst, uint32_t want) {
    uint32_t avail = (want <= s->rx_used) ? want : s->rx_used;
    if (avail == 0) return 0;

    /* First chunk: from rx_head to end of buffer (or avail, whichever smaller). */
    uint32_t chunk1 = SOCK_RXBUF_SIZE - s->rx_head;
    if (chunk1 > avail) chunk1 = avail;
    memcpy(dst, s->rxbuf + s->rx_head, chunk1);

    /* Second chunk: wrap around to the beginning. */
    uint32_t chunk2 = avail - chunk1;
    if (chunk2 > 0) {
        memcpy(dst + chunk1, s->rxbuf, chunk2);
    }

    s->rx_head = (s->rx_head + avail) % SOCK_RXBUF_SIZE;
    s->rx_used -= avail;
    return avail;
}

/*
 * Try to drain any saved OOO segment into the rx ring now that rcv_nxt has
 * advanced.  Called after every in-order segment is accepted.
 * CHANGED: new function to support OOO assembly.
 */
static void tcp_ooo_drain(sock_t* s) {
    if (!tcp_ooo) return;
    int idx = sock_index(s);

    /* NET-P1-B: keep sweeping the 4-slot row until a full pass makes no
     * progress. Each pass: discard stale slots, merge the slot whose data
     * meets rcv_nxt (trimming any duplicate prefix). */
    bool progress = true;
    while (progress) {
        progress = false;
        for (int k = 0; k < TCP_OOO_DEPTH; k++) {
            tcp_ooo_slot_t* slot = &tcp_ooo[idx][k];
            if (!slot->valid) continue;

            uint32_t seg_end = slot->seq + slot->len;
            if (SEQ32_GEQ(s->rcv_nxt, seg_end)) {
                /* Entirely duplicate by now: free the slot. */
                slot->valid = false;
                continue;
            }
            if (SEQ32_GT(slot->seq, s->rcv_nxt)) continue;  /* still a gap */

            /* slot->seq <= rcv_nxt < seg_end: trim any duplicate prefix. */
            uint32_t trim = s->rcv_nxt - slot->seq;
            uint16_t len  = (uint16_t)(slot->len - trim);
            /* Only consume the slot if the WHOLE remainder fits. Partially
             * pushing and clearing would drop the tail forever while
             * advancing rcv_nxt past it (an ACK lie). Retry after the app
             * drains the ring. */
            uint16_t freeb = (uint16_t)(SOCK_RXBUF_SIZE - s->rx_used);
            if (len > freeb) return;
            s->rcv_nxt += rx_ring_push(s, slot->data + trim, len);
            slot->valid = false;
            progress = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Active open                                                         */
/* ------------------------------------------------------------------ */
int tcp_connect(sock_t* s, uint32_t dst_ip, uint16_t dst_port) {
    if (s->state != TCP_CLOSED) return SOCK_EINVAL;

    s->remote_ip   = dst_ip;
    s->remote_port = dst_port;
    s->local_ip    = net_get_ip();
    if (s->local_port == 0) s->local_port = sock_alloc_port();

    /* Initialise OOO slots and retry counters for this socket. */
    int idx = sock_index(s);
    tcp_ooo_clear(idx);
    tcp_rt_count[idx]       = 0;
    tcp_rt_rto_ms[idx]      = TCP_RTO_INIT_MS;
    txq_reset(s);            /* NET-GAPS N1+N4: fresh TX queue + cwnd */

    uint32_t isn = gen_isn(s->local_ip, s->local_port,
                           s->remote_ip, s->remote_port);
    s->snd_una   = isn;
    s->snd_nxt   = isn + 1;        /* SYN consumes one seq */
    s->rcv_nxt   = 0;
    s->state     = TCP_SYN_SENT;
    s->reset     = false;

    /* Arm BEFORE transmitting (see tcp_send): on the in-kernel loopback path the
     * SYN-ACK is delivered + processed by lo_drain() inside the sock_poll() pump
     * BELOW -- ip_tx() only ENQUEUES TCP loopback packets; their delivery is
     * deferred to lo_drain() in sock_poll() -- and that processing disarms the
     * retransmit. Arming AFTER the xmit would leave rt_pending re-armed for the
     * (by then already-ACKed) SYN once the deferred ACK is processed, and nothing
     * could clear it (the ms clock is frozen inside this IF=0 syscall, so no
     * RTO-retransmit ever fires). Only the ORDER matters (arm before the disarm),
     * not WHERE the disarm happens. */
    tcp_arm_retransmit(s, TCP_SYN, isn, NULL, 0);
    if (tcp_xmit(s, TCP_SYN, isn, 0, NULL, 0) != 0) {
        tcp_disarm_retransmit(s);
        s->state = TCP_CLOSED;
        return SOCK_ECONN;
    }

    /* Pump until ESTABLISHED, RST, or timeout. The wall-clock guard ALONE cannot
     * terminate this loop: now_ms() is frozen inside this IF=0 syscall, so a SYN
     * to a non-listening loopback port (which tcp_input drops with NO RST) would
     * spin forever and hang the whole cooperative OS. The frozen-tick backstop
     * guarantees we bail to SOCK_ETIMEDOUT -- exactly the negative rc callers
     * (e.g. dzclient's connect-retry) expect when the peer is not yet listening. */
    uint64_t start = now_ms();
    uint64_t last  = start;
    uint32_t frozen = 0;
    while (now_ms() - start < TCP_CONNECT_MS) {
        sock_poll();
        if (s->reset)                     { s->state = TCP_CLOSED; return SOCK_ECONN; }
        if (s->state == TCP_ESTABLISHED)  return SOCK_OK;
        if (s->state == TCP_CLOSED)       return SOCK_ECONN;
        uint64_t n = now_ms();
        if (n != last) { last = n; frozen = 0; }
        else if (++frozen >= TCP_FROZEN_SPIN_CAP) break;
    }
    /* NET-HARDENING F9: a timed-out/aborted active open must DROP its armed SYN
     * retransmit before closing. Otherwise rt_pending stays true on a CLOSED
     * socket and -- if the caller leaks the fd instead of close()ing it -- a later
     * sock_poll()->tcp_tick() re-emits the stale SYN to the old remote (once the
     * frozen wall-clock advances past the RTO), up to TCP_RTO_MAX_RETRIES times.
     * Mirrors the disarm already done on the xmit-failure path above. */
    tcp_disarm_retransmit(s);
    s->state = TCP_CLOSED;
    return SOCK_ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Send                                                                */
/* ------------------------------------------------------------------ */
/*
 * NET-GAPS N1+N4: windowed TCP send over the per-socket TX retransmit queue.
 *
 * Every transmitted payload byte is remembered in the tcp_txq ring until the
 * peer's cumulative ACK covers it, so loss ANYWHERE in the pipeline is
 * repairable from snd_una (tcp_tick RTO / 3-dup-ACK fast retransmit) -- the
 * old design could only resend the newest segment. In-flight bytes are gated
 * by min(cwnd, peer window): cwnd starts at 4*MSS (parity with the old fixed
 * 4-segment pipeline), slow-starts +MSS per ACK, and collapses on loss
 * (RFC 5681-lite; see cwnd_on_ack/cwnd_on_loss).
 *
 * Degrade path (txq alloc failed at boot): single-outstanding stop-and-wait
 * through the rt_* slot -- the exact pre-N1 behavior.
 *
 * Returns total bytes accepted (may be less than `len` on error mid-way).
 */
int tcp_send(sock_t* s, const void* data, uint16_t len) {
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
        return SOCK_ECONN;
    if (len == 0) return 0;

    tcp_txq_t*     q    = txq_of(s);
    const uint8_t* ptr  = (const uint8_t*)data;
    uint16_t       sent = 0;

    while (sent < len) {
        /* Determine this segment's size: min(remaining, effective-MSS, peer wnd).
         * TCP-ROBUST: cap to the peer's advertised MSS when smaller than ours so
         * we never oversize a segment a real server told us it won't accept. */
        uint16_t eff_mss = (s->peer_mss && s->peer_mss < TCP_MSS) ? s->peer_mss : TCP_MSS;
        uint16_t chunk = (uint16_t)(len - sent);
        if (chunk > eff_mss) chunk = eff_mss;
        /* Respect peer's advertised send window (never send 0-window data).
         * NET-P1-C: the old guard (`snd_wnd > 0 && ...`) skipped the clamp
         * exactly when the window was ZERO, so the persist branch below was
         * unreachable and we transmitted INTO a zero window -- the rig's
         * window-shut peer caught it (probes=0 delivered=1). Every connected
         * socket carries a real advertised window (sock_socket seeds 1024;
         * the handshake + every non-RST segment update it), so snd_wnd==0
         * means the peer genuinely closed its window. */
        if (chunk > s->snd_wnd) chunk = (uint16_t)s->snd_wnd;
        if (chunk == 0) {
            /* NET-P1-C: zero-window PERSIST timer. The old code just spun
             * sock_poll() here -- no probe and NO BOUND, so a lost window
             * update deadlocked the send forever inside a syscall (IF=0:
             * the frozen-tick freeze family). Send a 1-byte probe with
             * exponential backoff until the peer re-advertises window; the
             * probe carries the next unsent byte at snd_nxt (snd_nxt is NOT
             * advanced -- if the peer happens to accept it, the next real
             * segment resends that byte and the receiver trims the
             * duplicate prefix). Bounded by wall clock AND the iteration
             * cap (the load-bearing backstop when ticks are frozen). */
            uint64_t pstart   = now_ms();
            uint64_t plast    = 0;
            uint64_t last_now = pstart;
            uint32_t pint     = TCP_RTO_INIT_MS;
            uint32_t frozen   = 0;
            while (s->snd_wnd == 0) {
                uint64_t now = now_ms();
                if (plast == 0 || now - plast >= (uint64_t)pint) {
                    tcp_xmit(s, TCP_ACK, s->snd_nxt, s->rcv_nxt,
                             ptr + sent, 1);
                    plast = now;
                    pint *= 2;
                    if (pint > TCP_RTO_MAX_MS) pint = TCP_RTO_MAX_MS;
                }
                sock_poll();
                if (s->reset) return sent ? (int)sent : SOCK_ECONN;
                if (now - pstart >= TCP_CONNECT_MS)
                    return sent ? (int)sent : SOCK_ETIMEDOUT;
                /* Frozen-tick backstop: the cap counts only CONSECUTIVE
                 * iterations where the millisecond clock did not move, so it
                 * fires exactly in the IF=0/frozen-PIT condition it guards
                 * against and never races the wall-clock budget above. */
                if (now != last_now) { last_now = now; frozen = 0; }
                else if (++frozen >= 200000)
                    return sent ? (int)sent : SOCK_ETIMEDOUT;
            }
            continue;
        }

        /* NET-GAPS N1+N4 send gate: this chunk must fit the congestion window
         * (bytes in flight + chunk <= min(cwnd, peer wnd)) AND the TX queue
         * must have room to remember it. Fallback (no txq): single outstanding
         * segment, stop-and-wait via rt_pending. */
        uint32_t flight = s->snd_nxt - s->snd_una;
        uint32_t limit  = q ? q->cwnd : TCP_MSS;
        if (limit > s->snd_wnd) limit = s->snd_wnd;
        bool must_wait = q ? (flight + chunk > limit ||
                              txq_free_bytes(s) < chunk)
                           : s->rt_pending;
        if (must_wait) {
            /* Drain until an ACK frees window/queue space. The wall-clock
             * guard alone cannot terminate this inside an IF=0 syscall (the
             * ms clock is frozen), so the frozen-tick backstop is load-
             * bearing, exactly as in tcp_connect. */
            uint32_t old_una = s->snd_una;
            uint64_t start = now_ms();
            uint64_t last  = start;
            uint32_t frozen = 0;
            bool progress = false;
            while (now_ms() - start < TCP_CONNECT_MS) {
                sock_poll();
                if (s->reset) return sent ? (int)sent : SOCK_ECONN;
                if (SEQ32_GT(s->snd_una, old_una) ||
                    (q == NULL && !s->rt_pending)) { progress = true; break; }
                uint64_t n = now_ms();
                if (n != last) { last = n; frozen = 0; }
                else if (++frozen >= TCP_FROZEN_SPIN_CAP) break;
            }
            if (!progress)
                return sent ? (int)sent : SOCK_ETIMEDOUT;
            continue;   /* re-evaluate window + chunk sizing */
        }

        uint32_t seq = s->snd_nxt;
        /* Remember the bytes BEFORE transmitting. On the in-kernel loopback
         * path (127/8) ip_tx() only ENQUEUES the segment; it is delivered by
         * lo_drain() inside a later sock_poll() still within this call, the
         * peer ACKs, and that ACK CONSUMES the queued bytes. The remember
         * must precede that deferred consume (the old single-slot code had
         * the same arm-before-xmit rule for the same reason). */
        if (q) txq_append(s, ptr + sent, chunk);
        else   tcp_arm_retransmit(s, TCP_ACK | TCP_PSH, seq, ptr + sent, chunk);
        if (tcp_xmit(s, TCP_ACK | TCP_PSH, seq, s->rcv_nxt,
                     ptr + sent, chunk) != 0) {
            if (q) txq_rollback(s, chunk);
            else   tcp_disarm_retransmit(s);
            return sent ? (int)sent : SOCK_ECONN;
        }
        s->snd_nxt += chunk;
        sent = (uint16_t)(sent + chunk);

        /* Non-blocking poll: pick up any ACKs that arrived while we were
         * building and transmitting, without waiting a full RTT. */
        sock_poll();
        if (s->reset) return sent ? (int)sent : SOCK_ECONN;
    }

    /* Final drain: wait briefly until everything sent is ACKed so the caller
     * doesn't close the connection before the peer has confirmed receipt. The
     * frozen-tick backstop is essential: now_ms() can't advance inside this IF=0
     * syscall, so without it a never-ACKed loopback segment would spin forever. */
    {
        uint64_t start = now_ms();
        uint64_t last  = start;
        uint32_t frozen = 0;
        while (((q && q->used > 0) || (!q && s->rt_pending)) &&
               now_ms() - start < TCP_CONNECT_MS) {
            sock_poll();
            if (s->reset) break;
            uint64_t n = now_ms();
            if (n != last) { last = n; frozen = 0; }
            else if (++frozen >= TCP_FROZEN_SPIN_CAP) break;
        }
    }

    return (int)sent;
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */
/*
 * CHANGED: pumps more aggressively (up to 32 polls) when the ring is
 * empty and the connection is alive, to allow multiple back-to-back
 * segments to accumulate.  Returns SOCK_ECONN on RST.
 */
int tcp_recv(sock_t* s, void* buf, uint16_t len) {
    /* Return RST error immediately. */
    if (s->reset) return SOCK_ECONN;

    /* Pump the wire so data has a chance to arrive. NET-GAPS N3: FIN_WAIT /
     * FIN_WAIT_2 pump too -- our send half is closed but the peer may still be
     * streaming data at us (half-close). */
    if (s->rx_used == 0 &&
        (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT ||
         s->state == TCP_FIN_WAIT    || s->state == TCP_FIN_WAIT_2)) {
        for (int i = 0; i < 32 && s->rx_used == 0 && !s->reset; i++)
            sock_poll();
    }

    if (s->reset) return SOCK_ECONN;

    if (s->rx_used > 0) {
        uint16_t wnd_before = rcv_wnd(s);
        uint32_t got = rx_ring_pop(s, (uint8_t*)buf, len);
        /* NET-HARDENING-1 (SWS / receiver window reopen, RFC 813): if draining the
         * ring just reopened our advertised window from below one MSS to >= one
         * MSS, emit an immediate window-update ACK. Otherwise a sender parked in
         * its zero-window persist loop only learns of the reopened window via its
         * own exponential-backoff probe (up to ~30 s later). Fires only on the
         * below-MSS -> open transition, so it cannot ACK-storm. */
        if (s->state == TCP_ESTABLISHED && wnd_before < TCP_MSS &&
            rcv_wnd(s) >= TCP_MSS)
            tcp_send_ack(s);
        return (int)got;
    }
    /* Peer closed and ring drained -> EOF. */
    if (s->state == TCP_CLOSE_WAIT || s->state == TCP_CLOSED ||
        s->state == TCP_TIME_WAIT) {
        return 0;
    }
    return 0;   /* would-block; sock_recv translates to EAGAIN */
}

/* ------------------------------------------------------------------ */
/* Close                                                               */
/* ------------------------------------------------------------------ */
int tcp_close(sock_t* s) {
    if (s->state == TCP_CLOSED) return SOCK_OK;

    /* Clear OOO side-table slots on close. */
    tcp_ooo_clear(sock_index(s));

    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        uint32_t seq = s->snd_nxt;
        tcp_xmit(s, TCP_FIN | TCP_ACK, seq, s->rcv_nxt, NULL, 0);
        s->snd_nxt += 1;   /* FIN consumes one seq */
        tcp_arm_retransmit(s, TCP_FIN | TCP_ACK, seq, NULL, 0);
        /* NET-HARDENING-2: passive close (CLOSE_WAIT) -> LAST_ACK so tcp_tick
         * RETRANSMITS our FIN until the peer ACKs it (the old CLOSE_WAIT->TIME_WAIT
         * never retransmitted a lost passive-close FIN); active close -> FIN_WAIT. */
        s->state = (s->state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK : TCP_FIN_WAIT;
        s->time_wait_ms = now_ms();

        /* Brief drain: on loopback the peer ACKs our FIN within a few polls and
         * the close completes synchronously (-> CLOSED/TIME_WAIT, freed below). A
         * short iteration cap keeps the syscall fast; if our FIN is still un-acked
         * when it expires (a lossy NIC close), DEFER teardown to background. */
        uint32_t spins = 0;
        while (spins++ < TCP_CLOSE_DRAIN_ITERS) {
            sock_poll();
            if (s->state == TCP_CLOSED || s->state == TCP_TIME_WAIT ||
                s->state == TCP_FIN_WAIT_2) break;
            /* Our FIN acked AND every queued data byte acked (N1). */
            if (!s->rt_pending &&
                (!tcp_txq || tcp_txq[sock_index(s)].used == 0)) break;
        }

        /* DEFERRED TEARDOWN: if our FIN is still outstanding, leave the socket in
         * its closing state (FIN_WAIT / LAST_ACK) with rt_pending armed. sock_close()
         * orphans it and tcp_tick() retransmits the FIN under the advancing clock,
         * reaping it once the FIN is acked or the retry budget is exhausted. On a
         * completed (loopback) close rt_pending is clear, so we fall through to
         * CLOSED and the slot is freed immediately as before. */
        if ((s->rt_pending || (tcp_txq && tcp_txq[sock_index(s)].used > 0)) &&
            s->state != TCP_CLOSED && s->state != TCP_TIME_WAIT &&
            s->state != TCP_FIN_WAIT_2)
            return SOCK_OK;
    }
    if (s->state == TCP_FIN_WAIT_2)
        return SOCK_OK;   /* N3: linger for the peer's FIN; tick reaps idle FW2 */
    s->state = TCP_CLOSED;
    return SOCK_OK;
}

/* ------------------------------------------------------------------ */
/* Inbound segment handling                                            */
/* ------------------------------------------------------------------ */
/*
 * CHANGED:
 *   - ACK comparisons use SEQ32_GEQ() for wraparound safety.
 *   - In-order data: always appended + ACKed; OOO drain called after.
 *   - Out-of-order data: saved in OOO side-table slot (if not duplicate).
 *   - Duplicate data (seq + len <= rcv_nxt): silently drop + re-ACK.
 *   - Duplicate ACKs: handled gracefully (no fast-retransmit, but no crash).
 *   - RST: sets s->reset but leaves state for tcp_recv() to surface the
 *     error; also clears retransmit machinery.
 *   - FIN: handled for both ESTABLISHED and FIN_WAIT; rcv_nxt incremented
 *     even when seq is slightly ahead (common with last-segment piggybacking).
 *   - Peer window (snd_wnd) updated on every segment for flow control.
 */
void tcp_input(uint32_t src_ip, uint32_t dst_ip,
               const uint8_t* seg, uint16_t seg_len) {
    if (seg_len < sizeof(tcp_hdr_t)) return;

    const tcp_hdr_t* th = (const tcp_hdr_t*)seg;

    /* TCP-ROBUST (RX checksum verification): the checksum covers the IPv4
     * pseudo-header + TCP header + payload, so a VALID segment sums to 0
     * (net_transport_checksum returns the final ~sum). Corrupt -> silent drop
     * (the peer retransmits); never let corrupt bytes reach the stream.
     *
     * NET-HARDENING-1: a checksum FIELD of 0 is tolerated ONLY for loopback
     * (127/8) sources -- in-kernel injectors / lo_drain leave it 0. A NIC-
     * delivered TCP segment MUST carry a real checksum (TCP checksum is
     * mandatory; a 0 there is corruption or forgery), so it is verified
     * unconditionally. The martian filter already guarantees a 127/8 src can
     * only have arrived via lo_drain, so this cannot be spoofed from the wire. */
    if (!(((src_ip >> 24) == 127u) && th->checksum == 0) &&
        net_transport_checksum(src_ip, dst_ip, IPPROTO_TCP, seg, seg_len) != 0) {
        return;
    }

    uint16_t src_port = net_ntohs(th->src_port);
    uint16_t dst_port = net_ntohs(th->dst_port);

    sock_t* s = sock_find_tcp(src_ip, src_port, dst_port);
    if (s == NULL) return;   /* no matching socket -> drop (no RST emitted) */

    /* Parse header length (data offset); clamp to valid range. */
    uint8_t ihl = (uint8_t)((th->data_off >> 4) * 4);
    if (ihl < sizeof(tcp_hdr_t)) ihl = sizeof(tcp_hdr_t);
    if (ihl > seg_len)           ihl = (uint8_t)seg_len;

    uint32_t seq       = net_ntohl(th->seq);
    uint32_t ack       = net_ntohl(th->ack);
    uint8_t  flags     = th->flags;
    const uint8_t* payload     = seg + ihl;
    uint16_t       payload_len = (uint16_t)(seg_len - ihl);

    /* RST handling. Done BEFORE the window update (NET-HARDENING F6): a RST
     * carries no meaningful advertised window, so writing snd_wnd from it would
     * clobber the peer's last real value.
     *
     * NET-HARDENING F12 (RFC 793 / RFC 5961): a RST must be VALIDATED before it
     * is honored. The old code reset on ANY RST matching the 4-tuple, so a single
     * stray/forged RST to a listening port set the LISTENER to CLOSED -- killing
     * accept() and (via synq_listener()==NULL) collapsing every pending half-open
     * -- and any off-path RST blind-tore-down an ESTABLISHED connection. */
    if (flags & TCP_RST) {
        if (s->state == TCP_LISTEN)
            return;                       /* RFC 793: ignore a RST in LISTEN */
        if (s->state == TCP_SYN_SENT) {
            /* RFC 793: a RST during active open is acceptable only if it ACKs
             * the SYN we sent; otherwise it is not for this connection. */
            if (!(flags & TCP_ACK) || !SEQ32_GEQ(ack, s->snd_nxt))
                return;
        } else {
            /* RFC 5961: in a synchronized state honor the RST only if its seq is
             * inside the current receive window (seq==rcv_nxt always qualifies,
             * covering a zero window); an off-window / blind RST is dropped. */
            uint32_t winhi = s->rcv_nxt + rcv_wnd(s);
            if (seq != s->rcv_nxt &&
                (!SEQ32_GEQ(seq, s->rcv_nxt) || SEQ32_GEQ(seq, winhi)))
                return;
        }
        s->reset = true;
        s->state = TCP_CLOSED;
        tcp_disarm_retransmit(s);
        return;
    }

    /* Update peer's advertised window on every non-RST segment. Capture the
     * PREVIOUS window first: RFC 5681 defines a duplicate ACK (the fast-
     * retransmit trigger) as one whose advertised window is UNCHANGED -- a pure
     * window update is NOT a dup-ACK (NET-HARDENING F4). */
    uint16_t prev_snd_wnd = s->snd_wnd;
    s->snd_wnd = net_ntohs(th->window);

    switch (s->state) {
    case TCP_LISTEN:
        /* NET-P1-A: passive open runs through the half-open SYN side-table.
         * A SYN records ~32 B of state + answers SYN-ACK (NO sock_t burned);
         * the final handshake ACK promotes the entry into an ESTABLISHED
         * child on the accept queue -- the only point a socket is consumed.
         * A SYN flood now exhausts a 1 KB table (evict-oldest) instead of
         * starving every socket in the system. */
        if (flags & TCP_SYN) {
            synq_on_syn(s, src_ip, src_port, seq, net_ntohs(th->window));
        } else if (flags & TCP_ACK) {
            synq_on_ack(s, src_ip, src_port, ack, net_ntohs(th->window));
        }
        return;

    case TCP_SYN_RCVD:
        /* Server-side: waiting for final ACK of 3-way handshake. */
        if (flags & TCP_ACK) {
            /* Verify the ACK covers our SYN-ACK. */
            if (!SEQ32_GEQ(ack, s->snd_nxt)) return;  /* unexpected ack */

            s->snd_una = ack;
            tcp_disarm_retransmit(s);
            s->state = TCP_ESTABLISHED;

            /* Add to parent's accept queue. */
            if (s->parent) {
                s->accept_next = s->parent->accept_queue;
                s->parent->accept_queue = s;
            }
        }
        return;

    case TCP_SYN_SENT:
        /* Expect SYN|ACK acking our ISN+1. */
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            /* CHANGED: SEQ32_GEQ for wraparound-safe ack check. */
            if (!SEQ32_GEQ(ack, s->snd_nxt)) { /* unexpected ack */ return; }
            /* TCP-ROBUST: learn the server's options (MSS/wscale/SACK/TS) from
             * its SYN-ACK so our send path can size segments to the peer's MSS. */
            tcp_parse_options(s, seg, ihl);
            s->rcv_nxt = seq + 1;          /* their SYN consumes one seq */
            s->snd_una = ack;
            tcp_disarm_retransmit(s);       /* our SYN is acked */
            s->state = TCP_ESTABLISHED;
            tcp_send_ack(s);                /* complete the handshake */
        } else if (flags & TCP_SYN) {
            /* Simultaneous open (rare): ack their SYN, stay until their ACK. */
            s->rcv_nxt = seq + 1;
            tcp_send_ack(s);
        }
        return;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_FIN_WAIT:
    case TCP_FIN_WAIT_2:      /* NET-GAPS N3: receive stays open until the peer FINs */
    case TCP_LAST_ACK:        /* NET-HARDENING-2: process the peer's ACK of our FIN */
    case TCP_TIME_WAIT: {
        /* ----------------------------------------------------------
         * ACK processing: free outstanding segment if fully covered.
         * CHANGED: SEQ32_GEQ for wraparound; also update snd_una.
         * ---------------------------------------------------------- */
        /* NET-HARDENING F13 (RFC 793 ACK acceptability): ignore an ACK that
         * acknowledges data we never sent (ack > snd_nxt). Without this an
         * off-path/forged ACK advances snd_una PAST snd_nxt, corrupting the send
         * sequence space (every later SEQ32 comparison, RTT sampling, and the
         * dup-ACK / fast-retransmit logic). Acceptable iff ack <= snd_nxt. */
        if ((flags & TCP_ACK) && SEQ32_GEQ(s->snd_nxt, ack)) {
            int idx = sock_index(s);
            tcp_txq_t* q = txq_of(s);

            if (SEQ32_GT(ack, s->snd_una)) {
                /* --- Advancing cumulative ACK --- */
                /* NET-GAPS N1: consume the TX queue's acked prefix. */
                if (q && q->used) {
                    uint32_t acked = ack - s->snd_una;
                    uint32_t da = (acked < q->used) ? acked : q->used;
                    q->head  = (q->head + da) % TCP_TXQ_SIZE;
                    q->used -= da;
                    if (q->used == 0) {
                        /* Karn: sample RTT only when nothing in this window
                         * was retransmitted; conservatively only on a full
                         * drain (the oldest byte's send time is exact then). */
                        if (q->rtx_count == 0 && q->sent_ms)
                            tcp_rtt_update(s, (uint32_t)(now_ms() - q->sent_ms));
                    } else {
                        q->sent_ms = now_ms();   /* timer restarts for the new oldest */
                    }
                    q->rtx_count = 0;
                    q->rto_ms = s->base_rto_ms ? s->base_rto_ms : TCP_RTO_INIT_MS;
                    /* NET-GAPS N4: grow cwnd once per advancing ACK. */
                    cwnd_on_ack(s);
                }
                /* Control-slot coverage (SYN/FIN, or data on the no-txq
                 * degrade path). TCP-ROBUST adaptive RTO (RFC6298 + Karn):
                 * sample RTT only for a segment that was NOT retransmitted
                 * (count==0), so an ambiguous ACK never corrupts the
                 * estimator. */
                if (s->rt_pending) {
                    uint32_t end = s->rt_seq + s->rt_len +
                                   ((s->rt_flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u);
                    if (SEQ32_GEQ(ack, end)) {
                        if (tcp_rt_count[idx] == 0)
                            tcp_rtt_update(s, (uint32_t)(now_ms() - s->rt_time_ms));
                        tcp_disarm_retransmit(s);
                    }
                }
                s->snd_una = ack;
                tcp_dupack[idx] = 0;
            } else if (ack == s->snd_una && payload_len == 0 &&
                       (flags & (TCP_SYN | TCP_FIN)) == 0 &&
                       s->snd_wnd == prev_snd_wnd &&
                       s->state == TCP_ESTABLISHED &&
                       (s->rt_pending || (q && q->used))) {
                /* TCP-ROBUST (RFC 5681 fast retransmit): a duplicate ACK
                 * does not advance snd_una, carries no data, AND advertises
                 * an UNCHANGED window (NET-HARDENING F4: a pure window update
                 * -- same ack, no data, different window -- is the receiver
                 * opening/shrinking its buffer, NOT a sign of loss; counting
                 * it would spuriously trip fast-retransmit). The 3rd genuine
                 * dup means a segment was almost certainly lost -> resend the
                 * oldest unacked segment NOW (~RTT recovery vs a full RTO).
                 * Fires exactly once at 3 (== not >=), so later dups don't
                 * re-storm the wire. Counted only while something is actually
                 * outstanding (TX queue bytes or the control slot). */
                if (++tcp_dupack[idx] == 3) {
                    cwnd_on_loss(s, 0);       /* N4: halve into recovery */
                    tcp_fast_retransmit(s);   /* N1: resends from snd_una */
                }
            }
            if (s->state == TCP_FIN_WAIT && SEQ32_GEQ(ack, s->snd_nxt)) {
                /* NET-GAPS N3 (RFC 793 FIN-WAIT-2): our FIN is acked but the
                 * peer has NOT closed its half yet -- wait for its FIN with
                 * receive still open. The old collapse straight to TIME_WAIT
                 * dropped any data the peer was still sending (half-close data
                 * loss). time_wait_ms doubles as the FIN_WAIT_2 idle stamp so
                 * a peer that never FINs cannot pin the slot (reaped in
                 * tcp_tick after TCP_TIMEWAIT_MS). */
                s->state = TCP_FIN_WAIT_2;
                s->time_wait_ms = now_ms();
            }
            /* NET-HARDENING-2: in passive close (LAST_ACK) the peer ACKing our
             * FIN completes the close -> CLOSED (an orphaned socket is then
             * reaped by tcp_tick). */
            if (s->state == TCP_LAST_ACK && SEQ32_GEQ(ack, s->snd_nxt)) {
                tcp_disarm_retransmit(s);
                s->state = TCP_CLOSED;
            }
        }

        /* ----------------------------------------------------------
         * Data segment handling.
         * CHANGED: in-order accepted + OOO-drain; duplicates re-ACKed;
         * OOO saved in side-table for later in-order arrival.
         * ---------------------------------------------------------- */
        /* NET-GAPS N3: data is accepted in FIN_WAIT and FIN_WAIT_2 too -- our
         * close only shut the SEND half; the receive half stays open until the
         * peer's FIN arrives (RFC 793). Only TIME_WAIT refuses new data. */
        if (payload_len > 0 && s->state != TCP_TIME_WAIT) {

            uint32_t seg_end = seq + payload_len;

            if (seq == s->rcv_nxt) {
                /* --- In-order segment --- */
                /* Advance only by bytes actually buffered: if the ring is full
                 * the dropped tail stays un-ACKed so the peer retransmits it. */
                s->rcv_nxt += rx_ring_push(s, payload, payload_len);
                /* Try to merge any previously-saved OOO segment. */
                tcp_ooo_drain(s);
                tcp_send_ack(s);

            } else if (SEQ32_GT(s->rcv_nxt, seq) && SEQ32_GEQ(seg_end, s->rcv_nxt)) {
                /* --- Partial duplicate: leading bytes already received.
                 *     Trim the duplicate prefix and accept the new tail. --- */
                uint32_t trim       = s->rcv_nxt - seq;
                const uint8_t* new_start = payload + trim;
                uint16_t       new_len   = (uint16_t)(payload_len - trim);
                if (new_len > 0) {
                    s->rcv_nxt += rx_ring_push(s, new_start, new_len);
                    tcp_ooo_drain(s);
                }
                tcp_send_ack(s);

            } else if (SEQ32_GT(seq, s->rcv_nxt)) {
                /* --- Out-of-order segment: save in the 4-slot row --- */
                /* NET-P1-B: dedupe by seq, take a free slot, else evict the
                 * slot FARTHEST from rcv_nxt if this segment is nearer
                 * (earlier data fills the gap sooner). No buffer (alloc
                 * failed at boot) degrades to drop + re-ACK: correct, the
                 * peer retransmits in order.
                 * NET-HARDENING-2 (RFC 793 receive-window acceptability): only
                 * buffer a future segment that falls WITHIN the advertised window
                 * [rcv_nxt, rcv_nxt + rcv_wnd). An out-of-window (arbitrarily far)
                 * segment is dropped (not buffered) so a peer/attacker cannot
                 * pollute the OOO slots with data that can never drain; we still
                 * re-ACK rcv_nxt so a legitimate sender retransmits in window. */
                if (tcp_ooo && !SEQ32_GEQ(seq, s->rcv_nxt + rcv_wnd(s))) {
                    int idx = sock_index(s);
                    tcp_ooo_slot_t* dst = NULL;
                    tcp_ooo_slot_t* far = NULL;
                    bool dup = false;
                    for (int k = 0; k < TCP_OOO_DEPTH; k++) {
                        tcp_ooo_slot_t* slot = &tcp_ooo[idx][k];
                        if (slot->valid && slot->seq == seq) { dup = true; break; }
                        if (!slot->valid) { if (!dst) dst = slot; continue; }
                        if (!far || SEQ32_GT(slot->seq, far->seq)) far = slot;
                    }
                    if (!dup) {
                        if (!dst && far && SEQ32_GT(far->seq, seq)) dst = far;
                        if (dst) {
                            uint16_t save_len = payload_len;
                            if (save_len > TCP_MSS) save_len = TCP_MSS;
                            dst->valid = true;
                            dst->seq   = seq;
                            dst->len   = save_len;
                            memcpy(dst->data, payload, save_len);
                        }
                    }
                }
                /* ACK our current position to prompt the server to
                 * fill the gap or retransmit the missing segment. */
                tcp_send_ack(s);

            } else {
                /* --- Pure duplicate (seg_end <= rcv_nxt): re-ACK. --- */
                tcp_send_ack(s);
            }
        } else if (payload_len == 0 && (flags & TCP_ACK)) {
            /* Pure ACK (window update, keepalive probe reply, etc.) --
             * already handled above; nothing more to do. */
        }

        /* ----------------------------------------------------------
         * Peer FIN handling.
         * CHANGED: accept FIN even if seq == rcv_nxt (in-order) or if
         * the FIN was piggybacked on the last data segment already
         * consumed above (seq + payload_len == rcv_nxt after push).
         * ---------------------------------------------------------- */
        if (flags & TCP_FIN) {
            /* TCP-ROBUST 2MSL: a FIN retransmitted by the peer during TIME_WAIT
             * (its ACK of our FIN was lost) is ABSORBED -- re-ACK it and restart
             * the 2MSL linger (RFC 793) so the peer's close completes cleanly.
             *
             * NET-HARDENING-1 (FIN_WAIT-2 absorb fix): only absorb an ALREADY-
             * consumed/duplicate FIN (seq < rcv_nxt). The active-close path
             * collapses FIN_WAIT->TIME_WAIT on the ACK of our FIN BEFORE consuming
             * the peer's FIN (no FIN_WAIT_2 state), so a FIRST-TIME in-order FIN
             * (seq == rcv_nxt) can arrive here; it must fall through to the in-
             * order branch below to advance rcv_nxt and ACK the peer's FIN with
             * the correct cumulative ack (rcv_nxt+1). The old code re-ACKed a
             * STALE rcv_nxt, so the peer never saw its FIN acknowledged and
             * retransmitted to RTO exhaustion (clean close -> RST). */
            if (s->state == TCP_TIME_WAIT && SEQ32_GT(s->rcv_nxt, seq)) {
                tcp_send_ack(s);
                s->time_wait_ms = now_ms();
                return;
            }
            /* In-order FIN: advance rcv_nxt. */
            if (seq + payload_len == s->rcv_nxt) {
                s->rcv_nxt += 1;   /* FIN consumes one seq */
                tcp_send_ack(s);
                if (s->state == TCP_ESTABLISHED)   s->state = TCP_CLOSE_WAIT;
                else if (s->state == TCP_FIN_WAIT ||
                         s->state == TCP_FIN_WAIT_2) { s->state = TCP_TIME_WAIT; s->time_wait_ms = now_ms(); }
            } else if (seq == s->rcv_nxt && payload_len == 0) {
                /* FIN with no data (common simultaneous FIN). The payload_len==0
                 * guard is load-bearing: a FIN piggybacked on in-order data whose
                 * payload was DROPPED (rx ring full -> rx_ring_push returned 0, so
                 * rcv_nxt did not advance) leaves seq==rcv_nxt with payload_len>0.
                 * Without the guard this branch would consume the FIN at the wrong
                 * sequence (rcv_nxt += 1 mid-stream) and falsely ACK dropped bytes.
                 * Excluding it lets the segment fall through + re-ACK current
                 * rcv_nxt, so the peer retransmits data+FIN together. */
                s->rcv_nxt += 1;
                tcp_send_ack(s);
                if (s->state == TCP_ESTABLISHED)   s->state = TCP_CLOSE_WAIT;
                else if (s->state == TCP_FIN_WAIT ||
                         s->state == TCP_FIN_WAIT_2) { s->state = TCP_TIME_WAIT; s->time_wait_ms = now_ms(); }
            } else if (payload_len == 0 && SEQ32_GT(s->rcv_nxt, seq)) {
                /* NET-HARDENING F10 (RFC 793): a DUPLICATE, already-consumed FIN
                 * (seq < rcv_nxt) -- the peer retransmitted it because our ACK of
                 * its FIN was lost. Re-ACK so the actively-closing peer's FIN is
                 * acknowledged and it stops retransmitting; otherwise it exhausts
                 * its RTO budget and turns what was a clean close into a RESET.
                 * Mirrors the TIME_WAIT absorb above; fires only for an old pure
                 * FIN, so it never re-ACKs in-order data and cannot ACK-storm. */
                tcp_send_ack(s);
            }
            /* If FIN arrived OOO just ACK current position; we'll re-process
             * it when the gap fills (limitation: no FIN in OOO slot). */
        }
        return;
    }

    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Timer / retransmit with exponential back-off (called from sock_poll)*/
/* ------------------------------------------------------------------ */
/*
 * CHANGED: implements multi-retry exponential back-off up to
 * TCP_RTO_MAX_RETRIES (was retransmit-once then give-up).  Each retry
 * doubles the RTO up to TCP_RTO_MAX_MS.  Uses the per-socket side-table
 * counters tcp_rt_count[] / tcp_rt_rto_ms[].
 */
void tcp_tick(sock_t* s) {
    /* NET-HARDENING-2 (orphan reaper): a socket the app already closed but whose
     * FIN was DEFERRED (orphaned by sock_close) is freed the moment its close
     * completes -- as soon as it reaches CLOSED or TIME_WAIT. An orphan gets NO
     * 2MSL linger, so even a lossy-NIC close cannot pin a slot (the audit's
     * table-exhaustion hazard); it only lived long enough to retransmit the FIN. */
    if (s->orphaned && (s->state == TCP_CLOSED || s->state == TCP_TIME_WAIT)) {
        memset(s, 0, sizeof(*s));
        return;
    }

    /* TCP-ROBUST 2MSL: reap a TIME_WAIT socket once the 2MSL linger elapses, so
     * it no longer sits in TIME_WAIT forever (the prior behavior). A TIME_WAIT
     * socket has no outstanding segment, so this must precede the rt_pending
     * early-return. (Proven deterministically via the RIG-TIME-HOOK clock.) */
    if (s->state == TCP_TIME_WAIT) {
        if (now_ms() - s->time_wait_ms >= TCP_TIMEWAIT_MS) s->state = TCP_CLOSED;
        return;
    }

    /* NET-GAPS N3: an idle FIN_WAIT_2 (our FIN acked, peer never sends its own
     * FIN) is reaped after the same linger, so a vanished peer cannot pin the
     * slot forever. time_wait_ms was stamped at the FIN_WAIT->FIN_WAIT_2
     * transition. Our FIN was acked to get here, so no retransmit is pending. */
    if (s->state == TCP_FIN_WAIT_2) {
        if (now_ms() - s->time_wait_ms >= TCP_TIMEWAIT_MS) s->state = TCP_CLOSED;
        return;
    }

    /* NET-GAPS N1: TX-queue RTO -- retransmit the OLDEST unacked bytes
     * (go-back-N head, resegmented to the effective MSS) with the same
     * backoff + give-up budget as the control slot. Runs BEFORE the control
     * slot so a lost data segment is repaired ahead of a queued FIN. */
    if (tcp_txq && s->state != TCP_CLOSED) {
        tcp_txq_t* q = &tcp_txq[sock_index(s)];
        if (q->used > 0 && now_ms() - q->sent_ms >= (uint64_t)q->rto_ms) {
            if (q->rtx_count >= TCP_RTO_MAX_RETRIES) {
                /* Exhausted: same fate as the control slot's give-up. */
                q->used = 0;
                tcp_disarm_retransmit(s);
                s->reset = true;
                s->state = TCP_CLOSED;
                return;
            }
            uint16_t eff_mss = (s->peer_mss && s->peer_mss < TCP_MSS)
                                   ? s->peer_mss : TCP_MSS;
            uint16_t n = txq_peek_oldest(s, tcp_rtx_buf, eff_mss);
            if (n) {
                cwnd_on_loss(s, 1);   /* N4: an RTO is hard loss -> 1*MSS */
                tcp_xmit(s, TCP_ACK | TCP_PSH, s->snd_una, s->rcv_nxt,
                         tcp_rtx_buf, n);
            }
            q->rtx_count++;
            q->rto_ms *= 2;
            if (q->rto_ms > TCP_RTO_MAX_MS) q->rto_ms = TCP_RTO_MAX_MS;
            q->sent_ms = now_ms();
        }
    }

    if (!s->rt_pending) return;

    int idx        = sock_index(s);
    uint32_t rto   = tcp_rt_rto_ms[idx];

    if (now_ms() - s->rt_time_ms < (uint64_t)rto) return;

    if (tcp_rt_count[idx] >= TCP_RTO_MAX_RETRIES) {
        /* Exhausted retries: give up. */
        tcp_disarm_retransmit(s);
        if (s->state == TCP_SYN_SENT || s->state == TCP_ESTABLISHED ||
            s->state == TCP_FIN_WAIT || s->state == TCP_LAST_ACK) {
            s->reset = true;
            s->state = TCP_CLOSED;   /* orphan reaped on the next tick */
        }
        return;
    }

    /* Retransmit the outstanding segment. */
    uint32_t ack_val = (s->state == TCP_SYN_SENT) ? 0 : s->rcv_nxt;
    tcp_xmit(s, s->rt_flags, s->rt_seq, ack_val,
             s->rt_len ? s->rt_data : NULL, s->rt_len);

    /* Exponential back-off: double RTO, cap at max. */
    tcp_rt_count[idx]++;
    rto *= 2;
    if (rto > TCP_RTO_MAX_MS) rto = TCP_RTO_MAX_MS;
    tcp_rt_rto_ms[idx] = rto;
    s->rt_time_ms      = now_ms();
    s->rt_done         = true;   /* keep rt_done semantics for any code that reads it */
}

#ifdef NET_SELFTEST
/* ------------------------------------------------------------------ */
/* NET-GAPS rig hooks (NET_SELFTEST only; compiled out of default)     */
/* ------------------------------------------------------------------ */
/* Stage `len` bytes (<= TCP_MSS) as one SENT-but-UNACKED segment starting at
 * snd_nxt, WITHOUT transmitting -- a deterministic mirror of what tcp_send()'s
 * remembering does per segment, minus its blocking drains (unusable at boot,
 * see NETP1K). The rig drives retransmission via tcp_tick() + the
 * RIG-TIME-HOOK clock and asserts on what hits the tx tap. */
void tcp_rig_stage_tx(sock_t* s, const void* data, uint16_t len) {
    if (len > TCP_MSS) len = TCP_MSS;
    if (tcp_txq)
        txq_append(s, (const uint8_t*)data, len);
    else
        tcp_arm_retransmit(s, TCP_ACK | TCP_PSH, s->snd_nxt, data, len);
    s->snd_nxt += len;
}

/* Congestion-window observer for the rig (N4). */
uint32_t tcp_rig_cwnd(sock_t* s) {
    tcp_txq_t* q = txq_of(s);
    return q ? q->cwnd : 0;
}
#endif /* NET_SELFTEST */
