/*
 * test_rapid_offload.c -- Rapid-fire CPU1 offload stress test
 * ============================================================
 *
 * STRESS TEST: One thread issuing 100 sequential offloads in a tight loop to
 * verify job slot reuse is race-free and ownership tracking is sound.
 *
 * Tests for:
 *   1. NO OWNERSHIP ASSERTION PANICS: own_transition() doesn't panic on slot reuse.
 *   2. NO UAF CRASHES: No use-after-free on arg/result buffers.
 *   3. JOB SLOT LOCK PREVENTS CLOBBERING: cpu1_job_lock serializes slot writes.
 *   4. ALL JOBS COMPLETE: Every iteration gets a valid result.
 *
 * Design:
 *   - Simple sequential loop: 100 offloads, each with unique input.
 *   - Small matrices (4x4) for speed.
 *   - Compute CPU0 reference, call SYS_CPU1_OFFLOAD, verify result, repeat.
 *   - Count successes and report pass rate.
 *
 * Output:
 *   RAPID_OFFLOAD: PASS 100/100 successful
 *   RAPID_OFFLOAD: SKIP (no SMP offload syscall)
 *   RAPID_OFFLOAD: FAIL <N>/100 (first failure at iteration <i>)
 *
 * Build (freestanding, no crt0 -- matches cpu1offload.c):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c tests/test_rapid_offload.c -o test_rapid_offload.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       test_rapid_offload.o -o test_rapid_offload
 */

/* ---- syscall numbers (match kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_CPU1_OFFLOAD  83

/* ---- offload job type ---- */
#define CPU1_JOB_MATMUL   1

/* ---- error codes (negative ABI) ---- */
#define ENOTSUP_NEG       95

typedef unsigned char  u8;
typedef int            i32;
typedef long long      i64;
typedef unsigned long  size_t;

/* Small matrix size for fast sequential execution (4x4 = 16 elements). */
#define N 4
#define NUM_ITERATIONS 100

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

static void fail(const char *why, int iter) {
    print("RAPID_OFFLOAD: FAIL ");
    print(why);
    if (iter >= 0) {
        print(" at iteration ");
        print_dec(iter);
    }
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

/* Static data buffers (avoid stack issues in tight loop). */
static matmul_arg_t g_arg;
static i64 g_result[N * N];
static i64 g_reference[N * N];

/*
 * do_one_offload -- perform one offload iteration with unique input.
 *
 * Vary the matrix content by iteration index so each offload has unique
 * operands (helps catch clobbering/stale-data bugs).
 *
 * Returns 0 on success, -1 on failure (after printing diagnostic).
 */
static int do_one_offload(int iter) {
    /* 1. Deterministic matrices (vary by iteration index). */
    g_arg.n = N;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            /* Unique pattern per iteration. */
            g_arg.A[i * N + j] = (i + j + iter) % 11;
            g_arg.B[i * N + j] = (i * 2 + j + iter + 1) % 7;
        }
    }

    /* 2. CPU0 reference matmul (int64 accumulation). */
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            i64 s = 0;
            for (int k = 0; k < N; k++) {
                s += (i64)g_arg.A[r * N + k] * (i64)g_arg.B[k * N + c];
            }
            g_reference[r * N + c] = s;
        }
    }

    /* 3. Offload to CPU1. */
    long arg_len = (long)sizeof(i32) + 2L * (long)N * (long)N * (long)sizeof(i32);
    long res_len = (long)N * (long)N * (long)sizeof(i64);

    long rc = sc(SYS_CPU1_OFFLOAD, CPU1_JOB_MATMUL,
                 (long)&g_arg, arg_len, (long)g_result, res_len, 0);

    /* Handle ENOTSUP (syscall not registered -- DEFAULT kernel). */
    if (rc == -ENOTSUP_NEG) {
        return -ENOTSUP_NEG;  /* Signal to caller to skip test. */
    }

    /* Any other negative return is a real failure. */
    if (rc < 0) {
        print("[Iter "); print_dec(iter); print("] offload failed rc=");
        print_dec(rc); print("\n");
        return -1;
    }

    /* rc >= 0 is the APIC ID that ran the job (must be 1 for CPU1). */
    int apic_id = (int)rc;
    if (apic_id != 1) {
        print("[Iter "); print_dec(iter); print("] wrong APIC ID: ");
        print_dec(apic_id); print("\n");
        return -1;
    }

    /* 4. Verify result matches reference (bit-for-bit). */
    int mismatches = 0;
    for (int i = 0; i < N * N; i++) {
        if (g_result[i] != g_reference[i]) {
            mismatches++;
        }
    }

    if (mismatches != 0) {
        print("[Iter "); print_dec(iter); print("] mismatches=");
        print_dec(mismatches); print(" (expected ");
        print_dec(g_reference[0]); print(", got ");
        print_dec(g_result[0]); print("...)\n");
        return -1;
    }

    return 0;  /* Success */
}

/*
 * _start -- main entry point.
 *
 * Steps:
 *   1. Run NUM_ITERATIONS offloads sequentially.
 *   2. Count successes.
 *   3. Report PASS/FAIL with success rate.
 */
void _start(void) {
    print("[RAPID_OFFLOAD] Starting rapid-fire stress test (N=");
    print_dec(N); print(", iterations="); print_dec(NUM_ITERATIONS); print(")\n");

    int success_count = 0;
    int first_failure = -1;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        int rc = do_one_offload(i);

        /* Handle SKIP case (ENOTSUP on first iteration). */
        if (rc == -ENOTSUP_NEG && i == 0) {
            print("RAPID_OFFLOAD: SKIP (no SMP offload syscall)\n");
            sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
            for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
        }

        if (rc < 0) {
            /* Failure (diagnostic already printed by do_one_offload). */
            if (first_failure < 0) {
                first_failure = i;
            }
            /* Keep going to measure how many succeed. */
        } else {
            success_count++;
            /* Print progress every 10 iterations. */
            if ((i + 1) % 10 == 0) {
                print("[RAPID_OFFLOAD] Progress: ");
                print_dec(i + 1);
                print("/");
                print_dec(NUM_ITERATIONS);
                print(" (");
                print_dec(success_count);
                print(" successful)\n");
            }
        }
    }

    /* Report final result. */
    if (success_count == NUM_ITERATIONS) {
        print("RAPID_OFFLOAD: PASS ");
        print_dec(success_count);
        print("/");
        print_dec(NUM_ITERATIONS);
        print(" successful\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    } else {
        print("RAPID_OFFLOAD: FAIL ");
        print_dec(success_count);
        print("/");
        print_dec(NUM_ITERATIONS);
        print(" successful (first failure at iteration ");
        print_dec(first_failure);
        print(")\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }

    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
