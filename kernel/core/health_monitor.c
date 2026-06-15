/*
 * Health Monitoring System
 * ========================
 *
 * Per-CPU health monitoring with leak detection for ownership tracking.
 * Detects memory leaks by comparing allocation/free counts across all CPUs.
 * Monitors CPU stalls and queue depth anomalies via a background thread.
 */

#include "../include/health_monitor.h"
#include "../include/smp.h"
#include "../include/kernel.h"
#include "../include/sched.h"
#include "../include/drivers.h"
#include "../include/string.h"

/* Global health state */
static system_health_t g_system_health;
static int g_health_monitor_running = 0;

/**
 * health_monitor_init - Initialize health monitoring subsystem
 *
 * Called once at boot by the BSP. Zeroes all per-CPU counters and global state.
 * Spawns the health monitoring background thread.
 */
void health_monitor_init(void) {
    /* Zero all health state */
    memset(&g_system_health, 0, sizeof(system_health_t));

    kprintf("[HEALTH] Health monitoring initialized\n");

    /* Note: The monitoring thread will be started later after the scheduler
     * is running. For now we just initialize the data structures. */
}

/**
 * health_monitor_thread - Background monitoring thread (kernel thread)
 *
 * Runs in an infinite loop, sampling health metrics every 5 seconds and
 * detecting anomalies (CPU stalls, memory leaks). This is a kernel-mode
 * thread that never returns.
 *
 * @param arg: Unused thread argument
 */
static void health_monitor_thread(void* arg) {
    (void)arg;

    kprintf("[HEALTH] Health monitor thread started\n");
    g_health_monitor_running = 1;

    while (1) {
        /* Sleep for 5 seconds (5000 ms) */
        uint64_t now = timer_get_ticks();
        uint64_t wake_time = now + 5000;  /* PIT runs at 1000 Hz, so ticks == ms */

        /* Yield-based sleep: wait until wake_time is reached.
         * We can't call sys_sleep directly from kernel context, so we use
         * a simple busy-wait with yields. In a production system, this would
         * use a proper kernel sleep primitive. */
        while (timer_get_ticks() < wake_time) {
            /* Yield to other processes to avoid hogging CPU */
            schedule();
        }

        /* Sample all CPUs */
        health_monitor_sample();

#if defined(SMP_DSPLIT) && defined(SMP_RUNMASK)
        /* DESKTOP-SPLIT-0 observation (one-shot, ~60s in = sample 12): print
         * the OBSERVED ran_on_cpus reality for the proof -- the desktop core
         * (compositor/init/terminal) must show ran=0x1 (CPU0 only) and at
         * least one allowlisted BATCH app must have run with bit1 set. Reads
         * scalar PCB fields via the ref-safe table API; prints once, costs
         * nothing afterward (the T410 idle budget). */
        {
            static int dsplit_reported = 0;
            static int dsplit_samples  = 0;
            dsplit_samples++;
            if (!dsplit_reported && dsplit_samples >= 12) {
                dsplit_reported = 1;
                extern process_t* process_get_by_pid(uint32_t pid);
                extern void process_unref(process_t* proc);
                int cpu1_users = 0;
                for (uint32_t pid = 1; pid < 256; pid++) {
                    process_t* p = process_get_by_pid(pid);
                    if (!p) continue;
                    uint32_t ran = p->ran_on_cpus;
                    int core =
                        (p->name[0]=='s' && p->name[1]=='b') /* sbin/...    */ ||
                        p->pid == 1;                          /* init        */
                    if (ran & 0x2u) cpu1_users++;
                    if ((ran & 0x2u) || core) {
                        kprintf("[DSPLIT] observed: pid=%d '%s' ran=0x%x\n",
                                p->pid, p->name, ran);
                    }
                    process_unref(p);
                }
                kprintf("[DSPLIT] observation: live procs that ran on CPU1 = %d\n",
                        cpu1_users);
            }
        }
#endif
#if defined(SMP_THREAD_INHERIT) && defined(SMP_RUNMASK)
        /* SMP-THREAD-INHERIT-0 observation (one-shot, ~60s in = sample 12): the
         * threaded BATCH probe. The parent (is_thread==0) AND both worker
         * threads (is_thread==1, named "threadprobe-thr") must show ran=0x2
         * (CPU1) with the SHARED mm accumulator never spanning two CPUs --
         * proving the workers INHERITED the parent's CPU1 placement, not the
         * CPU0 ctor default. All three are persistent CPU1 residents, so the
         * live walk reliably catches them. Scalar PCB reads via the ref-safe
         * table API; one-shot. */
        {
            static int ti_reported = 0;
            static int ti_samples  = 0;
            ti_samples++;
            if (!ti_reported && ti_samples >= 12) {
                ti_reported = 1;
                extern process_t* process_get_by_pid(uint32_t pid);
                extern void process_unref(process_t* proc);
                int parent_found = 0, parent_cpu1 = 0, parent_batch = 0;
                uint32_t mm_ran = 0, home = 99;
                int workers = 0, workers_cpu1 = 0, workers_cpu0 = 0, workers_batch = 0;
                for (uint32_t pid = 1; pid < 256; pid++) {
                    process_t* p = process_get_by_pid(pid);
                    if (!p) continue;
                    /* name prefix "threadprobe" matches the parent AND its
                     * "threadprobe-thr" worker threads. */
                    const char* pref = "threadprobe";
                    int match = 1;
                    for (int k = 0; pref[k]; k++) {
                        if (p->name[k] != pref[k]) { match = 0; break; }
                    }
                    if (match) {
                        uint32_t ran = p->ran_on_cpus;
                        int cls = (int)p->sched.sched_class;
                        if (p->is_thread) {
                            workers++;
                            if (ran == 0x2u) workers_cpu1++;
                            if (ran & 0x1u)  workers_cpu0++;
                            if (cls == SCHED_CLASS_BATCH) workers_batch++;
                            kprintf("[THREADINHERIT] observed: worker pid=%d "
                                    "ran=0x%x class=%d\n", p->pid, ran, cls);
                        } else {
                            parent_found = 1;
                            parent_cpu1  = (ran == 0x2u);
                            parent_batch = (cls == SCHED_CLASS_BATCH);
                            if (p->mm_place) {
                                mm_ran = p->mm_place->ran_on_cpus;
                                home   = p->mm_place->home_cpu;
                            }
                            kprintf("[THREADINHERIT] observed: parent '%s' pid=%d "
                                    "ran=0x%x home_cpu=%u mm_ran=0x%x class=%d\n",
                                    p->name, p->pid, ran, home, mm_ran, cls);
                        }
                    }
                    process_unref(p);
                }
                int mm_bits = 0;
                for (uint32_t r = mm_ran; r; r &= (r - 1)) mm_bits++;
                int batch_parent_cpu1 = parent_found && parent_cpu1;
                int workers_same_cpu  = (workers == 2) && (workers_cpu1 == 2) &&
                                        (workers_cpu0 == 0);
                int sched_inherit     = parent_batch && (workers == 2) &&
                                        (workers_batch == 2);
                int mm_single_cpu     = (mm_bits <= 1) && (home == 1);
                kprintf("[THREADINHERIT] summary: batch_parent_cpu1=%d "
                        "workers_same_cpu=%d sched_inherit=%d mm_single_cpu=%d "
                        "workers=%d (cpu1=%d cpu0=%d)\n",
                        batch_parent_cpu1, workers_same_cpu, sched_inherit,
                        mm_single_cpu, workers, workers_cpu1, workers_cpu0);
            }
        }
#endif

        /* Detect anomalies */
        bool stalls = health_monitor_detect_stalls();
        bool leaks = health_monitor_detect_leaks();
        bool deadlock = health_monitor_detect_deadlock();

        if (deadlock) {
            kprintf("[HEALTH] SYSTEM DEADLOCK: all CPUs frozen!\n");
            health_monitor_report();
        } else if (stalls) {
            kprintf("[HEALTH] CPU stall detected!\n");
            health_monitor_report();
        }

        if (leaks) {
            kprintf("[HEALTH] Memory leak detected!\n");
            health_monitor_report();
        }
    }
}

/**
 * health_monitor_start_thread - Start the health monitoring background thread
 *
 * Should be called after the scheduler is initialized and running.
 * Creates a kernel thread that runs health_monitor_thread().
 */
void health_monitor_start_thread(void) {
    if (g_health_monitor_running) {
        kprintf("[HEALTH] Monitor thread already running\n");
        return;
    }

    /* Create a kernel thread for health monitoring.
     * Since we don't have a proper kernel thread creation API yet,
     * we'll create a process with a kernel-mode entry point.
     * This is a simplified approach - a full implementation would
     * use a dedicated kernel thread subsystem. */

    process_t* monitor_proc = process_create("health_monitor",
                                             (void*)health_monitor_thread);
    if (!monitor_proc) {
        kprintf("[HEALTH] Failed to create monitor thread\n");
        return;
    }

    /* Mark as ready and add to scheduler */
    process_set_ready(monitor_proc);
    scheduler_add_process(monitor_proc);

    kprintf("[HEALTH] Health monitor thread scheduled\n");
}

/**
 * health_monitor_sample - Snapshot current health metrics from all online CPUs
 *
 * Reads per-CPU counters and updates the global system_health structure.
 * Call periodically (e.g., every 1-10 seconds) from a monitoring thread.
 */
void health_monitor_sample(void) {
    g_system_health.sample_count++;

    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        percpu_data_t* cpu_data_ptr = cpu_data(cpu);
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];

        /* Save previous heartbeat for stall detection */
        snapshot->last_heartbeat = snapshot->heartbeat;

        /* Sample current values from the percpu_data health struct */
        snapshot->heartbeat = cpu_data_ptr->health.heartbeat;
        snapshot->queue_depth = 0;  /* TODO: Add queue_depth to percpu_data_t.health */
        snapshot->ownership_allocs = cpu_data_ptr->health.ownership_allocs;
        snapshot->ownership_frees = cpu_data_ptr->health.ownership_frees;
    }
}

/**
 * health_monitor_detect_stalls - Detect CPUs with stalled heartbeats
 *
 * Compares current heartbeat to last_heartbeat for each online CPU.
 * A CPU is considered stalled if its heartbeat has not advanced.
 *
 * Returns: true if any CPU is stalled, false otherwise
 */
bool health_monitor_detect_stalls(void) {
    uint32_t stalled = 0;

    /* Skip stall detection on first sample (no baseline yet) */
    if (g_system_health.sample_count <= 1) {
        return false;
    }

    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];

        /* A CPU is stalled if heartbeat hasn't advanced since last sample */
        if (snapshot->heartbeat == snapshot->last_heartbeat) {
            stalled++;
            kprintf("[HEALTH] CPU%d stalled: heartbeat=%lu (no change)\n",
                    cpu, (unsigned long)snapshot->heartbeat);
        }
    }

    if (stalled > 0) {
        g_system_health.stalls_detected += stalled;
    }

    return stalled > 0;
}

/**
 * health_monitor_detect_leaks - Detect CPUs with growing memory ownership leaks
 *
 * Computes ownership_leaks = ownership_allocs - ownership_frees for each CPU.
 * A leak is reported if the difference exceeds a threshold (100 objects).
 *
 * Returns: true if any CPU has suspected leaks, false otherwise
 */
bool health_monitor_detect_leaks(void) {
    uint32_t leaked_cpus = 0;
    const uint32_t LEAK_THRESHOLD = 100;

    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];
        uint32_t allocs = snapshot->ownership_allocs;
        uint32_t frees = snapshot->ownership_frees;
        uint32_t leaks = (allocs > frees) ? (allocs - frees) : 0;

        snapshot->ownership_leaks = leaks;

        /* Threshold: Warn if >100 leaked objects */
        if (leaks > LEAK_THRESHOLD) {
            kprintf("[HEALTH] CPU%d leak: %u objects (%u allocs - %u frees)\n",
                    cpu, leaks, allocs, frees);
            leaked_cpus++;
        }
    }

    return leaked_cpus > 0;
}

/**
 * health_monitor_detect_deadlock - Detect system-wide deadlock condition
 *
 * Checks if ALL online CPUs have stalled simultaneously (heartbeat not advancing).
 * This simple heuristic indicates a likely deadlock when all CPUs are frozen.
 *
 * Returns: true if deadlock detected (all CPUs stalled), false otherwise
 */
bool health_monitor_detect_deadlock(void) {
    /* Skip deadlock detection on first sample (no baseline yet) */
    if (g_system_health.sample_count <= 1) {
        return false;
    }

    /* Skip deadlock detection on single-CPU systems */
    if (smp_num_online <= 1) {
        return false;
    }

    int stalled_cpus = 0;

    /* Count how many CPUs have stalled heartbeats */
    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];
        uint64_t current = snapshot->heartbeat;
        uint64_t prev = snapshot->last_heartbeat;

        if (current == prev) {
            stalled_cpus++;
        }
    }

    /* If ALL online CPUs stalled simultaneously = likely deadlock */
    if (stalled_cpus == smp_num_online) {
        kprintf("[HEALTH] DEADLOCK DETECTED: all %d CPUs stalled\n",
                smp_num_online);
        g_system_health.deadlocks_detected++;
        return true;
    }

    return false;
}

/**
 * health_monitor_report - Log human-readable health summary to console
 *
 * Prints the current state of all online CPUs including heartbeat values,
 * queue depth, ownership statistics, and detection counters.
 */
void health_monitor_report(void) {
    kprintf("\n=== Health Monitor Report ===\n");
    kprintf("Samples: %lu\n", (unsigned long)g_system_health.sample_count);
    kprintf("Total stalls detected: %u\n", g_system_health.stalls_detected);
    kprintf("Total deadlocks detected: %u\n", g_system_health.deadlocks_detected);
    kprintf("Total panics detected: %u\n", g_system_health.panics_detected);
    kprintf("\n");

    uint32_t total_allocs = 0;
    uint32_t total_frees = 0;
    uint32_t total_leaks = 0;

    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];

        kprintf("CPU%d:\n", cpu);
        kprintf("  Heartbeat: %lu (prev: %lu, delta: %ld)\n",
                (unsigned long)snapshot->heartbeat,
                (unsigned long)snapshot->last_heartbeat,
                (long)(snapshot->heartbeat - snapshot->last_heartbeat));
        kprintf("  Queue depth: %u\n", snapshot->queue_depth);
        kprintf("  Ownership: allocs=%u, frees=%u, leaks=%u\n",
                snapshot->ownership_allocs,
                snapshot->ownership_frees,
                snapshot->ownership_leaks);
        kprintf("\n");

        total_allocs += snapshot->ownership_allocs;
        total_frees += snapshot->ownership_frees;
        total_leaks += snapshot->ownership_leaks;
    }

    kprintf("System totals:\n");
    kprintf("  Allocs: %u\n", total_allocs);
    kprintf("  Frees: %u\n", total_frees);
    kprintf("  Leaks: %u\n", total_leaks);
    kprintf("============================\n\n");
}

/**
 * health_monitor_tick - Update heartbeat counter for current CPU
 *
 * Called by scheduler tick handler on each CPU. Increments the local
 * heartbeat counter to signal liveness.
 */
void health_monitor_tick(void) {
    /* Get current CPU's health structure */
    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) {
        return;
    }

    percpu_data_t* cpu_data_ptr = cpu_data(cpu);
    __atomic_add_fetch(&cpu_data_ptr->health.heartbeat, 1, __ATOMIC_RELAXED);
}

/**
 * health_monitor_record_alloc - Record a kmalloc_ref allocation
 *
 * Called by kmalloc_ref() to track ownership allocations for leak detection.
 */
void health_monitor_record_alloc(void) {
    uint32_t cpu = cpu_id();
    if (cpu >= smp_num_online) return;
    __atomic_add_fetch(&cpu_data(cpu)->health.ownership_allocs, 1, __ATOMIC_RELAXED);
}

/**
 * health_monitor_record_free - Record a kput(refcount=0) free
 *
 * Called by kput() when the final reference is released.
 */
void health_monitor_record_free(void) {
    uint32_t cpu = cpu_id();
    if (cpu >= smp_num_online) return;
    __atomic_add_fetch(&cpu_data(cpu)->health.ownership_frees, 1, __ATOMIC_RELAXED);
}

/**
 * health_monitor_record_queue_depth - Update current queue depth
 *
 * Called by scheduler to reflect the current runnable thread count.
 *
 * @param depth: Number of runnable threads in the current CPU's run queue
 *
 * NOTE: Currently a no-op because queue_depth is not yet added to percpu_data_t.
 * When implementing SMP scheduler with per-CPU runqueues, add a queue_depth
 * field to the health struct in smp.h and update this function.
 */
void health_monitor_record_queue_depth(uint32_t depth) {
    (void)depth;
    /* TODO: Add queue_depth field to percpu_data_t.health in smp.h */
    /* uint32_t cpu = cpu_id();
     * if (cpu >= smp_num_online) return;
     * __atomic_store_n(&cpu_data(cpu)->health.queue_depth, depth, __ATOMIC_RELAXED);
     */
}

/**
 * health_monitor_get_state - Get pointer to global health state
 *
 * Returns: Pointer to the system_health_t structure (read-only for callers)
 */
const system_health_t* health_monitor_get_state(void) {
    return &g_system_health;
}

/**
 * health_monitor_get_cpu_health - Get per-CPU health snapshot
 *
 * @param cpu_id: Logical CPU ID (0 to MAX_CPUS-1)
 * @return: Pointer to per_cpu_health_t for the specified CPU, or NULL if invalid
 */
const per_cpu_health_t* health_monitor_get_cpu_health(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return NULL;
    }
    return &g_system_health.cpu[cpu_id];
}

/* ============================================================================
 * Legacy API (for backward compatibility)
 * ========================================================================== */

/**
 * health_monitor_reset - Reset all health monitoring counters
 *
 * Clears allocation, free, and leak counters for all CPUs.
 */
void health_monitor_reset(void) {
    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        cpu_data(cpu)->health.ownership_allocs = 0;
        cpu_data(cpu)->health.ownership_frees = 0;
        cpu_data(cpu)->health.ownership_leaks = 0;
    }
}

/**
 * health_monitor_print_stats - Print health statistics for all CPUs
 *
 * Alias for health_monitor_report() for backward compatibility.
 */
void health_monitor_print_stats(void) {
    health_monitor_report();
}

/**
 * health_monitor_get_total_leaks - Get total leak count across all CPUs
 *
 * Returns: Total number of leaked objects across all online CPUs
 */
uint32_t health_monitor_get_total_leaks(void) {
    uint32_t total = 0;

    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        total += g_system_health.cpu[cpu].ownership_leaks;
    }

    return total;
}

/* ============================================================================
 * Deadlock Recovery
 * ========================================================================== */

/**
 * health_monitor_recover_deadlock - Attempt to recover from detected deadlock
 *
 * Implements fail-stop principle: log diagnostic state, then panic cleanly.
 * This is safer than attempting force-unlock recovery which could corrupt state.
 *
 * Strategy:
 *   1. Log comprehensive health state via health_monitor_report()
 *   2. Trigger kernel_panic() with deadlock message
 *
 * Future enhancements (post-SMP stabilization):
 *   - Resource dependency graph analysis to identify victim process
 *   - Selective force-unlock of specific locks with known safety properties
 *   - Process termination with rollback to last known-good state
 *
 * Design rationale:
 *   - Fail-stop: Better to crash visibly than corrupt silently
 *   - Reproducibility: Clean panic gives debuggable state, force-unlock may hide bugs
 *   - Safety: Without full lock ordering and dependency tracking, recovery is unsafe
 *
 * @note This function does NOT return (calls kernel_panic which halts the system)
 */
NORETURN void health_monitor_recover_deadlock(void) {
    kprintf("\n");
    kprintf("\033[1;31m"); /* Red bold */
    kprintf("╔════════════════════════════════════════════════════════════╗\n");
    kprintf("║  DEADLOCK DETECTED - ATTEMPTING RECOVERY                   ║\n");
    kprintf("╚════════════════════════════════════════════════════════════╝\n");
    kprintf("\033[0m"); /* Reset */
    kprintf("\n");

    kprintf("[HEALTH] Deadlock recovery initiated\n");
    kprintf("[HEALTH] Strategy: Log state + fail-stop (panic)\n");
    kprintf("\n");

    /* Strategy 1: Log comprehensive health state for post-mortem analysis */
    kprintf("[HEALTH] Step 1: Logging system health state...\n");
    health_monitor_report();

    /* Strategy 2: Trigger kernel panic (clean fail-stop)
     *
     * Rationale:
     *   - Better to panic cleanly than hang forever (liveness vs. safety)
     *   - Better to crash visibly than corrupt silently (observability)
     *   - Panic handler provides full diagnostic dump (registers, stack, memory)
     *   - Reproducible failure is debuggable; corrupted state is not
     *
     * Future alternative strategies (requires more SMP infrastructure):
     *   - Deadlock cycle detection via resource allocation graphs
     *   - Victim process selection (e.g., youngest, least CPU time)
     *   - Force-unlock with lock ordering validation
     *   - Process termination with state rollback
     *
     * For now: Fail-stop is the only safe option without full lock tracking.
     */
    kprintf("[HEALTH] Step 2: Triggering kernel panic (fail-stop)...\n");
    kprintf("\n");
    kprintf("[HEALTH] \033[1;33mFail-stop principle:\033[0m\n");
    kprintf("[HEALTH]   Better to crash visibly than corrupt silently\n");
    kprintf("[HEALTH]   Better to panic cleanly than hang forever\n");
    kprintf("\n");

    /* This call does NOT return - kernel_panic halts all CPUs */
    kernel_panic("Deadlock detected - all CPUs stalled");

    /* Unreachable - panic halts the system */
    __builtin_unreachable();
}
