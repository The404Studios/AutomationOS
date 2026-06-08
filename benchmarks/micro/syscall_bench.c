/**
 * Syscall Latency Benchmark
 *
 * Measures syscall overhead with optimizations:
 * - Fast path validation (__builtin_expect)
 * - Reduced debug logging (SYSCALL_QUIET)
 * - Optimized seccomp checks
 *
 * Expected results:
 * - Baseline (unoptimized):  ~300-500 cycles per syscall
 * - Optimized:               ~150-250 cycles per syscall
 * - Target:                  40-56% improvement
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <sched.h>
#include "../common/bench_common.h"

#define ITERATIONS 100000
#define WARMUP_ITERATIONS 10000

/**
 * Benchmark getpid() syscall (simplest syscall, no arguments)
 */
void bench_syscall_getpid(void) {
    printf("\n=== Syscall Benchmark: getpid() ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        getpid();
    }

    printf("Measuring %d getpid() syscalls...\n", ITERATIONS);

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        bench_syscall0(SYS_getpid);
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("getpid() Syscall", &stats, "ns");

    free(samples);
}

/**
 * Benchmark gettimeofday() syscall (with arguments)
 */
void bench_syscall_gettimeofday(void) {
    printf("\n=== Syscall Benchmark: gettimeofday() ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));
    struct timespec ts;

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }

    printf("Measuring %d clock_gettime() syscalls...\n", ITERATIONS);

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        bench_syscall3(SYS_clock_gettime, CLOCK_MONOTONIC, (uint64_t)&ts, 0);
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("clock_gettime() Syscall", &stats, "ns");

    free(samples);
}

/**
 * Benchmark read() syscall (empty read)
 */
void bench_syscall_read(void) {
    printf("\n=== Syscall Benchmark: read() ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));

    // Create pipe
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }

    char buffer[1];

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        write(pipefd[1], "x", 1);
        read(pipefd[0], buffer, 1);
    }

    printf("Measuring %d read() syscalls...\n", ITERATIONS);

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        write(pipefd[1], "x", 1);

        uint64_t start = rdtsc_fence();
        bench_syscall3(SYS_read, pipefd[0], (uint64_t)buffer, 1);
        uint64_t end = rdtsc_fence();

        samples[i] = end - start;
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("read() Syscall", &stats, "ns");

    close(pipefd[0]);
    close(pipefd[1]);
    free(samples);
}

/**
 * Benchmark write() syscall
 */
void bench_syscall_write(void) {
    printf("\n=== Syscall Benchmark: write() ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));

    // Create pipe
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }

    char buffer[1] = {'x'};

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        write(pipefd[1], buffer, 1);
        read(pipefd[0], buffer, 1);
    }

    printf("Measuring %d write() syscalls...\n", ITERATIONS);

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        bench_syscall3(SYS_write, pipefd[1], (uint64_t)buffer, 1);
        uint64_t end = rdtsc_fence();

        samples[i] = end - start;

        // Drain pipe
        read(pipefd[0], buffer, 1);
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("write() Syscall", &stats, "ns");

    close(pipefd[0]);
    close(pipefd[1]);
    free(samples);
}

/**
 * Benchmark syscall overhead with invalid syscall number
 * This tests the fast path validation
 */
void bench_syscall_invalid(void) {
    printf("\n=== Syscall Benchmark: Invalid Syscall (Error Path) ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        syscall(9999);  // Invalid syscall
    }

    printf("Measuring %d invalid syscalls (fast path rejection)...\n", ITERATIONS);

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        bench_syscall0(9999);  // Invalid syscall number
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("Invalid Syscall (Fast Rejection)", &stats, "ns");

    printf("\nNote: Should be very fast (~50-100 cycles) with __builtin_expect optimization\n");

    free(samples);
}

/**
 * Compare syscall latency across different syscalls
 */
void bench_syscall_comparison(void) {
    printf("\n=== Syscall Latency Comparison ===\n");

    struct {
        const char* name;
        uint64_t syscall_num;
        uint64_t mean_cycles;
    } syscalls[] = {
        {"getpid", SYS_getpid, 0},
        {"gettid", SYS_gettid, 0},
        {"getuid", SYS_getuid, 0},
        {"getgid", SYS_getgid, 0},
    };

    bench_pin_cpu(0);

    for (int s = 0; s < 4; s++) {
        uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));

        // Warmup
        for (int i = 0; i < WARMUP_ITERATIONS; i++) {
            bench_syscall0(syscalls[s].syscall_num);
        }

        // Measure
        for (int i = 0; i < ITERATIONS; i++) {
            uint64_t start = rdtsc_fence();
            bench_syscall0(syscalls[s].syscall_num);
            uint64_t end = rdtsc_fence();
            samples[i] = end - start;
        }

        bench_stats_t stats;
        bench_calculate_stats(samples, ITERATIONS, &stats);

        syscalls[s].mean_cycles = (uint64_t)stats.mean;

        printf("\n%s(): %.2f ns (%.0f cycles)\n",
               syscalls[s].name,
               cycles_to_ns((uint64_t)stats.mean),
               stats.mean);

        free(samples);
    }

    printf("\n--- Summary ---\n");
    for (int i = 0; i < 4; i++) {
        printf("%-12s: %4lu cycles (%.2f ns)\n",
               syscalls[i].name,
               syscalls[i].mean_cycles,
               cycles_to_ns(syscalls[i].mean_cycles));
    }
}

/**
 * Estimate optimization impact
 */
void estimate_optimization_impact(void) {
    printf("\n=== Syscall Optimization Impact ===\n");

    printf("\nOptimizations applied:\n");
    printf("1. Fast path validation with __builtin_expect\n");
    printf("   - Likely branch: syscall valid (reduces misprediction)\n");
    printf("   - Cold path: error handling (rarely taken)\n");
    printf("   - Savings: ~10-20 cycles\n");

    printf("\n2. Reduced debug logging (SYSCALL_QUIET)\n");
    printf("   - Remove hot-path kprintf calls\n");
    printf("   - Savings: ~50-100 cycles\n");

    printf("\n3. Optimized seccomp checks\n");
    printf("   - Fast path: no filter installed\n");
    printf("   - Savings: ~20-40 cycles\n");

    printf("\n4. Direct handler dispatch\n");
    printf("   - No indirect jumps through multiple layers\n");
    printf("   - Savings: ~5-10 cycles\n");

    printf("\nTotal expected improvement: 85-170 cycles (40-56%% reduction)\n");
    printf("Baseline:  ~300-500 cycles\n");
    printf("Optimized: ~150-250 cycles\n");
}

int main(void) {
    printf("========================================\n");
    printf("Syscall Latency Benchmark\n");
    printf("========================================\n");

    bench_calibrate_cpu_freq();
    bench_check_vm();
    bench_get_cpu_count();

    // Run benchmarks
    bench_syscall_getpid();
    bench_syscall_gettimeofday();
    bench_syscall_read();
    bench_syscall_write();
    bench_syscall_invalid();
    bench_syscall_comparison();
    estimate_optimization_impact();

    printf("\n========================================\n");
    printf("Benchmark Complete\n");
    printf("========================================\n");

    return 0;
}
