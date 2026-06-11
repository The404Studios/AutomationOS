#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

// Spinlock structure (must be initialized to 0/unlocked)
typedef struct {
    volatile uint32_t lock;
#ifdef SPINLOCK_DEBUG
    uint32_t owner_cpu;             // CPU that owns the lock (debug only)
    const char* name;               // Lock name for debugging (debug only)
#endif
} spinlock_t;

// Initialize spinlock
static inline void spin_lock_init(spinlock_t* lock) {
    lock->lock = 0;
#ifdef SPINLOCK_DEBUG
    lock->owner_cpu = 0xFFFFFFFF;
    lock->name = NULL;
#endif
}

// Initialize named spinlock
static inline void spin_lock_init_named(spinlock_t* lock, const char* name) {
    lock->lock = 0;
#ifdef SPINLOCK_DEBUG
    lock->owner_cpu = 0xFFFFFFFF;
    lock->name = name;
#else
    (void)name;
#endif
}

// Acquire spinlock (busy-wait until acquired)
static inline void spin_lock(spinlock_t* lock) {
    while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        // Busy-wait (pause for x86 to reduce memory traffic)
        __asm__ volatile("pause" ::: "memory");
    }
}

// Try to acquire spinlock (non-blocking)
// Returns true if acquired, false if already locked
static inline bool spin_trylock(spinlock_t* lock) {
    return !__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE);
}

// Release spinlock
static inline void spin_unlock(spinlock_t* lock) {
#ifdef SPINLOCK_DEBUG
    lock->owner_cpu = 0xFFFFFFFF;
#endif
    __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
}

// Check if spinlock is held (for debugging)
static inline bool spin_is_locked(spinlock_t* lock) {
    return __atomic_load_n(&lock->lock, __ATOMIC_RELAXED) != 0;
}

// IRQ-safe spinlock operations
// These save and restore interrupt flags

// Save RFLAGS and disable interrupts
static inline uint64_t save_flags_cli(void) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

// Restore RFLAGS
static inline void restore_flags(uint64_t flags) {
    __asm__ volatile(
        "push %0\n"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

// Acquire spinlock with interrupts disabled
static inline void spin_lock_irqsave(spinlock_t* lock, uint64_t* flags) {
    *flags = save_flags_cli();
    spin_lock(lock);
}

// Release spinlock and restore interrupts
static inline void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    restore_flags(flags);
}

// Acquire spinlock with interrupts disabled (no flag save)
static inline void spin_lock_irq(spinlock_t* lock) {
    __asm__ volatile("cli" ::: "memory");
    spin_lock(lock);
}

// Release spinlock with interrupts enabled (no flag restore)
static inline void spin_unlock_irq(spinlock_t* lock) {
    spin_unlock(lock);
    __asm__ volatile("sti" ::: "memory");
}

// Aliases: some subsystems (epoll) use the acquire/release naming.
static inline void spinlock_acquire(spinlock_t* lock) { spin_lock(lock); }
static inline void spinlock_release(spinlock_t* lock) { spin_unlock(lock); }
static inline void spinlock_init(spinlock_t* lock)    { spin_lock_init(lock); }

#endif
