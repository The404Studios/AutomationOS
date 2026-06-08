/*
 * autodhcp.c -- Auto-DHCP on boot (freestanding, ring 3).
 * ========================================================
 *
 * Spawned by init early in the boot sequence.  Waits for the NIC to
 * auto-negotiate link, then runs a full DHCP handshake and applies the
 * lease to the kernel.  Runs entirely in the background -- init does NOT
 * wait for it.  If DHCP fails (no server, no NIC, no link), it prints a
 * one-line message and exits silently; the user can always run "dhcpc run"
 * manually later.
 *
 * Flow:
 *   1. Sleep 2 seconds (let the NIC finish PHY auto-negotiation).
 *   2. Call SYS_NET_INFO -- if no NIC or link is down, exit quietly.
 *   3. Run dhcp_acquire() for the full DISCOVER->OFFER->REQUEST->ACK.
 *   4. On success, apply the lease via SYS_NET_CONFIG.
 *   5. Exit 0.
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.
 * Inline syscalls + fixed buffers + own helpers only.
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/autodhcp/autodhcp.c -o autodhcp.o
 *   objdump -d autodhcp.o | grep fs:0x28   # MUST be empty
 *
 * Link with crt0.o + dhcp.o:
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       userspace/crt0.o autodhcp.o dhcp.o -o autodhcp.elf
 */

#include "../../lib/net/dhcp.h"

/* -------------------------------------------------------------------------
 * Syscall numbers (must match kernel/include/syscall.h).
 * ----------------------------------------------------------------------- */
#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_SLEEP      9
#define SYS_NET_INFO   59
#define SYS_NET_CONFIG 89

/* 6-argument inline syscall: rdi/rsi/rdx/r10/r8/r9. */
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

/* -------------------------------------------------------------------------
 * Tiny freestanding helpers.
 * ----------------------------------------------------------------------- */

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

static void print_dec(unsigned int v)
{
    char buf[12];
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

/* -------------------------------------------------------------------------
 * net_info_t mirror (must match kernel's uapi_net_info_t / 80 bytes).
 * ----------------------------------------------------------------------- */
typedef struct {
    char          ifname[16];
    unsigned char mac[6];
    unsigned char _pad[2];
    unsigned int  ip;
    unsigned int  netmask;
    unsigned int  gateway;
    unsigned int  dns;
    unsigned char up;
    unsigned char dhcp;
    unsigned char _reserved[6];
    unsigned long long tx_packets;
    unsigned long long rx_packets;
    unsigned long long tx_bytes;
    unsigned long long rx_bytes;
} autodhcp_net_info_t;

/* -------------------------------------------------------------------------
 * Entry point (crt0 calls main(argc, argv)).
 * ----------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * Step 1: Wait 2 seconds for the NIC PHY to auto-negotiate link.
     * On real hardware (e.g. the T410's 82577LM) the PHY needs ~1-2s after
     * driver init before link_up settles.  On QEMU/virtio it is instant,
     * but the sleep is harmless (this runs in the background).
     */
    sc(SYS_SLEEP, 2000, 0, 0, 0, 0, 0);

    /*
     * Step 2: Probe SYS_NET_INFO -- is there a NIC?  Is link up?
     */
    autodhcp_net_info_t info;
    {
        unsigned char *p = (unsigned char *)&info;
        for (unsigned long i = 0; i < sizeof(info); i++) p[i] = 0;
    }

    long rc = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0, 0);
    if (rc < 0) {
        /* No NIC driver wired -- nothing to do. */
        print("[AUTODHCP] no NIC detected, skipping\n");
        return 0;
    }

    if (!info.up) {
        print("[AUTODHCP] NIC present but link is down, skipping\n");
        return 0;
    }

    /* Already have a non-zero IP?  Skip (user set a static config). */
    if (info.ip != 0) {
        print("[AUTODHCP] NIC already has IP ");
        print_ip(info.ip);
        print(", skipping DHCP\n");
        return 0;
    }

    /*
     * Step 3: Link is up and no IP configured -- run DHCP.
     */
    print("[AUTODHCP] link up on ");
    print(info.ifname);
    print(", requesting DHCP lease...\n");

    dhcp_lease_t lease;
    {
        unsigned char *p = (unsigned char *)&lease;
        for (unsigned long i = 0; i < sizeof(lease); i++) p[i] = 0;
    }

    int dhrc = dhcp_acquire(&lease);
    if (dhrc != DHCP_OK) {
        print("[AUTODHCP] DHCP failed (no server responded), skipping\n");
        return 1;
    }

    /*
     * Step 4: Print the lease and apply it.
     */
    print("[AUTODHCP] lease: ip ");
    print_ip(lease.ip);
    print("  mask ");
    print_ip(lease.netmask);
    print("  gw ");
    print_ip(lease.gateway);
    print("  dns ");
    print_ip(lease.dns);
    print("  lease ");
    print_dec(lease.lease_secs);
    print("s\n");

    /* Apply via SYS_NET_CONFIG (must match uapi_net_config_t layout). */
    struct {
        char         ifname[16];
        unsigned int ip;
        unsigned int netmask;
        unsigned int gateway;
        unsigned int dns;
        unsigned int flags;
    } cfg;
    {
        unsigned char *cp = (unsigned char *)&cfg;
        for (unsigned long i = 0; i < sizeof(cfg); i++) cp[i] = 0;
    }
    cfg.ifname[0] = 'e'; cfg.ifname[1] = 't';
    cfg.ifname[2] = 'h'; cfg.ifname[3] = '0';
    cfg.ip      = lease.ip;
    cfg.netmask = lease.netmask;
    cfg.gateway = lease.gateway;
    cfg.dns     = lease.dns;
    cfg.flags   = 0;

    long cfgrc = sc(SYS_NET_CONFIG, (long)&cfg, 0, 0, 0, 0, 0);
    if (cfgrc == 0) {
        print("[AUTODHCP] lease applied successfully\n");
    } else {
        print("[AUTODHCP] WARNING: could not apply lease to kernel\n");
    }

    return 0;
}
