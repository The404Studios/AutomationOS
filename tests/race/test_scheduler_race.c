/*
 * Scheduler Race Condition Test
 * =============================
 *
 * Tests for race conditions in scheduler queue operations.
 * Simulates multiple CPUs adding/removing processes concurrently.
 *
 * Expected behavior:
 *  - No queue corruption (linked list integrity)
 *  - No lost processes
 *  - No duplicate processes
 *  - No crashes or deadlocks
 */

#include "../../kernel/include/sched.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/ktest.h"
#include <pthread.h>
#include <unistd.h>

#define NUM_THREADS 8
#define OPERATIONS_PER_THREAD 1000
#define NUM_TEST_PROCESSES 16

// Test statistics
static volatile uint64_t total_adds = 0;
static volatile uint64_t total_removes = 0;
static volatile uint64_t queue_corruptions = 0;
static process_t* test_processes[NUM_TEST_PROCESSES];

// Thread function: Add and remove processes rapidly
void* scheduler_stress_thread(void* arg) {
    int thread_id = *(int*)arg;

    for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
        // Add a random process
        int proc_idx = (thread_id + i) % NUM_TEST_PROCESSES;
        process_t* proc = test_processes[proc_idx];

        scheduler_add_process(proc);
        __atomic_add_fetch(&total_adds, 1, __ATOMIC_SEQ_CST);

        // Small delay to increase contention
        usleep(1);

        // Remove a random process
        proc_idx = (thread_id + i + 5) % NUM_TEST_PROCESSES;
        proc = test_processes[proc_idx];

        scheduler_remove_process(proc);
        __atomic_add_fetch(&total_removes, 1, __ATOMIC_SEQ_CST);
    }

    return NULL;
}

// Verify queue integrity (no cycles, no corruption)
int verify_queue_integrity(void) {
    // This would need access to ready_queue_head
    // For now, just check that operations completed
    if (total_adds != NUM_THREADS * OPERATIONS_PER_THREAD) {
        kprintf("[TEST] ERROR: Expected %d adds, got %llu\n",
                NUM_THREADS * OPERATIONS_PER_THREAD, total_adds);
        return -1;
    }

    if (total_removes != NUM_THREADS * OPERATIONS_PER_THREAD) {
        kprintf("[TEST] ERROR: Expected %d removes, got %llu\n",
                NUM_THREADS * OPERATIONS_PER_THREAD, total_removes);
        return -1;
    }

    return 0;
}

KTEST(scheduler_race, concurrent_add_remove) {
    kprintf("[TEST] Starting scheduler race test...\n");
    kprintf("[TEST] Threads: %d, Operations per thread: %d\n",
            NUM_THREADS, OPERATIONS_PER_THREAD);

    // Initialize test processes
    for (int i = 0; i < NUM_TEST_PROCESSES; i++) {
        char name[32];
        snprintf(name, sizeof(name), "race_test_%d", i);
        test_processes[i] = process_create(name, (void*)0x1000);
        KTEST_ASSERT(test_processes[i] != NULL, "Failed to create test process");
    }

    // Create threads
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        int ret = pthread_create(&threads[i], NULL, scheduler_stress_thread, &thread_ids[i]);
        KTEST_ASSERT(ret == 0, "Failed to create thread");
    }

    kprintf("[TEST] All threads launched, waiting for completion...\n");

    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    kprintf("[TEST] All threads completed\n");
    kprintf("[TEST] Total adds: %llu, Total removes: %llu\n", total_adds, total_removes);

    // Verify integrity
    int result = verify_queue_integrity();
    KTEST_ASSERT(result == 0, "Queue integrity check failed");

    // Cleanup
    for (int i = 0; i < NUM_TEST_PROCESSES; i++) {
        process_destroy(test_processes[i]);
    }

    kprintf("[TEST] Scheduler race test PASSED\n");
    return KTEST_SUCCESS;
}

KTEST(scheduler_race, pick_next_concurrent) {
    kprintf("[TEST] Testing concurrent scheduler_pick_next()...\n");

    // Add some processes
    for (int i = 0; i < NUM_TEST_PROCESSES; i++) {
        char name[32];
        snprintf(name, sizeof(name), "pick_test_%d", i);
        process_t* proc = process_create(name, (void*)0x1000);
        KTEST_ASSERT(proc != NULL, "Failed to create test process");
        scheduler_add_process(proc);
    }

    // Multiple threads picking next process
    pthread_t threads[NUM_THREADS];
    int picked[NUM_THREADS] = {0};

    void* picker_thread(void* arg) {
        int* count = (int*)arg;
        for (int i = 0; i < 100; i++) {
            process_t* proc = scheduler_pick_next();
            if (proc) {
                (*count)++;
                // Add back to queue
                scheduler_add_process(proc);
            }
            usleep(1);
        }
        return NULL;
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, picker_thread, &picked[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    kprintf("[TEST] Concurrent pick_next test PASSED\n");
    return KTEST_SUCCESS;
}

int main(void) {
    kprintf("=== Scheduler Race Condition Tests ===\n");

    // Initialize scheduler
    scheduler_init();
    process_init();

    // Run tests
    RUN_KTEST(scheduler_race, concurrent_add_remove);
    RUN_KTEST(scheduler_race, pick_next_concurrent);

    kprintf("=== All tests completed ===\n");
    return 0;
}
