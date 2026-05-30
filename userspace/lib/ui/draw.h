/**
 * Drawing Utilities - High-level drawing API
 *
 * Provides simple drawing primitives for UI rendering
 */

#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>
#include <stdbool.h>

// Color helper macros
#define ARGB(a,r,g,b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGB(r,g,b) ARGB(255, r, g, b)
#define ALPHA(c) (((c) >> 24) & 0xFF)
#define RED(c) (((c) >> 16) & 0xFF)
#define GREEN(c) (((c) >> 8) & 0xFF)
#define BLUE(c) ((c) & 0xFF)

// Common colors
#define COLOR_TRANSPARENT ARGB(0, 0, 0, 0)
#define COLOR_BLACK RGB(0, 0, 0)
#define COLOR_WHITE RGB(255, 255, 255)
#define COLOR_RED RGB(255, 0, 0)
#define COLOR_GREEN RGB(0, 255, 0)
#define COLOR_BLUE RGB(0, 0, 255)
#define COLOR_GRAY RGB(128, 128, 128)
#define COLOR_LIGHT_GRAY RGB(211, 211, 211)
#define COLOR_DARK_GRAY RGB(64, 64, 64)

/**
 * Fill rectangle with solid color
 */
void draw_fill_rect(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                    int32_t x, int32_t y, uint32_t width, uint32_t height,
                    uint32_t color);

/**
 * Draw rectangle outline
 */
void draw_rect(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
               int32_t x, int32_t y, uint32_t width, uint32_t height,
               uint32_t color, uint32_t thickness);

/**
 * Draw rounded rectangle (filled)
 */
void draw_rounded_rect(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                       int32_t x, int32_t y, uint32_t width, uint32_t height,
                       uint32_t radius, uint32_t color);

/**
 * Draw circle (filled)
 */
void draw_circle(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                 int32_t cx, int32_t cy, uint32_t radius, uint32_t color);

/**
 * Draw line
 */
void draw_line(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
               int32_t x1, int32_t y1, int32_t x2, int32_t y2,
               uint32_t color, uint32_t thickness);

/**
 * Draw horizontal line (optimized)
 */
void draw_hline(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                int32_t x, int32_t y, uint32_t length, uint32_t color);

/**
 * Draw vertical line (optimized)
 */
void draw_vline(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                int32_t x, int32_t y, uint32_t length, uint32_t color);

/**
 * Alpha blend two colors
 */
uint32_t draw_blend(uint32_t src, uint32_t dst);

/**
 * Set pixel with bounds checking
 */
void draw_pixel(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                int32_t x, int32_t y, uint32_t color);

/**
 * Get pixel with bounds checking
 */
uint32_t draw_get_pixel(const uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                        int32_t x, int32_t y);

/**
 * Clear framebuffer to color
 */
void draw_clear(uint32_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t color);

/**
 * Apply gaussian blur to region (simple box blur approximation)
 */
void draw_blur_region(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                      int32_t x, int32_t y, uint32_t width, uint32_t height,
                      uint32_t radius);

/**
 * Draw shadow effect
 */
void draw_shadow(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                 int32_t x, int32_t y, uint32_t width, uint32_t height,
                 uint32_t blur, uint8_t opacity);

#endif // DRAW_H
