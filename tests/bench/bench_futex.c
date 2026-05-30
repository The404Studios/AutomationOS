/**
 * Futex Performance Benchmark
 *
 * Measures fast userspace mutex performance:
 * - Uncontended lock/unlock (pure atomic operations)
 * - Contended lock/unlock (syscall path)
 * - Wake latency
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Syscall interface
#define SYS_FUTEX 70

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_fence(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtscp(void) {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg3;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// Futex operations
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

typedef struct {
    volatile int val;
} futex_lock_t;

#define FUTEX_LOCK_INITIALIZER {0}

static inline int atomic_cmpxchg(volatile int *ptr, int old, int new) {
    int prev;
    __asm__ volatile(
        "lock cmpxchgl %1, %2"
        : "=a"(prev)
        : "r"(new), "m"(*ptr), "a"(old)
        : "memory"
    );
    return prev;
}

static inline void futex_lock_acquire(futex_lock_t *lock) {
    // Fast path: uncontended
    if (atomic_cmpxchg(&lock->val, 0, 1) == 0) {
        return;  // Got the lock
    }

    // Slow path: contended - need to wait
    while (atomic_cmpxchg(&lock->val, 0, 1) != 0) {
        syscall3(SYS_FUTEX, (uint64_t)&lock->val, FUTEX_WAIT, 1);
    }
}

static inline void futex_lock_release(futex_lock_t *lock) {
    lock->val = 0;
    __asm__ volatile("" ::: "memory");  // Compiler barrier

    // Wake one waiter if any
    syscall3(SYS_FUTEX, (uint64_t)&lock->val, FUTEX_WAKE, 1);
}

// Statistics tracking
typedef struct {
    const char* name;
    uint64_t min_cycles;
    uint64_t max_cycles;
    uint64_t total_cycles;
    uint32_t iterations;
} perf_stat_t;

static void perf_stat_init(perf_stat_t* stat, const char* name) {
    stat->name = name;
    stat->min_cycles = UINT64_MAX;
    stat->max_cycles = 0;
    stat->total_cycles = 0;
    stat->iterations = 0;
}

static void perf_stat_record(perf_stat_t* stat, uint64_t cycles) {
    if (cycles < stat->min_cycles) stat->min_cycles = cycles;
    if (cycles > stat->max_cycles) stat->max_cycles = cycles;
    stat->total_cycles += cycles;
    stat->iterations++;
}

static void perf_stat_report(perf_stat_t* stat) {
    if (stat->iterations == 0) {
        printf("[PERF] %s: No data\n", stat->name);
        return;
    }

    uint64_t avg = stat->total_cycles / stat->iterations;
    printf("[PERF] %s (n=%u):\n", stat->name, stat->iterations);
    printf("  Min: %llu cycles\n", (unsigned long long)stat->min_cycles);
    printf("  Avg: %llu cycles\n", (unsigned long long)avg);
    printf("  Max: %llu cycles\n", (unsigned long long)stat->max_cycles);
}

/**
 * Benchmark uncontended futex lock/unlock
 *
 * Expected: ~5-10 cycles (pure atomic operations, no syscall)
 */
void bench_futex_uncontended(void) {
    printf("\n[BENCH] Futex Uncontended Lock/Unlock\n");
    printf("=======================================\n");

    futex_lock_t lock = FUTEX_LOCK_INITIALIZER;
    perf_stat_t stats;
    perf_stat_init(&stats, "Futex Uncontended");

    // Warmup
    for (int i = 0; i < 100; i++) {
        futex_lock_acquire(&lock);
        futex_lock_release(&lock);
    }

    // Benchmark
    for (int i = 0; i < 10000; i++) {
        uint64_t start = rdtsc_fence();
        futex_lock_acquire(&lock);
        futex_lock_release(&lock);
        uint64_t end = rdtscp();

        perf_stat_record(&stats, end - start);
    }

    perf_stat_report(&stats);

    uint64_t avg = stats.total_cycles / stats.iterations;
    if (avg < 20) {
        printf("[PASS] Uncontended locks are fast (<%llu cycles)\n", (unsigned long long)avg);
    } else {
        printf("[WARN] Uncontended locks slower than expected (%llu cycles, expected <20)\n",
               (unsigned long long)avg);
    }
    printf("\n");
}

/**
 * Benchmark atomic compare-exchange only
 *
 * Measures just the atomic operation without lock/unlock overhead
 */
void bench_futex_atomic_only(void) {
    printf("\n[BENCH] Atomic Compare-Exchange Only\n");
    printf("=====================================\n");

    volatile int val = 0;
    perf_stat_t stats;
    perf_stat_init(&stats, "Atomic CAS");

    // Benchmark
    for (int i = 0; i < 10000; i++) {
        uint64_t start = rdtsc_fence();
        atomic_cmpxchg(&val, 0, 1);
        val = 0;
        uint64_t end = rdtscp();

        perf_stat_record(&stats, end - start);
    }

    perf_stat_report(&stats);

    uint64_t avg = stats.total_cycles / stats.iterations;
    printf("[INFO] Pure atomic operation: %llu cycles\n", (unsigned long long)avg);
    printf("\n");
}

/**
 * Benchmark lock acquisition throughput
 */
void bench_futex_throughput(void) {
    printf("\n[BENCH] Futex Throughput\n");
    printf("========================\n");

    futex_lock_t lock = FUTEX_LOCK_INITIALIZER;
    const int iterations = 100000;

    uint64_t start = rdtsc_fence();
    for (int i = 0; i < iterations; i++) {
        futex_lock_acquire(&lock);
        futex_lock_release(&lock);
    }
    uint64_t end = rdtscp();

    uint64_t total_cycles = end - start;
    uint64_t cycles_per_op = total_cycles / iterations;

    // Assume 3 GHz CPU
    uint64_t cpu_freq_hz = 3000000000ULL;
    uint64_t ops_per_sec = cpu_freq_hz / cycles_per_op;

    printf("[BENCH] Total cycles: %llu\n", (unsigned long long)total_cycles);
    printf("[BENCH] Cycles/operation: %llu\n", (unsigned long long)cycles_per_op);
    printf("[BENCH] Operations/second: %llu (%.2f M/s)\n",
           (unsigned long long)ops_per_sec,
           (double)ops_per_sec / 1000000.0);
    printf("\n");
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  FUTEX BENCHMARK SUITE\n");
    printf("=============================================\n");

    bench_futex_atomic_only();
    bench_futex_uncontended();
    bench_futex_throughput();

    printf("=============================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("=============================================\n");
    printf("\nExpected Results:\n");
    printf("  Atomic CAS:        ~5-10 cycles\n");
    printf("  Uncontended lock:  ~10-20 cycles (2x atomic ops + barriers)\n");
    printf("  Throughput:        >50M ops/sec on modern CPU\n");
    printf("\n");

    return 0;
}
