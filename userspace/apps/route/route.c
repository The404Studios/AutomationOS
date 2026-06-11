/*
 * route.c -- Routing table display tool (freestanding, ring 3).
 * ==============================================================
 *
 * Calls SYS_ROUTE_TABLE to dump the kernel's IPv4 routing table and prints
 * it in a human-readable table format:
 *
 *   Destination      Netmask          Gateway          Iface
 *   0.0.0.0          0.0.0.0          10.0.2.2         10.0.2.15
 *   10.0.2.0         255.255.255.0    0.0.0.0          10.0.2.15
 *
 * Usage:
 *   route                   -- display routing table
 *   route (no args)         -- run built-in self-test (no network)
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.
 * Inline syscalls + fixed buffers + own helpers only.
 *
 * Build (flags DIRECT on cmdline -- NEVER via shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/route/route.c -o route.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o route.o -o build/route
 *   objdump -d build/route | grep fs:0x28   # MUST be empty
 */

/* ---- syscall numbers (must match kernel/include/syscall.h) ---- */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_ROUTE_TABLE 90

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

/*
 * ip_to_str -- convert a host-order IPv4 address to a dotted-quad string.
 * Returns the length written (not including NUL).  buf must be >= 16 bytes.
 */
static int ip_to_str(unsigned int ip, char *buf)
{
    int pos = 0;
    for (int octet = 3; octet >= 0; octet--) {
        unsigned int b = (ip >> (octet * 8)) & 0xFFu;
        /* Write 1-3 decimal digits. */
        if (b >= 100) {
            buf[pos++] = (char)('0' + b / 100);
            buf[pos++] = (char)('0' + (b / 10) % 10);
            buf[pos++] = (char)('0' + b % 10);
        } else if (b >= 10) {
            buf[pos++] = (char)('0' + b / 10);
            buf[pos++] = (char)('0' + b % 10);
        } else {
            buf[pos++] = (char)('0' + b);
        }
        if (octet > 0)
            buf[pos++] = '.';
    }
    buf[pos] = '\0';
    return pos;
}

/*
 * print_ip_padded -- print a dotted-quad IP left-justified in a field of
 * `width` characters (padded with spaces on the right).
 */
static void print_ip_padded(unsigned int ip, int width)
{
    char buf[16];
    int len = ip_to_str(ip, buf);
    print(buf);
    for (int i = len; i < width; i++)
        print_ch(' ');
}

/* ---- route_info_t (must match kernel/include/netif.h) ---- */
typedef struct {
    unsigned int dest;       /* host order */
    unsigned int mask;       /* host order */
    unsigned int gateway;    /* host order, 0 = on-link */
    unsigned int iface;      /* host order */
} route_info_t;

#define MAX_ROUTES  16

/* ---- self-test (offline, no network) ---- */

static int selftest(void)
{
    int pass = 1;

    /* Test 1: ip_to_str produces correct dotted-quad. */
    char buf[16];
    ip_to_str(0x0A000202u, buf);   /* 10.0.2.2 */
    const char *expect = "10.0.2.2";
    for (int i = 0; ; i++) {
        if (buf[i] != expect[i]) {
            print("  FAIL: ip_to_str(10.0.2.2) = ");
            print(buf);
            print("\n");
            pass = 0;
            break;
        }
        if (buf[i] == '\0') break;
    }

    /* Test 2: ip_to_str for 0.0.0.0. */
    ip_to_str(0x00000000u, buf);
    expect = "0.0.0.0";
    for (int i = 0; ; i++) {
        if (buf[i] != expect[i]) {
            print("  FAIL: ip_to_str(0.0.0.0) = ");
            print(buf);
            print("\n");
            pass = 0;
            break;
        }
        if (buf[i] == '\0') break;
    }

    /* Test 3: ip_to_str for 255.255.255.255. */
    ip_to_str(0xFFFFFFFFu, buf);
    expect = "255.255.255.255";
    for (int i = 0; ; i++) {
        if (buf[i] != expect[i]) {
            print("  FAIL: ip_to_str(255.255.255.255) = ");
            print(buf);
            print("\n");
            pass = 0;
            break;
        }
        if (buf[i] == '\0') break;
    }

    /* Test 4: route_info_t struct size. */
    if (sizeof(route_info_t) != 16) {
        print("  FAIL: sizeof(route_info_t) != 16\n");
        pass = 0;
    }

    return pass ? 0 : 1;
}

/* ---- entry point ---- */

int main(int argc, char **argv)
{
    (void)argv;

    /* Self-test mode: no arguments. */
    if (argc <= 1) {
        int rc = selftest();
        if (rc == 0) {
            print("ROUTE SELFTEST: PASS\n");
            return 0;
        } else {
            print("ROUTE SELFTEST: FAIL\n");
            return 1;
        }
    }

    /* Live mode: display routing table. */
    route_info_t entries[MAX_ROUTES];

    /* Zero the buffer. */
    unsigned char *p = (unsigned char *)entries;
    for (unsigned long i = 0; i < sizeof(entries); i++) p[i] = 0;

    long n = sc(SYS_ROUTE_TABLE, (long)entries, MAX_ROUTES, 0, 0, 0, 0);
    if (n <= 0) {
        print("route: no routes (table empty or syscall failed)\n");
        return 1;
    }

    /* Print header. */
    #define COL_W  17
    print("Destination      Netmask          Gateway          Iface\n");

    /* Print each route entry. */
    for (long i = 0; i < n; i++) {
        print_ip_padded(entries[i].dest,    COL_W);
        print_ip_padded(entries[i].mask,    COL_W);
        print_ip_padded(entries[i].gateway, COL_W);

        /* Iface column: no padding needed (last column). */
        char buf[16];
        ip_to_str(entries[i].iface, buf);
        print(buf);
        print("\n");
    }

    /* Summary line. */
    print_dec((unsigned long)n);
    print(" route(s)\n");

    return 0;
}
