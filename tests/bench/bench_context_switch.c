/**
 * Context Switch Benchmark
 *
 * Measures the cost of context switching between two processes.
 * This includes saving and restoring all CPU state (registers, CR3, etc.)
 */

#include "../../kernel/include/sched.h"
#include "../../kernel/include/perf.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/mem.h"

#define BENCH_ITERATIONS 10000

/**
 * Benchmark raw context switch overhead
 *
 * Creates two minimal processes and switches between them repeatedly.
 * Measures cycles per switch.
 */
void bench_context_switch(void) {
    kprintf("\n[BENCH] Context Switch Benchmark\n");
    kprintf("=================================\n");

    // Allocate two processes
    process_t* proc1 = process_create("bench_proc1", NULL);
    process_t* proc2 = process_create("bench_proc2", NULL);

    if (!proc1 || !proc2) {
        kprintf("[BENCH] ERROR: Failed to create benchmark processes\n");
        return;
    }

    // Initialize statistics
    perf_stat_t stats;
    perf_stat_init(&stats, "Context Switch");

    kprintf("[BENCH] Running %d context switches...\n", BENCH_ITERATIONS * 2);

    // Warmup (to populate caches)
    for (int i = 0; i < 100; i++) {
        context_switch(proc1, proc2);
        context_switch(proc2, proc1);
    }

    // Benchmark loop
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        // Measure proc1 -> proc2
        uint64_t start = rdtsc_fence();
        context_switch(proc1, proc2);
        uint64_t end = rdtscp();
        perf_stat_record(&stats, end - start);

        // Measure proc2 -> proc1
        start = rdtsc_fence();
        context_switch(proc2, proc1);
        end = rdtscp();
        perf_stat_record(&stats, end - start);
    }

    // Report results
    perf_stat_report(&stats);

    // Calculate switches per second
    uint64_t avg_cycles = stats.total_cycles / stats.iterations;
    uint64_t cpu_freq_hz = perf_get_cpu_freq_mhz() * 1000000;
    uint64_t switches_per_sec = cpu_freq_hz / avg_cycles;

    kprintf("\n[BENCH] Performance:\n");
    kprintf("  Switches/second: %llu\n", switches_per_sec);
    kprintf("  Time per switch: %.2f us\n", cycles_to_us(avg_cycles));

    // Cleanup
    process_destroy(proc1);
    process_destroy(proc2);

    kprintf("\n");
}

/**
 * Benchmark context switch with varying workloads
 *
 * Tests context switch performance under different conditions:
 * - Clean processes (no FPU/SIMD state)
 * - Dirty FPU state
 * - Different page table sizes
 */
void bench_context_switch_workloads(void) {
    kprintf("\n[BENCH] Context Switch Workload Benchmark\n");
    kprintf("==========================================\n");

    // TODO: Implement different workload tests
    // 1. Clean context switch (current implementation)
    // 2. After FPU/SSE usage
    // 3. With large address space (many page tables)
    // 4. Cross-CPU context switch (on SMP systems)

    kprintf("[BENCH] Workload tests not yet implemented\n");
}

/**
 * Benchmark scheduler overhead
 *
 * Measures the cost of the scheduler's pick_next operation
 * with varying numbers of ready processes.
 */
void bench_scheduler_overhead(void) {
    kprintf("\n[BENCH] Scheduler Overhead Benchmark\n");
    kprintf("=====================================\n");

    const int queue_sizes[] = {1, 5, 10, 50, 100, 500, 1000};
    const int num_sizes = sizeof(queue_sizes) / sizeof(queue_sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int queue_size = queue_sizes[s];

        // Create N processes
        process_t** procs = kmalloc(queue_size * sizeof(process_t*));
        for (int i = 0; i < queue_size; i++) {
            procs[i] = process_create("bench_proc", NULL);
            if (procs[i]) {
                scheduler_add_process(procs[i]);
            }
        }

        // Measure pick_next performance
        perf_stat_t stats;
        perf_stat_init(&stats, "scheduler_pick_next");

        for (int i = 0; i < 1000; i++) {
            uint64_t start = rdtsc_fence();
            process_t* next = scheduler_pick_next();
            uint64_t end = rdtscp();

            if (next) {
                perf_stat_record(&stats, end - start);
                // Add back to queue
                scheduler_add_process(next);
            }
        }

        kprintf("\n[BENCH] Queue size: %d processes\n", queue_size);
        perf_stat_report(&stats);

        // Cleanup
        for (int i = 0; i < queue_size; i++) {
            if (procs[i]) {
                process_destroy(procs[i]);
            }
        }
        kfree(procs);
    }

    kprintf("\n");
}

/**
 * Run all context switch benchmarks
 */
void bench_context_switch_all(void) {
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  CONTEXT SWITCH BENCHMARK SUITE\n");
    kprintf("=============================================\n");

    bench_context_switch();
    bench_scheduler_overhead();
    bench_context_switch_workloads();

    kprintf("=============================================\n");
    kprintf("  BENCHMARK COMPLETE\n");
    kprintf("=============================================\n");
    kprintf("\n");
}
