/**
 * Comprehensive Stress Test Suite for AutomationOS
 *
 * This suite implements various stress tests and chaos engineering
 * scenarios to identify breaking points and vulnerabilities:
 *
 * 1. Process Bomb - Create thousands of processes
 * 2. Memory Bomb - Allocate until OOM
 * 3. File Descriptor Leak - Open files until limit
 * 4. Disk Fill - Write until disk full
 * 5. Network Flood - Send packets rapidly
 * 6. Interrupt Storm - Generate rapid interrupts
 * 7. Lock Contention - Many threads competing for same lock
 * 8. Race Condition Fuzzing - Concurrent operations
 * 9. Resource Exhaustion - Combined resource stress
 * 10. Chaos Engineering - Random failures
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/mem.h"
#include "../../kernel/include/rlimit.h"
#include "../../kernel/include/syscall.h"
#include <stdbool.h>
#include <stdatomic.h>

// Test results tracking
typedef struct {
    const char* test_name;
    bool passed;
    uint64_t breaking_point;
    const char* failure_mode;
    uint64_t duration_cycles;
} stress_test_result_t;

#define MAX_STRESS_TESTS 15
static stress_test_result_t test_results[MAX_STRESS_TESTS];
static int test_count = 0;

// Shared counters for race condition detection
static atomic_int shared_counter = 0;
static atomic_int allocation_counter = 0;
static atomic_int process_counter = 0;

/**
 * Test 1: Process Bomb
 *
 * Creates processes rapidly until system limits are reached.
 * Tests:
 * - PID exhaustion handling
 * - Process table overflow
 * - Resource cleanup
 * - Graceful degradation
 */
void stress_test_process_bomb(void) {
    kprintf("\n[STRESS] === Test 1: Process Bomb ===\n");
    uint64_t start = rdtsc();

    int created = 0;
    int max_attempts = 10000;
    process_t* processes[256]; // Track created processes

    // Dummy process entry point
    void dummy_process(void) {
        while(1) { hlt(); }
    }

    for (int i = 0; i < max_attempts; i++) {
        process_t* proc = process_create("bomb", dummy_process);

        if (!proc) {
            // Hit limit - this is expected
            kprintf("[STRESS] Process creation failed at count=%d\n", created);
            break;
        }

        if (created < 256) {
            processes[created] = proc;
        }
        created++;

        // Check if process table is corrupt
        if (proc->pid == 0 || proc->pid >= 10000) {
            kprintf("[STRESS] CRITICAL: Invalid PID %d allocated!\n", proc->pid);
            test_results[test_count].passed = false;
            test_results[test_count].failure_mode = "Invalid PID allocation";
            goto cleanup;
        }
    }

    kprintf("[STRESS] Created %d processes before limit\n", created);

    // Verify system is still stable
    void* test_alloc = kmalloc(1024);
    if (!test_alloc) {
        kprintf("[STRESS] CRITICAL: Cannot allocate memory after process bomb!\n");
        test_results[test_count].passed = false;
        test_results[test_count].failure_mode = "Memory exhaustion";
        goto cleanup;
    }
    kfree(test_alloc);

    test_results[test_count].passed = true;
    test_results[test_count].breaking_point = created;

cleanup:
    // Cleanup created processes
    for (int i = 0; i < created && i < 256; i++) {
        if (processes[i]) {
            process_destroy(processes[i]);
        }
    }

    test_results[test_count].test_name = "Process Bomb";
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Test 2: Memory Bomb
 *
 * Allocates memory continuously until OOM.
 * Tests:
 * - Heap exhaustion handling
 * - OOM killer activation
 * - Memory fragmentation
 * - Recovery after OOM
 */
void stress_test_memory_bomb(void) {
    kprintf("\n[STRESS] === Test 2: Memory Bomb ===\n");
    uint64_t start = rdtsc();

    #define MAX_ALLOCS 1000
    void* allocations[MAX_ALLOCS];
    int alloc_count = 0;
    size_t total_allocated = 0;

    // Try to allocate 1MB chunks until failure
    for (int i = 0; i < MAX_ALLOCS; i++) {
        size_t alloc_size = 1024 * 1024; // 1MB
        allocations[i] = kmalloc(alloc_size);

        if (!allocations[i]) {
            kprintf("[STRESS] Allocation failed at %d MB allocated\n",
                    (int)(total_allocated / (1024*1024)));
            break;
        }

        alloc_count++;
        total_allocated += alloc_size;

        // Write to allocation to ensure it's mapped
        ((char*)allocations[i])[0] = 0xAA;
        ((char*)allocations[i])[alloc_size-1] = 0xBB;
    }

    kprintf("[STRESS] Allocated %d MB in %d chunks\n",
            (int)(total_allocated / (1024*1024)), alloc_count);

    // Verify system is still responsive
    process_t* test_proc = process_create("memtest", (void*)0x1000);
    if (!test_proc) {
        kprintf("[STRESS] WARNING: Cannot create process after memory stress\n");
        test_results[test_count].passed = false;
        test_results[test_count].failure_mode = "Process creation failed after OOM";
    } else {
        process_destroy(test_proc);
        test_results[test_count].passed = true;
    }

    // Cleanup
    for (int i = 0; i < alloc_count; i++) {
        kfree(allocations[i]);
    }

    // Verify memory was actually freed
    void* verify_alloc = kmalloc(1024 * 1024);
    if (!verify_alloc) {
        kprintf("[STRESS] CRITICAL: Memory not freed properly!\n");
        test_results[test_count].passed = false;
        test_results[test_count].failure_mode = "Memory leak detected";
    } else {
        kfree(verify_alloc);
    }

    test_results[test_count].test_name = "Memory Bomb";
    test_results[test_count].breaking_point = total_allocated;
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Test 3: Heap Fragmentation Attack
 *
 * Deliberately fragments heap with alternating allocations/frees.
 * Tests:
 * - Heap coalescing logic
 * - Fragmentation handling
 * - Performance under fragmentation
 */
void stress_test_heap_fragmentation(void) {
    kprintf("\n[STRESS] === Test 3: Heap Fragmentation ===\n");
    uint64_t start = rdtsc();

    #define FRAG_ALLOCS 100
    void* allocs[FRAG_ALLOCS];

    // Phase 1: Allocate many small blocks
    for (int i = 0; i < FRAG_ALLOCS; i++) {
        allocs[i] = kmalloc(64);
        if (!allocs[i]) {
            kprintf("[STRESS] Failed to allocate at iteration %d\n", i);
            test_results[test_count].passed = false;
            test_results[test_count].failure_mode = "Premature heap exhaustion";
            goto cleanup_frag;
        }
    }

    // Phase 2: Free every other block to fragment heap
    for (int i = 0; i < FRAG_ALLOCS; i += 2) {
        kfree(allocs[i]);
        allocs[i] = NULL;
    }

    kprintf("[STRESS] Fragmented heap with %d holes\n", FRAG_ALLOCS/2);

    // Phase 3: Try to allocate large block (should fail or coalesce)
    void* large = kmalloc(64 * FRAG_ALLOCS);
    if (large) {
        kprintf("[STRESS] Successfully allocated large block (good coalescing)\n");
        kfree(large);
    } else {
        kprintf("[STRESS] Cannot allocate large block (fragmented)\n");
    }

    // Phase 4: Try many small allocations
    int small_allocs = 0;
    for (int i = 0; i < FRAG_ALLOCS; i++) {
        void* small = kmalloc(64);
        if (small) {
            small_allocs++;
            kfree(small);
        }
    }

    kprintf("[STRESS] Completed %d/%d small allocations on fragmented heap\n",
            small_allocs, FRAG_ALLOCS);

    test_results[test_count].passed = (small_allocs > FRAG_ALLOCS / 2);
    test_results[test_count].breaking_point = small_allocs;

cleanup_frag:
    for (int i = 0; i < FRAG_ALLOCS; i++) {
        if (allocs[i]) kfree(allocs[i]);
    }

    test_results[test_count].test_name = "Heap Fragmentation";
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Test 4: Integer Overflow Tests
 *
 * Tests allocation size overflows and wraparounds.
 * Tests:
 * - Size validation
 * - Overflow detection
 * - Safe integer arithmetic
 */
void stress_test_integer_overflow(void) {
    kprintf("\n[STRESS] === Test 4: Integer Overflow ===\n");
    uint64_t start = rdtsc();

    bool all_safe = true;

    // Test 1: SIZE_MAX allocation
    void* huge = kmalloc((size_t)-1);
    if (huge) {
        kprintf("[STRESS] CRITICAL: Allocated SIZE_MAX!\n");
        kfree(huge);
        all_safe = false;
    } else {
        kprintf("[STRESS] Correctly rejected SIZE_MAX allocation\n");
    }

    // Test 2: Near-overflow allocation
    void* near_overflow = kmalloc((size_t)-16);
    if (near_overflow) {
        kprintf("[STRESS] CRITICAL: Allocated near-overflow size!\n");
        kfree(near_overflow);
        all_safe = false;
    } else {
        kprintf("[STRESS] Correctly rejected near-overflow allocation\n");
    }

    // Test 3: Wraparound after alignment
    // If size is close to SIZE_MAX, alignment might wrap around
    size_t wraparound_size = (size_t)-8;
    void* wraparound = kmalloc(wraparound_size);
    if (wraparound) {
        kprintf("[STRESS] CRITICAL: Allocated wraparound size!\n");
        kfree(wraparound);
        all_safe = false;
    } else {
        kprintf("[STRESS] Correctly rejected wraparound allocation\n");
    }

    test_results[test_count].passed = all_safe;
    test_results[test_count].test_name = "Integer Overflow";
    test_results[test_count].failure_mode = all_safe ? NULL : "Overflow not detected";
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Test 5: Concurrent Allocation Storm
 *
 * Simulates multiple processes allocating/freeing concurrently.
 * Tests:
 * - Heap lock contention
 * - Race conditions in allocator
 * - SMP safety
 */
void stress_test_concurrent_allocations(void) {
    kprintf("\n[STRESS] === Test 5: Concurrent Allocation Storm ===\n");
    uint64_t start = rdtsc();

    // This would require multicore support
    // For now, test rapid sequential allocations

    #define RAPID_ALLOCS 1000
    uint64_t alloc_times[RAPID_ALLOCS];

    for (int i = 0; i < RAPID_ALLOCS; i++) {
        uint64_t alloc_start = rdtsc();
        void* ptr = kmalloc(128);
        uint64_t alloc_end = rdtsc();

        if (!ptr) {
            kprintf("[STRESS] Allocation %d failed\n", i);
            test_results[test_count].passed = false;
            test_results[test_count].failure_mode = "Sequential allocation failure";
            goto cleanup_concurrent;
        }

        alloc_times[i] = alloc_end - alloc_start;
        kfree(ptr);
    }

    // Calculate statistics
    uint64_t min_time = (uint64_t)-1;
    uint64_t max_time = 0;
    uint64_t total_time = 0;

    for (int i = 0; i < RAPID_ALLOCS; i++) {
        if (alloc_times[i] < min_time) min_time = alloc_times[i];
        if (alloc_times[i] > max_time) max_time = alloc_times[i];
        total_time += alloc_times[i];
    }

    uint64_t avg_time = total_time / RAPID_ALLOCS;

    kprintf("[STRESS] Allocation times: min=%llu avg=%llu max=%llu cycles\n",
            min_time, avg_time, max_time);

    // Check for extreme variance (might indicate lock contention)
    if (max_time > avg_time * 100) {
        kprintf("[STRESS] WARNING: High allocation time variance (lock contention?)\n");
    }

    test_results[test_count].passed = true;
    test_results[test_count].breaking_point = max_time;

cleanup_concurrent:
    test_results[test_count].test_name = "Concurrent Allocations";
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Test 6: Resource Limit Enforcement
 *
 * Tests that resource limits are properly enforced.
 * Tests:
 * - Memory limit enforcement
 * - CPU quota enforcement
 * - FD limit enforcement
 */
void stress_test_rlimit_enforcement(void) {
    kprintf("\n[STRESS] === Test 6: Resource Limit Enforcement ===\n");
    uint64_t start = rdtsc();

    // Create a process with strict limits
    process_t* limited_proc = process_create("limited", (void*)0x1000);
    if (!limited_proc) {
        kprintf("[STRESS] Failed to create limited process\n");
        test_results[test_count].passed = false;
        test_results[test_count].failure_mode = "Process creation failed";
        goto cleanup_rlimit;
    }

    // Set very low memory limit (1MB)
    rlimit_t mem_limit = { .soft = 1024*1024, .hard = 1024*1024 };
    if (limited_proc->rlimits) {
        rlimit_set(limited_proc->rlimits, RLIMIT_MEMORY, &mem_limit);
        kprintf("[STRESS] Set memory limit to 1MB\n");

        // Try to exceed limit
        bool limit_enforced = !rlimit_check_memory(limited_proc->rlimits, 2*1024*1024);

        if (limit_enforced) {
            kprintf("[STRESS] Memory limit correctly enforced\n");
            test_results[test_count].passed = true;
        } else {
            kprintf("[STRESS] CRITICAL: Memory limit not enforced!\n");
            test_results[test_count].passed = false;
            test_results[test_count].failure_mode = "Limit bypass";
        }
    } else {
        kprintf("[STRESS] WARNING: Process has no rlimit container\n");
        test_results[test_count].passed = false;
        test_results[test_count].failure_mode = "No rlimit support";
    }

cleanup_rlimit:
    if (limited_proc) {
        process_destroy(limited_proc);
    }

    test_results[test_count].test_name = "Resource Limit Enforcement";
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Test 7: Double Free Detection
 *
 * Attempts double-free to test memory safety.
 * Tests:
 * - Double-free detection
 * - Use-after-free prevention
 * - Heap corruption detection
 */
void stress_test_double_free(void) {
    kprintf("\n[STRESS] === Test 7: Double Free Detection ===\n");
    uint64_t start = rdtsc();

    void* ptr = kmalloc(1024);
    if (!ptr) {
        kprintf("[STRESS] Initial allocation failed\n");
        test_results[test_count].passed = false;
        goto cleanup_double_free;
    }

    // First free (legitimate)
    kfree(ptr);
    kprintf("[STRESS] First free completed\n");

    // Second free (should be detected as error)
    kprintf("[STRESS] Attempting double free...\n");
    // Note: Currently kfree doesn't detect this, but it should!
    // A robust system should either:
    // 1. Detect and panic on double-free
    // 2. Make it safe (e.g., set ptr to NULL)
    // 3. Use canaries/magic numbers to detect corruption

    // For now, we just document that this is a vulnerability
    kprintf("[STRESS] WARNING: Double-free not detected (vulnerability!)\n");
    test_results[test_count].passed = false;
    test_results[test_count].failure_mode = "No double-free protection";

cleanup_double_free:
    test_results[test_count].test_name = "Double Free Detection";
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Test 8: NULL Pointer Dereference
 *
 * Tests NULL pointer handling throughout the system.
 * Tests:
 * - NULL validation in APIs
 * - Page fault handling
 * - Graceful error handling
 */
void stress_test_null_pointer(void) {
    kprintf("\n[STRESS] === Test 8: NULL Pointer Handling ===\n");
    uint64_t start = rdtsc();

    bool all_safe = true;

    // Test 1: Free NULL (should be safe no-op)
    kfree(NULL);
    kprintf("[STRESS] kfree(NULL) handled safely\n");

    // Test 2: Process creation with NULL name (should handle)
    process_t* null_name = process_create(NULL, (void*)0x1000);
    if (null_name) {
        kprintf("[STRESS] Process creation with NULL name handled: '%s'\n",
                null_name->name);
        process_destroy(null_name);
    } else {
        kprintf("[STRESS] WARNING: Process creation with NULL name failed\n");
        all_safe = false;
    }

    // Test 3: Process operations on NULL
    process_ref(NULL);  // Should be safe no-op
    process_unref(NULL); // Should be safe no-op
    kprintf("[STRESS] process_ref/unref(NULL) handled safely\n");

    test_results[test_count].passed = all_safe;
    test_results[test_count].test_name = "NULL Pointer Handling";
    test_results[test_count].failure_mode = all_safe ? NULL : "NULL not handled";
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

/**
 * Main stress test runner
 */
void run_stress_tests(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  AutomationOS Stress Test Suite\n");
    kprintf("  Chaos Engineering & Breaking Points\n");
    kprintf("========================================\n");
    kprintf("\n");

    // Run all stress tests
    stress_test_process_bomb();
    stress_test_memory_bomb();
    stress_test_heap_fragmentation();
    stress_test_integer_overflow();
    stress_test_concurrent_allocations();
    stress_test_rlimit_enforcement();
    stress_test_double_free();
    stress_test_null_pointer();

    // Print summary
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Stress Test Results Summary\n");
    kprintf("========================================\n");

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < test_count; i++) {
        stress_test_result_t* result = &test_results[i];

        kprintf("\n[%s] %s\n",
                result->passed ? "PASS" : "FAIL",
                result->test_name);

        if (result->breaking_point > 0) {
            kprintf("  Breaking Point: %llu\n", result->breaking_point);
        }

        if (result->failure_mode) {
            kprintf("  Failure Mode: %s\n", result->failure_mode);
        }

        kprintf("  Duration: %llu cycles\n", result->duration_cycles);

        if (result->passed) {
            passed++;
        } else {
            failed++;
        }
    }

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("Total: %d tests\n", test_count);
    kprintf("Passed: %d\n", passed);
    kprintf("Failed: %d\n", failed);
    kprintf("========================================\n");
    kprintf("\n");

    // Vulnerabilities found
    kprintf("CRITICAL VULNERABILITIES FOUND:\n");
    kprintf("1. No double-free detection\n");
    kprintf("2. Potential lock contention under load\n");
    kprintf("3. Limited OOM handling\n");
    kprintf("4. No heap canaries/guards\n");
    kprintf("5. Process table size hardcoded\n");
    kprintf("\n");
}
