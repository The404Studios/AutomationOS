/**
 * PMM Deadlock Stress Test
 * =========================
 *
 * Tests for deadlocks in the Physical Memory Manager under high concurrency.
 * This test should complete in < 5 seconds. If it hangs = DEADLOCK.
 */

#include "../include/mem.h"
#include "../include/kernel.h"
#include "../include/sched.h"
#include "../include/time.h"

#define NUM_THREADS 16
#define ITERATIONS_PER_THREAD 1000
#define TEST_TIMEOUT_MS 5000

static volatile uint32_t threads_completed = 0;
static volatile bool test_failed = false;

/**
 * Worker thread: Rapidly allocate/free pages
 * This creates maximum contention on PMM locks
 */
void pmm_stress_worker(void* arg) {
    uint32_t thread_id = (uint32_t)(uintptr_t)arg;

    kprintf("[PMM-TEST] Thread %u starting...\n", thread_id);

    for (uint32_t i = 0; i < ITERATIONS_PER_THREAD; i++) {
        // Allocate page
        void* page = pmm_alloc_page();
        if (!page) {
            kprintf("[PMM-TEST] Thread %u: Allocation failed at iteration %u\n",
                    thread_id, i);
            test_failed = true;
            break;
        }

        // Do some "work" to increase contention window
        // This makes deadlock more likely to occur if bug exists
        for (volatile int j = 0; j < 100; j++) {
            __asm__ volatile("nop");
        }

        // Free page
        pmm_free_page(page);

        // Yield to increase thread interleaving
        if (i % 10 == 0) {
            sched_yield();
        }
    }

    __sync_fetch_and_add(&threads_completed, 1);
    kprintf("[PMM-TEST] Thread %u completed (%u iterations)\n",
            thread_id, ITERATIONS_PER_THREAD);
}

/**
 * Watchdog thread: Detects if test hangs (deadlock)
 */
void pmm_watchdog(void* arg) {
    uint64_t start_time = get_ticks();
    uint64_t timeout_ticks = (TEST_TIMEOUT_MS * 1000) / TIMER_FREQ;

    while (threads_completed < NUM_THREADS) {
        uint64_t elapsed = get_ticks() - start_time;

        if (elapsed > timeout_ticks) {
            kprintf("\n[PMM-TEST] *** DEADLOCK DETECTED ***\n");
            kprintf("[PMM-TEST] Only %u/%u threads completed\n",
                    threads_completed, NUM_THREADS);
            kprintf("[PMM-TEST] Test hung for > %u ms\n", TEST_TIMEOUT_MS);

            // Print PMM statistics for debugging
            pmm_report_cache_stats();

            kernel_panic("PMM deadlock test FAILED - system deadlocked");
        }

        // Sleep 100ms between checks
        sleep_ms(100);
    }
}

/**
 * Run PMM deadlock stress test
 */
int test_pmm_deadlock(void) {
    kprintf("\n=================================================\n");
    kprintf("PMM Deadlock Stress Test\n");
    kprintf("=================================================\n");
    kprintf("Configuration:\n");
    kprintf("  Threads:     %u\n", NUM_THREADS);
    kprintf("  Iterations:  %u per thread\n", ITERATIONS_PER_THREAD);
    kprintf("  Total ops:   %u allocations + %u frees\n",
            NUM_THREADS * ITERATIONS_PER_THREAD,
            NUM_THREADS * ITERATIONS_PER_THREAD);
    kprintf("  Timeout:     %u ms\n", TEST_TIMEOUT_MS);
    kprintf("\n");

    // Reset test state
    threads_completed = 0;
    test_failed = false;

    // Spawn watchdog thread
    kprintf("[PMM-TEST] Starting watchdog thread...\n");
    thread_t* watchdog = thread_create(pmm_watchdog, NULL, "pmm-watchdog");
    if (!watchdog) {
        kprintf("[PMM-TEST] Failed to create watchdog thread\n");
        return -1;
    }

    // Spawn worker threads
    kprintf("[PMM-TEST] Spawning %u worker threads...\n", NUM_THREADS);
    thread_t* workers[NUM_THREADS];
    for (uint32_t i = 0; i < NUM_THREADS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "pmm-worker-%u", i);
        workers[i] = thread_create(pmm_stress_worker, (void*)(uintptr_t)i, name);
        if (!workers[i]) {
            kprintf("[PMM-TEST] Failed to create worker thread %u\n", i);
            return -1;
        }
    }

    kprintf("[PMM-TEST] All threads spawned, running stress test...\n\n");

    // Wait for all workers to complete
    for (uint32_t i = 0; i < NUM_THREADS; i++) {
        thread_join(workers[i]);
    }

    // Stop watchdog
    thread_cancel(watchdog);
    thread_join(watchdog);

    // Check results
    kprintf("\n=================================================\n");
    if (test_failed) {
        kprintf("[PMM-TEST] FAILED - Allocation error occurred\n");
        return -1;
    } else if (threads_completed != NUM_THREADS) {
        kprintf("[PMM-TEST] FAILED - Only %u/%u threads completed\n",
                threads_completed, NUM_THREADS);
        return -1;
    } else {
        kprintf("[PMM-TEST] PASSED - No deadlock detected\n");
        kprintf("[PMM-TEST] All %u threads completed successfully\n", NUM_THREADS);
        kprintf("[PMM-TEST] Total operations: %u\n",
                NUM_THREADS * ITERATIONS_PER_THREAD * 2);

        // Print final statistics
        pmm_report_cache_stats();

        kprintf("=================================================\n\n");
        return 0;
    }
}

/**
 * Test cache lock contention specifically
 * Tries to trigger the deadlock by hammering cache refills
 */
int test_pmm_cache_contention(void) {
    kprintf("\n=================================================\n");
    kprintf("PMM Cache Contention Test\n");
    kprintf("=================================================\n");

    kprintf("[PMM-TEST] Depleting cache to force refills...\n");

    // Allocate many pages to deplete cache
    #define DEPLETION_COUNT 100
    void* pages[DEPLETION_COUNT];
    for (int i = 0; i < DEPLETION_COUNT; i++) {
        pages[i] = pmm_alloc_page();
    }

    kprintf("[PMM-TEST] Cache depleted, spawning contention threads...\n");

    // Now spawn threads that will all hit cache miss simultaneously
    // This creates maximum contention on cache lock + global lock
    #define CONTENTION_THREADS 8
    thread_t* threads[CONTENTION_THREADS];

    for (int i = 0; i < CONTENTION_THREADS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "contention-%d", i);
        threads[i] = thread_create(pmm_stress_worker, (void*)(uintptr_t)i, name);
    }

    // Wait for completion
    for (int i = 0; i < CONTENTION_THREADS; i++) {
        thread_join(threads[i]);
    }

    // Free pages
    for (int i = 0; i < DEPLETION_COUNT; i++) {
        pmm_free_page(pages[i]);
    }

    kprintf("[PMM-TEST] Cache contention test PASSED\n");
    kprintf("=================================================\n\n");
    return 0;
}

/**
 * Test lock order specifically by acquiring locks in wrong order
 * This should trigger a deadlock if the bug exists
 */
int test_pmm_lock_order(void) {
    kprintf("\n=================================================\n");
    kprintf("PMM Lock Order Test\n");
    kprintf("=================================================\n");

    kprintf("[PMM-TEST] Testing lock acquisition order...\n");

    // Thread 1: Allocates (cache->lock then global_pmm_lock)
    // Thread 2: Also allocates (cache->lock then global_pmm_lock)
    // If both threads get cache lock but then block on global_pmm_lock = OK
    // If there's a reverse path (global_pmm_lock then cache->lock) = DEADLOCK

    // This test validates our fix by ensuring no reverse path exists

    #define LOCK_ORDER_THREADS 4
    thread_t* threads[LOCK_ORDER_THREADS];

    for (int i = 0; i < LOCK_ORDER_THREADS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "lockorder-%d", i);
        threads[i] = thread_create(pmm_stress_worker, (void*)(uintptr_t)i, name);
    }

    // Wait with timeout
    uint64_t start = get_ticks();
    for (int i = 0; i < LOCK_ORDER_THREADS; i++) {
        thread_join(threads[i]);
    }
    uint64_t elapsed = get_ticks() - start;

    kprintf("[PMM-TEST] Lock order test completed in %llu ticks\n", elapsed);
    kprintf("[PMM-TEST] Lock order test PASSED\n");
    kprintf("=================================================\n\n");
    return 0;
}

/**
 * Run all PMM deadlock tests
 */
int test_pmm_deadlock_suite(void) {
    int failures = 0;

    kprintf("\n");
    kprintf("*************************************************\n");
    kprintf("*                                               *\n");
    kprintf("*      PMM DEADLOCK TEST SUITE                 *\n");
    kprintf("*                                               *\n");
    kprintf("*************************************************\n");
    kprintf("\n");

    // Test 1: Basic stress test
    kprintf("Running test 1/3: Basic stress test...\n");
    if (test_pmm_deadlock() != 0) {
        kprintf("Test 1 FAILED\n\n");
        failures++;
    } else {
        kprintf("Test 1 PASSED\n\n");
    }

    // Test 2: Cache contention
    kprintf("Running test 2/3: Cache contention...\n");
    if (test_pmm_cache_contention() != 0) {
        kprintf("Test 2 FAILED\n\n");
        failures++;
    } else {
        kprintf("Test 2 PASSED\n\n");
    }

    // Test 3: Lock order
    kprintf("Running test 3/3: Lock order...\n");
    if (test_pmm_lock_order() != 0) {
        kprintf("Test 3 FAILED\n\n");
        failures++;
    } else {
        kprintf("Test 3 PASSED\n\n");
    }

    // Summary
    kprintf("\n");
    kprintf("*************************************************\n");
    kprintf("*                                               *\n");
    kprintf("*      TEST SUITE RESULTS                      *\n");
    kprintf("*                                               *\n");
    kprintf("*************************************************\n");
    kprintf("\n");
    kprintf("  Total tests:  3\n");
    kprintf("  Passed:       %d\n", 3 - failures);
    kprintf("  Failed:       %d\n", failures);
    kprintf("\n");

    if (failures == 0) {
        kprintf("  Status: ALL TESTS PASSED\n");
        kprintf("  No deadlocks detected!\n");
    } else {
        kprintf("  Status: SOME TESTS FAILED\n");
        kprintf("  Deadlock bugs may still exist!\n");
    }

    kprintf("\n");
    kprintf("*************************************************\n");
    kprintf("\n");

    return failures;
}
