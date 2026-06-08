/*
 * TLB Shootdown Benchmark
 * =======================
 *
 * Measures IPI count and latency during heavy munmap workloads.
 * Validates lazy TLB shootdown effectiveness.
 *
 * Test scenarios:
 *   1. Single-page unmap (1000 iterations)
 *   2. Multi-page unmap (100 pages x 100 iterations)
 *   3. Heavy munmap stress (simulate real workload)
 *
 * Expected results with lazy TLB:
 *   - IPI reduction: 60-80%
 *   - Latency improvement: 40-60%
 */

#include "../common/bench_common.h"
#include "../../kernel/include/mem.h"
#include "../../kernel/include/tlb.h"
#include "../../kernel/include/ipi.h"
#include "../../kernel/include/smp.h"
#include "../../kernel/include/kernel.h"

#define TEST_ITERATIONS 1000
#define MULTI_PAGE_COUNT 100
#define HEAVY_ITERATIONS 100

// Test statistics
static struct {
    uint64_t ipi_sent_before;
    uint64_t ipi_sent_after;
    uint64_t ipi_avoided_before;
    uint64_t ipi_avoided_after;
    uint64_t start_tsc;
    uint64_t end_tsc;
} bench_stats;

// Capture TLB statistics before test
static void capture_tlb_stats_before(void) {
    bench_stats.ipi_sent_before = 0;
    bench_stats.ipi_avoided_before = 0;

    // Sum across all CPUs
    extern struct {
        uint64_t lazy_flushes;
        uint64_t immediate_flushes;
        uint64_t batched_flushes;
        uint64_t ipi_sent;
        uint64_t ipi_avoided;
    } tlb_stats[MAX_CPUS];

    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        bench_stats.ipi_sent_before += tlb_stats[cpu].ipi_sent;
        bench_stats.ipi_avoided_before += tlb_stats[cpu].ipi_avoided;
    }

    bench_stats.start_tsc = rdtsc();
}

// Capture TLB statistics after test
static void capture_tlb_stats_after(void) {
    bench_stats.end_tsc = rdtsc();

    bench_stats.ipi_sent_after = 0;
    bench_stats.ipi_avoided_after = 0;

    extern struct {
        uint64_t lazy_flushes;
        uint64_t immediate_flushes;
        uint64_t batched_flushes;
        uint64_t ipi_sent;
        uint64_t ipi_avoided;
    } tlb_stats[MAX_CPUS];

    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        bench_stats.ipi_sent_after += tlb_stats[cpu].ipi_sent;
        bench_stats.ipi_avoided_after += tlb_stats[cpu].ipi_avoided;
    }
}

// Print benchmark results
static void print_bench_results(const char* test_name) {
    uint64_t cycles = bench_stats.end_tsc - bench_stats.start_tsc;
    uint64_t ipi_sent = bench_stats.ipi_sent_after - bench_stats.ipi_sent_before;
    uint64_t ipi_avoided = bench_stats.ipi_avoided_after - bench_stats.ipi_avoided_before;
    uint64_t total_ipi_ops = ipi_sent + ipi_avoided;

    kprintf("\n=== %s ===\n", test_name);
    kprintf("  Total cycles:     %llu\n", cycles);
    kprintf("  IPIs sent:        %llu\n", ipi_sent);
    kprintf("  IPIs avoided:     %llu\n", ipi_avoided);
    kprintf("  Total IPI ops:    %llu\n", total_ipi_ops);

    if (total_ipi_ops > 0) {
        uint64_t avoidance_pct = (ipi_avoided * 100) / total_ipi_ops;
        kprintf("  IPI reduction:    %llu%%\n", avoidance_pct);
    }

    if (total_ipi_ops > 0) {
        uint64_t cycles_per_op = cycles / total_ipi_ops;
        kprintf("  Cycles/IPI op:    %llu\n", cycles_per_op);
    }

    kprintf("\n");
}

// Test 1: Single-page unmap
static void test_single_page_unmap(void) {
    kprintf("[BENCH] Starting single-page unmap test (%d iterations)...\n", TEST_ITERATIONS);

    // Create test address space
    uint64_t cr3 = paging_create_address_space();
    if (!cr3) {
        kprintf("[BENCH] Failed to create address space\n");
        return;
    }

    paging_set_target(cr3);

    // Allocate and map test pages
    void* virt_addr = (void*)0x10000000ULL;
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        void* phys = pmm_alloc_page();
        if (phys) {
            paging_map_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE), phys,
                          PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }
    }

    paging_reset_target();

    // Benchmark: unmap pages one at a time
    capture_tlb_stats_before();

    paging_set_target(cr3);
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        paging_unmap_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE));
    }
    paging_reset_target();

    capture_tlb_stats_after();

    // Cleanup
    paging_destroy_address_space(cr3);

    print_bench_results("Single-Page Unmap Test");
}

// Test 2: Multi-page unmap
static void test_multi_page_unmap(void) {
    kprintf("[BENCH] Starting multi-page unmap test (%d pages x %d iterations)...\n",
            MULTI_PAGE_COUNT, HEAVY_ITERATIONS);

    // Create test address space
    uint64_t cr3 = paging_create_address_space();
    if (!cr3) {
        kprintf("[BENCH] Failed to create address space\n");
        return;
    }

    paging_set_target(cr3);

    // Allocate and map test pages
    void* virt_addr = (void*)0x20000000ULL;
    for (int iter = 0; iter < HEAVY_ITERATIONS; iter++) {
        for (int i = 0; i < MULTI_PAGE_COUNT; i++) {
            void* phys = pmm_alloc_page();
            if (phys) {
                paging_map_page((void*)((uint64_t)virt_addr +
                              (iter * MULTI_PAGE_COUNT + i) * PAGE_SIZE),
                              phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
            }
        }
    }

    paging_reset_target();

    // Benchmark: unmap pages in batches
    capture_tlb_stats_before();

    paging_set_target(cr3);
    for (int iter = 0; iter < HEAVY_ITERATIONS; iter++) {
        for (int i = 0; i < MULTI_PAGE_COUNT; i++) {
            paging_unmap_page((void*)((uint64_t)virt_addr +
                            (iter * MULTI_PAGE_COUNT + i) * PAGE_SIZE));
        }
    }
    paging_reset_target();

    capture_tlb_stats_after();

    // Cleanup
    paging_destroy_address_space(cr3);

    print_bench_results("Multi-Page Unmap Test");
}

// Test 3: Heavy munmap stress (simulates real workload)
static void test_heavy_munmap_stress(void) {
    kprintf("[BENCH] Starting heavy munmap stress test...\n");

    // Simulate realistic munmap patterns: allocate, use, free
    capture_tlb_stats_before();

    for (int iter = 0; iter < HEAVY_ITERATIONS; iter++) {
        // Create temporary address space
        uint64_t cr3 = paging_create_address_space();
        if (!cr3) continue;

        paging_set_target(cr3);

        // Allocate some pages
        void* virt_addr = (void*)0x30000000ULL;
        for (int i = 0; i < 50; i++) {
            void* phys = pmm_alloc_page();
            if (phys) {
                paging_map_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE),
                              phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
            }
        }

        // Unmap half of them (simulates partial munmap)
        for (int i = 0; i < 25; i++) {
            paging_unmap_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE));
        }

        paging_reset_target();

        // Destroy address space (unmaps remaining pages)
        paging_destroy_address_space(cr3);
    }

    capture_tlb_stats_after();

    print_bench_results("Heavy Munmap Stress Test");
}

// Main benchmark entry point
void tlb_shootdown_benchmark(void) {
    kprintf("\n========================================\n");
    kprintf("   TLB Shootdown Benchmark\n");
    kprintf("========================================\n");
    kprintf("CPUs: %u\n", smp_num_cpus);
    kprintf("Testing lazy TLB shootdown effectiveness\n");
    kprintf("========================================\n\n");

    // Reset TLB statistics
    tlb_reset_stats();

    // Run tests
    test_single_page_unmap();
    test_multi_page_unmap();
    test_heavy_munmap_stress();

    // Print final statistics
    kprintf("\n========================================\n");
    kprintf("   Final TLB Statistics\n");
    kprintf("========================================\n");
    tlb_print_stats();

    kprintf("\n========================================\n");
    kprintf("   Benchmark Complete\n");
    kprintf("========================================\n\n");
}
