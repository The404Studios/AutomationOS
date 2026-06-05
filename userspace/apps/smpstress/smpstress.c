/*
 * smpstress.c -- 2-CPU SMP dispatch stress harness (ring 3, freestanding).
 * ============================================================================
 *
 * The proving ground for the SMP scaling work: BEFORE any per-CPU scheduler /
 * ring-3 dispatch / IPI change lands, this repeatedly drives the existing CPU1
 * coprocessor path and proves it stays CORRECT under thousands of dispatches --
 * not merely that it boots. It is a STRESS superset of cpu1offload: instead of
 * one offload it does JOBS of them, varying the operands every iteration so a
 * stale / lost / cross-contaminated result is caught by the per-job verify.
 *
 * Each iteration:
 *   1. Build a fresh deterministic n x n int32 A,B seeded by the iteration index
 *      (so every job's correct answer differs -- a CPU1 that returned a previous
 *      job's result, or never woke, mismatches).
 *   2. Compute the CPU0 reference (int64 accumulation, identical arithmetic).
 *   3. Offload the WHOLE matmul to CPU1 via SYS_CPU1_OFFLOAD (job-id based: the
 *      kernel validates+copies and picks the function; CPU1 never sees a user
 *      pointer or user code). The syscall returns the apic id that ran it.
 *   4. Verify the result bit-for-bit and that CPU1 (apic 1) ran it.
 *
 * Counters proved: jobs, cpu1_jobs (apic==1), mismatches, timeouts (CPU1 wedged
 * / lost wakeup), errors. PASS requires every job to have run on CPU1 with the
 * exact result and ZERO timeouts.
 *
 * Output (single line for the smoke grep):
 *   SMPSTRESS: PASS jobs=2000 cpu1_jobs=2000 mismatches=0 timeouts=0 errors=0 checksum=...
 *   SMPSTRESS: SKIP single CPU            (DEFAULT kernel: SYS_CPU1_OFFLOAD unregistered)
 *   SMPSTRESS: FAIL jobs=... mismatches=... timeouts=...
 *
 * On the DEFAULT (single-core) kernel SYS_CPU1_OFFLOAD is NOT registered, so the
 * very first offload returns ENOTSUP (-95): the harness prints SKIP and exits
 * cleanly -- completely harmless in the default boot (no fault, no loop).
 *
 * Build: bare _start, no crt0 (mirrors cpu1offload/nettest).
 */

/* ---- syscall numbers (match kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_CPU1_OFFLOAD  83   /* gated: only registered under SMP_FOUNDATION */

/* ---- offload job-type selector (match syscall.h CPU1_JOB_MATMUL) ---- */
#define CPU1_JOB_MATMUL   1

/* ---- negated errno values the dispatcher returns (kernel/include/errno.h) ---- */
#define RC_ENOTSUP  (-95)   /* unregistered syscall # on the default kernel */
#define RC_EAGAIN   (-11)   /* CPU1 wedged / job timed out (lost wakeup)     */

typedef int        i32;
typedef long long  i64;

/* Small problem size (cheap matmul) so the test maximizes DISPATCH COUNT -- the
 * thing being stressed is the mailbox handoff repeatability, not arithmetic size.
 * n=16 is 4096 int64 mults/job; JOBS dispatches dominate the runtime. */
#define N     16
#define JOBS  2000

/* 6-argument inline syscall (args rdi/rsi/rdx/r10/r8/r9). */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny serial diagnostics (fd 1) ---- */
static unsigned long k_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }
static void print_dec(i64 v) {
    char b[24]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    do { b[i++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v > 0);
    if (neg) { char m = '-'; sc(SYS_WRITE, 1, (long)&m, 1, 0, 0, 0); }
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}

/* user-arg block the kernel expects: { int32 n; int32 A[n*n]; int32 B[n*n]; } */
typedef struct { i32 n; i32 A[N * N]; i32 B[N * N]; } matmul_arg_t;

static matmul_arg_t g_arg;          /* offload request block (.bss)        */
static i64 g_result[N * N];         /* offloaded result (int64) from CPU1  */
static i64 g_ref[N * N];            /* CPU0 reference result               */

void _start(void) {
    print("[SMPSTRESS] starting (n="); print_dec(N);
    print(" jobs=");  print_dec(JOBS); print(")\n");

    const long arg_len = (long)sizeof(i32) + 2L * (long)N * (long)N * (long)sizeof(i32);
    const long res_len = (long)N * (long)N * (long)sizeof(i64);

    i64 jobs_done = 0, cpu1_jobs = 0, mismatches = 0, timeouts = 0, errors = 0;
    i64 checksum = 0;

    for (i64 it = 0; it < JOBS; it++) {
        /* 1. Fresh per-iteration operands (small ints; int64 accumulator can't
         *    overflow). Seeding by `it` makes every job's answer different. */
        g_arg.n = N;
        for (int i = 0; i < N; i++) {
            for (int j = 0; j < N; j++) {
                g_arg.A[i * N + j] = (i + j + (int)it) % 7;
                g_arg.B[i * N + j] = (i * 3 + j + 1 + (int)it) % 5;
            }
        }

        /* 2. CPU0 reference (exact same arithmetic as the kernel's matmul). */
        for (int r = 0; r < N; r++) {
            for (int c = 0; c < N; c++) {
                i64 s = 0;
                for (int k = 0; k < N; k++)
                    s += (i64)g_arg.A[r * N + k] * (i64)g_arg.B[k * N + c];
                g_ref[r * N + c] = s;
            }
        }

        /* 3. Offload to CPU1. Retry a few times on EAGAIN to absorb transient
         *    contention (another process holding the single job slot) before
         *    counting a hard timeout. */
        long rc = 0;
        for (int tries = 0; ; tries++) {
            rc = sc(SYS_CPU1_OFFLOAD, CPU1_JOB_MATMUL,
                    (long)&g_arg, arg_len, (long)g_result, res_len, 0);
            if (it == 0 && rc == RC_ENOTSUP) {           /* default kernel: no CPU1 */
                print("SMPSTRESS: SKIP single CPU\n");
                sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
                for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
            }
            if (rc == RC_EAGAIN && tries < 3) { sc(SYS_YIELD, 0, 0, 0, 0, 0, 0); continue; }
            break;
        }
        if (rc == RC_EAGAIN) { timeouts++; continue; }   /* CPU1 wedged after retries */
        if (rc < 0)          { errors++;   continue; }   /* EINVAL/EFAULT/ENOMEM */

        long by_apic = rc;
        if (by_apic == 1) cpu1_jobs++;

        /* 4. Verify bit-for-bit against the reference. */
        int bad = 0;
        for (int i = 0; i < N * N; i++) {
            if (g_result[i] != g_ref[i]) { bad = 1; break; }
        }
        if (bad) mismatches++;

        checksum += g_result[0] + g_result[N * N - 1];
        jobs_done++;

        if ((it & 0x1FF) == 0x1FF) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);  /* be cooperative */
    }

    /* PASS iff every job ran on CPU1 with the exact result and nothing timed out. */
    int ok = (mismatches == 0 && timeouts == 0 && errors == 0 &&
              jobs_done == JOBS && cpu1_jobs == JOBS);

    print("SMPSTRESS: ");
    print(ok ? "PASS" : "FAIL");
    print(" jobs=");        print_dec(jobs_done);
    print(" cpu1_jobs=");   print_dec(cpu1_jobs);
    print(" mismatches=");  print_dec(mismatches);
    print(" timeouts=");    print_dec(timeouts);
    print(" errors=");      print_dec(errors);
    print(" checksum=");    print_dec(checksum);
    print("\n");

    sc(SYS_EXIT, ok ? 0 : 1, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
