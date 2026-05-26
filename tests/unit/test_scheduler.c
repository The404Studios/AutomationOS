#include "../../kernel/include/sched.h"
#include "../../kernel/include/kernel.h"

// Mock memory functions for testing
void* test_mem[10];
int test_mem_idx = 0;

void* kmalloc(size_t size) {
    (void)size;
    if (test_mem_idx >= 10) return NULL;
    static char buffer[10][512];
    test_mem[test_mem_idx] = buffer[test_mem_idx];
    return test_mem[test_mem_idx++];
}

void kfree(void* ptr) {
    (void)ptr;
    // No-op for testing
}

void* pmm_alloc_page(void) {
    static char page[4096];
    return page;
}

void pmm_free_page(void* page) {
    (void)page;
}

uint64_t read_cr3(void) {
    return 0;
}

// Dummy process entry points
void process_a_entry(void) {
    while (1);
}

void process_b_entry(void) {
    while (1);
}

void process_c_entry(void) {
    while (1);
}

// Test scheduler initialization
void test_scheduler_init(void) {
    kprintf("[TEST] Testing scheduler_init...\n");

    scheduler_init();

    // Verify scheduler is initialized (no processes)
    process_t* next = scheduler_pick_next();
    ASSERT(next == NULL);

    kprintf("[TEST] scheduler_init: PASS\n");
}

// Test adding processes to ready queue
void test_scheduler_add_process(void) {
    kprintf("[TEST] Testing scheduler_add_process...\n");

    scheduler_init();
    process_init();

    process_t* proc_a = process_create("test_a", process_a_entry);
    process_t* proc_b = process_create("test_b", process_b_entry);

    ASSERT(proc_a != NULL);
    ASSERT(proc_b != NULL);

    scheduler_add_process(proc_a);
    scheduler_add_process(proc_b);

    // Verify both processes are in queue
    process_t* next1 = scheduler_pick_next();
    ASSERT(next1 == proc_a);

    process_t* next2 = scheduler_pick_next();
    ASSERT(next2 == proc_b);

    // Queue should be empty now
    process_t* next3 = scheduler_pick_next();
    ASSERT(next3 == NULL);

    kprintf("[TEST] scheduler_add_process: PASS\n");
}

// Test round-robin scheduling
void test_scheduler_round_robin(void) {
    kprintf("[TEST] Testing round-robin scheduling...\n");

    scheduler_init();
    process_init();

    process_t* proc_a = process_create("test_a", process_a_entry);
    process_t* proc_b = process_create("test_b", process_b_entry);
    process_t* proc_c = process_create("test_c", process_c_entry);

    ASSERT(proc_a != NULL);
    ASSERT(proc_b != NULL);
    ASSERT(proc_c != NULL);

    // Add all to queue
    scheduler_add_process(proc_a);
    scheduler_add_process(proc_b);
    scheduler_add_process(proc_c);

    // Pick should follow round-robin order
    process_t* next1 = scheduler_pick_next();
    ASSERT(next1 == proc_a);

    process_t* next2 = scheduler_pick_next();
    ASSERT(next2 == proc_b);

    process_t* next3 = scheduler_pick_next();
    ASSERT(next3 == proc_c);

    kprintf("[TEST] round-robin scheduling: PASS\n");
}

// Test removing process from queue
void test_scheduler_remove_process(void) {
    kprintf("[TEST] Testing scheduler_remove_process...\n");

    scheduler_init();
    process_init();

    process_t* proc_a = process_create("test_a", process_a_entry);
    process_t* proc_b = process_create("test_b", process_b_entry);
    process_t* proc_c = process_create("test_c", process_c_entry);

    scheduler_add_process(proc_a);
    scheduler_add_process(proc_b);
    scheduler_add_process(proc_c);

    // Remove middle process
    scheduler_remove_process(proc_b);

    // Should get a then c
    process_t* next1 = scheduler_pick_next();
    ASSERT(next1 == proc_a);

    process_t* next2 = scheduler_pick_next();
    ASSERT(next2 == proc_c);

    // Queue empty
    process_t* next3 = scheduler_pick_next();
    ASSERT(next3 == NULL);

    kprintf("[TEST] scheduler_remove_process: PASS\n");
}

void run_scheduler_tests(void) {
    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   Running Scheduler Tests          \n");
    kprintf("=====================================\n");
    kprintf("\n");

    test_scheduler_init();
    test_scheduler_add_process();
    test_scheduler_round_robin();
    test_scheduler_remove_process();

    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   All Scheduler Tests Passed       \n");
    kprintf("=====================================\n");
    kprintf("\n");
}
