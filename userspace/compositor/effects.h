/**
 * AutomationOS Effects Engine
 *
 * Visual effects for compositor
 */

#ifndef EFFECTS_H
#define EFFECTS_H

#include <stdint.h>
#include <stdbool.h>
#include "compositor.h"
#include "gpu.h"

/**
 * Effect types
 */
typedef enum {
    EFFECT_SHADOW,          // Drop shadow
    EFFECT_BLUR,            // Gaussian blur
    EFFECT_DIM,             // Dim unfocused windows
    EFFECT_WOBBLE,          // Wobbly windows
    EFFECT_BURN,            // Fire effect on close
    EFFECT_CUBE,            // 3D cube workspace switcher
} effect_type_t;

/**
 * Shadow parameters
 */
typedef struct {
    uint32_t color;         // RGBA
    int32_t offset_x;
    int32_t offset_y;
    float blur_radius;
    float opacity;
} shadow_params_t;

/**
 * Blur parameters
 */
typedef struct {
    float radius;           // Blur kernel radius
    int passes;             // Number of blur passes
} blur_params_t;

/**
 * Effect settings
 */
typedef struct {
    bool shadows_enabled;
    bool blur_enabled;
    bool dim_inactive_enabled;
    bool wobbly_enabled;

    shadow_params_t shadow;
    blur_params_t blur;
    float dim_factor;       // 0.0 - 1.0
} effect_settings_t;

// Effect initialization
void effects_init(effect_settings_t *settings);
void effects_cleanup(void);

// Apply effects
void apply_window_effects(compositor_t *comp, window_t *window);
void apply_global_effects(compositor_t *comp);

// Individual effects
void effect_draw_shadow(gpu_context_t *gpu, const rect_t *window_rect, const shadow_params_t *params);
void effect_apply_blur(gpu_context_t *gpu, texture_t *texture, const blur_params_t *params);
void effect_apply_dim(gpu_context_t *gpu, const rect_t *rect, float factor);
void effect_wobbly_update(window_t *window, float delta_time);

// Shader-based effects
shader_t *effect_create_shadow_shader(gpu_context_t *gpu);
shader_t *effect_create_blur_shader(gpu_context_t *gpu);
shader_t *effect_create_dim_shader(gpu_context_t *gpu);

// Effect utilities
void effect_gaussian_kernel(float *kernel, int size, float sigma);

#endif // EFFECTS_H
