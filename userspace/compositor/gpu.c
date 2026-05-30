/**
 * AutomationOS GPU Implementation
 *
 * OpenGL/EGL backend for hardware-accelerated rendering
 * DRM/KMS support for direct rendering
 */

#include "gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// OpenGL/EGL includes (if available)
#ifdef HAS_OPENGL
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#endif

// DRM includes
#ifdef HAS_DRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

/**
 * OpenGL backend data
 */
typedef struct {
    #ifdef HAS_OPENGL
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config;
    #endif
    bool initialized;
} opengl_backend_t;

/**
 * DRM backend data
 */
typedef struct {
    #ifdef HAS_DRM
    int fd;
    drmModeRes *resources;
    drmModeConnector *connector;
    drmModeCrtc *crtc;
    uint32_t fb_id;
    #endif
    bool initialized;
} drm_backend_t;

/**
 * Initialize OpenGL backend
 */
static bool init_opengl_backend(gpu_context_t *gpu) {
    #ifdef HAS_OPENGL
    opengl_backend_t *backend = calloc(1, sizeof(opengl_backend_t));
    if (!backend) return false;

    // Initialize EGL display
    backend->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (backend->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        free(backend);
        return false;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(backend->display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        free(backend);
        return false;
    }

    printf("[GPU] EGL %d.%d initialized\n", major, minor);

    // Choose config
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(backend->display, config_attribs, &backend->config, 1, &num_configs)) {
        fprintf(stderr, "Failed to choose EGL config\n");
        eglTerminate(backend->display);
        free(backend);
        return false;
    }

    // Create context
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    backend->context = eglCreateContext(backend->display, backend->config, EGL_NO_CONTEXT, context_attribs);
    if (backend->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        eglTerminate(backend->display);
        free(backend);
        return false;
    }

    backend->initialized = true;
    gpu->backend_data = backend;

    printf("[GPU] OpenGL ES 2.0 context created\n");
    return true;

    #else
    fprintf(stderr, "[GPU] OpenGL support not compiled\n");
    return false;
    #endif
}

/**
 * Initialize DRM backend
 */
static bool init_drm_backend(gpu_context_t *gpu, const char *device_path) {
    #ifdef HAS_DRM
    drm_backend_t *backend = calloc(1, sizeof(drm_backend_t));
    if (!backend) return false;

    // Open DRM device
    backend->fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (backend->fd < 0) {
        fprintf(stderr, "Failed to open DRM device: %s\n", device_path);
        free(backend);
        return false;
    }

    // Get DRM resources
    backend->resources = drmModeGetResources(backend->fd);
    if (!backend->resources) {
        fprintf(stderr, "Failed to get DRM resources\n");
        close(backend->fd);
        free(backend);
        return false;
    }

    // Find connector
    for (int i = 0; i < backend->resources->count_connectors; i++) {
        backend->connector = drmModeGetConnector(backend->fd, backend->resources->connectors[i]);
        if (backend->connector->connection == DRM_MODE_CONNECTED) {
            break;
        }
        drmModeFreeConnector(backend->connector);
        backend->connector = NULL;
    }

    if (!backend->connector) {
        fprintf(stderr, "No connected display found\n");
        drmModeFreeResources(backend->resources);
        close(backend->fd);
        free(backend);
        return false;
    }

    // Get CRTC
    backend->crtc = drmModeGetCrtc(backend->fd, backend->connector->encoder_id);
    if (!backend->crtc) {
        fprintf(stderr, "Failed to get CRTC\n");
        drmModeFreeConnector(backend->connector);
        drmModeFreeResources(backend->resources);
        close(backend->fd);
        free(backend);
        return false;
    }

    gpu->width = backend->crtc->mode.hdisplay;
    gpu->height = backend->crtc->mode.vdisplay;

    backend->initialized = true;
    gpu->backend_data = backend;

    printf("[GPU] DRM backend initialized: %ux%u\n", gpu->width, gpu->height);
    return true;

    #else
    fprintf(stderr, "[GPU] DRM support not compiled\n");
    return false;
    #endif
}

/**
 * Initialize GPU context
 */
gpu_context_t *gpu_init(const char *device_path) {
    gpu_context_t *gpu = calloc(1, sizeof(gpu_context_t));
    if (!gpu) {
        fprintf(stderr, "Failed to allocate GPU context\n");
        return NULL;
    }

    // Try OpenGL first, fall back to DRM
    if (init_opengl_backend(gpu)) {
        gpu->backend = GPU_BACKEND_OPENGL;
        gpu->initialized = true;
    } else if (device_path && init_drm_backend(gpu, device_path)) {
        gpu->backend = GPU_BACKEND_DRM;
        gpu->initialized = true;
    } else {
        fprintf(stderr, "Failed to initialize any GPU backend\n");
        free(gpu);
        return NULL;
    }

    printf("[GPU] Initialized (backend: %s)\n",
           gpu->backend == GPU_BACKEND_OPENGL ? "OpenGL" :
           gpu->backend == GPU_BACKEND_DRM ? "DRM" : "Unknown");

    return gpu;
}

/**
 * Cleanup GPU context
 */
void gpu_cleanup(gpu_context_t *gpu) {
    if (!gpu) return;

    if (gpu->backend == GPU_BACKEND_OPENGL) {
        #ifdef HAS_OPENGL
        opengl_backend_t *backend = gpu->backend_data;
        if (backend) {
            if (backend->context != EGL_NO_CONTEXT) {
                eglDestroyContext(backend->display, backend->context);
            }
            if (backend->display != EGL_NO_DISPLAY) {
                eglTerminate(backend->display);
            }
            free(backend);
        }
        #endif
    } else if (gpu->backend == GPU_BACKEND_DRM) {
        #ifdef HAS_DRM
        drm_backend_t *backend = gpu->backend_data;
        if (backend) {
            if (backend->crtc) drmModeFreeCrtc(backend->crtc);
            if (backend->connector) drmModeFreeConnector(backend->connector);
            if (backend->resources) drmModeFreeResources(backend->resources);
            if (backend->fd >= 0) close(backend->fd);
            free(backend);
        }
        #endif
    }

    free(gpu);
    printf("[GPU] Cleaned up\n");
}

/**
 * Create framebuffer
 */
framebuffer_t *gpu_create_framebuffer(gpu_context_t *gpu, uint32_t width, uint32_t height) {
    if (!gpu || !gpu->initialized) return NULL;

    framebuffer_t *fb = calloc(1, sizeof(framebuffer_t));
    if (!fb) return NULL;

    fb->width = width;
    fb->height = height;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        // Create OpenGL framebuffer object
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        fb->backend_fb = (void *)(uintptr_t)fbo;
    }
    #endif

    return fb;
}

/**
 * Destroy framebuffer
 */
void gpu_destroy_framebuffer(gpu_context_t *gpu, framebuffer_t *fb) {
    if (!gpu || !fb) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL && fb->backend_fb) {
        GLuint fbo = (GLuint)(uintptr_t)fb->backend_fb;
        glDeleteFramebuffers(1, &fbo);
    }
    #endif

    free(fb);
}

/**
 * Upload texture to GPU
 */
texture_t *gpu_upload_texture(gpu_context_t *gpu, const uint32_t *pixels, uint32_t width, uint32_t height) {
    if (!gpu || !pixels) return NULL;

    texture_t *texture = calloc(1, sizeof(texture_t));
    if (!texture) return NULL;

    texture->width = width;
    texture->height = height;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        texture->backend_texture = (void *)(uintptr_t)tex;
    }
    #endif

    return texture;
}

/**
 * Update texture data
 */
void gpu_update_texture(gpu_context_t *gpu, texture_t *texture, const uint32_t *pixels, uint32_t width, uint32_t height) {
    if (!gpu || !texture || !pixels) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL && texture->backend_texture) {
        GLuint tex = (GLuint)(uintptr_t)texture->backend_texture;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    #endif
}

/**
 * Destroy texture
 */
void gpu_destroy_texture(gpu_context_t *gpu, texture_t *texture) {
    if (!gpu || !texture) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL && texture->backend_texture) {
        GLuint tex = (GLuint)(uintptr_t)texture->backend_texture;
        glDeleteTextures(1, &tex);
    }
    #endif

    free(texture);
}

shader_t *gpu_create_shader(gpu_context_t *gpu, const char *vertex_src, const char *fragment_src) {
    if (!gpu || !vertex_src || !fragment_src) return NULL;

    shader_t *shader = calloc(1, sizeof(shader_t));
    if (!shader) return NULL;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        // Shader compilation is backend-specific; keep a valid handle object so
        // callers can use effects paths even when GL support is stubbed out.
        shader->backend_shader = NULL;
    }
    #endif

    return shader;
}

void gpu_use_shader(gpu_context_t *gpu, shader_t *shader) {
    (void)gpu;
    (void)shader;
}

void gpu_destroy_shader(gpu_context_t *gpu, shader_t *shader) {
    (void)gpu;
    if (!shader) return;
    free(shader);
}

void gpu_set_uniform_float(gpu_context_t *gpu, shader_t *shader, const char *name, float value) {
    (void)gpu; (void)shader; (void)name; (void)value;
}

void gpu_set_uniform_vec2(gpu_context_t *gpu, shader_t *shader, const char *name, float x, float y) {
    (void)gpu; (void)shader; (void)name; (void)x; (void)y;
}

void gpu_set_uniform_vec4(gpu_context_t *gpu, shader_t *shader, const char *name,
                          float x, float y, float z, float w) {
    (void)gpu; (void)shader; (void)name; (void)x; (void)y; (void)z; (void)w;
}

void gpu_set_uniform_mat4(gpu_context_t *gpu, shader_t *shader, const char *name, const float *matrix) {
    (void)gpu; (void)shader; (void)name; (void)matrix;
}

void gpu_set_blend_mode(gpu_context_t *gpu, bool enabled) {
    if (!gpu) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        if (enabled) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }
    }
    #else
    (void)enabled;
    #endif
}

void gpu_set_viewport(gpu_context_t *gpu, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!gpu) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        glViewport(x, y, width, height);
    }
    #else
    (void)x; (void)y; (void)width; (void)height;
    #endif
}

/**
 * Begin frame rendering
 */
void gpu_begin_frame(gpu_context_t *gpu, framebuffer_t *fb) {
    if (!gpu || !fb) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        GLuint fbo = (GLuint)(uintptr_t)fb->backend_fb;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, fb->width, fb->height);
    }
    #endif
}

/**
 * End frame rendering
 */
void gpu_end_frame(gpu_context_t *gpu) {
    if (!gpu) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        glFlush();
    }
    #endif
}

/**
 * Clear framebuffer
 */
void gpu_clear(gpu_context_t *gpu, float r, float g, float b, float a) {
    if (!gpu) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    #endif
}

/**
 * Draw textured quad
 */
void gpu_draw_textured_quad(gpu_context_t *gpu, texture_t *texture, const rect_t *src, const rect_t *dst, float alpha) {
    if (!gpu || !texture) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        // Simplified - real implementation would use VBOs and shaders
        GLuint tex = (GLuint)(uintptr_t)texture->backend_texture;
        glBindTexture(GL_TEXTURE_2D, tex);

        // Set alpha blending
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Draw quad (pseudo-code - needs proper vertex data)
        // In real implementation: setup vertex buffer, bind shader, draw
    }
    #endif
}

/**
 * Present framebuffer (swap buffers)
 */
void gpu_present(gpu_context_t *gpu, framebuffer_t *fb, bool vsync) {
    if (!gpu) return;

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        opengl_backend_t *backend = gpu->backend_data;
        if (backend && backend->display != EGL_NO_DISPLAY && backend->surface != EGL_NO_SURFACE) {
            eglSwapInterval(backend->display, vsync ? 1 : 0);
            eglSwapBuffers(backend->display, backend->surface);
        }
    }
    #endif

    #ifdef HAS_DRM
    if (gpu->backend == GPU_BACKEND_DRM) {
        drm_backend_t *backend = gpu->backend_data;
        if (backend && backend->fb_id) {
            drmModePageFlip(backend->fd, backend->crtc->crtc_id, backend->fb_id,
                          DRM_MODE_PAGE_FLIP_EVENT, NULL);
        }
    }
    #endif
}

/**
 * Get GPU renderer info
 */
const char *gpu_get_renderer(gpu_context_t *gpu) {
    if (!gpu) return "Unknown";

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        return (const char *)glGetString(GL_RENDERER);
    }
    #endif

    return "AutomationOS Software Renderer";
}

/**
 * Get GPU vendor info
 */
const char *gpu_get_vendor(gpu_context_t *gpu) {
    if (!gpu) return "Unknown";

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        return (const char *)glGetString(GL_VENDOR);
    }
    #endif

    return "AutomationOS";
}

const char *gpu_get_version(gpu_context_t *gpu) {
    if (!gpu) return "Unknown";

    #ifdef HAS_OPENGL
    if (gpu->backend == GPU_BACKEND_OPENGL) {
        return (const char *)glGetString(GL_VERSION);
    }
    #endif

    return "0.1";
}

/**
 * Draw rectangle
 */
void gpu_draw_rect(gpu_context_t *gpu, const rect_t *rect, uint32_t color) {
    if (!gpu || !rect) return;
    // Implementation for solid color rectangles
}

/**
 * Draw rounded rectangle
 */
void gpu_draw_rounded_rect(gpu_context_t *gpu, const rect_t *rect, float radius, uint32_t color) {
    if (!gpu || !rect) return;
    // Implementation for rounded rectangles (used for window decorations)
}
