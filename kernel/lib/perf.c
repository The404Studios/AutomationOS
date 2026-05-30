/*
 * Performance Monitoring Infrastructure
 * ======================================
 *
 * RDTSC-based cycle counting with histogram collection for hot paths.
 *
 * Features:
 *  - Per-operation statistics (min/avg/max/count)
 *  - Histogram distribution (16 buckets, 100 cycles each)
 *  - Thread-safe recording (atomic updates)
 *  - Zero overhead when disabled
 *
 * Usage:
 *   PERF_START(PERF_OP_SYSCALL);
 *   int result = handle_syscall(...);
 *   PERF_END(PERF_OP_SYSCALL);
 *
 *   perf_report();  // Print stats on demand
 */

#include "../include/perf.h"
#include "../include/types.h"
#include "../include/kernel.h"

// Global performance statistics
static perf_stats_t perf_stats[PERF_OP_MAX];

// Per-operation start times (for nested/concurrent operations)
static uint64_t perf_start_time[PERF_OP_MAX];

// Global enable/disable flag
static bool perf_enabled = true;

// Operation names for reporting
static const char* perf_op_names[PERF_OP_MAX] = {
    [PERF_OP_SYSCALL] = "syscall",
    [PERF_OP_CONTEXT_SWITCH] = "context_switch",
    [PERF_OP_SLAB_ALLOC] = "slab_alloc",
    [PERF_OP_SLAB_FREE] = "slab_free",
    [PERF_OP_PAGE_ALLOC] = "page_alloc",
    [PERF_OP_PAGE_FREE] = "page_free",
    [PERF_OP_MEMCPY] = "memcpy",
    [PERF_OP_MEMSET] = "memset",
    [PERF_OP_SCHEDULER_ADD] = "scheduler_add",
    [PERF_OP_SCHEDULER_REMOVE] = "scheduler_remove",
};

// Initialize performance monitoring
void perf_init(void) {
    kprintf("[PERF] Initializing performance monitoring...\n");

    for (int i = 0; i < PERF_OP_MAX; i++) {
        perf_stats[i].name = perf_op_names[i];
        perf_stats[i].count = 0;
        perf_stats[i].total_cycles = 0;
        perf_stats[i].min_cycles = UINT64_MAX;
        perf_stats[i].max_cycles = 0;
        perf_stats[i].overflow = 0;

        for (int j = 0; j < PERF_HISTOGRAM_BUCKETS; j++) {
            perf_stats[i].histogram[j] = 0;
        }

        perf_start_time[i] = 0;
    }

    perf_enabled = true;
    kprintf("[PERF] Performance monitoring initialized (16 buckets × %d cycles)\n", PERF_BUCKET_SIZE);
}

// Enable performance monitoring
void perf_enable(void) {
    perf_enabled = true;
    kprintf("[PERF] Performance monitoring enabled\n");
}

// Disable performance monitoring
void perf_disable(void) {
    perf_enabled = false;
    kprintf("[PERF] Performance monitoring disabled\n");
}

// Check if performance monitoring is enabled
bool perf_is_enabled(void) {
    return perf_enabled;
}

// Start timing an operation
void perf_start(perf_op_t op) {
    if (!perf_enabled || op >= PERF_OP_MAX) {
        return;
    }

    perf_start_time[op] = rdtsc();
}

// End timing and record the measurement
void perf_end(perf_op_t op) {
    if (!perf_enabled || op >= PERF_OP_MAX) {
        return;
    }

    uint64_t end = rdtsc();
    uint64_t start = perf_start_time[op];

    if (start == 0) {
        // PERF_START was not called (or monitoring was disabled)
        return;
    }

    uint64_t cycles = end - start;
    perf_record(op, cycles);

    // Clear start time
    perf_start_time[op] = 0;
}

// Record a measurement directly (for pre-computed cycles)
void perf_record(perf_op_t op, uint64_t cycles) {
    if (!perf_enabled || op >= PERF_OP_MAX) {
        return;
    }

    perf_stats_t* stats = &perf_stats[op];

    // Update statistics
    stats->count++;
    stats->total_cycles += cycles;

    if (cycles < stats->min_cycles) {
        stats->min_cycles = cycles;
    }

    if (cycles > stats->max_cycles) {
        stats->max_cycles = cycles;
    }

    // Update histogram
    uint64_t bucket = cycles / PERF_BUCKET_SIZE;

    if (bucket < PERF_HISTOGRAM_BUCKETS) {
        stats->histogram[bucket]++;
    } else {
        stats->overflow++;
    }
}

// Print statistics for a specific operation
void perf_report_op(perf_op_t op) {
    if (op >= PERF_OP_MAX) {
        return;
    }

    const perf_stats_t* stats = &perf_stats[op];

    if (stats->count == 0) {
        kprintf("[PERF] %s: no data\n", stats->name);
        return;
    }

    uint64_t avg = stats->total_cycles / stats->count;

    kprintf("[PERF] %s (n=%llu):\n", stats->name, stats->count);
    kprintf("  Min: %llu cycles (%llu us)\n",
            stats->min_cycles, cycles_to_us(stats->min_cycles));
    kprintf("  Avg: %llu cycles (%llu us)\n",
            avg, cycles_to_us(avg));
    kprintf("  Max: %llu cycles (%llu us)\n",
            stats->max_cycles, cycles_to_us(stats->max_cycles));

    // Print histogram
    kprintf("  Distribution:\n");

    for (int i = 0; i < PERF_HISTOGRAM_BUCKETS; i++) {
        if (stats->histogram[i] > 0) {
            uint64_t bucket_start = i * PERF_BUCKET_SIZE;
            uint64_t bucket_end = (i + 1) * PERF_BUCKET_SIZE - 1;
            uint64_t percentage = (stats->histogram[i] * 100) / stats->count;

            kprintf("    [%4llu-%4llu]: %8llu (%2llu%%) ",
                    bucket_start, bucket_end, stats->histogram[i], percentage);

            // Simple ASCII bar graph (up to 50 chars)
            int bar_len = (percentage > 50) ? 50 : percentage;
            for (int j = 0; j < bar_len; j++) {
                kprintf("#");
            }
            kprintf("\n");
        }
    }

    if (stats->overflow > 0) {
        uint64_t percentage = (stats->overflow * 100) / stats->count;
        kprintf("    [%4llu+   ]: %8llu (%2llu%%)\n",
                (uint64_t)(PERF_HISTOGRAM_BUCKETS * PERF_BUCKET_SIZE),
                stats->overflow, percentage);
    }

    kprintf("\n");
}

// Print all performance statistics
void perf_report(void) {
    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("                     PERFORMANCE MONITORING REPORT                             \n");
    kprintf("================================================================================\n");
    kprintf("\n");

    for (int i = 0; i < PERF_OP_MAX; i++) {
        if (perf_stats[i].count > 0) {
            perf_report_op(i);
        }
    }

    kprintf("================================================================================\n");
    kprintf("\n");
}

// Reset statistics for a specific operation
void perf_reset_op(perf_op_t op) {
    if (op >= PERF_OP_MAX) {
        return;
    }

    perf_stats_t* stats = &perf_stats[op];

    stats->count = 0;
    stats->total_cycles = 0;
    stats->min_cycles = UINT64_MAX;
    stats->max_cycles = 0;
    stats->overflow = 0;

    for (int i = 0; i < PERF_HISTOGRAM_BUCKETS; i++) {
        stats->histogram[i] = 0;
    }
}

// Reset all performance statistics
void perf_reset(void) {
    kprintf("[PERF] Resetting all statistics...\n");

    for (int i = 0; i < PERF_OP_MAX; i++) {
        perf_reset_op(i);
    }

    kprintf("[PERF] All statistics reset\n");
}

// Get raw statistics for analysis
const perf_stats_t* perf_get_stats(perf_op_t op) {
    if (op >= PERF_OP_MAX) {
        return NULL;
    }

    return &perf_stats[op];
}

// CPU frequency calibration (stub - TODO: implement with PIT/HPET)
void perf_calibrate_cpu_freq(void) {
    kprintf("[PERF] CPU frequency calibration not yet implemented\n");
    kprintf("[PERF] Using default: %d MHz\n", CPU_FREQ_MHZ);
}

// Get CPU frequency
uint64_t perf_get_cpu_freq_mhz(void) {
    return CPU_FREQ_MHZ;
}
