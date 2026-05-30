/*
 * clockapp.c -- "Clock+" : a Windows-11-style Clock app (freestanding, ring 3).
 * ============================================================================
 *
 * A from-scratch, no-libc GUI app for AutomationOS built directly on the
 * "Wayland-lite" client (userspace/lib/wl) + the 8x16 bitmap font
 * (userspace/lib/font).  Window: 760x520, title "Clock+".
 *
 * Layout: a LEFT tab rail (Windows-11 style nav, icon + label) selecting one
 * of four panes; the rest of the window is the active pane's content.
 *
 *   [Clock]      -- big digital HH:MM:SS (uptime clock from SYS_GET_TICKS_MS)
 *                   + a live analog face (integer Q16 sin table, Bresenham
 *                   hands).  This is the "wall clock" derived from uptime, so
 *                   it ticks deterministically with no RTC dependency.
 *   [Alarm]      -- set an alarm as an offset from "now" (mm:ss via +/- chips);
 *                   ARM it; when the uptime crosses the target it FLASHES the
 *                   pane red and calls SYS_BEEP.  Shows the armed alarm with an
 *                   On/Off toggle.
 *   [Timer]      -- a countdown you dial in (mm:ss); Start/Pause/Reset.  Beeps
 *                   (SYS_BEEP) and flashes when it reaches zero.
 *   [Stopwatch]  -- Start/Stop, Lap, Reset; elapsed shown as MM:SS.cc, up to
 *                   six laps listed.
 *
 * ALL timing uses SYS_GET_TICKS_MS (syscall 40, monotonic ms since boot).
 * The audible alert uses SYS_BEEP (syscall 45); if it is not wired the call
 * just returns negative and is ignored (the visual flash still happens).
 *
 * Input: mouse (pointer) + keyboard.  Global keys:
 *   1/2/3/4         -- jump to Clock / Alarm / Timer / Stopwatch pane
 *   Space           -- Start/Stop (Timer & Stopwatch), Arm/Disarm (Alarm)
 *   R               -- Reset the active pane
 *   L               -- Lap (Stopwatch)
 *
 * No libc: pure inline syscalls + tiny freestanding helpers.  Buttons, chrome
 * and the analog face are drawn pixel-by-pixel into the shared SHM buffer.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable;
 * mirrors scripts/build_all.sh cc()/$LD and build_wl_app):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/clockapp/clockapp.c -o /tmp/clockapp.o
 *   gcc ... -c userspace/lib/wl/wl_client.c   -o /tmp/wlc.o
 *   gcc ... -c userspace/lib/font/bitfont.c   -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/clockapp.o /tmp/wlc.o /tmp/bf.o -o /tmp/clockapp.elf
 *   objdump -d /tmp/clockapp.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output (fd 1):
 *   [CLOCK+] starting
 *   [CLOCK+] window ready
 *   [CLOCK+] alarm fired / timer done / lap / reset ...
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* =========================================================================
 * Syscall numbers (must match kernel/include/syscall.h).
 * ========================================================================= */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40   /* monotonic ms since boot                  */
#define SYS_BEEP         45   /* PC-speaker / HDA tone(freq_hz, ms)       */

/* 3-arg inline syscall (rdi/rsi/rdx). No fs:0x28 stack canary. */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* =========================================================================
 * Minimal freestanding helpers / types.
 * ========================================================================= */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef long           i64;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

static i64 ticks_ms(void) { return sc(SYS_GET_TICKS_MS, 0, 0, 0); }

/* Audible alert: best-effort, ignored if SYS_BEEP isn't wired. */
static void beep(unsigned freq_hz, unsigned ms)
{
    sc(SYS_BEEP, (long)freq_hz, (long)ms, 0);
}

/* Write exactly `width` decimal digits (zero-padded) into dst[0..width-1]. */
static void int_to_dec(char *dst, i32 val, int width)
{
    if (val < 0) val = 0;
    for (int i = width - 1; i >= 0; i--) {
        dst[i] = (char)('0' + (val % 10));
        val /= 10;
    }
    dst[width] = '\0';
}

/* "HH:MM:SS" from milliseconds (uptime-clock). */
static void ms_to_hms(char *dst, i64 ms)
{
    if (ms < 0) ms = 0;
    i32 ss = (i32)((ms / 1000) % 60);
    i32 mm = (i32)((ms / 60000) % 60);
    i32 hh = (i32)((ms / 3600000) % 100);
    int_to_dec(dst,     hh, 2); dst[2] = ':';
    int_to_dec(dst + 3, mm, 2); dst[5] = ':';
    int_to_dec(dst + 6, ss, 2); dst[8] = '\0';
}

/* "MM:SS" from milliseconds. */
static void ms_to_mmss(char *dst, i64 ms)
{
    if (ms < 0) ms = 0;
    i32 ss = (i32)((ms / 1000) % 60);
    i32 mm = (i32)((ms / 60000) % 100);
    int_to_dec(dst,     mm, 2); dst[2] = ':';
    int_to_dec(dst + 3, ss, 2); dst[5] = '\0';
}

/* "MM:SS.cc" (centiseconds) from milliseconds -- stopwatch readout. */
static void ms_to_mmsscc(char *dst, i64 ms)
{
    if (ms < 0) ms = 0;
    i32 cc = (i32)((ms / 10) % 100);
    i32 ss = (i32)((ms / 1000) % 60);
    i32 mm = (i32)((ms / 60000) % 100);
    int_to_dec(dst,     mm, 2); dst[2] = ':';
    int_to_dec(dst + 3, ss, 2); dst[5] = '.';
    int_to_dec(dst + 6, cc, 2); dst[8] = '\0';
}

/* =========================================================================
 * Window geometry.
 * ========================================================================= */
#define WIN_W      760
#define WIN_H      520

/* Left navigation rail (Windows-11 style). */
#define RAIL_W     176
#define RAIL_PAD   12
#define NAV_X      8
#define NAV_W      (RAIL_W - 16)
#define NAV_H      48
#define NAV_GAP    8
#define NAV_Y0     64          /* below the rail title */

/* Content region (right of the rail). */
#define CT_X       (RAIL_W + 24)
#define CT_W       (WIN_W - RAIL_W - 48)
#define CT_Y       28

/* =========================================================================
 * Windows-11 inspired palette (light "Mica"-ish chrome, blue accent).
 * ========================================================================= */
#define COL_WIN_BG     0xFFF3F3F3u  /* window background (light)            */
#define COL_RAIL_BG    0xFFEAEAEAu  /* nav rail surface                     */
#define COL_CARD       0xFFFFFFFFu  /* content card                         */
#define COL_CARD_EDGE  0xFFE2E2E2u  /* card hairline border                 */
#define COL_NAV_SEL    0xFFE3EEFAu  /* selected nav item fill (tint)        */
#define COL_NAV_HOV    0xFFE9E9E9u  /* hovered nav item fill                */
#define COL_ACCENT     0xFF0067C0u  /* Windows blue accent                  */
#define COL_ACCENT_HI  0xFF1A7AD4u  /* accent hover                         */
#define COL_ACCENT_BAR 0xFF005FB8u  /* selection indicator pill             */
#define COL_TEXT       0xFF1A1A1Au  /* primary text                        */
#define COL_TEXT_DIM   0xFF606060u  /* secondary text                      */
#define COL_TEXT_ON_AC 0xFFFFFFFFu  /* text on accent                       */
#define COL_BTN        0xFFFBFBFBu  /* neutral button face                  */
#define COL_BTN_HOV    0xFFF0F0F0u  /* neutral button hover                 */
#define COL_BTN_EDGE   0xFFD6D6D6u  /* neutral button border                */
#define COL_FACE       0xFFFFFFFFu  /* analog clock face                    */
#define COL_RIM        0xFFCFCFCFu  /* analog rim                           */
#define COL_TICK_MAJ   0xFF707070u  /* hour ticks                           */
#define COL_TICK_MIN   0xFFC2C2C2u  /* minute ticks                         */
#define COL_HAND_HR    0xFF202020u  /* hour hand                            */
#define COL_HAND_MIN   0xFF404040u  /* minute hand                          */
#define COL_HAND_SEC   COL_ACCENT   /* second hand                          */
#define COL_FLASH      0xFFD13438u  /* alert flash (Windows red)            */
#define COL_OK         0xFF107C10u  /* "on/running" green                   */
#define COL_LAP_ROW     0xFFF6F6F6u  /* lap row band                         */

/* =========================================================================
 * Drawing primitives (clamped to the window).
 * ========================================================================= */
static u32 *FB;          /* shared pixel buffer            */
static u32  STRIDE;      /* stride in pixels (stride/4)    */

static void fill_rect(i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > WIN_W) x2 = WIN_W;
    i32 y2 = y + h; if (y2 > WIN_H) y2 = WIN_H;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = FB + (u32)yy * STRIDE;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void hline(i32 x, i32 y, i32 len, u32 c) { fill_rect(x, y, len, 1, c); }
static void vline(i32 x, i32 y, i32 len, u32 c) { fill_rect(x, y, 1, len, c); }

static void pset(i32 x, i32 y, u32 color)
{
    if (x < 0 || x >= WIN_W || y < 0 || y >= WIN_H) return;
    FB[(u32)y * STRIDE + (u32)x] = color;
}

/*
 * Rounded-rect fill (Windows-11 chrome).  Corners cut with an integer
 * radius^2 distance test; small radii (4..14) look crisp at this scale.
 */
static void fill_round_rect(i32 x, i32 y, i32 w, i32 h, i32 r, u32 color)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    i32 r2 = r * r;
    for (i32 yy = 0; yy < h; yy++) {
        i32 dy = (yy < r) ? (r - 1 - yy)
               : (yy >= h - r) ? (yy - (h - r)) : -1;
        i32 inset = 0;
        if (dy >= 0) {
            inset = r;
            while (inset > 0 && (r - 1 - inset) * (r - 1 - inset) + dy * dy > r2)
                inset--;
        }
        fill_rect(x + inset, y + yy, w - 2 * inset, 1, color);
    }
}

/* Rounded-rect 1px outline (drawn as a slightly larger fill behind, then the
 * face on top -- cheaper and crisper than tracing the arc by hand). */
static void round_rect_bordered(i32 x, i32 y, i32 w, i32 h, i32 r,
                                u32 face, u32 edge)
{
    fill_round_rect(x, y, w, h, r, edge);
    fill_round_rect(x + 1, y + 1, w - 2, h - 2, r - 1, face);
}

/* Bresenham line (pset clamps). */
static void draw_line(i32 x0, i32 y0, i32 x1, i32 y1, u32 color)
{
    i32 dx = x1 - x0; if (dx < 0) dx = -dx;
    i32 dy = y1 - y0; if (dy < 0) dy = -dy;
    i32 sx = (x1 > x0) ? 1 : -1;
    i32 sy = (y1 > y0) ? 1 : -1;
    i32 err = dx - dy;
    for (;;) {
        pset(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Thick line: parallel offsets (good for 2-3px hands). */
static void draw_thick_line(i32 x0, i32 y0, i32 x1, i32 y1, int t, u32 c)
{
    draw_line(x0, y0, x1, y1, c);
    if (t >= 2) {
        draw_line(x0 + 1, y0, x1 + 1, y1, c);
        draw_line(x0, y0 + 1, x1, y1 + 1, c);
    }
    if (t >= 3) {
        draw_line(x0 - 1, y0, x1 - 1, y1, c);
        draw_line(x0, y0 - 1, x1, y1 - 1, c);
    }
}

static void fill_circle(i32 cx, i32 cy, i32 r, u32 color)
{
    i32 r2 = r * r;
    for (i32 dy = -r; dy <= r; dy++) {
        i32 lim = r2 - dy * dy;
        i32 dx = r;
        while (dx > 0 && dx * dx > lim) dx--;
        fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

static void circle_outline(i32 cx, i32 cy, i32 r, u32 color)
{
    i32 x = r, y = 0, err = 0;
    while (x >= y) {
        pset(cx + x, cy + y, color); pset(cx + y, cy + x, color);
        pset(cx - y, cy + x, color); pset(cx - x, cy + y, color);
        pset(cx - x, cy - y, color); pset(cx - y, cy - x, color);
        pset(cx + y, cy - x, color); pset(cx + x, cy - y, color);
        y++; err += 2 * y - 1;
        if (err > 0) { x--; err -= 2 * x + 1; }
    }
}

/* =========================================================================
 * Text helpers.
 * ========================================================================= */
static void text(i32 x, i32 y, const char *s, u32 color)
{
    font_draw_string(FB, (int)STRIDE, WIN_W, WIN_H, x, y, s, color);
}

static void text_center(i32 cx, i32 y, const char *s, u32 color)
{
    int tw = font_text_width(s);
    text(cx - tw / 2, y, s, color);
}

/*
 * Big text: render each glyph into an 8x16 scratch buffer then block-scale by
 * `scale`.  Returns the total pixel width drawn (so callers can center).
 */
static int text_scaled(i32 x, i32 y, const char *s, int scale, u32 color)
{
    int cx = x;
    for (int i = 0; s[i]; i++) {
        u32 scratch[8 * 16];
        for (int k = 0; k < 8 * 16; k++) scratch[k] = 0;
        font_draw_char(scratch, 8, 8, 16, 0, 0, s[i], 0xFFFFFFFFu);
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (scratch[gy * 8 + gx])
                    fill_rect(cx + gx * scale, y + gy * scale, scale, scale, color);
        cx += 8 * scale;
    }
    return cx - x;
}

static int text_scaled_width(const char *s, int scale)
{
    return (int)k_strlen(s) * 8 * scale;
}

/* =========================================================================
 * Q16 sin/cos table (64 entries over 0..2*pi).  angle_idx maps a fraction of
 * a full turn to a table index with 0 = 12 o'clock, advancing clockwise.
 * ========================================================================= */
static const i32 sin64[64] = {
        0,   6424,  12539,  18204,  23170,  27245,  30273,  32138,
    32767,  32138,  30273,  27245,  23170,  18204,  12539,   6424,
        0,  -6424, -12539, -18204, -23170, -27245, -30273, -32138,
   -32767, -32138, -30273, -27245, -23170, -18204, -12539,  -6424,
        0,   6424,  12539,  18204,  23170,  27245,  30273,  32138,
    32767,  32138,  30273,  27245,  23170,  18204,  12539,   6424,
        0,  -6424, -12539, -18204, -23170, -27245, -30273, -32138,
   -32767, -32138, -30273, -27245, -23170, -18204, -12539,  -6424
};
static i32 isin64(int idx) { return sin64[idx & 63]; }
static i32 icos64(int idx) { return sin64[(idx + 16) & 63]; }

/* num/den fraction of a turn -> table index, 0 = top, clockwise. */
static int angle_idx(i32 num, i32 den)
{
    i32 idx = (64 * num / den) - 16;
    return idx & 63;
}

/* =========================================================================
 * Panes + click-rect helper.
 * ========================================================================= */
#define PANE_CLOCK     0
#define PANE_ALARM     1
#define PANE_TIMER     2
#define PANE_STOPWATCH 3
#define N_PANES        4

static const char *PANE_NAME[N_PANES] = { "Clock", "Alarm", "Timer", "Stopwatch" };
/* One-char "icon" glyph rendered in a tinted square at the nav-rail left. */
static const char  PANE_ICON[N_PANES] = { 'C', 'A', 'T', 'S' };

static int hit(i32 mx, i32 my, i32 x, i32 y, i32 w, i32 h)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* A labelled accent / neutral button.  Returns its hit-test rect via globals
 * is unnecessary -- callers pass explicit rects to hit(). */
static void draw_button(i32 x, i32 y, i32 w, i32 h, const char *label,
                        int accent, int hovered, int enabled)
{
    u32 face, edge, txt;
    if (accent) {
        face = hovered ? COL_ACCENT_HI : COL_ACCENT;
        edge = COL_ACCENT_BAR;
        txt  = COL_TEXT_ON_AC;
    } else {
        face = hovered ? COL_BTN_HOV : COL_BTN;
        edge = COL_BTN_EDGE;
        txt  = COL_TEXT;
    }
    if (!enabled) { face = COL_BTN; edge = COL_BTN_EDGE; txt = COL_TEXT_DIM; }
    round_rect_bordered(x, y, w, h, 7, face, edge);
    text_center(x + w / 2, y + (h - FONT_H) / 2, label, txt);
}

/* A small square +/- (or other) chip button. */
static void draw_chip(i32 x, i32 y, i32 s, const char *glyph, int hovered)
{
    round_rect_bordered(x, y, s, s, 6,
                        hovered ? COL_BTN_HOV : COL_BTN, COL_BTN_EDGE);
    text_center(x + s / 2, y + (s - FONT_H) / 2, glyph, COL_TEXT);
}

/* =========================================================================
 * Entry point.
 * ========================================================================= */
void _start(void)
{
    print("[CLOCK+] starting\n");

    if (wl_connect() != 0) {
        print("[CLOCK+] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Clock+");
    if (!win) {
        print("[CLOCK+] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }
    FB     = win->pixels;
    STRIDE = win->stride / 4u;
    print("[CLOCK+] window ready\n");

    /* ---- App state ---- */
    int pane = PANE_CLOCK;

    /* Mouse tracking (for hover + click edge detection). */
    i32 mx = -1, my = -1;
    int prev_btn = 0;

    /* Alarm: target = absolute uptime ms; offset dialed in mm:ss. */
    int alarm_off_mm = 0, alarm_off_ss = 30;  /* default +00:30 */
    int alarm_armed  = 0;
    i64 alarm_target = 0;
    int alarm_firing = 0;            /* latched until dismissed       */

    /* Timer: countdown dialed in mm:ss. */
    int timer_set_mm = 5, timer_set_ss = 0;   /* default 05:00 */
    i64 timer_remain_ms = (i64)(5 * 60) * 1000;
    int timer_running = 0;
    i64 timer_anchor  = 0;           /* tick when (re)started         */
    int timer_done    = 0;

    /* Stopwatch. */
    int sw_running = 0;
    i64 sw_accum   = 0;
    i64 sw_anchor  = 0;
    i64 sw_laps[6]; int sw_lap_n = 0;
    for (int i = 0; i < 6; i++) sw_laps[i] = 0;

    /* ---- Frame loop ---- */
    int kind, ea, eb, ec;
    for (;;) {
        i64 now = ticks_ms();

        /* ============================ INPUT ============================ */
        int click = 0;        /* a left-button press happened this frame   */
        int cx = mx, cy = my; /* click position                            */
        int key_press = 0, keycode = 0;

        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                mx = ea; my = eb;
                int btn = (ec & 1);
                if (btn && !prev_btn) { click = 1; cx = mx; cy = my; }
                prev_btn = btn;
            } else if (kind == WL_EVENT_KEY) {
                if (eb == 1) { key_press = 1; keycode = ea; }
            }
        }

        /* ---- Global keyboard shortcuts ---- */
        if (key_press) {
            switch (keycode) {
                case 2: pane = PANE_CLOCK;     break;  /* '1' */
                case 3: pane = PANE_ALARM;     break;  /* '2' */
                case 4: pane = PANE_TIMER;     break;  /* '3' */
                case 5: pane = PANE_STOPWATCH; break;  /* '4' */
                default: break;
            }
        }

        /* ---- Nav-rail clicks ---- */
        if (click && cx >= 0 && cx < RAIL_W) {
            for (int i = 0; i < N_PANES; i++) {
                i32 ny = NAV_Y0 + i * (NAV_H + NAV_GAP);
                if (hit(cx, cy, NAV_X, ny, NAV_W, NAV_H)) pane = i;
            }
        }

        /* ===================== PER-PANE LOGIC ====================== */

        /* ---------- Alarm ---------- */
        if (pane == PANE_ALARM) {
            /* Adjuster chip rects (mirrored in the draw section). */
            i32 ax = CT_X + 40, ay = CT_Y + 96, cs = 36, big = 64;
            /* mm -/+  then  ss -/+  (chip, gap, big number, gap, chip) */
            i32 mm_minus = ax;
            i32 mm_plus  = ax + cs + 8 + big + 8;
            i32 ss_minus = mm_plus + cs + 40;
            i32 ss_plus  = ss_minus + cs + 8 + big + 8;

            if (click) {
                if (hit(cx, cy, mm_minus, ay, cs, cs) && alarm_off_mm > 0) alarm_off_mm--;
                if (hit(cx, cy, mm_plus,  ay, cs, cs) && alarm_off_mm < 99) alarm_off_mm++;
                if (hit(cx, cy, ss_minus, ay, cs, cs)) alarm_off_ss = (alarm_off_ss + 59) % 60;
                if (hit(cx, cy, ss_plus,  ay, cs, cs)) alarm_off_ss = (alarm_off_ss + 1)  % 60;
            }

            /* Arm/Disarm button. */
            i32 bx = CT_X + 40, by = CT_Y + 188, bw = 150, bh = 44;
            int toggle = (click && hit(cx, cy, bx, by, bw, bh)) ||
                         (key_press && keycode == 57 /*Space*/);
            if (toggle) {
                if (!alarm_armed) {
                    i64 off = ((i64)alarm_off_mm * 60 + alarm_off_ss) * 1000;
                    alarm_target = now + off;
                    alarm_armed  = 1;
                    alarm_firing = 0;
                } else {
                    alarm_armed  = 0;
                    alarm_firing = 0;
                }
            }
            /* Dismiss-flash / reset. */
            if (key_press && keycode == 19 /*R*/) {
                alarm_armed = 0; alarm_firing = 0;
            }
            /* Fire check. */
            if (alarm_armed && !alarm_firing && now >= alarm_target) {
                alarm_firing = 1;
                print("[CLOCK+] alarm fired\n");
            }
            if (alarm_firing) {
                /* Beep in short bursts while firing. */
                if (((now / 400) & 1) == 0) beep(880, 120);
            }
        }

        /* ---------- Timer ---------- */
        if (pane == PANE_TIMER) {
            i32 ax = CT_X + 40, ay = CT_Y + 96, cs = 36, big = 64;
            i32 mm_minus = ax;
            i32 mm_plus  = ax + cs + 8 + big + 8;
            i32 ss_minus = mm_plus + cs + 40;
            i32 ss_plus  = ss_minus + cs + 8 + big + 8;

            if (click && !timer_running) {
                if (hit(cx, cy, mm_minus, ay, cs, cs) && timer_set_mm > 0)  timer_set_mm--;
                if (hit(cx, cy, mm_plus,  ay, cs, cs) && timer_set_mm < 99) timer_set_mm++;
                if (hit(cx, cy, ss_minus, ay, cs, cs)) timer_set_ss = (timer_set_ss + 59) % 60;
                if (hit(cx, cy, ss_plus,  ay, cs, cs)) timer_set_ss = (timer_set_ss + 1)  % 60;
                timer_remain_ms = ((i64)timer_set_mm * 60 + timer_set_ss) * 1000;
                timer_done = 0;
            }

            /* Start/Pause + Reset buttons. */
            i32 bx = CT_X + 40, by = CT_Y + 196, bw = 130, bh = 44, bgap = 16;
            i32 rx = bx + bw + bgap, rw = 110;
            int sp = (click && hit(cx, cy, bx, by, bw, bh)) ||
                     (key_press && keycode == 57 /*Space*/);
            int rs = (click && hit(cx, cy, rx, by, rw, bh)) ||
                     (key_press && keycode == 19 /*R*/);
            if (sp) {
                if (!timer_running) {
                    if (timer_remain_ms <= 0)
                        timer_remain_ms = ((i64)timer_set_mm * 60 + timer_set_ss) * 1000;
                    if (timer_remain_ms > 0) {
                        timer_running = 1;
                        timer_anchor  = now;
                        timer_done    = 0;
                    }
                } else {
                    /* pause: bank the elapsed slice */
                    timer_remain_ms -= (now - timer_anchor);
                    if (timer_remain_ms < 0) timer_remain_ms = 0;
                    timer_running = 0;
                }
            }
            if (rs) {
                timer_running   = 0;
                timer_remain_ms = ((i64)timer_set_mm * 60 + timer_set_ss) * 1000;
                timer_done      = 0;
                print("[CLOCK+] timer reset\n");
            }
            /* Countdown completion. */
            if (timer_running) {
                i64 left = timer_remain_ms - (now - timer_anchor);
                if (left <= 0 && !timer_done) {
                    timer_done      = 1;
                    timer_running   = 0;
                    timer_remain_ms = 0;
                    print("[CLOCK+] timer done\n");
                }
            }
            if (timer_done) {
                if (((now / 400) & 1) == 0) beep(660, 140);
            }
        }

        /* ---------- Stopwatch ---------- */
        if (pane == PANE_STOPWATCH) {
            i32 bx = CT_X + 40, by = CT_Y + 200, bw = 130, bh = 44, bgap = 16;
            i32 lx = bx + bw + bgap, lw = 110;
            i32 rx = lx + lw + bgap, rw = 110;
            int sp = (click && hit(cx, cy, bx, by, bw, bh)) ||
                     (key_press && keycode == 57 /*Space*/);
            int lap = (click && hit(cx, cy, lx, by, lw, bh)) ||
                      (key_press && keycode == 38 /*L*/);
            int rs = (click && hit(cx, cy, rx, by, rw, bh)) ||
                     (key_press && keycode == 19 /*R*/);
            if (sp) {
                if (!sw_running) { sw_running = 1; sw_anchor = now; }
                else { sw_accum += now - sw_anchor; sw_running = 0; }
            }
            if (lap && sw_running && sw_lap_n < 6) {
                sw_laps[sw_lap_n++] = sw_accum + (now - sw_anchor);
                print("[CLOCK+] lap\n");
            }
            if (rs) {
                sw_running = 0; sw_accum = 0; sw_lap_n = 0;
                print("[CLOCK+] sw reset\n");
            }
        }

        /* =========================== RENDER ========================== */
        fill_rect(0, 0, WIN_W, WIN_H, COL_WIN_BG);

        /* ---- Left navigation rail ---- */
        fill_rect(0, 0, RAIL_W, WIN_H, COL_RAIL_BG);
        vline(RAIL_W, 0, WIN_H, COL_CARD_EDGE);
        text(RAIL_PAD, 22, "Clock+", COL_TEXT);
        text(RAIL_PAD, 40, "AutomationOS", COL_TEXT_DIM);

        for (int i = 0; i < N_PANES; i++) {
            i32 ny = NAV_Y0 + i * (NAV_H + NAV_GAP);
            int sel = (pane == i);
            int hov = hit(mx, my, NAV_X, ny, NAV_W, NAV_H);
            u32 nf  = sel ? COL_NAV_SEL : (hov ? COL_NAV_HOV : COL_RAIL_BG);
            fill_round_rect(NAV_X, ny, NAV_W, NAV_H, 8, nf);
            /* Selection indicator pill (left). */
            if (sel) fill_round_rect(NAV_X + 2, ny + 12, 3, NAV_H - 24, 2, COL_ACCENT_BAR);
            /* Icon tile. */
            i32 ix = NAV_X + 14, iy = ny + (NAV_H - 28) / 2;
            char ig[2] = { PANE_ICON[i], 0 };
            fill_round_rect(ix, iy, 28, 28, 7, sel ? COL_ACCENT : COL_CARD_EDGE);
            text_center(ix + 14, iy + (28 - FONT_H) / 2, ig,
                        sel ? COL_TEXT_ON_AC : COL_TEXT_DIM);
            /* Label. */
            text(NAV_X + 52, ny + (NAV_H - FONT_H) / 2, PANE_NAME[i],
                 sel ? COL_TEXT : COL_TEXT_DIM);
        }

        /* ---- Content card ---- */
        round_rect_bordered(CT_X - 12, CT_Y - 12, CT_W + 24, WIN_H - CT_Y - 16,
                            12, COL_CARD, COL_CARD_EDGE);
        /* Pane title. */
        text_scaled(CT_X, CT_Y, PANE_NAME[pane], 2, COL_TEXT);
        hline(CT_X, CT_Y + 40, CT_W, COL_CARD_EDGE);

        /* =================== CLOCK PANE =================== */
        if (pane == PANE_CLOCK) {
            char hms[12];
            ms_to_hms(hms, now);

            /* Big digital readout (centered in left ~half of the card). */
            int scale = 5;
            int bw = text_scaled_width(hms, scale);
            int colcx = CT_X + (CT_W * 38 / 100);
            text_scaled(colcx - bw / 2, CT_Y + 96, hms, scale, COL_TEXT);
            text_center(colcx, CT_Y + 96 + 16 * scale + 14,
                        "Uptime clock - ms since boot", COL_TEXT_DIM);

            /* Analog face (right side of the card). */
            i32 cx0 = CT_X + (CT_W * 78 / 100);
            i32 cy0 = CT_Y + 180;
            i32 R   = 110;
            fill_circle(cx0, cy0, R, COL_FACE);
            circle_outline(cx0, cy0, R, COL_RIM);
            circle_outline(cx0, cy0, R - 1, COL_RIM);

            /* Tick marks. */
            for (int ti = 0; ti < 60; ti++) {
                int major = (ti % 5 == 0);
                int aidx  = angle_idx(ti, 60);
                i32 s = isin64(aidx), co = icos64(aidx);
                i32 r0 = R - (major ? 14 : 8), r1 = R - 4;
                i32 x0 = cx0 + (i32)(r0 * s  / 32767);
                i32 y0 = cy0 - (i32)(r0 * co / 32767);
                i32 x1 = cx0 + (i32)(r1 * s  / 32767);
                i32 y1 = cy0 - (i32)(r1 * co / 32767);
                u32 tc = major ? COL_TICK_MAJ : COL_TICK_MIN;
                draw_line(x0, y0, x1, y1, tc);
                if (major) draw_line(x0 + 1, y0, x1 + 1, y1, tc);
            }

            /* Derive 12h/60m/60s from uptime. */
            i64 secs = now / 1000;
            i32 sec  = (i32)(secs % 60);
            i32 minu = (i32)((secs / 60) % 60);
            i32 hr   = (i32)((secs / 3600) % 12);

            /* Hour hand (note: screen y grows downward, so subtract cos). */
            {
                int aidx = angle_idx(hr * 60 + minu, 720);
                i32 s = isin64(aidx), co = icos64(aidx);
                i32 rr = R * 50 / 100;
                draw_thick_line(cx0, cy0, cx0 + (i32)(rr * s / 32767),
                                cy0 - (i32)(rr * co / 32767), 3, COL_HAND_HR);
            }
            /* Minute hand. */
            {
                int aidx = angle_idx(minu * 60 + sec, 3600);
                i32 s = isin64(aidx), co = icos64(aidx);
                i32 rr = R * 74 / 100;
                draw_thick_line(cx0, cy0, cx0 + (i32)(rr * s / 32767),
                                cy0 - (i32)(rr * co / 32767), 2, COL_HAND_MIN);
            }
            /* Second hand + tail. */
            {
                int aidx = angle_idx(sec, 60);
                i32 s = isin64(aidx), co = icos64(aidx);
                i32 rr = R * 86 / 100, rt = R * 16 / 100;
                draw_line(cx0, cy0, cx0 + (i32)(rr * s / 32767),
                          cy0 - (i32)(rr * co / 32767), COL_HAND_SEC);
                draw_line(cx0, cy0, cx0 - (i32)(rt * s / 32767),
                          cy0 + (i32)(rt * co / 32767), COL_HAND_SEC);
            }
            fill_circle(cx0, cy0, 4, COL_HAND_HR);
        }

        /* =================== ALARM PANE =================== */
        else if (pane == PANE_ALARM) {
            int firing = alarm_firing;
            /* Flash the card content red on/off while firing. */
            if (firing && ((now / 300) & 1) == 0)
                fill_round_rect(CT_X - 2, CT_Y + 52, CT_W + 4, WIN_H - CT_Y - 80,
                                10, COL_FLASH);

            u32 lblc = (firing && ((now / 300) & 1) == 0) ? COL_TEXT_ON_AC : COL_TEXT_DIM;
            u32 numc = (firing && ((now / 300) & 1) == 0) ? COL_TEXT_ON_AC : COL_TEXT;

            text(CT_X + 40, CT_Y + 62, "Ring in (mm : ss from now)", lblc);

            i32 ax = CT_X + 40, ay = CT_Y + 96, cs = 36, big = 64;
            i32 mm_minus = ax;
            i32 mm_plus  = ax + cs + 8 + big + 8;
            i32 ss_minus = mm_plus + cs + 40;
            i32 ss_plus  = ss_minus + cs + 8 + big + 8;
            char nb[4];

            draw_chip(mm_minus, ay, cs, "-", hit(mx, my, mm_minus, ay, cs, cs));
            int_to_dec(nb, alarm_off_mm, 2);
            text_scaled(mm_minus + cs + 8, ay - 8, nb, 4, numc);
            draw_chip(mm_plus, ay, cs, "+", hit(mx, my, mm_plus, ay, cs, cs));

            text_scaled(mm_plus + cs + 14, ay - 8, ":", 4, numc);

            draw_chip(ss_minus, ay, cs, "-", hit(mx, my, ss_minus, ay, cs, cs));
            int_to_dec(nb, alarm_off_ss, 2);
            text_scaled(ss_minus + cs + 8, ay - 8, nb, 4, numc);
            draw_chip(ss_plus, ay, cs, "+", hit(mx, my, ss_plus, ay, cs, cs));

            /* Arm / Disarm button + status. */
            i32 bx = CT_X + 40, by = CT_Y + 188, bw = 150, bh = 44;
            draw_button(bx, by, bw, bh, alarm_armed ? "Disarm" : "Set alarm",
                        !alarm_armed, hit(mx, my, bx, by, bw, bh), 1);

            if (firing) {
                text(bx + bw + 24, by + (bh - FONT_H) / 2,
                     "ALARM!  (R or Disarm to stop)", COL_FLASH);
            } else if (alarm_armed) {
                i64 left = alarm_target - now; if (left < 0) left = 0;
                char tb[12]; ms_to_mmss(tb, left);
                char line[40]; int p = 0;
                const char *pre = "Armed - rings in ";
                for (int i = 0; pre[i]; i++) line[p++] = pre[i];
                for (int i = 0; tb[i]; i++) line[p++] = tb[i];
                line[p] = 0;
                text(bx + bw + 24, by + (bh - FONT_H) / 2, line, COL_OK);
            } else {
                text(bx + bw + 24, by + (bh - FONT_H) / 2, "Off", COL_TEXT_DIM);
            }

            /* Armed-alarm list row (On/Off badge). */
            i32 ly = CT_Y + 270;
            fill_round_rect(CT_X + 40, ly, CT_W - 80, 40, 8, COL_LAP_ROW);
            {
                char tb[12]; ms_to_mmss(tb, ((i64)alarm_off_mm*60+alarm_off_ss)*1000);
                char line[28]; int p = 0;
                const char *pre = "Alarm  +";
                for (int i = 0; pre[i]; i++) line[p++] = pre[i];
                for (int i = 0; tb[i]; i++) line[p++] = tb[i];
                line[p] = 0;
                text(CT_X + 56, ly + (40 - FONT_H) / 2, line, COL_TEXT);
            }
            {
                const char *badge = alarm_armed ? "On" : "Off";
                u32 bc = alarm_armed ? COL_OK : COL_TEXT_DIM;
                int tw = font_text_width(badge);
                text(CT_X + CT_W - 40 - tw, ly + (40 - FONT_H) / 2, badge, bc);
            }
            text(CT_X + 40, WIN_H - 44, "Space: arm/disarm   R: stop", COL_TEXT_DIM);
        }

        /* =================== TIMER PANE =================== */
        else if (pane == PANE_TIMER) {
            int done = timer_done;
            i64 left = timer_running ? (timer_remain_ms - (now - timer_anchor))
                                     : timer_remain_ms;
            if (left < 0) left = 0;

            if (done && ((now / 300) & 1) == 0)
                fill_round_rect(CT_X - 2, CT_Y + 52, CT_W + 4, WIN_H - CT_Y - 80,
                                10, COL_FLASH);
            u32 onflash = (done && ((now / 300) & 1) == 0);
            u32 numc = onflash ? COL_TEXT_ON_AC : COL_TEXT;
            u32 lblc = onflash ? COL_TEXT_ON_AC : COL_TEXT_DIM;

            if (!timer_running && !done) {
                /* Setup mode: mm:ss adjusters. */
                text(CT_X + 40, CT_Y + 62, "Set countdown (mm : ss)", lblc);
                i32 ax = CT_X + 40, ay = CT_Y + 96, cs = 36, big = 64;
                i32 mm_minus = ax;
                i32 mm_plus  = ax + cs + 8 + big + 8;
                i32 ss_minus = mm_plus + cs + 40;
                i32 ss_plus  = ss_minus + cs + 8 + big + 8;
                char nb[4];
                draw_chip(mm_minus, ay, cs, "-", hit(mx, my, mm_minus, ay, cs, cs));
                int_to_dec(nb, timer_set_mm, 2);
                text_scaled(mm_minus + cs + 8, ay - 8, nb, 4, numc);
                draw_chip(mm_plus, ay, cs, "+", hit(mx, my, mm_plus, ay, cs, cs));
                text_scaled(mm_plus + cs + 14, ay - 8, ":", 4, numc);
                draw_chip(ss_minus, ay, cs, "-", hit(mx, my, ss_minus, ay, cs, cs));
                int_to_dec(nb, timer_set_ss, 2);
                text_scaled(ss_minus + cs + 8, ay - 8, nb, 4, numc);
                draw_chip(ss_plus, ay, cs, "+", hit(mx, my, ss_plus, ay, cs, cs));
            } else {
                /* Running / done: big remaining readout. */
                char tb[12]; ms_to_mmss(tb, left);
                int scale = 6;
                int bw = text_scaled_width(tb, scale);
                text_scaled(CT_X + (CT_W - bw) / 2, CT_Y + 90, tb, scale,
                            done ? numc : COL_ACCENT);
                const char *msg = done ? "Time's up!" : "Counting down...";
                text_center(CT_X + CT_W / 2, CT_Y + 90 + 16 * scale + 16, msg, lblc);
            }

            /* Start/Pause + Reset. */
            i32 bx = CT_X + 40, by = CT_Y + 196, bw = 130, bh = 44, bgap = 16;
            i32 rx = bx + bw + bgap, rw = 110;
            draw_button(bx, by, bw, bh, timer_running ? "Pause" : "Start",
                        !timer_running, hit(mx, my, bx, by, bw, bh), 1);
            draw_button(rx, by, rw, bh, "Reset", 0, hit(mx, my, rx, by, rw, bh), 1);
            text(CT_X + 40, WIN_H - 44, "Space: start/pause   R: reset", COL_TEXT_DIM);
        }

        /* =================== STOPWATCH PANE =================== */
        else if (pane == PANE_STOPWATCH) {
            i64 disp = sw_accum + (sw_running ? (now - sw_anchor) : 0);
            char tb[12]; ms_to_mmsscc(tb, disp);
            int scale = 6;
            int bw = text_scaled_width(tb, scale);
            text_scaled(CT_X + (CT_W - bw) / 2, CT_Y + 80, tb, scale,
                        sw_running ? COL_ACCENT : COL_TEXT);

            i32 bx = CT_X + 40, by = CT_Y + 200, bw2 = 130, bh = 44, bgap = 16;
            i32 lx = bx + bw2 + bgap, lw = 110;
            i32 rx = lx + lw + bgap, rw = 110;
            draw_button(bx, by, bw2, bh, sw_running ? "Stop" : "Start",
                        !sw_running, hit(mx, my, bx, by, bw2, bh), 1);
            draw_button(lx, by, lw, bh, "Lap", 0, hit(mx, my, lx, by, lw, bh),
                        sw_running);
            draw_button(rx, by, rw, bh, "Reset", 0, hit(mx, my, rx, by, rw, bh), 1);

            /* Lap list. */
            i32 ly = by + bh + 18;
            if (sw_lap_n > 0) {
                text(CT_X + 40, ly, "Lap", COL_TEXT_DIM);
                text(CT_X + CT_W - 120, ly, "Split", COL_TEXT_DIM);
                hline(CT_X + 40, ly + FONT_H + 2, CT_W - 80, COL_CARD_EDGE);
                ly += FONT_H + 8;
                for (int i = sw_lap_n - 1; i >= 0; i--) {
                    int row = sw_lap_n - 1 - i;
                    i32 ry = ly + row * (FONT_H + 8);
                    fill_round_rect(CT_X + 40, ry - 2, CT_W - 80, FONT_H + 6, 5,
                                    COL_LAP_ROW);
                    char nb[8]; nb[0]='L'; nb[1]='a'; nb[2]='p'; nb[3]=' ';
                    int_to_dec(nb + 4, i + 1, 2); nb[6] = 0;
                    text(CT_X + 56, ry, nb, COL_TEXT_DIM);
                    char lt[12]; ms_to_mmsscc(lt, sw_laps[i]);
                    int lw2 = font_text_width(lt);
                    text(CT_X + CT_W - 56 - lw2, ry, lt, COL_ACCENT);
                }
            }
            text(CT_X + 40, WIN_H - 44,
                 "Space: start/stop   L: lap   R: reset", COL_TEXT_DIM);
        }

        /* ---- Present ---- */
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0);
    }
}
