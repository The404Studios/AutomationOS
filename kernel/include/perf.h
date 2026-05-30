#ifndef PERF_H
#define PERF_H

/**
 * Performance Profiling & Measurement Infrastructure
 *
 * Provides cycle-accurate timing using RDTSC (Read Time-Stamp Counter)
 * and utilities for profiling kernel performance.
 *
 * Usage:
 *   PERF_TIMER_START();
 *   // ... code to measure ...
 *   PERF_TIMER_END("operation_name");
 *
 * Or:
 *   uint64_t start = rdtsc();
 *   // ... code ...
 *   uint64_t cycles = rdtsc() - start;
 */

#include "types.h"
#include "kernel.h"

// Read Time-Stamp Counter (cycle-accurate)
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Serialize execution before RDTSC (prevent out-of-order execution)
static inline uint64_t rdtsc_fence(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Read Time-Stamp Counter with full serialization (most accurate)
static inline uint64_t rdtscp(void) {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

// Estimated CPU frequency (for converting cycles to time)
// TODO: Calibrate this at boot time using PIT or HPET
#ifndef CPU_FREQ_MHZ
#define CPU_FREQ_MHZ 3000  // 3 GHz assumption
#endif

// Convert cycles to microseconds (integer: returns whole microseconds)
static inline uint64_t cycles_to_us(uint64_t cycles) {
    return cycles / CPU_FREQ_MHZ;
}

// Convert cycles to microseconds fractional part (hundredths of a us)
static inline uint64_t cycles_to_us_frac(uint64_t cycles) {
    return (cycles * 100 / CPU_FREQ_MHZ) % 100;
}

// Convert cycles to milliseconds (integer: returns whole milliseconds)
static inline uint64_t cycles_to_ms(uint64_t cycles) {
    return cycles / (CPU_FREQ_MHZ * 1000);
}

// Convert cycles to milliseconds fractional part (hundredths of a ms)
static inline uint64_t cycles_to_ms_frac(uint64_t cycles) {
    return (cycles * 100 / (CPU_FREQ_MHZ * 1000)) % 100;
}

// Simple timer macros (uses stack variable)
#define PERF_TIMER_START() \
    __perf_start = rdtsc()

#define PERF_TIMER_END(name) \
    do { \
        uint64_t __perf_end = rdtsc(); \
        uint64_t __perf_cycles = __perf_end - __perf_start; \
        kprintf("[PERF] %s: %lu cycles (%lu.%02lu us)\n", \
                name, (unsigned long)__perf_cycles, \
                (unsigned long)cycles_to_us(__perf_cycles), \
                (unsigned long)cycles_to_us_frac(__perf_cycles)); \
    } while(0)

// Accurate timer macros (with fencing)
#define PERF_TIMER_START_ACCURATE() \
    uint64_t __perf_start = rdtsc_fence()

#define PERF_TIMER_END_ACCURATE(name) \
    do { \
        uint64_t __perf_end = rdtscp(); \
        uint64_t __perf_cycles = __perf_end - __perf_start; \
        kprintf("[PERF] %s: %llu cycles (%llu.%02llu us)\n", \
                name, __perf_cycles, \
                cycles_to_us(__perf_cycles), \
                cycles_to_us_frac(__perf_cycles)); \
    } while(0)

// Statistical profiling (multiple runs)
#define PERF_BENCH_ITERATIONS 1000

typedef struct {
    const char* name;
    uint64_t min_cycles;
    uint64_t max_cycles;
    uint64_t total_cycles;
    uint32_t iterations;
} perf_stat_t;

static inline void perf_stat_init(perf_stat_t* stat, const char* name) {
    stat->name = name;
    stat->min_cycles = UINT64_MAX;
    stat->max_cycles = 0;
    stat->total_cycles = 0;
    stat->iterations = 0;
}

static inline void perf_stat_record(perf_stat_t* stat, uint64_t cycles) {
    if (cycles < stat->min_cycles) stat->min_cycles = cycles;
    if (cycles > stat->max_cycles) stat->max_cycles = cycles;
    stat->total_cycles += cycles;
    stat->iterations++;
}

static inline void perf_stat_report(perf_stat_t* stat) {
    if (stat->iterations == 0) {
        kprintf("[PERF] %s: No data\n", stat->name);
        return;
    }

    uint64_t avg = stat->total_cycles / stat->iterations;

    kprintf("[PERF] %s (n=%u):\n", stat->name, stat->iterations);
    kprintf("  Min: %llu cycles (%llu.%02llu us)\n",
            stat->min_cycles, cycles_to_us(stat->min_cycles),
            cycles_to_us_frac(stat->min_cycles));
    kprintf("  Avg: %llu cycles (%llu.%02llu us)\n",
            avg, cycles_to_us(avg), cycles_to_us_frac(avg));
    kprintf("  Max: %llu cycles (%llu.%02llu us)\n",
            stat->max_cycles, cycles_to_us(stat->max_cycles),
            cycles_to_us_frac(stat->max_cycles));
}

// Performance counter snapshot
typedef struct {
    uint64_t timestamp;
    uint64_t instructions;   // TODO: Read from PMU
    uint64_t cache_misses;   // TODO: Read from PMU
    uint64_t branch_misses;  // TODO: Read from PMU
} perf_snapshot_t;

static inline void perf_snapshot_take(perf_snapshot_t* snap) {
    snap->timestamp = rdtsc();
    // TODO: Read PMU counters when implemented
    snap->instructions = 0;
    snap->cache_misses = 0;
    snap->branch_misses = 0;
}

static inline uint64_t perf_snapshot_diff(perf_snapshot_t* start, perf_snapshot_t* end) {
    return end->timestamp - start->timestamp;
}

// CPU frequency calibration (call at boot)
void perf_calibrate_cpu_freq(void);
uint64_t perf_get_cpu_freq_mhz(void);

// Performance monitoring control
void perf_enable(void);
void perf_disable(void);
bool perf_is_enabled(void);

// ============================================================================
// PERF_START / PERF_END - Simplified macros for hot path instrumentation
// ============================================================================

// Simple start/end macros (no stack variable needed, uses global storage)
#define PERF_START(label) \
    perf_start(label)

#define PERF_END(label) \
    perf_end(label)

// Per-operation performance tracking
typedef enum {
    PERF_OP_SYSCALL = 0,
    PERF_OP_CONTEXT_SWITCH,
    PERF_OP_SLAB_ALLOC,
    PERF_OP_SLAB_FREE,
    PERF_OP_PAGE_ALLOC,
    PERF_OP_PAGE_FREE,
    PERF_OP_MEMCPY,
    PERF_OP_MEMSET,
    PERF_OP_SCHEDULER_ADD,
    PERF_OP_SCHEDULER_REMOVE,
    PERF_OP_MAX
} perf_op_t;

// Histogram bucket configuration
#define PERF_HISTOGRAM_BUCKETS 16
#define PERF_BUCKET_SIZE 100  // cycles per bucket

// Per-operation statistics with histogram
typedef struct {
    const char* name;
    uint64_t count;
    uint64_t total_cycles;
    uint64_t min_cycles;
    uint64_t max_cycles;
    uint64_t histogram[PERF_HISTOGRAM_BUCKETS];
    uint64_t overflow;  // samples > max bucket
} perf_stats_t;

// Core performance tracking API
void perf_init(void);
void perf_start(perf_op_t op);
void perf_end(perf_op_t op);
void perf_record(perf_op_t op, uint64_t cycles);

// Reporting
void perf_report(void);
void perf_report_op(perf_op_t op);
void perf_reset(void);
void perf_reset_op(perf_op_t op);

// Get raw stats for analysis
const perf_stats_t* perf_get_stats(perf_op_t op);

#endif // PERF_H
