/**
 * Huge Page TLB Benchmark
 *
 * Measures TLB miss reduction when using 2MB huge pages vs 4KB pages.
 * Demonstrates 99%+ reduction in TLB pressure for large allocations.
 */

#include "../../kernel/include/mem.h"
#include "../../kernel/include/perf.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/x86_64.h"

#define ALLOC_SIZE_MB 100
#define ALLOC_SIZE_BYTES (ALLOC_SIZE_MB * 1024 * 1024)
#define PAGE_4KB 4096
#define PAGE_2MB (2 * 1024 * 1024)

// Virtual address base for test mappings (in kernel space, above identity map)
#define TEST_VADDR_BASE 0xFFFFFF8000000000ULL

/**
 * Read CR3 page fault counter (if available via performance counters)
 * For now, we'll use indirect measurement via timing
 */
static inline uint64_t read_tlb_misses(void) {
    // TODO: Use performance monitoring counters (PMC) when available
    // For now, we measure via rdtsc timing - TLB misses show up as slower access
    return 0;
}

/**
 * Benchmark memory access pattern with 4KB pages
 * Random access across 100MB to maximize TLB pressure
 */
void bench_4kb_pages(void) {
    kprintf("\n[BENCH] 4KB Pages - TLB Pressure Test\n");
    kprintf("======================================\n");

    // Allocate 100MB as 4KB pages (25600 pages)
    size_t num_pages = ALLOC_SIZE_BYTES / PAGE_4KB;
    kprintf("[BENCH] Allocating %zu MB as %zu x 4KB pages...\n",
            ALLOC_SIZE_MB, num_pages);

    // Allocate physical pages
    void** pages = kmalloc(num_pages * sizeof(void*));
    if (!pages) {
        kprintf("[BENCH] ERROR: Failed to allocate page array\n");
        return;
    }

    for (size_t i = 0; i < num_pages; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) {
            kprintf("[BENCH] ERROR: Failed to allocate page %zu\n", i);
            goto cleanup_4kb;
        }
    }

    // Map pages into virtual address space
    for (size_t i = 0; i < num_pages; i++) {
        void* vaddr = (void*)(TEST_VADDR_BASE + i * PAGE_4KB);
        paging_map_page(vaddr, pages[i], PAGE_PRESENT | PAGE_WRITE);
    }

    kprintf("[BENCH] Mapped %zu pages at %p\n", num_pages, (void*)TEST_VADDR_BASE);

    // Benchmark: Random access across all pages (maximum TLB pressure)
    // Access one byte per page to force TLB lookups
    perf_stat_t access_stats;
    perf_stat_init(&access_stats, "4KB random access");

    for (int iter = 0; iter < 1000; iter++) {
        uint64_t start = rdtsc_fence();

        volatile uint8_t sum = 0;
        // Access every 64th page (stride pattern to stress TLB)
        for (size_t i = 0; i < num_pages; i += 64) {
            volatile uint8_t* addr = (volatile uint8_t*)(TEST_VADDR_BASE + i * PAGE_4KB);
            sum += *addr;
        }

        uint64_t end = rdtscp();
        perf_stat_record(&access_stats, end - start);
    }

    perf_stat_report(&access_stats);

    uint64_t avg_cycles = access_stats.total_cycles / access_stats.iterations;
    uint64_t accesses = num_pages / 64;
    uint64_t cycles_per_access = avg_cycles / accesses;

    kprintf("\n[BENCH] Analysis:\n");
    kprintf("  Total pages: %zu (requires %zu TLB entries)\n", num_pages, num_pages);
    kprintf("  Accesses per iteration: %llu\n", accesses);
    kprintf("  Cycles per access: %llu\n", cycles_per_access);
    kprintf("  TLB misses expected: HIGH (CPU TLB ~1536 entries, we need %zu)\n", num_pages);

cleanup_4kb:
    // Unmap and free
    for (size_t i = 0; i < num_pages; i++) {
        void* vaddr = (void*)(TEST_VADDR_BASE + i * PAGE_4KB);
        paging_unmap_page(vaddr);
        if (pages[i]) {
            pmm_free_page(pages[i]);
        }
    }
    kfree(pages);
}

/**
 * Benchmark memory access pattern with 2MB huge pages
 * Same access pattern, but with 512x fewer TLB entries needed
 */
void bench_2mb_huge_pages(void) {
    kprintf("\n[BENCH] 2MB Huge Pages - TLB Pressure Test\n");
    kprintf("==========================================\n");

    // Allocate 100MB as 2MB pages (50 huge pages)
    size_t num_huge_pages = ALLOC_SIZE_BYTES / PAGE_2MB;
    kprintf("[BENCH] Allocating %zu MB as %zu x 2MB huge pages...\n",
            ALLOC_SIZE_MB, num_huge_pages);

    // Allocate physical huge pages
    void** huge_pages = kmalloc(num_huge_pages * sizeof(void*));
    if (!huge_pages) {
        kprintf("[BENCH] ERROR: Failed to allocate huge page array\n");
        return;
    }

    for (size_t i = 0; i < num_huge_pages; i++) {
        huge_pages[i] = pmm_alloc_huge_page();
        if (!huge_pages[i]) {
            kprintf("[BENCH] ERROR: Failed to allocate huge page %zu\n", i);
            goto cleanup_2mb;
        }
    }

    // Map huge pages into virtual address space
    for (size_t i = 0; i < num_huge_pages; i++) {
        void* vaddr = (void*)(TEST_VADDR_BASE + i * PAGE_2MB);
        int ret = paging_map_huge_page(vaddr, huge_pages[i], PAGE_PRESENT | PAGE_WRITE);
        if (ret != 0) {
            kprintf("[BENCH] ERROR: Failed to map huge page %zu\n", i);
            goto cleanup_2mb;
        }
    }

    kprintf("[BENCH] Mapped %zu huge pages at %p\n", num_huge_pages, (void*)TEST_VADDR_BASE);

    // Benchmark: Same access pattern as 4KB test
    // Access equivalent positions (every 64 * 4KB = every 256KB)
    perf_stat_t access_stats;
    perf_stat_init(&access_stats, "2MB random access");

    size_t num_4kb_equivalents = ALLOC_SIZE_BYTES / PAGE_4KB;

    for (int iter = 0; iter < 1000; iter++) {
        uint64_t start = rdtsc_fence();

        volatile uint8_t sum = 0;
        // Access same pattern as 4KB test (every 64th 4KB position)
        for (size_t i = 0; i < num_4kb_equivalents; i += 64) {
            volatile uint8_t* addr = (volatile uint8_t*)(TEST_VADDR_BASE + i * PAGE_4KB);
            sum += *addr;
        }

        uint64_t end = rdtscp();
        perf_stat_record(&access_stats, end - start);
    }

    perf_stat_report(&access_stats);

    uint64_t avg_cycles = access_stats.total_cycles / access_stats.iterations;
    uint64_t accesses = num_4kb_equivalents / 64;
    uint64_t cycles_per_access = avg_cycles / accesses;

    kprintf("\n[BENCH] Analysis:\n");
    kprintf("  Total huge pages: %zu (requires %zu TLB entries)\n",
            num_huge_pages, num_huge_pages);
    kprintf("  Accesses per iteration: %llu\n", accesses);
    kprintf("  Cycles per access: %llu\n", cycles_per_access);
    kprintf("  TLB misses expected: LOW (%zu entries fits in CPU TLB)\n", num_huge_pages);

cleanup_2mb:
    // Unmap and free
    for (size_t i = 0; i < num_huge_pages; i++) {
        void* vaddr = (void*)(TEST_VADDR_BASE + i * PAGE_2MB);
        paging_unmap_huge_page(vaddr);
        if (huge_pages[i]) {
            pmm_free_huge_page(huge_pages[i]);
        }
    }
    kfree(huge_pages);
}

/**
 * Compare TLB performance: 4KB vs 2MB pages
 */
void bench_huge_pages_comparison(void) {
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  HUGE PAGE TLB BENCHMARK\n");
    kprintf("=============================================\n");
    kprintf("\n");
    kprintf("Goal: Measure TLB miss reduction with 2MB huge pages\n");
    kprintf("Test: Allocate 100MB, random access pattern\n");
    kprintf("\n");
    kprintf("Expected results:\n");
    kprintf("  4KB pages: 25,600 TLB entries needed -> HIGH TLB miss rate\n");
    kprintf("  2MB pages:     50 TLB entries needed -> MINIMAL TLB misses\n");
    kprintf("  Improvement: 512x reduction in TLB pressure\n");
    kprintf("\n");

    // Test 4KB pages
    bench_4kb_pages();

    // Test 2MB huge pages
    bench_2mb_huge_pages();

    // Report huge page statistics
    kprintf("\n[BENCH] Huge Page Allocator Statistics\n");
    kprintf("=======================================\n");
    uint64_t allocated, freed, failures;
    pmm_get_huge_page_stats(&allocated, &freed, &failures);
    kprintf("  Huge pages allocated: %llu\n", allocated);
    kprintf("  Huge pages freed: %llu\n", freed);
    kprintf("  Allocation failures: %llu\n", failures);
    kprintf("  Currently in use: %llu (should be 0 after benchmark)\n", allocated - freed);

    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  BENCHMARK COMPLETE\n");
    kprintf("=============================================\n");
    kprintf("\n");
}

/**
 * Transparent huge page test: automatically promote large mmap allocations
 */
void bench_transparent_huge_pages(void) {
    kprintf("\n[BENCH] Transparent Huge Pages\n");
    kprintf("==============================\n");
    kprintf("\n");
    kprintf("Testing automatic promotion of large allocations to huge pages...\n");
    kprintf("(Feature not yet implemented - manual huge page API demonstrated above)\n");
    kprintf("\n");

    // TODO: Implement transparent huge pages:
    // - vmm_mmap_anon() checks if size >= 2MB
    // - Allocates 2MB-aligned physical memory via pmm_alloc_huge_page()
    // - Maps using paging_map_huge_page() instead of 4KB pages
    // - Falls back to 4KB pages if huge page allocation fails

    kprintf("To enable THP: modify vmm_mmap_anon() in vmm.c:\n");
    kprintf("  if (len >= 2*1024*1024 && !(len & 0x1FFFFF)) {\n");
    kprintf("      // Use huge pages for 2MB-aligned allocations\n");
    kprintf("      void* phys = pmm_alloc_huge_page();\n");
    kprintf("      paging_map_huge_page(vaddr, phys, flags);\n");
    kprintf("  }\n");
    kprintf("\n");
}
