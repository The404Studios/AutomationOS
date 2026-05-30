/**
 * Rendering and Compositing Module - Implementation
 *
 * Implements window compositing using the painter's algorithm.
 * Handles double buffering to prevent tearing.
 */

#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Compare function for sorting windows by z-order (qsort)
 */
static int compare_windows_z_order(const void *a, const void *b) {
    const window_t *win_a = *(const window_t **)a;
    const window_t *win_b = *(const window_t **)b;
    return (int)win_a->z_order - (int)win_b->z_order;
}

/**
 * Initialize the renderer
 */
renderer_t *renderer_init(framebuffer_t *fb) {
    if (!fb) {
        fprintf(stderr, "[Renderer] Invalid framebuffer\n");
        return NULL;
    }

    renderer_t *renderer = calloc(1, sizeof(renderer_t));
    if (!renderer) {
        fprintf(stderr, "[Renderer] Failed to allocate renderer\n");
        return NULL;
    }

    renderer->fb = fb;
    renderer->window_count = 0;
    renderer->use_alpha_blending = true;
    renderer->cursor_x = 400;
    renderer->cursor_y = 300;
    renderer->cursor_visible = true;

    // Allocate back buffer for double buffering
    size_t buffer_size = fb->width * fb->height * 4;
    renderer->back_buffer = calloc(1, buffer_size);
    if (!renderer->back_buffer) {
        fprintf(stderr, "[Renderer] Failed to allocate back buffer\n");
        free(renderer);
        return NULL;
    }

    printf("[Renderer] Initialized with %dx%d back buffer\n", fb->width, fb->height);
    return renderer;
}

/**
 * Cleanup renderer resources
 */
void renderer_cleanup(renderer_t *renderer) {
    if (!renderer) return;

    // Free all windows
    for (uint32_t i = 0; i < renderer->window_count; i++) {
        if (renderer->windows[i]) {
            renderer_destroy_window(renderer->windows[i]);
        }
    }

    // Free back buffer
    free(renderer->back_buffer);
    free(renderer);

    printf("[Renderer] Cleaned up\n");
}

/**
 * Clear the back buffer to a solid color
 */
void renderer_clear(renderer_t *renderer, uint32_t color) {
    if (!renderer || !renderer->back_buffer) return;

    uint32_t pixel_count = renderer->fb->width * renderer->fb->height;
    for (uint32_t i = 0; i < pixel_count; i++) {
        renderer->back_buffer[i] = color;
    }
}

/**
 * Add a window to the compositor
 */
int renderer_add_window(renderer_t *renderer, window_t *window) {
    if (!renderer || !window) return -1;

    if (renderer->window_count >= MAX_WINDOWS) {
        fprintf(stderr, "[Renderer] Maximum window count reached\n");
        return -1;
    }

    renderer->windows[renderer->window_count++] = window;
    printf("[Renderer] Added window %u (%ux%u at %d,%d)\n",
           window->id, window->width, window->height, window->x, window->y);

    return 0;
}

/**
 * Remove a window from the compositor
 */
int renderer_remove_window(renderer_t *renderer, uint32_t window_id) {
    if (!renderer) return -1;

    for (uint32_t i = 0; i < renderer->window_count; i++) {
        if (renderer->windows[i]->id == window_id) {
            // Shift remaining windows down
            for (uint32_t j = i; j < renderer->window_count - 1; j++) {
                renderer->windows[j] = renderer->windows[j + 1];
            }
            renderer->window_count--;
            printf("[Renderer] Removed window %u\n", window_id);
            return 0;
        }
    }

    return -1;  // Not found
}

/**
 * Get a window by ID
 */
window_t *renderer_get_window(renderer_t *renderer, uint32_t window_id) {
    if (!renderer) return NULL;

    for (uint32_t i = 0; i < renderer->window_count; i++) {
        if (renderer->windows[i]->id == window_id) {
            return renderer->windows[i];
        }
    }

    return NULL;
}

/**
 * Alpha blend two colors
 */
uint32_t alpha_blend(uint32_t src, uint32_t dst) {
    uint32_t src_a = (src >> 24) & 0xFF;

    // Fully opaque or no alpha - just return source
    if (src_a == 0xFF) return src;
    if (src_a == 0) return dst;

    // Extract components
    uint32_t src_r = (src >> 16) & 0xFF;
    uint32_t src_g = (src >> 8) & 0xFF;
    uint32_t src_b = src & 0xFF;

    uint32_t dst_r = (dst >> 16) & 0xFF;
    uint32_t dst_g = (dst >> 8) & 0xFF;
    uint32_t dst_b = dst & 0xFF;

    // Alpha blend formula: result = src * alpha + dst * (1 - alpha)
    uint32_t out_r = (src_r * src_a + dst_r * (255 - src_a)) / 255;
    uint32_t out_g = (src_g * src_a + dst_g * (255 - src_a)) / 255;
    uint32_t out_b = (src_b * src_a + dst_b * (255 - src_a)) / 255;

    return 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
}

/**
 * Composite a single window onto the back buffer
 */
static void composite_window(renderer_t *renderer, window_t *window) {
    if (!window->visible || !window->pixels) return;

    framebuffer_t *fb = renderer->fb;

    // Calculate clipping bounds
    int32_t clip_x_start = (window->x < 0) ? -window->x : 0;
    int32_t clip_y_start = (window->y < 0) ? -window->y : 0;

    int32_t clip_x_end = window->width;
    if (window->x + (int32_t)window->width > (int32_t)fb->width) {
        clip_x_end = fb->width - window->x;
    }

    int32_t clip_y_end = window->height;
    if (window->y + (int32_t)window->height > (int32_t)fb->height) {
        clip_y_end = fb->height - window->y;
    }

    // Skip if window is completely off-screen
    if (clip_x_start >= clip_x_end || clip_y_start >= clip_y_end) return;
    if (window->x >= (int32_t)fb->width || window->y >= (int32_t)fb->height) return;

    uint32_t window_pixels_per_line = window->pitch / 4;
    bool use_alpha = renderer->use_alpha_blending && window->has_alpha;

    // Composite visible portion
    for (int32_t y = clip_y_start; y < clip_y_end; y++) {
        int32_t fb_y = window->y + y;
        if (fb_y < 0 || fb_y >= (int32_t)fb->height) continue;

        for (int32_t x = clip_x_start; x < clip_x_end; x++) {
            int32_t fb_x = window->x + x;
            if (fb_x < 0 || fb_x >= (int32_t)fb->width) continue;

            uint32_t src_pixel = window->pixels[y * window_pixels_per_line + x];

            // Calculate back buffer offset
            uint32_t dst_offset = fb_y * fb->width + fb_x;

            if (use_alpha) {
                // Alpha blend with existing back buffer content
                renderer->back_buffer[dst_offset] =
                    alpha_blend(src_pixel, renderer->back_buffer[dst_offset]);
            } else {
                // Direct copy (opaque)
                renderer->back_buffer[dst_offset] = src_pixel;
            }
        }
    }
}

/**
 * Composite all visible windows to the back buffer
 */
void renderer_composite_windows(renderer_t *renderer) {
    if (!renderer) return;

    // Sort windows by z-order (back to front)
    if (renderer->window_count > 1) {
        qsort(renderer->windows, renderer->window_count,
              sizeof(window_t *), compare_windows_z_order);
    }

    // Composite each window in order
    for (uint32_t i = 0; i < renderer->window_count; i++) {
        composite_window(renderer, renderer->windows[i]);
    }
}

/**
 * Present (flip) the back buffer to the framebuffer
 */
void renderer_present(renderer_t *renderer) {
    if (!renderer || !renderer->fb || !renderer->back_buffer) return;

    // Copy back buffer to framebuffer
    memcpy(renderer->fb->pixels, renderer->back_buffer,
           renderer->fb->width * renderer->fb->height * 4);
}

/**
 * Create a test window with solid color
 */
window_t *renderer_create_test_window(uint32_t id, int32_t x, int32_t y,
                                       uint32_t width, uint32_t height,
                                       uint32_t color) {
    window_t *window = calloc(1, sizeof(window_t));
    if (!window) {
        fprintf(stderr, "[Renderer] Failed to allocate window\n");
        return NULL;
    }

    window->id = id;
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    window->pitch = width * 4;
    window->visible = true;
    window->z_order = id;  // Default z-order
    window->has_alpha = ((color >> 24) != 0xFF);

    // Allocate pixel buffer
    window->pixels = calloc(width * height, sizeof(uint32_t));
    if (!window->pixels) {
        fprintf(stderr, "[Renderer] Failed to allocate window pixels\n");
        free(window);
        return NULL;
    }

    // Fill with solid color
    for (uint32_t i = 0; i < width * height; i++) {
        window->pixels[i] = color;
    }

    return window;
}

/**
 * Destroy a window and free its resources
 */
void renderer_destroy_window(window_t *window) {
    if (!window) return;

    free(window->pixels);
    free(window);
}

/**
 * Set cursor position
 */
void renderer_set_cursor_position(renderer_t *renderer, int32_t x, int32_t y) {
    if (!renderer) return;
    renderer->cursor_x = x;
    renderer->cursor_y = y;
}

/**
 * Get cursor position
 */
void renderer_get_cursor_position(renderer_t *renderer, int32_t *x, int32_t *y) {
    if (!renderer) return;
    if (x) *x = renderer->cursor_x;
    if (y) *y = renderer->cursor_y;
}

/**
 * Set cursor visibility
 */
void renderer_set_cursor_visible(renderer_t *renderer, bool visible) {
    if (!renderer) return;
    renderer->cursor_visible = visible;
}

/**
 * Simple arrow cursor bitmap (16x16 pixels)
 * 1 = white, 2 = black outline, 0 = transparent
 */
static const uint8_t cursor_bitmap[16][16] = {
    {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 2, 0, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 2, 0, 0, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0},
};

/**
 * Draw cursor on back buffer
 */
void renderer_draw_cursor(renderer_t *renderer) {
    if (!renderer || !renderer->cursor_visible || !renderer->back_buffer) {
        return;
    }

    framebuffer_t *fb = renderer->fb;
    int32_t cursor_size = 16;

    for (int32_t y = 0; y < cursor_size; y++) {
        int32_t fb_y = renderer->cursor_y + y;
        if (fb_y < 0 || fb_y >= (int32_t)fb->height) continue;

        for (int32_t x = 0; x < cursor_size; x++) {
            int32_t fb_x = renderer->cursor_x + x;
            if (fb_x < 0 || fb_x >= (int32_t)fb->width) continue;

            uint8_t pixel = cursor_bitmap[y][x];
            if (pixel == 0) continue;  // Transparent

            uint32_t color;
            if (pixel == 1) {
                color = 0xFFFFFFFF;  // White fill
            } else {
                color = 0xFF000000;  // Black outline
            }

            uint32_t dst_offset = fb_y * fb->width + fb_x;
            renderer->back_buffer[dst_offset] = color;
        }
    }
}
