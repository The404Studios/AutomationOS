/**
 * File Explorer - Sidebar Implementation
 *
 * Implements the sidebar with places, devices, and network locations
 */

#include "explorer.h"
#include <stdio.h>
#include <string.h>

#define PLACE_HEIGHT 32
#define SEPARATOR_HEIGHT 10

/**
 * Calculate sidebar layout
 */
void sidebar_update_layout(sidebar_t *sidebar) {
    if (!sidebar) return;

    // Places are laid out vertically
    // This would calculate positions for rendering
}

/**
 * Hit test sidebar
 */
int32_t sidebar_hit_test(sidebar_t *sidebar, int32_t x, int32_t y) {
    if (!sidebar) return -1;

    int32_t place_y = sidebar->geometry.y - sidebar->scroll_offset;

    for (uint32_t i = 0; i < sidebar->place_count; i++) {
        sidebar_place_t *place = &sidebar->places[i];

        if (place->is_separator) {
            place_y += SEPARATOR_HEIGHT;
            continue;
        }

        if (y >= place_y && y < place_y + PLACE_HEIGHT) {
            return i;
        }

        place_y += PLACE_HEIGHT;
    }

    return -1;
}

/**
 * Render sidebar
 */
void sidebar_render(sidebar_t *sidebar) {
    if (!sidebar) return;

    // TODO: Draw background
    // TODO: Draw section headers
    // TODO: Draw places with icons
}
