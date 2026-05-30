/**
 * Frame Time Profiling Module - Header
 *
 * Comprehensive performance profiling for identifying bottlenecks
 */

#ifndef PROFILING_H
#define PROFILING_H

#include <stdint.h>
#include <stdbool.h>

// Stage identifiers (exported constants)
extern const int PROFILER_STAGE_CLEAR_BUFFER;
extern const int PROFILER_STAGE_DAMAGE_CHECK;
extern const int PROFILER_STAGE_WINDOW_COMPOSITE;
extern const int PROFILER_STAGE_CURSOR_RENDER;
extern const int PROFILER_STAGE_BUFFER_FLIP;
extern const int PROFILER_STAGE_DAMAGE_UPDATE;

/**
 * Initialize profiler
 */
void profiler_init(void);

/**
 * Cleanup profiler
 */
void profiler_cleanup(void);

/**
 * Begin frame profiling
 */
void profiler_frame_begin(void);

/**
 * End frame profiling
 */
void profiler_frame_end(void);

/**
 * Begin stage timing
 */
void profiler_stage_begin(int stage);

/**
 * End stage timing
 */
void profiler_stage_end(int stage);

/**
 * Record pixels drawn
 */
void profiler_record_pixels_drawn(uint64_t pixel_count);

/**
 * Record window composed
 */
void profiler_record_window_composed(void);

/**
 * Get current FPS
 */
uint32_t profiler_get_fps(void);

/**
 * Get average frame time
 */
uint64_t profiler_get_avg_frame_time(void);

/**
 * Print detailed report
 */
void profiler_print_report(void);

/**
 * Print stage breakdown
 */
void profiler_print_stage_breakdown(void);

/**
 * Identify bottleneck stage
 */
const char *profiler_identify_bottleneck(void);

/**
 * Reset statistics
 */
void profiler_reset(void);

/**
 * Enable/disable profiling
 */
void profiler_set_enabled(bool enabled);

/**
 * Check if profiling is enabled
 */
bool profiler_is_enabled(void);

/**
 * Export profiling data to CSV
 */
bool profiler_export_csv(const char *filename);

/**
 * Get stage enum from name
 */
int profiler_stage_from_name(const char *name);

#endif // PROFILING_H
