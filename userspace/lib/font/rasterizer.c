/**
 * Font Rasterizer - Text Rendering to Framebuffer
 *
 * Renders anti-aliased text to ARGB framebuffers with alpha blending.
 */

#include "font.h"
#include "font_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * Alpha blend a grayscale pixel onto ARGB background
 */
static inline uint32_t blend_pixel(uint32_t bg, uint32_t fg_color, uint8_t alpha) {
    if (alpha == 0) return bg;
    if (alpha == 255) return fg_color;

    // Extract channels
    uint8_t bg_a = (bg >> 24) & 0xFF;
    uint8_t bg_r = (bg >> 16) & 0xFF;
    uint8_t bg_g = (bg >> 8) & 0xFF;
    uint8_t bg_b = bg & 0xFF;

    uint8_t fg_a = (fg_color >> 24) & 0xFF;
    uint8_t fg_r = (fg_color >> 16) & 0xFF;
    uint8_t fg_g = (fg_color >> 8) & 0xFF;
    uint8_t fg_b = fg_color & 0xFF;

    // Apply glyph alpha to foreground alpha
    uint32_t src_alpha = (fg_a * alpha) / 255;
    uint32_t inv_alpha = 255 - src_alpha;

    // Blend
    uint8_t out_r = (fg_r * src_alpha + bg_r * inv_alpha) / 255;
    uint8_t out_g = (fg_g * src_alpha + bg_g * inv_alpha) / 255;
    uint8_t out_b = (fg_b * src_alpha + bg_b * inv_alpha) / 255;
    uint8_t out_a = bg_a + ((255 - bg_a) * src_alpha) / 255;

    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

/**
 * Render single glyph to framebuffer
 */
int font_render_glyph(font_t* font, uint32_t* fb, uint32_t fb_width,
                      uint32_t fb_height, uint32_t fb_pitch,
                      int32_t x, int32_t y, uint32_t codepoint, uint32_t color) {
    if (!font || !fb) return -1;

    const font_glyph_t* glyph = font_get_glyph(font, codepoint);
    if (!glyph) return -1;

    // Calculate glyph position (y is baseline)
    int32_t glyph_x = x + glyph->bearing_x;
    int32_t glyph_y = y + glyph->bearing_y;

    // Render glyph bitmap
    if (glyph->bitmap && glyph->width > 0 && glyph->height > 0) {
        for (uint32_t row = 0; row < glyph->height; row++) {
            int32_t fb_y = glyph_y + row;
            if (fb_y < 0 || fb_y >= (int32_t)fb_height) continue;

            for (uint32_t col = 0; col < glyph->width; col++) {
                int32_t fb_x = glyph_x + col;
                if (fb_x < 0 || fb_x >= (int32_t)fb_width) continue;

                uint8_t alpha = glyph->bitmap[row * glyph->width + col];
                if (alpha == 0) continue;

                // Calculate framebuffer offset
                uint32_t offset = (fb_y * (fb_pitch / 4)) + fb_x;
                fb[offset] = blend_pixel(fb[offset], color, alpha);
            }
        }
    }

    return glyph->advance;
}

/**
 * UTF-8 decoder
 */
uint32_t font_utf8_decode(const char** text) {
    if (!text || !*text) return 0;

    const uint8_t* s = (const uint8_t*)*text;
    uint32_t codepoint = 0;
    int bytes = 0;

    // Determine character length
    if ((*s & 0x80) == 0) {
        // 0xxxxxxx - 1 byte (ASCII)
        codepoint = *s;
        bytes = 1;
    } else if ((*s & 0xE0) == 0xC0) {
        // 110xxxxx 10xxxxxx - 2 bytes
        codepoint = *s & 0x1F;
        bytes = 2;
    } else if ((*s & 0xF0) == 0xE0) {
        // 1110xxxx 10xxxxxx 10xxxxxx - 3 bytes
        codepoint = *s & 0x0F;
        bytes = 3;
    } else if ((*s & 0xF8) == 0xF0) {
        // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx - 4 bytes
        codepoint = *s & 0x07;
        bytes = 4;
    } else {
        // Invalid UTF-8
        *text = (const char*)(s + 1);
        return 0xFFFD;  // Replacement character
    }

    // Decode continuation bytes
    for (int i = 1; i < bytes; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            // Invalid continuation byte
            *text = (const char*)(s + i);
            return 0xFFFD;
        }
        codepoint = (codepoint << 6) | (s[i] & 0x3F);
    }

    *text = (const char*)(s + bytes);
    return codepoint;
}

/**
 * Count UTF-8 characters
 */
size_t font_utf8_strlen(const char* text) {
    if (!text) return 0;

    size_t count = 0;
    const char* p = text;

    while (*p) {
        font_utf8_decode(&p);
        count++;
    }

    return count;
}

/**
 * Measure text dimensions
 */
bool font_measure_text(font_t* font, const char* text, font_metrics_t* metrics) {
    if (!font || !text || !metrics) return false;

    font_get_metrics(font, metrics);

    int32_t x = 0;
    int32_t max_width = 0;
    const char* p = text;
    uint32_t prev_codepoint = 0;

    while (*p) {
        uint32_t codepoint = font_utf8_decode(&p);
        if (codepoint == 0) break;

        // Handle newlines
        if (codepoint == '\n') {
            if (x > max_width) max_width = x;
            x = 0;
            prev_codepoint = 0;
            continue;
        }

        // Apply kerning
        if (prev_codepoint) {
            x += font_get_kerning(font, prev_codepoint, codepoint);
        }

        // Get glyph advance
        const font_glyph_t* glyph = font_get_glyph(font, codepoint);
        if (glyph) {
            x += glyph->advance;
        }

        prev_codepoint = codepoint;
    }

    if (x > max_width) max_width = x;
    metrics->width = max_width;

    return true;
}

/**
 * Render text to framebuffer
 */
int font_render_text(font_t* font, uint32_t* fb, uint32_t fb_width,
                     uint32_t fb_height, uint32_t fb_pitch,
                     int32_t x, int32_t y, const char* text,
                     const font_render_opts_t* opts) {
    if (!font || !fb || !text) return -1;

    // Use default options if none provided
    font_render_opts_t default_opts = FONT_RENDER_OPTS_DEFAULT;
    if (!opts) opts = &default_opts;

    // Handle alignment
    int32_t cursor_x = x;
    int32_t cursor_y = y;

    if (opts->align != FONT_ALIGN_LEFT) {
        font_metrics_t metrics;
        font_measure_text(font, text, &metrics);

        if (opts->align == FONT_ALIGN_CENTER) {
            cursor_x -= metrics.width / 2;
        } else if (opts->align == FONT_ALIGN_RIGHT) {
            cursor_x -= metrics.width;
        }
    }

    // Render glyphs
    const char* p = text;
    uint32_t prev_codepoint = 0;
    int count = 0;

    while (*p) {
        uint32_t codepoint = font_utf8_decode(&p);
        if (codepoint == 0) break;

        // Handle newlines
        if (codepoint == '\n') {
            cursor_x = x;
            font_metrics_t metrics;
            font_get_metrics(font, &metrics);
            cursor_y += metrics.height + opts->line_spacing;
            prev_codepoint = 0;
            continue;
        }

        // Apply kerning
        if (prev_codepoint) {
            cursor_x += font_get_kerning(font, prev_codepoint, codepoint);
        }

        // Render glyph
        int advance = font_render_glyph(font, fb, fb_width, fb_height, fb_pitch,
                                         cursor_x, cursor_y, codepoint, opts->color);
        if (advance < 0) {
            continue;  // Skip failed glyphs
        }

        cursor_x += advance;
        prev_codepoint = codepoint;
        count++;

        // Handle line wrapping
        if (opts->wrap_width > 0 && cursor_x >= (int32_t)(x + opts->wrap_width)) {
            cursor_x = x;
            font_metrics_t metrics;
            font_get_metrics(font, &metrics);
            cursor_y += metrics.height + opts->line_spacing;
            prev_codepoint = 0;
        }
    }

    return count;
}
