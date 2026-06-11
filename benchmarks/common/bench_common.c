/**
 * Common benchmark utilities implementation
 */
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>

uint64_t cpu_freq_mhz = 0;

/**
 * Calibrate CPU frequency using RDTSC and sleep
 */
void bench_calibrate_cpu_freq(void) {
    printf("[CALIBRATE] Measuring CPU frequency...\n");

    // Measure multiple times for accuracy
    uint64_t measurements[5];
    for (int i = 0; i < 5; i++) {
        uint64_t start = rdtsc_fence();
        usleep(100000);  // 100ms
        uint64_t end = rdtsc_fence();
        measurements[i] = (end - start) / 100000;  // cycles per microsecond = MHz
    }

    // Take median to avoid outliers
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (measurements[i] > measurements[j]) {
                uint64_t tmp = measurements[i];
                measurements[i] = measurements[j];
                measurements[j] = tmp;
            }
        }
    }

    cpu_freq_mhz = measurements[2];  // Median
    printf("[CALIBRATE] CPU frequency: %lu MHz\n", cpu_freq_mhz);
}

/**
 * Comparison function for qsort
 */
static int compare_uint64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/**
 * Calculate statistics from samples
 */
void bench_calculate_stats(uint64_t* samples, uint32_t count, bench_stats_t* stats) {
    if (count == 0) return;

    // Sort samples for percentile calculation
    uint64_t* sorted = malloc(count * sizeof(uint64_t));
    memcpy(sorted, samples, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), compare_uint64);

    // Min, max, total
    stats->min = sorted[0];
    stats->max = sorted[count - 1];
    stats->count = count;

    stats->total = 0;
    for (uint32_t i = 0; i < count; i++) {
        stats->total += sorted[i];
    }

    // Mean
    stats->mean = (double)stats->total / (double)count;

    // Standard deviation
    double variance = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        double diff = (double)sorted[i] - stats->mean;
        variance += diff * diff;
    }
    stats->std_dev = sqrt(variance / (double)count);

    // Percentiles
    stats->p50 = sorted[count / 2];
    stats->p95 = sorted[(count * 95) / 100];
    stats->p99 = sorted[(count * 99) / 100];

    free(sorted);
}

/**
 * Print benchmark statistics
 */
void bench_print_stats(const char* name, bench_stats_t* stats, const char* unit) {
    printf("\n=== %s ===\n", name);
    printf("Samples:    %lu\n", stats->count);
    printf("Min:        %.2f %s\n", cycles_to_ns(stats->min), unit);
    printf("Max:        %.2f %s\n", cycles_to_ns(stats->max), unit);
    printf("Mean:       %.2f %s\n", cycles_to_ns((uint64_t)stats->mean), unit);
    printf("Std Dev:    %.2f %s\n", cycles_to_ns((uint64_t)stats->std_dev), unit);
    printf("Median:     %.2f %s\n", cycles_to_ns(stats->p50), unit);
    printf("P95:        %.2f %s\n", cycles_to_ns(stats->p95), unit);
    printf("P99:        %.2f %s\n", cycles_to_ns(stats->p99), unit);
    printf("Raw cycles: min=%lu, mean=%.0f, p50=%lu\n",
           stats->min, stats->mean, stats->p50);
}

/**
 * Print comparison between baseline and optimized
 */
double bench_print_comparison(const char* name,
                              bench_stats_t* baseline,
                              bench_stats_t* optimized) {
    printf("\n=== COMPARISON: %s ===\n", name);

    // Calculate improvements
    double mean_improvement = ((double)baseline->mean - (double)optimized->mean) / (double)baseline->mean * 100.0;
    double p50_improvement = ((double)baseline->p50 - (double)optimized->p50) / (double)baseline->p50 * 100.0;
    double p99_improvement = ((double)baseline->p99 - (double)optimized->p99) / (double)baseline->p99 * 100.0;

    printf("Baseline mean:   %.2f ns\n", cycles_to_ns((uint64_t)baseline->mean));
    printf("Optimized mean:  %.2f ns\n", cycles_to_ns((uint64_t)optimized->mean));
    printf("Mean speedup:    %.1f%% %s\n",
           fabs(mean_improvement),
           mean_improvement > 0 ? "faster" : "SLOWER");

    printf("\nBaseline p50:    %.2f ns\n", cycles_to_ns(baseline->p50));
    printf("Optimized p50:   %.2f ns\n", cycles_to_ns(optimized->p50));
    printf("P50 speedup:     %.1f%% %s\n",
           fabs(p50_improvement),
           p50_improvement > 0 ? "faster" : "SLOWER");

    printf("\nBaseline p99:    %.2f ns\n", cycles_to_ns(baseline->p99));
    printf("Optimized p99:   %.2f ns\n", cycles_to_ns(optimized->p99));
    printf("P99 speedup:     %.1f%% %s\n",
           fabs(p99_improvement),
           p99_improvement > 0 ? "faster" : "SLOWER");

    // Overall verdict
    if (mean_improvement >= 30.0) {
        printf("\n✓ PERFORMANCE TARGET MET (%.1f%% improvement)\n", mean_improvement);
    } else if (mean_improvement >= 10.0) {
        printf("\n⚠ PARTIAL IMPROVEMENT (%.1f%% improvement, target: 30%%)\n", mean_improvement);
    } else if (mean_improvement > 0) {
        printf("\n⚠ MINOR IMPROVEMENT (%.1f%% improvement, target: 30%%)\n", mean_improvement);
    } else {
        printf("\n✗ REGRESSION (%.1f%% slower!)\n", fabs(mean_improvement));
    }

    return mean_improvement;
}

/**
 * Check if PCID is supported
 */
bool bench_check_pcid_support(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );

    bool pcid_supported = (ecx & (1 << 17)) != 0;
    printf("[INFO] PCID support: %s\n", pcid_supported ? "YES" : "NO");
    return pcid_supported;
}

/**
 * Check if running in VM
 */
bool bench_check_vm(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );

    // Check hypervisor bit
    bool is_vm = (ecx & (1 << 31)) != 0;
    printf("[INFO] Running in VM: %s\n", is_vm ? "YES" : "NO");
    return is_vm;
}

/**
 * Get CPU count
 */
uint32_t bench_get_cpu_count(void) {
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    printf("[INFO] CPU cores: %ld\n", nprocs);
    return (uint32_t)nprocs;
}

/**
 * Pin thread to CPU
 */
void bench_pin_cpu(uint32_t cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        printf("[INFO] Pinned to CPU %u\n", cpu_id);
    } else {
        printf("[WARN] Failed to pin to CPU %u\n", cpu_id);
    }
}
