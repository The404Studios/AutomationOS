/*
 * smoketest.c -- Kernel/syscall smoke test (freestanding, ring 3).
 * =================================================================
 *
 * Runs a series of self-contained checks via inline syscalls and
 * reports results to serial (fd 1) as:
 *   [SMOKE] <name>: PASS
 *   [SMOKE] <name>: FAIL (<detail>)
 * and ends with:
 *   [SMOKE] DONE: <p>/<t> passed
 *
 * Checks performed
 * ----------------
 * (a) ticks_advance  -- SYS_GET_TICKS_MS advances after a yield-spin
 * (b) mmap_rw        -- SYS_MMAP 64 KB anon map, write pattern, read back
 * (c) shm_rw         -- SYS_SHMGET + SYS_SHMAT, write/read a word
 * (d) msgq_roundtrip -- SYS_MSGGET + SYS_MSGSND + SYS_MSGRCV round-trip
 * (e) fb_acquire     -- SYS_FB_ACQUIRE returns 0 with sane w/h
 * (f) readdir_root   -- SYS_OPENDIR("/") + SYS_READDIR lists > 0 entries
 *
 * No libc -- only inline syscalls and local helpers.
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/smoketest/smoketest.c -o /tmp/smoke.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/smoke.o -o /tmp/smoke.elf
 *   objdump -d /tmp/smoke.elf | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_GETPID       8
#define SYS_YIELD        15
#define SYS_SHMGET       18
#define SYS_SHMAT        19
#define SYS_MSGGET       22
#define SYS_MSGSND       23
#define SYS_MSGRCV       24
#define SYS_OPENDIR      30
#define SYS_READDIR      31
#define SYS_CLOSEDIR     32
#define SYS_MMAP         37
#define SYS_MUNMAP       38
#define SYS_FB_ACQUIRE   39
#define SYS_GET_TICKS_MS 40

/* IPC flags (from kernel/include/ipc.h) */
#define IPC_PRIVATE  0
#define IPC_CREAT    0x0200

/* -----------------------------------------------------------------------
 * Types
 * --------------------------------------------------------------------- */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef long               ssize_t;
typedef unsigned long      size_t;

/* Framebuffer info (from kernel/include/syscall.h) */
typedef struct {
    uint64_t vaddr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} fb_acquire_t;

/* Minimal dirent (matches kernel vfs.h) */
#define NAME_MAX 256
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[NAME_MAX];
};

/* msgbuf for message queue round-trip */
struct msgbuf {
    int64_t mtype;
    char    mtext[64];
};

/* -----------------------------------------------------------------------
 * Inline syscall helper (3 general args, enough for all uses here)
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* 4-arg variant needed for msgsnd/msgrcv */
static inline long sc4(long n, long a1, long a2, long a3, long a4)
{
    long r;
    register long r10 asm("r10") = a4;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Freestanding string helpers
 * --------------------------------------------------------------------- */
static size_t k_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void k_memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
}

static int k_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Serial output helpers
 * --------------------------------------------------------------------- */
static void serial_write(const char *s, size_t len)
{
    sc(SYS_WRITE, 1, (long)s, (long)len);
}

static void serial_puts(const char *s)
{
    serial_write(s, k_strlen(s));
}

/* Write a decimal number (uint64) */
static void serial_uint(uint64_t v)
{
    char buf[24];
    int  i = sizeof(buf) - 1;
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

/* Write a signed decimal number */
static void serial_int(int64_t v)
{
    if (v < 0) {
        serial_puts("-");
        serial_uint((uint64_t)(-v));
    } else {
        serial_uint((uint64_t)v);
    }
}

/* -----------------------------------------------------------------------
 * Test framework helpers
 * --------------------------------------------------------------------- */
static int g_pass = 0;
static int g_total = 0;

static void report_pass(const char *name)
{
    serial_puts("[SMOKE] ");
    serial_puts(name);
    serial_puts(": PASS\n");
    g_pass++;
    g_total++;
}

static void report_fail(const char *name, const char *detail)
{
    serial_puts("[SMOKE] ");
    serial_puts(name);
    serial_puts(": FAIL (");
    serial_puts(detail);
    serial_puts(")\n");
    g_total++;
}

/* -----------------------------------------------------------------------
 * (a) ticks_advance -- SYS_GET_TICKS_MS returns a monotonically
 *     advancing counter.  Spin-yield until the counter changes.
 * --------------------------------------------------------------------- */
static void test_ticks_advance(void)
{
    const char *name = "ticks_advance";
    long t0 = sc(SYS_GET_TICKS_MS, 0, 0, 0);

    /* Yield up to 1000 times waiting for the tick to advance */
    int spins = 0;
    long t1   = t0;
    while (t1 <= t0 && spins < 1000) {
        sc(SYS_YIELD, 0, 0, 0);
        t1 = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        spins++;
    }

    if (t1 > t0) {
        report_pass(name);
    } else {
        serial_puts("[SMOKE] ticks_advance: FAIL (t0=");
        serial_int((int64_t)t0);
        serial_puts(" t1=");
        serial_int((int64_t)t1);
        serial_puts(")\n");
        g_total++;
    }
}

/* -----------------------------------------------------------------------
 * (b) mmap_rw -- Map 64 KB, write a known byte pattern, read it back,
 *     then unmap.
 * --------------------------------------------------------------------- */
static void test_mmap_rw(void)
{
    const char *name  = "mmap_rw";
    const long  len   = 64 * 1024;   /* 64 KB */
    const long  prot  = 3;            /* PROT_READ | PROT_WRITE */

    long addr = sc(SYS_MMAP, 0, len, prot);

    if (addr <= 0) {
        serial_puts("[SMOKE] mmap_rw: FAIL (mmap returned ");
        serial_int((int64_t)addr);
        serial_puts(")\n");
        g_total++;
        return;
    }

    /* Write pattern: byte[i] = (i & 0xFF) */
    unsigned char *p = (unsigned char *)addr;
    for (long i = 0; i < len; i++)
        p[i] = (unsigned char)(i & 0xFF);

    /* Verify */
    int ok = 1;
    for (long i = 0; i < len; i++) {
        if (p[i] != (unsigned char)(i & 0xFF)) {
            ok = 0;
            break;
        }
    }

    sc(SYS_MUNMAP, addr, len, 0);

    if (ok)
        report_pass(name);
    else
        report_fail(name, "pattern mismatch");
}

/* -----------------------------------------------------------------------
 * (c) shm_rw -- shmget + shmat, write a magic word, read it back.
 * --------------------------------------------------------------------- */
static void test_shm_rw(void)
{
    const char *name    = "shm_rw";
    const long  size    = 4096;
    const long  flags   = IPC_CREAT | 0600;
    const long  magic   = 0xDEADBEEF12345678LL;

    long id = sc(SYS_SHMGET, IPC_PRIVATE, size, flags);
    if (id < 0) {
        serial_puts("[SMOKE] shm_rw: FAIL (shmget=");
        serial_int((int64_t)id);
        serial_puts(")\n");
        g_total++;
        return;
    }

    long addr = sc(SYS_SHMAT, id, 0, 0);
    if (addr <= 0) {
        serial_puts("[SMOKE] shm_rw: FAIL (shmat=");
        serial_int((int64_t)addr);
        serial_puts(")\n");
        g_total++;
        return;
    }

    volatile long *word = (volatile long *)addr;
    *word = magic;

    if (*word == magic)
        report_pass(name);
    else
        report_fail(name, "readback mismatch");
}

/* -----------------------------------------------------------------------
 * (d) msgq_roundtrip -- msgget + msgsnd + msgrcv.
 * --------------------------------------------------------------------- */
static void test_msgq_roundtrip(void)
{
    const char *name  = "msgq_roundtrip";
    const long  flags = IPC_CREAT | 0600;
    const char  payload[] = "smoketest_msg";

    long qid = sc(SYS_MSGGET, IPC_PRIVATE, flags, 0);
    if (qid < 0) {
        serial_puts("[SMOKE] msgq_roundtrip: FAIL (msgget=");
        serial_int((int64_t)qid);
        serial_puts(")\n");
        g_total++;
        return;
    }

    /* Send */
    struct msgbuf snd;
    k_memset(&snd, 0, sizeof(snd));
    snd.mtype = 1;
    for (size_t i = 0; i < sizeof(payload); i++)
        snd.mtext[i] = payload[i];

    long sz = (long)(k_strlen(payload) + 1);
    /* msgsnd(qid, &snd, sz, 0) -- mtype is not counted in sz */
    long r = sc4(SYS_MSGSND, qid, (long)&snd, sz, 0);
    if (r < 0) {
        serial_puts("[SMOKE] msgq_roundtrip: FAIL (msgsnd=");
        serial_int((int64_t)r);
        serial_puts(")\n");
        g_total++;
        return;
    }

    /* Receive */
    struct msgbuf rcv;
    k_memset(&rcv, 0, sizeof(rcv));
    /* msgrcv(qid, &rcv, sizeof(rcv.mtext), type=1, flags=0) */
    long got = sc4(SYS_MSGRCV, qid, (long)&rcv, (long)sizeof(rcv.mtext), 1);
    if (got < 0) {
        serial_puts("[SMOKE] msgq_roundtrip: FAIL (msgrcv=");
        serial_int((int64_t)got);
        serial_puts(")\n");
        g_total++;
        return;
    }

    if (k_memcmp(rcv.mtext, payload, sizeof(payload)) == 0 && rcv.mtype == 1)
        report_pass(name);
    else
        report_fail(name, "payload/type mismatch");
}

/* -----------------------------------------------------------------------
 * (e) fb_acquire -- SYS_FB_ACQUIRE fills fb_acquire_t, returns 0 and
 *     reports sane (> 0) width and height.
 * --------------------------------------------------------------------- */
static void test_fb_acquire(void)
{
    const char *name = "fb_acquire";
    fb_acquire_t info;
    k_memset(&info, 0, sizeof(info));

    long r = sc(SYS_FB_ACQUIRE, (long)&info, 0, 0);
    if (r != 0) {
        serial_puts("[SMOKE] fb_acquire: FAIL (ret=");
        serial_int((int64_t)r);
        serial_puts(")\n");
        g_total++;
        return;
    }

    if (info.width == 0 || info.height == 0) {
        serial_puts("[SMOKE] fb_acquire: FAIL (w=");
        serial_uint((uint64_t)info.width);
        serial_puts(" h=");
        serial_uint((uint64_t)info.height);
        serial_puts(")\n");
        g_total++;
        return;
    }

    report_pass(name);
}

/* -----------------------------------------------------------------------
 * (f) readdir_root -- open "/" and count entries with SYS_READDIR.
 * --------------------------------------------------------------------- */
static void test_readdir_root(void)
{
    const char *name = "readdir_root";
    const char *path = "/";

    long dirfd = sc(SYS_OPENDIR, (long)path, 0, 0);
    if (dirfd < 0) {
        serial_puts("[SMOKE] readdir_root: FAIL (opendir=");
        serial_int((int64_t)dirfd);
        serial_puts(")\n");
        g_total++;
        return;
    }

    struct dirent ent;
    int count = 0;
    long r;
    while ((r = sc(SYS_READDIR, dirfd, (long)&ent, 0)) > 0)
        count++;

    sc(SYS_CLOSEDIR, dirfd, 0, 0);

    if (count > 0)
        report_pass(name);
    else {
        serial_puts("[SMOKE] readdir_root: FAIL (entries=");
        serial_int((int64_t)count);
        serial_puts(" last_ret=");
        serial_int((int64_t)r);
        serial_puts(")\n");
        g_total++;
    }
}

/* -----------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial_puts("[SMOKE] starting smoketest\n");

    test_ticks_advance();
    test_mmap_rw();
    test_shm_rw();
    test_msgq_roundtrip();
    test_fb_acquire();
    test_readdir_root();

    /* Summary line */
    serial_puts("[SMOKE] DONE: ");
    serial_uint((uint64_t)g_pass);
    serial_puts("/");
    serial_uint((uint64_t)g_total);
    serial_puts(" passed\n");

    /* Loop yielding -- kernel expects userspace to never return */
    while (1)
        sc(SYS_YIELD, 0, 0, 0);
}
