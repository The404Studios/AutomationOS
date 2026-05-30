/*
 * qa.c -- QA Self-Test Dashboard (freestanding, ring 3).
 * =======================================================
 *
 * Opens a 640x500 window titled "System QA".  Runs a suite of live
 * system tests and displays each as a row with PASS / FAIL / WARN / N/A
 * status, colored indicator, and a detail string.  The full suite re-runs
 * automatically every ~5 seconds, or on demand via the "Run All" button.
 *
 * Test suite (rows, in order):
 *   SYSCALL-PID    SYS_GETPID(8)  returns > 0
 *   SYSCALL-TICKS  SYS_GET_TICKS_MS(40) monotonically increases (two calls)
 *   MEM-4K         mmap 4 KiB, pattern write+read, munmap
 *   MEM-64K        mmap 64 KiB, pattern write+read across pages, munmap
 *   MEM-256K       mmap 256 KiB, spot-check every 4 KiB page, munmap
 *   PROCLIST       SYS_PROCLIST(44) returns >= 1 process
 *   SYSINFO        SYS_SYSINFO(62) free_mem <= total_mem && uptime > 0
 *   SPAWN/REAP     spawn sbin/clock, kill it; repeat 3x; mem returns ~baseline
 *   CLIPBOARD      SYS_CLIP_SET(63)/GET(64) round-trip a known string
 *   TIME           SYS_GETTIME(42) year in 2020..2030
 *   RNG            SYS_RANDOM(43) two 16-byte draws differ
 *
 * Any test returning -ENOSYS (−38) shows "n/a (not wired)" not FAIL.
 *
 * UI layout (640 x 500):
 *   [  0.. 39]  Header bar  : "System QA" + overall PASS/WARN/FAIL + counts
 *   [ 40.. 72]  Button bar  : "Run All" button + auto-run countdown
 *   [ 73..472]  Test list   : scrollable rows, 26px each
 *   [473..499]  Status bar  : last action / run time
 *
 * Build (ALL flags DIRECTLY on cmdline — no shell variable):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/qa/qa.c -o /tmp/qa.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/qa.o /tmp/wlc.o /tmp/bf.o -o /tmp/qa.elf
 *   objdump -d /tmp/qa.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [QA] <test>=<PASS|FAIL|NA|WARN> <detail>
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* =========================================================================
 * Syscall numbers
 * ======================================================================= */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_SPAWN        16
#define SYS_KILL         26
#define SYS_MMAP         37
#define SYS_MUNMAP       38
#define SYS_GET_TICKS_MS 40
#define SYS_GETTIME      42
#define SYS_RANDOM       43
#define SYS_PROCLIST     44
#define SYS_GETPID       8
#define SYS_SYSINFO      62
#define SYS_CLIP_SET     63
#define SYS_CLIP_GET     64

/* -ENOSYS is the canonical "not wired" sentinel from the kernel. */
#define ENOSYS_NEG  (-38L)

/* =========================================================================
 * Inline syscall helpers (no stack canary — no libc, no __stack_chk_guard)
 * ======================================================================= */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static inline long sc6(long n, long a1, long a2, long a3,
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

/* =========================================================================
 * Minimal freestanding helpers
 * ======================================================================= */
typedef unsigned int  u32;
typedef unsigned long u64;
typedef int           i32;
typedef long          i64;
typedef unsigned char u8;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/* Inline itoa for unsigned decimal, returns pointer to static buf. */
static char _itoa_buf[24];
static const char *uitoa(u64 v)
{
    char *p = _itoa_buf + 23;
    *p = '\0';
    if (v == 0) { *--p = '0'; return p; }
    while (v) { *--p = '0' + (int)(v % 10); v /= 10; }
    return p;
}

/* Signed variant. */
static const char *itoa(i64 v)
{
    if (v < 0) {
        const char *u = uitoa((u64)(-v));
        char *p = _itoa_buf + 23;
        /* The unsigned result is in _itoa_buf; prepend '-' before it. */
        unsigned long len = k_strlen(u);
        /* shift right one char and prepend */
        /* simplest: just re-render */
        u64 uv = (u64)(-v);
        char *q = _itoa_buf + 23;
        *q = '\0';
        while (uv) { *--q = '0' + (int)(uv % 10); uv /= 10; }
        *--q = '-';
        (void)u; (void)len; (void)p;
        return q;
    }
    return uitoa((u64)v);
}

/* k_memset without libc */
static void k_memset(void *dst, int val, unsigned long n)
{
    u8 *d = (u8 *)dst;
    while (n--) *d++ = (u8)val;
}

/* k_memcmp */
static int k_memcmp(const void *a, const void *b, unsigned long n)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

/* k_strcmp */
static int k_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Simple string copy (no overflow — caller ensures buf is large enough) */
static void k_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++));
}

/* Append string to a char buffer (simple, no bounds check beyond caller) */
static char *k_append(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    return dst;
}

/* =========================================================================
 * Window / layout constants
 * ======================================================================= */
#define WIN_W   640
#define WIN_H   500

#define HDR_H    40
#define BTN_BAR_Y 40
#define BTN_BAR_H 33
#define LIST_Y   73
#define LIST_H   400   /* 400 px = up to 15 visible rows @ 26px each */
#define STATUS_Y 473
#define STATUS_H  27

#define ROW_H    26

/* =========================================================================
 * Colors
 * ======================================================================= */
#define C_BG_DARK    0xFF1A1A2Eu
#define C_BG_HDR     0xFF0F3460u
#define C_BG_BTN_BAR 0xFF16213Eu
#define C_BG_STATUS  0xFF0D0D1Bu
#define C_BG_ROW_ODD 0xFF1E1E2Eu
#define C_BG_ROW_EVN 0xFF252540u
#define C_BG_HOVER   0xFF2E3A5Cu
#define C_SEP        0xFF334466u
#define C_WHITE      0xFFFFFFFFu
#define C_GRAY       0xFF8899BBu
#define C_PASS       0xFF22CC66u
#define C_WARN       0xFFFFAA00u
#define C_FAIL       0xFFFF3355u
#define C_NA         0xFF6677AAu
#define C_RUNNING    0xFF55AAFFu
#define C_BTN_FACE   0xFF1A4A8Au
#define C_BTN_HOV    0xFF2260B0u
#define C_BTN_PRESS  0xFF0A2A6Au
#define C_ACCENT     0xFF4488FFu

/* =========================================================================
 * Drawing primitives
 * ======================================================================= */
static void fill_rect(u32 *buf, i32 x, i32 y, i32 w, i32 h, u32 color)
{
    for (i32 yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= WIN_H) continue;
        for (i32 xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= WIN_W) continue;
            buf[yy * WIN_W + xx] = color;
        }
    }
}

static void hline(u32 *buf, i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, x, y, len, 1, color);
}

static void draw_border(u32 *buf, i32 x, i32 y, i32 w, i32 h, u32 color)
{
    hline(buf, x,         y,         w, color);
    hline(buf, x,         y + h - 1, w, color);
    fill_rect(buf, x,         y, 1, h, color);
    fill_rect(buf, x + w - 1, y, 1, h, color);
}

static void draw_str(u32 *buf, i32 x, i32 y, const char *s, u32 color)
{
    font_draw_string(buf, WIN_W, WIN_W, WIN_H, x, y, s, color);
}

/* Right-aligned string within a box (x_right is the right edge). */
static void draw_str_right(u32 *buf, i32 x_right, i32 y,
                            const char *s, u32 color)
{
    i32 w = (i32)(k_strlen(s) * FONT_W);
    draw_str(buf, x_right - w, y, s, color);
}

/* =========================================================================
 * sysinfo_t / rtc_time_t / proc_entry_t (must match kernel ABI exactly)
 * ======================================================================= */
typedef struct {
    u64 total_mem;
    u64 free_mem;
    u64 uptime_ms;
    u32 proc_count;
    u32 _pad;
} sysinfo_t;

typedef struct {
    unsigned short year;
    unsigned char  month;
    unsigned char  day;
    unsigned char  hour;
    unsigned char  min;
    unsigned char  sec;
} rtc_time_t;

/* Minimal proc entry returned by SYS_PROCLIST (32-byte compact form used
 * by most sysmon implementations; we only need the pid field, so we use
 * a struct large enough to not overflow the kernel's write). */
typedef struct {
    u32 pid;
    u32 ppid;
    u32 state;
    u32 prio;
    u64 cpu_ticks;
    u32 mem_pages;
    u32 vma_count;
    char name[32];
} proc_entry_t;

/* =========================================================================
 * Test result state
 * ======================================================================= */
#define STATUS_PENDING  0
#define STATUS_RUNNING  1
#define STATUS_PASS     2
#define STATUS_WARN     3
#define STATUS_FAIL     4
#define STATUS_NA       5

#define NUM_TESTS  11

typedef struct {
    const char *name;           /* short label, max ~14 chars */
    int         status;         /* STATUS_* */
    char        detail[64];     /* detail string shown on the row */
} test_result_t;

static test_result_t g_tests[NUM_TESTS] = {
    { "SYSCALL-PID",   STATUS_PENDING, "pending" },
    { "SYSCALL-TICKS", STATUS_PENDING, "pending" },
    { "MEM-4K",        STATUS_PENDING, "pending" },
    { "MEM-64K",       STATUS_PENDING, "pending" },
    { "MEM-256K",      STATUS_PENDING, "pending" },
    { "PROCLIST",      STATUS_PENDING, "pending" },
    { "SYSINFO",       STATUS_PENDING, "pending" },
    { "SPAWN/REAP",    STATUS_PENDING, "pending" },
    { "CLIPBOARD",     STATUS_PENDING, "pending" },
    { "TIME",          STATUS_PENDING, "pending" },
    { "RNG",           STATUS_PENDING, "pending" },
};

/* Running counts (updated after each full suite run). */
static int g_n_pass, g_n_warn, g_n_fail, g_n_na;

/* =========================================================================
 * Serial logging
 * ======================================================================= */
static void qa_log(const char *test, const char *status, const char *detail)
{
    /* "[QA] test=STATUS detail\n" */
    char buf[128];
    char *p = buf;
    p = k_append(p, "[QA] ");
    p = k_append(p, test);
    p = k_append(p, "=");
    p = k_append(p, status);
    p = k_append(p, " ");
    p = k_append(p, detail);
    p = k_append(p, "\n");
    *p = '\0';
    print(buf);
}

static void result_set(int idx, int status, const char *detail)
{
    g_tests[idx].status = status;
    k_strcpy(g_tests[idx].detail, detail);
    const char *sname;
    switch (status) {
        case STATUS_PASS: sname = "PASS"; break;
        case STATUS_WARN: sname = "WARN"; break;
        case STATUS_FAIL: sname = "FAIL"; break;
        case STATUS_NA:   sname = "NA";   break;
        default:          sname = "RUNNING"; break;
    }
    qa_log(g_tests[idx].name, sname, detail);
}

/* =========================================================================
 * Pattern fill / verify helpers for memory test
 * ======================================================================= */
static int mem_pattern_test(void *addr, unsigned long len)
{
    /* Write a simple byte pattern: addr_byte XOR position byte. */
    u8 *p = (u8 *)addr;
    for (unsigned long i = 0; i < len; i++)
        p[i] = (u8)(i ^ 0xA5u);
    /* Read back and verify. */
    for (unsigned long i = 0; i < len; i++) {
        if (p[i] != (u8)(i ^ 0xA5u))
            return 0; /* mismatch */
    }
    return 1; /* ok */
}

/* Spot-check every 4096 bytes (one byte per page) for large buffers. */
static int mem_pattern_spot(void *addr, unsigned long len)
{
    u8 *p = (u8 *)addr;
    /* Write one byte per page. */
    for (unsigned long off = 0; off < len; off += 4096)
        p[off] = (u8)(off ^ 0x5Au);
    /* Verify. */
    for (unsigned long off = 0; off < len; off += 4096) {
        if (p[off] != (u8)(off ^ 0x5Au))
            return 0;
    }
    return 1;
}

/* =========================================================================
 * Individual test runners
 * ======================================================================= */

/* --- T0: SYSCALL-PID --- */
static void run_syscall_pid(void)
{
    long pid = sc(SYS_GETPID, 0, 0, 0);
    if (pid == ENOSYS_NEG) {
        result_set(0, STATUS_NA, "not wired");
        return;
    }
    if (pid > 0) {
        char buf[32];
        char *p = buf;
        p = k_append(p, "pid=");
        p = k_append(p, itoa(pid));
        *p = '\0';
        result_set(0, STATUS_PASS, buf);
    } else {
        char buf[32];
        char *p = buf;
        p = k_append(p, "got ");
        p = k_append(p, itoa(pid));
        *p = '\0';
        result_set(0, STATUS_FAIL, buf);
    }
}

/* --- T1: SYSCALL-TICKS --- */
static void run_syscall_ticks(void)
{
    long t0 = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    if (t0 == ENOSYS_NEG) {
        result_set(1, STATUS_NA, "not wired");
        return;
    }
    /* Small busy-loop to ensure some ticks pass. */
    for (volatile int i = 0; i < 100000; i++);
    long t1 = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    if (t1 >= t0) {
        char buf[48];
        char *p = buf;
        p = k_append(p, "t0=");
        p = k_append(p, itoa(t0));
        p = k_append(p, " t1=");
        p = k_append(p, itoa(t1));
        *p = '\0';
        result_set(1, STATUS_PASS, buf);
    } else {
        result_set(1, STATUS_FAIL, "ticks went backward");
    }
}

/* --- T2: MEM-4K --- */
static void run_mem_4k(void)
{
    long addr = sc6(SYS_MMAP, 0, 4096, 3, 0, 0, 0);
    if (addr == ENOSYS_NEG) { result_set(2, STATUS_NA, "not wired"); return; }
    if (addr <= 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "mmap err="); p = k_append(p, itoa(addr)); *p = '\0';
        result_set(2, STATUS_FAIL, buf); return;
    }
    int ok = mem_pattern_test((void *)addr, 4096);
    sc6(SYS_MUNMAP, addr, 4096, 0, 0, 0, 0);
    result_set(2, ok ? STATUS_PASS : STATUS_FAIL,
               ok ? "4096B pattern ok" : "pattern mismatch");
}

/* --- T3: MEM-64K --- */
static void run_mem_64k(void)
{
    long addr = sc6(SYS_MMAP, 0, 65536, 3, 0, 0, 0);
    if (addr == ENOSYS_NEG) { result_set(3, STATUS_NA, "not wired"); return; }
    if (addr <= 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "mmap err="); p = k_append(p, itoa(addr)); *p = '\0';
        result_set(3, STATUS_FAIL, buf); return;
    }
    int ok = mem_pattern_spot((void *)addr, 65536);
    sc6(SYS_MUNMAP, addr, 65536, 0, 0, 0, 0);
    result_set(3, ok ? STATUS_PASS : STATUS_FAIL,
               ok ? "64KiB spot-check ok" : "pattern mismatch");
}

/* --- T4: MEM-256K --- */
static void run_mem_256k(void)
{
    long addr = sc6(SYS_MMAP, 0, 262144, 3, 0, 0, 0);
    if (addr == ENOSYS_NEG) { result_set(4, STATUS_NA, "not wired"); return; }
    if (addr <= 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "mmap err="); p = k_append(p, itoa(addr)); *p = '\0';
        result_set(4, STATUS_FAIL, buf); return;
    }
    int ok = mem_pattern_spot((void *)addr, 262144);
    sc6(SYS_MUNMAP, addr, 262144, 0, 0, 0, 0);
    result_set(4, ok ? STATUS_PASS : STATUS_FAIL,
               ok ? "256KiB spot-check ok" : "pattern mismatch");
}

/* --- T5: PROCLIST --- */
static void run_proclist(void)
{
    proc_entry_t entries[32];
    k_memset(entries, 0, sizeof(entries));
    long n = sc6(SYS_PROCLIST, (long)entries, 32, 0, 0, 0, 0);
    if (n == ENOSYS_NEG) { result_set(5, STATUS_NA, "not wired"); return; }
    if (n < 1) {
        char buf[32]; char *p = buf;
        p = k_append(p, "got n="); p = k_append(p, itoa(n)); *p = '\0';
        result_set(5, STATUS_FAIL, buf); return;
    }
    char buf[32]; char *p = buf;
    p = k_append(p, "n="); p = k_append(p, itoa(n)); *p = '\0';
    result_set(5, STATUS_PASS, buf);
}

/* --- T6: SYSINFO --- */
static void run_sysinfo(void)
{
    sysinfo_t si;
    k_memset(&si, 0, sizeof(si));
    long r = sc(SYS_SYSINFO, (long)&si, 0, 0);
    if (r == ENOSYS_NEG) { result_set(6, STATUS_NA, "not wired"); return; }
    if (r != 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "err="); p = k_append(p, itoa(r)); *p = '\0';
        result_set(6, STATUS_FAIL, buf); return;
    }
    /* Sanity: free <= total and uptime > 0. */
    if (si.free_mem > si.total_mem) {
        result_set(6, STATUS_FAIL, "free>total"); return;
    }
    if (si.uptime_ms == 0) {
        result_set(6, STATUS_FAIL, "uptime=0"); return;
    }
    char buf[64]; char *p = buf;
    p = k_append(p, "up="); p = k_append(p, uitoa(si.uptime_ms / 1000)); p = k_append(p, "s");
    p = k_append(p, " free="); p = k_append(p, uitoa(si.free_mem >> 20)); p = k_append(p, "MB");
    *p = '\0';
    result_set(6, STATUS_PASS, buf);
}

/* --- T7: SPAWN/REAP --- */
#define SPAWN_CYCLES  3
static void run_spawn_reap(void)
{
    /* Self PID — never kill this. */
    long self = sc(SYS_GETPID, 0, 0, 0);

    /* Baseline memory before spawning. */
    sysinfo_t si_before, si_after;
    k_memset(&si_before, 0, sizeof(si_before));
    k_memset(&si_after,  0, sizeof(si_after));
    long si_ok = sc(SYS_SYSINFO, (long)&si_before, 0, 0);
    long baseline = (si_ok == 0) ? (long)si_before.free_mem : -1;

    int spawned = 0, killed = 0;
    int spawn_failed = 0;

    for (int i = 0; i < SPAWN_CYCLES; i++) {
        long pid = sc(SYS_SPAWN, (long)"sbin/clock", 0, 0);
        if (pid == ENOSYS_NEG) {
            result_set(7, STATUS_NA, "SYS_SPAWN not wired");
            return;
        }
        if (pid <= 0) { spawn_failed = 1; break; }
        spawned++;

        /* Guard: never kill pid 0, 1, or self. */
        if (pid <= 1 || pid == self) continue;

        /* Small yield before kill — let the process initialise. */
        for (int j = 0; j < 3; j++)
            sc(SYS_YIELD, 0, 0, 0);

        long kr = sc(SYS_KILL, pid, 9, 0);
        (void)kr;
        killed++;

        /* Yield again to allow teardown. */
        for (int j = 0; j < 5; j++)
            sc(SYS_YIELD, 0, 0, 0);
    }

    if (spawn_failed) {
        result_set(7, STATUS_FAIL, "spawn returned <=0"); return;
    }

    /* Check memory after cycles. */
    si_ok = sc(SYS_SYSINFO, (long)&si_after, 0, 0);

    char buf[64]; char *p = buf;
    p = k_append(p, "spawn="); p = k_append(p, itoa(spawned));
    p = k_append(p, " kill="); p = k_append(p, itoa(killed));

    if (baseline > 0 && si_ok == 0) {
        long after = (long)si_after.free_mem;
        /* Allow up to 512 KiB of "slop" (kernel bookkeeping). */
        long slop = 512L * 1024L;
        long lost = baseline - after;
        if (lost > slop) {
            /* Memory still dropping — possible leak. */
            p = k_append(p, " LEAK~"); p = k_append(p, itoa(lost >> 10)); p = k_append(p, "KB");
            *p = '\0';
            result_set(7, STATUS_WARN, buf);
            return;
        }
        p = k_append(p, " memdelta="); p = k_append(p, itoa(lost >> 10)); p = k_append(p, "KB");
    }
    *p = '\0';
    result_set(7, STATUS_PASS, buf);
}

/* --- T8: CLIPBOARD --- */
static const char CLIP_PROBE[] = "QA_CLIP_TEST_42!";
static void run_clipboard(void)
{
    long sr = sc6(SYS_CLIP_SET, (long)CLIP_PROBE, (long)k_strlen(CLIP_PROBE), 0, 0, 0, 0);
    if (sr == ENOSYS_NEG) { result_set(8, STATUS_NA, "not wired"); return; }
    if (sr < 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "set err="); p = k_append(p, itoa(sr)); *p = '\0';
        result_set(8, STATUS_FAIL, buf); return;
    }

    char readback[64];
    k_memset(readback, 0, sizeof(readback));
    long gr = sc6(SYS_CLIP_GET, (long)readback, (long)sizeof(readback) - 1, 0, 0, 0, 0);
    if (gr == ENOSYS_NEG) { result_set(8, STATUS_NA, "not wired"); return; }
    if (gr < 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "get err="); p = k_append(p, itoa(gr)); *p = '\0';
        result_set(8, STATUS_FAIL, buf); return;
    }

    if (k_strcmp(readback, CLIP_PROBE) == 0) {
        result_set(8, STATUS_PASS, "round-trip ok");
    } else {
        result_set(8, STATUS_FAIL, "data mismatch");
    }
}

/* --- T9: TIME --- */
static void run_time(void)
{
    rtc_time_t t;
    k_memset(&t, 0, sizeof(t));
    long r = sc(SYS_GETTIME, (long)&t, 0, 0);
    if (r == ENOSYS_NEG) { result_set(9, STATUS_NA, "not wired"); return; }
    if (r != 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "err="); p = k_append(p, itoa(r)); *p = '\0';
        result_set(9, STATUS_FAIL, buf); return;
    }
    if (t.year < 2020 || t.year > 2030) {
        char buf[48]; char *p = buf;
        p = k_append(p, "implausible year=");
        p = k_append(p, uitoa(t.year));
        *p = '\0';
        result_set(9, STATUS_FAIL, buf); return;
    }
    char buf[48]; char *p = buf;
    p = k_append(p, uitoa(t.year)); p = k_append(p, "-");
    if (t.month < 10) { p = k_append(p, "0"); }
    p = k_append(p, uitoa(t.month)); p = k_append(p, "-");
    if (t.day < 10) { p = k_append(p, "0"); }
    p = k_append(p, uitoa(t.day));
    p = k_append(p, " ");
    if (t.hour < 10) { p = k_append(p, "0"); }
    p = k_append(p, uitoa(t.hour)); p = k_append(p, ":");
    if (t.min < 10) { p = k_append(p, "0"); }
    p = k_append(p, uitoa(t.min));
    *p = '\0';
    result_set(9, STATUS_PASS, buf);
}

/* --- T10: RNG --- */
static void run_rng(void)
{
    u8 buf_a[16], buf_b[16];
    k_memset(buf_a, 0, 16);
    k_memset(buf_b, 0, 16);

    long ra = sc(SYS_RANDOM, (long)buf_a, 16, 0);
    if (ra == ENOSYS_NEG) { result_set(10, STATUS_NA, "not wired"); return; }
    if (ra < 0) {
        char buf[32]; char *p = buf;
        p = k_append(p, "err="); p = k_append(p, itoa(ra)); *p = '\0';
        result_set(10, STATUS_FAIL, buf); return;
    }
    for (volatile int i = 0; i < 10000; i++);
    long rb = sc(SYS_RANDOM, (long)buf_b, 16, 0);
    if (rb < 0) {
        result_set(10, STATUS_FAIL, "second draw failed"); return;
    }

    /* The two 16-byte draws must differ (collision probability ~2^-128). */
    if (k_memcmp(buf_a, buf_b, 16) != 0) {
        result_set(10, STATUS_PASS, "two draws differ");
    } else {
        result_set(10, STATUS_FAIL, "draws identical (broken RNG)");
    }
}

/* =========================================================================
 * Full suite runner
 * ======================================================================= */
static void run_all_tests(void)
{
    /* Mark all as running before we begin so the display shows activity. */
    for (int i = 0; i < NUM_TESTS; i++)
        g_tests[i].status = STATUS_RUNNING;

    run_syscall_pid();
    run_syscall_ticks();
    run_mem_4k();
    run_mem_64k();
    run_mem_256k();
    run_proclist();
    run_sysinfo();
    run_spawn_reap();
    run_clipboard();
    run_time();
    run_rng();

    /* Tally counts. */
    g_n_pass = g_n_warn = g_n_fail = g_n_na = 0;
    for (int i = 0; i < NUM_TESTS; i++) {
        switch (g_tests[i].status) {
            case STATUS_PASS: g_n_pass++; break;
            case STATUS_WARN: g_n_warn++; break;
            case STATUS_FAIL: g_n_fail++; break;
            case STATUS_NA:   g_n_na++;   break;
            default: break;
        }
    }
}

/* =========================================================================
 * UI rendering
 * ======================================================================= */

/* Visible test rows (scroll offset). */
static int g_scroll_offset = 0;        /* first visible row index */
#define MAX_VISIBLE  ((LIST_H) / ROW_H) /* 15 */

/* Auto-run interval: ~5 seconds.  We compare against GET_TICKS_MS. */
#define AUTO_INTERVAL_MS  5000

static long g_last_run_ms  = 0;
static long g_run_count    = 0;

/* Button state for "Run All" */
#define BTN_RUN_X   8
#define BTN_RUN_Y   (BTN_BAR_Y + 5)
#define BTN_RUN_W   90
#define BTN_RUN_H   22

/* Scroll bar (right edge of list). */
#define SCROLL_X    (WIN_W - 12)
#define SCROLL_W    10

static u32 color_for_status(int status)
{
    switch (status) {
        case STATUS_PASS:    return C_PASS;
        case STATUS_WARN:    return C_WARN;
        case STATUS_FAIL:    return C_FAIL;
        case STATUS_NA:      return C_NA;
        case STATUS_RUNNING: return C_RUNNING;
        default:             return C_GRAY;
    }
}

static const char *label_for_status(int status)
{
    switch (status) {
        case STATUS_PASS:    return "PASS";
        case STATUS_WARN:    return "WARN";
        case STATUS_FAIL:    return "FAIL";
        case STATUS_NA:      return "N/A ";
        case STATUS_RUNNING: return "... ";
        default:             return "----";
    }
}

static int g_btn_pressed = 0;  /* "Run All" button pressed state */

static void render_frame(u32 *buf, long now_ms)
{
    /* ---- Clear ---- */
    fill_rect(buf, 0, 0, WIN_W, WIN_H, C_BG_DARK);

    /* ---- Header bar ---- */
    fill_rect(buf, 0, 0, WIN_W, HDR_H, C_BG_HDR);
    draw_str(buf, 8, 12, "System QA", C_WHITE);

    /* Overall status badge */
    int overall;
    u32 badge_color;
    const char *badge_label;
    if (g_n_fail > 0) {
        overall = STATUS_FAIL; badge_color = C_FAIL; badge_label = "FAIL";
    } else if (g_n_warn > 0) {
        overall = STATUS_WARN; badge_color = C_WARN; badge_label = "WARN";
    } else if (g_n_pass > 0 && (g_n_pass + g_n_na) == NUM_TESTS) {
        overall = STATUS_PASS; badge_color = C_PASS; badge_label = "PASS";
    } else {
        overall = STATUS_PENDING; badge_color = C_GRAY; badge_label = "----";
    }
    (void)overall;

    /* Badge box */
    fill_rect(buf, WIN_W - 80, 8, 72, 24, badge_color);
    draw_str(buf, WIN_W - 72, 12, badge_label, 0xFF000000u);

    /* Count summary */
    {
        char summary[64];
        char *p = summary;
        p = k_append(p, "P:"); p = k_append(p, itoa(g_n_pass));
        p = k_append(p, " W:"); p = k_append(p, itoa(g_n_warn));
        p = k_append(p, " F:"); p = k_append(p, itoa(g_n_fail));
        p = k_append(p, " N:"); p = k_append(p, itoa(g_n_na));
        *p = '\0';
        draw_str(buf, WIN_W - 250, 13, summary, C_GRAY);
    }

    /* ---- Button bar ---- */
    fill_rect(buf, 0, BTN_BAR_Y, WIN_W, BTN_BAR_H, C_BG_BTN_BAR);
    hline(buf, 0, BTN_BAR_Y + BTN_BAR_H - 1, WIN_W, C_SEP);

    /* "Run All" button */
    u32 btn_face = g_btn_pressed ? C_BTN_PRESS : C_BTN_FACE;
    fill_rect(buf, BTN_RUN_X, BTN_RUN_Y, BTN_RUN_W, BTN_RUN_H, btn_face);
    draw_border(buf, BTN_RUN_X, BTN_RUN_Y, BTN_RUN_W, BTN_RUN_H, C_ACCENT);
    draw_str(buf, BTN_RUN_X + 16, BTN_RUN_Y + 3, "Run All", C_WHITE);

    /* Auto-run countdown */
    {
        long next_ms = g_last_run_ms + AUTO_INTERVAL_MS;
        long remaining = (next_ms - now_ms) / 1000;
        if (remaining < 0) remaining = 0;
        char cbuf[32];
        char *p = cbuf;
        p = k_append(p, "auto in "); p = k_append(p, itoa(remaining));
        p = k_append(p, "s   run#"); p = k_append(p, itoa(g_run_count));
        *p = '\0';
        draw_str(buf, BTN_RUN_X + BTN_RUN_W + 12, BTN_RUN_Y + 3, cbuf, C_GRAY);
    }

    /* ---- Test list ---- */
    fill_rect(buf, 0, LIST_Y, WIN_W, LIST_H, C_BG_DARK);
    hline(buf, 0, LIST_Y, WIN_W, C_SEP);

    /* Column header */
    {
        int hy = LIST_Y + 1;
        fill_rect(buf, 0, hy, WIN_W - SCROLL_W - 2, FONT_H + 4, C_BG_BTN_BAR);
        draw_str(buf,   6, hy + 2, "TEST",   C_GRAY);
        draw_str(buf, 148, hy + 2, "STATUS", C_GRAY);
        draw_str(buf, 228, hy + 2, "DETAIL", C_GRAY);
        hline(buf, 0, hy + FONT_H + 3, WIN_W, C_SEP);
    }
    int list_content_y = LIST_Y + FONT_H + 6;

    int max_visible = (LIST_H - FONT_H - 6) / ROW_H;
    if (max_visible > NUM_TESTS) max_visible = NUM_TESTS;

    /* Clamp scroll. */
    int max_scroll = NUM_TESTS - max_visible;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;
    if (g_scroll_offset < 0) g_scroll_offset = 0;

    for (int vi = 0; vi < max_visible; vi++) {
        int ti = vi + g_scroll_offset;
        if (ti >= NUM_TESTS) break;

        int ry = list_content_y + vi * ROW_H;

        /* Row background */
        u32 row_bg = (vi & 1) ? C_BG_ROW_ODD : C_BG_ROW_EVN;
        fill_rect(buf, 0, ry, WIN_W - SCROLL_W - 2, ROW_H, row_bg);

        /* Status indicator bar (left edge, 4px wide) */
        u32 ind_color = color_for_status(g_tests[ti].status);
        fill_rect(buf, 0, ry, 4, ROW_H, ind_color);

        /* Test name */
        draw_str(buf, 10, ry + 5, g_tests[ti].name, C_WHITE);

        /* Status label with color */
        const char *slabel = label_for_status(g_tests[ti].status);
        fill_rect(buf, 145, ry + 3, 72, ROW_H - 6, ind_color);
        draw_str(buf, 149, ry + 5, slabel, 0xFF000000u);

        /* Detail text */
        draw_str(buf, 228, ry + 5, g_tests[ti].detail, C_GRAY);

        /* Row separator */
        hline(buf, 0, ry + ROW_H - 1, WIN_W - SCROLL_W - 2, C_SEP);
    }

    /* ---- Scroll bar ---- */
    if (NUM_TESTS > max_visible && max_visible > 0) {
        int sb_h = LIST_H - FONT_H - 6;
        fill_rect(buf, SCROLL_X, list_content_y, SCROLL_W, sb_h, C_BG_BTN_BAR);
        /* Thumb */
        int thumb_h = sb_h * max_visible / NUM_TESTS;
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = list_content_y + (sb_h - thumb_h) * g_scroll_offset / (NUM_TESTS > max_visible ? NUM_TESTS - max_visible : 1);
        fill_rect(buf, SCROLL_X, thumb_y, SCROLL_W, thumb_h, C_ACCENT);
    }

    /* ---- Status bar ---- */
    fill_rect(buf, 0, STATUS_Y, WIN_W, STATUS_H, C_BG_STATUS);
    hline(buf, 0, STATUS_Y, WIN_W, C_SEP);

    {
        char sbuf[80];
        char *p = sbuf;
        p = k_append(p, "Last run: ");
        if (g_last_run_ms > 0) {
            p = k_append(p, uitoa(g_last_run_ms / 1000)); p = k_append(p, "s uptime");
        } else {
            p = k_append(p, "not yet run");
        }
        p = k_append(p, "   Tests: "); p = k_append(p, itoa(NUM_TESTS));
        *p = '\0';
        draw_str(buf, 8, STATUS_Y + 6, sbuf, C_GRAY);
    }
}

/* =========================================================================
 * Entry point
 * ======================================================================= */
void _start(void)
{
    print("[QA] starting\n");

    if (wl_connect() != 0) {
        print("[QA] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "System QA");
    if (!win) {
        print("[QA] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    print("[QA] window created\n");

    /* Run first suite immediately. */
    run_all_tests();
    g_last_run_ms = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    if (g_last_run_ms < 0) g_last_run_ms = 0;
    g_run_count++;

    for (;;) {
        long now_ms = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        if (now_ms < 0) now_ms = 0;

        /* Auto-rerun every AUTO_INTERVAL_MS. */
        if (now_ms - g_last_run_ms >= AUTO_INTERVAL_MS) {
            run_all_tests();
            g_last_run_ms = now_ms;
            g_run_count++;
        }

        /* Poll input events. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                int mx = ea, my = eb;
                int btn = (ec & 1);

                /* "Run All" button click. */
                if (btn &&
                    mx >= BTN_RUN_X && mx < BTN_RUN_X + BTN_RUN_W &&
                    my >= BTN_RUN_Y && my < BTN_RUN_Y + BTN_RUN_H) {
                    g_btn_pressed = 1;
                    run_all_tests();
                    g_last_run_ms = now_ms;
                    g_run_count++;
                } else if (!btn) {
                    g_btn_pressed = 0;
                }

                /* Scroll wheel / drag via pointer Y in list area. */
                /* Simple: clicking top-quarter of list scrolls up,
                 *         bottom-quarter scrolls down (touch-friendly). */
                if (btn && my >= LIST_Y && my < STATUS_Y) {
                    int list_mid = LIST_Y + LIST_H / 2;
                    if (my < list_mid - LIST_H / 4)
                        g_scroll_offset--;
                    else if (my > list_mid + LIST_H / 4)
                        g_scroll_offset++;
                }
            }
            if (kind == WL_EVENT_KEY && ec /* pressed */) {
                /* Up/Down arrow keys: keycodes 72 (up) and 80 (down). */
                if (ea == 72) g_scroll_offset--;
                if (ea == 80) g_scroll_offset++;
            }
        }

        render_frame(win->pixels, now_ms);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0);
    }
}
