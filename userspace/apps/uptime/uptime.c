/*
 * uptime.c -- time-since-boot reporter for the from-scratch x86_64 OS.
 * ====================================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own number-formatting helpers. Single self-contained file.
 *
 * Queries SYS_SYSINFO(62): sc(62, &info, 0, 0) -> 0 on success, < 0 on error.
 * Renders info.uptime_ms as HH:MM:SS (with day count when it overflows 24h)
 * and prints the live process count alongside:
 *
 *   up 01:23:45,  7 processes
 *
 * The sysinfo_t layout (total_mem / free_mem / uptime_ms / proc_count) mirrors
 * userspace/lib/aictl/aictl.h EXACTLY.
 *
 * --------------------------------------------------------------------------
 * USAGE (argv is provided by crt0):  uptime  (no options needed)
 *
 * SELF-TEST: SYS_SYSINFO returning >= 0 (so uptime is readable, >= 0) =>
 * "UPTIME SELFTEST: PASS", otherwise "UPTIME SELFTEST: FAIL". The report is
 * printed either way.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/uptime/uptime.c -o uptime.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       uptime.o crt0.o -o build/uptime
 *   objdump -d build/uptime | grep fs:0x28   # MUST be empty
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
 * sysinfo_t -- byte-for-byte mirror of userspace/lib/aictl/aictl.h.
 * --------------------------------------------------------------------- */
typedef struct {
    u64 total_mem;   /* total physical memory in bytes */
    u64 free_mem;    /* free physical memory in bytes  */
    u64 uptime_ms;   /* milliseconds since boot        */
    u32 proc_count;  /* total live processes           */
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

#define BUF_CAP 512
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
/* Two-digit zero-padded field (for HH:MM:SS). */
static void buf_put2(u64 val)
{
    buf_putc((char)('0' + (int)((val / 10ULL) % 10ULL)));
    buf_putc((char)('0' + (int)(val % 10ULL)));
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
static long uptime_render(void)
{
    sysinfo_t info;
    {
        char *p = (char *)&info;
        for (unsigned long i = 0; i < sizeof(info); i++) p[i] = 0;
    }

    long rc = sc(SYS_SYSINFO, (long)&info, 0, 0);
    if (rc < 0) {
        buf_puts("uptime: SYS_SYSINFO unavailable\n");
        return rc;
    }

    u64 total_s = info.uptime_ms / 1000ULL;
    u64 days    = total_s / 86400ULL;
    u64 hh      = (total_s % 86400ULL) / 3600ULL;
    u64 mm      = (total_s % 3600ULL) / 60ULL;
    u64 ss      = total_s % 60ULL;

    buf_puts("up ");
    if (days > 0) {
        buf_putu(days);
        buf_puts(days == 1 ? " day, " : " days, ");
    }
    buf_put2(hh);
    buf_putc(':');
    buf_put2(mm);
    buf_putc(':');
    buf_put2(ss);

    buf_puts(",  ");
    buf_putu((u64)info.proc_count);
    buf_puts(info.proc_count == 1 ? " process\n" : " processes\n");

    return rc;
}

/* =======================================================================
 *  Self-test: SYS_SYSINFO >= 0 (uptime readable, >= 0) => PASS.
 * ======================================================================= */
static int uptime_selftest(void)
{
    buf_puts("UPTIME SELFTEST: begin\n");
    long rc = uptime_render();
    if (rc >= 0) {
        buf_puts("UPTIME SELFTEST: PASS\n");
        flush_out();
        return 0;
    }
    buf_puts("UPTIME SELFTEST: FAIL\n");
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
        (void)uptime_render();
        flush_out();
        return 0;
    }
    return uptime_selftest();
}
