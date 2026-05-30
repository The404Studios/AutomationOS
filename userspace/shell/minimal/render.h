/**
 * Rendering Helpers - Header
 *
 * Simple framebuffer drawing functions
 */

#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// FRAMEBUFFER STRUCTURE
// ============================================================================

typedef struct framebuffer {
    uint32_t *pixels;       // Pointer to pixel buffer (32-bit RGBA)
    uint32_t width;         // Width in pixels
    uint32_t height;        // Height in pixels
    uint32_t pitch;         // Bytes per scanline
    uint32_t bpp;           // Bits per pixel (typically 32)
    size_t size;            // Total size in bytes
    int fd;                 // File descriptor for /dev/fb0
    void *mapped_addr;      // mmap'd address
} framebuffer_t;

// ============================================================================
// FRAMEBUFFER API
// ============================================================================

framebuffer_t *fb_init(void);
void fb_cleanup(framebuffer_t *fb);
void fb_clear(framebuffer_t *fb, uint32_t color);
void fb_fill_rect(framebuffer_t *fb, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color);
void fb_draw_rect(framebuffer_t *fb, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color);
void fb_put_pixel(framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t color);

// ============================================================================
// DRAWING HELPERS
// ============================================================================

void draw_text(framebuffer_t *fb, uint32_t x, uint32_t y,
               const char *text, uint32_t color);
void draw_line(framebuffer_t *fb, int32_t x0, int32_t y0,
               int32_t x1, int32_t y1, uint32_t color);

// ============================================================================
// COLOR UTILITIES
// ============================================================================

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

#endif // RENDER_H
