/*
 * dhcp.c -- Freestanding DHCP client (userspace, ring 3) for AutomationOS.
 * =======================================================================
 *
 * Implements dhcp_acquire() and the offline dhcp_selftest() declared in
 * dhcp.h.  See that header for the transport rationale (no SYS_BIND -> use a
 * broadcast SENDTO with the BOOTP broadcast flag set and let the kernel UDP
 * demux deliver the reply to our socket) and the integrator note about
 * applying the lease being out of scope.
 *
 * No libc: all byte handling uses the small static helpers below and every
 * scratch buffer is a fixed-size local array.  Every receive wait is a bounded
 * poll/yield loop so an absent or hostile DHCP server can never hang the OS.
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dhcp.c -o dhcp.o
 *   objdump -d dhcp.o | grep fs:0x28   # MUST be empty (no stack canary)
 */

#include "dhcp.h"

/* ---- fixed-width types (freestanding) ----------------------------------- */
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

/* ---- syscall numbers (from kernel/include/syscall.h) -------------------- */
#define SYS_YIELD       15
#define SYS_RANDOM      43   /* CSPRNG bytes: sc(43, buf, len, 0,0,0)        */
#define SYS_SOCKET      51   /* sc(51, type, 0,0,0,0) -> fd                   */
#define SYS_CLOSE_SK    55   /* sc(55, fd, 0,0,0,0)                           */
#define SYS_SENDTO      56   /* sc(56, fd, buf, len, ip_host, port)           */
#define SYS_RECVFROM    57   /* sc(57, fd, buf, len, &sock_addr|0, 0)         */
#define SYS_SOCK_POLL   58   /* sc(58, 0,0,0,0,0) -- pump NIC/timers          */
#define SYS_NET_INFO    59   /* sc(59, &net_info, 0,0,0,0)                     */
/* Raw-frame fallback path (only used when DHCP_USE_RAW_FALLBACK is defined). */
#define SYS_NET_SEND    68
#define SYS_NET_RECV    69

/* ---- socket type -------------------------------------------------------- */
#define SOCK_DGRAM      2

/* ---- DHCP/BOOTP wire constants ------------------------------------------ */
#define DHCP_CLIENT_PORT   68
#define DHCP_SERVER_PORT   67
#define IP_BROADCAST       0xFFFFFFFFu   /* 255.255.255.255 (host order)      */

#define BOOTP_OP_REQUEST   1
#define BOOTP_OP_REPLY     2
#define BOOTP_HTYPE_ETHER  1
#define BOOTP_HLEN_ETHER   6
#define BOOTP_FLAG_BCAST   0x8000u       /* ask server to broadcast its reply */
#define DHCP_MAGIC         0x63825363u   /* magic cookie, big-endian on wire  */

/* DHCP message types (option 53). */
#define DHCPDISCOVER  1
#define DHCPOFFER     2
#define DHCPREQUEST   3
#define DHCPDECLINE   4
#define DHCPACK       5
#define DHCPNAK       6
#define DHCPRELEASE   7

/* DHCP option codes we care about. */
#define OPT_PAD            0
#define OPT_SUBNET_MASK    1
#define OPT_ROUTER         3
#define OPT_DNS            6
#define OPT_REQUESTED_IP   50
#define OPT_LEASE_TIME     51
#define OPT_MSG_TYPE       53
#define OPT_SERVER_ID      54
#define OPT_PARAM_LIST     55
#define OPT_END            255

/*
 * BOOTP fixed header layout (236 bytes) then the 4-byte magic cookie then a
 * variable options area.  Offsets used directly when building/parsing:
 *
 *   0  op(1) htype(1) hlen(1) hops(1)
 *   4  xid(4)
 *   8  secs(2) flags(2)
 *  12  ciaddr(4)
 *  16  yiaddr(4)   <- the IP the server is offering us
 *  20  siaddr(4)
 *  24  giaddr(4)
 *  28  chaddr(16)  <- client hardware addr (our MAC in first 6 bytes)
 *  44  sname(64)
 * 108  file(128)
 * 236  magic cookie(4)
 * 240  options...
 */
#define BOOTP_OFF_OP      0
#define BOOTP_OFF_HTYPE   1
#define BOOTP_OFF_HLEN    2
#define BOOTP_OFF_HOPS    3
#define BOOTP_OFF_XID     4
#define BOOTP_OFF_SECS    8
#define BOOTP_OFF_FLAGS   10
#define BOOTP_OFF_CIADDR  12
#define BOOTP_OFF_YIADDR  16
#define BOOTP_OFF_SIADDR  20
#define BOOTP_OFF_GIADDR  24
#define BOOTP_OFF_CHADDR  28
#define BOOTP_OFF_SNAME   44
#define BOOTP_OFF_FILE    108
#define BOOTP_OFF_MAGIC   236
#define BOOTP_OFF_OPTIONS 240

#define DHCP_PKT_MAX      576   /* RFC minimum reassembly; ample for us      */

/* EAGAIN as returned by the kernel (would-block, keep polling). */
#define EAGAIN_NEG      (-11)

/* Per-receive poll bound, and number of DISCOVER/REQUEST retransmits. */
#define DHCP_POLL_MAX    200000
#define DHCP_RETRIES     4

/* ---- raw 5-arg inline syscall (rdi/rsi/rdx/r10/r8) ---------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* recvfrom out-address payload (kernel fills ip+port in HOST order). */
typedef struct {
    u32 ip;
    u16 port;
    u16 _pad;
} sock_addr_t;

/* Mirror of the kernel net_info_t (SYS_NET_INFO). */
typedef struct {
    u8  mac[6];
    u8  _pad[2];
    u32 ip;
    u32 gateway;
} net_info_t;

/* ---- tiny freestanding helpers ----------------------------------------- */

static void d_memzero(void* p, unsigned int n)
{
    u8* b = (u8*)p;
    for (unsigned int i = 0; i < n; i++) b[i] = 0;
}

static void d_memcpy(void* dst, const void* src, unsigned int n)
{
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
}

/* Read a big-endian u32 from p (network order -> host value). */
static u32 be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] <<  8) | ((u32)p[3]);
}

/* Store a host u32 as big-endian (network order) at p. */
static void put_be32(u8* p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >>  8);
    p[3] = (u8)(v);
}

/* Store a host u16 as big-endian at p. */
static void put_be16(u8* p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v);
}

/* ---- MAC retrieval ------------------------------------------------------ */

/* Fill mac[6] from SYS_NET_INFO. Returns 0 on success, negative on failure. */
static int get_mac(u8 mac[6])
{
    net_info_t info;
    d_memzero(&info, sizeof(info));
    long rc = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0);
    if (rc < 0) return DHCP_E_NOMAC;
    d_memcpy(mac, info.mac, 6);
    return 0;
}

/* ---- transaction id ----------------------------------------------------- */

/*
 * Get a random 32-bit xid via SYS_RANDOM.  If the CSPRNG syscall is missing or
 * fails we fall back to mixing the NIC pump counter so the xid is at least
 * non-constant (it does not need to be cryptographic, just unpredictable
 * enough that a stale reply from a previous run is unlikely to match).
 */
static u32 gen_xid(void)
{
    u32 x = 0;
    long rc = sc(SYS_RANDOM, (long)&x, (long)sizeof(x), 0, 0, 0);
    if (rc != (long)sizeof(x) || x == 0) {
        u32 t = (u32)sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
        x ^= (t * 2654435761u) ^ 0xA53C9E37u;
        if (x == 0) x = 0x44484350u; /* "DHCP" */
    }
    return x;
}

/* ---- packet building ---------------------------------------------------- */

/*
 * Build a DHCP request packet (DISCOVER or REQUEST) into `out` (capacity
 * DHCP_PKT_MAX).  `mtype` is DHCPDISCOVER or DHCPREQUEST.  For REQUEST,
 * `req_ip` (host order) is added as option 50 and `server_id` (host order) as
 * option 54; pass 0 for both on DISCOVER.  Returns the total packet length.
 */
static int build_packet(u8* out, u32 xid, const u8 mac[6], u8 mtype,
                        u32 req_ip, u32 server_id)
{
    d_memzero(out, BOOTP_OFF_OPTIONS);

    out[BOOTP_OFF_OP]    = BOOTP_OP_REQUEST;
    out[BOOTP_OFF_HTYPE] = BOOTP_HTYPE_ETHER;
    out[BOOTP_OFF_HLEN]  = BOOTP_HLEN_ETHER;
    out[BOOTP_OFF_HOPS]  = 0;

    put_be32(out + BOOTP_OFF_XID, xid);
    put_be16(out + BOOTP_OFF_SECS, 0);
    put_be16(out + BOOTP_OFF_FLAGS, (u16)BOOTP_FLAG_BCAST);

    /* ciaddr/yiaddr/siaddr/giaddr already zero from d_memzero. */

    d_memcpy(out + BOOTP_OFF_CHADDR, mac, 6);   /* rest of chaddr stays 0 */

    /* magic cookie (big-endian on the wire). */
    put_be32(out + BOOTP_OFF_MAGIC, DHCP_MAGIC);

    int p = BOOTP_OFF_OPTIONS;

    /* Option 53: DHCP message type. */
    out[p++] = OPT_MSG_TYPE;
    out[p++] = 1;
    out[p++] = mtype;

    /* Option 55: parameter request list -> subnet(1), router(3), dns(6). */
    out[p++] = OPT_PARAM_LIST;
    out[p++] = 3;
    out[p++] = OPT_SUBNET_MASK;
    out[p++] = OPT_ROUTER;
    out[p++] = OPT_DNS;

    /* REQUEST adds the requested IP (50) and the server id (54). */
    if (mtype == DHCPREQUEST) {
        out[p++] = OPT_REQUESTED_IP;
        out[p++] = 4;
        put_be32(out + p, req_ip);
        p += 4;

        out[p++] = OPT_SERVER_ID;
        out[p++] = 4;
        put_be32(out + p, server_id);
        p += 4;
    }

    /* Option 255: end. */
    out[p++] = OPT_END;

    return p;
}

/* ---- option parsing ----------------------------------------------------- */

/*
 * Locate DHCP option `code` within the options area of a received packet.
 * `pkt`/`len` is the whole datagram.  On success returns the option *data*
 * length and sets *data_off to the offset of the first data byte; returns -1
 * if not found or the options area is malformed.  Skips PAD (0) bytes and
 * stops at END (255).
 */
static int find_option(const u8* pkt, int len, u8 code, int* data_off)
{
    int p = BOOTP_OFF_OPTIONS;
    while (p < len) {
        u8 op = pkt[p];
        if (op == OPT_PAD) { p++; continue; }
        if (op == OPT_END) return -1;
        if (p + 1 >= len) return -1;             /* truncated length byte */
        int olen = pkt[p + 1];
        int odata = p + 2;
        if (odata + olen > len) return -1;        /* truncated data */
        if (op == code) {
            *data_off = odata;
            return olen;
        }
        p = odata + olen;
    }
    return -1;
}

/* Return the DHCP message type (option 53) of pkt, or -1 if absent. */
static int parse_msg_type(const u8* pkt, int len)
{
    int off;
    int ol = find_option(pkt, len, OPT_MSG_TYPE, &off);
    if (ol < 1) return -1;
    return pkt[off];
}

/*
 * Parse a DHCP reply (OFFER or ACK) into *lease.  Reads yiaddr from the BOOTP
 * header and options 1/3/6/54/51.  All addresses converted to host order.
 * Returns 0 on success (yiaddr present), negative on a malformed packet.
 */
static int parse_lease(const u8* pkt, int len, dhcp_lease_t* lease)
{
    if (len < BOOTP_OFF_OPTIONS) return DHCP_E_PARSE;

    d_memzero(lease, sizeof(*lease));

    lease->ip = be32(pkt + BOOTP_OFF_YIADDR);     /* offered IP (host order) */

    int off, ol;

    ol = find_option(pkt, len, OPT_SUBNET_MASK, &off);
    if (ol == 4) lease->netmask = be32(pkt + off);

    ol = find_option(pkt, len, OPT_ROUTER, &off);
    if (ol >= 4) lease->gateway = be32(pkt + off);   /* first router only */

    ol = find_option(pkt, len, OPT_DNS, &off);
    if (ol >= 4) lease->dns = be32(pkt + off);       /* first DNS only */

    ol = find_option(pkt, len, OPT_SERVER_ID, &off);
    if (ol == 4) lease->server = be32(pkt + off);

    ol = find_option(pkt, len, OPT_LEASE_TIME, &off);
    if (ol == 4) lease->lease_secs = be32(pkt + off);

    if (lease->ip == 0) return DHCP_E_PARSE;          /* no usable address */
    return 0;
}

/* ---- bounded receive ---------------------------------------------------- */

/*
 * Wait (bounded) for a DHCP reply on socket `fd` whose xid matches `xid` and
 * whose option 53 message type equals `want_type`.  Copies the datagram into
 * buf (capacity cap) and returns its length, or a negative DHCP_E* on
 * timeout.  A DHCPNAK with matching xid short-circuits to DHCP_E_NAK.
 */
static int wait_reply(int fd, u32 xid, u8 want_type, u8* buf, int cap)
{
    sock_addr_t from;

    for (int it = 0; it < DHCP_POLL_MAX; it++) {
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);          /* drive RX + timers */

        long n = sc(SYS_RECVFROM, fd, (long)buf, (long)cap, (long)&from, 0);
        if (n > 0) {
            int rlen = (int)n;
            /* Must be a BOOTP reply with our xid and the magic cookie. */
            if (rlen >= BOOTP_OFF_OPTIONS &&
                buf[BOOTP_OFF_OP] == BOOTP_OP_REPLY &&
                be32(buf + BOOTP_OFF_XID) == xid &&
                be32(buf + BOOTP_OFF_MAGIC) == DHCP_MAGIC) {

                int mt = parse_msg_type(buf, rlen);
                if (mt == DHCPNAK) return DHCP_E_NAK;
                if (mt == (int)want_type) return rlen;
            }
            /* Not for us / wrong type: keep waiting within the bound. */
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
            continue;
        }
        if (n == EAGAIN_NEG || n == 0 || n < 0) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
            continue;
        }
    }
    return DHCP_E_TIMEO;
}

#ifdef DHCP_USE_RAW_FALLBACK
/*
 * Optional raw-frame helpers (Ethernet + IPv4 + UDP) for environments where
 * the kernel UDP demux does not deliver the broadcast OFFER/ACK to our socket.
 * Disabled by default; enable with -DDHCP_USE_RAW_FALLBACK.  Only the framing
 * primitives are provided here -- the integrator can drive them in parallel
 * with the socket path if needed.
 */
#define ETH_HLEN      14
#define IP_HLEN       20
#define UDP_HLEN      8
#define ETH_P_IP      0x0800
#define IPPROTO_UDP   17

/* One's-complement Internet checksum over n bytes at p. */
static u16 inet_csum(const u8* p, int n)
{
    u32 sum = 0;
    int i = 0;
    while (i + 1 < n) { sum += ((u32)p[i] << 8) | p[i + 1]; i += 2; }
    if (i < n) sum += (u32)p[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

/*
 * Build an Ethernet+IP+UDP frame carrying `payload` (len bytes) from
 * (src_mac, 0.0.0.0:68) to (ff:ff:ff:ff:ff:ff, 255.255.255.255:67) into
 * `frame` (must be >= ETH_HLEN+IP_HLEN+UDP_HLEN+len).  Returns frame length.
 */
static int build_raw_frame(u8* frame, const u8 src_mac[6],
                           const u8* payload, int len)
{
    int p = 0;
    /* Ethernet: dst = broadcast, src = our MAC, type = IPv4. */
    for (int i = 0; i < 6; i++) frame[p++] = 0xFF;
    d_memcpy(frame + p, src_mac, 6); p += 6;
    put_be16(frame + p, (u16)ETH_P_IP); p += 2;

    u8* ip = frame + p;
    int iptot = IP_HLEN + UDP_HLEN + len;
    d_memzero(ip, IP_HLEN);
    ip[0] = 0x45;                       /* version 4, IHL 5 */
    put_be16(ip + 2, (u16)iptot);       /* total length */
    ip[8] = 64;                         /* TTL */
    ip[9] = IPPROTO_UDP;                /* protocol */
    put_be32(ip + 12, 0x00000000u);     /* src 0.0.0.0 */
    put_be32(ip + 16, IP_BROADCAST);    /* dst 255.255.255.255 */
    put_be16(ip + 10, inet_csum(ip, IP_HLEN));
    p += IP_HLEN;

    u8* udp = frame + p;
    put_be16(udp + 0, (u16)DHCP_CLIENT_PORT);
    put_be16(udp + 2, (u16)DHCP_SERVER_PORT);
    put_be16(udp + 4, (u16)(UDP_HLEN + len));
    put_be16(udp + 6, 0);              /* checksum optional for IPv4 UDP */
    p += UDP_HLEN;

    d_memcpy(frame + p, payload, (unsigned int)len);
    p += len;
    return p;
}
#endif /* DHCP_USE_RAW_FALLBACK */

/* ========================================================================= *
 * Public API
 * ========================================================================= */

int dhcp_acquire(dhcp_lease_t* out)
{
    if (!out) return DHCP_E_INVAL;

    u8 mac[6];
    if (get_mac(mac) != 0) return DHCP_E_NOMAC;

    long fd = sc(SYS_SOCKET, SOCK_DGRAM, 0, 0, 0, 0);
    if (fd < 0) return DHCP_E_SOCK;

    u8 pkt[DHCP_PKT_MAX];
    u8 rbuf[DHCP_PKT_MAX];
    int result = DHCP_E_TIMEO;

    for (int attempt = 0; attempt < DHCP_RETRIES; attempt++) {
        u32 xid = gen_xid();

        /* ---- DISCOVER ---- */
        int dlen = build_packet(pkt, xid, mac, DHCPDISCOVER, 0, 0);
        long sent = sc(SYS_SENDTO, fd, (long)pkt, (long)dlen,
                       (long)IP_BROADCAST, DHCP_SERVER_PORT);
        if (sent < 0) { result = DHCP_E_SEND; continue; }

        /* ---- wait OFFER ---- */
        int olen = wait_reply(fd, xid, DHCPOFFER, rbuf, (int)sizeof(rbuf));
        if (olen < 0) { result = olen; continue; }

        dhcp_lease_t offered;
        if (parse_lease(rbuf, olen, &offered) != 0) {
            result = DHCP_E_PARSE;
            continue;
        }

        /* ---- REQUEST the offered address from the offering server ---- */
        int rlen = build_packet(pkt, xid, mac, DHCPREQUEST,
                                offered.ip, offered.server);
        sent = sc(SYS_SENDTO, fd, (long)pkt, (long)rlen,
                  (long)IP_BROADCAST, DHCP_SERVER_PORT);
        if (sent < 0) { result = DHCP_E_SEND; continue; }

        /* ---- wait ACK ---- */
        int alen = wait_reply(fd, xid, DHCPACK, rbuf, (int)sizeof(rbuf));
        if (alen == DHCP_E_NAK) { result = DHCP_E_NAK; continue; }
        if (alen < 0) { result = alen; continue; }

        dhcp_lease_t acked;
        if (parse_lease(rbuf, alen, &acked) != 0) {
            result = DHCP_E_PARSE;
            continue;
        }

        /* The ACK is authoritative; if it omitted an option, fall back to the
         * value the OFFER gave us so the caller still gets a usable config. */
        if (acked.netmask    == 0) acked.netmask    = offered.netmask;
        if (acked.gateway    == 0) acked.gateway    = offered.gateway;
        if (acked.dns        == 0) acked.dns        = offered.dns;
        if (acked.server     == 0) acked.server     = offered.server;
        if (acked.lease_secs == 0) acked.lease_secs = offered.lease_secs;

        *out = acked;
        result = DHCP_OK;
        break;
    }

    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    return result;
}

/* ========================================================================= *
 * Offline self test (no live DHCP server required)
 * ========================================================================= */

/*
 * Hardcoded sample DHCPACK datagram (BOOTP reply).  Expected extracted values:
 *   yiaddr      = 192.168.1.100  (0xC0A80164)
 *   netmask (1) = 255.255.255.0  (0xFFFFFF00)
 *   router  (3) = 192.168.1.1    (0xC0A80101)
 *   dns     (6) = 8.8.8.8        (0x08080808)
 *   server (54) = 192.168.1.1    (0xC0A80101)
 *   lease  (51) = 86400 sec      (0x00015180)
 *   msgtype(53) = DHCPACK (5)
 */
static const u8 g_sample_ack[] = {
    /* op htype hlen hops */
    0x02, 0x01, 0x06, 0x00,
    /* xid */
    0x39, 0x03, 0xF3, 0x26,
    /* secs flags */
    0x00, 0x00, 0x80, 0x00,
    /* ciaddr */
    0x00, 0x00, 0x00, 0x00,
    /* yiaddr = 192.168.1.100 */
    0xC0, 0xA8, 0x01, 0x64,
    /* siaddr = 192.168.1.1 */
    0xC0, 0xA8, 0x01, 0x01,
    /* giaddr */
    0x00, 0x00, 0x00, 0x00,
    /* chaddr (16) */
    0x00, 0x0C, 0x29, 0xAB, 0xCD, 0xEF, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* sname (64) -- zero */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* file (128) -- zero */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* magic cookie 0x63825363 */
    0x63, 0x82, 0x53, 0x63,
    /* options: */
    OPT_MSG_TYPE,    1, DHCPACK,                       /* 53: ACK            */
    OPT_SERVER_ID,   4, 0xC0, 0xA8, 0x01, 0x01,        /* 54: 192.168.1.1    */
    OPT_LEASE_TIME,  4, 0x00, 0x01, 0x51, 0x80,        /* 51: 86400          */
    OPT_SUBNET_MASK, 4, 0xFF, 0xFF, 0xFF, 0x00,        /* 1 : 255.255.255.0  */
    OPT_ROUTER,      4, 0xC0, 0xA8, 0x01, 0x01,        /* 3 : 192.168.1.1    */
    OPT_DNS,         4, 0x08, 0x08, 0x08, 0x08,        /* 6 : 8.8.8.8        */
    OPT_END
};

int dhcp_selftest(void)
{
    /* ---- 1. Build a DISCOVER and verify its structure ---- */
    u8 pkt[DHCP_PKT_MAX];
    u8 mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    u32 xid = 0x12345678u;

    int dlen = build_packet(pkt, xid, mac, DHCPDISCOVER, 0, 0);
    if (dlen <= BOOTP_OFF_OPTIONS) return -101;

    if (pkt[BOOTP_OFF_OP]    != BOOTP_OP_REQUEST) return -102;
    if (pkt[BOOTP_OFF_HTYPE] != BOOTP_HTYPE_ETHER) return -103;
    if (pkt[BOOTP_OFF_HLEN]  != BOOTP_HLEN_ETHER) return -104;

    /* xid round-trips big-endian. */
    if (be32(pkt + BOOTP_OFF_XID) != xid) return -105;

    /* broadcast flag set. */
    if (((((u32)pkt[BOOTP_OFF_FLAGS] << 8) | pkt[BOOTP_OFF_FLAGS + 1]) &
         BOOTP_FLAG_BCAST) == 0) return -106;

    /* chaddr carries our MAC. */
    for (int i = 0; i < 6; i++)
        if (pkt[BOOTP_OFF_CHADDR + i] != mac[i]) return -107;

    /* magic cookie present at offset 236. */
    if (be32(pkt + BOOTP_OFF_MAGIC) != DHCP_MAGIC) return -108;

    /* option 53 present and == DHCPDISCOVER. */
    if (parse_msg_type(pkt, dlen) != DHCPDISCOVER) return -109;

    /* param request list (option 55) present. */
    int off;
    if (find_option(pkt, dlen, OPT_PARAM_LIST, &off) < 1) return -110;

    /* ---- 2. Parse the hardcoded sample ACK and check extracted fields ---- */
    int slen = (int)sizeof(g_sample_ack);

    if (parse_msg_type(g_sample_ack, slen) != DHCPACK) return -120;

    dhcp_lease_t lease;
    if (parse_lease(g_sample_ack, slen, &lease) != 0) return -121;

    if (lease.ip         != 0xC0A80164u) return -122;  /* 192.168.1.100 */
    if (lease.netmask    != 0xFFFFFF00u) return -123;  /* 255.255.255.0 */
    if (lease.gateway    != 0xC0A80101u) return -124;  /* 192.168.1.1   */
    if (lease.dns        != 0x08080808u) return -125;  /* 8.8.8.8       */
    if (lease.server     != 0xC0A80101u) return -126;  /* 192.168.1.1   */
    if (lease.lease_secs != 86400u)      return -127;  /* 0x00015180    */

    /* ---- 3. Build a REQUEST and verify options 50/54 round-trip ---- */
    int rlen = build_packet(pkt, xid, mac, DHCPREQUEST,
                            0xC0A80164u, 0xC0A80101u);
    if (parse_msg_type(pkt, rlen) != DHCPREQUEST) return -130;

    int rio = find_option(pkt, rlen, OPT_REQUESTED_IP, &off);
    if (rio != 4 || be32(pkt + off) != 0xC0A80164u) return -131;

    int sio = find_option(pkt, rlen, OPT_SERVER_ID, &off);
    if (sio != 4 || be32(pkt + off) != 0xC0A80101u) return -132;

    return DHCP_OK;
}
