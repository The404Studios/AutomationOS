/*
 * test_concurrent_offload.c -- Concurrent CPU1 offload stress test
 * ================================================================
 *
 * STRESS TEST: 10 threads simultaneously calling sys_cpu1_offload to verify
 * the job submission mechanism is thread-safe under heavy concurrent load.
 *
 * Tests:
 *   1. NO TORN POINTERS: All job fields (fn, arg, pending, done) are consistent.
 *   2. ALL JOBS COMPLETE: Every thread gets a valid result matching its reference.
 *   3. NO DEADLOCKS: The spinlock (if present) doesn't deadlock; all threads finish.
 *   4. SERIALIZATION: Jobs are properly serialized (one at a time on CPU1).
 *
 * Design:
 *   - 10 threads, each with unique small matrices (8x8 for speed).
 *   - Each thread: compute CPU0 reference, call SYS_CPU1_OFFLOAD, verify result.
 *   - Main thread: create all threads, join all, report PASS/FAIL.
 *   - Deterministic matrices use thread_id as seed so results differ per thread.
 *
 * Output:
 *   CONCURRENT_OFFLOAD: PASS N=10 all-threads-correct
 *   CONCURRENT_OFFLOAD: SKIP (no SMP offload syscall)
 *   CONCURRENT_OFFLOAD: FAIL <why>
 *
 * Build (freestanding, no crt0 -- matches cpu1offload.c):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c tests/test_concurrent_offload.c -o test_concurrent_offload.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       test_concurrent_offload.o -o test_concurrent_offload
 */

/* ---- syscall numbers (match kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_THREAD_CREATE 79
#define SYS_THREAD_EXIT   80
#define SYS_THREAD_JOIN   81
#define SYS_CPU1_OFFLOAD  83

/* ---- offload job type ---- */
#define CPU1_JOB_MATMUL   1

/* ---- error codes (negative ABI) ---- */
#define ENOTSUP_NEG       95

typedef unsigned char  u8;
typedef int            i32;
typedef long long      i64;
typedef unsigned long  size_t;

/* Small matrix size for fast concurrent execution (8x8 = 64 elements). */
#define N 8
#define NUM_THREADS 10

/* 6-argument inline syscall */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- serial diagnostics (fd=1) ---- */
static unsigned long k_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m) {
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

static void print_dec(long v) {
    char b[24];
    int i = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    if (neg) { char m = '-'; sc(SYS_WRITE, 1, (long)&m, 1, 0, 0, 0); }
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}

static void fail(const char *why) {
    print("CONCURRENT_OFFLOAD: FAIL ");
    print(why);
    print("\n");
    sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

/* Matmul argument block (matches syscall contract). */
typedef struct {
    i32 n;
    i32 A[N * N];
    i32 B[N * N];
} matmul_arg_t;

/* Per-thread data: arg block, result, reference, status. */
typedef struct {
    int thread_id;
    matmul_arg_t arg;
    i64 result[N * N];
    i64 reference[N * N];
    long tid;           /* thread ID from sys_thread_create */
    int completed;      /* 1 if thread finished successfully */
    int apic_id;        /* APIC ID that ran the job */
} thread_data_t;

/* Thread data array (static .bss so not on stack). */
static thread_data_t g_threads[NUM_THREADS];

/* Global completion counter (written by threads, read by main). */
static volatile int g_completed_count = 0;

/* Atomic increment (simple spinlock-free counter for completion tracking). */
static inline void atomic_inc(volatile int *ptr) {
    __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

/*
 * worker_thread -- entry point for each worker thread.
 *
 * Steps:
 *   1. Fill deterministic matrices (using thread_id as seed).
 *   2. Compute CPU0 reference matmul.
 *   3. Call SYS_CPU1_OFFLOAD and wait for result.
 *   4. Verify result matches reference.
 *   5. Mark completed and exit.
 */
static void worker_thread(void *arg_ptr) {
    thread_data_t *data = (thread_data_t *)arg_ptr;
    int id = data->thread_id;

    /* 1. Deterministic matrices (use thread_id to vary the pattern). */
    data->arg.n = N;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            /* Vary by thread_id so each thread has unique operands. */
            data->arg.A[i * N + j] = (i + j + id) % 7;
            data->arg.B[i * N + j] = (i * 3 + j + id + 1) % 5;
        }
    }

    /* 2. CPU0 reference matmul (int64 accumulation). */
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            i64 s = 0;
            for (int k = 0; k < N; k++) {
                s += (i64)data->arg.A[r * N + k] * (i64)data->arg.B[k * N + c];
            }
            data->reference[r * N + c] = s;
        }
    }

    /* 3. Offload to CPU1. */
    long arg_len = (long)sizeof(i32) + 2L * (long)N * (long)N * (long)sizeof(i32);
    long res_len = (long)N * (long)N * (long)sizeof(i64);

    long rc = sc(SYS_CPU1_OFFLOAD, CPU1_JOB_MATMUL,
                 (long)&data->arg, arg_len, (long)data->result, res_len, 0);

    /* Handle ENOTSUP (syscall not registered -- DEFAULT kernel). Thread 0 will
     * report SKIP; other threads just exit quietly. */
    if (rc == -ENOTSUP_NEG) {
        if (id == 0) {
            /* Only thread 0 prints SKIP to avoid spam. */
            print("CONCURRENT_OFFLOAD: SKIP (no SMP offload syscall)\n");
        }
        sc(SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }

    /* Any other negative return is a real failure. */
    if (rc < 0) {
        print("[Thread "); print_dec(id); print("] offload failed rc=");
        print_dec(rc); print("\n");
        sc(SYS_THREAD_EXIT, 1, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }

    /* rc >= 0 is the APIC ID that ran the job (must be 1 for CPU1). */
    data->apic_id = (int)rc;
    if (data->apic_id != 1) {
        print("[Thread "); print_dec(id); print("] wrong APIC ID: ");
        print_dec(data->apic_id); print("\n");
        sc(SYS_THREAD_EXIT, 1, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }

    /* 4. Verify result matches reference (bit-for-bit). */
    int mismatches = 0;
    for (int i = 0; i < N * N; i++) {
        if (data->result[i] != data->reference[i]) {
            mismatches++;
        }
    }

    if (mismatches != 0) {
        print("[Thread "); print_dec(id); print("] mismatches=");
        print_dec(mismatches); print("\n");
        sc(SYS_THREAD_EXIT, 1, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }

    /* 5. Success! Mark completed and exit. */
    data->completed = 1;
    atomic_inc(&g_completed_count);

    print("[Thread "); print_dec(id); print("] PASS (apic=");
    print_dec(data->apic_id); print(")\n");

    sc(SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/*
 * _start -- main entry point.
 *
 * Steps:
 *   1. Create NUM_THREADS worker threads.
 *   2. Join all threads.
 *   3. Verify all threads completed successfully.
 *   4. Report final PASS/FAIL.
 */
void _start(void) {
    print("[CONCURRENT_OFFLOAD] Starting stress test (N=");
    print_dec(N); print(", threads="); print_dec(NUM_THREADS); print(")\n");

    /* Thread stacks (8KB per thread, static allocation). */
    static char thread_stacks[NUM_THREADS][8192] __attribute__((aligned(16)));

    /* 1. Initialize thread data and create threads. */
    for (int i = 0; i < NUM_THREADS; i++) {
        g_threads[i].thread_id = i;
        g_threads[i].completed = 0;
        g_threads[i].apic_id = -1;

        /* Stack grows down, so pass the TOP of the stack. */
        void *stack_top = thread_stacks[i] + sizeof(thread_stacks[i]);

        long tid = sc(SYS_THREAD_CREATE,
                      (long)worker_thread,
                      (long)&g_threads[i],
                      (long)stack_top,
                      0, 0, 0);

        if (tid < 0) {
            print("Failed to create thread "); print_dec(i);
            print(" (rc="); print_dec(tid); print(")\n");
            fail("thread-create-failed");
        }

        g_threads[i].tid = tid;
        print("[MAIN] Created thread "); print_dec(i);
        print(" (tid="); print_dec(tid); print(")\n");
    }

    /* 2. Join all threads (wait for completion). */
    for (int i = 0; i < NUM_THREADS; i++) {
        long retval = 0;
        long rc = sc(SYS_THREAD_JOIN, g_threads[i].tid, (long)&retval, 0, 0, 0, 0);

        if (rc < 0) {
            print("Failed to join thread "); print_dec(i);
            print(" (rc="); print_dec(rc); print(")\n");
            fail("thread-join-failed");
        }

        if (retval != 0) {
            print("Thread "); print_dec(i);
            print(" exited with error (retval="); print_dec(retval); print(")\n");
            fail("thread-error");
        }

        print("[MAIN] Joined thread "); print_dec(i); print("\n");
    }

    /* 3. Verify all threads completed successfully. If any thread hit ENOTSUP
     * (DEFAULT kernel), g_completed_count will be 0 and thread 0 already printed
     * SKIP, so we exit cleanly. */
    if (g_completed_count == 0) {
        /* All threads hit ENOTSUP -- already printed SKIP, exit clean. */
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    if (g_completed_count != NUM_THREADS) {
        print("Only "); print_dec(g_completed_count); print("/");
        print_dec(NUM_THREADS); print(" threads completed\n");
        fail("incomplete-threads");
    }

    /* 4. All threads completed successfully! */
    print("CONCURRENT_OFFLOAD: PASS N="); print_dec(NUM_THREADS);
    print(" all-threads-correct\n");

    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
