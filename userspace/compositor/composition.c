/**
 * Window Composition Engine
 *
 * Implements painter's algorithm for compositing windows.
 */

#include "fb_compositor.h"
#include <stdlib.h>
#include <string.h>

// External blit functions
extern void blit_surface_to_buffer(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                                   const surface_t *surface, const rect_t *dst_rect,
                                   float alpha, bool use_alpha_blend);
extern void blit_fill_rect(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                          const rect_t *rect, uint32_t color);
extern void blit_draw_rounded_rect(uint32_t *dst, uint32_t dst_width, uint32_t dst_height,
                                  const rect_t *rect, uint32_t color, uint32_t thickness);

// Window decoration constants
#define TITLEBAR_HEIGHT 28
#define BORDER_WIDTH 1
#define CORNER_RADIUS 6

// Colors (ARGB32)
#define COLOR_TITLEBAR_ACTIVE     0xFF2C3E50
#define COLOR_TITLEBAR_INACTIVE   0xFF34495E
#define COLOR_TITLEBAR_TEXT       0xFFECF0F1
#define COLOR_BORDER_ACTIVE       0xFF3498DB
#define COLOR_BORDER_INACTIVE     0xFF7F8C8D
#define COLOR_WINDOW_SHADOW       0x40000000

/**
 * Compare windows by z-order (for qsort)
 */
static int compare_windows(const void *a, const void *b) {
    const window_t *win_a = *(const window_t **)a;
    const window_t *win_b = *(const window_t **)b;
    return (int)win_a->z_order - (int)win_b->z_order;
}

/**
 * Draw window decorations (title bar and border)
 */
static void draw_decorations(fb_compositor_t *comp, window_t *window) {
    if (!window->mapped || window->fullscreen) return;
    if (window->type != WINDOW_NORMAL && window->type != WINDOW_DIALOG) return;

    uint32_t *buffer = comp->back_buffer;
    uint32_t width = comp->fb->width;
    uint32_t height = comp->fb->height;

    // Choose colors based on focus
    uint32_t titlebar_color = window->focused ?
                             COLOR_TITLEBAR_ACTIVE : COLOR_TITLEBAR_INACTIVE;
    uint32_t border_color = window->focused ?
                           COLOR_BORDER_ACTIVE : COLOR_BORDER_INACTIVE;

    // Draw title bar
    rect_t titlebar = {
        window->geometry.x,
        window->geometry.y - TITLEBAR_HEIGHT,
        window->geometry.width,
        TITLEBAR_HEIGHT
    };
    blit_fill_rect(buffer, width, height, &titlebar, titlebar_color);

    // Draw border
    rect_t window_with_border = {
        window->geometry.x - BORDER_WIDTH,
        window->geometry.y - TITLEBAR_HEIGHT - BORDER_WIDTH,
        window->geometry.width + 2 * BORDER_WIDTH,
        window->geometry.height + TITLEBAR_HEIGHT + 2 * BORDER_WIDTH
    };
    blit_draw_rounded_rect(buffer, width, height, &window_with_border,
                          border_color, BORDER_WIDTH);

    // TODO: Draw title text (requires font rendering from Agent 6)
    // For now, just draw placeholder title bar
}

/**
 * Draw drop shadow for window
 */
static void draw_shadow(fb_compositor_t *comp, window_t *window) {
    if (!comp->use_alpha_blending) return;
    if (!window->mapped) return;

    uint32_t *buffer = comp->back_buffer;
    uint32_t width = comp->fb->width;
    uint32_t height = comp->fb->height;

    // Simple shadow: offset rectangle with transparency
    int32_t shadow_offset = 4;
    rect_t shadow = {
        window->geometry.x + shadow_offset,
        window->geometry.y + shadow_offset,
        window->geometry.width,
        window->geometry.height
    };

    blit_fill_rect(buffer, width, height, &shadow, COLOR_WINDOW_SHADOW);
}

/**
 * Composite a single window to back buffer
 */
static void composite_window(fb_compositor_t *comp, window_t *window) {
    if (!window->mapped || window->minimized) return;
    if (!window->surface) return;

    // Draw shadow first (behind window)
    draw_shadow(comp, window);

    // Draw decorations
    draw_decorations(comp, window);

    // Blit window surface
    blit_surface_to_buffer(comp->back_buffer,
                          comp->fb->width,
                          comp->fb->height,
                          window->surface,
                          &window->geometry,
                          window->alpha,
                          comp->use_alpha_blending);
}

/**
 * Composite all visible windows using painter's algorithm
 *
 * Renders windows in back-to-front order based on z_order.
 */
void composite_windows(fb_compositor_t *comp) {
    if (!comp) return;

    // Sort windows by z-order (back to front)
    if (comp->window_count > 1) {
        qsort(comp->windows, comp->window_count,
              sizeof(window_t *), compare_windows);
    }

    // Render desktop background (if present)
    for (uint32_t i = 0; i < comp->window_count; i++) {
        if (comp->windows[i]->type == WINDOW_DESKTOP) {
            composite_window(comp, comp->windows[i]);
            break;
        }
    }

    // Render all other windows
    for (uint32_t i = 0; i < comp->window_count; i++) {
        window_t *window = comp->windows[i];

        // Skip desktop (already rendered)
        if (window->type == WINDOW_DESKTOP) continue;

        // Skip if not in damage region (optimization)
        if (comp->use_damage_tracking && !comp->damage.full_redraw) {
            bool damaged = false;
            for (uint32_t j = 0; j < comp->damage.count; j++) {
                if (rect_intersects(&window->geometry, &comp->damage.regions[j])) {
                    damaged = true;
                    break;
                }
            }
            if (!damaged) continue;
        }

        composite_window(comp, window);
    }
}

/**
 * Simple arrow cursor bitmap (16x16)
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
void draw_cursor(uint32_t *buffer, uint32_t width, uint32_t height,
                int32_t x, int32_t y) {
    if (!buffer) return;

    for (int32_t cy = 0; cy < 16; cy++) {
        int32_t screen_y = y + cy;
        if (screen_y < 0 || screen_y >= (int32_t)height) continue;

        for (int32_t cx = 0; cx < 16; cx++) {
            int32_t screen_x = x + cx;
            if (screen_x < 0 || screen_x >= (int32_t)width) continue;

            uint8_t pixel = cursor_bitmap[cy][cx];
            if (pixel == 0) continue;  // Transparent

            uint32_t color;
            if (pixel == 1) {
                color = 0xFFFFFFFF;  // White fill
            } else {
                color = 0xFF000000;  // Black outline
            }

            buffer[screen_y * width + screen_x] = color;
        }
    }
}
