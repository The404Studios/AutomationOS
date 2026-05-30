/*
 * stopwatch.c -- Digital stopwatch + analog/digital clock (freestanding, ring 3).
 * ================================================================================
 *
 * Window: 420x420, title "Stopwatch".
 *
 * Two tabs (top bar):
 *   [STOPWATCH]  --  MM:SS.cs readout with Start/Stop, Reset, Lap buttons.
 *                    Up to 5 lap times shown below.
 *   [CLOCK]      --  Digital HH:MM:SS + analog clock face.
 *                    (Hidden if SYS_GETTIME returns a negative value.)
 *
 * Keyboard:
 *   Space (57) -- Start / Stop
 *   R     (19) -- Reset
 *   L     (38) -- Lap
 *
 * Timing:
 *   Elapsed time is accumulated in `elapsed_ms` using tick-delta pairs:
 *     - On Start:  record start_tick = sc(SYS_GET_TICKS_MS,...)
 *     - On Stop:   elapsed_ms += sc(SYS_GET_TICKS_MS,...) - start_tick
 *     - On Reset:  elapsed_ms = 0; clear laps; set running=0.
 *     - Each frame redraw: displayed_ms = elapsed_ms + (running ? now-start_tick : 0)
 *
 * Analog clock face:
 *   Circle outline via midpoint (distance-squared test).
 *   Hands via Bresenham line, thickness 2-3 px.
 *   sin/cos from a 64-entry precomputed fixed-point table (Q16, i.e. *65536).
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/stopwatch/stopwatch.c -o /tmp/sw.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/sw.o /tmp/wlc.o /tmp/bf.o -o /tmp/sw.elf
 *   objdump -d /tmp/sw.elf | grep fs:0x28   # MUST be empty
 *
 * Serial:
 *   [SW] starting
 *   [SW] start
 *   [SW] stop
 *   [SW] reset
 *   [SW] lap
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* =========================================================================
 * Syscall numbers and inline helpers.
 * ========================================================================= */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_GETTIME      42

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
 * Minimal freestanding helpers.
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

/* Integer to decimal string, right-zero-padded to `width` digits.
 * dst must have at least width+1 bytes. Writes exactly `width` digits. */
static void int_to_dec(char *dst, i32 val, int width)
{
    for (int i = width - 1; i >= 0; i--) {
        dst[i] = '0' + (char)(val % 10);
        val /= 10;
    }
    dst[width] = '\0';
}

/* Build "MM:SS.cs" string from total milliseconds. */
static void ms_to_mmsscs(char *dst, i64 ms)
{
    if (ms < 0) ms = 0;
    i32 cs   = (i32)((ms / 10) % 100);
    i32 secs = (i32)((ms / 1000) % 60);
    i32 mins = (i32)((ms / 60000) % 100);
    /* MM:SS.cs  -> 8 chars + NUL */
    int_to_dec(dst,     mins, 2);
    dst[2] = ':';
    int_to_dec(dst + 3, secs, 2);
    dst[5] = '.';
    int_to_dec(dst + 6, cs,   2);
    dst[8] = '\0';
}

/* Build "HH:MM:SS" */
static void hms_to_str(char *dst, u8 h, u8 m, u8 s)
{
    int_to_dec(dst,     (i32)h, 2);
    dst[2] = ':';
    int_to_dec(dst + 3, (i32)m, 2);
    dst[5] = ':';
    int_to_dec(dst + 6, (i32)s, 2);
    dst[8] = '\0';
}

/* =========================================================================
 * RTC time struct (matches kernel rtc_time_t).
 * ========================================================================= */
typedef struct {
    u16 year;
    u8  month;
    u8  day;
    u8  hour;
    u8  min;
    u8  sec;
    u8  _pad;
} rtc_time_t;

/* =========================================================================
 * Window constants.
 * ========================================================================= */
#define WIN_W  420
#define WIN_H  420

/* Tab bar */
#define TAB_H       38
#define TAB_SW_X     0
#define TAB_SW_W   210
#define TAB_CL_X   210
#define TAB_CL_W   210

/* =========================================================================
 * Colors (dark theme).
 * ========================================================================= */
#define COL_BG         0xFF1A1A2Eu  /* deep navy background              */
#define COL_PANEL      0xFF16213Eu  /* slightly lighter panel            */
#define COL_TAB_ACTIVE 0xFF0F3460u  /* active tab                        */
#define COL_TAB_HOVER  0xFF1A1A3Eu  /* inactive tab                      */
#define COL_SEP        0xFF4A4A8Au  /* separator lines                   */
#define COL_TEXT       0xFFE0E0FFu  /* primary text (near-white blue)    */
#define COL_TEXT_DIM   0xFF7070A0u  /* dimmed text                       */
#define COL_ACCENT_RUN 0xFF00E5FFu  /* cyan accent while running         */
#define COL_ACCENT_STP 0xFFFFFFFFu  /* white when stopped                */
#define COL_BTN        0xFF0F3460u  /* button face                       */
#define COL_BTN_HL     0xFF1A5090u  /* button hover / active             */
#define COL_BTN_TXT    0xFFD0D0FFu  /* button label                      */
#define COL_LAP_ROW    0xFF1E1E3Cu  /* lap row background                */
#define COL_LAP_TXT    0xFF80C0FFu  /* lap time text                     */
#define COL_CLOCK_RIM  0xFF4040A0u  /* analog clock ring                 */
#define COL_CLOCK_FACE 0xFF0D0D25u  /* clock face fill                   */
#define COL_HAND_HR    0xFFFFFFFFu  /* hour hand                         */
#define COL_HAND_MIN   0xFF80C0FFu  /* minute hand                       */
#define COL_HAND_SEC   0xFF00E5FFu  /* second hand                       */
#define COL_TICK_MAJOR 0xFF8080C0u  /* hour tick marks                   */
#define COL_TICK_MINOR 0xFF404070u  /* minute tick marks                 */

/* =========================================================================
 * Drawing primitives.
 * ========================================================================= */
static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > WIN_W) x2 = WIN_W;
    i32 y2 = y + h; if (y2 > WIN_H) y2 = WIN_H;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = color;
    }
}

static void hline(u32 *buf, u32 stride_px,
                  i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, stride_px, x, y, len, 1, color);
}

static void vline(u32 *buf, u32 stride_px,
                  i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, stride_px, x, y, 1, len, color);
}

static void draw_border(u32 *buf, u32 stride_px,
                        i32 x, i32 y, i32 w, i32 h, u32 color)
{
    hline(buf, stride_px, x, y,         w, color);
    hline(buf, stride_px, x, y + h - 1, w, color);
    vline(buf, stride_px, x,         y, h, color);
    vline(buf, stride_px, x + w - 1, y, h, color);
}

/* Set a pixel (clamped). */
static void pset(u32 *buf, u32 stride_px, i32 x, i32 y, u32 color)
{
    if (x < 0 || x >= WIN_W || y < 0 || y >= WIN_H) return;
    buf[(u32)y * stride_px + (u32)x] = color;
}

/* Bresenham line (1-pixel, no clipping guard needed -- pset clamps). */
static void draw_line(u32 *buf, u32 stride_px,
                      i32 x0, i32 y0, i32 x1, i32 y1, u32 color)
{
    i32 dx = x1 - x0; if (dx < 0) dx = -dx;
    i32 dy = y1 - y0; if (dy < 0) dy = -dy;
    i32 sx = (x1 > x0) ? 1 : -1;
    i32 sy = (y1 > y0) ? 1 : -1;
    i32 err = dx - dy;
    for (;;) {
        pset(buf, stride_px, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Thick line: draw 3 parallel lines offset perpendicularly.
 * Works well for hand-thickness of 2-3 pixels. */
static void draw_thick_line(u32 *buf, u32 stride_px,
                            i32 x0, i32 y0, i32 x1, i32 y1,
                            int thickness, u32 color)
{
    draw_line(buf, stride_px, x0, y0, x1, y1, color);
    if (thickness >= 2) {
        i32 ddx = x1 - x0;
        i32 ddy = y1 - y0;
        /* perpendicular offset by 1 pixel */
        i32 len = ddx * ddx + ddy * ddy;
        /* use small integer perpendicular: swap + negate */
        (void)len;
        /* diagonal-ish: offset in both dims */
        draw_line(buf, stride_px, x0+1, y0, x1+1, y1, color);
        draw_line(buf, stride_px, x0, y0+1, x1, y1+1, color);
    }
    if (thickness >= 3) {
        draw_line(buf, stride_px, x0-1, y0, x1-1, y1, color);
        draw_line(buf, stride_px, x0, y0-1, x1, y1-1, color);
    }
}

/* Draw small filled circle (for clock hand tip / center dot). */
static void fill_circle_small(u32 *buf, u32 stride_px,
                               i32 cx, i32 cy, i32 r, u32 color)
{
    for (i32 dy = -r; dy <= r; dy++)
        for (i32 dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                pset(buf, stride_px, cx+dx, cy+dy, color);
}

/* Draw a circle outline (midpoint algorithm). */
static void draw_circle(u32 *buf, u32 stride_px,
                        i32 cx, i32 cy, i32 r, u32 color)
{
    i32 x = r, y = 0, err = 0;
    while (x >= y) {
        pset(buf, stride_px, cx+x, cy+y, color);
        pset(buf, stride_px, cx+y, cy+x, color);
        pset(buf, stride_px, cx-y, cy+x, color);
        pset(buf, stride_px, cx-x, cy+y, color);
        pset(buf, stride_px, cx-x, cy-y, color);
        pset(buf, stride_px, cx-y, cy-x, color);
        pset(buf, stride_px, cx+y, cy-x, color);
        pset(buf, stride_px, cx+x, cy-y, color);
        y++;
        err += 2*y - 1;
        if (err > 0) { x--; err -= 2*x + 1; }
    }
}

/* Draw a filled circle for clock face background (large, so scanline). */
static void fill_circle_large(u32 *buf, u32 stride_px,
                               i32 cx, i32 cy, i32 r, u32 color)
{
    i32 r2 = r * r;
    for (i32 dy = -r; dy <= r; dy++) {
        i32 y = cy + dy;
        if (y < 0 || y >= WIN_H) continue;
        /* compute dx range: dx^2 <= r2 - dy^2 */
        i32 lim = r2 - dy*dy;
        /* integer sqrt approximation (Newton) */
        i32 dx = r;
        while (dx > 0 && dx*dx > lim) dx--;
        fill_rect(buf, stride_px, cx-dx, y, 2*dx+1, 1, color);
    }
}

/* =========================================================================
 * Fixed-point sin/cos table (64 entries for 0..2*pi, Q16).
 * Index k covers angle k * (2*pi/64).
 * Generated: sin_table[k] = (int)(sin(k*2*pi/64) * 65536.0)
 * We use a 64-entry table; angles are mapped by (second * 64 / 60) etc.
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

/* cos(a) = sin(a + 16)  [quarter period = 16 entries] */
static i32 isin64(int idx) { return sin64[idx & 63]; }
static i32 icos64(int idx) { return sin64[(idx + 16) & 63]; }

/* Map angle (0=12 o'clock, clockwise) to table index.
 * angle_num / angle_den is fraction of full circle (0..1).
 * Returns index in 0..63. */
static int angle_idx(i32 num, i32 den)
{
    /* index = 64 * num / den, offset -16 so 0 = top (12 o'clock = -quarter) */
    /* 12 o'clock = -pi/2 => index offset = -16 */
    i32 idx = (64 * num / den) - 16;
    return idx & 63;
}

/* =========================================================================
 * Tab IDs.
 * ========================================================================= */
#define TAB_STOPWATCH 0
#define TAB_CLOCK     1

/* =========================================================================
 * Stopwatch state.
 * ========================================================================= */
#define MAX_LAPS 5

/* =========================================================================
 * Button geometry (stopwatch tab).
 * ========================================================================= */
/* Row 1: time display area -- y = TAB_H + 20..TAB_H+110 */
#define BTN_Y       (TAB_H + 140)
#define BTN_H       44
#define BTN_GAP     16

/* Three buttons horizontally centered.
 * Widths: Start/Stop=120, Reset=90, Lap=90 */
#define BTN_SS_W    120
#define BTN_RS_W     90
#define BTN_LP_W     90

#define BTN_TOTAL_W (BTN_SS_W + BTN_RS_W + BTN_LP_W + 2*BTN_GAP)
#define BTN_X0      ((WIN_W - BTN_TOTAL_W) / 2)

#define BTN_SS_X    BTN_X0
#define BTN_RS_X    (BTN_SS_X + BTN_SS_W + BTN_GAP)
#define BTN_LP_X    (BTN_RS_X + BTN_RS_W + BTN_GAP)

/* =========================================================================
 * Entry point.
 * ========================================================================= */
void _start(void)
{
    print("[SW] starting\n");

    if (wl_connect() != 0) {
        print("[SW] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Stopwatch");
    if (!win) {
        print("[SW] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    u32 stride_px = win->stride / 4u;

    /* ---- Check RTC availability ---- */
    rtc_time_t rtc;
    int rtc_ok = (sc(SYS_GETTIME, (long)&rtc, 0, 0) >= 0);

    /* ---- State ---- */
    int  tab          = TAB_STOPWATCH;
    int  running      = 0;
    i64  elapsed_ms   = 0;    /* total accumulated time (not including current run) */
    i64  start_tick   = 0;    /* tick snapshot when last started */
    i64  lap_ms[MAX_LAPS];
    int  lap_count    = 0;
    for (int i = 0; i < MAX_LAPS; i++) lap_ms[i] = 0;

    /* ---- Frame loop ---- */
    int kind, ea, eb, ec;
    for (;;) {

        /* ---- Current displayed time ---- */
        i64 now_tick = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        i64 disp_ms  = elapsed_ms + (running ? (now_tick - start_tick) : 0);

        /* ---- Drain events ---- */
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {

            if (kind == WL_EVENT_KEY && ec == 0 /* check b=pressed */) {
                /* eb = pressed flag */
                if (eb == 1) {
                    if (ea == 57) { /* Space */
                        if (!running) {
                            running    = 1;
                            start_tick = now_tick;
                            print("[SW] start\n");
                        } else {
                            elapsed_ms += now_tick - start_tick;
                            running     = 0;
                            print("[SW] stop\n");
                        }
                    } else if (ea == 19) { /* R */
                        elapsed_ms = 0;
                        running    = 0;
                        lap_count  = 0;
                        print("[SW] reset\n");
                    } else if (ea == 38) { /* L */
                        if (running && lap_count < MAX_LAPS) {
                            i64 cur_ms = elapsed_ms + (now_tick - start_tick);
                            lap_ms[lap_count++] = cur_ms;
                            print("[SW] lap\n");
                        }
                    }
                }
            }

            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea;
                i32 my = (i32)eb;
                int pressed = (ec & 1);

                if (pressed) {
                    /* Tab bar clicks */
                    if (my >= 0 && my < TAB_H) {
                        if (mx >= TAB_SW_X && mx < TAB_SW_X + TAB_SW_W)
                            tab = TAB_STOPWATCH;
                        else if (mx >= TAB_CL_X && mx < TAB_CL_X + TAB_CL_W)
                            tab = TAB_CLOCK;
                    }

                    /* Stopwatch button clicks */
                    if (tab == TAB_STOPWATCH &&
                        my >= BTN_Y && my < BTN_Y + BTN_H) {

                        /* Start/Stop button */
                        if (mx >= BTN_SS_X && mx < BTN_SS_X + BTN_SS_W) {
                            i64 t = sc(SYS_GET_TICKS_MS, 0, 0, 0);
                            if (!running) {
                                running    = 1;
                                start_tick = t;
                                print("[SW] start\n");
                            } else {
                                elapsed_ms += t - start_tick;
                                running     = 0;
                                print("[SW] stop\n");
                            }
                        }
                        /* Reset button */
                        if (mx >= BTN_RS_X && mx < BTN_RS_X + BTN_RS_W) {
                            elapsed_ms = 0;
                            running    = 0;
                            lap_count  = 0;
                            print("[SW] reset\n");
                        }
                        /* Lap button */
                        if (mx >= BTN_LP_X && mx < BTN_LP_X + BTN_LP_W) {
                            if (running && lap_count < MAX_LAPS) {
                                i64 t2 = sc(SYS_GET_TICKS_MS, 0, 0, 0);
                                i64 cur_ms = elapsed_ms + (t2 - start_tick);
                                lap_ms[lap_count++] = cur_ms;
                                print("[SW] lap\n");
                            }
                        }
                    }
                }
            }
        }

        /* ---- Recalculate disp_ms after event handling ---- */
        now_tick = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        disp_ms  = elapsed_ms + (running ? (now_tick - start_tick) : 0);

        /* ==================================================================
         * RENDER
         * ================================================================== */

        /* Background */
        fill_rect(win->pixels, stride_px, 0, 0, WIN_W, WIN_H, COL_BG);

        /* ---- Tab bar ---- */
        /* Stopwatch tab */
        u32 sw_tab_col = (tab == TAB_STOPWATCH) ? COL_TAB_ACTIVE : COL_TAB_HOVER;
        fill_rect(win->pixels, stride_px,
                  TAB_SW_X, 0, TAB_SW_W, TAB_H, sw_tab_col);
        {
            const char *lbl = "STOPWATCH";
            int tw = font_text_width(lbl);
            int tx = TAB_SW_X + (TAB_SW_W - tw) / 2;
            int ty = (TAB_H - FONT_H) / 2;
            font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                             tx, ty, lbl, COL_TEXT);
        }
        /* Active indicator bar */
        if (tab == TAB_STOPWATCH)
            hline(win->pixels, stride_px, TAB_SW_X, TAB_H - 2, TAB_SW_W, COL_ACCENT_STP);

        /* Clock tab (only if RTC available) */
        if (rtc_ok) {
            u32 cl_tab_col = (tab == TAB_CLOCK) ? COL_TAB_ACTIVE : COL_TAB_HOVER;
            fill_rect(win->pixels, stride_px,
                      TAB_CL_X, 0, TAB_CL_W, TAB_H, cl_tab_col);
            {
                const char *lbl = "CLOCK";
                int tw = font_text_width(lbl);
                int tx = TAB_CL_X + (TAB_CL_W - tw) / 2;
                int ty = (TAB_H - FONT_H) / 2;
                font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                 tx, ty, lbl, COL_TEXT);
            }
            if (tab == TAB_CLOCK)
                hline(win->pixels, stride_px, TAB_CL_X, TAB_H - 2, TAB_CL_W, COL_ACCENT_RUN);
        }

        /* Tab separator */
        hline(win->pixels, stride_px, 0, TAB_H - 1, WIN_W, COL_SEP);
        /* Divider between tabs */
        vline(win->pixels, stride_px, TAB_CL_X, 0, TAB_H, COL_SEP);

        /* Content area y = TAB_H .. WIN_H */

        if (tab == TAB_STOPWATCH) {
            /* ============================================================
             * STOPWATCH TAB
             * ============================================================ */

            /* ---- Big time readout (MM:SS.cs) ---- */
            /* We scale up the font using a 3x character cell:
             * each char is drawn 3 times with 1px offsets to fake bold,
             * then we space them out.
             * For simplicity: draw at scale 3 via repeated offsets. */
            char tbuf[16];
            ms_to_mmsscs(tbuf, disp_ms);

            /* Draw large time in the center of the content area.
             * Use scale=4 by rendering each character multiple times shifted. */
            int scale = 4;
            int ch_w  = FONT_W * scale;
            int ch_h  = FONT_H * scale;
            int tw    = (int)k_strlen(tbuf) * ch_w;
            int tx    = (WIN_W - tw) / 2;
            int ty    = TAB_H + 28;
            u32 time_color = running ? COL_ACCENT_RUN : COL_ACCENT_STP;

            /* Render each character at 4x scale via tiled pixel drawing */
            for (int ci = 0; tbuf[ci]; ci++) {
                char c = tbuf[ci];
                int cx_base = tx + ci * ch_w;
                /* Get the glyph row bits via font_draw_char into a tiny temp buf,
                 * then upscale. We don't have direct glyph access, so instead
                 * we render the character at 1x into a scratch row and upscale. */
                /* Strategy: render char to scratch strip, then upscale to screen */
                /* scratch: 8 wide, 16 tall */
                u32 scratch[8 * 16];
                for (int si = 0; si < 8*16; si++) scratch[si] = 0;
                font_draw_char(scratch, 8, 8, 16, 0, 0, c, 0xFFFFFFFFu);
                /* Upscale to screen */
                for (int gy = 0; gy < 16; gy++) {
                    for (int gx = 0; gx < 8; gx++) {
                        if (scratch[gy*8 + gx]) {
                            /* paint scale x scale block */
                            fill_rect(win->pixels, stride_px,
                                      cx_base + gx*scale,
                                      ty + gy*scale,
                                      scale, scale, time_color);
                        }
                    }
                }
            }

            /* ---- Thin separator below time ---- */
            hline(win->pixels, stride_px,
                  tx - 8, ty + ch_h + 8, tw + 16, COL_SEP);

            /* ---- Buttons ---- */
            /* Start/Stop */
            {
                u32 btn_col = running ? 0xFF003050u : COL_BTN;
                fill_rect(win->pixels, stride_px,
                          BTN_SS_X, BTN_Y, BTN_SS_W, BTN_H, btn_col);
                draw_border(win->pixels, stride_px,
                            BTN_SS_X, BTN_Y, BTN_SS_W, BTN_H,
                            running ? COL_ACCENT_RUN : COL_SEP);
                const char *lbl = running ? "STOP" : "START";
                int lw = font_text_width(lbl);
                font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                 BTN_SS_X + (BTN_SS_W - lw) / 2,
                                 BTN_Y + (BTN_H - FONT_H) / 2,
                                 lbl,
                                 running ? COL_ACCENT_RUN : COL_BTN_TXT);
            }
            /* Reset */
            {
                fill_rect(win->pixels, stride_px,
                          BTN_RS_X, BTN_Y, BTN_RS_W, BTN_H, COL_BTN);
                draw_border(win->pixels, stride_px,
                            BTN_RS_X, BTN_Y, BTN_RS_W, BTN_H, COL_SEP);
                const char *lbl = "RESET";
                int lw = font_text_width(lbl);
                font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                 BTN_RS_X + (BTN_RS_W - lw) / 2,
                                 BTN_Y + (BTN_H - FONT_H) / 2,
                                 lbl, COL_BTN_TXT);
            }
            /* Lap */
            {
                u32 lap_col = (running && lap_count < MAX_LAPS) ? COL_BTN : 0xFF101020u;
                fill_rect(win->pixels, stride_px,
                          BTN_LP_X, BTN_Y, BTN_LP_W, BTN_H, lap_col);
                draw_border(win->pixels, stride_px,
                            BTN_LP_X, BTN_Y, BTN_LP_W, BTN_H, COL_SEP);
                const char *lbl = "LAP";
                int lw = font_text_width(lbl);
                u32 ltxt = (running && lap_count < MAX_LAPS) ? COL_BTN_TXT : COL_TEXT_DIM;
                font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                 BTN_LP_X + (BTN_LP_W - lw) / 2,
                                 BTN_Y + (BTN_H - FONT_H) / 2,
                                 lbl, ltxt);
            }

            /* ---- Lap list ---- */
            int lap_y0 = BTN_Y + BTN_H + 14;
            if (lap_count > 0) {
                /* Header */
                font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                 24, lap_y0, "LAP", COL_TEXT_DIM);
                font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                 WIN_W - 100, lap_y0, "TIME", COL_TEXT_DIM);
                hline(win->pixels, stride_px,
                      20, lap_y0 + FONT_H + 2, WIN_W - 40, COL_SEP);
                lap_y0 += FONT_H + 6;

                for (int li = lap_count - 1; li >= 0 && (lap_count - 1 - li) < MAX_LAPS; li--) {
                    int row = lap_count - 1 - li;
                    int ry  = lap_y0 + row * (FONT_H + 4);
                    /* Row background */
                    fill_rect(win->pixels, stride_px,
                              20, ry, WIN_W - 40, FONT_H + 2, COL_LAP_ROW);
                    /* Lap number */
                    char nbuf[8];
                    nbuf[0] = 'L'; nbuf[1] = 'a'; nbuf[2] = 'p'; nbuf[3] = ' ';
                    int_to_dec(nbuf + 4, li + 1, 2);
                    nbuf[6] = '\0';
                    font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                     28, ry + 1, nbuf, COL_TEXT_DIM);
                    /* Lap time */
                    char ltbuf[16];
                    ms_to_mmsscs(ltbuf, lap_ms[li]);
                    int ltw = font_text_width(ltbuf);
                    font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                     WIN_W - 20 - ltw, ry + 1, ltbuf, COL_LAP_TXT);
                }
            }

        } else {
            /* ============================================================
             * CLOCK TAB
             * ============================================================ */

            /* Refresh RTC */
            sc(SYS_GETTIME, (long)&rtc, 0, 0);

            /* ---- Digital time display ---- */
            char hms[16];
            hms_to_str(hms, rtc.hour, rtc.min, rtc.sec);

            int scale = 3;
            int ch_w  = FONT_W * scale;
            int tw    = (int)k_strlen(hms) * ch_w;
            int tx    = (WIN_W - tw) / 2;
            int ty    = TAB_H + 12;

            for (int ci = 0; hms[ci]; ci++) {
                char c = hms[ci];
                int cx_base = tx + ci * ch_w;
                u32 scratch[8 * 16];
                for (int si = 0; si < 8*16; si++) scratch[si] = 0;
                font_draw_char(scratch, 8, 8, 16, 0, 0, c, 0xFFFFFFFFu);
                for (int gy = 0; gy < 16; gy++) {
                    for (int gx = 0; gx < 8; gx++) {
                        if (scratch[gy*8 + gx]) {
                            fill_rect(win->pixels, stride_px,
                                      cx_base + gx*scale,
                                      ty + gy*scale,
                                      scale, scale, COL_ACCENT_RUN);
                        }
                    }
                }
            }

            /* Date line: YYYY-MM-DD */
            {
                char dbuf[16];
                /* year */
                int_to_dec(dbuf, (i32)rtc.year, 4);
                dbuf[4] = '-';
                int_to_dec(dbuf + 5, (i32)rtc.month, 2);
                dbuf[7] = '-';
                int_to_dec(dbuf + 8, (i32)rtc.day, 2);
                dbuf[10] = '\0';
                int dtw = font_text_width(dbuf);
                font_draw_string(win->pixels, (int)stride_px, WIN_W, WIN_H,
                                 (WIN_W - dtw) / 2,
                                 ty + FONT_H * scale + 4,
                                 dbuf, COL_TEXT_DIM);
            }

            /* ---- Analog clock face ---- */
            /* Center and radius */
            i32 face_y0 = ty + FONT_H * scale + 4 + FONT_H + 10;
            i32 face_h  = WIN_H - face_y0 - 10;
            i32 r_outer = (face_h < WIN_W - 60) ? face_h / 2 : (WIN_W - 60) / 2;
            if (r_outer < 20) r_outer = 20;
            i32 cx = WIN_W / 2;
            i32 cy = face_y0 + r_outer;

            /* Fill face */
            fill_circle_large(win->pixels, stride_px, cx, cy, r_outer, COL_CLOCK_FACE);

            /* Rim rings */
            draw_circle(win->pixels, stride_px, cx, cy, r_outer,     COL_CLOCK_RIM);
            draw_circle(win->pixels, stride_px, cx, cy, r_outer - 1, COL_CLOCK_RIM);
            draw_circle(win->pixels, stride_px, cx, cy, r_outer - 2, COL_SEP);

            /* Tick marks */
            for (int ti = 0; ti < 60; ti++) {
                int is_hour = (ti % 5 == 0);
                int aidx    = angle_idx(ti, 60);
                i32 s       = isin64(aidx);
                i32 co      = icos64(aidx);
                i32 r0 = r_outer - (is_hour ? 10 : 6);
                i32 r1 = r_outer - 3;
                i32 x0 = cx + (i32)(r0 * s  / 32767);
                i32 y0_ = cy + (i32)(r0 * co / 32767);  /* note: cos for vertical */
                i32 x1 = cx + (i32)(r1 * s  / 32767);
                i32 y1_ = cy + (i32)(r1 * co / 32767);
                u32 tc = is_hour ? COL_TICK_MAJOR : COL_TICK_MINOR;
                draw_line(win->pixels, stride_px, x0, y0_, x1, y1_, tc);
                if (is_hour)
                    draw_line(win->pixels, stride_px, x0+1, y0_, x1+1, y1_, tc);
            }

            /* Hour hand: 0..11 hours + fractional minutes */
            {
                i32 total_min = (i32)rtc.hour * 60 + (i32)rtc.min;
                int aidx = angle_idx(total_min, 720); /* 12 hours = 720 min */
                i32 s  = isin64(aidx);
                i32 co = icos64(aidx);
                i32 rh = r_outer * 50 / 100;
                i32 hx = cx + (i32)(rh * s  / 32767);
                i32 hy = cy + (i32)(rh * co / 32767);
                draw_thick_line(win->pixels, stride_px, cx, cy, hx, hy, 3, COL_HAND_HR);
            }

            /* Minute hand */
            {
                i32 total_sec = (i32)rtc.min * 60 + (i32)rtc.sec;
                int aidx = angle_idx(total_sec, 3600); /* 60 min = 3600 sec */
                i32 s  = isin64(aidx);
                i32 co = icos64(aidx);
                i32 rm = r_outer * 75 / 100;
                i32 mx2 = cx + (i32)(rm * s  / 32767);
                i32 my2 = cy + (i32)(rm * co / 32767);
                draw_thick_line(win->pixels, stride_px, cx, cy, mx2, my2, 2, COL_HAND_MIN);
            }

            /* Second hand */
            {
                int aidx = angle_idx((i32)rtc.sec, 60);
                i32 s  = isin64(aidx);
                i32 co = icos64(aidx);
                i32 rs = r_outer * 85 / 100;
                i32 sx2 = cx + (i32)(rs * s  / 32767);
                i32 sy2 = cy + (i32)(rs * co / 32767);
                draw_line(win->pixels, stride_px, cx, cy, sx2, sy2, COL_HAND_SEC);
                /* counter-weight tail */
                i32 rt = r_outer * 15 / 100;
                i32 tx2 = cx - (i32)(rt * s  / 32767);
                i32 ty2 = cy - (i32)(rt * co / 32767);
                draw_line(win->pixels, stride_px, cx, cy, tx2, ty2, COL_HAND_SEC);
            }

            /* Center dot */
            fill_circle_small(win->pixels, stride_px, cx, cy, 3, COL_HAND_HR);
        }

        /* ---- Commit frame ---- */
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0);
    }
}
