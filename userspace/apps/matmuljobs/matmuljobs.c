/*
 * matmuljobs -- a matmul computed THROUGH the userspace job queue.
 * ================================================================
 *
 * Proves the "compute coordination layer" end to end: the M output rows of a
 * float matmul C = A*B are partitioned into several independent JOBS, submitted
 * to the job queue (userspace/lib/jobs), pulled and computed by WORKER THREADS on
 * the SHARED result buffer, drained, and the threaded result is compared
 * bit-for-bit against a single-threaded scalar reference (tensor_matmul_ref).
 *
 * WHY THIS IS CORRECT (no overlap): tensor_matmul is ROW-INDEPENDENT -- output
 * row i depends only on A's row i and all of B. So a job that owns the contiguous
 * row range [r0,r1) computes tensor_matmul(&C[r0*N], &A[r0*K], B, r1-r0, N, K)
 * into a DISJOINT slice of C. No two jobs touch the same C bytes; no locking of C
 * is needed; and each job runs the exact same per-row arithmetic (same k order)
 * as the reference, so the results are EXACT, not merely close.
 *
 * **THIS IS NOT A SPEEDUP.** AutomationOS is SINGLE-CORE: the 2 worker threads
 * time-share ONE cpu, so dispatching the matmul through the queue is *slower*
 * than doing it inline (queue + futex + context-switch overhead) -- never faster.
 * The deliverable is the COORDINATION MACHINERY proven correct (work submitted,
 * pulled by workers, computed on shared memory, collected, result == reference).
 * Real parallel speedup needs SMP (multiple cpus) -- a separate, later project.
 *
 * Freestanding: inline `syscall` for write/exit; the job + tensor libs are linked
 * in. Prints exactly one PASS/FAIL line for the smoke grep, then exits.
 */

#include "../../lib/jobs/jobs.h"
#include "../../lib/tensor/tensor.h"

typedef unsigned long size_t;

#define SYS_EXIT  0
#define SYS_WRITE 3

static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void out_int(long v) {
    char b[24]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i++] = '-';
    char r[26]; int j = 0;
    while (i > 0) r[j++] = b[--i];
    r[j] = '\0';
    out(r);
}

/* ---- problem size ---------------------------------------------------------
 * M=N=K=64: big enough to span several jobs (8 jobs * 8 rows each) yet tiny
 * enough to compute in a blink so the boot smoke stays fast. All float, 16-byte
 * aligned static buffers (matmul is movups-safe, but aligning is free + tidy). */
#define MM 64
#define MN 64
#define MK 64

#define N_WORKERS 2     /* TWO workers: proves the queue dispatches to multiple
                         * consumers (MPMC). Single constant -> trivial to change. */
#define N_JOBS    8     /* partition the M rows into 8 contiguous row ranges */

static float A[MM * MK] __attribute__((aligned(16)));
static float B[MK * MN] __attribute__((aligned(16)));
static float C[MM * MN] __attribute__((aligned(16)));   /* job-computed (shared) */
static float C_ref[MM * MN] __attribute__((aligned(16))); /* scalar reference */

/* Each job owns a contiguous output row range [r0, r1). Static array of descs so
 * their addresses stay valid for the whole run (we pass &desc as the job arg). */
typedef struct { int r0, r1; } rowrange_t;
static rowrange_t g_ranges[N_JOBS];

/* The job body: compute this job's row band into the SHARED C. Disjoint from
 * every other job's band, so no synchronization on C is required. */
static void matmul_band(void* arg) {
    rowrange_t* rr = (rowrange_t*)arg;
    int r0 = rr->r0, r1 = rr->r1;
    /* C[r0..r1) = A[r0..r1) * B. Row-major slice: &C[r0*N], &A[r0*K], full B. */
    tensor_matmul(&C[r0 * MN], &A[r0 * MK], B, r1 - r0, MN, MK);
}

static int feq(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d < 0.0001f;   /* should be EXACT (same per-row arithmetic); tiny eps */
}

void _start(void) {
    out("matmuljobs: start (matmul through job queue; ");
    out_int(N_WORKERS); out(" workers, ");
    out_int(N_JOBS); out(" jobs, M=N=K="); out_int(MM); out(")\n");

    /* ---- Fill A and B deterministically (small ints -> exact in float). ---- */
    for (int i = 0; i < MM; i++)
        for (int k = 0; k < MK; k++)
            A[i * MK + k] = (float)(((i + 1) * (k + 2)) % 13 - 6);
    for (int k = 0; k < MK; k++)
        for (int j = 0; j < MN; j++)
            B[k * MN + j] = (float)(((k + 2) * (j + 3)) % 11 - 5);
    for (int i = 0; i < MM * MN; i++) C[i] = 0.0f;

    /* ---- Single-threaded reference (the source of truth). ---- */
    tensor_matmul_ref(C_ref, A, B, MM, MN, MK);

    /* ---- Bring up the worker pool. ---- */
    int started = jobsys_init(N_WORKERS);
    if (started <= 0) {
        out("matmuljobs: FAIL jobsys_init returned "); out_int(started); out("\n");
        sc(SYS_EXIT, 1, 0, 0);
        for (;;) {}
    }

    /* ---- Partition M rows into N_JOBS contiguous bands and submit them. ----
     * MM (64) / N_JOBS (8) = 8 rows per band, evenly; the last band absorbs any
     * remainder if the numbers were not evenly divisible. */
    int per = MM / N_JOBS;
    for (int b = 0; b < N_JOBS; b++) {
        int r0 = b * per;
        int r1 = (b == N_JOBS - 1) ? MM : (r0 + per);
        g_ranges[b].r0 = r0;
        g_ranges[b].r1 = r1;
        jobsys_submit(matmul_band, &g_ranges[b]);
    }

    /* ---- Wait for ALL jobs to complete (blocking futex drain), then stop. ---- */
    jobsys_drain();
    jobsys_shutdown();   /* must shut down cleanly BEFORE we return */

    /* ---- Verify the threaded result equals the scalar reference, exactly. ---- */
    int mismatches = 0;
    int first_bad = -1;
    for (int i = 0; i < MM * MN; i++) {
        if (!feq(C[i], C_ref[i])) {
            if (first_bad < 0) first_bad = i;
            mismatches++;
        }
    }

    if (mismatches == 0) {
        /* ONE unambiguous line for the smoke grep, with proof the machinery ran:
         * job count, worker count, and two sentinel result cells. */
        out("matmuljobs: PASS result-matches-ref jobs=");
        out_int(N_JOBS); out("/"); out_int(N_JOBS);
        out(" workers="); out_int(started);
        out(" C[0]="); out_int((long)C[0]);
        out(" C[last]="); out_int((long)C[MM * MN - 1]);
        out("\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else {
        out("matmuljobs: FAIL result-mismatch count="); out_int(mismatches);
        out(" first_idx="); out_int(first_bad);
        out(" C="); out_int((long)C[first_bad >= 0 ? first_bad : 0]);
        out(" ref="); out_int((long)C_ref[first_bad >= 0 ? first_bad : 0]);
        out("\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    for (;;) {}
}
