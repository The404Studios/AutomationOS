/*
 * Test case for automationos-lock-balance check
 *
 * This file demonstrates good and bad patterns for lock operations.
 */

// Simulated lock types and functions
typedef struct spinlock {
    int locked;
} spinlock_t;

void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
void mutex_lock(spinlock_t *lock);
void mutex_unlock(spinlock_t *lock);

// GOOD: Balanced lock/unlock
void locks_good_1(spinlock_t *lock) {
    spinlock_acquire(lock);

    // Critical section

    spinlock_release(lock);  // ✅ Balanced
}

// GOOD: Balanced with early return
void locks_good_2(spinlock_t *lock, int condition) {
    spinlock_acquire(lock);

    if (condition) {
        spinlock_release(lock);  // ✅ Released before return
        return;
    }

    spinlock_release(lock);  // ✅ Released on normal path
}

// GOOD: Multiple locks balanced
void locks_good_3(spinlock_t *lock1, spinlock_t *lock2) {
    spinlock_acquire(lock1);
    spinlock_acquire(lock2);

    // Critical section with both locks

    spinlock_release(lock2);  // ✅ Both released
    spinlock_release(lock1);  // ✅ In reverse order
}

// BAD: Missing unlock
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: function 'locks_bad_1' has unbalanced lock operations: 1 acquire(s), 0 release(s); may cause deadlock [automationos-lock-balance]
void locks_bad_1(spinlock_t *lock) {
    spinlock_acquire(lock);

    // Critical section

    // ❌ Missing spinlock_release(lock)!
}

// BAD: Early return without unlock
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: function 'locks_bad_2' has unbalanced lock operations [automationos-lock-balance]
void locks_bad_2(spinlock_t *lock, int error) {
    spinlock_acquire(lock);

    if (error) {
        return;  // ❌ Lock not released on this path!
    }

    spinlock_release(lock);  // Only released on non-error path
}

// BAD: Unbalanced multiple locks
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: function 'locks_bad_3' has unbalanced lock operations [automationos-lock-balance]
void locks_bad_3(spinlock_t *lock1, spinlock_t *lock2) {
    spinlock_acquire(lock1);
    spinlock_acquire(lock2);

    spinlock_release(lock1);  // ❌ lock2 never released!
}

// BAD: Double unlock
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: function 'locks_bad_4' has unbalanced lock operations: 1 acquire(s), 2 release(s) [automationos-lock-balance]
void locks_bad_4(spinlock_t *lock) {
    spinlock_acquire(lock);

    spinlock_release(lock);
    spinlock_release(lock);  // ❌ Double unlock!
}

// BAD: Unlock without acquire
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: function 'locks_bad_5' has unbalanced lock operations: 0 acquire(s), 1 release(s) [automationos-lock-balance]
void locks_bad_5(spinlock_t *lock) {
    // ❌ No acquire, but unlock called
    spinlock_release(lock);
}

// GOOD: Mutex lock balanced
void mutex_good(spinlock_t *lock) {
    mutex_lock(lock);

    // Critical section

    mutex_unlock(lock);  // ✅ Balanced
}

// BAD: Mutex not unlocked on all paths
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: function 'mutex_bad' has unbalanced lock operations [automationos-lock-balance]
void mutex_bad(spinlock_t *lock, int condition) {
    mutex_lock(lock);

    if (condition) {
        // ❌ No unlock before return
        return;
    }

    mutex_unlock(lock);
}

// GOOD: Lock in loop (balanced overall)
void locks_loop_good(spinlock_t *lock, int count) {
    for (int i = 0; i < count; i++) {
        spinlock_acquire(lock);

        // Process item i

        spinlock_release(lock);  // ✅ Released in each iteration
    }
}

// BAD: Lock in loop, unlock outside (imbalanced)
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: function 'locks_loop_bad' has unbalanced lock operations [automationos-lock-balance]
void locks_loop_bad(spinlock_t *lock, int count) {
    for (int i = 0; i < count; i++) {
        spinlock_acquire(lock);

        // Process item i

        // ❌ Lock acquired N times, but only released once!
    }

    spinlock_release(lock);  // Only one unlock for N acquires
}
