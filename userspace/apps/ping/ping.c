/*
 * ping.c -- userspace ICMP echo (ping) tool (freestanding, ring 3).
 * =================================================================
 *
 * A from-scratch `ping` for AutomationOS userspace. NO libc, NO stdio,
 * NO malloc, NO standard headers -- everything is built on inline syscalls,
 * fixed-size buffers and a handful of static helpers. Diagnostics go to
 * serial via SYS_WRITE(fd=1).
 *
 * Linked with userspace/crt0.asm, which supplies _start and calls
 *   int main(int argc, char **argv);
 * the return value of main becomes the SYS_EXIT status.
 *
 * Usage:
 *   ping HOST [count]
 *       Resolve HOST (dotted-quad parsed locally, or a name via dns_resolve)
 *       to an IPv4 address, then send `count` (default 4) ICMP echo requests
 *       and report the replies + a transmitted/received summary.
 *
 *   ping            (argc <= 1)
 *       Self-test: ping the gateway (info.gateway, default 10.0.2.2) and
 *       print one of:
 *           PING SELFTEST: PASS              (>=1 ICMP echo reply received)
 *           PING SELFTEST: SKIP (no link)    (NIC link down)
 *           PING SELFTEST: FAIL              (link up but TX/build path errors)
 *       main returns 0 on PASS/SKIP, 1 on FAIL.
 *
 * All multi-byte IP/ICMP fields are BIG-ENDIAN on the wire. Frame building
 * mirrors userspace/apps/nettest/nettest.c. Every receive loop is bounded by
 * an iteration cap plus SYS_YIELD so a quiet network can never hang us.
 *
 * Build (EXACT -- flags passed DIRECTLY; objdump grep for fs:0x28 MUST be empty):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/ping/ping.c -o ping.o
 *   objdump -d ping.o | grep fs:0x28   # MUST be empty (no stack canary)
 */

#include "../../lib/net/dns.h"   /* int dns_resolve(const char*, unsigned int*); */

/* ---- syscall numbers ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
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
 * 6-argument inline syscall (n + 5 args -> rdi/rsi/rdx/r10/r8).
 * Signature is exactly as specified for this tool.
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
static unsigned long k_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0); }
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
/* Big-endian helpers (wire order, from host order). */
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
 * Returns the value already in wire (big-endian) order, ready to drop into
 * a header field with k_memcpy. The complement of the running sum is taken
 * in host order, then we byte-swap the result for the wire.
 */
static u16 inet_csum(const u8 *data, unsigned long len) {
    u32 sum = 0;
    unsigned long i = 0;
    for (; i + 1 < len; i += 2) {
        sum += (u32)((data[i] << 8) | data[i + 1]);
    }
    if (i < len) {                 /* odd trailing byte */
        sum += (u32)(data[i] << 8);
    }
    while (sum >> 16) {            /* fold carries */
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    u16 host = (u16)(~sum & 0xFFFF);
    return be16(host);            /* place in big-endian wire order */
}

#define ETH_HLEN     14
#define ETH_P_ARP    0x0806
#define ETH_P_IP     0x0800
#define IPPROTO_ICMP 1
#define ICMP_ECHO    8           /* echo request type */
#define ICMP_ECHOREP 0           /* echo reply type   */

/* Receive-loop bounds (iteration cap + yield per spin). */
#define RX_TRIES_ARP  40000
#define RX_TRIES_ECHO 40000

/* ARP byte offsets within the Ethernet payload. */
#define ARP_OFF      ETH_HLEN
#define ARP_LEN      28
#define ARP_FRAME_LEN (ETH_HLEN + ARP_LEN)

/* Default gateway when SYS_NET_INFO reports none (QEMU slirp). */
#define DEFAULT_GW   0x0A000202u /* 10.0.2.2 */

/* ICMP echo payload (small, fixed). */
static const u8 echo_payload[] = {
    'A','u','t','o','m','a','t','i','o','n','O','S',
    'p','i','n','g'
};
#define ECHO_PAYLOAD_LEN ((u32)sizeof(echo_payload))

/* -----------------------------------------------------------------------
 * ARP: learn the next-hop MAC for `target_ip` (host order). Broadcasts an
 * ARP request, then bounded-polls for the matching reply. Writes 6 MAC
 * bytes to out_mac. Returns 1 on success, 0 on timeout/no-reply.
 * --------------------------------------------------------------------- */
static int arp_resolve(const net_info_t *info, u32 target_ip, u8 out_mac[6]) {
    u8 frame[64];
    k_memset(frame, 0, sizeof(frame));

    /* Ethernet header: dst = broadcast, src = our MAC, type = ARP. */
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    k_memcpy(frame + 6, info->mac, 6);
    u16 et = be16(ETH_P_ARP);
    k_memcpy(frame + 12, &et, 2);

    /* ARP payload (Ethernet/IPv4). */
    u8 *a = frame + ARP_OFF;
    u16 htype = be16(1);          k_memcpy(a + 0,  &htype, 2);  /* Ethernet */
    u16 ptype = be16(ETH_P_IP);   k_memcpy(a + 2,  &ptype, 2);  /* IPv4     */
    a[4] = 6;  /* hlen */
    a[5] = 4;  /* plen */
    u16 oper = be16(1);           k_memcpy(a + 6,  &oper, 2);   /* request  */
    k_memcpy(a + 8, info->mac, 6);                              /* sender HW */
    u32 spa = be32(info->ip);     k_memcpy(a + 14, &spa, 4);    /* sender IP */
    /* a+18..23 target HW = 0 (unknown) */
    u32 tpa = be32(target_ip);    k_memcpy(a + 24, &tpa, 4);    /* target IP */

    print("[PING] ARP who-has "); print_ip(target_ip); print(" ...\n");

    long sent = sc(SYS_NET_SEND, (long)frame, (long)ARP_FRAME_LEN, 0, 0, 0);
    if (sent < 0) {
        print("[PING] ARP send failed rc="); print_dec((unsigned long)(-sent)); print("\n");
        return 0;
    }

    u8 rxbuf[1600];
    for (long tries = 0; tries < RX_TRIES_ARP; tries++) {
        long n = sc(SYS_NET_RECV, (long)rxbuf, (long)sizeof(rxbuf), 0, 0, 0);
        if (n <= 0) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        if (n < ETH_HLEN + ARP_LEN) continue;

        u16 type = rd_be16(rxbuf + 12);
        if (type != ETH_P_ARP) continue;

        u16 op = rd_be16(rxbuf + ARP_OFF + 6);
        if (op != 2) continue;                       /* want a reply */

        u32 reply_spa = rd_be32(rxbuf + ARP_OFF + 14);
        if (reply_spa != target_ip) continue;        /* must be our target */

        k_memcpy(out_mac, rxbuf + ARP_OFF + 8, 6);   /* sender HW = its MAC */
        print("[PING] gateway/next-hop is at "); print_mac(out_mac); print("\n");
        return 1;
    }
    print("[PING] ARP timeout for "); print_ip(target_ip); print("\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * Build + send one ICMP echo request, then bounded-poll for a matching
 * echo reply from `dst_ip` (host order) carrying our id/seq.
 *
 * Returns 1 if a matching reply was received, 0 on timeout, -1 if the
 * TX/build path itself errored.
 * --------------------------------------------------------------------- */
static int ping_once(const net_info_t *info, const u8 gw_mac[6],
                     u32 dst_ip, u16 id, u16 seq) {
    /* Layout: Ethernet(14) | IPv4(20) | ICMP(8 + payload). */
    u8 frame[ETH_HLEN + 20 + 8 + 64];
    k_memset(frame, 0, sizeof(frame));

    u32 icmp_len = 8 + ECHO_PAYLOAD_LEN;      /* ICMP header + payload */
    u32 ip_total = 20 + icmp_len;             /* IPv4 header + ICMP    */
    u32 frame_len = ETH_HLEN + ip_total;

    /* --- Ethernet header --- */
    k_memcpy(frame + 0, gw_mac, 6);           /* dst = next-hop MAC */
    k_memcpy(frame + 6, info->mac, 6);        /* src = our MAC      */
    u16 et = be16(ETH_P_IP);
    k_memcpy(frame + 12, &et, 2);

    /* --- IPv4 header --- */
    u8 *ip = frame + ETH_HLEN;
    ip[0]  = 0x45;                             /* version 4, IHL 5 (20 bytes) */
    ip[1]  = 0x00;                             /* DSCP/ECN */
    u16 tl = be16((u16)ip_total);  k_memcpy(ip + 2, &tl, 2); /* total length */
    u16 idf = be16(id);            k_memcpy(ip + 4, &idf, 2);/* identification */
    ip[6]  = 0x00; ip[7] = 0x00;               /* flags + fragment offset = 0 */
    ip[8]  = 64;                               /* TTL */
    ip[9]  = IPPROTO_ICMP;                     /* protocol */
    ip[10] = 0x00; ip[11] = 0x00;              /* checksum (zero for calc) */
    u32 src = be32(info->ip);      k_memcpy(ip + 12, &src, 4);
    u32 dst = be32(dst_ip);        k_memcpy(ip + 16, &dst, 4);
    u16 ipck = inet_csum(ip, 20);  k_memcpy(ip + 10, &ipck, 2); /* IP checksum */

    /* --- ICMP echo request --- */
    u8 *ic = ip + 20;
    ic[0] = ICMP_ECHO;                         /* type 8 */
    ic[1] = 0x00;                              /* code 0 */
    ic[2] = 0x00; ic[3] = 0x00;               /* checksum (zero for calc) */
    u16 idbe = be16(id);   k_memcpy(ic + 4, &idbe, 2);  /* identifier */
    u16 sqbe = be16(seq);  k_memcpy(ic + 6, &sqbe, 2);  /* sequence   */
    k_memcpy(ic + 8, echo_payload, ECHO_PAYLOAD_LEN);
    u16 icck = inet_csum(ic, icmp_len); k_memcpy(ic + 2, &icck, 2); /* ICMP csum */

    long sent = sc(SYS_NET_SEND, (long)frame, (long)frame_len, 0, 0, 0);
    if (sent < 0) {
        print("[PING] send failed rc="); print_dec((unsigned long)(-sent)); print("\n");
        return -1;
    }

    /* Bounded poll for the matching ICMP echo reply. */
    u8 rxbuf[1600];
    for (long tries = 0; tries < RX_TRIES_ECHO; tries++) {
        long n = sc(SYS_NET_RECV, (long)rxbuf, (long)sizeof(rxbuf), 0, 0, 0);
        if (n <= 0) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        if (n < ETH_HLEN + 20 + 8) continue;

        if (rd_be16(rxbuf + 12) != ETH_P_IP) continue;

        u8 *rip = rxbuf + ETH_HLEN;
        if ((rip[0] >> 4) != 4) continue;                 /* IPv4 */
        u32 ihl = (u32)(rip[0] & 0x0F) * 4;
        if (ihl < 20) continue;
        if (rip[9] != IPPROTO_ICMP) continue;             /* ICMP */
        if (rd_be32(rip + 12) != dst_ip) continue;        /* from our target */
        if ((unsigned long)n < (unsigned long)(ETH_HLEN + ihl + 8)) continue;

        u8 *ricmp = rip + ihl;
        if (ricmp[0] != ICMP_ECHOREP) continue;           /* type 0 reply */
        if (ricmp[1] != 0) continue;                      /* code 0 */
        if (rd_be16(ricmp + 4) != id) continue;           /* our id */
        if (rd_be16(ricmp + 6) != seq) continue;          /* our seq */

        print("[PING] reply from "); print_ip(dst_ip);
        print(" seq="); print_dec(seq); print("\n");
        return 1;
    }

    print("[PING] timeout seq="); print_dec(seq); print("\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * Argument parsing helpers (no libc).
 * --------------------------------------------------------------------- */
/* Parse a non-negative decimal integer; returns -1 on bad input. */
static long parse_uint(const char *s) {
    if (!s || !*s) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > 1000000) return 1000000;   /* clamp absurd counts */
    }
    return v;
}

/*
 * Run `count` echoes against dst_ip (host order). Returns the number of
 * replies received, or -1 if the very first transmit/build path errors
 * (used to distinguish FAIL from a quiet network in the self-test).
 */
static long do_pings(const net_info_t *info, const u8 gw_mac[6],
                     u32 dst_ip, long count) {
    long received = 0;
    int hard_err = 0;
    for (long i = 0; i < count; i++) {
        int r = ping_once(info, gw_mac, dst_ip, /*id*/0x4242, /*seq*/(u16)(i + 1));
        if (r == 1) received++;
        else if (r < 0 && i == 0) hard_err = 1;   /* TX path broke immediately */
    }
    if (hard_err && received == 0) return -1;
    return received;
}

int main(int argc, char **argv) {
    print("[PING] starting\n");

    /* 1. Query NIC info (MAC + IP + gateway). */
    net_info_t info;
    k_memset(&info, 0, sizeof(info));
    long ir = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0);

    int selftest = (argc <= 1);

    /* Link-down detection: SYS_NET_INFO error OR a zero MAC/IP. */
    int link_down = (ir != 0);
    if (!link_down) {
        int mac_zero = 1;
        for (int i = 0; i < 6; i++) if (info.mac[i]) { mac_zero = 0; break; }
        if (mac_zero || info.ip == 0) link_down = 1;
    }

    if (link_down) {
        if (selftest) {
            print("PING SELFTEST: SKIP (no link)\n");
            return 0;                       /* deterministic on no-NIC boots */
        }
        print("[PING] error: network link is down\n");
        return 1;
    }

    print("[PING] MAC "); print_mac(info.mac);
    print(" IP ");  print_ip(info.ip);
    print(" GW ");  print_ip(info.gateway);
    print("\n");

    /* 2. Determine the target IP. */
    u32 gw = info.gateway ? info.gateway : DEFAULT_GW;
    u32 target;
    long count;

    if (selftest) {
        target = gw;                        /* self-test pings the gateway   */
        count  = 2;                         /* once or twice                  */
    } else {
        unsigned int resolved = 0;
        if (dns_resolve(argv[1], &resolved) != 0 || resolved == 0) {
            print("[PING] could not resolve host: "); print(argv[1]); print("\n");
            return 1;
        }
        target = resolved;                  /* dns_resolve gives host order   */
        count  = (argc >= 3) ? parse_uint(argv[2]) : 4;
        if (count < 0) count = 4;           /* bad count -> default           */
        if (count == 0) count = 1;
    }

    print("[PING] target "); print_ip(target);
    print(" count "); print_dec((unsigned long)count); print("\n");

    /*
     * 3. Resolve next-hop MAC via ARP. On QEMU slirp everything routes via
     *    the gateway MAC, so ARP for the gateway and use that for all TX.
     *    (If the target happens to be the gateway, this is also exactly right.)
     */
    u8 gw_mac[6];
    if (!arp_resolve(&info, gw, gw_mac)) {
        if (selftest) {
            /* Link is up but we never learned the next hop: the TX/RX path is
             * not delivering -- that's a genuine self-test failure. */
            print("PING SELFTEST: FAIL\n");
            return 1;
        }
        print("[PING] failed to resolve next-hop MAC\n");
        return 1;
    }

    /* 4. Fire the echoes and tally. */
    long received = do_pings(&info, gw_mac, target, count);

    if (selftest) {
        if (received < 0) {
            print("PING SELFTEST: FAIL\n");     /* link up but TX/build errored */
            return 1;
        }
        if (received > 0) {
            print("PING SELFTEST: PASS\n");
            return 0;
        }
        /* Link up, frames built + sent cleanly, but no echo reply came back.
         * Keep this deterministic-ish: a NIC that ARP-replied but drops ICMP
         * is unusual; report FAIL so a broken path is visible, but the common
         * QEMU-slirp gateway answers ICMP -> PASS above. */
        print("PING SELFTEST: FAIL\n");
        return 1;
    }

    /* Normal-mode summary. */
    print("[PING] "); print_dec((unsigned long)count);
    print(" transmitted, "); print_dec((unsigned long)received);
    print(" received\n");

    return 0;
}
