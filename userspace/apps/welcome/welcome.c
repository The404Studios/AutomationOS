/*
 * welcome.c -- Welcome / onboarding screen for AutomationOS (freestanding, ring 3)
 * =================================================================================
 *
 * Window: 600x460, title "Welcome to AutomationOS".
 *
 * Layout (dark theme, rounded-rect cards, accent animation):
 *   - Hero header: large 2x-scaled title "Welcome to AutomationOS" drawn by
 *     blitting each 8x16 glyph into a 16x32 block. Subtitle at normal scale.
 *   - Animated shimmer line under the title using SYS_GET_TICKS_MS.
 *   - "Keyboard Shortcuts" card.
 *   - "Apps" card with app catalog.
 *
 * Build (ALL flags DIRECTLY on the command line – no shell vars):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/welcome/welcome.c -o /tmp/wel.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/wel.o /tmp/wlc.o /tmp/bf.o -o /tmp/wel.elf
 *   objdump -d /tmp/wel.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [WELCOME] starting
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -------------------------------------------------------------------------
 * Syscall numbers and inline helpers.
 * ----------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_SPAWN        16

/* 3-arg inline syscall (required form per task spec). */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* 6-arg variant for wl_client compatibility (yield / write). */
static inline long sc6(long n, long a1, long a2, long a3,
                        long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* -------------------------------------------------------------------------
 * Minimal freestanding helpers.
 * ----------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned long long u64;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc6(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

/* -------------------------------------------------------------------------
 * Window / layout constants.
 * ----------------------------------------------------------------------- */
#define WIN_W   600
#define WIN_H   460

/* Current surface size in pixels. Initialized to the creation size and
 * refreshed from win->w/win->h every frame (and on WL_EVENT_RESIZE) so all
 * clamps and layout use the LIVE buffer dimensions. A resize (Maximize/snap)
 * changes win->{w,h,stride,pixels}; every pixel write below is bounded to
 * these so a smaller window cannot overflow and a larger one has no stale
 * margins. */
static i32 g_w = WIN_W;
static i32 g_h = WIN_H;

/* -------------------------------------------------------------------------
 * Color palette.
 * ----------------------------------------------------------------------- */
/* Background: very dark navy */
#define COL_BG          0xFF0F1117u
/* Card background: slightly lighter */
#define COL_CARD        0xFF1A1D27u
/* Card border */
#define COL_CARD_BORDER 0xFF2E3348u
/* Header region bg */
#define COL_HEADER_BG   0xFF13161Fu
/* Title text: near white */
#define COL_TITLE       0xFFF0F4FFu
/* Subtitle: muted */
#define COL_SUBTITLE    0xFF8899BBu
/* Section heading: accent */
#define COL_HEADING     0xFF5C9BFFu
/* Body text */
#define COL_BODY        0xFFCDD5E8u
/* Shortcut key labels: slightly highlighted */
#define COL_KEY         0xFFFFCC66u
/* App name accent */
#define COL_APP         0xFF79E6A2u
/* Shimmer accent base */
#define COL_ACCENT      0xFF4477FFu
/* Separator line */
#define COL_SEP         0xFF252A3Eu

/* -------------------------------------------------------------------------
 * Drawing primitives (ARGB32, stride in pixels).
 * ----------------------------------------------------------------------- */

static void fill_rect(u32 *buf, i32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > g_w) x2 = g_w;
    i32 y2 = y + h; if (y2 > g_h) y2 = g_h;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = color;
    }
}

/* Rounded-rect helper: fills a rect with radius-r corners. */
static void fill_rrect(u32 *buf, i32 stride_px,
                       i32 x, i32 y, i32 w, i32 h, i32 r, u32 color)
{
    if (r < 1) { fill_rect(buf, stride_px, x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Center cross */
    fill_rect(buf, stride_px, x + r, y,     w - 2*r, h,     color);
    fill_rect(buf, stride_px, x,     y + r, r,        h - 2*r, color);
    fill_rect(buf, stride_px, x + w - r, y + r, r, h - 2*r, color);

    /* Four corner quarter-discs (radius r). */
    i32 r2 = r * r;
    for (i32 dy = 0; dy < r; dy++) {
        for (i32 dx = 0; dx < r; dx++) {
            if ((dy + 1)*(dy + 1) + (dx + 1)*(dx + 1) <= r2 + 2*r + 1) {
                /* top-left corner */
                i32 px, py;
                px = x + r - 1 - dx; py = y + r - 1 - dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
                /* top-right corner */
                px = x + w - r + dx; py = y + r - 1 - dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
                /* bottom-left corner */
                px = x + r - 1 - dx; py = y + h - r + dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
                /* bottom-right corner */
                px = x + w - r + dx; py = y + h - r + dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
            }
        }
    }
}

/* Rounded-rect border (outline only, 1px). */
static void draw_rrect_border(u32 *buf, i32 stride_px,
                              i32 x, i32 y, i32 w, i32 h, i32 r, u32 color)
{
    /* Top / bottom horizontal segments. */
    fill_rect(buf, stride_px, x + r, y,         w - 2*r, 1,     color);
    fill_rect(buf, stride_px, x + r, y + h - 1, w - 2*r, 1,     color);
    /* Left / right vertical segments. */
    fill_rect(buf, stride_px, x,         y + r, 1, h - 2*r, color);
    fill_rect(buf, stride_px, x + w - 1, y + r, 1, h - 2*r, color);

    /* Corner arcs (thin, 1-pixel outline). */
    i32 r_inner = r - 1;
    i32 r2_outer = r * r;
    i32 r2_inner = r_inner * r_inner;
    for (i32 dy = 0; dy < r; dy++) {
        for (i32 dx = 0; dx < r; dx++) {
            i32 d2 = (dy + 1)*(dy + 1) + (dx + 1)*(dx + 1) - 2*(dy + dx + 1);
            /* pixel is on the arc if it's within the outer disc and outside the inner disc */
            i32 outer = ((r - dx)*(r - dx) + (r - dy)*(r - dy));
            i32 inner = ((r - 1 - dx)*(r - 1 - dx) + (r - 1 - dy)*(r - 1 - dy));
            (void)d2; (void)r2_outer; (void)r2_inner;
            if (outer <= r2_outer + 1 && inner > r_inner * r_inner) {
                i32 px, py;
                px = x + dx;           py = y + dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
                px = x + w - 1 - dx;  py = y + dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
                px = x + dx;           py = y + h - 1 - dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
                px = x + w - 1 - dx;  py = y + h - 1 - dy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    buf[py * stride_px + px] = color;
            }
        }
    }
}

/* Horizontal line helper. */
static void hline(u32 *buf, i32 stride_px,
                  i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, stride_px, x, y, len, 1, color);
}

/* -------------------------------------------------------------------------
 * 2x-scaled glyph drawing.
 * Each 8x16 glyph is rendered into a 16x32 block by writing each source
 * pixel as a 2x2 block of destination pixels.
 * ----------------------------------------------------------------------- */
static void font_draw_char_2x(u32 *buf, i32 stride_px,
                               i32 x, i32 y, char c, u32 color)
{
    /* We need to access the raw glyph bits.  bitfont exposes font_draw_char
     * which writes pixels — instead, we use it row-by-row to a tiny scratch
     * buffer and then scale up.  The scratch is 8 pixels wide, 16 high.     */

    /* Trick: draw the glyph into a tiny 8x16 scratch at (0,0), then for
     * each lit pixel in the scratch write a 2x2 block at the destination.   */
    u32 scratch[8 * 16];  /* 128 u32 on the stack — fine with mno-red-zone */

    /* Clear scratch to transparent. */
    for (i32 i = 0; i < 8 * 16; i++) scratch[i] = 0x00000000u;

    /* Draw the glyph into scratch (stride = 8 pixels). */
    font_draw_char(scratch, 8, 8, 16, 0, 0, c, 0xFFFFFFFFu);

    /* Now scale up 2x into dest buffer. */
    for (i32 row = 0; row < 16; row++) {
        for (i32 col = 0; col < 8; col++) {
            if (scratch[row * 8 + col] == 0xFFFFFFFFu) {
                /* Write 2x2 block. */
                i32 dx = x + col * 2;
                i32 dy = y + row * 2;
                fill_rect(buf, stride_px, dx, dy, 2, 2, color);
            }
        }
    }
}

/* Draw a 2x-scaled string; returns total x advance in pixels (16 per char). */
static i32 font_draw_string_2x(u32 *buf, i32 stride_px,
                                i32 x, i32 y, const char *s, u32 color)
{
    i32 cx = x;
    while (*s) {
        font_draw_char_2x(buf, stride_px, cx, y, *s, color);
        cx += 16;   /* FONT_W * 2 */
        s++;
    }
    return cx - x;
}

/* Return pixel width of a 2x-scaled string. */
static i32 font_text_width_2x(const char *s)
{
    return font_text_width(s) * 2;
}

/* -------------------------------------------------------------------------
 * Linear interpolation of two ARGB colors (t in 0..255).
 * ----------------------------------------------------------------------- */
static u32 lerp_color(u32 a, u32 b, i32 t)
{
    if (t <= 0)   return a;
    if (t >= 255) return b;
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, abl = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bbl = b & 0xFF;
    u32 rr = ar + (u32)((i32)(br - ar) * t / 255);
    u32 rg = ag + (u32)((i32)(bg - ag) * t / 255);
    u32 rb = abl + (u32)((i32)(bbl - abl) * t / 255);
    return 0xFF000000u | (rr << 16) | (rg << 8) | rb;
}

/* -------------------------------------------------------------------------
 * Animated shimmer: a moving highlight bar that sweeps across the window
 * under the hero title.  Position is derived from SYS_GET_TICKS_MS.
 *
 * The shimmer is a translucent horizontal band ~30px tall that moves
 * across the full width over a period of ~2000ms.
 * We approximate alpha-blend by mixing with the bg color.
 * ----------------------------------------------------------------------- */
#define SHIMMER_PERIOD_MS  2000
#define SHIMMER_W          120    /* width of the shimmer highlight */
#define SHIMMER_H          4      /* height of the shimmer line */

static void draw_shimmer(u32 *buf, i32 stride_px, u64 ms)
{
    /* Shimmer lives under the title area.  Y coordinate is fixed. */
    i32 sy = 112; /* just under the 2x title */

    /* Phase: 0..g_w over SHIMMER_PERIOD_MS, then wraps. */
    i32 phase = (i32)((ms % (u64)SHIMMER_PERIOD_MS) *
                      (u64)(g_w + SHIMMER_W) / (u64)SHIMMER_PERIOD_MS)
                - SHIMMER_W;

    /* Draw the shimmer gradient: bright in the center, fading at edges. */
    for (i32 dx = 0; dx < SHIMMER_W; dx++) {
        i32 px = phase + dx;
        if (px < 0 || px >= g_w) continue;

        /* Brightness: 0 at edges, 255 at center */
        i32 dist_center = dx - SHIMMER_W / 2;
        if (dist_center < 0) dist_center = -dist_center;
        i32 bright = 255 - (dist_center * 255) / (SHIMMER_W / 2 + 1);
        if (bright < 0) bright = 0;

        /* Color blends from COL_ACCENT to white at the peak. */
        u32 shimmer_col = lerp_color(COL_ACCENT, 0xFF99BBFF, bright);

        for (i32 dy = 0; dy < SHIMMER_H; dy++) {
            i32 py = sy + dy;
            if (py < 0 || py >= g_h) continue;
            /* Mix 50% shimmer over existing pixel. */
            u32 existing = buf[py * stride_px + px];
            u32 er = (existing >> 16) & 0xFF;
            u32 eg = (existing >> 8)  & 0xFF;
            u32 eb =  existing        & 0xFF;
            u32 sr = (shimmer_col >> 16) & 0xFF;
            u32 sg = (shimmer_col >> 8)  & 0xFF;
            u32 sb =  shimmer_col        & 0xFF;
            u32 mr = (er + sr) >> 1;
            u32 mg = (eg + sg) >> 1;
            u32 mb = (eb + sb) >> 1;
            buf[py * stride_px + px] = 0xFF000000u | (mr << 16) | (mg << 8) | mb;
        }
    }
}

/* -------------------------------------------------------------------------
 * Draw a section heading label with a small colored bar on the left.
 * ----------------------------------------------------------------------- */
static void draw_section_heading(u32 *buf, i32 stride_px,
                                  i32 x, i32 y, const char *label, u32 accent)
{
    /* Colored bar: 3px wide, full font height + 2. */
    fill_rect(buf, stride_px, x, y - 1, 3, FONT_H + 2, accent);
    font_draw_string(buf, stride_px, g_w, g_h,
                     x + 7, y, label, accent);
}

/* -------------------------------------------------------------------------
 * Main full-frame render.
 * ----------------------------------------------------------------------- */
static void draw_frame(u32 *buf, i32 stride_px, u64 ms)
{
    /* --- Background fill (clears the FULL current surface; no stale margins
     * after a resize) --- */
    fill_rect(buf, stride_px, 0, 0, g_w, g_h, COL_BG);

    /* ================================================================
     * HERO HEADER (y=0..135)
     * ============================================================== */
    i32 header_h = 136;
    fill_rrect(buf, stride_px, 0, 0, g_w, header_h, 0, COL_HEADER_BG);

    /* Title: "Welcome to AutomationOS" rendered 2x (16x32 per glyph).
     * At 2x scale, each char is 16px wide, 32px tall.
     * String width = font_text_width_2x("Welcome to AutomationOS") */
    const char *title = "Welcome to AutomationOS";
    i32 title_w = font_text_width_2x(title);
    i32 title_x = (g_w - title_w) / 2;
    i32 title_y = 28;
    font_draw_string_2x(buf, stride_px, title_x, title_y, title, COL_TITLE);

    /* Animated shimmer under title (before subtitle, so subtitle draws on top). */
    draw_shimmer(buf, stride_px, ms);

    /* Subtitle at normal 1x scale. */
    const char *subtitle = "A desktop built from scratch";
    i32 sub_w = font_text_width(subtitle);
    i32 sub_x = (g_w - sub_w) / 2;
    i32 sub_y = title_y + 32 + 12;  /* 32px glyph height + 12px gap */
    font_draw_string(buf, stride_px, g_w, g_h,
                     sub_x, sub_y, subtitle, COL_SUBTITLE);

    /* Thin separator below header. */
    hline(buf, stride_px, 16, header_h, g_w - 32, COL_SEP);

    /* ================================================================
     * KEYBOARD SHORTCUTS CARD (left column)
     * ============================================================== */
    i32 pad  = 12;
    i32 col1_x = pad;
    i32 col1_w = 270;
    i32 cards_y = header_h + 10;
    i32 card_h  = g_h - cards_y - pad;

    fill_rrect(buf, stride_px,
               col1_x, cards_y, col1_w, card_h, 8, COL_CARD);
    draw_rrect_border(buf, stride_px,
                      col1_x, cards_y, col1_w, card_h, 8, COL_CARD_BORDER);

    /* Heading */
    draw_section_heading(buf, stride_px,
                         col1_x + 14, cards_y + 12,
                         "Keyboard Shortcuts", COL_HEADING);

    /* Shortcut rows */
    struct { const char *key; const char *desc; } shortcuts[] = {
        { "Alt+Tab",       "Switch windows"         },
        { "Alt+Q",         "Close window"           },
        { "Alt+F4",        "Close window"           },
        { "Alt+M",         "Minimize window"        },
        { "Alt+K",         "Force quit app"         },
        { "Drag to edge",  "Snap window"            },
    };
    i32 sc_count = 6;
    i32 sc_y = cards_y + 12 + FONT_H + 10;
    for (i32 i = 0; i < sc_count; i++) {
        i32 row_y = sc_y + i * (FONT_H + 7);

        /* Key label background pill */
        i32 kw = font_text_width(shortcuts[i].key) + 8;
        fill_rrect(buf, stride_px,
                   col1_x + 14, row_y - 2, kw, FONT_H + 4, 3,
                   0xFF1E2438u);
        font_draw_string(buf, stride_px, g_w, g_h,
                         col1_x + 18, row_y, shortcuts[i].key, COL_KEY);

        /* Separator dash */
        font_draw_string(buf, stride_px, g_w, g_h,
                         col1_x + 14 + kw + 4, row_y, "->", COL_CARD_BORDER);

        /* Description */
        font_draw_string(buf, stride_px, g_w, g_h,
                         col1_x + 14 + kw + 26, row_y, shortcuts[i].desc, COL_BODY);
    }

    /* ================================================================
     * APPS CARD (right column)
     * ============================================================== */
    i32 col2_x = col1_x + col1_w + pad;
    i32 col2_w = g_w - col2_x - pad;

    fill_rrect(buf, stride_px,
               col2_x, cards_y, col2_w, card_h, 8, COL_CARD);
    draw_rrect_border(buf, stride_px,
                      col2_x, cards_y, col2_w, card_h, 8, COL_CARD_BORDER);

    /* Heading */
    draw_section_heading(buf, stride_px,
                         col2_x + 14, cards_y + 12,
                         "Applications", COL_APP);

    /* App catalog: two-column list to fit in the card. */
    const char *apps_col_a[] = {
        "Terminal", "Files", "Calculator",
        "Paint", "Snake", "Tetris",
    };
    const char *apps_col_b[] = {
        "2048", "Notes", "Sheet",
        "Task Mgr", "Calendar", "Dashboard",
    };
    i32 app_count = 6;
    i32 app_y  = cards_y + 12 + FONT_H + 10;
    i32 dot_x  = col2_x + 14;
    i32 name_x = dot_x + 12;
    i32 col_b_offset = col2_w / 2 - 4;

    for (i32 i = 0; i < app_count; i++) {
        i32 row_y = app_y + i * (FONT_H + 6);

        /* Column A */
        fill_rect(buf, stride_px, dot_x, row_y + 5, 5, 5, COL_APP);
        font_draw_string(buf, stride_px, g_w, g_h,
                         name_x, row_y, apps_col_a[i], COL_BODY);

        /* Column B */
        fill_rect(buf, stride_px,
                  dot_x + col_b_offset, row_y + 5, 5, 5, COL_APP);
        font_draw_string(buf, stride_px, g_w, g_h,
                         name_x + col_b_offset, row_y, apps_col_b[i], COL_BODY);
    }

    /* Hint at bottom of apps card. */
    i32 hint_y = cards_y + card_h - FONT_H - 12;
    /* Thin separator above hint. */
    hline(buf, stride_px,
          col2_x + 8, hint_y - 6, col2_w - 16, COL_SEP);
    font_draw_string(buf, stride_px, g_w, g_h,
                     col2_x + 14, hint_y,
                     "Use the App Launcher to start any app",
                     COL_SUBTITLE);
}

/* -------------------------------------------------------------------------
 * Entry point.
 * ----------------------------------------------------------------------- */
void _start(void)
{
    print("[WELCOME] starting\n");

    if (wl_connect() != 0) {
        print("[WELCOME] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Welcome to AutomationOS");
    if (!win) {
        print("[WELCOME] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    print("[WELCOME] window created\n");

    /* Live geometry. Initialized from the created window and refreshed every
     * frame (and on WL_EVENT_RESIZE) from win->{w,h,stride}, which the library
     * updates -- buffer included -- before the resize event surfaces. */
    g_w = (i32)win->w;
    g_h = (i32)win->h;
    i32 stride_px = (i32)(win->stride / 4u);

    /* Frame loop. */
    for (;;) {
        /* Drain events. We only act on resize; flush the rest. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_RESIZE) {
                /* The library already reallocated the buffer and updated
                 * win->{w,h,stride,pixels}. Re-cache geometry so this frame's
                 * clamps and layout match the new surface exactly. */
                g_w = (i32)win->w;
                g_h = (i32)win->h;
                stride_px = (i32)(win->stride / 4u);
            }
        }

        /* Defensive re-read: stay in sync with the live window even if a
         * resize arrived without a discrete event. */
        g_w = (i32)win->w;
        g_h = (i32)win->h;
        stride_px = (i32)(win->stride / 4u);

        /* Get current time for the shimmer animation. */
        u64 ms = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0);

        /* Render the full frame into the live buffer. */
        draw_frame(win->pixels, stride_px, ms);

        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0);
    }
}
