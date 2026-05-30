/*
 * ttf.h -- Anti-aliased, scalable TrueType text rendering (freestanding, ring 3).
 * =============================================================================
 *
 * A small standalone module that wraps stb_truetype (vendored as
 * stb_truetype.h in this directory) to render anti-aliased, arbitrarily
 * scalable text directly onto an ARGB32 pixel buffer (e.g. a wl_window's
 * shared framebuffer).
 *
 * It is de-POSIX'd: there is NO fopen / stdio. A .ttf font is brought into
 * the address space with SYS_MAP_FILE (zero-copy mapping of an initrd file)
 * via the libc map_file() wrapper, then handed to stbtt_InitFont.
 *
 * Dependencies (link these objects / archives):
 *   - libc:  malloc/free (stdlib.c), memcpy/memset/strlen (string.c),
 *            map_file/write (syscall.c)
 *   - libm:  floor/ceil/sqrt/pow/fmod/cos/acos/fabs (math.c)
 *   stb_truetype's STBTT_* hooks are mapped to the above in ttf.c, so no
 *   host <math.h>/<stdlib.h>/<string.h> are pulled in.
 *
 * Usage sketch:
 *   if (ttf_load("/fonts/DejaVuSans.ttf") != 0) { ...fatal... }
 *   ttf_draw_text(win->pixels, win->stride / 4, win->w, win->h,
 *                 20, 40, "AutomationOS 0123", 18, 0xFFFFFFFFu);
 *
 * The module keeps ONE active font (a global) plus a glyph coverage cache
 * keyed by (codepoint, pixel-size). That's sufficient to prove AA + scaling
 * for the desktop; the integrator can extend to multiple faces later.
 */

#ifndef FONT_TTF_H
#define FONT_TTF_H

typedef unsigned int  ttf_u32;
typedef int           ttf_i32;

/*
 * Map a .ttf from the initrd by VFS path (e.g. "/fonts/DejaVuSans.ttf") using
 * SYS_MAP_FILE and initialise the font for subsequent ttf_draw_text() calls.
 *
 * Returns:
 *    0  on success
 *   <0  on failure:
 *       -1  map_file() failed (file missing / empty / not in initrd)
 *       -2  stbtt_InitFont() rejected the data (not a usable TrueType face)
 *
 * Re-calling ttf_load() replaces the active font and flushes the glyph cache.
 */
int ttf_load(const char *initrd_path);

/* Returns 1 if a font is currently loaded and usable, else 0. */
int ttf_is_loaded(void);

/*
 * Rasterize `str` (NUL-terminated, ASCII / Latin-1 code points) at `px_size`
 * pixels of cap-to-baseline scale and alpha-blend the anti-aliased grayscale
 * coverage onto an ARGB32 buffer.
 *
 *   buf        ARGB32 destination (0xAARRGGBB per pixel)
 *   stride_px  pixels per row (== bytes-per-row / 4)
 *   bw, bh     buffer width / height in pixels (used for clipping)
 *   x, y       pen origin: x = left edge of first glyph,
 *              y = the text BASELINE (glyphs ascend above it)
 *   str        text to draw
 *   px_size    target glyph height in pixels (scalable; 12, 18, 32, ...)
 *   color      0xAARRGGBB; the glyph coverage modulates this color's alpha,
 *              so the RGB is the ink color and coverage gives the soft edges
 *
 * Glyphs advance by their scaled horizontal advance; basic kerning between
 * adjacent code points is applied when the font provides it.
 */
void ttf_draw_text(ttf_u32 *buf, int stride_px, int bw, int bh,
                   int x, int y, const char *str,
                   int px_size, ttf_u32 color);

/*
 * Measure the pixel width that ttf_draw_text() would advance for `str` at
 * `px_size` (sum of scaled advances + kerning). Returns 0 if no font loaded.
 * Useful for centering / layout by the integrator.
 */
int ttf_text_width(const char *str, int px_size);

/*
 * Report the font's scaled ascent / descent / line gap in pixels for a given
 * `px_size` (any out-pointer may be NULL). Lets callers position the baseline
 * and compute line height = ascent - descent + line_gap.
 */
void ttf_vmetrics(int px_size, int *ascent, int *descent, int *line_gap);

#endif /* FONT_TTF_H */
