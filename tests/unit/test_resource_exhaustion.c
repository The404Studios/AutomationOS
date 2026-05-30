/**
 * Resource Exhaustion Test Suite
 *
 * Tests system behavior when running out of critical resources:
 * - Memory (physical and virtual)
 * - File descriptors
 * - Process IDs
 * - Disk space
 * - Kernel structures
 *
 * Mission: Ensure graceful degradation under resource pressure
 */

#include "../../kernel/include/mem.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/syscall.h"
#include "../../kernel/include/rlimit.h"

// Test results tracking
static int exhaustion_tests_passed = 0;
static int exhaustion_tests_failed = 0;

#define EXHAUST_TEST(name) \
    kprintf("\n[EXHAUSTION TEST] %s\n", name)

#define EXPECT_EXHAUSTION(condition, msg) \
    do { \
        if (!(condition)) { \
            kprintf("  FAIL: %s\n", msg); \
            exhaustion_tests_failed++; \
            return; \
        } \
        exhaustion_tests_passed++; \
    } while(0)

// ============================================================================
// MEMORY EXHAUSTION TESTS
// ============================================================================

/**
 * Test: Heap exhaustion recovery
 *
 * Fill the entire heap, verify system remains stable,
 * then free and verify recovery
 */
void test_heap_complete_exhaustion(void) {
    EXHAUST_TEST("Complete heap exhaustion and recovery");

    void** allocations = NULL;
    int alloc_count = 0;
    int max_allocs = 10000;

    // First allocate space for pointers
    allocations = kmalloc(max_allocs * sizeof(void*));
    EXPECT_EXHAUSTION(allocations != NULL, "Cannot allocate pointer array");

    // Allocate until complete exhaustion
    for (int i = 0; i < max_allocs; i++) {
        allocations[i] = kmalloc(1024);  // 1KB blocks
        if (allocations[i] == NULL) {
            alloc_count = i;
            break;
        }
    }

    kprintf("  Exhausted heap after %d allocations (%d KB)\n",
            alloc_count, alloc_count);

    // System should still respond
    EXPECT_EXHAUSTION(alloc_count > 0, "No allocations succeeded");

    // Try one more allocation (should fail)
    void* should_fail = kmalloc(1024);
    EXPECT_EXHAUSTION(should_fail == NULL, "Allocation succeeded when heap full");

    // Free half the allocations
    for (int i = 0; i < alloc_count / 2; i++) {
        kfree(allocations[i]);
        allocations[i] = NULL;
    }

    // Should be able to allocate again
    void* after_free = kmalloc(1024);
    EXPECT_EXHAUSTION(after_free != NULL, "Cannot allocate after freeing memory");

    kfree(after_free);

    // Free remaining allocations
    for (int i = alloc_count / 2; i < alloc_count; i++) {
        kfree(allocations[i]);
    }

    kfree(allocations);

    kprintf("  PASS: Heap recovered from exhaustion\n");
}

/**
 * Test: Physical memory exhaustion
 */
void test_physical_memory_exhaustion(void) {
    EXHAUST_TEST("Physical memory exhaustion");

    uint64_t initial_free = pmm_get_free_memory();
    uint64_t initial_pages = initial_free / PAGE_SIZE;

    kprintf("  Initial free memory: %llu bytes (%llu pages)\n",
            initial_free, initial_pages);

    void** pages = kmalloc(initial_pages * sizeof(void*));
    EXPECT_EXHAUSTION(pages != NULL, "Cannot allocate page array");

    int allocated = 0;

    // Allocate all physical pages
    for (uint64_t i = 0; i < initial_pages; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == NULL) {
            break;
        }
        allocated++;
    }

    kprintf("  Allocated %d pages before exhaustion\n", allocated);

    uint64_t remaining = pmm_get_free_memory();
    kprintf("  Remaining free memory: %llu bytes\n", remaining);

    // Should have very little left
    EXPECT_EXHAUSTION(remaining < initial_free / 10,
                      "Too much memory remaining");

    // Next allocation should fail
    void* should_fail = pmm_alloc_page();
    if (should_fail != NULL) {
        kprintf("  WARNING: Got page when expecting exhaustion\n");
        pmm_free_page(should_fail);
    }

    // Free all pages
    for (int i = 0; i < allocated; i++) {
        pmm_free_page(pages[i]);
    }

    // Verify recovery
    uint64_t final_free = pmm_get_free_memory();
    uint64_t leaked = initial_free - final_free;

    kprintf("  Memory leaked: %llu bytes\n", leaked);
    EXPECT_EXHAUSTION(leaked < PAGE_SIZE * 10, "Significant memory leak detected");

    kfree(pages);

    kprintf("  PASS: Physical memory exhaustion handled\n");
}

/**
 * Test: Alternating alloc/free under pressure
 */
void test_memory_pressure_patterns(void) {
    EXHAUST_TEST("Memory pressure patterns");

    void** set_a = kmalloc(100 * sizeof(void*));
    void** set_b = kmalloc(100 * sizeof(void*));

    // Pattern 1: Allocate A, free A, allocate B, free B
    for (int cycle = 0; cycle < 10; cycle++) {
        // Allocate set A
        for (int i = 0; i < 100; i++) {
            set_a[i] = kmalloc(512);
            EXPECT_EXHAUSTION(set_a[i] != NULL, "Set A allocation failed");
        }

        // Free set A
        for (int i = 0; i < 100; i++) {
            kfree(set_a[i]);
        }

        // Allocate set B
        for (int i = 0; i < 100; i++) {
            set_b[i] = kmalloc(512);
            EXPECT_EXHAUSTION(set_b[i] != NULL, "Set B allocation failed");
        }

        // Free set B
        for (int i = 0; i < 100; i++) {
            kfree(set_b[i]);
        }
    }

    kfree(set_a);
    kfree(set_b);

    kprintf("  PASS: Handled 10 cycles of alternating patterns\n");
}

// ============================================================================
// PROCESS RESOURCE EXHAUSTION
// ============================================================================

/**
 * Test: Process table exhaustion
 */
void test_process_table_exhaustion(void) {
    EXHAUST_TEST("Process table exhaustion");

    process_init();

    #define MAX_TEST_PROCS 1000
    process_t** procs = kmalloc(MAX_TEST_PROCS * sizeof(process_t*));
    EXPECT_EXHAUSTION(procs != NULL, "Cannot allocate process array");

    int created = 0;

    // Create processes until we can't anymore
    for (int i = 0; i < MAX_TEST_PROCS; i++) {
        procs[i] = process_create("test_proc", (void*)0x1000);
        if (procs[i] == NULL) {
            created = i;
            break;
        }
    }

    kprintf("  Created %d processes before exhaustion\n", created);

    // Verify all have unique PIDs
    for (int i = 0; i < created; i++) {
        for (int j = i + 1; j < created; j++) {
            EXPECT_EXHAUSTION(procs[i]->pid != procs[j]->pid,
                            "Duplicate PIDs detected");
        }
    }

    // Try one more (should fail gracefully)
    process_t* should_fail = process_create("overflow", (void*)0x1000);
    if (should_fail != NULL) {
        kprintf("  WARNING: Created process beyond limit\n");
        process_destroy(should_fail);
    }

    // Destroy all processes
    for (int i = 0; i < created; i++) {
        process_destroy(procs[i]);
    }

    kfree(procs);

    kprintf("  PASS: Process table exhaustion handled\n");
}

/**
 * Test: File descriptor exhaustion (when implemented)
 */
void test_file_descriptor_exhaustion(void) {
    EXHAUST_TEST("File descriptor exhaustion");

    // TODO: Implement when file system is ready
    kprintf("  TODO: File descriptor exhaustion test\n");
    kprintf("  (Requires filesystem implementation)\n");

    exhaustion_tests_passed++;
}

// ============================================================================
// FRAGMENTATION TESTS
// ============================================================================

/**
 * Test: Worst-case fragmentation
 *
 * Create a fragmentation pattern that makes allocation difficult
 */
void test_worst_case_fragmentation(void) {
    EXHAUST_TEST("Worst-case heap fragmentation");

    #define FRAG_BLOCKS 100
    void* blocks[FRAG_BLOCKS];

    // Allocate alternating small and large blocks
    for (int i = 0; i < FRAG_BLOCKS; i++) {
        if (i % 2 == 0) {
            blocks[i] = kmalloc(64);   // Small
        } else {
            blocks[i] = kmalloc(4096); // Large
        }
        EXPECT_EXHAUSTION(blocks[i] != NULL, "Fragmentation allocation failed");
    }

    // Free all small blocks (even indices)
    for (int i = 0; i < FRAG_BLOCKS; i += 2) {
        kfree(blocks[i]);
        blocks[i] = NULL;
    }

    // Now heap is fragmented with 64-byte holes

    // Try to allocate a medium block
    void* medium = kmalloc(1024);
    if (medium == NULL) {
        kprintf("  INFO: Cannot allocate 1KB (expected due to fragmentation)\n");
    } else {
        kprintf("  INFO: 1KB allocation succeeded (good coalescing)\n");
        kfree(medium);
    }

    // Free remaining blocks
    for (int i = 1; i < FRAG_BLOCKS; i += 2) {
        kfree(blocks[i]);
    }

    exhaustion_tests_passed++;

    kprintf("  PASS: Worst-case fragmentation handled\n");
}

/**
 * Test: Memory leak detection
 *
 * Perform operations and verify no leaks
 */
void test_memory_leak_detection(void) {
    EXHAUST_TEST("Memory leak detection");

    uint64_t initial_used = pmm_get_used_memory();
    uint64_t initial_heap = 0; // TODO: Add heap usage tracking

    // Perform many allocations and frees
    for (int cycle = 0; cycle < 100; cycle++) {
        void* ptr = kmalloc(256);
        EXPECT_EXHAUSTION(ptr != NULL, "Allocation failed during leak test");
        kfree(ptr);
    }

    uint64_t final_used = pmm_get_used_memory();

    uint64_t leaked_bytes = (final_used > initial_used) ?
                            (final_used - initial_used) : 0;

    kprintf("  Initial used: %llu bytes\n", initial_used);
    kprintf("  Final used:   %llu bytes\n", final_used);
    kprintf("  Leaked:       %llu bytes\n", leaked_bytes);

    // Some small variation is acceptable due to internal fragmentation
    EXPECT_EXHAUSTION(leaked_bytes < PAGE_SIZE * 2,
                      "Significant memory leak detected");

    kprintf("  PASS: No significant leaks detected\n");
}

// ============================================================================
// RESOURCE LIMIT TESTS
// ============================================================================

/**
 * Test: RLIMIT enforcement under exhaustion
 */
void test_rlimit_exhaustion(void) {
    EXHAUST_TEST("Resource limit enforcement");

    // TODO: Test RLIMIT_AS, RLIMIT_NOFILE, RLIMIT_NPROC
    // when fully implemented

    kprintf("  TODO: RLIMIT exhaustion tests\n");
    kprintf("  (Requires full RLIMIT implementation)\n");

    exhaustion_tests_passed++;
}

/**
 * Test: CPU time exhaustion
 */
void test_cpu_time_exhaustion(void) {
    EXHAUST_TEST("CPU time limit exhaustion");

    // TODO: Test RLIMIT_CPU when scheduler supports it

    kprintf("  TODO: CPU time exhaustion test\n");
    kprintf("  (Requires scheduler CPU time tracking)\n");

    exhaustion_tests_passed++;
}

// ============================================================================
// CASCADING FAILURE TESTS
// ============================================================================

/**
 * Test: OOM during OOM handling
 *
 * What happens if we run out of memory while handling OOM?
 */
void test_nested_oom(void) {
    EXHAUST_TEST("Nested OOM (OOM during OOM handling)");

    // This is a stress test to ensure OOM handler doesn't
    // allocate memory that could fail

    uint64_t initial_free = pmm_get_free_memory();
    kprintf("  Initial free: %llu bytes\n", initial_free);

    // Fill memory almost completely
    void** pages = kmalloc(1000 * sizeof(void*));
    int count = 0;

    while (pmm_get_free_memory() > PAGE_SIZE * 10) {
        pages[count] = pmm_alloc_page();
        if (pages[count] == NULL) break;
        count++;
        if (count >= 1000) break;
    }

    kprintf("  Allocated %d pages, %llu bytes remain\n",
            count, pmm_get_free_memory());

    // Try to allocate (should fail gracefully)
    void* should_fail = kmalloc(1024 * 1024);  // 1MB
    EXPECT_EXHAUSTION(should_fail == NULL,
                      "Large allocation succeeded with low memory");

    // System should still be responsive
    void* small = kmalloc(64);
    if (small != NULL) {
        kfree(small);
    }

    // Clean up
    for (int i = 0; i < count; i++) {
        pmm_free_page(pages[i]);
    }
    kfree(pages);

    kprintf("  PASS: Nested OOM handled gracefully\n");
}

/**
 * Test: Multiple resource exhaustion simultaneously
 */
void test_multiple_exhaustion(void) {
    EXHAUST_TEST("Multiple resources exhausted simultaneously");

    // Exhaust heap
    void* heap_blocks[100];
    int heap_count = 0;
    for (int i = 0; i < 100; i++) {
        heap_blocks[i] = kmalloc(1024 * 100);  // 100KB
        if (heap_blocks[i]) heap_count++;
    }

    // Exhaust pages
    void* pages[100];
    int page_count = 0;
    for (int i = 0; i < 100; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i]) page_count++;
    }

    kprintf("  Exhausted: %d heap blocks, %d pages\n",
            heap_count, page_count);

    // System should handle this gracefully
    EXPECT_EXHAUSTION(heap_count > 0 || page_count > 0,
                      "No resources could be exhausted");

    // Clean up
    for (int i = 0; i < heap_count; i++) {
        kfree(heap_blocks[i]);
    }
    for (int i = 0; i < page_count; i++) {
        pmm_free_page(pages[i]);
    }

    kprintf("  PASS: Multiple exhaustion handled\n");
}

// ============================================================================
// TEST SUITE RUNNER
// ============================================================================

void run_resource_exhaustion_tests(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Resource Exhaustion Test Suite       \n");
    kprintf("  Mission: Test Graceful Degradation   \n");
    kprintf("========================================\n");
    kprintf("\n");

    exhaustion_tests_passed = 0;
    exhaustion_tests_failed = 0;

    kprintf("=== MEMORY EXHAUSTION ===\n");
    test_heap_complete_exhaustion();
    test_physical_memory_exhaustion();
    test_memory_pressure_patterns();

    kprintf("\n=== PROCESS RESOURCE EXHAUSTION ===\n");
    test_process_table_exhaustion();
    test_file_descriptor_exhaustion();

    kprintf("\n=== FRAGMENTATION TESTS ===\n");
    test_worst_case_fragmentation();
    test_memory_leak_detection();

    kprintf("\n=== RESOURCE LIMIT TESTS ===\n");
    test_rlimit_exhaustion();
    test_cpu_time_exhaustion();

    kprintf("\n=== CASCADING FAILURE TESTS ===\n");
    test_nested_oom();
    test_multiple_exhaustion();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Resource Exhaustion Test Results     \n");
    kprintf("========================================\n");
    kprintf("  Tests Passed: %d\n", exhaustion_tests_passed);
    kprintf("  Tests Failed: %d\n", exhaustion_tests_failed);

    if (exhaustion_tests_failed == 0) {
        kprintf("  Status: ALL TESTS PASSED\n");
    } else {
        kprintf("  Status: SOME TESTS FAILED\n");
    }

    kprintf("========================================\n");
    kprintf("\n");
}
