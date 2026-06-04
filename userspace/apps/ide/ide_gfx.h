/*
 * ide_gfx.h -- clipped 2D drawing primitives over an ARGB32 canvas.
 * All coordinates are absolute within the canvas; every call clips to [0,w)x[0,h).
 */
#ifndef IDE_GFX_H
#define IDE_GFX_H

#include <stdint.h>

/* RUNTIME UI text scale. The base bitmap font is 8x16; the glyph cell is
 * (8,16) * g_ui_pct/100, rendered via font2_draw_cell_clip (fractional, bounds-
 * safe). Making these RUNTIME vars (not #defines) lets the user zoom the whole
 * IDE live with Ctrl+mouse-wheel / Ctrl +/-/0 -- the layout reflows because every
 * panel derives its geometry from GFX_FW/GFX_FH each frame. The macro aliases keep
 * all ~150 existing call sites unchanged. Default 138% => an 11x22 cell (readable
 * on a 1024x768 panel, between the too-small 8x16 and the too-big 16x32). */
extern int g_ui_pct;     /* zoom percent of the 8x16 base, 100..250          */
extern int g_gfx_fw;     /* current cell width  (8..20)                      */
extern int g_gfx_fh;     /* current cell height (16..40)                     */
void gfx_set_scale(int pct);   /* clamp pct to [100,250], recompute the cell  */
#define GFX_FW g_gfx_fw
#define GFX_FH g_gfx_fh

typedef struct {
    uint32_t* px;    /* ARGB32 pixels        */
    int       stride;/* row stride in PIXELS */
    int       w, h;  /* canvas size in px    */
} Canvas;

void gfx_fill   (Canvas* c, int x, int y, int w, int h, uint32_t col);          /* opaque */
void gfx_blend  (Canvas* c, int x, int y, int w, int h, uint32_t argb);         /* alpha  */
void gfx_round  (Canvas* c, int x, int y, int w, int h, int r, uint32_t col);   /* rounded*/
void gfx_stroke (Canvas* c, int x, int y, int w, int h, uint32_t col);          /* 1px box*/
void gfx_hline  (Canvas* c, int x, int y, int len, uint32_t col);
void gfx_vline  (Canvas* c, int x, int y, int len, uint32_t col);
void gfx_line   (Canvas* c, int x0, int y0, int x1, int y1, uint32_t col);      /* Bresenham */
void gfx_dashed (Canvas* c, int x0, int y0, int x1, int y1, uint32_t col, int dash);

/* Text via the 8x16 bitfont. gfx_text_n draws at most n chars. */
void gfx_text   (Canvas* c, int x, int y, const char* s, uint32_t col);
void gfx_text_n (Canvas* c, int x, int y, const char* s, int n, uint32_t col);
/* Draw text clipped to x in [clip_x, clip_x+clip_w). */
void gfx_text_clip(Canvas* c, int x, int y, const char* s, uint32_t col,
                   int clip_x, int clip_w);
int  gfx_textw  (const char* s);   /* = strlen(s)*GFX_FW */

#endif /* IDE_GFX_H */
