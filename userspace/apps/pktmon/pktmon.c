/*
 * pktmon.c -- packet monitor / sniffer (freestanding, ring 3).
 * ===========================================================
 *
 * A tiny tcpdump-lite for AutomationOS userspace. NO libc, NO stdio, NO malloc,
 * NO standard headers -- everything is inline syscalls, fixed-size buffers and a
 * handful of static helpers, all copied verbatim from the conventions in
 * userspace/apps/ping/ping.c, nc/nc.c and wget/wget.c (same `sc()` macro, same
 * print/itoa/print_mac/print_ip helpers, same crt0 `int main(argc,argv)` entry).
 *
 * pktmon reads RAW Ethernet frames the same way the kernel net stack's readers
 * do (ping/nettest): it bounded-polls SYS_NET_RECV, yielding on an empty read,
 * and for each delivered frame it decodes and prints:
 *
 *   - source / destination MAC + EtherType
 *   - for IPv4 (0x0800): source / destination IP, protocol name (TCP/UDP/ICMP)
 *   - for TCP/UDP: source + destination ports
 *   - for ARP (0x0806): a short "who-has / is-at" line
 *
 * It prints N frames (default 20) then exits. The receive loop is bounded by a
 * per-frame iteration cap plus SYS_YIELD, so a quiet network can NEVER hang it:
 * it just prints however many frames arrived before the cap and exits 0.
 *
 * Usage (linked with crt0.o -> int main(int argc, char **argv)):
 *   pktmon            capture 20 frames (default), decode, exit
 *   pktmon N          capture up to N frames (1..100000), decode, exit
 *
 * To see traffic, generate some from another shell/program while pktmon runs,
 * e.g. `ping 10.0.2.2` or `dig`/`nc`/`wget` -- their ARP + IPv4 frames (and the
 * gateway's replies) will scroll past. On a dead-quiet link pktmon still returns
 * after the iteration cap with a "(timeout)" note.
 *
 * Build (EXACT -- flags passed DIRECTLY on the command line, NEVER via a
 * variable; objdump grep for fs:0x28 MUST be empty -- no stack canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/pktmon/pktmon.c -o /tmp/pktmon.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/crt0.o /tmp/pktmon.o -o /tmp/pktmon.elf
 *   objdump -d /tmp/pktmon.elf | grep fs:0x28   # MUST be empty
 */

/* ---- syscall numbers (per AutomationOS ABI; identical to ping/nettest) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3    /* write(fd, buf, len)   fd1 = stdout/serial   */
#define SYS_YIELD         15   /* cooperative yield                           */
#define SYS_NET_INFO      59   /* net_info(net_info_t*) -> 0 | -errno         */
#define SYS_NET_RECV      69   /* net_recv(buf,len) -> framelen | 0 | -errno  */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* net_info_t mirror (must match kernel/include/net.h -- same as ping/nettest). */
typedef struct {
    u8  mac[6];
    u8  _pad[2];
    u32 ip;        /* host byte order */
    u32 gateway;   /* host byte order */
} net_info_t;

/*
 * 6-argument inline syscall (n + 5 args -> rdi/rsi/rdx/r10/r8). Copied EXACTLY
 * from ping.c / nc.c -- do not change the clobber list or constraints.
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

/* ---- tiny serial diagnostics (fd 1) -- copied from ping.c ---- */
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
static void print_hex4(u16 v) { print_hex2((u8)(v >> 8)); print_hex2((u8)v); }
static void print_mac(const u8 m[6]) {
    for (int i = 0; i < 6; i++) { if (i) print(":"); print_hex2(m[i]); }
}
static void print_ip(u32 ip /* host order */) {
    print_dec((ip >> 24) & 0xFF); print(".");
    print_dec((ip >> 16) & 0xFF); print(".");
    print_dec((ip >> 8)  & 0xFF); print(".");
    print_dec(ip & 0xFF);
}

/* Read big-endian fields from a byte pointer -> host order (from ping.c). */
static u16 rd_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* Parse a non-negative decimal integer; -1 on bad input (from ping.c). */
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

#define ETH_HLEN     14
#define ETH_P_IP     0x0800
#define ETH_P_ARP    0x0806
#define ETH_P_IPV6   0x86DD
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/*
 * Per-frame receive bound. Each spin is one SYS_NET_RECV plus a SYS_YIELD on an
 * empty read, so this is purely a safety cap that guarantees we return on a
 * quiet link. Same magnitude as the RX caps in ping.c / nettest.c (40000).
 */
#define RX_TRIES_PER_FRAME 40000

#define DEFAULT_COUNT  20
#define MAX_COUNT      100000

/* RX scratch buffer (>= max Ethernet frame; matches ping/nettest 1600). */
static u8 rxbuf[1600];

/* -----------------------------------------------------------------------
 * Decode + print one captured frame. `n` is the frame length in bytes.
 * Prints exactly one line per frame (newline-terminated).
 * --------------------------------------------------------------------- */
static void decode_frame(long idx, const u8 *f, long n) {
    print("#"); print_dec((unsigned long)idx);
    print(" len="); print_dec((unsigned long)n);

    if (n < ETH_HLEN) { print(" [runt]\n"); return; }

    /* Ethernet: dst[0..5], src[6..11], ethertype[12..13] (big-endian). */
    const u8 *dst = f + 0;
    const u8 *src = f + 6;
    u16 et = rd_be16(f + 12);

    print(" "); print_mac(src);
    print(" -> "); print_mac(dst);
    print(" type=0x"); print_hex4(et);

    if (et == ETH_P_ARP) {
        print(" ARP");
        if (n >= ETH_HLEN + 28) {
            u16 op = rd_be16(f + ETH_HLEN + 6);
            u32 spa = rd_be32(f + ETH_HLEN + 14);   /* sender protocol addr */
            u32 tpa = rd_be32(f + ETH_HLEN + 24);   /* target protocol addr */
            if (op == 1) {
                print(" who-has "); print_ip(tpa);
                print(" tell ");    print_ip(spa);
            } else if (op == 2) {
                print(" "); print_ip(spa);
                print(" is-at ");   print_mac(f + ETH_HLEN + 8);
            }
        }
        print("\n");
        return;
    }

    if (et == ETH_P_IPV6) { print(" IPv6\n"); return; }

    if (et != ETH_P_IP) { print("\n"); return; }   /* unknown L3 -- header only */

    /* ---- IPv4 ---- */
    if (n < ETH_HLEN + 20) { print(" IPv4 [truncated]\n"); return; }
    const u8 *ip = f + ETH_HLEN;
    if ((ip[0] >> 4) != 4) { print(" IPv4? [bad ver]\n"); return; }

    u32 ihl = (u32)(ip[0] & 0x0F) * 4;            /* IPv4 header length, bytes */
    if (ihl < 20) { print(" IPv4 [bad ihl]\n"); return; }

    u8  proto = ip[9];
    u32 sip   = rd_be32(ip + 12);
    u32 dip   = rd_be32(ip + 16);

    print(" IPv4 "); print_ip(sip); print(" -> "); print_ip(dip);

    /* L4 header sits at ip + ihl (only if the frame is long enough). */
    const u8 *l4 = ip + ihl;
    long l4_off = (long)(ETH_HLEN + ihl);

    if (proto == IPPROTO_ICMP) {
        print(" ICMP");
        if (n >= l4_off + 2) {
            u8 ic_type = l4[0];
            if      (ic_type == 8) print(" echo-request");
            else if (ic_type == 0) print(" echo-reply");
            else { print(" type="); print_dec(ic_type); }
        }
    } else if (proto == IPPROTO_TCP) {
        print(" TCP");
        if (n >= l4_off + 4) {
            print(" "); print_dec(rd_be16(l4 + 0));   /* src port */
            print(" -> "); print_dec(rd_be16(l4 + 2)); /* dst port */
        }
    } else if (proto == IPPROTO_UDP) {
        print(" UDP");
        if (n >= l4_off + 4) {
            print(" "); print_dec(rd_be16(l4 + 0));   /* src port */
            print(" -> "); print_dec(rd_be16(l4 + 2)); /* dst port */
        }
    } else {
        print(" proto="); print_dec(proto);
    }

    print("\n");
}

int main(int argc, char **argv) {
    print("[PKTMON] starting\n");

    /* Determine how many frames to capture (argv[1], default 20). */
    long want = DEFAULT_COUNT;
    if (argc >= 2 && argv[1]) {
        long v = parse_uint(argv[1]);
        if (v < 0) {
            print("[PKTMON] usage: pktmon [count]\n");
            return 1;
        }
        if (v == 0)        v = DEFAULT_COUNT;   /* 0 -> default, never infinite */
        if (v > MAX_COUNT) v = MAX_COUNT;       /* clamp absurd counts          */
        want = v;
    }

    /* Show the NIC identity (best-effort; not fatal if link is down). */
    net_info_t info;
    for (int i = 0; i < (int)sizeof(info); i++) ((u8 *)&info)[i] = 0;
    long ir = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0);
    if (ir == 0) {
        print("[PKTMON] NIC "); print_mac(info.mac);
        print(" IP "); print_ip(info.ip);
        print(" GW "); print_ip(info.gateway);
        print("\n");
    } else {
        print("[PKTMON] warning: SYS_NET_INFO rc=");
        print_dec((unsigned long)(-ir));
        print(" (capturing anyway)\n");
    }

    print("[PKTMON] capturing "); print_dec((unsigned long)want);
    print(" frame(s) -- generate traffic with ping/dig/nc to see them\n");

    /*
     * Capture loop. The OUTER loop counts delivered frames (>= 0, <= want); the
     * INNER loop bounded-polls SYS_NET_RECV exactly like ping/nettest: a return
     * of 0 (nothing pending) or negative (transient) yields and retries, up to a
     * fixed cap so a silent link can never hang us. If the cap is hit before a
     * frame arrives we stop -- printing however many we got.
     */
    long captured = 0;
    int  timed_out = 0;

    for (long got = 0; got < want; got++) {
        long n = 0;
        long tries = 0;
        for (; tries < RX_TRIES_PER_FRAME; tries++) {
            n = sc(SYS_NET_RECV, (long)rxbuf, (long)sizeof(rxbuf), 0, 0, 0);
            if (n > 0) break;                       /* got a frame */
            sc(SYS_YIELD, 0, 0, 0, 0, 0);           /* none yet -> yield + retry */
        }
        if (n <= 0) { timed_out = 1; break; }       /* cap hit on a quiet link */

        decode_frame(got + 1, rxbuf, n);
        captured++;
    }

    print("[PKTMON] done: "); print_dec((unsigned long)captured);
    print(" frame(s) captured");
    if (timed_out) print(" (timeout: link quiet -- generate traffic and retry)");
    print("\n");

    return 0;
}
