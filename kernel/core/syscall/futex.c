/*
 * Futex (Fast Userspace Mutex) Implementation
 * ============================================
 *
 * Futexes provide userspace-first synchronization with kernel fallback only on
 * contention. The fast path is a pure userspace atomic compare-and-swap (CAS)
 * with NO syscall overhead (~5 cycles). The slow path enters the kernel only
 * when a lock is contended, blocking the waiter until woken (~500 cycles).
 *
 * This is the Linux futex model: uncontended locks are invisibly fast;
 * contention is handled efficiently by the kernel wait queue.
 *
 * Architecture:
 * -------------
 * 1. Userspace fast path: atomic CAS on a shared int (lock = 0 → 1)
 *    - If CAS succeeds: lock acquired, no syscall
 *    - If CAS fails: lock is contended, fall back to kernel
 *
 * 2. Kernel slow path (SYS_FUTEX):
 *    - FUTEX_WAIT: block on a futex address until woken
 *    - FUTEX_WAKE: wake N waiters on a futex address
 *
 * 3. Wait queue hash table: each futex address maps to a wait queue
 *    - Hash by physical page + offset to handle shared memory
 *    - Spinlock per bucket for concurrency
 *
 * Security & Validation:
 * ----------------------
 * - Userspace addresses are validated (must be aligned, user-accessible)
 * - Atomic load verifies expected value AFTER enqueuing (prevents lost wakeups)
 * - Physical address hashing prevents VA aliasing attacks
 * - Timeouts prevent indefinite blocking
 *
 * Performance:
 * ------------
 * - Uncontended lock: ~5 cycles (atomic CAS only)
 * - Contended lock: ~500 cycles (syscall + wait queue)
 * - Wake: ~300 cycles (syscall + scheduler)
 *
 * Example Usage (userspace):
 * ---------------------------
 *   // Lock acquisition
 *   if (atomic_cas(&lock, 0, 1) == 0) {
 *       // Got lock, no syscall!
 *   } else {
 *       // Contended, sleep in kernel
 *       futex(&lock, FUTEX_WAIT, 1, NULL);
 *   }
 *
 *   // Lock release
 *   lock = 0;
 *   futex(&lock, FUTEX_WAKE, 1, NULL);  // wake one waiter
 */

#include "../../include/syscall.h"
#include "../../include/kernel.h"
#include "../../include/sched.h"
#include "../../include/mem.h"
#include "../../include/string.h"
#include "../../include/spinlock.h"
#include "../../include/x86_64.h"   /* read_cr3 */

// Futex operations
#define FUTEX_WAIT              0   // Block until woken or value mismatch
#define FUTEX_WAKE              1   // Wake N waiters
#define FUTEX_WAIT_TIMEOUT      2   // Block with timeout (not yet implemented)

// Futex wait queue hash table
#define FUTEX_HASHBITS  8
#define FUTEX_HASHSIZE  (1 << FUTEX_HASHBITS)
#define FUTEX_HASHMASK  (FUTEX_HASHSIZE - 1)

// Per-bucket futex wait queue. The bucket holds ONLY the wait_object queue: all
// serialization (value test + enqueue + wake) is on the wait_object's own lock
// (wq.wobj.lock), so wait and wake share a single lock domain (FUTEX-A). The old
// per-bucket spinlock (only ever guarded the value test, on a DIFFERENT lock than the
// enqueue) and the `addr` scalar (written, never read) were both vestigial and removed.
typedef struct futex_bucket {
    wait_queue_t wq;
} futex_bucket_t;

// Global futex hash table
static futex_bucket_t futex_table[FUTEX_HASHSIZE];
static int futex_initialized = 0;

// Initialize futex subsystem (called from kernel init)
void futex_init(void) {
    for (int i = 0; i < FUTEX_HASHSIZE; i++) {
        wq_init(&futex_table[i].wq);
    }
    futex_initialized = 1;
    kprintf("[FUTEX] Initialized futex subsystem (%d hash buckets)\n", FUTEX_HASHSIZE);
}

// Hash function: map physical address to bucket
// Uses physical address to handle shared memory correctly (different VA, same PA)
static inline uint32_t futex_hash(uint64_t phys_addr) {
    // Mix the page number and offset for better distribution
    uint64_t hash = phys_addr;
    hash ^= (hash >> 12);  // Mix in page number
    hash ^= (hash >> 24);  // Further mixing
    return (uint32_t)(hash & FUTEX_HASHMASK);
}

// Validate userspace futex address
// Returns 0 on success, -EINVAL on failure
static int futex_validate_addr(void* uaddr) {
    // Must be aligned to 4 bytes (atomic ops require alignment)
    if (((uint64_t)uaddr & 0x3) != 0) {
        return EINVAL;  // Misaligned address
    }

    // Must be in userspace (< 0x800000000000)
    if ((uint64_t)uaddr >= 0x800000000000ULL) {
        return EINVAL;  // Kernel address
    }

    // Must be mapped (check page table)
    // For now, we trust the address; page fault will catch unmapped access
    return 0;
}

// Get physical address for a userspace virtual address
// Returns physical address, or 0 on failure
static uint64_t futex_get_phys_addr(void* uaddr) {
    // Walk the page table to get physical address
    // This is a simplified version; production code would use vmm_virt_to_phys()

    uint64_t virt = (uint64_t)uaddr;
    uint64_t cr3 = read_cr3() & ~0xFFFULL;

    // Require PAGE_USER as well as PAGE_PRESENT at every level: a futex word must be in
    // a USER-accessible page. Without this, a present-but-kernel-only or PROT_NONE page
    // in the user VA range would resolve to a physical address the process must not be
    // able to wait/wake on (it could then park on, or be woken via, a frame outside its
    // rights). Returning 0 here makes futex_wait/wake fail with EFAULT.
    const uint64_t PUSER = PAGE_PRESENT | PAGE_USER;

    // PML4 index
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t* pml4 = (uint64_t*)cr3;
    if ((pml4[pml4_idx] & PUSER) != PUSER) {
        return 0;
    }

    // PDPT index
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFFULL);
    if ((pdpt[pdpt_idx] & PUSER) != PUSER) {
        return 0;
    }

    // PD index
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);
    if ((pd[pd_idx] & PUSER) != PUSER) {
        return 0;
    }

    // Check for 2MB huge page (PAGE_USER already verified at the PD level above)
    if (pd[pd_idx] & (1ULL << 7)) {
        uint64_t phys_base = pd[pd_idx] & ~0x1FFFFFULL;
        uint64_t offset = virt & 0x1FFFFF;
        return phys_base + offset;
    }

    // PT index (4KB page)
    uint64_t pt_idx = (virt >> 12) & 0x1FF;
    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFFULL);
    if ((pt[pt_idx] & PUSER) != PUSER) {
        return 0;
    }

    uint64_t phys_base = pt[pt_idx] & ~0xFFFULL;
    uint64_t offset = virt & 0xFFF;
    return phys_base + offset;
}

// FUTEX_WAIT: block until woken or value mismatch
// Args: uaddr = futex address, val = expected value
// Returns: 0 on wakeup, -EAGAIN if value mismatch, -EINVAL on error
static int64_t futex_wait(int* uaddr, int val) {
    // Validate address
    int ret = futex_validate_addr(uaddr);
    if (ret != 0) {
        return ret;
    }

    // Get physical address for hashing
    uint64_t phys_addr = futex_get_phys_addr(uaddr);
    if (phys_addr == 0) {
        return EFAULT;  // Unmapped address
    }

    // Hash to bucket
    uint32_t bucket_idx = futex_hash(phys_addr);
    futex_bucket_t* bucket = &futex_table[bucket_idx];

    // CRITICAL ordering (FUTEX-A lost-wakeup fix): the value test, the enqueue, and the
    // BLOCKED store must all happen under the SAME lock that futex_wake takes -- here the
    // wait_object's own lock (bucket->wq.wobj.lock), which wo_pop_head_matching acquires.
    // The old code tested *uaddr under a SEPARATE bucket->lock and then released it BEFORE
    // wq_block_current linked the waiter (under wo->lock), so a wake landing in that gap
    // found an empty queue and was lost (latent on cooperative-uniprocessor, live on
    // SMP/PREEMPT). wait_object_prepare_futex does the test+link+BLOCKED atomically:
    //   - returns 1: current is linked, tagged with phys_addr (FUTEX-B key), and BLOCKED
    //     -> deschedule via wait_object_park_committed (no relink);
    //   - returns 0: *uaddr already changed -> nothing linked, return EAGAIN.
    // bucket->lock is no longer needed and has been removed.
    if (!wait_object_prepare_futex(&bucket->wq.wobj, uaddr, val, phys_addr)) {
        return EAGAIN;  // value mismatch -> do not sleep
    }

    wait_object_park_committed(&bucket->wq.wobj);

    // Resumed here after wakeup. Clear the futex key so a later non-futex block
    // (waitpid/sleep) is never mistaken for a futex waiter.
    process_t* self = process_get_current();
    if (self) self->futex_key = 0;
    return 0;
}

// FUTEX_WAKE: wake up to N waiters
// Args: uaddr = futex address, nr_wake = number of waiters to wake
// Returns: number of waiters woken
static int64_t futex_wake(int* uaddr, int nr_wake) {
    // Validate address
    int ret = futex_validate_addr(uaddr);
    if (ret != 0) {
        return ret;
    }

    // Get physical address for hashing
    uint64_t phys_addr = futex_get_phys_addr(uaddr);
    if (phys_addr == 0) {
        return EFAULT;  // Unmapped address
    }

    // Hash to bucket
    uint32_t bucket_idx = futex_hash(phys_addr);
    futex_bucket_t* bucket = &futex_table[bucket_idx];

    // Wake up to nr_wake waiters PARKED ON THIS ADDRESS. wq_wake_one_key filters by
    // process_t.futex_key == phys_addr, so a waiter that hash-collided into this bucket
    // on a different address is skipped (left blocked) rather than spuriously woken
    // (FUTEX-B). INT32_MAX is the broadcast (wake-all-on-this-key) case.
    int woken = 0;

    if (nr_wake == INT32_MAX) {
        // Wake all waiters on this key (not the whole bucket).
        while (wq_wake_one_key(&bucket->wq, phys_addr) != NULL) {
            woken++;
        }
    } else if (nr_wake >= 1) {
        for (int i = 0; i < nr_wake; i++) {
            if (wq_wake_one_key(&bucket->wq, phys_addr) == NULL) {
                break;  // No more matching waiters
            }
            woken++;
        }
    }

    return woken;
}

// SYS_FUTEX: futex syscall dispatcher
// Args:
//   uaddr = userspace futex address (int*)
//   op = futex operation (FUTEX_WAIT, FUTEX_WAKE, ...)
//   val = operation-specific value
//   timeout = timeout (unused for now)
//   uaddr2 = second address (unused for now)
//   val3 = operation-specific value (unused for now)
// Returns: operation-specific result, or negative errno on error
int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                  uint64_t timeout, uint64_t uaddr2, uint64_t val3) {
    (void)uaddr2;
    (void)val3;
    (void)timeout;

    // Ensure futex subsystem is initialized
    if (!futex_initialized) {
        futex_init();
    }

    int* futex_addr = (int*)uaddr;
    int futex_val = (int)val;
    int futex_op = (int)op;

    switch (futex_op) {
        case FUTEX_WAIT:
            return futex_wait(futex_addr, futex_val);

        case FUTEX_WAKE:
            return futex_wake(futex_addr, futex_val);

        case FUTEX_WAIT_TIMEOUT:
            // TODO: Implement timeout support. Until then this is a defined
            // "unsupported" result (futextest asserts ENOTSUP), so DO NOT
            // change the return value. The per-call kprintf was removed: it
            // spammed the serial log on every invocation (futextest probes
            // this op repeatedly) and drowned out real boot output.
            return ENOTSUP;

        default:
            kprintf("[FUTEX] Invalid futex operation: %d\n", futex_op);
            return EINVAL;
    }
}
