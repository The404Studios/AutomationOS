/*
 * date.c -- current date/time reporter for AutomationOS (x86_64).
 * ===============================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own helpers. Single self-contained file. Mirrors the structure of the
 * sibling tools userspace/apps/uptime/uptime.c and free/free.c.
 *
 * Time source: SYS_TIME (41) -- sc(41, 0, 0, 0, 0, 0) -> seconds since the
 * Unix epoch (rtc_unix_time, int64_t). The epoch seconds are converted to a
 * UTC calendar string with our own days-from-civil inverse (Howard Hinnant's
 * public-domain algorithm), and printed as:
 *
 *   YYYY-MM-DD HH:MM:SS UTC
 *
 * --------------------------------------------------------------------------
 * USAGE:  date            -> full UTC timestamp
 *         date +FORMAT    -> the FORMAT operand is accepted; we still emit
 *                            the full timestamp (a minimal compatibility nod;
 *                            strftime is out of scope for a freestanding tool)
 *
 * SELF-TEST (argc <= 1): PASS iff SYS_TIME returns a plausible epoch (> 0) and
 * the conversion yields a 4-digit year (1000..9999). The timestamp is printed
 * either way; main returns 0 on PASS, 1 on FAIL.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/date/date.c -o date.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       date.o crt0.o -o build/date
 *   objdump -d build/date | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Freestanding integer types (no stdint.h).
 * --------------------------------------------------------------------- */
typedef unsigned long long u64;

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE    3
#define SYS_TIME     41   /* sc(41,...) -> int64_t Unix epoch seconds */

#define FD_STDOUT    1

/* -----------------------------------------------------------------------
 * Inline syscall helper (6-arg form per the project ABI).
 * --------------------------------------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding helpers + output buffer.
 * ======================================================================= */
#define BUF_CAP 128
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
/* Zero-padded fixed-width field (used for the date/time components). */
static void buf_putn(long val, int width)
{
    char tmp[24];
    int  i = 0;
    if (val < 0) val = 0;
    if (val == 0) tmp[i++] = '0';
    while (val > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(val % 10));
        val /= 10;
    }
    for (int pad = i; pad < width; pad++) buf_putc('0');
    while (i-- > 0) buf_putc(tmp[i]);
}

static void flush_out(void)
{
    sc(SYS_WRITE, FD_STDOUT, (long)g_buf, (long)g_pos, 0, 0);
    g_pos = 0;
    g_buf[0] = '\0';
}

/* =======================================================================
 *  Broken-down UTC calendar time, computed from epoch seconds.
 * ======================================================================= */
typedef struct {
    long year;   /* full 4-digit year, e.g. 2026 */
    int  month;  /* 1..12 */
    int  day;    /* 1..31 */
    int  hour;   /* 0..23 */
    int  min;    /* 0..59 */
    int  sec;    /* 0..59 */
} cal_t;

/*
 * civil_from_epoch -- inverse of days-from-civil (Howard Hinnant, public
 * domain). Valid for all dates; we only feed it non-negative epoch values.
 */
static void civil_from_epoch(long epoch, cal_t *out)
{
    long days = epoch / 86400L;
    long secs = epoch % 86400L;
    if (secs < 0) { secs += 86400L; days -= 1; }

    out->hour = (int)(secs / 3600L);
    out->min  = (int)((secs % 3600L) / 60L);
    out->sec  = (int)(secs % 60L);

    /* z = days since 1970-01-01; shift epoch to a 0000-03-01 era. */
    long z = days + 719468L;
    long era = (z >= 0 ? z : z - 146096L) / 146097L;
    long doe = z - era * 146097L;                       /* [0, 146096] */
    long yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L; /* [0,399] */
    long y   = yoe + era * 400L;
    long doy = doe - (365L * yoe + yoe / 4L - yoe / 100L); /* [0, 365] */
    long mp  = (5L * doy + 2L) / 153L;                   /* [0, 11] */
    long d   = doy - (153L * mp + 2L) / 5L + 1L;         /* [1, 31] */
    long m   = mp < 10L ? mp + 3L : mp - 9L;             /* [1, 12] */

    out->year  = y + (m <= 2L ? 1L : 0L);
    out->month = (int)m;
    out->day   = (int)d;
}

/* =======================================================================
 *  Render "YYYY-MM-DD HH:MM:SS UTC" into the buffer for a given cal_t.
 * ======================================================================= */
static void render_timestamp(const cal_t *c)
{
    buf_putn(c->year, 4);
    buf_putc('-');
    buf_putn(c->month, 2);
    buf_putc('-');
    buf_putn(c->day, 2);
    buf_putc(' ');
    buf_putn(c->hour, 2);
    buf_putc(':');
    buf_putn(c->min, 2);
    buf_putc(':');
    buf_putn(c->sec, 2);
    buf_puts(" UTC\n");
}

/* =======================================================================
 *  Entry point. crt0 provides _start and calls main(argc, argv).
 *  Returns 1 (in self-test mode) when the time source is implausible.
 * ======================================================================= */
int main(int argc, char **argv)
{
    (void)argv;   /* a "+FORMAT" operand is accepted but we emit the default */

    long epoch = sc(SYS_TIME, 0, 0, 0, 0, 0);

    cal_t c;
    int plausible = (epoch > 0);
    if (plausible) {
        civil_from_epoch(epoch, &c);
        if (c.year < 1000 || c.year > 9999) plausible = 0;
    }

    if (plausible) {
        render_timestamp(&c);
    } else {
        /* Time source unavailable: emit a clearly-marked placeholder. */
        buf_puts("date: SYS_TIME unavailable\n");
    }

    if (argc > 1) {
        flush_out();
        return plausible ? 0 : 1;
    }

    /* Self-test: PASS iff epoch > 0 and a 4-digit year was produced. */
    if (plausible) buf_puts("DATE SELFTEST: PASS\n");
    else           buf_puts("DATE SELFTEST: FAIL\n");
    flush_out();
    return plausible ? 0 : 1;
}
