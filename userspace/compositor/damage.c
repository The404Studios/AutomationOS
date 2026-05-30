/**
 * Damage Tracking Module
 *
 * Tracks damaged (changed) regions to optimize rendering.
 * Only redraws areas that have changed instead of the entire screen.
 */

#include "fb_compositor.h"
#include <string.h>

/**
 * Add damage region
 *
 * Marks a rectangular area as needing redraw.
 */
void damage_add_region(damage_tracker_t *damage, const rect_t *rect) {
    if (!damage || !rect) return;

    // If already full redraw, no need to track individual regions
    if (damage->full_redraw) return;

    // If we've exceeded max damage regions, mark full redraw
    if (damage->count >= MAX_DAMAGE_REGIONS) {
        damage->full_redraw = true;
        damage->count = 0;
        return;
    }

    // Try to merge with existing regions first (optimization)
    for (uint32_t i = 0; i < damage->count; i++) {
        if (rect_intersects(&damage->regions[i], rect)) {
            // Merge overlapping regions
            rect_union(&damage->regions[i], &damage->regions[i], rect);
            return;
        }
    }

    // Add new damage region
    damage->regions[damage->count++] = *rect;
}

/**
 * Mark entire screen as damaged (full redraw)
 */
void damage_mark_full_redraw(damage_tracker_t *damage) {
    if (!damage) return;

    damage->full_redraw = true;
    damage->count = 0;
}

/**
 * Clear all damage regions
 *
 * Called after frame is rendered.
 */
void damage_clear(damage_tracker_t *damage) {
    if (!damage) return;

    damage->count = 0;
    damage->full_redraw = false;
}

/**
 * Check if a region is damaged
 *
 * Returns true if the region needs to be redrawn.
 */
bool damage_is_region_damaged(damage_tracker_t *damage, const rect_t *rect) {
    if (!damage || !rect) return true;

    // Full redraw - everything is damaged
    if (damage->full_redraw) return true;

    // No damage regions - nothing is damaged
    if (damage->count == 0) return false;

    // Check if rect intersects any damage region
    for (uint32_t i = 0; i < damage->count; i++) {
        if (rect_intersects(&damage->regions[i], rect)) {
            return true;
        }
    }

    return false;
}
