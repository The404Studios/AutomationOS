/**
 * Huge Page Unit Tests
 *
 * Tests the 2MB huge page allocator and mapping functionality.
 */

#include "../include/mem.h"
#include "../include/kernel.h"
#include "../include/perf.h"

void test_huge_page_allocator(void) {
    kprintf("\n[TEST] Huge Page Allocator\n");
    kprintf("===========================\n");

    // Test 1: Allocate a single huge page
    kprintf("[TEST] Allocating single 2MB huge page...\n");
    void* huge_page = pmm_alloc_huge_page();
    if (!huge_page) {
        kprintf("[TEST] FAIL: Could not allocate huge page\n");
        return;
    }
    kprintf("[TEST] PASS: Allocated huge page at %p\n", huge_page);

    // Verify 2MB alignment
    uint64_t addr = (uint64_t)huge_page;
    if (addr & 0x1FFFFF) {
        kprintf("[TEST] FAIL: Huge page not 2MB-aligned (addr=%p)\n", huge_page);
        pmm_free_huge_page(huge_page);
        return;
    }
    kprintf("[TEST] PASS: Huge page is 2MB-aligned\n");

    // Test 2: Write and read from huge page
    kprintf("[TEST] Testing huge page access...\n");
    uint8_t* ptr = (uint8_t*)huge_page;
    for (size_t i = 0; i < 2 * 1024 * 1024; i += 4096) {
        ptr[i] = (uint8_t)(i & 0xFF);
    }
    for (size_t i = 0; i < 2 * 1024 * 1024; i += 4096) {
        if (ptr[i] != (uint8_t)(i & 0xFF)) {
            kprintf("[TEST] FAIL: Data mismatch at offset %zu\n", i);
            pmm_free_huge_page(huge_page);
            return;
        }
    }
    kprintf("[TEST] PASS: Huge page memory access works\n");

    // Test 3: Free and re-allocate
    kprintf("[TEST] Freeing huge page...\n");
    pmm_free_huge_page(huge_page);

    kprintf("[TEST] Re-allocating huge page...\n");
    huge_page = pmm_alloc_huge_page();
    if (!huge_page) {
        kprintf("[TEST] FAIL: Could not re-allocate huge page\n");
        return;
    }
    kprintf("[TEST] PASS: Re-allocated huge page at %p\n", huge_page);

    pmm_free_huge_page(huge_page);

    // Test 4: Allocate multiple huge pages
    kprintf("[TEST] Allocating 10 huge pages...\n");
    void* huge_pages[10];
    for (int i = 0; i < 10; i++) {
        huge_pages[i] = pmm_alloc_huge_page();
        if (!huge_pages[i]) {
            kprintf("[TEST] WARN: Could only allocate %d huge pages\n", i);
            // Free what we got
            for (int j = 0; j < i; j++) {
                pmm_free_huge_page(huge_pages[j]);
            }
            return;
        }
    }
    kprintf("[TEST] PASS: Allocated 10 huge pages\n");

    // Verify all are unique and aligned
    for (int i = 0; i < 10; i++) {
        uint64_t addr_i = (uint64_t)huge_pages[i];
        if (addr_i & 0x1FFFFF) {
            kprintf("[TEST] FAIL: Huge page %d not aligned\n", i);
            goto cleanup_multiple;
        }
        for (int j = i + 1; j < 10; j++) {
            if (huge_pages[i] == huge_pages[j]) {
                kprintf("[TEST] FAIL: Duplicate huge pages at indices %d and %d\n", i, j);
                goto cleanup_multiple;
            }
        }
    }
    kprintf("[TEST] PASS: All huge pages are unique and aligned\n");

cleanup_multiple:
    for (int i = 0; i < 10; i++) {
        pmm_free_huge_page(huge_pages[i]);
    }

    kprintf("[TEST] Huge page allocator tests complete\n\n");
}

void test_huge_page_mapping(void) {
    kprintf("\n[TEST] Huge Page Mapping\n");
    kprintf("========================\n");

    // Test virtual address (in kernel space)
    void* test_vaddr = (void*)0xFFFFFF8000000000ULL;

    // Test 1: Map a huge page
    kprintf("[TEST] Mapping 2MB huge page...\n");
    void* huge_page = pmm_alloc_huge_page();
    if (!huge_page) {
        kprintf("[TEST] FAIL: Could not allocate huge page\n");
        return;
    }

    int ret = paging_map_huge_page(test_vaddr, huge_page, PAGE_PRESENT | PAGE_WRITE);
    if (ret != 0) {
        kprintf("[TEST] FAIL: paging_map_huge_page returned %d\n", ret);
        pmm_free_huge_page(huge_page);
        return;
    }
    kprintf("[TEST] PASS: Mapped huge page at virt=%p, phys=%p\n", test_vaddr, huge_page);

    // Test 2: Access the mapped huge page
    kprintf("[TEST] Accessing mapped huge page...\n");
    uint8_t* ptr = (uint8_t*)test_vaddr;
    for (size_t i = 0; i < 2 * 1024 * 1024; i += 4096) {
        ptr[i] = (uint8_t)(i & 0xFF);
    }
    for (size_t i = 0; i < 2 * 1024 * 1024; i += 4096) {
        if (ptr[i] != (uint8_t)(i & 0xFF)) {
            kprintf("[TEST] FAIL: Data mismatch at offset %zu\n", i);
            goto cleanup_mapping;
        }
    }
    kprintf("[TEST] PASS: Huge page mapping works correctly\n");

    // Test 3: Unmap the huge page
    kprintf("[TEST] Unmapping huge page...\n");
    ret = paging_unmap_huge_page(test_vaddr);
    if (ret != 0) {
        kprintf("[TEST] FAIL: paging_unmap_huge_page returned %d\n", ret);
        goto cleanup_mapping;
    }
    kprintf("[TEST] PASS: Unmapped huge page\n");

    pmm_free_huge_page(huge_page);

    // Test 4: Alignment validation
    kprintf("[TEST] Testing alignment validation...\n");
    void* unaligned_vaddr = (void*)0xFFFFFF8000100000ULL;  // Not 2MB-aligned
    void* aligned_phys = pmm_alloc_huge_page();
    if (aligned_phys) {
        ret = paging_map_huge_page(unaligned_vaddr, aligned_phys, PAGE_PRESENT | PAGE_WRITE);
        if (ret == 0) {
            kprintf("[TEST] FAIL: Should reject unaligned virtual address\n");
            paging_unmap_huge_page(unaligned_vaddr);
        } else {
            kprintf("[TEST] PASS: Correctly rejected unaligned virtual address\n");
        }
        pmm_free_huge_page(aligned_phys);
    }

    kprintf("[TEST] Huge page mapping tests complete\n\n");
    return;

cleanup_mapping:
    paging_unmap_huge_page(test_vaddr);
    pmm_free_huge_page(huge_page);
}

void test_huge_pages_performance(void) {
    kprintf("\n[TEST] Huge Page Performance\n");
    kprintf("============================\n");

    // Measure allocation time
    perf_stat_t alloc_stats;
    perf_stat_init(&alloc_stats, "huge page allocation");

    for (int i = 0; i < 100; i++) {
        uint64_t start = rdtsc_fence();
        void* huge_page = pmm_alloc_huge_page();
        uint64_t end = rdtscp();

        if (huge_page) {
            perf_stat_record(&alloc_stats, end - start);
            pmm_free_huge_page(huge_page);
        } else {
            kprintf("[TEST] WARN: Allocation failed at iteration %d\n", i);
            break;
        }
    }

    perf_stat_report(&alloc_stats);

    // Compare to 512 x 4KB page allocations
    perf_stat_t small_alloc_stats;
    perf_stat_init(&small_alloc_stats, "512 x 4KB allocation");

    for (int i = 0; i < 100; i++) {
        void* pages[512];
        uint64_t start = rdtsc_fence();
        for (int j = 0; j < 512; j++) {
            pages[j] = pmm_alloc_page();
        }
        uint64_t end = rdtscp();

        perf_stat_record(&small_alloc_stats, end - start);

        for (int j = 0; j < 512; j++) {
            if (pages[j]) pmm_free_page(pages[j]);
        }
    }

    perf_stat_report(&small_alloc_stats);

    uint64_t huge_avg = alloc_stats.total_cycles / alloc_stats.iterations;
    uint64_t small_avg = small_alloc_stats.total_cycles / small_alloc_stats.iterations;

    kprintf("\n[TEST] Performance Comparison:\n");
    kprintf("  1x 2MB huge page: %llu cycles\n", huge_avg);
    kprintf("  512x 4KB pages: %llu cycles\n", small_avg);
    kprintf("  Speedup: %.2fx\n", (double)small_avg / (double)huge_avg);

    kprintf("[TEST] Performance tests complete\n\n");
}

void run_huge_page_tests(void) {
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  HUGE PAGE TEST SUITE\n");
    kprintf("=============================================\n");

    test_huge_page_allocator();
    test_huge_page_mapping();
    test_huge_pages_performance();

    // Report final statistics
    uint64_t allocated, freed, failures;
    pmm_get_huge_page_stats(&allocated, &freed, &failures);
    kprintf("\n[TEST] Final Statistics:\n");
    kprintf("  Huge pages allocated: %llu\n", allocated);
    kprintf("  Huge pages freed: %llu\n", freed);
    kprintf("  Allocation failures: %llu\n", failures);
    kprintf("  Leaked huge pages: %llu (should be 0)\n", allocated - freed);

    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  TEST SUITE COMPLETE\n");
    kprintf("=============================================\n");
    kprintf("\n");
}
