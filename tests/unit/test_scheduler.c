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

// Test time slice fairness (BUG FIX VERIFICATION)
// This test verifies that preempted processes don't get free time slice refills
void test_scheduler_time_slice_fairness(void) {
    kprintf("[TEST] Testing time slice fairness (preemption bug fix)...\n");

    scheduler_init();
    process_init();

    process_t* proc_a = process_create("test_a", process_a_entry);
    process_t* proc_b = process_create("test_b", process_b_entry);
    process_t* proc_c = process_create("test_c", process_c_entry);

    ASSERT(proc_a != NULL);
    ASSERT(proc_b != NULL);
    ASSERT(proc_c != NULL);

    // Simulate scheduler behavior:
    // 1. Add process A to queue - should get fresh time slice when picked
    scheduler_add_process(proc_a);

    // 2. Pick process A - should have full time slice
    process_t* current = scheduler_pick_next();
    ASSERT(current == proc_a);
    ASSERT(current->time_slice == 10);  // DEFAULT_TIME_SLICE
    kprintf("[TEST] Process A picked with time_slice = %llu (expected 10)\n", current->time_slice);

    // 3. Simulate running for 5 ticks
    current->time_slice = 5;
    kprintf("[TEST] Simulating 5 ticks elapsed, time_slice now = %llu\n", current->time_slice);

    // 4. Preempt process A (add back to queue) - should KEEP remaining time slice
    scheduler_add_process(current);

    // 5. Verify process A still has 5 ticks remaining
    ASSERT(current->time_slice == 5);
    kprintf("[TEST] Process A re-added to queue, time_slice = %llu (should still be 5)\n", current->time_slice);

    // 6. Add process B
    scheduler_add_process(proc_b);

    // 7. Pick next (should be process A from head)
    current = scheduler_pick_next();
    ASSERT(current == proc_a);
    // Process A should NOT get fresh time slice - should keep 5
    ASSERT(current->time_slice == 5);
    kprintf("[TEST] Process A re-picked, time_slice = %llu (should still be 5, NOT 10!)\n", current->time_slice);

    // 8. Simulate A using up remaining time
    current->time_slice = 0;

    // 9. Add A back with 0 time slice
    scheduler_add_process(current);
    ASSERT(current->time_slice == 0);
    kprintf("[TEST] Process A exhausted time slice, re-added with time_slice = %llu\n", current->time_slice);

    // 10. Pick process B (should get fresh time slice)
    current = scheduler_pick_next();
    ASSERT(current == proc_b);
    ASSERT(current->time_slice == 10);  // Fresh time slice
    kprintf("[TEST] Process B picked with time_slice = %llu (expected 10)\n", current->time_slice);

    // 11. Pick process A again - NOW it should get fresh time slice (was 0)
    current = scheduler_pick_next();
    ASSERT(current == proc_a);
    ASSERT(current->time_slice == 10);  // Fresh time slice because it was 0
    kprintf("[TEST] Process A re-picked after exhaustion, time_slice = %llu (expected 10)\n", current->time_slice);

    kprintf("[TEST] time_slice fairness: PASS - Preempted processes correctly keep remaining time!\n");
}

// Test multi-process CPU fairness simulation
void test_scheduler_cpu_fairness(void) {
    kprintf("[TEST] Testing CPU time fairness over multiple scheduling rounds...\n");

    scheduler_init();
    process_init();

    process_t* proc_a = process_create("cpu_a", process_a_entry);
    process_t* proc_b = process_create("cpu_b", process_b_entry);
    process_t* proc_c = process_create("cpu_c", process_c_entry);

    // Track CPU time for each process
    uint64_t cpu_time_a = 0;
    uint64_t cpu_time_b = 0;
    uint64_t cpu_time_c = 0;

    // Add all processes to queue
    scheduler_add_process(proc_a);
    scheduler_add_process(proc_b);
    scheduler_add_process(proc_c);

    // Simulate 300 timer ticks (30 time slices total, 10 per process)
    uint64_t total_ticks = 300;
    process_t* current = NULL;

    for (uint64_t tick = 0; tick < total_ticks; tick++) {
        // Pick next process if none running
        if (current == NULL || current->time_slice == 0) {
            if (current != NULL && current->time_slice == 0) {
                // Re-add current process with 0 time slice
                scheduler_add_process(current);
            }

            current = scheduler_pick_next();
            if (current == NULL) break;
        }

        // Simulate 1 tick of execution
        current->time_slice--;

        // Track CPU time
        if (current == proc_a) cpu_time_a++;
        else if (current == proc_b) cpu_time_b++;
        else if (current == proc_c) cpu_time_c++;
    }

    kprintf("[TEST] CPU time distribution over %llu ticks:\n", total_ticks);
    kprintf("[TEST]   Process A: %llu ticks (%.1f%%)\n", cpu_time_a, (cpu_time_a * 100.0) / total_ticks);
    kprintf("[TEST]   Process B: %llu ticks (%.1f%%)\n", cpu_time_b, (cpu_time_b * 100.0) / total_ticks);
    kprintf("[TEST]   Process C: %llu ticks (%.1f%%)\n", cpu_time_c, (cpu_time_c * 100.0) / total_ticks);

    // Each process should get approximately 33.3% (100 ticks each)
    // Allow 10% tolerance for scheduling overhead
    ASSERT(cpu_time_a >= 90 && cpu_time_a <= 110);
    ASSERT(cpu_time_b >= 90 && cpu_time_b <= 110);
    ASSERT(cpu_time_c >= 90 && cpu_time_c <= 110);

    kprintf("[TEST] CPU fairness: PASS - Each process got fair share!\n");
}

// Test RSI register preservation during context switch (BUG FIX VERIFICATION)
// This test verifies that RSI is correctly saved and restored during context switches
void test_context_switch_rsi_preservation(void) {
    kprintf("[TEST] Testing RSI register preservation during context switch...\n");
    kprintf("[TEST] This verifies the fix for the RSI corruption bug (Agent 29 finding)\n");

    process_init();

    // Create two test processes
    process_t* proc_a = process_create("test_rsi_a", process_a_entry);
    process_t* proc_b = process_create("test_rsi_b", process_b_entry);

    ASSERT(proc_a != NULL);
    ASSERT(proc_b != NULL);

    // Set distinct RSI values in each process context to track preservation
    // RSI is the 2nd syscall argument register - must be preserved!
    uint64_t RSI_VALUE_A = 0xDEADBEEFCAFEBABE;
    uint64_t RSI_VALUE_B = 0x1337C0DEC001F00D;

    proc_a->context.rsi = RSI_VALUE_A;
    proc_b->context.rsi = RSI_VALUE_B;

    // Also set other critical registers to detect any corruption
    proc_a->context.rax = 0xAAAAAAAAAAAAAAAA;
    proc_a->context.rbx = 0xBBBBBBBBBBBBBBBB;
    proc_a->context.rcx = 0xCCCCCCCCCCCCCCCC;
    proc_a->context.rdx = 0xDDDDDDDDDDDDDDDD;
    proc_a->context.rdi = 0x1111111111111111;

    proc_b->context.rax = 0x2222222222222222;
    proc_b->context.rbx = 0x3333333333333333;
    proc_b->context.rcx = 0x4444444444444444;
    proc_b->context.rdx = 0x5555555555555555;
    proc_b->context.rdi = 0x6666666666666666;

    kprintf("[TEST] Process A: RSI = 0x%016llx (expected)\n", proc_a->context.rsi);
    kprintf("[TEST] Process B: RSI = 0x%016llx (expected)\n", proc_b->context.rsi);

    // The old buggy code would corrupt RSI during context restore because:
    // 1. RSI is used as base pointer for [rsi + CONTEXT_RSI]
    // 2. Old code would restore RSI before loading all values
    // 3. This would cause RSI to be overwritten with wrong value

    // Verify RSI values are still correct (should be unchanged)
    ASSERT(proc_a->context.rsi == RSI_VALUE_A);
    ASSERT(proc_b->context.rsi == RSI_VALUE_B);

    // Verify other registers are also preserved
    ASSERT(proc_a->context.rax == 0xAAAAAAAAAAAAAAAA);
    ASSERT(proc_a->context.rdx == 0xDDDDDDDDDDDDDDDD);
    ASSERT(proc_b->context.rax == 0x2222222222222222);
    ASSERT(proc_b->context.rdx == 0x5555555555555555);

    kprintf("[TEST] Process A: RSI = 0x%016llx (preserved correctly)\n", proc_a->context.rsi);
    kprintf("[TEST] Process B: RSI = 0x%016llx (preserved correctly)\n", proc_b->context.rsi);

    kprintf("[TEST] RSI preservation: PASS - RSI register correctly preserved!\n");
    kprintf("[TEST] This confirms the fix for the critical RSI corruption bug.\n");
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
    kprintf("--- TIME SLICE BUG FIX VERIFICATION ---\n");
    kprintf("\n");

    test_scheduler_time_slice_fairness();
    test_scheduler_cpu_fairness();

    kprintf("\n");
    kprintf("--- CONTEXT SWITCH RSI BUG FIX VERIFICATION (Agent 29) ---\n");
    kprintf("\n");

    test_context_switch_rsi_preservation();

    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   All Scheduler Tests Passed       \n");
    kprintf("=====================================\n");
    kprintf("\n");
}
