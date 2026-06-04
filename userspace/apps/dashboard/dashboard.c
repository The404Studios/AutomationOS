/*
 * dashboard.c -- System dashboard (freestanding, ring 3, WL-DIRECT).
 * ==================================================================
 *
 * A polished system-monitor dashboard for AutomationOS.
 * Window: 520x420, title "Dashboard".
 *
 * Data sources:
 *   SYS_GET_TICKS_MS (40) -- uptime milliseconds, always available.
 *   SYS_PROCLIST     (44) -- fills procinfo_t array, returns count.
 *
 * Layout (dark theme):
 *   [0..39]   Header bar: "AutomationOS" + "Uptime HH:MM:SS"
 *   [40..119] Gauge row: "Processes: N" bar + activity sparkline
 *   [120..419] Process list panel (top 6 procs) + rounded panels
 *
 * Build (ALL flags DIRECT on cmdline):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/dashboard/dashboard.c -o /tmp/db.o
 *   gcc <same> -c userspace/lib/wl/wl_client.c   -o /tmp/wlc.o
 *   gcc <same> -c userspace/lib/font/bitfont.c   -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/db.o /tmp/wlc.o /tmp/bf.o -o /tmp/db.elf
 *   objdump -d /tmp/db.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [DASH] starting
 *   [DASH] procs=N uptime=Ts
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline helper (3-arg, no shell-var risk).
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_PROCLIST     44

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
 * Freestanding types and helpers.
 * --------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;

static u64 k_strlen(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void serial(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/* Format a decimal number, zero-padded to `width` digits, into buf. */
static char *fmt_uint(u64 v, char *buf, int width)
{
    char tmp[22];
    int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { u64 x = v; while (x) { tmp[i++] = (char)('0' + x % 10); x /= 10; } }
    /* tmp has digits in reverse; right-justify with zero-padding */
    int total = i < width ? width : i;
    char *p = buf;
    /* leading pad */
    for (int k = i; k < width; k++) *p++ = '0';
    /* reverse digits */
    for (int j = i - 1; j >= 0; j--) *p++ = tmp[j];
    *p = '\0';
    return buf;
}

/* Append src to dst, return pointer past NUL. */
static char *append(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

/* Format ms as "HH:MM:SS" into buf (must be >=9 bytes). */
static void fmt_hms(char *buf, u64 ms)
{
    u64 secs  = ms / 1000ULL;
    u64 hh    = secs / 3600ULL;
    u64 mm    = (secs % 3600ULL) / 60ULL;
    u64 ss    = secs % 60ULL;
    char nb[8];
    char *p = buf;
    p = append(p, fmt_uint(hh, nb, 2)); *p++ = ':';
    p = append(p, fmt_uint(mm, nb, 2)); *p++ = ':';
    p = append(p, fmt_uint(ss, nb, 2));
    *p = '\0';
}

/* Format u64 decimal (no padding). */
static void fmt_u64(u64 v, char *buf)
{
    fmt_uint(v, buf, 1);
}

/* -----------------------------------------------------------------------
 * procinfo_t: 64 bytes matching kernel layout (SYS_PROCLIST proc_info_t).
 * { unsigned pid, parent_pid, state, flags; char name[32];
 *   u64 cpu_ticks; u64 ctx_switches; }
 * --------------------------------------------------------------------- */
typedef struct {
    unsigned int       pid;
    unsigned int       parent_pid;
    unsigned int       state;
    unsigned int       flags;
    char               name[32];
    unsigned long long cpu_ticks;
    unsigned long long ctx_switches;
} procinfo_t;

#define MAX_PROCS  32
#define SHOW_PROCS  6

/* -----------------------------------------------------------------------
 * Window geometry.
 * --------------------------------------------------------------------- */
#define WIN_W   520
#define WIN_H   420

/* Header bar */
#define HDR_H   44

/* Gauge panel */
#define GAUGE_Y   50
#define GAUGE_H   76

/* Process list panel */
#define PROC_Y    136
#define PROC_H    (WIN_H - PROC_Y - 8)

/* Sparkline ring buffer */
#define SPARK_LEN  120

/* -----------------------------------------------------------------------
 * Color palette (dark theme with accent gradients).
 * --------------------------------------------------------------------- */
#define C_BG         0xFF0F1117u   /* main background              */
#define C_HDR_TOP    0xFF1A1F2Eu   /* header gradient top          */
#define C_HDR_BOT    0xFF141824u   /* header gradient bottom       */
#define C_PANEL      0xFF161B26u   /* panel background             */
#define C_PANEL2     0xFF1E2435u   /* alternate panel shade        */
#define C_BORDER     0xFF2A3248u   /* panel border                 */
#define C_ACCENT     0xFF4E9EFFu   /* blue accent                  */
#define C_ACCENT2    0xFF7B5EFFu   /* purple accent                */
#define C_ACCENT3    0xFF00E5B4u   /* teal accent                  */
#define C_TEXT       0xFFEAEDF5u   /* primary text                 */
#define C_TEXT2      0xFF9BA8C8u   /* secondary text               */
#define C_TEXT3      0xFF5A6585u   /* dim text                     */
#define C_BAR_BG     0xFF1E2435u   /* bar track                    */
#define C_BAR_FG     0xFF4E9EFFu   /* bar fill                     */
#define C_BAR_HOT    0xFFFF6B6Bu   /* bar fill when high           */
#define C_SPARK_FG   0xFF00E5B4u   /* sparkline line colour        */
#define C_SPARK_FILL 0xFF003D2Fu   /* sparkline fill colour        */
#define C_ROW_A      0xFF161B26u   /* row even background          */
#define C_ROW_B      0xFF1A2030u   /* row odd background           */
#define C_ROW_RUN    0xFF1A2E1Au   /* running process highlight    */
#define C_SEP        0xFF252D42u   /* separator line               */
#define C_WHITE      0xFFFFFFFFu

/* State color */
static u32 state_color(unsigned int s)
{
    switch (s) {
        case 0:  return 0xFF4CFF82u;   /* running -- green  */
        case 1:  return 0xFF4E9EFFu;   /* ready   -- blue   */
        case 2:  return 0xFFFFBF42u;   /* blocked -- amber  */
        case 3:  return 0xFFFF5555u;   /* zombie  -- red    */
        default: return C_TEXT3;
    }
}

static const char *state_str(unsigned int s)
{
    switch (s) {
        case 0:  return "RUN  ";
        case 1:  return "READY";
        case 2:  return "BLOCK";
        case 3:  return "ZOMBI";
        default: return "?    ";
    }
}

/* -----------------------------------------------------------------------
 * Drawing primitives.
 * stride_px = stride / 4 (pixels per row).
 * --------------------------------------------------------------------- */

static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = col;
    }
}

/* Blend a color over a pixel (src-alpha simple lerp, 8-bit alpha from col). */
static void blend_pixel(u32 *p, u32 col, u32 alpha)
{
    /* alpha in [0,255]: 0=transparent, 255=opaque */
    u32 rb = (*p & 0x00FF00FFu);
    u32 ag = (*p & 0xFF00FF00u) >> 8;
    u32 srb = (col & 0x00FF00FFu);
    u32 sag = (col & 0xFF00FF00u) >> 8;
    u32 out_rb = (rb  + ((srb  - rb)  * alpha >> 8)) & 0x00FF00FFu;
    u32 out_ag = (ag  + ((sag  - ag)  * alpha >> 8)) & 0x00FF00FFu;
    *p = (out_ag << 8) | out_rb;
}

/* Vertical gradient fill over a rectangle. */
static void fill_grad_v(u32 *buf, u32 bw, u32 bh, u32 spx,
                        i32 x, i32 y, i32 w, i32 h,
                        u32 col_top, u32 col_bot)
{
    for (i32 dy = 0; dy < h; dy++) {
        /* Interpolate R, G, B channels */
        u32 t = (u32)dy * 255u / (u32)(h > 1 ? h - 1 : 1);
        u32 r = ((col_top >> 16 & 0xFF) * (255 - t) + (col_bot >> 16 & 0xFF) * t) / 255;
        u32 g = ((col_top >>  8 & 0xFF) * (255 - t) + (col_bot >>  8 & 0xFF) * t) / 255;
        u32 b = ((col_top       & 0xFF) * (255 - t) + (col_bot       & 0xFF) * t) / 255;
        u32 col = 0xFF000000u | (r << 16) | (g << 8) | b;
        fill_rect(buf, bw, bh, spx, x, y + dy, w, 1, col);
    }
}

/*
 * Rounded-rectangle fill: fills a rounded rect with corner radius `r`.
 * Uses simple circle-corner masking (no sqrt, integer test).
 */
static void fill_rrect(u32 *buf, u32 bw, u32 bh, u32 spx,
                       i32 x, i32 y, i32 w, i32 h, i32 cr, u32 col)
{
    if (cr < 1 || w < 2 * cr || h < 2 * cr) {
        fill_rect(buf, bw, bh, spx, x, y, w, h, col);
        return;
    }
    /* Body (middle strip without corners) */
    fill_rect(buf, bw, bh, spx, x,      y + cr, w,  h - 2 * cr, col);
    /* Top strip */
    fill_rect(buf, bw, bh, spx, x + cr, y,      w - 2 * cr, cr, col);
    /* Bottom strip */
    fill_rect(buf, bw, bh, spx, x + cr, y + h - cr, w - 2 * cr, cr, col);
    /* Four corners: scan row by row */
    for (i32 dy = 0; dy < cr; dy++) {
        /* Determine horizontal extent at this row from corner edges */
        i32 dist = cr - 1 - dy;
        /* Integer circle: x^2 + dy^2 <= cr^2, find max dx for each row */
        i32 dx = 0;
        while ((dx + 1) * (dx + 1) + dist * dist <= cr * cr) dx++;
        /* top-left corner */
        fill_rect(buf, bw, bh, spx,
                  x + cr - 1 - dx, y + dy,
                  dx + 1, 1, col);
        /* top-right corner */
        fill_rect(buf, bw, bh, spx,
                  x + w - cr, y + dy,
                  dx + 1, 1, col);
        /* bottom-left corner */
        fill_rect(buf, bw, bh, spx,
                  x + cr - 1 - dx, y + h - 1 - dy,
                  dx + 1, 1, col);
        /* bottom-right corner */
        fill_rect(buf, bw, bh, spx,
                  x + w - cr, y + h - 1 - dy,
                  dx + 1, 1, col);
    }
}

/* Draw a 1-px border around a rounded rect (top/bottom/left/right lines). */
static void border_rrect(u32 *buf, u32 bw, u32 bh, u32 spx,
                         i32 x, i32 y, i32 w, i32 h, u32 col)
{
    /* Simple bounding box border (corners visually fine at 8px radius) */
    fill_rect(buf, bw, bh, spx, x,     y,     w, 1, col);
    fill_rect(buf, bw, bh, spx, x,     y+h-1, w, 1, col);
    fill_rect(buf, bw, bh, spx, x,     y,     1, h, col);
    fill_rect(buf, bw, bh, spx, x+w-1, y,     1, h, col);
}

/* Draw a horizontal progress bar from x,y, total width W, fill frac 0..255. */
static void draw_bar(u32 *buf, u32 bw, u32 bh, u32 spx,
                     i32 x, i32 y, i32 w, i32 h,
                     u32 frac_256, u32 col_bg, u32 col_fg)
{
    fill_rrect(buf, bw, bh, spx, x, y, w, h, h / 2, col_bg);
    i32 filled = (i32)((u64)w * frac_256 / 256ULL);
    if (filled > 0)
        fill_rrect(buf, bw, bh, spx, x, y, filled, h, h / 2, col_fg);
}

/*
 * Draw a sparkline (filled area chart) from a ring buffer.
 * ring[head] is the oldest sample; values are in [0, max_val].
 * Area fills from bottom of the chart rectangle upwards.
 */
static void draw_sparkline(u32 *buf, u32 bw, u32 bh, u32 spx,
                           i32 x, i32 y, i32 w, i32 h,
                           const unsigned int *ring, int ring_len,
                           int head, unsigned int max_val,
                           u32 col_fill, u32 col_line)
{
    if (max_val == 0) max_val = 1;
    /* Draw one vertical column per pixel-column (scaled over ring_len samples) */
    for (i32 px = 0; px < w; px++) {
        int idx = (head + (int)((u64)px * (u64)ring_len / (u64)w)) % ring_len;
        unsigned int val = ring[idx];
        i32 bar_h = (i32)((u64)val * (u64)h / (u64)max_val);
        if (bar_h > h) bar_h = h;
        if (bar_h > 0) {
            /* Filled area */
            fill_rect(buf, bw, bh, spx, x + px, y + h - bar_h, 1, bar_h, col_fill);
            /* Top pixel brighter (line effect) */
            fill_rect(buf, bw, bh, spx, x + px, y + h - bar_h, 1, 1, col_line);
        }
    }
}

/* -----------------------------------------------------------------------
 * Application state.
 * --------------------------------------------------------------------- */

static procinfo_t g_procs[MAX_PROCS];
static int        g_proc_count = 0;

/* Sparkline ring buffer: stores frame-tick deltas as a proxy for activity. */
static unsigned int g_spark[SPARK_LEN];
static int          g_spark_head = 0;     /* oldest entry index */
static unsigned int g_spark_max  = 1;

/* Serial log throttle */
static int g_serial_frame = 0;
#define SERIAL_INTERVAL  300   /* ~10 seconds at ~30fps */

/* -----------------------------------------------------------------------
 * String helpers (no libc).
 * --------------------------------------------------------------------- */

static int strncpy_safe(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* -----------------------------------------------------------------------
 * Draw the header bar.
 * --------------------------------------------------------------------- */
static void draw_header(u32 *buf, u32 bw, u32 bh, u32 spx,
                        u64 uptime_ms, int frame)
{
    /* Gradient background */
    fill_grad_v(buf, bw, bh, spx, 0, 0, (i32)bw, HDR_H, C_HDR_TOP, C_HDR_BOT);

    /* Accent stripe at top edge */
    /* Animated rainbow-ish accent: shift based on frame */
    for (i32 px = 0; px < (i32)bw; px++) {
        /* hue cycle along x + time */
        int hue = ((px * 360 / (i32)bw) + frame * 2) % 360;
        /* Simple hue-to-rgb (6 sectors) */
        int sector = hue / 60;
        int frac   = (hue % 60) * 255 / 60;
        u32 r, g, b;
        switch (sector) {
            case 0: r=255;      g=(u32)frac; b=0;         break;
            case 1: r=255-frac; g=255;       b=0;         break;
            case 2: r=0;        g=255;       b=(u32)frac; break;
            case 3: r=0;        g=255-frac;  b=255;       break;
            case 4: r=(u32)frac;g=0;         b=255;       break;
            default:r=255;      g=0;         b=255-frac;  break;
        }
        u32 col = 0xFF000000u | (r << 16) | (g << 8) | b;
        /* Draw 2-pixel-tall accent at top */
        if (px < (i32)bw && 0 < (i32)bh)
            buf[0 * spx + (u32)px] = col;
        if (px < (i32)bw && 1 < (i32)bh)
            buf[1 * spx + (u32)px] = col;
    }

    /* "AutomationOS" label (big, left-aligned) */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     12, (HDR_H - FONT_H) / 2,
                     "AutomationOS", C_ACCENT);

    /* Uptime HH:MM:SS label (right side) */
    char ubuf[32];
    char *p = ubuf;
    p = append(p, "Up ");
    char hms[12];
    fmt_hms(hms, uptime_ms);
    append(p, hms);

    int uw = font_text_width(ubuf);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     (int)bw - uw - 12, (HDR_H - FONT_H) / 2,
                     ubuf, C_TEXT2);

    /* Separator line at bottom of header */
    fill_rect(buf, bw, bh, spx, 0, HDR_H - 1, (i32)bw, 1, C_ACCENT);
}

/* -----------------------------------------------------------------------
 * Draw the gauge / sparkline panel.
 * --------------------------------------------------------------------- */
static void draw_gauges(u32 *buf, u32 bw, u32 bh, u32 spx,
                        int proc_count, int frame)
{
    i32 px = 8, py = GAUGE_Y, pw = (i32)bw - 16, ph = GAUGE_H;

    /* Panel background */
    fill_rrect(buf, bw, bh, spx, px, py, pw, ph, 8, C_PANEL);
    border_rrect(buf, bw, bh, spx, px, py, pw, ph, C_BORDER);

    /* --- Processes gauge (left half) --- */
    i32 gx = px + 10, gy = py + 10;

    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     gx, gy, "PROCESSES", C_TEXT3);

    char nbuf[16];
    fmt_u64((u64)proc_count, nbuf);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     gx, gy + FONT_H + 2, nbuf, C_ACCENT);

    /* Bar (processes / 32 = full) */
    i32 bar_w = pw / 2 - 30;
    u32 frac = (u32)proc_count * 256u / 32u;
    if (frac > 255) frac = 255;
    u32 bar_col = (proc_count > 24) ? C_BAR_HOT : C_BAR_FG;
    draw_bar(buf, bw, bh, spx,
             gx, gy + FONT_H * 2 + 6, bar_w, 10,
             frac, C_BAR_BG, bar_col);

    /* Fraction label e.g. "8 / 32" */
    char frbuf[24];
    char *fp = frbuf;
    fp = append(fp, nbuf); fp = append(fp, " / 32");
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     gx, gy + FONT_H * 2 + 20, frbuf, C_TEXT3);

    /* --- Sparkline / activity graph (right half) --- */
    i32 sx = px + pw / 2 + 4;
    i32 sy = py + 10;
    i32 sw = pw / 2 - 14;
    i32 sh = ph - 20;

    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     sx, sy, "ACTIVITY", C_TEXT3);

    /* Draw sparkline background */
    fill_rrect(buf, bw, bh, spx, sx, sy + FONT_H + 2, sw, sh - FONT_H - 2, 4, C_BAR_BG);

    draw_sparkline(buf, bw, bh, spx,
                   sx, sy + FONT_H + 2, sw, sh - FONT_H - 2,
                   g_spark, SPARK_LEN, g_spark_head, g_spark_max,
                   C_SPARK_FILL, C_SPARK_FG);

    /* Subtle frame counter label */
    char fbuf[12];
    fmt_u64((u64)frame, fbuf);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     sx + sw - (int)k_strlen(fbuf) * FONT_W - 2,
                     sy + FONT_H + 2,
                     fbuf, C_TEXT3);

    (void)frame; /* suppress unused-warning */
}

/* -----------------------------------------------------------------------
 * Draw the process list panel.
 * --------------------------------------------------------------------- */
static void draw_proclist(u32 *buf, u32 bw, u32 bh, u32 spx)
{
    i32 px = 8, py = PROC_Y, pw = (i32)bw - 16, ph = PROC_H;

    /* Panel background */
    fill_rrect(buf, bw, bh, spx, px, py, pw, ph, 8, C_PANEL);
    border_rrect(buf, bw, bh, spx, px, py, pw, ph, C_BORDER);

    /* Header row */
    i32 hx = px + 8, hy = py + 8;
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     hx,       hy, "PID",   C_ACCENT2);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     hx + 40,  hy, "NAME",  C_ACCENT2);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     hx + 200, hy, "STATE", C_ACCENT2);

    /* Separator */
    fill_rect(buf, bw, bh, spx, px + 8, py + FONT_H + 12, pw - 16, 1, C_SEP);

    /* Process rows */
    int show = g_proc_count < SHOW_PROCS ? g_proc_count : SHOW_PROCS;
    i32 row_h = FONT_H + 6;
    i32 row_y0 = py + FONT_H + 16;

    for (int i = 0; i < show; i++) {
        procinfo_t *pi = &g_procs[i];
        i32 ry = row_y0 + i * row_h;

        /* Alternate row background */
        u32 row_bg = (pi->state == 0) ? C_ROW_RUN : ((i & 1) ? C_ROW_B : C_ROW_A);
        fill_rrect(buf, bw, bh, spx,
                   px + 4, ry - 2, pw - 8, row_h, 4, row_bg);

        /* PID */
        char pidbuf[12];
        fmt_u64((u64)pi->pid, pidbuf);
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         hx, ry, pidbuf, C_TEXT2);

        /* Name (up to 20 chars) */
        char namebuf[24];
        strncpy_safe(namebuf, pi->name, 21);
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         hx + 40, ry, namebuf, C_TEXT);

        /* State */
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         hx + 200, ry,
                         state_str(pi->state),
                         state_color(pi->state));
    }

    /* If no data available */
    if (g_proc_count <= 0) {
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         hx, row_y0, "(no process data)", C_TEXT3);
    }

    /* Footer: proc count summary */
    char sumbuf[40];
    char *sp = sumbuf;
    char nb[12];
    fmt_u64((u64)g_proc_count, nb);
    sp = append(sp, nb);
    sp = append(sp, " total processes");
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     hx, py + ph - FONT_H - 6, sumbuf, C_TEXT3);
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial("[DASH] starting\n");

    /* Connect to the compositor. */
    if (wl_connect() != 0) {
        serial("[DASH] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Dashboard");
    if (!win) {
        serial("[DASH] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    u32 spx = win->stride / 4u;   /* stride in PIXELS */

    /* Clear to background colour. */
    fill_rect(win->pixels, win->w, win->h, spx,
              0, 0, (i32)win->w, (i32)win->h, C_BG);

    /* ---- State ---- */
    int frame           = 0;
    int proc_refresh    = 0;   /* countdown to next SYS_PROCLIST call */
    u64 prev_ticks      = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0);

    /* ---- Frame loop ---- */
    for (;;) {
        /* Drain input events (mouse/keyboard -- not used, but drain anyway).
         * On WL_EVENT_RESIZE the library has ALREADY reallocated the buffer and
         * updated win->{w,h,stride,pixels}; we re-read win->w/h/pixels fresh
         * every frame below, so the only cached value to refresh is the stride
         * (spx). We recompute it before each render, which covers resize. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) { /* discard */ }

        /* Refresh stride-in-pixels each frame so a resize that changes the
         * buffer pitch cannot push writes out of bounds. */
        spx = win->stride / 4u;

        /* --- Data acquisition --- */

        /* Uptime */
        u64 ticks = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0);

        /* Sparkline: push frame-to-frame tick delta (ms since last frame).
         * Normalise to ~0-100 range (a 33ms frame = 100%). */
        u64 dt = ticks > prev_ticks ? ticks - prev_ticks : 0u;
        prev_ticks = ticks;
        unsigned int spark_val = (unsigned int)(dt > 200u ? 200u : dt);
        g_spark[g_spark_head] = spark_val;
        g_spark_head = (g_spark_head + 1) % SPARK_LEN;
        /* Update rolling max (decay slowly) */
        if (spark_val > g_spark_max) g_spark_max = spark_val;
        else if ((frame & 63) == 0 && g_spark_max > 1) g_spark_max--;

        /* Process list refresh every ~30 frames */
        if (proc_refresh <= 0) {
            long cnt = sc(SYS_PROCLIST, (long)g_procs,
                          (long)MAX_PROCS, 0);
            if (cnt >= 0) {
                g_proc_count = (int)cnt;
                if (g_proc_count > MAX_PROCS) g_proc_count = MAX_PROCS;
            } else {
                g_proc_count = 0;
            }
            proc_refresh = 30;

            /* Periodic serial log */
            g_serial_frame += 30;
            if (g_serial_frame >= SERIAL_INTERVAL) {
                g_serial_frame = 0;
                char logbuf[64];
                char *lp = logbuf;
                char nb[12];
                lp = append(lp, "[DASH] procs=");
                fmt_u64((u64)g_proc_count, nb);
                lp = append(lp, nb);
                lp = append(lp, " uptime=");
                fmt_u64(ticks / 1000ULL, nb);
                lp = append(lp, nb);
                lp = append(lp, "s\n");
                serial(logbuf);
            }
        } else {
            proc_refresh--;
        }

        /* --- Render --- */
        fill_rect(win->pixels, win->w, win->h, spx,
                  0, 0, (i32)win->w, (i32)win->h, C_BG);

        draw_header(win->pixels, win->w, win->h, spx, ticks, frame);
        draw_gauges(win->pixels, win->w, win->h, spx, g_proc_count, frame);
        draw_proclist(win->pixels, win->w, win->h, spx);

        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0);

        frame++;
    }
}
