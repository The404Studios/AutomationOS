/*
 * fpm.c -- FPM fixed-point math library implementation.
 * =====================================================
 * See fpm.h for the API and rationale. Everything is integer Q16.16: no libc,
 * no libm, no floating point. The same source compiles under host gcc (for
 * tests/fpm_hosttest.c) and under the freestanding ring-3 toolchain.
 */
/*
 * NO-FLOAT / NO-XMM / NO-CANARY GATE. Four freestanding gcc-15 quirks to pin:
 *   1. no-tree-vectorize   -- at -O2 the tree/SLP vectorizer turns the
 *      per-element loops (matrix zeroing, table fill) into INTEGER SSE
 *      (pxor/movdqa over xmm). Not floating point, but our objdump gate wants
 *      literally zero xmm, so we disable only the auto-vectorizer.
 *   2. no-tree-loop-distribute-patterns -- keeps GCC from synthesizing libc
 *      memset/memcpy out of our hand loops (this TU links no libc).
 * (1) and (2) are the no-xmm pair from userspace/lib/ui/ui.c.
 *   3. no-stack-protector -- gcc-15 re-injects a %fs:0x28 canary (calling the
 *      absent __stack_chk_fail) for functions with on-stack arrays, and the
 *      fxm3/fxm4 builders return 9/16-entry struct arrays. This mirrors
 *      userspace/lib/g3d/g3d.c, the other fixed-point matrix lib, which pins the
 *      same for exactly this reason.
 *   4. target("no-sse") -- the vectorizer pragma is NOT enough here: the RTL
 *      move-by-pieces expander copies the 64-byte by-value fxm4 structs with
 *      integer-SSE movdqa/movups regardless of (1) (measured: 56 xmm lines).
 *      ui.c never hits this (no big struct returns); g3d.o ships those movdqa
 *      because its gate only greps the ELF for fs:0x28. Our fpm gate asserts
 *      literally 0 xmm, so we retarget the whole TU to no-SSE: struct copies
 *      become plain 64-bit mov pairs; all Q16.16 arithmetic is untouched (there
 *      is no float anywhere, so no SSE ABI is needed).
 * Together: 0 xmm, 0 canary, 0 libc refs. Skipped on the host build so the KAT
 * harness stays plain (stdio/libm there are fine).
 *
 * The block sits ABOVE the #include on purpose: #pragma GCC target/optimize only
 * affect functions defined AFTER them, and fpm.h carries static-inline vector
 * helpers (fxv3_sub & co.) that gcc can out-of-line into this TU -- with the
 * pragmas below the include, those keep the default SSE target and leak
 * movq/psubd xmm back in (measured on fxv3_sub).
 */
#ifndef FPM_HOSTTEST
#pragma GCC optimize("no-tree-vectorize")
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#pragma GCC optimize("no-stack-protector")
#pragma GCC target("no-sse")
#endif

#include "fpm.h"

/* ====================================================================== *
 *  Scalar core (saturating). The clamp primitive fpm_sat_i64 and the
 *  unsaturated product term fpm_mul_i64 live in fpm.h so the header's
 *  vector inlines share them.
 * ====================================================================== */

fx fx_mul(fx a, fx b)
{
    /* product is Q32.32 in an int64 (max |a*b| = 2^62); shift back to Q16.16. */
    return fpm_sat_i64(fpm_mul_i64(a, b));
}

fx fx_div(fx a, fx b)
{
    /* 0-divisor: saturate by the sign of the numerator (no trap, no wrap).
     * (a * FX_ONE, not a << 16: identical value/codegen, but the multiply is
     * defined for negative a in strict C where the left shift is not.) */
    if (b == 0) return a < 0 ? FX_MIN : FX_MAX;
    fpm_i64 q = ((fpm_i64)a * FX_ONE) / (fpm_i64)b;
    return fpm_sat_i64(q);
}

fx fx_lerp(fx a, fx b, fx t)
{
    /* a + (b-a)*t, all in int64 so (b-a) can exceed int32 without wrapping. */
    fpm_i64 d = (fpm_i64)b - (fpm_i64)a;
    fpm_i64 m = (d * (fpm_i64)t) >> FX_SHIFT;
    return fpm_sat_i64((fpm_i64)a + m);
}

fx fx_sat_add(fx a, fx b) { return fpm_sat_i64((fpm_i64)a + (fpm_i64)b); }
fx fx_sat_sub(fx a, fx b) { return fpm_sat_i64((fpm_i64)a - (fpm_i64)b); }

/* Largest integer <= a, as fx: mask off the fractional bits (arithmetic floor
 * for negatives because the mask keeps the sign bits). */
fx fx_floor(fx a) { return (fx)((fpm_u32)a & 0xFFFF0000u); }

fx fx_ceil(fx a)
{
    fx f = fx_floor(a);
    return (f == a) ? f : fx_sat_add(f, FX_ONE);
}

/* Round half up (toward +inf): floor(a + 0.5). When a + 0.5 saturates (a in
 * [32767.5, FX_MAX]) return FX_MAX un-floored: the ideal result 32768.0 is
 * unrepresentable, and flooring the clamped sum would un-saturate back down to
 * 32767.0 -- returning FX_MAX keeps round consistent with fx_ceil's saturation
 * convention (round(x) must never be < ceil(x) - 1). */
fx fx_round(fx a)
{
    fx s = fx_sat_add(a, FX_HALF);
    return (s == FX_MAX) ? FX_MAX : fx_floor(s);
}

/* ====================================================================== *
 *  Roots
 * ====================================================================== */

/* Integer sqrt of a 64-bit value (classic bit-by-bit). */
static fpm_u64 isqrt64(fpm_u64 n)
{
    fpm_u64 res = 0;
    fpm_u64 bit = (fpm_u64)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else                { res >>= 1; }
        bit >>= 2;
    }
    return res;
}

/* sqrt of a Q16.16 value, returned as Q16.16.  sqrt(x_real)*2^16 =
 * sqrt(x_fx * 2^16) = isqrt(x_fx << 16). */
fx fx_sqrt(fx a)
{
    if (a <= 0) return 0;
    return (fx)isqrt64((fpm_u64)(fpm_u32)a << FX_SHIFT);
}

/*
 * rsqrt core: 1/sqrt(V) in Q16.16 where V is passed in Q32.32 (as a u64). The
 * caller keeps the FULL Q32.32 squared length -- shifting a small squared length
 * down to Q16.16 first would discard nearly all its significant bits (e.g. a
 * length of 0.01 has a squared length of 1e-4, only ~6 units in Q16.16), which
 * is exactly what wrecks normalize() for small vectors.
 *
 * Seed from the digit-by-digit isqrt (NOT a blind fx_div(1, sqrt)): with
 * root = isqrt64(v32) = sqrt(V) in Q16.16, the seed 1/sqrt(V) in Q16.16 is
 * 2^32 / root -- already accurate to ~1/root. It is then refined with two Newton
 * iterations of  y <- y * (3 - V*y^2) / 2, with V*y formed first (large-ish
 * intermediate) so precision is not lost forming y^2 for small y.
 *
 * ERROR BOUND: over V in [1e-3, 30000] the measured worst-case relative error is
 * ~2e-3 (inside the 5e-3 contract); for the normalize magnitudes 1e-2..1e3 the
 * resulting |len-1| stays < 1e-2 -- see fpm_hosttest.
 */
static fx rsqrt_q32(fpm_u64 v32)
{
    if (v32 == 0) return 0;
    fpm_u64 root = isqrt64(v32);                       /* sqrt(V) in Q16.16 */
    if (root == 0) return 0;
    fpm_i64 y = (fpm_i64)(((fpm_u64)1 << 32) / root);  /* seed 1/sqrt(V), Q16.16 */
    for (int it = 0; it < 2; it++) {
        fpm_i64 h  = (fpm_i64)((v32 * (fpm_u64)y) >> 32);   /* V*y   (Q16.16) */
        fpm_i64 e  = (h * y) >> FX_SHIFT;                   /* V*y^2 (Q16.16) */
        fpm_i64 tm = ((fpm_i64)3 << FX_SHIFT) - e;          /* 3 - V*y^2      */
        y = (y * tm) >> (FX_SHIFT + 1);                     /* * 0.5          */
        if (y > FX_MAX) y = FX_MAX;
        if (y < 0)      y = 0;
    }
    return (fx)y;
}

fx fx_rsqrt(fx a)
{
    if (a <= 0) return 0;
    /* a is V in Q16.16; the Q32.32 form is a << 16. */
    return rsqrt_q32((fpm_u64)(fpm_u32)a << FX_SHIFT);
}

/* Squared magnitude in Q32.32 as u64 (overflow-safe: 3 * 2^62 < 2^64). */
static inline fpm_u64 sumsq2_q32(fx x, fx y)
{
    return (fpm_u64)((fpm_i64)x * x) + (fpm_u64)((fpm_i64)y * y);
}
static inline fpm_u64 sumsq3_q32(fx x, fx y, fx z)
{
    return (fpm_u64)((fpm_i64)x * x) + (fpm_u64)((fpm_i64)y * y)
         + (fpm_u64)((fpm_i64)z * z);
}

/* isqrt64 of a Q32.32 value yields the magnitude directly in Q16.16. */
fx fx_len2(fx x, fx y)
{
    fpm_u64 r = isqrt64(sumsq2_q32(x, y));
    return r > (fpm_u64)FX_MAX ? FX_MAX : (fx)r;
}
fx fx_len3(fx x, fx y, fx z)
{
    fpm_u64 r = isqrt64(sumsq3_q32(x, y, z));
    return r > (fpm_u64)FX_MAX ? FX_MAX : (fx)r;
}

/* ====================================================================== *
 *  Trig table (1024 steps), built once with an integer polynomial.
 * ---------------------------------------------------------------------- *
 *  Mirrors g3d's poly_sin_rad: a 9-term odd Taylor series in Q16.16 radians,
 *  range-reduced to [-pi/2, pi/2] via sine symmetries. FP-free, < 1e-3 error.
 * ====================================================================== */
static fx  g_sin_tab[FPM_ANG_STEPS];
static int g_sin_ready = 0;

static fx poly_sin_rad(fx x)
{
    /*
     * sin x = x * (1 - x2*(1/6 - x2*(1/120 - x2*(1/5040 - x2/362880)))), |x|<=pi/2.
     *
     * Evaluated with Horner's rule in Q30 int64 intermediates. Naive Q16.16
     * term-by-term evaluation suffers catastrophic cancellation of the large
     * alternating terms near x=pi/2 (~135 LSB, ~2e-3 abs error at the peaks);
     * Horner keeps every intermediate O(1) and the extra 14 fractional bits
     * drop the fixed-point rounding below the ~2e-5 Taylor-truncation floor.
     *
     * Q30 constants = round(coeff * 2^30). x2 in Q30 = (x_fx * x_fx) >> 2
     * (x_fx is Q16.16, the product is Q32.32, >>2 lands in Q30).
     */
    const fpm_i64 ONE_Q30 = (fpm_i64)1 << 30;
    const fpm_i64 C1 = 178956971;   /* 1/6      */
    const fpm_i64 C2 = 8947849;     /* 1/120    */
    const fpm_i64 C3 = 213044;      /* 1/5040   */
    const fpm_i64 C4 = 2959;        /* 1/362880 */
    fpm_i64 x2 = ((fpm_i64)x * x) >> 2;                 /* Q30 */
    fpm_i64 t = C3 - ((x2 * C4) >> 30);
    t = C2 - ((x2 * t) >> 30);
    t = C1 - ((x2 * t) >> 30);
    t = ONE_Q30 - ((x2 * t) >> 30);                     /* sin(x)/x in Q30 */
    return (fx)(((fpm_i64)x * t) >> 30);                /* * x -> Q16.16   */
}

static void sin_table_init(void)
{
    if (g_sin_ready) return;
    const fx PI = FX_PI, HALF_PI = FX_HALF_PI, TWO_PI = FX_TAU;
    for (int i = 0; i < FPM_ANG_STEPS; i++) {
        fx theta = fx_mul(fx_ratio(i, FPM_ANG_STEPS), TWO_PI);   /* [0,2pi) */
        fx s;
        if      (theta <= HALF_PI)      s =  poly_sin_rad(theta);
        else if (theta <= PI)           s =  poly_sin_rad(PI - theta);
        else if (theta <= PI + HALF_PI) s = -poly_sin_rad(theta - PI);
        else                            s = -poly_sin_rad(TWO_PI - theta);
        g_sin_tab[i] = s;
    }
    /* Publish the table before the ready flag: the barrier stops the compiler
     * from sinking table stores past the flag store (single-threaded per
     * process, but a signal handler calling fx_sin mid-init must never see
     * ready==1 with a half-written table). */
    __asm__ __volatile__("" ::: "memory");
    g_sin_ready = 1;
}

void fpm_init(void) { sin_table_init(); }

fx fx_sin(fpm_i32 brad)
{
    if (!g_sin_ready) sin_table_init();
    return g_sin_tab[(fpm_u32)brad & FPM_ANG_MASK];
}
fx fx_cos(fpm_i32 brad)
{
    if (!g_sin_ready) sin_table_init();
    return g_sin_tab[((fpm_u32)brad + (FPM_ANG_STEPS / 4)) & FPM_ANG_MASK];
}

/* ====================================================================== *
 *  CORDIC (from scratch): atan2 -> brads, then asin/acos via identities.
 * ---------------------------------------------------------------------- *
 *  Vectoring mode rotates the vector (x,y) onto +x, accumulating the angle.
 *  Angle unit is brads (1024 = full turn), so the arctan table holds
 *  atan(2^-i) converted to brads, Q16.16 (generated on the host; no float in
 *  this file). No gain correction is needed because we only want the ANGLE.
 * ====================================================================== */
static const fx CORDIC_ATAN[16] = {
    8388608, /* atan(2^-0) = 128.000000 brad */
    4952084, /* atan(2^-1) =  75.562812 brad */
    2616545, /* atan(2^-2) =  39.925315 brad */
    1328199, /* atan(2^-3) =  20.266713 brad */
    666677,  /* atan(2^-4) =  10.172684 brad */
    333664,  /* atan(2^-5) =   5.091301 brad */
    166872,  /* atan(2^-6) =   2.546272 brad */
    83441,   /* atan(2^-7) =   1.273214 brad */
    41721,   /* atan(2^-8) =   0.636617 brad */
    20861,   /* atan(2^-9) =   0.318309 brad */
    10430,   /* atan(2^-10)=   0.159155 brad */
    5215,    /* atan(2^-11)=   0.079577 brad */
    2608,    /* atan(2^-12)=   0.039789 brad */
    1304,    /* atan(2^-13)=   0.019894 brad */
    652,     /* atan(2^-14)=   0.009947 brad */
    326      /* atan(2^-15)=   0.004974 brad */
};

fx fx_atan2(fx y, fx x)
{
    if (x == 0 && y == 0) return 0;

    fpm_i64 vx = x, vy = y;
    fpm_i64 base = 0;
    /* Fold x<0 into x>=0 by a 180-degree rotation; identity:
     *   atan2(y,x) = atan2(-y,-x) + sign(y)*pi  (pi == 512 brads). */
    if (vx < 0) {
        vx = -vx; vy = -vy;
        base = (y >= 0) ? (fpm_i64)512 * FX_ONE : -(fpm_i64)512 * FX_ONE;
    }
    fpm_i64 ang = 0;
    for (int i = 0; i < 16; i++) {
        fpm_i64 dx = vx >> i, dy = vy >> i;
        if (vy > 0) { vx += dy; vy -= dx; ang += CORDIC_ATAN[i]; }
        else        { vx -= dy; vy += dx; ang -= CORDIC_ATAN[i]; }
    }
    return (fx)(base + ang);
}

fx fx_asin(fx a)
{
    if (a >=  FX_ONE) return fx_from_int(256);      /* +90 deg */
    if (a <= -FX_ONE) return fx_from_int(-256);     /* -90 deg */
    fx c = fx_sqrt(FX_ONE - fx_mul(a, a));          /* cos = sqrt(1-a^2) */
    return fx_atan2(a, c);
}
fx fx_acos(fx a)
{
    if (a >=  FX_ONE) return 0;                      /* 0 deg   */
    if (a <= -FX_ONE) return fx_from_int(512);       /* 180 deg */
    fx s = fx_sqrt(FX_ONE - fx_mul(a, a));          /* sin = sqrt(1-a^2) */
    return fx_atan2(s, a);
}

/* ====================================================================== *
 *  Vectors: length + normalize (via rsqrt of the overflow-safe squared len)
 * ====================================================================== */
fx fxv2_len(fxv2 a) { return fx_len2(a.x, a.y); }
fxv2 fxv2_normalize(fxv2 a)
{
    fpm_u64 v32 = sumsq2_q32(a.x, a.y);             /* |a|^2 in Q32.32 */
    if (v32 == 0) return fxv2_mk(0, 0);
    fx inv = rsqrt_q32(v32);                        /* 1/|a| in Q16.16 */
    return fxv2_mk(fx_mul(a.x, inv), fx_mul(a.y, inv));
}
fx fxv3_len(fxv3 a) { return fx_len3(a.x, a.y, a.z); }
fxv3 fxv3_normalize(fxv3 a)
{
    fpm_u64 v32 = sumsq3_q32(a.x, a.y, a.z);
    if (v32 == 0) return fxv3_mk(0, 0, 0);
    fx inv = rsqrt_q32(v32);
    return fxv3_mk(fx_mul(a.x, inv), fx_mul(a.y, inv), fx_mul(a.z, inv));
}

/* ====================================================================== *
 *  fxm3 (column-major: m[col*3 + row])
 * ====================================================================== */
fxm3 fxm3_identity(void)
{
    fxm3 r;
    for (int i = 0; i < 9; i++) r.m[i] = 0;
    r.m[0] = r.m[4] = r.m[8] = FX_ONE;
    return r;
}
/* Row sums accumulate the UNSATURATED int64 product terms (fpm_mul_i64) and
 * clamp once: bit-identical to per-term fx_mul + int32 adds whenever the sum is
 * in range, but immune to the int32 wrap those adds suffer (terms <= 2^46 each,
 * so a 4-term sum stays < 2^48, far inside int64). */
fxm3 fxm3_mul(fxm3 a, fxm3 b)
{
    fxm3 r;
    for (int c = 0; c < 3; c++) for (int row = 0; row < 3; row++) {
        fpm_i64 s = 0;
        for (int k = 0; k < 3; k++) s += fpm_mul_i64(a.m[k*3+row], b.m[c*3+k]);
        r.m[c*3+row] = fpm_sat_i64(s);
    }
    return r;
}
fxv3 fxm3_mul_vec(fxm3 m, fxv3 v)
{
    return fxv3_mk(
        fpm_sat_i64(fpm_mul_i64(m.m[0], v.x) + fpm_mul_i64(m.m[3], v.y) + fpm_mul_i64(m.m[6], v.z)),
        fpm_sat_i64(fpm_mul_i64(m.m[1], v.x) + fpm_mul_i64(m.m[4], v.y) + fpm_mul_i64(m.m[7], v.z)),
        fpm_sat_i64(fpm_mul_i64(m.m[2], v.x) + fpm_mul_i64(m.m[5], v.y) + fpm_mul_i64(m.m[8], v.z)));
}
fxm3 fxm3_transpose(fxm3 a)
{
    fxm3 r;
    for (int c = 0; c < 3; c++) for (int row = 0; row < 3; row++)
        r.m[c*3+row] = a.m[row*3+c];
    return r;
}

/* 2x2 minor a*d - b*c in Q16.16, UNSATURATED in int64 (inputs are the
 * normalized entries below, < 2^24, so |minor| < 2^33 -- exact). */
static fpm_i64 minor2_i64(fx a, fx b, fx c, fx d)
{
    return ((fpm_i64)a * d - (fpm_i64)b * c) >> FX_SHIFT;
}

/* value * 2^k with saturation, for undoing the normalization shifts below.
 * The +-2^37 pre-clamp makes the left shift (k up to 25 across all callers)
 * overflow-free; anything that large saturates to FX_MAX/FX_MIN anyway. */
static fx unshift_sat(fpm_i64 v, int k)
{
    if (k >= 0) {
        const fpm_i64 LIM = (fpm_i64)1 << 37;
        if (v >  LIM) return FX_MAX;
        if (v < -LIM) return FX_MIN;
        return fpm_sat_i64(v << k);
    }
    return fpm_sat_i64(v >> -k);
}

/*
 * Shared normalized-adjugate core for fxm3_inverse / fxm4_inverse_affine.
 *
 * A naive Q16.16 adjugate breaks in both directions: minors saturate at int32
 * once entries exceed ~32 (det ~ entry^3), and for tiny entries (uniform scale
 * 0.01 -> minors of ~7 raw units) the quotients lose all precision. So we first
 * scale the whole matrix by a power of two so its max |entry| lands in
 * [2^18, 2^24) (raw), and build the adjugate + det of that well-conditioned
 * copy exactly in int64 (minors < 2^33, det terms < 2^57, sum < 2^59 -- no
 * saturation possible). Callers undo the scale: inv(A * 2^s) = inv(A) * 2^-s.
 */
typedef struct {
    fpm_i64 adj[9];    /* adjugate of B, stored in RESULT layout [col*3+row] */
    fpm_i64 det;       /* det(B), Q16.16 in int64 (0 => singular)           */
    int     s;         /* A = B * 2^s, s in [-18, 7]                        */
} fpm_inv3;

static fpm_inv3 inv3_prepare(fxm3 A)
{
    fpm_inv3 c;
    c.det = 0; c.s = 0;

    /* --- normalize: B = A * 2^-s with max|B| in [2^18, 2^24) --- */
    fpm_i64 maxe = 0;
    for (int i = 0; i < 9; i++) {
        fpm_i64 v = A.m[i];
        if (v < 0) v = -v;                 /* int64: |INT32_MIN| is fine */
        if (v > maxe) maxe = v;
    }
    if (maxe == 0) return c;                /* zero matrix: singular */

    int s = 0;
    while (maxe >= ((fpm_i64)1 << 24)) { maxe >>= 1; s++; }              /* s <=  7 */
    while (maxe <  ((fpm_i64)1 << 18) && s > -18) { maxe <<= 1; s--; }   /* s >= -18 */
    c.s = s;

    fx B[9];
    for (int i = 0; i < 9; i++) {
        fpm_i64 v = A.m[i];
        B[i] = (fx)(s >= 0 ? (v >> s) : (v << -s));   /* < 2^24 by construction */
    }

    /* element(row,col) = B[col*3+row] */
    fx m00 = B[0], m10 = B[1], m20 = B[2];
    fx m01 = B[3], m11 = B[4], m21 = B[5];
    fx m02 = B[6], m12 = B[7], m22 = B[8];

    /* adjugate in result layout: adj[col*3+row] = numerator of inv(B)[row,col] */
    c.adj[0] = minor2_i64(m11, m12, m21, m22);   /* inv(0,0) */
    c.adj[1] = minor2_i64(m12, m10, m22, m20);   /* inv(1,0) */
    c.adj[2] = minor2_i64(m10, m11, m20, m21);   /* inv(2,0) */
    c.adj[3] = minor2_i64(m02, m01, m22, m21);   /* inv(0,1) */
    c.adj[4] = minor2_i64(m00, m02, m20, m22);   /* inv(1,1) */
    c.adj[5] = minor2_i64(m01, m00, m21, m20);   /* inv(2,1) */
    c.adj[6] = minor2_i64(m01, m02, m11, m12);   /* inv(0,2) */
    c.adj[7] = minor2_i64(m02, m00, m12, m10);   /* inv(1,2) */
    c.adj[8] = minor2_i64(m00, m01, m10, m11);   /* inv(2,2) */

    /* det(B) by cofactor expansion along column 0:
     * det = B[0,0]*C00 + B[1,0]*C10 + B[2,0]*C20, and C(k,0) is exactly the
     * adjugate entry adj[0,k] = our adj[k*3 + 0] slots -> adj[0], adj[3],
     * adj[6]. Terms < 2^57, sum < 2^59: exact in int64. */
    c.det = ((fpm_i64)m00 * c.adj[0] + (fpm_i64)m10 * c.adj[3]
           + (fpm_i64)m20 * c.adj[6]) >> FX_SHIFT;
    return c;
}

/* General 3x3 inverse: see inv3_prepare. Singular -> identity. */
fxm3 fxm3_inverse(fxm3 A)
{
    fpm_inv3 c = inv3_prepare(A);
    if (c.det == 0) return fxm3_identity();

    fxm3 r;
    for (int i = 0; i < 9; i++) {
        fpm_i64 q = (c.adj[i] * FX_ONE) / c.det;       /* inv(B) entry, Q16.16 */
        r.m[i] = unshift_sat(q, -c.s);                 /* inv(A) = inv(B)*2^-s */
    }
    return r;
}

/* ====================================================================== *
 *  fxm4 (column-major: m[col*4 + row])
 * ====================================================================== */
fxm4 fxm4_identity(void)
{
    fxm4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = FX_ONE;
    return r;
}
/* Same int64-accumulate + single-clamp discipline as fxm3_mul (see above). */
fxm4 fxm4_mul(fxm4 a, fxm4 b)
{
    fxm4 r;
    for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) {
        fpm_i64 s = 0;
        for (int k = 0; k < 4; k++) s += fpm_mul_i64(a.m[k*4+row], b.m[c*4+k]);
        r.m[c*4+row] = fpm_sat_i64(s);
    }
    return r;
}
fxv3 fxm4_mul_point(fxm4 m, fxv3 p)
{
    return fxv3_mk(
        fpm_sat_i64(fpm_mul_i64(m.m[0], p.x) + fpm_mul_i64(m.m[4], p.y) + fpm_mul_i64(m.m[8],  p.z) + m.m[12]),
        fpm_sat_i64(fpm_mul_i64(m.m[1], p.x) + fpm_mul_i64(m.m[5], p.y) + fpm_mul_i64(m.m[9],  p.z) + m.m[13]),
        fpm_sat_i64(fpm_mul_i64(m.m[2], p.x) + fpm_mul_i64(m.m[6], p.y) + fpm_mul_i64(m.m[10], p.z) + m.m[14]));
}
fxv3 fxm4_mul_dir(fxm4 m, fxv3 v)
{
    return fxv3_mk(
        fpm_sat_i64(fpm_mul_i64(m.m[0], v.x) + fpm_mul_i64(m.m[4], v.y) + fpm_mul_i64(m.m[8],  v.z)),
        fpm_sat_i64(fpm_mul_i64(m.m[1], v.x) + fpm_mul_i64(m.m[5], v.y) + fpm_mul_i64(m.m[9],  v.z)),
        fpm_sat_i64(fpm_mul_i64(m.m[2], v.x) + fpm_mul_i64(m.m[6], v.y) + fpm_mul_i64(m.m[10], v.z)));
}
fxm4 fxm4_translate(fx x, fx y, fx z)
{
    fxm4 r = fxm4_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}
fxm4 fxm4_scale(fx sx, fx sy, fx sz)
{
    fxm4 r = fxm4_identity();
    r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
    return r;
}
fxm4 fxm4_rotate_x(fpm_i32 brad)
{
    fx c = fx_cos(brad), s = fx_sin(brad);
    fxm4 r = fxm4_identity();
    r.m[5] = c;  r.m[9]  = -s;
    r.m[6] = s;  r.m[10] =  c;
    return r;
}
fxm4 fxm4_rotate_y(fpm_i32 brad)
{
    fx c = fx_cos(brad), s = fx_sin(brad);
    fxm4 r = fxm4_identity();
    r.m[0] =  c;  r.m[8]  = s;
    r.m[2] = -s;  r.m[10] = c;
    return r;
}
fxm4 fxm4_rotate_z(fpm_i32 brad)
{
    fx c = fx_cos(brad), s = fx_sin(brad);
    fxm4 r = fxm4_identity();
    r.m[0] = c;  r.m[4] = -s;
    r.m[1] = s;  r.m[5] =  c;
    return r;
}
fxm4 fxm4_transpose(fxm4 a)
{
    fxm4 r;
    for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++)
        r.m[c*4+row] = a.m[row*4+c];
    return r;
}

/* Right-handed perspective, camera looks down -Z, clip z in [-w, w]. */
fxm4 fxm4_perspective(fpm_i32 fov, fx aspect, fx znear, fx zfar)
{
    fpm_i32 half = fov / 2;
    fx s = fx_sin(half), c = fx_cos(half);
    fx f = fx_div(c, s);                        /* cot(fov/2) = 1/tan */

    fxm4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0;
    r.m[0]  = fx_div(f, aspect);
    r.m[5]  = f;
    fx nf   = znear - zfar;
    r.m[10] = fx_div(zfar + znear, nf);
    r.m[11] = fx_from_int(-1);
    r.m[14] = fx_div(fx_mul(fx_mul(fx_from_int(2), zfar), znear), nf);
    r.m[15] = 0;
    return r;
}

/* Right-handed lookAt. */
fxm4 fxm4_lookat(fxv3 eye, fxv3 target, fxv3 up)
{
    fxv3 f = fxv3_normalize(fxv3_sub(target, eye));    /* forward */
    fxv3 s = fxv3_normalize(fxv3_cross(f, up));        /* right   */
    fxv3 u = fxv3_cross(s, f);                         /* true up */

    fxm4 r = fxm4_identity();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
    /* basis components are unit-scale (|v| <= ~1), but negate via int64 anyway
     * so a saturated FX_MIN can never be UB-negated */
    r.m[2]  = fpm_sat_i64(-(fpm_i64)f.x);
    r.m[6]  = fpm_sat_i64(-(fpm_i64)f.y);
    r.m[10] = fpm_sat_i64(-(fpm_i64)f.z);
    r.m[12] = fpm_sat_i64(-(fpm_i64)fxv3_dot(s, eye));
    r.m[13] = fpm_sat_i64(-(fpm_i64)fxv3_dot(u, eye));
    r.m[14] = fxv3_dot(f, eye);
    return r;
}

/* Inverse of an affine 4x4 (see fpm.h): invert the upper-left 3x3 and map the
 * translation by -Uinv * t. Valid for rotation + (non-uniform) scale + translate
 * ONLY -- the bottom row must be (0,0,0,1). If the 3x3 is singular the result
 * carries an identity rotation block and translation -t (not a full identity).
 *
 * The translation is computed straight from the UNROUNDED adjugate (Cramer):
 *   nt = -inv(U)*t = -(adj(B)*t') / det * 2^(st - s)
 * instead of multiplying the already-quantized inv(U) by t -- that double
 * rounding is amplified by |U| on the round trip (measured 0.044 identity
 * error at uniform scale 200 vs ~0.004 this way). */
fxm4 fxm4_inverse_affine(fxm4 A)
{
    fxm3 U;
    U.m[0] = A.m[0]; U.m[1] = A.m[1]; U.m[2] = A.m[2];
    U.m[3] = A.m[4]; U.m[4] = A.m[5]; U.m[5] = A.m[6];
    U.m[6] = A.m[8]; U.m[7] = A.m[9]; U.m[8] = A.m[10];

    fpm_inv3 c = inv3_prepare(U);

    fxm4 r = fxm4_identity();
    if (c.det == 0) {
        /* singular: identity block, translation -t */
        r.m[12] = fpm_sat_i64(-(fpm_i64)A.m[12]);
        r.m[13] = fpm_sat_i64(-(fpm_i64)A.m[13]);
        r.m[14] = fpm_sat_i64(-(fpm_i64)A.m[14]);
        return r;
    }

    /* rotation/scale block: inv(U) = (adj(B)/det) * 2^-s */
    for (int i = 0; i < 9; i++) {
        fpm_i64 q = (c.adj[i] * FX_ONE) / c.det;
        r.m[(i / 3) * 4 + (i % 3)] = unshift_sat(q, -c.s);
    }

    /* translation: normalize t to t' = t * 2^-st with |t'| < 2^24, then
     * nt_row = -((adj(B) * t')_row / det) * 2^(st - s).
     * adj terms < 2^33, t' < 2^24 -> products < 2^57, 3-term sum < 2^59;
     * (Q16.16 adj * Q16.16 t' sum) / (Q16.16 det) lands directly in Q16.16. */
    fpm_i64 t64[3] = { A.m[12], A.m[13], A.m[14] };
    fpm_i64 maxt = 0;
    for (int i = 0; i < 3; i++) {
        fpm_i64 v = t64[i] < 0 ? -t64[i] : t64[i];
        if (v > maxt) maxt = v;
    }
    int st = 0;
    while (maxt >= ((fpm_i64)1 << 24)) { maxt >>= 1; st++; }   /* st <= 7 */
    for (int row = 0; row < 3; row++) {
        fpm_i64 num = c.adj[0*3+row] * (t64[0] >> st)
                    + c.adj[1*3+row] * (t64[1] >> st)
                    + c.adj[2*3+row] * (t64[2] >> st);
        fpm_i64 y = num / c.det;                     /* (inv(B)*t')_row, Q16.16 */
        r.m[12 + row] = unshift_sat(-y, st - c.s);   /* negate + undo scales */
    }
    return r;
}
