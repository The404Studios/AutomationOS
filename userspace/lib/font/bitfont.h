/**
 * bitfont.h - Self-contained 8x16 monospace bitmap font renderer
 *
 * Draws into ARGB32 pixel buffers (0x00RRGGBB layout).
 * Pure C, freestanding-safe: no libc, no libm, no syscalls, no file I/O.
 * Compile with -ffreestanding -nostdlib -fno-builtin.
 *
 * Consumer usage:
 *   #include "userspace/lib/font/bitfont.h"
 *   Link: gcc ... bitfont.o your_code.o -o output
 *
 * The font is the IBM VGA 8x16 (CP437) bitmap, public domain.
 * Each glyph is 16 bytes; one byte per row; MSB = leftmost pixel.
 * Glyphs are stored for ASCII 0x20 (' ') through 0x7E ('~').
 * Characters outside that range are rendered as a hollow box.
 */

#ifndef BITFONT_H
#define BITFONT_H

/* Font metrics */
#define FONT_W  8   /* glyph width in pixels  */
#define FONT_H  16  /* glyph height in pixels */

/**
 * font_draw_char - Draw a single character at pixel position (x, y).
 *
 * @buf    ARGB32 pixel buffer (row-major, no padding assumed beyond stride).
 * @stride Row stride in PIXELS (not bytes).
 * @bw     Buffer width  in pixels (used for clipping).
 * @bh     Buffer height in pixels (used for clipping).
 * @x      Left edge of glyph cell (may be negative; clipped).
 * @y      Top  edge of glyph cell (may be negative; clipped).
 * @c      Character to draw.
 * @color  Foreground color in ARGB32 (e.g. 0xFFFFFFFF = opaque white).
 *
 * Only pixels where the glyph bit is 1 are written; background is
 * unchanged, so text overlays cleanly onto any existing content.
 */
void font_draw_char(unsigned int *buf, int stride, int bw, int bh,
                    int x, int y, char c, unsigned int color);

/**
 * font_draw_string - Draw a NUL-terminated ASCII string left-to-right.
 *
 * Returns the total pixel advance (number of pixels moved in X).
 * Newlines and other control characters are skipped (not rendered).
 */
int font_draw_string(unsigned int *buf, int stride, int bw, int bh,
                     int x, int y, const char *s, unsigned int color);

/**
 * font_text_width - Return the pixel width of a NUL-terminated string.
 *
 * Equal to strlen(s) * FONT_W.  Does not consider clipping.
 */
int font_text_width(const char *s);

/**
 * bitfont_selftest - Verify glyph orientation is MSB-leftmost (correct).
 *
 * Checks known rows of '(' and ')' (IBM VGA 8x16 reference) against the
 * expected MSB-first column positions so an accidental reversion to LSB
 * ordering is caught immediately.
 *
 * Returns 0 on pass, a non-zero check-index on failure.
 */
int bitfont_selftest(void);

#endif /* BITFONT_H */
