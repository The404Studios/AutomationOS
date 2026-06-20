/*
 * socket.c -- BSD-ish socket table, central RX poll/demux, shared IPv4 TX.
 * =======================================================================
 *
 * This is the hub of the transport layer that the networking engineer added on
 * top of the existing Ethernet/ARP/IPv4/ICMP stack in kernel/net/net.c.
 *
 *   - Owns the socket table and the BSD-ish API (sock_socket/connect/send/recv/
 *     close, sendto/recvfrom, bind).
 *   - Owns sock_poll(): the single function that pulls frames off the NIC via
 *     net_recv(), demuxes IPv4 -> UDP/TCP into per-socket queues, and runs TCP
 *     timers/retransmit. EVERYTHING that makes RX progress goes through here.
 *   - Provides the shared checksum helpers and the IPv4 frame builder/transmit
 *     used by udp.c and tcp.c (declared in socket.h / via ip_tx() below).
 *
 * Poll-mode, single-threaded -- same model as net.c. No IRQs.
 *
 * Scope: kernel/net/socket.c (new).
 */

#include "../include/socket.h"
#include "../include/net.h"
#include "../include/route.h"
#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/string.h"
#include "../include/drivers.h"   /* timer_get_ticks_ms */
#include "../include/mem.h"       /* kmalloc + copy_from_user/copy_to_user */
#include "../include/sched.h"    /* process_get_current (for owner_pid)   */

/* ------------------------------------------------------------------ */
/* Socket table                                                        */
/* ------------------------------------------------------------------ */
/* Heap-backed (NOT a static array): each sock_t is ~21KB (8KB TCP rxbuf + 8
 * queued UDP datagrams + TCP retransmit slot), so SOCK_MAX of them in .bss
 * (~338KB) would overrun the tiny kernel/initrd gap. kmalloc'd once in
 * sock_init() instead; .bss cost here is just a pointer. */
static sock_t* g_socks = NULL;
static bool   g_inited = false;

/* Exposed so tcp.c can map a sock_t* back to its slot index (see sock_index). */
sock_t* sock_table_base(void) { return g_socks; }
static uint16_t g_next_port = 49152;   /* ephemeral range start */

/* RX scratch for sock_poll(). Static (single-threaded poll). */
static uint8_t g_rx[ETH_MAX_FRAME];
/* TX scratch for ip_tx(). */
static uint8_t g_tx[ETH_MAX_FRAME];

static sock_t* sock_from_fd(int fd) {
    if (!g_socks) return NULL;
    if (fd < 0 || fd >= SOCK_MAX) return NULL;
    if (!g_socks[fd].used) return NULL;
    return &g_socks[fd];
}

uint16_t sock_alloc_port(void) {
    uint16_t p = 49152;

    /* AUDIT FIX (gap-fix-round): guard before the table exists, and detect
     * collisions so a wrapped ephemeral range can't hand out a live port. */
    if (!g_socks) {
        p = g_next_port++;
        if (g_next_port == 0 || g_next_port < 49152) g_next_port = 49152;
        return p;
    }

    const uint16_t MAX_SCAN = 1000;   /* bounded retry (ports 49152-65535) */
    uint16_t scans = 0;
    for (;;) {
        p = g_next_port++;
        if (g_next_port == 0 || g_next_port < 49152) g_next_port = 49152;

        bool in_use = false;
        for (int i = 0; i < SOCK_MAX; i++) {
            if (g_socks[i].used && g_socks[i].local_port == p) { in_use = true; break; }
        }
        if (!in_use) return p;
        if (++scans >= MAX_SCAN) return p;   /* exhausted: return anyway */
    }
}

/* ------------------------------------------------------------------ */
/* Checksums (shared)                                                  */
/* ------------------------------------------------------------------ */
uint16_t net_inet_checksum(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip,
                                uint8_t proto, const void* seg, uint32_t len) {
    /* IPv4 pseudo-header: src(4) dst(4) zero(1) proto(1) tcp/udp-len(2). */
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += proto;
    sum += len & 0xFFFF;

    const uint8_t* p = (const uint8_t*)seg;
    uint32_t n = len;
    while (n > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }
    if (n) sum += (uint32_t)(p[0] << 8);

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* IPv4 transmit (shared by udp.c / tcp.c)                             */
/* ------------------------------------------------------------------ */
/* Resolve the next-hop MAC for dst_ip. On QEMU user-net everything can go via
 * the gateway, and slirp answers ARP for the gateway, so we resolve the gateway
 * MAC (or the dst directly if cached). Returns 0 on success. Best-effort
 * ARP-request + a few polls if not cached. */
static int resolve_mac(uint32_t dst_ip, uint8_t out[ETH_ALEN]) {
    uint32_t gw = NET_QEMU_GATEWAY;
    if (net_arp_lookup(dst_ip, out) == 0) return 0;
    if (net_arp_lookup(gw, out) == 0) return 0;

    /* Not cached yet -- resolve the gateway (next hop). The previous code
     * busy-polled a fixed 200000 iterations WITHOUT a time bound; under QEMU
     * slirp the ARP reply round-trips slower than that tight loop completes,
     * so the very first outbound packet (e.g. a DNS query) failed. Wait on a
     * WALL-CLOCK budget (PIT millisecond counter) instead and re-issue the
     * who-has periodically while we drain RX, so first-contact TX waits
     * correctly for ARP. (net_init() also pre-resolves the gateway, so this is
     * normally a no-op fast path.) */
    const uint64_t budget_ms  = 300;   /* upper bound on first-contact wait */
    const uint64_t reissue_ms = 50;    /* re-ARP cadence                    */
    /* HARD ITERATION CAP — the load-bearing backstop. This function runs inside a
     * syscall, which executes with interrupts DISABLED (IA32_FMASK clears IF), so
     * the PIT IRQ cannot fire and timer_get_ticks_ms() is FROZEN. That makes the
     * wall-clock budget below useless on its own: `now - start` stays 0 forever,
     * so the timeout never fires. On real hardware where the NIC has no link
     * (e.g. the T410's 82577LM with no cable) ARP never resolves and this loop
     * would spin FOREVER with interrupts off -> the whole cooperative system
     * freezes (this is exactly the autodhcp-at-boot freeze). The iteration cap
     * guarantees termination regardless of the clock; the wall-clock budget is
     * the (faster) early-out on QEMU where ticks do advance via a different path.
     * Mirrors the iter_cap that net_init()'s ARP settle already uses for the same
     * frozen-tick reason. */
    const uint32_t iter_cap   = 200000;
    uint64_t start = timer_get_ticks_ms();
    uint64_t last_req = 0;
    uint32_t iters = 0;
    for (;;) {
        uint64_t now = timer_get_ticks_ms();

        /* (Re)issue the who-has on first pass and every reissue_ms. */
        if (last_req == 0 || now - last_req >= reissue_ms) {
            net_arp_request(gw);
            last_req = now;
        }

        net_recv(g_rx, sizeof(g_rx));
        if (net_arp_lookup(gw, out) == 0) return 0;
        if (net_arp_lookup(dst_ip, out) == 0) return 0;

        if (now - start >= budget_ms) break;
        if (++iters >= iter_cap) break;   /* frozen-tick backstop: never hang */
    }
    return -1;
}

/*
 * Send one IPv4 fragment.
 */
static int ip_send_fragment(uint8_t dmac[ETH_ALEN], uint32_t dst_ip,
                            uint8_t proto, uint16_t id, uint16_t frag_off,
                            bool more_frags, const uint8_t* data, uint16_t len) {
    uint8_t* f = g_tx;

    /* Ethernet header. */
    eth_hdr_t* eh = (eth_hdr_t*)f;
    memcpy(eh->dst, dmac, ETH_ALEN);
    net_get_mac(eh->src);
    eh->ethertype = net_htons(ETH_P_IP);

    /* IPv4 header. */
    ipv4_hdr_t* ip = (ipv4_hdr_t*)(f + ETH_HLEN);
    uint16_t ip_total = (uint16_t)(sizeof(ipv4_hdr_t) + len);
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = net_htons(ip_total);
    ip->id        = net_htons(id);
    /* frag_off is in 8-byte units; MF flag is bit 13. */
    uint16_t frag_flags = (frag_off / 8) & 0x1FFF;
    if (more_frags) frag_flags |= 0x2000;
    ip->frag_off  = net_htons(frag_flags);
    ip->ttl       = 64;
    ip->proto     = proto;
    ip->checksum  = 0;
    ip->src       = net_htonl(net_get_ip());
    ip->dst       = net_htonl(dst_ip);
    ip->checksum  = net_htons(net_inet_checksum(ip, sizeof(ipv4_hdr_t)));

    /* Payload. */
    memcpy((uint8_t*)ip + sizeof(ipv4_hdr_t), data, len);

    uint16_t total = (uint16_t)(ETH_HLEN + ip_total);
    /* Pad runt frames to the Ethernet minimum. */
    if (total < ETH_MIN_FRAME) {
        memset(f + total, 0, (uint32_t)(ETH_MIN_FRAME - total));
        total = ETH_MIN_FRAME;
    }
    return (net_send(f, total) > 0) ? 0 : -1;
}

/*
 * Build and transmit one IPv4 packet carrying `seg` (a fully-built UDP/TCP
 * segment INCLUDING its transport header and checksum). proto is
 * IPPROTO_UDP/IPPROTO_TCP. dst_ip is host byte order.
 *
 * Fragments if seg_len > MTU payload (1480 bytes).
 * Returns 0 on success.
 */
int ip_tx(uint32_t dst_ip, uint8_t proto, const void* seg, uint16_t seg_len) {
    static uint16_t ip_id = 1;

#ifdef NET_SELFTEST
    /* NET-P1-A0: while the in-kernel test rig is live it swallows every
     * outbound segment here -- BEFORE net_up()/ARP/the NIC are touched --
     * so selftests can assert on exactly what the stack tried to send with
     * zero hardware dependency. Inactive rig = zero behavior change. */
    {
        extern int net_testrig_tx_tap(uint32_t dst, uint8_t proto,
                                      const void* seg, uint16_t len);
        if (net_testrig_tx_tap(dst_ip, proto, seg, seg_len)) return 0;
    }
#endif

    if (!net_up()) return -1;

    /* Resolve next-hop MAC (use routing table if available). */
    uint8_t dmac[ETH_ALEN];
    uint32_t next_hop = dst_ip;
    uint32_t gateway = 0;
    uint32_t iface = 0;

    /* Try routing table lookup. */
    if (route_lookup(dst_ip, &gateway, &iface) == 0) {
        if (gateway != 0) next_hop = gateway;
    } else {
        /* Fallback: use hardcoded gateway for QEMU user-net. */
        next_hop = NET_QEMU_GATEWAY;
    }

    if (resolve_mac(next_hop, dmac) != 0) return -1;

    uint16_t mtu_payload = ETH_MTU - sizeof(ipv4_hdr_t); /* 1480 bytes */

    /* If packet fits in one frame, send it unfragmented. */
    if (seg_len <= mtu_payload) {
        return ip_send_fragment(dmac, dst_ip, proto, ip_id++, 0, false, seg, seg_len);
    }

    /* Fragment the packet. */
    uint16_t id = ip_id++;
    uint16_t offset = 0;
    const uint8_t* data = (const uint8_t*)seg;

    while (offset < seg_len) {
        uint16_t chunk = seg_len - offset;
        bool more = true;

        /* Fragment payload must be a multiple of 8 bytes (except last). */
        if (chunk > mtu_payload) {
            chunk = mtu_payload & ~7u; /* round down to 8-byte boundary */
        } else {
            more = false; /* last fragment */
        }

        if (ip_send_fragment(dmac, dst_ip, proto, id, offset, more,
                             data + offset, chunk) != 0) {
            return -1;
        }

        offset = (uint16_t)(offset + chunk);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Socket lookup helpers used by udp.c / tcp.c                          */
/* ------------------------------------------------------------------ */
sock_t* sock_find_udp(uint16_t local_port) {
    if (!g_socks) return NULL;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].used && g_socks[i].type == SOCK_DGRAM &&
            g_socks[i].local_port == local_port) {
            return &g_socks[i];
        }
    }
    return NULL;
}

sock_t* sock_find_tcp(uint32_t remote_ip, uint16_t remote_port,
                      uint16_t local_port) {
    if (!g_socks) return NULL;

    /* First pass: exact match for connected/connecting sockets. */
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t* s = &g_socks[i];
        if (!s->used || s->type != SOCK_STREAM) continue;
        if (s->local_port != local_port) continue;
        if (s->state != TCP_LISTEN && s->state != TCP_CLOSED) {
            /* Match the connected peer. */
            if (s->remote_port != 0 &&
                (s->remote_ip != remote_ip || s->remote_port != remote_port))
                continue;
            return s;
        }
    }

    /* Second pass: find listening socket on this port. */
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t* s = &g_socks[i];
        if (!s->used || s->type != SOCK_STREAM) continue;
        if (s->state == TCP_LISTEN && s->local_port == local_port) {
            return s;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* RX demux                                                            */
/* ------------------------------------------------------------------ */
/* Demux one IPv4 packet (UDP/TCP) into the owning socket. ARP/ICMP were
 * already handled inside net_recv() before we get here. */
static void ipv4_demux(const uint8_t* ip_start, uint16_t ip_avail) {
    if (ip_avail < sizeof(ipv4_hdr_t)) return;
    const ipv4_hdr_t* ip = (const ipv4_hdr_t*)ip_start;

    if ((ip->ver_ihl >> 4) != 4) return;
    uint16_t ihl = (uint16_t)((ip->ver_ihl & 0x0F) * 4);
    if (ihl < sizeof(ipv4_hdr_t)) return;

    uint16_t tot = net_ntohs(ip->total_len);
    if (tot > ip_avail) tot = ip_avail;
    if (tot < ihl) return;

    uint32_t src = net_ntohl(ip->src);
    uint32_t dst = net_ntohl(ip->dst);

    /* Only packets destined for us (or broadcast) are interesting. */
    if (dst != net_get_ip() && dst != 0xFFFFFFFFu) return;

    const uint8_t* seg = ip_start + ihl;
    uint16_t seg_len = (uint16_t)(tot - ihl);

    if (ip->proto == IPPROTO_UDP) {
        udp_input(src, dst, seg, seg_len);
    } else if (ip->proto == IPPROTO_TCP) {
        tcp_input(src, dst, seg, seg_len);
    }
}

#ifdef NET_SELFTEST
/* NET-P1-A0: raw IPv4 injection seam for the in-kernel test rig
 * (kernel/net/net_testrig.c) -- hands a crafted packet to the SAME demux
 * the NIC path uses, so rig-tested behavior is real-path behavior. */
void sock_testrig_inject_ipv4(const uint8_t* ip_start, uint16_t ip_avail) {
    ipv4_demux(ip_start, ip_avail);
}
#endif

int sock_poll(void) {
    if (!net_up() || !g_socks) return 0;
    int frames = 0;

    /* Drain whatever the NIC has pending this call (bounded). */
    for (int i = 0; i < 64; i++) {
        int n = net_recv(g_rx, sizeof(g_rx));
        if (n <= 0) break;
        frames++;
        if ((uint16_t)n < ETH_HLEN) continue;

        const eth_hdr_t* eh = (const eth_hdr_t*)g_rx;
        if (net_ntohs(eh->ethertype) == ETH_P_IP) {
            ipv4_demux(g_rx + ETH_HLEN, (uint16_t)(n - ETH_HLEN));
        }
    }

    /* Run TCP timers / retransmit-once on all live TCP sockets. */
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].used && g_socks[i].type == SOCK_STREAM) {
            tcp_tick(&g_socks[i]);
        }
    }
    /* NET-P1-A: half-open side-table timers (SYN-ACK retransmit + expiry). */
    tcp_synq_tick();
    return frames;
}

/* POLL-SELECT-0: non-destructive readiness of a socket fd. See socket.h.
 * Returns -1 if fd is not a live socket. Pure reads of the socket state — no
 * data dequeued, no timers advanced (the caller pumps sock_poll separately). */
int sock_poll_bits(int fd) {
    sock_t* so = sock_from_fd(fd);
    if (!so) return -1;
    int bits = 0;
    if (so->type == SOCK_DGRAM) {
        if (so->dq_count > 0) bits |= SOCKPOLL_READ;
        bits |= SOCKPOLL_WRITE;                 /* UDP is connectionless: always sendable */
        return bits;
    }
    /* TCP */
    if (so->reset) { bits |= SOCKPOLL_ERR | SOCKPOLL_READ; }
    if (so->rx_used > 0) bits |= SOCKPOLL_READ; /* buffered stream bytes */
    if (so->state == TCP_ESTABLISHED && so->snd_wnd > 0) bits |= SOCKPOLL_WRITE;
    if (so->state == TCP_CLOSE_WAIT) {
        /* peer sent FIN: a read returns EOF (won't block) and the stream is hanging up */
        bits |= SOCKPOLL_READ | SOCKPOLL_HUP;
    }
    return bits;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void sock_init(void) {
    if (g_socks == NULL) {
        g_socks = (sock_t*)kmalloc(sizeof(sock_t) * SOCK_MAX);
        if (g_socks == NULL) {
            /* NET-P1-E heap-headroom assert: SOCK_MAX=32 makes this ONE
             * ~1.4 MB allocation -- if the heap can't carry it, say so
             * LOUDLY (all networking is dead without the table). */
            kprintf("[SOCK] init FAILED: no heap for socket table (%u bytes, SOCK_MAX=%u)\n",
                    (unsigned)(sizeof(sock_t) * SOCK_MAX), (unsigned)SOCK_MAX);
            g_inited = false;
            return;
        }
        kprintf("[SOCK] table: %u sockets, %u KB\n", (unsigned)SOCK_MAX,
                (unsigned)((sizeof(sock_t) * SOCK_MAX) / 1024));
    }
    memset(g_socks, 0, sizeof(sock_t) * SOCK_MAX);
    g_inited = true;
    g_next_port = 49152;
    /* NET-P1-B: the per-socket OOO reassembly buffers live on the heap for
     * the same .bss/initrd reason this table does. Re-init zeroes them. */
    tcp_buffers_init();
}

int sock_socket(int type) {
    if (!g_inited) sock_init();
    if (!g_socks) return SOCK_ENOMEM;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return SOCK_EINVAL;

    for (int i = 0; i < SOCK_MAX; i++) {
        if (!g_socks[i].used) {
            sock_t* s = &g_socks[i];
            memset(s, 0, sizeof(*s));
            s->used       = true;
            s->type       = type;
            s->local_ip   = net_get_ip();
            s->local_port = sock_alloc_port();
            s->state      = TCP_CLOSED;
            s->snd_wnd    = 1024;
            /* Track the creating process so sock_cleanup_process can
             * reclaim leaked sockets on process death. */
            {
                process_t* cur = process_get_current();
                s->owner_pid = cur ? cur->pid : 0;
            }
            return i;
        }
    }
    return SOCK_ENOMEM;
}

int sock_bind(int s, uint16_t local_port) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;
    /* Reject if another socket of the same type already owns the port. */
    for (int i = 0; i < SOCK_MAX; i++) {
        if (i != s && g_socks[i].used && g_socks[i].type == so->type &&
            g_socks[i].local_port == local_port) {
            return SOCK_EINVAL;
        }
    }
    so->local_port = local_port;
    return SOCK_OK;
}

int sock_connect(int s, uint32_t ip, uint16_t port) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;

    so->remote_ip   = ip;
    so->remote_port = port;

    if (so->type == SOCK_DGRAM) {
        /* UDP "connect" just records the default peer. */
        return SOCK_OK;
    }
    /* TCP: perform the active-open handshake (pumps sock_poll internally). */
    return tcp_connect(so, ip, port);
}

int sock_listen(int s, int backlog) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;
    if (so->type != SOCK_STREAM) return SOCK_EINVAL;  /* TCP only */
    if (so->state != TCP_CLOSED) return SOCK_EINVAL;  /* must be unconnected */

    so->backlog = backlog;
    so->accept_queue = NULL;
    so->accept_next = NULL;
    so->parent = NULL;
    so->state = TCP_LISTEN;

    return SOCK_OK;
}

int sock_accept(int s) {
    sock_t* listener = sock_from_fd(s);
    if (!listener) return SOCK_EBADF;
    if (listener->state != TCP_LISTEN) return SOCK_EINVAL;

    /* Check if there's a completed connection in the accept queue. */
    if (!listener->accept_queue) return SOCK_EAGAIN;  /* would block */

    /* Dequeue the first completed connection. */
    sock_t* child = listener->accept_queue;
    listener->accept_queue = child->accept_next;
    child->accept_next = NULL;
    child->parent = NULL;

    /* Find the descriptor for this child socket. */
    if (!g_socks) return SOCK_EBADF;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (&g_socks[i] == child) {
            return i;
        }
    }

    /* Should never happen if accept_queue was valid. */
    return SOCK_EBADF;
}

int sock_send(int s, const void* buf, uint32_t len) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;
    if (buf == NULL || len == 0) return SOCK_EINVAL;

    if (so->type == SOCK_DGRAM) {
        if (so->remote_port == 0) return SOCK_ECONN;   /* not "connected" */
        return sock_sendto(s, buf, len, so->remote_ip, so->remote_port);
    }
    /* TCP: tcp_send() handles segmentation internally, so pass the full
     * length (capped to uint16_t max).  The old code truncated to TCP_MSS
     * here, which silently dropped all bytes beyond 1024 on large sends. */
    if (so->state != TCP_ESTABLISHED && so->state != TCP_CLOSE_WAIT)
        return SOCK_ECONN;
    return tcp_send(so, buf, (uint16_t)(len > 0xFFFF ? 0xFFFF : len));
}

int sock_recv(int s, void* buf, uint32_t len) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;
    if (buf == NULL || len == 0) return SOCK_EINVAL;

    if (so->type == SOCK_DGRAM) {
        return sock_recvfrom(s, buf, len, NULL, NULL);
    }
    /* TCP: pull from the stream ring; pump the wire a bit if empty. */
    int r = tcp_recv(so, buf, (uint16_t)(len > 0xFFFF ? 0xFFFF : len));
    if (r == 0 && so->state == TCP_ESTABLISHED) {
        /* Nothing buffered yet but still connected -> would-block. */
        return SOCK_EAGAIN;
    }
    return r;
}

int sock_sendto(int s, const void* buf, uint32_t len,
                uint32_t ip, uint16_t port) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;
    if (so->type != SOCK_DGRAM) return SOCK_EINVAL;
    if (buf == NULL || len == 0) return SOCK_EINVAL;
    if (len > UDP_DGRAM_MAX) len = UDP_DGRAM_MAX;
    return udp_send_to(so, ip, port, buf, (uint16_t)len);
}

int sock_recvfrom(int s, void* buf, uint32_t len,
                  uint32_t* out_ip, uint16_t* out_port) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;
    if (so->type != SOCK_DGRAM) return SOCK_EINVAL;
    if (buf == NULL || len == 0) return SOCK_EINVAL;

    if (so->dq_count == 0) return SOCK_EAGAIN;

    /* Pop the oldest datagram. */
    uint8_t idx = so->dq_head;
    uint16_t dlen = so->dgram[idx].len;
    uint16_t cpy = (dlen > len) ? (uint16_t)len : dlen;
    memcpy(buf, so->dgram[idx].data, cpy);
    if (out_ip)   *out_ip   = so->dgram[idx].src_ip;
    if (out_port) *out_port = so->dgram[idx].src_port;

    so->dq_head = (uint8_t)((so->dq_head + 1) % UDP_QUEUE_DEPTH);
    so->dq_count--;
    return (int)cpy;
}

int sock_close(int s) {
    sock_t* so = sock_from_fd(s);
    if (!so) return SOCK_EBADF;

    if (so->type == SOCK_STREAM &&
        (so->state == TCP_ESTABLISHED || so->state == TCP_CLOSE_WAIT)) {
        tcp_close(so);   /* send FIN, drive a few polls */
    }

    /* Unlink from listener bookkeeping BEFORE zeroing, so no accept_queue /
     * accept_next dangles at this freed slot (else a later sock_accept would
     * hand back a wiped or reused socket). */
    if (so->parent) {
        /* We are a child possibly still queued on the parent's accept_queue. */
        sock_t* lst = so->parent;
        if (lst->accept_queue == so) {
            lst->accept_queue = so->accept_next;
        } else {
            for (sock_t* it = lst->accept_queue; it; it = it->accept_next) {
                if (it->accept_next == so) { it->accept_next = so->accept_next; break; }
            }
        }
    }
    if (so->state == TCP_LISTEN) {
        /* Closing a listener: close ALL child sockets (both queued-and-accepted
         * on the accept_queue AND still-handshaking SYN_RCVD children that
         * haven't been enqueued yet).
         *
         * BUG-FIX (socket leak): the old code merely orphaned accept_queue
         * children (cleared parent/accept_next) but left them used=true with
         * no owner -- they leaked permanently because owner_pid==0 (set by
         * tcp_input's memset) meant sock_cleanup_process never found them.
         *
         * BUG-FIX (SYN_RCVD dangling parent): SYN_RCVD children have
         * parent==so but are NOT on the accept_queue.  If the listener closes
         * and their slot is reused, completing the handshake writes into
         * garbage (use-after-free).  The table scan below catches them.
         */

        /* First, detach the accept_queue so recursive sock_close on children
         * doesn't re-walk it (sock_close checks so->parent and unlinks). */
        sock_t* aq = so->accept_queue;
        so->accept_queue = NULL;

        /* Close everything on the accept queue. */
        while (aq) {
            sock_t* nx = aq->accept_next;
            aq->parent = NULL;
            aq->accept_next = NULL;
            /* Find the child's fd index and close it properly. */
            for (int ci = 0; ci < SOCK_MAX; ci++) {
                if (&g_socks[ci] == aq) {
                    /* Send FIN if established, then zero the slot. */
                    if (aq->type == SOCK_STREAM &&
                        (aq->state == TCP_ESTABLISHED || aq->state == TCP_CLOSE_WAIT)) {
                        tcp_close(aq);
                    }
                    memset(aq, 0, sizeof(*aq));
                    break;
                }
            }
            aq = nx;
        }

        /* Scan the whole table for SYN_RCVD (or any other) children still
         * pointing at this listener.  These aren't on the accept_queue yet. */
        for (int ci = 0; ci < SOCK_MAX; ci++) {
            if (g_socks[ci].used && g_socks[ci].parent == so) {
                g_socks[ci].parent = NULL;
                g_socks[ci].accept_next = NULL;
                if (g_socks[ci].type == SOCK_STREAM &&
                    (g_socks[ci].state == TCP_ESTABLISHED ||
                     g_socks[ci].state == TCP_CLOSE_WAIT)) {
                    tcp_close(&g_socks[ci]);
                }
                memset(&g_socks[ci], 0, sizeof(g_socks[ci]));
            }
        }
    }

    memset(so, 0, sizeof(*so));
    return SOCK_OK;
}

/* ------------------------------------------------------------------ */
/* Process cleanup: close all sockets owned by a dying process         */
/* ------------------------------------------------------------------ */
/*
 * sock_cleanup_process - reclaim sockets held by a dying process.
 *
 * Called from process_unref() when the last reference to a process drops
 * to zero.  Without this, sockets created by a process that exits without
 * explicitly closing them stay marked `used` forever, leaking descriptors
 * (and, for TCP, leaving half-open connections).
 */
void sock_cleanup_process(uint32_t pid) {
    if (!g_socks) return;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].used && g_socks[i].owner_pid == pid) {
            sock_close(i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* In-kernel self test                                                 */
/* ------------------------------------------------------------------ */
void sock_selftest(void) {
    if (!net_up()) {
        kprintf("[SOCK] selftest skipped: net not up\n");
        return;
    }
    sock_init();

    /* --- UDP: fire a datagram at the gateway's discard/echo and report TX. --- */
    kprintf("[SOCK] selftest: UDP sendto 10.0.2.2:7 ...\n");
    int us = sock_socket(SOCK_DGRAM);
    if (us < 0) { kprintf("[SOCK] UDP socket failed\n"); return; }
    const char* msg = "ping-udp";
    int sr = sock_sendto(us, msg, 8, NET_QEMU_GATEWAY, 7);
    kprintf("[SOCK] UDP sendto rc=%d\n", sr);
    /* Pump for a possible reply (slirp may not echo; this is best-effort). */
    for (int i = 0; i < 50000; i++) {
        sock_poll();
        uint8_t rb[64]; uint32_t fip; uint16_t fp;
        int r = sock_recvfrom(us, rb, sizeof(rb), &fip, &fp);
        if (r > 0) {
            kprintf("[SOCK] UDP RX %d bytes from %u.%u.%u.%u:%u\n", r,
                    (fip>>24)&0xFF,(fip>>16)&0xFF,(fip>>8)&0xFF,fip&0xFF, fp);
            break;
        }
    }
    sock_close(us);

    /* --- TCP: attempt active-open to the gateway. slirp doesn't run a TCP
     * server on arbitrary ports, so this is expected to time out -- it
     * exercises the handshake/retransmit path either way. --- */
    kprintf("[SOCK] selftest: TCP connect 10.0.2.2:9 ...\n");
    int ts = sock_socket(SOCK_STREAM);
    if (ts < 0) { kprintf("[SOCK] TCP socket failed\n"); return; }
    int cr = sock_connect(ts, NET_QEMU_GATEWAY, 9);
    if (cr == 0) {
        kprintf("[SOCK] TCP connect OK (handshake completed)\n");
    } else {
        kprintf("[SOCK] TCP connect rc=%d (no server -- handshake path ok)\n", cr);
    }
    sock_close(ts);
    kprintf("[SOCK] selftest done\n");
}

/* ------------------------------------------------------------------ */
/* Syscall handlers (proposed surface -- see socket.h)                 */
/* ------------------------------------------------------------------ */
#include "../include/mem.h"   /* copy_from_user / copy_to_user / COPY_SUCCESS */

int64_t sys_sock_socket(uint64_t type, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    return sock_socket((int)type);
}

int64_t sys_sock_connect(uint64_t s, uint64_t ip, uint64_t port,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;(void)a5;(void)a6;
    return sock_connect((int)s, (uint32_t)ip, (uint16_t)port);
}

int64_t sys_sock_send(uint64_t s, uint64_t buf, uint64_t len,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;(void)a5;(void)a6;
    if (buf == 0 || len == 0) return SOCK_EINVAL;
    if (len > 0xFFFF) len = 0xFFFF;

    /* Send in chunks through a stack buffer to avoid a large kmalloc while
     * still supporting sends up to 64KB (the uint16_t TCP len limit). */
    uint8_t kbuf[4096];
    uint32_t total_sent = 0;
    uint32_t remaining = (uint32_t)len;

    while (remaining > 0) {
        uint32_t chunk = (remaining > sizeof(kbuf)) ? sizeof(kbuf) : remaining;
        if (copy_from_user(kbuf, (const void*)(buf + total_sent), chunk) != COPY_SUCCESS)
            return total_sent ? (int64_t)total_sent : SOCK_EINVAL;
        int r = sock_send((int)s, kbuf, chunk);
        if (r <= 0)
            return total_sent ? (int64_t)total_sent : (int64_t)r;
        total_sent += (uint32_t)r;
        remaining  -= (uint32_t)r;
        if ((uint32_t)r < chunk) break;   /* short send: don't spin */
    }
    return (int64_t)total_sent;
}

int64_t sys_sock_recv(uint64_t s, uint64_t buf, uint64_t len,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;(void)a5;(void)a6;
    if (buf == 0 || len == 0) return SOCK_EINVAL;
    uint8_t kbuf[4096];
    uint32_t cap = (len > sizeof(kbuf)) ? sizeof(kbuf) : (uint32_t)len;
    int r = sock_recv((int)s, kbuf, cap);
    if (r > 0) {
        if (copy_to_user((void*)buf, kbuf, (size_t)r) != COPY_SUCCESS)
            return SOCK_EINVAL;
    }
    return r;
}

int64_t sys_sock_close(uint64_t s, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    return sock_close((int)s);
}

int64_t sys_sock_sendto(uint64_t s, uint64_t buf, uint64_t len,
                        uint64_t ip, uint64_t port, uint64_t a6) {
    (void)a6;
    if (buf == 0 || len == 0) return SOCK_EINVAL;
    if (len > UDP_DGRAM_MAX) len = UDP_DGRAM_MAX;
    uint8_t kbuf[UDP_DGRAM_MAX];
    if (copy_from_user(kbuf, (const void*)buf, (size_t)len) != COPY_SUCCESS)
        return SOCK_EINVAL;
    return sock_sendto((int)s, kbuf, (uint32_t)len, (uint32_t)ip, (uint16_t)port);
}

int64_t sys_sock_recvfrom(uint64_t s, uint64_t buf, uint64_t len,
                          uint64_t out_addr, uint64_t a5, uint64_t a6) {
    (void)a5;(void)a6;
    if (buf == 0 || len == 0) return SOCK_EINVAL;
    uint8_t kbuf[UDP_DGRAM_MAX];
    uint32_t cap = (len > sizeof(kbuf)) ? sizeof(kbuf) : (uint32_t)len;
    uint32_t fip = 0; uint16_t fp = 0;
    int r = sock_recvfrom((int)s, kbuf, cap, &fip, &fp);
    if (r > 0) {
        if (copy_to_user((void*)buf, kbuf, (size_t)r) != COPY_SUCCESS)
            return SOCK_EINVAL;
        if (out_addr) {
            sock_addr_t a = { fip, fp, 0 };
            if (copy_to_user((void*)out_addr, &a, sizeof(a)) != COPY_SUCCESS)
                return SOCK_EINVAL;
        }
    }
    return r;
}

int64_t sys_sock_poll(uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    return sock_poll();
}

int64_t sys_sock_bind(uint64_t s, uint64_t port, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;(void)a4;(void)a5;(void)a6;
    return sock_bind((int)s, (uint16_t)port);
}

int64_t sys_sock_listen(uint64_t s, uint64_t backlog, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;(void)a4;(void)a5;(void)a6;
    return sock_listen((int)s, (int)backlog);
}

int64_t sys_sock_accept(uint64_t s, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    return sock_accept((int)s);
}
