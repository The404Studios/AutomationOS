/*
 * uidemo.c -- M4 UI toolkit demo application (freestanding, ring 3).
 * ===================================================================
 *
 * Demonstrates the M4 retained-mode UI toolkit.  Opens a 420x260 window
 * titled "Welcome", builds a small widget tree (header label, sub-label,
 * accent button, click counter label), and spins the event loop.  Each
 * button click increments a counter whose text is updated live via
 * ui_label_set_text(), and a diagnostic line is printed to serial.
 *
 * No libc: all helpers are inline / file-static.
 *
 * Build (EXACT -- flags DIRECTLY on the command line, never via a shell
 * variable, so -fno-stack-protector is never silently dropped):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/uidemo/uidemo.c -o /tmp/uidemo.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/ui/ui.c -o /tmp/ui.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/uidemo.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/uidemo.elf
 *
 *   objdump -d /tmp/uidemo.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [UIDEMO] starting
 *   [UIDEMO] button clicked 1
 *   [UIDEMO] button clicked 2
 *   ...
 */

/* Pull in only the ui.h API -- no other external headers. */
#include "../../lib/ui/ui.h"

/* -----------------------------------------------------------------------
 * Freestanding helpers -- no libc.
 * --------------------------------------------------------------------- */

#define SYS_WRITE  3

/*
 * Minimal 3-arg inline syscall matching the task spec signature exactly.
 * (rdi=a1, rsi=a2, rdx=a3)
 */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* NUL-terminated string length (no libc). */
static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/* Write a NUL-terminated string to serial (fd 1). */
static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/*
 * Format a non-negative integer `n` into `buf` (must be >= 12 bytes).
 * Returns pointer to the NUL-terminated result inside `buf`.
 */
static char *fmt_uint(unsigned int n, char *buf, int bufsz)
{
    /* Write digits right-to-left, then reverse. */
    int i = 0;
    if (n == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }
    while (n > 0 && i < bufsz - 1) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    buf[i] = '\0';
    /* Reverse in-place. */
    int lo = 0, hi = i - 1;
    while (lo < hi) {
        char tmp = buf[lo];
        buf[lo]  = buf[hi];
        buf[hi]  = tmp;
        lo++; hi--;
    }
    return buf;
}

/* Copy `src` into `dst` up to `n-1` chars (always NUL-terminates). */
static void k_strlcpy(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Concatenate `src` onto `dst`; `n` = total buf size. */
static void k_strlcat(char *dst, const char *src, int n)
{
    int len = 0;
    while (dst[len]) len++;
    int i = 0;
    while (len + i < n - 1 && src[i]) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = '\0';
}

/* -----------------------------------------------------------------------
 * Button state -- shared between _start() and the on_click callback.
 * --------------------------------------------------------------------- */

/* Static click counter, updated in the callback. */
static unsigned int g_click_count = 0;

/* Pointer to the counter label widget; set before ui_app_run(). */
static ui_widget_t *g_counter_label = 0;

/*
 * on_click -- called by the toolkit whenever the "Click me" button is
 * pressed.  `ud` is the counter label widget (same as g_counter_label).
 */
static void on_click(void *ud)
{
    /* Increment the counter. */
    g_click_count++;

    /* Format "clicks: N" into a local buffer (32 bytes is plenty). */
    char buf[32];
    char numbuf[12];
    k_strlcpy(buf, "clicks: ", sizeof(buf));
    fmt_uint(g_click_count, numbuf, sizeof(numbuf));
    k_strlcat(buf, numbuf, sizeof(buf));

    /* Update the label in the widget tree. */
    ui_widget_t *lbl = (ui_widget_t *)ud;
    ui_label_set_text(lbl, buf);

    /* Emit diagnostic to serial. */
    serial_print("[UIDEMO] button clicked ");
    serial_print(numbuf);
    serial_print("\n");
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */

void _start(void)
{
    serial_print("[UIDEMO] starting\n");

    /* ---- Create the application window (420 x 260). ---- */
    ui_app_t *app = ui_app_create("Welcome", 420, 260);

    /* ---- Retrieve the root panel (covers whole window). ---- */
    ui_widget_t *root = ui_app_root(app);

    /*
     * Layout (all coordinates relative to the root / parent):
     *
     *   +------------------------------------------+  y=0
     *   |  [header]  "Welcome to AutomationOS"      |  y=20
     *   |  [sub]     "M4 UI toolkit demo"           |  y=48
     *   |                                           |
     *   |  [  Click me  ]   (accent button)         |  y=100
     *   |  clicks: 0        (counter label)         |  y=160
     *   +------------------------------------------+
     */

    /* Header label: large white text near the top, centred-ish at x=20. */
    ui_label(root, 20, 20, "Welcome to AutomationOS", 0xFFFFFFFF);

    /* Sub-label: greyed secondary text. */
    ui_label(root, 20, 48, "M4 UI toolkit demo", 0xFFAEAEB2);

    /* Counter label -- allocate BEFORE the button so we can pass it as ud. */
    g_counter_label = ui_label(root, 20, 160, "clicks: 0", 0xFFFFFFFF);

    /*
     * Accent button: 160 x 40 px, accent blue fill (the toolkit renders
     * the bg; button text is centered by the toolkit).
     * We pass g_counter_label as `ud` so the callback can call
     * ui_label_set_text on it.
     */
    ui_button(root, 20, 100, 160, 40, "Click me", on_click, (void *)g_counter_label);

    /* ---- Enter the event loop (never returns). ---- */
    ui_app_run(app);
}
