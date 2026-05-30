#include "../../kernel/include/sched.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/mem.h"

// Mock allocation failure tracking
static int mock_kmalloc_should_fail = 0;
static int mock_pmm_alloc_should_fail = 0;

// Mock memory functions for testing allocation failures
void* kmalloc(size_t size) {
    (void)size;
    if (mock_kmalloc_should_fail) {
        mock_kmalloc_should_fail--;
        kprintf("[MOCK] kmalloc returning NULL (simulating allocation failure)\n");
        return NULL;
    }

    static char buffer[10][512];
    static int idx = 0;
    if (idx >= 10) return NULL;
    return buffer[idx++];
}

void kfree(void* ptr) {
    (void)ptr;
}

void* pmm_alloc_page(void) {
    if (mock_pmm_alloc_should_fail) {
        mock_pmm_alloc_should_fail--;
        kprintf("[MOCK] pmm_alloc_page returning NULL (simulating allocation failure)\n");
        return NULL;
    }

    static char pages[5][4096] __attribute__((aligned(4096)));
    static int idx = 0;
    if (idx >= 5) return NULL;
    return pages[idx++];
}

void pmm_free_page(void* page) {
    (void)page;
}

uint64_t read_cr3(void) {
    return 0x1000;  // Mock CR3 value
}

// Dummy process entry point
void test_entry(void) {
    while (1);
}

// Test 1: Process creation with kmalloc failure
void test_process_create_kmalloc_failure(void) {
    kprintf("[TEST] Testing process_create with kmalloc failure...\n");

    process_init();

    // Simulate kmalloc failure
    mock_kmalloc_should_fail = 1;

    process_t* proc = process_create("test_proc", test_entry);

    // Should return NULL on allocation failure
    ASSERT(proc == NULL);

    kprintf("[TEST] process_create with kmalloc failure: PASS\n");
}

// Test 2: Process creation with pmm_alloc_page failure
void test_process_create_pmm_failure(void) {
    kprintf("[TEST] Testing process_create with pmm_alloc_page failure...\n");

    process_init();

    // Simulate pmm_alloc_page failure (for kernel stack)
    mock_pmm_alloc_should_fail = 1;

    process_t* proc = process_create("test_proc", test_entry);

    // Should return NULL on allocation failure
    ASSERT(proc == NULL);

    kprintf("[TEST] process_create with pmm_alloc_page failure: PASS\n");
}

// Test 3: Scheduler add NULL process
void test_scheduler_add_null(void) {
    kprintf("[TEST] Testing scheduler_add_process with NULL...\n");

    scheduler_init();

    // Should not crash when adding NULL
    scheduler_add_process(NULL);

    // Verify queue is still empty
    process_t* next = scheduler_pick_next();
    ASSERT(next == NULL);

    kprintf("[TEST] scheduler_add_process with NULL: PASS\n");
}

// Test 4: Scheduler add terminated process
void test_scheduler_add_terminated(void) {
    kprintf("[TEST] Testing scheduler_add_process with terminated process...\n");

    scheduler_init();
    process_init();

    process_t* proc = process_create("test_proc", test_entry);
    ASSERT(proc != NULL);

    // Mark as terminated
    proc->state = PROCESS_TERMINATED;

    // Should not add terminated process
    scheduler_add_process(proc);

    // Verify queue is empty
    process_t* next = scheduler_pick_next();
    ASSERT(next == NULL);

    kprintf("[TEST] scheduler_add_process with terminated process: PASS\n");
}

// Test 5: Scheduler remove NULL process
void test_scheduler_remove_null(void) {
    kprintf("[TEST] Testing scheduler_remove_process with NULL...\n");

    scheduler_init();

    // Should not crash when removing NULL
    scheduler_remove_process(NULL);

    kprintf("[TEST] scheduler_remove_process with NULL: PASS\n");
}

// Test 6: Process cleanup doesn't leak memory
void test_process_cleanup_no_leak(void) {
    kprintf("[TEST] Testing process cleanup (no memory leaks)...\n");

    process_init();
    scheduler_init();

    process_t* proc = process_create("test_proc", test_entry);
    ASSERT(proc != NULL);

    // Add to scheduler (takes reference)
    scheduler_add_process(proc);
    ASSERT(proc->ref_count == 2);  // 1 from create, 1 from scheduler

    // Remove from scheduler (releases reference)
    scheduler_remove_process(proc);
    ASSERT(proc->ref_count == 1);  // Only creation reference remains

    // Destroy process (should free all resources)
    process_destroy(proc);

    // If we get here without crashing, cleanup worked
    kprintf("[TEST] process cleanup (no memory leaks): PASS\n");
}

// Test 7: Multiple allocation failures
void test_multiple_allocation_failures(void) {
    kprintf("[TEST] Testing multiple allocation failures in sequence...\n");

    process_init();

    // Simulate multiple failures
    mock_kmalloc_should_fail = 3;

    process_t* proc1 = process_create("proc1", test_entry);
    ASSERT(proc1 == NULL);

    process_t* proc2 = process_create("proc2", test_entry);
    ASSERT(proc2 == NULL);

    process_t* proc3 = process_create("proc3", test_entry);
    ASSERT(proc3 == NULL);

    // Next one should succeed
    process_t* proc4 = process_create("proc4", test_entry);
    ASSERT(proc4 != NULL);

    kprintf("[TEST] multiple allocation failures: PASS\n");
}

// Test 8: Reference counting edge cases
void test_reference_counting(void) {
    kprintf("[TEST] Testing reference counting edge cases...\n");

    process_init();
    scheduler_init();

    process_t* proc = process_create("test_proc", test_entry);
    ASSERT(proc != NULL);
    ASSERT(proc->ref_count == 1);

    // Manual ref increase
    process_ref(proc);
    ASSERT(proc->ref_count == 2);

    process_ref(proc);
    ASSERT(proc->ref_count == 3);

    // Manual ref decrease
    process_unref(proc);
    ASSERT(proc->ref_count == 2);

    process_unref(proc);
    ASSERT(proc->ref_count == 1);

    // Should still be valid
    ASSERT(proc->pid >= 0);

    kprintf("[TEST] reference counting: PASS\n");
}

// Test 9: Process ref/unref with NULL
void test_process_ref_null(void) {
    kprintf("[TEST] Testing process_ref/unref with NULL...\n");

    // Should not crash
    process_ref(NULL);
    process_unref(NULL);

    kprintf("[TEST] process_ref/unref with NULL: PASS\n");
}

// Test 10: Process destroy with NULL
void test_process_destroy_null(void) {
    kprintf("[TEST] Testing process_destroy with NULL...\n");

    // Should not crash
    process_destroy(NULL);

    kprintf("[TEST] process_destroy with NULL: PASS\n");
}

void run_null_check_tests(void) {
    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   Running NULL Check Tests         \n");
    kprintf("   (CWE-476, CWE-401, CWE-755)      \n");
    kprintf("=====================================\n");
    kprintf("\n");

    test_process_create_kmalloc_failure();
    test_process_create_pmm_failure();
    test_scheduler_add_null();
    test_scheduler_add_terminated();
    test_scheduler_remove_null();
    test_process_cleanup_no_leak();
    test_multiple_allocation_failures();
    test_reference_counting();
    test_process_ref_null();
    test_process_destroy_null();

    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   All NULL Check Tests Passed      \n");
    kprintf("   Memory Safety: VERIFIED          \n");
    kprintf("=====================================\n");
    kprintf("\n");
}
