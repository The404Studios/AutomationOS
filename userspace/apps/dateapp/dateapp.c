/*
 * dateapp.c -- Real date/time clock GUI (freestanding, ring 3).
 * =============================================================
 *
 * Opens a 360x160 window titled "Date & Time".  Shows:
 *
 *   Date:  2026-05-28
 *   Time:  14:03:22
 *
 * Updated every frame via ui_app_set_tick().
 *
 * Time source (in priority order):
 *   1. SYS_TIME (41) -- returns Unix epoch seconds via rtc_unix_time().
 *      If the syscall returns >= 0 it is used to compute the calendar date.
 *   2. Fallback: SYS_GET_TICKS_MS (40) -- uptime since boot, displayed as
 *      elapsed HH:MM:SS so the app still runs before the syscall is wired.
 *
 * No libc: pure inline syscalls + tiny freestanding helpers.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *
 *   cd /path/to/Kernel
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -Iuserspace/lib/ui \
 *       -c userspace/apps/dateapp/dateapp.c -o /tmp/dateapp.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/ui/ui.c           -o /tmp/ui.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c    -o /tmp/wlc.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c    -o /tmp/bf.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/dateapp.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/dateapp.elf
 *
 *   # Verify no stack-canary reference:
 *   objdump -d /tmp/dateapp.elf | grep 'fs:0x28'   # must produce no output
 *
 * Serial output on start:
 *   [DATEAPP] starting
 *   [DATEAPP] SYS_TIME available -- showing real date
 *   OR
 *   [DATEAPP] SYS_TIME not wired -- showing uptime fallback
 */

#include "../../lib/ui/ui.h"

/* -----------------------------------------------------------------------
 * Syscall numbers.
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_GET_TICKS_MS 40
#define SYS_TIME         41   /* returns int64_t Unix epoch seconds */

/* -----------------------------------------------------------------------
 * Inline syscall helper (up to 3 args -- sufficient for our usage).
 * --------------------------------------------------------------------- */
static inline long sc3(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Freestanding helpers.
 * --------------------------------------------------------------------- */

static unsigned long da_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial_print(const char *m)
{
    sc3(SYS_WRITE, 1, (long)m, (long)da_strlen(m));
}

/* Write val as a zero-padded N-digit decimal into buf (no NUL). */
static void fmt_pad(char *buf, unsigned long val, int digits)
{
    int i;
    for (i = digits - 1; i >= 0; i--) {
        buf[i] = (char)('0' + (val % 10));
        val /= 10;
    }
}

/* Format Unix epoch seconds as "YYYY-MM-DD" into buf (needs >= 11 bytes). */
static void fmt_date(char *buf, long unix_sec)
{
    /*
     * Inverse of the days-from-civil formula (Howard Hinnant, public domain).
     * Given z = days since 1970-01-01, compute (y, m, d).
     */
    long z  = unix_sec / 86400L;
    long r0 = z + 719468L;
    long era = (r0 >= 0 ? r0 : r0 - 146096L) / 146097L;
    long doe = r0 - era * 146097L;
    long yoe = (doe - doe/1460L + doe/36524L - doe/146096L) / 365L;
    long y   = yoe + era * 400L;
    long doy = doe - (365L*yoe + yoe/4L - yoe/100L);
    long mp  = (5L*doy + 2L) / 153L;
    long d   = doy - (153L*mp + 2L)/5L + 1L;
    long m   = (mp < 10L) ? (mp + 3L) : (mp - 9L);
    if (m <= 2L) y += 1L;

    fmt_pad(buf + 0, (unsigned long)y, 4);
    buf[4] = '-';
    fmt_pad(buf + 5, (unsigned long)m, 2);
    buf[7] = '-';
    fmt_pad(buf + 8, (unsigned long)d, 2);
    buf[10] = '\0';
}

/* Format Unix epoch seconds as "HH:MM:SS" into buf (needs >= 9 bytes). */
static void fmt_time(char *buf, long unix_sec)
{
    long sod = unix_sec % 86400L;   /* seconds of day */
    if (sod < 0) sod += 86400L;
    long hh  = sod / 3600L;
    long mm  = (sod % 3600L) / 60L;
    long ss  = sod % 60L;

    fmt_pad(buf + 0, (unsigned long)hh, 2);
    buf[2] = ':';
    fmt_pad(buf + 3, (unsigned long)mm, 2);
    buf[5] = ':';
    fmt_pad(buf + 6, (unsigned long)ss, 2);
    buf[8] = '\0';
}

/* Format uptime-ms as "Uptime HH:MM:SS" into buf (needs >= 16 bytes). */
static void fmt_uptime(char *buf_label, char *buf_time, unsigned long ms)
{
    unsigned long secs = ms / 1000UL;
    unsigned long hh   = secs / 3600UL;
    unsigned long mm   = (secs % 3600UL) / 60UL;
    unsigned long ss   = secs % 60UL;

    /* label */
    const char *s = "Uptime";
    unsigned long i = 0;
    while (s[i]) { buf_label[i] = s[i]; i++; }
    buf_label[i] = '\0';

    /* time */
    fmt_pad(buf_time + 0, hh, 2);
    buf_time[2] = ':';
    fmt_pad(buf_time + 3, mm, 2);
    buf_time[5] = ':';
    fmt_pad(buf_time + 6, ss, 2);
    buf_time[8] = '\0';
}

/* -----------------------------------------------------------------------
 * Tick state shared between _start and tick_cb.
 * --------------------------------------------------------------------- */

typedef struct {
    ui_widget_t *date_label;   /* "YYYY-MM-DD" or "Uptime" */
    ui_widget_t *time_label;   /* "HH:MM:SS" */
    int          use_rtc;      /* 1 if SYS_TIME is available, 0 for uptime */
} tick_state_t;

static tick_state_t g_state;

/*
 * tick_cb -- called once per frame by the toolkit event loop.
 * Selects between the real-time path and the uptime fallback.
 */
static void tick_cb(void *ud)
{
    (void)ud;
    char date_buf[16];
    char time_buf[16];

    if (g_state.use_rtc) {
        long unix_sec = sc3(SYS_TIME, 0, 0, 0);
        if (unix_sec >= 0) {
            fmt_date(date_buf, unix_sec);
            fmt_time(time_buf, unix_sec);
        } else {
            /* Syscall wired but returned error -- fall back gracefully */
            const char *e1 = "----/--/--";
            const char *e2 = "--:--:--";
            unsigned long i;
            for (i = 0; e1[i]; i++) date_buf[i] = e1[i]; date_buf[i] = '\0';
            for (i = 0; e2[i]; i++) time_buf[i] = e2[i]; time_buf[i] = '\0';
        }
        ui_label_set_text(g_state.date_label, date_buf);
        ui_label_set_text(g_state.time_label, time_buf);
    } else {
        unsigned long ms = (unsigned long)sc3(SYS_GET_TICKS_MS, 0, 0, 0);
        char lbl[16];
        fmt_uptime(lbl, time_buf, ms);
        ui_label_set_text(g_state.date_label, lbl);
        ui_label_set_text(g_state.time_label, time_buf);
    }
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */

void _start(void)
{
    serial_print("[DATEAPP] starting\n");

    /* Probe SYS_TIME: a return value >= 0 means the syscall is wired. */
    long probe = sc3(SYS_TIME, 0, 0, 0);
    g_state.use_rtc = (probe >= 0) ? 1 : 0;

    if (g_state.use_rtc)
        serial_print("[DATEAPP] SYS_TIME available -- showing real date\n");
    else
        serial_print("[DATEAPP] SYS_TIME not wired -- showing uptime fallback\n");

    /*
     * Layout (360 x 160 window):
     *
     *   y=16   "Date & Time" header label  (light grey 0xFFAEAEB2)
     *   y=52   date card panel  (dark card 0xFF2C2C2E), 320x36 at x=20
     *            label "Date:"  at card-relative x=12,y=10  (grey)
     *            label value    at card-relative x=72,y=10  (white)
     *   y=100  time card panel  (dark card 0xFF2C2C2E), 320x36 at x=20
     *            label "Time:"  at card-relative x=12,y=10  (grey)
     *            label value    at card-relative x=72,y=10  (white)
     *
     * 360px window; "Date & Time" is ~12 chars * 8px = 96px -> center x=132.
     */

    ui_app_t    *app  = ui_app_create("Date & Time", 360, 160);
    ui_widget_t *root = ui_app_root(app);

    /* Header */
    ui_label(root, 132, 16, "Date & Time", 0xFFAEAEB2);

    /* Date card */
    ui_widget_t *date_card = ui_panel(root, 20, 50, 320, 36, 0xFF2C2C2E);
    ui_label(date_card, 12, 10, "Date:", 0xFF8E8E93);
    g_state.date_label = ui_label(date_card, 72, 10, "----------", 0xFFFFFFFF);

    /* Time card */
    ui_widget_t *time_card = ui_panel(root, 20, 98, 320, 36, 0xFF2C2C2E);
    ui_label(time_card, 12, 10, "Time:", 0xFF8E8E93);
    g_state.time_label = ui_label(time_card, 72, 10, "--:--:--", 0xFFFFFFFF);

    /* Register tick for live updates. */
    ui_app_set_tick(app, tick_cb, 0);

    /* Enter event loop (never returns). */
    ui_app_run(app);
}
