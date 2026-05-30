/**
 * AutomationOS Window Manager - Window Resizing
 *
 * Implements window resizing by dragging edges and corners
 */

#include "window_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// Resize direction flags
#define RESIZE_NONE   0
#define RESIZE_LEFT   (1 << 0)
#define RESIZE_RIGHT  (1 << 1)
#define RESIZE_TOP    (1 << 2)
#define RESIZE_BOTTOM (1 << 3)

// Minimum window size
#define MIN_WINDOW_WIDTH 100
#define MIN_WINDOW_HEIGHT 100

/**
 * Determine resize direction from cursor position
 */
static uint32_t get_resize_direction(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window) return RESIZE_NONE;

    // Cannot resize maximized or fullscreen windows
    if (window->maximized || window->fullscreen) return RESIZE_NONE;

    uint32_t direction = RESIZE_NONE;
    rect_t *frame = &window->frame_geometry;

    // Resize hotspot size (8 pixels)
    #define RESIZE_HOTSPOT 8

    // Check edges
    if (x >= frame->x && x < frame->x + RESIZE_HOTSPOT) {
        direction |= RESIZE_LEFT;
    }
    if (x >= frame->x + frame->w - RESIZE_HOTSPOT && x < frame->x + frame->w) {
        direction |= RESIZE_RIGHT;
    }
    if (y >= frame->y && y < frame->y + RESIZE_HOTSPOT) {
        direction |= RESIZE_TOP;
    }
    if (y >= frame->y + frame->h - RESIZE_HOTSPOT && y < frame->y + frame->h) {
        direction |= RESIZE_BOTTOM;
    }

    return direction;
}

/**
 * Start window resize operation
 */
void wm_start_resize(window_manager_t *wm, window_t *window, int32_t start_x, int32_t start_y, uint32_t direction) {
    if (!wm || !window || direction == RESIZE_NONE) return;

    // Cannot resize maximized or fullscreen windows
    if (window->maximized || window->fullscreen) {
        printf("[RESIZE] Cannot resize maximized/fullscreen window %u\n", window->id);
        return;
    }

    wm->resizing = true;
    wm->grabbed_window = window;
    wm->drag_start_x = start_x;
    wm->drag_start_y = start_y;
    wm->drag_start_geometry = window->geometry;

    // Store resize direction in a custom field
    // In a full implementation, add a resize_direction field to window_manager_t
    // For now, we'll use the existing state

    printf("[RESIZE] Started resizing window %u (direction=0x%x) from (%d,%d)\n",
           window->id, direction, start_x, start_y);
}

/**
 * Update window size during resize
 */
void wm_update_resize(window_manager_t *wm, int32_t current_x, int32_t current_y, uint32_t direction) {
    if (!wm || !wm->resizing || !wm->grabbed_window) return;

    window_t *window = wm->grabbed_window;

    // Calculate delta from resize start
    int32_t dx = current_x - wm->drag_start_x;
    int32_t dy = current_y - wm->drag_start_y;

    // Start with original geometry
    int32_t new_x = wm->drag_start_geometry.x;
    int32_t new_y = wm->drag_start_geometry.y;
    uint32_t new_w = wm->drag_start_geometry.w;
    uint32_t new_h = wm->drag_start_geometry.h;

    // Apply resize based on direction
    if (direction & RESIZE_LEFT) {
        new_x += dx;
        new_w -= dx;
    }
    if (direction & RESIZE_RIGHT) {
        new_w += dx;
    }
    if (direction & RESIZE_TOP) {
        new_y += dy;
        new_h -= dy;
    }
    if (direction & RESIZE_BOTTOM) {
        new_h += dy;
    }

    // Enforce minimum size
    if (new_w < MIN_WINDOW_WIDTH) {
        if (direction & RESIZE_LEFT) {
            new_x -= (MIN_WINDOW_WIDTH - new_w);
        }
        new_w = MIN_WINDOW_WIDTH;
    }
    if (new_h < MIN_WINDOW_HEIGHT) {
        if (direction & RESIZE_TOP) {
            new_y -= (MIN_WINDOW_HEIGHT - new_h);
        }
        new_h = MIN_WINDOW_HEIGHT;
    }

    // Apply new geometry
    if (new_x != window->geometry.x || new_y != window->geometry.y) {
        wm_move_window(wm, window, new_x, new_y);
    }
    if (new_w != window->geometry.w || new_h != window->geometry.h) {
        wm_resize_window(wm, window, new_w, new_h);
    }
}

/**
 * End window resize operation
 */
void wm_end_resize(window_manager_t *wm) {
    if (!wm || !wm->resizing) return;

    printf("[RESIZE] Ended resizing window %u to %ux%u\n",
           wm->grabbed_window->id,
           wm->grabbed_window->geometry.w,
           wm->grabbed_window->geometry.h);

    wm->resizing = false;
    wm->grabbed_window = NULL;
}

/**
 * Cancel window resize operation (restore original size)
 */
void wm_cancel_resize(window_manager_t *wm) {
    if (!wm || !wm->resizing || !wm->grabbed_window) return;

    // Restore original geometry
    wm_move_window(wm, wm->grabbed_window,
                  wm->drag_start_geometry.x,
                  wm->drag_start_geometry.y);
    wm_resize_window(wm, wm->grabbed_window,
                    wm->drag_start_geometry.w,
                    wm->drag_start_geometry.h);

    printf("[RESIZE] Cancelled resize, restored window %u to %ux%u\n",
           wm->grabbed_window->id,
           wm->drag_start_geometry.w,
           wm->drag_start_geometry.h);

    wm->resizing = false;
    wm->grabbed_window = NULL;
}

/**
 * Check if currently resizing
 */
bool wm_is_resizing(window_manager_t *wm) {
    if (!wm) return false;
    return wm->resizing;
}

/**
 * Get window being resized
 */
window_t *wm_get_resized_window(window_manager_t *wm) {
    if (!wm || !wm->resizing) return NULL;
    return wm->grabbed_window;
}

/**
 * Get cursor type for resize direction
 * Returns cursor ID for compositor to display
 */
typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_RESIZE_N,
    CURSOR_RESIZE_S,
    CURSOR_RESIZE_E,
    CURSOR_RESIZE_W,
    CURSOR_RESIZE_NE,
    CURSOR_RESIZE_NW,
    CURSOR_RESIZE_SE,
    CURSOR_RESIZE_SW,
} cursor_type_t;

cursor_type_t wm_get_resize_cursor(uint32_t direction) {
    // Determine cursor based on resize direction
    if (direction == (RESIZE_TOP | RESIZE_LEFT)) return CURSOR_RESIZE_NW;
    if (direction == (RESIZE_TOP | RESIZE_RIGHT)) return CURSOR_RESIZE_NE;
    if (direction == (RESIZE_BOTTOM | RESIZE_LEFT)) return CURSOR_RESIZE_SW;
    if (direction == (RESIZE_BOTTOM | RESIZE_RIGHT)) return CURSOR_RESIZE_SE;
    if (direction == RESIZE_TOP) return CURSOR_RESIZE_N;
    if (direction == RESIZE_BOTTOM) return CURSOR_RESIZE_S;
    if (direction == RESIZE_LEFT) return CURSOR_RESIZE_W;
    if (direction == RESIZE_RIGHT) return CURSOR_RESIZE_E;
    return CURSOR_ARROW;
}

/**
 * Handle mouse input for resize detection
 * Call this on mouse button press to check if starting resize
 */
bool wm_handle_resize_input(window_manager_t *wm, window_t *window, int32_t x, int32_t y, bool pressed) {
    if (!wm || !window) return false;

    if (pressed) {
        // Check if clicking on window edge
        uint32_t direction = get_resize_direction(wm, window, x, y);
        if (direction != RESIZE_NONE) {
            wm_start_resize(wm, window, x, y, direction);
            return true;  // Handled as resize
        }
    } else {
        // Mouse release - end resize if active
        if (wm->resizing && wm->grabbed_window == window) {
            wm_end_resize(wm);
            return true;
        }
    }

    return false;
}

/**
 * Update cursor appearance based on hover position
 * Call this on mouse motion to update cursor
 */
void wm_update_resize_cursor(window_manager_t *wm, int32_t x, int32_t y) {
    if (!wm) return;

    // If currently resizing, keep resize cursor
    if (wm->resizing) return;

    // Find window under cursor
    window_t *window = wm_window_at(wm, x, y);
    if (!window) {
        // TODO: Set cursor to arrow
        return;
    }

    // Check if hovering over resize edge
    uint32_t direction = get_resize_direction(wm, window, x, y);
    if (direction != RESIZE_NONE) {
        cursor_type_t cursor = wm_get_resize_cursor(direction);
        // TODO: Set cursor via compositor
        printf("[RESIZE] Cursor over resize edge: cursor=%d\n", cursor);
    }
}

/**
 * Resize window to specific dimensions
 * Helper function for programmatic resizing
 */
void wm_resize_window_to(window_manager_t *wm, window_t *window, uint32_t width, uint32_t height) {
    if (!wm || !window) return;

    // Enforce minimum size
    if (width < MIN_WINDOW_WIDTH) width = MIN_WINDOW_WIDTH;
    if (height < MIN_WINDOW_HEIGHT) height = MIN_WINDOW_HEIGHT;

    wm_resize_window(wm, window, width, height);
}

/**
 * Resize window proportionally (maintain aspect ratio)
 */
void wm_resize_window_proportional(window_manager_t *wm, window_t *window, float scale_factor) {
    if (!wm || !window || scale_factor <= 0.0f) return;

    uint32_t new_w = (uint32_t)(window->geometry.w * scale_factor);
    uint32_t new_h = (uint32_t)(window->geometry.h * scale_factor);

    wm_resize_window_to(wm, window, new_w, new_h);
}
