/**
 * AutomationOS Window Manager Implementation
 */

#include "window_manager.h"
#include "../compositor/animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default settings
#define DEFAULT_DECORATION_HEIGHT 32
#define DEFAULT_BORDER_WIDTH 1
#define DEFAULT_GAP_SIZE 8

static uint32_t g_next_window_id = 1;

/**
 * Initialize window manager
 */
window_manager_t *wm_init(compositor_t *comp) {
    if (!comp) {
        fprintf(stderr, "Window manager requires valid compositor\n");
        return NULL;
    }

    window_manager_t *wm = calloc(1, sizeof(window_manager_t));
    if (!wm) {
        fprintf(stderr, "Failed to allocate window manager\n");
        return NULL;
    }

    wm->compositor = comp;
    wm->workspace_count = 0;
    wm->active_workspace = 0;
    wm->focused_window = NULL;
    wm->grabbed_window = NULL;

    // Settings
    wm->decorations_enabled = true;
    wm->decoration_height = DEFAULT_DECORATION_HEIGHT;
    wm->border_width = DEFAULT_BORDER_WIDTH;
    wm->gap_size = DEFAULT_GAP_SIZE;

    // Drag state
    wm->dragging = false;
    wm->resizing = false;

    // Create default workspace
    workspace_t *ws = wm_create_workspace(wm, "Desktop");
    if (!ws) {
        free(wm);
        return NULL;
    }

    printf("[WM] Initialized\n");
    return wm;
}

/**
 * Cleanup window manager
 */
void wm_cleanup(window_manager_t *wm) {
    if (!wm) return;

    // Cleanup workspaces
    for (uint32_t i = 0; i < wm->workspace_count; i++) {
        if (wm->workspaces[i]) {
            free(wm->workspaces[i]);
        }
    }

    free(wm);
    printf("[WM] Cleaned up\n");
}

/**
 * Update window manager (called per frame)
 */
void wm_update(window_manager_t *wm) {
    if (!wm) return;

    // Update current workspace
    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return;

    // Apply tiling if enabled
    if (ws->tiling_mode != TILING_NONE) {
        wm_tile_windows(wm, ws);
    }
}

/**
 * Create workspace
 */
workspace_t *wm_create_workspace(window_manager_t *wm, const char *name) {
    if (!wm || wm->workspace_count >= 16) return NULL;

    workspace_t *ws = calloc(1, sizeof(workspace_t));
    if (!ws) return NULL;

    ws->id = wm->workspace_count;
    strncpy(ws->name, name, sizeof(ws->name) - 1);
    ws->window_count = 0;
    ws->tiling_mode = TILING_NONE;

    wm->workspaces[wm->workspace_count++] = ws;

    printf("[WM] Created workspace: %s\n", name);
    return ws;
}

/**
 * Switch to workspace
 */
void wm_switch_workspace(window_manager_t *wm, uint32_t workspace_id) {
    if (!wm || workspace_id >= wm->workspace_count) return;

    wm->active_workspace = workspace_id;

    // Unmap windows from old workspace
    // Map windows from new workspace
    workspace_t *ws = wm->workspaces[workspace_id];

    // Apply workspace switch animation
    printf("[WM] Switched to workspace: %s\n", ws->name);
}

/**
 * Create window
 */
window_t *wm_create_window(window_manager_t *wm, window_type_t type, uint32_t width, uint32_t height, const char *title) {
    if (!wm) return NULL;

    // Allocate window
    window_t *window = calloc(1, sizeof(window_t));
    if (!window) {
        fprintf(stderr, "Failed to allocate window\n");
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

    // Apply window rules
    // const window_rule_t *rule = wm_find_rule(wm, app_name);

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

            // Shift remaining windows
            for (uint32_t j = i; j < ws->window_count - 1; j++) {
                ws->windows[j] = ws->windows[j + 1];
            }
            ws->window_count--;

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

    // Notify compositor of mapping change
    wm_notify_mapping_change(wm, window, true);

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

    // Notify compositor of mapping change
    wm_notify_mapping_change(wm, window, false);

    printf("[WM] Unmapped window %u\n", window->id);
}

/**
 * Focus window
 */
void wm_focus_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    window_t *old_focus = wm->focused_window;

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

    // Notify compositor of focus change
    wm_notify_focus_change(wm, old_focus, window);
    compositor_set_focus(wm, window);

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
        // Restore previous geometry
    } else {
        // Maximize
        window->maximized = true;

        // Save current geometry
        rect_t saved_geometry = window->geometry;

        // Get display size
        display_t *display = wm->compositor->displays[0];
        if (display) {
            window->geometry.x = 0;
            window->geometry.y = wm->decoration_height;
            window->geometry.w = display->width;
            window->geometry.h = display->height - wm->decoration_height;
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

    // Send close request to application
    // Application should call wm_destroy_window when ready

    printf("[WM] Close requested for window %u\n", window->id);
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

    // Notify compositor of geometry change
    wm_notify_geometry_change(wm, window);
    compositor_sync_geometry(wm, window);
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

    // Notify compositor of geometry change
    wm_notify_geometry_change(wm, window);
    compositor_sync_geometry(wm, window);
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
            }
            break;
        }

        case TILING_GRID: {
            // Grid layout
            uint32_t cols = (uint32_t)sqrt(ws->window_count);
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
 * Handle mouse button
 */
void wm_handle_mouse_button(window_manager_t *wm, int32_t x, int32_t y, uint32_t button, bool pressed) {
    if (!wm) return;

    if (pressed) {
        // Find window under cursor
        window_t *window = wm_window_at(wm, x, y);
        if (window) {
            // Focus window
            wm_focus_window(wm, window);

            // Check if clicking on decorations
            bool on_decoration = (y < window->geometry.y);

            if (on_decoration && button == 1) {
                // Start dragging
                wm->dragging = true;
                wm->grabbed_window = window;
                wm->drag_start_x = x;
                wm->drag_start_y = y;
                wm->drag_start_geometry = window->geometry;
            }
        }
    } else {
        // Release
        wm->dragging = false;
        wm->resizing = false;
        wm->grabbed_window = NULL;
    }
}

/**
 * Handle mouse motion
 */
void wm_handle_mouse_motion(window_manager_t *wm, int32_t x, int32_t y) {
    if (!wm) return;

    if (wm->dragging && wm->grabbed_window) {
        // Move window
        int32_t dx = x - wm->drag_start_x;
        int32_t dy = y - wm->drag_start_y;
        wm_move_window(wm, wm->grabbed_window,
                      wm->drag_start_geometry.x + dx,
                      wm->drag_start_geometry.y + dy);
    }
}

/**
 * Handle keyboard
 */
void wm_handle_key(window_manager_t *wm, uint32_t key, uint32_t modifiers, bool pressed) {
    if (!wm || !pressed) return;

    // Alt+Tab: Switch windows
    // Alt+F4: Close window
    // Super+arrows: Tiling
    // etc.
}

/**
 * Add window rule
 */
void wm_add_rule(window_manager_t *wm, const window_rule_t *rule) {
    if (!wm || !rule || wm->rule_count >= 64) return;

    wm->rules[wm->rule_count++] = *rule;
    printf("[WM] Added rule for app: %s\n", rule->app_name);
}

/**
 * Find window rule by app name
 */
const window_rule_t *wm_find_rule(window_manager_t *wm, const char *app_name) {
    if (!wm || !app_name) return NULL;

    for (uint32_t i = 0; i < wm->rule_count; i++) {
        if (strcmp(wm->rules[i].app_name, app_name) == 0) {
            return &wm->rules[i];
        }
    }

    return NULL;
}

/**
 * Draw window decorations
 */
void wm_draw_decorations(window_manager_t *wm, window_t *window) {
    if (!wm || !window || !wm->decorations_enabled) return;
    if (window->type != WINDOW_NORMAL) return;

    // TODO: Render titlebar, close button, minimize/maximize buttons
    // This would render into the frame_geometry area above the window content
}

/**
 * Hit test window decorations
 */
bool wm_hit_test_decorations(window_manager_t *wm, window_t *window, int32_t x, int32_t y) {
    if (!wm || !window || !wm->decorations_enabled) return false;
    if (window->type != WINDOW_NORMAL) return false;

    // Check if point is in decoration area
    if (y >= window->frame_geometry.y &&
        y < window->geometry.y &&
        x >= window->frame_geometry.x &&
        x < window->frame_geometry.x + window->frame_geometry.w) {
        return true;
    }

    return false;
}

/**
 * Move window to different workspace
 */
void wm_move_window_to_workspace(window_manager_t *wm, window_t *window, uint32_t workspace_id) {
    if (!wm || !window || workspace_id >= wm->workspace_count) return;

    // Remove from current workspace
    workspace_t *current_ws = wm->workspaces[wm->active_workspace];
    if (current_ws) {
        for (uint32_t i = 0; i < current_ws->window_count; i++) {
            if (current_ws->windows[i] == window) {
                // Shift remaining windows
                for (uint32_t j = i; j < current_ws->window_count - 1; j++) {
                    current_ws->windows[j] = current_ws->windows[j + 1];
                }
                current_ws->window_count--;
                break;
            }
        }
    }

    // Add to target workspace
    workspace_t *target_ws = wm->workspaces[workspace_id];
    if (target_ws && target_ws->window_count < MAX_WINDOWS) {
        target_ws->windows[target_ws->window_count++] = window;
    }

    printf("[WM] Moved window %u to workspace %u\n", window->id, workspace_id);
}

/**
 * Bring window to front (alias for raise_window)
 */
void wm_bring_to_front(window_manager_t *wm, window_t *window) {
    wm_raise_window(wm, window);
}
