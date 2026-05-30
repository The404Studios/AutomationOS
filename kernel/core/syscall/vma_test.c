/**
 * VMA Test Syscall Handler
 * =========================
 *
 * Provides a test interface for the VMA red-black tree implementation.
 * Used by userspace benchmarking tools to validate and measure performance.
 */

#include "../../include/syscall.h"
#include "../../include/sched.h"
#include "../../include/vma.h"
#include "../../include/kernel.h"
#include "../../include/string.h"
#include "../../include/mem.h"

// Test operations (must match userspace definitions)
#define VMA_OP_ADD      1
#define VMA_OP_FIND     2
#define VMA_OP_COUNT    3
#define VMA_OP_VERIFY   4
#define VMA_OP_CLEAR    5
#define VMA_OP_BENCH    6

// Request structure (must match userspace)
typedef struct {
    uint32_t op;
    uint64_t vaddr;
    uint64_t length;
    uint32_t perm;
    uint32_t result;
    uint64_t time_ns;
} vma_test_req_t;

// Read TSC (timestamp counter) for precise timing
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Convert TSC ticks to approximate nanoseconds (assumes 2.5 GHz CPU)
static inline uint64_t tsc_to_ns(uint64_t tsc) {
    // For a 2.5 GHz CPU: 1 tick = 0.4 ns
    // Multiply by 4 and divide by 10 to avoid floating point
    return (tsc * 4) / 10;
}

/**
 * sys_vma_test: VMA tree testing and benchmarking syscall
 *
 * Operations:
 *   VMA_OP_ADD    - Add a VMA to the current process
 *   VMA_OP_FIND   - Find VMA containing an address
 *   VMA_OP_COUNT  - Count VMAs in the tree
 *   VMA_OP_VERIFY - Verify RB-tree properties
 *   VMA_OP_CLEAR  - Clear all VMAs
 *   VMA_OP_BENCH  - Benchmark lookup with timing
 */
int64_t sys_vma_test(uint64_t req_ptr, uint64_t arg2, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)arg6;

    process_t* proc = process_get_current();
    if (!proc) {
        return -ESRCH;  // No current process
    }

    // Copy request from userspace
    vma_test_req_t req;
    if (!req_ptr || req_ptr >= 0x0000800000000000ULL) {
        return -EFAULT;
    }
    memcpy(&req, (void*)req_ptr, sizeof(req));

    // Dispatch based on operation
    switch (req.op) {
        case VMA_OP_ADD: {
            // Add a test VMA
            vma_t desc = {
                .vaddr = req.vaddr,
                .length = req.length,
                .perm = req.perm,
                .flags = 0,
                .backing = VMA_ANON,
                .file_ptr = NULL,
                .file_off = 0,
                .file_sz = 0
            };
            vma_add(proc, &desc);
            req.result = 1;
            break;
        }

        case VMA_OP_FIND: {
            // Find VMA containing address
            vma_t* v = vma_find(proc, req.vaddr);
            req.result = (v != NULL) ? 1 : 0;
            break;
        }

        case VMA_OP_COUNT: {
            // Count VMAs in tree
            req.result = vma_count(proc);
            break;
        }

        case VMA_OP_VERIFY: {
            // Verify RB-tree properties
            req.result = vma_rb_verify(proc);
            break;
        }

        case VMA_OP_CLEAR: {
            // Clear all VMAs (for testing)
            vma_clear(proc);
            req.result = 0;
            break;
        }

        case VMA_OP_BENCH: {
            // Benchmark lookup with timing
            uint64_t start = rdtsc();
            vma_t* v = vma_find(proc, req.vaddr);
            uint64_t end = rdtsc();

            req.result = (v != NULL) ? 1 : 0;
            req.time_ns = tsc_to_ns(end - start);
            break;
        }

        default:
            return -EINVAL;
    }

    // Copy result back to userspace
    memcpy((void*)req_ptr, &req, sizeof(req));

    return 0;
}
