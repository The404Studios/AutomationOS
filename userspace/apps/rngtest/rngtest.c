/*
 * rngtest.c -- Userspace RNG sanity test (freestanding, ring 3)
 * ==============================================================
 *
 * Tests SYS_RANDOM (syscall 41) by:
 *   1. Requesting 32 random bytes, printing them as hex to serial (fd 1).
 *   2. Verifying the buffer is not all-zero.
 *   3. Requesting two 8-byte samples and verifying they differ.
 *   4. If SYS_RANDOM returns ENOSYS (-38) it prints a clear message and exits.
 *
 * Serial output format:
 *   [RNG] SYS_RANDOM returned: <ret>
 *   [RNG] bytes: XX XX XX XX ... (32 bytes)
 *   [RNG] not-all-zero: PASS / FAIL
 *   [RNG] two-samples-differ: PASS / FAIL (a=<hex> b=<hex>)
 *   [RNG] DONE: <p>/<t> passed
 *
 * Build (flags DIRECTLY on command line -- never via shell var):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/rngtest/rngtest.c -o /tmp/rngtest.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/rngtest.o -o /tmp/rngtest.elf
 *   objdump -d /tmp/rngtest.elf | grep 'fs:0x28'   # must be empty
 *
 * Canary check: the objdump grep above MUST produce no output.
 * If it does, the -fno-stack-protector flag was not applied.
 */

/* -----------------------------------------------------------------------
 * Syscall numbers
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_EXIT         0
#define SYS_YIELD        15
#define SYS_RANDOM       41   /* proposed; fills user buffer with random bytes */

/* -----------------------------------------------------------------------
 * Types (freestanding -- no libc headers)
 * --------------------------------------------------------------------- */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef unsigned long      size_t;
typedef long               ssize_t;

/* -----------------------------------------------------------------------
 * Inline syscall helpers
 * --------------------------------------------------------------------- */

/* 3-argument syscall */
static inline long sc3(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* 2-argument syscall (pads remaining to 0) */
static inline long sc2(long n, long a1, long a2)
{
    return sc3(n, a1, a2, 0);
}

/* -----------------------------------------------------------------------
 * Freestanding I/O helpers
 * --------------------------------------------------------------------- */

static size_t k_strlen(const char *s)
{
    size_t i = 0;
    while (s[i]) i++;
    return i;
}

static void serial_write(const char *s, size_t len)
{
    sc3(SYS_WRITE, 1, (long)s, (long)len);
}

static void serial_puts(const char *s)
{
    serial_write(s, k_strlen(s));
}

/* Write a uint64 as decimal */
static void serial_u64(uint64_t v)
{
    char buf[24];
    int  i = (int)(sizeof(buf)) - 1;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    } else {
        while (v) {
            buf[--i] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    serial_puts(buf + i);
}

/* Write a signed int64 as decimal */
static void serial_i64(int64_t v)
{
    if (v < 0) {
        serial_puts("-");
        serial_u64((uint64_t)(-v));
    } else {
        serial_u64((uint64_t)v);
    }
}

/* Hex digits */
static const char hex_chars[] = "0123456789ABCDEF";

/* Write a single byte as two hex digits */
static void serial_hex_byte(uint8_t b)
{
    char buf[2];
    buf[0] = hex_chars[(b >> 4) & 0xF];
    buf[1] = hex_chars[ b       & 0xF];
    serial_write(buf, 2);
}

/* Write a uint64 as 16 hex digits */
static void serial_hex64(uint64_t v)
{
    for (int shift = 60; shift >= 0; shift -= 4)
        serial_write(&hex_chars[(v >> shift) & 0xF], 1);
}

/* -----------------------------------------------------------------------
 * Test framework
 * --------------------------------------------------------------------- */

static int g_pass  = 0;
static int g_total = 0;

static void pass(const char *name)
{
    serial_puts("[RNG] ");
    serial_puts(name);
    serial_puts(": PASS\n");
    g_pass++;
    g_total++;
}

static void fail(const char *name, const char *detail)
{
    serial_puts("[RNG] ");
    serial_puts(name);
    serial_puts(": FAIL (");
    serial_puts(detail);
    serial_puts(")\n");
    g_total++;
}

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

static int all_zero(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 0) return 0;
    return 1;
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/*
 * test_random_bytes:
 *   Call SYS_RANDOM for 32 bytes, print them, check not-all-zero.
 */
static void test_random_bytes(void)
{
    uint8_t buf[32];

    /* Zero the buffer so we can detect a no-op syscall */
    for (int i = 0; i < 32; i++) buf[i] = 0;

    long ret = sc2(SYS_RANDOM, (long)buf, 32);

    serial_puts("[RNG] SYS_RANDOM returned: ");
    serial_i64((int64_t)ret);
    serial_puts("\n");

    if (ret == -38 /* ENOSYS */ || ret == -1 /* ENOTSUP */) {
        serial_puts("[RNG] SYS_RANDOM not yet wired (ENOSYS/ENOTSUP) -- skipping\n");
        return;
    }

    if (ret < 0) {
        fail("sys_random_call", "unexpected negative return");
        return;
    }

    /* Print the bytes */
    serial_puts("[RNG] bytes: ");
    for (int i = 0; i < 32; i++) {
        serial_hex_byte(buf[i]);
        if (i < 31) serial_puts(" ");
    }
    serial_puts("\n");

    /* Check not-all-zero */
    if (!all_zero(buf, 32))
        pass("not-all-zero");
    else
        fail("not-all-zero", "all 32 bytes are 0x00");
}

/*
 * test_two_samples_differ:
 *   Request two independent 8-byte samples.  They must differ.
 *   (Probability of collision is ~1/2^64 with any decent RNG.)
 */
static void test_two_samples_differ(void)
{
    uint64_t a = 0, b = 0;

    long ra = sc2(SYS_RANDOM, (long)&a, 8);
    long rb = sc2(SYS_RANDOM, (long)&b, 8);

    if (ra < 0 || rb < 0) {
        /* Syscall not wired -- silently skip */
        return;
    }

    serial_puts("[RNG] sample_a=");
    serial_hex64(a);
    serial_puts(" sample_b=");
    serial_hex64(b);
    serial_puts("\n");

    if (a != b)
        pass("two-samples-differ");
    else
        fail("two-samples-differ", "both samples identical -- very suspicious");
}

/*
 * test_large_fill:
 *   Request 256 bytes, verify at least 8 distinct byte values appear
 *   (a uniform PRNG over 256 bytes will almost certainly use many values).
 */
static void test_large_fill(void)
{
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = 0;

    long ret = sc2(SYS_RANDOM, (long)buf, 256);
    if (ret < 0) return; /* not wired */

    /* Count distinct byte values */
    uint8_t seen[256];
    for (int i = 0; i < 256; i++) seen[i] = 0;
    int distinct = 0;
    for (int i = 0; i < 256; i++) {
        if (!seen[buf[i]]) {
            seen[buf[i]] = 1;
            distinct++;
        }
    }

    serial_puts("[RNG] 256-byte fill: ");
    serial_u64((uint64_t)distinct);
    serial_puts(" distinct byte values\n");

    if (distinct >= 16)
        pass("large-fill-distinct");
    else
        fail("large-fill-distinct", "fewer than 16 distinct values in 256 bytes");
}

/* -----------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial_puts("[RNG] rngtest starting\n");

    test_random_bytes();
    test_two_samples_differ();
    test_large_fill();

    serial_puts("[RNG] DONE: ");
    serial_u64((uint64_t)g_pass);
    serial_puts("/");
    serial_u64((uint64_t)g_total);
    serial_puts(" passed\n");

    /* Never return to kernel; yield forever */
    while (1)
        sc3(SYS_YIELD, 0, 0, 0);
}
