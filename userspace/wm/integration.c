/**
 * AutomationOS Window Manager - Compositor Integration Layer
 *
 * This file provides the bridge between the window manager and compositor,
 * handling the coordination of window rendering, layout decisions, and
 * synchronization of window state.
 */

#include "window_manager.h"
#include "../compositor/compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Notify compositor of window geometry change
 */
void wm_notify_geometry_change(window_manager_t *wm, window_t *window) {
    if (!wm || !window || !wm->compositor) return;

    // Mark the old and new positions as damaged
    compositor_add_damage(wm->compositor, &window->frame_geometry);

    // The compositor will pick up the new geometry on next frame
    printf("[WM-COMPOSITOR] Notified geometry change for window %u\n", window->id);
}

/**
 * Notify compositor of window mapping change
 */
void wm_notify_mapping_change(window_manager_t *wm, window_t *window, bool mapped) {
    if (!wm || !window || !wm->compositor) return;

    if (mapped) {
        // Add window to compositor's rendering list
        compositor_add_window(wm->compositor, window);
        compositor_add_damage(wm->compositor, &window->frame_geometry);
        printf("[WM-COMPOSITOR] Added window %u to compositor\n", window->id);
    } else {
        // Remove window from compositor's rendering list
        compositor_remove_window(wm->compositor, window->id);
        compositor_add_damage(wm->compositor, &window->frame_geometry);
        printf("[WM-COMPOSITOR] Removed window %u from compositor\n", window->id);
    }
}

/**
 * Notify compositor of focus change
 */
void wm_notify_focus_change(window_manager_t *wm, window_t *old_focus, window_t *new_focus) {
    if (!wm || !wm->compositor) return;

    // Mark both windows as needing redraw (decorations change)
    if (old_focus) {
        compositor_add_damage(wm->compositor, &old_focus->frame_geometry);
    }
    if (new_focus) {
        compositor_add_damage(wm->compositor, &new_focus->frame_geometry);
    }

    printf("[WM-COMPOSITOR] Focus changed from %u to %u\n",
           old_focus ? old_focus->id : 0,
           new_focus ? new_focus->id : 0);
}

/**
 * Request compositor to render decorations for window
 * This is called by compositor during rendering
 */
void wm_render_window_decorations(window_manager_t *wm, window_t *window, uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height) {
    if (!wm || !window || !framebuffer) return;
    if (!wm->decorations_enabled || window->type != WINDOW_NORMAL) return;

    // Decoration geometry
    int32_t dec_x = window->frame_geometry.x;
    int32_t dec_y = window->frame_geometry.y;
    uint32_t dec_w = window->frame_geometry.w;
    uint32_t dec_h = wm->decoration_height;

    // Titlebar background color (different if focused)
    uint32_t bg_color = window->focused ? 0xFF3498DB : 0xFF34495E;  // Blue if focused, dark gray otherwise
    uint32_t text_color = 0xFFECF0F1;  // Light gray text

    // Draw titlebar background
    for (uint32_t y = 0; y < dec_h; y++) {
        if (dec_y + y < 0 || dec_y + y >= (int32_t)fb_height) continue;

        for (uint32_t x = 0; x < dec_w; x++) {
            if (dec_x + x < 0 || dec_x + x >= (int32_t)fb_width) continue;

            uint32_t fb_offset = (dec_y + y) * fb_width + (dec_x + x);
            framebuffer[fb_offset] = bg_color;
        }
    }

    // Draw title text (simple 8x8 bitmap font for now)
    // TODO: Use proper font rendering from Agent 6
    int32_t text_x = dec_x + 8;
    int32_t text_y = dec_y + 12;

    // For now, just draw a simple indicator that this is the title area
    // Real text rendering will be implemented by font system

    // Draw window control buttons (close, minimize, maximize)
    uint32_t btn_size = 16;
    uint32_t btn_spacing = 4;
    uint32_t btn_y_offset = (dec_h - btn_size) / 2;

    // Close button (red)
    int32_t close_x = dec_x + dec_w - btn_size - btn_spacing;
    int32_t close_y = dec_y + btn_y_offset;
    uint32_t close_color = 0xFFE74C3C;  // Red

    for (uint32_t y = 0; y < btn_size; y++) {
        if (close_y + y < 0 || close_y + y >= (int32_t)fb_height) continue;
        for (uint32_t x = 0; x < btn_size; x++) {
            if (close_x + x < 0 || close_x + x >= (int32_t)fb_width) continue;
            uint32_t fb_offset = (close_y + y) * fb_width + (close_x + x);
            framebuffer[fb_offset] = close_color;
        }
    }

    // Maximize button (green)
    int32_t max_x = close_x - btn_size - btn_spacing;
    int32_t max_y = close_y;
    uint32_t max_color = 0xFF2ECC71;  // Green

    for (uint32_t y = 0; y < btn_size; y++) {
        if (max_y + y < 0 || max_y + y >= (int32_t)fb_height) continue;
        for (uint32_t x = 0; x < btn_size; x++) {
            if (max_x + x < 0 || max_x + x >= (int32_t)fb_width) continue;
            uint32_t fb_offset = (max_y + y) * fb_width + (max_x + x);
            framebuffer[fb_offset] = max_color;
        }
    }

    // Minimize button (yellow)
    int32_t min_x = max_x - btn_size - btn_spacing;
    int32_t min_y = max_y;
    uint32_t min_color = 0xFFF39C12;  // Yellow

    for (uint32_t y = 0; y < btn_size; y++) {
        if (min_y + y < 0 || min_y + y >= (int32_t)fb_height) continue;
        for (uint32_t x = 0; x < btn_size; x++) {
            if (min_x + x < 0 || min_x + x >= (int32_t)fb_width) continue;
            uint32_t fb_offset = (min_y + y) * fb_width + (min_x + x);
            framebuffer[fb_offset] = min_color;
        }
    }

    // Draw border around window
    if (wm->border_width > 0) {
        uint32_t border_color = window->focused ? 0xFF3498DB : 0xFF95A5A6;  // Blue if focused, gray otherwise

        // Top border (part of titlebar)
        // Left border
        for (uint32_t y = 0; y < window->frame_geometry.h; y++) {
            if (dec_y + y < 0 || dec_y + y >= (int32_t)fb_height) continue;
            for (uint32_t x = 0; x < wm->border_width; x++) {
                if (dec_x + x < 0 || dec_x + x >= (int32_t)fb_width) continue;
                uint32_t fb_offset = (dec_y + y) * fb_width + (dec_x + x);
                framebuffer[fb_offset] = border_color;
            }
        }

        // Right border
        for (uint32_t y = 0; y < window->frame_geometry.h; y++) {
            if (dec_y + y < 0 || dec_y + y >= (int32_t)fb_height) continue;
            for (uint32_t x = 0; x < wm->border_width; x++) {
                int32_t border_x = dec_x + dec_w - wm->border_width + x;
                if (border_x < 0 || border_x >= (int32_t)fb_width) continue;
                uint32_t fb_offset = (dec_y + y) * fb_width + border_x;
                framebuffer[fb_offset] = border_color;
            }
        }

        // Bottom border
        for (uint32_t x = 0; x < dec_w; x++) {
            if (dec_x + x < 0 || dec_x + x >= (int32_t)fb_width) continue;
            for (uint32_t y = 0; y < wm->border_width; y++) {
                int32_t border_y = dec_y + window->frame_geometry.h - wm->border_width + y;
                if (border_y < 0 || border_y >= (int32_t)fb_height) continue;
                uint32_t fb_offset = border_y * fb_width + (dec_x + x);
                framebuffer[fb_offset] = border_color;
            }
        }
    }
}

/**
 * Query window manager for z-order (rendering order)
 * Returns window list sorted from bottom to top
 */
uint32_t wm_get_window_z_order(window_manager_t *wm, window_t **out_windows, uint32_t max_windows) {
    if (!wm || !out_windows) return 0;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < ws->window_count && count < max_windows; i++) {
        window_t *win = ws->windows[i];
        if (win->mapped && !win->minimized) {
            out_windows[count++] = win;
        }
    }

    return count;
}

/**
 * Query window manager for window geometry
 * Compositor calls this to get current window positions
 */
bool wm_get_window_geometry(window_manager_t *wm, uint32_t window_id, rect_t *geometry, rect_t *frame_geometry) {
    if (!wm) return false;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return false;

    for (uint32_t i = 0; i < ws->window_count; i++) {
        if (ws->windows[i]->id == window_id) {
            if (geometry) *geometry = ws->windows[i]->geometry;
            if (frame_geometry) *frame_geometry = ws->windows[i]->frame_geometry;
            return true;
        }
    }

    return false;
}

/**
 * Compositor integration main loop hook
 * Called by compositor each frame to sync with WM
 */
void wm_compositor_frame_sync(window_manager_t *wm) {
    if (!wm) return;

    // Update window manager state
    wm_update(wm);

    // Update animations
    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (ws) {
        for (uint32_t i = 0; i < ws->window_count; i++) {
            window_t *win = ws->windows[i];
            if (win->animation) {
                // Animation system will update transforms
                // Compositor reads these transforms during rendering
            }
        }
    }

    // No explicit return value needed - compositor reads window state directly
}

/**
 * Sync window geometry to compositor
 * Send IPC message with updated window position/size
 */
void compositor_sync_geometry(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    // Pack geometry data
    struct {
        int32_t x, y;
        uint32_t w, h;
    } geom_data = {
        .x = window->geometry.x,
        .y = window->geometry.y,
        .w = window->geometry.w,
        .h = window->geometry.h
    };

    // Send geometry update via IPC
    wm_ipc_send_event(window->id, "geometry", &geom_data, sizeof(geom_data));

    printf("[WM-COMPOSITOR] Synced geometry for window %u: (%d,%d) %ux%u\n",
           window->id, geom_data.x, geom_data.y, geom_data.w, geom_data.h);
}

/**
 * Set window focus in compositor
 * Notify compositor which window should receive input
 */
void compositor_set_focus(window_manager_t *wm, window_t *window) {
    if (!wm) return;

    uint32_t window_id = window ? window->id : 0;

    // Send focus change via IPC
    wm_ipc_send_event(window_id, "focus", &window_id, sizeof(window_id));

    printf("[WM-COMPOSITOR] Set focus to window %u\n", window_id);
}

/**
 * Request compositor to render window decorations
 * Called during compositor's render pass
 */
void compositor_render_decorations(window_manager_t *wm, window_t *window,
                                   uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height) {
    // This is an alias for the existing function
    wm_render_window_decorations(wm, window, framebuffer, fb_width, fb_height);
}
