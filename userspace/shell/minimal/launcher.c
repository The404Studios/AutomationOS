/**
 * App Launcher Module - Implementation
 *
 * Provides a simple popup menu to launch applications
 */

#include "launcher.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

// Launcher colors
#define LAUNCHER_BG_COLOR     0xE0F0F0F0  // Light gray with transparency
#define LAUNCHER_ITEM_COLOR   0xFFFFFFFF  // White background
#define LAUNCHER_HOVER_COLOR  0xFF4A90E2  // Blue on hover
#define LAUNCHER_TEXT_COLOR   0xFF202020  // Dark text

#define LAUNCHER_WIDTH  300
#define LAUNCHER_HEIGHT 400
#define ITEM_HEIGHT     50
#define ITEM_PADDING    10

// ============================================================================
// INITIALIZATION
// ============================================================================

launcher_t *launcher_init(framebuffer_t *fb) {
    if (!fb) {
        return NULL;
    }

    launcher_t *launcher = calloc(1, sizeof(launcher_t));
    if (!launcher) {
        return NULL;
    }

    launcher->fb = fb;

    // Center launcher on screen
    launcher->width = LAUNCHER_WIDTH;
    launcher->height = LAUNCHER_HEIGHT;
    launcher->x = (fb->width - launcher->width) / 2;
    launcher->y = (fb->height - launcher->height) / 2;

    launcher->bg_color = LAUNCHER_BG_COLOR;
    launcher->item_count = 0;
    launcher->selected_item = -1;
    launcher->hover_item = -1;

    // Add default applications
    launcher_add_item(launcher, "Terminal", "/usr/bin/terminal");
    launcher_add_item(launcher, "File Manager", "/usr/bin/files");
    launcher_add_item(launcher, "Settings", "/usr/bin/settings");
    launcher_add_item(launcher, "About", "/usr/bin/about");

    printf("[Launcher] Initialized with %u items\n", launcher->item_count);

    return launcher;
}

// ============================================================================
// CLEANUP
// ============================================================================

void launcher_cleanup(launcher_t *launcher) {
    if (!launcher) {
        return;
    }

    free(launcher);
}

// ============================================================================
// ITEMS
// ============================================================================

void launcher_add_item(launcher_t *launcher, const char *name, const char *exec_path) {
    if (!launcher || !name || !exec_path) {
        return;
    }

    if (launcher->item_count >= MAX_LAUNCHER_ITEMS) {
        fprintf(stderr, "[Launcher] Cannot add item, max items reached\n");
        return;
    }

    launcher_item_t *item = &launcher->items[launcher->item_count];
    strncpy(item->name, name, sizeof(item->name) - 1);
    strncpy(item->exec_path, exec_path, sizeof(item->exec_path) - 1);

    // Assign a color based on index (for visual variety)
    uint32_t colors[] = {
        0xFF4A90E2,  // Blue
        0xFF50C878,  // Green
        0xFFFF6B6B,  // Red
        0xFFFFA500,  // Orange
        0xFF9B59B6,  // Purple
        0xFF3498DB,  // Light blue
    };
    item->icon_color = colors[launcher->item_count % 6];

    launcher->item_count++;
}

// ============================================================================
// UPDATE
// ============================================================================

void launcher_update(launcher_t *launcher, int32_t mouse_x, int32_t mouse_y) {
    if (!launcher) {
        return;
    }

    // Check which item is being hovered
    launcher->hover_item = -1;

    if (mouse_x >= (int32_t)launcher->x &&
        mouse_x < (int32_t)(launcher->x + launcher->width) &&
        mouse_y >= (int32_t)launcher->y &&
        mouse_y < (int32_t)(launcher->y + launcher->height)) {

        int32_t rel_y = mouse_y - launcher->y - ITEM_PADDING;
        int32_t item_idx = rel_y / ITEM_HEIGHT;

        if (item_idx >= 0 && item_idx < (int32_t)launcher->item_count) {
            launcher->hover_item = item_idx;
        }
    }
}

// ============================================================================
// RENDERING
// ============================================================================

void launcher_render(launcher_t *launcher) {
    if (!launcher || !launcher->fb) {
        return;
    }

    // Draw launcher background
    fb_fill_rect(launcher->fb, launcher->x, launcher->y,
                 launcher->width, launcher->height,
                 launcher->bg_color);

    // Draw border
    fb_draw_rect(launcher->fb, launcher->x, launcher->y,
                 launcher->width, launcher->height,
                 0xFF808080);

    // Draw title
    fb_fill_rect(launcher->fb, launcher->x, launcher->y,
                 launcher->width, ITEM_HEIGHT,
                 0xFF4A90E2);

    // Draw items
    for (uint32_t i = 0; i < launcher->item_count; i++) {
        launcher_item_t *item = &launcher->items[i];

        uint32_t item_y = launcher->y + ITEM_HEIGHT + (i * ITEM_HEIGHT);
        uint32_t item_color = (launcher->hover_item == (int32_t)i) ?
                              LAUNCHER_HOVER_COLOR : LAUNCHER_ITEM_COLOR;

        // Draw item background
        fb_fill_rect(launcher->fb, launcher->x + ITEM_PADDING, item_y,
                     launcher->width - (2 * ITEM_PADDING), ITEM_HEIGHT - 2,
                     item_color);

        // Draw icon placeholder (colored square)
        uint32_t icon_size = ITEM_HEIGHT - (2 * ITEM_PADDING);
        fb_fill_rect(launcher->fb, launcher->x + (2 * ITEM_PADDING), item_y + ITEM_PADDING,
                     icon_size, icon_size, item->icon_color);

        // TODO: Draw item text when we have font rendering
        // For now, items are just colored rectangles with colored icons
    }
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

static void launch_app(const char *exec_path) {
    printf("[Launcher] Launching: %s\n", exec_path);

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        execl(exec_path, exec_path, NULL);

        // If execl fails
        fprintf(stderr, "[Launcher] Failed to launch %s\n", exec_path);
        exit(1);
    } else if (pid < 0) {
        fprintf(stderr, "[Launcher] Failed to fork process\n");
    } else {
        // Parent process
        printf("[Launcher] Launched app with PID %d\n", pid);

        // Don't wait for child - let it run independently
        // We could add the PID to a list of running apps here
    }
}

void launcher_handle_click(launcher_t *launcher, int32_t x, int32_t y) {
    if (!launcher) {
        return;
    }

    // Check if click is within launcher bounds
    if (x < (int32_t)launcher->x || x >= (int32_t)(launcher->x + launcher->width) ||
        y < (int32_t)launcher->y || y >= (int32_t)(launcher->y + launcher->height)) {
        return;
    }

    // Skip title area
    if (y < (int32_t)(launcher->y + ITEM_HEIGHT)) {
        return;
    }

    // Determine which item was clicked
    int32_t rel_y = y - launcher->y - ITEM_HEIGHT;
    int32_t item_idx = rel_y / ITEM_HEIGHT;

    if (item_idx >= 0 && item_idx < (int32_t)launcher->item_count) {
        launcher_item_t *item = &launcher->items[item_idx];
        printf("[Launcher] Item clicked: %s\n", item->name);

        // Launch the application
        launch_app(item->exec_path);
    }
}
