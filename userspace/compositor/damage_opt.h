/**
 * Optimized Damage Tracking Module - Header
 *
 * Minimize overdraw by tracking dirty regions
 */

#ifndef DAMAGE_OPT_H
#define DAMAGE_OPT_H

#include "fb_compositor.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize damage tracker
 */
void damage_init(damage_tracker_t *damage);

/**
 * Clear all damage regions
 */
void damage_clear(damage_tracker_t *damage);

/**
 * Mark entire screen for redraw
 */
void damage_mark_full_redraw(damage_tracker_t *damage);

/**
 * Add damage region with automatic merging
 */
void damage_add_region(damage_tracker_t *damage, const rect_t *rect);

/**
 * Check if a region is damaged
 */
bool damage_is_region_damaged(damage_tracker_t *damage, const rect_t *rect);

/**
 * Clip damage regions to screen bounds
 */
void damage_clip_to_screen(damage_tracker_t *damage, uint32_t width, uint32_t height);

/**
 * Calculate total damaged pixels
 */
uint64_t damage_calculate_total_pixels(const damage_tracker_t *damage,
                                       uint32_t screen_width, uint32_t screen_height);

/**
 * Optimize damage regions (merge nearby, remove overlaps)
 */
void damage_optimize(damage_tracker_t *damage);

/**
 * Print damage statistics
 */
void damage_print_stats(const damage_tracker_t *damage,
                       uint32_t screen_width, uint32_t screen_height);

/**
 * Copy damage regions for iteration
 */
uint32_t damage_get_regions(const damage_tracker_t *damage, rect_t *regions_out,
                            uint32_t max_regions);

/**
 * Check if damage tracker is empty
 */
bool damage_is_empty(const damage_tracker_t *damage);

/**
 * Get damage coverage percentage
 */
float damage_get_coverage_percent(const damage_tracker_t *damage,
                                  uint32_t screen_width, uint32_t screen_height);

/**
 * Reset to initial state
 */
void damage_reset(damage_tracker_t *damage);

#endif // DAMAGE_OPT_H
