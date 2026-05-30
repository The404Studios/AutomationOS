/**
 * Terminal Font Integration
 *
 * Monospace font rendering for terminal emulator.
 */

#ifndef TERMINAL_FONT_INTEGRATION_H
#define TERMINAL_FONT_INTEGRATION_H

#include <stdint.h>
#include <stdbool.h>
#include "../../../include/font.h"

// Terminal font context
typedef struct {
    font_t* mono_font;       // Monospace font
    uint32_t cell_width;     // Character cell width
    uint32_t cell_height;    // Character cell height
    int32_t ascent;          // Font ascent (baseline offset)
    int32_t descent;         // Font descent
} terminal_font_ctx_t;

/**
 * Initialize terminal font system
 *
 * @param font_size Font size in points (e.g., 12.0)
 * @return Font context, or NULL on failure
 */
terminal_font_ctx_t* terminal_font_init(float font_size);

/**
 * Shutdown terminal font system
 *
 * @param ctx Font context to free
 */
void terminal_font_shutdown(terminal_font_ctx_t* ctx);

/**
 * Render terminal cell (single character)
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param col Column (0-based)
 * @param row Row (0-based)
 * @param codepoint Unicode codepoint
 * @param fg_color Foreground color (ARGB)
 * @param bg_color Background color (ARGB)
 * @param bold Whether to render bold
 * @param italic Whether to render italic
 */
void terminal_render_cell(terminal_font_ctx_t* ctx,
                           uint32_t* fb, uint32_t fb_width,
                           uint32_t fb_height, uint32_t fb_pitch,
                           uint32_t col, uint32_t row,
                           uint32_t codepoint,
                           uint32_t fg_color, uint32_t bg_color,
                           bool bold, bool italic);

/**
 * Render terminal line (optimized batch rendering)
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param row Row number
 * @param text UTF-8 text to render
 * @param fg_color Foreground color
 * @param bg_color Background color
 */
void terminal_render_line(terminal_font_ctx_t* ctx,
                           uint32_t* fb, uint32_t fb_width,
                           uint32_t fb_height, uint32_t fb_pitch,
                           uint32_t row,
                           const char* text,
                           uint32_t fg_color, uint32_t bg_color);

/**
 * Render cursor
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param col Cursor column
 * @param row Cursor row
 * @param color Cursor color
 * @param style Cursor style (0=block, 1=underline, 2=bar)
 */
void terminal_render_cursor(terminal_font_ctx_t* ctx,
                              uint32_t* fb, uint32_t fb_width,
                              uint32_t fb_height, uint32_t fb_pitch,
                              uint32_t col, uint32_t row,
                              uint32_t color, uint32_t style);

/**
 * Calculate terminal dimensions from window size
 *
 * @param ctx Font context
 * @param window_width Window width in pixels
 * @param window_height Window height in pixels
 * @param cols Output: number of columns
 * @param rows Output: number of rows
 */
void terminal_calculate_dimensions(terminal_font_ctx_t* ctx,
                                     uint32_t window_width,
                                     uint32_t window_height,
                                     uint32_t* cols, uint32_t* rows);

/**
 * Set font size (re-calculates cell dimensions)
 *
 * @param ctx Font context
 * @param font_size New font size in points
 * @return true on success
 */
bool terminal_set_font_size(terminal_font_ctx_t* ctx, float font_size);

#endif // TERMINAL_FONT_INTEGRATION_H
