/**
 * File Explorer - View Modes Implementation
 *
 * Implements different view modes: Icons, List, Columns, Gallery
 */

#include "explorer.h"
#include "file_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Calculate layout for icon view
 */
void view_layout_icons(file_view_t *view) {
    if (!view || view->item_count == 0) return;

    uint32_t item_width = view->icon_size + view->spacing * 2;
    uint32_t item_height = view->icon_size + view->spacing + 40;  // Space for text

    // Calculate columns based on view width
    view->columns = (view->geometry.w - view->spacing) / item_width;
    if (view->columns == 0) view->columns = 1;

    // Calculate rows
    uint32_t rows = (view->item_count + view->columns - 1) / view->columns;

    // Layout items in grid
    for (uint32_t i = 0; i < view->item_count; i++) {
        uint32_t row = i / view->columns;
        uint32_t col = i % view->columns;

        view->items[i].bounds.x = col * item_width + view->spacing;
        view->items[i].bounds.y = row * item_height + view->spacing - view->scroll_offset;
        view->items[i].bounds.w = item_width - view->spacing;
        view->items[i].bounds.h = item_height - view->spacing;
    }

    // Update max scroll
    view->max_scroll = (rows * item_height) - view->geometry.h;
    if (view->max_scroll < 0) view->max_scroll = 0;
}

/**
 * Calculate layout for list view
 */
void view_layout_list(file_view_t *view) {
    if (!view || view->item_count == 0) return;

    uint32_t row_height = 32;
    uint32_t y = 0;

    // Header row
    y += row_height;

    // Layout items vertically
    for (uint32_t i = 0; i < view->item_count; i++) {
        view->items[i].bounds.x = 0;
        view->items[i].bounds.y = y - view->scroll_offset;
        view->items[i].bounds.w = view->geometry.w;
        view->items[i].bounds.h = row_height;

        y += row_height;
    }

    // Update max scroll
    view->max_scroll = (y + row_height) - view->geometry.h;
    if (view->max_scroll < 0) view->max_scroll = 0;
}

/**
 * Calculate layout for column view (Miller columns)
 */
void view_layout_columns(file_view_t *view) {
    if (!view || view->item_count == 0) return;

    // Column view shows multiple directory levels side by side
    // For now, just show current directory like list view
    view_layout_list(view);
}

/**
 * Calculate layout for gallery view (large thumbnails)
 */
void view_layout_gallery(file_view_t *view) {
    if (!view || view->item_count == 0) return;

    uint32_t item_size = 256;  // Large thumbnails
    uint32_t item_width = item_size + 20;
    uint32_t item_height = item_size + 60;

    view->columns = (view->geometry.w - 20) / item_width;
    if (view->columns == 0) view->columns = 1;

    uint32_t rows = (view->item_count + view->columns - 1) / view->columns;

    for (uint32_t i = 0; i < view->item_count; i++) {
        uint32_t row = i / view->columns;
        uint32_t col = i % view->columns;

        view->items[i].bounds.x = col * item_width + 10;
        view->items[i].bounds.y = row * item_height + 10 - view->scroll_offset;
        view->items[i].bounds.w = item_size;
        view->items[i].bounds.h = item_size + 40;
    }

    view->max_scroll = (rows * item_height) - view->geometry.h;
    if (view->max_scroll < 0) view->max_scroll = 0;
}

/**
 * Update view layout based on current mode
 */
void view_update_layout(file_view_t *view) {
    if (!view) return;

    switch (view->mode) {
        case VIEW_MODE_ICONS:
            view_layout_icons(view);
            break;
        case VIEW_MODE_LIST:
            view_layout_list(view);
            break;
        case VIEW_MODE_COLUMNS:
            view_layout_columns(view);
            break;
        case VIEW_MODE_GALLERY:
            view_layout_gallery(view);
            break;
    }
}

/**
 * Find item at position
 */
int32_t view_hit_test(file_view_t *view, int32_t x, int32_t y) {
    if (!view) return -1;

    for (uint32_t i = 0; i < view->item_count; i++) {
        rect_t *bounds = &view->items[i].bounds;
        if (x >= bounds->x && x < bounds->x + bounds->w &&
            y >= bounds->y && y < bounds->y + bounds->h) {
            return i;
        }
    }

    return -1;
}

/**
 * Render icons view
 */
void view_render_icons(file_view_t *view) {
    if (!view) return;

    for (uint32_t i = 0; i < view->item_count; i++) {
        file_view_item_t *item = &view->items[i];

        // Skip items outside visible area
        if (item->bounds.y + item->bounds.h < 0 ||
            item->bounds.y > view->geometry.h) {
            continue;
        }

        // TODO: Draw icon, thumbnail, and filename
    }
}

/**
 * Render list view
 */
void view_render_list(file_view_t *view) {
    if (!view) return;

    // TODO: Draw column headers
    // TODO: Draw rows with columns: Name | Size | Type | Modified
}

/**
 * Render columns view
 */
void view_render_columns(file_view_t *view) {
    if (!view) return;
    view_render_list(view);  // Placeholder
}

/**
 * Render gallery view
 */
void view_render_gallery(file_view_t *view) {
    if (!view) return;

    for (uint32_t i = 0; i < view->item_count; i++) {
        file_view_item_t *item = &view->items[i];

        if (item->bounds.y + item->bounds.h < 0 ||
            item->bounds.y > view->geometry.h) {
            continue;
        }

        // TODO: Draw large thumbnail with minimal text
    }
}
