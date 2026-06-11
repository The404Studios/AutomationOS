/**
 * Window Management Module
 *
 * Window creation, destruction, and surface management.
 */

#include "fb_compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Create a new window
 */
window_t *window_create(uint32_t id, window_type_t type, int32_t x, int32_t y,
                       uint32_t width, uint32_t height) {
    window_t *window = calloc(1, sizeof(window_t));
    if (!window) {
        fprintf(stderr, "[Window] Failed to allocate window\n");
        return NULL;
    }

    window->id = id;
    window->type = type;
    window->geometry.x = x;
    window->geometry.y = y;
    window->geometry.width = width;
    window->geometry.height = height;

    // Calculate frame geometry (includes decorations)
    window->frame_geometry = window->geometry;
    if (type == WINDOW_NORMAL || type == WINDOW_DIALOG) {
        window->frame_geometry.y -= 28;  // Title bar height
        window->frame_geometry.height += 28;
    }

    window->mapped = false;
    window->minimized = false;
    window->maximized = false;
    window->fullscreen = false;
    window->focused = false;
    window->app_id = 0;
    window->z_order = id;  // Default z-order
    window->alpha = 1.0f;

    strcpy(window->title, "Untitled");

    // Allocate surface
    window->surface = calloc(1, sizeof(surface_t));
    if (!window->surface) {
        fprintf(stderr, "[Window] Failed to allocate surface\n");
        free(window);
        return NULL;
    }

    // Cap untrusted dimensions (from the create-window IPC request) to [1, MAX] and use
    // 64-bit arithmetic so width*height can neither overflow the calloc size nor the
    // fill bound. The SHM surface segment is sized for exactly these pixels and is never
    // resized, so remember the capacity for window_update_surface's source clamp.
    if (width  == 0) width  = 1;
    if (height == 0) height = 1;
    if (width  > WINDOW_MAX_DIM) width  = WINDOW_MAX_DIM;
    if (height > WINDOW_MAX_DIM) height = WINDOW_MAX_DIM;
    uint64_t npix = (uint64_t)width * (uint64_t)height;

    window->surface->width = width;
    window->surface->height = height;
    window->surface->pitch = width * 4;
    window->surface->dirty = true;
    window->surface_capacity_pixels = npix;

    // Allocate pixel buffer
    window->surface->pixels = calloc(npix, sizeof(uint32_t));
    if (!window->surface->pixels) {
        fprintf(stderr, "[Window] Failed to allocate pixel buffer\n");
        free(window->surface);
        free(window);
        return NULL;
    }

    // Fill with default color (light gray)
    uint32_t default_color = 0xFFECF0F1;
    for (uint64_t i = 0; i < npix; i++) {
        window->surface->pixels[i] = default_color;
    }

    printf("[Window] Created window %u (%ux%u at %d,%d)\n",
           id, width, height, x, y);

    return window;
}

/**
 * Destroy a window and free resources
 */
void window_destroy(window_t *window) {
    if (!window) return;

    if (window->surface) {
        free(window->surface->pixels);
        free(window->surface);
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
 * Update window surface with new pixel data
 *
 * This is called when an application draws to its window buffer.
 */
void window_update_surface(window_t *window, const uint32_t *pixels,
                          uint32_t width, uint32_t height) {
    if (!window || !pixels) return;

    // Cap untrusted update dimensions (client-controlled SHM header) the same way as
    // create, in 64-bit, so the dest (re)allocation can't overflow.
    if (width  == 0) width  = 1;
    if (height == 0) height = 1;
    if (width  > WINDOW_MAX_DIM) width  = WINDOW_MAX_DIM;
    if (height > WINDOW_MAX_DIM) height = WINDOW_MAX_DIM;
    uint64_t npix = (uint64_t)width * (uint64_t)height;

    // Resize surface if dimensions changed
    if (window->surface->width != width || window->surface->height != height) {
        free(window->surface->pixels);

        window->surface->width = width;
        window->surface->height = height;
        window->surface->pitch = width * 4;

        window->surface->pixels = calloc(npix, sizeof(uint32_t));
        if (!window->surface->pixels) {
            fprintf(stderr, "[Window] Failed to reallocate pixel buffer\n");
            return;
        }

        // Update geometry
        window->geometry.width = width;
        window->geometry.height = height;
    }

    // Copy new pixel data, CLAMPING the SOURCE read to the create-time SHM segment
    // capacity. The segment is never resized, so an update requesting more pixels than
    // it holds (a client enlarging surface->width/height in the shared header) would
    // otherwise read past the end of the mapped segment (OOB read / info-leak across
    // the IPC boundary). Copy at most min(update pixels, segment capacity); the dest
    // (sized to npix) keeps any remaining pixels zero-filled from calloc.
    uint64_t copy_pix = npix;
    if (window->surface_capacity_pixels && copy_pix > window->surface_capacity_pixels)
        copy_pix = window->surface_capacity_pixels;
    memcpy(window->surface->pixels, pixels, copy_pix * sizeof(uint32_t));
    window->surface->dirty = true;
}

/**
 * Create a test window with solid color
 *
 * Helper function for testing.
 */
window_t *window_create_test(uint32_t id, int32_t x, int32_t y,
                            uint32_t width, uint32_t height,
                            uint32_t color) {
    window_t *window = window_create(id, WINDOW_NORMAL, x, y, width, height);
    if (!window) return NULL;

    // Fill with test color
    for (uint32_t i = 0; i < width * height; i++) {
        window->surface->pixels[i] = color;
    }

    window->mapped = true;
    return window;
}
