/**
 * AutomationOS Compositor - GPU-Accelerated Desktop Compositing
 *
 * Features:
 * - Hardware-accelerated rendering (OpenGL/DRM)
 * - VSync synchronization for tear-free display
 * - Damage tracking for efficient redraws
 * - Triple buffering for smooth frame pacing
 * - Window texture management
 * - Multi-monitor support
 * - 60+ FPS target
 *
 * Architecture:
 * - GPU context manages rendering state
 * - Each window is a texture
 * - Compositor blends all windows to framebuffer
 * - Effects pipeline applies visual enhancements
 * - Present with VSync to avoid tearing
 */

#include "compositor.h"
#include "gpu.h"
#include "animations.h"
#include "effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

// Performance monitoring
#define FPS_SAMPLE_INTERVAL 1000000  // 1 second in microseconds
#define TARGET_FRAME_TIME_US 16667   // 60 FPS = 16.67ms per frame

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Initialize compositor
 */
compositor_t *compositor_init(const char *gpu_device) {
    compositor_t *comp = calloc(1, sizeof(compositor_t));
    if (!comp) {
        fprintf(stderr, "Failed to allocate compositor\n");
        return NULL;
    }

    // Initialize GPU context
    comp->gpu = gpu_init(gpu_device);
    if (!comp->gpu) {
        fprintf(stderr, "Failed to initialize GPU\n");
        free(comp);
        return NULL;
    }

    // Default settings
    comp->vsync_enabled = true;
    comp->effects_enabled = true;
    comp->fps = 0;
    comp->window_count = 0;
    comp->display_count = 0;

    // Initialize framebuffers for triple buffering
    for (int i = 0; i < 3; i++) {
        comp->framebuffers[i] = NULL;  // Will be created when displays are added
    }

    comp->current_fb = 0;
    comp->last_fps_update = get_time_us();
    comp->frame_count = 0;

    // Initialize damage tracking
    comp->damage_region_count = 0;

    // Initialize cursor
    comp->cursor_x = 400;
    comp->cursor_y = 300;
    comp->cursor_visible = true;

    printf("[Compositor] Initialized (GPU: %s, VSync: %s)\n",
           gpu_device,
           comp->vsync_enabled ? "enabled" : "disabled");

    return comp;
}

/**
 * Cleanup compositor resources
 */
void compositor_cleanup(compositor_t *comp) {
    if (!comp) return;

    // Cleanup framebuffers
    for (int i = 0; i < 3; i++) {
        if (comp->framebuffers[i]) {
            gpu_destroy_framebuffer(comp->gpu, comp->framebuffers[i]);
        }
    }

    // Cleanup displays
    for (int i = 0; i < comp->display_count; i++) {
        if (comp->displays[i]) {
            display_cleanup(comp->displays[i]);
        }
    }

    // Cleanup windows
    for (int i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]) {
            window_cleanup(comp->gpu, comp->windows[i]);
        }
    }

    // Cleanup GPU context
    if (comp->gpu) {
        gpu_cleanup(comp->gpu);
    }

    free(comp);
    printf("[Compositor] Cleaned up\n");
}

/**
 * Add display to compositor
 */
int compositor_add_display(compositor_t *comp, display_t *display) {
    if (!comp || !display) return -1;
    if (comp->display_count >= MAX_DISPLAYS) {
        fprintf(stderr, "Maximum display count reached\n");
        return -1;
    }

    comp->displays[comp->display_count++] = display;

    // Create framebuffers for this display if not already created
    if (!comp->framebuffers[0]) {
        for (int i = 0; i < 3; i++) {
            comp->framebuffers[i] = gpu_create_framebuffer(
                comp->gpu,
                display->width,
                display->height
            );
            if (!comp->framebuffers[i]) {
                fprintf(stderr, "Failed to create framebuffer %d\n", i);
                return -1;
            }
        }
    }

    printf("[Compositor] Added display: %dx%d @ %dHz\n",
           display->width, display->height, display->refresh_rate);

    return 0;
}

/**
 * Add window to compositor
 */
int compositor_add_window(compositor_t *comp, window_t *window) {
    if (!comp || !window) return -1;
    if (comp->window_count >= MAX_WINDOWS) {
        fprintf(stderr, "Maximum window count reached\n");
        return -1;
    }

    // Upload window surface to GPU texture
    if (window->surface && !window->texture) {
        window->texture = gpu_upload_texture(
            comp->gpu,
            window->surface->pixels,
            window->surface->width,
            window->surface->height
        );
        if (!window->texture) {
            fprintf(stderr, "Failed to upload window texture\n");
            return -1;
        }
    }

    comp->windows[comp->window_count++] = window;

    // Mark entire window area as damaged
    compositor_add_damage(comp, &window->geometry);

    printf("[Compositor] Added window %u (%dx%d)\n",
           window->id, window->geometry.w, window->geometry.h);

    return 0;
}

/**
 * Remove window from compositor
 */
void compositor_remove_window(compositor_t *comp, uint32_t window_id) {
    if (!comp) return;

    for (int i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]->id == window_id) {
            // Mark window area as damaged before removal
            compositor_add_damage(comp, &comp->windows[i]->geometry);

            // Cleanup window
            window_cleanup(comp->gpu, comp->windows[i]);

            // Shift remaining windows
            for (int j = i; j < comp->window_count - 1; j++) {
                comp->windows[j] = comp->windows[j + 1];
            }
            comp->window_count--;

            printf("[Compositor] Removed window %u\n", window_id);
            return;
        }
    }
}

/**
 * Add damage region for efficient redraws
 */
void compositor_add_damage(compositor_t *comp, const rect_t *rect) {
    if (!comp || !rect) return;
    if (comp->damage_region_count >= MAX_DAMAGE_REGIONS) {
        // Too many damage regions, mark full redraw
        comp->full_redraw = true;
        return;
    }

    comp->damage_regions[comp->damage_region_count++] = *rect;
}

/**
 * Clear all damage regions
 */
static void clear_damage(compositor_t *comp) {
    comp->damage_region_count = 0;
    comp->full_redraw = false;
}

/**
 * Check if region is damaged
 */
static bool is_region_damaged(compositor_t *comp, const rect_t *rect) {
    if (comp->full_redraw) return true;

    for (int i = 0; i < comp->damage_region_count; i++) {
        if (rect_intersects(rect, &comp->damage_regions[i])) {
            return true;
        }
    }

    return false;
}

/**
 * Render single window
 */
static void render_window(compositor_t *comp, window_t *window) {
    if (!window->mapped || window->minimized) return;

    // Update texture if surface is dirty
    if (window->surface && window->surface->dirty && window->texture) {
        gpu_update_texture(
            comp->gpu,
            window->texture,
            window->surface->pixels,
            window->surface->width,
            window->surface->height
        );
        window->surface->dirty = false;
    }

    // Calculate source and destination rectangles
    rect_t src_rect = { 0, 0, window->geometry.w, window->geometry.h };
    rect_t dst_rect = window->geometry;

    // Apply animations (scale, position, alpha)
    float alpha = 1.0f;
    if (window->animation) {
        animation_update(window->animation);
        if (window->animation->type == ANIM_FADE) {
            alpha = window->animation->current;
        }
        // Scale and position animations handled by animation system
    }

    // Apply dimming effect for unfocused windows
    if (comp->effects_enabled && !window->focused) {
        alpha *= 0.85f;  // Dim to 85%
    }

    // Draw window texture
    if (window->texture) {
        gpu_draw_textured_quad(
            comp->gpu,
            window->texture,
            &src_rect,
            &dst_rect,
            alpha
        );
    }

    // Draw window decorations if enabled
    if (window->type == WINDOW_NORMAL && !window->fullscreen) {
        render_window_decorations(comp, window);
    }

    // Apply effects (shadows, blur, etc.)
    if (comp->effects_enabled) {
        apply_window_effects(comp, window);
    }
}

/**
 * Main render loop - compose all windows to framebuffer
 */
static void compositor_render_frame(compositor_t *comp) {
    // Get next framebuffer (triple buffering)
    framebuffer_t *fb = comp->framebuffers[comp->current_fb];

    // Begin rendering to framebuffer
    gpu_begin_frame(comp->gpu, fb);

    // Clear background (only if full redraw or no damage tracking)
    if (comp->full_redraw || comp->damage_region_count == 0) {
        gpu_clear(comp->gpu, 0.1f, 0.1f, 0.15f, 1.0f);  // Dark blue-grey
    }

    // Render windows in back-to-front order (painter's algorithm)
    for (int i = 0; i < comp->window_count; i++) {
        window_t *window = comp->windows[i];

        // Skip if window is not damaged (optimization)
        if (!is_region_damaged(comp, &window->geometry)) {
            continue;
        }

        render_window(comp, window);
    }

    // Apply global effects (e.g., screen dimming, color filters)
    if (comp->effects_enabled) {
        apply_global_effects(comp);
    }

    // End rendering
    gpu_end_frame(comp->gpu);

    // Clear damage regions for next frame
    clear_damage(comp);

    // Advance to next framebuffer
    comp->current_fb = (comp->current_fb + 1) % 3;
}

/**
 * Present framebuffer to display (with VSync)
 */
static void compositor_present(compositor_t *comp) {
    // Present with VSync to avoid tearing
    framebuffer_t *fb = comp->framebuffers[comp->current_fb];
    gpu_present(comp->gpu, fb, comp->vsync_enabled);
}

/**
 * Update FPS counter
 */
static void update_fps(compositor_t *comp) {
    comp->frame_count++;

    uint64_t now = get_time_us();
    uint64_t elapsed = now - comp->last_fps_update;

    if (elapsed >= FPS_SAMPLE_INTERVAL) {
        comp->fps = (uint32_t)((comp->frame_count * 1000000) / elapsed);
        comp->frame_count = 0;
        comp->last_fps_update = now;

        // Log FPS periodically
        printf("[Compositor] FPS: %u\n", comp->fps);
    }
}

/**
 * Main compositor frame - called once per frame
 */
void compositor_frame(compositor_t *comp) {
    if (!comp) return;

    uint64_t frame_start = get_time_us();

    // Update animations
    for (int i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]->animation) {
            animation_update(comp->windows[i]->animation);
            compositor_add_damage(comp, &comp->windows[i]->geometry);
        }
    }

    // Render frame
    compositor_render_frame(comp);

    // Present to display
    compositor_present(comp);

    // Update FPS counter
    update_fps(comp);

    // Frame pacing - sleep if we rendered too fast
    uint64_t frame_time = get_time_us() - frame_start;
    if (frame_time < TARGET_FRAME_TIME_US) {
        usleep(TARGET_FRAME_TIME_US - frame_time);
    }
}

/**
 * Enable/disable VSync
 */
void compositor_set_vsync(compositor_t *comp, bool enabled) {
    if (!comp) return;
    comp->vsync_enabled = enabled;
    printf("[Compositor] VSync: %s\n", enabled ? "enabled" : "disabled");
}

/**
 * Enable/disable effects
 */
void compositor_set_effects(compositor_t *comp, bool enabled) {
    if (!comp) return;
    comp->effects_enabled = enabled;
    printf("[Compositor] Effects: %s\n", enabled ? "enabled" : "disabled");
}

/**
 * Get current FPS
 */
uint32_t compositor_get_fps(compositor_t *comp) {
    return comp ? comp->fps : 0;
}

/**
 * Mark full redraw needed
 */
void compositor_mark_full_redraw(compositor_t *comp) {
    if (comp) {
        comp->full_redraw = true;
    }
}

/**
 * Set cursor position
 */
void compositor_set_cursor_position(compositor_t *comp, int32_t x, int32_t y) {
    if (!comp) return;
    comp->cursor_x = x;
    comp->cursor_y = y;

    // Mark cursor area as damaged for redraw
    rect_t cursor_rect = { x, y, 16, 16 };
    compositor_add_damage(comp, &cursor_rect);
}

/**
 * Get cursor position
 */
void compositor_get_cursor_position(compositor_t *comp, int32_t *x, int32_t *y) {
    if (!comp) return;
    if (x) *x = comp->cursor_x;
    if (y) *y = comp->cursor_y;
}

/**
 * Set cursor visibility
 */
void compositor_set_cursor_visible(compositor_t *comp, bool visible) {
    if (!comp) return;
    comp->cursor_visible = visible;
    compositor_mark_full_redraw(comp);
}
