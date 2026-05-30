/**
 * AutomationOS Performance Monitoring & Optimization
 *
 * Real-time FPS tracking, frame time analysis, and performance warnings
 */

#ifndef PERFORMANCE_H
#define PERFORMANCE_H

#include <stdint.h>
#include <stdbool.h>

// Performance thresholds
#define TARGET_FPS 60
#define MIN_ACCEPTABLE_FPS 55
#define TARGET_FRAME_TIME_US 16667  // 60 FPS = 16.67ms
#define MAX_ACCEPTABLE_FRAME_TIME_US 18182  // 55 FPS = 18.18ms

// Frame timing sample size
#define FRAME_HISTORY_SIZE 120  // 2 seconds at 60 FPS

/**
 * Frame timing statistics
 */
typedef struct {
    uint64_t frame_times[FRAME_HISTORY_SIZE];
    uint32_t frame_index;
    uint64_t total_frames;

    // Statistics
    uint32_t current_fps;
    uint64_t avg_frame_time_us;
    uint64_t min_frame_time_us;
    uint64_t max_frame_time_us;
    uint32_t dropped_frames;

    // Performance warnings
    bool low_fps_warning;
    bool high_frame_time_warning;
    uint32_t consecutive_slow_frames;
} perf_stats_t;

/**
 * GPU performance metrics
 */
typedef struct {
    float gpu_usage_percent;
    uint64_t vram_used_mb;
    uint64_t vram_total_mb;
    uint32_t texture_count;
    uint32_t draw_calls;
} gpu_stats_t;

/**
 * Initialize performance monitoring
 */
void perf_init(perf_stats_t *stats);

/**
 * Record frame timing
 */
void perf_record_frame(perf_stats_t *stats, uint64_t frame_time_us);

/**
 * Update FPS calculation (call every second)
 */
void perf_update_fps(perf_stats_t *stats);

/**
 * Get current FPS
 */
uint32_t perf_get_fps(const perf_stats_t *stats);

/**
 * Get average frame time
 */
uint64_t perf_get_avg_frame_time(const perf_stats_t *stats);

/**
 * Check for performance issues
 */
bool perf_check_health(const perf_stats_t *stats);

/**
 * Print performance report
 */
void perf_print_report(const perf_stats_t *stats, const gpu_stats_t *gpu);

/**
 * Get performance grade (A+ to F)
 */
const char *perf_get_grade(const perf_stats_t *stats);

/**
 * Reset statistics
 */
void perf_reset_stats(perf_stats_t *stats);

#endif // PERFORMANCE_H
