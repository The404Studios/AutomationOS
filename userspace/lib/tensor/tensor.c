/* tensor.c -- SSE kernels for the AutomationOS tensor runtime (single-threaded,
 * freestanding). Each op processes 4 floats per iteration via gcc `v4sf`
 * vectors (a compiler extension -- no <xmmintrin.h>, no libm) with a scalar
 * REMAINDER loop for counts not divisible by 4. The aligned vector path
 * dereferences `v4sf*` (movaps), so buffers must be 16-byte aligned per the
 * contract documented in tensor.h; the scalar tails make no such assumption.
 *
 * These kernels are the hand-vectorized counterparts of the obviously-correct
 * scalar references in tensor.h; tensortest compares the two within an epsilon. */

#include "tensor.h"

/* An unaligned (1-byte-aligned) view of a v4sf. Dereferencing it emits movups
 * rather than movaps, so it is safe at ANY address. The matmul below uses this
 * for B's row chunks and C's row stores because a buffer that is 16-aligned at
 * its base still has rows starting at offset (row * N * 4) bytes -- which is
 * only 16-aligned when N is a multiple of 4. Since the caller does not control
 * N, we must not assume per-row alignment here. (The elementwise ops below DO
 * keep the aligned fast path: their contract -- the whole contiguous buffer is
 * 16-aligned -- is reasonable and is met by aligned static buffers.) */
typedef float v4sf_u __attribute__((vector_size(16), aligned(1)));

/* ---------------------------------------------------------------------------
 * tensor_matmul: C[MxN] = A[MxK] * B[KxN], row-major.
 *
 * Generalizes matbench's register-accumulator kernel. For each output row i,
 * walk the columns in blocks of 4 (j, j+4, ...). The 4-column accumulator `cv`
 * stays in an xmm register across the entire k loop -- C is stored ONCE per
 * 4-col block, not reloaded every k (the memory-traffic cut is the real win).
 * Each k: broadcast A[i][k] across a v4sf, multiply B's CONTIGUOUS row chunk
 * b[k][j..j+3] (unaligned-safe load, since B's row start alignment depends on
 * N), accumulate. Same k-order as the scalar reference, so results match. The
 * trailing <4 columns (when N % 4 != 0) are finished by a scalar N-remainder
 * tail that needs no alignment.
 * ------------------------------------------------------------------------- */
void tensor_matmul(float* C, const float* A, const float* B, int M, int N, int K) {
    int nv = N & ~3;                 /* largest multiple of 4 <= N (vector cols) */
    for (int i = 0; i < M; i++) {
        const float* arow = &A[i * K];
        float* crow = &C[i * N];

        /* --- packed body: 4 columns at a time --- */
        for (int j = 0; j < nv; j += 4) {
            v4sf cv = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int k = 0; k < K; k++) {
                float aik = arow[k];
                v4sf av = {aik, aik, aik, aik};
                cv += av * *(const v4sf_u*)&B[k * N + j];  /* B row chunk (movups) */
            }
            *(v4sf_u*)&crow[j] = cv;                        /* C store (movups) */
        }

        /* --- scalar N-remainder tail: the trailing N%4 columns --- */
        for (int j = nv; j < N; j++) {
            float s = 0.0f;
            for (int k = 0; k < K; k++) s += arow[k] * B[k * N + j];
            crow[j] = s;
        }
    }
}

/* ---------------------------------------------------------------------------
 * tensor_add: out[i] = a[i] + b[i]. Packed v4sf add over the 4-aligned body,
 * scalar tail for the trailing n%4.
 * ------------------------------------------------------------------------- */
void tensor_add(float* out, const float* a, const float* b, int n) {
    int nv = n & ~3;
    for (int i = 0; i < nv; i += 4)
        *(v4sf*)&out[i] = *(const v4sf*)&a[i] + *(const v4sf*)&b[i];
    for (int i = nv; i < n; i++) out[i] = a[i] + b[i];
}

/* ---------------------------------------------------------------------------
 * tensor_scale: out[i] = a[i] * s. Broadcast s into a v4sf and multiply the
 * packed body; scalar tail for n%4.
 * ------------------------------------------------------------------------- */
void tensor_scale(float* out, const float* a, float s, int n) {
    int nv = n & ~3;
    v4sf sv = {s, s, s, s};
    for (int i = 0; i < nv; i += 4)
        *(v4sf*)&out[i] = *(const v4sf*)&a[i] * sv;
    for (int i = nv; i < n; i++) out[i] = a[i] * s;
}

/* ---------------------------------------------------------------------------
 * tensor_relu: out[i] = max(0, a[i]). Branchless SSE max: the per-lane compare
 * (av > zero) yields an all-ones mask where the lane is positive and all-zero
 * elsewhere; reinterpreting that integer mask and AND-ing it with av selects
 * a[i] for positive lanes and 0 for the rest. Scalar tail for n%4.
 *
 * NB: gcc lowers `av > zero` to an integer (v4si) compare result; we AND it
 * into the float bit-pattern via a union-free reinterpret using a v4si view.
 * ------------------------------------------------------------------------- */
typedef int v4si __attribute__((vector_size(16)));

void tensor_relu(float* out, const float* a, int n) {
    int nv = n & ~3;
    v4sf zero = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < nv; i += 4) {
        v4sf av = *(const v4sf*)&a[i];
        v4si mask = (av > zero);                 /* -1 (all ones) where a>0 */
        v4si bits = (v4si)av & mask;             /* keep a's bits, else 0 */
        *(v4sf*)&out[i] = (v4sf)bits;
    }
    for (int i = nv; i < n; i++) out[i] = a[i] > 0.0f ? a[i] : 0.0f;
}

/* ---------------------------------------------------------------------------
 * tensor_dot: sum_i a[i]*b[i]. Multiply-accumulate the packed body into a v4sf,
 * horizontal-sum the 4 lanes, then add the scalar tail (n%4). The horizontal
 * sum just reads the 4 lanes out -- simple and obviously correct.
 * ------------------------------------------------------------------------- */
float tensor_dot(const float* a, const float* b, int n) {
    int nv = n & ~3;
    v4sf acc = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < nv; i += 4)
        acc += *(const v4sf*)&a[i] * *(const v4sf*)&b[i];
    float sum = acc[0] + acc[1] + acc[2] + acc[3];   /* horizontal sum */
    for (int i = nv; i < n; i++) sum += a[i] * b[i]; /* scalar tail */
    return sum;
}
