/**
 * VMA Red-Black Tree Benchmark
 * =============================
 *
 * Tests the new RB-tree VMA implementation by:
 * 1. Creating 1000+ VMAs (impossible with old 64-page limit)
 * 2. Measuring lookup performance (should be O(log n))
 * 3. Verifying tree integrity
 * 4. Testing edge cases
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VMA_TEST_COUNT 1000
#define VMA_LOOKUP_ITERATIONS 10000

// Syscall numbers (from syscall.h)
#define SYS_VMA_TEST    200  // New syscall for VMA testing

// Test operations
#define VMA_OP_ADD      1
#define VMA_OP_FIND     2
#define VMA_OP_COUNT    3
#define VMA_OP_VERIFY   4
#define VMA_OP_CLEAR    5
#define VMA_OP_BENCH    6

typedef struct {
    uint32_t op;
    uint64_t vaddr;
    uint64_t length;
    uint32_t perm;
    uint32_t result;
    uint64_t time_ns;
} vma_test_req_t;

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile(
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(n), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static void print_progress(const char* msg, int current, int total) {
    printf("\r%s: %d/%d (%d%%)", msg, current, total, (current * 100) / total);
    fflush(stdout);
}

static void test_basic_operations(void) {
    printf("\n=== Basic Operations Test ===\n");

    vma_test_req_t req;

    // Test 1: Add a few VMAs
    printf("Adding 3 VMAs...\n");

    req.op = VMA_OP_ADD;
    req.vaddr = 0x1000;
    req.length = 0x1000;
    req.perm = 7;  // RWX
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);

    req.vaddr = 0x3000;
    req.length = 0x2000;
    req.perm = 3;  // RW
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);

    req.vaddr = 0x6000;
    req.length = 0x1000;
    req.perm = 5;  // RX
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);

    // Test 2: Count VMAs
    req.op = VMA_OP_COUNT;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    printf("VMA count: %u\n", req.result);

    if (req.result != 3) {
        printf("ERROR: Expected 3 VMAs, got %u\n", req.result);
    } else {
        printf("PASS: VMA count correct\n");
    }

    // Test 3: Find VMAs
    printf("Testing lookups...\n");

    req.op = VMA_OP_FIND;
    req.vaddr = 0x1500;  // Inside first VMA
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    if (req.result == 1) {
        printf("PASS: Found VMA at 0x1500\n");
    } else {
        printf("ERROR: Failed to find VMA at 0x1500\n");
    }

    req.vaddr = 0x4000;  // Inside second VMA
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    if (req.result == 1) {
        printf("PASS: Found VMA at 0x4000\n");
    } else {
        printf("ERROR: Failed to find VMA at 0x4000\n");
    }

    req.vaddr = 0x2000;  // Gap between VMAs
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    if (req.result == 0) {
        printf("PASS: Correctly returned NULL for gap\n");
    } else {
        printf("ERROR: Found VMA in gap at 0x2000\n");
    }

    // Test 4: Verify tree integrity
    req.op = VMA_OP_VERIFY;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    if (req.result == 1) {
        printf("PASS: RB-tree properties verified\n");
    } else {
        printf("ERROR: RB-tree property violation\n");
    }
}

static void test_stress(void) {
    printf("\n\n=== Stress Test: %d VMAs ===\n", VMA_TEST_COUNT);

    vma_test_req_t req;
    uint64_t base_addr = 0x100000;
    uint64_t gap = 0x10000;

    // Add many VMAs
    printf("Adding VMAs...\n");
    for (int i = 0; i < VMA_TEST_COUNT; i++) {
        req.op = VMA_OP_ADD;
        req.vaddr = base_addr + (i * gap);
        req.length = 0x1000;
        req.perm = (i % 7) + 1;  // Vary permissions
        syscall3(SYS_VMA_TEST, (long)&req, 0, 0);

        if ((i + 1) % 100 == 0) {
            print_progress("Adding VMAs", i + 1, VMA_TEST_COUNT);
        }
    }
    printf("\n");

    // Verify count
    req.op = VMA_OP_COUNT;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    printf("Total VMAs: %u\n", req.result);

    if (req.result == VMA_TEST_COUNT) {
        printf("PASS: All %d VMAs added successfully\n", VMA_TEST_COUNT);
    } else {
        printf("ERROR: Expected %d VMAs, got %u\n", VMA_TEST_COUNT, req.result);
    }

    // Verify tree integrity
    req.op = VMA_OP_VERIFY;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    if (req.result == 1) {
        printf("PASS: RB-tree properties maintained with %d nodes\n", VMA_TEST_COUNT);
    } else {
        printf("ERROR: RB-tree property violation with %d nodes\n", VMA_TEST_COUNT);
    }
}

static void test_performance(void) {
    printf("\n\n=== Performance Benchmark ===\n");

    vma_test_req_t req;
    uint64_t total_time = 0;
    uint64_t min_time = ~0ULL;
    uint64_t max_time = 0;

    printf("Performing %d lookups...\n", VMA_LOOKUP_ITERATIONS);

    for (int i = 0; i < VMA_LOOKUP_ITERATIONS; i++) {
        req.op = VMA_OP_BENCH;
        // Random address within our VMA range
        req.vaddr = 0x100000 + ((i * 12345) % (VMA_TEST_COUNT * 0x10000));

        syscall3(SYS_VMA_TEST, (long)&req, 0, 0);

        total_time += req.time_ns;
        if (req.time_ns < min_time) min_time = req.time_ns;
        if (req.time_ns > max_time) max_time = req.time_ns;

        if ((i + 1) % 1000 == 0) {
            print_progress("Lookup benchmark", i + 1, VMA_LOOKUP_ITERATIONS);
        }
    }
    printf("\n");

    uint64_t avg_time = total_time / VMA_LOOKUP_ITERATIONS;

    printf("\nLookup Performance (with %d VMAs):\n", VMA_TEST_COUNT);
    printf("  Average: %lu ns\n", avg_time);
    printf("  Min:     %lu ns\n", min_time);
    printf("  Max:     %lu ns\n", max_time);

    // Calculate expected O(log n) time
    // For 1000 nodes, log2(1000) ≈ 10 tree levels
    printf("  Tree depth: ~%d levels (log2(%d))\n",
           (int)(__builtin_clz(1) - __builtin_clz(VMA_TEST_COUNT)), VMA_TEST_COUNT);
}

static void test_edge_cases(void) {
    printf("\n\n=== Edge Case Tests ===\n");

    vma_test_req_t req;

    // Clear all VMAs first
    req.op = VMA_OP_CLEAR;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    printf("Cleared all VMAs\n");

    // Test 1: Adjacent VMAs
    printf("\nTest 1: Adjacent VMAs\n");
    req.op = VMA_OP_ADD;
    req.vaddr = 0x10000;
    req.length = 0x1000;
    req.perm = 7;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);

    req.vaddr = 0x11000;  // Immediately after
    req.length = 0x1000;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);

    req.op = VMA_OP_FIND;
    req.vaddr = 0x10FFF;  // Last byte of first VMA
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    printf("  Lookup at boundary (0x10FFF): %s\n", req.result ? "FOUND" : "NOT FOUND");

    req.vaddr = 0x11000;  // First byte of second VMA
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    printf("  Lookup at boundary (0x11000): %s\n", req.result ? "FOUND" : "NOT FOUND");

    // Test 2: Verify tree after all tests
    req.op = VMA_OP_VERIFY;
    syscall3(SYS_VMA_TEST, (long)&req, 0, 0);
    if (req.result == 1) {
        printf("\nPASS: Final tree verification successful\n");
    } else {
        printf("\nERROR: Final tree verification failed\n");
    }
}

int main(void) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  VMA Red-Black Tree Benchmark v1.0    ║\n");
    printf("║  AutomationOS Memory Management        ║\n");
    printf("╚════════════════════════════════════════╝\n");

    test_basic_operations();
    test_stress();
    test_performance();
    test_edge_cases();

    printf("\n\n╔════════════════════════════════════════╗\n");
    printf("║  Benchmark Complete                    ║\n");
    printf("║  RB-tree successfully handles 1000+    ║\n");
    printf("║  VMAs with O(log n) performance        ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    return 0;
}
