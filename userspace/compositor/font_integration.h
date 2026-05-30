/**
 * Compositor Font Integration
 *
 * Wrapper for font library providing compositor-specific text rendering.
 * Manages font loading, caching, and rendering for window decorations.
 */

#ifndef COMPOSITOR_FONT_INTEGRATION_H
#define COMPOSITOR_FONT_INTEGRATION_H

#include <stdint.h>
#include <stdbool.h>
#include "../../include/font.h"

// Font pool for compositor
typedef struct {
    font_t* title_font;      // 12pt for window titles
    font_t* menu_font;       // 12pt for menus
    font_t* ui_font;         // 11pt for UI elements
    font_t* mono_font;       // 10pt monospace for debug
} compositor_font_pool_t;

/**
 * Initialize compositor font system
 *
 * @param cache_size Glyph cache size (default: 2000)
 * @return Font pool handle, or NULL on failure
 */
compositor_font_pool_t* compositor_font_init(size_t cache_size);

/**
 * Shutdown font system and free resources
 *
 * @param pool Font pool to free
 */
void compositor_font_shutdown(compositor_font_pool_t* pool);

/**
 * Render window title in decoration bar
 *
 * @param pool Font pool
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position (left edge)
 * @param y Y position (top edge of decoration)
 * @param width Maximum width for title
 * @param height Decoration bar height
 * @param title Title text (UTF-8)
 * @param focused Whether window is focused
 */
void compositor_render_title(compositor_font_pool_t* pool,
                              uint32_t* fb, uint32_t fb_width,
                              uint32_t fb_height, uint32_t fb_pitch,
                              int32_t x, int32_t y,
                              uint32_t width, uint32_t height,
                              const char* title, bool focused);

/**
 * Render menu item text
 *
 * @param pool Font pool
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position
 * @param y Y position
 * @param text Menu item text
 * @param selected Whether item is selected
 */
void compositor_render_menu_item(compositor_font_pool_t* pool,
                                  uint32_t* fb, uint32_t fb_width,
                                  uint32_t fb_height, uint32_t fb_pitch,
                                  int32_t x, int32_t y,
                                  const char* text, bool selected);

/**
 * Measure text dimensions (for layout calculations)
 *
 * @param pool Font pool
 * @param text Text to measure
 * @param font_type Font type (0=title, 1=menu, 2=ui, 3=mono)
 * @param metrics Output metrics
 * @return true on success
 */
bool compositor_measure_text(compositor_font_pool_t* pool,
                              const char* text, int font_type,
                              font_metrics_t* metrics);

/**
 * Warm font cache with common glyphs
 *
 * @param pool Font pool
 */
void compositor_warm_cache(compositor_font_pool_t* pool);

/**
 * Get cache statistics
 *
 * @param hits Output: cache hits
 * @param misses Output: cache misses
 * @param size Output: cache size
 */
void compositor_font_stats(size_t* hits, size_t* misses, size_t* size);

#endif // COMPOSITOR_FONT_INTEGRATION_H
