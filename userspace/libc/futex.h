/*
 * Futex Userspace Library - Fast Userspace Mutex
 * ===============================================
 *
 * Provides userspace-first locking with kernel fallback only on contention.
 * This is the Linux futex model: uncontended locks are ~5 cycles (atomic only),
 * contended locks fall back to kernel wait queues (~500 cycles).
 */

#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>
#include <stdbool.h>

// Futex syscall operations
#define FUTEX_WAIT      0   // Block until woken or value mismatch
#define FUTEX_WAKE      1   // Wake N waiters

// Futex lock structure
typedef struct {
    volatile int lock;  // 0 = unlocked, 1 = locked
} futex_lock_t;

// Atomic operations (GCC built-ins)
static inline int atomic_load(const int* ptr) {
    return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

static inline void atomic_store(int* ptr, int val) {
    __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST);
}

static inline bool atomic_cas(int* ptr, int expected, int desired) {
    return __atomic_compare_exchange_n(ptr, &expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Futex syscall wrapper
static inline long sys_futex(int* uaddr, int op, int val) {
    register long rax asm("rax") = 70;  // SYS_FUTEX
    register long rdi asm("rdi") = (long)uaddr;
    register long rsi asm("rsi") = op;
    register long rdx asm("rdx") = val;
    register long r10 asm("r10") = 0;
    register long r8 asm("r8") = 0;
    register long r9 asm("r9") = 0;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return rax;
}

// Initialize futex lock (set to unlocked)
static inline void futex_lock_init(futex_lock_t* fl) {
    atomic_store(&fl->lock, 0);
}

// Acquire futex lock (blocking)
static inline void futex_lock_acquire(futex_lock_t* fl) {
    // Fast path: try to acquire with atomic CAS (no syscall)
    if (atomic_cas(&fl->lock, 0, 1)) {
        // Got lock immediately!
        return;
    }

    // Slow path: lock is contended, sleep in kernel
    // Loop in case of spurious wakeups
    while (1) {
        // Try to acquire again before sleeping
        if (atomic_cas(&fl->lock, 0, 1)) {
            return;
        }

        // Sleep until woken (futex syscall)
        sys_futex(&fl->lock, FUTEX_WAIT, 1);
    }
}

// Release futex lock
static inline void futex_lock_release(futex_lock_t* fl) {
    // Release lock
    atomic_store(&fl->lock, 0);

    // Wake one waiter (if any)
    sys_futex(&fl->lock, FUTEX_WAKE, 1);
}

// Try to acquire lock (non-blocking)
// Returns true if acquired, false if already locked
static inline bool futex_lock_trylock(futex_lock_t* fl) {
    return atomic_cas(&fl->lock, 0, 1);
}

#endif /* FUTEX_H */
