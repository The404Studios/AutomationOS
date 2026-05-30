/*
 * sysinfo.c -- System / About GUI app (freestanding, ring 3).
 * ============================================================
 *
 * Opens a 320x200 window titled "System", shows a set of static info
 * labels ("AutomationOS", "Desktop: M5", "Compositor: running") plus a
 * live "Uptime: HH:MM:SS" label updated each frame, and an "OK" button
 * that prints a line to serial.
 *
 * No libc: pure inline syscalls + tiny helpers.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/sysinfo/sysinfo.c -o /tmp/sysinfo.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/ui/ui.c            -o /tmp/ui.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c     -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c     -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/sysinfo.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/sysinfo.elf
 *   objdump -d /tmp/sysinfo.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [SYSINFO] starting
 *   [SYSINFO] OK clicked
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

/* Zero-pad 2-digit decimal into buf[0..1], no NUL. */
static void fmt_dd(char *buf, unsigned int val)
{
    buf[0] = (char)('0' + (val / 10) % 10);
    buf[1] = (char)('0' + (val % 10));
}

/*
 * Format milliseconds as "Uptime: HH:MM:SS" (null-terminated).
 * buf must be >= 17 bytes.
 */
static void fmt_uptime(char *buf, unsigned long ms)
{
    unsigned long secs = ms / 1000UL;
    unsigned int  hh   = (unsigned int)(secs / 3600UL);
    unsigned int  mm   = (unsigned int)((secs % 3600UL) / 60UL);
    unsigned int  ss   = (unsigned int)(secs % 60UL);

    /* "Uptime: HH:MM:SS" */
    buf[0]  = 'U'; buf[1]  = 'p'; buf[2]  = 't'; buf[3]  = 'i';
    buf[4]  = 'm'; buf[5]  = 'e'; buf[6]  = ':'; buf[7]  = ' ';
    fmt_dd(buf + 8,  hh);
    buf[10] = ':';
    fmt_dd(buf + 11, mm);
    buf[13] = ':';
    fmt_dd(buf + 14, ss);
    buf[16] = '\0';
}

/* -----------------------------------------------------------------------
 * Tick + button state.
 * --------------------------------------------------------------------- */

static ui_widget_t *g_uptime_label = 0;

static void tick_cb(void *ud)
{
    (void)ud;
    char buf[20];
    unsigned long ms = (unsigned long)sc(SYS_GET_TICKS_MS, 0, 0, 0);
    fmt_uptime(buf, ms);
    ui_label_set_text(g_uptime_label, buf);
}

static void on_ok(void *ud)
{
    (void)ud;
    serial_print("[SYSINFO] OK clicked\n");
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */

void _start(void)
{
    serial_print("[SYSINFO] starting\n");

    /* Window: 320 x 200, titled "System". */
    ui_app_t    *app  = ui_app_create("System", 320, 200);
    ui_widget_t *root = ui_app_root(app);

    /*
     * Layout (all x/y relative to root = window origin):
     *
     *   y=18   "AutomationOS"       -- white title
     *   y=46   "Desktop: M5"        -- light grey
     *   y=68   "Compositor: running"-- light grey
     *   y=96   "Uptime: HH:MM:SS"   -- white, live
     *   y=148  [ OK ]               -- button (100x32), x=20
     */
    ui_label(root, 20, 18, "AutomationOS",         0xFFFFFFFF);
    ui_label(root, 20, 46, "Desktop: M5",           0xFFAEAEB2);
    ui_label(root, 20, 68, "Compositor: running",   0xFFAEAEB2);

    g_uptime_label = ui_label(root, 20, 96, "Uptime: 00:00:00", 0xFFFFFFFF);

    ui_button(root, 20, 148, 100, 32, "OK", on_ok, 0);

    ui_app_set_tick(app, tick_cb, 0);

    ui_app_run(app);
}
