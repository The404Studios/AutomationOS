/**
 * Framebuffer Access Module - Header
 *
 * Provides userspace access to the kernel framebuffer via memory mapping.
 */

#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* size_t (used by the framebuffer size field below) */

/**
 * Framebuffer structure
 */
typedef struct {
    uint32_t *pixels;       // Pointer to pixel buffer (32-bit RGBA)
    uint32_t width;         // Width in pixels
    uint32_t height;        // Height in pixels
    uint32_t pitch;         // Bytes per scanline
    uint32_t bpp;           // Bits per pixel (typically 32)
    size_t size;            // Total size in bytes
    int fd;                 // File descriptor for /dev/fb0
    void *mapped_addr;      // mmap'd address
} framebuffer_t;

/**
 * Initialize and map the framebuffer
 *
 * Opens /dev/fb0 and mmaps the framebuffer memory into userspace.
 *
 * @return Framebuffer structure on success, NULL on failure
 */
framebuffer_t *fb_init(void);

/**
 * Cleanup and unmap framebuffer
 *
 * @param fb Framebuffer to cleanup
 */
void fb_cleanup(framebuffer_t *fb);

/**
 * Clear framebuffer to solid color
 *
 * @param fb Framebuffer
 * @param color 32-bit RGBA color (0xRRGGBBAA)
 */
void fb_clear(framebuffer_t *fb, uint32_t color);

/**
 * Fill a rectangular region
 *
 * @param fb Framebuffer
 * @param x X coordinate
 * @param y Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color 32-bit RGBA color
 */
void fb_fill_rect(framebuffer_t *fb, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color);

/**
 * Blit (copy) pixel data to framebuffer
 *
 * @param fb Framebuffer
 * @param dst_x Destination X coordinate
 * @param dst_y Destination Y coordinate
 * @param src_pixels Source pixel buffer
 * @param src_width Source width
 * @param src_height Source height
 * @param src_pitch Source pitch (bytes per line)
 */
void fb_blit(framebuffer_t *fb, uint32_t dst_x, uint32_t dst_y,
             const uint32_t *src_pixels, uint32_t src_width,
             uint32_t src_height, uint32_t src_pitch);

/**
 * Put a single pixel (inline for performance)
 *
 * @param fb Framebuffer
 * @param x X coordinate
 * @param y Y coordinate
 * @param color 32-bit RGBA color
 */
static inline void fb_put_pixel(framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t color) {
    if (fb && x < fb->width && y < fb->height) {
        fb->pixels[y * (fb->pitch / 4) + x] = color;
    }
}

/**
 * Get a pixel value
 *
 * @param fb Framebuffer
 * @param x X coordinate
 * @param y Y coordinate
 * @return Pixel color, or 0 if out of bounds
 */
static inline uint32_t fb_get_pixel(framebuffer_t *fb, uint32_t x, uint32_t y) {
    if (fb && x < fb->width && y < fb->height) {
        return fb->pixels[y * (fb->pitch / 4) + x];
    }
    return 0;
}

#endif /* FB_H */
