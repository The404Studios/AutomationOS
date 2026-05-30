/**
 * AutomationOS Window Manager - Focus Management
 *
 * Handles window focus switching, keyboard focus traversal, and focus policies
 */

#include "window_manager.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * Focus window and update compositor
 */
void wm_focus_window(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return;

    window_t *old_focus = wm->focused_window;

    // No change needed
    if (old_focus == window) return;

    // Unfocus previous window
    if (old_focus) {
        old_focus->focused = false;
        printf("[FOCUS] Unfocused window %u\n", old_focus->id);
    }

    // Focus new window
    wm->focused_window = window;
    window->focused = true;

    // Raise to top of stack
    wm_raise_window(wm, window);

    // Notify compositor
    wm_notify_focus_change(wm, old_focus, window);

    printf("[FOCUS] Focused window %u: %s\n", window->id, window->title);
}

/**
 * Unfocus all windows
 */
void wm_unfocus_all(window_manager_t *wm) {
    if (!wm) return;

    if (wm->focused_window) {
        wm->focused_window->focused = false;
        wm_notify_focus_change(wm, wm->focused_window, NULL);
        printf("[FOCUS] Unfocused all windows\n");
        wm->focused_window = NULL;
    }
}

/**
 * Focus next window in workspace
 */
void wm_focus_next(window_manager_t *wm) {
    if (!wm) return;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws || ws->window_count == 0) return;

    // Find current focused window index
    int32_t current = -1;
    for (uint32_t i = 0; i < ws->window_count; i++) {
        if (ws->windows[i] == wm->focused_window) {
            current = i;
            break;
        }
    }

    // Calculate next index (wrap around)
    int32_t next = (current + 1) % ws->window_count;

    // Skip minimized windows
    uint32_t attempts = 0;
    while (ws->windows[next]->minimized && attempts < ws->window_count) {
        next = (next + 1) % ws->window_count;
        attempts++;
    }

    // Focus next window
    if (!ws->windows[next]->minimized) {
        wm_focus_window(wm, ws->windows[next]);
    }
}

/**
 * Focus previous window in workspace
 */
void wm_focus_prev(window_manager_t *wm) {
    if (!wm) return;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws || ws->window_count == 0) return;

    // Find current focused window index
    int32_t current = -1;
    for (uint32_t i = 0; i < ws->window_count; i++) {
        if (ws->windows[i] == wm->focused_window) {
            current = i;
            break;
        }
    }

    // Calculate previous index (wrap around)
    int32_t prev = (current - 1 + ws->window_count) % ws->window_count;

    // Skip minimized windows
    uint32_t attempts = 0;
    while (ws->windows[prev]->minimized && attempts < ws->window_count) {
        prev = (prev - 1 + ws->window_count) % ws->window_count;
        attempts++;
    }

    // Focus previous window
    if (!ws->windows[prev]->minimized) {
        wm_focus_window(wm, ws->windows[prev]);
    }
}

/**
 * Focus window under cursor (click-to-focus)
 */
void wm_focus_at_point(window_manager_t *wm, int32_t x, int32_t y) {
    if (!wm) return;

    window_t *window = wm_window_at(wm, x, y);
    if (window) {
        wm_focus_window(wm, window);
    } else {
        // Clicked on empty desktop - unfocus all
        wm_unfocus_all(wm);
    }
}

/**
 * Focus most recently used window (when current is closed)
 */
void wm_focus_mru(window_manager_t *wm) {
    if (!wm) return;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws || ws->window_count == 0) {
        wm_unfocus_all(wm);
        return;
    }

    // Focus the topmost non-minimized window
    for (int32_t i = ws->window_count - 1; i >= 0; i--) {
        if (!ws->windows[i]->minimized) {
            wm_focus_window(wm, ws->windows[i]);
            return;
        }
    }

    // All windows minimized
    wm_unfocus_all(wm);
}

/**
 * Focus policy: handle window destruction
 * Called when a window is destroyed to update focus
 */
void wm_focus_handle_destroy(window_manager_t *wm, window_t *destroyed_window) {
    if (!wm || !destroyed_window) return;

    // If the destroyed window was focused, focus MRU
    if (wm->focused_window == destroyed_window) {
        wm->focused_window = NULL;
        wm_focus_mru(wm);
    }
}

/**
 * Focus policy: handle window minimization
 */
void wm_focus_handle_minimize(window_manager_t *wm, window_t *minimized_window) {
    if (!wm || !minimized_window) return;

    // If the minimized window was focused, focus MRU
    if (wm->focused_window == minimized_window) {
        wm->focused_window = NULL;
        wm_focus_mru(wm);
    }
}

/**
 * Focus policy: handle new window creation
 */
void wm_focus_handle_create(window_manager_t *wm, window_t *new_window) {
    if (!wm || !new_window) return;

    // Auto-focus new windows (can be made configurable)
    wm_focus_window(wm, new_window);
}

/**
 * Focus policy: handle workspace switch
 */
void wm_focus_handle_workspace_switch(window_manager_t *wm, uint32_t new_workspace_id) {
    if (!wm) return;

    workspace_t *ws = wm->workspaces[new_workspace_id];
    if (!ws) return;

    // Unfocus current window (in old workspace)
    wm_unfocus_all(wm);

    // Focus topmost window in new workspace
    wm_focus_mru(wm);
}

/**
 * Check if window can receive focus
 */
bool wm_can_focus(window_manager_t *wm, window_t *window) {
    if (!wm || !window) return false;

    // Cannot focus minimized windows
    if (window->minimized) return false;

    // Cannot focus unmapped windows
    if (!window->mapped) return false;

    // Some window types don't receive focus (desktop, dock)
    if (window->type == WINDOW_DESKTOP || window->type == WINDOW_DOCK) {
        return false;
    }

    return true;
}

/**
 * Get currently focused window
 */
window_t *wm_get_focused_window(window_manager_t *wm) {
    if (!wm) return NULL;
    return wm->focused_window;
}
