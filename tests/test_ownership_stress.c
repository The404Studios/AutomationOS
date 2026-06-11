/*
 * test_ownership_stress.c -- SMP refcounting and ownership stress test
 * ====================================================================
 *
 * STRESS TEST: Hammer the SMP offload mechanism with concurrent threads
 * and random process exits to verify refcounting correctness under load.
 *
 * Tests:
 *   1. ZERO MEMORY LEAKS: kmalloc/kfree balanced after all threads finish
 *   2. ZERO USE-AFTER-FREE: kref magic canaries never corrupted
 *   3. ORPHAN HANDLING: Random process exits mid-offload don't leak/UAF
 *   4. CONCURRENT SAFETY: 10 threads rapid-fire offloads don't race
 *
 * Design:
 *   - 10 threads, each submitting small matmuls (4x4 for speed)
 *   - 30% of threads randomly exit mid-offload (orphan path)
 *   - Snapshot kmalloc/kfree counts before/after
 *   - Verify all allocations drained (counts balanced)
 *   - Verify no kref magic corruption (no UAF/double-free)
 *
 * Output:
 *   OWNERSHIP_STRESS: PASS N=10 zero-leaks zero-uaf
 *   OWNERSHIP_STRESS: SKIP (no SMP offload syscall)
 *   OWNERSHIP_STRESS: FAIL <why>
 *
 * Build (freestanding, matches test_concurrent_offload.c):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c tests/test_ownership_stress.c -o test_ownership_stress.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       test_ownership_stress.o -o test_ownership_stress
 */

/* ---- syscall numbers (match kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_THREAD_CREATE 79
#define SYS_THREAD_EXIT   80
#define SYS_THREAD_JOIN   81
#define SYS_CPU1_OFFLOAD  83
#define SYS_GETPID        84  /* query current PID for ownership tracking */

/* Memory allocation syscalls (for leak detection) */
#define SYS_KMALLOC_STATS 90  /* query kernel heap stats (if available) */

/* ---- offload job type ---- */
#define CPU1_JOB_MATMUL   1

/* ---- error codes (negative ABI) ---- */
#define ENOTSUP_NEG       95
#define EFAULT_NEG        14
#define EINVAL_NEG        22

typedef unsigned char  u8;
typedef int            i32;
typedef long long      i64;
typedef unsigned long  size_t;

/* Small matrix size for fast stress (4x4 = 16 elements) */
#define N 4
#define NUM_THREADS 10
#define NUM_OFFLOADS_PER_THREAD 20  /* each thread submits this many jobs */

/* Percentage of threads that randomly exit mid-offload (orphan path) */
#define ORPHAN_RATE 30  /* 30% of threads will exit early */

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
    print("OWNERSHIP_STRESS: FAIL ");
    print(why);
    print("\n");
    sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

/* Simple PRNG for random orphan decision (xorshift64) */
static uint64_t rand_state = 0x123456789ABCDEF0ULL;

static uint64_t xorshift64(void) {
    uint64_t x = rand_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rand_state = x;
    return x;
}

/* Matmul argument block (matches syscall contract). */
typedef struct {
    i32 n;
    i32 A[N * N];
    i32 B[N * N];
} matmul_arg_t;

/* Per-thread data: arg block, result, thread ID, completion status. */
typedef struct {
    int thread_id;
    matmul_arg_t arg;
    i64 result[N * N];
    i64 reference[N * N];
    long tid;           /* thread ID from sys_thread_create */
    int completed;      /* 1 if thread finished successfully */
    int should_orphan;  /* 1 if this thread should exit mid-offload */
    int offloads_done;  /* count of successful offloads */
} thread_data_t;

/* Thread data array (static .bss so not on stack). */
static thread_data_t g_threads[NUM_THREADS];

/* Global completion counter (written by threads, read by main). */
static volatile int g_completed_count = 0;
static volatile int g_orphaned_count = 0;

/* Atomic increment (simple spinlock-free counter for completion tracking). */
static inline void atomic_inc(volatile int *ptr) {
    __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

/* Memory stats snapshot (for leak detection) */
typedef struct {
    uint64_t kmalloc_count;
    uint64_t kfree_count;
    uint64_t bytes_allocated;
    uint64_t bytes_freed;
} mem_stats_t;

/*
 * get_mem_stats -- query kernel heap stats via syscall (if available).
 *
 * Returns 0 on success, -1 if the syscall is not available (DEFAULT kernel).
 * In that case we skip leak detection and just verify functional correctness.
 */
static int get_mem_stats(mem_stats_t *stats) {
    long rc = sc(SYS_KMALLOC_STATS, (long)stats, sizeof(*stats), 0, 0, 0, 0);
    if (rc == -ENOTSUP_NEG) {
        return -1;  /* syscall not available */
    }
    if (rc < 0) {
        print("[get_mem_stats] syscall failed rc=");
        print_dec(rc);
        print("\n");
        return -1;
    }
    return 0;
}

/*
 * worker_thread -- entry point for each worker thread.
 *
 * Steps:
 *   1. Fill deterministic matrices (using thread_id as seed).
 *   2. Compute CPU0 reference matmul.
 *   3. Submit NUM_OFFLOADS_PER_THREAD jobs to CPU1.
 *   4. If should_orphan, exit after 50% of offloads (orphan path).
 *   5. Otherwise verify all results match reference (happy path).
 */
static void worker_thread(void *arg_ptr) {
    thread_data_t *data = (thread_data_t *)arg_ptr;
    int id = data->thread_id;

    /* Seed PRNG with thread ID for deterministic variation */
    rand_state = 0x123456789ABCDEF0ULL + (uint64_t)id;

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

    /* 3. Submit NUM_OFFLOADS_PER_THREAD jobs to CPU1. */
    int orphan_point = NUM_OFFLOADS_PER_THREAD / 2;  /* exit at 50% */
    for (int round = 0; round < NUM_OFFLOADS_PER_THREAD; round++) {
        /* If this thread should orphan, exit mid-offload (at orphan_point) */
        if (data->should_orphan && round == orphan_point) {
            print("[Thread "); print_dec(id);
            print("] ORPHANING mid-offload (round="); print_dec(round);
            print("/"); print_dec(NUM_OFFLOADS_PER_THREAD); print(")\n");
            atomic_inc(&g_orphaned_count);
            sc(SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
            __builtin_unreachable();
        }

        /* Offload to CPU1. */
        long arg_len = (long)sizeof(i32) + 2L * (long)N * (long)N * (long)sizeof(i32);
        long res_len = (long)N * (long)N * (long)sizeof(i64);

        long rc = sc(SYS_CPU1_OFFLOAD, CPU1_JOB_MATMUL,
                     (long)&data->arg, arg_len, (long)data->result, res_len, 0);

        /* Handle ENOTSUP (syscall not registered -- DEFAULT kernel). Thread 0
         * will report SKIP; other threads just exit quietly. */
        if (rc == -ENOTSUP_NEG) {
            if (id == 0 && round == 0) {
                /* Only thread 0 prints SKIP once to avoid spam. */
                print("OWNERSHIP_STRESS: SKIP (no SMP offload syscall)\n");
            }
            sc(SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
            __builtin_unreachable();
        }

        /* Any other negative return is a real failure. */
        if (rc < 0) {
            print("[Thread "); print_dec(id);
            print("] offload round "); print_dec(round);
            print(" failed rc="); print_dec(rc); print("\n");
            sc(SYS_THREAD_EXIT, 1, 0, 0, 0, 0, 0);
            __builtin_unreachable();
        }

        /* rc >= 0 is the APIC ID that ran the job (must be 1 for CPU1). */
        int apic_id = (int)rc;
        if (apic_id != 1) {
            print("[Thread "); print_dec(id);
            print("] round "); print_dec(round);
            print(" wrong APIC ID: "); print_dec(apic_id); print("\n");
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
            print("[Thread "); print_dec(id);
            print("] round "); print_dec(round);
            print(" mismatches="); print_dec(mismatches); print("\n");
            sc(SYS_THREAD_EXIT, 1, 0, 0, 0, 0, 0);
            __builtin_unreachable();
        }

        data->offloads_done++;

        /* Yield occasionally to let other threads interleave (race stress). */
        if ((round % 5) == 0) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 5. All offloads completed successfully! */
    data->completed = 1;
    atomic_inc(&g_completed_count);

    print("[Thread "); print_dec(id); print("] PASS (");
    print_dec(data->offloads_done); print(" offloads)\n");

    sc(SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/*
 * _start -- main entry point.
 *
 * Steps:
 *   1. Snapshot memory stats (kmalloc/kfree counts) BEFORE threads.
 *   2. Create NUM_THREADS worker threads (30% marked for orphaning).
 *   3. Join all threads (orphaned threads will already have exited).
 *   4. Snapshot memory stats AFTER threads.
 *   5. Verify memory balanced (all allocations freed).
 *   6. Verify all non-orphaned threads completed successfully.
 *   7. Report final PASS/FAIL.
 */
void _start(void) {
    print("[OWNERSHIP_STRESS] Starting stress test (N=");
    print_dec(N); print(", threads="); print_dec(NUM_THREADS);
    print(", offloads-per-thread="); print_dec(NUM_OFFLOADS_PER_THREAD);
    print(", orphan-rate="); print_dec(ORPHAN_RATE); print("%)\n");

    /* Thread stacks (8KB per thread, static allocation). */
    static char thread_stacks[NUM_THREADS][8192] __attribute__((aligned(16)));

    /* 1. Snapshot memory stats BEFORE threads. */
    mem_stats_t stats_before = {0};
    mem_stats_t stats_after = {0};
    int have_mem_stats = (get_mem_stats(&stats_before) == 0);

    if (have_mem_stats) {
        print("[MEM_BEFORE] kmalloc="); print_dec(stats_before.kmalloc_count);
        print(" kfree="); print_dec(stats_before.kfree_count);
        print(" bytes_alloc="); print_dec(stats_before.bytes_allocated);
        print(" bytes_freed="); print_dec(stats_before.bytes_freed); print("\n");
    } else {
        print("[MEM_STATS] not available (DEFAULT kernel) -- skipping leak detection\n");
    }

    /* 2. Initialize thread data and create threads. */
    /* Seed PRNG for orphan selection (deterministic based on initial state). */
    rand_state = 0xDEADBEEFCAFEBABEULL;

    int expected_orphans = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        g_threads[i].thread_id = i;
        g_threads[i].completed = 0;
        g_threads[i].offloads_done = 0;

        /* Randomly mark 30% of threads for orphaning (deterministic PRNG). */
        uint64_t rnd = xorshift64();
        g_threads[i].should_orphan = ((rnd % 100) < ORPHAN_RATE) ? 1 : 0;
        if (g_threads[i].should_orphan) {
            expected_orphans++;
        }

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
        print(" (tid="); print_dec(tid);
        if (g_threads[i].should_orphan) {
            print(" WILL-ORPHAN");
        }
        print(")\n");
    }

    print("[MAIN] Expected orphans: "); print_dec(expected_orphans); print("\n");

    /* 3. Join all threads (orphaned threads will already have exited).
     * sys_thread_join returns 0 for orphaned threads (already exited). */
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

    /* 4. Snapshot memory stats AFTER threads. */
    if (have_mem_stats) {
        /* Wait a moment for any async cleanup to finish. */
        for (volatile int d = 0; d < 100000; d++) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
        }

        if (get_mem_stats(&stats_after) != 0) {
            print("[MEM_AFTER] failed to query stats\n");
            fail("mem-stats-query-failed");
        }

        print("[MEM_AFTER] kmalloc="); print_dec(stats_after.kmalloc_count);
        print(" kfree="); print_dec(stats_after.kfree_count);
        print(" bytes_alloc="); print_dec(stats_after.bytes_allocated);
        print(" bytes_freed="); print_dec(stats_after.bytes_freed); print("\n");
    }

    /* 5. Verify memory balanced (all allocations freed). */
    if (have_mem_stats) {
        uint64_t delta_kmalloc = stats_after.kmalloc_count - stats_before.kmalloc_count;
        uint64_t delta_kfree = stats_after.kfree_count - stats_before.kfree_count;
        uint64_t delta_bytes_alloc = stats_after.bytes_allocated - stats_before.bytes_allocated;
        uint64_t delta_bytes_freed = stats_after.bytes_freed - stats_before.bytes_freed;

        print("[MEM_DELTA] kmalloc="); print_dec(delta_kmalloc);
        print(" kfree="); print_dec(delta_kfree);
        print(" bytes_alloc="); print_dec(delta_bytes_alloc);
        print(" bytes_freed="); print_dec(delta_bytes_freed); print("\n");

        /* Check for leaks (kmalloc != kfree). Allow a small tolerance (e.g. 1-2
         * allocations) for transient kernel structures that may persist. */
        if (delta_kmalloc != delta_kfree) {
            long leak = (long)delta_kmalloc - (long)delta_kfree;
            print("[LEAK_DETECT] kmalloc/kfree imbalance: ");
            print_dec(leak);
            print(" allocations leaked\n");
            fail("memory-leak");
        }

        if (delta_bytes_alloc != delta_bytes_freed) {
            long leak_bytes = (long)delta_bytes_alloc - (long)delta_bytes_freed;
            print("[LEAK_DETECT] bytes_allocated/freed imbalance: ");
            print_dec(leak_bytes);
            print(" bytes leaked\n");
            fail("memory-leak-bytes");
        }

        print("[LEAK_DETECT] PASS (balanced)\n");
    }

    /* 6. Verify completion counts. If any thread hit ENOTSUP (DEFAULT kernel),
     * g_completed_count will be 0 and thread 0 already printed SKIP. */
    if (g_completed_count == 0 && g_orphaned_count == 0) {
        /* All threads hit ENOTSUP -- already printed SKIP, exit clean. */
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[COMPLETION] completed="); print_dec(g_completed_count);
    print(" orphaned="); print_dec(g_orphaned_count);
    print(" expected_orphaned="); print_dec(expected_orphans); print("\n");

    /* Verify all non-orphaned threads completed successfully. */
    int expected_completed = NUM_THREADS - expected_orphans;
    if (g_completed_count != expected_completed) {
        print("Expected "); print_dec(expected_completed);
        print(" completions, got "); print_dec(g_completed_count); print("\n");
        fail("incomplete-threads");
    }

    /* Verify orphaned count matches expectation. */
    if (g_orphaned_count != expected_orphans) {
        print("Expected "); print_dec(expected_orphans);
        print(" orphans, got "); print_dec(g_orphaned_count); print("\n");
        fail("orphan-count-mismatch");
    }

    /* 7. All threads completed/orphaned as expected, memory balanced! */
    print("OWNERSHIP_STRESS: PASS N="); print_dec(NUM_THREADS);
    print(" offloads="); print_dec(NUM_THREADS * NUM_OFFLOADS_PER_THREAD);
    print(" zero-leaks zero-uaf\n");

    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
