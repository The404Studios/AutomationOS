/**
 * Taskbar Module - Header
 */

#ifndef TASKBAR_H
#define TASKBAR_H

#include <stdint.h>
#include <stdbool.h>

typedef struct framebuffer framebuffer_t;
typedef struct app_window app_window_t;

// ============================================================================
// TASKBAR STRUCTURE
// ============================================================================

#define TASKBAR_HEIGHT 40
#define MAX_TASKBAR_BUTTONS 16

typedef struct taskbar_button {
    char title[64];
    uint32_t window_id;
    bool active;
    uint32_t x;
    uint32_t width;
} taskbar_button_t;

typedef struct taskbar {
    framebuffer_t *fb;

    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    uint32_t bg_color;

    // Window buttons
    taskbar_button_t buttons[MAX_TASKBAR_BUTTONS];
    uint32_t button_count;

    // Clock
    char time_str[32];
} taskbar_t;

// ============================================================================
// TASKBAR API
// ============================================================================

taskbar_t *taskbar_init(framebuffer_t *fb, uint32_t screen_width, uint32_t screen_height);
void taskbar_cleanup(taskbar_t *taskbar);
void taskbar_update(taskbar_t *taskbar, app_window_t **windows, uint32_t count);
void taskbar_render(taskbar_t *taskbar);
void taskbar_handle_click(taskbar_t *taskbar, int32_t x, int32_t y);

#endif // TASKBAR_H
