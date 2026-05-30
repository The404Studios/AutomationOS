/**
 * AutomationOS Enhanced Shadow System Implementation
 */

#include "shadow_system.h"
#include <stdio.h>
#include <math.h>

/**
 * Material Design 3 Shadow Specifications
 *
 * Each shadow has two layers:
 * 1. Key shadow - Sharp, directional (simulates direct light)
 * 2. Ambient shadow - Soft, diffuse (simulates scattered light)
 */
const shadow_spec_t SHADOW_SPECS[5] = {
    // SHADOW_SM (Elevation 1)
    {
        .key_offset_y = 1,
        .key_blur = 2.0f,
        .key_opacity = 0.1f,
        .ambient_offset_y = 1,
        .ambient_blur = 3.0f,
        .ambient_opacity = 0.06f,
        .color = 0x00000000,  // Black (alpha in opacity)
    },
    // SHADOW_MD (Elevation 2)
    {
        .key_offset_y = 2,
        .key_blur = 4.0f,
        .key_opacity = 0.15f,
        .ambient_offset_y = 1,
        .ambient_blur = 5.0f,
        .ambient_opacity = 0.1f,
        .color = 0x00000000,
    },
    // SHADOW_LG (Elevation 4)
    {
        .key_offset_y = 4,
        .key_blur = 8.0f,
        .key_opacity = 0.2f,
        .ambient_offset_y = 2,
        .ambient_blur = 10.0f,
        .ambient_opacity = 0.14f,
        .color = 0x00000000,
    },
    // SHADOW_XL (Elevation 8)
    {
        .key_offset_y = 8,
        .key_blur = 16.0f,
        .key_opacity = 0.25f,
        .ambient_offset_y = 4,
        .ambient_blur = 20.0f,
        .ambient_opacity = 0.18f,
        .color = 0x00000000,
    },
    // SHADOW_XXL (Elevation 16)
    {
        .key_offset_y = 16,
        .key_blur = 32.0f,
        .key_opacity = 0.3f,
        .ambient_offset_y = 8,
        .ambient_blur = 40.0f,
        .ambient_opacity = 0.22f,
        .color = 0x00000000,
    },
};

/**
 * Get shadow specification for level
 */
const shadow_spec_t *shadow_get_spec(shadow_level_t level) {
    if (level >= 0 && level < 5) {
        return &SHADOW_SPECS[level];
    }
    return &SHADOW_SPECS[SHADOW_MD];  // Default to medium
}

/**
 * Convert float opacity to RGBA color
 */
static uint32_t apply_opacity(uint32_t color, float opacity) {
    uint8_t alpha = (uint8_t)(opacity * 255.0f);
    return (color & 0x00FFFFFF) | (alpha << 24);
}

/**
 * Draw layered shadow (key + ambient)
 */
void shadow_draw_layered(gpu_context_t *gpu,
                         const rect_t *rect,
                         shadow_level_t level,
                         bool active) {
    if (!gpu || !rect) return;

    const shadow_spec_t *spec = shadow_get_spec(level);

    // Increase shadow for active/focused windows
    float intensity_multiplier = active ? 1.2f : 1.0f;

    // Draw ambient shadow (larger, softer)
    rect_t ambient_rect = {
        .x = rect->x - (int32_t)spec->ambient_blur,
        .y = rect->y + spec->ambient_offset_y - (int32_t)spec->ambient_blur,
        .w = rect->w + (int32_t)(spec->ambient_blur * 2),
        .h = rect->h + (int32_t)(spec->ambient_blur * 2),
    };
    uint32_t ambient_color = apply_opacity(spec->color,
                                           spec->ambient_opacity * intensity_multiplier);
    gpu_draw_blurred_rect(gpu, &ambient_rect, spec->ambient_blur, ambient_color);

    // Draw key shadow (smaller, sharper)
    rect_t key_rect = {
        .x = rect->x - (int32_t)spec->key_blur,
        .y = rect->y + spec->key_offset_y - (int32_t)spec->key_blur,
        .w = rect->w + (int32_t)(spec->key_blur * 2),
        .h = rect->h + (int32_t)(spec->key_blur * 2),
    };
    uint32_t key_color = apply_opacity(spec->color,
                                       spec->key_opacity * intensity_multiplier);
    gpu_draw_blurred_rect(gpu, &key_rect, spec->key_blur, key_color);
}

/**
 * Draw shadow with custom parameters
 */
void shadow_draw_custom(gpu_context_t *gpu,
                        const rect_t *rect,
                        const shadow_spec_t *spec) {
    if (!gpu || !rect || !spec) return;

    // Draw ambient shadow
    rect_t ambient_rect = {
        .x = rect->x - (int32_t)spec->ambient_blur,
        .y = rect->y + spec->ambient_offset_y - (int32_t)spec->ambient_blur,
        .w = rect->w + (int32_t)(spec->ambient_blur * 2),
        .h = rect->h + (int32_t)(spec->ambient_blur * 2),
    };
    uint32_t ambient_color = apply_opacity(spec->color, spec->ambient_opacity);
    gpu_draw_blurred_rect(gpu, &ambient_rect, spec->ambient_blur, ambient_color);

    // Draw key shadow
    rect_t key_rect = {
        .x = rect->x - (int32_t)spec->key_blur,
        .y = rect->y + spec->key_offset_y - (int32_t)spec->key_blur,
        .w = rect->w + (int32_t)(spec->key_blur * 2),
        .h = rect->h + (int32_t)(spec->key_blur * 2),
    };
    uint32_t key_color = apply_opacity(spec->color, spec->key_opacity);
    gpu_draw_blurred_rect(gpu, &key_rect, spec->key_blur, key_color);
}

/**
 * Linear interpolation helper
 */
static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/**
 * Interpolate between shadow levels
 */
shadow_spec_t shadow_interpolate(shadow_level_t from,
                                  shadow_level_t to,
                                  float t) {
    const shadow_spec_t *spec_from = shadow_get_spec(from);
    const shadow_spec_t *spec_to = shadow_get_spec(to);

    shadow_spec_t result;

    result.key_offset_y = (int32_t)lerp((float)spec_from->key_offset_y,
                                        (float)spec_to->key_offset_y, t);
    result.key_blur = lerp(spec_from->key_blur, spec_to->key_blur, t);
    result.key_opacity = lerp(spec_from->key_opacity, spec_to->key_opacity, t);

    result.ambient_offset_y = (int32_t)lerp((float)spec_from->ambient_offset_y,
                                            (float)spec_to->ambient_offset_y, t);
    result.ambient_blur = lerp(spec_from->ambient_blur, spec_to->ambient_blur, t);
    result.ambient_opacity = lerp(spec_from->ambient_opacity, spec_to->ambient_opacity, t);

    result.color = spec_from->color;  // Keep color the same

    return result;
}

/**
 * Draw rounded shadow (for rounded windows)
 */
void shadow_draw_rounded(gpu_context_t *gpu,
                         const rect_t *rect,
                         float corner_radius,
                         shadow_level_t level) {
    if (!gpu || !rect) return;

    const shadow_spec_t *spec = shadow_get_spec(level);

    // Draw ambient shadow with rounded corners
    rect_t ambient_rect = {
        .x = rect->x - (int32_t)spec->ambient_blur,
        .y = rect->y + spec->ambient_offset_y - (int32_t)spec->ambient_blur,
        .w = rect->w + (int32_t)(spec->ambient_blur * 2),
        .h = rect->h + (int32_t)(spec->ambient_blur * 2),
    };
    uint32_t ambient_color = apply_opacity(spec->color, spec->ambient_opacity);
    gpu_draw_rounded_blurred_rect(gpu, &ambient_rect,
                                   corner_radius + spec->ambient_blur,
                                   spec->ambient_blur,
                                   ambient_color);

    // Draw key shadow with rounded corners
    rect_t key_rect = {
        .x = rect->x - (int32_t)spec->key_blur,
        .y = rect->y + spec->key_offset_y - (int32_t)spec->key_blur,
        .w = rect->w + (int32_t)(spec->key_blur * 2),
        .h = rect->h + (int32_t)(spec->key_blur * 2),
    };
    uint32_t key_color = apply_opacity(spec->color, spec->key_opacity);
    gpu_draw_rounded_blurred_rect(gpu, &key_rect,
                                   corner_radius + spec->key_blur,
                                   spec->key_blur,
                                   key_color);
}
