/*
 * perftest.c -- Ring-3 test for SYS_PERF_REPORT (72)
 *
 * ABI (from kernel/core/syscall/handlers.c :: sys_perf_report,
 *       kernel/lib/perf.c :: perf_report,
 *       kernel/include/syscall.h line 282):
 *
 *   int64_t sys_perf_report(arg1..arg6 -- all ignored)
 *
 *   Effect  : calls perf_report() which dumps RDTSC-based per-operation
 *             statistics (syscall, context_switch, slab_alloc, ...) to the
 *             kernel serial/kprintf console.
 *   Returns : 0 always (unconditional return 0 in handler).
 *
 * Test strategy:
 *   1. Call SYS_PERF_REPORT with zero arguments.
 *   2. Verify the return value is >= 0 (kernel contract: always 0).
 *   3. Print PASS or FAIL accordingly.
 *
 * Build:
 *   x86_64-elf-gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *     -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign \
 *     -O2 -o perftest perftest.c
 */

/* ── fixed-width types (no libc) ─────────────────────────────────────── */
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef unsigned long      size_t;

/* ── syscall numbers ─────────────────────────────────────────────────── */
#define SYS_EXIT        0
#define SYS_WRITE       3
#define SYS_PERF_REPORT 72

/* ── 6-argument inline syscall wrapper ───────────────────────────────── */
static inline int64_t sc(uint64_t nr,
                          uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6)
{
    int64_t ret;
    register uint64_t r10 asm("r10") = a4;
    register uint64_t r8  asm("r8")  = a5;
    register uint64_t r9  asm("r9")  = a6;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* ── tiny helpers ────────────────────────────────────────────────────── */
static void write_str(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    sc(SYS_WRITE, 1, (uint64_t)s, len, 0, 0, 0);
}

static char *i64toa(int64_t v, char *buf, int bufsz)
{
    char tmp[24];
    int neg = 0, i = 0, j;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) tmp[i++] = '-';
    j = 0;
    if (i >= bufsz) i = bufsz - 1;
    while (i-- > 0) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

/* ── test ────────────────────────────────────────────────────────────── */
void _start(void)
{
    char numbuf[24];
    int64_t ret;

    write_str("PERFTEST: calling SYS_PERF_REPORT\n");

    /*
     * All six arguments are ignored by the kernel handler.
     * Pass zeros; the syscall simply triggers perf_report() which
     * writes the statistics banner to the kernel console (kprintf/serial).
     */
    ret = sc(SYS_PERF_REPORT, 0, 0, 0, 0, 0, 0);

    write_str("PERFTEST: SYS_PERF_REPORT returned ");
    write_str(i64toa(ret, numbuf, sizeof numbuf));
    write_str("\n");

    /* Contract: handler always returns 0 */
    if (ret >= 0) {
        write_str("PERFTEST: PASS\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    } else {
        write_str("PERFTEST: FAIL (expected >= 0, got ");
        write_str(i64toa(ret, numbuf, sizeof numbuf));
        write_str(")\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }

    /* unreachable */
    while (1) {}
}
