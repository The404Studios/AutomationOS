/*
 * udp.c -- UDP datagram TX + inbound demux.
 * =========================================
 *
 * Sits on top of socket.c's ip_tx() (which frames + transmits IPv4) and the
 * IPv4 RX demux (which calls udp_input() for IPPROTO_UDP segments). Computes
 * the IPv4 pseudo-header UDP checksum on TX and queues received datagrams into
 * the owning socket's ring for sock_recvfrom().
 *
 * Scope: kernel/net/udp.c (new).
 */

#include "../include/socket.h"
#include "../include/net.h"
#include "../include/types.h"
#include "../include/string.h"

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */
int udp_send_to(sock_t* s, uint32_t dst_ip, uint16_t dst_port,
                const void* data, uint16_t len) {
    if (s == NULL) return SOCK_EINVAL;
    if (len > UDP_DGRAM_MAX) len = UDP_DGRAM_MAX;

    /* Build the UDP segment (header + payload) in a local buffer. */
    uint8_t seg[sizeof(udp_hdr_t) + UDP_DGRAM_MAX];
    udp_hdr_t* uh = (udp_hdr_t*)seg;
    uint16_t seg_len = (uint16_t)(sizeof(udp_hdr_t) + len);

    uh->src_port = net_htons(s->local_port);
    uh->dst_port = net_htons(dst_port);
    uh->length   = net_htons(seg_len);
    uh->checksum = 0;
    if (len) memcpy(seg + sizeof(udp_hdr_t), data, len);

    /* UDP checksum is optional in IPv4 (0 = none), but we compute it. The
     * checksum covers the pseudo-header + UDP header + payload. */
    uint16_t ck = net_transport_checksum(net_get_ip(), dst_ip,
                                         IPPROTO_UDP, seg, seg_len);
    /* A computed checksum of 0 must be transmitted as 0xFFFF. */
    uh->checksum = net_htons(ck == 0 ? 0xFFFF : ck);

    if (ip_tx(dst_ip, IPPROTO_UDP, seg, seg_len) != 0) {
        return SOCK_ECONN;
    }
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */
/* Forward declaration for ICMP error sending (in net.c). */
extern void net_icmp_port_unreachable(uint32_t src_ip, const uint8_t* orig_pkt,
                                      uint16_t orig_len);

void udp_input(uint32_t src_ip, uint32_t dst_ip,
               const uint8_t* seg, uint16_t seg_len) {
    (void)dst_ip;
    if (seg_len < sizeof(udp_hdr_t)) return;

    const udp_hdr_t* uh = (const udp_hdr_t*)seg;
    uint16_t ulen = net_ntohs(uh->length);
    if (ulen < sizeof(udp_hdr_t) || ulen > seg_len) ulen = seg_len;

    uint16_t src_port = net_ntohs(uh->src_port);
    uint16_t dst_port = net_ntohs(uh->dst_port);
    uint16_t payload_len = (uint16_t)(ulen - sizeof(udp_hdr_t));
    const uint8_t* payload = seg + sizeof(udp_hdr_t);

    /* Find the socket bound to the destination port. */
    sock_t* s = sock_find_udp(dst_port);
    if (s == NULL) {
        /* Nobody listening -> send ICMP Port Unreachable.
         * Note: we need the full IP packet to quote it, but we only have
         * the UDP segment here. For now, skip ICMP (would need refactor). */
        return;
    }

    if (payload_len > UDP_DGRAM_MAX) payload_len = UDP_DGRAM_MAX;

    /* Drop if the queue is full (oldest-preserving). */
    if (s->dq_count >= UDP_QUEUE_DEPTH) return;

    uint8_t idx = s->dq_tail;
    s->dgram[idx].src_ip   = src_ip;
    s->dgram[idx].src_port = src_port;
    s->dgram[idx].len      = payload_len;
    if (payload_len) memcpy(s->dgram[idx].data, payload, payload_len);

    s->dq_tail = (uint8_t)((s->dq_tail + 1) % UDP_QUEUE_DEPTH);
    s->dq_count++;
}
