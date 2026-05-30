/* matbench -- the first brick of the tensor runtime: a single-threaded float
 * matmul with an HONEST scalar baseline (kept un-vectorized) and a hand-written
 * SSE version (gcc v4sf vectors), a correctness check, and a timing comparison.
 * Proves the SSE foundation pays off with real FLOPS. The THREADED version comes
 * once the preemptive scheduler is the default. gcc-built (emits SSE), crt0-linked;
 * freestanding, inline syscalls only. No libc, no intrinsic headers (v4sf is a
 * pure compiler extension, so this stays freestanding). */

typedef unsigned long size_t;
typedef float v4sf __attribute__((vector_size(16)));   /* 4 packed floats (xmm) */

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(3 /*SYS_WRITE*/, 1, (long)s, (long)slen(s)); }
static void out_uint(unsigned long v) {
    char b[24]; int i = 24; b[--i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    out(&b[i]);
}
static unsigned long ticks_ms(void) { return (unsigned long)sc(40 /*SYS_GET_TICKS_MS*/, 0, 0, 0); }

#define N      64      /* matrix dim (multiple of 4 for the SSE inner loop) */
#define ITERS  50      /* repeat count for measurable timing (kept modest so the
                          boot-time benchmark stays quick under QEMU's slow TCG) */

static float A[N*N]  __attribute__((aligned(16)));
static float B[N*N]  __attribute__((aligned(16)));
static float Cs[N*N] __attribute__((aligned(16)));
static float Cv[N*N] __attribute__((aligned(16)));

/* Honest scalar baseline: explicitly NOT auto-vectorized, so the comparison
 * below measures scalar-vs-SSE rather than SSE-vs-SSE. */
__attribute__((optimize("no-tree-vectorize")))
static void matmul_scalar(const float* a, const float* b, float* c) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float s = 0.0f;
            for (int k = 0; k < N; k++) s += a[i*N + k] * b[k*N + j];
            c[i*N + j] = s;
        }
}

/* Hand-vectorized SSE: outer-product / SAXPY form. Broadcast A[i][k] across a
 * v4sf, multiply by B's CONTIGUOUS row (4 floats at a time), accumulate into 4
 * columns of C. B and C are read/written in aligned 16-byte chunks. Same k-order
 * as the scalar version, so results match bit-for-bit. */
static void matmul_sse(const float* a, const float* b, float* c) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j += 4) {
            // The 4-column accumulator stays in an xmm register across the whole
            // k loop -- C is stored ONCE per 4-col block, not reloaded/restored
            // every k. That cut in memory traffic is the real win over the naive
            // form. Each k: broadcast a[i][k], multiply B's contiguous aligned
            // row chunk b[k][j..j+3], fma into the accumulator.
            v4sf cv = {0, 0, 0, 0};
            for (int k = 0; k < N; k++) {
                float aik = a[i*N + k];
                v4sf av = {aik, aik, aik, aik};
                cv += av * *(const v4sf*)&b[k*N + j];
            }
            *(v4sf*)&c[i*N + j] = cv;
        }
    }
}

static float fabsf_(float x) { return x < 0.0f ? -x : x; }

int main(void) {
    /* Deterministic small-integer fill -- bounded values stay exact in float. */
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i*N + j] = (float)(((i + 1) * (j + 2)) % 7);
            B[i*N + j] = (float)(((i + 3) * (j + 1)) % 5);
        }

    /* Correctness: the two kernels must agree. */
    matmul_scalar(A, B, Cs);
    matmul_sse(A, B, Cv);
    int ok = 1;
    for (int idx = 0; idx < N*N; idx++)
        if (fabsf_(Cs[idx] - Cv[idx]) > 0.01f) { ok = 0; break; }

    /* Timing. */
    unsigned long t0 = ticks_ms();
    for (int it = 0; it < ITERS; it++) matmul_scalar(A, B, Cs);
    unsigned long t1 = ticks_ms();
    for (int it = 0; it < ITERS; it++) matmul_sse(A, B, Cv);
    unsigned long t2 = ticks_ms();

    unsigned long s_ms = t1 - t0, v_ms = t2 - t1;
    unsigned long speedup_x100 = (v_ms > 0) ? (s_ms * 100 / v_ms) : 0;

    out("MATBENCH: N=");        out_uint(N);
    out(" iters=");             out_uint(ITERS);
    out(" scalar=");            out_uint(s_ms);
    out("ms sse=");             out_uint(v_ms);
    out("ms speedupx100=");     out_uint(speedup_x100);
    out("\n");
    out(ok ? "MATBENCH: PASS\n" : "MATBENCH: FAIL (scalar/sse mismatch)\n");
    return 0;
}
