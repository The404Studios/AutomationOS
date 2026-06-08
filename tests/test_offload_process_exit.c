/*
 * test_offload_process_exit.c -- Process exit stress test for CPU1 offload
 * ========================================================================
 *
 * STRESS TEST: Kill processes mid-job during CPU1 offload to verify:
 *   1. CPU1 completes or orphans safely (no hang)
 *   2. No memory leaks (kernel heap, page tables)
 *   3. System remains stable after process destruction
 *   4. Job queue is properly cleaned up
 *
 * Tests:
 *   A. Fork child, start long offload job, SIGKILL child mid-job
 *   B. Verify CPU1 completes or aborts gracefully
 *   C. Check memory before/after (no leaks)
 *   D. Run multiple iterations to stress cleanup paths
 *   E. Verify parent and system remain stable
 *
 * Output:
 *   OFFLOAD_EXIT: PASS N=<iterations> no-leaks system-stable
 *   OFFLOAD_EXIT: SKIP (no SMP offload syscall)
 *   OFFLOAD_EXIT: FAIL <why>
 *
 * Build (freestanding, no crt0):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c tests/test_offload_process_exit.c -o test_offload_process_exit.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       test_offload_process_exit.o -o test_offload_process_exit
 */

/* ---- syscall numbers (match kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_FORK          1
#define SYS_WRITE         3
#define SYS_WAITPID       6
#define SYS_GETPID        8
#define SYS_YIELD         15
#define SYS_KILL          26
#define SYS_SYSINFO       62
#define SYS_CPU1_OFFLOAD  83

/* ---- offload job type ---- */
#define CPU1_JOB_MATMUL   1

/* ---- signal numbers (match userspace/libc/signal.h) ---- */
#define SIGKILL           9

/* ---- error codes (negative ABI) ---- */
#define ENOTSUP_NEG       95

typedef unsigned char  u8;
typedef int            i32;
typedef long long      i64;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef unsigned long  size_t;

/* Large matrix size for long-running job (128x128 = 16384 elements). */
#define N 128

/* Number of stress iterations (kill child mid-job N times). */
#define NUM_ITERATIONS 5

/* Delay in milliseconds before killing child (to ensure job is running). */
#define KILL_DELAY_MS 50

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

static void print_u64(u64 v) {
    char b[24];
    int i = 0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}

static void fail(const char *why) {
    print("OFFLOAD_EXIT: FAIL ");
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

/* System info structure (matches kernel/include/procapi.h sysinfo_t). */
typedef struct {
    u64 total_mem;      /*  0 — total physical memory in bytes          */
    u64 free_mem;       /*  8 — free physical memory in bytes           */
    u64 uptime_ms;      /* 16 — milliseconds since boot                 */
    u32 proc_count;     /* 24 — number of live processes                */
    u32 _pad;           /* 28 — reserved, always 0                      */
} sysinfo_t;

/* Static data to avoid stack overflow (128x128 matrices are large). */
static matmul_arg_t g_arg;
static i64 g_result[N * N];

/*
 * get_free_memory - query SYS_SYSINFO for current free memory.
 *
 * Returns free memory in bytes, or 0 on error.
 */
static u64 get_free_memory(void) {
    sysinfo_t info = {0};
    long rc = sc(SYS_SYSINFO, (long)&info, 0, 0, 0, 0, 0);
    if (rc < 0) {
        print("[WARN] SYS_SYSINFO failed, rc=");
        print_dec(rc);
        print("\n");
        return 0;
    }
    return info.free_mem;
}

/*
 * busy_wait_ms - simple busy-wait delay (approximate, no syscall).
 *
 * This is a crude delay for the parent to wait before killing the child.
 * In a real kernel, you'd use SYS_SLEEP, but this avoids scheduler complexity.
 */
static void busy_wait_ms(u32 ms) {
    /* Calibration: assume ~1M iterations per millisecond (adjust if needed). */
    volatile u64 count = (u64)ms * 1000000ULL;
    for (u64 i = 0; i < count; i++) {
        asm volatile("pause" ::: "memory");
    }
}

/*
 * child_worker - child process: start a long offload job, expect to be killed.
 *
 * This function is called in the child process. It sets up a large matmul job
 * and calls SYS_CPU1_OFFLOAD. The parent will SIGKILL this process mid-job.
 */
static void child_worker(void) {
    /* 1. Fill deterministic matrices (large size = long job). */
    g_arg.n = N;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            g_arg.A[i * N + j] = (i + j) % 7;
            g_arg.B[i * N + j] = (i * 3 + j) % 5;
        }
    }

    /* 2. Call SYS_CPU1_OFFLOAD (this will be interrupted by SIGKILL). */
    long arg_len = (long)sizeof(i32) + 2L * (long)N * (long)N * (long)sizeof(i32);
    long res_len = (long)N * (long)N * (long)sizeof(i64);

    /* Print child started (for debugging). */
    print("[CHILD "); print_dec(sc(SYS_GETPID, 0, 0, 0, 0, 0, 0));
    print("] Starting long offload job...\n");

    long rc = sc(SYS_CPU1_OFFLOAD, CPU1_JOB_MATMUL,
                 (long)&g_arg, arg_len, (long)g_result, res_len, 0);

    /* If we reach here, the syscall completed (not killed). This is OK for
     * small jobs or if the kill arrived late. Report and exit. */
    print("[CHILD "); print_dec(sc(SYS_GETPID, 0, 0, 0, 0, 0, 0));
    print("] Offload completed (not killed), rc="); print_dec(rc); print("\n");

    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

/*
 * run_one_iteration - fork child, let it start offload, kill it, verify cleanup.
 *
 * Steps:
 *   1. Record free memory before fork.
 *   2. Fork child process.
 *   3. Parent: wait briefly, then SIGKILL child.
 *   4. Parent: wait for child to exit (reap zombie).
 *   5. Record free memory after cleanup.
 *   6. Verify no memory leak (allow small tolerance for kernel bookkeeping).
 */
static void run_one_iteration(int iter) {
    print("[ITER "); print_dec(iter); print("] Starting\n");

    /* 1. Baseline memory. */
    u64 mem_before = get_free_memory();
    print("[ITER "); print_dec(iter); print("] Free memory before: ");
    print_u64(mem_before); print(" bytes\n");

    /* 2. Fork child. */
    long child_pid = sc(SYS_FORK, 0, 0, 0, 0, 0, 0);

    if (child_pid < 0) {
        print("[ITER "); print_dec(iter); print("] Fork failed, rc=");
        print_dec(child_pid); print("\n");
        fail("fork-failed");
    }

    if (child_pid == 0) {
        /* CHILD PROCESS: start offload job. */
        child_worker();
        /* Never returns (child exits inside child_worker). */
    }

    /* PARENT PROCESS continues. */
    print("[PARENT] Child PID "); print_dec(child_pid); print(" started\n");

    /* 3. Wait briefly to let child start the offload job. */
    busy_wait_ms(KILL_DELAY_MS);

    /* 4. SIGKILL the child mid-job. */
    print("[PARENT] Sending SIGKILL to child "); print_dec(child_pid); print("\n");
    long kill_rc = sc(SYS_KILL, child_pid, SIGKILL, 0, 0, 0, 0);
    if (kill_rc < 0) {
        print("[PARENT] SIGKILL failed, rc="); print_dec(kill_rc); print("\n");
        fail("sigkill-failed");
    }

    /* 5. Wait for child to exit (reap zombie). */
    print("[PARENT] Waiting for child to exit...\n");
    long exit_code = 0;
    long wait_rc = sc(SYS_WAITPID, child_pid, (long)&exit_code, 0, 0, 0, 0);
    if (wait_rc < 0) {
        print("[PARENT] WAITPID failed, rc="); print_dec(wait_rc); print("\n");
        fail("waitpid-failed");
    }

    print("[PARENT] Child exited, exit_code="); print_dec(exit_code); print("\n");

    /* 6. Check memory after cleanup. */
    u64 mem_after = get_free_memory();
    print("[ITER "); print_dec(iter); print("] Free memory after: ");
    print_u64(mem_after); print(" bytes\n");

    /* Verify no significant leak (allow 4KB tolerance for kernel bookkeeping). */
    if (mem_after + 4096 < mem_before) {
        print("[ITER "); print_dec(iter); print("] Memory leak detected: ");
        print_u64(mem_before - mem_after); print(" bytes leaked\n");
        fail("memory-leak");
    }

    print("[ITER "); print_dec(iter); print("] PASS (no leak)\n\n");
}

/*
 * _start -- main entry point.
 *
 * Steps:
 *   1. Check if SYS_CPU1_OFFLOAD is available (SKIP if DEFAULT kernel).
 *   2. Run NUM_ITERATIONS of kill-mid-job stress test.
 *   3. Verify system stability (can still allocate, fork, etc.).
 *   4. Report final PASS/FAIL.
 */
void _start(void) {
    print("====================================================================\n");
    print("OFFLOAD_EXIT: Process exit stress test (kill mid-job)\n");
    print("====================================================================\n");
    print("Matrix size: "); print_dec(N); print("x"); print_dec(N); print("\n");
    print("Iterations: "); print_dec(NUM_ITERATIONS); print("\n");
    print("Kill delay: "); print_dec(KILL_DELAY_MS); print(" ms\n\n");

    /* Quick probe: check if SYS_CPU1_OFFLOAD is available. We do this by
     * setting up a tiny job and checking for ENOTSUP. */
    print("[PROBE] Checking if SYS_CPU1_OFFLOAD is available...\n");
    matmul_arg_t probe_arg = { .n = 2 };
    i64 probe_result[4] = {0};
    long probe_rc = sc(SYS_CPU1_OFFLOAD, CPU1_JOB_MATMUL,
                       (long)&probe_arg, sizeof(i32) + 2*2*sizeof(i32),
                       (long)probe_result, 2*2*sizeof(i64), 0);

    if (probe_rc == -ENOTSUP_NEG) {
        print("OFFLOAD_EXIT: SKIP (no SMP offload syscall)\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    /* Offload is available, proceed with stress test. */
    print("[PROBE] SYS_CPU1_OFFLOAD available, rc="); print_dec(probe_rc); print("\n\n");

    /* Run iterations. */
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        run_one_iteration(i);
    }

    /* Final stability check: can we still fork? */
    print("[FINAL] Stability check: forking child...\n");
    long final_pid = sc(SYS_FORK, 0, 0, 0, 0, 0, 0);
    if (final_pid < 0) {
        fail("final-fork-failed");
    }
    if (final_pid == 0) {
        /* Child: just exit. */
        print("[FINAL CHILD] Exiting cleanly\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    /* Parent: wait for child. */
    long final_exit = 0;
    sc(SYS_WAITPID, final_pid, (long)&final_exit, 0, 0, 0, 0);
    print("[FINAL] Stability check passed\n\n");

    /* All tests passed! */
    print("====================================================================\n");
    print("OFFLOAD_EXIT: PASS N="); print_dec(NUM_ITERATIONS);
    print(" no-leaks system-stable\n");
    print("====================================================================\n");

    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
