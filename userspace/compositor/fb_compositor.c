/**
 * AutomationOS Framebuffer Compositor - Implementation
 *
 * Software rendering compositor using direct framebuffer access.
 */

#include "fb_compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// External functions from blit.c and composition.c
extern void blit_surface_to_buffer(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                                   const surface_t *surface, const rect_t *dst_rect,
                                   float alpha, bool use_alpha_blend);
extern void composite_windows(fb_compositor_t *comp);
extern void draw_cursor(uint32_t *buffer, uint32_t width, uint32_t height,
                       int32_t x, int32_t y);

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Initialize framebuffer compositor
 */
fb_compositor_t *fb_compositor_init(void) {
    fb_compositor_t *comp = calloc(1, sizeof(fb_compositor_t));
    if (!comp) {
        fprintf(stderr, "[FB Compositor] Failed to allocate compositor\n");
        return NULL;
    }

    // Initialize framebuffer
    comp->fb = fb_init();
    if (!comp->fb) {
        fprintf(stderr, "[FB Compositor] Failed to initialize framebuffer\n");
        free(comp);
        return NULL;
    }

    // Allocate back buffer for double buffering
    size_t buffer_size = comp->fb->width * comp->fb->height * sizeof(uint32_t);
    comp->back_buffer = calloc(1, buffer_size);
    if (!comp->back_buffer) {
        fprintf(stderr, "[FB Compositor] Failed to allocate back buffer\n");
        fb_cleanup(comp->fb);
        free(comp);
        return NULL;
    }

    // Initialize settings
    comp->window_count = 0;
    comp->fps = 0;
    comp->frame_count = 0;
    comp->last_fps_update = get_time_us();
    comp->last_frame_time = 0;
    comp->last_vsync_time = get_time_us();
    comp->cursor_x = comp->fb->width / 2;
    comp->cursor_y = comp->fb->height / 2;
    comp->cursor_visible = true;
    comp->use_alpha_blending = true;
    comp->use_damage_tracking = true;
    comp->vsync_enabled = true;  // VSync enabled by default

    // Initialize damage tracking
    comp->damage.count = 0;
    comp->damage.full_redraw = true;  // First frame is full redraw

    printf("[FB Compositor] Initialized successfully\n");
    printf("[FB Compositor]   Resolution: %ux%u\n", comp->fb->width, comp->fb->height);
    printf("[FB Compositor]   Back buffer: %.2f MB\n", buffer_size / (1024.0 * 1024.0));
    printf("[FB Compositor]   VSync: enabled (60 FPS target)\n");

    return comp;
}

/**
 * Cleanup compositor resources
 */
void fb_compositor_cleanup(fb_compositor_t *comp) {
    if (!comp) return;

    // Cleanup all windows
    for (uint32_t i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]) {
            window_destroy(comp->windows[i]);
        }
    }

    // Cleanup framebuffer and buffers
    free(comp->back_buffer);
    fb_cleanup(comp->fb);
    free(comp);

    printf("[FB Compositor] Cleaned up\n");
}

/**
 * Update FPS counter
 */
static void update_fps(fb_compositor_t *comp) {
    comp->frame_count++;

    uint64_t now = get_time_us();
    uint64_t elapsed = now - comp->last_fps_update;

    // Update FPS every second
    if (elapsed >= 1000000) {
        comp->fps = (uint32_t)((comp->frame_count * 1000000) / elapsed);
        comp->frame_count = 0;
        comp->last_fps_update = now;

        printf("[FB Compositor] FPS: %u\n", comp->fps);
    }
}

/**
 * Wait for VSync (vertical blank interval)
 * This is a software-based VSync simulation using precise timing.
 * For 60Hz display: wait until start of next 16.67ms frame window.
 */
static void wait_vsync(fb_compositor_t *comp) {
    if (!comp->vsync_enabled) {
        return;  // VSync disabled, no waiting
    }

    // Target frame time for 60 FPS: 16666.67 microseconds
    const uint64_t FRAME_TIME_60HZ = 16667;

    uint64_t now = get_time_us();
    uint64_t elapsed_since_last_vsync = now - comp->last_vsync_time;

    // If we're within the VSync window, wait for next one
    if (elapsed_since_last_vsync < FRAME_TIME_60HZ) {
        uint64_t wait_time = FRAME_TIME_60HZ - elapsed_since_last_vsync;
        usleep(wait_time);
    }

    // Update last VSync time
    comp->last_vsync_time = get_time_us();
}

/**
 * Swap back buffer to front buffer with VSync synchronization
 * This implements the page flip operation.
 */
static void swap_buffers(fb_compositor_t *comp) {
    // Wait for VSync before swapping to eliminate tearing
    wait_vsync(comp);

    // Perform the swap - copy back buffer to front buffer
    // This is the atomic page flip operation
    // In a real implementation with hardware support, this would be a pointer swap
    memcpy(comp->fb->pixels, comp->back_buffer,
           comp->fb->width * comp->fb->height * sizeof(uint32_t));
}

/**
 * Main compositor frame - called once per frame
 */
void fb_compositor_frame(fb_compositor_t *comp) {
    if (!comp) return;

    uint64_t frame_start = get_time_us();

    // Clear back buffer if full redraw
    if (comp->damage.full_redraw) {
        memset(comp->back_buffer, 0,
               comp->fb->width * comp->fb->height * sizeof(uint32_t));
    }

    // Composite all windows to back buffer
    composite_windows(comp);

    // Draw cursor
    if (comp->cursor_visible) {
        draw_cursor(comp->back_buffer, comp->fb->width, comp->fb->height,
                   comp->cursor_x, comp->cursor_y);
    }

    // Swap buffers with VSync synchronization (eliminates tearing)
    swap_buffers(comp);

    // Clear damage regions for next frame
    damage_clear(&comp->damage);

    // Update FPS counter
    update_fps(comp);

    // Track frame timing for variance measurement
    comp->last_frame_time = get_time_us() - frame_start;
}

/**
 * Add window to compositor
 */
int fb_compositor_add_window(fb_compositor_t *comp, window_t *window) {
    if (!comp || !window) return -1;

    if (comp->window_count >= MAX_WINDOWS) {
        fprintf(stderr, "[FB Compositor] Maximum window count reached\n");
        return -1;
    }

    comp->windows[comp->window_count++] = window;

    // Mark window area as damaged
    damage_add_region(&comp->damage, &window->geometry);

    printf("[FB Compositor] Added window %u (%ux%u at %d,%d)\n",
           window->id, window->geometry.width, window->geometry.height,
           window->geometry.x, window->geometry.y);

    return 0;
}

/**
 * Remove window from compositor
 */
void fb_compositor_remove_window(fb_compositor_t *comp, uint32_t window_id) {
    if (!comp) return;

    for (uint32_t i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]->id == window_id) {
            // Mark window area as damaged
            damage_add_region(&comp->damage, &comp->windows[i]->geometry);

            // Destroy window
            window_destroy(comp->windows[i]);

            // Shift remaining windows
            for (uint32_t j = i; j < comp->window_count - 1; j++) {
                comp->windows[j] = comp->windows[j + 1];
            }
            comp->window_count--;

            printf("[FB Compositor] Removed window %u\n", window_id);
            return;
        }
    }
}

/**
 * Find window by ID
 */
window_t *fb_compositor_find_window(fb_compositor_t *comp, uint32_t window_id) {
    if (!comp) return NULL;

    for (uint32_t i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]->id == window_id) {
            return comp->windows[i];
        }
    }

    return NULL;
}

/**
 * Raise window to top of stack
 */
void fb_compositor_raise_window(fb_compositor_t *comp, uint32_t window_id) {
    if (!comp) return;

    window_t *window = fb_compositor_find_window(comp, window_id);
    if (!window) return;

    // Find highest z-order
    uint32_t max_z = 0;
    for (uint32_t i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]->z_order > max_z) {
            max_z = comp->windows[i]->z_order;
        }
    }

    // Set window to top
    window->z_order = max_z + 1;
    damage_add_region(&comp->damage, &window->geometry);
}

/**
 * Lower window to bottom of stack
 */
void fb_compositor_lower_window(fb_compositor_t *comp, uint32_t window_id) {
    if (!comp) return;

    window_t *window = fb_compositor_find_window(comp, window_id);
    if (!window) return;

    // Find lowest z-order
    uint32_t min_z = UINT32_MAX;
    for (uint32_t i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]->z_order < min_z) {
            min_z = comp->windows[i]->z_order;
        }
    }

    // Set window to bottom
    if (min_z > 0) {
        window->z_order = min_z - 1;
    } else {
        window->z_order = 0;
    }
    damage_mark_full_redraw(&comp->damage);
}

/**
 * Set cursor position
 */
void fb_compositor_set_cursor_position(fb_compositor_t *comp, int32_t x, int32_t y) {
    if (!comp) return;

    // Mark old and new cursor positions as damaged
    rect_t old_cursor = { comp->cursor_x, comp->cursor_y, 16, 16 };
    rect_t new_cursor = { x, y, 16, 16 };
    damage_add_region(&comp->damage, &old_cursor);
    damage_add_region(&comp->damage, &new_cursor);

    comp->cursor_x = x;
    comp->cursor_y = y;
}

/**
 * Get cursor position
 */
void fb_compositor_get_cursor_position(fb_compositor_t *comp, int32_t *x, int32_t *y) {
    if (!comp) return;
    if (x) *x = comp->cursor_x;
    if (y) *y = comp->cursor_y;
}

/**
 * Set cursor visibility
 */
void fb_compositor_set_cursor_visible(fb_compositor_t *comp, bool visible) {
    if (!comp) return;
    comp->cursor_visible = visible;
    rect_t cursor = { comp->cursor_x, comp->cursor_y, 16, 16 };
    damage_add_region(&comp->damage, &cursor);
}

/**
 * Enable/disable alpha blending
 */
void fb_compositor_set_alpha_blending(fb_compositor_t *comp, bool enabled) {
    if (!comp) return;
    comp->use_alpha_blending = enabled;
    damage_mark_full_redraw(&comp->damage);
}

/**
 * Enable/disable damage tracking
 */
void fb_compositor_set_damage_tracking(fb_compositor_t *comp, bool enabled) {
    if (!comp) return;
    comp->use_damage_tracking = enabled;
    if (!enabled) {
        damage_mark_full_redraw(&comp->damage);
    }
}

/**
 * Get current FPS
 */
uint32_t fb_compositor_get_fps(fb_compositor_t *comp) {
    return comp ? comp->fps : 0;
}

/**
 * Enable/disable VSync
 */
void fb_compositor_set_vsync(fb_compositor_t *comp, bool enabled) {
    if (!comp) return;
    comp->vsync_enabled = enabled;
    printf("[FB Compositor] VSync: %s\n", enabled ? "enabled" : "disabled");
}

/**
 * Get last frame time in microseconds
 */
uint64_t fb_compositor_get_frame_time(fb_compositor_t *comp) {
    return comp ? comp->last_frame_time : 0;
}

/**
 * Check if two rectangles intersect
 */
bool rect_intersects(const rect_t *a, const rect_t *b) {
    return !(a->x + a->width <= b->x ||
             b->x + b->width <= a->x ||
             a->y + a->height <= b->y ||
             b->y + b->height <= a->y);
}

/**
 * Union of two rectangles
 */
void rect_union(rect_t *result, const rect_t *a, const rect_t *b) {
    int32_t x1 = (a->x < b->x) ? a->x : b->x;
    int32_t y1 = (a->y < b->y) ? a->y : b->y;
    int32_t x2 = ((a->x + a->width) > (b->x + b->width)) ?
                 (a->x + a->width) : (b->x + b->width);
    int32_t y2 = ((a->y + a->height) > (b->y + b->height)) ?
                 (a->y + a->height) : (b->y + b->height);

    result->x = x1;
    result->y = y1;
    result->width = x2 - x1;
    result->height = y2 - y1;
}

/**
 * Check if rectangle contains point
 */
bool rect_contains_point(const rect_t *rect, int32_t x, int32_t y) {
    return (x >= rect->x && x < rect->x + rect->width &&
            y >= rect->y && y < rect->y + rect->height);
}
