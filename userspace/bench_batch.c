/*
 * Batched Syscall Benchmark
 * =========================
 *
 * Measures syscall overhead reduction from batched syscall interface.
 * Compares 100 individual read() calls vs. 100 batched read() calls.
 *
 * Expected speedup: ~10-20x reduction in syscall overhead.
 */

#include "syscalls.h"
#include "libc.h"

/* Syscall number for batch submit */
#define SYS_BATCH_SUBMIT 82

/* Syscall request structure (must match kernel ABI) */
typedef struct {
    int      syscall_num;
    unsigned int reserved;
    uint64_t args[6];
} syscall_request_t;

/* Batch ring structure (must match kernel ABI) */
typedef struct {
    syscall_request_t* sq;
    int64_t*           cq;
    unsigned int       sq_size;
    unsigned int       cq_size;
} batch_ring_t;

/* Raw syscall wrapper for batch_submit */
static inline int64_t batch_submit(batch_ring_t* ring, uint64_t count) {
    int64_t ret;
    asm volatile(
        "mov %1, %%rdi\n\t"
        "mov %2, %%rsi\n\t"
        "mov %3, %%rax\n\t"
        "syscall\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)ring), "r"(count), "i"(SYS_BATCH_SUBMIT)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return ret;
}

/* Get monotonic timestamp in milliseconds */
static inline uint64_t get_ticks_ms(void) {
    int64_t ret;
    asm volatile(
        "mov $40, %%rax\n\t"  /* SYS_GET_TICKS_MS */
        "syscall\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        :
        : "rax", "rcx", "r11", "memory"
    );
    return (uint64_t)ret;
}

void _start(void) {
    /* Test parameters */
    const int NUM_SYSCALLS = 100;
    const int ITERATIONS = 10;

    print("Batched Syscall Benchmark\n");
    print("=========================\n\n");

    /* Allocate buffers for batch ring */
    syscall_request_t sq[NUM_SYSCALLS];
    int64_t cq[NUM_SYSCALLS];
    batch_ring_t ring = {
        .sq = sq,
        .cq = cq,
        .sq_size = NUM_SYSCALLS,
        .cq_size = NUM_SYSCALLS
    };

    /* Dummy buffer for read() syscalls */
    char dummy_buf[16];

    /*
     * Benchmark 1: Individual syscalls (100x read from invalid fd)
     * This measures baseline syscall overhead.
     */
    print("Benchmark 1: Individual syscalls...\n");
    uint64_t individual_total_ms = 0;

    for (int iter = 0; iter < ITERATIONS; iter++) {
        uint64_t start = get_ticks_ms();

        for (int i = 0; i < NUM_SYSCALLS; i++) {
            /* read(999, dummy_buf, 1) - will fail with EBADF, but that's OK */
            read(999, dummy_buf, 1);
        }

        uint64_t end = get_ticks_ms();
        individual_total_ms += (end - start);
    }

    print("  Individual: ");
    print_num(individual_total_ms);
    print(" ms total (");
    print_num(NUM_SYSCALLS * ITERATIONS);
    print(" syscalls)\n");

    /*
     * Benchmark 2: Batched syscalls (1 batch of 100 reads)
     * This measures reduced overhead from batching.
     */
    print("Benchmark 2: Batched syscalls...\n");
    uint64_t batched_total_ms = 0;

    for (int iter = 0; iter < ITERATIONS; iter++) {
        /* Prepare submission queue with 100 read() requests */
        for (int i = 0; i < NUM_SYSCALLS; i++) {
            sq[i].syscall_num = 2;  /* SYS_READ */
            sq[i].reserved = 0;
            sq[i].args[0] = 999;    /* fd (invalid) */
            sq[i].args[1] = (uint64_t)dummy_buf;
            sq[i].args[2] = 1;      /* count */
            sq[i].args[3] = 0;
            sq[i].args[4] = 0;
            sq[i].args[5] = 0;
        }

        uint64_t start = get_ticks_ms();

        /* Submit batch (1 syscall for 100 operations) */
        int64_t result = batch_submit(&ring, NUM_SYSCALLS);

        uint64_t end = get_ticks_ms();
        batched_total_ms += (end - start);

        /* Verify batch was executed */
        if (result != NUM_SYSCALLS) {
            print("ERROR: batch_submit returned ");
            print_num(result);
            print(" (expected ");
            print_num(NUM_SYSCALLS);
            print(")\n");
            exit(1);
        }
    }

    print("  Batched:    ");
    print_num(batched_total_ms);
    print(" ms total (");
    print_num(ITERATIONS);
    print(" batch syscalls)\n\n");

    /*
     * Calculate speedup
     */
    print("Results:\n");
    print("--------\n");
    print("Individual: ");
    print_num(individual_total_ms);
    print(" ms\n");
    print("Batched:    ");
    print_num(batched_total_ms);
    print(" ms\n");

    if (batched_total_ms > 0) {
        /* Calculate speedup (avoid division by zero) */
        uint64_t speedup = (individual_total_ms * 10) / batched_total_ms;
        print("Speedup:    ");
        print_num(speedup / 10);
        print(".");
        print_num(speedup % 10);
        print("x\n");

        /* Calculate overhead reduction percentage */
        uint64_t reduction = ((individual_total_ms - batched_total_ms) * 100) / individual_total_ms;
        print("Overhead reduction: ");
        print_num(reduction);
        print("%\n");
    }

    /*
     * Verify completion queue contents (all reads should fail with EBADF)
     */
    print("\nVerifying completion queue...\n");
    int failures = 0;
    for (int i = 0; i < NUM_SYSCALLS; i++) {
        /* EBADF = -9 (bad file descriptor) */
        if (cq[i] != -9) {
            print("WARNING: cq[");
            print_num(i);
            print("] = ");
            print_num(cq[i]);
            print(" (expected -9)\n");
            failures++;
        }
    }

    if (failures == 0) {
        print("All ");
        print_num(NUM_SYSCALLS);
        print(" results verified correctly!\n");
    } else {
        print("ERROR: ");
        print_num(failures);
        print(" results had unexpected values\n");
    }

    print("\nBenchmark complete!\n");
    exit(0);
}
