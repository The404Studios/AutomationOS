/**
 * Font Rendering Library - Public API
 *
 * TrueType font loading and rendering with anti-aliasing and caching.
 * Uses stb_truetype for font parsing and rasterization.
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Forward declarations
typedef struct font font_t;
typedef struct font_glyph font_glyph_t;

// Font rendering quality
typedef enum {
    FONT_QUALITY_LOW = 0,      // No anti-aliasing (1-bit)
    FONT_QUALITY_MEDIUM = 4,   // 4-level grayscale
    FONT_QUALITY_HIGH = 8,     // 8-level grayscale (default)
    FONT_QUALITY_ULTRA = 16    // 16-level grayscale
} font_quality_t;

// Text alignment
typedef enum {
    FONT_ALIGN_LEFT,
    FONT_ALIGN_CENTER,
    FONT_ALIGN_RIGHT
} font_align_t;

// Font styles (bitflags)
#define FONT_STYLE_NORMAL    0x00
#define FONT_STYLE_BOLD      0x01
#define FONT_STYLE_ITALIC    0x02
#define FONT_STYLE_UNDERLINE 0x04

// Rendered glyph structure
struct font_glyph {
    uint8_t* bitmap;     // Grayscale bitmap (8-bit per pixel)
    uint32_t width;
    uint32_t height;
    int32_t bearing_x;   // Horizontal bearing (offset from cursor)
    int32_t bearing_y;   // Vertical bearing (offset from baseline)
    int32_t advance;     // Horizontal advance to next glyph
};

// Text measurement result
typedef struct {
    uint32_t width;      // Total width in pixels
    uint32_t height;     // Total height in pixels (ascent + descent)
    int32_t ascent;      // Distance from baseline to top
    int32_t descent;     // Distance from baseline to bottom
    uint32_t line_gap;   // Spacing between lines
} font_metrics_t;

// Text rendering options
typedef struct {
    uint32_t color;           // ARGB color
    font_align_t align;       // Horizontal alignment
    uint32_t wrap_width;      // Line wrap width (0 = no wrap)
    uint32_t line_spacing;    // Extra line spacing in pixels
    uint8_t style;            // Font style flags
} font_render_opts_t;

/**
 * Initialize font system (must be called before any font operations)
 *
 * @param cache_size Maximum number of glyphs to cache (default: 1000)
 * @return true on success, false on failure
 */
bool font_init(size_t cache_size);

/**
 * Shutdown font system and free all resources
 */
void font_shutdown(void);

/**
 * Load TrueType font from file
 *
 * @param path Path to .ttf file
 * @return Font handle, or NULL on failure
 */
font_t* font_load(const char* path);

/**
 * Load TrueType font from memory buffer
 *
 * @param data Font data buffer
 * @param size Size of font data in bytes
 * @return Font handle, or NULL on failure
 */
font_t* font_load_memory(const void* data, size_t size);

/**
 * Free font and release resources
 *
 * @param font Font to free
 */
void font_free(font_t* font);

/**
 * Set font size in points (at 72 DPI)
 *
 * @param font Font handle
 * @param size_pt Font size in points (e.g., 12, 14, 16)
 */
void font_set_size(font_t* font, float size_pt);

/**
 * Set font rendering quality
 *
 * @param font Font handle
 * @param quality Rendering quality level
 */
void font_set_quality(font_t* font, font_quality_t quality);

/**
 * Measure text dimensions without rendering
 *
 * @param font Font handle
 * @param text UTF-8 encoded text
 * @param metrics Output metrics structure
 * @return true on success, false on failure
 */
bool font_measure_text(font_t* font, const char* text, font_metrics_t* metrics);

/**
 * Render text to ARGB framebuffer
 *
 * @param font Font handle
 * @param fb Framebuffer pointer (32-bit ARGB)
 * @param fb_width Framebuffer width in pixels
 * @param fb_height Framebuffer height in pixels
 * @param fb_pitch Framebuffer pitch in bytes
 * @param x X coordinate (top-left for left-aligned text)
 * @param y Y coordinate (baseline position)
 * @param text UTF-8 encoded text
 * @param opts Rendering options (NULL for defaults)
 * @return Number of characters rendered, or -1 on failure
 */
int font_render_text(font_t* font, uint32_t* fb, uint32_t fb_width,
                     uint32_t fb_height, uint32_t fb_pitch,
                     int32_t x, int32_t y, const char* text,
                     const font_render_opts_t* opts);

/**
 * Render single glyph to ARGB framebuffer
 *
 * @param font Font handle
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch in bytes
 * @param x X coordinate
 * @param y Y coordinate (baseline)
 * @param codepoint Unicode codepoint
 * @param color ARGB color
 * @return Horizontal advance in pixels, or -1 on failure
 */
int font_render_glyph(font_t* font, uint32_t* fb, uint32_t fb_width,
                      uint32_t fb_height, uint32_t fb_pitch,
                      int32_t x, int32_t y, uint32_t codepoint, uint32_t color);

/**
 * Get glyph from cache (or rasterize if not cached)
 *
 * @param font Font handle
 * @param codepoint Unicode codepoint
 * @return Glyph structure, or NULL on failure
 */
const font_glyph_t* font_get_glyph(font_t* font, uint32_t codepoint);

/**
 * Get font metrics (ascent, descent, line gap)
 *
 * @param font Font handle
 * @param metrics Output metrics structure
 */
void font_get_metrics(font_t* font, font_metrics_t* metrics);

/**
 * Decode UTF-8 character and advance pointer
 *
 * @param text Pointer to UTF-8 string (will be advanced)
 * @return Unicode codepoint, or 0 on error
 */
uint32_t font_utf8_decode(const char** text);

/**
 * Count characters in UTF-8 string
 *
 * @param text UTF-8 string
 * @return Number of Unicode codepoints
 */
size_t font_utf8_strlen(const char* text);

/**
 * Clear glyph cache for specific font
 *
 * @param font Font handle (NULL = clear all caches)
 */
void font_cache_clear(font_t* font);

/**
 * Get cache statistics
 *
 * @param hits Output: cache hits
 * @param misses Output: cache misses
 * @param size Output: current cache size
 */
void font_cache_stats(size_t* hits, size_t* misses, size_t* size);

// Default rendering options
static const font_render_opts_t FONT_RENDER_OPTS_DEFAULT = {
    .color = 0xFF000000,         // Black
    .align = FONT_ALIGN_LEFT,
    .wrap_width = 0,
    .line_spacing = 0,
    .style = FONT_STYLE_NORMAL
};

#endif // FONT_H
