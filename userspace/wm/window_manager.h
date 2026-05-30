/**
 * AutomationOS Window Manager
 *
 * Manages window placement, focus, decorations, and workspaces
 */

#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../compositor/compositor.h"

/**
 * Window placement modes
 */
typedef enum {
    PLACEMENT_FLOATING,     // Free-form positioning
    PLACEMENT_TILING,       // Automatic tiling
    PLACEMENT_MAXIMIZED,    // Maximized
    PLACEMENT_FULLSCREEN,   // Fullscreen
} placement_mode_t;

/**
 * Tiling modes
 */
typedef enum {
    TILING_NONE,
    TILING_HORIZONTAL,      // Side by side
    TILING_VERTICAL,        // Stacked
    TILING_GRID,            // Grid layout
    TILING_MASTER_STACK,    // Master + stack (i3-style)
} tiling_mode_t;

/**
 * Window rule - per-app configuration
 */
typedef struct {
    char app_name[64];
    window_type_t type;
    placement_mode_t placement;
    bool decorations;
    int32_t workspace;      // -1 for current
} window_rule_t;

/**
 * Workspace
 */
typedef struct {
    uint32_t id;
    char name[64];
    window_t *windows[MAX_WINDOWS];
    uint32_t window_count;
    tiling_mode_t tiling_mode;
} workspace_t;

/**
 * Window Manager
 */
typedef struct window_manager {
    compositor_t *compositor;

    workspace_t *workspaces[16];
    uint32_t workspace_count;
    uint32_t active_workspace;

    window_t *focused_window;
    window_t *grabbed_window;   // Window being dragged/resized

    // Window rules
    window_rule_t rules[64];
    uint32_t rule_count;

    // Settings
    bool decorations_enabled;
    uint32_t decoration_height;
    uint32_t border_width;
    uint32_t gap_size;          // Gap between windows (tiling mode)

    // Drag state
    bool dragging;
    bool resizing;
    int32_t drag_start_x;
    int32_t drag_start_y;
    rect_t drag_start_geometry;
} window_manager_t;

// Core functions
window_manager_t *wm_init(compositor_t *comp);
void wm_cleanup(window_manager_t *wm);
void wm_update(window_manager_t *wm);

// Window management
window_t *wm_create_window(window_manager_t *wm, window_type_t type, uint32_t width, uint32_t height, const char *title);
void wm_destroy_window(window_manager_t *wm, uint32_t window_id);
void wm_map_window(window_manager_t *wm, window_t *window);
void wm_unmap_window(window_manager_t *wm, window_t *window);

// Window operations
void wm_focus_window(window_manager_t *wm, window_t *window);
void wm_raise_window(window_manager_t *wm, window_t *window);
void wm_minimize_window(window_manager_t *wm, window_t *window);
void wm_maximize_window(window_manager_t *wm, window_t *window);
void wm_fullscreen_window(window_manager_t *wm, window_t *window);
void wm_close_window(window_manager_t *wm, window_t *window);

// Window placement
void wm_move_window(window_manager_t *wm, window_t *window, int32_t x, int32_t y);
void wm_resize_window(window_manager_t *wm, window_t *window, uint32_t width, uint32_t height);
void wm_center_window(window_manager_t *wm, window_t *window);

// Window decorations
void wm_draw_decorations(window_manager_t *wm, window_t *window);
bool wm_hit_test_decorations(window_manager_t *wm, window_t *window, int32_t x, int32_t y);

// Workspace management
workspace_t *wm_create_workspace(window_manager_t *wm, const char *name);
void wm_switch_workspace(window_manager_t *wm, uint32_t workspace_id);
void wm_move_window_to_workspace(window_manager_t *wm, window_t *window, uint32_t workspace_id);

// Tiling
void wm_tile_windows(window_manager_t *wm, workspace_t *workspace);
void wm_set_tiling_mode(window_manager_t *wm, tiling_mode_t mode);

// Window rules
void wm_add_rule(window_manager_t *wm, const window_rule_t *rule);
const window_rule_t *wm_find_rule(window_manager_t *wm, const char *app_name);

// Input handling
void wm_handle_mouse_button(window_manager_t *wm, int32_t x, int32_t y, uint32_t button, bool pressed);
void wm_handle_mouse_motion(window_manager_t *wm, int32_t x, int32_t y);
void wm_handle_key(window_manager_t *wm, uint32_t key, uint32_t modifiers, bool pressed);
void wm_handle_scroll(window_manager_t *wm, int32_t x, int32_t y, int32_t scroll_x, int32_t scroll_y);
void wm_handle_touch(window_manager_t *wm, uint32_t touch_id, int32_t x, int32_t y, bool pressed);
void wm_cycle_windows(window_manager_t *wm, int direction);
void wm_process_input_events(window_manager_t *wm);
void wm_get_mouse_position(int32_t *x, int32_t *y);

// Utilities
window_t *wm_window_at(window_manager_t *wm, int32_t x, int32_t y);
void wm_bring_to_front(window_manager_t *wm, window_t *window);

// IPC functions
int wm_ipc_init(window_manager_t *wm, const char *socket_path);
void wm_ipc_cleanup(void);
void wm_ipc_handle_events(window_manager_t *wm);
int wm_ipc_send_event(uint32_t window_id, const char *event_type, const void *data, size_t data_len);
int wm_ipc_connect_compositor(const char *socket_path);
void wm_ipc_sync_windows(int comp_fd, window_manager_t *wm);

// Compositor integration functions (integration.c)
void compositor_sync_geometry(window_manager_t *wm, window_t *window);
void compositor_set_focus(window_manager_t *wm, window_t *window);
void compositor_render_decorations(window_manager_t *wm, window_t *window,
                                   uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height);
void wm_notify_geometry_change(window_manager_t *wm, window_t *window);
void wm_notify_mapping_change(window_manager_t *wm, window_t *window, bool mapped);
void wm_notify_focus_change(window_manager_t *wm, window_t *old_focus, window_t *new_focus);
void wm_render_window_decorations(window_manager_t *wm, window_t *window,
                                  uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height);
uint32_t wm_get_window_z_order(window_manager_t *wm, window_t **out_windows, uint32_t max_windows);
bool wm_get_window_geometry(window_manager_t *wm, uint32_t window_id, rect_t *geometry, rect_t *frame_geometry);
void wm_compositor_frame_sync(window_manager_t *wm);

#endif // WINDOW_MANAGER_H
