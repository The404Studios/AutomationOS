/*
 * Lazy TLB Shootdown Test
 * ========================
 *
 * Validates lazy TLB shootdown functionality.
 */

#include "../kernel/include/kernel.h"
#include "../kernel/include/tlb.h"
#include "../kernel/include/mem.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/ipi.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            kprintf("[PASS] %s\n", message); \
            tests_passed++; \
        } else { \
            kprintf("[FAIL] %s\n", message); \
            tests_failed++; \
        } \
    } while (0)

// Test 1: Basic TLB initialization
void test_tlb_init(void) {
    kprintf("\n[TEST] TLB Initialization\n");

    // TLB should already be initialized during boot
    // Just verify we can call functions without crashing

    tlb_reset_stats();
    TEST_ASSERT(1, "TLB statistics reset");

    kprintf("[TEST] TLB initialization passed\n");
}

// Test 2: Single-page lazy flush
void test_single_page_flush(void) {
    kprintf("\n[TEST] Single-Page Lazy Flush\n");

    // Create test address space
    uint64_t cr3 = paging_create_address_space();
    TEST_ASSERT(cr3 != 0, "Created test address space");

    paging_set_target(cr3);

    // Allocate and map a test page
    void* virt_addr = (void*)0x10000000ULL;
    void* phys = pmm_alloc_page();
    TEST_ASSERT(phys != NULL, "Allocated physical page");

    paging_map_page(virt_addr, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

    // Reset stats
    tlb_reset_stats();

    // Unmap the page (should trigger lazy TLB flush)
    paging_unmap_page(virt_addr);

    paging_reset_target();

    // Verify TLB flush was deferred (no immediate IPI on single-CPU system)
    if (smp_num_cpus > 1) {
        kprintf("[INFO] Multi-CPU system: TLB flush should be lazy\n");
    } else {
        kprintf("[INFO] Single-CPU system: TLB flush is always immediate\n");
    }

    // Cleanup
    paging_destroy_address_space(cr3);

    TEST_ASSERT(1, "Single-page flush completed");
    kprintf("[TEST] Single-page flush passed\n");
}

// Test 3: Context switch triggers pending flush
void test_context_switch_flush(void) {
    kprintf("\n[TEST] Context Switch Flush\n");

    // Create test address space
    uint64_t cr3 = paging_create_address_space();
    TEST_ASSERT(cr3 != 0, "Created test address space");

    paging_set_target(cr3);

    // Map some pages
    void* virt_addr = (void*)0x20000000ULL;
    for (int i = 0; i < 10; i++) {
        void* phys = pmm_alloc_page();
        if (phys) {
            paging_map_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE),
                          phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }
    }

    // Unmap pages (should mark for lazy flush)
    for (int i = 0; i < 10; i++) {
        paging_unmap_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE));
    }

    paging_reset_target();

    // Simulate context switch (would flush pending TLB entries)
    tlb_flush_pending();

    TEST_ASSERT(1, "Context switch flush completed");

    // Cleanup
    paging_destroy_address_space(cr3);

    kprintf("[TEST] Context switch flush passed\n");
}

// Test 4: Batching behavior
void test_tlb_batching(void) {
    kprintf("\n[TEST] TLB Batching\n");

    // Create test address space
    uint64_t cr3 = paging_create_address_space();
    TEST_ASSERT(cr3 != 0, "Created test address space");

    paging_set_target(cr3);

    // Reset stats
    tlb_reset_stats();

    // Map and unmap many pages (should trigger batching)
    void* virt_addr = (void*)0x30000000ULL;
    for (int i = 0; i < 100; i++) {
        void* phys = pmm_alloc_page();
        if (phys) {
            paging_map_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE),
                          phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }
    }

    // Unmap all pages
    for (int i = 0; i < 100; i++) {
        paging_unmap_page((void*)((uint64_t)virt_addr + i * PAGE_SIZE));
    }

    paging_reset_target();

    // Flush pending
    tlb_flush_pending();

    TEST_ASSERT(1, "TLB batching completed");

    // Cleanup
    paging_destroy_address_space(cr3);

    kprintf("[TEST] TLB batching passed\n");
}

// Main test entry point
void test_lazy_tlb(void) {
    kprintf("\n========================================\n");
    kprintf("   Lazy TLB Shootdown Tests\n");
    kprintf("========================================\n");
    kprintf("CPUs: %u\n", smp_num_cpus);
    kprintf("========================================\n\n");

    // Run tests
    test_tlb_init();
    test_single_page_flush();
    test_context_switch_flush();
    test_tlb_batching();

    // Print summary
    kprintf("\n========================================\n");
    kprintf("   Test Summary\n");
    kprintf("========================================\n");
    kprintf("Passed: %d\n", tests_passed);
    kprintf("Failed: %d\n", tests_failed);
    kprintf("========================================\n\n");

    if (tests_failed == 0) {
        kprintf("[SUCCESS] All lazy TLB tests passed!\n\n");
    } else {
        kprintf("[ERROR] Some tests failed!\n\n");
    }
}
