/*
 * abicheck.c -- ABI struct-size verifier (freestanding, ring 3).
 * ===============================================================
 *
 * Checks that sizeof() for every kernel/userspace ABI struct matches
 * the expected compile-time constant.  If the kernel changes a struct
 * layout and forgets to bump the userspace mirror, this tool will catch
 * it at build time (_Static_assert) or at run time (size comparison).
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.
 * Inline syscalls + fixed buffers + own helpers only.
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/abicheck/abicheck.c -o abicheck.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o abicheck.o -o build/abicheck
 *   objdump -d build/abicheck | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Freestanding integer types (no stdint.h).
 * --------------------------------------------------------------------- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned long      usize;

/* -----------------------------------------------------------------------
 * Syscall numbers (match kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_WRITE  3

#define FD_STDOUT  1

/* -----------------------------------------------------------------------
 * 6-argument inline syscall.
 * --------------------------------------------------------------------- */
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

/* -----------------------------------------------------------------------
 * Tiny freestanding helpers.
 * --------------------------------------------------------------------- */
static usize k_strlen(const char *s)
{
    usize n = 0;
    while (s[n]) n++;
    return n;
}

static void k_puts(const char *s)
{
    sc(SYS_WRITE, FD_STDOUT, (long)s, (long)k_strlen(s), 0, 0, 0);
}

/* Print a decimal number (up to 64-bit). */
static void k_putu(u64 v)
{
    char buf[24];
    int  pos = 23;
    buf[pos] = '\0';
    if (v == 0) { buf[--pos] = '0'; }
    else {
        while (v > 0) {
            buf[--pos] = '0' + (char)(v % 10);
            v /= 10;
        }
    }
    k_puts(&buf[pos]);
}

/* -----------------------------------------------------------------------
 * ABI struct mirrors -- byte-for-byte copies of the kernel definitions.
 *
 * Each struct is followed by a _Static_assert so any drift between this
 * file and the kernel header is caught at COMPILE TIME.
 * --------------------------------------------------------------------- */

/* --- uapi_net_info_t (uapi/net.h, SYS_NET_INFO = 59) --- */
#define NET_INFO_ABI_SIZE  80

typedef struct {
    char  ifname[16];
    u8    mac[6];
    u8    _pad[2];
    u32   ip;
    u32   netmask;
    u32   gateway;
    u32   dns;
    u8    up;
    u8    dhcp;
    u8    _reserved[6];
    u64   tx_packets;
    u64   rx_packets;
    u64   tx_bytes;
    u64   rx_bytes;
} net_info_t;

_Static_assert(sizeof(net_info_t) == NET_INFO_ABI_SIZE,
               "net_info_t ABI size drift");

/* --- uapi_net_config_t (uapi/net.h, SYS_NET_CONFIG = 89) --- */
#define NET_CONFIG_ABI_SIZE  36

typedef struct {
    char  ifname[16];
    u32   ip;
    u32   netmask;
    u32   gateway;
    u32   dns;
    u32   flags;
} net_config_t;

_Static_assert(sizeof(net_config_t) == NET_CONFIG_ABI_SIZE,
               "net_config_t ABI size drift");

/* --- uapi_route_info_t (uapi/net.h, SYS_ROUTE_TABLE = 90) --- */
#define ROUTE_INFO_ABI_SIZE  16

typedef struct {
    u32  dest;
    u32  mask;
    u32  gateway;
    u32  iface_ip;
} route_info_t;

_Static_assert(sizeof(route_info_t) == ROUTE_INFO_ABI_SIZE,
               "route_info_t ABI size drift");

/* --- uapi_arp_info_t (uapi/net.h, SYS_ARP_TABLE = 91) --- */
#define ARP_INFO_ABI_SIZE  12

typedef struct {
    u32  ip;
    u8   mac[6];
    u8   valid;
    u8   _pad;
} arp_info_t;

_Static_assert(sizeof(arp_info_t) == ARP_INFO_ABI_SIZE,
               "arp_info_t ABI size drift");

/* --- sysinfo_t (procapi.h, SYS_SYSINFO = 62) --- */
#define SYSINFO_ABI_SIZE  32

typedef struct {
    u64  total_mem;
    u64  free_mem;
    u64  uptime_ms;
    u32  proc_count;
    u32  _pad;
} sysinfo_t;

_Static_assert(sizeof(sysinfo_t) == SYSINFO_ABI_SIZE,
               "sysinfo_t ABI size drift");

/* --- rtc_time_t (rtc.h, SYS_GETTIME = 42) --- */
#define RTC_TIME_ABI_SIZE  8

typedef struct {
    u16  year;
    u8   month;
    u8   day;
    u8   hour;
    u8   min;
    u8   sec;
} rtc_time_t;

_Static_assert(sizeof(rtc_time_t) == RTC_TIME_ABI_SIZE,
               "rtc_time_t ABI size drift");

/* --- fb_acquire_t (syscall.h, SYS_FB_ACQUIRE = 11) --- */
#define FB_ACQUIRE_ABI_SIZE  24

typedef struct {
    u64  vaddr;
    u32  width;
    u32  height;
    u32  pitch;
    u32  bpp;
} fb_acquire_t;

_Static_assert(sizeof(fb_acquire_t) == FB_ACQUIRE_ABI_SIZE,
               "fb_acquire_t ABI size drift");

/* -----------------------------------------------------------------------
 * Test runner.
 * --------------------------------------------------------------------- */

typedef struct {
    const char *name;
    usize       actual;
    usize       expected;
} abi_check_t;

static int check_one(const abi_check_t *c)
{
    k_puts("  ");
    k_puts(c->name);

    /* Pad to a fixed column. */
    usize nlen = k_strlen(c->name);
    for (usize i = nlen; i < 16; i++) k_puts(" ");

    k_putu(c->actual);
    k_puts(" bytes ... ");

    if (c->actual == c->expected) {
        k_puts("PASS\n");
        return 0;
    } else {
        k_puts("FAIL (expected ");
        k_putu(c->expected);
        k_puts(")\n");
        return 1;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    k_puts("ABICHECK:\n");

    abi_check_t checks[] = {
        { "net_info_t",    sizeof(net_info_t),    NET_INFO_ABI_SIZE    },
        { "net_config_t",  sizeof(net_config_t),  NET_CONFIG_ABI_SIZE  },
        { "route_info_t",  sizeof(route_info_t),  ROUTE_INFO_ABI_SIZE  },
        { "arp_info_t",    sizeof(arp_info_t),    ARP_INFO_ABI_SIZE    },
        { "sysinfo_t",     sizeof(sysinfo_t),     SYSINFO_ABI_SIZE     },
        { "rtc_time_t",    sizeof(rtc_time_t),    RTC_TIME_ABI_SIZE    },
        { "fb_acquire_t",  sizeof(fb_acquire_t),  FB_ACQUIRE_ABI_SIZE  },
    };

    int fail = 0;
    int count = (int)(sizeof(checks) / sizeof(checks[0]));

    for (int i = 0; i < count; i++) {
        fail += check_one(&checks[i]);
    }

    if (fail == 0) {
        k_puts("RESULT: ALL PASS\n");
    } else {
        k_puts("RESULT: ");
        k_putu((u64)fail);
        k_puts(" FAILED\n");
    }

    sc(SYS_EXIT, fail ? 1 : 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
