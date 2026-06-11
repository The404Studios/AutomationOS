/*
 * traceroute.c -- userspace ICMP-based hop discovery (freestanding, ring 3).
 * ==========================================================================
 *
 * A from-scratch `traceroute` for AutomationOS userspace.  NO libc, NO stdio,
 * NO malloc, NO standard headers -- everything is built on inline syscalls,
 * fixed-size buffers and a handful of static helpers.  Diagnostics go to
 * fd 1 via SYS_WRITE.
 *
 * Linked with userspace/crt0.asm, which supplies _start and calls
 *   int main(int argc, char **argv);
 * the return value of main becomes the SYS_EXIT status.
 *
 * Usage:
 *   traceroute HOST [-m MAX_TTL] [-q N_PROBES_PER_HOP]
 *       Resolve HOST, then iterate TTL 1..MAX_TTL sending N_PROBES_PER_HOP
 *       ICMP ECHO requests per TTL level.  For each probe we expect either:
 *         - ICMP TIME EXCEEDED (type 11, code 0) from an intermediate router
 *           -- printed as the hop address; continue to next TTL.
 *         - ICMP ECHO REPLY (type 0) from the destination
 *           -- printed and then exit (destination reached).
 *       Stop when ECHO REPLY arrives from dst, or MAX_TTL is reached.
 *       Defaults: MAX_TTL=30, N_PROBES_PER_HOP=3.
 *
 *   traceroute          (argc <= 1)
 *       Self-test: traceroute the gateway (info.gateway, default 10.0.2.2).
 *       The gateway is 1 hop away (direct) so either ECHO REPLY (type 0) or
 *       TIME EXCEEDED (type 11) from the gateway qualifies as a detected hop.
 *       Prints one of:
 *           TRACEROUTE SELFTEST: PASS
 *           TRACEROUTE SELFTEST: SKIP (no link)
 *           TRACEROUTE SELFTEST: FAIL
 *       Returns 0 on PASS/SKIP, 1 on FAIL.
 *
 * Deviations from real UNIX traceroute:
 *   - Real traceroute uses UDP probes to incrementing dest ports (by default).
 *     We use ICMP ECHO probes (like traceroute -I / Windows tracert) because
 *     our kernel's raw socket stack exposes ICMP through the same SYS_NET_SEND
 *     / SYS_NET_RECV interface already used by ping.c.
 *   - No DNS reverse-lookup of hop IPs (no PTR query).
 *   - RTT uses SYS_GET_TICKS_MS (1 ms resolution); if the syscall is
 *     unavailable (returns 0) we print "ok" instead.
 *   - No IPv6 support.
 *   - No parallel probe dispatch; probes within a TTL level are sequential.
 *   - Timeout per probe is bounded by RX_TRIES_PROBE poll iterations + yield,
 *     not a wall-clock timer.
 *
 * Build (EXACT -- no fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/traceroute/traceroute.c -o traceroute.o
 *   objdump -d traceroute.o | grep fs:0x28   # MUST be empty
 */

#include "../../lib/net/dns.h"   /* int dns_resolve(const char*, unsigned int*); */

/* ---- syscall numbers ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_GET_TICKS_MS  40
#define SYS_YIELD         15
#define SYS_NET_INFO      59
#define SYS_NET_SEND      68
#define SYS_NET_RECV      69

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* net_info_t mirror -- MUST match kernel net_info_ext_t (kernel/include/netif.h). */
typedef struct {
    char ifname[16];
    u8   mac[6];
    u8   _pad[2];
    u32  ip;
    u32  netmask;
    u32  gateway;
    u32  dns;
    u8   up;
    u8   dhcp_active;
    u8   _pad2[6];
    u64  tx_packets;
    u64  rx_packets;
    u64  tx_bytes;
    u64  rx_bytes;
} net_info_t;

/*
 * 6-argument inline syscall (n + up to 5 args -> rdi/rsi/rdx/r10/r8).
 */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny serial diagnostics (fd 1) ---- */
static unsigned long k_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void print(const char *m) {
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0);
}
static void print_dec(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0); }
}
static void print_hex2(u8 v) {
    const char *h = "0123456789abcdef";
    char b[2] = { h[(v >> 4) & 0xF], h[v & 0xF] };
    sc(SYS_WRITE, 1, (long)b, 2, 0, 0);
}
static void print_mac(const u8 m[6]) {
    for (int i = 0; i < 6; i++) { if (i) print(":"); print_hex2(m[i]); }
}
static void print_ip(u32 ip /* host order */) {
    print_dec((ip >> 24) & 0xFF); print(".");
    print_dec((ip >> 16) & 0xFF); print(".");
    print_dec((ip >> 8)  & 0xFF); print(".");
    print_dec(ip & 0xFF);
}

/* ---- tiny mem helpers ---- */
static void *k_memset(void *d, int c, unsigned long n) {
    u8 *p = (u8*)d; while (n--) *p++ = (u8)c; return d;
}
static void *k_memcpy(void *d, const void *s, unsigned long n) {
    u8 *dp = (u8*)d; const u8 *sp = (const u8*)s; while (n--) *dp++ = *sp++; return d;
}
/* Big-endian helpers (wire order). */
static u16 be16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static u32 be32(u32 x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
/* Read a 16-bit big-endian field from a byte pointer -> host order. */
static u16 rd_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
/* Read a 32-bit big-endian field from a byte pointer -> host order. */
static u32 rd_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/*
 * Standard 16-bit one's-complement Internet checksum over `len` bytes.
 * Returns value in wire (big-endian) order ready to write into the header.
 */
static u16 inet_csum(const u8 *data, unsigned long len) {
    u32 sum = 0;
    unsigned long i = 0;
    for (; i + 1 < len; i += 2)
        sum += (u32)((data[i] << 8) | data[i + 1]);
    if (i < len)                     /* odd trailing byte */
        sum += (u32)(data[i] << 8);
    while (sum >> 16)                /* fold carries */
        sum = (sum & 0xFFFF) + (sum >> 16);
    u16 host = (u16)(~sum & 0xFFFF);
    return be16(host);               /* to big-endian wire order */
}

/* ---- protocol constants ---- */
#define ETH_HLEN        14
#define ETH_P_ARP       0x0806
#define ETH_P_IP        0x0800
#define IPPROTO_ICMP    1
#define ICMP_ECHOREP    0    /* echo reply   */
#define ICMP_TIMEEXC    11   /* time exceeded */
#define ICMP_ECHO       8    /* echo request */
#define ICMP_CODE_TTL   0    /* TTL exceeded in transit */

/* Receive-loop iteration caps -- prevent infinite spin on quiet networks. */
#define RX_TRIES_ARP    40000
#define RX_TRIES_PROBE  40000

/* ARP frame layout constants. */
#define ARP_OFF         ETH_HLEN
#define ARP_LEN         28
#define ARP_FRAME_LEN   (ETH_HLEN + ARP_LEN)

/* Default gateway when SYS_NET_INFO reports none (QEMU slirp). */
#define DEFAULT_GW      0x0A000202u  /* 10.0.2.2 */

/* Traceroute defaults. */
#define DEFAULT_MAX_TTL  30
#define DEFAULT_NPROBES  3

/* Probe ICMP identifier (fixed; we filter replies by id+seq). */
#define PROBE_ID         0x5452u   /* 'TR' */

/* ICMP echo payload -- small, fixed, distinct from ping. */
static const u8 probe_payload[] = {
    'T','r','a','c','e','R','o','u','t','e','O','S'
};
#define PROBE_PAYLOAD_LEN  ((u32)sizeof(probe_payload))

/* -----------------------------------------------------------------------
 * get_ticks_ms -- wrapper around SYS_GET_TICKS_MS.
 * Returns 0 if the syscall is unavailable or returns 0 (treated as "not
 * supported") -- callers print "ok" in that case.
 * --------------------------------------------------------------------- */
static u64 get_ticks_ms(void) {
    long r = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    if (r <= 0) return 0;
    return (u64)r;
}

/* -----------------------------------------------------------------------
 * arp_resolve -- learn the next-hop MAC for `target_ip` (host order).
 * Broadcasts an ARP request, then bounded-polls for the reply.
 * Writes 6 bytes to out_mac.  Returns 1 on success, 0 on timeout.
 * --------------------------------------------------------------------- */
static int arp_resolve(const net_info_t *info, u32 target_ip, u8 out_mac[6]) {
    u8 frame[64];
    k_memset(frame, 0, sizeof(frame));

    /* Ethernet header: dst=broadcast, src=our MAC, type=ARP. */
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    k_memcpy(frame + 6, info->mac, 6);
    u16 et = be16(ETH_P_ARP);
    k_memcpy(frame + 12, &et, 2);

    /* ARP payload (Ethernet / IPv4). */
    u8 *a = frame + ARP_OFF;
    u16 htype = be16(1);         k_memcpy(a + 0,  &htype, 2);  /* Ethernet */
    u16 ptype = be16(ETH_P_IP);  k_memcpy(a + 2,  &ptype, 2);  /* IPv4     */
    a[4] = 6;  /* hlen */
    a[5] = 4;  /* plen */
    u16 oper = be16(1);          k_memcpy(a + 6,  &oper, 2);   /* request  */
    k_memcpy(a + 8,  info->mac, 6);
    u32 spa = be32(info->ip);    k_memcpy(a + 14, &spa, 4);
    /* a+18..23: target HW = 0 (unknown) */
    u32 tpa = be32(target_ip);   k_memcpy(a + 24, &tpa, 4);

    print("[TR] ARP who-has "); print_ip(target_ip); print(" ...\n");

    long sent = sc(SYS_NET_SEND, (long)frame, (long)ARP_FRAME_LEN, 0, 0, 0);
    if (sent < 0) {
        print("[TR] ARP send failed rc=");
        print_dec((unsigned long)(-sent)); print("\n");
        return 0;
    }

    u8 rxbuf[1600];
    for (long tries = 0; tries < RX_TRIES_ARP; tries++) {
        long n = sc(SYS_NET_RECV, (long)rxbuf, (long)sizeof(rxbuf), 0, 0, 0);
        if (n <= 0) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        if (n < ETH_HLEN + ARP_LEN) continue;

        if (rd_be16(rxbuf + 12) != ETH_P_ARP) continue;

        u16 op = rd_be16(rxbuf + ARP_OFF + 6);
        if (op != 2) continue;                         /* want ARP reply */

        u32 reply_spa = rd_be32(rxbuf + ARP_OFF + 14);
        if (reply_spa != target_ip) continue;

        k_memcpy(out_mac, rxbuf + ARP_OFF + 8, 6);
        print("[TR] gateway MAC "); print_mac(out_mac); print("\n");
        return 1;
    }
    print("[TR] ARP timeout for "); print_ip(target_ip); print("\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * Probe result codes returned by send_probe().
 * --------------------------------------------------------------------- */
#define PROBE_TIMEOUT   0   /* no matching reply within RX_TRIES_PROBE  */
#define PROBE_TIMEEXC   1   /* ICMP TIME EXCEEDED (type 11) received     */
#define PROBE_ECHOREPLY 2   /* ICMP ECHO REPLY (type 0) received         */
#define PROBE_TXERR    -1   /* SYS_NET_SEND failed                       */

/*
 * send_probe -- build and TX one ICMP echo at the specified TTL, then
 * bounded-poll for either ICMP TIME EXCEEDED or ICMP ECHO REPLY.
 *
 * Parameters:
 *   info    -- our own NIC info
 *   gw_mac  -- next-hop MAC (resolved by arp_resolve)
 *   dst_ip  -- final destination (host order)
 *   ttl     -- IPv4 TTL field for this probe (1-based)
 *   id      -- ICMP identifier (distinguishes our probes)
 *   seq     -- ICMP sequence number (unique per probe)
 *   out_src -- filled with the source IP of the replying host (host order)
 *   out_rtt_ms -- filled with RTT in ms; 0 means "ticks unavailable"
 *
 * Returns one of the PROBE_* codes above.
 *
 * Frame layout: Ethernet(14) | IPv4(20) | ICMP(8 + probe_payload)
 *
 * For TIME EXCEEDED replies the inner (original) IP+ICMP header is also
 * checked so we match on our id+seq and ignore stray ICMP TTL errors not
 * meant for us.
 */
static int send_probe(const net_info_t *info, const u8 gw_mac[6],
                      u32 dst_ip, u8 ttl, u16 id, u16 seq,
                      u32 *out_src, u64 *out_rtt_ms) {
    static u8 frame[ETH_HLEN + 20 + 8 + 64];   /* static: no stack growth */
    k_memset(frame, 0, sizeof(frame));

    u32 icmp_len  = 8 + PROBE_PAYLOAD_LEN;
    u32 ip_total  = 20 + icmp_len;
    u32 frame_len = ETH_HLEN + ip_total;

    /* --- Ethernet header --- */
    k_memcpy(frame + 0, gw_mac,    6);
    k_memcpy(frame + 6, info->mac, 6);
    u16 et = be16(ETH_P_IP);
    k_memcpy(frame + 12, &et, 2);

    /* --- IPv4 header --- */
    u8 *ip = frame + ETH_HLEN;
    ip[0] = 0x45;                                   /* ver=4, IHL=5 (20 B)    */
    ip[1] = 0x00;                                   /* DSCP/ECN               */
    u16 tl = be16((u16)ip_total);  k_memcpy(ip + 2,  &tl,  2); /* total len  */
    u16 idf = be16(id);            k_memcpy(ip + 4,  &idf, 2); /* ident       */
    ip[6] = 0x00; ip[7] = 0x00;                     /* flags + frag offset    */
    ip[8] = ttl;                                    /* << TTL set here        */
    ip[9] = IPPROTO_ICMP;
    ip[10] = 0x00; ip[11] = 0x00;                   /* checksum (zero first)  */
    u32 src = be32(info->ip);      k_memcpy(ip + 12, &src, 4);
    u32 dst = be32(dst_ip);        k_memcpy(ip + 16, &dst, 4);
    u16 ipck = inet_csum(ip, 20);  k_memcpy(ip + 10, &ipck, 2); /* IP csum   */

    /* --- ICMP echo request --- */
    u8 *ic = ip + 20;
    ic[0] = ICMP_ECHO;                              /* type 8                 */
    ic[1] = 0x00;                                   /* code 0                 */
    ic[2] = 0x00; ic[3] = 0x00;                     /* checksum (zero first)  */
    u16 idbe = be16(id);   k_memcpy(ic + 4, &idbe, 2);
    u16 sqbe = be16(seq);  k_memcpy(ic + 6, &sqbe, 2);
    k_memcpy(ic + 8, probe_payload, PROBE_PAYLOAD_LEN);
    u16 icck = inet_csum(ic, icmp_len);
    k_memcpy(ic + 2, &icck, 2);

    /* Record send time (0 if syscall not available). */
    u64 t0 = get_ticks_ms();

    long sent = sc(SYS_NET_SEND, (long)frame, (long)frame_len, 0, 0, 0);
    if (sent < 0) {
        *out_src     = 0;
        *out_rtt_ms  = 0;
        return PROBE_TXERR;
    }

    /* Bounded poll: accept ECHO REPLY from dst, or TIME EXCEEDED from anyone
     * whose embedded IP header matches our probe (id + seq). */
    static u8 rxbuf[1600];
    for (long tries = 0; tries < RX_TRIES_PROBE; tries++) {
        long n = sc(SYS_NET_RECV, (long)rxbuf, (long)sizeof(rxbuf), 0, 0, 0);
        if (n <= 0) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        if (n < ETH_HLEN + 20 + 8) continue;
        if (rd_be16(rxbuf + 12) != ETH_P_IP) continue;

        u8 *rip = rxbuf + ETH_HLEN;
        if ((rip[0] >> 4) != 4) continue;             /* IPv4 only            */
        u32 rihl = (u32)(rip[0] & 0x0F) * 4;
        if (rihl < 20) continue;
        if (rip[9] != IPPROTO_ICMP) continue;
        if ((unsigned long)n < (unsigned long)(ETH_HLEN + rihl + 8)) continue;

        u32 reply_src = rd_be32(rip + 12);            /* replying host IP     */
        u8 *ricmp = rip + rihl;
        u8  rtype = ricmp[0];
        u8  rcode = ricmp[1];

        /* --- Case 1: ICMP ECHO REPLY (type 0) from the destination. --- */
        if (rtype == ICMP_ECHOREP && rcode == 0) {
            /* Must be from dst_ip and carry our id+seq. */
            if (reply_src != dst_ip) continue;
            if (rd_be16(ricmp + 4) != id)  continue;
            if (rd_be16(ricmp + 6) != seq) continue;

            u64 t1 = get_ticks_ms();
            *out_src    = reply_src;
            *out_rtt_ms = (t1 > t0) ? (t1 - t0) : 0;
            return PROBE_ECHOREPLY;
        }

        /* --- Case 2: ICMP TIME EXCEEDED (type 11, code 0). --- */
        if (rtype == ICMP_TIMEEXC && rcode == ICMP_CODE_TTL) {
            /*
             * The ICMP TIME EXCEEDED payload is:
             *   4 bytes unused | original IP header | first 8 bytes of original
             *   IP payload (= first 8 bytes of our ICMP echo header).
             *
             * Layout within ricmp:
             *   ricmp[0]  = type (11)
             *   ricmp[1]  = code (0)
             *   ricmp[2..3] = checksum
             *   ricmp[4..7] = unused (0)
             *   ricmp[8..] = original IP header (>= 20 bytes) + 8 bytes payload
             *
             * Minimum frame size for the inner header check:
             *   ETH + outer-IP(rihl) + ICMP-hdr(8) + inner-IP(20) + inner-ICMP(8)
             *                                      = ETH + rihl + 36
             */
            unsigned long need = (unsigned long)(ETH_HLEN + rihl + 8 + 20 + 8);
            if ((unsigned long)n < need) continue;

            u8 *inner_ip = ricmp + 8;               /* original IP header     */
            if ((inner_ip[0] >> 4) != 4) continue;   /* must be IPv4           */
            u32 inner_ihl = (u32)(inner_ip[0] & 0x0F) * 4;
            if (inner_ihl < 20) continue;
            if (inner_ip[9] != IPPROTO_ICMP) continue;

            /* Verify inner dst == our dst_ip (we sent it there). */
            if (rd_be32(inner_ip + 16) != dst_ip) continue;

            /* First 8 bytes of inner ICMP = type/code/csum/id/seq. */
            u8 *inner_icmp = inner_ip + inner_ihl;
            if (rd_be16(inner_icmp + 4) != id)  continue; /* our id?  */
            if (rd_be16(inner_icmp + 6) != seq) continue; /* our seq? */

            u64 t1 = get_ticks_ms();
            *out_src    = reply_src;
            *out_rtt_ms = (t1 > t0) ? (t1 - t0) : 0;
            return PROBE_TIMEEXC;
        }
        /* Any other ICMP type: keep polling. */
    }

    *out_src    = 0;
    *out_rtt_ms = 0;
    return PROBE_TIMEOUT;
}

/* -----------------------------------------------------------------------
 * Argument parsing helpers (no libc).
 * --------------------------------------------------------------------- */
static int k_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static long parse_uint(const char *s) {
    if (!s || !*s) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > 65535) return 65535;
    }
    return v;
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(int argc, char **argv) {
    print("[TR] traceroute starting\n");

    /* 1. Query NIC info. */
    net_info_t info;
    k_memset(&info, 0, sizeof(info));
    long ir = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0);

    int selftest = (argc <= 1);

    /* Link-down detection: error OR zero MAC/IP. */
    int link_down = (ir != 0);
    if (!link_down) {
        int mac_zero = 1;
        for (int i = 0; i < 6; i++) if (info.mac[i]) { mac_zero = 0; break; }
        if (mac_zero || info.ip == 0) link_down = 1;
    }

    if (link_down) {
        if (selftest) {
            print("TRACEROUTE SELFTEST: SKIP (no link)\n");
            return 0;
        }
        print("[TR] error: network link is down\n");
        return 1;
    }

    print("[TR] MAC "); print_mac(info.mac);
    print(" IP ");  print_ip(info.ip);
    print(" GW ");  print_ip(info.gateway);
    print("\n");

    /* 2. Parse arguments. */
    u32  gw       = info.gateway ? info.gateway : DEFAULT_GW;
    u32  dst_ip   = 0;
    long max_ttl  = DEFAULT_MAX_TTL;
    long nprobes  = DEFAULT_NPROBES;

    if (selftest) {
        dst_ip = gw;
        max_ttl = 2;    /* gateway is 1 hop; give it a tiny margin */
        nprobes = 3;
    } else {
        /* Parse: traceroute HOST [-m MAX_TTL] [-q N_PROBES] */
        unsigned int resolved = 0;
        if (dns_resolve(argv[1], &resolved) != 0 || resolved == 0) {
            print("[TR] could not resolve host: "); print(argv[1]); print("\n");
            return 1;
        }
        dst_ip = resolved;

        for (int i = 2; i < argc; i++) {
            if (k_strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
                long v = parse_uint(argv[i + 1]);
                if (v > 0 && v <= 255) max_ttl = v;
                i++;
            } else if (k_strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
                long v = parse_uint(argv[i + 1]);
                if (v > 0 && v <= 10) nprobes = v;
                i++;
            }
            /* Unknown flags silently ignored -- freestanding, no getopt. */
        }
    }

    print("[TR] target "); print_ip(dst_ip);
    print(" max_ttl="); print_dec((unsigned long)max_ttl);
    print(" probes/hop="); print_dec((unsigned long)nprobes);
    print("\n");

    /* 3. ARP-resolve the next-hop MAC (gateway). */
    u8 gw_mac[6];
    if (!arp_resolve(&info, gw, gw_mac)) {
        if (selftest) {
            print("TRACEROUTE SELFTEST: FAIL\n");
            return 1;
        }
        print("[TR] failed to resolve next-hop MAC\n");
        return 1;
    }

    /* 4. Main TTL loop. */
    int reached  = 0;   /* set when ECHO REPLY arrives from dst */
    int got_any  = 0;   /* set when at least one reply was seen (any hop) */
    int tx_err   = 0;   /* set if TX failed immediately on first probe     */

    for (long ttl = 1; ttl <= max_ttl && !reached; ttl++) {
        print(" "); print_dec((unsigned long)ttl); print("  ");

        /* We send nprobes probes for this TTL; for each we print the result. */
        for (long q = 0; q < nprobes; q++) {
            /*
             * Sequence number: (ttl-1)*nprobes + q + 1, clamped to u16.
             * The id is constant (PROBE_ID) so TIME EXCEEDED replies can be
             * matched back to this traceroute session across multiple probes.
             */
            u16 seq = (u16)(((ttl - 1) * nprobes + q + 1) & 0xFFFF);
            if (seq == 0) seq = 1;   /* avoid seq=0, some stacks are odd */

            u32 reply_src   = 0;
            u64 rtt_ms      = 0;
            int r = send_probe(&info, gw_mac, dst_ip,
                               (u8)ttl, PROBE_ID, seq,
                               &reply_src, &rtt_ms);

            if (r == PROBE_TXERR) {
                print("TX_ERR  ");
                if (ttl == 1 && q == 0) tx_err = 1;
                continue;
            }
            if (r == PROBE_TIMEOUT) {
                print("*  ");
                continue;
            }

            /* TIMEEXC or ECHOREPLY -- print source IP + RTT. */
            got_any = 1;
            print_ip(reply_src); print("  ");
            if (rtt_ms > 0) {
                print_dec((unsigned long)rtt_ms); print(" ms  ");
            } else {
                print("ok  ");
            }

            if (r == PROBE_ECHOREPLY) {
                /*
                 * Destination reached: mark and break out of probe loop.
                 * We still finish printing any remaining probe slots as "*"
                 * or let the TTL loop exit naturally -- but to keep output
                 * clean we break here.  The outer TTL loop checks `reached`.
                 */
                reached = 1;
                /* Print remaining probes as skipped (destination reached). */
                for (long rq = q + 1; rq < nprobes; rq++) print("(done)  ");
                break;
            }
            /* TIMEEXC: continue to next probe at same TTL. */
        }
        print("\n");
    }

    if (!selftest) {
        if (reached)
            print("[TR] destination reached\n");
        else
            print("[TR] max TTL reached without destination reply\n");
        return 0;
    }

    /* Self-test verdict:
     *   PASS  -- we got at least one valid reply (TIMEEXC or ECHOREPLY)
     *            from any hop (gateway at TTL=1 should answer immediately)
     *   SKIP  -- already handled above (link down)
     *   FAIL  -- link up, ARP succeeded, but zero replies received
     */
    if (tx_err && !got_any) {
        print("TRACEROUTE SELFTEST: FAIL\n");
        return 1;
    }
    if (got_any) {
        print("TRACEROUTE SELFTEST: PASS\n");
        return 0;
    }
    print("TRACEROUTE SELFTEST: FAIL\n");
    return 1;
}
