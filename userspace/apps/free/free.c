/*
 * free.c -- physical memory summary for the from-scratch x86_64 OS.
 * =================================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own number-formatting helpers. Single self-contained file.
 *
 * Queries SYS_SYSINFO(62): sc(62, &info, 0, 0) -> 0 on success, < 0 on error.
 * Prints total / used / free physical memory in both MiB and KiB, in the
 * familiar `free` layout:
 *
 *                 total       used       free
 *   Mem:           1024        137        887   (MiB)
 *   Mem:        1048576     140288     908288   (KiB)
 *
 * The sysinfo_t layout (total_mem / free_mem / uptime_ms / proc_count) mirrors
 * userspace/lib/aictl/aictl.h EXACTLY. used = total - free (clamped >= 0).
 *
 * --------------------------------------------------------------------------
 * USAGE (argv is provided by crt0):  free  (no options needed)
 *
 * SELF-TEST: SYS_SYSINFO returning >= 0 => "FREE SELFTEST: PASS",
 * otherwise "FREE SELFTEST: FAIL". The report is printed either way.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/free/free.c -o free.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       free.o crt0.o -o build/free
 *   objdump -d build/free | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Freestanding integer types (no stdint.h). Match aictl.h.
 * --------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef unsigned long long u64;

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT     0
#define SYS_WRITE    3
#define SYS_SYSINFO  62   /* sc(62, &info, 0, 0) -> 0 or -errno */

#define FD_STDOUT    1

/* -----------------------------------------------------------------------
 * sysinfo_t -- byte-for-byte mirror of kernel/include/procapi.h (32 bytes).
 * --------------------------------------------------------------------- */
typedef struct {
    u64 total_mem;   /* total physical memory in bytes */
    u64 free_mem;    /* free physical memory in bytes  */
    u64 uptime_ms;   /* milliseconds since boot        */
    u32 proc_count;  /* total live processes           */
    u32 _pad;        /* reserved, always 0             */
} sysinfo_t;

/* -----------------------------------------------------------------------
 * Inline syscall helper.
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding helpers + output buffer.
 * ======================================================================= */
static unsigned long s_strlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}

#define BUF_CAP 2048
static char g_buf[BUF_CAP];
static unsigned long g_pos;

static void buf_putc(char c)
{
    if (g_pos + 1 < BUF_CAP) g_buf[g_pos++] = c;
    g_buf[g_pos] = '\0';
}
static void buf_puts(const char *s)
{
    while (s && *s && g_pos + 1 < BUF_CAP) g_buf[g_pos++] = *s++;
    g_buf[g_pos] = '\0';
}
static void buf_putu(u64 val)
{
    char tmp[24];
    int  i = 0;
    if (val == 0) { buf_putc('0'); return; }
    while (val > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(val % 10ULL));
        val /= 10ULL;
    }
    while (i-- > 0) buf_putc(tmp[i]);
}
/* Append val right-justified in a field of `width` (space padded). */
static void buf_putu_pad(u64 val, int width)
{
    int digits = 1;
    u64 v = val;
    while (v >= 10ULL) { v /= 10ULL; digits++; }
    for (int i = digits; i < width; i++) buf_putc(' ');
    buf_putu(val);
}
static void buf_puts_pad(const char *s, int width)
{
    int n = (int)s_strlen(s);
    for (int i = n; i < width; i++) buf_putc(' ');
    buf_puts(s);
}

static void flush_out(void)
{
    sc(SYS_WRITE, FD_STDOUT, (long)g_buf, (long)g_pos);
    g_pos = 0;
    g_buf[0] = '\0';
}

/* =======================================================================
 *  Build the report. Returns the SYS_SYSINFO return code (>= 0 / < 0).
 * ======================================================================= */
static long free_render(void)
{
    sysinfo_t info;
    /* Zero it so a failed call shows clean zeros. */
    {
        char *p = (char *)&info;
        for (unsigned long i = 0; i < sizeof(info); i++) p[i] = 0;
    }

    long rc = sc(SYS_SYSINFO, (long)&info, 0, 0);

    if (rc < 0) {
        buf_puts("free: SYS_SYSINFO unavailable\n");
        buf_puts("              total       used       free\n");
        buf_puts("  Mem:          n/a        n/a        n/a   (MiB)\n");
        buf_puts("  Mem:          n/a        n/a        n/a   (KiB)\n");
        return rc;
    }

    u64 total_b = info.total_mem;
    u64 free_b  = info.free_mem;
    u64 used_b  = (total_b >= free_b) ? (total_b - free_b) : 0;

    u64 total_kib = total_b / 1024ULL;
    u64 free_kib  = free_b  / 1024ULL;
    u64 used_kib  = used_b  / 1024ULL;

    u64 total_mib = total_b / (1024ULL * 1024ULL);
    u64 free_mib  = free_b  / (1024ULL * 1024ULL);
    u64 used_mib  = used_b  / (1024ULL * 1024ULL);

    buf_puts("              total       used       free\n");

    buf_puts("  Mem:   ");
    buf_putu_pad(total_mib, 10);
    buf_putu_pad(used_mib, 11);
    buf_putu_pad(free_mib, 11);
    buf_puts("   (MiB)\n");

    buf_puts("  Mem:   ");
    buf_putu_pad(total_kib, 10);
    buf_putu_pad(used_kib, 11);
    buf_putu_pad(free_kib, 11);
    buf_puts("   (KiB)\n");

    (void)buf_puts_pad;   /* helper kept for symmetry with sibling tools */
    return rc;
}

/* =======================================================================
 *  Self-test: SYS_SYSINFO >= 0 => PASS.
 * ======================================================================= */
static int free_selftest(void)
{
    buf_puts("FREE SELFTEST: begin\n");
    long rc = free_render();
    if (rc >= 0) {
        buf_puts("FREE SELFTEST: PASS\n");
        flush_out();
        return 0;
    }
    buf_puts("FREE SELFTEST: FAIL\n");
    flush_out();
    return 1;
}

/* =======================================================================
 *  Entry point. crt0 provides _start and calls main(argc, argv).
 *  With args we just print the report; with none we run the self-test.
 * ======================================================================= */
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        (void)free_render();
        flush_out();
        return 0;
    }
    return free_selftest();
}
