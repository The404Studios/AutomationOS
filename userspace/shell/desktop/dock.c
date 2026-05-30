/**
 * Dock (Application Launcher) Implementation
 *
 * macOS-style dock with app icons, running indicators, magnification effect,
 * and smooth animations.
 */

#include "desktop_shell.h"
#include "../../lib/compositor_client/compositor_client.h"
#include "../../lib/ui/draw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Dock constants
#define DOCK_DEFAULT_ICON_SIZE 64
#define DOCK_PADDING 8
#define DOCK_MARGIN 16
#define DOCK_MAX_SCALE 1.5f
#define DOCK_MAGNIFY_RADIUS 150.0f

// ============================================================================
// DOCK ITEM HELPERS
// ============================================================================

static dock_item_t *dock_item_create(const char *app_id, const char *name, bool pinned) {
    dock_item_t *item = calloc(1, sizeof(dock_item_t));
    if (!item) return NULL;

    strncpy(item->app_id, app_id, sizeof(item->app_id) - 1);
    strncpy(item->name, name, sizeof(item->name) - 1);
    item->pinned = pinned;
    item->running = false;
    item->window_count = 0;
    item->notification_count = 0;
    item->scale = 1.0f;

    // TODO: Load app icon from filesystem
    // item->icon = texture_load(icon_path);

    return item;
}

static void dock_item_destroy(dock_item_t *item) {
    if (!item) return;

    // TODO: Free texture
    // if (item->icon) texture_destroy(item->icon);

    free(item);
}

static void dock_item_render(dock_item_t *item, theme_t *theme) {
    if (!item) return;

    // Calculate scaled size
    uint32_t scaled_size = (uint32_t)(item->bounds.width * item->scale);
    int32_t offset_y = (int32_t)(item->bounds.width - scaled_size);

    rect_t scaled_bounds = {
        .x = item->bounds.x - (int32_t)(scaled_size - item->bounds.width) / 2,
        .y = item->bounds.y + offset_y,
        .width = scaled_size,
        .height = scaled_size
    };

    // Render icon shadow
    // TODO: Draw shadow effect

    // Render icon background (rounded square)
    // TODO: Draw rounded rect with theme->dock_bg color

    // Render icon texture
    // TODO: Blit texture at scaled_bounds

    // Render running indicator (dot below icon)
    if (item->running) {
        int32_t dot_x = scaled_bounds.x + (int32_t)scaled_bounds.width / 2 - 3;
        int32_t dot_y = scaled_bounds.y + (int32_t)scaled_bounds.height + 6;
        // TODO: Draw filled circle (6px diameter) at (dot_x, dot_y)
        // color: theme->primary
    }

    // Render notification badge
    if (item->notification_count > 0) {
        int32_t badge_x = scaled_bounds.x + (int32_t)scaled_bounds.width - 8;
        int32_t badge_y = scaled_bounds.y;
        // TODO: Draw red circle with notification count
        // background: theme->error
        // text: white, centered
    }

    (void)theme;
}

// ============================================================================
// DOCK LAYOUT
// ============================================================================

static void dock_update_layout(dock_t *dock) {
    if (!dock || dock->count == 0) return;

    uint32_t icon_size = dock->icon_size;
    uint32_t spacing = DOCK_PADDING;

    if (dock->position == DOCK_BOTTOM) {
        // Centered at bottom
        uint32_t total_width = dock->count * (icon_size + spacing) - spacing + 2 * DOCK_PADDING;
        int32_t start_x = (int32_t)(dock->window->bounds.width - total_width) / 2;
        int32_t y = (int32_t)dock->window->bounds.height - (int32_t)icon_size - DOCK_MARGIN;

        for (uint32_t i = 0; i < dock->count; i++) {
            dock->items[i]->bounds.x = start_x + DOCK_PADDING + (int32_t)i * (int32_t)(icon_size + spacing);
            dock->items[i]->bounds.y = y;
            dock->items[i]->bounds.width = icon_size;
            dock->items[i]->bounds.height = icon_size;
        }
    } else if (dock->position == DOCK_LEFT) {
        // Centered on left side
        uint32_t total_height = dock->count * (icon_size + spacing) - spacing + 2 * DOCK_PADDING;
        int32_t start_y = (int32_t)(dock->window->bounds.height - total_height) / 2;
        int32_t x = DOCK_MARGIN;

        for (uint32_t i = 0; i < dock->count; i++) {
            dock->items[i]->bounds.x = x;
            dock->items[i]->bounds.y = start_y + DOCK_PADDING + (int32_t)i * (int32_t)(icon_size + spacing);
            dock->items[i]->bounds.width = icon_size;
            dock->items[i]->bounds.height = icon_size;
        }
    }
}

// ============================================================================
// MAGNIFICATION EFFECT
// ============================================================================

static void dock_update_magnification(dock_t *dock) {
    if (!dock || !dock->magnify_on_hover) {
        // Reset all scales to 1.0
        for (uint32_t i = 0; i < dock->count; i++) {
            dock->items[i]->scale = 1.0f;
        }
        return;
    }

    // Calculate distance from mouse to each icon
    for (uint32_t i = 0; i < dock->count; i++) {
        dock_item_t *item = dock->items[i];

        // Center of icon
        float icon_center_x = (float)(item->bounds.x + (int32_t)item->bounds.width / 2);
        float icon_center_y = (float)(item->bounds.y + (int32_t)item->bounds.height / 2);

        // Distance from mouse
        float dx = (float)dock->mouse_pos.x - icon_center_x;
        float dy = (float)dock->mouse_pos.y - icon_center_y;
        float distance = sqrtf(dx * dx + dy * dy);

        // Calculate scale based on distance
        if (distance < DOCK_MAGNIFY_RADIUS) {
            float ratio = 1.0f - (distance / DOCK_MAGNIFY_RADIUS);
            item->scale = 1.0f + ratio * (DOCK_MAX_SCALE - 1.0f);
        } else {
            item->scale = 1.0f;
        }

        // Smooth interpolation
        // TODO: Add smooth animation over time
    }
}

// ============================================================================
// DOCK API
// ============================================================================

dock_t *dock_create(desktop_shell_t *shell) {
    if (!shell) return NULL;

    printf("[Dock] Creating dock\n");

    dock_t *dock = calloc(1, sizeof(dock_t));
    if (!dock) {
        fprintf(stderr, "[Dock] ERROR: Failed to allocate dock\n");
        return NULL;
    }

    dock->theme = &shell->theme;
    dock->position = DOCK_BOTTOM;
    dock->icon_size = DOCK_DEFAULT_ICON_SIZE;
    dock->padding = DOCK_PADDING;
    dock->autohide = false;
    dock->magnify_on_hover = true;
    dock->hover_index = -1;
    dock->count = 0;

    // TODO: Create dock window
    // dock->window = window_create(...);

    // Add some default pinned apps
    dock_add_app(dock, "com.automationos.files", "Files", true);
    dock_add_app(dock, "com.automationos.browser", "Browser", true);
    dock_add_app(dock, "com.automationos.terminal", "Terminal", true);
    dock_add_app(dock, "com.automationos.settings", "Settings", true);

    // Update layout
    dock_update_layout(dock);

    printf("[Dock] Dock created successfully with %u apps\n", dock->count);
    return dock;
}

void dock_destroy(dock_t *dock) {
    if (!dock) return;

    printf("[Dock] Destroying dock\n");

    // Destroy all dock items
    for (uint32_t i = 0; i < dock->count; i++) {
        if (dock->items[i]) {
            dock_item_destroy(dock->items[i]);
        }
    }

    // TODO: Destroy dock window
    // if (dock->window) window_destroy(dock->window);

    free(dock);
}

void dock_add_app(dock_t *dock, const char *app_id, const char *name, bool pinned) {
    if (!dock || !app_id || !name) return;

    if (dock->count >= 64) {
        fprintf(stderr, "[Dock] ERROR: Dock is full (max 64 apps)\n");
        return;
    }

    // Check if app already exists
    for (uint32_t i = 0; i < dock->count; i++) {
        if (strcmp(dock->items[i]->app_id, app_id) == 0) {
            printf("[Dock] App %s already in dock\n", app_id);
            return;
        }
    }

    dock_item_t *item = dock_item_create(app_id, name, pinned);
    if (!item) {
        fprintf(stderr, "[Dock] ERROR: Failed to create dock item for %s\n", app_id);
        return;
    }

    dock->items[dock->count++] = item;
    dock_update_layout(dock);

    printf("[Dock] Added app %s (%s) to dock\n", name, app_id);
}

void dock_remove_app(dock_t *dock, const char *app_id) {
    if (!dock || !app_id) return;

    for (uint32_t i = 0; i < dock->count; i++) {
        if (strcmp(dock->items[i]->app_id, app_id) == 0) {
            // Only remove if not running or not pinned
            if (!dock->items[i]->running && !dock->items[i]->pinned) {
                dock_item_destroy(dock->items[i]);

                // Shift remaining items
                for (uint32_t j = i; j < dock->count - 1; j++) {
                    dock->items[j] = dock->items[j + 1];
                }

                dock->count--;
                dock_update_layout(dock);

                printf("[Dock] Removed app %s from dock\n", app_id);
                return;
            }
        }
    }
}

void dock_update(dock_t *dock, uint64_t delta_us) {
    if (!dock) return;

    // Update launcher status
    launcher_update();

    // Update running indicators for all dock items
    for (uint32_t i = 0; i < dock->count; i++) {
        dock_item_t *item = dock->items[i];
        item->running = launcher_is_running(item->app_id);
        item->window_count = launcher_get_window_count(item->app_id);
    }

    // Update magnification effect
    dock_update_magnification(dock);

    // Check for hover
    dock->hover_index = -1;
    for (uint32_t i = 0; i < dock->count; i++) {
        if (rect_contains(&dock->items[i]->bounds, dock->mouse_pos.x, dock->mouse_pos.y)) {
            dock->hover_index = (int32_t)i;
            break;
        }
    }

    (void)delta_us;
}

void dock_handle_click(dock_t *dock, int32_t x, int32_t y) {
    if (!dock) return;

    // Check which dock item was clicked
    for (uint32_t i = 0; i < dock->count; i++) {
        dock_item_t *item = dock->items[i];
        if (rect_contains(&item->bounds, x, y)) {
            printf("[Dock] Clicked on %s\n", item->name);

            // Launch app if not running, or focus if already running
            if (!item->running) {
                launcher_launch_app(item->app_id);
            } else {
                // TODO: Focus existing window
                printf("[Dock] TODO: Focus existing window for %s\n", item->app_id);
            }

            return;
        }
    }
}

void dock_render(dock_t *dock) {
    if (!dock || !dock->theme) return;

    // Render dock background
    // Calculate dock bounds based on position and content
    rect_t bg_bounds;
    if (dock->position == DOCK_BOTTOM) {
        uint32_t total_width = dock->count * (dock->icon_size + dock->padding) - dock->padding + 2 * DOCK_PADDING;
        uint32_t dock_height = dock->icon_size + 2 * DOCK_PADDING + DOCK_MARGIN;

        bg_bounds.x = (int32_t)(dock->window->bounds.width - total_width) / 2 - DOCK_PADDING;
        bg_bounds.y = (int32_t)dock->window->bounds.height - (int32_t)dock_height;
        bg_bounds.width = total_width + 2 * DOCK_PADDING;
        bg_bounds.height = dock_height;
    } else {
        uint32_t total_height = dock->count * (dock->icon_size + dock->padding) - dock->padding + 2 * DOCK_PADDING;
        uint32_t dock_width = dock->icon_size + 2 * DOCK_PADDING + DOCK_MARGIN;

        bg_bounds.x = 0;
        bg_bounds.y = (int32_t)(dock->window->bounds.height - total_height) / 2 - DOCK_PADDING;
        bg_bounds.width = dock_width;
        bg_bounds.height = total_height + 2 * DOCK_PADDING;
    }

    // TODO: Draw rounded rectangle with blur and transparency
    // color: dock->theme->dock_bg
    // corner_radius: dock->theme->corner_radius
    // blur_radius: dock->theme->blur_radius

    // Render dock items (back to front based on scale)
    // Items with larger scale should render last (on top)
    for (uint32_t i = 0; i < dock->count; i++) {
        dock_item_render(dock->items[i], dock->theme);
    }
}
