/**
 * Image Loading Library - Public API
 *
 * Supports PNG, JPEG, BMP, GIF formats with transparency
 * Uses stb_image for decoding
 */

#ifndef IMAGE_H
#define IMAGE_H

/*
 * De-POSIX'd / dual-mode header.
 * ------------------------------------------------------------------
 * Define IMAGE_FREESTANDING (e.g. -DIMAGE_FREESTANDING) to build this
 * module for the ring-3, no-libc/no-stdio world used by the desktop
 * apps (image viewer). In that mode we DO NOT pull in the hosted
 * <stdint.h>/<stdbool.h> headers; instead we provide the same fixed-
 * width typedefs ourselves so the public struct layout is identical
 * to the hosted build. The fopen/printf-based entry points
 * (image_load, image_detect_format, icon_*) are compiled out in
 * freestanding mode -- use image_load_from_memory() instead, fed by a
 * SYS_MAP_FILE'd initrd buffer.
 *
 * Without IMAGE_FREESTANDING the module behaves exactly as before for
 * the existing hosted consumers (files/preview.c, desktop/desktop.c).
 */
#ifdef IMAGE_FREESTANDING
typedef unsigned char       uint8_t;
typedef unsigned int        uint32_t;
typedef int                 int32_t;
typedef unsigned long       size_t;
typedef int                 bool;
#define true  1
#define false 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#else
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* size_t */
#endif

// ============================================================================
// IMAGE DATA STRUCTURE
// ============================================================================

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t channels;      // 1=grayscale, 2=gray+alpha, 3=RGB, 4=RGBA
    uint8_t *data;          // Pixel data (row-major, top-to-bottom)
    size_t data_size;       // Size in bytes
    bool owns_data;         // If true, image_free() will free data
} image_t;

typedef enum {
    IMAGE_FORMAT_UNKNOWN = 0,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_BMP,
    IMAGE_FORMAT_GIF,
    IMAGE_FORMAT_TGA,
    IMAGE_FORMAT_PSD,
    IMAGE_FORMAT_HDR,
} image_format_t;

typedef enum {
    IMAGE_LOAD_AUTO = 0,    // Auto-detect channels
    IMAGE_LOAD_GRAY = 1,    // Force grayscale
    IMAGE_LOAD_GRAY_ALPHA = 2,
    IMAGE_LOAD_RGB = 3,     // Force RGB
    IMAGE_LOAD_RGBA = 4,    // Force RGBA
} image_load_mode_t;

// ============================================================================
// CORE API
// ============================================================================

#ifndef IMAGE_FREESTANDING
/**
 * Load image from file path  (hosted only -- requires fopen)
 * Returns NULL on failure
 */
image_t *image_load(const char *path, image_load_mode_t mode);

/**
 * Detect image format without loading  (hosted only -- requires fopen)
 */
image_format_t image_detect_format(const char *path);
#endif /* !IMAGE_FREESTANDING */

/**
 * Load image from memory buffer (PNG/JPEG/BMP/GIF bytes).
 * Available in both hosted and freestanding builds -- this is the path
 * the ring-3 image viewer uses with a SYS_MAP_FILE'd initrd buffer.
 */
image_t *image_load_from_memory(const void *buffer, size_t size, image_load_mode_t mode);

/**
 * Free image and its data
 */
void image_free(image_t *image);

/**
 * Get pixel at (x, y) as RGBA
 * Returns 0 if out of bounds
 */
uint32_t image_get_pixel(const image_t *image, uint32_t x, uint32_t y);

/**
 * Set pixel at (x, y) from RGBA
 */
void image_set_pixel(image_t *image, uint32_t x, uint32_t y, uint32_t rgba);

/**
 * Get raw pixel data pointer
 */
const uint8_t *image_get_pixels(const image_t *image);

/**
 * Get stride (bytes per row)
 */
size_t image_get_stride(const image_t *image);

// ============================================================================
// IMAGE MANIPULATION
// ============================================================================

/**
 * Convert image to RGBA format (in-place or new)
 */
image_t *image_convert_to_rgba(image_t *image, bool in_place);

/**
 * Resize image using nearest-neighbor scaling
 */
image_t *image_resize(const image_t *image, uint32_t new_width, uint32_t new_height);

/**
 * Resize image using bilinear interpolation
 */
image_t *image_resize_bilinear(const image_t *image, uint32_t new_width, uint32_t new_height);

/**
 * Crop image to rectangle
 */
image_t *image_crop(const image_t *image, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

/**
 * Flip image vertically
 */
void image_flip_vertical(image_t *image);

/**
 * Flip image horizontally
 */
void image_flip_horizontal(image_t *image);

/**
 * Rotate image 90 degrees clockwise
 */
image_t *image_rotate_90(const image_t *image);

/**
 * Apply premultiplied alpha
 */
void image_premultiply_alpha(image_t *image);

// ============================================================================
// BLENDING & COMPOSITING
// ============================================================================

/**
 * Blend source image onto destination at (x, y)
 * Uses alpha blending for transparency
 */
void image_blit(image_t *dest, const image_t *src, int32_t dest_x, int32_t dest_y);

/**
 * Blend with opacity
 */
void image_blit_alpha(image_t *dest, const image_t *src, int32_t dest_x, int32_t dest_y, float opacity);

/**
 * Blit scaled
 */
void image_blit_scaled(image_t *dest, const image_t *src, int32_t dest_x, int32_t dest_y,
                       uint32_t dest_width, uint32_t dest_height);

// ============================================================================
// ICON LOADING
// ============================================================================

#ifndef IMAGE_FREESTANDING   /* icon set loading requires fopen/snprintf */
typedef struct {
    image_t **images;       // Array of images (different sizes)
    uint32_t count;         // Number of images
    const char *name;       // Icon name (e.g., "terminal")
} icon_set_t;

/**
 * Load icon set from directory
 * Expects: /path/to/icon_name/16x16.png, 24x24.png, etc.
 */
icon_set_t *icon_load_set(const char *icon_name);

/**
 * Get best icon for target size
 */
image_t *icon_get_best_size(const icon_set_t *set, uint32_t target_size);

/**
 * Free icon set
 */
void icon_free_set(icon_set_t *set);
#endif /* !IMAGE_FREESTANDING */

// ============================================================================
// UTILITIES
// ============================================================================

/**
 * Create empty image
 */
image_t *image_create(uint32_t width, uint32_t height, uint32_t channels);

/**
 * Clone image
 */
image_t *image_clone(const image_t *image);

/**
 * Fill image with solid color (RGBA)
 */
void image_fill(image_t *image, uint32_t rgba);

/**
 * Get format name string
 */
const char *image_format_name(image_format_t format);

/**
 * Get last error message
 */
const char *image_get_error(void);

// ============================================================================
// FRAMEBUFFER UTILITIES
// ============================================================================

/**
 * Blit image directly to framebuffer
 * Framebuffer must be 32-bit RGBA format
 */
void image_blit_to_framebuffer(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                                const image_t *image, int32_t x, int32_t y);

/**
 * Blit image to framebuffer with scaling
 */
void image_blit_to_framebuffer_scaled(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                                      const image_t *image, int32_t x, int32_t y,
                                      uint32_t dest_width, uint32_t dest_height);

#endif // IMAGE_H
