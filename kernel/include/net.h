#ifndef NET_H
#define NET_H

#include "types.h"

/*
 * Kernel networking API
 * =====================
 *
 * A small, self-contained networking stack built on top of QEMU's emulated
 * Intel e1000 NIC. Scope is intentionally narrow (demonstrable bring-up):
 *
 *   - e1000 PCI detect, BAR map, reset, RX/TX descriptor rings + DMA buffers.
 *   - Read the burned-in MAC address.
 *   - Ethernet II framing.
 *   - ARP (request/reply, tiny cache).
 *   - IPv4 + ICMP echo (ping) against the QEMU user-net gateway (10.0.2.2).
 *
 * Everything here lives under the networking engineer's scope:
 *   kernel/drivers/net/*  and  kernel/net/*  and this header. No edits to the
 *   shared syscall glue -- the integrator wires net_init() and the proposed
 *   SYS_NET_* syscalls (see report / kernel/net/netsyscall.c helpers below).
 */

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define ETH_ALEN          6        /* bytes in a MAC address           */
#define ETH_HLEN          14       /* Ethernet II header length        */
#define ETH_MIN_FRAME     60       /* minimum frame (sans FCS)         */
#define ETH_MAX_FRAME     1518     /* maximum frame (sans FCS)         */
#define ETH_MTU           1500     /* payload MTU                      */

/* EtherTypes (host byte order; serialized big-endian on the wire). */
#define ETH_P_IP          0x0800
#define ETH_P_ARP         0x0806

/* ARP. */
#define ARP_HTYPE_ETHER   1
#define ARP_OP_REQUEST    1
#define ARP_OP_REPLY      2

/* IPv4 protocol numbers. */
#define IPPROTO_ICMP      1
#define IPPROTO_TCP       6
#define IPPROTO_UDP       17

/* ICMP types. */
#define ICMP_ECHO_REPLY          0
#define ICMP_DEST_UNREACH        3
#define ICMP_ECHO_REQUEST        8
#define ICMP_TIME_EXCEEDED      11

/* ICMP Destination Unreachable codes. */
#define ICMP_NET_UNREACH         0
#define ICMP_HOST_UNREACH        1
#define ICMP_PROT_UNREACH        2
#define ICMP_PORT_UNREACH        3
#define ICMP_FRAG_NEEDED         4

/* ICMP Time Exceeded codes. */
#define ICMP_TTL_EXCEEDED        0
#define ICMP_FRAG_TIMEOUT        1

/* QEMU user-net (slirp) well-known addresses. */
#define NET_QEMU_GATEWAY  0x0A000202u  /* 10.0.2.2  (host/gateway)      */
#define NET_QEMU_DNS      0x0A000203u  /* 10.0.2.3  (DNS)               */
#define NET_QEMU_GUEST    0x0A00020Fu  /* 10.0.2.15 (first DHCP lease)  */

/* ------------------------------------------------------------------ */
/* Wire-format structures (all big-endian fields packed)              */
/* ------------------------------------------------------------------ */

#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;   /* big-endian */
} PACKED eth_hdr_t;

typedef struct {
    uint16_t htype;       /* hardware type (1 = Ethernet)      */
    uint16_t ptype;       /* protocol type (0x0800 = IPv4)     */
    uint8_t  hlen;        /* hardware addr len (6)             */
    uint8_t  plen;        /* protocol addr len (4)             */
    uint16_t oper;        /* 1 = request, 2 = reply            */
    uint8_t  sha[ETH_ALEN]; /* sender hardware addr            */
    uint8_t  spa[4];      /* sender protocol (IPv4) addr       */
    uint8_t  tha[ETH_ALEN]; /* target hardware addr            */
    uint8_t  tpa[4];      /* target protocol (IPv4) addr       */
} PACKED arp_pkt_t;

typedef struct {
    uint8_t  ver_ihl;     /* version (4 bits) | IHL (4 bits)   */
    uint8_t  tos;
    uint16_t total_len;   /* big-endian                        */
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;    /* header checksum                   */
    uint32_t src;         /* big-endian IPv4                   */
    uint32_t dst;         /* big-endian IPv4                   */
} PACKED ipv4_hdr_t;

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;          /* echo identifier                   */
    uint16_t seq;         /* echo sequence number              */
} PACKED icmp_hdr_t;

/* ------------------------------------------------------------------ */
/* Byte-order helpers                                                  */
/* ------------------------------------------------------------------ */

static inline uint16_t net_htons(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}
static inline uint16_t net_ntohs(uint16_t x) { return net_htons(x); }

static inline uint32_t net_htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static inline uint32_t net_ntohl(uint32_t x) { return net_htonl(x); }

/* ------------------------------------------------------------------ */
/* Low-level NIC driver API (kernel/drivers/net/e1000.c)               */
/* ------------------------------------------------------------------ */

/* Detect + bring up the e1000. Returns 0 on success, negative on error. */
int  e1000_init(void);

/* True once e1000_init() succeeded and a NIC is usable. */
bool e1000_present(void);

/* Copy the 6-byte MAC into out[6]. Returns 0 on success. */
int  e1000_get_mac(uint8_t out[ETH_ALEN]);

/* Transmit one raw Ethernet frame (already framed). Returns bytes sent. */
int  e1000_tx(const void* frame, uint16_t len);

/*
 * Poll for one received frame. Copies up to buf_len bytes into buf and
 * returns the frame length (>0), 0 if no frame is pending, negative on error.
 */
int  e1000_rx_poll(void* buf, uint16_t buf_len);

/* True if the NIC is up AND the MAC reports link (post auto-neg on PCH parts). */
bool e1000_link_up(void);

/* Matched PCI device id (e.g. 0x10EA = T410 82577LM); 0 if no NIC matched. */
uint16_t e1000_device_id(void);

/* ------------------------------------------------------------------ */
/* Stack API (kernel/net/*)                                            */
/* ------------------------------------------------------------------ */

/*
 * Bring up the whole networking stack: e1000_init() then assign the guest a
 * static IPv4 (NET_QEMU_GUEST) and arm ARP/IP. Idempotent. Returns 0 on
 * success, negative if no NIC was found. THE INTEGRATOR CALLS THIS once during
 * kernel init (after pci_init()).
 */
int  net_init(void);

/* True if net_init() succeeded. */
bool net_up(void);

/* Copy the NIC MAC. Returns 0 on success. */
int  net_get_mac(uint8_t out[ETH_ALEN]);

/* Local IPv4 address (host byte order). 0 until net_init(). */
uint32_t net_get_ip(void);

/* Update the legacy stack's IP / gateway (used by netif_sync_globals after DHCP). */
void net_set_ip(uint32_t ip);
void net_set_gateway(uint32_t gw);

/*
 * Transmit a fully-built Ethernet frame on the wire (thin wrapper over the
 * NIC). Returns bytes sent, or negative on error.
 */
int  net_send(const void* frame, uint16_t len);

/*
 * Poll the NIC for one received frame, feed it through the stack (ARP cache
 * learning, ARP replies, ICMP echo-reply capture), and copy the raw frame to
 * the caller. Returns frame length (>0), 0 if nothing pending, negative on
 * error. Call this in a loop to service the device.
 */
int  net_recv(void* buf, uint16_t buf_len);

/*
 * Send a broadcast ARP request for `target_ip` (host byte order). Returns 0 if
 * the request was put on the wire. The reply is learned the next time
 * net_recv()/net_poll() runs.
 */
int  net_arp_request(uint32_t target_ip);

/*
 * Look up `ip` (host byte order) in the ARP cache, copying the MAC to out[6].
 * Returns 0 if found, negative if not (caller should net_arp_request + poll).
 */
int  net_arp_lookup(uint32_t ip, uint8_t out[ETH_ALEN]);

/*
 * Send one ICMP echo request (ping) to `dst_ip` (host byte order) with the
 * given identifier/sequence and `payload_len` bytes of zero payload. Resolves
 * the destination MAC via ARP (must already be cached -- call net_arp_request
 * + poll first, or use the convenience net_ping()). Returns 0 on send.
 */
int  net_icmp_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                   uint16_t payload_len);

/*
 * Convenience blocking-ish ping: ARP-resolve `dst_ip` (host order) if needed,
 * send one echo request, then poll up to `timeout_polls` times for the reply.
 * Returns 0 if a matching echo reply arrived, negative on timeout/error.
 * Intended for the in-kernel self-test, not a userspace path.
 */
int  net_ping(uint32_t dst_ip, uint32_t timeout_polls);

/*
 * One-shot in-kernel self test: print MAC, ARP-resolve the gateway, ping it.
 * Safe to call from kernel init after net_init(). Prints results to serial.
 */
void net_selftest(void);

/*
 * Send ICMP error messages (Type 3 Destination Unreachable, Type 11 Time Exceeded).
 * These quote the original IP header + 8 bytes of the triggering packet.
 * orig_ip points to the original IPv4 header, orig_len is the available length.
 */
void net_icmp_dest_unreach(uint8_t code, const uint8_t* orig_ip, uint16_t orig_len);
void net_icmp_time_exceeded(uint8_t code, const uint8_t* orig_ip, uint16_t orig_len);

/* ------------------------------------------------------------------ */
/* Syscall surface (wired — see kernel/core/syscall/syscall.c)         */
/* ------------------------------------------------------------------ */
/*
 * SYS_NET_SEND  = 68   sys_net_send(buf, len)              -> bytes / -errno
 * SYS_NET_RECV  = 69   sys_net_recv(buf, len)              -> len / 0 / -errno
 * SYS_NET_INFO  = 59   sys_net_info(&uapi_net_info_t)      -> 0 / -errno
 * SYS_NET_CONFIG= 89   sys_net_config(&uapi_net_config_t)  -> 0 / -errno
 * SYS_ROUTE_TABLE=90   sys_route_table(buf, max)           -> count / -errno
 * SYS_ARP_TABLE = 91   sys_arp_table(buf, max)             -> count / -errno
 *
 * Payload structs live in uapi/net.h (canonical, _Static_assert-guarded).
 * Legacy aliases (net_info_ext_t, route_info_t, arp_info_t) in netif.h.
 */

/* Syscall handler prototypes (kernel/net/netsyscall.c). */
int64_t sys_net_send(uint64_t buf, uint64_t len, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_net_recv(uint64_t buf, uint64_t len, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_net_info(uint64_t out_info, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* NET_H */
