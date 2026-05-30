/**
 * AutomationOS Window Manager - Window Decorations
 *
 * Handles window decorations: title bars, borders, and window control buttons
 * (close, minimize, maximize)
 */

#include "window_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Decoration dimensions
#define TITLEBAR_HEIGHT 32
#define BORDER_WIDTH 1
#define BUTTON_SIZE 16
#define BUTTON_SPACING 4
#define BUTTON_MARGIN 8

// Button positions (from right edge)
#define CLOSE_BTN_OFFSET (BUTTON_MARGIN)
#define MAX_BTN_OFFSET (BUTTON_MARGIN + BUTTON_SIZE + BUTTON_SPACING)
#define MIN_BTN_OFFSET (BUTTON_MARGIN + 2 * (BUTTON_SIZE + BUTTON_SPACING))

/**
 * Calculate decoration geometry for window
 */
void wm_calculate_decoration_geometry(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    if (!wm->decorations_enabled || window->type != WINDOW_NORMAL) {
        // No decorations - frame matches content
        window->frame_geometry = window->geometry;
        return;
    }

    // Frame geometry includes titlebar and borders
    window->frame_geometry.x = window->geometry.x;
    window->frame_geometry.y = window->geometry.y - wm->decoration_height;
    window->frame_geometry.w = window->geometry.w;
    window->frame_geometry.h = window->geometry.h + wm->decoration_height;
}

/**
 * Hit test titlebar area
 */
bool wm_hit_test_titlebar(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window) return false;
    if (!wm->decorations_enabled || window->type != WINDOW_NORMAL) return false;

    // Check if point is in titlebar
    if (y >= window->frame_geometry.y &&
        y < window->geometry.y &&
        x >= window->frame_geometry.x &&
        x < window->frame_geometry.x + window->frame_geometry.w) {
        return true;
    }

    return false;
}

/**
 * Hit test close button
 */
bool wm_hit_test_close_button(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window) return false;
    if (!wm_hit_test_titlebar(wm, window, x, y)) return false;

    int32_t btn_x = window->frame_geometry.x + window->frame_geometry.w - CLOSE_BTN_OFFSET - BUTTON_SIZE;
    int32_t btn_y = window->frame_geometry.y + (wm->decoration_height - BUTTON_SIZE) / 2;

    return (x >= btn_x && x < btn_x + BUTTON_SIZE &&
            y >= btn_y && y < btn_y + BUTTON_SIZE);
}

/**
 * Hit test maximize button
 */
bool wm_hit_test_maximize_button(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window) return false;
    if (!wm_hit_test_titlebar(wm, window, x, y)) return false;

    int32_t btn_x = window->frame_geometry.x + window->frame_geometry.w - MAX_BTN_OFFSET - BUTTON_SIZE;
    int32_t btn_y = window->frame_geometry.y + (wm->decoration_height - BUTTON_SIZE) / 2;

    return (x >= btn_x && x < btn_x + BUTTON_SIZE &&
            y >= btn_y && y < btn_y + BUTTON_SIZE);
}

/**
 * Hit test minimize button
 */
bool wm_hit_test_minimize_button(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window) return false;
    if (!wm_hit_test_titlebar(wm, window, x, y)) return false;

    int32_t btn_x = window->frame_geometry.x + window->frame_geometry.w - MIN_BTN_OFFSET - BUTTON_SIZE;
    int32_t btn_y = window->frame_geometry.y + (wm->decoration_height - BUTTON_SIZE) / 2;

    return (x >= btn_x && x < btn_x + BUTTON_SIZE &&
            y >= btn_y && y < btn_y + BUTTON_SIZE);
}

/**
 * Hit test window borders for resizing
 * Returns resize direction flags
 */
typedef enum {
    RESIZE_NONE = 0,
    RESIZE_LEFT = (1 << 0),
    RESIZE_RIGHT = (1 << 1),
    RESIZE_TOP = (1 << 2),
    RESIZE_BOTTOM = (1 << 3),
} resize_direction_t;

uint32_t wm_hit_test_borders(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window) return RESIZE_NONE;
    if (window->maximized || window->fullscreen) return RESIZE_NONE;

    uint32_t direction = RESIZE_NONE;

    // Border hit zone (8 pixels)
    #define BORDER_HIT_ZONE 8

    rect_t *frame = &window->frame_geometry;

    // Check edges
    if (x >= frame->x && x < frame->x + BORDER_HIT_ZONE) {
        direction |= RESIZE_LEFT;
    }
    if (x >= frame->x + frame->w - BORDER_HIT_ZONE && x < frame->x + frame->w) {
        direction |= RESIZE_RIGHT;
    }
    if (y >= frame->y && y < frame->y + BORDER_HIT_ZONE) {
        direction |= RESIZE_TOP;
    }
    if (y >= frame->y + frame->h - BORDER_HIT_ZONE && y < frame->y + frame->h) {
        direction |= RESIZE_BOTTOM;
    }

    return direction;
}

/**
 * Handle click on window decorations
 * Returns true if click was handled by decorations
 */
bool wm_handle_decoration_click(window_manager_t *wm, window_t *window, int32_t x, int32_t y, uint32_t button) {
    if (!wm || !window || button != 1) return false;  // Only left click

    // Check close button
    if (wm_hit_test_close_button(wm, window, x, y)) {
        wm_close_window(wm, window);
        printf("[DECORATIONS] Close button clicked on window %u\n", window->id);
        return true;
    }

    // Check maximize button
    if (wm_hit_test_maximize_button(wm, window, x, y)) {
        wm_maximize_window(wm, window);
        printf("[DECORATIONS] Maximize button clicked on window %u\n", window->id);
        return true;
    }

    // Check minimize button
    if (wm_hit_test_minimize_button(wm, window, x, y)) {
        wm_minimize_window(wm, window);
        printf("[DECORATIONS] Minimize button clicked on window %u\n", window->id);
        return true;
    }

    // Check if clicking titlebar (for dragging)
    if (wm_hit_test_titlebar(wm, window, x, y)) {
        // Titlebar clicked but not on buttons - ready for drag
        printf("[DECORATIONS] Titlebar clicked on window %u (ready for drag)\n", window->id);
        return true;  // Handled, caller should start drag operation
    }

    return false;  // Not on decorations
}

/**
 * Handle double-click on titlebar (maximize toggle)
 */
void wm_handle_titlebar_doubleclick(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    wm_maximize_window(wm, window);
    printf("[DECORATIONS] Titlebar double-clicked on window %u\n", window->id);
}

/**
 * Enable/disable decorations for window
 */
void wm_set_window_decorations(window_manager_t *wm, window_t *window, bool enabled) {
    if (!wm || !window) return;

    bool was_enabled = wm->decorations_enabled;
    wm->decorations_enabled = enabled;

    // Recalculate frame geometry
    wm_calculate_decoration_geometry(wm, window);

    // Mark for redraw
    compositor_add_damage(wm->compositor, &window->frame_geometry);

    printf("[DECORATIONS] %s decorations for window %u\n",
           enabled ? "Enabled" : "Disabled", window->id);
}

/**
 * Set decoration theme colors
 */
typedef struct {
    uint32_t titlebar_active;
    uint32_t titlebar_inactive;
    uint32_t border_active;
    uint32_t border_inactive;
    uint32_t text_color;
    uint32_t close_btn_color;
    uint32_t max_btn_color;
    uint32_t min_btn_color;
} decoration_theme_t;

static decoration_theme_t g_decoration_theme = {
    .titlebar_active = 0xFF3498DB,    // Blue
    .titlebar_inactive = 0xFF34495E,  // Dark gray
    .border_active = 0xFF3498DB,      // Blue
    .border_inactive = 0xFF95A5A6,    // Gray
    .text_color = 0xFFECF0F1,         // Light gray
    .close_btn_color = 0xFFE74C3C,    // Red
    .max_btn_color = 0xFF2ECC71,      // Green
    .min_btn_color = 0xFFF39C12,      // Yellow
};

void wm_set_decoration_theme(const decoration_theme_t *theme) {
    if (theme) {
        g_decoration_theme = *theme;
        printf("[DECORATIONS] Theme updated\n");
    }
}

const decoration_theme_t *wm_get_decoration_theme(void) {
    return &g_decoration_theme;
}

/**
 * Draw window title text (simple version, will be enhanced by font system)
 */
void wm_draw_window_title(window_manager_t *wm, window_t *window, uint32_t *framebuffer, uint32_t fb_width) {
    if (!wm || !window || !framebuffer) return;

    // Title position (left side of titlebar)
    int32_t text_x = window->frame_geometry.x + 8;
    int32_t text_y = window->frame_geometry.y + (wm->decoration_height - 8) / 2;

    // TODO: Render actual text using font system from Agent 6
    // For now, just mark that this is where the title would be

    printf("[DECORATIONS] Would render title '%s' at (%d,%d)\n",
           window->title, text_x, text_y);
}
