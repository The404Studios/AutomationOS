/**
 * Optimized Blitting Module
 *
 * Provides high-performance pixel copy operations with alpha blending.
 */

#include "fb_compositor.h"
#include <string.h>

/**
 * Alpha blend two ARGB32 pixels
 *
 * Uses premultiplied alpha formula:
 * result = src + dst * (1 - src_alpha)
 */
static inline uint32_t alpha_blend_pixel(uint32_t src, uint32_t dst) {
    uint32_t src_a = (src >> 24) & 0xFF;

    // Fully opaque - just return source
    if (src_a == 0xFF) return src;

    // Fully transparent - return destination
    if (src_a == 0) return dst;

    // Extract components
    uint32_t src_r = (src >> 16) & 0xFF;
    uint32_t src_g = (src >> 8) & 0xFF;
    uint32_t src_b = src & 0xFF;

    uint32_t dst_r = (dst >> 16) & 0xFF;
    uint32_t dst_g = (dst >> 8) & 0xFF;
    uint32_t dst_b = dst & 0xFF;

    // Alpha blend: result = src * alpha + dst * (1 - alpha)
    uint32_t inv_alpha = 255 - src_a;
    uint32_t out_r = (src_r * src_a + dst_r * inv_alpha) / 255;
    uint32_t out_g = (src_g * src_a + dst_g * inv_alpha) / 255;
    uint32_t out_b = (src_b * src_a + dst_b * inv_alpha) / 255;

    return 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
}

/**
 * Apply global alpha to pixel
 */
static inline uint32_t apply_alpha(uint32_t pixel, float alpha) {
    if (alpha >= 1.0f) return pixel;
    if (alpha <= 0.0f) return 0;

    uint32_t a = (uint32_t)(((pixel >> 24) & 0xFF) * alpha);
    return (pixel & 0x00FFFFFF) | (a << 24);
}

/**
 * Fast opaque blit (no alpha blending)
 *
 * Uses memcpy for maximum performance when possible.
 */
static void blit_opaque(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                       const surface_t *surface, const rect_t *dst_rect) {
    // Calculate clipping
    int32_t clip_x = (dst_rect->x < 0) ? 0 : dst_rect->x;
    int32_t clip_y = (dst_rect->y < 0) ? 0 : dst_rect->y;
    int32_t clip_x_end = dst_rect->x + dst_rect->width;
    int32_t clip_y_end = dst_rect->y + dst_rect->height;

    if (clip_x_end > (int32_t)dst_width) clip_x_end = dst_width;
    if (clip_y_end > (int32_t)dst_height) clip_y_end = dst_height;

    if (clip_x >= clip_x_end || clip_y >= clip_y_end) return;

    // Source offset if partially clipped
    int32_t src_x_offset = (dst_rect->x < 0) ? -dst_rect->x : 0;
    int32_t src_y_offset = (dst_rect->y < 0) ? -dst_rect->y : 0;

    uint32_t src_pixels_per_line = surface->pitch / 4;
    uint32_t copy_width = clip_x_end - clip_x;

    // Fast path: use memcpy for each scanline
    for (int32_t y = clip_y; y < clip_y_end; y++) {
        int32_t src_y = src_y_offset + (y - clip_y);
        if (src_y >= (int32_t)surface->height) break;

        const uint32_t *src_line = &surface->pixels[src_y * src_pixels_per_line + src_x_offset];
        uint32_t *dst_line = &dst[y * dst_width + clip_x];

        memcpy(dst_line, src_line, copy_width * sizeof(uint32_t));
    }
}

/**
 * Alpha-blended blit
 *
 * Blends source pixels with destination using alpha channel.
 */
static void blit_alpha(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                      const surface_t *surface, const rect_t *dst_rect,
                      float global_alpha) {
    // Calculate clipping
    int32_t clip_x = (dst_rect->x < 0) ? 0 : dst_rect->x;
    int32_t clip_y = (dst_rect->y < 0) ? 0 : dst_rect->y;
    int32_t clip_x_end = dst_rect->x + dst_rect->width;
    int32_t clip_y_end = dst_rect->y + dst_rect->height;

    if (clip_x_end > (int32_t)dst_width) clip_x_end = dst_width;
    if (clip_y_end > (int32_t)dst_height) clip_y_end = dst_height;

    if (clip_x >= clip_x_end || clip_y >= clip_y_end) return;

    // Source offset if partially clipped
    int32_t src_x_offset = (dst_rect->x < 0) ? -dst_rect->x : 0;
    int32_t src_y_offset = (dst_rect->y < 0) ? -dst_rect->y : 0;

    uint32_t src_pixels_per_line = surface->pitch / 4;

    // Per-pixel alpha blending
    for (int32_t y = clip_y; y < clip_y_end; y++) {
        int32_t src_y = src_y_offset + (y - clip_y);
        if (src_y >= (int32_t)surface->height) break;

        for (int32_t x = clip_x; x < clip_x_end; x++) {
            int32_t src_x = src_x_offset + (x - clip_x);
            if (src_x >= (int32_t)surface->width) break;

            uint32_t src_pixel = surface->pixels[src_y * src_pixels_per_line + src_x];

            // Apply global alpha if needed
            if (global_alpha < 1.0f) {
                src_pixel = apply_alpha(src_pixel, global_alpha);
            }

            uint32_t dst_offset = y * dst_width + x;
            dst[dst_offset] = alpha_blend_pixel(src_pixel, dst[dst_offset]);
        }
    }
}

/**
 * Blit surface to buffer with optional alpha blending
 *
 * Public API for blitting operations.
 */
void blit_surface_to_buffer(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                           const surface_t *surface, const rect_t *dst_rect,
                           float alpha, bool use_alpha_blend) {
    if (!dst || !surface || !dst_rect) return;
    if (!surface->pixels) return;

    // Completely transparent - skip
    if (alpha <= 0.0f) return;

    // Completely off-screen - skip
    if (dst_rect->x >= (int32_t)dst_width ||
        dst_rect->y >= (int32_t)dst_height ||
        dst_rect->x + dst_rect->width <= 0 ||
        dst_rect->y + dst_rect->height <= 0) {
        return;
    }

    // Choose fast or alpha path
    if (use_alpha_blend && alpha < 1.0f) {
        blit_alpha(dst, dst_width, dst_height, surface, dst_rect, alpha);
    } else if (use_alpha_blend) {
        // Alpha blending but no global alpha
        blit_alpha(dst, dst_width, dst_height, surface, dst_rect, 1.0f);
    } else {
        // Opaque fast path
        blit_opaque(dst, dst_width, dst_height, surface, dst_rect);
    }
}

/**
 * Fill rectangle with solid color
 */
void blit_fill_rect(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                   const rect_t *rect, uint32_t color) {
    if (!dst || !rect) return;

    // Clipping
    int32_t x1 = (rect->x < 0) ? 0 : rect->x;
    int32_t y1 = (rect->y < 0) ? 0 : rect->y;
    int32_t x2 = rect->x + rect->width;
    int32_t y2 = rect->y + rect->height;

    if (x2 > (int32_t)dst_width) x2 = dst_width;
    if (y2 > (int32_t)dst_height) y2 = dst_height;

    if (x1 >= x2 || y1 >= y2) return;

    // Fill scanlines
    for (int32_t y = y1; y < y2; y++) {
        for (int32_t x = x1; x < x2; x++) {
            dst[y * dst_width + x] = color;
        }
    }
}

/**
 * Draw rounded rectangle outline
 */
void blit_draw_rounded_rect(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                           const rect_t *rect, uint32_t color, uint32_t thickness) {
    if (!dst || !rect) return;

    // Top border
    rect_t top = { rect->x, rect->y, rect->width, thickness };
    blit_fill_rect(dst, dst_width, dst_height, &top, color);

    // Bottom border
    rect_t bottom = { rect->x, rect->y + rect->height - thickness,
                     rect->width, thickness };
    blit_fill_rect(dst, dst_width, dst_height, &bottom, color);

    // Left border
    rect_t left = { rect->x, rect->y, thickness, rect->height };
    blit_fill_rect(dst, dst_width, dst_height, &left, color);

    // Right border
    rect_t right = { rect->x + rect->width - thickness, rect->y,
                    thickness, rect->height };
    blit_fill_rect(dst, dst_width, dst_height, &right, color);
}
