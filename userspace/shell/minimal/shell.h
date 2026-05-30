/**
 * Minimal Desktop Shell - Core Header
 */

#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct framebuffer framebuffer_t;
typedef struct desktop desktop_t;
typedef struct taskbar taskbar_t;
typedef struct launcher launcher_t;
typedef struct app_window app_window_t;

// ============================================================================
// SHELL STRUCTURE
// ============================================================================

typedef struct {
    // Display
    framebuffer_t *fb;
    uint32_t screen_width;
    uint32_t screen_height;

    // Components
    desktop_t *desktop;
    taskbar_t *taskbar;
    launcher_t *launcher;

    // Windows
    app_window_t *windows[32];
    uint32_t window_count;

    // State
    bool launcher_open;
    int32_t mouse_x;
    int32_t mouse_y;
    uint32_t mouse_buttons;
} shell_t;

// ============================================================================
// SHELL API
// ============================================================================

shell_t *shell_init(void);
void shell_cleanup(shell_t *shell);
void shell_handle_events(shell_t *shell);
void shell_update(shell_t *shell);
void shell_render(shell_t *shell);

#endif // SHELL_H
