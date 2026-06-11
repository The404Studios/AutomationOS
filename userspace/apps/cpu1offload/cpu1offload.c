/*
 * cpu1offload.c -- userspace -> CPU1 matmul offload probe (freestanding, ring 3).
 * ============================================================================
 *
 * Proves the userspace->CPU1 bridge end to end: a NORMAL ring-3 process offloads
 * an integer matrix multiply to CPU1 (the TRUSTED coprocessor) via the gated
 * SYS_CPU1_OFFLOAD syscall and verifies the returned result against its own CPU0
 * reference.
 *
 *   1. Build a small deterministic n x n int32 A, B (n=64).
 *   2. Compute a CPU0 reference matmul (int64 accumulation).
 *   3. Pack { int32 n; int32 A[n*n]; int32 B[n*n]; } and call SYS_CPU1_OFFLOAD
 *      with CPU1_JOB_MATMUL. The kernel validates+copies the operands, runs the
 *      WHOLE matmul on CPU1, and copies the int64 result back. The syscall's
 *      return value is the apic id that ran it (must be 1 == CPU1).
 *   4. Compare the offloaded result vs the reference, bit-for-bit.
 *
 * Output (single line for the smoke/boot grep):
 *   CPU1OFFLOAD: PASS result-matches-ref n=64 by_apic=1   (SMP build, CPU1 ran it)
 *   CPU1OFFLOAD: SKIP (no SMP offload syscall)            (DEFAULT build: syscall
 *                                                          unregistered -> ENOTSUP)
 *   CPU1OFFLOAD: FAIL <why>                               (mismatch / bad rc / etc.)
 *
 * On the DEFAULT (single-core) kernel SYS_CPU1_OFFLOAD is NOT registered, so the
 * dispatcher returns ENOTSUP (-95). The app detects that and prints SKIP, so it is
 * completely harmless in the default boot (no fault, clean exit).
 *
 * Pure userspace: it does NO MMIO and touches NO hardware -- it just drives the
 * kernel syscall. All diagnostics go to serial via SYS_WRITE(fd=1).
 *
 * Build (bare _start, no crt0 -- mirrors nettest):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/cpu1offload/cpu1offload.c -o cpu1offload.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       cpu1offload.o -o cpu1offload
 */

/* ---- syscall numbers (match kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_CPU1_OFFLOAD  83   /* gated: only registered under SMP_FOUNDATION */

/* ---- offload job-type selector (match syscall.h CPU1_JOB_MATMUL) ---- */
#define CPU1_JOB_MATMUL   1

/* ---- ENOTSUP / EOPNOTSUPP from kernel/include/errno.h (negative ABI) ---- */
#define ENOTSUP_NEG       95   /* dispatcher returns ENOTSUP for an unregistered # */

typedef unsigned char  u8;
typedef int            i32;
typedef long long      i64;

/* problem size: 64x64 -- well under the kernel's MM_N cap (128), big enough to be
 * a real multiply yet trivial to compute so the boot self-test stays fast. The
 * deterministic fills use small values so the int64 accumulator never overflows. */
#define N 64

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
static void print_dec(long v) {
    char b[24]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    if (neg) { char m = '-'; sc(SYS_WRITE, 1, (long)&m, 1, 0, 0, 0); }
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}

/* The user-arg block the kernel expects: { int32 n; int32 A[n*n]; int32 B[n*n]; }.
 * Laid out as a packed static struct so A immediately follows n and B follows A --
 * exactly the byte layout the handler reads (n, then A, then B). Static (.bss) so
 * the ~256 KB never lands on the stack. */
typedef struct {
    i32 n;
    i32 A[N * N];
    i32 B[N * N];
} matmul_arg_t;

static matmul_arg_t g_arg;                 /* the offload request block          */
static i64 g_result[N * N];                /* offloaded result (int64) from CPU1 */
static i64 g_ref[N * N];                   /* CPU0 reference result              */

static void fail(const char *why) {
    print("CPU1OFFLOAD: FAIL ");
    print(why);
    print("\n");
    sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

void _start(void) {
    print("[CPU1OFFLOAD] starting (n="); print_dec(N); print(")\n");

    /* 1. Deterministic small-int inputs (same generator shape as the kernel's own
     *    mm_fill, but independent -- this is the userspace operand). */
    g_arg.n = N;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            g_arg.A[i * N + j] = (i + j) % 7;
            g_arg.B[i * N + j] = (i * 3 + j + 1) % 5;
        }
    }

    /* 2. CPU0 reference matmul (int64 accumulation, identical arithmetic to the
     *    kernel's matmul_band_n so the comparison is EXACT, not approximate). */
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            i64 s = 0;
            for (int k = 0; k < N; k++) {
                s += (i64)g_arg.A[r * N + k] * (i64)g_arg.B[k * N + c];
            }
            g_ref[r * N + c] = s;
        }
    }

    /* 3. Offload to CPU1. arg_len/res_len must match the kernel's exact contract:
     *    arg = sizeof(int32) + 2*n*n*sizeof(int32); res = n*n*sizeof(int64). */
    long arg_len = (long)sizeof(i32) + 2L * (long)N * (long)N * (long)sizeof(i32);
    long res_len = (long)N * (long)N * (long)sizeof(i64);

    long rc = sc(SYS_CPU1_OFFLOAD, CPU1_JOB_MATMUL,
                 (long)&g_arg, arg_len, (long)g_result, res_len, 0);

    /* 3a. DEFAULT kernel: syscall unregistered -> ENOTSUP. Print SKIP + exit clean
     *     (this path is what makes the app harmless in the default boot). */
    if (rc == -ENOTSUP_NEG) {
        print("CPU1OFFLOAD: SKIP (no SMP offload syscall)\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    /* 3b. Any other negative return is a real failure of the offload path. */
    if (rc < 0) {
        print("rc="); print_dec(rc);
        fail("offload-syscall-error");
    }

    /* On success the syscall returns the apic id that ran the job. CPU1 == 1. */
    long by_apic = rc;
    if (by_apic != 1) {
        print("by_apic="); print_dec(by_apic);
        fail("did-not-run-on-cpu1");
    }

    /* 4. Verify the offloaded result equals the CPU0 reference, bit-for-bit. */
    int mismatches = 0;
    int first_bad = -1;
    for (int i = 0; i < N * N; i++) {
        if (g_result[i] != g_ref[i]) {
            if (first_bad < 0) first_bad = i;
            mismatches++;
        }
    }

    if (mismatches != 0) {
        print("mismatches="); print_dec(mismatches);
        print(" first_idx="); print_dec(first_bad);
        print(" got=");  print_dec(g_result[first_bad >= 0 ? first_bad : 0]);
        print(" ref=");  print_dec(g_ref[first_bad >= 0 ? first_bad : 0]);
        fail("result-mismatch");
    }

    /* Single unambiguous PASS line for the boot grep, with the by_apic proof that
     * CPU1 (apic 1) genuinely computed the result. */
    print("CPU1OFFLOAD: PASS result-matches-ref n="); print_dec(N);
    print(" by_apic="); print_dec(by_apic);
    print(" C[0]="); print_dec(g_result[0]);
    print(" C[last]="); print_dec(g_result[N * N - 1]);
    print("\n");

    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
