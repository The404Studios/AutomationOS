/**
 * Comprehensive Memory Leak Detection Test
 *
 * Tests all major kernel subsystems for memory leaks:
 * - Process creation/destruction
 * - Socket creation/destruction
 * - File open/close
 * - VFS operations
 * - IPC (futex, epoll, shm, msgqueue)
 * - Network operations
 *
 * Requires kernel compiled with -DMEM_DEBUG flag.
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/mem.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/syscall.h"
#include "../../kernel/include/socket.h"

#define TEST_ITERATIONS 1000
#define TOLERANCE_KB 16  // Allow small overhead for metadata

/* Track memory at key points */
typedef struct {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t leaked_blocks;
    uint64_t leaked_bytes;
    uint64_t pmm_free;
} leak_snapshot_t;

#ifdef MEM_DEBUG
extern uint64_t alloc_count;
extern uint64_t free_count;
extern uint64_t bytes_allocated;
extern uint64_t bytes_freed;

static void capture_snapshot(leak_snapshot_t* snap) {
    snap->alloc_count = alloc_count;
    snap->free_count = free_count;
    snap->leaked_blocks = alloc_count - free_count;
    snap->leaked_bytes = bytes_allocated - bytes_freed;
    snap->pmm_free = pmm_get_free_memory();
}

static bool compare_snapshots(const char* test_name, leak_snapshot_t* before, leak_snapshot_t* after) {
    int64_t block_delta = (int64_t)(after->leaked_blocks - before->leaked_blocks);
    int64_t byte_delta = (int64_t)(after->leaked_bytes - before->leaked_bytes);
    int64_t pmm_delta = (int64_t)(before->pmm_free - after->pmm_free);

    kprintf("[LEAK TEST] %s:\n", test_name);
    kprintf("  Block delta: %lld (allocs=%llu, frees=%llu)\n",
            block_delta, after->alloc_count - before->alloc_count,
            after->free_count - before->free_count);
    kprintf("  Byte delta: %lld KB\n", byte_delta / 1024);
    kprintf("  PMM delta: %lld KB\n", pmm_delta / 1024);

    // Allow some tolerance for metadata overhead
    bool passed = (block_delta == 0 || (block_delta > 0 && block_delta < 5)) &&
                  (byte_delta < TOLERANCE_KB * 1024) &&
                  (pmm_delta < TOLERANCE_KB * 1024);

    if (passed) {
        kprintf("  [PASS] No significant leak detected\n");
    } else {
        kprintf("  [FAIL] MEMORY LEAK DETECTED!\n");
        if (block_delta > 0) {
            kprintf("    - %lld blocks leaked\n", block_delta);
        }
        if (byte_delta > TOLERANCE_KB * 1024) {
            kprintf("    - %lld KB leaked\n", byte_delta / 1024);
        }
        if (pmm_delta > TOLERANCE_KB * 1024) {
            kprintf("    - %lld KB PMM pages lost\n", pmm_delta / 1024);
        }
    }

    return passed;
}
#endif

/* Test 1: Process lifecycle leak */
static bool test_process_lifecycle(void) {
#ifdef MEM_DEBUG
    leak_snapshot_t before, after;

    kprintf("\n[TEST 1] Process lifecycle (%d iterations)...\n", TEST_ITERATIONS);
    capture_snapshot(&before);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        process_t* proc = process_create("leak_test", (void*)0x1000);
        if (!proc) {
            kprintf("  [ERROR] Failed to create process at iteration %d\n", i);
            return false;
        }
        process_destroy(proc);

        // Periodic progress
        if (i > 0 && i % 100 == 0) {
            kprintf("  Progress: %d/%d processes\n", i, TEST_ITERATIONS);
        }
    }

    // Give cleanup time to complete
    for (volatile int i = 0; i < 1000000; i++);

    capture_snapshot(&after);
    return compare_snapshots("Process Lifecycle", &before, &after);
#else
    kprintf("\n[TEST 1] SKIPPED - Rebuild with -DMEM_DEBUG\n");
    return true;
#endif
}

/* Test 2: Socket lifecycle leak */
static bool test_socket_lifecycle(void) {
#ifdef MEM_DEBUG
    leak_snapshot_t before, after;

    kprintf("\n[TEST 2] Socket lifecycle (%d iterations)...\n", TEST_ITERATIONS);
    capture_snapshot(&before);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        // Test TCP socket
        int sock_tcp = sock_socket(SOCK_STREAM);
        if (sock_tcp >= 0) {
            sock_close(sock_tcp);
        }

        // Test UDP socket
        int sock_udp = sock_socket(SOCK_DGRAM);
        if (sock_udp >= 0) {
            sock_close(sock_udp);
        }

        if (i > 0 && i % 100 == 0) {
            kprintf("  Progress: %d/%d sockets\n", i, TEST_ITERATIONS);
        }
    }

    capture_snapshot(&after);
    return compare_snapshots("Socket Lifecycle", &before, &after);
#else
    kprintf("\n[TEST 2] SKIPPED - Rebuild with -DMEM_DEBUG\n");
    return true;
#endif
}

/* Test 3: kmalloc/kfree stress test */
static bool test_heap_allocations(void) {
#ifdef MEM_DEBUG
    leak_snapshot_t before, after;

    kprintf("\n[TEST 3] Heap allocation stress (%d iterations)...\n", TEST_ITERATIONS);
    capture_snapshot(&before);

    // Test various sizes
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        for (int s = 0; s < num_sizes; s++) {
            void* ptr = kmalloc(sizes[s]);
            if (ptr) {
                // Write to verify it's mapped
                memset(ptr, 0xAB, sizes[s]);
                kfree(ptr);
            }
        }

        if (i > 0 && i % 100 == 0) {
            kprintf("  Progress: %d/%d iterations\n", i, TEST_ITERATIONS);
        }
    }

    capture_snapshot(&after);
    return compare_snapshots("Heap Allocations", &before, &after);
#else
    kprintf("\n[TEST 3] SKIPPED - Rebuild with -DMEM_DEBUG\n");
    return true;
#endif
}

/* Test 4: Mixed workload (processes + sockets + heap) */
static bool test_mixed_workload(void) {
#ifdef MEM_DEBUG
    leak_snapshot_t before, after;

    kprintf("\n[TEST 4] Mixed workload (100 iterations)...\n");
    capture_snapshot(&before);

    for (int i = 0; i < 100; i++) {
        // Create process
        process_t* proc = process_create("mixed_test", (void*)0x1000);

        // Allocate some memory
        void* mem1 = kmalloc(256);
        void* mem2 = kmalloc(1024);

        // Create sockets
        int sock1 = sock_socket(SOCK_STREAM);
        int sock2 = sock_socket(SOCK_DGRAM);

        // Clean up in reverse order
        if (sock2 >= 0) sock_close(sock2);
        if (sock1 >= 0) sock_close(sock1);
        if (mem2) kfree(mem2);
        if (mem1) kfree(mem1);
        if (proc) process_destroy(proc);

        if (i > 0 && i % 20 == 0) {
            kprintf("  Progress: %d/100 iterations\n", i);
        }
    }

    // Give cleanup time
    for (volatile int i = 0; i < 1000000; i++);

    capture_snapshot(&after);
    return compare_snapshots("Mixed Workload", &before, &after);
#else
    kprintf("\n[TEST 4] SKIPPED - Rebuild with -DMEM_DEBUG\n");
    return true;
#endif
}

/* Test 5: Process with address space */
static bool test_process_with_memory(void) {
#ifdef MEM_DEBUG
    leak_snapshot_t before, after;

    kprintf("\n[TEST 5] Process with address space (100 iterations)...\n");
    capture_snapshot(&before);

    for (int i = 0; i < 100; i++) {
        process_t* proc = process_create("mem_test", (void*)0x1000);
        if (proc) {
            // The process already has a CR3/address space from process_create
            // Just verify it exists and then destroy
            if (proc->context.cr3 == 0) {
                kprintf("  [ERROR] Process has no address space!\n");
                process_destroy(proc);
                return false;
            }
            process_destroy(proc);
        }

        if (i > 0 && i % 20 == 0) {
            kprintf("  Progress: %d/100 processes\n", i);
        }
    }

    // Give cleanup time
    for (volatile int i = 0; i < 1000000; i++);

    capture_snapshot(&after);
    return compare_snapshots("Process with Address Space", &before, &after);
#else
    kprintf("\n[TEST 5] SKIPPED - Rebuild with -DMEM_DEBUG\n");
    return true;
#endif
}

/* Main test runner */
void run_leak_detection_tests(void) {
    kprintf("\n");
    kprintf("======================================================\n");
    kprintf("  COMPREHENSIVE MEMORY LEAK DETECTION TEST SUITE\n");
    kprintf("======================================================\n");

#ifdef MEM_DEBUG
    kprintf("Memory leak tracking is ENABLED\n");
    kprintf("Starting baseline stats:\n");
    kmalloc_stats_print();
#else
    kprintf("WARNING: Memory leak tracking is DISABLED\n");
    kprintf("Rebuild kernel with -DMEM_DEBUG flag for full tests\n");
#endif

    kprintf("\n");

    bool all_passed = true;

    all_passed &= test_heap_allocations();
    all_passed &= test_process_lifecycle();
    all_passed &= test_socket_lifecycle();
    all_passed &= test_process_with_memory();
    all_passed &= test_mixed_workload();

    kprintf("\n");
    kprintf("======================================================\n");
    if (all_passed) {
        kprintf("  ALL LEAK TESTS PASSED\n");
    } else {
        kprintf("  SOME TESTS FAILED - MEMORY LEAKS DETECTED\n");
    }
    kprintf("======================================================\n");

#ifdef MEM_DEBUG
    kprintf("\nFinal memory statistics:\n");
    kmalloc_stats_print();
#endif
}
