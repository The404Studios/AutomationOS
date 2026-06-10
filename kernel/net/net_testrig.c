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
    rig_inject(src_ip, dst_ip, IPPROTO_TCP, g_seg, tlen);
}

/* ------------------------------------------------------------------ */
/* The A0 selftest: prove INJECT (loopback) and CAPTURE (tap).         */
/* ------------------------------------------------------------------ */
void net_testrig_selftest(void) {
    uint32_t my_ip   = net_get_ip();
    const uint32_t peer_ip = 0x0A000263;   /* 10.0.2.99 -- a fake test peer */
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

    g_rig_active = 0;
}

#else  /* !NET_SELFTEST */
/* Default builds compile this TU empty (no tap, no rig, no .bss). */
typedef int net_testrig_not_compiled_t;
#endif /* NET_SELFTEST */
