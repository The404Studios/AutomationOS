/**
 * AutomationOS Background Blur Effects Implementation
 */

#include "blur_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/**
 * Blur context structure
 */
struct blur_context {
    gpu_context_t *gpu;

    // Cached shaders
    shader_t *gaussian_h_shader;
    shader_t *gaussian_v_shader;
    shader_t *box_shader;
    shader_t *kawase_shader;

    // Temporary textures for multi-pass blur
    texture_t *temp_texture_1;
    texture_t *temp_texture_2;

    uint32_t temp_width;
    uint32_t temp_height;
};

/**
 * Preset blur parameters
 */
const blur_params_t BLUR_PANEL_BACKGROUND = {
    .radius = 20.0f,
    .quality = BLUR_QUALITY_MEDIUM,
    .opacity = 0.8f,
    .saturation_boost = true,
};

const blur_params_t BLUR_DOCK_BACKGROUND = {
    .radius = 30.0f,
    .quality = BLUR_QUALITY_MEDIUM,
    .opacity = 0.85f,
    .saturation_boost = true,
};

const blur_params_t BLUR_MENU_BACKGROUND = {
    .radius = 15.0f,
    .quality = BLUR_QUALITY_LOW,
    .opacity = 0.9f,
    .saturation_boost = false,
};

const blur_params_t BLUR_DIALOG_BACKGROUND = {
    .radius = 25.0f,
    .quality = BLUR_QUALITY_MEDIUM,
    .opacity = 0.75f,
    .saturation_boost = true,
};

const blur_params_t BLUR_NOTIFICATION = {
    .radius = 12.0f,
    .quality = BLUR_QUALITY_LOW,
    .opacity = 0.95f,
    .saturation_boost = false,
};

/**
 * Initialize blur system
 */
blur_context_t *blur_init(gpu_context_t *gpu) {
    if (!gpu) return NULL;

    blur_context_t *ctx = calloc(1, sizeof(blur_context_t));
    if (!ctx) {
        fprintf(stderr, "[Blur] Failed to allocate context\n");
        return NULL;
    }

    ctx->gpu = gpu;

    // Create shaders
    ctx->gaussian_h_shader = blur_create_gaussian_shader(gpu, true);
    ctx->gaussian_v_shader = blur_create_gaussian_shader(gpu, false);
    ctx->box_shader = blur_create_box_shader(gpu);
    ctx->kawase_shader = blur_create_kawase_shader(gpu);

    // Temporary textures will be allocated on demand
    ctx->temp_texture_1 = NULL;
    ctx->temp_texture_2 = NULL;
    ctx->temp_width = 0;
    ctx->temp_height = 0;

    printf("[Blur] Effects initialized\n");
    return ctx;
}

/**
 * Cleanup blur system
 */
void blur_cleanup(blur_context_t *ctx) {
    if (!ctx) return;

    // Cleanup shaders
    if (ctx->gaussian_h_shader) gpu_destroy_shader(ctx->gpu, ctx->gaussian_h_shader);
    if (ctx->gaussian_v_shader) gpu_destroy_shader(ctx->gpu, ctx->gaussian_v_shader);
    if (ctx->box_shader) gpu_destroy_shader(ctx->gpu, ctx->box_shader);
    if (ctx->kawase_shader) gpu_destroy_shader(ctx->gpu, ctx->kawase_shader);

    // Cleanup temporary textures
    if (ctx->temp_texture_1) gpu_destroy_texture(ctx->gpu, ctx->temp_texture_1);
    if (ctx->temp_texture_2) gpu_destroy_texture(ctx->gpu, ctx->temp_texture_2);

    free(ctx);
    printf("[Blur] Effects cleaned up\n");
}

/**
 * Ensure temporary textures are allocated
 */
static bool ensure_temp_textures(blur_context_t *ctx, uint32_t width, uint32_t height) {
    if (ctx->temp_texture_1 && ctx->temp_width == width && ctx->temp_height == height) {
        return true;  // Already allocated
    }

    // Cleanup old textures
    if (ctx->temp_texture_1) gpu_destroy_texture(ctx->gpu, ctx->temp_texture_1);
    if (ctx->temp_texture_2) gpu_destroy_texture(ctx->gpu, ctx->temp_texture_2);

    // Allocate new textures
    ctx->temp_texture_1 = gpu_create_texture(ctx->gpu, width, height);
    ctx->temp_texture_2 = gpu_create_texture(ctx->gpu, width, height);

    if (!ctx->temp_texture_1 || !ctx->temp_texture_2) {
        fprintf(stderr, "[Blur] Failed to allocate temporary textures\n");
        return false;
    }

    ctx->temp_width = width;
    ctx->temp_height = height;

    return true;
}

/**
 * Blur a region
 */
void blur_region(blur_context_t *ctx,
                 const rect_t *region,
                 const blur_params_t *params) {
    if (!ctx || !region || !params) return;

    // Capture region to texture
    texture_t *region_texture = gpu_capture_region(ctx->gpu, region);
    if (!region_texture) return;

    // Apply blur based on quality
    texture_t *blurred = NULL;
    switch (params->quality) {
        case BLUR_QUALITY_LOW:
            blurred = blur_box(ctx, region_texture, params->radius);
            break;
        case BLUR_QUALITY_MEDIUM:
            blurred = blur_kawase(ctx, region_texture, (int)(params->radius / 5.0f));
            break;
        case BLUR_QUALITY_HIGH:
        case BLUR_QUALITY_ULTRA:
            blurred = blur_gaussian_two_pass(ctx, region_texture,
                                             params->radius, params->quality);
            break;
    }

    if (!blurred) {
        gpu_destroy_texture(ctx->gpu, region_texture);
        return;
    }

    // Apply saturation boost if enabled
    if (params->saturation_boost) {
        // Increase saturation by 20%
        gpu_adjust_saturation(ctx->gpu, blurred, 1.2f);
    }

    // Composite blurred texture back to screen
    gpu_draw_textured_quad(ctx->gpu, blurred, NULL, region, params->opacity);

    // Cleanup
    gpu_destroy_texture(ctx->gpu, region_texture);
    // Note: blurred texture might be temp_texture, don't destroy
}

/**
 * Blur behind window
 */
void blur_behind_window(blur_context_t *ctx,
                        window_t *window,
                        const blur_params_t *params) {
    if (!ctx || !window || !params) return;

    // Capture background behind window
    rect_t bg_rect = window->geometry;
    blur_region(ctx, &bg_rect, params);
}

/**
 * Apply vibrancy effect
 */
void blur_vibrancy(blur_context_t *ctx,
                   const rect_t *region,
                   float blur_radius,
                   float saturation,
                   float brightness) {
    if (!ctx || !region) return;

    blur_params_t params = {
        .radius = blur_radius,
        .quality = BLUR_QUALITY_MEDIUM,
        .opacity = 0.9f,
        .saturation_boost = false,
    };

    // Capture and blur
    texture_t *region_texture = gpu_capture_region(ctx->gpu, region);
    if (!region_texture) return;

    texture_t *blurred = blur_gaussian_two_pass(ctx, region_texture,
                                                 params.radius, params.quality);
    if (!blurred) {
        gpu_destroy_texture(ctx->gpu, region_texture);
        return;
    }

    // Apply saturation and brightness
    gpu_adjust_saturation(ctx->gpu, blurred, saturation);
    gpu_adjust_brightness(ctx->gpu, blurred, brightness);

    // Composite
    gpu_draw_textured_quad(ctx->gpu, blurred, NULL, region, params.opacity);

    gpu_destroy_texture(ctx->gpu, region_texture);
}

/**
 * Two-pass Gaussian blur
 */
texture_t *blur_gaussian_two_pass(blur_context_t *ctx,
                                  texture_t *input,
                                  float radius,
                                  blur_quality_t quality) {
    if (!ctx || !input) return NULL;

    // Get texture dimensions
    uint32_t width, height;
    gpu_get_texture_size(ctx->gpu, input, &width, &height);

    // Ensure temporary textures
    if (!ensure_temp_textures(ctx, width, height)) {
        return NULL;
    }

    // Calculate kernel size based on quality
    int kernel_size;
    switch (quality) {
        case BLUR_QUALITY_LOW:    kernel_size = 9; break;
        case BLUR_QUALITY_MEDIUM: kernel_size = 15; break;
        case BLUR_QUALITY_HIGH:   kernel_size = 21; break;
        case BLUR_QUALITY_ULTRA:  kernel_size = 31; break;
        default: kernel_size = 15;
    }

    // Pass 1: Horizontal blur
    gpu_bind_shader(ctx->gpu, ctx->gaussian_h_shader);
    gpu_set_shader_uniform_float(ctx->gpu, "blur_radius", radius);
    gpu_set_shader_uniform_int(ctx->gpu, "kernel_size", kernel_size);
    gpu_render_to_texture(ctx->gpu, ctx->temp_texture_1, input);

    // Pass 2: Vertical blur
    gpu_bind_shader(ctx->gpu, ctx->gaussian_v_shader);
    gpu_set_shader_uniform_float(ctx->gpu, "blur_radius", radius);
    gpu_set_shader_uniform_int(ctx->gpu, "kernel_size", kernel_size);
    gpu_render_to_texture(ctx->gpu, ctx->temp_texture_2, ctx->temp_texture_1);

    return ctx->temp_texture_2;
}

/**
 * Box blur (single pass)
 */
texture_t *blur_box(blur_context_t *ctx,
                    texture_t *input,
                    float radius) {
    if (!ctx || !input) return NULL;

    uint32_t width, height;
    gpu_get_texture_size(ctx->gpu, input, &width, &height);

    if (!ensure_temp_textures(ctx, width, height)) {
        return NULL;
    }

    gpu_bind_shader(ctx->gpu, ctx->box_shader);
    gpu_set_shader_uniform_float(ctx->gpu, "blur_radius", radius);
    gpu_render_to_texture(ctx->gpu, ctx->temp_texture_1, input);

    return ctx->temp_texture_1;
}

/**
 * Kawase blur (fast, iterative)
 */
texture_t *blur_kawase(blur_context_t *ctx,
                       texture_t *input,
                       int iterations) {
    if (!ctx || !input || iterations <= 0) return NULL;

    uint32_t width, height;
    gpu_get_texture_size(ctx->gpu, input, &width, &height);

    if (!ensure_temp_textures(ctx, width, height)) {
        return NULL;
    }

    texture_t *current = input;
    texture_t *target = ctx->temp_texture_1;

    gpu_bind_shader(ctx->gpu, ctx->kawase_shader);

    for (int i = 0; i < iterations; i++) {
        gpu_set_shader_uniform_int(ctx->gpu, "iteration", i);
        gpu_render_to_texture(ctx->gpu, target, current);

        // Ping-pong between textures
        texture_t *temp = current;
        current = target;
        target = (target == ctx->temp_texture_1) ? ctx->temp_texture_2 : ctx->temp_texture_1;
    }

    return current;
}

/**
 * Create Gaussian blur shader (horizontal or vertical)
 */
shader_t *blur_create_gaussian_shader(gpu_context_t *gpu, bool horizontal) {
    if (!gpu) return NULL;

    const char *direction = horizontal ? "horizontal" : "vertical";

    const char *vertex_shader =
        "#version 120\n"
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 v_texcoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "    v_texcoord = texcoord;\n"
        "}\n";

    char fragment_shader[2048];
    snprintf(fragment_shader, sizeof(fragment_shader),
        "#version 120\n"
        "varying vec2 v_texcoord;\n"
        "uniform sampler2D texture;\n"
        "uniform float blur_radius;\n"
        "uniform int kernel_size;\n"
        "void main() {\n"
        "    vec2 texel_size = 1.0 / textureSize(texture, 0);\n"
        "    vec4 result = vec4(0.0);\n"
        "    float total_weight = 0.0;\n"
        "    int half_kernel = kernel_size / 2;\n"
        "    for (int i = -half_kernel; i <= half_kernel; i++) {\n"
        "        float offset = float(i);\n"
        "        vec2 sample_offset = %s ? vec2(offset * texel_size.x, 0.0) : vec2(0.0, offset * texel_size.y);\n"
        "        float weight = exp(-(offset * offset) / (2.0 * blur_radius * blur_radius));\n"
        "        result += texture2D(texture, v_texcoord + sample_offset) * weight;\n"
        "        total_weight += weight;\n"
        "    }\n"
        "    gl_FragColor = result / total_weight;\n"
        "}\n",
        horizontal ? "true" : "false");

    return gpu_create_shader(gpu, vertex_shader, fragment_shader);
}

/**
 * Create box blur shader
 */
shader_t *blur_create_box_shader(gpu_context_t *gpu) {
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
        "uniform float blur_radius;\n"
        "void main() {\n"
        "    vec2 texel_size = 1.0 / textureSize(texture, 0);\n"
        "    vec4 result = vec4(0.0);\n"
        "    int samples = int(blur_radius);\n"
        "    for (int x = -samples; x <= samples; x++) {\n"
        "        for (int y = -samples; y <= samples; y++) {\n"
        "            vec2 offset = vec2(float(x), float(y)) * texel_size;\n"
        "            result += texture2D(texture, v_texcoord + offset);\n"
        "        }\n"
        "    }\n"
        "    float total = float((samples * 2 + 1) * (samples * 2 + 1));\n"
        "    gl_FragColor = result / total;\n"
        "}\n";

    return gpu_create_shader(gpu, vertex_shader, fragment_shader);
}

/**
 * Create Kawase blur shader
 */
shader_t *blur_create_kawase_shader(gpu_context_t *gpu) {
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
        "uniform int iteration;\n"
        "void main() {\n"
        "    vec2 texel_size = 1.0 / textureSize(texture, 0);\n"
        "    float offset = float(iteration) + 0.5;\n"
        "    vec4 result = texture2D(texture, v_texcoord);\n"
        "    result += texture2D(texture, v_texcoord + vec2(offset, offset) * texel_size);\n"
        "    result += texture2D(texture, v_texcoord + vec2(-offset, offset) * texel_size);\n"
        "    result += texture2D(texture, v_texcoord + vec2(offset, -offset) * texel_size);\n"
        "    result += texture2D(texture, v_texcoord + vec2(-offset, -offset) * texel_size);\n"
        "    gl_FragColor = result / 5.0;\n"
        "}\n";

    return gpu_create_shader(gpu, vertex_shader, fragment_shader);
}
