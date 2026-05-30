/**
 * AutomationOS Effects Engine Implementation
 */

#include "effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static effect_settings_t *g_effect_settings = NULL;

/**
 * Initialize effects system
 */
void effects_init(effect_settings_t *settings) {
    g_effect_settings = settings;

    if (!g_effect_settings) {
        fprintf(stderr, "Failed to initialize effects\n");
        return;
    }

    // Default settings
    g_effect_settings->shadows_enabled = true;
    g_effect_settings->blur_enabled = true;
    g_effect_settings->dim_inactive_enabled = true;
    g_effect_settings->wobbly_enabled = false;

    // Default shadow
    g_effect_settings->shadow.color = 0x80000000;  // Semi-transparent black
    g_effect_settings->shadow.offset_x = 0;
    g_effect_settings->shadow.offset_y = 4;
    g_effect_settings->shadow.blur_radius = 12.0f;
    g_effect_settings->shadow.opacity = 0.6f;

    // Default blur
    g_effect_settings->blur.radius = 8.0f;
    g_effect_settings->blur.passes = 2;

    // Default dim
    g_effect_settings->dim_factor = 0.15f;  // Dim by 15%

    printf("[Effects] Initialized\n");
}

/**
 * Cleanup effects system
 */
void effects_cleanup(void) {
    g_effect_settings = NULL;
    printf("[Effects] Cleaned up\n");
}

/**
 * Generate Gaussian blur kernel
 */
void effect_gaussian_kernel(float *kernel, int size, float sigma) {
    float sum = 0.0f;
    int half = size / 2;

    // Generate kernel values
    for (int i = 0; i < size; i++) {
        int x = i - half;
        float value = expf(-(x * x) / (2.0f * sigma * sigma));
        kernel[i] = value;
        sum += value;
    }

    // Normalize
    for (int i = 0; i < size; i++) {
        kernel[i] /= sum;
    }
}

/**
 * Draw drop shadow
 */
void effect_draw_shadow(gpu_context_t *gpu, const rect_t *window_rect, const shadow_params_t *params) {
    if (!gpu || !window_rect || !params) return;

    // Shadow rectangle (offset from window)
    rect_t shadow_rect = {
        .x = window_rect->x + params->offset_x - (int32_t)params->blur_radius,
        .y = window_rect->y + params->offset_y - (int32_t)params->blur_radius,
        .w = window_rect->w + (int32_t)(params->blur_radius * 2),
        .h = window_rect->h + (int32_t)(params->blur_radius * 2),
    };

    // Draw blurred rectangle
    // In real implementation: render to texture, apply blur shader, then composite
    gpu_draw_rounded_rect(gpu, &shadow_rect, 8.0f, params->color);
}

/**
 * Apply blur effect to texture
 */
void effect_apply_blur(gpu_context_t *gpu, texture_t *texture, const blur_params_t *params) {
    if (!gpu || !texture || !params) return;

    // Two-pass Gaussian blur (horizontal + vertical)
    // This is a simplified version - real implementation uses shaders

    for (int pass = 0; pass < params->passes; pass++) {
        // Horizontal blur pass
        // shader: blur_horizontal with radius = params->radius

        // Vertical blur pass
        // shader: blur_vertical with radius = params->radius
    }
}

/**
 * Apply dim effect to unfocused window
 */
void effect_apply_dim(gpu_context_t *gpu, const rect_t *rect, float factor) {
    if (!gpu || !rect) return;

    // Draw semi-transparent black overlay
    uint32_t dim_color = (uint32_t)(factor * 255.0f) << 24;  // Alpha only
    gpu_draw_rect(gpu, rect, dim_color);
}

/**
 * Update wobbly window physics
 */
void effect_wobbly_update(window_t *window, float delta_time) {
    if (!window) return;

    // Simplified spring physics for wobbly effect
    // Real implementation uses mass-spring-damper system

    // Apply spring forces to window vertices
    // Update vertex positions based on velocity and forces
    // Dampen oscillations over time
}

/**
 * Apply effects to window
 */
void apply_window_effects(compositor_t *comp, window_t *window) {
    if (!comp || !window || !g_effect_settings) return;

    // Drop shadow
    if (g_effect_settings->shadows_enabled && window->type == WINDOW_NORMAL) {
        effect_draw_shadow(comp->gpu, &window->geometry, &g_effect_settings->shadow);
    }

    // Blur background (for transparent windows)
    if (g_effect_settings->blur_enabled && window->type == WINDOW_DIALOG) {
        // Blur region behind window
        effect_apply_blur(comp->gpu, window->texture, &g_effect_settings->blur);
    }

    // Dim unfocused windows
    if (g_effect_settings->dim_inactive_enabled && !window->focused) {
        effect_apply_dim(comp->gpu, &window->geometry, g_effect_settings->dim_factor);
    }
}

/**
 * Apply global effects (screen-wide)
 */
void apply_global_effects(compositor_t *comp) {
    if (!comp || !g_effect_settings) return;

    // Global effects can include:
    // - Color temperature adjustment
    // - Screen dimming
    // - Zoom/magnifier
    // - Screen recording overlay
    // - Night mode filter
}

/**
 * Create shadow shader
 */
shader_t *effect_create_shadow_shader(gpu_context_t *gpu) {
    if (!gpu) return NULL;

    const char *vertex_shader =
        "#version 120\n"
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 v_texcoord;\n"
        "uniform mat4 projection;\n"
        "void main() {\n"
        "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
        "    v_texcoord = texcoord;\n"
        "}\n";

    const char *fragment_shader =
        "#version 120\n"
        "varying vec2 v_texcoord;\n"
        "uniform sampler2D texture;\n"
        "uniform vec4 shadow_color;\n"
        "uniform float blur_radius;\n"
        "void main() {\n"
        "    // Simplified shadow - real impl uses blur kernel\n"
        "    vec4 color = texture2D(texture, v_texcoord);\n"
        "    gl_FragColor = shadow_color * color.a;\n"
        "}\n";

    return gpu_create_shader(gpu, vertex_shader, fragment_shader);
}

/**
 * Create blur shader
 */
shader_t *effect_create_blur_shader(gpu_context_t *gpu) {
    if (!gpu) return NULL;

    const char *vertex_shader =
        "#version 120\n"
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 v_texcoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "    v_texcoord = texcoord;\n"
        "}\n";

    const char *fragment_shader =
        "#version 120\n"
        "varying vec2 v_texcoord;\n"
        "uniform sampler2D texture;\n"
        "uniform vec2 direction;\n"
        "uniform float blur_radius;\n"
        "void main() {\n"
        "    // Gaussian blur\n"
        "    vec4 color = vec4(0.0);\n"
        "    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);\n"
        "    vec2 tex_offset = 1.0 / textureSize(texture, 0) * direction * blur_radius;\n"
        "    color += texture2D(texture, v_texcoord) * weights[0];\n"
        "    for(int i = 1; i < 5; i++) {\n"
        "        color += texture2D(texture, v_texcoord + tex_offset * float(i)) * weights[i];\n"
        "        color += texture2D(texture, v_texcoord - tex_offset * float(i)) * weights[i];\n"
        "    }\n"
        "    gl_FragColor = color;\n"
        "}\n";

    return gpu_create_shader(gpu, vertex_shader, fragment_shader);
}

/**
 * Create dim shader
 */
shader_t *effect_create_dim_shader(gpu_context_t *gpu) {
    if (!gpu) return NULL;

    const char *vertex_shader =
        "#version 120\n"
        "attribute vec2 position;\n"
        "void main() {\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "}\n";

    const char *fragment_shader =
        "#version 120\n"
        "uniform vec4 dim_color;\n"
        "void main() {\n"
        "    gl_FragColor = dim_color;\n"
        "}\n";

    return gpu_create_shader(gpu, vertex_shader, fragment_shader);
}
