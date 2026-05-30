/* tensor.h -- the first brick of the AutomationOS tensor runtime: a small,
 * SINGLE-THREADED, freestanding SSE float tensor-kernel library. Each public op
 * has a hand-vectorized SSE kernel (gcc `v4sf` vectors -- a pure compiler
 * extension, so NO intrinsic headers and NO libm are needed) that processes 4
 * floats per iteration with a scalar REMAINDER loop for element counts that are
 * not a multiple of 4. The threaded version comes LATER; this stays simple and
 * correct.
 *
 * ALIGNMENT CONTRACT: the elementwise ops (`tensor_add`, `tensor_scale`,
 * `tensor_relu`, `tensor_dot`) dereference their CONTIGUOUS buffers as `v4sf*`
 * (a 16-byte aligned movaps). Callers MUST pass 16-byte aligned buffers for
 * those four ops (declare them `__attribute__((aligned(16)))`). Their scalar
 * remainder tail makes NO alignment assumption -- only the packed body does.
 *
 * `tensor_matmul` is more forgiving: its vector path uses an UNALIGNED-safe load
 * (movups) for B's row chunks and C's row stores, because a buffer that is
 * 16-aligned at its base still has each row start at byte offset row*N*4, which
 * is only 16-aligned when N is a multiple of 4 -- and the caller does not
 * control N. So matmul works for ANY M/N/K given contiguous row-major storage;
 * a 16-aligned backing store is nice-to-have (movups on aligned data is as fast
 * as movaps on modern x86) but NOT required for correctness. The N-remainder
 * tail (when N%4 != 0) is plain scalar code and needs no alignment.
 *
 * This header also exposes an OBVIOUSLY-correct scalar reference for every op
 * (the `_ref` variants), kept un-vectorized, so the self-test can compare the
 * SSE result against an independent baseline. */

#ifndef TENSOR_H
#define TENSOR_H

/* 4 packed floats == one xmm register. Pure gcc extension (no <xmmintrin.h>). */
typedef float v4sf __attribute__((vector_size(16)));

/* ============================ SSE kernels ================================= */

/* C[MxN] = A[MxK] * B[KxN], row-major. SSE path broadcasts A[i][k] across a
 * v4sf and FMAs B's contiguous aligned row chunk into a 4-wide C accumulator
 * held in a register across the whole k loop; an N-remainder tail finishes the
 * trailing <4 columns with scalar code. */
void tensor_matmul(float* C, const float* A, const float* B, int M, int N, int K);

/* out[i] = a[i] + b[i], i in [0,n). 4-wide packed body + scalar tail. */
void tensor_add(float* out, const float* a, const float* b, int n);

/* out[i] = a[i] * s. 4-wide packed body (s broadcast) + scalar tail. */
void tensor_scale(float* out, const float* a, float s, int n);

/* out[i] = max(0, a[i]). Branchless SSE max via a (a>0) compare-mask AND
 * (the mask is all-ones where a>0, all-zero elsewhere) + scalar tail. */
void tensor_relu(float* out, const float* a, int n);

/* sum_i a[i]*b[i]. 4-wide multiply-accumulate into a v4sf, horizontal-sum the
 * 4 lanes, then add the scalar tail. */
float tensor_dot(const float* a, const float* b, int n);

/* ===================== Obviously-correct scalar references ================ */
/* Kept un-vectorized (no SIMD, plain loops) so the self-test compares the SSE
 * kernels above against an INDEPENDENT baseline rather than SSE-vs-SSE. */

__attribute__((optimize("no-tree-vectorize")))
static inline void tensor_matmul_ref(float* C, const float* A, const float* B,
                                     int M, int N, int K) {
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++) {
            float s = 0.0f;
            for (int k = 0; k < K; k++) s += A[i * K + k] * B[k * N + j];
            C[i * N + j] = s;
        }
}

__attribute__((optimize("no-tree-vectorize")))
static inline void tensor_add_ref(float* out, const float* a, const float* b, int n) {
    for (int i = 0; i < n; i++) out[i] = a[i] + b[i];
}

__attribute__((optimize("no-tree-vectorize")))
static inline void tensor_scale_ref(float* out, const float* a, float s, int n) {
    for (int i = 0; i < n; i++) out[i] = a[i] * s;
}

__attribute__((optimize("no-tree-vectorize")))
static inline void tensor_relu_ref(float* out, const float* a, int n) {
    for (int i = 0; i < n; i++) out[i] = a[i] > 0.0f ? a[i] : 0.0f;
}

__attribute__((optimize("no-tree-vectorize")))
static inline float tensor_dot_ref(const float* a, const float* b, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

#endif /* TENSOR_H */
