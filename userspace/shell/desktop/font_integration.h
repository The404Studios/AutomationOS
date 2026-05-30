/**
 * Desktop Shell Font Integration
 *
 * Font rendering for panel, dock, menus, and desktop elements.
 */

#ifndef DESKTOP_FONT_INTEGRATION_H
#define DESKTOP_FONT_INTEGRATION_H

#include <stdint.h>
#include <stdbool.h>
#include "../../../include/font.h"

// Font pool for desktop shell
typedef struct {
    font_t* panel_font;      // 13px for panel text (clock, indicators)
    font_t* dock_font;       // 10px for dock app labels
    font_t* menu_font;       // 12px for menu items
    font_t* notification_font; // 11px for notifications
} desktop_font_pool_t;

/**
 * Initialize desktop shell font system
 *
 * @return Font pool handle, or NULL on failure
 */
desktop_font_pool_t* desktop_font_init(void);

/**
 * Shutdown desktop font system
 *
 * @param pool Font pool to free
 */
void desktop_font_shutdown(desktop_font_pool_t* pool);

/**
 * Render panel clock text
 *
 * @param pool Font pool
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position (right-aligned)
 * @param y Y position (top of panel)
 * @param time_str Time string (e.g., "14:30")
 */
void desktop_render_clock(desktop_font_pool_t* pool,
                           uint32_t* fb, uint32_t fb_width,
                           uint32_t fb_height, uint32_t fb_pitch,
                           int32_t x, int32_t y,
                           const char* time_str);

/**
 * Render dock app label
 *
 * @param pool Font pool
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position (center of icon)
 * @param y Y position (below icon)
 * @param label App label text
 */
void desktop_render_dock_label(desktop_font_pool_t* pool,
                                 uint32_t* fb, uint32_t fb_width,
                                 uint32_t fb_height, uint32_t fb_pitch,
                                 int32_t x, int32_t y,
                                 const char* label);

/**
 * Render menu item text
 *
 * @param pool Font pool
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position (left)
 * @param y Y position (top of item)
 * @param item_height Menu item height
 * @param text Menu item text
 * @param selected Whether item is selected/hovered
 */
void desktop_render_menu_item(desktop_font_pool_t* pool,
                                uint32_t* fb, uint32_t fb_width,
                                uint32_t fb_height, uint32_t fb_pitch,
                                int32_t x, int32_t y,
                                uint32_t item_height,
                                const char* text, bool selected);

/**
 * Render notification text
 *
 * @param pool Font pool
 * @param fb Framebuffer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param fb_pitch Framebuffer pitch
 * @param x X position
 * @param y Y position
 * @param width Maximum width (for wrapping)
 * @param title Notification title
 * @param body Notification body text
 */
void desktop_render_notification(desktop_font_pool_t* pool,
                                   uint32_t* fb, uint32_t fb_width,
                                   uint32_t fb_height, uint32_t fb_pitch,
                                   int32_t x, int32_t y,
                                   uint32_t width,
                                   const char* title,
                                   const char* body);

/**
 * Measure text for layout
 *
 * @param pool Font pool
 * @param text Text to measure
 * @param font_type Font type (0=panel, 1=dock, 2=menu, 3=notification)
 * @param metrics Output metrics
 * @return true on success
 */
bool desktop_measure_text(desktop_font_pool_t* pool,
                            const char* text, int font_type,
                            font_metrics_t* metrics);

#endif // DESKTOP_FONT_INTEGRATION_H
