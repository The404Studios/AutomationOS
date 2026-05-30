/**
 * AutomationOS Window Manager - Window Dragging
 *
 * Implements drag-to-move window functionality
 */

#include "window_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/**
 * Start window drag operation
 */
void wm_start_drag(window_manager_t *wm, window_t *window, int32_t start_x, int32_t start_y) {
    if (!wm || !window) return;

    // Cannot drag maximized or fullscreen windows
    if (window->maximized || window->fullscreen) {
        printf("[DRAG] Cannot drag maximized/fullscreen window %u\n", window->id);
        return;
    }

    wm->dragging = true;
    wm->grabbed_window = window;
    wm->drag_start_x = start_x;
    wm->drag_start_y = start_y;
    wm->drag_start_geometry = window->geometry;

    printf("[DRAG] Started dragging window %u from (%d,%d)\n",
           window->id, start_x, start_y);
}

/**
 * Update window position during drag
 */
void wm_update_drag(window_manager_t *wm, int32_t current_x, int32_t current_y) {
    if (!wm || !wm->dragging || !wm->grabbed_window) return;

    // Calculate delta from drag start
    int32_t dx = current_x - wm->drag_start_x;
    int32_t dy = current_y - wm->drag_start_y;

    // Calculate new position
    int32_t new_x = wm->drag_start_geometry.x + dx;
    int32_t new_y = wm->drag_start_geometry.y + dy;

    // Optional: Constrain to screen bounds
    display_t *display = wm->compositor->displays[0];
    if (display) {
        // Keep at least a small part of the window visible
        int32_t min_visible = 50;  // Minimum pixels that must stay visible

        if (new_x + wm->grabbed_window->geometry.w < min_visible) {
            new_x = min_visible - wm->grabbed_window->geometry.w;
        }
        if (new_x > (int32_t)display->width - min_visible) {
            new_x = display->width - min_visible;
        }
        if (new_y < 0) {
            new_y = 0;  // Can't drag above screen
        }
        if (new_y > (int32_t)display->height - min_visible) {
            new_y = display->height - min_visible;
        }
    }

    // Update window position
    wm_move_window(wm, wm->grabbed_window, new_x, new_y);
}

/**
 * End window drag operation
 */
void wm_end_drag(window_manager_t *wm) {
    if (!wm || !wm->dragging) return;

    printf("[DRAG] Ended dragging window %u at (%d,%d)\n",
           wm->grabbed_window->id,
           wm->grabbed_window->geometry.x,
           wm->grabbed_window->geometry.y);

    wm->dragging = false;
    wm->grabbed_window = NULL;
}

/**
 * Cancel window drag operation (restore original position)
 */
void wm_cancel_drag(window_manager_t *wm) {
    if (!wm || !wm->dragging || !wm->grabbed_window) return;

    // Restore original position
    wm_move_window(wm, wm->grabbed_window,
                  wm->drag_start_geometry.x,
                  wm->drag_start_geometry.y);

    printf("[DRAG] Cancelled drag, restored window %u to (%d,%d)\n",
           wm->grabbed_window->id,
           wm->drag_start_geometry.x,
           wm->drag_start_geometry.y);

    wm->dragging = false;
    wm->grabbed_window = NULL;
}

/**
 * Check if currently dragging
 */
bool wm_is_dragging(window_manager_t *wm) {
    if (!wm) return false;
    return wm->dragging;
}

/**
 * Get window being dragged
 */
window_t *wm_get_dragged_window(window_manager_t *wm) {
    if (!wm || !wm->dragging) return NULL;
    return wm->grabbed_window;
}

/**
 * Snap window to screen edges/other windows
 * Called during drag to provide "magnetic" snap behavior
 */
void wm_apply_snap(window_manager_t *wm, window_t *window, int32_t *x, int32_t *y) {
    if (!wm || !window || !x || !y) return;

    #define SNAP_THRESHOLD 10  // Pixels

    display_t *display = wm->compositor->displays[0];
    if (!display) return;

    // Snap to screen edges
    if (*x < SNAP_THRESHOLD && *x > -SNAP_THRESHOLD) {
        *x = 0;  // Snap to left edge
    }
    if (*y < SNAP_THRESHOLD && *y > -SNAP_THRESHOLD) {
        *y = 0;  // Snap to top edge
    }

    int32_t right_edge = display->width - window->geometry.w;
    if (*x > right_edge - SNAP_THRESHOLD && *x < right_edge + SNAP_THRESHOLD) {
        *x = right_edge;  // Snap to right edge
    }

    int32_t bottom_edge = display->height - window->geometry.h;
    if (*y > bottom_edge - SNAP_THRESHOLD && *y < bottom_edge + SNAP_THRESHOLD) {
        *y = bottom_edge;  // Snap to bottom edge
    }

    // Optional: Snap to other windows
    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (ws) {
        for (uint32_t i = 0; i < ws->window_count; i++) {
            window_t *other = ws->windows[i];
            if (other == window || !other->mapped || other->minimized) continue;

            // Check alignment with other window edges
            // Left edge to right edge
            if (*x > other->geometry.x + other->geometry.w - SNAP_THRESHOLD &&
                *x < other->geometry.x + other->geometry.w + SNAP_THRESHOLD) {
                *x = other->geometry.x + other->geometry.w;
            }

            // Right edge to left edge
            if (*x + window->geometry.w > other->geometry.x - SNAP_THRESHOLD &&
                *x + window->geometry.w < other->geometry.x + SNAP_THRESHOLD) {
                *x = other->geometry.x - window->geometry.w;
            }

            // Similar for top/bottom
            if (*y > other->geometry.y + other->geometry.h - SNAP_THRESHOLD &&
                *y < other->geometry.y + other->geometry.h + SNAP_THRESHOLD) {
                *y = other->geometry.y + other->geometry.h;
            }

            if (*y + window->geometry.h > other->geometry.y - SNAP_THRESHOLD &&
                *y + window->geometry.h < other->geometry.y + SNAP_THRESHOLD) {
                *y = other->geometry.y - window->geometry.h;
            }
        }
    }
}

/**
 * Drag window with snap behavior
 */
void wm_update_drag_with_snap(window_manager_t *wm, int32_t current_x, int32_t current_y) {
    if (!wm || !wm->dragging || !wm->grabbed_window) return;

    // Calculate delta from drag start
    int32_t dx = current_x - wm->drag_start_x;
    int32_t dy = current_y - wm->drag_start_y;

    // Calculate new position
    int32_t new_x = wm->drag_start_geometry.x + dx;
    int32_t new_y = wm->drag_start_geometry.y + dy;

    // Apply snapping
    wm_apply_snap(wm, wm->grabbed_window, &new_x, &new_y);

    // Update window position
    wm_move_window(wm, wm->grabbed_window, new_x, new_y);
}

/**
 * Aero Snap: Drag window to screen edge to tile
 * (Like Windows 7+ snap feature)
 */
void wm_check_aero_snap(window_manager_t *wm, int32_t mouse_x, int32_t mouse_y) {
    if (!wm || !wm->dragging || !wm->grabbed_window) return;

    display_t *display = wm->compositor->displays[0];
    if (!display) return;

    window_t *window = wm->grabbed_window;

    #define AERO_SNAP_THRESHOLD 5

    // Check if dragging to left edge
    if (mouse_x < AERO_SNAP_THRESHOLD) {
        // Snap to left half
        window->geometry.x = 0;
        window->geometry.y = 0;
        window->geometry.w = display->width / 2;
        window->geometry.h = display->height;
        wm_calculate_decoration_geometry(wm, window);
        printf("[DRAG] Aero snap: left half\n");
        return;
    }

    // Check if dragging to right edge
    if (mouse_x > (int32_t)display->width - AERO_SNAP_THRESHOLD) {
        // Snap to right half
        window->geometry.x = display->width / 2;
        window->geometry.y = 0;
        window->geometry.w = display->width / 2;
        window->geometry.h = display->height;
        wm_calculate_decoration_geometry(wm, window);
        printf("[DRAG] Aero snap: right half\n");
        return;
    }

    // Check if dragging to top edge
    if (mouse_y < AERO_SNAP_THRESHOLD) {
        // Maximize
        wm_maximize_window(wm, window);
        printf("[DRAG] Aero snap: maximize\n");
        return;
    }
}
