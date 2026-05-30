/**
 * Bitmap Font Rendering
 */

#ifndef TERMINAL_FONT_H
#define TERMINAL_FONT_H

#include <stdint.h>

/**
 * Render a character to framebuffer using 8x16 VGA font
 */
void font_render_char(uint32_t *pixels, uint32_t width, uint32_t height,
                     uint32_t x, uint32_t y, char ch, uint32_t color);

#endif // TERMINAL_FONT_H
