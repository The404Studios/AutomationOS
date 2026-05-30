/**
 * Memory Allocation Benchmark
 *
 * Measures malloc/free performance with different allocation sizes.
 * Tests tcache (thread-local cache) and general allocator efficiency.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * Benchmark small allocations (tcache size)
 *
 * Expected: <50 cycles (tcache hit)
 */
void bench_small_allocations(void) {
    printf("\n[BENCH] Small Allocations (64 bytes)\n");
    printf("=====================================\n");

    const size_t alloc_size = 64;
    const int iterations = 10000;
    void* ptrs[1000];

    perf_stat_t malloc_stats, free_stats;
    perf_stat_init(&malloc_stats, "malloc(64)");
    perf_stat_init(&free_stats, "free(64)");

    // Warmup
    for (int i = 0; i < 100; i++) {
        void* p = malloc(alloc_size);
        free(p);
    }

    // Benchmark malloc
    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        void* p = malloc(alloc_size);
        uint64_t end = rdtscp();

        if (p) {
            perf_stat_record(&malloc_stats, end - start);
            ptrs[i % 1000] = p;

            // Free occasionally to prevent OOM
            if ((i % 1000) == 999) {
                for (int j = 0; j < 1000; j++) {
                    free(ptrs[j]);
                }
            }
        }
    }

    // Benchmark free
    for (int i = 0; i < 1000; i++) {
        void* p = malloc(alloc_size);
        ptrs[i] = p;
    }

    for (int i = 0; i < 1000; i++) {
        uint64_t start = rdtsc_fence();
        free(ptrs[i]);
        uint64_t end = rdtscp();
        perf_stat_record(&free_stats, end - start);
    }

    perf_stat_report(&malloc_stats);
    perf_stat_report(&free_stats);

    uint64_t malloc_avg = malloc_stats.total_cycles / malloc_stats.iterations;
    uint64_t free_avg = free_stats.total_cycles / free_stats.iterations;

    if (malloc_avg < 50) {
        printf("[PASS] malloc tcache fast (<50 cycles)\n");
    } else if (malloc_avg < 100) {
        printf("[INFO] malloc moderate (50-100 cycles)\n");
    } else {
        printf("[WARN] malloc slow (>100 cycles)\n");
    }

    printf("\n");
}

/**
 * Benchmark different allocation sizes
 */
void bench_allocation_sizes(void) {
    printf("\n[BENCH] Allocation Performance vs Size\n");
    printf("=======================================\n");

    const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    const int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        size_t size = sizes[s];
        const int iterations = 1000;

        uint64_t malloc_total = 0;
        uint64_t free_total = 0;
        void* ptrs[1000];

        // Benchmark malloc
        for (int i = 0; i < iterations; i++) {
            uint64_t start = rdtsc_fence();
            void* p = malloc(size);
            uint64_t end = rdtscp();

            if (p) {
                ptrs[i] = p;
                malloc_total += (end - start);
            }
        }

        // Benchmark free
        for (int i = 0; i < iterations; i++) {
            if (ptrs[i]) {
                uint64_t start = rdtsc_fence();
                free(ptrs[i]);
                uint64_t end = rdtscp();
                free_total += (end - start);
            }
        }

        printf("  Size %5zu bytes: malloc %4llu cycles, free %4llu cycles\n",
               size,
               (unsigned long long)(malloc_total / iterations),
               (unsigned long long)(free_total / iterations));
    }

    printf("\n");
}

/**
 * Benchmark allocation/free patterns
 */
void bench_allocation_patterns(void) {
    printf("\n[BENCH] Allocation Patterns\n");
    printf("===========================\n");

    const int iterations = 1000;
    const size_t alloc_size = 128;

    // Pattern 1: Allocate all, then free all (stack-like)
    printf("[BENCH] Pattern 1: Allocate all, then free all (LIFO)\n");
    void** ptrs1 = malloc(iterations * sizeof(void*));
    uint64_t start = rdtsc_fence();
    for (int i = 0; i < iterations; i++) {
        ptrs1[i] = malloc(alloc_size);
    }
    for (int i = iterations - 1; i >= 0; i--) {
        free(ptrs1[i]);
    }
    uint64_t end = rdtscp();
    printf("  Total: %llu cycles (%llu per operation)\n",
           (unsigned long long)(end - start),
           (unsigned long long)((end - start) / (iterations * 2)));
    free(ptrs1);

    // Pattern 2: Interleaved alloc/free
    printf("[BENCH] Pattern 2: Interleaved alloc/free\n");
    start = rdtsc_fence();
    for (int i = 0; i < iterations; i++) {
        void* p = malloc(alloc_size);
        free(p);
    }
    end = rdtscp();
    printf("  Total: %llu cycles (%llu per operation)\n",
           (unsigned long long)(end - start),
           (unsigned long long)((end - start) / (iterations * 2)));

    // Pattern 3: Random-size allocations
    printf("[BENCH] Pattern 3: Random-size allocations\n");
    void** ptrs3 = malloc(iterations * sizeof(void*));
    start = rdtsc_fence();
    for (int i = 0; i < iterations; i++) {
        size_t size = (i * 37) % 2048 + 16;  // Pseudo-random 16-2048 bytes
        ptrs3[i] = malloc(size);
    }
    for (int i = 0; i < iterations; i++) {
        free(ptrs3[i]);
    }
    end = rdtscp();
    printf("  Total: %llu cycles (%llu per operation)\n",
           (unsigned long long)(end - start),
           (unsigned long long)((end - start) / (iterations * 2)));
    free(ptrs3);

    printf("\n");
}

/**
 * Benchmark realloc performance
 */
void bench_realloc(void) {
    printf("\n[BENCH] Realloc Performance\n");
    printf("===========================\n");

    const int iterations = 1000;

    // Growing realloc
    printf("[BENCH] Growing realloc (64 -> 128 -> 256 bytes)\n");
    uint64_t total = 0;
    for (int i = 0; i < iterations; i++) {
        void* p = malloc(64);

        uint64_t start = rdtsc_fence();
        p = realloc(p, 128);
        p = realloc(p, 256);
        uint64_t end = rdtscp();

        total += (end - start);
        free(p);
    }
    printf("  Average: %llu cycles per double-realloc\n",
           (unsigned long long)(total / iterations));

    // Shrinking realloc
    printf("[BENCH] Shrinking realloc (1024 -> 512 -> 256 bytes)\n");
    total = 0;
    for (int i = 0; i < iterations; i++) {
        void* p = malloc(1024);

        uint64_t start = rdtsc_fence();
        p = realloc(p, 512);
        p = realloc(p, 256);
        uint64_t end = rdtscp();

        total += (end - start);
        free(p);
    }
    printf("  Average: %llu cycles per double-realloc\n",
           (unsigned long long)(total / iterations));

    printf("\n");
}

/**
 * Benchmark calloc (zeroed allocation)
 */
void bench_calloc(void) {
    printf("\n[BENCH] Calloc vs Malloc+Memset\n");
    printf("================================\n");

    const size_t size = 1024;
    const int iterations = 1000;

    // Benchmark calloc
    uint64_t calloc_total = 0;
    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        void* p = calloc(1, size);
        uint64_t end = rdtscp();
        calloc_total += (end - start);
        free(p);
    }

    // Benchmark malloc + memset
    uint64_t malloc_memset_total = 0;
    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        void* p = malloc(size);
        if (p) memset(p, 0, size);
        uint64_t end = rdtscp();
        malloc_memset_total += (end - start);
        free(p);
    }

    printf("  calloc(%zu):         %llu cycles\n",
           size, (unsigned long long)(calloc_total / iterations));
    printf("  malloc+memset(%zu):  %llu cycles\n",
           size, (unsigned long long)(malloc_memset_total / iterations));

    printf("\n");
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  MEMORY ALLOCATION BENCHMARK SUITE\n");
    printf("=============================================\n");

    bench_small_allocations();
    bench_allocation_sizes();
    bench_allocation_patterns();
    bench_realloc();
    bench_calloc();

    printf("=============================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("=============================================\n");
    printf("\nExpected Results:\n");
    printf("  Small malloc (64B):  <50 cycles (tcache)\n");
    printf("  Medium malloc:       <200 cycles\n");
    printf("  Large malloc:        <1000 cycles\n");
    printf("  Free:                <50 cycles (tcache)\n");
    printf("\n");

    return 0;
}
