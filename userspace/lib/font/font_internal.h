/**
 * Font Library Internal Header
 *
 * Internal structures and functions used by font subsystem components.
 * Not exposed to external users.
 */

#ifndef FONT_INTERNAL_H
#define FONT_INTERNAL_H

#include "font.h"

// Forward declare font structure (defined in ttf_parser.c)
// struct font is opaque to external users

/**
 * Rasterize a glyph (called by cache when glyph not found)
 *
 * @param font Font handle
 * @param codepoint Unicode codepoint
 * @return Newly allocated glyph, or NULL on failure
 */
font_glyph_t* font_rasterize_glyph_internal(font_t* font, uint32_t codepoint);

/**
 * Get glyph advance width without rasterizing
 *
 * @param font Font handle
 * @param codepoint Unicode codepoint
 * @return Advance width in pixels
 */
int font_get_glyph_advance(font_t* font, uint32_t codepoint);

/**
 * Get kerning adjustment between two glyphs
 *
 * @param font Font handle
 * @param codepoint1 First codepoint
 * @param codepoint2 Second codepoint
 * @return Kerning adjustment in pixels (can be negative)
 */
int font_get_kerning(font_t* font, uint32_t codepoint1, uint32_t codepoint2);

#endif // FONT_INTERNAL_H
