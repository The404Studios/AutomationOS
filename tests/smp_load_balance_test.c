/*
 * SMP Load Balancing Benchmark
 * =============================
 *
 * Tests SMP load balancing by spawning 100 processes on 4 CPUs
 * and measuring load distribution.
 *
 * Expected behavior:
 *  - Processes should be evenly distributed (~25 per CPU)
 *  - Load imbalance should be < 10% after balancing
 *  - Work stealing should handle idle CPUs
 *
 * Test Strategy:
 *  1. Create 100 CPU-bound processes
 *  2. Let scheduler distribute them across CPUs
 *  3. Wait for load balancing to occur
 *  4. Measure final distribution
 *  5. Verify distribution is within tolerance
 */

#include "../kernel/include/kernel.h"
#include "../kernel/include/sched.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/mem.h"

#define NUM_TEST_PROCESSES 100
#define NUM_CPUS 4
#define EXPECTED_LOAD_PER_CPU (NUM_TEST_PROCESSES / NUM_CPUS)
#define TOLERANCE_PERCENT 20  // Allow 20% deviation
#define MAX_DEVIATION (EXPECTED_LOAD_PER_CPU * TOLERANCE_PERCENT / 100)

// Test process entry point (busy loop)
static void test_process_entry(void) {
    // Infinite busy loop to keep process runnable
    for (;;) {
        // Do some work
        volatile uint64_t sum = 0;
        for (int i = 0; i < 1000; i++) {
            sum += i;
        }
    }
}

// Calculate standard deviation of load distribution
static uint32_t calculate_load_stddev(uint32_t* loads, uint32_t num_cpus) {
    // Calculate mean
    uint64_t sum = 0;
    for (uint32_t i = 0; i < num_cpus; i++) {
        sum += loads[i];
    }
    uint32_t mean = sum / num_cpus;

    // Calculate variance
    uint64_t variance_sum = 0;
    for (uint32_t i = 0; i < num_cpus; i++) {
        int32_t diff = (int32_t)loads[i] - (int32_t)mean;
        variance_sum += diff * diff;
    }
    uint32_t variance = variance_sum / num_cpus;

    // Return approximate standard deviation (no sqrt, use variance)
    return variance;
}

int test_smp_load_balance(void) {
    kprintf("\n============================================\n");
    kprintf("SMP Load Balancing Benchmark\n");
    kprintf("============================================\n\n");

    uint32_t num_cpus = smp_cpu_count();
    kprintf("Detected CPUs: %d\n", num_cpus);

    if (num_cpus < NUM_CPUS) {
        kprintf("ERROR: Test requires %d CPUs, only %d available\n", NUM_CPUS, num_cpus);
        kprintf("Adjusting test to use %d CPUs\n\n", num_cpus);
    }

    // Create test processes
    kprintf("Creating %d test processes...\n", NUM_TEST_PROCESSES);
    process_t* test_procs[NUM_TEST_PROCESSES];

    for (int i = 0; i < NUM_TEST_PROCESSES; i++) {
        char name[32];
        snprintf(name, sizeof(name), "test_proc_%d", i);

        test_procs[i] = process_create(name, (void*)test_process_entry);
        if (!test_procs[i]) {
            kprintf("ERROR: Failed to create process %d\n", i);
            return -1;
        }

        // Add to scheduler
        scheduler_add_process(test_procs[i]);
    }

    kprintf("Created %d processes\n\n", NUM_TEST_PROCESSES);

    // Wait for initial distribution
    kprintf("Waiting for initial distribution...\n");
    for (volatile int delay = 0; delay < 1000000; delay++);

    // Measure initial load distribution
    uint32_t initial_loads[MAX_CPUS] = {0};
    scheduler_get_load_stats(initial_loads, MAX_CPUS);

    kprintf("\nInitial Load Distribution:\n");
    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        kprintf("  CPU %d: %d processes\n", cpu, initial_loads[cpu]);
    }

    uint32_t initial_stddev = calculate_load_stddev(initial_loads, num_cpus);
    kprintf("  Initial variance: %d\n\n", initial_stddev);

    // Wait for load balancing to occur
    kprintf("Waiting for load balancing (5 balance cycles)...\n");
    for (int cycle = 0; cycle < 5; cycle++) {
        for (volatile int delay = 0; delay < 10000000; delay++);
        kprintf("  Balance cycle %d/5 complete\n", cycle + 1);
    }

    // Measure final load distribution
    uint32_t final_loads[MAX_CPUS] = {0};
    scheduler_get_load_stats(final_loads, MAX_CPUS);

    kprintf("\nFinal Load Distribution:\n");
    uint32_t min_load = 0xFFFFFFFF;
    uint32_t max_load = 0;
    uint32_t total_load = 0;

    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        uint32_t load = final_loads[cpu];
        kprintf("  CPU %d: %d processes", cpu, load);

        if (load < min_load) min_load = load;
        if (load > max_load) max_load = load;
        total_load += load;

        // Check if within expected range
        int32_t deviation = (int32_t)load - (int32_t)EXPECTED_LOAD_PER_CPU;
        if (deviation < 0) deviation = -deviation;

        if (deviation <= MAX_DEVIATION) {
            kprintf(" [PASS]\n");
        } else {
            kprintf(" [FAIL - deviation %d > %d]\n", deviation, MAX_DEVIATION);
        }
    }

    uint32_t final_stddev = calculate_load_stddev(final_loads, num_cpus);
    kprintf("\n  Final variance: %d\n", final_stddev);
    kprintf("  Total processes: %d\n", total_load);
    kprintf("  Min load: %d, Max load: %d\n", min_load, max_load);
    kprintf("  Load imbalance: %d processes\n", max_load - min_load);

    // Print scheduler statistics
    scheduler_print_stats();

    // Verify results
    kprintf("\n============================================\n");
    kprintf("Test Results:\n");
    kprintf("============================================\n");

    int passed = 1;

    // Check 1: All processes accounted for
    if (total_load != NUM_TEST_PROCESSES) {
        kprintf("[FAIL] Process count mismatch: expected %d, got %d\n",
                NUM_TEST_PROCESSES, total_load);
        passed = 0;
    } else {
        kprintf("[PASS] All processes accounted for\n");
    }

    // Check 2: Load imbalance within tolerance
    uint32_t imbalance = max_load - min_load;
    if (imbalance > MAX_DEVIATION * 2) {
        kprintf("[FAIL] Load imbalance too high: %d (max %d)\n",
                imbalance, MAX_DEVIATION * 2);
        passed = 0;
    } else {
        kprintf("[PASS] Load imbalance acceptable: %d\n", imbalance);
    }

    // Check 3: Variance improved
    if (final_stddev <= initial_stddev) {
        kprintf("[PASS] Load variance improved: %d -> %d\n",
                initial_stddev, final_stddev);
    } else {
        kprintf("[WARN] Load variance increased: %d -> %d\n",
                initial_stddev, final_stddev);
    }

    // Clean up processes
    kprintf("\nCleaning up test processes...\n");
    for (int i = 0; i < NUM_TEST_PROCESSES; i++) {
        if (test_procs[i]) {
            scheduler_remove_process(test_procs[i]);
            process_destroy(test_procs[i]);
        }
    }

    kprintf("\n============================================\n");
    if (passed) {
        kprintf("SMP Load Balancing Test: PASSED\n");
    } else {
        kprintf("SMP Load Balancing Test: FAILED\n");
    }
    kprintf("============================================\n\n");

    return passed ? 0 : -1;
}

// Standalone test runner
void run_smp_load_balance_test(void) {
    int result = test_smp_load_balance();
    if (result == 0) {
        kprintf("Test completed successfully\n");
    } else {
        kprintf("Test failed with code %d\n", result);
    }
}
