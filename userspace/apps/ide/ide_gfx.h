/*
 * ide_gfx.h -- clipped 2D drawing primitives over an ARGB32 canvas.
 * All coordinates are absolute within the canvas; every call clips to [0,w)x[0,h).
 */
#ifndef IDE_GFX_H
#define IDE_GFX_H

#include <stdint.h>

/* UI text scale: the base bitmap font is 8x16; at IDE_UI_SCALE=2 the whole IDE
 * renders crisp 16x32 text (the desktop felt "too small"). GFX_FW/GFX_FH are the
 * single source of truth the layout math is built on, so bumping them here scales
 * the entire IDE coherently; the fixed pixel constants in ide_theme.h are grown
 * proportionally to match. Text is drawn via font2_draw_scaled_clip (bounds-safe). */
#define IDE_UI_SCALE 2
#define GFX_FW (8  * IDE_UI_SCALE)   /* font cell width                 */
#define GFX_FH (16 * IDE_UI_SCALE)   /* font cell height                */

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
