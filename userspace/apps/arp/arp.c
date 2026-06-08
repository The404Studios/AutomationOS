/*
 * arp.c -- userspace ARP cache show / probe tool (freestanding, ring 3).
 * ======================================================================
 *
 * A from-scratch `arp` for AutomationOS userspace, modelled directly off
 * ping/ping.c (ARP frame build + reply parse) and netinfo/netinfo.c (the
 * "actively probe the hosts we care about" cache-discovery trick). Completely
 * freestanding: NO libc, NO stdio, NO malloc, NO standard headers -- every I/O
 * op is an inline `syscall`, every buffer is a fixed-size local, and all
 * string/number formatting is done by the small static helpers below. The
 * syscall ABI, the 6-arg inline-syscall macro, argv handling and the
 * print/itoa helpers are copied VERBATIM from the model net tools
 * (nc/nc.c, ping/ping.c, wget/wget.c, netinfo/netinfo.c, netscan/netscan.c) --
 * nothing here is invented.
 *
 * Linked with userspace/crt0.asm, which supplies _start and calls
 *   int main(int argc, char **argv);
 * the return value of main becomes the SYS_EXIT status.
 *
 * Usage:
 *   arp                 Show the ARP table the kernel has learned. There is no
 *                       "dump the cache" syscall, so -- exactly like netinfo --
 *                       we DISCOVER entries by broadcasting an ARP request for
 *                       the well-known hosts (gateway + DNS) and printing the
 *                       replies (the kernel stack also caches what we learn).
 *
 *   arp <ip|host>       Resolve ONE address: broadcast an ARP who-has for it and
 *                       print the learned MAC, or "no reply" on timeout. A
 *                       dotted-quad is parsed locally with NO network query; a
 *                       name triggers a bounded UDP DNS lookup (dns_resolve).
 *
 * Read-only: we NEVER mutate kernel network state (no add/delete). Every
 * receive loop is bounded by an iteration cap + SYS_YIELD (and a SYS_SOCK_POLL
 * pump per spin, like netinfo) so a quiet network can NEVER hang us.
 *
 * All multi-byte IP/ARP fields are BIG-ENDIAN on the wire. Addresses handed to
 * us by SYS_NET_INFO / dns_resolve are in HOST byte order (0x0A000202 ==
 * 10.0.2.2); be32()/be16() convert to wire order on send, rd_be32()/rd_be16()
 * convert back on parse.
 *
 * Build (EXACT -- flags passed DIRECTLY; objdump grep for fs:0x28 MUST be empty):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/arp/arp.c -o arp.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dns.c -o dns.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       crt0.o arp.o dns.o -o build/arp
 *   objdump -d build/arp | grep fs:0x28   # MUST be empty (no stack canary)
 */

#include "../../lib/net/dns.h"   /* int dns_resolve(const char*, unsigned int*); */

/* ---- syscall numbers (per AutomationOS ABI -- see kernel/include/syscall.h) */
#define SYS_EXIT          0
#define SYS_WRITE         3    /* write(fd, buf, len)   fd1 = stdout            */
#define SYS_YIELD         15   /* cooperative yield                             */
#define SYS_SOCK_POLL     58   /* pump the NIC RX/timers                        */
#define SYS_NET_INFO      59   /* sys_net_info(net_info_t*) -> 0/-errno         */
#define SYS_NET_SEND      68   /* send one raw Ethernet frame -> bytes/-errno   */
#define SYS_NET_RECV      69   /* poll one raw Ethernet frame -> len/0/-errno   */

#define FD_STDOUT         1

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/*
 * net_info_t mirror -- MUST match kernel net_info_ext_t (kernel/include/netif.h).
 */
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

/* ---- QEMU user-net (slirp) well-known values (kernel/include/net.h) ------- */
#define DEFAULT_GW      0x0A000202u  /* 10.0.2.2  (host/gateway)               */
#define QEMU_DNS_IP     0x0A000203u  /* 10.0.2.3  (DNS) = NET_QEMU_DNS         */

/* ===================================================================== */
/* Raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8) -- verbatim from ping.c  */
/* ===================================================================== */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* ===================================================================== */
/* Tiny freestanding helpers (mirrors ping.c / netinfo.c conventions)    */
/* ===================================================================== */
static unsigned long k_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void *k_memset(void *d, int c, unsigned long n) {
    u8 *p = (u8 *)d; while (n--) *p++ = (u8)c; return d;
}
static void *k_memcpy(void *d, const void *s, unsigned long n) {
    u8 *dp = (u8 *)d; const u8 *sp = (const u8 *)s; while (n--) *dp++ = *sp++; return d;
}

/* ---- stdout (fd1) print helpers ---- */
static void print(const char *m) { sc(SYS_WRITE, FD_STDOUT, (long)m, (long)k_strlen(m), 0, 0); }

static void print_dec(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    char rev[24]; int j = 0;
    while (i > 0) rev[j++] = b[--i];
    sc(SYS_WRITE, FD_STDOUT, (long)rev, (long)j, 0, 0);
}
static void print_hex2(u8 v) {
    const char *h = "0123456789abcdef";
    char b[2] = { h[(v >> 4) & 0xF], h[v & 0xF] };
    sc(SYS_WRITE, FD_STDOUT, (long)b, 2, 0, 0);
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

/*
 * Build a dotted-quad string A.B.C.D into out (cap >= 16) and return its
 * length, so we can pad the ADDRESS column to a fixed width (mirrors
 * netinfo.c's fmt_ip).
 */
static int fmt_ip(char *out, u32 ip) {
    int n = 0;
    u32 parts[4] = { (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF };
    for (int k = 0; k < 4; k++) {
        if (k) out[n++] = '.';
        u32 v = parts[k];
        char t[3]; int ti = 0;
        do { t[ti++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
        while (ti > 0) out[n++] = t[--ti];
    }
    out[n] = '\0';
    return n;
}
/* Print a string then pad with spaces out to `width` columns (left-justify). */
static void print_col(const char *s, int width) {
    int len = (int)k_strlen(s);
    print(s);
    for (int i = len; i < width; i++) print(" ");
}

/* ===================================================================== */
/* Big-endian helpers (wire order) -- identical to ping.c / netinfo.c    */
/* ===================================================================== */
static u16 be16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static u32 be32(u32 x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static u16 rd_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

#define ETH_HLEN      14
#define ETH_P_ARP     0x0806
#define ETH_P_IP      0x0800
#define ARP_OFF       ETH_HLEN
#define ARP_LEN       28
#define ARP_FRAME_LEN (ETH_HLEN + ARP_LEN)

/* ARP receive bound: iteration cap + SYS_YIELD per spin (same magnitude as
 * ping.c's RX_TRIES_ARP). Guarantees we always return on a quiet network. */
#define RX_TRIES_ARP  40000

#define COL_IP   18   /* width of the dotted-quad ADDRESS column */
#define COL_MAC  20   /* width of the HWADDRESS column           */

/* ===================================================================== */
/* ARP probe: learn the MAC for `target_ip` (host order). Broadcasts an  */
/* ARP request, then bounded-polls for the matching reply. Read-only re: */
/* OUR own state -- the kernel stack also caches the learned entry, but  */
/* WE never mutate kernel config. Returns 1 + fills out_mac, or 0.       */
/* Frame build + reply parse are taken straight from ping.c/netinfo.c.   */
/* ===================================================================== */
static int arp_probe(const net_info_t *info, u32 target_ip, u8 out_mac[6]) {
    u8 frame[64];
    k_memset(frame, 0, sizeof(frame));

    /* Ethernet header: dst = broadcast, src = our MAC, type = ARP. */
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    k_memcpy(frame + 6, info->mac, 6);
    u16 et = be16(ETH_P_ARP);
    k_memcpy(frame + 12, &et, 2);

    /* ARP payload (Ethernet/IPv4 request). */
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

    long sent = sc(SYS_NET_SEND, (long)frame, (long)ARP_FRAME_LEN, 0, 0, 0);
    if (sent < 0) return 0;

    u8 rxbuf[1600];
    for (long tries = 0; tries < RX_TRIES_ARP; tries++) {
        /* Pump the NIC, then poll for one frame (netinfo.c ordering). */
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
        long n = sc(SYS_NET_RECV, (long)rxbuf, (long)sizeof(rxbuf), 0, 0, 0);
        if (n <= 0) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        if (n < ETH_HLEN + ARP_LEN) continue;

        if (rd_be16(rxbuf + 12) != ETH_P_ARP) continue;
        if (rd_be16(rxbuf + ARP_OFF + 6) != 2) continue;          /* reply */
        if (rd_be32(rxbuf + ARP_OFF + 14) != target_ip) continue; /* ours  */

        k_memcpy(out_mac, rxbuf + ARP_OFF + 8, 6);                /* its MAC */
        return 1;
    }
    return 0;
}

/* Print one learned "ADDRESS  HWADDRESS  IFACE" row (netinfo column layout). */
static void print_row(u32 ip, const u8 mac[6]) {
    char ipbuf[24];
    fmt_ip(ipbuf, ip);
    print("  ");
    print_col(ipbuf, COL_IP);

    /* MAC into a buffer so we can pad it to a fixed column. */
    char mbuf[20]; int mi = 0;
    const char *h = "0123456789abcdef";
    for (int b = 0; b < 6; b++) {
        if (b) mbuf[mi++] = ':';
        mbuf[mi++] = h[(mac[b] >> 4) & 0xF];
        mbuf[mi++] = h[mac[b] & 0xF];
    }
    mbuf[mi] = '\0';
    print_col(mbuf, COL_MAC);
    print("eth0\n");
}

/* ===================================================================== */
/* Link-down detection (identical logic to ping.c / netinfo.c).          */
/* Returns 1 if the link is down (no NIC / not initialised), else 0.     */
/* ===================================================================== */
static int link_is_down(long ir, const net_info_t *info) {
    if (ir != 0) return 1;
    int mac_zero = 1;
    for (int i = 0; i < 6; i++) if (info->mac[i]) { mac_zero = 0; break; }
    if (mac_zero || info->ip == 0) return 1;
    return 0;
}

/* ===================================================================== */
/* `arp` (no args): show the learned ARP table.                          */
/*                                                                       */
/* No kernel cache-dump syscall exists, so -- exactly like netinfo -- we */
/* actively resolve the hosts we care about (gateway + DNS) and print    */
/* every reply. The kernel stack caches what we learn, so this reflects  */
/* the table the kernel now holds. Deterministic + bounded; on a quiet   */
/* network it prints "(no entries)" and exits 0.                         */
/* ===================================================================== */
static int do_show(const net_info_t *info) {
    print("ARP cache\n");
    print("  ");
    print_col("ADDRESS", COL_IP);
    print_col("HWADDRESS", COL_MAC);
    print("IFACE\n");

    /* Probe targets in priority order; skip duplicates and zeros. */
    u32 targets[2];
    int nt = 0;
    if (info->gateway) targets[nt++] = info->gateway;
    targets[nt++] = QEMU_DNS_IP;

    int found = 0;
    for (int i = 0; i < nt; i++) {
        u32 t = targets[i];
        int dup = 0;
        for (int j = 0; j < i; j++) if (targets[j] == t) dup = 1;
        if (dup) continue;

        u8 mac[6];
        if (arp_probe(info, t, mac)) {
            print_row(t, mac);
            found++;
        }
    }
    if (!found) print("  (no entries -- no ARP replies received)\n");
    return 0;
}

/* ===================================================================== */
/* `arp <ip|host>`: resolve ONE address and print its MAC (or no reply). */
/* Dotted-quad parses locally (no query); a name triggers dns_resolve.   */
/* Returns process exit code: 0 = resolved, 1 = no reply, 2 = bad host.  */
/* ===================================================================== */
static int do_resolve(const net_info_t *info, const char *host) {
    unsigned int target = 0;
    int dr = dns_resolve(host, &target);
    if (dr != 0 || target == 0) {
        print("arp: cannot resolve host '");
        print(host);
        print("' (dns rc=");
        print_dec((unsigned long)(dr < 0 ? -dr : dr));
        print(")\n");
        return 2;
    }

    print("arp: who-has ");
    print_ip(target);
    print(" ...\n");

    u8 mac[6];
    if (arp_probe(info, target, mac)) {
        print("  ");
        print_ip(target);
        print(" is-at ");
        print_mac(mac);
        print(" (eth0)\n");
        return 0;
    }

    print("  ");
    print_ip(target);
    print(" -- no reply\n");
    return 1;
}

/* ===================================================================== */
/* Entry point (crt0 supplies _start -> main(argc, argv)).               */
/* ===================================================================== */
int main(int argc, char **argv) {
    /* 1. Query NIC config (MAC + IP + gateway), same as ping.c/netinfo.c. */
    net_info_t info;
    k_memset(&info, 0, sizeof(info));
    long ir = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0);

    print("=== arp ===\n");

    if (link_is_down(ir, &info)) {
        /* Always works in QEMU: clean report, exit 0 (deterministic no-NIC). */
        print("  eth0 link DOWN (networking not enabled / no NIC)\n");
        print("  No IP configuration; ARP table unavailable.\n");
        return 0;
    }

    /* Default the gateway if the kernel reported none (QEMU slirp). */
    if (!info.gateway) info.gateway = DEFAULT_GW;

    print("  iface eth0  hwaddr ");
    print_mac(info.mac);
    print("  inet ");
    print_ip(info.ip);
    print("\n\n");

    /* No arg -> show the table; one arg -> resolve that IP/host. */
    if (argc <= 1)
        return do_show(&info);

    return do_resolve(&info, argv[1]);
}
