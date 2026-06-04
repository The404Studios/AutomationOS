/*
 * Example usage of scheduler per-CPU statistics and ready_count accessors
 * ========================================================================
 *
 * This file demonstrates how to use the newly implemented accessor and reset
 * functions for per-CPU scheduler statistics and ready_count tracking.
 *
 * These functions are useful for:
 *  - Debugging scheduler state
 *  - Monitoring load distribution across CPUs
 *  - Validating scheduler invariants
 *  - Performance analysis and profiling
 */

#include "../../include/sched.h"
#include "../../include/kernel.h"

// Example 1: Query ready_count for diagnostics
void example_query_ready_count(void) {
    // Get ready count for the current CPU
    uint32_t current_ready = scheduler_get_current_ready_count();
    kprintf("[STATS] Current CPU has %d processes ready\n", current_ready);

    // Query specific CPU (useful in SMP environment)
    for (uint32_t cpu = 0; cpu < 8; cpu++) {
        uint32_t ready = scheduler_get_ready_count(cpu);
        if (ready > 0) {
            kprintf("[STATS] CPU %d has %d processes ready\n", cpu, ready);
        }
    }
}

// Example 2: Validate scheduler state integrity
void example_validate_scheduler_state(void) {
    // Validate that ready_count matches actual runqueue contents
    // Note: Should be called with scheduler_lock held or interrupts disabled
    uint32_t cpu_id = 0;  // At N=1, always CPU 0

    int result = scheduler_validate_ready_count(cpu_id);
    if (result == 0) {
        kprintf("[STATS] CPU %d ready_count validation PASSED\n", cpu_id);
    } else {
        kprintf("[STATS] CPU %d ready_count validation FAILED\n", cpu_id);
    }
}

// Example 3: Reset ready_count during CPU bring-up
void example_cpu_initialization(uint32_t cpu_id) {
    kprintf("[STATS] Initializing CPU %d\n", cpu_id);

    // Reset ready_count to 0 (part of runqueue initialization)
    scheduler_reset_ready_count(cpu_id);

    kprintf("[STATS] CPU %d ready_count reset to 0\n", cpu_id);
}

// Example 4: Access per-CPU statistics (placeholder for future use)
void example_query_cpu_stats(void) {
    cpu_stats_t stats;
    uint32_t cpu_id = 0;

    int result = scheduler_get_cpu_stats(cpu_id, &stats);
    if (result == 0) {
        kprintf("[STATS] CPU %d statistics:\n", cpu_id);
        kprintf("[STATS]   reserved field: %llu\n", stats.reserved);
        // Note: The reserved field is currently unused (placeholder for brick 5+)
        // Future bricks will populate this with real per-CPU tick counters
    } else {
        kprintf("[STATS] Failed to retrieve stats for CPU %d\n", cpu_id);
    }
}

// Example 5: Reset all CPU statistics (e.g., for profiling sessions)
void example_reset_all_stats(void) {
    kprintf("[STATS] Resetting all CPU statistics\n");

    // Reset statistics for all online CPUs
    scheduler_reset_all_cpu_stats();

    kprintf("[STATS] All CPU statistics reset\n");
}

// Example 6: Monitoring load distribution in an SMP environment
void example_monitor_load_distribution(void) {
    kprintf("[STATS] ===== Load Distribution Report =====\n");

    uint32_t total_ready = 0;

    for (uint32_t cpu = 0; cpu < 8; cpu++) {
        uint32_t ready = scheduler_get_ready_count(cpu);
        total_ready += ready;

        if (ready > 0) {
            kprintf("[STATS] CPU %d: %d ready processes\n", cpu, ready);
        }
    }

    kprintf("[STATS] Total ready processes across all CPUs: %d\n", total_ready);
    kprintf("[STATS] ======================================\n");
}

/*
 * Integration notes:
 * ------------------
 *
 * 1. At N=1 (single CPU), only CPU 0 is online and used
 *    - All operations target cpus[0]
 *    - Multi-CPU loops will skip offline CPUs
 *
 * 2. When SMP is enabled (brick 5+):
 *    - These functions will work across all online CPUs
 *    - ready_count tracks per-CPU runqueue depth
 *    - cpu_stats_t will be populated with real tick counters
 *
 * 3. Thread safety:
 *    - scheduler_reset_ready_count() uses scheduler_lock internally
 *    - Other functions are safe for concurrent reads (atomic uint64_t on x86-64)
 *    - scheduler_validate_ready_count() must be called with lock held
 *
 * 4. Error handling:
 *    - Functions return 0/-1 or NULL for invalid cpu_id
 *    - Out-of-range CPU IDs are silently ignored
 */
