/*
 * powermenu.c -- Shutdown / Reboot menu GUI app (freestanding, ring 3).
 * =====================================================================
 *
 * Opens a small window titled "Power" with two big buttons:
 *
 *   +-----------------------------+
 *   |   Power Options             |
 *   |                             |
 *   |   [    Shut Down     ]      |
 *   |                             |
 *   |   [     Reboot       ]      |
 *   |                             |
 *   |   [     Cancel       ]      |
 *   +-----------------------------+
 *
 * "Shut Down" issues SYS_POWEROFF, which the kernel turns into an ACPI
 * soft-off (S5).  Under QEMU this terminates the emulator process cleanly,
 * which doubles as the cleanest possible automated power-off test.
 *
 * "Reboot" issues SYS_REBOOT (ACPI reset register, 8042 reset fallback).
 *
 * "Cancel" simply exits the app (SYS_EXIT) without touching power state.
 *
 * No libc: pure inline syscalls + the M4 UI toolkit.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/powermenu/powermenu.c -o /tmp/pm.o
 *   gcc ... -c userspace/lib/ui/ui.c        -o /tmp/ui.o
 *   gcc ... -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc ... -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/pm.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/powermenu.elf
 *   objdump -d /tmp/powermenu.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [POWERMENU] starting
 *   [POWERMENU] shutdown requested
 *   [POWERMENU] reboot requested
 */

#include "../../lib/ui/ui.h"

/* -----------------------------------------------------------------------
 * Syscall numbers (must match kernel/include/syscall.h).
 *   SYS_WRITE is already wired; SYS_POWEROFF / SYS_REBOOT are the two new
 *   syscalls this app depends on (proposed numbers 41 / 42).
 * --------------------------------------------------------------------- */
#define SYS_EXIT      0
#define SYS_WRITE     3
#define SYS_POWEROFF  46
#define SYS_REBOOT    47

/* -----------------------------------------------------------------------
 * Inline syscall helper -- no libc.
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

/* -----------------------------------------------------------------------
 * Button callbacks.
 * --------------------------------------------------------------------- */

static void on_shutdown(void *ud)
{
    (void)ud;
    serial_print("[POWERMENU] shutdown requested\n");

    /*
     * Ask the kernel to power the machine off (ACPI S5). This should not
     * return; if it does (e.g. the kernel lacks ACPI support), fall back to
     * exiting so the app does not spin uselessly.
     */
    sc(SYS_POWEROFF, 0, 0, 0);

    serial_print("[POWERMENU] poweroff returned -- not supported?\n");
    sc(SYS_EXIT, 1, 0, 0);
}

static void on_reboot(void *ud)
{
    (void)ud;
    serial_print("[POWERMENU] reboot requested\n");

    sc(SYS_REBOOT, 0, 0, 0);

    serial_print("[POWERMENU] reboot returned -- not supported?\n");
    sc(SYS_EXIT, 1, 0, 0);
}

static void on_cancel(void *ud)
{
    (void)ud;
    serial_print("[POWERMENU] cancelled\n");
    sc(SYS_EXIT, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial_print("[POWERMENU] starting\n");

    /* ---- Create window: 260 x 240. ---- */
    ui_app_t    *app  = ui_app_create("Power", 260, 240);
    ui_widget_t *root = ui_app_root(app);

    /* Title label. */
    ui_label(root, 20, 16, "Power Options", 0xFFFFFFFF);

    /*
     * Layout:
     *   Buttons are 220 wide, 44 tall, centered horizontally (x = 20).
     *   Rows at y = 52, 110, 168 with an 14px gap.
     */
#define BTN_X 20
#define BTN_W 220
#define BTN_H 44

    /* Shut Down -- destructive, give it a warm/red-ish tint via the panel. */
    ui_button(root, BTN_X, 52,  BTN_W, BTN_H, "Shut Down",
              on_shutdown, 0);

    /* Reboot. */
    ui_button(root, BTN_X, 110, BTN_W, BTN_H, "Reboot",
              on_reboot, 0);

    /* Cancel -- closes the menu without changing power state. */
    ui_button(root, BTN_X, 168, BTN_W, BTN_H, "Cancel",
              on_cancel, 0);

#undef BTN_X
#undef BTN_W
#undef BTN_H

    /* ---- Enter the event loop (never returns). ---- */
    ui_app_run(app);
}
