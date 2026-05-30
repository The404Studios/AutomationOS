/**
 * AutomationOS Compositor Utilities
 */

#include "compositor.h"
#include "gpu.h"
#include "animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Check if two rectangles intersect
 */
bool rect_intersects(const rect_t *a, const rect_t *b) {
    if (!a || !b) return false;

    return !(a->x + a->w <= b->x ||
             b->x + b->w <= a->x ||
             a->y + a->h <= b->y ||
             b->y + b->h <= a->y);
}

/**
 * Compute union of two rectangles
 */
void rect_union(rect_t *result, const rect_t *a, const rect_t *b) {
    if (!result || !a || !b) return;

    int32_t x1 = (a->x < b->x) ? a->x : b->x;
    int32_t y1 = (a->y < b->y) ? a->y : b->y;
    int32_t x2 = (a->x + a->w > b->x + b->w) ? a->x + a->w : b->x + b->w;
    int32_t y2 = (a->y + a->h > b->y + b->h) ? a->y + a->h : b->y + b->h;

    result->x = x1;
    result->y = y1;
    result->w = x2 - x1;
    result->h = y2 - y1;
}

/**
 * Create window
 */
window_t *window_create(uint32_t id, window_type_t type, const rect_t *geometry) {
    if (!geometry) return NULL;

    window_t *window = calloc(1, sizeof(window_t));
    if (!window) {
        fprintf(stderr, "Failed to allocate window\n");
        return NULL;
    }

    window->id = id;
    window->type = type;
    window->geometry = *geometry;
    window->frame_geometry = *geometry;
    window->mapped = false;
    window->minimized = false;
    window->maximized = false;
    window->fullscreen = false;
    window->focused = false;

    strcpy(window->title, "Window");

    return window;
}

/**
 * Cleanup window resources
 */
void window_cleanup(gpu_context_t *gpu, window_t *window) {
    if (!window) return;

    // Cleanup texture
    if (window->texture && gpu) {
        gpu_destroy_texture(gpu, window->texture);
    }

    // Cleanup surface
    if (window->surface) {
        if (window->surface->pixels) {
            free(window->surface->pixels);
        }
        free(window->surface);
    }

    // Cleanup animation
    if (window->animation) {
        animation_destroy(window->animation);
    }

    free(window);
}

/**
 * Set window title
 */
void window_set_title(window_t *window, const char *title) {
    if (!window || !title) return;
    strncpy(window->title, title, sizeof(window->title) - 1);
    window->title[sizeof(window->title) - 1] = '\0';
}

/**
 * Update window surface
 */
void window_update_surface(window_t *window, const uint32_t *pixels, uint32_t width, uint32_t height) {
    if (!window || !pixels) return;

    if (!window->surface) {
        window->surface = calloc(1, sizeof(surface_t));
        if (!window->surface) return;
    }

    // Reallocate if size changed
    if (window->surface->width != width || window->surface->height != height) {
        free(window->surface->pixels);
        window->surface->pixels = calloc(width * height, sizeof(uint32_t));
        window->surface->width = width;
        window->surface->height = height;
    }

    // Copy pixels
    memcpy(window->surface->pixels, pixels, width * height * sizeof(uint32_t));
    window->surface->dirty = true;
}

/**
 * Create display
 */
display_t *display_create(uint32_t id, uint32_t width, uint32_t height, uint32_t refresh_rate) {
    display_t *display = calloc(1, sizeof(display_t));
    if (!display) {
        fprintf(stderr, "Failed to allocate display\n");
        return NULL;
    }

    display->id = id;
    display->x = 0;
    display->y = 0;
    display->width = width;
    display->height = height;
    display->refresh_rate = refresh_rate;
    display->primary = false;

    snprintf(display->name, sizeof(display->name), "Display %u", id);

    printf("[Display] Created: %s (%ux%u @ %dHz)\n", display->name, width, height, refresh_rate);
    return display;
}

/**
 * Cleanup display resources
 */
void display_cleanup(display_t *display) {
    if (display) {
        free(display);
    }
}

/**
 * Render window decorations (title bar, buttons, borders)
 */
void render_window_decorations(compositor_t *comp, window_t *window) {
    if (!comp || !window) return;
    if (!window->mapped || window->fullscreen) return;

    // Title bar rectangle
    rect_t title_bar = {
        .x = window->frame_geometry.x,
        .y = window->frame_geometry.y,
        .w = window->frame_geometry.w,
        .h = 32,  // Title bar height
    };

    // Title bar background
    uint32_t title_bar_color = window->focused ? 0xFF3C3C3C : 0xFF2A2A2A;
    gpu_draw_rounded_rect(comp->gpu, &title_bar, 8.0f, title_bar_color);

    // Title text (simplified - real implementation uses text rendering)
    // draw_text(comp->gpu, window->title, title_bar.x + 12, title_bar.y + 8, 0xFFFFFFFF);

    // Close button
    rect_t close_button = {
        .x = title_bar.x + title_bar.w - 40,
        .y = title_bar.y + 4,
        .w = 32,
        .h = 24,
    };
    gpu_draw_rounded_rect(comp->gpu, &close_button, 4.0f, 0xFFE74C3C);

    // Maximize button
    rect_t maximize_button = {
        .x = close_button.x - 36,
        .y = title_bar.y + 4,
        .w = 32,
        .h = 24,
    };
    gpu_draw_rounded_rect(comp->gpu, &maximize_button, 4.0f, 0xFF27AE60);

    // Minimize button
    rect_t minimize_button = {
        .x = maximize_button.x - 36,
        .y = title_bar.y + 4,
        .w = 32,
        .h = 24,
    };
    gpu_draw_rounded_rect(comp->gpu, &minimize_button, 4.0f, 0xFFF39C12);

    // Window border
    rect_t border = window->frame_geometry;
    gpu_draw_rect(comp->gpu, &border, window->focused ? 0xFF0078D7 : 0xFF404040);
}
