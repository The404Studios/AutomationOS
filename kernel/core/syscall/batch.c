/*
 * Batched Syscall Interface (io_uring-style)
 * ===========================================
 *
 * Amortizes syscall overhead across N operations by submitting multiple
 * syscalls in a single kernel entry. Uses a submission queue (SQ) and
 * completion queue (CQ) shared between userspace and kernel.
 *
 * Performance goal: 1 context switch for 100 syscalls (100x reduction).
 *
 * Architecture:
 *   - Userspace fills submission queue with syscall requests
 *   - SYS_BATCH_SUBMIT (syscall 82) executes all queued syscalls
 *   - Results written to completion queue
 *   - Single user/kernel mode switch for entire batch
 */

#include "../../include/syscall.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"
#include "../../include/string.h"

/* Maximum syscalls per batch (prevent unbounded kernel execution) */
#define MAX_BATCH_SIZE 256

/* Syscall request structure (userspace-visible ABI) */
typedef struct {
    int32_t  syscall_num;   /* Syscall number (SYS_*) */
    uint32_t reserved;      /* Padding for 8-byte alignment */
    uint64_t args[6];       /* Syscall arguments */
} syscall_request_t;

/* Batch ring structure (userspace-visible ABI) */
typedef struct {
    syscall_request_t* sq;   /* Submission queue (userspace writes) */
    int64_t*           cq;   /* Completion queue (kernel writes) */
    uint32_t           sq_size;
    uint32_t           cq_size;
} batch_ring_t;

/*
 * SYS_BATCH_SUBMIT - Execute batched syscalls
 *
 * Arguments:
 *   ring_ptr - pointer to batch_ring_t in userspace
 *   count    - number of syscalls to execute (must be <= sq_size and cq_size)
 *
 * Returns:
 *   Number of syscalls executed on success
 *   Negative error code on failure
 *
 * Algorithm:
 *   1. Validate ring pointer and count
 *   2. Copy submission queue from userspace to kernel buffer
 *   3. Execute each syscall via syscall_dispatch()
 *   4. Write results to completion queue in userspace
 *   5. Return number of syscalls executed
 *
 * Error handling:
 *   - Individual syscall failures are written to CQ (not propagated)
 *   - Only structural failures (bad pointers, oversized batch) return error
 */
int64_t sys_batch_submit(uint64_t ring_ptr, uint64_t count, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    /* Validate parameters */
    if (!ring_ptr) {
        kprintf("[BATCH] NULL ring pointer\n");
        return EFAULT;
    }

    if (count == 0) {
        return 0;  /* No-op */
    }

    if (count > MAX_BATCH_SIZE) {
        kprintf("[BATCH] Batch too large: %llu (max %d)\n", count, MAX_BATCH_SIZE);
        return EINVAL;
    }

    /* Copy ring structure from userspace */
    batch_ring_t ring;
    if (copy_from_user(&ring, (const void*)ring_ptr, sizeof(batch_ring_t)) != COPY_SUCCESS) {
        kprintf("[BATCH] Failed to copy ring structure\n");
        return EFAULT;
    }

    /* Validate ring structure */
    if (!ring.sq || !ring.cq) {
        kprintf("[BATCH] NULL queue pointer (sq=%p cq=%p)\n", ring.sq, ring.cq);
        return EFAULT;
    }

    if (count > ring.sq_size || count > ring.cq_size) {
        kprintf("[BATCH] Count %llu exceeds queue sizes (sq=%u cq=%u)\n",
                count, ring.sq_size, ring.cq_size);
        return EINVAL;
    }

    /* Allocate kernel buffer for submission queue */
    size_t sq_bytes = count * sizeof(syscall_request_t);
    syscall_request_t* kernel_sq = kmalloc(sq_bytes);
    if (!kernel_sq) {
        kprintf("[BATCH] Failed to allocate kernel SQ buffer (%zu bytes)\n", sq_bytes);
        return ENOMEM;
    }

    /* Copy submission queue from userspace */
    if (copy_from_user(kernel_sq, ring.sq, sq_bytes) != COPY_SUCCESS) {
        kprintf("[BATCH] Failed to copy submission queue\n");
        kfree(kernel_sq);
        return EFAULT;
    }

    /* Allocate kernel buffer for completion queue */
    size_t cq_bytes = count * sizeof(int64_t);
    int64_t* kernel_cq = kmalloc(cq_bytes);
    if (!kernel_cq) {
        kprintf("[BATCH] Failed to allocate kernel CQ buffer (%zu bytes)\n", cq_bytes);
        kfree(kernel_sq);
        return ENOMEM;
    }

    /*
     * Execute each syscall in the batch
     *
     * NOTE: We execute ALL syscalls even if some fail. Individual failures
     * are written to the completion queue. Only structural failures (OOM,
     * bad pointers) cause early termination.
     */
    uint64_t executed = 0;
    for (uint64_t i = 0; i < count; i++) {
        syscall_request_t* req = &kernel_sq[i];

        /* Validate syscall number (prevent kernel corruption) */
        if (req->syscall_num < 0 || (uint64_t)req->syscall_num >= MAX_SYSCALLS) {
            kprintf("[BATCH] Invalid syscall %d at index %llu\n", req->syscall_num, i);
            kernel_cq[i] = EINVAL;
            executed++;
            continue;
        }

        /* Dispatch syscall (this is the hot path - minimize overhead) */
        int64_t result = syscall_dispatch(
            (uint64_t)req->syscall_num,
            req->args[0],
            req->args[1],
            req->args[2],
            req->args[3],
            req->args[4],
            req->args[5]
        );

        /* Store result in completion queue */
        kernel_cq[i] = result;
        executed++;
    }

    /* Copy completion queue to userspace */
    if (copy_to_user(ring.cq, kernel_cq, cq_bytes) != COPY_SUCCESS) {
        kprintf("[BATCH] Failed to copy completion queue\n");
        kfree(kernel_sq);
        kfree(kernel_cq);
        return EFAULT;
    }

    /* Cleanup */
    kfree(kernel_sq);
    kfree(kernel_cq);

#ifndef BATCH_QUIET
    kprintf("[BATCH] Executed %llu syscalls\n", executed);
#endif

    return (int64_t)executed;
}

/*
 * Performance Analysis
 * ====================
 *
 * Traditional approach (100 syscalls):
 *   - 100x context switch (user → kernel → user)
 *   - 100x syscall entry overhead (save/restore registers)
 *   - 100x page table switches (CR3 loads)
 *   - Estimated overhead: ~10,000 cycles per syscall = 1,000,000 cycles total
 *
 * Batched approach (100 syscalls):
 *   - 1x context switch
 *   - 1x syscall entry overhead
 *   - 1x page table switch (CR3 load)
 *   - 100x syscall_dispatch() (minimal overhead)
 *   - Estimated overhead: ~10,000 + 100*500 = 60,000 cycles total
 *
 * Speedup: 1,000,000 / 60,000 = 16.7x
 *
 * For I/O-bound workloads (e.g., 100x read()), the overhead reduction
 * allows the CPU to spend more time on actual work vs. context switching.
 */
