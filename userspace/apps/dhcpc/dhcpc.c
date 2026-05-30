/*
 * dhcpc.c -- DHCP client userspace tool (freestanding, ring 3).
 * ==============================================================
 *
 * Calls dhcp_acquire() to run a full DISCOVER->OFFER->REQUEST->ACK handshake
 * and prints the obtained lease in human-readable dotted-quad form:
 *
 *   ip a.b.c.d  mask a.b.c.d  gw a.b.c.d  dns a.b.c.d  server a.b.c.d  lease <secs>s
 *
 * On failure (no server, NAK, timeout):
 *
 *   dhcpc: no DHCP server responded
 *
 * NOTE: obtaining the lease does NOT apply it to the kernel's live network
 * configuration (static net.ip / net.gateway / net.netmask / DNS).  Applying
 * the lease requires a kernel hook -- e.g. a SYS_NET_SET_CONFIG syscall or a
 * writable sys_net_info path -- which does not yet exist.  This tool only
 * OBTAINS and DISPLAYS the lease.  On QEMU user-mode networking (slirp) the
 * server is present; expect:  ip 10.0.2.15  mask 255.255.255.0
 *                              gw 10.0.2.2   dns 10.0.2.3
 *
 * Self-test mode (argc <= 1 / no arguments):
 *   Calls dhcp_selftest() -- an entirely offline structural test that builds a
 *   sample DHCPDISCOVER packet and parses a hardcoded DHCPACK byte array.  No
 *   live DHCP server is needed.  Prints:
 *       DHCPC SELFTEST: PASS    (dhcp_selftest() returned 0) -> exit 0
 *       DHCPC SELFTEST: FAIL    (non-zero return)            -> exit 1
 *
 * Live mode (argc > 1, e.g. "dhcpc run"):
 *   Runs dhcp_acquire().  Prints lease on success (exit 0) or the error
 *   message on failure (exit 1).
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.
 * Inline syscalls + fixed buffers + own helpers only.
 *
 * Build flags (NEVER via a shell variable, NEVER fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/dhcpc/dhcpc.c -o dhcpc.o
 *   objdump -d dhcpc.o | grep fs:0x28   # MUST be empty
 *
 * Link with crt0.o (which provides _start -> main) and dhcp.o:
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       userspace/crt0.o dhcpc.o dhcp.o -o dhcpc.elf
 */

#include "../../lib/net/dhcp.h"

/* -------------------------------------------------------------------------
 * Syscall numbers (must match kernel/include/syscall.h).
 * ----------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_WRITE  3

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

/*
 * print_dec -- print an unsigned 32-bit value in decimal.
 */
static void print_dec(unsigned int v)
{
    char buf[12];
    int  i = 0;
    if (v == 0) { print_ch('0'); return; }
    do {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    /* digits are reversed */
    while (i > 0) print_ch(buf[--i]);
}

/*
 * print_ip -- convert a host-order IPv4 address to dotted-quad and print it.
 *
 * HOST order: 0xAABBCCDD -> "AA.BB.CC.DD"
 */
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
 * Entry point (crt0 calls main(argc, argv)).
 * ----------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argv;   /* argv[0] is the program name; argv[1..] are extra args */

    /* ------------------------------------------------------------------
     * Self-test mode: no arguments (argc <= 1).
     * Run the offline structural DHCP self-test; no live server needed.
     * ------------------------------------------------------------------ */
    if (argc <= 1) {
        int rc = dhcp_selftest();
        if (rc == 0) {
            print("DHCPC SELFTEST: PASS\n");
            return 0;
        } else {
            print("DHCPC SELFTEST: FAIL\n");
            return 1;
        }
    }

    /* ------------------------------------------------------------------
     * Live mode: at least one extra argument (e.g. "dhcpc run").
     * Run the full DHCP handshake and print the obtained lease.
     * ------------------------------------------------------------------ */
    dhcp_lease_t lease;
    /* Zero the struct so unset fields read as 0.0.0.0. */
    unsigned char *p = (unsigned char *)&lease;
    for (unsigned long i = 0; i < sizeof(lease); i++) p[i] = 0;

    int rc = dhcp_acquire(&lease);
    if (rc != DHCP_OK) {
        print("dhcpc: no DHCP server responded\n");
        return 1;
    }

    /*
     * Print the lease in the specified format:
     *   ip a.b.c.d  mask a.b.c.d  gw a.b.c.d  dns a.b.c.d  server a.b.c.d  lease <secs>s
     */
    print("ip ");      print_ip(lease.ip);
    print("  mask ");  print_ip(lease.netmask);
    print("  gw ");    print_ip(lease.gateway);
    print("  dns ");   print_ip(lease.dns);
    print("  server "); print_ip(lease.server);
    print("  lease "); print_dec(lease.lease_secs);
    print("s\n");

    return 0;
}
