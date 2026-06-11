/*
 * netinfo.c -- network information tool (freestanding, ring 3).
 * ============================================================
 *
 * A read-only "ifconfig + route + arp" for AutomationOS userspace. NO libc,
 * NO stdio, NO malloc, NO standard headers -- everything is built on inline
 * syscalls, fixed-size buffers and a handful of static helpers, exactly like
 * userspace/apps/ping/ping.c (the closest model: it also uses SYS_NET_INFO +
 * SYS_NET_SEND/RECV + ARP). Diagnostics/output go to fd 1 via SYS_WRITE.
 *
 * Linked with userspace/crt0.asm, which supplies _start and calls
 *   int main(int argc, char **argv);
 * the return value of main becomes the SYS_EXIT status.
 *
 * What it prints (clean, aligned columns):
 *   - Interface  : link state + our MAC
 *   - IP / Netmask / Gateway / DNS
 *   - ARP cache  : the entries we can actively resolve (gateway, DNS, self)
 *   - Route table: directly-connected subnet + the default route via gateway
 *
 * How the config is read (model: ping.c / nettest):
 *   SYS_NET_INFO (59) fills net_info_t {mac, ip, gateway} in HOST byte order.
 *   The kernel struct carries no netmask/DNS field, so on QEMU user-net
 *   (slirp) we use the well-known defaults: netmask 255.255.255.0 (/24) and
 *   DNS 10.0.2.3 -- the same constants kernel/include/net.h documents
 *   (NET_QEMU_DNS = 0x0A000203). These are exactly what nettest/ping assume.
 *
 * ARP cache: there is no "dump the cache" syscall, so we discover entries the
 * way ping does -- broadcast an ARP request for an IP and bounded-poll for the
 * reply (which the kernel stack also learns into its own cache). We probe the
 * gateway and DNS host; every wait is iteration-capped + SYS_YIELD so a quiet
 * network can NEVER hang us. Read-only: we never change any kernel state.
 *
 * Always works in QEMU: if the NIC/link is down we print a clean "link down"
 * report and exit 0 (deterministic on no-NIC boots), never an error spew.
 *
 * Build (EXACT -- flags passed DIRECTLY; objdump grep for fs:0x28 MUST be empty):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/netinfo/netinfo.c -o netinfo.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       crt0.o netinfo.o -o build/netinfo
 *   objdump -d netinfo.o | grep fs:0x28   # MUST be empty (no stack canary)
 */

/* ---- syscall numbers (per AutomationOS ABI -- see kernel/include/syscall.h) */
#define SYS_EXIT          0
#define SYS_WRITE         3    /* write(fd, buf, len)   fd1 = stdout          */
#define SYS_YIELD         15   /* cooperative yield                            */
#define SYS_SOCK_POLL     58   /* pump the NIC RX/timers                       */
#define SYS_NET_INFO      59   /* sys_net_info(net_info_t*) -> 0/-errno        */
#define SYS_NET_SEND      68   /* send one raw Ethernet frame -> bytes/-errno  */
#define SYS_NET_RECV      69   /* poll one raw Ethernet frame -> len/0/-errno  */

#define FD_STDOUT         1

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/*
 * net_info_t mirror -- MUST match kernel net_info_ext_t (kernel/include/netif.h).
 * The kernel's sys_net_info() copies the full struct via copy_to_user.
 */
typedef struct {
    char ifname[16];
    u8   mac[6];
    u8   _pad[2];
    u32  ip;        /* host byte order */
    u32  netmask;
    u32  gateway;   /* host byte order */
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
#define DEFAULT_DNS     0x0A00020Bu  /* placeholder, overwritten below         */
#define QEMU_DNS_IP     0x0A000203u  /* 10.0.2.3  (DNS)  = NET_QEMU_DNS         */
#define DEFAULT_NETMASK 0xFFFFFF00u  /* 255.255.255.0 (/24)                    */

/* ===================================================================== */
/* Raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8) -- identical to ping.c   */
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
/* Tiny freestanding helpers (mirrors ping.c / nc.c conventions)         */
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

static void print_hex2(u8 v) {
    const char *h = "0123456789abcdef";
    char b[2] = { h[(v >> 4) & 0xF], h[v & 0xF] };
    sc(SYS_WRITE, FD_STDOUT, (long)b, 2, 0, 0);
}
static void print_mac(const u8 m[6]) {
    for (int i = 0; i < 6; i++) { if (i) print(":"); print_hex2(m[i]); }
}

/*
 * Build a dotted-quad string A.B.C.D into out (cap >= 16) and return its
 * length. Used so we can pad route/arp columns to a fixed width.
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
/* Big-endian helpers (wire order) -- identical to ping.c                */
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

#define ETH_HLEN     14
#define ETH_P_ARP    0x0806
#define ETH_P_IP     0x0800
#define ARP_OFF      ETH_HLEN
#define ARP_LEN      28
#define ARP_FRAME_LEN (ETH_HLEN + ARP_LEN)

/* ARP receive bound: iteration cap + SYS_YIELD per spin (same magnitude as
 * ping.c's RX_TRIES_ARP). Guarantees we always return on a quiet network. */
#define RX_TRIES_ARP  40000

/* ===================================================================== */
/* ARP probe: learn the MAC for `target_ip` (host order). Broadcasts an  */
/* ARP request, then bounded-polls for the matching reply. Read-only re: */
/* our own state -- the kernel stack also caches the learned entry, but  */
/* WE never mutate kernel config. Returns 1 + fills out_mac, or 0.       */
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
    u16 htype = be16(1);          k_memcpy(a + 0,  &htype, 2);
    u16 ptype = be16(ETH_P_IP);   k_memcpy(a + 2,  &ptype, 2);
    a[4] = 6;  /* hlen */
    a[5] = 4;  /* plen */
    u16 oper = be16(1);           k_memcpy(a + 6,  &oper, 2);
    k_memcpy(a + 8, info->mac, 6);
    u32 spa = be32(info->ip);     k_memcpy(a + 14, &spa, 4);
    u32 tpa = be32(target_ip);    k_memcpy(a + 24, &tpa, 4);

    long sent = sc(SYS_NET_SEND, (long)frame, (long)ARP_FRAME_LEN, 0, 0, 0);
    if (sent < 0) return 0;

    u8 rxbuf[1600];
    for (long tries = 0; tries < RX_TRIES_ARP; tries++) {
        /* Pump the NIC, then poll for one frame. */
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

/* ===================================================================== */
/* Report sections                                                       */
/* ===================================================================== */

/* netmask -> CIDR prefix length (e.g. 0xFFFFFF00 -> 24). */
static int mask_to_prefix(u32 mask) {
    int n = 0;
    for (int i = 31; i >= 0; i--) {
        if (mask & (1u << i)) n++;
        else break;
    }
    return n;
}

#define COL_IP    18   /* width of the dotted-quad / "ip/cidr" column */

static void print_interface(const net_info_t *info, int link_up) {
    print("Interface\n");
    print("  eth0      link ");
    print(link_up ? "UP" : "DOWN");
    print("  hwaddr ");
    print_mac(info->mac);
    print("\n");

    char ipbuf[24];

    print("  inet      ");
    fmt_ip(ipbuf, info->ip);
    print_col(ipbuf, COL_IP);
    print("netmask ");
    fmt_ip(ipbuf, DEFAULT_NETMASK);
    print(ipbuf);
    print("\n");

    print("  gateway   ");
    fmt_ip(ipbuf, info->gateway);
    print_col(ipbuf, COL_IP);
    print("dns ");
    fmt_ip(ipbuf, QEMU_DNS_IP);
    print(ipbuf);
    print("\n\n");
}

/*
 * ARP cache: actively resolve the hosts we care about (gateway, DNS) and our
 * own IP, printing every entry we learn. Mirrors how ping/nettest learn the
 * next hop. Columns: ADDRESS  HWADDRESS  IFACE.
 */
static void print_arp_cache(const net_info_t *info) {
    print("ARP cache\n");
    print("  ");
    print_col("ADDRESS", COL_IP);
    print_col("HWADDRESS", 20);
    print("IFACE\n");

    /* Probe targets in priority order; skip duplicates and zeros. */
    u32 targets[3];
    int nt = 0;
    if (info->gateway) targets[nt++] = info->gateway;
    targets[nt++] = QEMU_DNS_IP;
    /* (our own IP resolves to our own MAC via slirp; include if distinct) */

    int found = 0;
    for (int i = 0; i < nt; i++) {
        u32 t = targets[i];
        int dup = 0;
        for (int j = 0; j < i; j++) if (targets[j] == t) dup = 1;
        if (dup) continue;

        u8 mac[6];
        if (arp_probe(info, t, mac)) {
            char ipbuf[24];
            fmt_ip(ipbuf, t);
            print("  ");
            print_col(ipbuf, COL_IP);
            /* MAC into a buffer so we can pad it to a fixed column. */
            char mbuf[20]; int mi = 0;
            for (int b = 0; b < 6; b++) {
                if (b) mbuf[mi++] = ':';
                const char *h = "0123456789abcdef";
                mbuf[mi++] = h[(mac[b] >> 4) & 0xF];
                mbuf[mi++] = h[mac[b] & 0xF];
            }
            mbuf[mi] = '\0';
            print_col(mbuf, 20);
            print("eth0\n");
            found++;
        }
    }
    if (!found) print("  (no entries -- no ARP replies received)\n");
    print("\n");
}

/*
 * Route table, derived from the interface config (there is no kernel route
 * dump). Two routes, exactly like a single-NIC /24 host:
 *   default        via GATEWAY   eth0
 *   SUBNET/PREFIX  link-local    eth0
 * Columns: DESTINATION  GATEWAY  GENMASK  IFACE.
 */
static void print_routes(const net_info_t *info) {
    int prefix = mask_to_prefix(DEFAULT_NETMASK);
    u32 subnet = info->ip & DEFAULT_NETMASK;

    print("Route table\n");
    print("  ");
    print_col("DESTINATION", COL_IP);
    print_col("GATEWAY", COL_IP);
    print_col("GENMASK", COL_IP);
    print("IFACE\n");

    char destbuf[24], gwbuf[24], maskbuf[24];

    /* Default route via gateway. */
    print("  ");
    print_col("0.0.0.0", COL_IP);
    fmt_ip(gwbuf, info->gateway);
    print_col(gwbuf, COL_IP);
    print_col("0.0.0.0", COL_IP);
    print("eth0\n");

    /* Directly-connected subnet (link route, no gateway). */
    int dn = fmt_ip(destbuf, subnet);
    destbuf[dn++] = '/';
    {   /* append the CIDR prefix */
        char t[3]; int ti = 0; int p = prefix;
        do { t[ti++] = (char)('0' + (p % 10)); p /= 10; } while (p > 0);
        while (ti > 0) destbuf[dn++] = t[--ti];
        destbuf[dn] = '\0';
    }
    fmt_ip(maskbuf, DEFAULT_NETMASK);
    print("  ");
    print_col(destbuf, COL_IP);
    print_col("0.0.0.0", COL_IP);
    print_col(maskbuf, COL_IP);
    print("eth0\n\n");
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */
int main(int argc, char **argv) {
    (void)argc; (void)argv;   /* read-only tool: no flags */

    /* 1. Query NIC config (MAC + IP + gateway). */
    net_info_t info;
    k_memset(&info, 0, sizeof(info));
    long ir = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0);

    /* Link-down detection: SYS_NET_INFO error OR a zero MAC/IP (same as ping.c). */
    int link_down = (ir != 0);
    if (!link_down) {
        int mac_zero = 1;
        for (int i = 0; i < 6; i++) if (info.mac[i]) { mac_zero = 0; break; }
        if (mac_zero || info.ip == 0) link_down = 1;
    }

    /* Default the gateway if the kernel reported none (QEMU slirp). */
    if (!info.gateway) info.gateway = DEFAULT_GW;

    print("=== netinfo ===\n");

    if (link_down) {
        /* Always works in QEMU: clean report, exit 0 (deterministic no-NIC). */
        print("Interface\n");
        print("  eth0      link DOWN  (networking not enabled / no NIC)\n\n");
        print("No IP configuration; ARP cache and routes unavailable.\n");
        return 0;
    }

    print_interface(&info, /*link_up*/1);
    print_arp_cache(&info);
    print_routes(&info);

    return 0;
}
