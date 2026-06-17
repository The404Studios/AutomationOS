/*
 * applauncher.c -- Application Launcher GUI (freestanding, ring 3).
 * =================================================================
 *
 * Opens a 520x420 window titled "Applications" showing a 4-column grid
 * of large buttons, one per installed app.  Clicking a button calls
 * SYS_SPAWN (16) with the app's relative initrd path and logs the result
 * to the serial port.
 *
 * Apps (16 total, 4 columns x 4 rows):
 *   Row 0: Terminal  Files      Calculator  Clock
 *   Row 1: Sys Info  Settings   Monitor     Editor
 *   Row 2: Paint     Snake      Synth       Tetris
 *   Row 3: 2048      Sheet      Notes       Date
 *
 * No libc: pure inline syscalls + tiny freestanding helpers.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/applauncher/applauncher.c -o /tmp/al.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/ui/ui.c -o /tmp/ui.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/al.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/al.elf
 *   objdump -d /tmp/al.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output on startup:
 *   [LAUNCHER] starting
 *
 * Serial output on each button click:
 *   [LAUNCHER] spawn sbin/<app> -> pid N
 */

#include "../../lib/ui/ui.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline syscall helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE  3
#define SYS_SPAWN  16

/*
 * sc() -- invoke a raw syscall with up to 3 arguments.
 * Clobbers rcx and r11 as per the x86-64 syscall ABI.
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

/* -----------------------------------------------------------------------
 * Freestanding helpers (no libc).
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
 * fmt_long_dec -- format a signed long into buf (>= 22 bytes).
 * Returns a pointer into buf pointing at the first digit (not buf[0]).
 * Always NUL-terminates.
 */
static char *fmt_long_dec(long val, char *buf, int bufsz)
{
    int negative = (val < 0);
    unsigned long uval = negative
        ? (unsigned long)(-(unsigned long)val)
        : (unsigned long)val;

    int i = bufsz - 1;
    buf[i] = '\0';
    i--;

    if (uval == 0UL) {
        buf[i] = '0';
        i--;
    } else {
        while (uval > 0UL && i >= 0) {
            buf[i] = (char)('0' + (int)(uval % 10UL));
            uval /= 10UL;
            i--;
        }
    }

    if (negative && i >= 0) {
        buf[i] = '-';
        i--;
    }

    return &buf[i + 1];
}

/* -----------------------------------------------------------------------
 * Per-button user-data.
 *
 * Each button's ud points to one app_entry_t.  The path[] field holds the
 * stable, NUL-padded static string that SYS_SPAWN reads.  128 bytes gives
 * the kernel room to copy a fixed-size chunk without overflowing.
 * --------------------------------------------------------------------- */

typedef struct {
    /*
     * path[] -- relative initrd path passed to SYS_SPAWN.
     * Declared as a 128-byte array; the string is copied in at
     * initialisation, the remainder is zero-filled (.bss / explicit pad).
     * The kernel reads a fixed-length block from this address, so the
     * extra zeroes act as safe padding.
     */
    char path[128];

    /* Human-readable label shown in the serial log (e.g. "sbin/terminal"). */
    /* (We reuse path[] for the log too -- it's already a C string.) */
} app_entry_t;

/* -----------------------------------------------------------------------
 * App table.
 *
 * 16 apps arranged in reading order (left-to-right, top-to-bottom):
 *   row 0: Terminal  Files       Calculator  Clock
 *   row 1: Sys Info  Settings    Monitor     Editor
 *   row 2: Paint     Snake       Synth       Tetris
 *   row 3: 2048      Sheet       Notes       Date
 * --------------------------------------------------------------------- */

#define APP_COUNT 18   /* CLAUDE-APP-0: + Claude chat + Anthropic panel (5th row) */

/*
 * Static storage for all app entries.  The path[] arrays sit in .bss
 * (zero-initialised) and are filled with known-safe strings in _start.
 * All bytes beyond the string terminator remain 0 -- the full 128-byte
 * block is safe to pass to the kernel.
 */
static app_entry_t g_apps[APP_COUNT];

/* Friendly caption for each button (parallel array, same order). */
static const char * const APP_LABELS[APP_COUNT] = {
    "Terminal",    /* 0  sbin/terminal    */
    "Files",       /* 1  sbin/filemanager */
    "Calculator",  /* 2  sbin/calculator  */
    "Clock",       /* 3  sbin/clock       */
    "System Info", /* 4  sbin/sysinfo     */
    "Settings",    /* 5  sbin/settings    */
    "Monitor",     /* 6  sbin/sysmon      */
    "Editor",      /* 7  sbin/editor      */
    "Paint",       /* 8  sbin/paint       */
    "Snake",       /* 9  sbin/snake       */
    "Synth",       /* 10 sbin/synth       */
    "Tetris",      /* 11 sbin/tetris      */
    "2048",        /* 12 sbin/game2048    */
    "Sheet",       /* 13 sbin/sheet       */
    "Notes",       /* 14 sbin/notes       */
    "Date",        /* 15 sbin/dateapp     */
    "Claude",      /* 16 sbin/claudechat  */
    "Anthropic",   /* 17 sbin/anthropic   */
};

/* Relative initrd paths -- no leading slash (initrd lookup is relative). */
static const char * const APP_PATHS[APP_COUNT] = {
    "sbin/terminal",
    "sbin/filemanager",
    "sbin/calculator",
    "sbin/clock",
    "sbin/sysinfo",
    "sbin/settings",
    "sbin/sysmon",
    "sbin/editor",
    "sbin/paint",
    "sbin/snake",
    "sbin/synth",
    "sbin/tetris",
    "sbin/game2048",
    "sbin/sheet",
    "sbin/notes",
    "sbin/dateapp",
    "sbin/claudechat",
    "sbin/anthropic",
};

/* -----------------------------------------------------------------------
 * Copy src into dst up to (dstsz-1) bytes, then zero the rest.
 * dst must be at least dstsz bytes.
 * --------------------------------------------------------------------- */
static void k_strncpy_pad(char *dst, const char *src, int dstsz)
{
    int i = 0;
    while (i < dstsz - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    /* NUL-terminate and zero-pad the remainder. */
    while (i < dstsz) {
        dst[i] = '\0';
        i++;
    }
}

/* -----------------------------------------------------------------------
 * Button on_click handler.
 *
 * Receives a pointer to the app_entry_t for the clicked button.
 * Calls SYS_SPAWN with the path, then logs the result to serial.
 * --------------------------------------------------------------------- */
static void on_launch(void *ud)
{
    app_entry_t *entry = (app_entry_t *)ud;

    /* Invoke SYS_SPAWN.  path[] is a stable static buffer >= 128 bytes. */
    long pid = sc(SYS_SPAWN, (long)entry->path, 0L, 0L);

    /*
     * Serial log: "[LAUNCHER] spawn sbin/<app> -> pid N\n"
     * Build the message in a stack buffer (safe: _start never returns and
     * stack depth here is shallow -- within a click callback).
     */
    char numstr_buf[24];
    char *numstr = fmt_long_dec(pid, numstr_buf, (int)sizeof(numstr_buf));

    serial_print("[LAUNCHER] spawn ");
    serial_print(entry->path);
    serial_print(" -> pid ");
    serial_print(numstr);
    serial_print("\n");
}

/* -----------------------------------------------------------------------
 * Layout constants.
 *
 * Window: 520 wide x 420 tall.
 *
 * Header label:  y = 14  (title "Applications")
 * Grid origin:   x = 16, y = 48
 * Button size:   w = 112, h = 72
 * Gap between buttons: 8px (horizontal and vertical)
 * 4 columns:
 *   col 0: x = 16
 *   col 1: x = 16 + 112 + 8  = 136
 *   col 2: x = 136 + 112 + 8 = 256
 *   col 3: x = 256 + 112 + 8 = 376
 * 4 rows:
 *   row 0: y = 48
 *   row 1: y = 48 + 72 + 8  = 128
 *   row 2: y = 128 + 72 + 8 = 208
 *   row 3: y = 208 + 72 + 8 = 288
 *
 * Grid right edge:  376 + 112 = 488 (+ 16 margin = 504 -- fits in 520)
 * Grid bottom edge: 288 + 72  = 360 (+ 16 margin = 376 -- fits in 420)
 * --------------------------------------------------------------------- */

#define WIN_W  520
#define WIN_H  460   /* CLAUDE-APP-0: room for the 5th row (Claude + Anthropic) */

#define COLS   4
#define BTN_W  112
#define BTN_H  72
#define GAP     8
#define GRID_X  16
#define GRID_Y  48

/* Column x for column index c (0-based). */
#define COL_X(c)  (GRID_X + (c) * (BTN_W + GAP))
/* Row y for row index r (0-based). */
#define ROW_Y(r)  (GRID_Y + (r) * (BTN_H + GAP))

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */

void _start(void)
{
    serial_print("[LAUNCHER] starting\n");

    /* ---- Populate the app-entry table ---- */
    for (int i = 0; i < APP_COUNT; i++) {
        k_strncpy_pad(g_apps[i].path, APP_PATHS[i],
                      (int)sizeof(g_apps[i].path));
    }

    /* ---- Create the window ---- */
    ui_app_t    *app  = ui_app_create("Applications", WIN_W, WIN_H);
    ui_widget_t *root = ui_app_root(app);

    /* Header label: "Applications" centred-ish near the top. */
    ui_label(root, 196, 14, "Applications", 0xFFAEAEB2);

    /* ---- Build the button grid as nested ROW CONTAINERS ----
     * The toolkit caps EVERY widget at UI_MAX_CHILDREN (16, ui.c). Attaching all
     * APP_COUNT buttons + the header directly to root silently DROPS the apps
     * past the 15th (attach_child returns -1, ui_button returns NULL) -- which is
     * exactly why Date/Claude/Anthropic never appeared. Wrap each row in a
     * transparent panel (bg == the window bg 0xFF1C1C1E, so invisible): root then
     * holds 1 label + ceil(APP_COUNT/COLS) row panels, and each row holds <= COLS
     * buttons -- both well under the cap. (Mirrors startmenu's row_box pattern.) */
    int nrows = (APP_COUNT + COLS - 1) / COLS;
    for (int r = 0; r < nrows; r++) {
        ui_widget_t *row_box = ui_panel(root, GRID_X, ROW_Y(r),
                                        COLS * (BTN_W + GAP), BTN_H, 0xFF1C1C1E);
        for (int c = 0; c < COLS; c++) {
            int i = r * COLS + c;
            if (i >= APP_COUNT) break;
            ui_button(row_box, c * (BTN_W + GAP), 0, BTN_W, BTN_H,
                      APP_LABELS[i], on_launch, (void *)&g_apps[i]);
        }
    }

    /* ---- Enter the event loop (never returns) ---- */
    ui_app_run(app);
}
