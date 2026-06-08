/**
 * Common benchmark utilities and timing functions
 */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdint.h>
#include <stdbool.h>

// CPU frequency (calibrated at runtime)
extern uint64_t cpu_freq_mhz;

// Statistics structure
typedef struct {
    uint64_t min;
    uint64_t max;
    uint64_t total;
    uint64_t count;
    double mean;
    double std_dev;
    uint64_t p50;    // Median
    uint64_t p95;
    uint64_t p99;
} bench_stats_t;

/**
 * Read CPU timestamp counter (serializing)
 * Use for accurate timing measurements
 */
static inline uint64_t rdtsc_fence(void) {
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc\n\t"
        "lfence"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

/**
 * Read CPU timestamp counter (non-serializing, faster)
 * Use when ordering doesn't matter
 */
static inline uint64_t rdtsc_fast(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/**
 * Convert cycles to nanoseconds
 */
static inline double cycles_to_ns(uint64_t cycles) {
    return (double)cycles * 1000.0 / (double)cpu_freq_mhz;
}

/**
 * Convert cycles to microseconds
 */
static inline double cycles_to_us(uint64_t cycles) {
    return (double)cycles / (double)cpu_freq_mhz;
}

/**
 * Convert cycles to milliseconds
 */
static inline double cycles_to_ms(uint64_t cycles) {
    return (double)cycles / ((double)cpu_freq_mhz * 1000.0);
}

/**
 * Calibrate CPU frequency
 * Call this before running benchmarks
 */
void bench_calibrate_cpu_freq(void);

/**
 * Calculate statistics from raw timing samples
 */
void bench_calculate_stats(uint64_t* samples, uint32_t count, bench_stats_t* stats);

/**
 * Print benchmark results
 */
void bench_print_stats(const char* name, bench_stats_t* stats, const char* unit);

/**
 * Print comparison between two benchmarks
 * Returns improvement percentage (positive = faster, negative = slower)
 */
double bench_print_comparison(const char* name,
                              bench_stats_t* baseline,
                              bench_stats_t* optimized);

/**
 * Check if PCID is supported
 */
bool bench_check_pcid_support(void);

/**
 * Check if we're running in a VM
 */
bool bench_check_vm(void);

/**
 * Get number of CPU cores
 */
uint32_t bench_get_cpu_count(void);

/**
 * Pin thread to specific CPU core
 */
void bench_pin_cpu(uint32_t cpu_id);

/**
 * Flush TLB (for testing context switch overhead)
 */
static inline void bench_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile (
        "mov %%cr3, %0\n\t"
        "mov %0, %%cr3"
        : "=r"(cr3)
        :
        : "memory"
    );
}

/**
 * Flush instruction cache
 */
static inline void bench_flush_icache(void) {
    __asm__ volatile (
        "wbinvd"
        :
        :
        : "memory"
    );
}

/**
 * Memory barrier
 */
static inline void bench_mfence(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

/**
 * Compiler barrier (prevent reordering)
 */
#define BENCH_BARRIER() __asm__ volatile ("" ::: "memory")

/**
 * Prevent compiler from optimizing away code
 */
#define BENCH_DONT_OPTIMIZE(x) __asm__ volatile ("" : "+r,m"(x) : : "memory")

/**
 * Warmup cache (run code once before measuring)
 */
#define BENCH_WARMUP(code, iterations) \
    do { \
        for (int _i = 0; _i < (iterations); _i++) { \
            code; \
            BENCH_BARRIER(); \
        } \
    } while (0)

/**
 * Run benchmark with timing
 */
#define BENCH_RUN(name, code, samples, count) \
    do { \
        BENCH_WARMUP(code, 100); \
        for (uint32_t _i = 0; _i < (count); _i++) { \
            uint64_t _start = rdtsc_fence(); \
            code; \
            uint64_t _end = rdtsc_fence(); \
            (samples)[_i] = _end - _start; \
            BENCH_BARRIER(); \
        } \
    } while (0)

/**
 * Syscall wrappers for benchmarking
 */
static inline int64_t bench_syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t bench_syscall1(uint64_t num, uint64_t arg1) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t bench_syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#endif // BENCH_COMMON_H
