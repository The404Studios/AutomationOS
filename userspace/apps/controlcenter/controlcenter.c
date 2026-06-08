/*
 * userspace/apps/controlcenter/controlcenter.c
 * ============================================
 * Windows-11-style Control Center / Quick Settings panel.
 *
 * Window: 420 x 520  (portrait quick-settings shape)
 *
 * Layout:
 *   -- Header bar: title + close button
 *   -- 2x3 toggle tile grid (Wi-Fi, Bluetooth, Airplane, Night light,
 *                             Dark mode, Do-not-disturb)
 *   -- Volume slider  (0-100)
 *   -- Brightness slider  (0-100)
 *   -- Footer: HH:MM:SS clock  +  battery/power indicator
 *
 * Syscalls used:
 *   SYS_GET_TICKS_MS=40  -- monotonic ms for uptime clock fallback  [REAL]
 *   SYS_GETTIME=42       -- broken-down RTC wall-clock time          [REAL]
 *   SYS_BEEP=45          -- audio feedback on button press (stub ok) [REAL]
 *   SYS_POWEROFF=46      -- reached via power indicator long-press    [REAL]
 *   SYS_NOTIFY=65        -- desktop notification on DND toggle        [REAL]
 *   SYS_SYSINFO=62       -- battery/uptime info (used if available)   [REAL if wired]
 *   Volume backend       -- STUB (no SYS_VOLUME in syscall.h)
 *   Brightness backend   -- STUB (no SYS_BRIGHTNESS in syscall.h)
 *   Wi-Fi / BT / Airplane/ Night-light / Dark-mode -- UI-state stubs
 *     (no kernel driver API present); visible state is maintained in
 *     g_state and persisted to /tmp/cc.conf.
 *
 * Build line for build_all.sh:
 *   cc userspace/apps/controlcenter/controlcenter.c /tmp/cc_app.o
 *   $LD /tmp/cc_app.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/controlcenter.elf
 *   cp /tmp/controlcenter.elf /tmp/ird/sbin/controlcenter
 *
 * Stack-canary free: compiled with -fno-stack-protector (project-wide CF).
 *   objdump -d /tmp/controlcenter.elf | grep fs:0x28  # must be empty
 */

#include "../../lib/ui/ui.h"

/* ---- Syscall numbers ---- */
#define SYS_READ         2
#define SYS_WRITE        3
#define SYS_OPEN         4
#define SYS_CLOSE        5
#define SYS_GET_TICKS_MS 40
#define SYS_GETTIME      42
#define SYS_BEEP         45
#define SYS_POWEROFF     46
#define SYS_SYSINFO      62
#define SYS_NOTIFY       65

/* ---- open flags ---- */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_CREAT   0x40
#define O_TRUNC   0x200

/* ---- Window geometry ---- */
#define WIN_W  420
#define WIN_H  520

/* ---- Colours (Windows 11 dark palette) ---- */
#define COL_BG         0xFF202020u   /* window background */
#define COL_SURFACE    0xFF2D2D2Du   /* tile bg off */
#define COL_SURFACE2   0xFF383838u   /* section bg */
#define COL_HEADER     0xFF1A1A1Au
#define COL_TILE_OFF   0xFF3A3A3Cu   /* tile background (inactive) */
#define COL_TILE_ON    0xFF0078D4u   /* Windows 11 accent blue (active) */
#define COL_TILE_HOVER 0xFF4A4A4Eu   /* hover (off) */
#define COL_TILE_HON   0xFF1690E8u   /* hover (on)  */
#define COL_TEXT       0xFFFFFFFFu
#define COL_SUBTEXT    0xFFAAAAAEu
#define COL_SLIDER_TRK 0xFF555555u
#define COL_SLIDER_FIL 0xFF0078D4u
#define COL_DIVIDER    0xFF444444u
#define COL_FOOTER     0xFF191919u
#define COL_POWER_ICON 0xFF60C060u   /* green = powered */

/* ---- Layout constants ---- */
#define HEADER_H       44
#define GRID_X         16
#define GRID_Y         (HEADER_H + 8)
#define TILE_W         116
#define TILE_H         72
#define TILE_GAP_X     10
#define TILE_GAP_Y     8
#define TILE_COLS      3
#define TILE_ROWS      2
#define SLIDERS_Y      (GRID_Y + TILE_ROWS*(TILE_H+TILE_GAP_Y) + 12)
#define SLIDER_H_EACH  52
#define FOOTER_Y       (WIN_H - 36)

/* ---- Persistent config layout (/tmp/cc.conf, 8 bytes) ---- */
/* byte 0: wifi, 1: bt, 2: airplane, 3: nightlight, 4: darkmode, 5: dnd
   byte 6: volume (0-100), byte 7: brightness (0-100) */
#define CONF_PATH  "/tmp/cc.conf"
#define CONF_SIZE  8

/* ================================================================== */
/* Freestanding helpers                                                */
/* ================================================================== */

static inline long cc_sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static unsigned long cc_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void cc_log(const char *m)
{
    cc_sc(SYS_WRITE, 1, (long)m, (long)cc_strlen(m));
}

/* utoa: unsigned long -> decimal string, 0-padded to `width` chars.
   Returns pointer past the NUL. */
static char *cc_utoa(unsigned long v, char *buf, int width, char pad)
{
    char tmp[22];
    int  len = 0;
    if (v == 0) { tmp[len++] = '0'; }
    else {
        unsigned long x = v;
        while (x) { tmp[len++] = (char)('0' + x % 10); x /= 10; }
    }
    /* reverse */
    for (int i = 0; i < len / 2; i++) {
        char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
    }
    int pad_n = width - len;
    char *p = buf;
    while (pad_n-- > 0) *p++ = pad;
    for (int i = 0; i < len; i++) *p++ = tmp[i];
    *p = '\0';
    return p;
}

static char *cc_append(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* ================================================================== */
/* RTC time struct (mirrors kernel rtc_time_t)                        */
/* ================================================================== */
typedef struct {
    unsigned short year;
    unsigned char  month;
    unsigned char  day;
    unsigned char  hour;
    unsigned char  min;
    unsigned char  sec;
} cc_rtc_t;

/* ================================================================== */
/* sysinfo struct (must match kernel sysinfo_t in procapi.h: 32 bytes) */
/* ================================================================== */
typedef struct {
    unsigned long long total_mem;   /* total physical memory in bytes */
    unsigned long long free_mem;    /* free physical memory in bytes  */
    unsigned long long uptime_ms;   /* milliseconds since boot        */
    unsigned int       proc_count;  /* total live processes           */
    unsigned int       _pad;        /* reserved, always 0             */
} cc_sysinfo_t;

/* ================================================================== */
/* Toggle tile definition                                              */
/* ================================================================== */
#define NUM_TILES 6

typedef struct {
    const char   *name;    /* display name */
    const char   *icon;    /* single-char glyph (ASCII art) */
    int           state;   /* 0=off, 1=on */
    ui_widget_t  *bg_pnl;  /* tile background panel */
    ui_widget_t  *lbl;     /* tile name label */
    ui_widget_t  *icon_lbl; /* icon label */
    ui_widget_t  *state_lbl;/* "On"/"Off" label */
} cc_tile_t;

/* ================================================================== */
/* App state                                                          */
/* ================================================================== */
typedef struct {
    cc_tile_t     tiles[NUM_TILES];
    int           volume;       /* 0-100  (STUB: no syscall) */
    int           brightness;   /* 0-100  (STUB: no syscall) */
    ui_widget_t  *lbl_clock;
    ui_widget_t  *lbl_power;
    ui_widget_t  *lbl_vol_val;
    ui_widget_t  *lbl_br_val;
    ui_widget_t  *vol_slider;
    ui_widget_t  *br_slider;
    int           rtc_ok;       /* 1 if SYS_GETTIME wired */
    int           sysinfo_ok;   /* 1 if SYS_SYSINFO wired */
    cc_rtc_t      rtc;
    cc_sysinfo_t  si;
} cc_state_t;

static cc_state_t g_state;

/* Tile index constants */
#define TILE_WIFI       0
#define TILE_BT         1
#define TILE_AIRPLANE   2
#define TILE_NIGHTLIGHT 3
#define TILE_DARKMODE   4
#define TILE_DND        5

/* ================================================================== */
/* Config persistence                                                  */
/* ================================================================== */
static void cc_save_config(void)
{
    char buf[CONF_SIZE];
    for (int i = 0; i < NUM_TILES; i++)
        buf[i] = (char)g_state.tiles[i].state;
    buf[6] = (char)g_state.volume;
    buf[7] = (char)g_state.brightness;

    long fd = cc_sc(SYS_OPEN, (long)CONF_PATH,
                    O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        cc_sc(SYS_WRITE, fd, (long)buf, CONF_SIZE);
        cc_sc(SYS_CLOSE, fd, 0, 0);
    }
}

static void cc_load_config(void)
{
    char buf[CONF_SIZE];
    long fd = cc_sc(SYS_OPEN, (long)CONF_PATH, O_RDONLY, 0);
    if (fd < 0) return;  /* first run: use defaults */
    long n = cc_sc(SYS_READ, fd, (long)buf, CONF_SIZE);
    cc_sc(SYS_CLOSE, fd, 0, 0);
    if (n < CONF_SIZE) return;
    for (int i = 0; i < NUM_TILES; i++)
        g_state.tiles[i].state = buf[i] ? 1 : 0;
    g_state.volume     = (unsigned char)buf[6];
    g_state.brightness = (unsigned char)buf[7];
}

/* ================================================================== */
/* Tile colour helpers                                                 */
/* ================================================================== */

/* Windows 11 accent palette for each tile when ON */
static unsigned int tile_accent_on[NUM_TILES] = {
    0xFF0078D4u,   /* Wi-Fi       blue   */
    0xFF0098FFu,   /* Bluetooth   azure  */
    0xFFFF453Au,   /* Airplane    red    */
    0xFFFF9F0Au,   /* Night light amber  */
    0xFF6E3FC9u,   /* Dark mode   violet */
    0xFF30D158u,   /* DND         green  */
};

static unsigned int tile_accent_off[NUM_TILES] = {
    COL_TILE_OFF, COL_TILE_OFF, COL_TILE_OFF,
    COL_TILE_OFF, COL_TILE_OFF, COL_TILE_OFF,
};

/* ================================================================== */
/* Update tile appearance after a toggle                              */
/* ================================================================== */
static void tile_refresh_appearance(int idx)
{
    cc_tile_t *t = &g_state.tiles[idx];
    /* Update the tile background color to reflect on/off state. */
    if (t->bg_pnl) {
        ui_widget_set_bg(t->bg_pnl,
                         t->state ? tile_accent_on[idx] : tile_accent_off[idx]);
    }
    if (t->state) {
        ui_label_set_text(t->state_lbl, "On");
    } else {
        ui_label_set_text(t->state_lbl, "Off");
    }
}

/* ================================================================== */
/* Toggle callbacks (one per tile; ui_button passes ud = tile index)  */
/* ================================================================== */
static void on_toggle_tile(void *ud)
{
    int idx = (int)(long)ud;
    if (idx < 0 || idx >= NUM_TILES) return;

    cc_tile_t *t = &g_state.tiles[idx];
    t->state ^= 1;
    tile_refresh_appearance(idx);
    cc_save_config();

    /* Fire a short beep as haptic-analog feedback (SYS_BEEP is real) */
    cc_sc(SYS_BEEP, 880, 60, 0);   /* 880 Hz, 60 ms */

    /* DND toggle: post a desktop notification (SYS_NOTIFY is real) */
    if (idx == TILE_DND) {
        const char *msg = t->state
            ? "Do Not Disturb\0Notifications muted\0"
            : "Do Not Disturb\0Notifications resumed\0";
        cc_sc(SYS_NOTIFY, (long)msg, 0, 0);
    }
}

/* ================================================================== */
/* Volume slider callback  (STUB: no volume syscall in syscall.h)     */
/* ================================================================== */
static void on_volume_change(int value, void *ud)
{
    (void)ud;
    g_state.volume = value;
    /* STUB: would call SYS_VOLUME (not defined) here */
    /* Update label */
    char buf[8];
    cc_utoa((unsigned long)value, buf, 0, ' ');
    ui_label_set_text(g_state.lbl_vol_val, buf);
    cc_save_config();
}

/* ================================================================== */
/* Brightness slider callback  (STUB: no brightness syscall)          */
/* ================================================================== */
static void on_brightness_change(int value, void *ud)
{
    (void)ud;
    g_state.brightness = value;
    /* STUB: would call SYS_BRIGHTNESS (not defined) here */
    char buf[8];
    cc_utoa((unsigned long)value, buf, 0, ' ');
    ui_label_set_text(g_state.lbl_br_val, buf);
    cc_save_config();
}

/* ================================================================== */
/* Power button callback  (SYS_POWEROFF is real)                      */
/* ================================================================== */
static void on_power(void *ud)
{
    (void)ud;
    cc_log("[CC] power off\n");
    cc_sc(SYS_POWEROFF, 0, 0, 0);
}

/* ================================================================== */
/* Per-frame tick: update clock + battery                             */
/* ================================================================== */
static void cc_tick(void *ud)
{
    cc_state_t *st = (cc_state_t *)ud;
    char buf[64];

    /* ---- Clock ---- */
    if (st->rtc_ok) {
        long r = cc_sc(SYS_GETTIME, (long)&st->rtc, 0, 0);
        if (r == 0) {
            char nb[6];
            char *pp = buf;
            pp = cc_append(pp, cc_utoa((unsigned long)st->rtc.hour, nb, 2, '0'));
            *pp++ = ':';
            pp = cc_append(pp, cc_utoa((unsigned long)st->rtc.min,  nb, 2, '0'));
            *pp++ = ':';
            pp = cc_append(pp, cc_utoa((unsigned long)st->rtc.sec,  nb, 2, '0'));
            *pp = '\0';
            ui_label_set_text(st->lbl_clock, buf);
        }
    } else {
        /* fallback: HH:MM:SS from monotonic ticks */
        long ms = cc_sc(SYS_GET_TICKS_MS, 0, 0, 0);
        if (ms < 0) ms = 0;
        unsigned long s   = (unsigned long)ms / 1000UL;
        unsigned long min = s / 60UL;
        unsigned long hr  = min / 60UL;
        s   %= 60UL;
        min %= 60UL;
        hr  %= 24UL;
        char nb[6];
        char *p = buf;
        p = cc_append(p, cc_utoa(hr,  nb, 2, '0')); *p++ = ':';
        p = cc_append(p, cc_utoa(min, nb, 2, '0')); *p++ = ':';
        p = cc_append(p, cc_utoa(s,   nb, 2, '0'));
        *p = '\0';
        ui_label_set_text(st->lbl_clock, buf);
    }

    /* ---- Battery/power indicator ---- */
    /* No SYS_BATTERY defined; show "AC" or query sysinfo for an uptime
       hint. STUB for real battery percentage. */
    if (st->sysinfo_ok) {
        long r = cc_sc(SYS_SYSINFO, (long)&st->si, 0, 0);
        if (r == 0) {
            char nb[12];
            char *p = buf;
            p = cc_append(p, "UP ");
            unsigned long secs = st->si.uptime_ms / 1000UL;
            unsigned long mins = secs / 60UL;
            p = cc_append(p, cc_utoa(mins, nb, 0, ' '));
            p = cc_append(p, "m  AC");
            *p = '\0';
            ui_label_set_text(st->lbl_power, buf);
        }
    }
    /* (if sysinfo not wired the label keeps its static "  Powered" text) */
}

/* ================================================================== */
/* Build tile row/col to pixel position                               */
/* ================================================================== */
static void tile_xy(int idx, int *ox, int *oy)
{
    int col = idx % TILE_COLS;
    int row = idx / TILE_COLS;
    *ox = GRID_X + col * (TILE_W + TILE_GAP_X);
    *oy = GRID_Y + row * (TILE_H + TILE_GAP_Y);
}

/* ================================================================== */
/* _start                                                              */
/* ================================================================== */
void _start(void)
{
    cc_log("[CC] Control Center starting\n");

    /* ---- Initialise state ---- */
    /* Defaults */
    g_state.tiles[TILE_WIFI      ] = (cc_tile_t){ "Wi-Fi",       "W", 1, 0,0,0,0 };
    g_state.tiles[TILE_BT        ] = (cc_tile_t){ "Bluetooth",   "B", 0, 0,0,0,0 };
    g_state.tiles[TILE_AIRPLANE  ] = (cc_tile_t){ "Airplane",    "A", 0, 0,0,0,0 };
    g_state.tiles[TILE_NIGHTLIGHT] = (cc_tile_t){ "Night Light", "N", 0, 0,0,0,0 };
    g_state.tiles[TILE_DARKMODE  ] = (cc_tile_t){ "Dark Mode",   "D", 1, 0,0,0,0 };
    g_state.tiles[TILE_DND       ] = (cc_tile_t){ "Focus",       "Z", 0, 0,0,0,0 };
    g_state.volume     = 75;
    g_state.brightness = 80;

    /* Load persisted config (overwrites defaults if file exists) */
    cc_load_config();

    /* Probe SYS_GETTIME */
    long rtc_probe = cc_sc(SYS_GETTIME, (long)&g_state.rtc, 0, 0);
    g_state.rtc_ok = (rtc_probe >= 0);

    /* Probe SYS_SYSINFO */
    long si_probe = cc_sc(SYS_SYSINFO, (long)&g_state.si, 0, 0);
    g_state.sysinfo_ok = (si_probe >= 0);

    /* ================================================================
     * Build UI
     * ============================================================== */
    ui_app_t    *app  = ui_app_create("Control Center", WIN_W, WIN_H);
    if (!app) {
        cc_log("[CC] ui_app_create failed\n");
        asm volatile("mov $0,%%rdi; mov $0,%%rax; syscall" ::: "rdi","rax");
        __builtin_unreachable();
    }
    ui_widget_t *root = ui_app_root(app);

    /* ---- Header bar ---- */
    ui_widget_t *hdr = ui_panel(root, 0, 0, WIN_W, HEADER_H, COL_HEADER);
    ui_label(hdr, 16, 14, "Quick Settings", COL_TEXT);
    /* Power-off button (top-right) */
    ui_button(hdr, WIN_W - 80, 8, 64, 28, "Power", on_power, 0);

    /* ---- Divider below header ---- */
    ui_panel(root, 0, HEADER_H, WIN_W, 1, COL_DIVIDER);

    /* ================================================================
     * Toggle tiles  (2 rows x 3 cols)
     * Each tile: a button widget acting as the hit-box, with overlaid
     * labels for icon, name, and state.
     * We build a visual panel behind the button label to fake a
     * rounded-rect tile. The ui_button text is empty; three child
     * labels inside a panel deliver the Win-11 look.
     * ============================================================== */
    for (int i = 0; i < NUM_TILES; i++) {
        cc_tile_t *t = &g_state.tiles[i];
        int tx, ty;
        tile_xy(i, &tx, &ty);

        /* Tile background panel (colour flips on/off via state_lbl text) */
        unsigned int bg = t->state ? tile_accent_on[i] : COL_TILE_OFF;
        t->bg_pnl = ui_panel(root, tx, ty, TILE_W, TILE_H, bg);

        /* Icon glyph (large-ish, top-left area) */
        /* We use ui_image_rect for a coloured icon dot */
        ui_image_rect(t->bg_pnl, 10, 10, 28,
                      t->state ? 0xCCFFFFFFu : 0x99808080u,
                      t->icon[0],
                      t->state ? 0xFF000000u : 0xFFFFFFFFu);

        /* Tile name */
        t->lbl = ui_label(t->bg_pnl, 10, 44, t->name,
                          t->state ? COL_TEXT : COL_SUBTEXT);

        /* On/Off indicator (bottom-right) */
        t->state_lbl = ui_label(t->bg_pnl, TILE_W - 28, 44,
                                t->state ? "On" : "Off",
                                t->state ? COL_TEXT : COL_SUBTEXT);

        /* Invisible button over the whole tile for click dispatch */
        ui_button(root, tx, ty, TILE_W, TILE_H, "",
                  on_toggle_tile, (void*)(long)i);
    }

    /* ================================================================
     * Sliders section
     * ============================================================== */
    int sy = SLIDERS_Y;

    /* Divider */
    ui_panel(root, 0, sy - 4, WIN_W, 1, COL_DIVIDER);

    /* ---- Volume slider ---- */
    ui_label(root, GRID_X, sy + 4, "Volume", COL_TEXT);
    char vol_buf[8];
    cc_utoa((unsigned long)g_state.volume, vol_buf, 0, ' ');
    g_state.lbl_vol_val = ui_label(root, WIN_W - 48, sy + 4, vol_buf, COL_SUBTEXT);
    g_state.vol_slider = ui_slider(root,
                                   GRID_X, sy + 22,
                                   WIN_W - GRID_X*2,
                                   0, 100, g_state.volume,
                                   on_volume_change, 0);

    sy += SLIDER_H_EACH;

    /* Divider */
    ui_panel(root, 0, sy - 4, WIN_W, 1, COL_DIVIDER);

    /* ---- Brightness slider ---- */
    ui_label(root, GRID_X, sy + 4, "Brightness", COL_TEXT);
    char br_buf[8];
    cc_utoa((unsigned long)g_state.brightness, br_buf, 0, ' ');
    g_state.lbl_br_val = ui_label(root, WIN_W - 48, sy + 4, br_buf, COL_SUBTEXT);
    g_state.br_slider = ui_slider(root,
                                  GRID_X, sy + 22,
                                  WIN_W - GRID_X*2,
                                  0, 100, g_state.brightness,
                                  on_brightness_change, 0);

    /* ================================================================
     * Footer: clock + power indicator
     * ============================================================== */
    ui_widget_t *footer = ui_panel(root, 0, FOOTER_Y, WIN_W, WIN_H - FOOTER_Y,
                                   COL_FOOTER);
    /* Divider above footer */
    ui_panel(root, 0, FOOTER_Y, WIN_W, 1, COL_DIVIDER);

    /* Clock label (left) */
    g_state.lbl_clock = ui_label(footer, 16, 10, "--:--:--", COL_TEXT);

    /* Battery/power label (right) */
    g_state.lbl_power = ui_label(footer, WIN_W - 110, 10,
                                 "  Powered", COL_POWER_ICON);

    /* ================================================================
     * Register per-frame tick + launch event loop
     * ============================================================== */
    ui_app_set_tick(app, cc_tick, &g_state);

    cc_log("[CC] window ready\n");
    ui_app_run(app);   /* never returns */
    __builtin_unreachable();
}
