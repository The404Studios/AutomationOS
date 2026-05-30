/**
 * App Launcher Module - Header
 */

#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct framebuffer framebuffer_t;

// ============================================================================
// LAUNCHER STRUCTURE
// ============================================================================

#define MAX_LAUNCHER_ITEMS 16

typedef struct launcher_item {
    char name[64];          // Display name
    char exec_path[256];    // Path to executable
    uint32_t icon_color;    // Icon color (placeholder)
} launcher_item_t;

typedef struct launcher {
    framebuffer_t *fb;

    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    uint32_t bg_color;

    // Menu items
    launcher_item_t items[MAX_LAUNCHER_ITEMS];
    uint32_t item_count;

    // Selection
    int32_t selected_item;  // -1 if none
    int32_t hover_item;     // -1 if none
} launcher_t;

// ============================================================================
// LAUNCHER API
// ============================================================================

launcher_t *launcher_init(framebuffer_t *fb);
void launcher_cleanup(launcher_t *launcher);
void launcher_update(launcher_t *launcher, int32_t mouse_x, int32_t mouse_y);
void launcher_render(launcher_t *launcher);
void launcher_handle_click(launcher_t *launcher, int32_t x, int32_t y);
void launcher_add_item(launcher_t *launcher, const char *name, const char *exec_path);

#endif // LAUNCHER_H
