/**
 * SIMD-Optimized Blitting Module - Header
 *
 * High-performance pixel copy with SSE2/AVX2 acceleration
 */

#ifndef SIMD_BLIT_H
#define SIMD_BLIT_H

#include "fb_compositor.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize SIMD blitting (detect CPU features)
 */
void simd_blit_init(void);

/**
 * Fast opaque blit (no alpha blending)
 */
void simd_blit_opaque(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                      const surface_t *surface, const rect_t *dst_rect);

/**
 * Alpha-blended blit
 */
void simd_blit_alpha(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                     const surface_t *surface, const rect_t *dst_rect,
                     float global_alpha);

/**
 * SIMD-optimized blit with alpha support (main API)
 */
void simd_blit_surface(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                       const surface_t *surface, const rect_t *dst_rect,
                       float alpha, bool use_alpha_blend);

/**
 * Fill rectangle with solid color (SIMD optimized)
 */
void simd_fill_rect(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                    const rect_t *rect, uint32_t color);

/**
 * Get SIMD capabilities string
 */
const char *simd_get_capabilities(void);

/**
 * Check if SIMD is available
 */
bool simd_is_available(void);

#endif // SIMD_BLIT_H
