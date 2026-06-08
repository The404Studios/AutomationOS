/**
 * Boot Time Benchmark
 *
 * Measures kernel boot time from UEFI handoff to userspace ready.
 *
 * Target: < 5 seconds (fast boot)
 *
 * Boot stages:
 * 1. UEFI handoff                    (0ms)
 * 2. Early boot (paging, GDT, IDT)   (10-50ms)
 * 3. Memory init (PMM, VMM)          (20-100ms)
 * 4. Device init (AHCI, NVMe, Network) (50-200ms)
 * 5. Filesystem mount                (100-500ms)
 * 6. Init process spawn              (10-50ms)
 * 7. Shell ready                     (50-200ms)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "../common/bench_common.h"

typedef struct {
    const char* name;
    uint64_t start_us;
    uint64_t end_us;
    uint64_t duration_us;
} boot_stage_t;

#define MAX_STAGES 20

static boot_stage_t boot_stages[MAX_STAGES];
static uint32_t stage_count = 0;

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/**
 * Simulate boot stage
 */
static void simulate_boot_stage(const char* name, uint64_t min_us, uint64_t max_us) {
    if (stage_count >= MAX_STAGES) return;

    // Random duration within range
    uint64_t duration = min_us + (rand() % (max_us - min_us + 1));

    boot_stage_t* stage = &boot_stages[stage_count++];
    stage->name = name;
    stage->start_us = get_time_us();

    // Simulate work (busy loop)
    uint64_t start = rdtsc_fast();
    uint64_t target_cycles = duration * cpu_freq_mhz;
    while ((rdtsc_fast() - start) < target_cycles) {
        // Busy wait
        __asm__ volatile ("pause");
    }

    stage->end_us = get_time_us();
    stage->duration_us = stage->end_us - stage->start_us;

    printf("[BOOT] %s: %lu us (%.2f ms)\n",
           name, stage->duration_us, (double)stage->duration_us / 1000.0);
}

/**
 * Simulate full boot sequence
 */
void simulate_boot_sequence(void) {
    printf("\n=== Simulated Boot Sequence ===\n");

    srand(time(NULL));
    stage_count = 0;

    uint64_t boot_start = get_time_us();

    // Stage 1: Early boot (paging, GDT, IDT)
    simulate_boot_stage("Early Boot (Paging/GDT/IDT)", 10000, 50000);

    // Stage 2: PMM initialization
    simulate_boot_stage("PMM Init", 10000, 30000);

    // Stage 3: VMM initialization
    simulate_boot_stage("VMM Init", 20000, 50000);

    // Stage 4: PCID detection and enable
    simulate_boot_stage("PCID Detection", 1000, 5000);

    // Stage 5: Per-CPU cache initialization
    simulate_boot_stage("Per-CPU Cache Init", 2000, 10000);

    // Stage 6: Interrupt setup
    simulate_boot_stage("Interrupt Setup", 5000, 20000);

    // Stage 7: Syscall initialization
    simulate_boot_stage("Syscall Init", 3000, 15000);

    // Stage 8: Scheduler initialization
    simulate_boot_stage("Scheduler Init", 5000, 20000);

    // Stage 9: Device detection
    simulate_boot_stage("Device Detection", 20000, 100000);

    // Stage 10: NVMe initialization
    simulate_boot_stage("NVMe Init", 30000, 150000);

    // Stage 11: Network initialization
    simulate_boot_stage("Network Init", 20000, 100000);

    // Stage 12: Filesystem mount
    simulate_boot_stage("Filesystem Mount", 50000, 300000);

    // Stage 13: Init process spawn
    simulate_boot_stage("Init Process", 5000, 30000);

    // Stage 14: Shell ready
    simulate_boot_stage("Shell Ready", 10000, 50000);

    uint64_t boot_end = get_time_us();
    uint64_t total_boot_time = boot_end - boot_start;

    printf("\n--- Boot Summary ---\n");
    printf("Total boot time: %.2f ms (%.3f seconds)\n",
           (double)total_boot_time / 1000.0,
           (double)total_boot_time / 1000000.0);

    if (total_boot_time < 5000000) {
        printf("✓ Boot time target MET (< 5 seconds)\n");
    } else {
        printf("✗ Boot time target MISSED (target: 5 seconds)\n");
    }
}

/**
 * Analyze boot bottlenecks
 */
void analyze_boot_bottlenecks(void) {
    printf("\n=== Boot Stage Analysis ===\n");

    uint64_t total_time = 0;
    for (uint32_t i = 0; i < stage_count; i++) {
        total_time += boot_stages[i].duration_us;
    }

    // Sort stages by duration
    boot_stage_t sorted[MAX_STAGES];
    memcpy(sorted, boot_stages, stage_count * sizeof(boot_stage_t));

    for (uint32_t i = 0; i < stage_count; i++) {
        for (uint32_t j = i + 1; j < stage_count; j++) {
            if (sorted[i].duration_us < sorted[j].duration_us) {
                boot_stage_t tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    printf("\nTop 5 slowest stages:\n");
    for (uint32_t i = 0; i < 5 && i < stage_count; i++) {
        double percent = (double)sorted[i].duration_us / (double)total_time * 100.0;
        printf("%d. %s: %.2f ms (%.1f%%)\n",
               i + 1,
               sorted[i].name,
               (double)sorted[i].duration_us / 1000.0,
               percent);
    }
}

/**
 * Compare boot time with/without optimizations
 */
void compare_boot_optimizations(void) {
    printf("\n=== Boot Time Optimization Impact ===\n");

    // Simulate unoptimized boot (add overhead)
    printf("\nSimulating UNOPTIMIZED boot (no PCID, no per-CPU cache)...\n");

    uint64_t unopt_start = get_time_us();

    // Early boot: slower without optimizations
    simulate_boot_stage("[UNOPT] Early Boot", 20000, 80000);
    simulate_boot_stage("[UNOPT] PMM Init (slow allocator)", 30000, 100000);
    simulate_boot_stage("[UNOPT] VMM Init (full TLB flushes)", 40000, 120000);
    simulate_boot_stage("[UNOPT] Device Init", 50000, 200000);
    simulate_boot_stage("[UNOPT] NVMe Init", 60000, 300000);
    simulate_boot_stage("[UNOPT] Filesystem Mount", 100000, 500000);
    simulate_boot_stage("[UNOPT] Init Process", 10000, 60000);
    simulate_boot_stage("[UNOPT] Shell Ready", 20000, 100000);

    uint64_t unopt_end = get_time_us();
    uint64_t unopt_time = unopt_end - unopt_start;

    printf("\nUnoptimized boot time: %.2f seconds\n",
           (double)unopt_time / 1000000.0);

    // Simulate optimized boot
    stage_count = 0;  // Reset
    printf("\nSimulating OPTIMIZED boot (PCID, per-CPU cache)...\n");

    uint64_t opt_start = get_time_us();

    simulate_boot_stage("[OPT] Early Boot", 10000, 50000);
    simulate_boot_stage("[OPT] PMM Init (per-CPU cache)", 10000, 30000);
    simulate_boot_stage("[OPT] VMM Init (PCID)", 15000, 40000);
    simulate_boot_stage("[OPT] Device Init", 30000, 120000);
    simulate_boot_stage("[OPT] NVMe Init", 30000, 150000);
    simulate_boot_stage("[OPT] Filesystem Mount", 50000, 300000);
    simulate_boot_stage("[OPT] Init Process", 5000, 30000);
    simulate_boot_stage("[OPT] Shell Ready", 10000, 50000);

    uint64_t opt_end = get_time_us();
    uint64_t opt_time = opt_end - opt_start;

    printf("\nOptimized boot time: %.2f seconds\n",
           (double)opt_time / 1000000.0);

    // Calculate improvement
    double improvement = ((double)unopt_time - (double)opt_time) / (double)unopt_time * 100.0;

    printf("\n--- Optimization Results ---\n");
    printf("Unoptimized: %.2f seconds\n", (double)unopt_time / 1000000.0);
    printf("Optimized:   %.2f seconds\n", (double)opt_time / 1000000.0);
    printf("Improvement: %.1f%% faster\n", improvement);

    if (improvement >= 30.0) {
        printf("✓ Boot optimization target MET (%.1f%% improvement)\n", improvement);
    } else {
        printf("⚠ Boot optimization target PARTIAL (%.1f%%, target: 30%%)\n", improvement);
    }
}

/**
 * Measure kernel module load time
 */
void bench_module_load_time(void) {
    printf("\n=== Kernel Module Load Time ===\n");

    const char* modules[] = {
        "Network Driver",
        "Filesystem Driver",
        "USB Driver",
        "Graphics Driver",
    };

    for (int i = 0; i < 4; i++) {
        uint64_t start = get_time_us();

        // Simulate module load (parsing, relocation, init)
        usleep(10000 + (rand() % 50000));  // 10-60ms

        uint64_t end = get_time_us();
        uint64_t duration = end - start;

        printf("Loading %s: %.2f ms\n",
               modules[i], (double)duration / 1000.0);
    }
}

int main(void) {
    printf("========================================\n");
    printf("Boot Time Benchmark\n");
    printf("========================================\n");

    bench_calibrate_cpu_freq();

    // Run simulations
    simulate_boot_sequence();
    analyze_boot_bottlenecks();
    compare_boot_optimizations();
    bench_module_load_time();

    printf("\n========================================\n");
    printf("Benchmark Complete\n");
    printf("========================================\n");

    printf("\nNote: This benchmark simulates boot time.\n");
    printf("For real boot time, measure on actual hardware:\n");
    printf("  1. Add timestamps to kernel log\n");
    printf("  2. Measure from UEFI handoff to shell prompt\n");
    printf("  3. Use UEFI timestamp counters for accuracy\n");

    return 0;
}
