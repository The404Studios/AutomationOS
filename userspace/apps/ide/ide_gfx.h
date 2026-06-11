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
extern int g_ui_pct;     /* zoom percent of the 8x16 base, 50..250           */
extern int g_gfx_fw;     /* current cell width  (4..20)                      */
extern int g_gfx_fh;     /* current cell height (8..40)                      */
void gfx_set_scale(int pct);   /* clamp pct to [50,250], recompute the cell   */
#define GFX_FW g_gfx_fw
#define GFX_FH g_gfx_fh

/* IDE behaviour knobs (Settings panel). Runtime vars so changes apply live;
 * defined in ide_gfx.c, edited via ide_inspector.c, persisted by ide_config.c. */
extern int g_tab_width;    /* soft-tab / tab-stop width (>=1)                   */
extern int g_blink_ms;     /* caret blink half-period (ms)                      */
extern int g_ac_visible;   /* max autocomplete rows shown (<= AC_MAX_MATCHES)   */
extern int g_ac_minpfx;    /* min typed prefix before the popup opens           */
extern int g_map_pan_step; /* LEGO map keyboard pan step (px)                   */
extern int g_autocomplete; /* 1 = autocomplete enabled                          */
extern int g_anno_gutter;  /* 1 = code-view semantic annotation gutter          */
extern int g_line_numbers; /* 1 = line-number gutter visible                    */
extern int g_auto_indent;  /* 1 = auto-indent new lines                         */
extern int g_live_reparse; /* 1 = re-parse model on every edit (experimental)   */
extern int g_theme_mode;   /* 0 = dark (default); persisted, themed later        */

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
