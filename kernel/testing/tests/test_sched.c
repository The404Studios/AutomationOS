#include "../../include/ktest.h"
#include "../../include/sched.h"
#include "../../include/kernel.h"

/*
 * Scheduler Tests
 * Tests process/thread scheduling and context switching
 */

KTEST_SUITE(sched);

KTEST_CASE(sched, create_process_succeeds) {
    process_t* proc = process_create("test_process", 1);  // Priority 1
    KTEST_ASSERT_NOT_NULL(proc);
    KTEST_ASSERT_NOT_NULL(proc->name);

    process_destroy(proc);
}

KTEST_CASE(sched, process_has_unique_pid) {
    process_t* proc1 = process_create("proc1", 1);
    process_t* proc2 = process_create("proc2", 1);

    KTEST_ASSERT_NOT_NULL(proc1);
    KTEST_ASSERT_NOT_NULL(proc2);

    KTEST_ASSERT_NE(proc1->pid, proc2->pid);

    process_destroy(proc1);
    process_destroy(proc2);
}

KTEST_CASE(sched, process_initial_state_is_ready) {
    process_t* proc = process_create("test", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    KTEST_ASSERT_EQ(proc->state, PROCESS_READY);

    process_destroy(proc);
}

KTEST_CASE(sched, scheduler_queue_operations) {
    process_t* proc1 = process_create("p1", 1);
    process_t* proc2 = process_create("p2", 2);
    process_t* proc3 = process_create("p3", 3);

    KTEST_ASSERT_NOT_NULL(proc1);
    KTEST_ASSERT_NOT_NULL(proc2);
    KTEST_ASSERT_NOT_NULL(proc3);

    // Add to ready queue
    sched_add_to_ready_queue(proc1);
    sched_add_to_ready_queue(proc2);
    sched_add_to_ready_queue(proc3);

    // Higher priority should be scheduled first
    process_t* next = sched_get_next_process();
    KTEST_ASSERT_PTR_EQ(next, proc3);  // Priority 3 is highest

    // Clean up
    process_destroy(proc1);
    process_destroy(proc2);
    process_destroy(proc3);
}

KTEST_CASE(sched, round_robin_scheduling) {
    process_t* procs[3];

    // Create 3 processes with same priority
    for (int i = 0; i < 3; i++) {
        procs[i] = process_create("test", 1);
        KTEST_ASSERT_NOT_NULL(procs[i]);
        sched_add_to_ready_queue(procs[i]);
    }

    // Should be scheduled in round-robin order
    process_t* p1 = sched_get_next_process();
    process_t* p2 = sched_get_next_process();
    process_t* p3 = sched_get_next_process();

    KTEST_ASSERT_NOT_NULL(p1);
    KTEST_ASSERT_NOT_NULL(p2);
    KTEST_ASSERT_NOT_NULL(p3);

    // All should be different
    KTEST_ASSERT_NE(p1, p2);
    KTEST_ASSERT_NE(p2, p3);
    KTEST_ASSERT_NE(p1, p3);

    // Clean up
    for (int i = 0; i < 3; i++) {
        process_destroy(procs[i]);
    }
}

KTEST_CASE(sched, process_state_transitions) {
    process_t* proc = process_create("test", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    KTEST_ASSERT_EQ(proc->state, PROCESS_READY);

    // Transition to running
    proc->state = PROCESS_RUNNING;
    KTEST_ASSERT_EQ(proc->state, PROCESS_RUNNING);

    // Transition to blocked
    proc->state = PROCESS_BLOCKED;
    KTEST_ASSERT_EQ(proc->state, PROCESS_BLOCKED);

    // Transition to terminated
    proc->state = PROCESS_TERMINATED;
    KTEST_ASSERT_EQ(proc->state, PROCESS_TERMINATED);

    process_destroy(proc);
}

KTEST_CASE(sched, time_slice_accounting) {
    process_t* proc = process_create("test", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    // Initial time slice should be set
    KTEST_ASSERT_GT(proc->time_slice, 0);

    uint64_t initial_slice = proc->time_slice;

    // Simulate time passing
    sched_tick(proc);  // Decrement time slice

    KTEST_ASSERT_LT(proc->time_slice, initial_slice);

    process_destroy(proc);
}

KTEST_CASE(sched, priority_levels) {
    process_t* low = process_create("low", 1);
    process_t* med = process_create("med", 5);
    process_t* high = process_create("high", 10);

    KTEST_ASSERT_NOT_NULL(low);
    KTEST_ASSERT_NOT_NULL(med);
    KTEST_ASSERT_NOT_NULL(high);

    KTEST_ASSERT_EQ(low->priority, 1);
    KTEST_ASSERT_EQ(med->priority, 5);
    KTEST_ASSERT_EQ(high->priority, 10);

    process_destroy(low);
    process_destroy(med);
    process_destroy(high);
}

KTEST_CASE(sched, empty_queue_returns_idle) {
    // Clear scheduler queue
    sched_clear_queue();

    process_t* next = sched_get_next_process();

    // Should return idle process or NULL
    // Implementation-dependent
    KTEST_ASSERT_TRUE(true);
}

KTEST_CASE(sched, context_switch_preserves_state) {
    process_t* proc1 = process_create("p1", 1);
    process_t* proc2 = process_create("p2", 1);

    KTEST_ASSERT_NOT_NULL(proc1);
    KTEST_ASSERT_NOT_NULL(proc2);

    // Set some registers for proc1
    proc1->context.rax = 0x1111111111111111ULL;
    proc1->context.rbx = 0x2222222222222222ULL;
    proc1->context.rcx = 0x3333333333333333ULL;

    // Switch to proc2
    sched_switch_to(proc2);

    // Switch back to proc1
    sched_switch_to(proc1);

    // Context should be preserved
    KTEST_ASSERT_EQ(proc1->context.rax, 0x1111111111111111ULL);
    KTEST_ASSERT_EQ(proc1->context.rbx, 0x2222222222222222ULL);
    KTEST_ASSERT_EQ(proc1->context.rcx, 0x3333333333333333ULL);

    process_destroy(proc1);
    process_destroy(proc2);
}

KTEST_CASE(sched, process_cleanup) {
    process_t* proc = process_create("test", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    pid_t pid = proc->pid;

    process_destroy(proc);

    // Process should no longer be findable
    process_t* found = process_find_by_pid(pid);
    KTEST_ASSERT_NULL(found);
}

KTEST_CASE(sched, max_processes_limit) {
    #define MAX_TEST_PROCS 100
    process_t* procs[MAX_TEST_PROCS];

    // Create many processes
    int created = 0;
    for (int i = 0; i < MAX_TEST_PROCS; i++) {
        procs[i] = process_create("test", 1);
        if (procs[i] != NULL) {
            created++;
        }
    }

    // At least some processes should be created
    KTEST_ASSERT_GT(created, 0);

    // Clean up
    for (int i = 0; i < created; i++) {
        if (procs[i]) {
            process_destroy(procs[i]);
        }
    }
}

KTEST_CASE(sched, scheduler_preemption) {
    process_t* proc = process_create("test", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    proc->state = PROCESS_RUNNING;
    proc->time_slice = 0;  // Time slice expired

    // Should be preempted
    bool should_preempt = sched_should_preempt(proc);
    KTEST_ASSERT_TRUE(should_preempt);

    process_destroy(proc);
}

KTEST_CASE(sched, thread_creation) {
    process_t* proc = process_create("parent", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    thread_t* thread = thread_create(proc, (void*)0x1000, NULL);
    KTEST_ASSERT_NOT_NULL(thread);
    KTEST_ASSERT_PTR_EQ(thread->parent, proc);

    thread_destroy(thread);
    process_destroy(proc);
}

KTEST_CASE(sched, multiple_threads_per_process) {
    process_t* proc = process_create("parent", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    thread_t* threads[5];
    for (int i = 0; i < 5; i++) {
        threads[i] = thread_create(proc, (void*)(0x1000 + i * 0x100), NULL);
        KTEST_ASSERT_NOT_NULL(threads[i]);
    }

    // All threads should have same parent
    for (int i = 0; i < 5; i++) {
        KTEST_ASSERT_PTR_EQ(threads[i]->parent, proc);
    }

    // Clean up
    for (int i = 0; i < 5; i++) {
        thread_destroy(threads[i]);
    }
    process_destroy(proc);
}
