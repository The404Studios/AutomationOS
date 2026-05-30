/* tensortest -- correctness self-test for the tensor-kernel library
 * (userspace/lib/tensor). For each op it runs the hand-vectorized SSE kernel and
 * an INDEPENDENT obviously-correct scalar reference (the *_ref inlines in
 * tensor.h) on the same small fixed inputs, compares within epsilon=1e-3, and
 * prints one line per op. Ends with "TENSORTEST: PASS" or "TENSORTEST: FAIL
 * op=<name>" so the boot smoke can gate it. Inputs are seeded through a
 * `volatile` array so nothing constant-folds. Static buffers are 16-byte aligned
 * to satisfy the library's aligned vector path. crt0-linked (main(argc,argv));
 * inline syscalls only -- freestanding, no libc, no intrinsic headers. */

#include "../../lib/tensor/tensor.h"

typedef unsigned long size_t;

#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_GET_TICKS_MS  40

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void out_uint(unsigned long v) {
    char b[24]; int i = 24; b[--i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    out(&b[i]);
}

static float fabsf_(float x) { return x < 0.0f ? -x : x; }
#define EPS 0.001f
static int near(float a, float b) { return fabsf_(a - b) < EPS; }

/* compare two float arrays within EPS */
static int arr_near(const float* x, const float* y, int n) {
    for (int i = 0; i < n; i++) if (!near(x[i], y[i])) return 0;
    return 1;
}

/* ---- static 16-aligned buffers (the library's aligned vector path needs them) ----
 * Sizes use deliberately NON-multiple-of-4 element counts (n=10, K=5) so the
 * scalar remainder tails are actually exercised. matmul: A[3x5], B[5x7], C[3x7]
 * (N=7 -> one 4-wide col block + a 3-wide scalar tail; K=5 inner). */
#define VN 10                         /* vector-op length (10 % 4 == 2 -> tail) */
static float a1[VN]  __attribute__((aligned(16)));
static float b1[VN]  __attribute__((aligned(16)));
static float ov[VN]  __attribute__((aligned(16)));   /* SSE result */
static float os[VN]  __attribute__((aligned(16)));   /* scalar ref result */

#define MM_M 3
#define MM_N 7                        /* 7 % 4 == 3 -> N-remainder tail */
#define MM_K 5
static float mA[MM_M * MM_K] __attribute__((aligned(16)));
static float mB[MM_K * MM_N] __attribute__((aligned(16)));
static float mCv[MM_M * MM_N] __attribute__((aligned(16)));   /* SSE */
static float mCs[MM_M * MM_N] __attribute__((aligned(16)));   /* scalar */

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int ok = 1;
    const char* failop = "";

    /* volatile seed => real SSE loads at runtime, no constant folding. */
    volatile float seed[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float s0 = seed[0], s1 = seed[1], s2 = seed[2], s3 = seed[3];

    /* Fill the vector-op inputs with bounded values (stay exact in float). */
    for (int i = 0; i < VN; i++) {
        a1[i] = (float)(((i + 1) * 2) % 9) + s0 * 0.0f + (i & 1 ? -1.0f : 1.0f);
        b1[i] = (float)(((i + 3) * 3) % 7) - s1 * 0.0f;
    }
    /* Ensure relu sees a spread of negatives and positives. */
    for (int i = 0; i < VN; i++) a1[i] = a1[i] - 4.0f + s2 * 0.0f;

    /* Fill the matmul inputs (small ints stay exact). seed used so non-const. */
    for (int i = 0; i < MM_M; i++)
        for (int k = 0; k < MM_K; k++)
            mA[i * MM_K + k] = (float)(((i + 1) * (k + 2)) % 7) + s3 * 0.0f;
    for (int k = 0; k < MM_K; k++)
        for (int j = 0; j < MM_N; j++)
            mB[k * MM_N + j] = (float)(((k + 3) * (j + 1)) % 5) + s0 * 0.0f;

    /* ---- 1. tensor_add ---- */
    tensor_add(ov, a1, b1, VN);
    tensor_add_ref(os, a1, b1, VN);
    {
        int p = arr_near(ov, os, VN);
        if (!p) { ok = 0; failop = "add"; }
        out("TENSORTEST: add        ");
        out(p ? "ok" : "FAIL");
        out(" [0]="); out_uint((unsigned long)(ov[0] + 100.5f));
        out(" [9]="); out_uint((unsigned long)(ov[9] + 100.5f));
        out("\n");
    }

    /* ---- 2. tensor_scale ---- */
    {
        float sc_factor = seed[1] + 0.5f;   /* 2.5, via volatile */
        tensor_scale(ov, a1, sc_factor, VN);
        tensor_scale_ref(os, a1, sc_factor, VN);
        int p = arr_near(ov, os, VN);
        if (!p && ok) { ok = 0; failop = "scale"; }
        out("TENSORTEST: scale      ");
        out(p ? "ok" : "FAIL");
        out(" [0]="); out_uint((unsigned long)(ov[0] + 100.5f));
        out("\n");
    }

    /* ---- 3. tensor_relu ---- */
    tensor_relu(ov, a1, VN);
    tensor_relu_ref(os, a1, VN);
    {
        int p = arr_near(ov, os, VN);
        if (!p && ok) { ok = 0; failop = "relu"; }
        /* count non-zero (clamped) lanes as evidence */
        int nz = 0; for (int i = 0; i < VN; i++) if (ov[i] > 0.0f) nz++;
        out("TENSORTEST: relu       ");
        out(p ? "ok" : "FAIL");
        out(" nonzero="); out_uint((unsigned long)nz);
        out("\n");
    }

    /* ---- 4. tensor_dot ---- */
    {
        float dv = tensor_dot(a1, b1, VN);
        float ds = tensor_dot_ref(a1, b1, VN);
        int p = near(dv, ds);
        if (!p && ok) { ok = 0; failop = "dot"; }
        out("TENSORTEST: dot        ");
        out(p ? "ok" : "FAIL");
        out(" v="); out_uint((unsigned long)(dv + 1000.5f));
        out(" (ref="); out_uint((unsigned long)(ds + 1000.5f)); out(")");
        out("\n");
    }

    /* ---- 5. tensor_matmul ---- */
    tensor_matmul(mCv, mA, mB, MM_M, MM_N, MM_K);
    tensor_matmul_ref(mCs, mA, mB, MM_M, MM_N, MM_K);
    {
        int p = arr_near(mCv, mCs, MM_M * MM_N);
        if (!p && ok) { ok = 0; failop = "matmul"; }
        out("TENSORTEST: matmul     ");
        out(p ? "ok" : "FAIL");
        out(" C00="); out_uint((unsigned long)(mCv[0] + 0.5f));
        out(" C06="); out_uint((unsigned long)(mCv[6] + 0.5f));   /* tail col */
        out(" C26="); out_uint((unsigned long)(mCv[MM_M * MM_N - 1] + 0.5f));
        out("\n");
    }

    if (ok) {
        out("TENSORTEST: PASS\n");
    } else {
        out("TENSORTEST: FAIL op=");
        out(failop);
        out("\n");
    }
    return ok ? 0 : 1;
}
