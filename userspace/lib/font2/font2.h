/**
 * font2.h - Scaled + lightly anti-aliased text rendering library
 *
 * Builds on top of bitfont (userspace/lib/font/bitfont.h).
 * Pure C, freestanding-safe: no libc, no libm, no syscalls.
 * Compile with -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *              -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2
 *
 * All pixel buffers are ARGB32 (unsigned int, 0xAARRGGBB).
 * stride is in PIXELS (if your compositor gives stride in bytes, divide by 4).
 *
 * Glyph metrics: base cell is FONT_W=8 wide, FONT_H=16 tall.
 * At integer scale N the cell becomes (8*N) wide and (16*N) tall.
 */

#ifndef FONT2_H
#define FONT2_H

/* --------------------------------------------------------------------------
 * Dependency: bitfont public types/metrics only.  We never call bitfont at
 * runtime here; font2.c uses its own inline glyph accessor.
 * -------------------------------------------------------------------------- */
#define FONT_W   8
#define FONT_H  16

/* --------------------------------------------------------------------------
 * font2_text_width  - pixel width of NUL-terminated string at given scale.
 * font2_text_height - pixel height of a single line at given scale.
 *
 * scale is clamped to [1, 8] internally.
 * -------------------------------------------------------------------------- */
int font2_text_width(const char *str, int scale);
int font2_text_height(int scale);

/* --------------------------------------------------------------------------
 * font2_draw_scaled
 *
 * Draw NUL-terminated ASCII string at integer scale (2, 3, 4, …).
 * Each glyph pixel is replicated into an N×N block for large crisp headings.
 * Background pixels are left untouched (text overlays existing content).
 *
 * @px     ARGB32 pixel buffer (row-major).
 * @stride Row stride in PIXELS.
 * @x, @y  Top-left origin of the first character cell.
 * @str    NUL-terminated ASCII string.
 * @scale  Integer scale factor (1 = normal, 2 = 2x, 3 = 3x, 4 = 4x, …).
 * @argb   Foreground colour in ARGB32.
 * -------------------------------------------------------------------------- */
void font2_draw_scaled(unsigned int *px, int stride,
                       int x, int y,
                       const char *str, int scale,
                       unsigned int argb);

/* --------------------------------------------------------------------------
 * font2_draw_scaled_clip
 *
 * Like font2_draw_scaled, but BOUNDS-SAFE: every pixel is written only if it
 * lies inside the destination buffer [0,maxw) x [0,maxh) AND inside the
 * horizontal clip window [clip_x0, clip_x1). This is the variant UI code MUST
 * use — font2_draw_scaled() has no bounds checking and will corrupt memory if
 * text reaches a buffer edge. Pass clip_x0=0, clip_x1=maxw for "no horizontal
 * clip" (still bounded by the buffer).
 *
 * @maxw,@maxh  destination buffer width/height in pixels (hard bounds).
 * @clip_x0,@clip_x1  horizontal clip window in pixels [x0, x1).
 * -------------------------------------------------------------------------- */
void font2_draw_scaled_clip(unsigned int *px, int stride, int maxw, int maxh,
                            int clip_x0, int clip_x1,
                            int x, int y,
                            const char *str, int scale,
                            unsigned int argb);

/* --------------------------------------------------------------------------
 * font2_draw_cell_clip — FRACTIONAL scaled text (nearest-neighbor) into an
 * arbitrary glyph cell (cell_w x cell_h), bounds-safe + horizontally clipped.
 *
 * Unlike font2_draw_scaled (integer replication only), this maps each output
 * pixel back to the 8x16 source by nearest-neighbor, so it can render any
 * intermediate size — e.g. 12x24 (1.5x) or 10x20 (1.25x) — to land between the
 * "too small" 8x16 and the "too big" 16x32. Same bounds/clip guarantees as
 * font2_draw_scaled_clip. cell advance == cell_w (matches font2_text_width when
 * width is computed as nchars*cell_w).
 * -------------------------------------------------------------------------- */
void font2_draw_cell_clip(unsigned int *px, int stride, int maxw, int maxh,
                          int clip_x0, int clip_x1,
                          int x, int y,
                          const char *str, int cell_w, int cell_h,
                          unsigned int argb);

/* --------------------------------------------------------------------------
 * font2_draw_aa
 *
 * Lightly anti-aliased 1× render via 2×2 supersampling.
 *
 * The glyph is sampled at 2× resolution (16×32 grid); each 2×2 block of
 * super-pixels is averaged to produce a coverage value in {0, 64, 128, 192,
 * 255}.  That coverage is used to linearly blend argb against bg (integer
 * arithmetic only), producing smooth-looking edges on bitmap glyphs.
 *
 * Always renders at 1× size (FONT_W × FONT_H per character).
 *
 * @px     ARGB32 pixel buffer.
 * @stride Row stride in PIXELS.
 * @x, @y  Top-left origin.
 * @str    NUL-terminated ASCII string.
 * @argb   Foreground colour (ARGB32).
 * @bg     Background colour (ARGB32) — used only for edge blending.
 * -------------------------------------------------------------------------- */
void font2_draw_aa(unsigned int *px, int stride,
                   int x, int y,
                   const char *str,
                   unsigned int argb, unsigned int bg);

/* --------------------------------------------------------------------------
 * font2_draw_shadow
 *
 * Draw text with a 1-pixel drop-shadow for perceived depth.
 * Renders the shadow first (offset +1,+1) then the foreground text on top.
 * Works at any integer scale.
 *
 * @shadow Shadow colour (often 0x80000000 = semi-transparent black).
 * -------------------------------------------------------------------------- */
void font2_draw_shadow(unsigned int *px, int stride,
                       int x, int y,
                       const char *str, int scale,
                       unsigned int fg, unsigned int shadow);

/* --------------------------------------------------------------------------
 * font2_draw_gradient
 *
 * Draw scaled text with a vertical colour gradient across the glyph height.
 * Row 0 of each glyph uses top_argb; the bottom row uses bottom_argb.
 * Intermediate rows are linearly interpolated per-channel (integer maths).
 *
 * @top_argb    Colour at top of each character cell.
 * @bottom_argb Colour at bottom of each character cell.
 * -------------------------------------------------------------------------- */
void font2_draw_gradient(unsigned int *px, int stride,
                         int x, int y,
                         const char *str, int scale,
                         unsigned int top_argb, unsigned int bottom_argb);

#endif /* FONT2_H */
