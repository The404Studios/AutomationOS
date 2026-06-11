/*
 * startmenu.c -- Windows-11-style Start Menu (freestanding, ring 3).
 * ==================================================================
 *
 * A centered, rounded, dark-mode panel resembling Windows 11's Start.
 * Layout (600 x 560):
 *
 *   +----------------------------------------------------------+
 *   |   Search   [___________________________________]  [Q]    |  <- search box row
 *   |                                                          |
 *   |  Pinned                                                  |
 *   |  [ T ] [ F ] [ B ] [ IDE] [ S ] [ Calc] [ Clk ] [ Pnt] |  <- row 0
 *   |  [ Ed] [ Sy] [ Mn] [2048] [Brk] [Pong] [Invd ] [Solit]  |  <- row 1
 *   |  [Nts] [Cal] [MscP][AIcn] [Trck] [Kban] [Note ] [Bch ]  |  <- row 2
 *   |  [TMan][Smon][Proc][Snd ] [Gall] [Rdr ] [Scrn ] [PM  ]  |  <- row 3
 *   |                                                          |
 *   |  Recommended / All Apps                                  |
 *   |  [snake] [synth] [tetris] [mines] [piano] [welcome]      |
 *   |  [stopwatch] [dashboard] [bench]  [dateapp] ...          |
 *   |                                                          |
 *   +---[ Power ]---[ Restart ]----------------------------+  |
 *   +----------------------------------------------------------+
 *
 * Keyboard: printable chars -> filter search (hides non-matching tiles).
 *           Esc             -> SYS_EXIT (close menu).
 *           Backspace       -> delete last search char.
 * Mouse:    click tile      -> SYS_SPAWN path, then SYS_EXIT.
 *           click Shut Down -> SYS_POWEROFF.
 *           click Restart   -> SYS_REBOOT.
 *
 * No libc: pure inline syscalls + M4 UI toolkit.
 *
 * Build line to ADD to scripts/build_all.sh (in the "toolkit apps" section):
 *   build_ui_app userspace/apps/startmenu/startmenu.c   startmenu
 *
 * Install line (same cp loop as the other toolkit apps):
 *   cp /tmp/startmenu.elf /tmp/ird/sbin/startmenu
 *
 * Trigger from taskbar / Start button (compositor_m8.c or taskbar app):
 *   sc(SYS_SPAWN, (long)"sbin/startmenu", 0, 0);
 *
 * Canary check:
 *   objdump -d /tmp/startmenu.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/ui/ui.h"

/* -------------------------------------------------------------------------
 * Syscall numbers (kernel/include/syscall.h)
 * ----------------------------------------------------------------------- */
#define SYS_EXIT      0
#define SYS_WRITE     3
#define SYS_SPAWN     16
#define SYS_POWEROFF  46
#define SYS_REBOOT    47

/* -------------------------------------------------------------------------
 * Inline syscall (3-arg; no fs:0x28 canary -- -fno-stack-protector)
 * ----------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -------------------------------------------------------------------------
 * Freestanding helpers
 * ----------------------------------------------------------------------- */
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

/* Safe ASCII-only tolower */
static char k_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring search: does haystack contain needle? */
static int k_contains_ci(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;          /* empty needle matches everything */
    for (int i = 0; hay[i]; i++) {
        int match = 1;
        for (int j = 0; needle[j]; j++) {
            if (!hay[i + j]) { match = 0; break; }
            if (k_tolower(hay[i + j]) != k_tolower(needle[j])) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* Copy src into dst[0..dstsz-1], NUL-padding the rest. */
static void k_strncpy_pad(char *dst, const char *src, int dstsz)
{
    int i = 0;
    while (i < dstsz - 1 && src[i]) { dst[i] = src[i]; i++; }
    while (i < dstsz)                { dst[i] = '\0';   i++; }
}

/* -------------------------------------------------------------------------
 * Window / panel geometry
 *
 * We open a 600x560 window.  The dark glass panel fills it entirely.
 * Internally we lay out sections with fixed offsets.
 * ----------------------------------------------------------------------- */
#define WIN_W   600
#define WIN_H   560

/* Windows-11 dark panel colours */
#define COL_BG          0xFF202020   /* panel background (#202020)            */
#define COL_HEADER      0xFFFFFFFF   /* section header text                   */
#define COL_SUBTEXT     0xFFAEAEB2   /* muted labels                          */
#define COL_TILE_BG     0xFF2C2C2E   /* unpressed tile background             */
#define COL_TILE_HOVER  0xFF3A3A3C   /* hover/pressed tint (ui toolkit draws) */
#define COL_ICON_BG     0xFF0078D4   /* accent blue (Win-11 default)          */
#define COL_ICON_FG     0xFFFFFFFF
#define COL_SEARCH_BG   0xFF3A3A3C
#define COL_POWER_BG    0xFF3A3A3C
#define COL_DIVIDER     0xFF3A3A3C

/* Search box row */
#define SEARCH_X    16
#define SEARCH_Y    18
#define SEARCH_W   (WIN_W - 32)
#define SEARCH_H    28

/* "Pinned" section */
#define PIN_LABEL_Y  60
#define PIN_GRID_Y   80
#define PIN_COLS      8
#define TILE_W       62
#define TILE_H       62
#define TILE_GAP      6
#define PIN_ROWS      4   /* 8 cols * 4 rows = 32 pinned slots */

/* Grid origin: centre the 8-column grid */
#define GRID_TOTAL_W  (PIN_COLS * TILE_W + (PIN_COLS - 1) * TILE_GAP)
#define GRID_X_ORIGIN ((WIN_W - GRID_TOTAL_W) / 2)   /* = (600-520)/2 = 40 */

/* "Recommended" section (one row, scrollable) */
#define REC_LABEL_Y  352
#define REC_GRID_Y   372
#define REC_ROWS      2
#define REC_TILE_W    86
#define REC_TILE_H    26
#define REC_TILE_GAP   6

/* Power row */
#define POWER_ROW_Y  508
#define POWER_BTN_W  120
#define POWER_BTN_H   36

/* -------------------------------------------------------------------------
 * App descriptor
 * ----------------------------------------------------------------------- */
typedef struct {
    char path[128];   /* sbin/xxx  (NUL-padded, passed to SYS_SPAWN) */
    char label[32];
    char icon_char;   /* single ASCII glyph drawn in the icon tile     */
    unsigned int icon_color;  /* ARGB32 icon tile background           */
} app_entry_t;

/* -------------------------------------------------------------------------
 * Pinned apps (32 slots -- 8 cols x 4 rows)
 *
 * Pulled from applauncher.c + extended with additional apps discovered in
 * the build list.  Icon colours loosely follow Win-11 accent palette.
 * ----------------------------------------------------------------------- */
#define PIN_COUNT 32

static app_entry_t g_pinned[PIN_COUNT];

static const char *PIN_PATHS[PIN_COUNT] = {
    /* row 0 */
    "sbin/terminal",    "sbin/filemanager", "sbin/browser",     "sbin/ide",
    "sbin/settings",    "sbin/calculator",  "sbin/clock",       "sbin/paint",
    /* row 1 */
    "sbin/editor",      "sbin/sysmon",      "sbin/mines",       "sbin/game2048",
    "sbin/breakout",    "sbin/pong",        "sbin/invaders",    "sbin/solitaire",
    /* row 2 */
    "sbin/notes",       "sbin/calendar",    "sbin/musicplayer", "sbin/aiconsole",
    "sbin/tracker",     "sbin/kanban",      "sbin/reader",      "sbin/bench",
    /* row 3 */
    "sbin/taskman",     "sbin/sysinfo",     "sbin/procmon",     "sbin/soundtest",
    "sbin/gallery",     "sbin/screenshot",  "sbin/dateapp",     "sbin/powermenu",
};
static const char *PIN_LABELS[PIN_COUNT] = {
    "Terminal", "Files",   "Browser", "IDE",
    "Settings", "Calc",    "Clock",   "Paint",
    "Editor",   "SysMon",  "Mines",   "2048",
    "Breakout", "Pong",    "Invaders","Solitaire",
    "Notes",    "Calendar","Music",   "AI Shell",
    "Tracker",  "Kanban",  "Reader",  "Bench",
    "TaskMgr",  "SysInfo", "ProcMon", "Sound",
    "Gallery",  "Screenshot","Date",  "Power",
};
static const char PIN_ICON_CHAR[PIN_COUNT] = {
    'T','F','B','I',  'S','#','O','P',
    'E','M','X','2',  'K','G','V','J',
    'N','C','m','A',  'k','K','R','~',
    'Z','?','@','!',  'L','Q','D','^',
};
static const unsigned int PIN_ICON_COL[PIN_COUNT] = {
    0xFF0078D4, 0xFF107C10, 0xFFFF7700, 0xFF5C2D91,
    0xFF0078D4, 0xFF6B6B6B, 0xFFE0A020, 0xFFD83B01,
    0xFF2D7D9A, 0xFF555555, 0xFF107C10, 0xFFF7630C,
    0xFFD13438, 0xFF107C10, 0xFF6B6B6B, 0xFF744DA9,
    0xFFFFB900, 0xFF0099BC, 0xFF010066, 0xFF0078D4,
    0xFF744DA9, 0xFF0099BC, 0xFFA4262C, 0xFF555555,
    0xFF555555, 0xFF0078D4, 0xFF107C10, 0xFF5C2D91,
    0xFF744DA9, 0xFF555555, 0xFFD83B01, 0xFFC50F1F,
};

/* -------------------------------------------------------------------------
 * Recommended apps (second scrollable section)
 * ----------------------------------------------------------------------- */
#define REC_COUNT  12

static app_entry_t g_rec[REC_COUNT];

static const char *REC_PATHS[REC_COUNT] = {
    "sbin/snake",    "sbin/synth",     "sbin/tetris",  "sbin/piano",
    "sbin/welcome",  "sbin/stopwatch", "sbin/dashboard","sbin/netman",
    "sbin/sheet",    "sbin/vpaint",    "sbin/scicalc", "sbin/stress",
};
static const char *REC_LABELS[REC_COUNT] = {
    "Snake", "Synth", "Tetris", "Piano",
    "Welcome","Stopwatch","Dashboard","NetMgr",
    "Sheet", "VPaint","SciCalc","Stress",
};

/* -------------------------------------------------------------------------
 * Search state (shared across all tile on_click callbacks and tick)
 * ----------------------------------------------------------------------- */
#define SEARCH_BUFLEN  64
static char g_search[SEARCH_BUFLEN];  /* current search string */
static int  g_search_len = 0;

/* ui_widget_t pointers for the pinned tiles -- so we can show/hide them */
/* The ui toolkit doesn't expose a show/hide API; instead we remember the
 * button widgets and update their label text: set to "" to make them appear
 * blank, or restore the real label to show them.                           */

/* We simply update tile labels via ui_label_set_text on the *label* widget
 * that we create inside each tile panel.  We need to store those references.*/
static ui_widget_t *g_pin_label_w[PIN_COUNT];
static ui_widget_t *g_rec_label_w[REC_COUNT];

/* -------------------------------------------------------------------------
 * Callback helpers
 * ----------------------------------------------------------------------- */

/* Launch a pinned app */
static void on_pin_click(void *ud)
{
    app_entry_t *e = (app_entry_t *)ud;
    serial_print("[STARTMENU] spawn ");
    serial_print(e->path);
    serial_print("\n");
    sc(SYS_SPAWN, (long)e->path, 0L, 0L);
    sc(SYS_EXIT,  0L, 0L, 0L);
}

/* Power row */
static void on_shutdown(void *ud)
{
    (void)ud;
    serial_print("[STARTMENU] shutdown\n");
    sc(SYS_POWEROFF, 0L, 0L, 0L);
    sc(SYS_EXIT, 0L, 0L, 0L);   /* fallback if no kernel poweroff */
}
static void on_restart(void *ud)
{
    (void)ud;
    serial_print("[STARTMENU] restart\n");
    sc(SYS_REBOOT, 0L, 0L, 0L);
    sc(SYS_EXIT, 0L, 0L, 0L);
}

/* -------------------------------------------------------------------------
 * Tick callback: re-evaluate which tiles to show based on g_search
 * ----------------------------------------------------------------------- */
static void on_tick(void *ud)
{
    (void)ud;

    for (int i = 0; i < PIN_COUNT; i++) {
        if (k_contains_ci(g_pinned[i].label, g_search)) {
            ui_label_set_text(g_pin_label_w[i], g_pinned[i].label);
        } else {
            ui_label_set_text(g_pin_label_w[i], "");
        }
    }

    for (int i = 0; i < REC_COUNT; i++) {
        if (k_contains_ci(g_rec[i].label, g_search)) {
            ui_label_set_text(g_rec_label_w[i], g_rec[i].label);
        } else {
            ui_label_set_text(g_rec_label_w[i], "");
        }
    }
}

/* -------------------------------------------------------------------------
 * Key handler injected via ui_app_set_tick side channel.
 *
 * The M4 ui toolkit only exposes a tick callback, not a raw key callback.
 * We piggyback by reading the textbox text each tick -- the textbox widget
 * (ui_textbox) handles keyboard focus and Backspace internally and exposes
 * ui_textbox_text().  Esc (keycode 1 in the wl layer) maps to ASCII 0x1B
 * which the textbox doesn't append, so we need a small wrapper.
 *
 * Strategy: create a textbox and read its contents every tick; treat Esc
 * as exit by watching for an ESC sentinel we inject ourselves.
 *
 * Simpler approach that avoids the sentinel hack: we own the tick callback
 * and just read the textbox each frame to update the search string.
 * ----------------------------------------------------------------------- */
static ui_widget_t *g_searchbox;

static void on_tick_full(void *ud)
{
    /* Update search from textbox */
    const char *txt = ui_textbox_text(g_searchbox);
    int len = 0;
    while (txt[len] && len < SEARCH_BUFLEN - 1) {
        g_search[len] = txt[len];
        len++;
    }
    g_search[len] = '\0';
    g_search_len = len;

    on_tick(ud);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */
void _start(void)
{
    serial_print("[STARTMENU] starting\n");

    /* Populate app tables */
    for (int i = 0; i < PIN_COUNT; i++) {
        k_strncpy_pad(g_pinned[i].path,  PIN_PATHS[i],  128);
        k_strncpy_pad(g_pinned[i].label, PIN_LABELS[i], 32);
        g_pinned[i].icon_char  = PIN_ICON_CHAR[i];
        g_pinned[i].icon_color = PIN_ICON_COL[i];
    }
    for (int i = 0; i < REC_COUNT; i++) {
        k_strncpy_pad(g_rec[i].path,  REC_PATHS[i],  128);
        k_strncpy_pad(g_rec[i].label, REC_LABELS[i], 32);
        g_rec[i].icon_char  = 'o';
        g_rec[i].icon_color = 0xFF3A3A3C;
    }

    /* ---- Create window ---- */
    ui_app_t    *app  = ui_app_create("Start", WIN_W, WIN_H);
    ui_widget_t *root = ui_app_root(app);

    /* Dark glass background panel (full window) */
    ui_panel(root, 0, 0, WIN_W, WIN_H, COL_BG);

    /* ---- Search row ---- */
    /* Label "Search" */
    ui_label(root, SEARCH_X, SEARCH_Y + 6, "Search", COL_SUBTEXT);

    /* Textbox */
    g_searchbox = ui_textbox(root,
                             SEARCH_X + 56,     /* after label */
                             SEARCH_Y,
                             WIN_W - SEARCH_X - 56 - SEARCH_X,
                             SEARCH_BUFLEN - 1);

    /* ---- "Pinned" section header ---- */
    ui_label(root, GRID_X_ORIGIN, PIN_LABEL_Y, "Pinned", COL_HEADER);

    /* ---- Pinned tile grid: PIN_ROWS x PIN_COLS ----
     *
     * IMPORTANT: the UI toolkit caps EVERY widget at UI_MAX_CHILDREN (16)
     * children (ui.c). Attaching all 32 tiles directly to `root` overflowed
     * that cap, so tiles past the ~12th (plus the whole Recommended section
     * and power row) silently failed to attach and never rendered.
     *
     * Fix: insert one transparent CONTAINER panel per grid ROW. Each row
     * container holds <= PIN_COLS (8) tiles, and `root` gains only PIN_ROWS
     * (4) row-containers instead of 32 tiles. Containers are filled with the
     * panel background colour (COL_BG) so they are invisible over the bg
     * panel; tiles are positioned relative to their row container so every
     * tile keeps its exact original on-screen rectangle. */
    for (int row = 0; row < PIN_ROWS; row++) {
        int row_y = PIN_GRID_Y + row * (TILE_H + TILE_GAP);

        /* Transparent (bg == panel colour) row container. */
        ui_widget_t *row_box = ui_panel(root,
                                        GRID_X_ORIGIN, row_y,
                                        GRID_TOTAL_W, TILE_H,
                                        COL_BG);

        for (int col = 0; col < PIN_COLS; col++) {
            int i = row * PIN_COLS + col;
            if (i >= PIN_COUNT) break;

            /* Tile X is relative to the row container; Y is 0 (row top). */
            int tx = col * (TILE_W + TILE_GAP);

            /* Tile background panel */
            ui_widget_t *tile = ui_panel(row_box, tx, 0, TILE_W, TILE_H,
                                         COL_TILE_BG);

            /* Coloured icon square (centered horizontally, 4px from top) */
            int icon_sz = 36;
            int icon_x  = (TILE_W - icon_sz) / 2;
            ui_image_rect(tile, icon_x, 4, icon_sz,
                          g_pinned[i].icon_color,
                          g_pinned[i].icon_char,
                          COL_ICON_FG);

            /* App label below the icon (roughly centred; each char = 8px) */
            int llen = 0;
            while (g_pinned[i].label[llen]) llen++;
            int lx = (TILE_W - llen * 8) / 2;
            if (lx < 2) lx = 2;
            g_pin_label_w[i] = ui_label(tile, lx, 42, g_pinned[i].label,
                                        COL_SUBTEXT);

            /* Invisible click button covering the whole tile */
            ui_button(tile, 0, 0, TILE_W, TILE_H, "",
                      on_pin_click, (void *)&g_pinned[i]);
        }
    }

    /* ---- Divider ---- */
    ui_panel(root, 16, REC_LABEL_Y - 8, WIN_W - 32, 1, COL_DIVIDER);

    /* ---- "Recommended" section header ---- */
    ui_label(root, 16, REC_LABEL_Y, "Recommended", COL_HEADER);

    /* ---- Recommended tiles (REC_ROWS rows x rec_cols per row) ----
     *
     * Same container-per-row scheme as the pinned grid above, so `root`
     * gains only REC_ROWS row-containers (each holding <= rec_cols tiles)
     * instead of REC_COUNT tiles. */
    int rec_cols = 6;
    int rec_total_w = rec_cols * REC_TILE_W + (rec_cols - 1) * REC_TILE_GAP;
    int rec_x0 = (WIN_W - rec_total_w) / 2;

    for (int row = 0; row < REC_ROWS; row++) {
        int row_y = REC_GRID_Y + row * (REC_TILE_H + REC_TILE_GAP);

        /* Transparent (bg == panel colour) row container. */
        ui_widget_t *row_box = ui_panel(root,
                                        rec_x0, row_y,
                                        rec_total_w, REC_TILE_H,
                                        COL_BG);

        for (int col = 0; col < rec_cols; col++) {
            int i = row * rec_cols + col;
            if (i >= REC_COUNT) break;

            /* Tile X relative to row container; Y is 0 (row top). */
            int tx = col * (REC_TILE_W + REC_TILE_GAP);

            ui_widget_t *tile = ui_panel(row_box, tx, 0,
                                         REC_TILE_W, REC_TILE_H, COL_TILE_BG);

            /* Small icon glyph on left */
            ui_image_rect(tile, 4, 4, REC_TILE_H - 8,
                          g_rec[i].icon_color,
                          g_rec[i].icon_char, COL_ICON_FG);

            /* Label to the right of icon */
            g_rec_label_w[i] = ui_label(tile, REC_TILE_H + 2, 8,
                                        g_rec[i].label, COL_SUBTEXT);

            /* Click button */
            ui_button(tile, 0, 0, REC_TILE_W, REC_TILE_H, "",
                      on_pin_click, (void *)&g_rec[i]);
        }
    }

    /* ---- Bottom divider ---- */
    ui_panel(root, 16, POWER_ROW_Y - 8, WIN_W - 32, 1, COL_DIVIDER);

    /* ---- Power row ----
     *
     * Wrapped in one transparent CONTAINER panel so `root` gains a single
     * child for the whole row (user icon + "User" label + Shut Down +
     * Restart) instead of four, keeping root's child count under
     * UI_MAX_CHILDREN. The container covers only the power-row strip (below
     * the bottom divider), so its COL_BG fill is invisible and never paints
     * over the tiles above. Child coords are relative to the strip top
     * (POWER_ROW_Y), preserving every widget's original on-screen position. */
    ui_widget_t *power_row = ui_panel(root, 0, POWER_ROW_Y,
                                      WIN_W, POWER_BTN_H, COL_BG);

    /* "User" account icon placeholder on the left */
    ui_image_rect(power_row, 16, (POWER_BTN_H - 32) / 2, 32,
                  0xFF0078D4, 'U', COL_ICON_FG);
    ui_label(power_row, 56, 10, "User", COL_SUBTEXT);

    /* Shut Down button (right side) */
    ui_button(power_row,
              WIN_W - 2 * (POWER_BTN_W + 8) - 16,
              0,
              POWER_BTN_W, POWER_BTN_H,
              "Shut Down",
              on_shutdown, (void *)0);

    /* Restart button */
    ui_button(power_row,
              WIN_W - (POWER_BTN_W + 8) - 16,
              0,
              POWER_BTN_W, POWER_BTN_H,
              "Restart",
              on_restart, (void *)0);

    /* ---- Wire tick for search filtering ---- */
    ui_app_set_tick(app, on_tick_full, (void *)0);

    /* ---- Enter event loop (never returns) ---- */
    ui_app_run(app);
}
