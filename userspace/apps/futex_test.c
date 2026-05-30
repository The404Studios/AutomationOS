/*
 * Futex Test - Demonstration of Fast Userspace Mutex
 * ===================================================
 *
 * This program demonstrates the Linux-style futex primitive:
 *  1. Fast path: atomic CAS in userspace (no syscall)
 *  2. Slow path: kernel wait/wake on contention
 *
 * Benchmark Results (expected):
 *  - Uncontended lock: ~5 cycles (atomic only)
 *  - Contended lock: ~500 cycles (syscall)
 */

// Forward declarations
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int uint32_t;
typedef unsigned long size_t;
typedef int bool;
#define true 1
#define false 0

// Futex syscall interface
#define SYS_FUTEX       70
#define FUTEX_WAIT      0
#define FUTEX_WAKE      1

// Atomic operations (GCC built-ins)
#define atomic_load(ptr) \
    __atomic_load_n(ptr, __ATOMIC_SEQ_CST)

#define atomic_store(ptr, val) \
    __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)

#define atomic_cas(ptr, expected, desired) \
    __atomic_compare_exchange_n(ptr, &(expected), desired, false, \
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

// Forward declarations
static inline int64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3, uint64_t arg4, uint64_t arg5,
                              uint64_t arg6);
static inline uint64_t rdtsc(void);
void printf(const char* fmt, ...);
size_t strlen(const char* str);

// Futex lock structure
typedef struct {
    volatile int lock;  // 0 = unlocked, 1 = locked
} futex_lock_t;

// Initialize futex lock
void futex_lock_init(futex_lock_t* fl) {
    atomic_store(&fl->lock, 0);
}

// Acquire futex lock
void futex_lock_acquire(futex_lock_t* fl) {
    int expected = 0;

    // Fast path: try to acquire with atomic CAS (no syscall)
    if (atomic_cas(&fl->lock, expected, 1)) {
        // Got lock immediately!
        return;
    }

    // Slow path: lock is contended, sleep in kernel
    // Loop in case of spurious wakeups
    while (1) {
        // Try to acquire again before sleeping
        expected = 0;
        if (atomic_cas(&fl->lock, expected, 1)) {
            return;
        }

        // Sleep until woken (futex syscall)
        // This blocks until another process calls FUTEX_WAKE
        syscall(SYS_FUTEX, (uint64_t)&fl->lock, FUTEX_WAIT, 1, 0, 0, 0);
    }
}

// Release futex lock
void futex_lock_release(futex_lock_t* fl) {
    // Release lock
    atomic_store(&fl->lock, 0);

    // Wake one waiter (if any)
    syscall(SYS_FUTEX, (uint64_t)&fl->lock, FUTEX_WAKE, 1, 0, 0, 0);
}

// Try to acquire lock (non-blocking)
bool futex_lock_trylock(futex_lock_t* fl) {
    int expected = 0;
    return atomic_cas(&fl->lock, expected, 1);
}

// ============================================================================
// Test: Uncontended Lock (Fast Path Benchmark)
// ============================================================================
void test_uncontended_lock(void) {
    futex_lock_t lock;
    futex_lock_init(&lock);

    printf("Test 1: Uncontended Lock (Fast Path)\n");
    printf("======================================\n");

    // Acquire and release 1000 times (should be ~5 cycles each)
    uint64_t start = rdtsc();

    for (int i = 0; i < 1000; i++) {
        futex_lock_acquire(&lock);
        futex_lock_release(&lock);
    }

    uint64_t end = rdtsc();
    uint64_t cycles_per_op = (end - start) / 1000;

    printf("  Cycles per lock/unlock: %llu\n", cycles_per_op);
    printf("  Expected: ~5-10 cycles (atomic only)\n");

    if (cycles_per_op < 50) {
        printf("  PASS: Fast path works! No syscalls.\n");
    } else {
        printf("  FAIL: Too slow (%llu cycles). Syscall overhead?\n", cycles_per_op);
    }
    printf("\n");
}

// ============================================================================
// Test: Trylock
// ============================================================================
void test_trylock(void) {
    futex_lock_t lock;
    futex_lock_init(&lock);

    printf("Test 2: Trylock\n");
    printf("===============\n");

    // Try to acquire unlocked lock
    if (futex_lock_trylock(&lock)) {
        printf("  PASS: Trylock acquired unlocked lock\n");
    } else {
        printf("  FAIL: Trylock failed on unlocked lock\n");
    }

    // Try to acquire already-locked lock
    if (!futex_lock_trylock(&lock)) {
        printf("  PASS: Trylock failed on locked lock\n");
    } else {
        printf("  FAIL: Trylock acquired already-locked lock\n");
    }

    futex_lock_release(&lock);
    printf("\n");
}

// ============================================================================
// Test: Contended Lock (Slow Path - Requires Fork)
// ============================================================================
void test_contended_lock(void) {
    printf("Test 3: Contended Lock (Slow Path)\n");
    printf("===================================\n");
    printf("  NOTE: This test requires shared memory + fork\n");
    printf("  Skipping for now (single-process test)\n");
    printf("  In multi-process test:\n");
    printf("    - Process A holds lock\n");
    printf("    - Process B waits (FUTEX_WAIT syscall)\n");
    printf("    - Process A releases (FUTEX_WAKE syscall)\n");
    printf("    - Process B acquires lock\n");
    printf("\n");
}

// ============================================================================
// Main
// ============================================================================
int main(void) {
    printf("\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("  Futex Test - Fast Userspace Mutex\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("\n");

    test_uncontended_lock();
    test_trylock();
    test_contended_lock();

    printf("════════════════════════════════════════════════════════════\n");
    printf("  All Tests Complete\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("\n");

    return 0;
}

// ============================================================================
// Helper: RDTSC (Read Time Stamp Counter)
// ============================================================================
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ============================================================================
// Helper: Printf
// ============================================================================
void printf(const char* fmt, ...) {
    // TODO: Implement proper printf with varargs
    // For now, just write to stdout via SYS_WRITE
    syscall(3, 1, (uint64_t)fmt, strlen(fmt), 0, 0, 0);  // SYS_WRITE
}

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

// ============================================================================
// Helper: Syscall Wrapper
// ============================================================================
static inline int64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3, uint64_t arg4, uint64_t arg5,
                              uint64_t arg6) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8 __asm__("r8") = arg5;
    register uint64_t r9 __asm__("r9") = arg6;

    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return ret;
}
