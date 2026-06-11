/**
 * icon.c  --  Procedural icon library implementation
 *
 * Compile (verify clean, no fs:0x28 stack canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone \
 *       -O2 -c userspace/lib/icon/icon.c -o /tmp/icon.o
 *
 * Freestanding: no libc, no libm, no syscalls.
 * All arithmetic is integer / fixed-point.
 */

#include "icon.h"
#include "../font/bitfont.h"   /* font_draw_char, font_draw_string, FONT_W, FONT_H */
#include "icon_assets.h"       /* embedded Google Material icons (ARGB silhouettes) */

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Pixel colour macros (0xAARRGGBB) */
#define IC_A(c)  (((c) >> 24) & 0xFFu)
#define IC_R(c)  (((c) >> 16) & 0xFFu)
#define IC_G(c)  (((c) >>  8) & 0xFFu)
#define IC_B(c)  (        (c) & 0xFFu)
#define IC_ARGB(a,r,g,b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
     ((uint32_t)(g) <<  8) | (uint32_t)(b))

/* ---------------------------------------------------------------------- */
/* Alpha-composite src OVER dst (both ARGB32).                             */
static inline uint32_t ic_blend(uint32_t src, uint32_t dst)
{
    unsigned sa = IC_A(src);
    if (sa == 0)   return dst;
    if (sa == 255) return src;

    unsigned da = IC_A(dst);
    unsigned inv = 255u - sa;
    unsigned oa  = sa + (da * inv + 127u) / 255u;

    /* Premultiplied-like composite for R/G/B */
#define BLEND_CH(sc, dc) \
    (((sc) * sa + (dc) * da * inv / 255u + 127u) / (oa ? oa : 1u))

    unsigned or_ = BLEND_CH(IC_R(src), IC_R(dst));
    unsigned og  = BLEND_CH(IC_G(src), IC_G(dst));
    unsigned ob  = BLEND_CH(IC_B(src), IC_B(dst));
#undef BLEND_CH
    return IC_ARGB(oa, or_, og, ob);
}

/* Write one pixel (alpha-composite over existing content). */
static inline void ic_put(uint32_t *px, int stride, int x, int y, uint32_t c)
{
    uint32_t *p = px + y * stride + x;
    *p = ic_blend(c, *p);
}

/* Write one pixel only if coordinates are inside [ox,oy .. ox+w, oy+h). */
static inline void ic_put_clipped(uint32_t *px, int stride,
                                  int ox, int oy, int w, int h,
                                  int x, int y, uint32_t c)
{
    if (x < ox || y < oy || x >= ox + w || y >= oy + h) return;
    ic_put(px, stride, x, y, c);
}

/* Fill a solid rectangle (clipped to icon cell). */
static void ic_fill_rect(uint32_t *px, int stride,
                         int cx, int cy, int cw, int ch,   /* clip rect  */
                         int rx, int ry, int rw, int rh,   /* draw rect  */
                         uint32_t col)
{
    /* Intersect */
    int x0 = rx < cx ? cx : rx;
    int y0 = ry < cy ? cy : ry;
    int x1 = (rx+rw) > (cx+cw) ? (cx+cw) : (rx+rw);
    int y1 = (ry+rh) > (cy+ch) ? (cy+ch) : (ry+rh);

    unsigned sa = IC_A(col);
    for (int row = y0; row < y1; row++) {
        uint32_t *row_ptr = px + row * stride + x0;
        if (sa == 255) {
            for (int col_ = x0; col_ < x1; col_++, row_ptr++)
                *row_ptr = col;
        } else {
            for (int col_ = x0; col_ < x1; col_++, row_ptr++)
                *row_ptr = ic_blend(col, *row_ptr);
        }
    }
}

/* Bresenham line (clipped to icon cell). */
static void ic_line(uint32_t *px, int stride,
                    int cx, int cy, int cw, int ch,
                    int x0, int y0, int x1, int y1,
                    uint32_t col, int thick)
{
    int dx = x1 - x0;  if (dx < 0) dx = -dx;
    int dy = y1 - y0;  if (dy < 0) dy = -dy;
    int sx = (x1 >= x0) ? 1 : -1;
    int sy = (y1 >= y0) ? 1 : -1;
    int err = dx - dy;
    int half = thick / 2;

    for (;;) {
        /* Stamp a `thick`-pixel wide spot */
        for (int ty = -half; ty <= half; ty++)
            for (int tx = -half; tx <= half; tx++)
                ic_put_clipped(px, stride, cx, cy, cw, ch, x0+tx, y0+ty, col);

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Filled circle (Bresenham, clipped to icon cell). */
static void ic_fill_circle(uint32_t *px, int stride,
                           int cx_clip, int cy_clip, int cw, int ch,
                           int cx, int cy, int r, uint32_t col)
{
    if (r <= 0) return;
    int x = 0, y = r, d = 1 - r;
    while (x <= y) {
        /* Fill horizontal spans for the 8 octants */
        ic_fill_rect(px, stride, cx_clip, cy_clip, cw, ch,
                     cx-y, cy-x, 2*y, 1, col);
        ic_fill_rect(px, stride, cx_clip, cy_clip, cw, ch,
                     cx-x, cy-y, 2*x, 1, col);
        ic_fill_rect(px, stride, cx_clip, cy_clip, cw, ch,
                     cx-y, cy+x, 2*y, 1, col);
        ic_fill_rect(px, stride, cx_clip, cy_clip, cw, ch,
                     cx-x, cy+y, 2*x, 1, col);
        if (d < 0) { d += 2*x + 3; }
        else       { d += 2*(x-y) + 5; y--; }
        x++;
    }
}

/* Filled quarter-circle (quadrant 0=TL,1=TR,2=BR,3=BL) centred at (cx,cy). */
static void ic_fill_qcircle(uint32_t *px, int stride,
                             int clip_x, int clip_y, int clip_w, int clip_h,
                             int cx, int cy, int r, int quad, uint32_t col)
{
    if (r <= 0) return;
    for (int dy = 0; dy <= r; dy++) {
        /* integer sqrt via Newton's method: x = r^2 - dy^2 */
        int rsq = r*r - dy*dy;
        /* isqrt */
        int xr = r;
        while (xr * xr > rsq) xr--;

        int px0, py0, pw, ph;
        switch (quad) {
        case 0: px0 = cx-xr; py0 = cy-dy; pw = xr; ph = 1; break; /* TL */
        case 1: px0 = cx;    py0 = cy-dy; pw = xr; ph = 1; break; /* TR */
        case 2: px0 = cx;    py0 = cy+dy; pw = xr; ph = 1; break; /* BR */
        default:px0 = cx-xr; py0 = cy+dy; pw = xr; ph = 1; break; /* BL */
        }
        ic_fill_rect(px, stride, clip_x, clip_y, clip_w, clip_h,
                     px0, py0, pw, ph, col);
    }
}

/* Rounded rectangle fill.  radius clamped to min(w,h)/2. */
static void ic_fill_rrect(uint32_t *px, int stride,
                          int clip_x, int clip_y, int clip_w, int clip_h,
                          int rx, int ry, int rw, int rh, int radius,
                          uint32_t col)
{
    int max_r = (rw < rh ? rw : rh) / 2;
    if (radius > max_r) radius = max_r;
    if (radius < 0) radius = 0;

    /* Centre cross */
    ic_fill_rect(px, stride, clip_x, clip_y, clip_w, clip_h,
                 rx + radius, ry,          rw - 2*radius, rh,        col);
    ic_fill_rect(px, stride, clip_x, clip_y, clip_w, clip_h,
                 rx,          ry + radius, radius,        rh-2*radius, col);
    ic_fill_rect(px, stride, clip_x, clip_y, clip_w, clip_h,
                 rx+rw-radius,ry + radius, radius,        rh-2*radius, col);

    /* Four corner quarter-circles */
    ic_fill_qcircle(px, stride, clip_x, clip_y, clip_w, clip_h,
                    rx+radius,    ry+radius,    radius, 0, col); /* TL */
    ic_fill_qcircle(px, stride, clip_x, clip_y, clip_w, clip_h,
                    rx+rw-radius, ry+radius,    radius, 1, col); /* TR */
    ic_fill_qcircle(px, stride, clip_x, clip_y, clip_w, clip_h,
                    rx+rw-radius, ry+rh-radius, radius, 2, col); /* BR */
    ic_fill_qcircle(px, stride, clip_x, clip_y, clip_w, clip_h,
                    rx+radius,    ry+rh-radius, radius, 3, col); /* BL */
}

/* Lerp two colours by t/256 (0=all a, 256=all b). */
static inline uint32_t ic_lerp_col(uint32_t a, uint32_t b, int t)
{
    if (t <= 0)   return a;
    if (t >= 256) return b;
    int ti = 256 - t;
    return IC_ARGB(
        (IC_A(a)*ti + IC_A(b)*t) >> 8,
        (IC_R(a)*ti + IC_R(b)*t) >> 8,
        (IC_G(a)*ti + IC_G(b)*t) >> 8,
        (IC_B(a)*ti + IC_B(b)*t) >> 8
    );
}

/* Lighten a colour by frac/256 towards white. */
static inline uint32_t ic_lighten(uint32_t c, int frac)
{
    return ic_lerp_col(c, IC_ARGB(IC_A(c), 255, 255, 255), frac);
}

/* Darken a colour by frac/256 towards black. */
static inline uint32_t ic_darken(uint32_t c, int frac)
{
    return ic_lerp_col(c, IC_ARGB(IC_A(c), 0, 0, 0), frac);
}

/* Derive a tile background from an accent: darken + slightly desaturate. */
static inline uint32_t ic_bg_from_accent(uint32_t accent)
{
    /* Desaturate 25 %, darken 30 % */
    unsigned r = IC_R(accent), g = IC_G(accent), b = IC_B(accent);
    unsigned grey = (r*77u + g*150u + b*29u) >> 8;
    unsigned dr = r + (grey - r) * 64u / 256u;
    unsigned dg = g + (grey - g) * 64u / 256u;
    unsigned db = b + (grey - b) * 64u / 256u;
    /* Darken 30 % */
    dr = dr * 178u / 256u;
    dg = dg * 178u / 256u;
    db = db * 178u / 256u;
    return IC_ARGB(IC_A(accent), dr, dg, db);
}

/* Draw the rounded-rect tile background with vertical gradient. */
static void ic_draw_tile_bg(uint32_t *px, int stride,
                            int x, int y, int size,
                            uint32_t bg, int corner_r)
{
    uint32_t top    = ic_lighten(bg, 30);  /* +12 % towards white */
    uint32_t bottom = ic_darken (bg, 30);  /* +12 % towards black */

    for (int row = 0; row < size; row++) {
        int t = row * 256 / (size > 1 ? size - 1 : 1);
        uint32_t row_col = ic_lerp_col(top, bottom, t);
        /* Paint the full-width row, then mask corners below */
        ic_fill_rect(px, stride, x, y, size, size,
                     x, y + row, size, 1, row_col);
    }

    /* Punch out corners by painting with transparent black (erase approach:
     * use the caller's existing background).  Since we don't know the
     * caller's bg, we overdraw the corners with a "round corner mask":
     * For each corner quadrant, pixels OUTSIDE the circle radius get alpha=0
     * (transparent) via ic_fill_qcircle.  Instead, we fill only the inside
     * of each corner circle with the gradient colour, which achieves the
     * rounded look without needing an erase operation.
     *
     * The simplest correct approach: re-draw the full rectangle then composite
     * the four corner quarter-discs in the GRADIENT colour (already done above
     * because ic_fill_rrect paints the gradient as solid colours).
     *
     * Here we use a per-pixel approach for the corner regions only. */

    /* Corner mask: for pixels in each corner box, check distance from the
     * corner circle centre; if outside, set pixel to 0x00000000 (transparent). */
    int r = corner_r;
    /* Top-left corner box */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int dist2 = (r-1-dx)*(r-1-dx) + (r-1-dy)*(r-1-dy);
            if (dist2 > r*r) {
                uint32_t *p = px + (y + dy) * stride + (x + dx);
                *p = 0x00000000;
            }
        }
    }
    /* Top-right corner box */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int dist2 = dx*dx + (r-1-dy)*(r-1-dy);
            if (dist2 > r*r) {
                uint32_t *p = px + (y + dy) * stride + (x + size - r + dx);
                *p = 0x00000000;
            }
        }
    }
    /* Bottom-right corner box */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int dist2 = dx*dx + dy*dy;
            if (dist2 > r*r) {
                uint32_t *p = px + (y + size - r + dy) * stride + (x + size - r + dx);
                *p = 0x00000000;
            }
        }
    }
    /* Bottom-left corner box */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int dist2 = (r-1-dx)*(r-1-dx) + dy*dy;
            if (dist2 > r*r) {
                uint32_t *p = px + (y + size - r + dy) * stride + (x + dx);
                *p = 0x00000000;
            }
        }
    }
}

/* Compute corner radius: ~22 % of size, min 2, max 16. */
static inline int ic_corner_r(int size)
{
    int r = size * 22 / 100;
    if (r < 2)  r = 2;
    if (r > 16) r = 16;
    return r;
}

/* String length (no libc). */
static int ic_strlen(const char *s)
{
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Case-insensitive character compare. */
static inline int ic_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Case-insensitive prefix match: does `str` start with `prefix`? */
static int ic_starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix) return 0;
    while (*prefix) {
        if (ic_tolower((unsigned char)*str) != ic_tolower((unsigned char)*prefix))
            return 0;
        str++; prefix++;
    }
    return 1;
}

/* Simple djb2 hash on a string. */
static uint32_t ic_hash(const char *s)
{
    uint32_t h = 5381u;
    if (!s) return h;
    while (*s) {
        h = ((h << 5) + h) ^ (unsigned char)*s;
        s++;
    }
    return h;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * icon_rounded_tile
 * -------------------------------------------------------------------------*/
void icon_rounded_tile(uint32_t *px, int stride, int x, int y, int size,
                       uint32_t bg, uint32_t fg, const char *glyph)
{
    if (!px || size <= 0) return;

    int r = ic_corner_r(size);

    /* Paint gradient background */
    ic_draw_tile_bg(px, stride, x, y, size, bg, r);

    /* Draw glyph text, at most 2 characters */
    if (!glyph || !glyph[0]) return;

    int glen = ic_strlen(glyph);
    if (glen > 2) glen = 2;

    /* bitfont glyphs are FONT_W x FONT_H (8x16).
     * Scale factor so the glyph fills ~55 % of the icon height. */
    /* We draw at 1:1 for small icons; for larger icons we do a 2x scale. */
    int scale = (size >= 40) ? 2 : 1;
    int gw = FONT_W  * scale * glen;
    int gh = FONT_H  * scale;

    /* Centre inside the tile */
    int gx = x + (size - gw) / 2;
    int gy = y + (size - gh) / 2;

    /* For 1x: just use font_draw_string. */
    if (scale == 1) {
        /* Clamp buffer dims to the tile so font renderer doesn't
         * need the full framebuffer size -- pass very large bw/bh. */
        const char *p = glyph;
        for (int i = 0; i < glen && *p; i++, p++) {
            font_draw_char(px, stride,
                           x + size, y + size,   /* bw/bh: end of tile */
                           gx + i * FONT_W, gy,
                           *p, fg);
        }
    } else {
        /* 2× software scale: draw into a tiny 1× buffer then blit 2×2 blocks */
        uint32_t tmp[FONT_W * 2 * FONT_H]; /* max 2 chars × 8 × 16 */
        /* Zero it */
        for (int i = 0; i < FONT_W * 2 * (int)FONT_H; i++) tmp[i] = 0;

        const char *p = glyph;
        for (int i = 0; i < glen && *p; i++, p++)
            font_draw_char(tmp, FONT_W * 2, FONT_W * 2, FONT_H,
                           i * FONT_W, 0, *p, fg);

        /* Blit 2×2 */
        for (int row = 0; row < FONT_H; row++) {
            for (int col = 0; col < FONT_W * glen; col++) {
                uint32_t c = tmp[row * FONT_W * 2 + col];
                if (IC_A(c) == 0) continue;
                int dx = gx + col * 2;
                int dy = gy + row * 2;
                for (int br = 0; br < 2; br++) {
                    for (int bc = 0; bc < 2; bc++) {
                        int px_ = dx + bc, py_ = dy + br;
                        if (px_ >= x && py_ >= y && px_ < x+size && py_ < y+size)
                            ic_put(px, stride, px_, py_, c);
                    }
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * icon_terminal  -- dark shell background, ">_" prompt
 * -------------------------------------------------------------------------*/
void icon_terminal(uint32_t *px, int stride, int x, int y, int size,
                   uint32_t accent)
{
    /* Dark charcoal background tile */
    uint32_t bg  = IC_ARGB(0xFF, 0x1E, 0x1E, 0x2E);
    uint32_t bar = IC_ARGB(0xFF, 0x31, 0x31, 0x45); /* title bar */
    int cr = ic_corner_r(size);

    /* Background */
    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Title bar strip (top ~20 %) */
    int bh = size / 5;
    if (bh < 2) bh = 2;
    ic_fill_rect(px, stride, x, y, size, size,
                 x, y + cr, size, bh - cr, bar);
    /* Dot indicators in bar */
    int dot_r = size / 20; if (dot_r < 1) dot_r = 1;
    uint32_t red_dot  = IC_ARGB(0xFF, 0xFF, 0x5F, 0x57);
    uint32_t yel_dot  = IC_ARGB(0xFF, 0xFF, 0xBD, 0x2E);
    uint32_t grn_dot  = IC_ARGB(0xFF, 0x28, 0xC9, 0x41);
    int dot_y = y + bh / 2;
    int dot_gap = size / 10; if (dot_gap < 2) dot_gap = 2;
    ic_fill_circle(px, stride, x, y, size, size,
                   x + cr + dot_gap,         dot_y, dot_r, red_dot);
    ic_fill_circle(px, stride, x, y, size, size,
                   x + cr + dot_gap*2 + dot_r*2+1, dot_y, dot_r, yel_dot);
    ic_fill_circle(px, stride, x, y, size, size,
                   x + cr + dot_gap*3 + dot_r*4+2, dot_y, dot_r, grn_dot);

    /* ">_" prompt text in the content area */
    /* Draw ">" chevron */
    int content_top = y + bh + size/10;
    int content_x   = x + size/10;
    int lh = size / 10; if (lh < 1) lh = 1;    /* line half-height */
    int lw = size / 5;  if (lw < 2) lw = 2;    /* chevron width */

    uint32_t green = IC_ARGB(0xFF,
                             (IC_R(accent)*3 + 0x50) / 4,
                             (IC_G(accent)*3 + 0xFF) / 4,
                             (IC_B(accent)*3 + 0x50) / 4);

    /* > : two lines forming a V-rotated-90 */
    ic_line(px, stride, x, y, size, size,
            content_x,       content_top,
            content_x + lw,  content_top + lh,
            green, lh > 1 ? 1 : 1);
    ic_line(px, stride, x, y, size, size,
            content_x + lw,  content_top + lh,
            content_x,       content_top + lh*2,
            green, 1);

    /* _ : underscore */
    int ux = content_x + lw + size/8;
    int uy = content_top + lh*2;
    int uw = lw;
    ic_fill_rect(px, stride, x, y, size, size,
                 ux, uy, uw, lh > 1 ? lh : 1, green);
}

/* -------------------------------------------------------------------------
 * icon_folder  -- manila folder
 * -------------------------------------------------------------------------*/
void icon_folder(uint32_t *px, int stride, int x, int y, int size,
                 uint32_t accent)
{
    uint32_t bg   = ic_bg_from_accent(accent);
    uint32_t body = accent;
    uint32_t tab  = ic_lighten(accent, 50);
    int cr = ic_corner_r(size);

    /* Tile background */
    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Folder body: lower ~65 % of the icon area */
    int pad   = size / 8;
    int fy    = y + size * 35 / 100;
    int fx    = x + pad;
    int fw    = size - pad*2;
    int fh    = size - fy + y - pad;
    int fcr   = fh / 6; if (fcr < 1) fcr = 1;

    ic_fill_rrect(px, stride, x, y, size, size,
                  fx, fy, fw, fh, fcr, body);

    /* Tab: small rounded rect on top-left of folder */
    int tw = fw * 42 / 100;
    int th = size / 8; if (th < 2) th = 2;
    int ty = fy - th + fcr;
    int tx = fx;
    ic_fill_rrect(px, stride, x, y, size, size,
                  tx, ty, tw, th + fcr, fcr, tab);

    /* Folder highlight: thin lighter strip across top of body */
    uint32_t hi = ic_lighten(body, 70);
    hi = IC_ARGB(0x55, IC_R(hi), IC_G(hi), IC_B(hi)); /* semi-transparent */
    ic_fill_rect(px, stride, x, y, size, size,
                 fx, fy, fw, fh / 5, hi);
}

/* -------------------------------------------------------------------------
 * icon_file_text  -- white page, dog-ear, ruled lines
 * -------------------------------------------------------------------------*/
void icon_file_text(uint32_t *px, int stride, int x, int y, int size,
                    uint32_t accent)
{
    uint32_t bg      = ic_bg_from_accent(accent);
    uint32_t page    = IC_ARGB(0xFF, 0xF5, 0xF5, 0xF5);
    uint32_t ear_col = IC_ARGB(0xFF, 0xCC, 0xCC, 0xCC);
    uint32_t line_col= IC_ARGB(0xA0, IC_R(accent)/2+60, IC_G(accent)/2+60, IC_B(accent)/2+60);
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Page */
    int pad = size / 8;
    int pw  = size * 60 / 100;
    int ph  = size * 70 / 100;
    int px_ = x + (size - pw) / 2;
    int py_ = y + (size - ph) / 2;
    int ear = pw / 4; /* dog-ear size */

    /* Page body (without top-right corner) */
    ic_fill_rect(px, stride, x, y, size, size,
                 px_, py_, pw - ear, ph, page);        /* left part */
    ic_fill_rect(px, stride, x, y, size, size,
                 px_ + pw - ear, py_ + ear, ear, ph - ear, page); /* right part */

    /* Dog-ear triangle: fill background colour over top-right corner, then
     * draw the fold line */
    ic_fill_rect(px, stride, x, y, size, size,
                 px_ + pw - ear, py_, ear, ear, bg);  /* cut corner */
    /* Fold crease */
    ic_line(px, stride, x, y, size, size,
            px_ + pw - ear, py_,
            px_ + pw,       py_ + ear,
            ear_col, 1);
    /* Small fold shadow triangle -- darker version of ear */
    for (int dy = 0; dy < ear; dy++) {
        int xe = px_ + pw - ear + dy;
        int ye = py_ + dy;
        ic_put_clipped(px, stride, x, y, size, size, xe, ye, ear_col);
    }

    /* Ruled lines (3-5 lines depending on size) */
    int nlines = (ph - ear - pad) / (ph / 5);
    if (nlines < 2) nlines = 2;
    if (nlines > 5) nlines = 5;
    int line_gap = (ph - ear - pad) / (nlines + 1);
    for (int li = 1; li <= nlines; li++) {
        int lx0 = px_ + pad/2;
        int lx1 = px_ + pw - ear - pad/2;
        int ly  = py_ + ear + li * line_gap;
        (void)lx1;
        ic_fill_rect(px, stride, x, y, size, size,
                     lx0, ly, pw - ear - pad, 1, line_col);
    }
}

/* -------------------------------------------------------------------------
 * icon_paint  -- artist brush + colour swatches
 * -------------------------------------------------------------------------*/
void icon_paint(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent)
{
    uint32_t bg  = ic_bg_from_accent(accent);
    int cr = ic_corner_r(size);
    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Colour swatch palette row at bottom */
    uint32_t swatches[4] = {
        IC_ARGB(0xFF, 0xFF, 0x4A, 0x4A),  /* red   */
        IC_ARGB(0xFF, 0xFF, 0xD0, 0x00),  /* yellow*/
        IC_ARGB(0xFF, 0x4A, 0xB0, 0xFF),  /* blue  */
        accent                             /* accent*/
    };
    int sw_y = y + size * 72 / 100;
    int sw_h = size / 7; if (sw_h < 2) sw_h = 2;
    int sw_w = size * 15 / 100; if (sw_w < 3) sw_w = 3;
    int sw_gap = size / 20; if (sw_gap < 1) sw_gap = 1;
    int total_sw = 4 * sw_w + 3 * sw_gap;
    int sw_x0 = x + (size - total_sw) / 2;
    for (int i = 0; i < 4; i++) {
        int scr = sw_w / 4; if (scr < 1) scr = 1;
        ic_fill_rrect(px, stride, x, y, size, size,
                      sw_x0 + i*(sw_w+sw_gap), sw_y, sw_w, sw_h, scr,
                      swatches[i]);
    }

    /* Brush: diagonal line from top-right to centre, with a round tip */
    int b_tip_x = x + size * 65 / 100;
    int b_tip_y = y + size * 58 / 100;
    int b_end_x = x + size * 25 / 100;
    int b_end_y = y + size * 18 / 100;
    uint32_t handle_col = IC_ARGB(0xFF, 0xC8, 0x96, 0x60);
    uint32_t tip_col    = IC_ARGB(0xFF, 0x30, 0x30, 0x30);
    int lw = size / 16; if (lw < 1) lw = 1;

    ic_line(px, stride, x, y, size, size,
            b_end_x, b_end_y, b_tip_x, b_tip_y,
            handle_col, lw);
    /* Bristle tip */
    ic_fill_circle(px, stride, x, y, size, size,
                   b_tip_x, b_tip_y, lw + 1, tip_col);
    /* Dot of paint at tip in accent */
    ic_fill_circle(px, stride, x, y, size, size,
                   b_tip_x, b_tip_y, lw, accent);
}

/* -------------------------------------------------------------------------
 * icon_calc  -- grid of buttons + = row
 * -------------------------------------------------------------------------*/
void icon_calc(uint32_t *px, int stride, int x, int y, int size,
               uint32_t accent)
{
    uint32_t bg      = ic_bg_from_accent(accent);
    uint32_t body    = IC_ARGB(0xFF, 0x2A, 0x2A, 0x3E);
    uint32_t btn_col = IC_ARGB(0xFF, 0x42, 0x42, 0x60);
    uint32_t eq_col  = accent;
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Calculator body */
    int pad = size / 8;
    int cw  = size - pad*2;
    int ch  = size - pad*2;
    int bcr = cr / 2; if (bcr < 1) bcr = 1;
    ic_fill_rrect(px, stride, x, y, size, size,
                  x + pad, y + pad, cw, ch, bcr, body);

    /* Grid: 3 columns × 4 rows of buttons */
    int cols = 3, rows = 4;
    int inner_pad = size / 16; if (inner_pad < 1) inner_pad = 1;
    int grid_x = x + pad + inner_pad;
    int grid_y = y + pad + inner_pad;
    int grid_w = cw - inner_pad*2;
    int grid_h = ch - inner_pad*2;
    int btn_w  = (grid_w - (cols-1)*inner_pad) / cols;
    int btn_h  = (grid_h - (rows-1)*inner_pad) / rows;
    if (btn_w < 2) btn_w = 2;
    if (btn_h < 2) btn_h = 2;
    int btn_cr = btn_h / 4; if (btn_cr < 1) btn_cr = 1;

    for (int row = 0; row < rows; row++) {
        int by = grid_y + row * (btn_h + inner_pad);
        for (int col = 0; col < cols; col++) {
            int bx = grid_x + col * (btn_w + inner_pad);
            uint32_t bc = (row == rows-1) ? eq_col : btn_col;
            ic_fill_rrect(px, stride, x, y, size, size,
                          bx, by, btn_w, btn_h, btn_cr, bc);
        }
    }
}

/* -------------------------------------------------------------------------
 * icon_clock  -- clock face with hour + minute hands
 * -------------------------------------------------------------------------*/
void icon_clock(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent)
{
    uint32_t bg    = ic_bg_from_accent(accent);
    uint32_t face  = IC_ARGB(0xFF, 0xF8, 0xF4, 0xE8);
    uint32_t rim   = accent;
    uint32_t hand  = IC_ARGB(0xFF, 0x20, 0x20, 0x30);
    uint32_t sec_h = IC_ARGB(0xFF, 0xFF, 0x40, 0x40);
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    int cx_ = x + size / 2;
    int cy_ = y + size / 2;
    int rim_r = size * 42 / 100;
    int face_r = rim_r - size / 14;

    ic_fill_circle(px, stride, x, y, size, size, cx_, cy_, rim_r, rim);
    ic_fill_circle(px, stride, x, y, size, size, cx_, cy_, face_r, face);

    /* Hour ticks: 12 small marks */
    for (int tick = 0; tick < 12; tick++) {
        /* angle in 256ths of a full circle (256 = 360°) */
        /* Fixed-point sin/cos via lookup-free integer approximation */
        /* We use a 256-step table approximation: sin(θ) ≈ ... */
        /* θ in units of 1/256 full circle = tick * 256 / 12 */
        int angle = tick * 256 / 12;
        /* Integer sin/cos — CORDIC-lite 16-bit fixed point (x16) */
        /* sin table for angles 0..63 (quarter circle, x256 scale) */
        static const int8_t sin_q[65] = {
              0,  4,  8, 13, 17, 22, 26, 31,
             35, 39, 44, 48, 52, 57, 61, 65,
             70, 74, 78, 82, 86, 90, 94, 98,
            102,106,109,113,117,120,124,127,
            131,134,137,141,144,147,150,153,
            156,159,162,165,167,170,173,175,
            178,180,183,185,187,189,191,193,
            195,197,199,201,202,204,205,207,
            208
        };
        /* Map angle (0-255) to sin/cos using the quarter table */
        /* sin(angle): */
        int q = angle & 63;
        int sin_v, cos_v;
        switch (angle >> 6) {
        case 0: sin_v =  sin_q[q];    cos_v =  sin_q[64-q]; break;
        case 1: sin_v =  sin_q[64-q]; cos_v = -sin_q[q];    break;
        case 2: sin_v = -sin_q[q];    cos_v = -sin_q[64-q]; break;
        default:sin_v = -sin_q[64-q]; cos_v =  sin_q[q];    break;
        }
        /* Outer tick point */
        int ox = cx_ + (cos_v * face_r) / 208;
        int oy = cy_ - (sin_v * face_r) / 208; /* subtract: y grows down */
        /* Inner tick point */
        int inner_r = face_r - face_r / 7;
        int ix = cx_ + (cos_v * inner_r) / 208;
        int iy = cy_ - (sin_v * inner_r) / 208;
        ic_line(px, stride, x, y, size, size,
                ix, iy, ox, oy,
                IC_ARGB(0x80, 0x60, 0x60, 0x80), 1);
    }

    /* Hour hand (pointing to ~10 o'clock, angle -60° from 12) */
    /* 10 o'clock = 300° from 12 = 300*256/360 = 213 units */
    {
        int angle = 213;
        int q = angle & 63;
        int sin_v, cos_v;
        static const int8_t sin_q[65] = {
              0,  4,  8, 13, 17, 22, 26, 31,
             35, 39, 44, 48, 52, 57, 61, 65,
             70, 74, 78, 82, 86, 90, 94, 98,
            102,106,109,113,117,120,124,127,
            131,134,137,141,144,147,150,153,
            156,159,162,165,167,170,173,175,
            178,180,183,185,187,189,191,193,
            195,197,199,201,202,204,205,207,
            208
        };
        switch (angle >> 6) {
        case 0: sin_v =  sin_q[q];    cos_v =  sin_q[64-q]; break;
        case 1: sin_v =  sin_q[64-q]; cos_v = -sin_q[q];    break;
        case 2: sin_v = -sin_q[q];    cos_v = -sin_q[64-q]; break;
        default:sin_v = -sin_q[64-q]; cos_v =  sin_q[q];    break;
        }
        int hr = face_r * 55 / 100;
        int hx = cx_ + (cos_v * hr) / 208;
        int hy = cy_ - (sin_v * hr) / 208;
        ic_line(px, stride, x, y, size, size,
                cx_, cy_, hx, hy, hand, size / 18 > 1 ? size / 18 : 1);
    }

    /* Minute hand (pointing to ~2 o'clock, angle 60° from 12) */
    {
        int angle = 43; /* 60*256/360 = 43 */
        int q = angle & 63;
        int sin_v, cos_v;
        static const int8_t sin_q[65] = {
              0,  4,  8, 13, 17, 22, 26, 31,
             35, 39, 44, 48, 52, 57, 61, 65,
             70, 74, 78, 82, 86, 90, 94, 98,
            102,106,109,113,117,120,124,127,
            131,134,137,141,144,147,150,153,
            156,159,162,165,167,170,173,175,
            178,180,183,185,187,189,191,193,
            195,197,199,201,202,204,205,207,
            208
        };
        switch (angle >> 6) {
        case 0: sin_v =  sin_q[q];    cos_v =  sin_q[64-q]; break;
        case 1: sin_v =  sin_q[64-q]; cos_v = -sin_q[q];    break;
        case 2: sin_v = -sin_q[q];    cos_v = -sin_q[64-q]; break;
        default:sin_v = -sin_q[64-q]; cos_v =  sin_q[q];    break;
        }
        int mr = face_r * 75 / 100;
        int mx = cx_ + (cos_v * mr) / 208;
        int my = cy_ - (sin_v * mr) / 208;
        ic_line(px, stride, x, y, size, size,
                cx_, cy_, mx, my, hand, size / 22 > 1 ? size / 22 : 1);
    }

    /* Second hand (pointing to ~6 o'clock) */
    {
        int sr = face_r * 80 / 100;
        ic_line(px, stride, x, y, size, size,
                cx_, cy_, cx_, cy_ + sr, sec_h, 1);
    }

    /* Centre dot */
    ic_fill_circle(px, stride, x, y, size, size,
                   cx_, cy_, size / 18 > 1 ? size / 18 : 1, hand);
}

/* -------------------------------------------------------------------------
 * icon_game  -- D-pad + two action buttons (SNES-style)
 * -------------------------------------------------------------------------*/
void icon_game(uint32_t *px, int stride, int x, int y, int size,
               uint32_t accent)
{
    uint32_t bg     = IC_ARGB(0xFF, 0x1A, 0x1A, 0x2E);
    uint32_t body   = IC_ARGB(0xFF, 0x28, 0x28, 0x45);
    uint32_t dpad   = IC_ARGB(0xFF, 0x40, 0x40, 0x60);
    uint32_t btn_a  = IC_ARGB(0xFF, 0xFF, 0x44, 0x44);
    uint32_t btn_b  = accent;
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Controller body: wide pill shape */
    int bw = size * 80 / 100;
    int bh = size * 55 / 100;
    int bx = x + (size - bw) / 2;
    int by = y + (size - bh) / 2;
    ic_fill_rrect(px, stride, x, y, size, size,
                  bx, by, bw, bh, bh/2, body);

    /* D-pad (cross shape) on left side */
    int dpad_cx = bx + bw / 4;
    int dpad_cy = by + bh / 2;
    int dpad_arm  = bh / 5; if (dpad_arm < 2) dpad_arm = 2;
    int dpad_thick= dpad_arm;
    /* Horizontal arm */
    ic_fill_rect(px, stride, x, y, size, size,
                 dpad_cx - dpad_arm*2, dpad_cy - dpad_thick/2,
                 dpad_arm*4, dpad_thick, dpad);
    /* Vertical arm */
    ic_fill_rect(px, stride, x, y, size, size,
                 dpad_cx - dpad_thick/2, dpad_cy - dpad_arm*2,
                 dpad_thick, dpad_arm*4, dpad);

    /* Action buttons A, B on right side */
    int ab_cx = bx + bw * 3 / 4;
    int ab_cy = by + bh / 2;
    int btn_r = bh / 7; if (btn_r < 2) btn_r = 2;
    int btn_off = btn_r + 1;
    ic_fill_circle(px, stride, x, y, size, size,
                   ab_cx + btn_off, ab_cy, btn_r, btn_a);  /* A: right */
    ic_fill_circle(px, stride, x, y, size, size,
                   ab_cx - btn_off, ab_cy, btn_r, btn_b);  /* B: left  */
}

/* -------------------------------------------------------------------------
 * icon_settings  -- 8-tooth gear
 * -------------------------------------------------------------------------*/
void icon_settings(uint32_t *px, int stride, int x, int y, int size,
                   uint32_t accent)
{
    uint32_t bg    = ic_bg_from_accent(accent);
    uint32_t gear  = accent;
    uint32_t hole  = ic_darken(bg, 60);
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    int cx_ = x + size / 2;
    int cy_ = y + size / 2;

    /* Outer gear radius: fill the gear using a pixel-test approach.
     * For each pixel, compute its distance from centre and angle,
     * then test: inside outer_r AND (inside inner_r OR in a tooth sector). */
    int outer_r = size * 38 / 100;
    int inner_r = size * 24 / 100;
    int tooth_r = size * 44 / 100;  /* tip of tooth */
    int n_teeth = 8;

    for (int py_off = -outer_r - size/8; py_off <= outer_r + size/8; py_off++) {
        for (int px_off = -outer_r - size/8; px_off <= outer_r + size/8; px_off++) {
            int px_ = cx_ + px_off;
            int py_ = cy_ + py_off;
            if (px_ < x || py_ < y || px_ >= x+size || py_ >= y+size) continue;

            int dist2 = px_off*px_off + py_off*py_off;

            /* Inside the hole: bg colour */
            if (dist2 < inner_r * inner_r) {
                ic_put(px, stride, px_, py_, hole);
                continue;
            }
            /* Outside the tooth tips: nothing */
            if (dist2 > tooth_r * tooth_r) continue;

            /* Determine angle in 256ths (atan2 integer approximation) */
            /* We use: sector = (angle mod (256/n_teeth)); tooth present
             * if sector < half-tooth arc */
            /* Integer atan2 approximation (result 0-255) */
            int ax = px_off, ay = -py_off; /* flip Y for standard math coords */
            /* Octant-based atan2 */
            int ang256;
            if (ax == 0 && ay == 0) { ang256 = 0; }
            else {
                int abx = ax < 0 ? -ax : ax;
                int aby = ay < 0 ? -ay : ay;
                int ratio; /* 0-128 = 0..1 for the minor/major axis */
                int major, is_steep;
                if (abx >= aby) { major = abx; ratio = aby * 128 / (abx ? abx : 1); is_steep = 0; }
                else            { major = aby; ratio = abx * 128 / (aby ? aby : 1); is_steep = 1; }
                (void)major;
                /* atan(ratio/128) ≈ ratio * 64 / 128 = ratio/2 (good approx for small angles) */
                /* More accurate: atan ≈ ratio/2 - ratio^3/(6*128^2) */
                int atan_part = ratio / 2; /* 0-64 maps to 0-45 degrees (0-32 in 256 units) */
                if (is_steep) atan_part = 64 - atan_part; /* complement to 90° */
                /* Now map to quadrant */
                if (ax >= 0 && ay >= 0) ang256 = atan_part;              /* Q1 0-64   */
                else if (ax < 0 && ay >= 0) ang256 = 128 - atan_part;    /* Q2 64-128 */
                else if (ax < 0 && ay < 0)  ang256 = 128 + atan_part;    /* Q3 128-192*/
                else                         ang256 = 256 - atan_part;   /* Q4 192-256*/
                ang256 &= 255;
            }

            /* Tooth check: divide full circle into 2*n_teeth sectors,
             * alternating tooth/gap */
            int sector_size = 256 / (2 * n_teeth);
            int sector      = (ang256 / sector_size) & 1;
            /* sector==0 → tooth, sector==1 → gap between teeth */
            int in_body   = (dist2 <= outer_r * outer_r);
            int in_tooth  = (dist2 <= tooth_r * tooth_r) && (sector == 0);

            if (in_body || in_tooth)
                ic_put(px, stride, px_, py_, gear);
        }
    }
}

/* -------------------------------------------------------------------------
 * icon_chart  -- ascending bar chart (3 columns)
 * -------------------------------------------------------------------------*/
void icon_chart(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent)
{
    uint32_t bg    = ic_bg_from_accent(accent);
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Chart area */
    int pad  = size / 8;
    int cax  = x + pad;
    int cay  = y + pad;
    int caw  = size - pad*2;
    int cah  = size - pad*2;

    /* Base line */
    uint32_t axis = IC_ARGB(0xA0, IC_R(accent)+40, IC_G(accent)+40, IC_B(accent)+40);
    if ((unsigned)(IC_R(accent)+40) > 255) axis = IC_ARGB(0xA0, 255, 255, 255);
    ic_fill_rect(px, stride, x, y, size, size,
                 cax, cay + cah - 1, caw, 1, axis);

    /* 3 bars, ascending heights */
    int n_bars = 3;
    int bar_gap = caw / (n_bars * 3 + 1);
    if (bar_gap < 1) bar_gap = 1;
    int bar_w   = bar_gap * 2;
    if (bar_w < 2) bar_w = 2;

    int heights[3] = { cah * 40/100, cah * 65/100, cah * 85/100 };
    uint32_t bar_col[3];
    bar_col[0] = ic_lighten(accent, 60);
    bar_col[1] = ic_lighten(accent, 30);
    bar_col[2] = accent;

    for (int i = 0; i < n_bars; i++) {
        int bx = cax + bar_gap + i * (bar_w + bar_gap);
        int bh = heights[i];
        int by = cay + cah - bh;
        int bcr = bar_w / 4; if (bcr < 1) bcr = 1;
        ic_fill_rrect(px, stride, x, y, size, size,
                      bx, by, bar_w, bh, bcr, bar_col[i]);
    }
}

/* -------------------------------------------------------------------------
 * icon_music  -- eighth note (filled head + stem + flag)
 * -------------------------------------------------------------------------*/
void icon_music(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent)
{
    uint32_t bg   = ic_bg_from_accent(accent);
    uint32_t note = ic_lighten(accent, 80);
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* Note head: ellipse approximated as a slightly squashed circle */
    int cx_   = x + size * 42 / 100;
    int cy_   = y + size * 68 / 100;
    int hr    = size / 9; if (hr < 2) hr = 2;
    int vr    = hr * 7 / 10; if (vr < 1) vr = 1;

    /* Fill ellipse row by row */
    for (int dy = -vr; dy <= vr; dy++) {
        /* x radius at this dy: hr * sqrt(1 - (dy/vr)^2) */
        int xr = hr * vr;
        int fac = vr*vr - dy*dy;
        if (fac < 0) fac = 0;
        /* integer sqrt of fac */
        int sq = vr;
        while (sq * sq > fac) sq--;
        xr = hr * sq / (vr ? vr : 1);
        if (xr < 0) xr = 0;
        ic_fill_rect(px, stride, x, y, size, size,
                     cx_ - xr, cy_ + dy, xr*2, 1, note);
    }

    /* Stem: vertical line up from right side of head */
    int stem_x = cx_ + hr - 1;
    int stem_y_bot = cy_;
    int stem_y_top = y + size * 20 / 100;
    int stem_thick = size / 22; if (stem_thick < 1) stem_thick = 1;
    ic_fill_rect(px, stride, x, y, size, size,
                 stem_x, stem_y_top, stem_thick, stem_y_bot - stem_y_top, note);

    /* Flag: two curves at top of stem (drawn as diagonal lines) */
    int fl_x0 = stem_x + stem_thick;
    int fl_y0 = stem_y_top;
    int fl_x1 = stem_x + size / 5;
    int fl_y1 = stem_y_top + size / 8;
    ic_line(px, stride, x, y, size, size,
            fl_x0, fl_y0, fl_x1, fl_y1, note, stem_thick);
    /* Second flag curl */
    int fl2_y0 = stem_y_top + size / 10;
    int fl2_y1 = fl2_y0 + size / 8;
    ic_line(px, stride, x, y, size, size,
            fl_x0, fl2_y0, fl_x1, fl2_y1, note, stem_thick);
}

/* -------------------------------------------------------------------------
 * icon_calendar  -- monthly grid with coloured header
 * -------------------------------------------------------------------------*/
void icon_calendar(uint32_t *px, int stride, int x, int y, int size,
                   uint32_t accent)
{
    uint32_t bg      = IC_ARGB(0xFF, 0xF8, 0xF8, 0xF8);
    uint32_t header  = accent;
    uint32_t border  = ic_darken(accent, 30);
    uint32_t cell_bg = IC_ARGB(0xFF, 0xFF, 0xFF, 0xFF);
    uint32_t today   = ic_lighten(accent, 50);
    int bg_cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, ic_bg_from_accent(accent), bg_cr);

    /* Calendar body */
    int pad = size / 10;
    int cx_ = x + pad, cy_ = y + pad;
    int cw  = size - pad*2, ch = size - pad*2;
    int bcr = bg_cr / 2; if (bcr < 1) bcr = 1;

    ic_fill_rrect(px, stride, x, y, size, size,
                  cx_, cy_, cw, ch, bcr, bg);

    /* Header strip (top ~25 %) */
    int hh = ch * 25 / 100; if (hh < 4) hh = 4;
    ic_fill_rect(px, stride, x, y, size, size,
                 cx_, cy_, cw, hh, header);
    /* Rounded top corners on header */
    for (int dr = 0; dr < bcr; dr++) {
        /* TL */
        uint32_t *p = px + (cy_ + dr) * stride + (cx_ + dr);
        *p = header;
        /* TR */
        p = px + (cy_ + dr) * stride + (cx_ + cw - 1 - dr);
        *p = header;
    }

    /* Ring hooks (small dark circles at top of header) */
    uint32_t hook = ic_darken(accent, 50);
    int hook_r = size / 22; if (hook_r < 1) hook_r = 1;
    ic_fill_circle(px, stride, x, y, size, size,
                   cx_ + cw / 3, cy_, hook_r, hook);
    ic_fill_circle(px, stride, x, y, size, size,
                   cx_ + cw * 2 / 3, cy_, hook_r, hook);

    /* Day grid: 7 columns × 4 rows */
    int grid_x = cx_ + 1;
    int grid_y = cy_ + hh + 1;
    int grid_w = cw - 2;
    int grid_h = ch - hh - 2;
    int cols = 7, rows = 4;
    int cell_w = grid_w / cols;
    int cell_h = grid_h / rows;
    if (cell_w < 2) cell_w = 2;
    if (cell_h < 2) cell_h = 2;

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            int gx = grid_x + col * cell_w;
            int gy = grid_y + row * cell_h;
            uint32_t cc = cell_bg;
            /* Highlight "today" at row1, col2 */
            if (row == 1 && col == 2) cc = today;
            if (cell_w > 2 && cell_h > 2)
                ic_fill_rect(px, stride, x, y, size, size,
                             gx+1, gy+1, cell_w-1, cell_h-1, cc);
        }
    }

    /* Grid lines */
    (void)border;
    for (int col = 1; col < cols; col++) {
        int lx = grid_x + col * cell_w;
        ic_fill_rect(px, stride, x, y, size, size,
                     lx, grid_y, 1, grid_h,
                     IC_ARGB(0x40, 0x80, 0x80, 0xA0));
    }
    for (int row = 1; row < rows; row++) {
        int ly = grid_y + row * cell_h;
        ic_fill_rect(px, stride, x, y, size, size,
                     grid_x, ly, grid_w, 1,
                     IC_ARGB(0x40, 0x80, 0x80, 0xA0));
    }
}

/* -------------------------------------------------------------------------
 * icon_ide  -- dark tile, small node-graph (connected boxes) + "</>" mark
 * -------------------------------------------------------------------------*/
void icon_ide(uint32_t *px, int stride, int x, int y, int size,
              uint32_t accent)
{
    /* Dark "editor" background, like the terminal tile */
    uint32_t bg     = IC_ARGB(0xFF, 0x1B, 0x1E, 0x2B);
    uint32_t link   = ic_darken(ic_lighten(accent, 40), 10); /* link lines */
    uint32_t node   = ic_lighten(accent, 25);                /* node fill   */
    uint32_t node_e = ic_lighten(accent, 80);                /* node edge   */
    uint32_t bracket= IC_ARGB(0xFF, 0xE6, 0xEC, 0xF5);       /* "</>" glyph  */
    int cr = ic_corner_r(size);

    ic_draw_tile_bg(px, stride, x, y, size, bg, cr);

    /* --- Node-graph: one root node up-left, two children down-right ------ */
    /* Node box geometry, all proportional so it scales 32-64 px cleanly. */
    int nw = size * 22 / 100; if (nw < 6) nw = 6;   /* node width  */
    int nh = size * 15 / 100; if (nh < 4) nh = 4;   /* node height */
    int ncr = nh / 3; if (ncr < 1) ncr = 1;         /* node corner */

    /* Root node (top-left) */
    int r_x = x + size * 14 / 100;
    int r_y = y + size * 20 / 100;
    /* Child A (mid-right) */
    int a_x = x + size * 54 / 100;
    int a_y = y + size * 16 / 100;
    /* Child B (bottom-right) */
    int b_x = x + size * 54 / 100;
    int b_y = y + size * 56 / 100;

    /* Centres of each node (for link endpoints) */
    int r_cx = r_x + nw / 2, r_cy = r_y + nh / 2;
    int a_cx = a_x + nw / 2, a_cy = a_y + nh / 2;
    int b_cx = b_x + nw / 2, b_cy = b_y + nh / 2;

    int lw = size / 24; if (lw < 1) lw = 1;          /* link thickness */

    /* Link lines drawn first so nodes overlap their ends */
    ic_line(px, stride, x, y, size, size,
            r_cx, r_cy, a_cx, a_cy, link, lw);
    ic_line(px, stride, x, y, size, size,
            r_cx, r_cy, b_cx, b_cy, link, lw);

    /* Small connector dots at link endpoints */
    int dot_r = lw; if (dot_r < 1) dot_r = 1;
    ic_fill_circle(px, stride, x, y, size, size, r_cx, r_cy, dot_r, node_e);
    ic_fill_circle(px, stride, x, y, size, size, a_cx, a_cy, dot_r, node_e);
    ic_fill_circle(px, stride, x, y, size, size, b_cx, b_cy, dot_r, node_e);

    /* Node boxes (edge ring then fill for a crisp outline) */
    int ew = 1 + size / 48; /* edge thickness */
    ic_fill_rrect(px, stride, x, y, size, size, r_x-ew, r_y-ew, nw+2*ew, nh+2*ew, ncr+ew, node_e);
    ic_fill_rrect(px, stride, x, y, size, size, a_x-ew, a_y-ew, nw+2*ew, nh+2*ew, ncr+ew, node_e);
    ic_fill_rrect(px, stride, x, y, size, size, b_x-ew, b_y-ew, nw+2*ew, nh+2*ew, ncr+ew, node_e);
    ic_fill_rrect(px, stride, x, y, size, size, r_x, r_y, nw, nh, ncr, node);
    ic_fill_rrect(px, stride, x, y, size, size, a_x, a_y, nw, nh, ncr, node);
    ic_fill_rrect(px, stride, x, y, size, size, b_x, b_y, nw, nh, ncr, node);

    /* --- "</>" code mark, bottom-left, over an empty area ---------------- */
    /* Built from three chevrons drawn with ic_line. */
    int gx = x + size * 16 / 100;     /* glyph left   */
    int gy = y + size * 74 / 100;     /* glyph mid-Y  */
    int gh = size * 12 / 100; if (gh < 3) gh = 3;   /* chevron half-height */
    int gw = size * 9  / 100; if (gw < 3) gw = 3;   /* chevron arm width   */
    int gt = 1 + size / 40;                          /* stroke thickness    */

    /* '<' : opening angle bracket */
    ic_line(px, stride, x, y, size, size, gx+gw, gy-gh, gx, gy, bracket, gt);
    ic_line(px, stride, x, y, size, size, gx, gy, gx+gw, gy+gh, bracket, gt);
    /* '>' : closing angle bracket */
    int hx = gx + gw + size * 16 / 100;
    ic_line(px, stride, x, y, size, size, hx, gy-gh, hx+gw, gy, bracket, gt);
    ic_line(px, stride, x, y, size, size, hx+gw, gy, hx, gy+gh, bracket, gt);
    /* '/' : forward slash between the brackets */
    int sx = gx + gw + size * 5 / 100;
    ic_line(px, stride, x, y, size, size, sx+gw/2, gy-gh, sx-gw/2, gy+gh, accent, gt);
}

/* =========================================================================
 * Drop shadow
 * ========================================================================= */
void icon_draw_shadow(uint32_t *px, int stride, int x, int y, int size,
                      int spread, uint8_t opacity)
{
    if (!px || spread <= 0) return;
    /* Paint increasingly transparent black squares offset by (spread/2, spread/2) */
    int off = spread / 2 + 1;
    for (int layer = spread; layer >= 1; layer--) {
        uint8_t a = (uint8_t)((unsigned)opacity * (unsigned)layer / (unsigned)spread);
        uint32_t sc = IC_ARGB(a, 0, 0, 0);
        /* We paint directly without clip-to-icon so the shadow bleeds outside */
        int sx = x + off - layer;
        int sy = y + off - layer;
        int sw = size + (layer - 1) * 2;
        int sh = size + (layer - 1) * 2;
        for (int row = 0; row < sh; row++) {
            int py_ = sy + row;
            if (py_ < 0) continue;
            for (int col_ = 0; col_ < sw; col_++) {
                int px__ = sx + col_;
                if (px__ < 0) continue;
                uint32_t *p = px + py_ * stride + px__;
                *p = ic_blend(sc, *p);
            }
        }
    }
}

/* =========================================================================
 * Accent colour palette
 * ========================================================================= */

uint32_t icon_accent_for_name(const char *name)
{
    /* 16 pleasant, visually distinct, fully-opaque accent colours */
    static const uint32_t palette[16] = {
        0xFF4A90D9,  /* sky blue        */
        0xFF7B68EE,  /* medium slate    */
        0xFF50C878,  /* emerald green   */
        0xFFFF6B6B,  /* coral red       */
        0xFFFFAA00,  /* amber           */
        0xFF00BCD4,  /* cyan            */
        0xFFE91E8C,  /* hot pink        */
        0xFF9C27B0,  /* purple          */
        0xFF4CAF50,  /* material green  */
        0xFFFF5722,  /* deep orange     */
        0xFF2196F3,  /* material blue   */
        0xFF009688,  /* teal            */
        0xFF8BC34A,  /* light green     */
        0xFFF44336,  /* material red    */
        0xFF3F51B5,  /* indigo          */
        0xFF795548,  /* brown           */
    };
    uint32_t h = ic_hash(name);
    return palette[h & 15];
}

/* =========================================================================
 * icon_for_app  dispatch
 * ========================================================================= */

/* Emit 1-2 uppercase initials from app_name into buf[3]. */
static void ic_initials(const char *name, char buf[3])
{
    buf[0] = buf[1] = buf[2] = 0;
    if (!name || !name[0]) { buf[0] = '?'; return; }
    /* First char */
    char c0 = name[0];
    if (c0 >= 'a' && c0 <= 'z') c0 -= 32;
    buf[0] = c0;
    /* Second: first char after a space or underscore, or second char */
    const char *p = name + 1;
    while (*p && *p != ' ' && *p != '_' && *p != '-') p++;
    if (*p) {
        p++;
        char c1 = *p;
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        buf[1] = c1;
    } else {
        char c1 = name[1];
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        buf[1] = c1 ? c1 : 0;
    }
}

/* Draw a Material-style app icon: accent rounded tile + white silhouette blit
 * (the silhouette is a real Google Material Design icon embedded in
 * icon_assets.h). The asset is nearest-neighbour scaled to ~64% and centered. */
static void icon_draw_material(uint32_t *px, int stride, int x, int y, int size,
                               uint32_t accent, const uint32_t *asset)
{
    icon_rounded_tile(px, stride, x, y, size, accent, 0xFFFFFFFFu, 0);
    int g = (size * 64) / 100;
    if (g < 8) g = (size > 8) ? 8 : size;
    int ox = x + (size - g) / 2;
    int oy = y + (size - g) / 2;
    for (int dy = 0; dy < g; dy++) {
        int sy = dy * ICON_ASSET_SZ / g;
        if (sy >= ICON_ASSET_SZ) sy = ICON_ASSET_SZ - 1;
        for (int dx = 0; dx < g; dx++) {
            int sx = dx * ICON_ASSET_SZ / g;
            if (sx >= ICON_ASSET_SZ) sx = ICON_ASSET_SZ - 1;
            uint32_t s = asset[sy * ICON_ASSET_SZ + sx];
            if (s >> 24) ic_put(px, stride, ox + dx, oy + dy, s);
        }
    }
}

/* Map an app name to an embedded Material asset (or NULL for procedural). */
static const uint32_t *icon_asset_for(const char *name)
{
    static const struct { const char *sub; const char *key; } map[] = {
        {"terminal","terminal"}, {"term","terminal"}, {"shell","terminal"},
        {"filemanager","files"}, {"files","files"},   {"file","files"},
        {"editor","editor"},     {"edit","editor"},
        {"notes","notes"},       {"note","notes"},
        {"calculator","calculator"}, {"calc","calculator"},
        {"clock","clock"},
        {"paint","paint"},
        {"snake","games"}, {"tetris","games"}, {"game2048","games"},
        {"2048","games"},  {"bubbletd","games"}, {"bubble","games"},
        {"zombietd","games"}, {"zombie","games"}, {"ztd","games"},
        {"game","games"},
        {"settings","settings"}, {"setting","settings"},
        {"dateapp","calendar"}, {"date","calendar"}, {"calendar","calendar"},
        {"ide","ide"}, {"code","ide"}, {"blueprint","ide"},
        {"sysmon","sysmon"}, {"taskman","sysmon"},
        {"synth","music"}, {"musicplayer","music"}, {"music","music"},
        {"soundtest","music"},
        {0,0}
    };
    if (!name) return 0;
    for (int i = 0; map[i].sub; i++) {
        if (ic_starts_with(name, map[i].sub)) {
            for (int j = 0; ICON_ASSETS[j].key; j++) {
                const char *a = ICON_ASSETS[j].key, *b = map[i].key;
                int k = 0;
                while (a[k] && a[k] == b[k]) k++;
                if (a[k] == 0 && b[k] == 0) return ICON_ASSETS[j].px;
            }
        }
    }
    return 0;
}

void icon_for_app(uint32_t *px, int stride, int x, int y, int size,
                  const char *app_name)
{
    if (!px || !app_name || size <= 0) return;

    uint32_t accent = icon_accent_for_name(app_name);

    /* Prefer a real Google Material icon when we have one for this app. */
    {
        const uint32_t *mat = icon_asset_for(app_name);
        if (mat) { icon_draw_material(px, stride, x, y, size, accent, mat); return; }
    }

    /* Terminal */
    if (ic_starts_with(app_name, "terminal") ||
        ic_starts_with(app_name, "term") ||
        ic_starts_with(app_name, "shell") ||
        ic_starts_with(app_name, "bash") ||
        ic_starts_with(app_name, "zsh") ||
        ic_starts_with(app_name, "sh"))
    {
        icon_terminal(px, stride, x, y, size, accent);
        return;
    }
    /* IDE / code map / blueprint -- check before "files"/"editor" so that
     * dev-tool names win even if they share a prefix region. */
    if (ic_starts_with(app_name, "ide") ||
        ic_starts_with(app_name, "code") ||
        ic_starts_with(app_name, "blueprint") ||
        ic_starts_with(app_name, "devtool") ||
        ic_starts_with(app_name, "vscode"))
    {
        icon_ide(px, stride, x, y, size, accent);
        return;
    }
    /* Folder / file manager */
    if (ic_starts_with(app_name, "filemanager") ||
        ic_starts_with(app_name, "folder") ||
        ic_starts_with(app_name, "files") ||
        ic_starts_with(app_name, "file") ||
        ic_starts_with(app_name, "fm") ||
        ic_starts_with(app_name, "explorer") ||
        ic_starts_with(app_name, "nautilus") ||
        ic_starts_with(app_name, "dolphin") ||
        ic_starts_with(app_name, "thunar"))
    {
        icon_folder(px, stride, x, y, size, accent);
        return;
    }
    /* Text editor / notes */
    if (ic_starts_with(app_name, "text") ||
        ic_starts_with(app_name, "editor") ||
        ic_starts_with(app_name, "notes") ||
        ic_starts_with(app_name, "note") ||
        ic_starts_with(app_name, "nano") ||
        ic_starts_with(app_name, "vim") ||
        ic_starts_with(app_name, "emacs") ||
        ic_starts_with(app_name, "gedit") ||
        ic_starts_with(app_name, "notepad") ||
        ic_starts_with(app_name, "kate"))
    {
        icon_file_text(px, stride, x, y, size, accent);
        return;
    }
    /* Paint / drawing */
    if (ic_starts_with(app_name, "paint") ||
        ic_starts_with(app_name, "draw") ||
        ic_starts_with(app_name, "gimp") ||
        ic_starts_with(app_name, "krita") ||
        ic_starts_with(app_name, "inkscape") ||
        ic_starts_with(app_name, "brush"))
    {
        icon_paint(px, stride, x, y, size, accent);
        return;
    }
    /* Calculator */
    if (ic_starts_with(app_name, "calc") ||
        ic_starts_with(app_name, "calculator") ||
        ic_starts_with(app_name, "bc") ||
        ic_starts_with(app_name, "math"))
    {
        icon_calc(px, stride, x, y, size, accent);
        return;
    }
    /* Clock */
    if (ic_starts_with(app_name, "clock") ||
        ic_starts_with(app_name, "time") ||
        ic_starts_with(app_name, "watch") ||
        ic_starts_with(app_name, "alarm") ||
        ic_starts_with(app_name, "timer"))
    {
        icon_clock(px, stride, x, y, size, accent);
        return;
    }
    /* Games */
    if (ic_starts_with(app_name, "game") ||
        ic_starts_with(app_name, "games") ||
        ic_starts_with(app_name, "game2048") ||
        ic_starts_with(app_name, "2048") ||
        ic_starts_with(app_name, "snake") ||
        ic_starts_with(app_name, "tetris") ||
        ic_starts_with(app_name, "play") ||
        ic_starts_with(app_name, "steam") ||
        ic_starts_with(app_name, "joystick"))
    {
        icon_game(px, stride, x, y, size, accent);
        return;
    }
    /* Settings */
    if (ic_starts_with(app_name, "settings") ||
        ic_starts_with(app_name, "prefs") ||
        ic_starts_with(app_name, "config") ||
        ic_starts_with(app_name, "system") ||
        ic_starts_with(app_name, "control"))
    {
        icon_settings(px, stride, x, y, size, accent);
        return;
    }
    /* Chart / stats / system monitor */
    if (ic_starts_with(app_name, "chart") ||
        ic_starts_with(app_name, "graph") ||
        ic_starts_with(app_name, "plot") ||
        ic_starts_with(app_name, "stats") ||
        ic_starts_with(app_name, "sysmon") ||
        ic_starts_with(app_name, "taskman") ||
        ic_starts_with(app_name, "monitor"))
    {
        icon_chart(px, stride, x, y, size, accent);
        return;
    }
    /* Music / synth / sound */
    if (ic_starts_with(app_name, "musicplayer") ||
        ic_starts_with(app_name, "music") ||
        ic_starts_with(app_name, "synth") ||
        ic_starts_with(app_name, "soundtest") ||
        ic_starts_with(app_name, "audio") ||
        ic_starts_with(app_name, "sound") ||
        ic_starts_with(app_name, "mpd") ||
        ic_starts_with(app_name, "rhythmbox") ||
        ic_starts_with(app_name, "spotify"))
    {
        icon_music(px, stride, x, y, size, accent);
        return;
    }
    /* Calendar / date app */
    if (ic_starts_with(app_name, "calendar") ||
        ic_starts_with(app_name, "dateapp") ||
        ic_starts_with(app_name, "date") ||
        ic_starts_with(app_name, "cal") ||
        ic_starts_with(app_name, "dates") ||
        ic_starts_with(app_name, "schedule"))
    {
        icon_calendar(px, stride, x, y, size, accent);
        return;
    }

    /* Fallback: generic rounded tile with initials */
    char init[3];
    ic_initials(app_name, init);
    uint32_t fg = IC_ARGB(0xFF, 0xFF, 0xFF, 0xFF); /* white text */
    icon_rounded_tile(px, stride, x, y, size, accent, fg, init);
}
