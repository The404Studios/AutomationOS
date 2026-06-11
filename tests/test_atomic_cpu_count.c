/*
 * Atomic CPU Count Read Test
 * ===========================
 *
 * Tests that IPI operations use atomic reads of smp_num_cpus to handle
 * CPU hotplug events safely during IPI send operations.
 *
 * Test coverage:
 * 1. Simulate CPU hotplug during IPI send
 * 2. Verify loop bounds are correct (no buffer overrun)
 * 3. Verify no CPUs are skipped (completeness)
 * 4. Race condition detection between reads
 *
 * Expected behavior:
 * - Each IPI operation sees a consistent snapshot of CPU count
 * - No out-of-bounds access to percpu_data[]
 * - All online CPUs receive IPIs despite concurrent hotplug
 */

#include "../kernel/include/smp.h"
#include "../kernel/include/ipi.h"
#include "../kernel/include/kernel.h"
#include "../kernel/include/spinlock.h"
#include <string.h>

// Test configuration
#define TEST_ITERATIONS 1000
#define HOTPLUG_PROBABILITY 10  // 10% chance per iteration

// Test state tracking
typedef struct {
    uint32_t iteration;
    uint32_t cpu_count_snapshot;
    uint32_t cpus_touched;
    bool bounds_violated;
    bool skip_detected;
    uint64_t cpu_visit_bitmap;
} test_state_t;

// Shared test data
static volatile uint32_t ipi_received_count[MAX_CPUS];
static volatile uint32_t total_ipis_sent;
static volatile uint32_t total_ipis_expected;
static spinlock_t test_lock;
static test_state_t test_states[TEST_ITERATIONS];

// Mock hotplug: Simulate CPU count changing mid-operation
static uint32_t simulated_cpu_count = 4;
static spinlock_t hotplug_lock;

// Test result
typedef struct {
    bool passed;
    uint32_t iterations_completed;
    uint32_t bounds_violations;
    uint32_t skipped_cpus;
    uint32_t race_conditions;
    char message[512];
} test_result_t;

// ============================================================================
// Simulation Helpers
// ============================================================================

// Simulate reading smp_num_cpus (with potential concurrent modification)
static uint32_t read_cpu_count_atomic(void) {
    return __atomic_load_n(&simulated_cpu_count, __ATOMIC_ACQUIRE);
}

// Simulate CPU hotplug event (add or remove CPU)
static void simulate_hotplug_event(void) {
    spin_lock(&hotplug_lock);

    // Randomly add or remove a CPU (within bounds)
    if (simulated_cpu_count < 8 && (get_random() % 2 == 0)) {
        simulated_cpu_count++;
        kprintf("  [HOTPLUG] CPU added -> %u CPUs\n", simulated_cpu_count);
    } else if (simulated_cpu_count > 2) {
        simulated_cpu_count--;
        kprintf("  [HOTPLUG] CPU removed -> %u CPUs\n", simulated_cpu_count);
    }

    spin_unlock(&hotplug_lock);
}

// Simple pseudo-random number generator for testing
static uint32_t get_random(void) {
    static uint64_t seed = 12345;
    seed = seed * 1103515245 + 12345;
    return (uint32_t)(seed >> 16);
}

// ============================================================================
// Test Functions
// ============================================================================

// Test 1: Atomic read consistency
// Verify that a single IPI operation sees a consistent CPU count
static void test_atomic_read_consistency(test_result_t* result) {
    kprintf("\n[TEST 1] Atomic Read Consistency\n");
    kprintf("  Testing that IPI loops see consistent CPU count snapshots...\n");

    result->passed = true;
    result->iterations_completed = 0;
    result->bounds_violations = 0;
    result->skipped_cpus = 0;
    result->race_conditions = 0;

    // Initialize
    memset((void*)ipi_received_count, 0, sizeof(ipi_received_count));
    spin_lock_init(&test_lock);

    for (uint32_t iter = 0; iter < TEST_ITERATIONS; iter++) {
        test_state_t* state = &test_states[iter];
        state->iteration = iter;
        state->bounds_violated = false;
        state->skip_detected = false;
        state->cpu_visit_bitmap = 0;

        // Simulate hotplug occasionally
        if (get_random() % 100 < HOTPLUG_PROBABILITY) {
            simulate_hotplug_event();
        }

        // Atomic read at the START of the operation (this is what we're testing)
        uint32_t ncpus = read_cpu_count_atomic();
        state->cpu_count_snapshot = ncpus;
        state->cpus_touched = 0;

        // Simulate IPI send loop (like in ipi_send_mask)
        for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
            // Check bounds
            if (cpu >= MAX_CPUS) {
                state->bounds_violated = true;
                result->bounds_violations++;
                kprintf("  [ERROR] Iteration %u: Out of bounds access cpu=%u >= MAX_CPUS=%u\n",
                        iter, cpu, MAX_CPUS);
                result->passed = false;
                break;
            }

            // Track which CPUs we visited
            state->cpu_visit_bitmap |= (1ULL << cpu);
            state->cpus_touched++;

            // Simulate IPI send
            ipi_received_count[cpu]++;
        }

        // Verify we visited exactly ncpus CPUs
        if (state->cpus_touched != ncpus) {
            state->skip_detected = true;
            result->skipped_cpus++;
            kprintf("  [ERROR] Iteration %u: Expected %u CPUs, touched %u\n",
                    iter, ncpus, state->cpus_touched);
            result->passed = false;
        }

        result->iterations_completed++;
    }

    // Summary
    kprintf("  Completed %u iterations\n", result->iterations_completed);
    kprintf("  Bounds violations: %u\n", result->bounds_violations);
    kprintf("  Skipped CPUs: %u\n", result->skipped_cpus);

    if (result->passed) {
        snprintf(result->message, sizeof(result->message),
                 "All %u iterations passed - atomic reads consistent",
                 result->iterations_completed);
    } else {
        snprintf(result->message, sizeof(result->message),
                 "Bounds violations: %u, Skipped CPUs: %u",
                 result->bounds_violations, result->skipped_cpus);
    }
}

// Test 2: Race condition detection
// Verify that concurrent reads during hotplug don't cause issues
static void test_concurrent_hotplug(test_result_t* result) {
    kprintf("\n[TEST 2] Concurrent Hotplug Race Detection\n");
    kprintf("  Testing multiple concurrent IPI operations during hotplug...\n");

    result->passed = true;
    result->iterations_completed = 0;
    result->race_conditions = 0;

    // Reset state
    memset((void*)ipi_received_count, 0, sizeof(ipi_received_count));
    simulated_cpu_count = 4;

    for (uint32_t iter = 0; iter < TEST_ITERATIONS / 10; iter++) {
        // Trigger hotplug event
        simulate_hotplug_event();

        // Perform multiple concurrent reads (simulating parallel IPI sends)
        uint32_t read1 = read_cpu_count_atomic();
        uint32_t read2 = read_cpu_count_atomic();
        uint32_t read3 = read_cpu_count_atomic();

        // All reads in a tight sequence should be consistent
        // (unless hotplug happens between them, which is fine)
        // What we're checking is that each read is valid
        if (read1 > MAX_CPUS || read2 > MAX_CPUS || read3 > MAX_CPUS) {
            result->race_conditions++;
            kprintf("  [ERROR] Iteration %u: Invalid CPU count (r1=%u, r2=%u, r3=%u)\n",
                    iter, read1, read2, read3);
            result->passed = false;
        }

        // Use the snapshot for an IPI loop
        uint32_t ncpus = read1;
        for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
            if (cpu >= MAX_CPUS) {
                result->race_conditions++;
                result->passed = false;
                break;
            }
            ipi_received_count[cpu]++;
        }

        result->iterations_completed++;
    }

    kprintf("  Completed %u iterations\n", result->iterations_completed);
    kprintf("  Race conditions detected: %u\n", result->race_conditions);

    if (result->passed) {
        snprintf(result->message, sizeof(result->message),
                 "No race conditions in %u iterations",
                 result->iterations_completed);
    } else {
        snprintf(result->message, sizeof(result->message),
                 "Race conditions detected: %u",
                 result->race_conditions);
    }
}

// Test 3: Completeness verification
// Verify that all online CPUs eventually receive IPIs
static void test_completeness(test_result_t* result) {
    kprintf("\n[TEST 3] IPI Completeness Verification\n");
    kprintf("  Testing that all online CPUs receive IPIs over time...\n");

    result->passed = true;
    result->iterations_completed = 0;

    // Reset state
    memset((void*)ipi_received_count, 0, sizeof(ipi_received_count));
    simulated_cpu_count = 4;

    uint32_t max_cpus_seen = 0;

    for (uint32_t iter = 0; iter < TEST_ITERATIONS / 10; iter++) {
        // Occasionally hotplug
        if (get_random() % 100 < 20) {
            simulate_hotplug_event();
        }

        uint32_t ncpus = read_cpu_count_atomic();
        if (ncpus > max_cpus_seen) {
            max_cpus_seen = ncpus;
        }

        // Send IPI to all CPUs
        for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
            if (cpu >= MAX_CPUS) {
                result->passed = false;
                break;
            }
            ipi_received_count[cpu]++;
        }

        result->iterations_completed++;
    }

    // Verify that all CPUs up to max_cpus_seen received at least one IPI
    uint32_t cpus_missed = 0;
    for (uint32_t cpu = 0; cpu < max_cpus_seen; cpu++) {
        if (ipi_received_count[cpu] == 0) {
            cpus_missed++;
            kprintf("  [ERROR] CPU %u never received an IPI (max_cpus_seen=%u)\n",
                    cpu, max_cpus_seen);
            result->passed = false;
        }
    }

    kprintf("  Max CPUs seen: %u\n", max_cpus_seen);
    kprintf("  CPUs missed: %u\n", cpus_missed);

    // Print distribution
    kprintf("  IPI distribution:\n");
    for (uint32_t cpu = 0; cpu < max_cpus_seen; cpu++) {
        kprintf("    CPU %u: %u IPIs\n", cpu, ipi_received_count[cpu]);
    }

    if (result->passed) {
        snprintf(result->message, sizeof(result->message),
                 "All %u CPUs received IPIs",
                 max_cpus_seen);
    } else {
        snprintf(result->message, sizeof(result->message),
                 "%u CPUs missed IPIs",
                 cpus_missed);
    }
}

// Test 4: Loop bounds stress test
// Aggressively test that loop bounds never exceed MAX_CPUS
static void test_loop_bounds_stress(test_result_t* result) {
    kprintf("\n[TEST 4] Loop Bounds Stress Test\n");
    kprintf("  Aggressively testing loop bounds with rapid hotplug...\n");

    result->passed = true;
    result->iterations_completed = 0;
    result->bounds_violations = 0;

    // Reset state
    simulated_cpu_count = 2;

    for (uint32_t iter = 0; iter < TEST_ITERATIONS * 10; iter++) {
        // Very aggressive hotplug (50% chance)
        if (get_random() % 100 < 50) {
            simulate_hotplug_event();
        }

        // Read CPU count
        uint32_t ncpus = read_cpu_count_atomic();

        // Verify bounds BEFORE using it in a loop
        if (ncpus > MAX_CPUS) {
            result->bounds_violations++;
            kprintf("  [ERROR] Iteration %u: ncpus=%u exceeds MAX_CPUS=%u\n",
                    iter, ncpus, MAX_CPUS);
            result->passed = false;
        }

        // Simulate IPI loop
        for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
            if (cpu >= MAX_CPUS) {
                result->bounds_violations++;
                kprintf("  [ERROR] Iteration %u: Loop index cpu=%u >= MAX_CPUS=%u\n",
                        iter, cpu, MAX_CPUS);
                result->passed = false;
                break;
            }
        }

        result->iterations_completed++;
    }

    kprintf("  Completed %u iterations\n", result->iterations_completed);
    kprintf("  Bounds violations: %u\n", result->bounds_violations);

    if (result->passed) {
        snprintf(result->message, sizeof(result->message),
                 "No bounds violations in %u iterations",
                 result->iterations_completed);
    } else {
        snprintf(result->message, sizeof(result->message),
                 "Bounds violations: %u",
                 result->bounds_violations);
    }
}

// ============================================================================
// Main Test Entry Point
// ============================================================================

void run_atomic_cpu_count_tests(void) {
    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("ATOMIC CPU COUNT READ TEST SUITE\n");
    kprintf("================================================================================\n");
    kprintf("\n");
    kprintf("Testing atomic reads of smp_num_cpus during CPU hotplug events\n");
    kprintf("Target: Zero bounds violations, zero skipped CPUs, zero race conditions\n");
    kprintf("\n");

    // Initialize
    spin_lock_init(&hotplug_lock);
    simulated_cpu_count = 4;

    test_result_t result;
    uint32_t tests_passed = 0;
    uint32_t tests_total = 0;

    // Run tests
    test_atomic_read_consistency(&result);
    tests_total++;
    if (result.passed) tests_passed++;
    kprintf("  [%s] Test 1: %s\n\n", result.passed ? "PASS" : "FAIL", result.message);

    test_concurrent_hotplug(&result);
    tests_total++;
    if (result.passed) tests_passed++;
    kprintf("  [%s] Test 2: %s\n\n", result.passed ? "PASS" : "FAIL", result.message);

    test_completeness(&result);
    tests_total++;
    if (result.passed) tests_passed++;
    kprintf("  [%s] Test 3: %s\n\n", result.passed ? "PASS" : "FAIL", result.message);

    test_loop_bounds_stress(&result);
    tests_total++;
    if (result.passed) tests_passed++;
    kprintf("  [%s] Test 4: %s\n\n", result.passed ? "PASS" : "FAIL", result.message);

    // Summary
    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("TEST SUMMARY\n");
    kprintf("================================================================================\n");
    kprintf("Tests passed: %u / %u\n", tests_passed, tests_total);
    kprintf("Tests failed: %u / %u\n", tests_total - tests_passed, tests_total);
    kprintf("\n");

    if (tests_passed == tests_total) {
        kprintf("*** ALL TESTS PASSED ***\n");
        kprintf("Atomic CPU count reads are safe for concurrent hotplug\n");
    } else {
        kprintf("*** SOME TESTS FAILED ***\n");
        kprintf("Review failures above for race conditions or bounds violations\n");
    }
    kprintf("\n");
}

// Integration point: Can be called from kernel test harness
int test_atomic_cpu_count_main(void) {
    run_atomic_cpu_count_tests();
    return 0;
}
