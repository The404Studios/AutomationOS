/**
 * AutomationOS Performance Monitoring Implementation
 */

#include "performance.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Initialize performance monitoring
 */
void perf_init(perf_stats_t *stats) {
    if (!stats) return;

    memset(stats, 0, sizeof(perf_stats_t));
    stats->min_frame_time_us = UINT64_MAX;
    stats->max_frame_time_us = 0;
    stats->current_fps = 0;
    stats->low_fps_warning = false;
    stats->high_frame_time_warning = false;
    stats->consecutive_slow_frames = 0;

    printf("[Performance] Monitoring initialized (target: %d FPS)\n", TARGET_FPS);
}

/**
 * Record frame timing
 */
void perf_record_frame(perf_stats_t *stats, uint64_t frame_time_us) {
    if (!stats) return;

    // Store frame time in circular buffer
    stats->frame_times[stats->frame_index] = frame_time_us;
    stats->frame_index = (stats->frame_index + 1) % FRAME_HISTORY_SIZE;
    stats->total_frames++;

    // Update min/max
    if (frame_time_us < stats->min_frame_time_us) {
        stats->min_frame_time_us = frame_time_us;
    }
    if (frame_time_us > stats->max_frame_time_us) {
        stats->max_frame_time_us = frame_time_us;
    }

    // Track dropped frames (> 18ms = <55 FPS)
    if (frame_time_us > MAX_ACCEPTABLE_FRAME_TIME_US) {
        stats->dropped_frames++;
        stats->consecutive_slow_frames++;

        // Warning after 10 consecutive slow frames
        if (stats->consecutive_slow_frames >= 10) {
            stats->high_frame_time_warning = true;
            printf("[Performance] ⚠️  HIGH FRAME TIME: %lu µs (target: %d µs)\n",
                   frame_time_us, TARGET_FRAME_TIME_US);
        }
    } else {
        stats->consecutive_slow_frames = 0;
        stats->high_frame_time_warning = false;
    }
}

/**
 * Calculate average frame time from history
 */
static uint64_t calculate_avg_frame_time(const perf_stats_t *stats) {
    uint64_t sum = 0;
    uint32_t count = stats->total_frames < FRAME_HISTORY_SIZE
                     ? (uint32_t)stats->total_frames
                     : FRAME_HISTORY_SIZE;

    if (count == 0) return 0;

    for (uint32_t i = 0; i < count; i++) {
        sum += stats->frame_times[i];
    }

    return sum / count;
}

/**
 * Update FPS calculation
 */
void perf_update_fps(perf_stats_t *stats) {
    if (!stats) return;

    // Calculate average frame time
    stats->avg_frame_time_us = calculate_avg_frame_time(stats);

    // Convert to FPS
    if (stats->avg_frame_time_us > 0) {
        stats->current_fps = (uint32_t)(1000000 / stats->avg_frame_time_us);
    }

    // Check for low FPS warning
    if (stats->current_fps < MIN_ACCEPTABLE_FPS) {
        stats->low_fps_warning = true;
        printf("[Performance] ⚠️  LOW FPS: %u (target: %d, min: %d)\n",
               stats->current_fps, TARGET_FPS, MIN_ACCEPTABLE_FPS);
    } else {
        stats->low_fps_warning = false;
    }

    // Log current FPS
    const char *grade = perf_get_grade(stats);
    printf("[Performance] FPS: %u | Avg Frame: %lu µs | Grade: %s\n",
           stats->current_fps,
           stats->avg_frame_time_us,
           grade);
}

/**
 * Get current FPS
 */
uint32_t perf_get_fps(const perf_stats_t *stats) {
    return stats ? stats->current_fps : 0;
}

/**
 * Get average frame time
 */
uint64_t perf_get_avg_frame_time(const perf_stats_t *stats) {
    return stats ? stats->avg_frame_time_us : 0;
}

/**
 * Check for performance issues
 */
bool perf_check_health(const perf_stats_t *stats) {
    if (!stats) return false;

    // Healthy if:
    // - FPS >= 55
    // - No consecutive slow frames
    // - Dropped frames < 5% of total

    bool fps_ok = stats->current_fps >= MIN_ACCEPTABLE_FPS;
    bool frame_time_ok = !stats->high_frame_time_warning;

    uint64_t total = stats->total_frames > 0 ? stats->total_frames : 1;
    bool drop_rate_ok = (stats->dropped_frames * 100 / total) < 5;

    return fps_ok && frame_time_ok && drop_rate_ok;
}

/**
 * Get performance grade
 */
const char *perf_get_grade(const perf_stats_t *stats) {
    if (!stats) return "F";

    if (stats->current_fps >= 60) {
        if (stats->dropped_frames == 0) {
            return "A+";  // Perfect 60 FPS
        }
        return "A";  // 60+ FPS with occasional drops
    } else if (stats->current_fps >= 58) {
        return "A-";  // Near-perfect
    } else if (stats->current_fps >= 55) {
        return "B+";  // Acceptable
    } else if (stats->current_fps >= 50) {
        return "B";   // Noticeable but playable
    } else if (stats->current_fps >= 45) {
        return "C";   // Sluggish
    } else if (stats->current_fps >= 30) {
        return "D";   // Poor
    } else {
        return "F";   // Unacceptable
    }
}

/**
 * Print detailed performance report
 */
void perf_print_report(const perf_stats_t *stats, const gpu_stats_t *gpu) {
    if (!stats) return;

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║       COMPOSITOR PERFORMANCE REPORT                  ║\n");
    printf("╠═══════════════════════════════════════════════════════╣\n");
    printf("║ FPS:            %3u / %d (Grade: %s)                 ║\n",
           stats->current_fps, TARGET_FPS, perf_get_grade(stats));
    printf("║ Avg Frame Time: %5lu µs (%.2f ms)                   ║\n",
           stats->avg_frame_time_us, stats->avg_frame_time_us / 1000.0);
    printf("║ Min Frame Time: %5lu µs                             ║\n",
           stats->min_frame_time_us);
    printf("║ Max Frame Time: %5lu µs                             ║\n",
           stats->max_frame_time_us);
    printf("║ Total Frames:   %lu                                  ║\n",
           stats->total_frames);
    printf("║ Dropped Frames: %u (%.2f%%)                          ║\n",
           stats->dropped_frames,
           stats->total_frames > 0 ? (stats->dropped_frames * 100.0 / stats->total_frames) : 0.0);

    if (gpu) {
        printf("╠═══════════════════════════════════════════════════════╣\n");
        printf("║ GPU Usage:      %.1f%%                               ║\n",
               gpu->gpu_usage_percent);
        printf("║ VRAM Used:      %lu / %lu MB                         ║\n",
               gpu->vram_used_mb, gpu->vram_total_mb);
        printf("║ Textures:       %u                                   ║\n",
               gpu->texture_count);
        printf("║ Draw Calls:     %u / frame                           ║\n",
               gpu->draw_calls);
    }

    printf("╠═══════════════════════════════════════════════════════╣\n");
    printf("║ Status: ");
    if (perf_check_health(stats)) {
        printf("✓ HEALTHY                                      ║\n");
    } else {
        printf("⚠️  PERFORMANCE ISSUES                          ║\n");
    }
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/**
 * Reset statistics
 */
void perf_reset_stats(perf_stats_t *stats) {
    if (!stats) return;

    stats->frame_index = 0;
    stats->total_frames = 0;
    stats->dropped_frames = 0;
    stats->min_frame_time_us = UINT64_MAX;
    stats->max_frame_time_us = 0;
    stats->consecutive_slow_frames = 0;
    stats->low_fps_warning = false;
    stats->high_frame_time_warning = false;

    memset(stats->frame_times, 0, sizeof(stats->frame_times));

    printf("[Performance] Statistics reset\n");
}
