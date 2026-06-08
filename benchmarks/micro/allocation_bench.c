/**
 * Page Allocation Latency Benchmark
 *
 * Measures kmalloc/kfree latency with and without per-CPU caches.
 *
 * Expected results:
 * - Without per-CPU cache: ~500-1000 cycles (O(n) scan of free lists)
 * - With per-CPU cache:    ~5-50 cycles (O(1) array access)
 * - Target:                10x improvement (90% reduction)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include "../common/bench_common.h"

#define ITERATIONS 100000
#define WARMUP_ITERATIONS 10000
#define PAGE_SIZE 4096
#define CACHE_SIZE 16

/**
 * Simulate kmalloc without per-CPU cache (O(n) scan)
 */
typedef struct page {
    struct page* next;
    bool is_free;
    char data[PAGE_SIZE - sizeof(struct page*) - sizeof(bool)];
} page_t;

static page_t* free_list_head = NULL;
static uint64_t alloc_count_slow = 0;

void* alloc_page_slow(void) {
    alloc_count_slow++;

    // O(n) scan to find free page
    page_t* current = free_list_head;
    while (current) {
        if (current->is_free) {
            current->is_free = false;
            return current;
        }
        current = current->next;
    }

    // Allocate new page
    page_t* page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (page) {
        page->is_free = false;
        page->next = free_list_head;
        free_list_head = page;
    }
    return page;
}

void free_page_slow(void* ptr) {
    page_t* page = (page_t*)ptr;
    page->is_free = true;
}

/**
 * Simulate kmalloc with per-CPU cache (O(1) access)
 */
typedef struct {
    void* pages[CACHE_SIZE];
    uint32_t count;
    uint64_t alloc_fast;
    uint64_t alloc_slow;
} per_cpu_cache_t;

static __thread per_cpu_cache_t cpu_cache = {0};

void* alloc_page_fast(void) {
    // Fast path: cache hit (O(1))
    if (cpu_cache.count > 0) {
        cpu_cache.alloc_fast++;
        cpu_cache.count--;
        return cpu_cache.pages[cpu_cache.count];
    }

    // Slow path: refill cache
    cpu_cache.alloc_slow++;

    for (uint32_t i = 0; i < CACHE_SIZE; i++) {
        void* page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
        if (!page) break;
        cpu_cache.pages[cpu_cache.count++] = page;
    }

    if (cpu_cache.count > 0) {
        cpu_cache.count--;
        return cpu_cache.pages[cpu_cache.count];
    }

    return NULL;
}

void free_page_fast(void* ptr) {
    // Fast path: return to cache if not full
    if (cpu_cache.count < CACHE_SIZE) {
        cpu_cache.pages[cpu_cache.count++] = ptr;
        return;
    }

    // Slow path: cache full, free directly
    free(ptr);
}

/**
 * Benchmark allocation without per-CPU cache
 */
void bench_alloc_slow(void) {
    printf("\n=== Allocation Benchmark (WITHOUT per-CPU cache) ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));
    void** pages = malloc(ITERATIONS * sizeof(void*));

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        void* page = alloc_page_slow();
        free_page_slow(page);
    }

    printf("Measuring %d allocations (O(n) scan)...\n", ITERATIONS);

    // Measure allocations
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        pages[i] = alloc_page_slow();
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    // Calculate stats
    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("Allocation (Slow Path - No Cache)", &stats, "ns");

    // Free all pages
    for (int i = 0; i < ITERATIONS; i++) {
        free_page_slow(pages[i]);
    }

    free(samples);
    free(pages);
}

/**
 * Benchmark allocation with per-CPU cache
 */
void bench_alloc_fast(void) {
    printf("\n=== Allocation Benchmark (WITH per-CPU cache) ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));
    void** pages = malloc(ITERATIONS * sizeof(void*));

    // Reset cache stats
    cpu_cache.alloc_fast = 0;
    cpu_cache.alloc_slow = 0;

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        void* page = alloc_page_fast();
        free_page_fast(page);
    }

    printf("Measuring %d allocations (O(1) cache access)...\n", ITERATIONS);

    // Measure allocations
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        pages[i] = alloc_page_fast();
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    // Calculate stats
    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("Allocation (Fast Path - With Cache)", &stats, "ns");

    // Print cache statistics
    uint64_t total_allocs = cpu_cache.alloc_fast + cpu_cache.alloc_slow;
    double hit_rate = (double)cpu_cache.alloc_fast / (double)total_allocs * 100.0;

    printf("\nCache Statistics:\n");
    printf("  Fast path (cache hit):  %lu (%.1f%%)\n", cpu_cache.alloc_fast, hit_rate);
    printf("  Slow path (cache miss): %lu (%.1f%%)\n", cpu_cache.alloc_slow, 100.0 - hit_rate);
    printf("  Cache size:             %u pages\n", CACHE_SIZE);

    // Free all pages
    for (int i = 0; i < ITERATIONS; i++) {
        free_page_fast(pages[i]);
    }

    free(samples);
    free(pages);
}

/**
 * Benchmark allocation under contention (multi-threaded)
 */
typedef struct {
    uint64_t* samples;
    uint32_t iterations;
    bool use_cache;
} thread_args_t;

void* bench_alloc_thread(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;

    // Warmup
    for (int i = 0; i < 1000; i++) {
        void* page = args->use_cache ? alloc_page_fast() : alloc_page_slow();
        if (args->use_cache) {
            free_page_fast(page);
        } else {
            free_page_slow(page);
        }
    }

    // Measure
    for (uint32_t i = 0; i < args->iterations; i++) {
        uint64_t start = rdtsc_fence();
        void* page = args->use_cache ? alloc_page_fast() : alloc_page_slow();
        uint64_t end = rdtsc_fence();
        args->samples[i] = end - start;

        if (args->use_cache) {
            free_page_fast(page);
        } else {
            free_page_slow(page);
        }
    }

    return NULL;
}

void bench_alloc_contention(void) {
    printf("\n=== Allocation Benchmark (Multi-threaded Contention) ===\n");

    uint32_t num_threads = bench_get_cpu_count();
    if (num_threads > 8) num_threads = 8;  // Cap at 8 threads

    uint32_t iterations_per_thread = ITERATIONS / num_threads;

    printf("Testing with %u threads (%u iterations each)...\n",
           num_threads, iterations_per_thread);

    // Test without cache
    {
        printf("\nWithout per-CPU cache (contended global allocator):\n");

        pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
        thread_args_t* args = malloc(num_threads * sizeof(thread_args_t));

        for (uint32_t i = 0; i < num_threads; i++) {
            args[i].samples = malloc(iterations_per_thread * sizeof(uint64_t));
            args[i].iterations = iterations_per_thread;
            args[i].use_cache = false;
            pthread_create(&threads[i], NULL, bench_alloc_thread, &args[i]);
        }

        for (uint32_t i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }

        // Aggregate results from all threads
        uint64_t* all_samples = malloc(ITERATIONS * sizeof(uint64_t));
        uint32_t idx = 0;
        for (uint32_t i = 0; i < num_threads; i++) {
            memcpy(&all_samples[idx], args[i].samples,
                   iterations_per_thread * sizeof(uint64_t));
            idx += iterations_per_thread;
            free(args[i].samples);
        }

        bench_stats_t stats;
        bench_calculate_stats(all_samples, ITERATIONS, &stats);
        bench_print_stats("Allocation (No Cache, Contended)", &stats, "ns");

        free(all_samples);
        free(threads);
        free(args);
    }

    // Test with cache
    {
        printf("\nWith per-CPU cache (no contention):\n");

        pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
        thread_args_t* args = malloc(num_threads * sizeof(thread_args_t));

        for (uint32_t i = 0; i < num_threads; i++) {
            args[i].samples = malloc(iterations_per_thread * sizeof(uint64_t));
            args[i].iterations = iterations_per_thread;
            args[i].use_cache = true;
            pthread_create(&threads[i], NULL, bench_alloc_thread, &args[i]);
        }

        for (uint32_t i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }

        // Aggregate results
        uint64_t* all_samples = malloc(ITERATIONS * sizeof(uint64_t));
        uint32_t idx = 0;
        for (uint32_t i = 0; i < num_threads; i++) {
            memcpy(&all_samples[idx], args[i].samples,
                   iterations_per_thread * sizeof(uint64_t));
            idx += iterations_per_thread;
            free(args[i].samples);
        }

        bench_stats_t stats;
        bench_calculate_stats(all_samples, ITERATIONS, &stats);
        bench_print_stats("Allocation (With Cache, No Contention)", &stats, "ns");

        free(all_samples);
        free(threads);
        free(args);
    }
}

int main(void) {
    printf("========================================\n");
    printf("Page Allocation Latency Benchmark\n");
    printf("========================================\n");

    bench_calibrate_cpu_freq();
    bench_check_vm();

    // Run benchmarks
    bench_alloc_slow();
    bench_alloc_fast();
    bench_alloc_contention();

    printf("\n========================================\n");
    printf("Benchmark Complete\n");
    printf("========================================\n");

    printf("\nExpected Results:\n");
    printf("  Without cache: ~500-1000 cycles (global allocator)\n");
    printf("  With cache:    ~5-50 cycles (per-CPU cache)\n");
    printf("  Target:        10x speedup (90%% reduction)\n");

    return 0;
}
