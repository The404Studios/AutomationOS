/*
 * Process Table Race Condition Test
 * ==================================
 *
 * Tests for race conditions in process table operations.
 * Simulates multiple CPUs creating/destroying processes concurrently.
 */

#include "../../kernel/include/sched.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/ktest.h"
#include <pthread.h>
#include <unistd.h>

#define NUM_THREADS 8
#define OPERATIONS_PER_THREAD 100
#define MAX_PID_SEEN 256

// Test statistics
static volatile uint64_t creates = 0;
static volatile uint64_t destroys = 0;
static volatile uint64_t lookups = 0;
static volatile uint64_t duplicate_pids = 0;
static volatile uint32_t pid_seen[MAX_PID_SEEN] = {0};

// Thread function: Create and destroy processes
void* process_create_destroy_thread(void* arg) {
    int thread_id = *(int*)arg;

    for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
        char name[32];
        snprintf(name, sizeof(name), "thread%d_proc%d", thread_id, i);

        // Create process
        process_t* proc = process_create(name, (void*)0x1000);
        if (!proc) {
            kprintf("[THREAD %d] Failed to create process\n", thread_id);
            continue;
        }

        uint32_t pid = proc->pid;
        __atomic_add_fetch(&creates, 1, __ATOMIC_SEQ_CST);

        // Check for duplicate PID
        uint32_t old_val = __atomic_fetch_add(&pid_seen[pid], 1, __ATOMIC_SEQ_CST);
        if (old_val > 0) {
            kprintf("[THREAD %d] ERROR: Duplicate PID %d detected!\n", thread_id, pid);
            __atomic_add_fetch(&duplicate_pids, 1, __ATOMIC_SEQ_CST);
        }

        // Keep process alive for a bit
        usleep(10);

        // Lookup the process
        process_t* found = process_get_by_pid(pid);
        if (found != proc) {
            kprintf("[THREAD %d] ERROR: Lookup mismatch for PID %d\n", thread_id, pid);
        } else {
            __atomic_add_fetch(&lookups, 1, __ATOMIC_SEQ_CST);
            // If process_get_by_pid takes reference, release it
            if (found) {
                process_unref(found);
            }
        }

        // Destroy process
        process_destroy(proc);
        __atomic_add_fetch(&destroys, 1, __ATOMIC_SEQ_CST);

        // Clear PID tracking
        __atomic_sub_fetch(&pid_seen[pid], 1, __ATOMIC_SEQ_CST);
    }

    return NULL;
}

KTEST(process_table_race, concurrent_create_destroy) {
    kprintf("[TEST] Starting process table race test...\n");
    kprintf("[TEST] Threads: %d, Operations per thread: %d\n",
            NUM_THREADS, OPERATIONS_PER_THREAD);

    creates = 0;
    destroys = 0;
    lookups = 0;
    duplicate_pids = 0;

    // Create threads
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        int ret = pthread_create(&threads[i], NULL,
                                process_create_destroy_thread, &thread_ids[i]);
        KTEST_ASSERT(ret == 0, "Failed to create thread");
    }

    kprintf("[TEST] All threads launched, waiting for completion...\n");

    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    kprintf("[TEST] All threads completed\n");
    kprintf("[TEST] Creates: %llu, Destroys: %llu, Lookups: %llu\n",
            creates, destroys, lookups);
    kprintf("[TEST] Duplicate PIDs detected: %llu\n", duplicate_pids);

    // Verify no duplicate PIDs
    KTEST_ASSERT(duplicate_pids == 0, "Duplicate PIDs detected!");

    // Verify all creates had matching destroys
    KTEST_ASSERT(creates == destroys, "Create/destroy count mismatch");

    kprintf("[TEST] Process table race test PASSED\n");
    return KTEST_SUCCESS;
}

// Thread function: Concurrent lookups
void* process_lookup_thread(void* arg) {
    int thread_id = *(int*)arg;

    for (int i = 0; i < 1000; i++) {
        // Try to lookup various PIDs
        for (uint32_t pid = 1; pid < 50; pid++) {
            process_t* proc = process_get_by_pid(pid);
            if (proc) {
                // Verify process is valid
                if (proc->pid != pid) {
                    kprintf("[THREAD %d] ERROR: PID mismatch! Expected %d, got %d\n",
                            thread_id, pid, proc->pid);
                }
                // Release reference if taken
                process_unref(proc);
            }
        }
    }

    return NULL;
}

KTEST(process_table_race, concurrent_lookup) {
    kprintf("[TEST] Testing concurrent process lookups...\n");

    // Create some processes
    #define NUM_PROCS 20
    process_t* procs[NUM_PROCS];
    for (int i = 0; i < NUM_PROCS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "lookup_test_%d", i);
        procs[i] = process_create(name, (void*)0x1000);
        KTEST_ASSERT(procs[i] != NULL, "Failed to create test process");
    }

    // Launch lookup threads
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, process_lookup_thread, &thread_ids[i]);
    }

    // Wait for threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Cleanup
    for (int i = 0; i < NUM_PROCS; i++) {
        process_destroy(procs[i]);
    }

    kprintf("[TEST] Concurrent lookup test PASSED\n");
    return KTEST_SUCCESS;
}

int main(void) {
    kprintf("=== Process Table Race Condition Tests ===\n");

    // Initialize
    process_init();
    scheduler_init();

    // Run tests
    RUN_KTEST(process_table_race, concurrent_create_destroy);
    RUN_KTEST(process_table_race, concurrent_lookup);

    kprintf("=== All tests completed ===\n");
    return 0;
}
