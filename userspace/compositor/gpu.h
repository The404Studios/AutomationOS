/**
 * AutomationOS GPU Abstraction Layer
 *
 * Provides hardware-accelerated rendering interface
 * Supports OpenGL, Vulkan, and DRM/KMS
 */

#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include <stdbool.h>
#include "compositor.h"

/**
 * GPU backend types
 */
typedef enum {
    GPU_BACKEND_OPENGL,
    GPU_BACKEND_VULKAN,
    GPU_BACKEND_DRM,
} gpu_backend_t;

/**
 * GPU context - opaque structure
 */
typedef struct gpu_context {
    gpu_backend_t backend;
    void *backend_data;

    uint32_t width, height;
    bool initialized;
} gpu_context_t;

/**
 * Framebuffer - render target
 */
typedef struct framebuffer {
    uint32_t width, height;
    void *backend_fb;
} framebuffer_t;

/**
 * Texture - GPU texture object
 */
typedef struct texture {
    uint32_t width, height;
    void *backend_texture;
} texture_t;

/**
 * Shader - GPU shader program
 */
typedef struct shader {
    void *backend_shader;
} shader_t;

// GPU initialization
gpu_context_t *gpu_init(const char *device_path);
void gpu_cleanup(gpu_context_t *gpu);

// Framebuffer management
framebuffer_t *gpu_create_framebuffer(gpu_context_t *gpu, uint32_t width, uint32_t height);
void gpu_destroy_framebuffer(gpu_context_t *gpu, framebuffer_t *fb);

// Texture management
texture_t *gpu_upload_texture(gpu_context_t *gpu, const uint32_t *pixels, uint32_t width, uint32_t height);
void gpu_update_texture(gpu_context_t *gpu, texture_t *texture, const uint32_t *pixels, uint32_t width, uint32_t height);
void gpu_destroy_texture(gpu_context_t *gpu, texture_t *texture);

// Shader management
shader_t *gpu_create_shader(gpu_context_t *gpu, const char *vertex_src, const char *fragment_src);
void gpu_use_shader(gpu_context_t *gpu, shader_t *shader);
void gpu_destroy_shader(gpu_context_t *gpu, shader_t *shader);

// Rendering
void gpu_begin_frame(gpu_context_t *gpu, framebuffer_t *fb);
void gpu_end_frame(gpu_context_t *gpu);
void gpu_clear(gpu_context_t *gpu, float r, float g, float b, float a);
void gpu_draw_textured_quad(gpu_context_t *gpu, texture_t *texture, const rect_t *src, const rect_t *dst, float alpha);
void gpu_draw_rect(gpu_context_t *gpu, const rect_t *rect, uint32_t color);
void gpu_draw_rounded_rect(gpu_context_t *gpu, const rect_t *rect, float radius, uint32_t color);
void gpu_present(gpu_context_t *gpu, framebuffer_t *fb, bool vsync);

// Shader uniforms
void gpu_set_uniform_float(gpu_context_t *gpu, shader_t *shader, const char *name, float value);
void gpu_set_uniform_vec2(gpu_context_t *gpu, shader_t *shader, const char *name, float x, float y);
void gpu_set_uniform_vec4(gpu_context_t *gpu, shader_t *shader, const char *name, float x, float y, float z, float w);
void gpu_set_uniform_mat4(gpu_context_t *gpu, shader_t *shader, const char *name, const float *matrix);

// Blending modes
void gpu_set_blend_mode(gpu_context_t *gpu, bool enabled);

// Viewport
void gpu_set_viewport(gpu_context_t *gpu, int32_t x, int32_t y, uint32_t width, uint32_t height);

// GPU info
const char *gpu_get_renderer(gpu_context_t *gpu);
const char *gpu_get_vendor(gpu_context_t *gpu);
const char *gpu_get_version(gpu_context_t *gpu);

#endif // GPU_H
