/*
 * Simple Futex Test - Uncontended Lock Benchmark
 * ===============================================
 *
 * This is a minimal test demonstrating futex fast path performance.
 * It measures the cost of uncontended lock/unlock operations.
 *
 * Expected: ~5-10 cycles per lock/unlock (atomic only, no syscalls)
 */

typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef int bool;
#define true 1
#define false 0

// Futex lock structure
typedef struct {
    volatile int lock;  // 0 = unlocked, 1 = locked
} futex_lock_t;

// Syscall numbers
#define SYS_WRITE 3
#define SYS_EXIT  0
#define SYS_FUTEX 70

// Futex operations
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

// ============================================================================
// Atomic Operations
// ============================================================================

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

// ============================================================================
// Syscall Wrapper
// ============================================================================

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    register uint64_t rax asm("rax") = num;
    register uint64_t rdi asm("rdi") = arg1;
    register uint64_t rsi asm("rsi") = arg2;
    register uint64_t rdx asm("rdx") = arg3;

    asm volatile(
        "syscall"
        : "=a"(ret)
        : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );

    return ret;
}

static inline int64_t syscall6(uint64_t num, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3, uint64_t arg4, uint64_t arg5,
                               uint64_t arg6) {
    int64_t ret;
    register uint64_t rax asm("rax") = num;
    register uint64_t rdi asm("rdi") = arg1;
    register uint64_t rsi asm("rsi") = arg2;
    register uint64_t rdx asm("rdx") = arg3;
    register uint64_t r10 asm("r10") = arg4;
    register uint64_t r8 asm("r8") = arg5;
    register uint64_t r9 asm("r9") = arg6;

    asm volatile(
        "syscall"
        : "=a"(ret)
        : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return ret;
}

// ============================================================================
// RDTSC - Read Time Stamp Counter
// ============================================================================

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ============================================================================
// Futex Lock API
// ============================================================================

static inline void futex_lock_init(futex_lock_t* fl) {
    atomic_store(&fl->lock, 0);
}

static inline void futex_lock_acquire(futex_lock_t* fl) {
    // Fast path: try to acquire with atomic CAS (no syscall)
    if (atomic_cas(&fl->lock, 0, 1)) {
        return;
    }

    // Slow path: lock is contended, sleep in kernel
    while (1) {
        if (atomic_cas(&fl->lock, 0, 1)) {
            return;
        }
        syscall6(SYS_FUTEX, (uint64_t)&fl->lock, FUTEX_WAIT, 1, 0, 0, 0);
    }
}

static inline void futex_lock_release(futex_lock_t* fl) {
    atomic_store(&fl->lock, 0);
    syscall6(SYS_FUTEX, (uint64_t)&fl->lock, FUTEX_WAKE, 1, 0, 0, 0);
}

static inline bool futex_lock_trylock(futex_lock_t* fl) {
    return atomic_cas(&fl->lock, 0, 1);
}

// ============================================================================
// Helper: Write String
// ============================================================================

static void write_str(const char* str) {
    int len = 0;
    while (str[len]) len++;
    syscall3(SYS_WRITE, 1, (uint64_t)str, len);
}

static void write_num(uint64_t num) {
    char buf[32];
    int i = 0;

    if (num == 0) {
        buf[i++] = '0';
    } else {
        char tmp[32];
        int j = 0;
        while (num > 0) {
            tmp[j++] = '0' + (num % 10);
            num /= 10;
        }
        while (j > 0) {
            buf[i++] = tmp[--j];
        }
    }

    syscall3(SYS_WRITE, 1, (uint64_t)buf, i);
}

// ============================================================================
// Main Test
// ============================================================================

int main(void) {
    futex_lock_t lock;
    futex_lock_init(&lock);

    write_str("\n");
    write_str("==========================================\n");
    write_str("  Futex Test - Fast Userspace Mutex\n");
    write_str("==========================================\n");
    write_str("\n");

    // Test 1: Uncontended Lock Performance
    write_str("Test 1: Uncontended Lock Performance\n");
    write_str("-------------------------------------\n");

    const int iterations = 1000;
    uint64_t start = rdtsc();

    for (int i = 0; i < iterations; i++) {
        futex_lock_acquire(&lock);
        futex_lock_release(&lock);
    }

    uint64_t end = rdtsc();
    uint64_t total_cycles = end - start;
    uint64_t cycles_per_op = total_cycles / iterations;

    write_str("  Iterations: ");
    write_num(iterations);
    write_str("\n");

    write_str("  Total cycles: ");
    write_num(total_cycles);
    write_str("\n");

    write_str("  Cycles per lock/unlock: ");
    write_num(cycles_per_op);
    write_str("\n");

    write_str("  Expected: ~5-10 cycles (atomic only)\n");
    write_str("\n");

    if (cycles_per_op < 50) {
        write_str("  PASS: Fast path works! No syscalls.\n");
    } else {
        write_str("  FAIL: Too slow (");
        write_num(cycles_per_op);
        write_str(" cycles). Syscall overhead?\n");
    }
    write_str("\n");

    // Test 2: Trylock
    write_str("Test 2: Trylock\n");
    write_str("----------------\n");

    futex_lock_init(&lock);

    if (futex_lock_trylock(&lock)) {
        write_str("  PASS: Trylock acquired unlocked lock\n");
    } else {
        write_str("  FAIL: Trylock failed on unlocked lock\n");
    }

    if (!futex_lock_trylock(&lock)) {
        write_str("  PASS: Trylock failed on locked lock\n");
    } else {
        write_str("  FAIL: Trylock acquired already-locked lock\n");
    }

    futex_lock_release(&lock);
    write_str("\n");

    write_str("==========================================\n");
    write_str("  All Tests Complete\n");
    write_str("==========================================\n");
    write_str("\n");

    syscall3(SYS_EXIT, 0, 0, 0);
    return 0;
}
