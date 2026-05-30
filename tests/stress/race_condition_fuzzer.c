/**
 * Race Condition Fuzzer for AutomationOS
 *
 * Attempts to trigger race conditions through:
 * - Concurrent modifications
 * - TOCTOU (Time-of-check-time-of-use) attacks
 * - Shared state manipulation
 * - Lock-free operation races
 * - Reference counting races
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/mem.h"
#include "../../kernel/include/spinlock.h"
#include <stdatomic.h>

// Shared state for race condition testing
static atomic_int race_counter = 0;
static atomic_int race_detected = 0;
static process_t* shared_process = NULL;
static void* shared_allocation = NULL;
static spinlock_t test_lock;

/**
 * Race Test 1: Reference Count Race
 *
 * Attempts to trigger use-after-free through ref count races.
 */
void race_test_refcount(void) {
    kprintf("\n[RACE] === Reference Count Race Test ===\n");

    // Create a process to race on
    process_t* victim = process_create("victim", (void*)0x1000);
    if (!victim) {
        kprintf("[RACE] Failed to create victim process\n");
        return;
    }

    kprintf("[RACE] Created victim process PID=%u\n", victim->pid);

    // Simulate concurrent ref/unref operations
    // In a real SMP system, these would run on different cores

    // Thread 1 simulation: Multiple refs
    for (int i = 0; i < 100; i++) {
        process_ref(victim);
    }

    // Thread 2 simulation: Multiple unrefs
    for (int i = 0; i < 100; i++) {
        process_unref(victim);
    }

    // Check if process still valid
    // If ref counting is broken, this could crash
    kprintf("[RACE] Victim process name: '%s' (PID=%u)\n",
            victim->name, victim->pid);

    // Final cleanup
    process_unref(victim);

    kprintf("[RACE] Reference count test complete - no crash\n");
}

/**
 * Race Test 2: Heap Allocation Race
 *
 * Attempts to trigger heap corruption through concurrent allocations.
 */
void race_test_heap_allocation(void) {
    kprintf("\n[RACE] === Heap Allocation Race Test ===\n");

    #define RACE_ALLOCS 100
    void* allocations[RACE_ALLOCS];

    // Simulate two threads allocating/freeing simultaneously
    // Thread 1: Allocate even indices
    for (int i = 0; i < RACE_ALLOCS; i += 2) {
        allocations[i] = kmalloc(chaos_random_range(64, 1024));
    }

    // Thread 2: Allocate odd indices
    for (int i = 1; i < RACE_ALLOCS; i += 2) {
        allocations[i] = kmalloc(chaos_random_range(64, 1024));
    }

    // Verify all allocations are unique
    for (int i = 0; i < RACE_ALLOCS; i++) {
        if (!allocations[i]) continue;

        for (int j = i + 1; j < RACE_ALLOCS; j++) {
            if (allocations[j] == allocations[i]) {
                kprintf("[RACE] CRITICAL: Duplicate allocation detected!\n");
                kprintf("[RACE] allocs[%d] == allocs[%d] == %p\n",
                        i, j, allocations[i]);
                race_detected++;
            }
        }
    }

    // Cleanup
    for (int i = 0; i < RACE_ALLOCS; i++) {
        if (allocations[i]) kfree(allocations[i]);
    }

    if (race_detected == 0) {
        kprintf("[RACE] No heap allocation races detected\n");
    }
}

/**
 * Race Test 3: Process Table Race
 *
 * Attempts to race on process table updates.
 */
void race_test_process_table(void) {
    kprintf("\n[RACE] === Process Table Race Test ===\n");

    #define RACE_PROCS 50
    process_t* processes[RACE_PROCS];

    // Create many processes rapidly
    for (int i = 0; i < RACE_PROCS; i++) {
        processes[i] = process_create("race", (void*)0x1000);
    }

    // Verify all PIDs are unique
    for (int i = 0; i < RACE_PROCS; i++) {
        if (!processes[i]) continue;

        for (int j = i + 1; j < RACE_PROCS; j++) {
            if (!processes[j]) continue;

            if (processes[i]->pid == processes[j]->pid) {
                kprintf("[RACE] CRITICAL: Duplicate PID %u assigned!\n",
                        processes[i]->pid);
                race_detected++;
            }
        }
    }

    // Cleanup
    for (int i = 0; i < RACE_PROCS; i++) {
        if (processes[i]) process_destroy(processes[i]);
    }

    if (race_detected == 0) {
        kprintf("[RACE] No process table races detected\n");
    }
}

/**
 * Race Test 4: TOCTOU (Time-of-Check-Time-of-Use)
 *
 * Tests for race conditions between validation and use.
 */
void race_test_toctou(void) {
    kprintf("\n[RACE] === TOCTOU Race Test ===\n");

    // Simulate TOCTOU on process lookup
    process_t* proc = process_create("toctou", (void*)0x1000);
    if (!proc) return;

    uint32_t pid = proc->pid;

    // Time of Check: Verify process exists
    process_t* check = process_get_by_pid(pid);
    if (!check) {
        kprintf("[RACE] Process lookup failed at check time\n");
        return;
    }

    // ** RACE WINDOW HERE **
    // In real system, another thread could destroy process here

    // Time of Use: Use the process
    kprintf("[RACE] Using process: '%s' PID=%u\n", check->name, check->pid);

    // Proper fix: Use reference counting
    process_ref(check);
    // ... use process ...
    process_unref(check);

    // Cleanup
    process_destroy(proc);

    kprintf("[RACE] TOCTOU test complete\n");
}

/**
 * Race Test 5: Lock-Free Data Structure Race
 *
 * Tests atomicity of lock-free operations.
 */
void race_test_lock_free(void) {
    kprintf("\n[RACE] === Lock-Free Data Structure Race ===\n");

    atomic_int counter = 0;
    const int iterations = 10000;

    // Simulate concurrent increments
    for (int i = 0; i < iterations; i++) {
        atomic_fetch_add(&counter, 1);
    }

    int final_value = atomic_load(&counter);

    if (final_value == iterations) {
        kprintf("[RACE] Lock-free counter correct: %d\n", final_value);
    } else {
        kprintf("[RACE] CRITICAL: Lock-free counter wrong: %d (expected %d)\n",
                final_value, iterations);
        race_detected++;
    }
}

/**
 * Race Test 6: Shared Resource Contention
 *
 * Multiple "threads" competing for shared resource.
 */
void race_test_shared_resource(void) {
    kprintf("\n[RACE] === Shared Resource Contention ===\n");

    spin_lock_init(&test_lock);

    atomic_int protected_counter = 0;
    atomic_int unprotected_counter = 0;

    const int operations = 1000;

    // Unprotected operations (will have races on real SMP)
    for (int i = 0; i < operations; i++) {
        int val = atomic_load(&unprotected_counter);
        // RACE WINDOW
        atomic_store(&unprotected_counter, val + 1);
    }

    // Protected operations
    for (int i = 0; i < operations; i++) {
        spin_lock(&test_lock);
        int val = atomic_load(&protected_counter);
        atomic_store(&protected_counter, val + 1);
        spin_unlock(&test_lock);
    }

    kprintf("[RACE] Unprotected counter: %d\n", atomic_load(&unprotected_counter));
    kprintf("[RACE] Protected counter: %d\n", atomic_load(&protected_counter));

    if (atomic_load(&protected_counter) != operations) {
        kprintf("[RACE] CRITICAL: Protected counter incorrect!\n");
        race_detected++;
    }
}

/**
 * Race Test 7: Double-Destroy Race
 *
 * Attempts to trigger double-free through concurrent destroys.
 */
void race_test_double_destroy(void) {
    kprintf("\n[RACE] === Double-Destroy Race Test ===\n");

    process_t* victim = process_create("destroy", (void*)0x1000);
    if (!victim) return;

    // Add references to prevent immediate free
    process_ref(victim);
    process_ref(victim);

    // Simulate two threads racing to destroy
    // Thread 1:
    process_unref(victim);

    // Thread 2:
    process_unref(victim);

    // Final unref (should actually free)
    process_unref(victim);

    kprintf("[RACE] Double-destroy test complete - no crash\n");
}

/**
 * Race Test 8: ABA Problem
 *
 * Tests for ABA problem in lock-free structures.
 */
void race_test_aba_problem(void) {
    kprintf("\n[RACE] === ABA Problem Test ===\n");

    // Classic ABA scenario with pointer
    void* original = kmalloc(1024);
    void* ptr_a = original;

    // Thread 1 reads pointer
    void* read1 = ptr_a;

    // Thread 2 frees and reallocates (might get same address)
    kfree(ptr_a);
    void* ptr_b = kmalloc(1024); // Might be same as ptr_a!

    // Thread 1 compares and swaps (thinks nothing changed)
    if (ptr_b == read1) {
        kprintf("[RACE] WARNING: ABA problem detected - same address reused!\n");
        kprintf("[RACE] Original: %p, New allocation: %p\n", read1, ptr_b);
        race_detected++;
    } else {
        kprintf("[RACE] No ABA problem detected (different addresses)\n");
    }

    kfree(ptr_b);
}

/**
 * Race Test 9: Scheduler Race Conditions
 *
 * Tests for races in scheduler state.
 */
void race_test_scheduler_races(void) {
    kprintf("\n[RACE] === Scheduler Race Test ===\n");

    // Create multiple processes and rapidly change states
    #define SCHED_RACE_PROCS 20
    process_t* procs[SCHED_RACE_PROCS];

    for (int i = 0; i < SCHED_RACE_PROCS; i++) {
        procs[i] = process_create("sched_race", (void*)0x1000);
        if (procs[i]) {
            // Rapidly add to scheduler
            scheduler_add_process(procs[i]);
        }
    }

    // Rapidly remove from scheduler
    for (int i = 0; i < SCHED_RACE_PROCS; i++) {
        if (procs[i]) {
            scheduler_remove_process(procs[i]);
            process_destroy(procs[i]);
        }
    }

    kprintf("[RACE] Scheduler race test complete\n");
}

/**
 * Race Test 10: Memory Ordering Issues
 *
 * Tests for memory reordering bugs.
 */
void race_test_memory_ordering(void) {
    kprintf("\n[RACE] === Memory Ordering Test ===\n");

    atomic_int flag = 0;
    atomic_int data = 0;

    // Producer
    atomic_store_explicit(&data, 42, memory_order_relaxed);
    atomic_store_explicit(&flag, 1, memory_order_release);

    // Consumer
    while (atomic_load_explicit(&flag, memory_order_acquire) == 0) {
        // Spin
    }
    int value = atomic_load_explicit(&data, memory_order_relaxed);

    if (value != 42) {
        kprintf("[RACE] CRITICAL: Memory ordering violation! Got %d\n", value);
        race_detected++;
    } else {
        kprintf("[RACE] Memory ordering correct\n");
    }
}

/**
 * Main race condition fuzzer
 */
void run_race_condition_tests(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Race Condition Fuzzer\n");
    kprintf("  Finding Concurrency Bugs\n");
    kprintf("========================================\n");
    kprintf("\n");

    race_detected = 0;

    // Run all race tests
    race_test_refcount();
    race_test_heap_allocation();
    race_test_process_table();
    race_test_toctou();
    race_test_lock_free();
    race_test_shared_resource();
    race_test_double_destroy();
    race_test_aba_problem();
    race_test_scheduler_races();
    race_test_memory_ordering();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Race Condition Fuzzer Results\n");
    kprintf("========================================\n");

    if (race_detected > 0) {
        kprintf("[RACE] CRITICAL: %d race conditions detected!\n", race_detected);
    } else {
        kprintf("[RACE] No race conditions detected in single-core mode\n");
        kprintf("[RACE] NOTE: Many races only appear under true SMP!\n");
    }

    kprintf("\n");
    kprintf("CONCURRENCY VULNERABILITIES:\n");
    kprintf("1. TOCTOU windows in process lookup\n");
    kprintf("2. Potential ABA problems in lock-free code\n");
    kprintf("3. Reference counting must use atomics\n");
    kprintf("4. Heap allocator lock contention under load\n");
    kprintf("5. Scheduler list manipulation needs careful locking\n");
    kprintf("6. Memory barriers needed for cross-core communication\n");
    kprintf("\n");
}

/**
 * Advanced: Systematic Concurrency Testing
 *
 * Uses systematic state space exploration (if we had proper threading).
 */
void systematic_concurrency_test(void) {
    kprintf("\n[RACE] === Systematic Concurrency Testing ===\n");

    // This would require:
    // 1. Multiple CPU cores
    // 2. Thread scheduler with controllable interleavings
    // 3. State space exploration (like Chess or PCT)

    kprintf("[RACE] Systematic testing requires:\n");
    kprintf("[RACE] - SMP support (multiple cores)\n");
    kprintf("[RACE] - Controllable thread scheduling\n");
    kprintf("[RACE] - State space exploration\n");
    kprintf("[RACE] - Happens-before relationship tracking\n");
    kprintf("\n");
}

// Helper for chaos tests
extern uint64_t chaos_seed;
static uint64_t chaos_random(void) {
    chaos_seed = chaos_seed * 1103515245 + 12345;
    return chaos_seed;
}

static uint32_t chaos_random_range(uint32_t min, uint32_t max) {
    return min + (chaos_random() % (max - min + 1));
}
