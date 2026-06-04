#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include "types.h"
#include "kernel.h"
#include "smp.h"

/*
 * health_monitor.h — System Health and Liveness Monitoring
 * =========================================================
 *
 * Provides per-CPU health metrics for detecting stalls, deadlocks, and memory
 * leaks in SMP environments. Tracks heartbeat counters, queue depth, and
 * reference-counted allocation statistics to enable early detection of:
 *   - CPU stalls (heartbeat not advancing)
 *   - Work queue backlog (queue_depth trending up)
 *   - Memory leaks (ownership_allocs > ownership_frees over time)
 *
 * Design:
 *   - Per-CPU lock-free counters (written by owning CPU only)
 *   - Centralized sampling via health_monitor_sample() (coordinator CPU reads)
 *   - Detection functions return counts (non-invasive, log only)
 *   - Integration points: kmalloc_ref/kput track ownership_{allocs,frees}
 *
 * Usage:
 *   // At boot (BSP only)
 *   health_monitor_init();
 *
 *   // Periodic sampling (e.g., timer tick on CPU 0)
 *   health_monitor_sample();
 *   bool stalls = health_monitor_detect_stalls();
 *   bool leaks = health_monitor_detect_leaks();
 *   if (stalls || leaks) {
 *       health_monitor_report();
 *   }
 */

/* =========================================================================
 * Data Structures
 * ========================================================================= */

/**
 * per_cpu_health_t — Health metrics for a single CPU
 *
 * Fields are written exclusively by the owning CPU (lock-free); read by
 * the monitoring CPU via health_monitor_sample().
 */
typedef struct per_cpu_health {
    /* Liveness tracking */
    uint64_t heartbeat;           // Increments every scheduler tick
    uint64_t last_heartbeat;      // Previous sample (for stall detection)

    /* Work queue metrics */
    uint32_t queue_depth;         // Pending runnable threads/jobs

    /* Memory ownership tracking (for leak detection) */
    uint32_t ownership_allocs;    // Total kmalloc_ref() calls
    uint32_t ownership_frees;     // Total kput(refcount=0) calls
    uint32_t ownership_leaks;     // allocs - frees (computed by detector)
} per_cpu_health_t;

/**
 * system_health_t — Global health state (all CPUs)
 *
 * Centralized structure for monitoring and diagnostics. Updated by
 * health_monitor_sample() and detection functions.
 */
typedef struct system_health {
    per_cpu_health_t cpu[MAX_CPUS];  // Per-CPU health snapshots

    /* Sampling metadata */
    uint64_t sample_count;            // Total samples taken

    /* Detection counters (cumulative since boot) */
    uint32_t stalls_detected;         // CPUs with non-advancing heartbeat
    uint32_t deadlocks_detected;      // Reserved for future deadlock detector
    uint32_t panics_detected;         // CPUs that entered panic state
} system_health_t;

/* =========================================================================
 * API Functions
 * ========================================================================= */

/**
 * health_monitor_init — Initialize health monitoring subsystem
 *
 * Called once at boot by the BSP. Zeroes all per-CPU counters and global state.
 * Safe to call before SMP bringup (only initializes data structures).
 */
void health_monitor_init(void);

/**
 * health_monitor_start_thread — Start the health monitoring background thread
 *
 * Should be called after the scheduler is initialized and running.
 * Creates a kernel thread that runs the health monitor loop, sampling metrics
 * every 5 seconds and detecting stalls and leaks.
 */
void health_monitor_start_thread(void);

/**
 * health_monitor_sample — Snapshot current health metrics from all online CPUs
 *
 * Reads per-CPU counters and updates the global system_health structure.
 * Call periodically (e.g., every 1-10 seconds) from a monitoring thread or
 * timer interrupt on the BSP.
 *
 * Thread-safety: Safe to call from any CPU, but typically invoked by a single
 * coordinator (e.g., CPU 0) to avoid redundant sampling.
 */
void health_monitor_sample(void);

/**
 * health_monitor_detect_stalls — Detect CPUs with stalled heartbeats
 *
 * Compares current heartbeat to last_heartbeat for each online CPU. A CPU is
 * considered stalled if its heartbeat has not advanced since the last sample.
 *
 * Returns: true if any CPU is stalled, false otherwise
 *
 * Side effects: Updates system_health.stalls_detected counter
 */
bool health_monitor_detect_stalls(void);

/**
 * health_monitor_detect_leaks — Detect CPUs with growing memory ownership leaks
 *
 * Computes ownership_leaks = ownership_allocs - ownership_frees for each CPU.
 * A leak is reported if the difference exceeds a threshold (e.g., 100 objects).
 *
 * Returns: true if any CPU has suspected leaks, false otherwise
 *
 * Side effects: Updates per_cpu_health.ownership_leaks field for each CPU
 *
 * Note: False positives are possible if allocations are legitimately long-lived
 * (e.g., persistent cache entries). Pair with temporal analysis (trending up
 * over multiple samples) for higher confidence.
 */
bool health_monitor_detect_leaks(void);

/**
 * health_monitor_detect_deadlock — Detect system-wide deadlock condition
 *
 * Checks if ALL online CPUs have stalled simultaneously (heartbeat not advancing).
 * This simple heuristic indicates a likely deadlock when all CPUs are frozen
 * in SMP environments with 2+ CPUs.
 *
 * Returns: true if deadlock detected (all CPUs stalled), false otherwise
 *
 * Side effects: Updates system_health.deadlocks_detected counter
 *
 * Note: Requires at least 2 samples for baseline comparison. Single-CPU systems
 * always return false (no deadlock detection on uniprocessor).
 */
bool health_monitor_detect_deadlock(void);

/**
 * health_monitor_report — Log human-readable health summary to console
 *
 * Prints the current state of all online CPUs including:
 *   - Heartbeat values (current, last, delta)
 *   - Queue depth
 *   - Ownership statistics (allocs, frees, suspected leaks)
 *   - Detection counters (stalls, deadlocks, panics)
 *
 * Thread-safety: Safe to call from any context (uses kprintf internally)
 */
void health_monitor_report(void);

/* =========================================================================
 * Per-CPU Helpers (called by owning CPU only)
 * ========================================================================= */

/**
 * health_monitor_tick — Update heartbeat counter for current CPU
 *
 * Called by scheduler tick handler on each CPU. Increments the local heartbeat
 * counter to signal liveness.
 *
 * Thread-safety: Must be called from the owning CPU (no cross-CPU writes)
 */
void health_monitor_tick(void);

/**
 * health_monitor_record_alloc — Record a kmalloc_ref allocation
 *
 * Called by kmalloc_ref() to track ownership allocations for leak detection.
 * Increments ownership_allocs for the current CPU.
 *
 * Thread-safety: Must be called from the owning CPU (no cross-CPU writes)
 */
void health_monitor_record_alloc(void);

/**
 * health_monitor_record_free — Record a kput(refcount=0) free
 *
 * Called by kput() when the final reference is released. Increments
 * ownership_frees for the current CPU.
 *
 * Thread-safety: Must be called from the owning CPU (no cross-CPU writes)
 */
void health_monitor_record_free(void);

/**
 * health_monitor_record_queue_depth — Update current queue depth
 *
 * Called by scheduler to reflect the current runnable thread count.
 * Updates queue_depth for the current CPU.
 *
 * @param depth: Number of runnable threads in the current CPU's run queue
 *
 * Thread-safety: Must be called from the owning CPU (no cross-CPU writes)
 */
void health_monitor_record_queue_depth(uint32_t depth);

/* =========================================================================
 * Accessor Functions (for diagnostics and testing)
 * ========================================================================= */

/**
 * health_monitor_get_state — Get pointer to global health state
 *
 * Returns: Pointer to the system_health_t structure (read-only for callers)
 *
 * Thread-safety: Pointer is stable; fields may change concurrently
 */
const system_health_t* health_monitor_get_state(void);

/**
 * health_monitor_get_cpu_health — Get per-CPU health snapshot
 *
 * @param cpu_id: Logical CPU ID (0 to MAX_CPUS-1)
 * @return: Pointer to per_cpu_health_t for the specified CPU, or NULL if invalid
 *
 * Thread-safety: Pointer is stable; fields may change concurrently
 */
const per_cpu_health_t* health_monitor_get_cpu_health(uint32_t cpu_id);

/* =========================================================================
 * Deadlock Recovery
 * ========================================================================= */

/**
 * health_monitor_recover_deadlock — Attempt to recover from detected deadlock
 *
 * Called when a deadlock is detected (e.g., all CPUs stalled for extended period).
 * Implements fail-stop principle: log comprehensive diagnostic state, then trigger
 * a clean kernel panic rather than allowing silent corruption or indefinite hang.
 *
 * Strategy:
 *   1. Log current health state via health_monitor_report()
 *   2. Trigger kernel_panic() with deadlock message
 *
 * Future enhancements (once SMP scheduler is stable):
 *   - Identify victim process via resource dependency graph
 *   - Force-unlock specific mutexes/spinlocks
 *   - Terminate victim process and continue system operation
 *
 * For now: Fail-stop is safer than attempting recovery with incomplete state.
 *
 * Design rationale:
 *   - Better to crash visibly (panic) than corrupt silently (force-unlock)
 *   - Panic provides full diagnostic dump (registers, stack, memory)
 *   - Reproducible failure is better than undefined behavior
 *
 * Thread-safety: Safe to call from any CPU (disables interrupts via panic)
 *
 * @note This function does NOT return (calls kernel_panic)
 */
NORETURN void health_monitor_recover_deadlock(void);

#endif // HEALTH_MONITOR_H
