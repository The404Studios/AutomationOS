/**
 * AutomationOS Window Manager - Window Operations
 *
 * Handles window creation, destruction, placement, and manipulation
 */

#include "window_manager.h"
#include "../compositor/animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t g_next_window_id = 1;

/**
 * Create window
 */
window_t *wm_create_window(window_manager_t *wm, window_type_t type,
                           uint32_t width, uint32_t height, const char *title) {
    if (!wm) return NULL;

    // Allocate window
    window_t *window = calloc(1, sizeof(window_t));
    if (!window) {
        fprintf(stderr, "[WM] Failed to allocate window\n");
        return NULL;
    }

    window->id = g_next_window_id++;
    window->type = type;
    window->mapped = false;
    window->minimized = false;
    window->maximized = false;
    window->fullscreen = false;
    window->focused = false;

    // Set title
    strncpy(window->title, title, sizeof(window->title) - 1);

    // Set geometry
    window->geometry.w = width;
    window->geometry.h = height;

    // Center window initially
    display_t *display = wm->compositor->displays[0];
    if (display) {
        window->geometry.x = (display->width - width) / 2;
        window->geometry.y = (display->height - height) / 2;
    }

    // Frame geometry includes decorations
    window->frame_geometry = window->geometry;
    if (wm->decorations_enabled && type == WINDOW_NORMAL) {
        window->frame_geometry.y -= wm->decoration_height;
        window->frame_geometry.h += wm->decoration_height;
    }

    // Create surface
    window->surface = calloc(1, sizeof(surface_t));
    if (window->surface) {
        window->surface->width = width;
        window->surface->height = height;
        window->surface->pixels = calloc(width * height, sizeof(uint32_t));
        window->surface->dirty = true;
    }

    // Add to current workspace
    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (ws && ws->window_count < MAX_WINDOWS) {
        ws->windows[ws->window_count++] = window;
    }

    printf("[WM] Created window %u: %s (%ux%u)\n", window->id, title, width, height);
    return window;
}

/**
 * Destroy window
 */
void wm_destroy_window(window_manager_t *wm, uint32_t window_id) {
    if (!wm) return;

    // Find and remove from workspace
    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return;

    for (uint32_t i = 0; i < ws->window_count; i++) {
        if (ws->windows[i]->id == window_id) {
            window_t *window = ws->windows[i];

            // Play close animation
            window->animation = animation_window_close();
            animation_start(window->animation, 1.0f, 0.0f);

            // Remove from compositor
            compositor_remove_window(wm->compositor, window_id);

            // Free resources
            if (window->surface) {
                if (window->surface->pixels) {
                    free(window->surface->pixels);
                }
                free(window->surface);
            }

            // Shift remaining windows
            for (uint32_t j = i; j < ws->window_count - 1; j++) {
                ws->windows[j] = ws->windows[j + 1];
            }
            ws->window_count--;

            free(window);
            printf("[WM] Destroyed window %u\n", window_id);
            return;
        }
    }
}

/**
 * Map window (make visible)
 */
void wm_map_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    window->mapped = true;

    // Add to compositor
    compositor_add_window(wm->compositor, window);

    // Play open animation
    window->animation = animation_window_open();
    animation_start(window->animation, 0.8f, 1.0f);

    // Focus window
    wm_focus_window(wm, window);

    printf("[WM] Mapped window %u\n", window->id);
}

/**
 * Unmap window (hide)
 */
void wm_unmap_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    window->mapped = false;
    compositor_remove_window(wm->compositor, window->id);

    printf("[WM] Unmapped window %u\n", window->id);
}

/**
 * Focus window
 */
void wm_focus_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    // Unfocus previous window
    if (wm->focused_window) {
        wm->focused_window->focused = false;
        compositor_add_damage(wm->compositor, &wm->focused_window->frame_geometry);
    }

    // Focus new window
    wm->focused_window = window;
    window->focused = true;

    // Raise to top
    wm_raise_window(wm, window);

    // Mark window as needing redraw
    compositor_add_damage(wm->compositor, &window->frame_geometry);

    printf("[WM] Focused window %u\n", window->id);
}

/**
 * Raise window to top of stack
 */
void wm_raise_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return;

    // Find window in stack
    int32_t index = -1;
    for (uint32_t i = 0; i < ws->window_count; i++) {
        if (ws->windows[i] == window) {
            index = i;
            break;
        }
    }

    if (index < 0) return;

    // Move to end of array (top of stack)
    for (int32_t i = index; i < (int32_t)ws->window_count - 1; i++) {
        ws->windows[i] = ws->windows[i + 1];
    }
    ws->windows[ws->window_count - 1] = window;
}

/**
 * Minimize window
 */
void wm_minimize_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    window->minimized = true;

    // Play minimize animation
    window->animation = animation_minimize();
    animation_start(window->animation, 0.0f, 1.0f);

    // Unmap window
    wm_unmap_window(wm, window);

    printf("[WM] Minimized window %u\n", window->id);
}

/**
 * Maximize window
 */
void wm_maximize_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    if (window->maximized) {
        // Restore
        window->maximized = false;
        // Restore previous geometry (stored in app_id field as hack)
    } else {
        // Maximize
        window->maximized = true;

        // Save current geometry (using app_id as storage - hack for now)
        // In real implementation, use a separate saved_geometry field

        // Get display size
        display_t *display = wm->compositor->displays[0];
        if (display) {
            window->geometry.x = 0;
            window->geometry.y = wm->decoration_height;
            window->geometry.w = display->width;
            window->geometry.h = display->height - wm->decoration_height;

            window->frame_geometry.x = 0;
            window->frame_geometry.y = 0;
            window->frame_geometry.w = display->width;
            window->frame_geometry.h = display->height;
        }

        // Play maximize animation
        window->animation = animation_maximize();
        animation_start(window->animation, 0.0f, 1.0f);
    }

    compositor_add_damage(wm->compositor, &window->frame_geometry);
    printf("[WM] %s window %u\n", window->maximized ? "Maximized" : "Restored", window->id);
}

/**
 * Fullscreen window
 */
void wm_fullscreen_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    window->fullscreen = !window->fullscreen;

    if (window->fullscreen) {
        // Get display size
        display_t *display = wm->compositor->displays[0];
        if (display) {
            window->geometry.x = 0;
            window->geometry.y = 0;
            window->geometry.w = display->width;
            window->geometry.h = display->height;
            window->frame_geometry = window->geometry;
        }
    }

    compositor_add_damage(wm->compositor, &window->frame_geometry);
    printf("[WM] %s window %u\n", window->fullscreen ? "Fullscreen" : "Windowed", window->id);
}

/**
 * Close window
 */
void wm_close_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    // Send close request to application via IPC
    // Application should call wm_destroy_window when ready

    printf("[WM] Close requested for window %u\n", window->id);

    // For now, just destroy immediately
    wm_destroy_window(wm, window->id);
}

/**
 * Move window
 */
void wm_move_window(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window) return;

    // Mark old position as damaged
    compositor_add_damage(wm->compositor, &window->frame_geometry);

    window->geometry.x = x;
    window->geometry.y = y;

    // Update frame geometry
    window->frame_geometry.x = x;
    window->frame_geometry.y = y - wm->decoration_height;

    // Mark new position as damaged
    compositor_add_damage(wm->compositor, &window->frame_geometry);
}

/**
 * Resize window
 */
void wm_resize_window(window_manager_t *wm, window_t *window, uint32_t width, uint32_t height) {
    if (!wm || !window) return;

    compositor_add_damage(wm->compositor, &window->frame_geometry);

    window->geometry.w = width;
    window->geometry.h = height;

    window->frame_geometry.w = width;
    window->frame_geometry.h = height + wm->decoration_height;

    // Resize surface
    if (window->surface) {
        free(window->surface->pixels);
        window->surface->width = width;
        window->surface->height = height;
        window->surface->pixels = calloc(width * height, sizeof(uint32_t));
        window->surface->dirty = true;
    }

    compositor_add_damage(wm->compositor, &window->frame_geometry);
}

/**
 * Center window on screen
 */
void wm_center_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    display_t *display = wm->compositor->displays[0];
    if (!display) return;

    int32_t x = (display->width - window->geometry.w) / 2;
    int32_t y = (display->height - window->geometry.h) / 2;

    wm_move_window(wm, window, x, y);
}

/**
 * Find window at coordinates
 */
window_t *wm_window_at(window_manager_t *wm, int32_t x, int32_t y) {
    if (!wm) return NULL;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return NULL;

    // Search top to bottom
    for (int32_t i = ws->window_count - 1; i >= 0; i--) {
        window_t *win = ws->windows[i];
        if (!win->mapped || win->minimized) continue;

        // Check frame geometry (includes decorations)
        if (x >= win->frame_geometry.x && x < win->frame_geometry.x + win->frame_geometry.w &&
            y >= win->frame_geometry.y && y < win->frame_geometry.y + win->frame_geometry.h) {
            return win;
        }
    }

    return NULL;
}

/**
 * Bring window to front
 */
void wm_bring_to_front(window_manager_t *wm, window_t *window) {
    wm_raise_window(wm, window);
}

/**
 * Tile windows in workspace
 */
void wm_tile_windows(window_manager_t *wm, workspace_t *ws) {
    if (!wm || !ws || ws->window_count == 0) return;

    display_t *display = wm->compositor->displays[0];
    if (!display) return;

    uint32_t usable_width = display->width - (ws->window_count + 1) * wm->gap_size;
    uint32_t usable_height = display->height - (ws->window_count + 1) * wm->gap_size;

    switch (ws->tiling_mode) {
        case TILING_HORIZONTAL: {
            // Side by side
            uint32_t width = usable_width / ws->window_count;
            for (uint32_t i = 0; i < ws->window_count; i++) {
                window_t *win = ws->windows[i];
                win->geometry.x = wm->gap_size + i * (width + wm->gap_size);
                win->geometry.y = wm->gap_size + wm->decoration_height;
                win->geometry.w = width;
                win->geometry.h = usable_height - wm->decoration_height;
                win->frame_geometry.x = win->geometry.x;
                win->frame_geometry.y = win->geometry.y - wm->decoration_height;
                win->frame_geometry.w = win->geometry.w;
                win->frame_geometry.h = win->geometry.h + wm->decoration_height;
            }
            break;
        }

        case TILING_VERTICAL: {
            // Stacked
            uint32_t height = usable_height / ws->window_count;
            for (uint32_t i = 0; i < ws->window_count; i++) {
                window_t *win = ws->windows[i];
                win->geometry.x = wm->gap_size;
                win->geometry.y = wm->gap_size + wm->decoration_height + i * (height + wm->gap_size);
                win->geometry.w = usable_width;
                win->geometry.h = height - wm->decoration_height;
                win->frame_geometry.x = win->geometry.x;
                win->frame_geometry.y = win->geometry.y - wm->decoration_height;
                win->frame_geometry.w = win->geometry.w;
                win->frame_geometry.h = win->geometry.h + wm->decoration_height;
            }
            break;
        }

        case TILING_GRID: {
            // Grid layout
            uint32_t cols = (uint32_t)sqrt(ws->window_count);
            if (cols == 0) cols = 1;
            uint32_t rows = (ws->window_count + cols - 1) / cols;
            uint32_t width = usable_width / cols;
            uint32_t height = usable_height / rows;

            for (uint32_t i = 0; i < ws->window_count; i++) {
                window_t *win = ws->windows[i];
                uint32_t col = i % cols;
                uint32_t row = i / cols;
                win->geometry.x = wm->gap_size + col * (width + wm->gap_size);
                win->geometry.y = wm->gap_size + wm->decoration_height + row * (height + wm->gap_size);
                win->geometry.w = width;
                win->geometry.h = height - wm->decoration_height;
                win->frame_geometry.x = win->geometry.x;
                win->frame_geometry.y = win->geometry.y - wm->decoration_height;
                win->frame_geometry.w = win->geometry.w;
                win->frame_geometry.h = win->geometry.h + wm->decoration_height;
            }
            break;
        }

        case TILING_MASTER_STACK: {
            // Master window + stack (i3-style)
            if (ws->window_count == 1) {
                // Only one window, use full space
                window_t *win = ws->windows[0];
                win->geometry.x = wm->gap_size;
                win->geometry.y = wm->gap_size + wm->decoration_height;
                win->geometry.w = usable_width;
                win->geometry.h = usable_height - wm->decoration_height;
            } else {
                // Master takes left half, rest stack on right
                uint32_t master_width = usable_width / 2;
                uint32_t stack_width = usable_width - master_width - wm->gap_size;
                uint32_t stack_height = usable_height / (ws->window_count - 1);

                // Master window
                window_t *master = ws->windows[0];
                master->geometry.x = wm->gap_size;
                master->geometry.y = wm->gap_size + wm->decoration_height;
                master->geometry.w = master_width;
                master->geometry.h = usable_height - wm->decoration_height;
                master->frame_geometry.x = master->geometry.x;
                master->frame_geometry.y = master->geometry.y - wm->decoration_height;
                master->frame_geometry.w = master->geometry.w;
                master->frame_geometry.h = master->geometry.h + wm->decoration_height;

                // Stack windows
                for (uint32_t i = 1; i < ws->window_count; i++) {
                    window_t *win = ws->windows[i];
                    win->geometry.x = wm->gap_size * 2 + master_width;
                    win->geometry.y = wm->gap_size + wm->decoration_height + (i - 1) * (stack_height + wm->gap_size);
                    win->geometry.w = stack_width;
                    win->geometry.h = stack_height - wm->decoration_height;
                    win->frame_geometry.x = win->geometry.x;
                    win->frame_geometry.y = win->geometry.y - wm->decoration_height;
                    win->frame_geometry.w = win->geometry.w;
                    win->frame_geometry.h = win->geometry.h + wm->decoration_height;
                }
            }
            break;
        }

        default:
            break;
    }

    // Mark full redraw
    compositor_mark_full_redraw(wm->compositor);
}

/**
 * Set tiling mode
 */
void wm_set_tiling_mode(window_manager_t *wm, tiling_mode_t mode) {
    if (!wm) return;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return;

    ws->tiling_mode = mode;
    wm_tile_windows(wm, ws);

    printf("[WM] Set tiling mode: %d\n", mode);
}

/**
 * Draw window decorations
 */
void wm_draw_decorations(window_manager_t *wm, window_t *window) {
    if (!wm || !window || !wm->decorations_enabled) return;
    if (window->type != WINDOW_NORMAL) return;

    // TODO: Render titlebar with title text
    // TODO: Render close/minimize/maximize buttons
    // This would render into a separate decoration surface
}

/**
 * Hit test window decorations
 */
bool wm_hit_test_decorations(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window || !wm->decorations_enabled) return false;
    if (window->type != WINDOW_NORMAL) return false;

    // Check if point is in decoration area (titlebar)
    if (y >= window->frame_geometry.y &&
        y < window->geometry.y &&
        x >= window->frame_geometry.x &&
        x < window->frame_geometry.x + window->frame_geometry.w) {
        return true;
    }

    return false;
}
