/**
 * AutomationOS Window Manager - Input Handling
 *
 * Handles keyboard and mouse input events, dispatches to windows
 */

#include "window_manager.h"
#include "../libc/syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// Input event types (from kernel)
#define INPUT_EVENT_KEY 0
#define INPUT_EVENT_REL 1
#define INPUT_EVENT_ABS 2

// Relative axes
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8

// Mouse buttons (from kernel input.h)
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

// Keyboard modifiers
#define MOD_SHIFT   (1 << 0)
#define MOD_CTRL    (1 << 1)
#define MOD_ALT     (1 << 2)
#define MOD_SUPER   (1 << 3)

// Common keycodes (Linux input event codes)
#define KEY_TAB     15
#define KEY_F4      62
#define KEY_LEFT    105
#define KEY_RIGHT   106
#define KEY_UP      103
#define KEY_DOWN    108
#define KEY_ESC     1
#define KEY_LEFTCTRL   29
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT    56

/**
 * Handle mouse button press/release
 */
void wm_handle_mouse_button(window_manager_t *wm, int32_t x, int32_t y,
                            uint32_t button, bool pressed) {
    if (!wm) return;

    if (pressed) {
        // Find window under cursor
        window_t *window = wm_window_at(wm, x, y);
        if (window) {
            // Focus window
            wm_focus_window(wm, window);

            // Check if clicking on decorations
            bool on_decoration = wm_hit_test_decorations(wm, window, x, y);

            if (on_decoration && button == 1) {  // Left click on titlebar
                // Check if clicking close button area (top-right corner)
                int32_t close_btn_x = window->frame_geometry.x + window->frame_geometry.w - 32;
                int32_t close_btn_y = window->frame_geometry.y;
                if (x >= close_btn_x && x < close_btn_x + 32 &&
                    y >= close_btn_y && y < close_btn_y + wm->decoration_height) {
                    // Close button clicked
                    wm_close_window(wm, window);
                    return;
                }

                // Check maximize button (next to close)
                int32_t max_btn_x = close_btn_x - 32;
                if (x >= max_btn_x && x < max_btn_x + 32 &&
                    y >= close_btn_y && y < close_btn_y + wm->decoration_height) {
                    // Maximize button clicked
                    wm_maximize_window(wm, window);
                    return;
                }

                // Check minimize button
                int32_t min_btn_x = max_btn_x - 32;
                if (x >= min_btn_x && x < min_btn_x + 32 &&
                    y >= close_btn_y && y < close_btn_y + wm->decoration_height) {
                    // Minimize button clicked
                    wm_minimize_window(wm, window);
                    return;
                }

                // Start dragging window
                wm->dragging = true;
                wm->grabbed_window = window;
                wm->drag_start_x = x;
                wm->drag_start_y = y;
                wm->drag_start_geometry = window->geometry;

                printf("[INPUT] Started dragging window %u\n", window->id);
            } else if (!on_decoration) {
                // Click in window content area - forward to application
                int32_t local_x = x - window->geometry.x;
                int32_t local_y = y - window->geometry.y;

                printf("[INPUT] Mouse click in window %u at (%d,%d)\n",
                       window->id, local_x, local_y);

                // TODO: Send mouse event to application via IPC
            }

            // Right click on titlebar for window menu
            if (on_decoration && button == 3) {  // Right click
                printf("[INPUT] Window menu requested for window %u\n", window->id);
                // TODO: Show window menu (minimize, maximize, close, etc.)
            }

            // Middle click on titlebar to maximize
            if (on_decoration && button == 2) {  // Middle click
                wm_maximize_window(wm, window);
            }
        }
    } else {
        // Button release
        if (wm->dragging) {
            printf("[INPUT] Stopped dragging window %u\n", wm->grabbed_window->id);
        }

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
    } else if (wm->resizing && wm->grabbed_window) {
        // Resize window
        int32_t dx = x - wm->drag_start_x;
        int32_t dy = y - wm->drag_start_y;

        uint32_t new_width = wm->drag_start_geometry.w + dx;
        uint32_t new_height = wm->drag_start_geometry.h + dy;

        // Enforce minimum size
        if (new_width < 100) new_width = 100;
        if (new_height < 100) new_height = 100;

        wm_resize_window(wm, wm->grabbed_window, new_width, new_height);
    } else {
        // Update cursor (change cursor shape over window edges/corners)
        window_t *window = wm_window_at(wm, x, y);
        if (window) {
            // Check if near window edges for resize cursor
            bool near_left = (x - window->frame_geometry.x) < 5;
            bool near_right = (window->frame_geometry.x + window->frame_geometry.w - x) < 5;
            bool near_top = (y - window->frame_geometry.y) < 5;
            bool near_bottom = (window->frame_geometry.y + window->frame_geometry.h - y) < 5;

            if ((near_left || near_right || near_top || near_bottom) &&
                !window->maximized && !window->fullscreen) {
                // TODO: Set resize cursor
                // Determine resize direction from combination of edges
            }
        }

        // Forward motion event to focused window
        if (wm->focused_window && wm->focused_window->mapped) {
            int32_t local_x = x - wm->focused_window->geometry.x;
            int32_t local_y = y - wm->focused_window->geometry.y;

            // Only forward if cursor is within window bounds
            if (local_x >= 0 && local_x < wm->focused_window->geometry.w &&
                local_y >= 0 && local_y < wm->focused_window->geometry.h) {
                // TODO: Send motion event to application via IPC
            }
        }
    }
}

/**
 * Handle keyboard input
 */
void wm_handle_key(window_manager_t *wm, uint32_t key, uint32_t modifiers, bool pressed) {
    if (!wm || !pressed) return;  // Only handle key press, not release

    printf("[INPUT] Key: %u, Modifiers: 0x%x\n", key, modifiers);

    // Window manager shortcuts
    if (modifiers & MOD_ALT) {
        switch (key) {
            case KEY_TAB:
                // Alt+Tab: Cycle through windows
                wm_cycle_windows(wm, (modifiers & MOD_SHIFT) ? -1 : 1);
                return;

            case KEY_F4:
                // Alt+F4: Close focused window
                if (wm->focused_window) {
                    wm_close_window(wm, wm->focused_window);
                }
                return;

            case KEY_ESC:
                // Alt+Esc: Minimize focused window
                if (wm->focused_window) {
                    wm_minimize_window(wm, wm->focused_window);
                }
                return;
        }
    }

    // Super (Windows key) shortcuts for tiling
    if (modifiers & MOD_SUPER) {
        workspace_t *ws = wm->workspaces[wm->active_workspace];

        switch (key) {
            case KEY_LEFT:
                // Super+Left: Tile horizontally
                wm_set_tiling_mode(wm, TILING_HORIZONTAL);
                return;

            case KEY_RIGHT:
                // Super+Right: Tile vertically
                wm_set_tiling_mode(wm, TILING_VERTICAL);
                return;

            case KEY_UP:
                // Super+Up: Maximize focused window
                if (wm->focused_window) {
                    wm_maximize_window(wm, wm->focused_window);
                }
                return;

            case KEY_DOWN:
                // Super+Down: Restore/disable tiling
                wm_set_tiling_mode(wm, TILING_NONE);
                return;

            // Workspace switching (Super+1..9)
            default:
                if (key >= 2 && key <= 10) {  // Number keys 1-9
                    uint32_t workspace_num = key - 2;
                    if (workspace_num < wm->workspace_count) {
                        wm_switch_workspace(wm, workspace_num);
                    }
                    return;
                }
                break;
        }
    }

    // Forward to focused window if no shortcut matched
    if (wm->focused_window && wm->focused_window->mapped) {
        printf("[INPUT] Forwarding key %u to window %u\n", key, wm->focused_window->id);
        // TODO: Send key event to application via IPC
    }
}

/**
 * Cycle through windows (Alt+Tab)
 */
void wm_cycle_windows(window_manager_t *wm, int direction) {
    if (!wm) return;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws || ws->window_count == 0) return;

    // Find current focused window index
    int32_t current_index = -1;
    for (uint32_t i = 0; i < ws->window_count; i++) {
        if (ws->windows[i] == wm->focused_window) {
            current_index = i;
            break;
        }
    }

    // Calculate next index
    int32_t next_index;
    if (current_index < 0) {
        next_index = 0;
    } else {
        next_index = current_index + direction;
        if (next_index < 0) {
            next_index = ws->window_count - 1;
        } else if (next_index >= (int32_t)ws->window_count) {
            next_index = 0;
        }
    }

    // Skip minimized windows
    int32_t attempts = 0;
    while (ws->windows[next_index]->minimized && attempts < (int32_t)ws->window_count) {
        next_index += direction;
        if (next_index < 0) {
            next_index = ws->window_count - 1;
        } else if (next_index >= (int32_t)ws->window_count) {
            next_index = 0;
        }
        attempts++;
    }

    // Focus next window
    if (!ws->windows[next_index]->minimized) {
        wm_focus_window(wm, ws->windows[next_index]);
        printf("[INPUT] Cycled to window %u\n", ws->windows[next_index]->id);
    }
}

/**
 * Handle scroll wheel
 */
void wm_handle_scroll(window_manager_t *wm, int32_t x, int32_t y,
                     int32_t scroll_x, int32_t scroll_y) {
    if (!wm) return;

    // Find window under cursor
    window_t *window = wm_window_at(wm, x, y);
    if (!window) return;

    // Forward scroll event to window
    int32_t local_x = x - window->geometry.x;
    int32_t local_y = y - window->geometry.y;

    printf("[INPUT] Scroll in window %u: (%d,%d)\n", window->id, scroll_x, scroll_y);

    // TODO: Send scroll event to application via IPC
}

/**
 * Handle touch input (for touchscreen support)
 */
void wm_handle_touch(window_manager_t *wm, uint32_t touch_id,
                    int32_t x, int32_t y, bool pressed) {
    if (!wm) return;

    // For now, treat touch as mouse input
    wm_handle_mouse_button(wm, x, y, 1, pressed);

    if (pressed) {
        printf("[INPUT] Touch down at (%d,%d)\n", x, y);
    } else {
        printf("[INPUT] Touch up at (%d,%d)\n", x, y);
    }

    // TODO: Implement gesture recognition for touch
    // - Swipe to switch workspaces
    // - Pinch to zoom/scale windows
    // - Two-finger scroll
}

/**
 * Process input events from kernel
 * Call this in the window manager main loop
 */
void wm_process_input_events(window_manager_t *wm) {
    if (!wm) return;

    // Mouse state tracking (persists between calls)
    static int32_t mouse_x = 400;
    static int32_t mouse_y = 300;
    static uint8_t mouse_buttons = 0;
    static uint32_t kbd_modifiers = 0;

    // Read all pending events
    input_event_t event;
    while (read_event(&event) > 0) {
        switch (event.type) {
            case INPUT_EVENT_KEY:
                // Keyboard or mouse button event
                if (event.code >= BTN_LEFT && event.code <= BTN_MIDDLE) {
                    // Mouse button
                    uint8_t button_bit = 0;
                    if (event.code == BTN_LEFT) button_bit = 1;
                    else if (event.code == BTN_RIGHT) button_bit = 2;
                    else if (event.code == BTN_MIDDLE) button_bit = 4;

                    bool pressed = (event.value != 0);
                    if (pressed) {
                        mouse_buttons |= button_bit;
                    } else {
                        mouse_buttons &= ~button_bit;
                    }

                    wm_handle_mouse_button(wm, mouse_x, mouse_y,
                                         (event.code == BTN_LEFT) ? 1 :
                                         (event.code == BTN_RIGHT) ? 3 : 2,
                                         pressed);
                } else {
                    // Keyboard key
                    bool pressed = (event.value != 0);

                    // Update modifier state
                    if (event.code == KEY_LEFTSHIFT || event.code == KEY_RIGHTSHIFT) {
                        if (pressed) kbd_modifiers |= MOD_SHIFT;
                        else kbd_modifiers &= ~MOD_SHIFT;
                    }
                    if (event.code == KEY_LEFTCTRL) {
                        if (pressed) kbd_modifiers |= MOD_CTRL;
                        else kbd_modifiers &= ~MOD_CTRL;
                    }
                    if (event.code == KEY_LEFTALT) {
                        if (pressed) kbd_modifiers |= MOD_ALT;
                        else kbd_modifiers &= ~MOD_ALT;
                    }

                    wm_handle_key(wm, event.code, kbd_modifiers, pressed);
                }
                break;

            case INPUT_EVENT_REL:
                // Relative movement (mouse)
                if (event.code == REL_X) {
                    mouse_x += event.value;
                    // Clamp to screen bounds
                    if (mouse_x < 0) mouse_x = 0;
                    if (mouse_x >= 800) mouse_x = 799;  // Assume 800x600 for now
                } else if (event.code == REL_Y) {
                    mouse_y += event.value;
                    if (mouse_y < 0) mouse_y = 0;
                    if (mouse_y >= 600) mouse_y = 599;
                } else if (event.code == REL_WHEEL) {
                    // Scroll wheel
                    wm_handle_scroll(wm, mouse_x, mouse_y, 0, event.value);
                }

                // Send motion event after X/Y updates
                if (event.code == REL_X || event.code == REL_Y) {
                    wm_handle_mouse_motion(wm, mouse_x, mouse_y);

                    // Update compositor cursor position
                    if (wm->compositor) {
                        compositor_set_cursor_position(wm->compositor, mouse_x, mouse_y);
                    }
                }
                break;

            case INPUT_EVENT_ABS:
                // Absolute positioning (touchscreen)
                // TODO: Handle absolute positioning
                break;

            default:
                break;
        }
    }
}

/**
 * Get current mouse position
 */
void wm_get_mouse_position(int32_t *x, int32_t *y) {
    // This would query the kernel for current mouse position
    // For now, return static values (handled by event processing)
    static int32_t mouse_x = 400;
    static int32_t mouse_y = 300;
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}
