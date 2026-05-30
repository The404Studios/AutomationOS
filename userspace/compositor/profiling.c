/**
 * Frame Time Profiling Module
 *
 * Comprehensive performance profiling to identify rendering bottlenecks.
 * Tracks time spent in each compositor stage and provides detailed reports.
 *
 * Stages tracked:
 * - Window composition
 * - Alpha blending
 * - Cursor rendering
 * - Buffer flip (present)
 * - Damage tracking overhead
 *
 * Target: < 33ms total frame time (30 FPS minimum)
 */

#include "fb_compositor.h"
#include "performance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>

#define PROFILER_MAX_SAMPLES 120  // 2 seconds at 60 FPS

/**
 * Profiling stage identifiers
 */
typedef enum {
    STAGE_CLEAR_BUFFER = 0,
    STAGE_DAMAGE_CHECK,
    STAGE_WINDOW_COMPOSITE,
    STAGE_CURSOR_RENDER,
    STAGE_BUFFER_FLIP,
    STAGE_DAMAGE_UPDATE,
    STAGE_COUNT
} profiler_stage_t;

static const char *stage_names[STAGE_COUNT] = {
    "Clear Buffer",
    "Damage Check",
    "Window Composite",
    "Cursor Render",
    "Buffer Flip",
    "Damage Update"
};

/**
 * Stage timing statistics
 */
typedef struct {
    uint64_t samples[PROFILER_MAX_SAMPLES];
    uint32_t sample_index;
    uint32_t sample_count;

    uint64_t total_time_us;
    uint64_t avg_time_us;
    uint64_t min_time_us;
    uint64_t max_time_us;
    uint32_t call_count;
} stage_stats_t;

/**
 * Frame profiler structure
 */
typedef struct {
    stage_stats_t stages[STAGE_COUNT];

    // Frame timing
    uint64_t frame_start_time;
    uint64_t current_stage_start;
    profiler_stage_t current_stage;

    // Overall frame stats
    uint64_t frame_times[PROFILER_MAX_SAMPLES];
    uint32_t frame_index;
    uint32_t total_frames;

    uint64_t avg_frame_time_us;
    uint64_t min_frame_time_us;
    uint64_t max_frame_time_us;

    // Performance counters
    uint64_t total_pixels_drawn;
    uint64_t total_windows_composed;
    uint32_t slow_frames;  // Frames > 33ms

    bool profiling_enabled;
} profiler_t;

static profiler_t g_profiler;
static bool g_profiler_initialized = false;

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Initialize profiler
 */
void profiler_init(void) {
    if (g_profiler_initialized) return;

    memset(&g_profiler, 0, sizeof(profiler_t));

    for (int i = 0; i < STAGE_COUNT; i++) {
        g_profiler.stages[i].min_time_us = UINT64_MAX;
    }

    g_profiler.min_frame_time_us = UINT64_MAX;
    g_profiler.profiling_enabled = true;

    g_profiler_initialized = true;

    printf("[Profiler] Initialized (30 FPS target = 33333 µs per frame)\n");
}

/**
 * Cleanup profiler
 */
void profiler_cleanup(void) {
    if (!g_profiler_initialized) return;

    g_profiler_initialized = false;
    memset(&g_profiler, 0, sizeof(profiler_t));

    printf("[Profiler] Cleaned up\n");
}

/**
 * Begin frame profiling
 */
void profiler_frame_begin(void) {
    if (!g_profiler_initialized || !g_profiler.profiling_enabled) return;

    g_profiler.frame_start_time = get_time_us();
}

/**
 * Begin stage timing
 */
void profiler_stage_begin(profiler_stage_t stage) {
    if (!g_profiler_initialized || !g_profiler.profiling_enabled) return;
    if (stage >= STAGE_COUNT) return;

    g_profiler.current_stage = stage;
    g_profiler.current_stage_start = get_time_us();
}

/**
 * End stage timing
 */
void profiler_stage_end(profiler_stage_t stage) {
    if (!g_profiler_initialized || !g_profiler.profiling_enabled) return;
    if (stage >= STAGE_COUNT) return;

    uint64_t end_time = get_time_us();
    uint64_t elapsed = end_time - g_profiler.current_stage_start;

    stage_stats_t *stats = &g_profiler.stages[stage];

    // Store sample
    stats->samples[stats->sample_index] = elapsed;
    stats->sample_index = (stats->sample_index + 1) % PROFILER_MAX_SAMPLES;
    if (stats->sample_count < PROFILER_MAX_SAMPLES) {
        stats->sample_count++;
    }

    // Update statistics
    stats->total_time_us += elapsed;
    stats->call_count++;

    if (elapsed < stats->min_time_us) stats->min_time_us = elapsed;
    if (elapsed > stats->max_time_us) stats->max_time_us = elapsed;

    if (stats->call_count > 0) {
        stats->avg_time_us = stats->total_time_us / stats->call_count;
    }
}

/**
 * End frame profiling
 */
void profiler_frame_end(void) {
    if (!g_profiler_initialized || !g_profiler.profiling_enabled) return;

    uint64_t end_time = get_time_us();
    uint64_t frame_time = end_time - g_profiler.frame_start_time;

    // Store frame time
    g_profiler.frame_times[g_profiler.frame_index] = frame_time;
    g_profiler.frame_index = (g_profiler.frame_index + 1) % PROFILER_MAX_SAMPLES;
    g_profiler.total_frames++;

    // Update frame statistics
    if (frame_time < g_profiler.min_frame_time_us) {
        g_profiler.min_frame_time_us = frame_time;
    }
    if (frame_time > g_profiler.max_frame_time_us) {
        g_profiler.max_frame_time_us = frame_time;
    }

    // Track slow frames (> 33ms = < 30 FPS)
    if (frame_time > 33333) {
        g_profiler.slow_frames++;
    }

    // Calculate average frame time
    uint64_t sum = 0;
    uint32_t count = (g_profiler.total_frames < PROFILER_MAX_SAMPLES)
                     ? (uint32_t)g_profiler.total_frames
                     : PROFILER_MAX_SAMPLES;

    for (uint32_t i = 0; i < count; i++) {
        sum += g_profiler.frame_times[i];
    }

    if (count > 0) {
        g_profiler.avg_frame_time_us = sum / count;
    }
}

/**
 * Record pixels drawn
 */
void profiler_record_pixels_drawn(uint64_t pixel_count) {
    if (!g_profiler_initialized) return;
    g_profiler.total_pixels_drawn += pixel_count;
}

/**
 * Record window composed
 */
void profiler_record_window_composed(void) {
    if (!g_profiler_initialized) return;
    g_profiler.total_windows_composed++;
}

/**
 * Get current FPS
 */
uint32_t profiler_get_fps(void) {
    if (!g_profiler_initialized || g_profiler.avg_frame_time_us == 0) return 0;

    return (uint32_t)(1000000 / g_profiler.avg_frame_time_us);
}

/**
 * Get average frame time in microseconds
 */
uint64_t profiler_get_avg_frame_time(void) {
    if (!g_profiler_initialized) return 0;
    return g_profiler.avg_frame_time_us;
}

/**
 * Print detailed profiling report
 */
void profiler_print_report(void) {
    if (!g_profiler_initialized) return;

    uint32_t fps = profiler_get_fps();
    float frame_time_ms = g_profiler.avg_frame_time_us / 1000.0f;

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║         COMPOSITOR PERFORMANCE PROFILING REPORT               ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ Overall Frame Performance:                                    ║\n");
    printf("║   Target FPS:       30 (33.33 ms/frame)                       ║\n");
    printf("║   Current FPS:      %-3u (%.2f ms/frame)                      ║\n",
           fps, frame_time_ms);
    printf("║   Avg Frame Time:   %lu µs                                    ║\n",
           g_profiler.avg_frame_time_us);
    printf("║   Min Frame Time:   %lu µs                                    ║\n",
           g_profiler.min_frame_time_us);
    printf("║   Max Frame Time:   %lu µs                                    ║\n",
           g_profiler.max_frame_time_us);
    printf("║   Total Frames:     %lu                                       ║\n",
           g_profiler.total_frames);
    printf("║   Slow Frames:      %u (%.1f%%)                               ║\n",
           g_profiler.slow_frames,
           g_profiler.total_frames > 0
               ? (float)g_profiler.slow_frames * 100.0f / g_profiler.total_frames
               : 0.0f);
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ Stage Breakdown (per frame):                                  ║\n");

    // Calculate total stage time
    uint64_t total_stage_time = 0;
    for (int i = 0; i < STAGE_COUNT; i++) {
        total_stage_time += g_profiler.stages[i].avg_time_us;
    }

    // Print each stage
    for (int i = 0; i < STAGE_COUNT; i++) {
        stage_stats_t *stats = &g_profiler.stages[i];
        if (stats->call_count == 0) continue;

        float percent = (total_stage_time > 0)
                        ? (float)stats->avg_time_us * 100.0f / total_stage_time
                        : 0.0f;

        printf("║   %-18s %6lu µs (%5.1f%%)                        ║\n",
               stage_names[i],
               stats->avg_time_us,
               percent);
    }

    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ Throughput:                                                   ║\n");
    printf("║   Total Pixels:     %lu                                       ║\n",
           g_profiler.total_pixels_drawn);
    printf("║   Total Windows:    %lu                                       ║\n",
           g_profiler.total_windows_composed);
    if (g_profiler.total_frames > 0) {
        printf("║   Avg Pixels/Frame: %lu                                    ║\n",
               g_profiler.total_pixels_drawn / g_profiler.total_frames);
        printf("║   Avg Windows/Frame:%.1f                                    ║\n",
               (float)g_profiler.total_windows_composed / g_profiler.total_frames);
    }

    printf("╠═══════════════════════════════════════════════════════════════╣\n");

    // Performance grade
    const char *grade;
    const char *status;

    if (fps >= 30) {
        grade = "✓ PASSING";
        status = "Target achieved";
    } else if (fps >= 25) {
        grade = "⚠ MARGINAL";
        status = "Close to target";
    } else {
        grade = "✗ FAILING";
        status = "Optimization needed";
    }

    printf("║ Status: %-18s %-33s ║\n", grade, status);
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/**
 * Print stage breakdown table
 */
void profiler_print_stage_breakdown(void) {
    if (!g_profiler_initialized) return;

    printf("\n[Profiler] Stage Timing Breakdown:\n");
    printf("%-20s %10s %10s %10s %10s %8s\n",
           "Stage", "Avg (µs)", "Min (µs)", "Max (µs)", "Calls", "%");
    printf("------------------------------------------------------------------------\n");

    uint64_t total_time = 0;
    for (int i = 0; i < STAGE_COUNT; i++) {
        total_time += g_profiler.stages[i].total_time_us;
    }

    for (int i = 0; i < STAGE_COUNT; i++) {
        stage_stats_t *stats = &g_profiler.stages[i];
        if (stats->call_count == 0) continue;

        float percent = (total_time > 0)
                        ? (float)stats->total_time_us * 100.0f / total_time
                        : 0.0f;

        printf("%-20s %10lu %10lu %10lu %10u %7.1f%%\n",
               stage_names[i],
               stats->avg_time_us,
               stats->min_time_us,
               stats->max_time_us,
               stats->call_count,
               percent);
    }

    printf("------------------------------------------------------------------------\n");
    printf("%-20s %10lu µs total\n", "TOTAL", total_time);
    printf("\n");
}

/**
 * Identify bottleneck (slowest stage)
 */
const char *profiler_identify_bottleneck(void) {
    if (!g_profiler_initialized) return "Unknown";

    uint64_t max_time = 0;
    int bottleneck_stage = -1;

    for (int i = 0; i < STAGE_COUNT; i++) {
        if (g_profiler.stages[i].avg_time_us > max_time) {
            max_time = g_profiler.stages[i].avg_time_us;
            bottleneck_stage = i;
        }
    }

    if (bottleneck_stage >= 0) {
        return stage_names[bottleneck_stage];
    }

    return "None";
}

/**
 * Reset profiling statistics
 */
void profiler_reset(void) {
    if (!g_profiler_initialized) return;

    for (int i = 0; i < STAGE_COUNT; i++) {
        memset(&g_profiler.stages[i], 0, sizeof(stage_stats_t));
        g_profiler.stages[i].min_time_us = UINT64_MAX;
    }

    g_profiler.frame_index = 0;
    g_profiler.total_frames = 0;
    g_profiler.avg_frame_time_us = 0;
    g_profiler.min_frame_time_us = UINT64_MAX;
    g_profiler.max_frame_time_us = 0;
    g_profiler.slow_frames = 0;
    g_profiler.total_pixels_drawn = 0;
    g_profiler.total_windows_composed = 0;

    printf("[Profiler] Statistics reset\n");
}

/**
 * Enable/disable profiling
 */
void profiler_set_enabled(bool enabled) {
    if (!g_profiler_initialized) return;

    g_profiler.profiling_enabled = enabled;

    printf("[Profiler] Profiling %s\n", enabled ? "enabled" : "disabled");
}

/**
 * Check if profiling is enabled
 */
bool profiler_is_enabled(void) {
    return g_profiler_initialized && g_profiler.profiling_enabled;
}

/**
 * Export profiling data to file (CSV format)
 */
bool profiler_export_csv(const char *filename) {
    if (!g_profiler_initialized) return false;

    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "[Profiler] Failed to open %s for writing\n", filename);
        return false;
    }

    // Write header
    fprintf(f, "Frame,Time_us,");
    for (int i = 0; i < STAGE_COUNT; i++) {
        fprintf(f, "%s_us", stage_names[i]);
        if (i < STAGE_COUNT - 1) fprintf(f, ",");
    }
    fprintf(f, "\n");

    // Write frame data
    uint32_t sample_count = (g_profiler.total_frames < PROFILER_MAX_SAMPLES)
                            ? (uint32_t)g_profiler.total_frames
                            : PROFILER_MAX_SAMPLES;

    for (uint32_t frame = 0; frame < sample_count; frame++) {
        fprintf(f, "%u,%lu,", frame, g_profiler.frame_times[frame]);

        for (int i = 0; i < STAGE_COUNT; i++) {
            stage_stats_t *stats = &g_profiler.stages[i];
            uint64_t sample = (frame < stats->sample_count)
                              ? stats->samples[frame]
                              : 0;
            fprintf(f, "%lu", sample);
            if (i < STAGE_COUNT - 1) fprintf(f, ",");
        }
        fprintf(f, "\n");
    }

    fclose(f);

    printf("[Profiler] Exported %u frames to %s\n", sample_count, filename);
    return true;
}

/**
 * Get profiler stage enum from name (for external API)
 */
int profiler_stage_from_name(const char *name) {
    for (int i = 0; i < STAGE_COUNT; i++) {
        if (strcmp(stage_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

// Export stage constants for external use
const int PROFILER_STAGE_CLEAR_BUFFER = STAGE_CLEAR_BUFFER;
const int PROFILER_STAGE_DAMAGE_CHECK = STAGE_DAMAGE_CHECK;
const int PROFILER_STAGE_WINDOW_COMPOSITE = STAGE_WINDOW_COMPOSITE;
const int PROFILER_STAGE_CURSOR_RENDER = STAGE_CURSOR_RENDER;
const int PROFILER_STAGE_BUFFER_FLIP = STAGE_BUFFER_FLIP;
const int PROFILER_STAGE_DAMAGE_UPDATE = STAGE_DAMAGE_UPDATE;
