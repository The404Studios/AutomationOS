/*
 * paint.c -- Simple paint application (freestanding, ring 3).
 * ============================================================
 *
 * Opens a 640x480 window titled "Paint". A toolbar at the top
 * provides 8 color swatches and a "Clear" button. The rest of
 * the window is a white canvas where holding the left mouse
 * button draws filled brush dots; motion between samples is
 * interpolated so strokes are continuous.
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/paint/paint.c -o /tmp/paint.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/paint.o /tmp/wlc.o /tmp/bf.o -o /tmp/paint.elf
 *   objdump -d /tmp/paint.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [PAINT] starting
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

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

/* -----------------------------------------------------------------------
 * Minimal freestanding helpers.
 * --------------------------------------------------------------------- */
typedef unsigned int  u32;
typedef int           i32;

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

/* abs() for integers without libm */
static i32 iabs(i32 v) { return v < 0 ? -v : v; }

/* -----------------------------------------------------------------------
 * Window / layout constants.
 * --------------------------------------------------------------------- */
#define WIN_W        640
#define WIN_H        480

/* Toolbar occupies the top TOOLBAR_H rows. */
#define TOOLBAR_H    40

/* Canvas starts below the toolbar. */
#define CANVAS_Y     TOOLBAR_H

/* Brush radius in pixels (filled circle). */
#define BRUSH_R      4

/* -----------------------------------------------------------------------
 * Color palette (8 swatches).
 * --------------------------------------------------------------------- */
#define NUM_COLORS   8

static const u32 palette[NUM_COLORS] = {
    0xFF000000u,   /* 0 black   */
    0xFFFFFFFFu,   /* 1 white   */
    0xFFFF0000u,   /* 2 red     */
    0xFF00CC00u,   /* 3 green   */
    0xFF0066FFu,   /* 4 blue    */
    0xFFFFCC00u,   /* 5 yellow  */
    0xFFFF6600u,   /* 6 orange  */
    0xFFCC00CCu,   /* 7 magenta */
};

/* Label text for each swatch (drawn on top). */
static const char * const palette_label[NUM_COLORS] = {
    " ", " ", " ", " ", " ", " ", " ", " ",
};

/* Swatch geometry: each swatch is SWATCH_W x SWATCH_H starting at SWATCH_X0.
 * A 1-pixel gap between swatches.                                        */
#define SWATCH_W     36
#define SWATCH_H     30
#define SWATCH_Y     5
#define SWATCH_X0    4
#define SWATCH_GAP   2

/* "Clear" button: placed to the right of the swatches. */
#define CLEAR_W      56
#define CLEAR_H      30
#define CLEAR_Y      5
#define CLEAR_X      (SWATCH_X0 + NUM_COLORS * (SWATCH_W + SWATCH_GAP) + 6)

/* Toolbar background color. */
#define TOOLBAR_BG   0xFF2D2D2Du
/* Toolbar separator line color. */
#define TOOLBAR_SEP  0xFF555555u
/* Selected swatch highlight border. */
#define SEL_BORDER   0xFFFFFFFFu
/* Button face. */
#define BTN_FACE     0xFF4A4A4Au
/* Button text. */
#define BTN_TEXT     0xFFEEEEEEu
/* Canvas background (white). */
#define CANVAS_BG    0xFFFFFFFFu

/* -----------------------------------------------------------------------
 * Drawing primitives into ARGB32 pixel buffer.
 * stride_px == stride / 4  (pixels per row).
 * --------------------------------------------------------------------- */

static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = color;
    }
}

/* Draw a horizontal 1-pixel-tall line (clipped). */
static void hline(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                  i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, bw, bh, stride_px, x, y, len, 1, color);
}

/* Draw a 1-pixel border rectangle (not filled). */
static void draw_border(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                        i32 x, i32 y, i32 w, i32 h, u32 color)
{
    /* top */
    hline(buf, bw, bh, stride_px, x, y, w, color);
    /* bottom */
    hline(buf, bw, bh, stride_px, x, y + h - 1, w, color);
    /* left */
    fill_rect(buf, bw, bh, stride_px, x,         y, 1, h, color);
    /* right */
    fill_rect(buf, bw, bh, stride_px, x + w - 1, y, 1, h, color);
}

/* Draw a filled circle (brush dot) centered at (cx,cy) with radius r.
 * Uses a simple scan-line approach: no sqrt, just integer test.       */
static void fill_circle(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                        i32 cx, i32 cy, i32 r, u32 color)
{
    /* Only paint within the canvas area (y >= CANVAS_Y). */
    i32 y0 = cy - r; if (y0 < CANVAS_Y) y0 = CANVAS_Y;
    i32 y1 = cy + r; if (y1 >= (i32)bh)  y1 = (i32)bh - 1;
    i32 r2 = r * r;
    for (i32 yy = y0; yy <= y1; yy++) {
        i32 dy = yy - cy;
        /* dx satisfying dx^2 + dy^2 <= r^2 */
        i32 dx_max_sq = r2 - dy * dy;
        if (dx_max_sq < 0) continue;
        /* integer square root (good enough for r<=32) */
        i32 dx = r;
        while (dx * dx > dx_max_sq) dx--;
        i32 x0 = cx - dx; if (x0 < 0) x0 = 0;
        i32 x1 = cx + dx; if (x1 >= (i32)bw) x1 = (i32)bw - 1;
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x0; xx <= x1; xx++)
            row[xx] = color;
    }
}

/*
 * Draw a thick stroke from (x0,y0) to (x1,y1) by stepping along the
 * line and placing a brush dot every step.  Uses integer Bresenham.
 */
static void stroke_line(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                        i32 x0, i32 y0, i32 x1, i32 y1,
                        i32 r, u32 color)
{
    i32 dx = iabs(x1 - x0);
    i32 dy = iabs(y1 - y0);
    i32 steps = dx > dy ? dx : dy;
    if (steps == 0) {
        fill_circle(buf, bw, bh, stride_px, x0, y0, r, color);
        return;
    }
    for (i32 i = 0; i <= steps; i++) {
        i32 x = x0 + (x1 - x0) * i / steps;
        i32 y = y0 + (y1 - y0) * i / steps;
        fill_circle(buf, bw, bh, stride_px, x, y, r, color);
    }
}

/* -----------------------------------------------------------------------
 * Toolbar render.
 * --------------------------------------------------------------------- */
static void draw_toolbar(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                         int sel_color)
{
    /* Background bar. */
    fill_rect(buf, bw, bh, stride_px, 0, 0, (i32)bw, TOOLBAR_H, TOOLBAR_BG);

    /* Separator line at bottom of toolbar. */
    hline(buf, bw, bh, stride_px, 0, TOOLBAR_H - 1, (i32)bw, TOOLBAR_SEP);

    /* Color swatches. */
    for (int i = 0; i < NUM_COLORS; i++) {
        i32 sx = SWATCH_X0 + i * (SWATCH_W + SWATCH_GAP);
        fill_rect(buf, bw, bh, stride_px,
                  sx, SWATCH_Y, SWATCH_W, SWATCH_H, palette[i]);
        if (i == sel_color) {
            /* White selection border (2px). */
            draw_border(buf, bw, bh, stride_px,
                        sx - 1, SWATCH_Y - 1, SWATCH_W + 2, SWATCH_H + 2,
                        SEL_BORDER);
            draw_border(buf, bw, bh, stride_px,
                        sx - 2, SWATCH_Y - 2, SWATCH_W + 4, SWATCH_H + 4,
                        SEL_BORDER);
        }
    }

    /* "Clear" button. */
    fill_rect(buf, bw, bh, stride_px,
              CLEAR_X, CLEAR_Y, CLEAR_W, CLEAR_H, BTN_FACE);
    draw_border(buf, bw, bh, stride_px,
                CLEAR_X, CLEAR_Y, CLEAR_W, CLEAR_H, 0xFF888888u);
    /* Center the label "Clear" (5 chars * 8 = 40px wide, 16px tall).
     * Button center_x = CLEAR_X + CLEAR_W/2 = CLEAR_X + 28
     * Text x = center_x - 20 = CLEAR_X + 8
     * Text y = CLEAR_Y + (CLEAR_H - FONT_H) / 2 = CLEAR_Y + 7          */
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     CLEAR_X + 8, CLEAR_Y + 7, "Clear", BTN_TEXT);
}

/* -----------------------------------------------------------------------
 * Hit-test helpers.
 * --------------------------------------------------------------------- */
static int hit_swatch(i32 mx, i32 my)
{
    if (my < SWATCH_Y || my >= SWATCH_Y + SWATCH_H) return -1;
    for (int i = 0; i < NUM_COLORS; i++) {
        i32 sx = SWATCH_X0 + i * (SWATCH_W + SWATCH_GAP);
        if (mx >= sx && mx < sx + SWATCH_W) return i;
    }
    return -1;
}

static int hit_clear(i32 mx, i32 my)
{
    return (mx >= CLEAR_X && mx < CLEAR_X + CLEAR_W &&
            my >= CLEAR_Y && my < CLEAR_Y + CLEAR_H);
}

static int in_toolbar(i32 my) { return my < TOOLBAR_H; }

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    print("[PAINT] starting\n");

    if (wl_connect() != 0) {
        print("[PAINT] wl_connect FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Paint");
    if (!win) {
        print("[PAINT] wl_create_window FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[PAINT] window created\n");

    u32 stride_px = win->stride / 4u;

    /* ---- Initial canvas: fill white. ---- */
    fill_rect(win->pixels, win->w, win->h, stride_px,
              0, CANVAS_Y, (i32)win->w, (i32)win->h - CANVAS_Y, CANVAS_BG);

    /* ---- State. ---- */
    int sel_color  = 0;      /* index into palette[]     */
    int drawing    = 0;      /* left-button held          */
    i32 prev_x     = -1;
    i32 prev_y     = -1;

    /* ---- Frame loop. ---- */
    for (;;) {
        /* Drain all pending events before rendering. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea;
                i32 my = (i32)eb;
                int btn_left = (ec & 1);   /* bit 0 = left button */

                /* --- Button state transitions. --- */
                if (btn_left && !drawing) {
                    /* Button just pressed. */
                    drawing = 1;
                    if (in_toolbar(my)) {
                        /* Toolbar click: check swatches and clear button. */
                        int sw = hit_swatch(mx, my);
                        if (sw >= 0) {
                            sel_color = sw;
                            print("[PAINT] color selected\n");
                        } else if (hit_clear(mx, my)) {
                            fill_rect(win->pixels, win->w, win->h, stride_px,
                                      0, CANVAS_Y,
                                      (i32)win->w, (i32)win->h - CANVAS_Y,
                                      CANVAS_BG);
                            print("[PAINT] canvas cleared\n");
                        }
                        /* Don't start a drawing stroke in the toolbar. */
                        drawing = 0;
                        prev_x = -1;
                        prev_y = -1;
                    } else {
                        /* Canvas area: start stroke. */
                        prev_x = mx;
                        prev_y = my;
                        fill_circle(win->pixels, win->w, win->h, stride_px,
                                    mx, my, BRUSH_R, palette[sel_color]);
                    }
                } else if (!btn_left && drawing) {
                    /* Button released. */
                    drawing = 0;
                    prev_x  = -1;
                    prev_y  = -1;
                } else if (btn_left && drawing) {
                    /* Motion while held -- draw only in canvas. */
                    if (!in_toolbar(my) && prev_x >= 0) {
                        stroke_line(win->pixels, win->w, win->h, stride_px,
                                    prev_x, prev_y, mx, my,
                                    BRUSH_R, palette[sel_color]);
                    }
                    /* Update previous position (even if in toolbar, so
                     * re-entering canvas resumes correctly). */
                    prev_x = mx;
                    prev_y = my;
                }
            }
            /* Key events not used in paint; silently ignored. */
        }

        /* Redraw toolbar every frame (keeps selection highlight fresh). */
        draw_toolbar(win->pixels, win->w, win->h, stride_px, sel_color);

        wl_commit(win);

        sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
