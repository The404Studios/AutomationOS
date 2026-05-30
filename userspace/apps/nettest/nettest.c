/*
 * nettest.c -- networking smoke test (freestanding, ring 3).
 * ==========================================================
 *
 * Exercises the proposed SYS_NET_* syscalls:
 *   - SYS_NET_INFO : fetch NIC MAC + assigned IPv4 + gateway, print them.
 *   - SYS_NET_SEND : hand-build and transmit a broadcast ARP request asking
 *                    "who has 10.0.2.2?".
 *   - SYS_NET_RECV : poll for inbound frames, print the first few, and report
 *                    when the gateway's ARP reply (or any IPv4 frame) arrives.
 *
 * This is a pure userspace driver of the kernel net stack -- it does NO MMIO
 * itself. Diagnostics go to serial via SYS_WRITE(fd=1).
 *
 * Build (EXACT -- flags passed DIRECTLY; objdump grep for fs:0x28 MUST be empty):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/nettest/nettest.c -o nettest.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       nettest.o -o nettest
 *   objdump -d nettest | grep fs:0x28   # MUST be empty
 *
 * NOTE: these syscall numbers are PROPOSED. The integrator must add
 *   SYS_NET_SEND=41, SYS_NET_RECV=42, SYS_NET_INFO=43 and route them to
 *   sys_net_send / sys_net_recv / sys_net_info (kernel/net/netsyscall.c).
 */

/* ---- syscall numbers ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_NET_SEND      68   /* kernel/include/syscall.h */
#define SYS_NET_RECV      69
#define SYS_NET_INFO      59

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* net_info_t mirror (must match kernel/include/net.h). */
typedef struct {
    u8  mac[6];
    u8  _pad[2];
    u32 ip;        /* host byte order */
    u32 gateway;   /* host byte order */
} net_info_t;

/* 6-argument inline syscall (args rdi/rsi/rdx/r10/r8/r9). */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny serial diagnostics (fd 1) ---- */
static unsigned long k_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }
static void print_dec(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}
static void print_hex2(u8 v) {
    const char *h = "0123456789abcdef";
    char b[2] = { h[(v >> 4) & 0xF], h[v & 0xF] };
    sc(SYS_WRITE, 1, (long)b, 2, 0, 0, 0);
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

#define ETH_HLEN   14
#define ETH_P_ARP  0x0806
#define ETH_P_IP   0x0800

void _start(void) {
    print("[NETTEST] starting\n");

    /* 1. Query NIC info. */
    net_info_t info;
    k_memset(&info, 0, sizeof(info));
    long ir = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0, 0);
    if (ir != 0) {
        print("[NETTEST] SYS_NET_INFO failed rc="); print_dec((unsigned long)(-ir));
        print(" (is networking wired/up?)\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }
    print("[NETTEST] MAC "); print_mac(info.mac);
    print(" IP ");  print_ip(info.ip);
    print(" GW ");  print_ip(info.gateway);
    print("\n");

    /* 2. Build a broadcast ARP request: who-has gateway? */
    u8 frame[64];
    k_memset(frame, 0, sizeof(frame));

    /* Ethernet header: dst = broadcast, src = our MAC, type = ARP. */
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    k_memcpy(frame + 6, info.mac, 6);
    u16 et = be16(ETH_P_ARP);
    k_memcpy(frame + 12, &et, 2);

    /* ARP payload (Ethernet/IPv4). */
    u8 *a = frame + ETH_HLEN;
    u16 htype = be16(1);   k_memcpy(a + 0,  &htype, 2);   /* Ethernet      */
    u16 ptype = be16(ETH_P_IP); k_memcpy(a + 2, &ptype, 2);/* IPv4         */
    a[4] = 6;   /* hlen */
    a[5] = 4;   /* plen */
    u16 oper = be16(1);    k_memcpy(a + 6,  &oper, 2);     /* request      */
    k_memcpy(a + 8, info.mac, 6);                          /* sender HW    */
    u32 spa = be32(info.ip);      k_memcpy(a + 14, &spa, 4);/* sender IP   */
    /* a+18..23 target HW = 0 (unknown) */
    u32 tpa = be32(info.gateway); k_memcpy(a + 24, &tpa, 4);/* target IP   */

    u32 frame_len = ETH_HLEN + 28;   /* 14 + 28-byte ARP = 42 */

    print("[NETTEST] TX ARP who-has "); print_ip(info.gateway);
    print(" (len "); print_dec(frame_len); print(")\n");

    long sent = sc(SYS_NET_SEND, (long)frame, (long)frame_len, 0, 0, 0, 0);
    if (sent < 0) {
        print("[NETTEST] SYS_NET_SEND failed rc="); print_dec((unsigned long)(-sent));
        print("\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }
    print("[NETTEST] TX ok, "); print_dec((unsigned long)sent); print(" bytes\n");

    /* 3. Poll for inbound frames; report ARP replies and any IPv4. */
    u8 rxbuf[1600];
    int frames_seen = 0;
    int arp_replies = 0;
    /* Stop as soon as the gateway's ARP reply arrives (the success signal); else
     * cap the poll so a quiet network can't hang the boot self-test. */
    for (long tries = 0; tries < 40000 && arp_replies == 0 && frames_seen < 8; tries++) {
        long n = sc(SYS_NET_RECV, (long)rxbuf, (long)sizeof(rxbuf), 0, 0, 0, 0);
        if (n <= 0) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
            continue;
        }
        frames_seen++;

        u16 type = (u16)((rxbuf[12] << 8) | rxbuf[13]);
        print("[NETTEST] RX frame len="); print_dec((unsigned long)n);
        print(" from "); print_mac(rxbuf + 6);
        print(" type=0x"); print_hex2((u8)(type >> 8)); print_hex2((u8)type);

        if (type == ETH_P_ARP && n >= ETH_HLEN + 28) {
            u16 op = (u16)((rxbuf[ETH_HLEN + 6] << 8) | rxbuf[ETH_HLEN + 7]);
            if (op == 2) {
                arp_replies++;
                print(" [ARP REPLY: gateway is at ");
                print_mac(rxbuf + ETH_HLEN + 8);
                print("]");
            } else {
                print(" [ARP req]");
            }
        } else if (type == ETH_P_IP) {
            print(" [IPv4]");
        }
        print("\n");
    }

    print("[NETTEST] done: "); print_dec((unsigned long)frames_seen);
    print(" frame(s), "); print_dec((unsigned long)arp_replies);
    print(" ARP reply(ies)\n");

    if (arp_replies > 0) {
        print("[NETTEST] PASS: NIC TX + RX + ARP round-trip works\n");
    } else if (frames_seen > 0) {
        print("[NETTEST] PARTIAL: RX works but no ARP reply seen\n");
    } else {
        print("[NETTEST] FAIL: no frames received\n");
    }

    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
