/*
 * net_testrig.c -- NET-P1-A0: the deterministic in-kernel network test rig.
 * ========================================================================
 *
 * The stack is poll-mode and tcp_input()/udp_input() are plain functions, so
 * the most provable way to test TCP behavior is to bypass the NIC entirely:
 *
 *   INJECT:  hand-craft raw IPv4 packets and feed them straight into the same
 *            demux the NIC path uses (sock_testrig_inject_ipv4 -> ipv4_demux).
 *            Any order, drops, dupes -- fully deterministic, no QEMU slirp
 *            timing, identical behavior in QEMU and on the T410.
 *   CAPTURE: a tap at the head of ip_tx() swallows every outbound segment
 *            (before ARP / the NIC are ever touched) into a small ring, so a
 *            selftest can assert on exactly what the stack tried to send
 *            (SYN-ACKs, pure ACKs, zero-window probes, retransmits).
 *
 * Every NET-P1 brick (SYN side-table, OOO reassembly, persist probes) proves
 * itself through this rig with a serial marker.
 *
 * GATING: the whole file is behind -DNET_SELFTEST (NET_SELFTEST=1 build env).
 * Default builds compile this TU empty and ip_tx carries no tap, so default
 * kernel behavior is unchanged. The rig runs once at boot, right after
 * sock_init(), BEFORE userspace exists -- it may freely reset the socket
 * table when it finishes.
 *
 * Scope: kernel/net/net_testrig.c (new, NET-P1-A0).
 */
#ifdef NET_SELFTEST

#include "../include/socket.h"
#include "../include/net.h"
#include "../include/kernel.h"
#include "../include/string.h"

/* socket.c (NET_SELFTEST-gated): raw IPv4 injection into ipv4_demux. */
void sock_testrig_inject_ipv4(const uint8_t* ip_start, uint16_t ip_avail);

/* ------------------------------------------------------------------ */
/* TX capture ring                                                     */
/* ------------------------------------------------------------------ */
#define RIG_CAP_MAX   32    /* captured segments per test step          */
#define RIG_CAP_BYTES 64    /* head bytes kept per segment (headers)    */

typedef struct {
    uint32_t dst_ip;        /* host order                               */
    uint8_t  proto;         /* IPPROTO_UDP / IPPROTO_TCP                */
    uint16_t seg_len;       /* full transport segment length            */
    uint8_t  seg[RIG_CAP_BYTES];
} rig_cap_t;

static rig_cap_t g_cap[RIG_CAP_MAX];
static int       g_cap_n      = 0;
static int       g_rig_active = 0;

#define RIG_PEER_IP 0x0A000263u     /* 10.0.2.99 -- the fake test peer */

/* NET-P1-C autoresponder: while armed, the tap PLAYS THE PEER for one
 * connection -- counts 1-byte persist probes (opens the window after the
 * 3rd) and ACKs real data segments so sends complete cleanly. Injecting
 * from inside the tap re-enters tcp_input on the same single-threaded call
 * stack; pure ACKs produce no TX, so the recursion terminates. */
static int      g_zw_armed = 0, g_zw_probes = 0;
static uint16_t g_zw_peer_port, g_zw_local_port;
static uint32_t g_zw_peer_seq;

/* TCP-ROBUST NETP1I: when armed, the next injected packet is corrupted so the
 * RX checksum verification must DROP it. g_corrupt_ip flips a bit in the IPv4
 * header checksum; g_corrupt_tcp mangles a TCP payload byte AFTER the checksum
 * was computed (so the stored checksum stays non-zero but no longer matches). */
static int g_corrupt_ip = 0, g_corrupt_tcp = 0;

static void rig_inject_tcp(uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port,
                           uint32_t seq, uint32_t ack, uint8_t flags,
                           uint16_t window,
                           const void* payload, uint16_t len);

/* Called from the head of ip_tx(): when the rig is live, record + swallow the
 * segment (return 1 -> ip_tx reports success without touching ARP/the NIC).
 * Inactive rig returns 0 -> the normal transmit path runs unchanged. */
int net_testrig_tx_tap(uint32_t dst_ip, uint8_t proto,
                       const void* seg, uint16_t seg_len) {
    if (!g_rig_active) return 0;
    if (g_cap_n < RIG_CAP_MAX) {
        rig_cap_t* c = &g_cap[g_cap_n++];
        c->dst_ip  = dst_ip;
        c->proto   = proto;
        c->seg_len = seg_len;
        uint16_t n = (seg_len < RIG_CAP_BYTES) ? seg_len : RIG_CAP_BYTES;
        memcpy(c->seg, seg, n);
    }

    if (g_zw_armed && proto == IPPROTO_TCP && seg_len >= sizeof(tcp_hdr_t)) {
        const tcp_hdr_t* th = (const tcp_hdr_t*)seg;
        uint8_t  ihl  = (uint8_t)((th->data_off >> 4) * 4);
        uint16_t plen = (seg_len > ihl) ? (uint16_t)(seg_len - ihl) : 0;
        if (net_ntohs(th->dst_port) == g_zw_peer_port) {
            if (plen == 1) {
                /* A zero-window persist probe. Stay shut for two, then
                 * re-advertise an open window on the third (NOT accepting
                 * the probe byte: ack = the probe's own seq). */
                g_zw_probes++;
                if (g_zw_probes == 3)
                    rig_inject_tcp(RIG_PEER_IP, g_zw_peer_port,
                                   net_get_ip(), g_zw_local_port,
                                   g_zw_peer_seq, net_ntohl(th->seq),
                                   TCP_ACK, 2048, NULL, 0);
            } else if (plen > 1) {
                /* Real data after the window opened: ACK it all. */
                rig_inject_tcp(RIG_PEER_IP, g_zw_peer_port,
                               net_get_ip(), g_zw_local_port,
                               g_zw_peer_seq, net_ntohl(th->seq) + plen,
                               TCP_ACK, 2048, NULL, 0);
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Raw packet builders                                                 */
/* ------------------------------------------------------------------ */
static uint8_t g_pkt[ETH_MAX_FRAME];   /* injection scratch (boot-time only) */
static uint8_t g_seg[ETH_MAX_FRAME];   /* transport segment scratch          */

/* Wrap a transport segment in an IPv4 header and feed it to the demux. */
static void rig_inject(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                       const void* seg, uint16_t seg_len) {
    ipv4_hdr_t* ip = (ipv4_hdr_t*)g_pkt;
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = net_htons((uint16_t)(sizeof(ipv4_hdr_t) + seg_len));
    ip->id        = net_htons(0x4242);
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = proto;
    ip->checksum  = 0;
    ip->src       = net_htonl(src_ip);
    ip->dst       = net_htonl(dst_ip);
    ip->checksum  = net_htons(net_inet_checksum(ip, sizeof(ipv4_hdr_t)));
    if (g_corrupt_ip) ip->checksum ^= 0x0100;   /* NETP1I: invalidate IP header */
    memcpy(g_pkt + sizeof(ipv4_hdr_t), seg, seg_len);
    sock_testrig_inject_ipv4(g_pkt, (uint16_t)(sizeof(ipv4_hdr_t) + seg_len));
}

/* Craft + inject one UDP datagram (checksums computed properly). */
static void rig_inject_udp(uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port,
                           const void* payload, uint16_t len) {
    udp_hdr_t* uh = (udp_hdr_t*)g_seg;
    uint16_t ulen = (uint16_t)(sizeof(udp_hdr_t) + len);
    uh->src_port = net_htons(src_port);
    uh->dst_port = net_htons(dst_port);
    uh->length   = net_htons(ulen);
    uh->checksum = 0;
    if (len) memcpy(g_seg + sizeof(udp_hdr_t), payload, len);
    uh->checksum = net_htons(net_transport_checksum(src_ip, dst_ip,
                                                    IPPROTO_UDP, g_seg, ulen));
    rig_inject(src_ip, dst_ip, IPPROTO_UDP, g_seg, ulen);
}

/* Craft + inject one TCP segment (no options; checksum computed properly). */
static void rig_inject_tcp(uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port,
                           uint32_t seq, uint32_t ack, uint8_t flags,
                           uint16_t window,
                           const void* payload, uint16_t len) {
    tcp_hdr_t* th = (tcp_hdr_t*)g_seg;
    uint16_t tlen = (uint16_t)(sizeof(tcp_hdr_t) + len);
    th->src_port = net_htons(src_port);
    th->dst_port = net_htons(dst_port);
    th->seq      = net_htonl(seq);
    th->ack      = net_htonl(ack);
    th->data_off = (uint8_t)((sizeof(tcp_hdr_t) / 4) << 4);
    th->flags    = flags;
    th->window   = net_htons(window);
    th->checksum = 0;
    th->urgent   = 0;
    if (len) memcpy(g_seg + sizeof(tcp_hdr_t), payload, len);
    th->checksum = net_htons(net_transport_checksum(src_ip, dst_ip,
                                                    IPPROTO_TCP, g_seg, tlen));
    /* NETP1I: corrupt a payload byte AFTER checksumming so the stored checksum
     * no longer matches the data -> tcp_input must drop it. */
    if (g_corrupt_tcp && len) g_seg[sizeof(tcp_hdr_t)] ^= 0xFF;
    rig_inject(src_ip, dst_ip, IPPROTO_TCP, g_seg, tlen);
}

/* ------------------------------------------------------------------ */
/* The A0 selftest: prove INJECT (loopback) and CAPTURE (tap).         */
/* ------------------------------------------------------------------ */
void net_testrig_selftest(void) {
    uint32_t my_ip   = net_get_ip();
    const uint32_t peer_ip = RIG_PEER_IP;
    int loopback = 0, cap = 0;

    g_rig_active = 1;
    g_cap_n      = 0;

    /* -- loopback: a crafted UDP datagram lands in a bound socket's queue
     *    and comes back byte-exact through sock_recvfrom. Proves the whole
     *    inject -> ipv4_demux -> udp_input -> dgram-ring -> API path. */
    int u = sock_socket(SOCK_DGRAM);
    if (u >= 0 && sock_bind(u, 47000) == 0) {
        rig_inject_udp(peer_ip, 7777, my_ip, 47000, "RIGPING", 7);
        uint8_t  buf[16];
        uint32_t sip = 0; uint16_t sport = 0;
        int n = sock_recvfrom(u, buf, sizeof(buf), &sip, &sport);
        loopback = (n == 7 && memcmp(buf, "RIGPING", 7) == 0 &&
                    sip == peer_ip && sport == 7777) ? 1 : 0;
    }

    /* -- capture: a crafted SYN at a listening TCP socket must make the
     *    stack EMIT a SYN-ACK -- which the tap records instead of the NIC.
     *    Proves inject -> tcp_input(LISTEN) -> tcp_xmit -> ip_tx -> tap. */
    int t = sock_socket(SOCK_STREAM);
    if (t >= 0 && sock_bind(t, 47001) == 0 && sock_listen(t, 4) == 0) {
        g_cap_n = 0;
        rig_inject_tcp(peer_ip, 40000, my_ip, 47001,
                       /*seq*/1000, /*ack*/0, TCP_SYN, 4096, NULL, 0);
        if (g_cap_n >= 1) {
            const rig_cap_t* c  = &g_cap[g_cap_n - 1];
            const tcp_hdr_t* th = (const tcp_hdr_t*)c->seg;
            cap = (c->proto == IPPROTO_TCP &&
                   c->dst_ip == peer_ip &&
                   c->seg_len >= sizeof(tcp_hdr_t) &&
                   th->flags == (TCP_SYN | TCP_ACK) &&
                   net_ntohl(th->ack) == 1001 &&
                   net_ntohs(th->dst_port) == 40000 &&
                   net_ntohs(th->src_port) == 47001) ? 1 : 0;
        }
        /* RST the half-open child (exercises the RST path on the way out). */
        rig_inject_tcp(peer_ip, 40000, my_ip, 47001,
                       /*seq*/1001, /*ack*/0, TCP_RST, 0, NULL, 0);
    }

    /* Boot-time rig, pre-userspace: a full table reset wipes the test
     * listener + the RST'd child in one stroke (sock_init is re-callable). */
    sock_init();

    kprintf("NETRIG: %s loopback=%d cap=%d\n",
            (loopback && cap) ? "PASS" : "FAIL", loopback, cap);

    /* ---------------------------------------------------------------- */
    /* NET-P1-A: the SYN side-table proof. 24 SYNs from distinct peer    */
    /* ports hit one listener (backlog 4). EVERY SYN must get a SYN-ACK  */
    /* (half-opens cost table entries, not sockets), and only the 4      */
    /* promoted handshakes may consume sock_t slots.                     */
    /* ---------------------------------------------------------------- */
    {
        int syns = 24, synacks = 0, established = 0, sockused = 0;

        int l = sock_socket(SOCK_STREAM);
        if (l >= 0 && sock_bind(l, 47002) == 0 && sock_listen(l, 4) == 0) {
            g_cap_n = 0;
            for (int i = 0; i < syns; i++) {
                rig_inject_tcp(peer_ip, (uint16_t)(40001 + i), my_ip, 47002,
                               /*seq*/ 5000u + (uint32_t)i * 100u, /*ack*/ 0,
                               TCP_SYN, 4096, NULL, 0);
            }
            for (int i = 0; i < g_cap_n; i++) {
                const tcp_hdr_t* th = (const tcp_hdr_t*)g_cap[i].seg;
                if (g_cap[i].proto == IPPROTO_TCP &&
                    th->flags == (TCP_SYN | TCP_ACK)) synacks++;
            }

            /* Promote the first 4: ACK each with the ISN read back from its
             * captured SYN-ACK (the rig sees exactly what the peer would). */
            for (int i = 0; i < 4; i++) {
                uint16_t want_port = (uint16_t)(40001 + i);
                for (int c = 0; c < g_cap_n; c++) {
                    const tcp_hdr_t* th = (const tcp_hdr_t*)g_cap[c].seg;
                    if (g_cap[c].proto == IPPROTO_TCP &&
                        th->flags == (TCP_SYN | TCP_ACK) &&
                        net_ntohs(th->dst_port) == want_port) {
                        uint32_t isn = net_ntohl(th->seq);
                        rig_inject_tcp(peer_ip, want_port, my_ip, 47002,
                                       /*seq*/ 5001u + (uint32_t)i * 100u,
                                       /*ack*/ isn + 1, TCP_ACK, 4096,
                                       NULL, 0);
                        break;
                    }
                }
            }

            /* Count promotions: accept() drains the queue; the table shows
             * the children's slots. */
            for (int i = 0; i < 4; i++) {
                int fd = sock_accept(l);
                if (fd >= 0) established++;
            }
            sock_t* base = sock_table_base();
            if (base) {
                for (int i = 0; i < SOCK_MAX; i++) {
                    if (base[i].used && base[i].type == SOCK_STREAM &&
                        base[i].state == TCP_ESTABLISHED &&
                        base[i].local_port == 47002) sockused++;
                }
            }
        }

        sock_init();   /* wipe the test listener + children */
        kprintf("NETP1A: SYNQ %s syns=%d synacks=%d established=%d sockused=%d\n",
                (synacks == syns && established == 4 && sockused == 4)
                    ? "PASS" : "FAIL",
                syns, synacks, established, sockused);
    }

    /* ---------------------------------------------------------------- */
    /* NET-P1-B: 4-slot OOO reassembly. Establish a connection, deliver  */
    /* segments #2,#3,#4 first (three gaps buffered), then #1 -- the     */
    /* drain must hand back all 4*1460 = 5840 bytes in order.            */
    /* ---------------------------------------------------------------- */
    {
        int reassembled = 0;
        static uint8_t pat[4 * TCP_MSS];     /* per-segment byte pattern  */
        static uint8_t got[4 * TCP_MSS];
        for (int i = 0; i < 4 * TCP_MSS; i++)
            pat[i] = (uint8_t)(0xA0 + (i / TCP_MSS));

        int fd = -1;
        int l = sock_socket(SOCK_STREAM);
        if (l >= 0 && sock_bind(l, 47003) == 0 && sock_listen(l, 2) == 0) {
            /* Handshake: SYN -> read ISN from the captured SYN-ACK -> ACK. */
            g_cap_n = 0;
            rig_inject_tcp(peer_ip, 41000, my_ip, 47003,
                           7000, 0, TCP_SYN, 4096, NULL, 0);
            if (g_cap_n >= 1) {
                const tcp_hdr_t* th = (const tcp_hdr_t*)g_cap[0].seg;
                uint32_t isn = net_ntohl(th->seq);
                rig_inject_tcp(peer_ip, 41000, my_ip, 47003,
                               7001, isn + 1, TCP_ACK, 4096, NULL, 0);
                fd = sock_accept(l);
            }
        }
        if (fd >= 0) {
            /* Peer stream starts at seq 7001. Deliver out of order:
             * #2, #3, #4 (all buffered as gaps), then #1 (drain-merges). */
            uint32_t base = 7001;
            for (int i = 1; i < 4; i++)
                rig_inject_tcp(peer_ip, 41000, my_ip, 47003,
                               base + (uint32_t)i * TCP_MSS, 0,
                               TCP_ACK | TCP_PSH, 4096,
                               pat + i * TCP_MSS, TCP_MSS);
            rig_inject_tcp(peer_ip, 41000, my_ip, 47003,
                           base, 0, TCP_ACK | TCP_PSH, 4096, pat, TCP_MSS);

            int n;
            while (reassembled < 4 * TCP_MSS &&
                   (n = sock_recv(fd, got + reassembled,
                                  (uint32_t)(4 * TCP_MSS - reassembled))) > 0)
                reassembled += n;
            if (reassembled == 4 * TCP_MSS &&
                memcmp(got, pat, 4 * TCP_MSS) != 0)
                reassembled = -1;             /* right count, wrong bytes */
        }

        sock_init();
        kprintf("NETP1B: OOO %s slots=4 reassembled=%d\n",
                (reassembled == 4 * TCP_MSS) ? "PASS" : "FAIL", reassembled);
    }

    /* ---------------------------------------------------------------- */
    /* NET-P1-C: zero-window persist probe. Establish with the peer      */
    /* advertising window 0, then sock_send 64 bytes: the persist timer  */
    /* must emit 1-byte probes (backoff) until the autoresponder opens   */
    /* the window on the 3rd, then the data flows and gets ACKed.        */
    /* ---------------------------------------------------------------- */
    {
        int probes = 0, delivered = 0;
        int fd = -1;
        int l = sock_socket(SOCK_STREAM);
        if (l >= 0 && sock_bind(l, 47004) == 0 && sock_listen(l, 2) == 0) {
            g_cap_n = 0;
            rig_inject_tcp(peer_ip, 42000, my_ip, 47004,
                           9000, 0, TCP_SYN, 4096, NULL, 0);
            if (g_cap_n >= 1) {
                const tcp_hdr_t* th = (const tcp_hdr_t*)g_cap[0].seg;
                uint32_t isn = net_ntohl(th->seq);
                /* Final ACK advertises a ZERO window: the child starts
                 * established but unsendable-to. */
                rig_inject_tcp(peer_ip, 42000, my_ip, 47004,
                               9001, isn + 1, TCP_ACK, 0, NULL, 0);
                fd = sock_accept(l);
            }
        }
        if (fd >= 0) {
            g_zw_armed      = 1;
            g_zw_probes     = 0;
            g_zw_peer_port  = 42000;
            g_zw_local_port = 47004;
            g_zw_peer_seq   = 9001;
            static uint8_t data[64];
            for (int i = 0; i < 64; i++) data[i] = 'X';
            int n = sock_send(fd, data, 64);
            delivered = (n == 64) ? 1 : 0;
            probes    = g_zw_probes;
            g_zw_armed = 0;
        }
        sock_init();
        kprintf("NETP1C: ZWND %s probes=%d delivered=%d\n",
                (probes == 3 && delivered == 1) ? "PASS" : "FAIL",
                probes, delivered);
    }

    /* ---------------------------------------------------------------- */
    /* NET-P1-D: UDP queue depth. Inject exactly UDP_QUEUE_DEPTH         */
    /* datagrams without draining; every one must be retrievable.        */
    /* ---------------------------------------------------------------- */
    {
        int queued = 0, dropped = 0;
        int u = sock_socket(SOCK_DGRAM);
        if (u >= 0 && sock_bind(u, 47005) == 0) {
            for (int i = 0; i < UDP_QUEUE_DEPTH; i++) {
                uint8_t b = (uint8_t)i;
                rig_inject_udp(peer_ip, 7777, my_ip, 47005, &b, 1);
            }
            uint8_t buf[4];
            while (sock_recvfrom(u, buf, sizeof(buf), NULL, NULL) > 0)
                queued++;
            dropped = UDP_QUEUE_DEPTH - queued;
        }
        sock_init();
        kprintf("NETP1D: UDPQ %s depth=%d queued=%d dropped=%d\n",
                (queued == UDP_QUEUE_DEPTH && dropped == 0) ? "PASS" : "FAIL",
                UDP_QUEUE_DEPTH, queued, dropped);
    }

    /* ---------------------------------------------------------------- */
    /* NET-P1-E: SOCK_MAX. All 32 slots must allocate; the 33rd must be  */
    /* cleanly rejected; the (now ~1.4 MB) table kmalloc must have held. */
    /* ---------------------------------------------------------------- */
    {
        int n = 0;
        int heapok = (sock_table_base() != NULL) ? 1 : 0;
        for (int i = 0; i < SOCK_MAX; i++)
            if (sock_socket(SOCK_DGRAM) >= 0) n++;
        int extra = sock_socket(SOCK_DGRAM);
        sock_init();
        kprintf("NETP1E: SOCKMAX %s n=%d heapok=%d extra_rejected=%d\n",
                (n == SOCK_MAX && heapok && extra < 0) ? "PASS" : "FAIL",
                n, heapok, (extra < 0) ? 1 : 0);
    }

    /* ---------------------------------------------------------------- */
    /* A4 (SOCKET-PARITY-0): setsockopt/getsockopt round-trip + the      */
    /* SO_REUSEADDR relaxation of the bind() duplicate-port check.        */
    /* ---------------------------------------------------------------- */
    {
        int type_ok = 0, rcvto_ok = 0, reuse_reject = 0, reuse_ok = 0;
        int u = sock_socket(SOCK_DGRAM);
        if (u >= 0) {
            int v = -1;
            if (sock_getsockopt(u, SOL_SOCKET, SO_TYPE, &v) == SOCK_OK &&
                v == SOCK_DGRAM)
                type_ok = 1;
            if (sock_setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, 500) == SOCK_OK) {
                v = -1;
                if (sock_getsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &v) == SOCK_OK &&
                    v == 500)
                    rcvto_ok = 1;
            }
            sock_bind(u, 47007);
            int w = sock_socket(SOCK_DGRAM);
            if (w >= 0) {
                /* No SO_REUSEADDR -> duplicate bind must be REJECTED. */
                if (sock_bind(w, 47007) < 0) reuse_reject = 1;
                /* With SO_REUSEADDR -> the same bind must SUCCEED. */
                sock_setsockopt(w, SOL_SOCKET, SO_REUSEADDR, 1);
                if (sock_bind(w, 47007) == SOCK_OK) reuse_ok = 1;
            }
        }
        sock_init();
        kprintf("NETP1F: SOCKOPT %s type=%d rcvtimeo=%d reuse_reject=%d reuse_ok=%d\n",
                (type_ok && rcvto_ok && reuse_reject && reuse_ok) ? "PASS" : "FAIL",
                type_ok, rcvto_ok, reuse_reject, reuse_ok);
    }

    /* ---------------------------------------------------------------- */
    /* A4: shutdown(2) half-close. SHUT_RD makes recv return EOF (0)      */
    /* even with a datagram queued; SHUT_WR makes sendto fail.            */
    /* ---------------------------------------------------------------- */
    {
        int rd_eof = 0, wr_blocked = 0;
        int u = sock_socket(SOCK_DGRAM);
        if (u >= 0 && sock_bind(u, 47008) == 0) {
            rig_inject_udp(peer_ip, 7777, my_ip, 47008, "Z", 1);
            sock_shutdown(u, SHUT_RD);
            uint8_t buf[8];
            int r = sock_recvfrom(u, buf, sizeof(buf), NULL, NULL);
            rd_eof = (r == 0) ? 1 : 0;
            sock_shutdown(u, SHUT_WR);
            int sr = sock_sendto(u, "Q", 1, peer_ip, 7777);
            wr_blocked = (sr < 0) ? 1 : 0;
        }
        sock_init();
        kprintf("NETP1G: SHUTDOWN %s rd_eof=%d wr_blocked=%d\n",
                (rd_eof && wr_blocked) ? "PASS" : "FAIL", rd_eof, wr_blocked);
    }

    /* ---------------------------------------------------------------- */
    /* A4: loopback. A UDP datagram to 127.0.0.1 must arrive byte-exact  */
    /* in a socket bound to that port -- exercising the REAL send path    */
    /* (ip_tx 127/8 short-circuit -> ipv4_demux -> udp_input -> queue).   */
    /* ---------------------------------------------------------------- */
    {
        int loop_ok = 0;
        int u = sock_socket(SOCK_DGRAM);
        if (u >= 0 && sock_bind(u, 47009) == 0) {
            int sr = sock_sendto(u, "LOOPBK", 6, 0x7F000001u, 47009);
            uint8_t buf[16]; uint32_t sip = 0; uint16_t sp = 0;
            int n = sock_recvfrom(u, buf, sizeof(buf), &sip, &sp);
            loop_ok = (sr == 6 && n == 6 && memcmp(buf, "LOOPBK", 6) == 0 &&
                       sp == 47009) ? 1 : 0;
        }
        sock_init();
        kprintf("NETP1H: LOOPBACK %s ok=%d\n", loop_ok ? "PASS" : "FAIL", loop_ok);
    }

    /* ---------------------------------------------------------------- */
    /* TCP-ROBUST NETP1I: RX checksum verification. A good TCP segment   */
    /* is delivered byte-exact; a segment whose checksum no longer       */
    /* matches its data is DROPPED (never reaches the stream). A good    */
    /* UDP datagram is delivered; one with a corrupt IPv4 header         */
    /* checksum is dropped at ipv4_demux before udp_input.               */
    /* ---------------------------------------------------------------- */
    {
        int tcp_good = 0, tcp_drop = 0, ip_drop = 0, ip_good = 0;

        /* TCP: establish a connection, then good-then-corrupt data. */
        int fd = -1;
        int l = sock_socket(SOCK_STREAM);
        if (l >= 0 && sock_bind(l, 47010) == 0 && sock_listen(l, 2) == 0) {
            g_cap_n = 0;
            rig_inject_tcp(peer_ip, 43000, my_ip, 47010,
                           8000, 0, TCP_SYN, 4096, NULL, 0);
            if (g_cap_n >= 1) {
                const tcp_hdr_t* th = (const tcp_hdr_t*)g_cap[0].seg;
                uint32_t isn = net_ntohl(th->seq);
                rig_inject_tcp(peer_ip, 43000, my_ip, 47010,
                               8001, isn + 1, TCP_ACK, 4096, NULL, 0);
                fd = sock_accept(l);
            }
        }
        if (fd >= 0) {
            uint8_t buf[16];
            rig_inject_tcp(peer_ip, 43000, my_ip, 47010,
                           8001, 0, TCP_ACK | TCP_PSH, 4096, "GOOD", 4);
            int n1 = sock_recv(fd, buf, sizeof(buf));
            tcp_good = (n1 == 4 && memcmp(buf, "GOOD", 4) == 0) ? 1 : 0;
            g_corrupt_tcp = 1;
            rig_inject_tcp(peer_ip, 43000, my_ip, 47010,
                           8005, 0, TCP_ACK | TCP_PSH, 4096, "BAD!", 4);
            g_corrupt_tcp = 0;
            int n2 = sock_recv(fd, buf, sizeof(buf));
            tcp_drop = (n2 <= 0) ? 1 : 0;
        }

        /* IP: a corrupt IPv4 header checksum must be dropped at ipv4_demux. */
        int u = sock_socket(SOCK_DGRAM);
        if (u >= 0 && sock_bind(u, 47011) == 0) {
            uint8_t buf[16];
            g_corrupt_ip = 1;
            rig_inject_udp(peer_ip, 7777, my_ip, 47011, "IPBAD", 5);
            g_corrupt_ip = 0;
            int nb = sock_recvfrom(u, buf, sizeof(buf), NULL, NULL);
            ip_drop = (nb <= 0) ? 1 : 0;
            rig_inject_udp(peer_ip, 7777, my_ip, 47011, "IPOK", 4);
            int ng = sock_recvfrom(u, buf, sizeof(buf), NULL, NULL);
            ip_good = (ng == 4 && memcmp(buf, "IPOK", 4) == 0) ? 1 : 0;
        }

        sock_init();
        kprintf("NETP1I: RXCKSUM %s tcp_good=%d tcp_drop=%d ip_drop=%d ip_good=%d\n",
                (tcp_good && tcp_drop && ip_drop && ip_good) ? "PASS" : "FAIL",
                tcp_good, tcp_drop, ip_drop, ip_good);
    }

    g_rig_active = 0;
}

#else  /* !NET_SELFTEST */
/* Default builds compile this TU empty (no tap, no rig, no .bss). */
typedef int net_testrig_not_compiled_t;
#endif /* NET_SELFTEST */
