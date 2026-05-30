/*
 * vpaint.c -- Vector drawing application (freestanding, ring 3).
 * ==============================================================
 *
 * A vector editor that maintains a list of shape objects (line, rectangle,
 * ellipse, freehand polyline, text label), each with position, stroke/fill
 * color, and stroke width.  The scene is re-rendered from the shape list
 * every frame so every edit is resolution-independent.
 *
 * Layout (840 x 560):
 *   Left sidebar  (0 .. SIDEBAR_W-1):  tool buttons
 *   Top palette   (SIDEBAR_W .. right, 0 .. PALETTE_H-1): color / stroke
 *   Status bar    (bottom STATUS_H rows): shape count + selected info
 *   Canvas        (remainder): white drawing area
 *
 * Tools:
 *   Select -- click shape to select, drag to move, drag corner handle to
 *             resize, Delete key to remove selected shape.
 *   Line   -- click-drag to draw a line.
 *   Rect   -- click-drag to draw a filled+stroked rectangle.
 *   Ellipse-- click-drag to draw a filled+stroked ellipse.
 *   Poly   -- click to add vertices, double-click to close/finish.
 *   Text   -- click to place a text label (fixed string "Text").
 *
 * Z-order: "Front" / "Back" buttons on the palette bar.
 * Undo:    circular history, Ctrl-Z (or U key).
 * Save:    writes /tmp/drawing.vec  (S key).
 *
 * Build (WSL Arch, NO stack protector, NO canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/vpaint/vpaint.c -o /tmp/vpaint.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/vpaint.o /tmp/wlc.o /tmp/bf.o -o /tmp/vpaint.elf
 *   objdump -d /tmp/vpaint.elf | grep fs:0x28   # must be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -------------------------------------------------------------------------
 * Syscall helpers (inline, no libc).
 * ----------------------------------------------------------------------- */
#define SYS_WRITE  3
#define SYS_YIELD  15
#define SYS_OPEN   2
#define SYS_CLOSE  6

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

static void sys_write_str(int fd, const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    sc6(SYS_WRITE, fd, (long)s, (long)n, 0, 0, 0);
}
static void print(const char *s) { sys_write_str(1, s); }

/* -------------------------------------------------------------------------
 * Types.
 * ----------------------------------------------------------------------- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned char  u8;

/* -------------------------------------------------------------------------
 * Window / layout constants.
 * ----------------------------------------------------------------------- */
#define WIN_W      840
#define WIN_H      560

#define SIDEBAR_W   80
#define PALETTE_H   44
#define STATUS_H    22

/* Canvas origin and size. */
#define CANVAS_X    SIDEBAR_W
#define CANVAS_Y    PALETTE_H
#define CANVAS_W    (WIN_W - SIDEBAR_W)
#define CANVAS_H    (WIN_H - PALETTE_H - STATUS_H)

/* -------------------------------------------------------------------------
 * Colors.
 * ----------------------------------------------------------------------- */
#define C_BG_DARK    0xFF1E1E2Eu
#define C_BG_MID     0xFF2A2A3Au
#define C_BG_LIGHT   0xFF3A3A4Au
#define C_ACCENT     0xFF5E81ACu
#define C_ACCENT2    0xFF88C0D0u
#define C_SEP        0xFF4A4A5Au
#define C_WHITE      0xFFFFFFFFu
#define C_BLACK      0xFF000000u
#define C_CANVAS     0xFFFFFFFFu
#define C_STATUS_BG  0xFF14141Eu
#define C_TEXT_DIM   0xFFAAAAAAu
#define C_SEL_HANDLE 0xFF5E81ACu
#define C_HOVER      0xFFBBDDFFu
#define C_SEL_BOX    0xFF5E81ACu

/* -------------------------------------------------------------------------
 * Palette (stroke / fill colors).
 * ----------------------------------------------------------------------- */
#define NUM_PAL  16

static const u32 palette_colors[NUM_PAL] = {
    0xFF000000u, 0xFF3B4252u, 0xFF4C566Au, 0xFFD8DEE9u,
    0xFFECEFF4u, 0xFFFFFFFFu, 0xFFBF616Au, 0xFFD08770u,
    0xFFEBCB8Bu, 0xFFA3BE8Cu, 0xFF88C0D0u, 0xFF81A1C1u,
    0xFF5E81ACu, 0xFFB48EADu, 0xFF8FBCBBu, 0x00000000u, /* last = transparent */
};

/* -------------------------------------------------------------------------
 * Shape types.
 * ----------------------------------------------------------------------- */
#define SH_LINE    0
#define SH_RECT    1
#define SH_ELLIPSE 2
#define SH_POLY    3
#define SH_TEXT    4

#define MAX_SHAPES   256
#define MAX_POLY_PTS  64

typedef struct {
    i32 type;       /* SH_* */
    /* Bounding / defining geometry */
    i32 x0, y0;    /* origin / start  (canvas coords) */
    i32 x1, y1;    /* end / corner-2  (canvas coords) */
    /* Poly points (used when type == SH_POLY) */
    i32 pts[MAX_POLY_PTS][2];
    i32 npts;
    /* Style */
    u32 stroke_col;
    u32 fill_col;   /* 0x00000000 = no fill */
    i32 stroke_w;   /* 1-8 */
    /* Text label (for SH_TEXT) -- fixed length, NUL terminated */
    char text[32];
    /* Alive flag */
    i32 alive;
} Shape;

static Shape shapes[MAX_SHAPES];
static i32   nshapes = 0;

/* -------------------------------------------------------------------------
 * Undo history.
 * ----------------------------------------------------------------------- */
#define UNDO_DEPTH 32

typedef struct {
    Shape  shapes[MAX_SHAPES];
    i32    nshapes;
} HistEntry;

static HistEntry history[UNDO_DEPTH];
static i32 hist_head = 0;   /* next write slot */
static i32 hist_count = 0;

static void history_push(void)
{
    HistEntry *e = &history[hist_head % UNDO_DEPTH];
    /* memcpy equivalent */
    for (i32 i = 0; i < MAX_SHAPES; i++) e->shapes[i] = shapes[i];
    e->nshapes = nshapes;
    hist_head++;
    if (hist_count < UNDO_DEPTH) hist_count++;
}

static void history_pop(void)
{
    if (hist_count == 0) return;
    hist_count--;
    hist_head--;
    if (hist_head < 0) hist_head += UNDO_DEPTH;
    HistEntry *e = &history[hist_head % UNDO_DEPTH];
    for (i32 i = 0; i < MAX_SHAPES; i++) shapes[i] = e->shapes[i];
    nshapes = e->nshapes;
}

/* -------------------------------------------------------------------------
 * Tool state.
 * ----------------------------------------------------------------------- */
#define TOOL_SELECT  0
#define TOOL_LINE    1
#define TOOL_RECT    2
#define TOOL_ELLIPSE 3
#define TOOL_POLY    4
#define TOOL_TEXT    5
#define NUM_TOOLS    6

static i32 cur_tool      = TOOL_SELECT;
static i32 sel_shape     = -1;   /* index of selected shape, -1 = none */
static i32 hover_shape   = -1;

/* Drawing drag state */
static i32 drag_active   = 0;
static i32 drag_sx, drag_sy;   /* start x/y (canvas coords) */
static i32 drag_cx, drag_cy;   /* current x/y */

/* Select drag state */
static i32 sel_dragging  = 0;
static i32 sel_drag_ox, sel_drag_oy;   /* offset from shape origin */
static i32 resize_handle = -1;   /* 0=TL,1=TR,2=BL,3=BR or -1 */
static i32 resize_orig_x0, resize_orig_y0;
static i32 resize_orig_x1, resize_orig_y1;

/* Poly-in-progress */
static i32 poly_wip      = 0;   /* 1 while adding vertices */
static Shape wip_shape;

/* Style selections */
static u32 cur_stroke    = 0xFF000000u;
static u32 cur_fill      = 0x00000000u;  /* transparent by default */
static i32 cur_sw        = 2;            /* stroke width */

/* Palette sub-selection: 0 = choosing stroke, 1 = choosing fill */
static i32 pal_mode      = 0;

/* -------------------------------------------------------------------------
 * Drawing primitives into ARGB32 buffer.
 * stride_px = stride in pixels (stride_bytes / 4).
 * ----------------------------------------------------------------------- */

static inline void setpx(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                          i32 x, i32 y, u32 col)
{
    if ((u32)x < bw && (u32)y < bh)
        buf[(u32)y * stride_px + (u32)x] = col;
}

static void fill_rect_c(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                         i32 x, i32 y, i32 w, i32 h, u32 col)
{
    if (w <= 0 || h <= 0) return;
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = col;
    }
}

static void draw_border_c(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                           i32 x, i32 y, i32 w, i32 h, u32 col)
{
    fill_rect_c(buf, stride_px, bw, bh, x,       y,       w, 1, col);
    fill_rect_c(buf, stride_px, bw, bh, x,       y+h-1,   w, 1, col);
    fill_rect_c(buf, stride_px, bw, bh, x,       y,       1, h, col);
    fill_rect_c(buf, stride_px, bw, bh, x+w-1,   y,       1, h, col);
}

/* Bresenham line with configurable width (stamped circles for width>1). */
static void iabs_local_dummy(void) {}  /* suppress unused warning */
static i32 iabs2(i32 v) { return v < 0 ? -v : v; }

static void stamp_square(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                          i32 cx, i32 cy, i32 hw, u32 col)
{
    /* hw = half-width; draws (2*hw+1) x (2*hw+1) square centered at cx,cy */
    fill_rect_c(buf, stride_px, bw, bh, cx - hw, cy - hw,
                2*hw+1, 2*hw+1, col);
}

static void draw_line_thick(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                             i32 x0, i32 y0, i32 x1, i32 y1,
                             i32 sw, u32 col)
{
    i32 hw = sw / 2;
    i32 dx = iabs2(x1 - x0), dy = iabs2(y1 - y0);
    i32 sx = x0 < x1 ? 1 : -1;
    i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx - dy;
    for (;;) {
        stamp_square(buf, stride_px, bw, bh, x0, y0, hw, col);
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Filled rectangle with optional stroke. */
static void draw_rect_shape(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                             i32 x0, i32 y0, i32 x1, i32 y1,
                             u32 fill, u32 stroke, i32 sw)
{
    /* Normalise. */
    if (x0 > x1) { i32 t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { i32 t = y0; y0 = y1; y1 = t; }
    i32 w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w <= 0 || h <= 0) return;
    if (fill >> 24) /* has alpha -- treat as opaque fill if alpha != 0 */
        fill_rect_c(buf, stride_px, bw, bh, x0, y0, w, h, fill);
    if (sw > 0 && (stroke >> 24)) {
        i32 hw = sw / 2;
        draw_border_c(buf, stride_px, bw, bh,
                      x0 - hw, y0 - hw, w + 2*hw, h + 2*hw, stroke);
        /* Extra lines for stroke_w > 1 */
        for (i32 s = 1; s < sw; s++) {
            draw_border_c(buf, stride_px, bw, bh,
                          x0 - hw + s, y0 - hw + s,
                          w + 2*hw - 2*s, h + 2*hw - 2*s, stroke);
        }
    }
}

/* Filled ellipse using midpoint algorithm. */
static void draw_ellipse_shape(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                                i32 x0, i32 y0, i32 x1, i32 y1,
                                u32 fill, u32 stroke, i32 sw)
{
    if (x0 > x1) { i32 t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { i32 t = y0; y0 = y1; y1 = t; }
    i32 cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    i32 rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx <= 0 || ry <= 0) return;

    /* Fill: scan lines */
    if (fill >> 24) {
        for (i32 yy = cy - ry; yy <= cy + ry; yy++) {
            /* x at this y: (x-cx)^2/rx^2 + (y-cy)^2/ry^2 <= 1 */
            i32 dy = yy - cy;
            /* dx^2 <= rx^2 * (1 - dy^2/ry^2) = rx^2 - rx^2*dy^2/ry^2 */
            /* Use 64-bit style via long long to avoid overflow */
            long long num = (long long)rx*rx * ((long long)ry*ry - (long long)dy*dy);
            long long den = (long long)ry*ry;
            if (den == 0) continue;
            long long dx2 = num / den;
            if (dx2 < 0) continue;
            i32 dx = (i32)dx2;
            /* integer sqrt */
            i32 dxi = rx;
            while ((long long)dxi*dxi > dx2) dxi--;
            fill_rect_c(buf, stride_px, bw, bh, cx - dxi, yy, 2*dxi+1, 1, fill);
        }
    }

    /* Stroke: midpoint ellipse outline, thick */
    if (sw > 0 && (stroke >> 24)) {
        /* Walk quarter ellipse. */
        long long rx2 = (long long)rx * rx;
        long long ry2 = (long long)ry * ry;
        i32 x = 0, y = ry;
        long long d1 = ry2 - rx2 * ry + rx2 / 4;
        while (2 * ry2 * x <= 2 * rx2 * y) {
            /* 4-quadrant symmetry */
            for (i32 s = 0; s < sw; s++) {
                stamp_square(buf, stride_px, bw, bh, cx+x, cy+y, 0, stroke);
                stamp_square(buf, stride_px, bw, bh, cx-x, cy+y, 0, stroke);
                stamp_square(buf, stride_px, bw, bh, cx+x, cy-y, 0, stroke);
                stamp_square(buf, stride_px, bw, bh, cx-x, cy-y, 0, stroke);
                (void)s;
            }
            if (d1 < 0) {
                x++;
                d1 += 2 * ry2 * x + ry2;
            } else {
                x++;
                y--;
                d1 += 2 * ry2 * x - 2 * rx2 * y + ry2;
            }
        }
        long long d2 = ry2 * ((long long)x * x + x) +
                       rx2 * ((long long)(y-1) * (y-1)) - rx2 * ry2;
        while (y >= 0) {
            stamp_square(buf, stride_px, bw, bh, cx+x, cy+y, 0, stroke);
            stamp_square(buf, stride_px, bw, bh, cx-x, cy+y, 0, stroke);
            stamp_square(buf, stride_px, bw, bh, cx+x, cy-y, 0, stroke);
            stamp_square(buf, stride_px, bw, bh, cx-x, cy-y, 0, stroke);
            if (d2 > 0) {
                y--;
                d2 -= 2 * rx2 * y + rx2;
            } else {
                x++;
                y--;
                d2 += 2 * ry2 * x - 2 * rx2 * y + rx2;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Render a single shape onto buf (canvas coordinates = screen - CANVAS_X/Y).
 * We add the canvas offset before passing to primitives.
 * ----------------------------------------------------------------------- */
static void render_shape(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                          const Shape *s, u32 stroke_override, int use_override)
{
    u32 sc = use_override ? stroke_override : s->stroke_col;
    u32 fc = s->fill_col;
    i32 sw = s->stroke_w;

    /* Canvas-to-screen offset */
    i32 ox = CANVAS_X, oy = CANVAS_Y;

    switch (s->type) {
    case SH_LINE:
        draw_line_thick(buf, stride_px, bw, bh,
                        s->x0 + ox, s->y0 + oy,
                        s->x1 + ox, s->y1 + oy, sw, sc);
        break;

    case SH_RECT:
        draw_rect_shape(buf, stride_px, bw, bh,
                        s->x0 + ox, s->y0 + oy,
                        s->x1 + ox, s->y1 + oy, fc, sc, sw);
        break;

    case SH_ELLIPSE:
        draw_ellipse_shape(buf, stride_px, bw, bh,
                           s->x0 + ox, s->y0 + oy,
                           s->x1 + ox, s->y1 + oy, fc, sc, sw);
        break;

    case SH_POLY:
        if (s->npts >= 2) {
            for (i32 i = 0; i < s->npts - 1; i++) {
                draw_line_thick(buf, stride_px, bw, bh,
                                s->pts[i][0] + ox,   s->pts[i][1] + oy,
                                s->pts[i+1][0] + ox, s->pts[i+1][1] + oy,
                                sw, sc);
            }
        }
        break;

    case SH_TEXT:
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         s->x0 + ox, s->y0 + oy, s->text, sc);
        break;
    }
}

/* -------------------------------------------------------------------------
 * Selection handles: 4 corner squares (3x3) around shape bounding box.
 * ----------------------------------------------------------------------- */
#define HANDLE_R  5  /* half-size of handle square */

static void shape_bbox(const Shape *s, i32 *bx0, i32 *by0, i32 *bx1, i32 *by1)
{
    switch (s->type) {
    case SH_POLY: {
        if (s->npts == 0) { *bx0 = *by0 = *bx1 = *by1 = 0; return; }
        i32 mnx = s->pts[0][0], mny = s->pts[0][1];
        i32 mxx = mnx, mxy = mny;
        for (i32 i = 1; i < s->npts; i++) {
            if (s->pts[i][0] < mnx) mnx = s->pts[i][0];
            if (s->pts[i][1] < mny) mny = s->pts[i][1];
            if (s->pts[i][0] > mxx) mxx = s->pts[i][0];
            if (s->pts[i][1] > mxy) mxy = s->pts[i][1];
        }
        *bx0 = mnx; *by0 = mny; *bx1 = mxx; *by1 = mxy;
        break;
    }
    default:
        *bx0 = s->x0 < s->x1 ? s->x0 : s->x1;
        *by0 = s->y0 < s->y1 ? s->y0 : s->y1;
        *bx1 = s->x0 > s->x1 ? s->x0 : s->x1;
        *by1 = s->y0 > s->y1 ? s->y0 : s->y1;
        break;
    }
}

static void draw_sel_handles(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                              const Shape *s)
{
    i32 bx0, by0, bx1, by1;
    shape_bbox(s, &bx0, &by0, &bx1, &by1);

    i32 ox = CANVAS_X, oy = CANVAS_Y;
    bx0 += ox; by0 += oy; bx1 += ox; by1 += oy;

    /* Dashed selection box */
    u32 sc = C_SEL_BOX;
    draw_border_c(buf, stride_px, bw, bh, bx0-2, by0-2, bx1-bx0+5, by1-by0+5, sc);

    /* 4 corner handles */
    i32 corners[4][2] = {
        {bx0, by0}, {bx1, by0}, {bx0, by1}, {bx1, by1}
    };
    for (i32 i = 0; i < 4; i++) {
        fill_rect_c(buf, stride_px, bw, bh,
                    corners[i][0] - HANDLE_R, corners[i][1] - HANDLE_R,
                    2*HANDLE_R+1, 2*HANDLE_R+1, C_WHITE);
        draw_border_c(buf, stride_px, bw, bh,
                      corners[i][0] - HANDLE_R, corners[i][1] - HANDLE_R,
                      2*HANDLE_R+1, 2*HANDLE_R+1, C_SEL_HANDLE);
    }
}

/* Hit-test a handle: returns 0-3 (TL/TR/BL/BR) or -1. */
static i32 hit_handle(const Shape *s, i32 mx, i32 my)
{
    i32 bx0, by0, bx1, by1;
    shape_bbox(s, &bx0, &by0, &bx1, &by1);
    bx0 += CANVAS_X; by0 += CANVAS_Y; bx1 += CANVAS_X; by1 += CANVAS_Y;
    i32 corners[4][2] = {
        {bx0, by0}, {bx1, by0}, {bx0, by1}, {bx1, by1}
    };
    for (i32 i = 0; i < 4; i++) {
        if (iabs2(mx - corners[i][0]) <= HANDLE_R + 2 &&
            iabs2(my - corners[i][1]) <= HANDLE_R + 2)
            return i;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Shape hit-test (point-in-bounding-box, generous).
 * ----------------------------------------------------------------------- */
static i32 hit_shape(const Shape *s, i32 cx, i32 cy)
{
    /* cx, cy are canvas coords */
    i32 bx0, by0, bx1, by1;
    shape_bbox(s, &bx0, &by0, &bx1, &by1);
    i32 pad = 4;
    return (cx >= bx0 - pad && cx <= bx1 + pad &&
            cy >= by0 - pad && cy <= by1 + pad);
}

/* Find topmost (highest index = front) alive shape under point. */
static i32 find_shape_at(i32 cx, i32 cy)
{
    for (i32 i = nshapes - 1; i >= 0; i--) {
        if (shapes[i].alive && hit_shape(&shapes[i], cx, cy))
            return i;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Sidebar: tool buttons.
 * ----------------------------------------------------------------------- */
static const char * const tool_names[NUM_TOOLS] = {
    "Sel", "Line", "Rect", "Oval", "Poly", "Text"
};

static void draw_sidebar(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                          i32 cur_t)
{
    fill_rect_c(buf, stride_px, bw, bh, 0, 0, SIDEBAR_W, WIN_H, C_BG_DARK);
    fill_rect_c(buf, stride_px, bw, bh, SIDEBAR_W-1, 0, 1, WIN_H, C_SEP);

    /* Title */
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     4, 8, "VPaint", C_ACCENT2);

    /* Tool buttons (start at y=36) */
    i32 btn_y = 36;
    for (i32 t = 0; t < NUM_TOOLS; t++) {
        i32 y = btn_y + t * 36;
        u32 face = (t == cur_t) ? C_ACCENT : C_BG_MID;
        u32 tcol = (t == cur_t) ? C_WHITE  : C_ACCENT2;
        fill_rect_c(buf, stride_px, bw, bh, 4, y, SIDEBAR_W - 8, 30, face);
        draw_border_c(buf, stride_px, bw, bh, 4, y, SIDEBAR_W - 8, 30,
                      t == cur_t ? C_ACCENT2 : C_SEP);
        /* Center text in button */
        i32 tw = font_text_width(tool_names[t]);
        i32 tx = 4 + (SIDEBAR_W - 8 - tw) / 2;
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         tx, y + 8, tool_names[t], tcol);
    }

    /* Z-order buttons */
    i32 zy = btn_y + NUM_TOOLS * 36 + 10;
    fill_rect_c(buf, stride_px, bw, bh, 4, zy, SIDEBAR_W-8, 22, C_BG_MID);
    draw_border_c(buf, stride_px, bw, bh, 4, zy, SIDEBAR_W-8, 22, C_SEP);
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     8, zy + 4, "Front", C_TEXT_DIM);

    fill_rect_c(buf, stride_px, bw, bh, 4, zy+26, SIDEBAR_W-8, 22, C_BG_MID);
    draw_border_c(buf, stride_px, bw, bh, 4, zy+26, SIDEBAR_W-8, 22, C_SEP);
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     8, zy+30, "Back", C_TEXT_DIM);

    /* Undo button */
    fill_rect_c(buf, stride_px, bw, bh, 4, zy+56, SIDEBAR_W-8, 22, C_BG_MID);
    draw_border_c(buf, stride_px, bw, bh, 4, zy+56, SIDEBAR_W-8, 22, C_SEP);
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     12, zy+60, "Undo", C_TEXT_DIM);
}

/* -------------------------------------------------------------------------
 * Palette bar: color swatches + stroke width + fill toggle + mode label.
 * ----------------------------------------------------------------------- */
#define SWATCH_W    20
#define SWATCH_H    20
#define SWATCH_GAP   2
#define SWATCH_Y0   12
#define SWATCH_X0   (SIDEBAR_W + 8)

static void draw_palette(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                          u32 s_col, u32 f_col, i32 sw, i32 pm)
{
    fill_rect_c(buf, stride_px, bw, bh, SIDEBAR_W, 0, WIN_W - SIDEBAR_W, PALETTE_H, C_BG_MID);
    fill_rect_c(buf, stride_px, bw, bh, SIDEBAR_W, PALETTE_H-1, WIN_W-SIDEBAR_W, 1, C_SEP);

    /* Mode label */
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     SWATCH_X0, 4,
                     pm == 0 ? "Stroke:" : "Fill:  ", C_ACCENT2);

    /* Color swatches */
    for (i32 i = 0; i < NUM_PAL; i++) {
        i32 sx = SWATCH_X0 + 56 + i * (SWATCH_W + SWATCH_GAP);
        u32 col = palette_colors[i];
        if (i == NUM_PAL - 1) {
            /* Transparent: draw checkerboard */
            for (i32 yy = SWATCH_Y0; yy < SWATCH_Y0 + SWATCH_H; yy++)
                for (i32 xx = sx; xx < sx + SWATCH_W; xx++) {
                    u32 c = ((xx + yy) & 1) ? 0xFFCCCCCCu : 0xFFFFFFFFu;
                    setpx(buf, stride_px, bw, bh, xx, yy, c);
                }
        } else {
            fill_rect_c(buf, stride_px, bw, bh, sx, SWATCH_Y0, SWATCH_W, SWATCH_H, col);
        }
        /* Highlight selected */
        u32 sel = (pm == 0) ? s_col : f_col;
        if (col == sel)
            draw_border_c(buf, stride_px, bw, bh,
                          sx - 2, SWATCH_Y0 - 2, SWATCH_W + 4, SWATCH_H + 4,
                          C_WHITE);
    }

    /* Mode toggle button */
    i32 mtx = SWATCH_X0 + 56 + NUM_PAL * (SWATCH_W + SWATCH_GAP) + 8;
    fill_rect_c(buf, stride_px, bw, bh, mtx, 8, 52, 26, C_BG_DARK);
    draw_border_c(buf, stride_px, bw, bh, mtx, 8, 52, 26, C_ACCENT);
    font_draw_string(buf, (int)stride_px, bw, bh,
                     mtx + 4, 14,
                     pm == 0 ? "->Fill" : "->Strk", C_ACCENT2);

    /* Stroke-width indicator (drawn further right) */
    i32 swx = mtx + 60;
    font_draw_string(buf, (int)stride_px, bw, bh, swx, 4, "W:", C_ACCENT2);
    /* Minus */
    fill_rect_c(buf, stride_px, bw, bh, swx + 20, 12, 12, 12, C_BG_DARK);
    draw_border_c(buf, stride_px, bw, bh, swx + 20, 12, 12, 12, C_SEP);
    font_draw_string(buf, (int)stride_px, bw, bh, swx + 23, 14, "-", C_WHITE);
    /* Current value */
    char sbuf[4];
    sbuf[0] = '0' + (char)(sw % 10);
    sbuf[1] = 0;
    if (sw >= 10) { sbuf[1] = sbuf[0]; sbuf[0] = '0' + (char)(sw / 10); sbuf[2] = 0; }
    font_draw_string(buf, (int)stride_px, bw, bh, swx + 35, 14, sbuf, C_WHITE);
    /* Plus */
    fill_rect_c(buf, stride_px, bw, bh, swx + 48, 12, 12, 12, C_BG_DARK);
    draw_border_c(buf, stride_px, bw, bh, swx + 48, 12, 12, 12, C_SEP);
    font_draw_string(buf, (int)stride_px, bw, bh, swx + 51, 14, "+", C_WHITE);

    /* Current stroke/fill preview squares */
    i32 px = swx + 68;
    fill_rect_c(buf, stride_px, bw, bh, px, 8, 16, 16, s_col >> 24 ? s_col : 0xFF888888u);
    draw_border_c(buf, stride_px, bw, bh, px, 8, 16, 16, C_WHITE);
    font_draw_string(buf, (int)stride_px, bw, bh, px + 18, 4, "S", 0xFF888888u);
    fill_rect_c(buf, stride_px, bw, bh, px, 26, 16, 16, f_col >> 24 ? f_col : 0xFF444444u);
    draw_border_c(buf, stride_px, bw, bh, px, 26, 16, 16, C_WHITE);
    font_draw_string(buf, (int)stride_px, bw, bh, px + 18, 22, "F", 0xFF888888u);
}

/* -------------------------------------------------------------------------
 * Status bar.
 * ----------------------------------------------------------------------- */
static void int_to_str(i32 v, char *out, int max_len)
{
    /* Simple integer to decimal string (non-negative only). */
    if (v < 0) { out[0] = '-'; v = -v; int_to_str(v, out+1, max_len-1); return; }
    if (max_len <= 1) { out[0] = 0; return; }
    char tmp[12]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0 && n < 11) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    for (int i = n - 1; i >= 0 && j < max_len - 1; i--) out[j++] = tmp[i];
    out[j] = 0;
}

static void str_cat(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (dst[i]) i++;
    int j = 0;
    while (src[j] && i + j < max_len - 1) { dst[i + j] = src[j]; j++; }
    dst[i + j] = 0;
}

static const char * const type_names[5] = {
    "Line", "Rect", "Oval", "Poly", "Text"
};

static void draw_status(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                         i32 ns, i32 sel, i32 tool)
{
    i32 sy = WIN_H - STATUS_H;
    fill_rect_c(buf, stride_px, bw, bh, 0, sy, WIN_W, STATUS_H, C_STATUS_BG);
    fill_rect_c(buf, stride_px, bw, bh, 0, sy, WIN_W, 1, C_SEP);

    char msg[128]; msg[0] = 0;
    str_cat(msg, " Shapes: ", 128);
    char nbuf[8]; int_to_str(ns, nbuf, 8);
    str_cat(msg, nbuf, 128);
    if (sel >= 0 && sel < ns && shapes[sel].alive) {
        str_cat(msg, "  |  Selected: ", 128);
        str_cat(msg, type_names[shapes[sel].type], 128);
        str_cat(msg, " #", 128);
        int_to_str(sel, nbuf, 8);
        str_cat(msg, nbuf, 128);
    }
    str_cat(msg, "  |  Tool: ", 128);
    str_cat(msg, tool_names[tool], 128);
    str_cat(msg, "  |  [Del]=delete  [U]=undo  [S]=save  [Tab]=stroke/fill", 128);

    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     4, sy + 3, msg, C_TEXT_DIM);
}

/* -------------------------------------------------------------------------
 * Canvas clear + full scene render.
 * ----------------------------------------------------------------------- */
static void render_scene(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                          i32 sel, i32 hover)
{
    /* Clear canvas area */
    fill_rect_c(buf, stride_px, bw, bh,
                CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H, C_CANVAS);

    /* Render all alive shapes */
    for (i32 i = 0; i < nshapes; i++) {
        if (!shapes[i].alive) continue;
        render_shape(buf, stride_px, bw, bh, &shapes[i], 0, 0);
    }

    /* Hover highlight (thin tinted border around bbox) */
    if (hover >= 0 && hover != sel && shapes[hover].alive) {
        i32 bx0, by0, bx1, by1;
        shape_bbox(&shapes[hover], &bx0, &by0, &bx1, &by1);
        bx0 += CANVAS_X; by0 += CANVAS_Y; bx1 += CANVAS_X; by1 += CANVAS_Y;
        draw_border_c(buf, stride_px, bw, bh,
                      bx0 - 2, by0 - 2, bx1 - bx0 + 5, by1 - by0 + 5,
                      C_HOVER);
    }

    /* Selection handles */
    if (sel >= 0 && sel < nshapes && shapes[sel].alive)
        draw_sel_handles(buf, stride_px, bw, bh, &shapes[sel]);
}

/* -------------------------------------------------------------------------
 * Add a new shape (returns index or -1 if full).
 * ----------------------------------------------------------------------- */
static i32 add_shape(const Shape *s)
{
    if (nshapes >= MAX_SHAPES) return -1;
    shapes[nshapes] = *s;
    shapes[nshapes].alive = 1;
    return nshapes++;
}

/* -------------------------------------------------------------------------
 * Z-order helpers.
 * ----------------------------------------------------------------------- */
static void bring_front(i32 idx)
{
    if (idx < 0 || idx >= nshapes - 1) return;
    Shape tmp = shapes[idx];
    for (i32 i = idx; i < nshapes - 1; i++)
        shapes[i] = shapes[i+1];
    shapes[nshapes-1] = tmp;
    sel_shape = nshapes - 1;
}

static void send_back(i32 idx)
{
    if (idx <= 0 || idx >= nshapes) return;
    Shape tmp = shapes[idx];
    for (i32 i = idx; i > 0; i--)
        shapes[i] = shapes[i-1];
    shapes[0] = tmp;
    sel_shape = 0;
}

/* -------------------------------------------------------------------------
 * Save scene to /tmp/drawing.vec (simple text format).
 * ----------------------------------------------------------------------- */
static void write_num(int fd, i32 n)
{
    char buf[16]; int_to_str(n, buf, 16);
    sys_write_str(fd, buf);
}

static void save_scene(void)
{
    long fd = sc6(SYS_OPEN, (long)"/tmp/drawing.vec",
                  0x241 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644, 0, 0, 0);
    if (fd < 0) { print("[VPAINT] save failed\n"); return; }
    int ifd = (int)fd;
    sys_write_str(ifd, "# Vector Paint scene\n");
    for (i32 i = 0; i < nshapes; i++) {
        if (!shapes[i].alive) continue;
        sys_write_str(ifd, "shape ");
        write_num(ifd, shapes[i].type);
        sys_write_str(ifd, " ");
        write_num(ifd, shapes[i].x0); sys_write_str(ifd, " ");
        write_num(ifd, shapes[i].y0); sys_write_str(ifd, " ");
        write_num(ifd, shapes[i].x1); sys_write_str(ifd, " ");
        write_num(ifd, shapes[i].y1); sys_write_str(ifd, " ");
        write_num(ifd, (i32)shapes[i].stroke_col); sys_write_str(ifd, " ");
        write_num(ifd, (i32)shapes[i].fill_col);   sys_write_str(ifd, " ");
        write_num(ifd, shapes[i].stroke_w);
        sys_write_str(ifd, "\n");
    }
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    print("[VPAINT] scene saved\n");
}

/* -------------------------------------------------------------------------
 * Palette / sidebar hit tests (screen coords).
 * ----------------------------------------------------------------------- */

/* Returns palette color index or -1. */
static i32 hit_palette_swatch(i32 mx, i32 my)
{
    if (my < SWATCH_Y0 || my >= SWATCH_Y0 + SWATCH_H) return -1;
    if (mx < SWATCH_X0 + 56) return -1;
    for (i32 i = 0; i < NUM_PAL; i++) {
        i32 sx = SWATCH_X0 + 56 + i * (SWATCH_W + SWATCH_GAP);
        if (mx >= sx && mx < sx + SWATCH_W) return i;
    }
    return -1;
}

/* Returns sidebar tool index (-1 if not hit), also checks z/undo buttons. */
/* Returns: >= 0 tool, -100 = Front, -101 = Back, -102 = Undo. */
static i32 hit_sidebar(i32 mx, i32 my)
{
    if (mx >= SIDEBAR_W) return -1;
    i32 btn_y = 36;
    for (i32 t = 0; t < NUM_TOOLS; t++) {
        i32 y = btn_y + t * 36;
        if (mx >= 4 && mx < SIDEBAR_W - 4 && my >= y && my < y + 30)
            return t;
    }
    i32 zy = btn_y + NUM_TOOLS * 36 + 10;
    if (mx >= 4 && mx < SIDEBAR_W - 4) {
        if (my >= zy     && my < zy + 22)    return -100; /* Front */
        if (my >= zy+26  && my < zy + 48)    return -101; /* Back  */
        if (my >= zy+56  && my < zy + 78)    return -102; /* Undo  */
    }
    return -1;
}

/* Returns: -200 = mode toggle, -210 = W-, -211 = W+, else -1. */
static i32 hit_palette_ctrl(i32 mx, i32 my)
{
    if (my >= PALETTE_H) return -1;
    /* Mode toggle button */
    i32 mtx = SWATCH_X0 + 56 + NUM_PAL * (SWATCH_W + SWATCH_GAP) + 8;
    if (mx >= mtx && mx < mtx + 52 && my >= 8 && my < 34) return -200;
    /* Stroke width - / + */
    i32 swx = mtx + 60;
    if (mx >= swx+20 && mx < swx+32 && my >= 12 && my < 24) return -210; /* minus */
    if (mx >= swx+48 && mx < swx+60 && my >= 12 && my < 24) return -211; /* plus  */
    return -1;
}

/* -------------------------------------------------------------------------
 * Main event loop helpers.
 * ----------------------------------------------------------------------- */

/* Apply a finalised shape from drag. */
static void finalise_drag(i32 end_x, i32 end_y)
{
    if (cur_tool == TOOL_SELECT || cur_tool == TOOL_TEXT || cur_tool == TOOL_POLY)
        return; /* handled elsewhere */

    /* Minimum size check */
    if (iabs2(end_x - drag_sx) < 2 && iabs2(end_y - drag_sy) < 2)
        return;

    history_push();

    Shape s;
    /* zero-init */
    for (i32 i = 0; i < (i32)sizeof(Shape); i++) ((u8 *)&s)[i] = 0;
    s.x0 = drag_sx; s.y0 = drag_sy;
    s.x1 = end_x;   s.y1 = end_y;
    s.stroke_col = cur_stroke;
    s.fill_col   = cur_fill;
    s.stroke_w   = cur_sw;
    s.alive      = 1;

    switch (cur_tool) {
    case TOOL_LINE:    s.type = SH_LINE;    break;
    case TOOL_RECT:    s.type = SH_RECT;    break;
    case TOOL_ELLIPSE: s.type = SH_ELLIPSE; break;
    default: return;
    }

    sel_shape = add_shape(&s);
}

/* -------------------------------------------------------------------------
 * _start
 * ----------------------------------------------------------------------- */
void _start(void)
{
    print("[VPAINT] starting\n");

    if (wl_connect() != 0) {
        print("[VPAINT] wl_connect FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Vector Paint");
    if (!win) {
        print("[VPAINT] wl_create_window FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[VPAINT] window created\n");

    u32 stride_px = win->stride / 4u;
    u32 bw = win->w, bh = win->h;

    /* Clears entire buffer once */
    fill_rect_c(win->pixels, stride_px, bw, bh, 0, 0, WIN_W, WIN_H, C_BG_DARK);

    /* Mouse state */
    i32 last_btn = 0;
    i32 last_mx = 0, last_my = 0;

    /* Keyboard: track ctrl key via keycode */
    /* We treat keycode 0x1D (left ctrl) as ctrl. */

    /* ------------------------------------------------------------------ */
    for (;;) {
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {

            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea, my = (i32)eb;
                i32 btn = ec & 1;
                i32 was_btn = last_btn;
                last_mx = mx; last_my = my;

                /* Convert to canvas coords */
                i32 cx = mx - CANVAS_X;
                i32 cy = my - CANVAS_Y;
                i32 on_canvas = (mx >= CANVAS_X && mx < CANVAS_X + CANVAS_W &&
                                 my >= CANVAS_Y && my < CANVAS_Y + CANVAS_H);

                /* Hover update (only in canvas, only in select mode) */
                if (cur_tool == TOOL_SELECT && on_canvas && !drag_active) {
                    hover_shape = find_shape_at(cx, cy);
                }

                /* ---------- Button press ---------- */
                if (btn && !was_btn) {
                    /* Toolbar / sidebar */
                    i32 sb = hit_sidebar(mx, my);
                    if (sb >= 0) {
                        /* Switch tool; finish any wip poly */
                        if (poly_wip) { poly_wip = 0; }
                        cur_tool = sb;
                        sel_shape = -1;
                        goto event_done;
                    }
                    if (sb == -100) { /* Front */
                        if (sel_shape >= 0) { history_push(); bring_front(sel_shape); }
                        goto event_done;
                    }
                    if (sb == -101) { /* Back */
                        if (sel_shape >= 0) { history_push(); send_back(sel_shape); }
                        goto event_done;
                    }
                    if (sb == -102) { /* Undo */
                        history_pop();
                        sel_shape = -1;
                        goto event_done;
                    }

                    /* Palette row */
                    if (my < PALETTE_H) {
                        i32 pi = hit_palette_swatch(mx, my);
                        if (pi >= 0) {
                            if (pal_mode == 0) cur_stroke = palette_colors[pi];
                            else               cur_fill   = palette_colors[pi];
                            /* Also update selected shape live */
                            if (sel_shape >= 0 && shapes[sel_shape].alive) {
                                history_push();
                                if (pal_mode == 0)
                                    shapes[sel_shape].stroke_col = cur_stroke;
                                else
                                    shapes[sel_shape].fill_col = cur_fill;
                            }
                            goto event_done;
                        }
                        i32 pc = hit_palette_ctrl(mx, my);
                        if (pc == -200) { pal_mode ^= 1; goto event_done; }
                        if (pc == -210) { if (cur_sw > 1) cur_sw--; goto event_done; }
                        if (pc == -211) { if (cur_sw < 16) cur_sw++; goto event_done; }
                        goto event_done; /* click in palette but no target */
                    }

                    if (!on_canvas) goto event_done;

                    /* ---- Canvas click ---- */
                    if (cur_tool == TOOL_SELECT) {
                        /* Check handle first */
                        if (sel_shape >= 0 && shapes[sel_shape].alive) {
                            i32 h = hit_handle(&shapes[sel_shape], mx, my);
                            if (h >= 0) {
                                resize_handle = h;
                                sel_dragging  = 0;
                                i32 bx0, by0, bx1, by1;
                                shape_bbox(&shapes[sel_shape], &bx0, &by0, &bx1, &by1);
                                resize_orig_x0 = bx0; resize_orig_y0 = by0;
                                resize_orig_x1 = bx1; resize_orig_y1 = by1;
                                drag_active = 1;
                                drag_sx = mx; drag_sy = my;
                                goto event_done;
                            }
                        }
                        resize_handle = -1;
                        i32 hit = find_shape_at(cx, cy);
                        if (hit >= 0) {
                            sel_shape = hit;
                            /* Start move drag */
                            sel_dragging = 1;
                            drag_active  = 1;
                            drag_sx = mx; drag_sy = my;
                        } else {
                            sel_shape = -1;
                            sel_dragging = 0;
                        }
                    } else if (cur_tool == TOOL_TEXT) {
                        /* Place text immediately */
                        history_push();
                        Shape s;
                        for (i32 k = 0; k < (i32)sizeof(Shape); k++) ((u8 *)&s)[k] = 0;
                        s.type = SH_TEXT;
                        s.x0 = cx; s.y0 = cy;
                        s.x1 = cx + 40; s.y1 = cy + FONT_H;
                        s.stroke_col = cur_stroke;
                        s.fill_col   = 0;
                        s.stroke_w   = cur_sw;
                        s.alive      = 1;
                        s.text[0]='T'; s.text[1]='e'; s.text[2]='x';
                        s.text[3]='t'; s.text[4]=0;
                        sel_shape = add_shape(&s);
                    } else if (cur_tool == TOOL_POLY) {
                        if (!poly_wip) {
                            /* Start poly */
                            for (i32 k=0; k<(i32)sizeof(wip_shape); k++)
                                ((u8*)&wip_shape)[k]=0;
                            wip_shape.type = SH_POLY;
                            wip_shape.stroke_col = cur_stroke;
                            wip_shape.fill_col   = cur_fill;
                            wip_shape.stroke_w   = cur_sw;
                            wip_shape.alive      = 1;
                            wip_shape.pts[0][0] = cx;
                            wip_shape.pts[0][1] = cy;
                            wip_shape.npts = 1;
                            poly_wip = 1;
                        } else {
                            /* Check double-click (close proximity to last point) */
                            i32 lpx = wip_shape.pts[wip_shape.npts-1][0];
                            i32 lpy = wip_shape.pts[wip_shape.npts-1][1];
                            i32 near = (iabs2(cx - lpx) < 8 && iabs2(cy - lpy) < 8);
                            if (near || wip_shape.npts >= MAX_POLY_PTS - 1) {
                                /* Finish */
                                history_push();
                                sel_shape = add_shape(&wip_shape);
                                poly_wip = 0;
                            } else {
                                /* Add vertex */
                                wip_shape.pts[wip_shape.npts][0] = cx;
                                wip_shape.pts[wip_shape.npts][1] = cy;
                                wip_shape.npts++;
                            }
                        }
                    } else {
                        /* Line / Rect / Ellipse: start drag */
                        drag_active = 1;
                        drag_sx = cx; drag_sy = cy;
                        drag_cx = cx; drag_cy = cy;
                    }
                }

                /* ---------- Motion ---------- */
                if (btn) {
                    if (drag_active) {
                        if (cur_tool == TOOL_SELECT) {
                            if (resize_handle >= 0 && sel_shape >= 0) {
                                /* Resize */
                                i32 ddx = mx - drag_sx;
                                i32 ddy = my - drag_sy;
                                i32 bx0 = resize_orig_x0, by0 = resize_orig_y0;
                                i32 bx1 = resize_orig_x1, by1 = resize_orig_y1;
                                switch (resize_handle) {
                                case 0: bx0 += ddx; by0 += ddy; break; /* TL */
                                case 1: bx1 += ddx; by0 += ddy; break; /* TR */
                                case 2: bx0 += ddx; by1 += ddy; break; /* BL */
                                case 3: bx1 += ddx; by1 += ddy; break; /* BR */
                                }
                                shapes[sel_shape].x0 = bx0;
                                shapes[sel_shape].y0 = by0;
                                shapes[sel_shape].x1 = bx1;
                                shapes[sel_shape].y1 = by1;
                                /* Poly resize: scale all points */
                                if (shapes[sel_shape].type == SH_POLY) {
                                    /* Simple: map points proportionally -- skip for poly */
                                }
                            } else if (sel_dragging && sel_shape >= 0) {
                                /* Move */
                                i32 ddx = mx - drag_sx;
                                i32 ddy = my - drag_sy;
                                drag_sx = mx; drag_sy = my;
                                shapes[sel_shape].x0 += ddx;
                                shapes[sel_shape].y0 += ddy;
                                shapes[sel_shape].x1 += ddx;
                                shapes[sel_shape].y1 += ddy;
                                if (shapes[sel_shape].type == SH_POLY) {
                                    for (i32 p = 0; p < shapes[sel_shape].npts; p++) {
                                        shapes[sel_shape].pts[p][0] += ddx;
                                        shapes[sel_shape].pts[p][1] += ddy;
                                    }
                                }
                            }
                        } else if (cur_tool != TOOL_POLY && cur_tool != TOOL_TEXT) {
                            drag_cx = cx; drag_cy = cy;
                        }
                    }
                }

                /* ---------- Button release ---------- */
                if (!btn && was_btn) {
                    if (drag_active) {
                        if (cur_tool != TOOL_SELECT &&
                            cur_tool != TOOL_POLY &&
                            cur_tool != TOOL_TEXT) {
                            finalise_drag(drag_cx, drag_cy);
                        }
                        if (cur_tool == TOOL_SELECT && (sel_dragging || resize_handle >= 0)) {
                            history_push();
                        }
                        drag_active  = 0;
                        sel_dragging = 0;
                        resize_handle = -1;
                    }
                }

                last_btn = btn;
                event_done:;

            } else if (kind == WL_EVENT_KEY) {
                i32 keycode = (i32)ea;
                i32 pressed = (i32)eb;
                if (!pressed) goto key_done;

                /* Key 0x53 = 'S', 0x55 = 'U', 0x08 = Delete/Backspace, 0x09 = Tab
                 * These are scan-code style values from the PS/2 driver; actual
                 * values depend on kernel mapping.  We accept both lower/upper via
                 * bitmask match where safe. */

                switch (keycode) {
                /* Delete / backspace */
                case 0x08: case 0x7F: case 0xFF:
                    if (sel_shape >= 0 && shapes[sel_shape].alive) {
                        history_push();
                        shapes[sel_shape].alive = 0;
                        sel_shape = -1;
                    }
                    break;
                /* U = undo */
                case 'u': case 'U': case 0x15:
                    history_pop();
                    sel_shape = -1;
                    break;
                /* S = save */
                case 's': case 'S': case 0x13:
                    save_scene();
                    break;
                /* Tab = toggle stroke/fill mode */
                case '\t':
                    pal_mode ^= 1;
                    break;
                /* Escape: deselect / cancel poly */
                case 0x1B:
                    poly_wip = 0;
                    sel_shape = -1;
                    break;
                /* Tool shortcuts */
                case '1': cur_tool = TOOL_SELECT;  sel_shape = -1; break;
                case '2': cur_tool = TOOL_LINE;    break;
                case '3': cur_tool = TOOL_RECT;    break;
                case '4': cur_tool = TOOL_ELLIPSE; break;
                case '5': cur_tool = TOOL_POLY;    break;
                case '6': cur_tool = TOOL_TEXT;    break;
                /* [ ] for stroke width */
                case '[': if (cur_sw > 1)  cur_sw--; break;
                case ']': if (cur_sw < 16) cur_sw++; break;
                /* F/B z-order */
                case 'f': case 'F':
                    if (sel_shape >= 0) { history_push(); bring_front(sel_shape); }
                    break;
                case 'b': case 'B':
                    if (sel_shape >= 0) { history_push(); send_back(sel_shape); }
                    break;
                }
                key_done:;
            }
        }

        /* ---- Render ---- */
        render_scene(win->pixels, stride_px, bw, bh, sel_shape, hover_shape);

        /* Drag preview (for shape-create tools while dragging) */
        if (drag_active && cur_tool != TOOL_SELECT &&
            cur_tool != TOOL_POLY && cur_tool != TOOL_TEXT) {
            Shape preview;
            for (i32 k = 0; k < (i32)sizeof(Shape); k++) ((u8*)&preview)[k] = 0;
            preview.x0 = drag_sx; preview.y0 = drag_sy;
            preview.x1 = drag_cx; preview.y1 = drag_cy;
            preview.stroke_col = cur_stroke;
            preview.fill_col   = cur_fill;
            preview.stroke_w   = cur_sw;
            preview.alive      = 1;
            switch (cur_tool) {
            case TOOL_LINE:    preview.type = SH_LINE;    break;
            case TOOL_RECT:    preview.type = SH_RECT;    break;
            case TOOL_ELLIPSE: preview.type = SH_ELLIPSE; break;
            default: break;
            }
            render_shape(win->pixels, stride_px, bw, bh, &preview, 0, 0);
        }

        /* Poly WIP preview */
        if (poly_wip && wip_shape.npts > 0) {
            /* Draw existing poly edges */
            if (wip_shape.npts > 1) {
                Shape tmp = wip_shape;
                render_shape(win->pixels, stride_px, bw, bh, &tmp, 0, 0);
            }
            /* Rubber-band line from last vertex to mouse */
            i32 lpx = wip_shape.pts[wip_shape.npts-1][0];
            i32 lpy = wip_shape.pts[wip_shape.npts-1][1];
            i32 rmx = last_mx - CANVAS_X, rmy = last_my - CANVAS_Y;
            draw_line_thick(win->pixels, stride_px, bw, bh,
                            lpx + CANVAS_X, lpy + CANVAS_Y,
                            rmx + CANVAS_X, rmy + CANVAS_Y,
                            1, 0xFF8888FFu);
        }

        draw_sidebar(win->pixels, stride_px, bw, bh, cur_tool);
        draw_palette(win->pixels, stride_px, bw, bh,
                     cur_stroke, cur_fill, cur_sw, pal_mode);
        draw_status(win->pixels, stride_px, bw, bh, nshapes, sel_shape, cur_tool);

        wl_commit(win);
        sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
