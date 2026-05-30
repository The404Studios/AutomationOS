/**
 * File Explorer Font Integration
 *
 * Font rendering for file lists, breadcrumbs, and status bar.
 */

#ifndef FILE_EXPLORER_FONT_INTEGRATION_H
#define FILE_EXPLORER_FONT_INTEGRATION_H

#include <stdint.h>
#include <stdbool.h>
#include "../../../include/font.h"

// File explorer font context
typedef struct {
    font_t* file_font;       // 12px for file names
    font_t* path_font;       // 11px for breadcrumbs
    font_t* status_font;     // 10px for status bar
    font_t* heading_font;    // 14px for column headers
} explorer_font_ctx_t;

/**
 * Initialize file explorer font system
 *
 * @return Font context, or NULL on failure
 */
explorer_font_ctx_t* explorer_font_init(void);

/**
 * Shutdown file explorer font system
 *
 * @param ctx Font context to free
 */
void explorer_font_shutdown(explorer_font_ctx_t* ctx);

/**
 * Render file name in list view
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position
 * @param y Y position (top of row)
 * @param row_height Row height
 * @param filename File name text
 * @param selected Whether file is selected
 */
void explorer_render_filename(explorer_font_ctx_t* ctx,
                                uint32_t* fb, uint32_t fb_width,
                                uint32_t fb_height, uint32_t fb_pitch,
                                int32_t x, int32_t y,
                                uint32_t row_height,
                                const char* filename,
                                bool selected);

/**
 * Render file metadata (size, date, etc.)
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position
 * @param y Y position
 * @param row_height Row height
 * @param metadata Metadata text
 * @param selected Whether file is selected
 */
void explorer_render_metadata(explorer_font_ctx_t* ctx,
                                uint32_t* fb, uint32_t fb_width,
                                uint32_t fb_height, uint32_t fb_pitch,
                                int32_t x, int32_t y,
                                uint32_t row_height,
                                const char* metadata,
                                bool selected);

/**
 * Render breadcrumb path segment
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position
 * @param y Y position
 * @param segment Path segment text
 * @param hover Whether segment is hovered
 * @return Width of rendered segment (for next segment position)
 */
uint32_t explorer_render_breadcrumb(explorer_font_ctx_t* ctx,
                                      uint32_t* fb, uint32_t fb_width,
                                      uint32_t fb_height, uint32_t fb_pitch,
                                      int32_t x, int32_t y,
                                      const char* segment,
                                      bool hover);

/**
 * Render status bar text
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position
 * @param y Y position (top of status bar)
 * @param status_height Status bar height
 * @param text Status text
 */
void explorer_render_status(explorer_font_ctx_t* ctx,
                              uint32_t* fb, uint32_t fb_width,
                              uint32_t fb_height, uint32_t fb_pitch,
                              int32_t x, int32_t y,
                              uint32_t status_height,
                              const char* text);

/**
 * Render column header
 *
 * @param ctx Font context
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position
 * @param y Y position
 * @param width Column width
 * @param header_height Header height
 * @param title Column title
 * @param sort_ascending Sort indicator (0=none, 1=asc, 2=desc)
 */
void explorer_render_column_header(explorer_font_ctx_t* ctx,
                                     uint32_t* fb, uint32_t fb_width,
                                     uint32_t fb_height, uint32_t fb_pitch,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t header_height,
                                     const char* title,
                                     uint32_t sort_ascending);

/**
 * Measure text for layout
 *
 * @param ctx Font context
 * @param text Text to measure
 * @param font_type Font type (0=file, 1=path, 2=status, 3=heading)
 * @param metrics Output metrics
 * @return true on success
 */
bool explorer_measure_text(explorer_font_ctx_t* ctx,
                             const char* text, int font_type,
                             font_metrics_t* metrics);

#endif // FILE_EXPLORER_FONT_INTEGRATION_H
