/**
 * Syscall Latency Benchmark - AutomationOS
 *
 * Measures syscall overhead with RDTSC to validate 40-56% improvement
 * from branch prediction hints and fast-path optimization.
 *
 * Expected results:
 * - Baseline (pre-optimization): 150-250 cycles
 * - Optimized (with hints):      65-110 cycles
 * - Improvement:                 40-56%
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Syscall numbers (must match kernel/include/syscall.h)
#define SYS_GETPID       8
#define SYS_GET_TICKS_MS 40
#define SYS_WRITE        3
#define SYS_READ         2
#define SYS_YIELD        15

#define ITERATIONS 10000
#define WARMUP_ITERATIONS 1000

// RDTSC timing
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Fence before RDTSC to serialize execution
static inline uint64_t rdtsc_fence(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Direct syscall wrapper (bypasses libc)
static inline int64_t syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
                     : "rcx", "r11", "memory");
    return ret;
}

// Bubble sort for percentiles
static void sort_samples(uint64_t* samples, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (samples[j] > samples[j + 1]) {
                uint64_t tmp = samples[j];
                samples[j] = samples[j + 1];
                samples[j + 1] = tmp;
            }
        }
    }
}

// Calculate statistics
typedef struct {
    uint64_t min;
    uint64_t max;
    uint64_t median;
    uint64_t p95;
    uint64_t p99;
    uint64_t mean;
} bench_stats_t;

static void calculate_stats(uint64_t* samples, int count, bench_stats_t* stats) {
    sort_samples(samples, count);

    stats->min = samples[0];
    stats->max = samples[count - 1];
    stats->median = samples[count / 2];
    stats->p95 = samples[(count * 95) / 100];
    stats->p99 = samples[(count * 99) / 100];

    uint64_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += samples[i];
    }
    stats->mean = sum / count;
}

// Print statistics
static void print_stats(const char* name, bench_stats_t* stats) {
    printf("\n%s:\n", name);
    printf("  Min:    %llu cycles\n", stats->min);
    printf("  Mean:   %llu cycles\n", stats->mean);
    printf("  Median: %llu cycles\n", stats->median);
    printf("  95%%:    %llu cycles\n", stats->p95);
    printf("  99%%:    %llu cycles\n", stats->p99);
    printf("  Max:    %llu cycles\n", stats->max);
}

// Benchmark getpid() (fast-path inlined syscall)
static void bench_getpid(void) {
    printf("\n=== Benchmarking SYS_GETPID (Fast-Path Inline) ===\n");

    uint64_t samples[ITERATIONS];

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        syscall0(SYS_GETPID);
    }

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        syscall0(SYS_GETPID);
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    calculate_stats(samples, ITERATIONS, &stats);
    print_stats("getpid()", &stats);

    printf("\nTarget: 65-110 cycles (40-56%% improvement over baseline)\n");
}

// Benchmark gettimeofday() (fast-path inlined syscall)
static void bench_get_ticks_ms(void) {
    printf("\n=== Benchmarking SYS_GET_TICKS_MS (Fast-Path Inline) ===\n");

    uint64_t samples[ITERATIONS];

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        syscall0(SYS_GET_TICKS_MS);
    }

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        syscall0(SYS_GET_TICKS_MS);
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    calculate_stats(samples, ITERATIONS, &stats);
    print_stats("get_ticks_ms()", &stats);

    printf("\nTarget: 65-110 cycles (fast-path read-only syscall)\n");
}

// Benchmark write() (standard syscall with handler dispatch)
static void bench_write(void) {
    printf("\n=== Benchmarking SYS_WRITE (Standard Dispatch) ===\n");

    uint64_t samples[ITERATIONS];
    char buf[1] = {'x'};

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        syscall3(SYS_WRITE, 1, (uint64_t)buf, 1);
    }

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        syscall3(SYS_WRITE, 1, (uint64_t)buf, 1);
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    calculate_stats(samples, ITERATIONS, &stats);
    print_stats("write(1 byte)", &stats);

    printf("\nNote: Write includes VFS/serial overhead (not just syscall entry)\n");
}

// Benchmark invalid syscall (error path validation)
static void bench_invalid_syscall(void) {
    printf("\n=== Benchmarking Invalid Syscall (Error Path) ===\n");

    uint64_t samples[ITERATIONS];

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        syscall0(9999);  // Invalid syscall number
    }

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        syscall0(9999);  // Invalid syscall number
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    calculate_stats(samples, ITERATIONS, &stats);
    print_stats("Invalid Syscall (Fast Rejection)", &stats);

    printf("\nTarget: <100 cycles (branch hint ensures fast path)\n");
}

// Main
int main(void) {
    printf("========================================\n");
    printf("Syscall Latency Benchmark - AutomationOS\n");
    printf("========================================\n");
    printf("Iterations: %d (warmup: %d)\n", ITERATIONS, WARMUP_ITERATIONS);
    printf("\nOptimizations tested:\n");
    printf("  1. Fast-path inline for getpid/get_ticks_ms\n");
    printf("  2. Branch prediction hints (__builtin_expect)\n");
    printf("  3. SYSCALL_QUIET removes debug logging\n");
    printf("  4. Direct handler dispatch\n");

    bench_getpid();
    bench_get_ticks_ms();
    bench_write();
    bench_invalid_syscall();

    printf("\n========================================\n");
    printf("Benchmark Complete\n");
    printf("========================================\n");
    printf("\nExpected improvement: 40-56%% latency reduction\n");
    printf("Baseline:  150-250 cycles\n");
    printf("Optimized: 65-110 cycles\n");

    return 0;
}
