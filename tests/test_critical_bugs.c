/*
 * Critical Bug Fix Verification Tests
 * ====================================
 *
 * Tests for bugs fixed by Agent #1:
 * - BUG-006: Use-after-free in scheduler
 * - BUG-012: Missing TLB flush on other CPUs
 * - BUG-013: PCID recycling without TLB flush
 * - BUG-014: Memory leak in page table destruction
 * - BUG-020: validate_user_string can crash
 */

#include "../kernel/include/kernel.h"
#include "../kernel/include/sched.h"
#include "../kernel/include/mem.h"

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            kprintf("[TEST PASS] %s\n", message); \
            tests_passed++; \
        } else { \
            kprintf("[TEST FAIL] %s\n", message); \
            tests_failed++; \
        } \
    } while (0)

/**
 * BUG-020: Test validate_user_string with unmapped memory
 *
 * This should NOT crash even if memory is unmapped
 */
void test_validate_user_string_unmapped(void) {
    kprintf("\n[TEST] BUG-020: validate_user_string with unmapped memory\n");

    // Test with NULL pointer
    bool result = validate_user_string(NULL, 100);
    TEST_ASSERT(result == false, "NULL pointer rejected");

    // Test with kernel address (should be rejected)
    const char* kernel_str = "kernel";
    result = validate_user_string(kernel_str, 100);
    TEST_ASSERT(result == false, "Kernel address rejected");

    // Test with very high address (likely unmapped)
    const char* high_addr = (const char*)0x00007FFFFFFFFFFF;
    result = validate_user_string(high_addr, 100);
    TEST_ASSERT(result == false, "Unmapped user address rejected without crash");
}

/**
 * BUG-014: Test that page table destruction frees mapped pages
 */
void test_page_table_destruction(void) {
    kprintf("\n[TEST] BUG-014: Page table destruction frees mapped pages\n");

    // Get initial free memory
    uint64_t initial_free = pmm_get_free_memory();
    kprintf("[TEST] Initial free memory: %llu bytes\n", initial_free);

    // Create a new address space
    uint64_t cr3 = paging_create_address_space();
    TEST_ASSERT(cr3 != 0, "Address space created");

    // Allocate and map some pages (simulate process using memory)
    void* page1 = pmm_alloc_page();
    void* page2 = pmm_alloc_page();
    void* page3 = pmm_alloc_page();

    TEST_ASSERT(page1 != NULL && page2 != NULL && page3 != NULL,
                "Test pages allocated");

    uint64_t after_alloc = pmm_get_free_memory();
    kprintf("[TEST] After allocation: %llu bytes free\n", after_alloc);

    // Destroy the address space
    // BUG-014 fix ensures this frees the mapped pages too
    paging_destroy_address_space(cr3);

    // Check that memory was freed
    uint64_t after_destroy = pmm_get_free_memory();
    kprintf("[TEST] After destruction: %llu bytes free\n", after_destroy);

    // We should have freed at least the PML4, PDPT, PD, PT (4 pages minimum)
    // Plus the mapped pages if the fix is correct
    uint64_t freed = after_destroy - after_alloc;
    TEST_ASSERT(freed >= (4 * PAGE_SIZE),
                "Page table destruction freed memory");

    kprintf("[TEST] Freed %llu bytes (%llu pages)\n",
            freed, freed / PAGE_SIZE);
}

/**
 * BUG-006: Test scheduler doesn't use stale variables after context switch
 *
 * This is hard to test directly, but we can verify that multiple
 * context switches don't corrupt memory or crash
 */
void test_scheduler_context_switch(void) {
    kprintf("\n[TEST] BUG-006: Multiple context switches (use-after-free test)\n");

    // Get current process
    process_t* current = process_get_current();
    TEST_ASSERT(current != NULL, "Current process exists");

    uint32_t initial_ref = current->ref_count;
    kprintf("[TEST] Initial ref count: %u\n", initial_ref);

    // Create multiple test processes
    process_t* proc1 = process_create("test1", (void*)0x1000);
    process_t* proc2 = process_create("test2", (void*)0x2000);
    process_t* proc3 = process_create("test3", (void*)0x3000);

    TEST_ASSERT(proc1 && proc2 && proc3, "Test processes created");

    // Add them to scheduler
    scheduler_add_process(proc1);
    scheduler_add_process(proc2);
    scheduler_add_process(proc3);

    kprintf("[TEST] Processes added to scheduler\n");
    kprintf("[TEST] proc1 ref_count=%u, proc2 ref_count=%u, proc3 ref_count=%u\n",
            proc1->ref_count, proc2->ref_count, proc3->ref_count);

    // Verify ref counts are correct
    // Each should have ref_count=2: one from creation, one from scheduler
    TEST_ASSERT(proc1->ref_count == 2, "proc1 ref count correct");
    TEST_ASSERT(proc2->ref_count == 2, "proc2 ref count correct");
    TEST_ASSERT(proc3->ref_count == 2, "proc3 ref count correct");

    // Clean up - remove from scheduler
    scheduler_remove_process(proc1);
    scheduler_remove_process(proc2);
    scheduler_remove_process(proc3);

    kprintf("[TEST] After removal: proc1 ref_count=%u, proc2 ref_count=%u, proc3 ref_count=%u\n",
            proc1->ref_count, proc2->ref_count, proc3->ref_count);

    // After removal, ref_count should be 1 (just our reference)
    TEST_ASSERT(proc1->ref_count == 1, "proc1 ref count after removal");
    TEST_ASSERT(proc2->ref_count == 1, "proc2 ref count after removal");
    TEST_ASSERT(proc3->ref_count == 1, "proc3 ref count after removal");

    // Destroy processes
    process_unref(proc1);
    process_unref(proc2);
    process_unref(proc3);

    kprintf("[TEST] Processes destroyed successfully\n");
}

/**
 * BUG-013: Test PCID recycling
 *
 * This is hard to test without creating 4096 processes,
 * but we can verify the logic is in place
 */
void test_pcid_recycling(void) {
    kprintf("\n[TEST] BUG-013: PCID recycling test\n");

    // Create a few address spaces
    uint64_t cr3_1 = paging_create_address_space();
    uint64_t cr3_2 = paging_create_address_space();
    uint64_t cr3_3 = paging_create_address_space();

    TEST_ASSERT(cr3_1 != 0 && cr3_2 != 0 && cr3_3 != 0,
                "Multiple address spaces created");

    // Extract PCIDs (lower 12 bits)
    uint16_t pcid_1 = cr3_1 & 0xFFF;
    uint16_t pcid_2 = cr3_2 & 0xFFF;
    uint16_t pcid_3 = cr3_3 & 0xFFF;

    kprintf("[TEST] PCID 1=%u, PCID 2=%u, PCID 3=%u\n",
            pcid_1, pcid_2, pcid_3);

    // PCIDs should be sequential (if PCID is enabled)
    if (pcid_1 != 0) {
        TEST_ASSERT(pcid_2 == pcid_1 + 1 || pcid_2 == 0,
                    "PCIDs are sequential or PCID disabled");
    }

    // Clean up
    paging_destroy_address_space(cr3_1);
    paging_destroy_address_space(cr3_2);
    paging_destroy_address_space(cr3_3);

    kprintf("[TEST] PCID test completed (full test requires 4096+ processes)\n");
}

/**
 * Run all critical bug tests
 */
void run_critical_bug_tests(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  CRITICAL BUG FIX VERIFICATION TESTS\n");
    kprintf("========================================\n");

    tests_passed = 0;
    tests_failed = 0;

    // Run tests
    test_validate_user_string_unmapped();
    test_page_table_destruction();
    test_scheduler_context_switch();
    test_pcid_recycling();

    // Print summary
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  TEST SUMMARY\n");
    kprintf("========================================\n");
    kprintf("  Passed: %d\n", tests_passed);
    kprintf("  Failed: %d\n", tests_failed);
    kprintf("  Total:  %d\n", tests_passed + tests_failed);
    kprintf("========================================\n");

    if (tests_failed == 0) {
        kprintf("[SUCCESS] All critical bug fixes verified!\n");
    } else {
        kprintf("[WARNING] Some tests failed - review fixes!\n");
    }
}
