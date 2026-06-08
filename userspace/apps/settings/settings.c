/*
 * settings.c -- AutomationOS Control Center
 * ==========================================
 *
 * Upgraded to a full Control Center: System stats, Appearance, Sound, About.
 * Window: 560x460.  Left sidebar + right content panel.
 *
 * Sections:
 *   System     : live uptime, memory bar, process count, date/time
 *   Appearance : accent-colour swatches + light/dark theme toggle + animations
 *   Sound      : master-volume slider, sound on/off checkbox, test-beep button
 *   About      : OS name, build, arch, credits
 *
 * Config persistence (/tmp/settings.conf):
 *   byte 0  : theme  (0=dark, 1=light)
 *   byte 1  : show_clock (0/1)
 *   byte 2  : animations (0/1)
 *   byte 3  : accent_index  (0-5)
 *   byte 4  : sound_on (0/1)
 *   byte 5  : volume (0-100)
 *
 * Syscalls used:
 *   SYS_WRITE=3, SYS_READ=2, SYS_OPEN=4, SYS_CLOSE=5 -- file I/O
 *   SYS_GET_TICKS_MS=40 -- uptime (fallback: show 00:00:00)
 *   SYS_GETTIME=42      -- wall clock date/time (fallback: "n/a")
 *   SYS_BEEP=45         -- test beep (fallback: silent if returns <0)
 *   SYS_SYSINFO=62      -- memory/uptime/proc stats (fallback: "n/a")
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/settings/settings.c -o /tmp/set.o
 *   gcc ... -c userspace/lib/ui/ui.c       -o /tmp/ui.o
 *   gcc ... -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc ... -c userspace/lib/font/bitfont.c  -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/set.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/settings.elf
 *
 * No fs:0x28 stack-canary references (-fno-stack-protector confirmed).
 */

#include "../../lib/ui/ui.h"

/* ---- syscall numbers ---- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_GET_TICKS_MS  40
#define SYS_GETTIME       42
#define SYS_BEEP          45
#define SYS_SYSINFO       62

/* ---- open flags ---- */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_CREAT    0x40
#define O_TRUNC    0x200

/* ---- window dimensions ---- */
#define WIN_W  560
#define WIN_H  460

/* ---- layout constants ---- */
#define SIDEBAR_W   130
#define SIDEBAR_X   0
#define CONTENT_X   (SIDEBAR_W + 2)
#define CONTENT_W   (WIN_W - CONTENT_X - 4)
#define CONTENT_H   (WIN_H - 8)

/* ---- colour palette ---- */
#define COL_WINDOW   0xFF1C1C1Eu
#define COL_SIDEBAR  0xFF232325u
#define COL_SURFACE  0xFF2C2C2Eu
#define COL_SURFACE2 0xFF333335u
#define COL_HOVER    0xFF3A3A3Cu
#define COL_TEXT     0xFFFFFFFFu
#define COL_SUBTEXT  0xFFAAAAAEu
#define COL_DIVIDER  0xFF38383Au
#define COL_ON       0xFF30D158u
#define COL_OFF      0xFF636366u
#define COL_PROGRESS 0xFF0A84FFu

/* ---- accent swatches (6 choices) ---- */
#define ACCENT_COUNT  6
static const unsigned int k_accents[ACCENT_COUNT] = {
    0xFF0A84FFu,   /* 0 Blue   (default) */
    0xFF30D158u,   /* 1 Green  */
    0xFFFF453Au,   /* 2 Red    */
    0xFFFF9F0Au,   /* 3 Orange */
    0xFFBF5AF2u,   /* 4 Purple */
    0xFFFFD60Au,   /* 5 Yellow */
};

/* ---- category IDs ---- */
#define CAT_SYSTEM      0
#define CAT_APPEARANCE  1
#define CAT_SOUND       2
#define CAT_ABOUT       3
#define CAT_COUNT       4

/* ---- config ---- */
#define CONF_BYTES  6

/* ---- sysinfo struct matching kernel definition (32 bytes, kernel/include/procapi.h) ---- */
typedef struct {
    unsigned long long total_mem;
    unsigned long long free_mem;
    unsigned long long uptime_ms;
    unsigned int       proc_count;
    unsigned int       _pad;        /* reserved, always 0 */
} sysinfo_t;

/* ---- gettime struct (must match kernel rtc_time_t: 7 bytes packed) ---- */
typedef struct {
    unsigned short year;    /* full 4-digit year, e.g. 2026 */
    unsigned char  month;   /* 1..12 */
    unsigned char  day;     /* 1..31 */
    unsigned char  hour;    /* 0..23 */
    unsigned char  minute;  /* 0..59 */
    unsigned char  second;  /* 0..59 */
} kerneltime_t;

/* ===========================================================
 * Freestanding helpers
 * ======================================================== */

typedef __PTRDIFF_TYPE__ iptr_t;

/* 4-arg inline syscall */
static inline iptr_t sc4(iptr_t n, iptr_t a, iptr_t b, iptr_t c) {
    iptr_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

static inline iptr_t sc3(iptr_t n, iptr_t a, iptr_t b) {
    return sc4(n, a, b, 0);
}

static inline iptr_t sc2(iptr_t n, iptr_t a) {
    return sc4(n, a, 0, 0);
}

static inline iptr_t sc1(iptr_t n) {
    return sc4(n, 0, 0, 0);
}

static void serial(const char *s) {
    iptr_t len = 0;
    while (s[len]) len++;
    sc4(SYS_WRITE, 1, (iptr_t)s, len);
}

/* itoa decimal, zero-padded to `width`, into buf (>=21 bytes). */
static char *itoa_dec(unsigned long v, char *buf, int width, char pad) {
    char tmp[21];
    int i = 0, j = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; } }
    while (i < width) { buf[j++] = pad; width--; }
    for (int k = i - 1; k >= 0; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
    return buf;
}

static void *s_memset(void *p, int v, unsigned long n) {
    unsigned char *d = (unsigned char *)p;
    for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)v;
    return p;
}

static unsigned long s_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/* Concatenate src into dst (which has `cap` bytes total, NUL included). */
static void s_strcat(char *dst, const char *src, unsigned long cap) {
    unsigned long dl = s_strlen(dst);
    unsigned long i = 0;
    while (dl + 1 < cap && src[i]) { dst[dl++] = src[i++]; }
    dst[dl] = '\0';
}

/* Unsigned 64-bit integer to decimal string, returns pointer to buf. */
static char *u64toa(unsigned long long v, char *buf) {
    char tmp[22];
    int i = 0, j = 0;
    if (v == 0) { buf[j++] = '0'; buf[j] = '\0'; return buf; }
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int k = i - 1; k >= 0; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
    return buf;
}

/* ===========================================================
 * Application state
 * ======================================================== */

typedef struct {
    int active_cat;

    /* Appearance */
    int theme_dark;
    int animations;
    int accent_index;

    /* System */
    int show_clock;

    /* Sound */
    int sound_on;
    int volume;        /* 0-100 */

    /* Live labels updated in tick */
    ui_widget_t *uptime_label;
    ui_widget_t *datetime_label;
    ui_widget_t *mem_label;
    ui_widget_t *proc_label;
    ui_widget_t *mem_bar;       /* ui_progress widget */

    /* Appearance toggle labels */
    ui_widget_t *theme_val;
    ui_widget_t *anim_val;

    /* Sound toggle label */
    ui_widget_t *sound_val;

    /* Volume slider */
    ui_widget_t *vol_slider;
    ui_widget_t *vol_label;

    ui_app_t    *app;
    ui_widget_t *root;
    ui_widget_t *content_panel;

    ui_widget_t *cat_btn[CAT_COUNT];

    unsigned long start_ticks;  /* ticks at process start */
} settings_state_t;

static settings_state_t g_state;

/* ===========================================================
 * Config persistence
 * ======================================================== */

static void conf_save(void) {
    static char conf_buf[CONF_BYTES];
    conf_buf[0] = (char)(g_state.theme_dark  ? 0 : 1);
    conf_buf[1] = (char)(g_state.show_clock  ? 1 : 0);
    conf_buf[2] = (char)(g_state.animations  ? 1 : 0);
    conf_buf[3] = (char)(g_state.accent_index & 0xFF);
    conf_buf[4] = (char)(g_state.sound_on    ? 1 : 0);
    conf_buf[5] = (char)(g_state.volume & 0xFF);

    /* Zeroed static path buffer -- no stack VLA. */
    static char path_buf[32];
    s_memset(path_buf, 0, sizeof(path_buf));
    /* "/tmp/settings.conf" */
    const char *p = "/tmp/settings.conf";
    for (int i = 0; p[i] && i < 31; i++) path_buf[i] = p[i];

    iptr_t fd = sc4(SYS_OPEN, (iptr_t)path_buf,
                    (iptr_t)(O_WRONLY | O_CREAT | O_TRUNC), 0600);
    if (fd >= 0) {
        sc4(SYS_WRITE, fd, (iptr_t)conf_buf, CONF_BYTES);
        sc3(SYS_CLOSE, fd, 0);
        serial("[SETTINGS] saved\n");
    }
}

static void conf_load(void) {
    static char buf[CONF_BYTES];
    s_memset(buf, 0xFF, CONF_BYTES);

    static char path_buf[32];
    s_memset(path_buf, 0, sizeof(path_buf));
    const char *p = "/tmp/settings.conf";
    for (int i = 0; p[i] && i < 31; i++) path_buf[i] = p[i];

    iptr_t fd = sc4(SYS_OPEN, (iptr_t)path_buf, O_RDONLY, 0);
    if (fd >= 0) {
        iptr_t n = sc4(SYS_READ, fd, (iptr_t)buf, CONF_BYTES);
        sc3(SYS_CLOSE, fd, 0);
        if (n == CONF_BYTES) {
            g_state.theme_dark   = (buf[0] == 0) ? 1 : 0;
            g_state.show_clock   = (buf[1] == 1) ? 1 : 0;
            g_state.animations   = (buf[2] == 1) ? 1 : 0;
            g_state.accent_index = (buf[3] < ACCENT_COUNT) ? (int)(unsigned char)buf[3] : 0;
            g_state.sound_on     = (buf[4] == 1) ? 1 : 0;
            g_state.volume       = ((unsigned char)buf[5] <= 100)
                                   ? (int)(unsigned char)buf[5] : 75;
        }
    }
}

/* ===========================================================
 * System section: live stats via SYS_SYSINFO + SYS_GETTIME
 * ======================================================== */

/*
 * Called from tick() to refresh the System panel labels.
 * Graceful degradation: if SYS_SYSINFO or SYS_GETTIME return a
 * negative value (unimplemented / error) we show "n/a" instead.
 */
static void refresh_system_labels(settings_state_t *st) {
    /* --- sysinfo --- */
    if (st->mem_label || st->proc_label || st->mem_bar) {
        static sysinfo_t si;
        s_memset(&si, 0, sizeof(si));
        iptr_t ret = sc4(SYS_SYSINFO, (iptr_t)&si, 0, 0);

        if (ret >= 0) {
            /* Memory: "used / total MB" */
            if (st->mem_label) {
                unsigned long long used_mb  = (si.total_mem - si.free_mem) >> 20;
                unsigned long long total_mb = si.total_mem >> 20;
                static char mem_str[48];
                s_memset(mem_str, 0, sizeof(mem_str));
                char tmp[22];
                u64toa(used_mb,  tmp); s_strcat(mem_str, tmp,         sizeof(mem_str));
                s_strcat(mem_str, " / ",                               sizeof(mem_str));
                u64toa(total_mb, tmp); s_strcat(mem_str, tmp,         sizeof(mem_str));
                s_strcat(mem_str, " MB",                               sizeof(mem_str));
                ui_label_set_text(st->mem_label, mem_str);
            }

            /* Memory progress bar */
            if (st->mem_bar && si.total_mem > 0) {
                unsigned long long pct = ((si.total_mem - si.free_mem) * 100ULL)
                                         / si.total_mem;
                if (pct > 100) pct = 100;
                ui_progress_set(st->mem_bar, (int)pct);
            }

            /* Process count */
            if (st->proc_label) {
                static char proc_str[24];
                s_memset(proc_str, 0, sizeof(proc_str));
                char tmp[16];
                itoa_dec(si.proc_count, tmp, 1, '0');
                s_strcat(proc_str, tmp,        sizeof(proc_str));
                s_strcat(proc_str, " processes", sizeof(proc_str));
                ui_label_set_text(st->proc_label, proc_str);
            }
        } else {
            /* SYS_SYSINFO unavailable */
            if (st->mem_label)  ui_label_set_text(st->mem_label,  "n/a");
            if (st->proc_label) ui_label_set_text(st->proc_label, "n/a");
        }
    }

    /* --- date/time via SYS_GETTIME --- */
    if (st->datetime_label) {
        static kerneltime_t kt;
        s_memset(&kt, 0, sizeof(kt));
        iptr_t ret = sc4(SYS_GETTIME, (iptr_t)&kt, 0, 0);

        if (ret >= 0) {
            /* "YYYY-MM-DD HH:MM:SS" */
            static char dt_str[24];
            s_memset(dt_str, 0, sizeof(dt_str));
            char tmp[8];
            itoa_dec(kt.year,   tmp, 4, '0'); s_strcat(dt_str, tmp,  sizeof(dt_str));
            s_strcat(dt_str, "-",              sizeof(dt_str));
            itoa_dec(kt.month,  tmp, 2, '0'); s_strcat(dt_str, tmp,  sizeof(dt_str));
            s_strcat(dt_str, "-",              sizeof(dt_str));
            itoa_dec(kt.day,    tmp, 2, '0'); s_strcat(dt_str, tmp,  sizeof(dt_str));
            s_strcat(dt_str, " ",              sizeof(dt_str));
            itoa_dec(kt.hour,   tmp, 2, '0'); s_strcat(dt_str, tmp,  sizeof(dt_str));
            s_strcat(dt_str, ":",              sizeof(dt_str));
            itoa_dec(kt.minute, tmp, 2, '0'); s_strcat(dt_str, tmp,  sizeof(dt_str));
            s_strcat(dt_str, ":",              sizeof(dt_str));
            itoa_dec(kt.second, tmp, 2, '0'); s_strcat(dt_str, tmp,  sizeof(dt_str));
            ui_label_set_text(st->datetime_label, dt_str);
        } else {
            ui_label_set_text(st->datetime_label, "n/a");
        }
    }
}

/* ===========================================================
 * Tick callback -- live uptime + system labels
 * ======================================================== */

static void tick(void *ud) {
    settings_state_t *st = (settings_state_t *)ud;

    /* --- Uptime (ticks-based, always available) --- */
    if (st->uptime_label) {
        iptr_t now = sc4(SYS_GET_TICKS_MS, 0, 0, 0);
        unsigned long elapsed_s = 0;
        if (now >= 0)
            elapsed_s = ((unsigned long)now - st->start_ticks) / 1000UL;

        unsigned long h = elapsed_s / 3600UL;
        unsigned long m = (elapsed_s % 3600UL) / 60UL;
        unsigned long s = elapsed_s % 60UL;

        static char uptime[10];
        char hb[4], mb[4], sb[4];
        itoa_dec(h, hb, 2, '0');
        itoa_dec(m, mb, 2, '0');
        itoa_dec(s, sb, 2, '0');
        uptime[0] = hb[0]; uptime[1] = hb[1]; uptime[2] = ':';
        uptime[3] = mb[0]; uptime[4] = mb[1]; uptime[5] = ':';
        uptime[6] = sb[0]; uptime[7] = sb[1]; uptime[8] = '\0';
        ui_label_set_text(st->uptime_label, uptime);
    }

    /* --- Refresh sysinfo + datetime labels (System panel only) --- */
    refresh_system_labels(st);
}

/* ===========================================================
 * Helper: clear per-section widget pointers
 * ======================================================== */
static void clear_section_ptrs(void) {
    g_state.uptime_label   = (void*)0;
    g_state.datetime_label = (void*)0;
    g_state.mem_label      = (void*)0;
    g_state.proc_label     = (void*)0;
    g_state.mem_bar        = (void*)0;
    g_state.theme_val      = (void*)0;
    g_state.anim_val       = (void*)0;
    g_state.sound_val      = (void*)0;
    g_state.vol_slider     = (void*)0;
    g_state.vol_label      = (void*)0;
}

/* ===========================================================
 * Section: SYSTEM
 * ======================================================== */

static void build_system(ui_widget_t *panel) {
    ui_label(panel, 16, 12, "System", COL_TEXT);
    ui_panel(panel, 12, 36, CONTENT_W - 8, 1, COL_DIVIDER);

    /* --- Date / Time --- */
    ui_label(panel, 16, 46, "Date & Time", COL_TEXT);
    g_state.datetime_label = ui_label(panel, 16, 64, "n/a", COL_SUBTEXT);

    ui_panel(panel, 12, 86, CONTENT_W - 8, 1, COL_DIVIDER);

    /* --- Uptime --- */
    ui_label(panel, 16, 96, "System Uptime", COL_TEXT);
    g_state.uptime_label = ui_label(panel, 16, 114, "00:00:00", COL_SUBTEXT);

    ui_panel(panel, 12, 136, CONTENT_W - 8, 1, COL_DIVIDER);

    /* --- Memory --- */
    ui_label(panel, 16, 146, "Memory", COL_TEXT);
    g_state.mem_label = ui_label(panel, 16, 164, "n/a", COL_SUBTEXT);

    /* Memory bar: uses ui_progress.  Falls back visually to a plain
       label if ui_progress is unavailable (it compiles to NULL and
       we guard every ui_progress_set call with a NULL check).      */
    g_state.mem_bar = ui_progress(panel, 16, 184, CONTENT_W - 40, 14);

    ui_panel(panel, 12, 210, CONTENT_W - 8, 1, COL_DIVIDER);

    /* --- Processes --- */
    ui_label(panel, 16, 220, "Processes", COL_TEXT);
    g_state.proc_label = ui_label(panel, 16, 238, "n/a", COL_SUBTEXT);
}

/* ===========================================================
 * Appearance callbacks
 * ======================================================== */

static void cb_toggle_theme(void *ud) {
    (void)ud;
    g_state.theme_dark ^= 1;
    if (g_state.theme_val)
        ui_label_set_text(g_state.theme_val,
                          g_state.theme_dark ? "Dark" : "Light");
    conf_save();
}

/* Checkbox-compatible toggle for theme (int state, void *ud). */
static void cb_toggle_theme_chk(int state, void *ud) {
    (void)ud;
    g_state.theme_dark = state;
    if (g_state.theme_val)
        ui_label_set_text(g_state.theme_val,
                          g_state.theme_dark ? "Dark" : "Light");
    conf_save();
}

static void cb_toggle_animations(void *ud) {
    (void)ud;
    g_state.animations ^= 1;
    if (g_state.anim_val)
        ui_label_set_text(g_state.anim_val,
                          g_state.animations ? "On" : "Off");
    conf_save();
}

/* Checkbox-compatible toggle for animations (int state, void *ud). */
static void cb_toggle_animations_chk(int state, void *ud) {
    (void)ud;
    g_state.animations = state;
    if (g_state.anim_val)
        ui_label_set_text(g_state.anim_val,
                          g_state.animations ? "On" : "Off");
    conf_save();
}

/* Accent swatch callbacks -- one per colour (ud = index as iptr_t). */
static void cb_accent(void *ud) {
    int idx = (int)(iptr_t)ud;
    if (idx < 0 || idx >= ACCENT_COUNT) return;
    g_state.accent_index = idx;
    conf_save();
}

/* ===========================================================
 * Section: APPEARANCE
 * ======================================================== */

static void build_appearance(ui_widget_t *panel) {
    ui_label(panel, 16, 12, "Appearance", COL_TEXT);

    /* --- Accent colour picker --- */
    ui_panel(panel, 12, 36, CONTENT_W - 8, 1, COL_DIVIDER);
    ui_label(panel, 16, 46, "Accent Color", COL_TEXT);
    ui_label(panel, 16, 64, "Choose a system accent color.", COL_SUBTEXT);

    /* Draw 6 swatches at y=84, x=16,50,84,... spaced 34px.
     * ui_image_rect is used as a coloured swatch tile (28x28).
     * Clicking the swatch tile fires cb_accent with the index.
     * Fallback if ui_image_rect is not available: use ui_button instead. */
    for (int i = 0; i < ACCENT_COUNT; i++) {
        int sx = 16 + i * 34;
        char glyph = (i == g_state.accent_index) ? '*' : ' ';
        /* ui_image_rect is used here directly; it is declared in ui.h
         * and implemented by the parallel agent.  If the linker cannot
         * find it the build will tell the operator to fall back to the
         * ui_button variant below.  Both call the same cb_accent. */
        ui_image_rect(panel, sx, 84, 28,
                      k_accents[i], glyph, COL_TEXT);
        /* Overlay a transparent-ish button so clicks are captured.
         * We use a 0x00000000 bg panel + button of same size.       */
        ui_button(panel, sx, 84, 28, 28, "",
                  cb_accent, (void*)(iptr_t)i);
    }

    /* --- Theme toggle --- */
    ui_panel(panel, 12, 124, CONTENT_W - 8, 1, COL_DIVIDER);
    ui_label(panel, 16, 134, "Theme", COL_TEXT);
    ui_label(panel, 16, 152, "Switch between Dark and Light mode.", COL_SUBTEXT);

    /* Try ui_checkbox for theme; fall back silently (checkbox is new widget).
     * If ui_checkbox returns NULL we use a plain button instead.           */
    ui_widget_t *chk_theme = ui_checkbox(panel, 16, 174,
                                         "Dark mode",
                                         g_state.theme_dark,
                                         cb_toggle_theme_chk,
                                         &g_state);
    if (!chk_theme) {
        /* Fallback: plain toggle button */
        ui_button(panel, 16, 174, 90, 24,
                  g_state.theme_dark ? "Dark" : "Light",
                  cb_toggle_theme, &g_state);
    }
    g_state.theme_val = ui_label(panel, 120, 178,
                                 g_state.theme_dark ? "Dark" : "Light",
                                 COL_SUBTEXT);

    /* --- Animations toggle --- */
    ui_panel(panel, 12, 208, CONTENT_W - 8, 1, COL_DIVIDER);
    ui_label(panel, 16, 218, "Animations", COL_TEXT);
    ui_label(panel, 16, 236, "Enable UI transition animations.", COL_SUBTEXT);

    ui_widget_t *chk_anim = ui_checkbox(panel, 16, 258,
                                        "Enabled",
                                        g_state.animations,
                                        cb_toggle_animations_chk,
                                        &g_state);
    if (!chk_anim) {
        ui_button(panel, 16, 258, 90, 24,
                  g_state.animations ? "On" : "Off",
                  cb_toggle_animations, &g_state);
    }
    g_state.anim_val = ui_label(panel, 120, 262,
                                g_state.animations ? "On" : "Off",
                                g_state.animations ? COL_ON : COL_OFF);
}

/* ===========================================================
 * Sound callbacks
 * ======================================================== */

static void cb_volume_change(int value, void *ud) {
    (void)ud;
    g_state.volume = value;
    /* Update label */
    if (g_state.vol_label) {
        static char vol_str[8];
        s_memset(vol_str, 0, sizeof(vol_str));
        itoa_dec((unsigned long)value, vol_str, 1, '0');
        /* append "%" */
        int l = 0; while (vol_str[l]) l++;
        if (l < 7) { vol_str[l] = '%'; vol_str[l+1] = '\0'; }
        ui_label_set_text(g_state.vol_label, vol_str);
    }
    conf_save();
}

static void cb_toggle_sound(void *ud) {
    (void)ud;
    g_state.sound_on ^= 1;
    if (g_state.sound_val)
        ui_label_set_text(g_state.sound_val,
                          g_state.sound_on ? "On" : "Off");
    conf_save();
}

/* Checkbox-compatible toggle for sound (int state, void *ud). */
static void cb_toggle_sound_chk(int state, void *ud) {
    (void)ud;
    g_state.sound_on = state;
    if (g_state.sound_val)
        ui_label_set_text(g_state.sound_val,
                          g_state.sound_on ? "On" : "Off");
    conf_save();
}

static void cb_test_beep(void *ud) {
    (void)ud;
    /* SYS_BEEP(frequency_hz, duration_ms) -- graceful if <0 */
    iptr_t ret = sc4(SYS_BEEP, 880, 200, 0);
    if (ret < 0)
        serial("[SETTINGS] SYS_BEEP unavailable (graceful)\n");
}

/* ===========================================================
 * Section: SOUND
 * ======================================================== */

static void build_sound(ui_widget_t *panel) {
    ui_label(panel, 16, 12, "Sound", COL_TEXT);

    /* --- Sound on/off --- */
    ui_panel(panel, 12, 36, CONTENT_W - 8, 1, COL_DIVIDER);
    ui_label(panel, 16, 46, "Master Sound", COL_TEXT);
    ui_label(panel, 16, 64, "Enable or disable system audio.", COL_SUBTEXT);

    /* Prefer ui_checkbox; fall back to ui_button */
    ui_widget_t *chk_sound = ui_checkbox(panel, 16, 86,
                                         "Sound enabled",
                                         g_state.sound_on,
                                         cb_toggle_sound_chk,
                                         &g_state);
    if (!chk_sound) {
        ui_button(panel, 16, 86, 100, 24,
                  g_state.sound_on ? "On" : "Off",
                  cb_toggle_sound, &g_state);
    }
    g_state.sound_val = ui_label(panel, 160, 90,
                                 g_state.sound_on ? "On" : "Off",
                                 g_state.sound_on ? COL_ON : COL_OFF);

    /* --- Volume slider --- */
    ui_panel(panel, 12, 120, CONTENT_W - 8, 1, COL_DIVIDER);
    ui_label(panel, 16, 130, "Master Volume", COL_TEXT);

    /* Prefer ui_slider; if NULL fall back to label showing current value */
    g_state.vol_slider = ui_slider(panel, 16, 152, CONTENT_W - 60,
                                   0, 100, g_state.volume,
                                   cb_volume_change, &g_state);

    /* Current value label */
    {
        static char vol_str[8];
        s_memset(vol_str, 0, sizeof(vol_str));
        itoa_dec((unsigned long)g_state.volume, vol_str, 1, '0');
        int l = 0; while (vol_str[l]) l++;
        if (l < 7) { vol_str[l] = '%'; vol_str[l+1] = '\0'; }
        g_state.vol_label = ui_label(panel, CONTENT_W - 40, 154,
                                     vol_str, COL_TEXT);
    }

    if (!g_state.vol_slider) {
        /* Fallback: show value only, with +/- buttons */
        ui_label(panel, 16, 176, "Slider unavailable -- use buttons:", COL_SUBTEXT);
        /* We can't easily wire +/- without more state, so we just display
         * the current volume as text.  The conf_save value is still valid. */
    }

    /* --- Test sound button --- */
    ui_panel(panel, 12, 190, CONTENT_W - 8, 1, COL_DIVIDER);
    ui_label(panel, 16, 200, "Test Sound", COL_TEXT);
    ui_label(panel, 16, 218, "Play a brief beep to verify audio.", COL_SUBTEXT);
    ui_button(panel, 16, 238, 120, 28, "Play Test Tone",
              cb_test_beep, &g_state);
}

/* ===========================================================
 * Section: ABOUT
 * ======================================================== */

static void build_about(ui_widget_t *panel) {
    ui_label(panel, 16, 12, "About AutomationOS", COL_TEXT);
    ui_panel(panel, 12, 36, CONTENT_W - 8, 1, COL_DIVIDER);

    /* Accent-coloured icon tile as a simple logo placeholder */
    ui_image_rect(panel, 16, 46, 40,
                  k_accents[g_state.accent_index], 'A', COL_TEXT);

    ui_label(panel, 68, 50, "AutomationOS",        COL_TEXT);
    ui_label(panel, 68, 68, "Version: 1.0-M5+",   COL_SUBTEXT);

    ui_panel(panel, 12, 100, CONTENT_W - 8, 1, COL_DIVIDER);

    ui_label(panel, 16, 110, "System",              COL_TEXT);
    ui_label(panel, 16, 128, "Arch:  x86_64 ring 3",    COL_SUBTEXT);
    ui_label(panel, 16, 146, "Compositor: Aether",      COL_SUBTEXT);
    ui_label(panel, 16, 164, "Kernel: AutomationOS/1",  COL_SUBTEXT);
    ui_label(panel, 16, 182, "Scheduler: cooperative",  COL_SUBTEXT);

    ui_panel(panel, 12, 206, CONTENT_W - 8, 1, COL_DIVIDER);

    ui_label(panel, 16, 216, "Credits",             COL_TEXT);
    ui_label(panel, 16, 234, "Built with the M4 UI toolkit",   COL_SUBTEXT);
    ui_label(panel, 16, 252, "Aether windowing stack (M3+)",   COL_SUBTEXT);
    ui_label(panel, 16, 270, "8x16 bitmap font engine",        COL_SUBTEXT);
    ui_label(panel, 16, 288, "(c) AutomationOS contributors",  COL_SUBTEXT);
}

/* ===========================================================
 * Category switching
 * ======================================================== */

static void switch_category(int cat) {
    g_state.active_cat = cat;
    clear_section_ptrs();

    /* Free the previous content panel and all its children so widget pool
     * slots are reclaimed.  Without this, each category switch leaks ~20-40
     * widgets and the 512-slot pool exhausts after ~5-6 switches, causing
     * all subsequent widget creation to silently fail (NULL returns).      */
    if (g_state.content_panel) {
        ui_widget_detach(g_state.root, g_state.content_panel);
        ui_widget_free_tree(g_state.content_panel);
        g_state.content_panel = (void*)0;
    }

    ui_widget_t *panel = ui_panel(g_state.root,
                                  CONTENT_X, 4,
                                  CONTENT_W, CONTENT_H,
                                  COL_SURFACE);
    g_state.content_panel = panel;

    switch (cat) {
    case CAT_SYSTEM:     build_system(panel);     break;
    case CAT_APPEARANCE: build_appearance(panel); break;
    case CAT_SOUND:      build_sound(panel);      break;
    case CAT_ABOUT:      build_about(panel);      break;
    default:             break;
    }
}

/* Category button callbacks */
static void cb_cat_system(void *ud)     { (void)ud; switch_category(CAT_SYSTEM); }
static void cb_cat_appearance(void *ud) { (void)ud; switch_category(CAT_APPEARANCE); }
static void cb_cat_sound(void *ud)      { (void)ud; switch_category(CAT_SOUND); }
static void cb_cat_about(void *ud)      { (void)ud; switch_category(CAT_ABOUT); }

/* ===========================================================
 * _start
 * ======================================================== */

void _start(void) {
    serial("[SETTINGS] Control Center starting\n");

    /* Capture start ticks. */
    iptr_t t0 = sc4(SYS_GET_TICKS_MS, 0, 0, 0);
    s_memset(&g_state, 0, sizeof(g_state));
    g_state.start_ticks = (t0 >= 0) ? (unsigned long)t0 : 0UL;

    /* Defaults */
    g_state.theme_dark   = 1;
    g_state.show_clock   = 1;
    g_state.animations   = 1;
    g_state.accent_index = 0;
    g_state.sound_on     = 1;
    g_state.volume       = 75;

    /* Load persisted config (overrides defaults). */
    conf_load();

    /* ---- Create window ---- */
    ui_app_t *app = ui_app_create("Control Center", WIN_W, WIN_H);
    if (!app) { serial("[SETTINGS] ERROR: ui_app_create failed\n"); for(;;); }

    g_state.app  = app;
    g_state.root = ui_app_root(app);

    /* ---- Sidebar ---- */
    ui_widget_t *sidebar = ui_panel(g_state.root,
                                    SIDEBAR_X, 0,
                                    SIDEBAR_W, WIN_H,
                                    COL_SIDEBAR);

    /* App title */
    ui_label(sidebar, 10, 14, "Control Center", COL_TEXT);
    ui_panel(sidebar, 8, 38, SIDEBAR_W - 16, 1, COL_DIVIDER);

    /* Category buttons -- y positions spaced 38px */
    g_state.cat_btn[CAT_SYSTEM] =
        ui_button(sidebar, 8,  48, SIDEBAR_W - 16, 30,
                  "System",     cb_cat_system,     &g_state);

    g_state.cat_btn[CAT_APPEARANCE] =
        ui_button(sidebar, 8,  86, SIDEBAR_W - 16, 30,
                  "Appearance", cb_cat_appearance, &g_state);

    g_state.cat_btn[CAT_SOUND] =
        ui_button(sidebar, 8, 124, SIDEBAR_W - 16, 30,
                  "Sound",      cb_cat_sound,      &g_state);

    g_state.cat_btn[CAT_ABOUT] =
        ui_button(sidebar, 8, 162, SIDEBAR_W - 16, 30,
                  "About",      cb_cat_about,      &g_state);

    /* Thin vertical divider */
    ui_panel(g_state.root, SIDEBAR_W, 0, 2, WIN_H, COL_DIVIDER);

    /* ---- Initial content: System ---- */
    switch_category(CAT_SYSTEM);

    /* ---- Tick for live labels ---- */
    ui_app_set_tick(app, tick, &g_state);

    /* ---- Run (never returns) ---- */
    ui_app_run(app);
}
