/**
 * Memory Allocation Benchmark
 *
 * Measures the performance of the physical memory manager (PMM)
 * and kernel heap allocator.
 */

#include "../../kernel/include/mem.h"
#include "../../kernel/include/perf.h"
#include "../../kernel/include/kernel.h"

#define BENCH_ITERATIONS 10000

/**
 * Benchmark PMM page allocation
 *
 * Measures the cost of allocating and freeing single pages
 */
void bench_pmm_alloc_free(void) {
    kprintf("\n[BENCH] PMM Page Allocation Benchmark\n");
    kprintf("======================================\n");

    perf_stat_t alloc_stats, free_stats;
    perf_stat_init(&alloc_stats, "pmm_alloc_page");
    perf_stat_init(&free_stats, "pmm_free_page");

    void* pages[100];

    // Benchmark allocation
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        void* page = pmm_alloc_page();
        uint64_t end = rdtscp();

        if (page) {
            perf_stat_record(&alloc_stats, end - start);
            pages[i % 100] = page;
        }

        // Free every 100th allocation
        if (i % 100 == 99) {
            for (int j = 0; j < 100; j++) {
                uint64_t free_start = rdtsc_fence();
                pmm_free_page(pages[j]);
                uint64_t free_end = rdtscp();
                perf_stat_record(&free_stats, free_end - free_start);
            }
        }
    }

    perf_stat_report(&alloc_stats);
    perf_stat_report(&free_stats);

    // Calculate throughput
    uint64_t avg_alloc = alloc_stats.total_cycles / alloc_stats.iterations;
    uint64_t cpu_freq_hz = perf_get_cpu_freq_mhz() * 1000000;
    uint64_t allocs_per_sec = cpu_freq_hz / avg_alloc;
    uint64_t mbytes_per_sec = (allocs_per_sec * 4096) / (1024 * 1024);

    kprintf("\n[BENCH] Throughput:\n");
    kprintf("  Allocations/second: %llu\n", allocs_per_sec);
    kprintf("  MB/second: %llu MB/s\n", mbytes_per_sec);

    kprintf("\n");
}

/**
 * Benchmark PMM bulk allocation
 *
 * Measures allocation performance when allocating many pages at once
 */
void bench_pmm_bulk_alloc(void) {
    kprintf("\n[BENCH] PMM Bulk Allocation Benchmark\n");
    kprintf("======================================\n");

    const int bulk_sizes[] = {1, 10, 100, 1000};
    const int num_sizes = sizeof(bulk_sizes) / sizeof(bulk_sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int bulk_size = bulk_sizes[s];
        void** pages = kmalloc(bulk_size * sizeof(void*));

        perf_stat_t stats;
        char name[64];
        snprintf(name, sizeof(name), "Bulk alloc %d pages", bulk_size);
        perf_stat_init(&stats, name);

        for (int i = 0; i < 1000; i++) {
            uint64_t start = rdtsc_fence();
            for (int j = 0; j < bulk_size; j++) {
                pages[j] = pmm_alloc_page();
            }
            uint64_t end = rdtscp();
            perf_stat_record(&stats, end - start);

            // Free pages
            for (int j = 0; j < bulk_size; j++) {
                if (pages[j]) {
                    pmm_free_page(pages[j]);
                }
            }
        }

        perf_stat_report(&stats);

        uint64_t avg = stats.total_cycles / stats.iterations;
        uint64_t cycles_per_page = avg / bulk_size;
        kprintf("  Cycles/page: %llu\n\n", cycles_per_page);

        kfree(pages);
    }
}

/**
 * Benchmark heap allocation (kmalloc/kfree)
 *
 * Measures kernel heap performance for various allocation sizes
 */
void bench_heap_alloc(void) {
    kprintf("\n[BENCH] Heap Allocation Benchmark\n");
    kprintf("==================================\n");

    const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 4096};
    const int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        size_t size = sizes[s];

        perf_stat_t alloc_stats, free_stats;
        char alloc_name[64], free_name[64];
        snprintf(alloc_name, sizeof(alloc_name), "kmalloc(%zu)", size);
        snprintf(free_name, sizeof(free_name), "kfree(%zu)", size);
        perf_stat_init(&alloc_stats, alloc_name);
        perf_stat_init(&free_stats, free_name);

        void* ptrs[100];

        for (int i = 0; i < 1000; i++) {
            // Allocate
            for (int j = 0; j < 100; j++) {
                uint64_t start = rdtsc_fence();
                ptrs[j] = kmalloc(size);
                uint64_t end = rdtscp();
                if (ptrs[j]) {
                    perf_stat_record(&alloc_stats, end - start);
                }
            }

            // Free
            for (int j = 0; j < 100; j++) {
                if (ptrs[j]) {
                    uint64_t start = rdtsc_fence();
                    kfree(ptrs[j]);
                    uint64_t end = rdtscp();
                    perf_stat_record(&free_stats, end - start);
                }
            }
        }

        perf_stat_report(&alloc_stats);
        perf_stat_report(&free_stats);
        kprintf("\n");
    }
}

/**
 * Benchmark memory access patterns
 *
 * Tests cache behavior with different access patterns
 */
void bench_memory_access(void) {
    kprintf("\n[BENCH] Memory Access Pattern Benchmark\n");
    kprintf("========================================\n");

    const size_t buffer_size = 1024 * 1024;  // 1 MB
    uint8_t* buffer = kmalloc(buffer_size);

    if (!buffer) {
        kprintf("[BENCH] ERROR: Failed to allocate buffer\n");
        return;
    }

    // Sequential read
    perf_stat_t seq_read_stats;
    perf_stat_init(&seq_read_stats, "Sequential read");

    for (int i = 0; i < 1000; i++) {
        uint64_t start = rdtsc_fence();
        volatile uint8_t sum = 0;
        for (size_t j = 0; j < buffer_size; j++) {
            sum += buffer[j];
        }
        uint64_t end = rdtscp();
        perf_stat_record(&seq_read_stats, end - start);
    }

    perf_stat_report(&seq_read_stats);

    // Random read
    perf_stat_t rand_read_stats;
    perf_stat_init(&rand_read_stats, "Random read (stride 4096)");

    for (int i = 0; i < 1000; i++) {
        uint64_t start = rdtsc_fence();
        volatile uint8_t sum = 0;
        for (size_t j = 0; j < buffer_size; j += 4096) {
            sum += buffer[j];
        }
        uint64_t end = rdtscp();
        perf_stat_record(&rand_read_stats, end - start);
    }

    perf_stat_report(&rand_read_stats);

    // Calculate cache miss impact
    uint64_t seq_avg = seq_read_stats.total_cycles / seq_read_stats.iterations;
    uint64_t rand_avg = rand_read_stats.total_cycles / rand_read_stats.iterations;
    uint64_t seq_per_byte = seq_avg / buffer_size;
    uint64_t rand_per_access = rand_avg / (buffer_size / 4096);

    kprintf("\n[BENCH] Analysis:\n");
    kprintf("  Sequential: %.2f cycles/byte\n", (double)seq_per_byte);
    kprintf("  Random (4KB stride): %llu cycles/access\n", rand_per_access);
    kprintf("  Cache miss penalty: ~%llu cycles\n", rand_per_access - seq_per_byte);

    kfree(buffer);
    kprintf("\n");
}

/**
 * Benchmark page table operations
 */
void bench_page_table_ops(void) {
    kprintf("\n[BENCH] Page Table Operations Benchmark\n");
    kprintf("========================================\n");

    perf_stat_t map_stats, unmap_stats;
    perf_stat_init(&map_stats, "paging_map_page");
    perf_stat_init(&unmap_stats, "paging_unmap_page");

    void* virt_base = (void*)0x10000000;  // Some virtual address

    for (int i = 0; i < 1000; i++) {
        void* phys = pmm_alloc_page();
        if (!phys) continue;

        void* virt = (void*)((uint64_t)virt_base + i * 0x1000);

        // Map
        uint64_t start = rdtsc_fence();
        paging_map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE);
        uint64_t end = rdtscp();
        perf_stat_record(&map_stats, end - start);

        // Unmap
        start = rdtsc_fence();
        paging_unmap_page(virt);
        end = rdtscp();
        perf_stat_record(&unmap_stats, end - start);

        pmm_free_page(phys);
    }

    perf_stat_report(&map_stats);
    perf_stat_report(&unmap_stats);

    kprintf("\n");
}

/**
 * Run all memory benchmarks
 */
void bench_memory_all(void) {
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  MEMORY BENCHMARK SUITE\n");
    kprintf("=============================================\n");

    bench_pmm_alloc_free();
    bench_pmm_bulk_alloc();
    bench_heap_alloc();
    bench_memory_access();
    bench_page_table_ops();

    kprintf("=============================================\n");
    kprintf("  BENCHMARK COMPLETE\n");
    kprintf("=============================================\n");
    kprintf("\n");
}
