/*
 * clock.c -- Live uptime clock GUI app (freestanding, ring 3).
 * =============================================================
 *
 * Opens a 280x140 window titled "Clock", shows a centered "HH:MM:SS"
 * uptime label updated every frame via the ui_app_set_tick() hook.
 * Uses SYS_GET_TICKS_MS (40) to read milliseconds since boot.
 *
 * No libc: pure inline syscalls + tiny helpers.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/clock/clock.c   -o /tmp/clock.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/ui/ui.c          -o /tmp/ui.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c   -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c   -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/clock.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/clock.elf
 *   objdump -d /tmp/clock.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [CLOCK] starting
 */

#include "../../lib/ui/ui.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline syscall helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_GET_TICKS_MS 40

static inline long sc(long n, long a1, long a2, long a3)
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

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/*
 * Format `val` as a zero-padded 2-digit decimal into buf[0..1], no NUL.
 * buf must have at least 2 bytes available.
 */
static void fmt_dd(char *buf, unsigned int val)
{
    buf[0] = (char)('0' + (val / 10) % 10);
    buf[1] = (char)('0' + (val % 10));
}

/*
 * Format milliseconds as "HH:MM:SS" (null-terminated) into buf.
 * buf must be at least 9 bytes (8 chars + NUL).
 */
static void fmt_hms(char *buf, unsigned long ms)
{
    unsigned long secs  = ms / 1000UL;
    unsigned int  hh    = (unsigned int)(secs / 3600UL);
    unsigned int  mm    = (unsigned int)((secs % 3600UL) / 60UL);
    unsigned int  ss    = (unsigned int)(secs % 60UL);

    fmt_dd(buf + 0, hh);
    buf[2] = ':';
    fmt_dd(buf + 3, mm);
    buf[5] = ':';
    fmt_dd(buf + 6, ss);
    buf[8] = '\0';
}

/* -----------------------------------------------------------------------
 * Tick state.
 * --------------------------------------------------------------------- */

/* Pointer to the time label (set in _start, read in the tick callback). */
static ui_widget_t *g_time_label = 0;

/*
 * tick_cb -- called once per frame by the toolkit event loop.
 * Reads the current uptime in ms, formats it as HH:MM:SS, and updates
 * the label widget.
 */
static void tick_cb(void *ud)
{
    (void)ud;
    char buf[16];
    unsigned long ms = (unsigned long)sc(SYS_GET_TICKS_MS, 0, 0, 0);
    fmt_hms(buf, ms);
    ui_label_set_text(g_time_label, buf);
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */

void _start(void)
{
    serial_print("[CLOCK] starting\n");

    /* Window: 280 x 140, titled "Clock". */
    ui_app_t    *app  = ui_app_create("Clock", 280, 140);
    ui_widget_t *root = ui_app_root(app);

    /*
     * Layout:
     *   y=20  "Clock" header label (light grey)
     *   y=62  HH:MM:SS time label, horizontally centered
     *         Window is 280px wide; "HH:MM:SS" is 8 chars * 8px = 64px.
     *         Center x = (280 - 64) / 2 = 108.
     */
    ui_label(root, 108, 20, "Clock", 0xFFAEAEB2);

    /* Time label: place it centered; text will be updated each tick. */
    g_time_label = ui_label(root, 108, 62, "00:00:00", 0xFFFFFFFF);

    /* Register the per-frame tick so the label is refreshed each frame. */
    ui_app_set_tick(app, tick_cb, 0);

    /* Enter the event loop (never returns). */
    ui_app_run(app);
}
