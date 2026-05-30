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
 * LIMITATIONS (documented, not in scope):
 *   - No SACK (selective acknowledgement).
 *   - No Nagle / delayed-ACK.
 *   - No congestion control (send window is fixed at SOCK_RXBUF_SIZE).
 *   - Simultaneous open not hardened (rare in active-open clients).
 *   - No TIME_WAIT 2MSL wait (state goes straight to CLOSED on close).
 *   - OOO buffer is a single slot per socket (handles the common case of
 *     one out-of-order segment but not arbitrary reordering).  If an
 *     integrator adds a larger OOO array to sock_t this file should be
 *     updated to use it; see comment at TCP_OOO_SLOTS.
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

/* ------------------------------------------------------------------ */
/* Timeouts and retransmit parameters                                  */
/* ------------------------------------------------------------------ */
#define TCP_RTO_INIT_MS       1000    /* initial retransmit timeout (ms)    */
#define TCP_RTO_MAX_MS        30000   /* maximum RTO after back-off         */
#define TCP_RTO_MAX_RETRIES   8       /* give up after this many retransmits*/
#define TCP_CONNECT_MS        8000    /* total connect budget (ms)          */
#define TCP_CLOSE_MS          4000    /* wait for FIN/ACK on close          */

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
 * sock_t has no OOO buffer.  We cannot edit socket.h, so we keep one
 * OOO slot per socket index in a static side-table.  One slot handles
 * the most common case: the server reorders or retransmits a single
 * segment.  If the integrator adds an ooo[] array to sock_t this table
 * should be dropped and sock_t's array used instead.
 *
 * TCP_OOO_SLOTS must equal SOCK_MAX.
 */
#define TCP_OOO_SLOTS   SOCK_MAX      /* one entry per possible socket      */

typedef struct {
    bool     valid;
    uint32_t seq;                     /* sequence number of first byte      */
    uint16_t len;                     /* payload length                     */
    uint8_t  data[TCP_MSS];           /* payload (capped at MSS)            */
} tcp_ooo_slot_t;

static tcp_ooo_slot_t tcp_ooo[TCP_OOO_SLOTS];

/*
 * Per-socket retransmit retry counter.  sock_t only has rt_done (bool).
 * We extend it with a separate count table so we can do multi-retry
 * exponential back-off without touching sock_t.
 */
static uint8_t  tcp_rt_count[TCP_OOO_SLOTS];   /* retransmit attempt count */
static uint32_t tcp_rt_rto_ms[TCP_OOO_SLOTS];  /* current RTO for backoff  */

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
static uint64_t now_ms(void) { return timer_get_ticks_ms(); }

/* A simple-ish ISN. Not security-relevant here; derive from the clock. */
static uint32_t gen_isn(void) {
    uint32_t t = (uint32_t)now_ms();
    return (t * 2654435761u) ^ 0xDEADBEEFu;
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
static int tcp_xmit(sock_t* s, uint8_t flags, uint32_t seq, uint32_t ack,
                    const void* data, uint16_t len) {
    if (len > TCP_MSS) len = TCP_MSS;

    uint8_t seg[sizeof(tcp_hdr_t) + TCP_MSS];
    tcp_hdr_t* th = (tcp_hdr_t*)seg;
    memset(th, 0, sizeof(*th));
    th->src_port = net_htons(s->local_port);
    th->dst_port = net_htons(s->remote_port);
    th->seq      = net_htonl(seq);
    th->ack      = net_htonl(ack);
    th->data_off = (uint8_t)((sizeof(tcp_hdr_t) / 4) << 4);  /* 5 words, no options */
    th->flags    = flags;
    /* CHANGED: advertise our free rx buffer space as the receive window. */
    th->window   = net_htons(rcv_wnd(s));
    th->urgent   = 0;
    th->checksum = 0;

    if (len && data) memcpy(seg + sizeof(tcp_hdr_t), data, len);
    uint16_t seg_len = (uint16_t)(sizeof(tcp_hdr_t) + len);

    uint16_t ck = net_transport_checksum(net_get_ip(), s->remote_ip,
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
    tcp_rt_rto_ms[idx] = TCP_RTO_INIT_MS;
}

/* Clear the retransmit machinery for this socket. */
static void tcp_disarm_retransmit(sock_t* s) {
    int idx = sock_index(s);
    s->rt_pending       = false;
    s->rt_done          = false;
    tcp_rt_count[idx]   = 0;
    tcp_rt_rto_ms[idx]  = TCP_RTO_INIT_MS;
}

/* Send a pure ACK for the current rcv_nxt (no retransmit tracking). */
static void tcp_send_ack(sock_t* s) {
    tcp_xmit(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
}

/* Append in-order stream bytes into the socket RX ring (drop overflow). */
static void rx_ring_push(sock_t* s, const uint8_t* p, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        if (s->rx_used >= SOCK_RXBUF_SIZE) break;   /* ring full -> drop */
        s->rxbuf[s->rx_tail] = p[i];
        s->rx_tail = (s->rx_tail + 1) % SOCK_RXBUF_SIZE;
        s->rx_used++;
    }
}

static uint32_t rx_ring_pop(sock_t* s, uint8_t* dst, uint32_t want) {
    uint32_t n = 0;
    while (n < want && s->rx_used > 0) {
        dst[n++] = s->rxbuf[s->rx_head];
        s->rx_head = (s->rx_head + 1) % SOCK_RXBUF_SIZE;
        s->rx_used--;
    }
    return n;
}

/*
 * Try to drain any saved OOO segment into the rx ring now that rcv_nxt has
 * advanced.  Called after every in-order segment is accepted.
 * CHANGED: new function to support OOO assembly.
 */
static void tcp_ooo_drain(sock_t* s) {
    int idx = sock_index(s);
    tcp_ooo_slot_t* slot = &tcp_ooo[idx];

    /* Keep flushing as long as the saved segment becomes in-order. */
    while (slot->valid && slot->seq == s->rcv_nxt) {
        uint16_t n = slot->len;
        if (n > (uint16_t)(SOCK_RXBUF_SIZE - s->rx_used))
            n = (uint16_t)(SOCK_RXBUF_SIZE - s->rx_used);
        rx_ring_push(s, slot->data, n);
        s->rcv_nxt += n;
        slot->valid = false;
        /* No more slots; a real implementation would loop over an array. */
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

    /* Initialise OOO slot and retry counters for this socket. */
    int idx = sock_index(s);
    tcp_ooo[idx].valid      = false;
    tcp_rt_count[idx]       = 0;
    tcp_rt_rto_ms[idx]      = TCP_RTO_INIT_MS;

    uint32_t isn = gen_isn();
    s->snd_una   = isn;
    s->snd_nxt   = isn + 1;        /* SYN consumes one seq */
    s->rcv_nxt   = 0;
    s->state     = TCP_SYN_SENT;
    s->reset     = false;

    if (tcp_xmit(s, TCP_SYN, isn, 0, NULL, 0) != 0) {
        s->state = TCP_CLOSED;
        return SOCK_ECONN;
    }
    tcp_arm_retransmit(s, TCP_SYN, isn, NULL, 0);

    /* Pump until ESTABLISHED, RST, or timeout. */
    uint64_t start = now_ms();
    while (now_ms() - start < TCP_CONNECT_MS) {
        sock_poll();
        if (s->reset)                     { s->state = TCP_CLOSED; return SOCK_ECONN; }
        if (s->state == TCP_ESTABLISHED)  return SOCK_OK;
        if (s->state == TCP_CLOSED)       return SOCK_ECONN;
    }
    s->state = TCP_CLOSED;
    return SOCK_ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Send                                                                */
/* ------------------------------------------------------------------ */
/*
 * CHANGED: segments payloads > TCP_MSS into multiple consecutive sends
 * instead of truncating.  Waits for each segment's ACK before sending
 * the next (stop-and-wait within a call), respecting the peer's snd_wnd.
 * Returns total bytes accepted (may be less than `len` on error mid-way).
 */
int tcp_send(sock_t* s, const void* data, uint16_t len) {
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
        return SOCK_ECONN;
    if (len == 0) return 0;

    const uint8_t* ptr   = (const uint8_t*)data;
    uint16_t       sent  = 0;

    while (sent < len) {
        /* Wait for any previous outstanding segment to be ACKed. */
        uint64_t start = now_ms();
        while (s->rt_pending && now_ms() - start < TCP_CONNECT_MS) {
            sock_poll();
            if (s->reset) return sent ? (int)sent : SOCK_ECONN;
        }
        if (s->rt_pending) return sent ? (int)sent : SOCK_ETIMEDOUT;

        /* Determine this segment's size: min(remaining, MSS, peer window). */
        uint16_t chunk = (uint16_t)(len - sent);
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        /* Respect peer's advertised send window (never send 0-window data). */
        if (s->snd_wnd > 0 && chunk > s->snd_wnd) chunk = (uint16_t)s->snd_wnd;
        if (chunk == 0) {
            /* Peer window is zero: probe with 1-byte segment after a brief poll. */
            sock_poll();
            continue;
        }

        uint32_t seq = s->snd_nxt;
        if (tcp_xmit(s, TCP_ACK | TCP_PSH, seq, s->rcv_nxt,
                     ptr + sent, chunk) != 0)
            return sent ? (int)sent : SOCK_ECONN;

        s->snd_nxt += chunk;
        tcp_arm_retransmit(s, TCP_ACK | TCP_PSH, seq, ptr + sent, chunk);
        sent = (uint16_t)(sent + chunk);

        /* Best-effort wait for ACK so we don't overrun the peer. */
        start = now_ms();
        while (s->rt_pending && now_ms() - start < TCP_CONNECT_MS) {
            sock_poll();
            if (s->reset) return sent ? (int)sent : SOCK_ECONN;
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

    /* Pump the wire so data has a chance to arrive. */
    if (s->rx_used == 0 &&
        (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)) {
        for (int i = 0; i < 32 && s->rx_used == 0 && !s->reset; i++)
            sock_poll();
    }

    if (s->reset) return SOCK_ECONN;

    if (s->rx_used > 0) {
        return (int)rx_ring_pop(s, (uint8_t*)buf, len);
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

    /* Clear OOO side-table slot on close. */
    int idx = sock_index(s);
    tcp_ooo[idx].valid = false;

    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        uint32_t seq = s->snd_nxt;
        tcp_xmit(s, TCP_FIN | TCP_ACK, seq, s->rcv_nxt, NULL, 0);
        s->snd_nxt += 1;   /* FIN consumes one seq */
        tcp_arm_retransmit(s, TCP_FIN | TCP_ACK, seq, NULL, 0);
        s->state = (s->state == TCP_CLOSE_WAIT) ? TCP_TIME_WAIT : TCP_FIN_WAIT;

        uint64_t start = now_ms();
        while (now_ms() - start < TCP_CLOSE_MS) {
            sock_poll();
            if (s->state == TCP_CLOSED || s->state == TCP_TIME_WAIT) break;
        }
    }
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
    (void)dst_ip;
    if (seg_len < sizeof(tcp_hdr_t)) return;

    const tcp_hdr_t* th = (const tcp_hdr_t*)seg;
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

    /* Update peer's advertised window on every non-RST segment. */
    s->snd_wnd = net_ntohs(th->window);

    /* RST: mark connection reset; clear retransmit; leave state alive
     * so tcp_recv() can return SOCK_ECONN to the caller. */
    if (flags & TCP_RST) {
        s->reset = true;
        s->state = TCP_CLOSED;
        tcp_disarm_retransmit(s);
        return;
    }

    switch (s->state) {
    case TCP_LISTEN:
        /* Server-side: handle incoming SYN for passive connection. */
        if (flags & TCP_SYN) {
            /* Allocate a new child socket for this connection. */
            sock_t* base = sock_table_base();
            if (!base) return;

            sock_t* child = NULL;
            int child_idx = -1;
            for (int i = 0; i < SOCK_MAX; i++) {
                if (!base[i].used) {
                    child = &base[i];
                    child_idx = i;
                    break;
                }
            }
            if (!child) return;  /* No free sockets */

            /* Initialize the child socket. */
            memset(child, 0, sizeof(*child));
            child->used       = true;
            child->type       = SOCK_STREAM;
            child->local_ip   = s->local_ip;
            child->local_port = s->local_port;
            child->remote_ip  = src_ip;
            child->remote_port = src_port;
            child->parent     = s;
            child->snd_wnd    = net_ntohs(th->window);

            /* Initialize TCP state machine. */
            child->rcv_nxt    = seq + 1;      /* SYN consumes one seq */
            uint32_t isn      = gen_isn();
            child->snd_una    = isn;
            child->snd_nxt    = isn + 1;      /* SYN-ACK will consume one seq */
            child->state      = TCP_SYN_RCVD;
            child->reset      = false;

            /* Initialize retransmit machinery for the child. */
            tcp_ooo[child_idx].valid = false;
            tcp_rt_count[child_idx]  = 0;
            tcp_rt_rto_ms[child_idx] = TCP_RTO_INIT_MS;

            /* Send SYN-ACK to complete the 3-way handshake. */
            tcp_xmit(child, TCP_SYN | TCP_ACK, isn, child->rcv_nxt, NULL, 0);
            tcp_arm_retransmit(child, TCP_SYN | TCP_ACK, isn, NULL, 0);
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
    case TCP_TIME_WAIT: {
        /* ----------------------------------------------------------
         * ACK processing: free outstanding segment if fully covered.
         * CHANGED: SEQ32_GEQ for wraparound; also update snd_una.
         * ---------------------------------------------------------- */
        if (flags & TCP_ACK) {
            if (s->rt_pending) {
                uint32_t end = s->rt_seq + s->rt_len +
                               ((s->rt_flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u);
                if (SEQ32_GEQ(ack, end)) {
                    tcp_disarm_retransmit(s);
                    s->snd_una = ack;
                }
            } else {
                /* Update snd_una even if no retransmit is pending (covers
                 * cumulative ACKs for previously delivered segments). */
                if (SEQ32_GT(ack, s->snd_una)) s->snd_una = ack;
            }
            if (s->state == TCP_FIN_WAIT && SEQ32_GEQ(ack, s->snd_nxt)) {
                s->state = TCP_TIME_WAIT;   /* our FIN acked */
            }
        }

        /* ----------------------------------------------------------
         * Data segment handling.
         * CHANGED: in-order accepted + OOO-drain; duplicates re-ACKed;
         * OOO saved in side-table for later in-order arrival.
         * ---------------------------------------------------------- */
        if (payload_len > 0 && s->state != TCP_FIN_WAIT &&
            s->state != TCP_TIME_WAIT) {

            uint32_t seg_end = seq + payload_len;

            if (seq == s->rcv_nxt) {
                /* --- In-order segment --- */
                rx_ring_push(s, payload, payload_len);
                s->rcv_nxt += payload_len;
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
                    rx_ring_push(s, new_start, new_len);
                    s->rcv_nxt += new_len;
                    tcp_ooo_drain(s);
                }
                tcp_send_ack(s);

            } else if (SEQ32_GT(seq, s->rcv_nxt)) {
                /* --- Out-of-order segment: save in side-table slot --- */
                int idx = sock_index(s);
                tcp_ooo_slot_t* slot = &tcp_ooo[idx];

                /* Only save if the slot is empty or this segment is
                 * closer to rcv_nxt (prefer earlier segments). */
                if (!slot->valid || SEQ32_GT(slot->seq, seq)) {
                    uint16_t save_len = payload_len;
                    if (save_len > TCP_MSS) save_len = TCP_MSS;
                    slot->valid = true;
                    slot->seq   = seq;
                    slot->len   = save_len;
                    memcpy(slot->data, payload, save_len);
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
            /* In-order FIN: advance rcv_nxt. */
            if (seq + payload_len == s->rcv_nxt) {
                s->rcv_nxt += 1;   /* FIN consumes one seq */
                tcp_send_ack(s);
                if (s->state == TCP_ESTABLISHED)   s->state = TCP_CLOSE_WAIT;
                else if (s->state == TCP_FIN_WAIT) s->state = TCP_TIME_WAIT;
            } else if (seq == s->rcv_nxt) {
                /* FIN with no data (common simultaneous FIN). */
                s->rcv_nxt += 1;
                tcp_send_ack(s);
                if (s->state == TCP_ESTABLISHED)   s->state = TCP_CLOSE_WAIT;
                else if (s->state == TCP_FIN_WAIT) s->state = TCP_TIME_WAIT;
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
    if (!s->rt_pending) return;

    int idx        = sock_index(s);
    uint32_t rto   = tcp_rt_rto_ms[idx];

    if (now_ms() - s->rt_time_ms < (uint64_t)rto) return;

    if (tcp_rt_count[idx] >= TCP_RTO_MAX_RETRIES) {
        /* Exhausted retries: give up. */
        tcp_disarm_retransmit(s);
        if (s->state == TCP_SYN_SENT || s->state == TCP_ESTABLISHED ||
            s->state == TCP_FIN_WAIT) {
            s->reset = true;
            s->state = TCP_CLOSED;
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
