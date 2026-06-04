/*
 * ide_gfx.c -- clipped drawing primitives. Freestanding; text via bitfont.
 */
#include "ide_gfx.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/font2/font2.h"

/* Runtime UI scale (see ide_gfx.h). Default 120% -> 9x19 cell: readable but
 * compact so the tab strips and side panels all fit without crowding (138% felt
 * "very big" and scrunched the chrome). The user can Ctrl+wheel / Ctrl+=/- to
 * rescale live; the whole layout reflows from this cell. */
int g_ui_pct = 120, g_gfx_fw = 9, g_gfx_fh = 19;

void gfx_set_scale(int pct) {
    if (pct < 100) pct = 100;
    if (pct > 250) pct = 250;
    g_ui_pct = pct;
    g_gfx_fw = 8  * pct / 100; if (g_gfx_fw < 8)  g_gfx_fw = 8;  if (g_gfx_fw > 20) g_gfx_fw = 20;
    g_gfx_fh = 16 * pct / 100; if (g_gfx_fh < 16) g_gfx_fh = 16; if (g_gfx_fh > 40) g_gfx_fh = 40;
}

#define A(c) (((c) >> 24) & 0xFFu)
#define R(c) (((c) >> 16) & 0xFFu)
#define G(c) (((c) >>  8) & 0xFFu)
#define B(c) ( (c)        & 0xFFu)

static inline void put(Canvas* c, int x, int y, uint32_t col) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->px[y * c->stride + x] = col;
}
static inline void blendpx(Canvas* c, int x, int y, uint32_t argb) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    unsigned a = A(argb);
    if (a == 0) return;
    uint32_t* d = &c->px[y * c->stride + x];
    if (a == 255) { *d = argb | 0xFF000000u; return; }
    uint32_t dst = *d;
    unsigned ia = 255u - a;
    unsigned r = (R(argb) * a + R(dst) * ia) / 255u;
    unsigned g = (G(argb) * a + G(dst) * ia) / 255u;
    unsigned b = (B(argb) * a + B(dst) * ia) / 255u;
    *d = 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void clip_rect(Canvas* c, int* x, int* y, int* w, int* h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > c->w) *w = c->w - *x;
    if (*y + *h > c->h) *h = c->h - *y;
}

void gfx_fill(Canvas* c, int x, int y, int w, int h, uint32_t col) {
    clip_rect(c, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    col |= 0xFF000000u;
    for (int j = 0; j < h; j++) {
        uint32_t* row = &c->px[(y + j) * c->stride + x];
        for (int i = 0; i < w; i++) row[i] = col;
    }
}

void gfx_blend(Canvas* c, int x, int y, int w, int h, uint32_t argb) {
    clip_rect(c, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) blendpx(c, x + i, y + j, argb);
}

void gfx_round(Canvas* c, int x, int y, int w, int h, int r, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    col |= 0xFF000000u;
    for (int j = 0; j < h; j++) {
        int dy = (j < r) ? (r - 1 - j) : (j >= h - r ? j - (h - r) : -1);
        int inset = 0;
        if (dy >= 0) {
            /* circle corner: inset where outside radius */
            int rr = r * r;
            while (inset < r) {
                int dx = r - 1 - inset;
                if (dx * dx + dy * dy <= rr) break;
                inset++;
            }
        }
        int cy = y + j, cx = x + inset, cw = w - 2 * inset;
        if (cy < 0 || cy >= c->h) continue;
        if (cx < 0) { cw += cx; cx = 0; }
        if (cx + cw > c->w) cw = c->w - cx;
        for (int i = 0; i < cw; i++) c->px[cy * c->stride + cx + i] = col;
    }
}

void gfx_hline(Canvas* c, int x, int y, int len, uint32_t col) {
    if (y < 0 || y >= c->h) return;
    if (x < 0) { len += x; x = 0; }
    if (x + len > c->w) len = c->w - x;
    col |= 0xFF000000u;
    for (int i = 0; i < len; i++) c->px[y * c->stride + x + i] = col;
}
void gfx_vline(Canvas* c, int x, int y, int len, uint32_t col) {
    if (x < 0 || x >= c->w) return;
    if (y < 0) { len += y; y = 0; }
    if (y + len > c->h) len = c->h - y;
    col |= 0xFF000000u;
    for (int i = 0; i < len; i++) c->px[(y + i) * c->stride + x] = col;
}

void gfx_stroke(Canvas* c, int x, int y, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    gfx_hline(c, x, y, w, col);
    gfx_hline(c, x, y + h - 1, w, col);
    gfx_vline(c, x, y, h, col);
    gfx_vline(c, x + w - 1, y, h, col);
}

void gfx_line(Canvas* c, int x0, int y0, int x1, int y1, uint32_t col) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2, e2;
    col |= 0xFF000000u;
    for (;;) {
        put(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}

void gfx_dashed(Canvas* c, int x0, int y0, int x1, int y1, uint32_t col, int dash) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2, e2;
    int n = 0;
    if (dash <= 0) dash = 4;
    col |= 0xFF000000u;
    for (;;) {
        if ((n / dash) % 2 == 0) put(c, x0, y0, col);
        n++;
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}

void gfx_text(Canvas* c, int x, int y, const char* s, uint32_t col) {
    /* Fractional scaled text at the runtime cell size, bounds-clipped to canvas. */
    font2_draw_cell_clip((unsigned int*)c->px, c->stride, c->w, c->h,
                         0, c->w, x, y, s, g_gfx_fw, g_gfx_fh, col | 0xFF000000u);
}

void gfx_text_n(Canvas* c, int x, int y, const char* s, int n, uint32_t col) {
    char buf[256];
    int i = 0;
    if (n > 255) n = 255;
    for (; i < n && s[i]; i++) buf[i] = s[i];
    buf[i] = 0;
    gfx_text(c, x, y, buf, col);
}

void gfx_text_clip(Canvas* c, int x, int y, const char* s, uint32_t col,
                   int clip_x, int clip_w) {
    /* Scaled text horizontally clipped to [clip_x, clip_x+clip_w) and bounded to
     * the canvas. font2_draw_cell_clip handles per-pixel clipping internally. */
    font2_draw_cell_clip((unsigned int*)c->px, c->stride, c->w, c->h,
                         clip_x, clip_x + clip_w, x, y, s,
                         g_gfx_fw, g_gfx_fh, col | 0xFF000000u);
}

int gfx_textw(const char* s) { int n = 0; if (s) while (s[n]) n++; return n * GFX_FW; }
