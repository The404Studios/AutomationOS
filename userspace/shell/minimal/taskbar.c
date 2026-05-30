/**
 * Taskbar Module - Implementation
 *
 * Renders taskbar at bottom of screen showing:
 * - Running applications
 * - System clock
 * - System tray icons (future)
 */

#include "taskbar.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Taskbar colors
#define TASKBAR_BG_COLOR     0xE0202020  // Dark gray with transparency
#define TASKBAR_BUTTON_COLOR 0xFF404040  // Button background
#define TASKBAR_TEXT_COLOR   0xFFFFFFFF  // White text
#define TASKBAR_ACTIVE_COLOR 0xFF4A90E2  // Blue for active window

#define BUTTON_PADDING 8
#define BUTTON_MIN_WIDTH 120

// ============================================================================
// INITIALIZATION
// ============================================================================

taskbar_t *taskbar_init(framebuffer_t *fb, uint32_t screen_width, uint32_t screen_height) {
    if (!fb) {
        return NULL;
    }

    taskbar_t *taskbar = calloc(1, sizeof(taskbar_t));
    if (!taskbar) {
        return NULL;
    }

    taskbar->fb = fb;
    taskbar->x = 0;
    taskbar->y = screen_height - TASKBAR_HEIGHT;
    taskbar->width = screen_width;
    taskbar->height = TASKBAR_HEIGHT;
    taskbar->bg_color = TASKBAR_BG_COLOR;
    taskbar->button_count = 0;

    strcpy(taskbar->time_str, "00:00");

    printf("[Taskbar] Initialized at (%u,%u) %ux%u\n",
           taskbar->x, taskbar->y, taskbar->width, taskbar->height);

    return taskbar;
}

// ============================================================================
// CLEANUP
// ============================================================================

void taskbar_cleanup(taskbar_t *taskbar) {
    if (!taskbar) {
        return;
    }

    free(taskbar);
}

// ============================================================================
// UPDATE
// ============================================================================

static void update_clock(taskbar_t *taskbar) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        snprintf(taskbar->time_str, sizeof(taskbar->time_str),
                 "%02d:%02d", tm->tm_hour, tm->tm_min);
    }
}

void taskbar_update(taskbar_t *taskbar, app_window_t **windows, uint32_t count) {
    if (!taskbar) {
        return;
    }

    // Update clock
    update_clock(taskbar);

    // Update window buttons
    taskbar->button_count = 0;

    // TODO: When we have actual app windows, create buttons for them
    // For now, just show placeholders
    (void)windows;
    (void)count;
}

// ============================================================================
// RENDERING
// ============================================================================

void taskbar_render(taskbar_t *taskbar) {
    if (!taskbar || !taskbar->fb) {
        return;
    }

    // Draw taskbar background
    fb_fill_rect(taskbar->fb, taskbar->x, taskbar->y,
                 taskbar->width, taskbar->height,
                 taskbar->bg_color);

    // Draw separator line at top of taskbar
    fb_fill_rect(taskbar->fb, taskbar->x, taskbar->y,
                 taskbar->width, 1, 0xFF505050);

    // Draw window buttons
    uint32_t button_x = BUTTON_PADDING;
    for (uint32_t i = 0; i < taskbar->button_count; i++) {
        taskbar_button_t *btn = &taskbar->buttons[i];

        uint32_t btn_color = btn->active ? TASKBAR_ACTIVE_COLOR : TASKBAR_BUTTON_COLOR;

        fb_fill_rect(taskbar->fb, taskbar->x + button_x, taskbar->y + 5,
                     btn->width, taskbar->height - 10, btn_color);

        // TODO: Draw button text when we have font rendering
        // For now, buttons are just colored rectangles

        button_x += btn->width + BUTTON_PADDING;
    }

    // Draw clock on right side
    uint32_t clock_width = 80;
    uint32_t clock_x = taskbar->width - clock_width - BUTTON_PADDING;

    // Draw clock background
    fb_fill_rect(taskbar->fb, taskbar->x + clock_x, taskbar->y + 5,
                 clock_width, taskbar->height - 10,
                 TASKBAR_BUTTON_COLOR);

    // TODO: Draw clock text when we have font rendering
    // For now, just draw the clock background
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

void taskbar_handle_click(taskbar_t *taskbar, int32_t x, int32_t y) {
    if (!taskbar) {
        return;
    }

    // Check if click is within taskbar bounds
    if (y < (int32_t)taskbar->y || y >= (int32_t)(taskbar->y + taskbar->height)) {
        return;
    }

    // Check which button was clicked
    uint32_t button_x = BUTTON_PADDING;
    for (uint32_t i = 0; i < taskbar->button_count; i++) {
        taskbar_button_t *btn = &taskbar->buttons[i];

        if (x >= (int32_t)(taskbar->x + button_x) &&
            x < (int32_t)(taskbar->x + button_x + btn->width)) {
            printf("[Taskbar] Button %u clicked (window %u)\n", i, btn->window_id);
            // TODO: Switch to clicked window
            break;
        }

        button_x += btn->width + BUTTON_PADDING;
    }
}
