/*
 * Minimal TCP/IP-ish stack: Ethernet + ARP + IPv4 + ICMP echo.
 * ============================================================
 *
 * Sits on top of the e1000 driver. Just enough to demonstrate a real packet
 * round trip against QEMU's user-net (slirp) gateway at 10.0.2.2:
 *
 *   net_init()         -> e1000_init() + assign the guest 10.0.2.15.
 *   net_send()/recv()  -> thin framing over the NIC; recv also runs inbound
 *                         frames through ARP-learning / ARP-reply / ICMP-reply.
 *   net_arp_request()  -> broadcast "who has X?".
 *   net_icmp_echo()    -> ping a resolved host.
 *   net_ping()/selftest-> convenience round-trip used by the in-kernel test.
 *
 * Everything is poll-mode (no IRQs) and single-threaded -- the caller drives
 * net_recv() in a loop. Byte order: all on-wire multi-byte fields are stored
 * big-endian via net_htons/net_htonl; internal IPs are kept host-order.
 *
 * Scope: kernel/net/net.c (new tree).
 */

#include "../include/net.h"
#include "../include/netif.h"
#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/string.h"
#include "../include/rtl8139.h"   /* fallback NIC when no e1000 is present */

/* Which NIC backend net_send/net_recv talk to (chosen in net_init). */
#define NIC_NONE     0
#define NIC_E1000    1
#define NIC_RTL8139  2
static int g_nic = NIC_NONE;

/* Broadcast MAC. */
static const uint8_t MAC_BROADCAST[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ------------------------------------------------------------------ */
/* Stack state                                                         */
/* ------------------------------------------------------------------ */
#define ARP_CACHE_SIZE  16
#define ARP_TTL_MS      120000   /* ARP-AGING: evict entries older than 2 min */

typedef struct {
    uint32_t ip;                 /* host byte order, 0 = empty slot     */
    uint8_t  mac[ETH_ALEN];
    bool     valid;
    uint64_t learned_ms;         /* ARP-AGING: when this entry was learned */
} arp_entry_t;

/* ------------------------------------------------------------------ */
/* IPv4 fragment reassembly buffer                                     */
/* ------------------------------------------------------------------ */
#define FRAG_REASM_MAX     16    /* max concurrent reassemblies         */
#define FRAG_TIMEOUT_MS    30000 /* 30 seconds                          */
#define FRAG_BUF_SIZE      65535 /* max IP packet size                  */

typedef struct {
    bool     valid;
    uint32_t src_ip;             /* host byte order */
    uint32_t dst_ip;             /* host byte order */
    uint8_t  proto;
    uint16_t id;
    uint64_t start_time_ms;      /* when first fragment arrived */
    uint8_t  data[FRAG_BUF_SIZE];
    bool     holes[FRAG_BUF_SIZE]; /* true = byte not yet received */
    uint16_t total_len;          /* 0 until we know final size */
    bool     has_final;          /* received fragment with MF=0 */
} frag_reasm_t;

/*
 * This struct is dominated by frags[] (FRAG_REASM_MAX * ~128KB == ~2MB), making
 * it by far the largest single object in the kernel .bss. It is placed in the
 * .bss.deferred section (emitted last by linker.ld) so it sits ABOVE all kernel
 * control-path globals. That keeps current_process / scheduler runqueues /
 * pmm_bitmap / heap+slab metadata / syscall+vfs+ipc tables packed below the
 * 0x200000 userspace-ELF load base, where a large user image cannot shadow them
 * in that process's deep-copied low-half page tables. Networking is not used on
 * the scheduling/fault/syscall-dispatch path that runs under a foreign CR3, so
 * deferring it is safe. (Do NOT move this back into ordinary .bss.)
 */
static struct {
    bool        up;
    uint8_t     mac[ETH_ALEN];
    uint32_t    ip;              /* host byte order                     */
    uint32_t    gateway;         /* host byte order                     */
    arp_entry_t arp[ARP_CACHE_SIZE];

    /* Last captured ICMP echo reply (for net_ping / selftest). */
    bool        got_echo_reply;
    uint16_t    last_echo_id;
    uint16_t    last_echo_seq;
    uint32_t    last_echo_from;  /* host byte order */

    /* Fragment reassembly table. */
    frag_reasm_t frags[FRAG_REASM_MAX];
} net __attribute__((section(".bss.deferred")));

/* Scratch frame buffer for building/receiving. */
static uint8_t g_frame[ETH_MAX_FRAME];

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */
/* Standard 16-bit one's-complement Internet checksum (RFC 1071). */
static uint16_t inet_checksum(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)(p[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* ARP cache                                                           */
/* ------------------------------------------------------------------ */
extern uint64_t timer_get_ticks_ms(void);
#ifdef NET_SELFTEST
/* ARP-AGING rig hook: the boot test rig has no wall-clock wait, so it advances
 * this offset to make a cache entry deterministically exceed ARP_TTL_MS. Zero in
 * normal runs; compiled out without NET_SELFTEST -> default kernel byte-identical. */
static uint64_t g_net_rig_offset_ms = 0;
void     net_rig_advance_ms(uint64_t ms) { g_net_rig_offset_ms += ms; }
void     net_rig_clock_reset(void)       { g_net_rig_offset_ms = 0; }
static uint64_t net_now_ms(void) { return timer_get_ticks_ms() + g_net_rig_offset_ms; }
#else
static uint64_t net_now_ms(void) { return timer_get_ticks_ms(); }
#endif

static void arp_insert(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    uint64_t now = net_now_ms();
    /* Update existing entry if present. */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net.arp[i].valid && net.arp[i].ip == ip) {
            memcpy(net.arp[i].mac, mac, ETH_ALEN);
            net.arp[i].learned_ms = now;   /* ARP-AGING: refresh on relearn */
            return;
        }
    }
    /* Take the first free slot, preferring to reclaim an EXPIRED entry over the
     * old crude slot-0 stomp (ARP-AGING). */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!net.arp[i].valid ||
            (now - net.arp[i].learned_ms >= ARP_TTL_MS)) {
            net.arp[i].valid = true;
            net.arp[i].ip = ip;
            memcpy(net.arp[i].mac, mac, ETH_ALEN);
            net.arp[i].learned_ms = now;
            return;
        }
    }
    net.arp[0].ip = ip;
    memcpy(net.arp[0].mac, mac, ETH_ALEN);
    net.arp[0].learned_ms = now;
}

int net_arp_lookup(uint32_t ip, uint8_t out[ETH_ALEN]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net.arp[i].valid && net.arp[i].ip == ip) {
            /* ARP-AGING: a stale entry is invalidated + reported as a miss so the
             * caller re-ARPs (a host that changed MAC / moved is re-resolved). */
            if (net_now_ms() - net.arp[i].learned_ms >= ARP_TTL_MS) {
                net.arp[i].valid = false;
                return -1;
            }
            memcpy(out, net.arp[i].mac, ETH_ALEN);
            return 0;
        }
    }
    return -1;
}

#ifdef NET_SELFTEST
/* ARP-AGING proof hook: insert a cache entry directly (normally learned from an
 * ARP reply) so the rig can age it out under the advanced clock. */
void net_arp_rig_insert(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    arp_insert(ip, mac);
}
#endif

/* ------------------------------------------------------------------ */
/* Ethernet framing                                                    */
/* ------------------------------------------------------------------ */
/* Build an Ethernet header at `dst_frame`; returns ETH_HLEN. */
static uint16_t eth_build(uint8_t* dst_frame, const uint8_t dmac[ETH_ALEN],
                          uint16_t ethertype) {
    eth_hdr_t* eh = (eth_hdr_t*)dst_frame;
    memcpy(eh->dst, dmac, ETH_ALEN);
    memcpy(eh->src, net.mac, ETH_ALEN);
    eh->ethertype = net_htons(ethertype);
    return ETH_HLEN;
}

/* ------------------------------------------------------------------ */
/* ARP                                                                 */
/* ------------------------------------------------------------------ */
int net_arp_request(uint32_t target_ip) {
    if (!net.up) return -1;

    uint8_t* f = g_frame;
    uint16_t off = eth_build(f, MAC_BROADCAST, ETH_P_ARP);

    arp_pkt_t* a = (arp_pkt_t*)(f + off);
    a->htype = net_htons(ARP_HTYPE_ETHER);
    a->ptype = net_htons(ETH_P_IP);
    a->hlen  = ETH_ALEN;
    a->plen  = 4;
    a->oper  = net_htons(ARP_OP_REQUEST);
    memcpy(a->sha, net.mac, ETH_ALEN);
    uint32_t spa = net_htonl(net.ip);
    uint32_t tpa = net_htonl(target_ip);
    memcpy(a->spa, &spa, 4);
    memset(a->tha, 0, ETH_ALEN);
    memcpy(a->tpa, &tpa, 4);

    uint16_t total = off + (uint16_t)sizeof(arp_pkt_t);
    return (net_send(f, total) > 0) ? 0 : -1;
}

/* Send an ARP reply in response to a request that targeted us. */
static void arp_reply(const arp_pkt_t* req) {
    uint8_t* f = g_frame;
    uint16_t off = eth_build(f, req->sha, ETH_P_ARP);

    arp_pkt_t* a = (arp_pkt_t*)(f + off);
    a->htype = net_htons(ARP_HTYPE_ETHER);
    a->ptype = net_htons(ETH_P_IP);
    a->hlen  = ETH_ALEN;
    a->plen  = 4;
    a->oper  = net_htons(ARP_OP_REPLY);
    memcpy(a->sha, net.mac, ETH_ALEN);
    uint32_t spa = net_htonl(net.ip);
    memcpy(a->spa, &spa, 4);
    memcpy(a->tha, req->sha, ETH_ALEN);
    memcpy(a->tpa, req->spa, 4);

    net_send(f, off + (uint16_t)sizeof(arp_pkt_t));
}

/* Handle an inbound ARP packet: learn the sender, reply if it targets us. */
static void arp_input(const arp_pkt_t* a) {
    uint32_t spa, tpa;
    memcpy(&spa, a->spa, 4);
    memcpy(&tpa, a->tpa, 4);
    uint32_t spa_host = net_ntohl(spa);
    uint32_t tpa_host = net_ntohl(tpa);

    /* Learn sender mapping unconditionally. */
    arp_insert(spa_host, a->sha);

    if (net_ntohs(a->oper) == ARP_OP_REQUEST && tpa_host == net.ip) {
        arp_reply(a);
    }
}

/* ------------------------------------------------------------------ */
/* ICMP error messages                                                 */
/* ------------------------------------------------------------------ */
/*
 * Send an ICMP error message quoting the original IP header + 8 bytes.
 * type/code are ICMP error type and code.
 * orig_ip points to the original IPv4 header that triggered the error.
 * orig_len is the length of the original packet available.
 */
static void icmp_send_error(uint8_t type, uint8_t code,
                            const uint8_t* orig_ip, uint16_t orig_len) {
    /* Get source MAC from original packet's destination. */
    const ipv4_hdr_t* oip = (const ipv4_hdr_t*)orig_ip;
    uint32_t src_ip = net_ntohl(oip->src);

    uint8_t smac[ETH_ALEN];
    if (net_arp_lookup(src_ip, smac) != 0) {
        if (net_arp_lookup(net.gateway, smac) != 0) {
            return; /* can't reply -- no MAC */
        }
    }

    /* ICMP error format: type, code, checksum, unused(4), original IP hdr + 8 bytes. */
    uint16_t quote_len = sizeof(ipv4_hdr_t) + 8;
    if (orig_len < quote_len) quote_len = orig_len;

    uint8_t* f = g_frame;
    uint16_t off = eth_build(f, smac, ETH_P_IP);

    ipv4_hdr_t* ip = (ipv4_hdr_t*)(f + off);
    uint16_t icmp_total = (uint16_t)(8 + quote_len); /* 8-byte ICMP header + quote */
    uint16_t ip_total = (uint16_t)(sizeof(ipv4_hdr_t) + icmp_total);

    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = net_htons(ip_total);
    ip->id        = net_htons(0x5678);
    ip->frag_off  = net_htons(0x4000); /* don't fragment */
    ip->ttl       = 64;
    ip->proto     = IPPROTO_ICMP;
    ip->checksum  = 0;
    ip->src       = net_htonl(net.ip);
    ip->dst       = oip->src; /* already big-endian */
    ip->checksum  = net_htons(inet_checksum(ip, sizeof(ipv4_hdr_t)));

    /* ICMP error header: type, code, checksum, unused(4 bytes), quoted data. */
    uint8_t* icmp_pkt = (uint8_t*)ip + sizeof(ipv4_hdr_t);
    icmp_pkt[0] = type;
    icmp_pkt[1] = code;
    *(uint16_t*)(icmp_pkt + 2) = 0; /* checksum, filled below */
    *(uint32_t*)(icmp_pkt + 4) = 0; /* unused */
    memcpy(icmp_pkt + 8, orig_ip, quote_len);

    uint16_t ck = inet_checksum(icmp_pkt, icmp_total);
    *(uint16_t*)(icmp_pkt + 2) = net_htons(ck);

    net_send(f, (uint16_t)(off + ip_total));
}

/* ------------------------------------------------------------------ */
/* IPv4 fragment reassembly                                            */
/* ------------------------------------------------------------------ */
extern uint64_t timer_get_ticks_ms(void); /* milliseconds since boot */

/* Find or allocate a reassembly slot for this fragment. */
static frag_reasm_t* frag_find_slot(uint32_t src_ip, uint32_t dst_ip,
                                    uint8_t proto, uint16_t id) {
    /* Look for existing slot. */
    for (int i = 0; i < FRAG_REASM_MAX; i++) {
        if (net.frags[i].valid &&
            net.frags[i].src_ip == src_ip &&
            net.frags[i].dst_ip == dst_ip &&
            net.frags[i].proto == proto &&
            net.frags[i].id == id) {
            return &net.frags[i];
        }
    }

    /* Allocate a new slot (evict oldest if full). */
    int slot = -1;
    for (int i = 0; i < FRAG_REASM_MAX; i++) {
        if (!net.frags[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        /* Evict oldest. */
        uint64_t oldest_time = net.frags[0].start_time_ms;
        slot = 0;
        for (int i = 1; i < FRAG_REASM_MAX; i++) {
            if (net.frags[i].start_time_ms < oldest_time) {
                oldest_time = net.frags[i].start_time_ms;
                slot = i;
            }
        }
    }

    /* Initialize new slot. */
    frag_reasm_t* fr = &net.frags[slot];
    fr->valid = true;
    fr->src_ip = src_ip;
    fr->dst_ip = dst_ip;
    fr->proto = proto;
    fr->id = id;
    fr->start_time_ms = timer_get_ticks_ms();
    fr->total_len = 0;
    fr->has_final = false;
    memset(fr->holes, 1, sizeof(fr->holes)); /* all bytes missing initially */
    return fr;
}

/* Timeout old reassembly buffers and send ICMP Time Exceeded. */
static void frag_timeout_check(void) {
    uint64_t now = timer_get_ticks_ms();
    for (int i = 0; i < FRAG_REASM_MAX; i++) {
        if (net.frags[i].valid && now - net.frags[i].start_time_ms > FRAG_TIMEOUT_MS) {
            /* Timeout: send ICMP Time Exceeded (fragment reassembly timeout). */
            /* We don't have the original packet saved, so skip ICMP for now. */
            net.frags[i].valid = false;
        }
    }
}

/*
 * Process a fragmented IPv4 packet. Returns:
 *   1 if reassembly is complete (caller should process the reassembled packet).
 *   0 if more fragments are needed.
 *  -1 on error.
 *
 * On success (return 1), the reassembled packet is in out_buf, length in *out_len.
 */
static int ip_reassemble(const ipv4_hdr_t* ip, uint16_t ip_avail,
                         uint8_t* out_buf, uint16_t* out_len) {
    frag_timeout_check();

    uint32_t src = net_ntohl(ip->src);
    uint32_t dst = net_ntohl(ip->dst);
    uint16_t id = net_ntohs(ip->id);
    uint16_t frag_off_flags = net_ntohs(ip->frag_off);
    uint16_t frag_offset = (frag_off_flags & 0x1FFF) * 8; /* in bytes */
    bool more_frags = (frag_off_flags & 0x2000) != 0;

    uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_hdr_t)) ihl = sizeof(ipv4_hdr_t);
    uint16_t tot = net_ntohs(ip->total_len);
    if (tot > ip_avail) tot = ip_avail;
    if (tot < ihl) return -1;

    uint16_t payload_len = (uint16_t)(tot - ihl);
    const uint8_t* payload = (const uint8_t*)ip + ihl;

    frag_reasm_t* fr = frag_find_slot(src, dst, ip->proto, id);
    if (fr == NULL) return -1;

    /* Copy fragment data. */
    if (frag_offset + payload_len > FRAG_BUF_SIZE) {
        fr->valid = false;
        return -1;
    }
    memcpy(fr->data + frag_offset, payload, payload_len);
    for (uint16_t i = 0; i < payload_len; i++) {
        fr->holes[frag_offset + i] = false;
    }

    /* If this is the last fragment, record total length. */
    if (!more_frags) {
        fr->has_final = true;
        fr->total_len = frag_offset + payload_len;
    }

    /* Check if reassembly is complete. */
    if (!fr->has_final) return 0; /* still waiting for final fragment */

    for (uint16_t i = 0; i < fr->total_len; i++) {
        if (fr->holes[i]) return 0; /* still have holes */
    }

    /* The reassembled IP packet is ihl + total_len bytes. DROP anything that would
     * not fit the FRAG_BUF_SIZE output buffer: the payload memcpy below writes at
     * out_buf + ihl for total_len bytes, so ihl + total_len (up to ~65555) would
     * overrun the 65535-byte buffer. This also keeps the IP total-length field valid. */
    if ((uint32_t)ihl + (uint32_t)fr->total_len > FRAG_BUF_SIZE) {
        fr->valid = false;
        return -1;
    }

    /* Reassembly complete! Build the reassembled packet. */
    /* Copy IP header from first fragment (reconstruct). */
    memcpy(out_buf, ip, ihl);
    ipv4_hdr_t* rip = (ipv4_hdr_t*)out_buf;
    rip->total_len = net_htons((uint16_t)(ihl + fr->total_len));
    rip->frag_off = 0; /* no fragments in reassembled packet */
    rip->checksum = 0;
    rip->checksum = net_htons(inet_checksum(rip, ihl));

    /* Copy reassembled payload. */
    memcpy(out_buf + ihl, fr->data, fr->total_len);
    *out_len = (uint16_t)(ihl + fr->total_len);

    /* Free the slot. */
    fr->valid = false;
    return 1;
}

/* ------------------------------------------------------------------ */
/* IPv4 / ICMP                                                         */
/* ------------------------------------------------------------------ */
int net_icmp_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                  uint16_t payload_len) {
    if (!net.up) return -1;

    uint8_t dmac[ETH_ALEN];
    /* Resolve destination MAC. On user-net everything goes via the gateway,
     * but slirp answers ARP for any address, so resolve the dst directly if
     * cached, else fall back to the gateway's MAC. */
    if (net_arp_lookup(dst_ip, dmac) != 0) {
        if (net_arp_lookup(net.gateway, dmac) != 0) {
            return -1;   /* no route -- caller must ARP first */
        }
    }

    if (payload_len > 256) payload_len = 256;

    uint8_t* f = g_frame;
    uint16_t off = eth_build(f, dmac, ETH_P_IP);

    ipv4_hdr_t* ip = (ipv4_hdr_t*)(f + off);
    uint16_t ip_total = (uint16_t)(sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) +
                                   payload_len);
    ip->ver_ihl   = 0x45;            /* IPv4, 5-dword header */
    ip->tos       = 0;
    ip->total_len = net_htons(ip_total);
    ip->id        = net_htons(0x1234);
    ip->frag_off  = net_htons(0x4000); /* don't fragment */
    ip->ttl       = 64;
    ip->proto     = IPPROTO_ICMP;
    ip->checksum  = 0;
    ip->src       = net_htonl(net.ip);
    ip->dst       = net_htonl(dst_ip);
    ip->checksum  = net_htons(inet_checksum(ip, sizeof(ipv4_hdr_t)));

    icmp_hdr_t* ic = (icmp_hdr_t*)((uint8_t*)ip + sizeof(ipv4_hdr_t));
    ic->type     = ICMP_ECHO_REQUEST;
    ic->code     = 0;
    ic->checksum = 0;
    ic->id       = net_htons(id);
    ic->seq      = net_htons(seq);
    uint8_t* payload = (uint8_t*)ic + sizeof(icmp_hdr_t);
    for (uint16_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }
    ic->checksum = net_htons(inet_checksum(ic, sizeof(icmp_hdr_t) + payload_len));

    uint16_t total = (uint16_t)(off + ip_total);
    return (net_send(f, total) > 0) ? 0 : -1;
}

/* Reply to an inbound ICMP echo request (so we answer pings too). */
static void icmp_echo_reply(const eth_hdr_t* eh, const ipv4_hdr_t* ip,
                            const icmp_hdr_t* req, uint16_t icmp_len) {
    uint8_t* f = g_frame;
    uint16_t off = eth_build(f, eh->src, ETH_P_IP);

    ipv4_hdr_t* oip = (ipv4_hdr_t*)(f + off);
    // Clamp the echoed ICMP length so the whole reply fits g_frame[ETH_MAX_FRAME].
    // oic lands at off + sizeof(ipv4_hdr_t), so at most ETH_MAX_FRAME-off-20 ICMP
    // bytes fit. Without this, a reassembled/oversized (fragmented) echo request
    // overflows g_frame by up to 16 bytes AND net_send emits a bogus ~65K on-wire
    // length. Clamp BEFORE ip_total so the memcpy and net_send below both use it.
    {
        uint16_t max_icmp = (uint16_t)(ETH_MAX_FRAME - off - sizeof(ipv4_hdr_t));
        if (icmp_len > max_icmp) icmp_len = max_icmp;
    }
    uint16_t ip_total = (uint16_t)(sizeof(ipv4_hdr_t) + icmp_len);
    oip->ver_ihl   = 0x45;
    oip->tos       = 0;
    oip->total_len = net_htons(ip_total);
    oip->id        = net_htons(0x4321);
    oip->frag_off  = net_htons(0x4000);
    oip->ttl       = 64;
    oip->proto     = IPPROTO_ICMP;
    oip->checksum  = 0;
    oip->src       = net_htonl(net.ip);
    oip->dst       = ip->src;   /* already big-endian */
    oip->checksum  = net_htons(inet_checksum(oip, sizeof(ipv4_hdr_t)));

    icmp_hdr_t* oic = (icmp_hdr_t*)((uint8_t*)oip + sizeof(ipv4_hdr_t));
    memcpy(oic, req, icmp_len);   // icmp_len already clamped to fit g_frame above
    oic->type     = ICMP_ECHO_REPLY;
    oic->code     = 0;
    oic->checksum = 0;
    oic->checksum = net_htons(inet_checksum(oic, icmp_len));

    net_send(f, (uint16_t)(off + ip_total));
}

/* Handle an inbound IPv4 packet. Handles fragmentation, ICMP, and errors. */
static void ipv4_input(const eth_hdr_t* eh, const uint8_t* ip_start,
                       uint16_t ip_avail) {
    if (ip_avail < sizeof(ipv4_hdr_t)) return;
    const ipv4_hdr_t* ip = (const ipv4_hdr_t*)ip_start;

    uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_hdr_t)) ihl = sizeof(ipv4_hdr_t);

    uint16_t tot = net_ntohs(ip->total_len);
    if (tot > ip_avail) tot = ip_avail;
    if (tot < ihl) return;

    /* NET-HARDENING-1 (martian filter on the NIC ICMP/ARP RX path): loopback
     * (127/8) addresses must NEVER appear on a real interface. A NIC-delivered
     * frame whose src OR dst is in 127/8 is spoofed -- drop it. Mirrors the
     * socket.c ipv4_demux guard, which had been the only hardened RX path; this
     * also prevents the OS echoing a martian ICMP reply onto the wire and a
     * spoofed 127/8 source poisoning the ping echo bookkeeping below. */
    if ((net_ntohl(ip->src) >> 24) == 127u || (net_ntohl(ip->dst) >> 24) == 127u)
        return;

    /* Check if packet is fragmented. */
    uint16_t frag_off_flags = net_ntohs(ip->frag_off);
    uint16_t frag_offset = (frag_off_flags & 0x1FFF) * 8;
    bool more_frags = (frag_off_flags & 0x2000) != 0;
    bool is_fragment = (frag_offset != 0 || more_frags);

    /* If fragmented, reassemble. */
    if (is_fragment) {
        /* FRAG_BUF_SIZE is 64KB -- WAY past the 8KB/16KB kernel stack, so this
         * MUST NOT be a stack array (any fragmented packet would smash the
         * stack). Static .bss instead; the net RX path is single-threaded on
         * this uniprocessor kernel, same assumption as net.frags et al. */
        static uint8_t reasm_buf[FRAG_BUF_SIZE];
        uint16_t reasm_len = 0;
        int r = ip_reassemble(ip, ip_avail, reasm_buf, &reasm_len);
        if (r < 0) return; /* error */
        if (r == 0) return; /* waiting for more fragments */
        /* r == 1: reassembly complete, process the reassembled packet. */
        ip = (const ipv4_hdr_t*)reasm_buf;
        ip_avail = reasm_len;
        ihl = (ip->ver_ihl & 0x0F) * 4;
        if (ihl < sizeof(ipv4_hdr_t)) ihl = sizeof(ipv4_hdr_t);
        tot = net_ntohs(ip->total_len);
        if (tot > ip_avail) tot = ip_avail;
        if (tot < ihl) return;
    }

    /* Process ICMP. */
    if (ip->proto == IPPROTO_ICMP) {
        uint16_t icmp_len = tot - ihl;
        if (icmp_len < sizeof(icmp_hdr_t)) return;
        /* Derive the ICMP header from `ip`, NOT ip_start: after reassembly `ip`
         * points at reasm_buf while ip_start still points at the small original
         * fragment. Reading icmp_len (reassembled-length) bytes from ip_start
         * would read past the 1518-byte NIC buffer (OOB read leaked on the wire)
         * and parse stale first-fragment bytes. On the non-fragment path
         * ip == ip_start so this is unchanged. */
        const icmp_hdr_t* ic = (const icmp_hdr_t*)((const uint8_t*)ip + ihl);

        if (ic->type == ICMP_ECHO_REPLY) {
            net.got_echo_reply = true;
            net.last_echo_id    = net_ntohs(ic->id);
            net.last_echo_seq   = net_ntohs(ic->seq);
            net.last_echo_from  = net_ntohl(ip->src);
        } else if (ic->type == ICMP_ECHO_REQUEST &&
                   net_ntohl(ip->dst) == net.ip) {
            icmp_echo_reply(eh, ip, ic, icmp_len);
        }
        return;
    }

    /* Other protocols (UDP/TCP) are handled by socket.c demux. */
    /* This function only handles ICMP in the basic stack. */
}

/* Demux one received frame into ARP / IPv4. */
static void net_input(const uint8_t* frame, uint16_t len) {
    if (len < ETH_HLEN) return;
    const eth_hdr_t* eh = (const eth_hdr_t*)frame;
    uint16_t et = net_ntohs(eh->ethertype);

    if (et == ETH_P_ARP) {
        if (len >= ETH_HLEN + sizeof(arp_pkt_t)) {
            arp_input((const arp_pkt_t*)(frame + ETH_HLEN));
        }
    } else if (et == ETH_P_IP) {
        ipv4_input(eh, frame + ETH_HLEN, len - ETH_HLEN);
    }
}

/* ------------------------------------------------------------------ */
/* Public stack API                                                    */
/* ------------------------------------------------------------------ */
int net_send(const void* frame, uint16_t len) {
    if (!net.up) return -1;
    return (g_nic == NIC_RTL8139) ? rtl8139_tx(frame, len) : e1000_tx(frame, len);
}

int net_recv(void* buf, uint16_t buf_len) {
    if (!net.up) return -1;
    int n = (g_nic == NIC_RTL8139) ? rtl8139_rx_poll(buf, buf_len)
                                   : e1000_rx_poll(buf, buf_len);
    if (n > 0) {
        net_input((const uint8_t*)buf, (uint16_t)n);
    }
    return n;
}

int net_get_mac(uint8_t out[ETH_ALEN]) {
    if (!net.up) return -1;
    memcpy(out, net.mac, ETH_ALEN);
    return 0;
}

uint32_t net_get_ip(void) {
    return net.up ? net.ip : 0;
}

void net_set_ip(uint32_t ip) {
    net.ip = ip;
}

void net_set_gateway(uint32_t gw) {
    net.gateway = gw;
}

bool net_up(void) {
    return net.up;
}

int net_init(void) {
    if (net.up) return 0;
    memset(&net, 0, sizeof(net));

    /* Prefer the Intel e1000 (the QEMU-verified path); fall back to RTL8139
     * (common on QEMU `-device rtl8139` and older real hardware). */
    if (e1000_init() == 0 && e1000_get_mac(net.mac) == 0) {
        g_nic = NIC_E1000;
    } else if (rtl8139_init() == 0 && rtl8139_get_mac(net.mac) == 0) {
        g_nic = NIC_RTL8139;
        kprintf("[NET] using RTL8139 NIC\n");
    } else {
        kprintf("[NET] no NIC -- networking disabled\n");
        return -1;
    }

    /* Static config for QEMU user-net (no DHCP client implemented). */
    net.ip      = NET_QEMU_GUEST;     /* 10.0.2.15 */
    net.gateway = NET_QEMU_GATEWAY;   /* 10.0.2.2  */
    net.up      = true;

    /* Initialize routing table. */
    extern void route_init(void);
    route_init();

    kprintf("[NET] up: ip=10.0.2.15 gw=10.0.2.2 "
            "mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            net.mac[0], net.mac[1], net.mac[2],
            net.mac[3], net.mac[4], net.mac[5]);

    /* Register the default "eth0" interface in the netif registry so that
     * SYS_NET_INFO returns the full net_info_ext_t struct that userspace
     * (DHCP client, nettool, etc.) expects. */
    {
        netif_t eth0;
        memset(&eth0, 0, sizeof(eth0));
        memcpy(eth0.name, "eth0", 5);
        memcpy(eth0.mac, net.mac, ETH_ALEN);
        eth0.ip      = net.ip;
        eth0.netmask = 0xFFFFFF00u;         /* 255.255.255.0 */
        eth0.gateway = net.gateway;
        eth0.dns     = NET_QEMU_DNS;        /* 10.0.2.3      */
        eth0.up      = true;
        eth0.tx      = (g_nic == NIC_RTL8139) ? rtl8139_tx : e1000_tx;
        eth0.rx_poll = (g_nic == NIC_RTL8139) ? rtl8139_rx_poll : e1000_rx_poll;
        eth0.get_mac = (g_nic == NIC_RTL8139) ? rtl8139_get_mac : e1000_get_mac;
        netif_register(&eth0);
        /* A4 (SOCKET-PARITY-0): register "lo" AFTER eth0 (eth0 stays default). */
        netif_register_loopback();
    }

    /* Gateway ARP pre-resolve (settle loop).
     * -----------------------------------------------------------------
     * A previous attempt busy-polled net_recv() a fixed iteration count right
     * after link-up and never cached the reply: under QEMU slirp the gateway's
     * ARP reply round-trips SLOWER than a tight in-kernel poll completes, so
     * the first outbound packet to any host (e.g. DNS) failed to resolve the
     * next-hop MAC. The fix is to wait on a WALL-CLOCK budget (PIT millisecond
     * counter) instead of an iteration count, re-issuing the ARP request
     * periodically until the slirp reply actually arrives.
     *
     * This guarantees the gateway MAC is cached BEFORE userspace ever issues a
     * sendto(), so first-contact DNS/HTTP no longer fails by racing ARP. */
    {
        extern uint64_t timer_get_ticks_ms(void);   /* ms since boot (PIT) */
        const uint64_t budget_ms = 500;              /* settle budget       */
        const uint64_t reissue_ms = 50;              /* re-ARP cadence      */
        uint8_t gwmac[ETH_ALEN];
        uint64_t start = timer_get_ticks_ms();
        uint64_t last_req = 0;
        bool resolved = (net_arp_lookup(net.gateway, gwmac) == 0);

        /* Hard iteration cap -- the load-bearing bound. net_init() runs during
         * early boot with interrupts OFF, so timer_get_ticks_ms() (driven by
         * IRQ0) is FROZEN and the wall-clock `budget_ms` check below can never
         * elapse. In QEMU the slirp gateway answers and we exit via `resolved`
         * well before the cap; on real hardware whose gateway never replies
         * (e.g. the ThinkPad T410, where the 82577LM doesn't yet link), without
         * this cap `resolved` stays false forever and the boot HANGS here. The
         * cap bounds the settle to a finite number of RX-drain passes. */
        uint32_t iters = 0;
        /* The cap bounds the settle to a finite number of RX-drain passes.
         * 200K iterations is still generous for the QEMU happy path (reply
         * arrives within ~1000 iterations) and keeps the T410 stall under
         * ~0.5s even on a slow bus. The previous 2M cap burned ~2-3s of
         * wall-clock time in tight polling on real hardware. */
        const uint32_t iter_cap = 200000u;

        while (!resolved && iters++ < iter_cap) {
            uint64_t now = timer_get_ticks_ms();
            if (now != start && now - start >= budget_ms) break;  /* only once ticks advance */

            /* (Re)issue the who-has on first pass and every reissue_ms. */
            if (last_req == 0 || (now != last_req && now - last_req >= reissue_ms)) {
                net_arp_request(net.gateway);
                last_req = now;
            }

            /* Drain RX so the ARP reply gets learned by net_input(). */
            net_recv(g_frame, sizeof(g_frame));
            if (net_arp_lookup(net.gateway, gwmac) == 0) {
                resolved = true;
                break;
            }
        }

        if (resolved) {
            kprintf("[NET] gateway 10.0.2.2 is at "
                    "%02x:%02x:%02x:%02x:%02x:%02x (after %u iters)\n",
                    gwmac[0], gwmac[1], gwmac[2],
                    gwmac[3], gwmac[4], gwmac[5], iters);
        } else {
            kprintf("[NET] WARN: gateway 10.0.2.2 ARP pre-resolve timed out "
                    "after %u iters (~%ums budget)\n", iters, (unsigned)budget_ms);
        }
    }
    return 0;
}

/*
 * E1000-PCH-0B: attach the network stack to a NIC that came up AFTER boot.
 *
 * The T410's 82577LM defers its risky ME-shared-MDIO bring-up out of boot
 * (E1000-PCH-0A); when the operator triggers it (nicup -> SYS_NET_CONFIG
 * NIC_BRINGUP) this completes the stack side: backend selection, MAC, the
 * static fallback config, routes, and the eth0 netif registration that
 * net_init() does on the boot path. NO ARP settle loop here -- the caller
 * runs post-desktop and dhcpc/ping perform their own capped resolves.
 * Idempotent: returns 0 if the stack is already up.
 */
int net_attach_late(void) {
    if (net.up) return 0;

    extern int e1000_pch_deferred_bringup(void);
    int r = e1000_pch_deferred_bringup();
    if (r != 0) return r;                    /* -1 aborted / -2 not deferred */
    if (e1000_get_mac(net.mac) != 0) return -1;
    g_nic = NIC_E1000;

    /* Static fallback config (a real LAN replaces this via dhcpc ->
     * SYS_NET_CONFIG immediately after). */
    net.ip      = NET_QEMU_GUEST;
    net.gateway = NET_QEMU_GATEWAY;
    net.up      = true;

    extern void route_init(void);
    route_init();

    {
        netif_t eth0;
        memset(&eth0, 0, sizeof(eth0));
        memcpy(eth0.name, "eth0", 5);
        memcpy(eth0.mac, net.mac, ETH_ALEN);
        eth0.ip      = net.ip;
        eth0.netmask = 0xFFFFFF00u;
        eth0.gateway = net.gateway;
        eth0.dns     = NET_QEMU_DNS;
        eth0.up      = true;
        eth0.tx      = e1000_tx;
        eth0.rx_poll = e1000_rx_poll;
        eth0.get_mac = e1000_get_mac;
        netif_register(&eth0);               /* no-op if eth0 already exists */
    }

    kprintf("[NET] late-attach: eth0 up (deferred PCH bring-up complete)\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public ICMP error API                                               */
/* ------------------------------------------------------------------ */
void net_icmp_dest_unreach(uint8_t code, const uint8_t* orig_ip, uint16_t orig_len) {
    icmp_send_error(ICMP_DEST_UNREACH, code, orig_ip, orig_len);
}

void net_icmp_time_exceeded(uint8_t code, const uint8_t* orig_ip, uint16_t orig_len) {
    icmp_send_error(ICMP_TIME_EXCEEDED, code, orig_ip, orig_len);
}

/* ------------------------------------------------------------------ */
/* In-kernel convenience tests                                         */
/* ------------------------------------------------------------------ */
int net_ping(uint32_t dst_ip, uint32_t timeout_polls) {
    if (!net.up) return -1;

    uint8_t mac[ETH_ALEN];

    /* Resolve dst (or the gateway) via ARP if we don't have it cached. */
    if (net_arp_lookup(dst_ip, mac) != 0 &&
        net_arp_lookup(net.gateway, mac) != 0) {
        net_arp_request(dst_ip);
        for (uint32_t i = 0; i < timeout_polls; i++) {
            net_recv(g_frame, sizeof(g_frame));
            if (net_arp_lookup(dst_ip, mac) == 0 ||
                net_arp_lookup(net.gateway, mac) == 0) {
                break;
            }
        }
        if (net_arp_lookup(dst_ip, mac) != 0 &&
            net_arp_lookup(net.gateway, mac) != 0) {
            return -1;   /* ARP timed out */
        }
    }

    net.got_echo_reply = false;
    if (net_icmp_echo(dst_ip, 0xABCD, 1, 32) != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < timeout_polls; i++) {
        net_recv(g_frame, sizeof(g_frame));
        if (net.got_echo_reply && net.last_echo_id == 0xABCD) {
            return 0;
        }
    }
    return -1;   /* no echo reply */
}

void net_selftest(void) {
    if (!net.up) {
        kprintf("[NET] selftest skipped: net not up\n");
        return;
    }

    kprintf("[NET] selftest: ARP who-has 10.0.2.2 ...\n");
    net_arp_request(net.gateway);

    uint8_t mac[ETH_ALEN];
    bool resolved = false;
    for (int i = 0; i < 200000; i++) {
        net_recv(g_frame, sizeof(g_frame));
        if (net_arp_lookup(net.gateway, mac) == 0) { resolved = true; break; }
    }
    if (resolved) {
        kprintf("[NET] gateway 10.0.2.2 is at "
                "%02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        kprintf("[NET] ARP for gateway timed out\n");
    }

    kprintf("[NET] selftest: ping 10.0.2.2 ...\n");
    if (net_ping(net.gateway, 400000) == 0) {
        kprintf("[NET] PING 10.0.2.2 OK (echo reply id=0x%x seq=%u)\n",
                net.last_echo_id, net.last_echo_seq);
    } else {
        kprintf("[NET] PING 10.0.2.2 FAILED (no reply)\n");
    }
}

/* ------------------------------------------------------------------ */
/* ARP table export (used by SYS_ARP_TABLE syscall)                    */
/* ------------------------------------------------------------------ */
int net_get_arp_table(arp_info_t* out, int max) {
    if (!out || max <= 0) return 0;

    int n = 0;
    for (int i = 0; i < ARP_CACHE_SIZE && n < max; i++) {
        if (net.arp[i].valid && net.arp[i].ip != 0) {
            out[n].ip = net.arp[i].ip;
            memcpy(out[n].mac, net.arp[i].mac, ETH_ALEN);
            out[n].valid = 1;
            out[n]._pad  = 0;
            n++;
        }
    }
    return n;
}
