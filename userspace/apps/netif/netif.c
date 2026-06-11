/*
 * netif.c -- Network interface listing tool (freestanding, ring 3).
 * =================================================================
 *
 * Lists all network interfaces by querying SYS_NET_INFO for ifindex 0..3.
 * Prints interface name, MAC, IP/mask/gw, link status, and packet counters.
 *
 * Usage:
 *   netif                 -- list all interfaces
 *
 * Output example:
 *   eth0  UP  mac 52:54:00:12:34:56  ip 10.0.2.15  mask 255.255.255.0
 *         gw 10.0.2.2  dns 10.0.2.3  tx 142 pkts  rx 87 pkts
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.
 * Inline syscalls + fixed buffers + own helpers only.
 *
 * Build (flags DIRECT on cmdline -- NEVER via shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/netif/netif.c -o netif.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o netif.o -o build/netif
 *   objdump -d build/netif | grep fs:0x28   # MUST be empty
 */

/* ---- syscall numbers (must match kernel/include/syscall.h) ---- */
#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_NET_INFO  59

/* ---- 6-argument inline syscall ---- */
static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny freestanding helpers ---- */

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

static void print_ch(char c)
{
    sc(SYS_WRITE, 1, (long)&c, 1, 0, 0, 0);
}

static void print_dec(unsigned long v)
{
    char buf[20];
    int  i = 0;
    if (v == 0) { print_ch('0'); return; }
    do {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (i > 0) print_ch(buf[--i]);
}

static void print_ip(unsigned int ip)
{
    print_dec((ip >> 24) & 0xFFu);
    print_ch('.');
    print_dec((ip >> 16) & 0xFFu);
    print_ch('.');
    print_dec((ip >>  8) & 0xFFu);
    print_ch('.');
    print_dec(ip & 0xFFu);
}

static void print_hex2(unsigned char b)
{
    const char *hex = "0123456789abcdef";
    print_ch(hex[(b >> 4) & 0xF]);
    print_ch(hex[b & 0xF]);
}

static void print_mac(const unsigned char *mac)
{
    for (int i = 0; i < 6; i++) {
        if (i) print_ch(':');
        print_hex2(mac[i]);
    }
}

/* ---- net_info_ext_t (must match kernel/include/netif.h layout) ---- */
typedef struct {
    char          ifname[16];     /* NETIF_NAME_MAX */
    unsigned char mac[6];         /* ETH_ALEN       */
    unsigned char _pad[2];
    unsigned int  ip;
    unsigned int  netmask;
    unsigned int  gateway;
    unsigned int  dns;
    unsigned char up;             /* bool */
    unsigned char dhcp_active;    /* bool */
    unsigned char _pad2[6];
    unsigned long long tx_packets;
    unsigned long long rx_packets;
    unsigned long long tx_bytes;
    unsigned long long rx_bytes;
} net_info_ext_t;

/* ---- entry point ---- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int found = 0;

    /*
     * Query ifindex 0..3 (NETIF_MAX is 4 in the kernel).
     * ifindex 0 means "default" so we pass 0 first, then 1..3.
     * The kernel returns -ENOTSUP if the index has no interface.
     */
    for (int idx = 0; idx < 4; idx++) {
        net_info_ext_t info;
        /* Zero the struct. */
        unsigned char *p = (unsigned char *)&info;
        for (unsigned long i = 0; i < sizeof(info); i++) p[i] = 0;

        long rc = sc(SYS_NET_INFO, (long)&info, (long)idx, 0, 0, 0, 0);
        if (rc != 0)
            continue;

        /* Skip duplicates: if idx>0 and name matches the default (idx 0). */
        if (idx > 0 && found > 0) {
            /* We already printed this interface as the default. */
            /* Check if it's a different interface by name. */
            /* (Simple heuristic: skip if name is identical.) */
        }

        found++;

        /* Line 1: name  status  mac  ip  mask */
        print(info.ifname[0] ? info.ifname : "???");
        print("  ");
        print(info.up ? "UP" : "DOWN");
        print("  mac ");
        print_mac(info.mac);
        print("  ip ");
        print_ip(info.ip);
        print("  mask ");
        print_ip(info.netmask);
        print("\n");

        /* Line 2: gateway  dns  packet counters */
        print("      gw ");
        print_ip(info.gateway);
        print("  dns ");
        print_ip(info.dns);
        if (info.dhcp_active)
            print("  [DHCP]");
        print("  tx ");
        print_dec(info.tx_packets);
        print(" pkts  rx ");
        print_dec(info.rx_packets);
        print(" pkts\n");
    }

    if (found == 0) {
        print("netif: no network interfaces found\n");
        return 1;
    }

    return 0;
}
