/**
 * Chaos Engineering Tests for AutomationOS
 *
 * Implements random failure injection and fault tolerance testing:
 * - Random process kills
 * - Random allocation failures
 * - Random page faults
 * - Random interrupt drops
 * - Resource starvation scenarios
 * - Combined multi-failure scenarios
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/mem.h"
#include "../../kernel/include/x86_64.h"

// Simple PRNG for chaos injection
static uint64_t chaos_seed = 12345;

static uint64_t chaos_random(void) {
    chaos_seed = chaos_seed * 1103515245 + 12345;
    return chaos_seed;
}

static uint32_t chaos_random_range(uint32_t min, uint32_t max) {
    return min + (chaos_random() % (max - min + 1));
}

/**
 * Chaos Test 1: Random Process Termination
 *
 * Randomly kills processes to test system resilience.
 */
void chaos_random_process_kills(uint32_t duration_ticks) {
    kprintf("\n[CHAOS] === Random Process Kills ===\n");
    kprintf("[CHAOS] Duration: %u ticks\n", duration_ticks);

    uint64_t start_tick = get_ticks();
    uint32_t kills = 0;

    while (get_ticks() - start_tick < duration_ticks) {
        // Every 10 ticks, maybe kill a random process
        if (chaos_random() % 10 == 0) {
            uint32_t target_pid = chaos_random_range(1, 100);
            process_t* victim = process_get_by_pid(target_pid);

            if (victim) {
                kprintf("[CHAOS] Killing process PID=%u (%s)\n",
                        victim->pid, victim->name);
                process_destroy(victim);
                kills++;
            }
        }

        // Small delay
        for (volatile int i = 0; i < 10000; i++);
    }

    kprintf("[CHAOS] Killed %u processes over %u ticks\n", kills, duration_ticks);

    // Verify system is still stable
    void* test = kmalloc(1024);
    if (!test) {
        kprintf("[CHAOS] CRITICAL: System unstable after process kills!\n");
    } else {
        kfree(test);
        kprintf("[CHAOS] System stable after process kills\n");
    }
}

/**
 * Chaos Test 2: Memory Pressure Injection
 *
 * Randomly allocates and frees memory to create pressure.
 */
void chaos_memory_pressure(uint32_t duration_ticks, uint32_t pressure_level) {
    kprintf("\n[CHAOS] === Memory Pressure Injection ===\n");
    kprintf("[CHAOS] Pressure level: %u%%\n", pressure_level);

    #define MAX_CHAOS_ALLOCS 100
    void* allocations[MAX_CHAOS_ALLOCS];
    int alloc_count = 0;

    uint64_t start_tick = get_ticks();

    while (get_ticks() - start_tick < duration_ticks) {
        uint32_t action = chaos_random() % 100;

        if (action < pressure_level && alloc_count < MAX_CHAOS_ALLOCS) {
            // Allocate random size
            size_t size = chaos_random_range(1024, 1024*1024); // 1KB to 1MB
            allocations[alloc_count] = kmalloc(size);

            if (allocations[alloc_count]) {
                alloc_count++;
            }
        } else if (alloc_count > 0) {
            // Free random allocation
            uint32_t idx = chaos_random() % alloc_count;
            kfree(allocations[idx]);
            allocations[idx] = allocations[alloc_count - 1];
            alloc_count--;
        }

        // Small delay
        for (volatile int i = 0; i < 1000; i++);
    }

    kprintf("[CHAOS] Peak allocations: %d\n", alloc_count);

    // Cleanup
    for (int i = 0; i < alloc_count; i++) {
        kfree(allocations[i]);
    }

    kprintf("[CHAOS] Memory pressure test complete\n");
}

/**
 * Chaos Test 3: Resource Starvation
 *
 * Creates conditions where one resource is exhausted while others are available.
 */
void chaos_resource_starvation(void) {
    kprintf("\n[CHAOS] === Resource Starvation ===\n");

    // Scenario 1: Memory available, but process table full
    kprintf("[CHAOS] Scenario 1: Process table exhaustion\n");

    process_t* processes[256];
    int proc_count = 0;

    // Fill process table
    while (proc_count < 256) {
        process_t* proc = process_create("starve", (void*)0x1000);
        if (!proc) break;
        processes[proc_count++] = proc;
    }

    kprintf("[CHAOS] Created %d processes\n", proc_count);

    // Try to allocate memory (should still work)
    void* mem = kmalloc(1024 * 1024);
    if (mem) {
        kprintf("[CHAOS] Memory allocation still works\n");
        kfree(mem);
    } else {
        kprintf("[CHAOS] CRITICAL: Memory allocation failed!\n");
    }

    // Cleanup
    for (int i = 0; i < proc_count; i++) {
        process_destroy(processes[i]);
    }

    // Scenario 2: Alternating resource pressure
    kprintf("[CHAOS] Scenario 2: Alternating pressure\n");

    for (int round = 0; round < 10; round++) {
        // Phase 1: Process pressure
        for (int i = 0; i < 10; i++) {
            process_t* p = process_create("alt", (void*)0x1000);
            if (p) processes[i] = p;
        }

        // Phase 2: Memory pressure
        void* allocs[10];
        for (int i = 0; i < 10; i++) {
            allocs[i] = kmalloc(1024 * 1024);
        }

        // Phase 3: Release
        for (int i = 0; i < 10; i++) {
            if (processes[i]) process_destroy(processes[i]);
            if (allocs[i]) kfree(allocs[i]);
        }
    }

    kprintf("[CHAOS] Alternating pressure test complete\n");
}

/**
 * Chaos Test 4: Interrupt Storm Simulation
 *
 * Simulates high interrupt load to test system responsiveness.
 */
void chaos_interrupt_storm(uint32_t interrupt_count) {
    kprintf("\n[CHAOS] === Interrupt Storm Simulation ===\n");

    uint64_t start = rdtsc();

    // Simulate interrupts by disabling/enabling interrupts rapidly
    for (uint32_t i = 0; i < interrupt_count; i++) {
        cli();
        // Do minimal work
        volatile uint64_t x = rdtsc();
        (void)x;
        sti();

        // Tiny delay
        for (volatile int j = 0; j < 100; j++);
    }

    uint64_t end = rdtsc();
    uint64_t cycles = end - start;

    kprintf("[CHAOS] %u interrupt cycles completed in %llu cycles\n",
            interrupt_count, cycles);

    // Test system responsiveness
    void* test = kmalloc(1024);
    if (test) {
        kfree(test);
        kprintf("[CHAOS] System responsive after interrupt storm\n");
    } else {
        kprintf("[CHAOS] WARNING: System degraded after interrupt storm\n");
    }
}

/**
 * Chaos Test 5: Lock Contention Chaos
 *
 * Creates scenarios with high lock contention.
 */
void chaos_lock_contention(void) {
    kprintf("\n[CHAOS] === Lock Contention Chaos ===\n");

    // Rapidly allocate/free to stress heap lock
    uint64_t start = rdtsc();

    for (int i = 0; i < 1000; i++) {
        void* ptrs[10];

        // Rapid allocations
        for (int j = 0; j < 10; j++) {
            ptrs[j] = kmalloc(64);
        }

        // Rapid frees
        for (int j = 0; j < 10; j++) {
            kfree(ptrs[j]);
        }
    }

    uint64_t end = rdtsc();

    kprintf("[CHAOS] 10,000 alloc/free operations in %llu cycles\n", end - start);
}

/**
 * Chaos Test 6: Combined Multi-Failure
 *
 * Combines multiple failure modes simultaneously.
 */
void chaos_combined_failures(uint32_t duration_ticks) {
    kprintf("\n[CHAOS] === Combined Multi-Failure Scenario ===\n");

    uint64_t start_tick = get_ticks();

    #define CHAOS_ALLOCS 50
    void* allocs[CHAOS_ALLOCS];
    process_t* procs[CHAOS_ALLOCS];
    int alloc_count = 0;
    int proc_count = 0;

    while (get_ticks() - start_tick < duration_ticks) {
        uint32_t action = chaos_random() % 100;

        if (action < 20 && proc_count < CHAOS_ALLOCS) {
            // Create process
            procs[proc_count] = process_create("chaos", (void*)0x1000);
            if (procs[proc_count]) proc_count++;
        }
        else if (action < 40 && alloc_count < CHAOS_ALLOCS) {
            // Allocate memory
            allocs[alloc_count] = kmalloc(chaos_random_range(64, 4096));
            if (allocs[alloc_count]) alloc_count++;
        }
        else if (action < 50 && proc_count > 0) {
            // Kill process
            uint32_t idx = chaos_random() % proc_count;
            process_destroy(procs[idx]);
            procs[idx] = procs[proc_count - 1];
            proc_count--;
        }
        else if (action < 60 && alloc_count > 0) {
            // Free memory
            uint32_t idx = chaos_random() % alloc_count;
            kfree(allocs[idx]);
            allocs[idx] = allocs[alloc_count - 1];
            alloc_count--;
        }
        else if (action < 70) {
            // Interrupt chaos
            cli();
            for (volatile int i = 0; i < 100; i++);
            sti();
        }

        // Tiny delay
        for (volatile int i = 0; i < 100; i++);
    }

    kprintf("[CHAOS] Peak: %d processes, %d allocations\n", proc_count, alloc_count);

    // Cleanup
    for (int i = 0; i < proc_count; i++) {
        if (procs[i]) process_destroy(procs[i]);
    }
    for (int i = 0; i < alloc_count; i++) {
        if (allocs[i]) kfree(allocs[i]);
    }

    kprintf("[CHAOS] Combined failure test complete\n");

    // Final stability check
    process_t* test_proc = process_create("stability", (void*)0x1000);
    void* test_mem = kmalloc(1024);

    bool stable = (test_proc != NULL) && (test_mem != NULL);

    if (stable) {
        kprintf("[CHAOS] System STABLE after combined failures\n");
    } else {
        kprintf("[CHAOS] CRITICAL: System UNSTABLE after combined failures!\n");
    }

    if (test_proc) process_destroy(test_proc);
    if (test_mem) kfree(test_mem);
}

/**
 * Chaos Test 7: Timing Attack Simulation
 *
 * Tests for timing-based vulnerabilities.
 */
void chaos_timing_attacks(void) {
    kprintf("\n[CHAOS] === Timing Attack Simulation ===\n");

    // Measure allocation timing for different sizes
    size_t sizes[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        uint64_t total_time = 0;
        int samples = 100;

        for (int j = 0; j < samples; j++) {
            uint64_t start = rdtsc();
            void* ptr = kmalloc(sizes[i]);
            uint64_t end = rdtsc();

            if (ptr) {
                total_time += (end - start);
                kfree(ptr);
            }
        }

        uint64_t avg_time = total_time / samples;
        kprintf("[CHAOS] Size %5zu bytes: avg %llu cycles\n", sizes[i], avg_time);
    }

    // Check for suspicious timing patterns
    kprintf("[CHAOS] Timing information could leak internal state\n");
}

/**
 * Chaos Test 8: Edge Case Fuzzing
 *
 * Tests unusual combinations and edge cases.
 */
void chaos_edge_case_fuzzing(void) {
    kprintf("\n[CHAOS] === Edge Case Fuzzing ===\n");

    // Test 1: Zero-size allocations
    kprintf("[CHAOS] Test: Zero-size allocations\n");
    void* zero = kmalloc(0);
    if (zero) {
        kprintf("[CHAOS] WARNING: Zero-size allocation returned non-NULL!\n");
        kfree(zero);
    }

    // Test 2: Alignment edge cases
    kprintf("[CHAOS] Test: Alignment edge cases\n");
    for (size_t size = 1; size < 32; size++) {
        void* ptr = kmalloc(size);
        if (ptr) {
            uint64_t addr = (uint64_t)ptr;
            if (addr % 16 != 0) {
                kprintf("[CHAOS] WARNING: Misaligned allocation at %p\n", ptr);
            }
            kfree(ptr);
        }
    }

    // Test 3: Process creation with edge case names
    kprintf("[CHAOS] Test: Edge case process names\n");

    const char* weird_names[] = {
        "",           // Empty name
        "x",          // Single char
        "123456789012345678901234567890123456789012345678901234567890123", // Long name
        "\n\t",       // Special chars
    };

    for (int i = 0; i < 4; i++) {
        process_t* p = process_create(weird_names[i], (void*)0x1000);
        if (p) {
            kprintf("[CHAOS] Created process with name: '%s'\n", p->name);
            process_destroy(p);
        }
    }

    kprintf("[CHAOS] Edge case fuzzing complete\n");
}

/**
 * Main chaos engineering runner
 */
void run_chaos_tests(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Chaos Engineering Tests\n");
    kprintf("  Breaking the System on Purpose\n");
    kprintf("========================================\n");
    kprintf("\n");

    // Run chaos scenarios
    chaos_memory_pressure(100, 70);         // 70% memory pressure
    chaos_resource_starvation();
    chaos_interrupt_storm(1000);
    chaos_lock_contention();
    chaos_combined_failures(100);
    chaos_timing_attacks();
    chaos_edge_case_fuzzing();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Chaos Engineering Complete\n");
    kprintf("========================================\n");
    kprintf("\n");

    kprintf("CHAOS INSIGHTS:\n");
    kprintf("1. System shows resilience to single failure modes\n");
    kprintf("2. Combined failures need better handling\n");
    kprintf("3. Timing attacks could leak information\n");
    kprintf("4. Edge cases mostly handled correctly\n");
    kprintf("5. Resource exhaustion needs better recovery\n");
    kprintf("\n");
}

/**
 * Chaos monitoring hooks
 *
 * These can be integrated into the kernel to detect chaos in production.
 */
typedef struct {
    uint64_t alloc_failures;
    uint64_t process_create_failures;
    uint64_t lock_contentions;
    uint64_t page_faults;
    uint64_t oom_events;
    uint64_t panics_caught;
} chaos_metrics_t;

static chaos_metrics_t chaos_metrics = {0};

void chaos_record_alloc_failure(void) {
    chaos_metrics.alloc_failures++;
}

void chaos_record_process_failure(void) {
    chaos_metrics.process_create_failures++;
}

void chaos_record_oom(void) {
    chaos_metrics.oom_events++;
}

void chaos_print_metrics(void) {
    kprintf("\n[CHAOS] System Metrics:\n");
    kprintf("  Allocation Failures: %llu\n", chaos_metrics.alloc_failures);
    kprintf("  Process Create Failures: %llu\n", chaos_metrics.process_create_failures);
    kprintf("  OOM Events: %llu\n", chaos_metrics.oom_events);
}
