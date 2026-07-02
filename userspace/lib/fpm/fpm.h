/*
 * fpm.h -- FPM: from-scratch fixed-point math library (freestanding, ring 3).
 * ==========================================================================
 *
 * A self-contained Q16.16 fixed-point math kernel for the software renderer,
 * the games, and any userspace that must do real-number math with NO floating
 * point, NO libm, and NO libc. It is a FUNCTIONAL superset of the ad-hoc Q16.16
 * core embedded in userspace/lib/g3d/g3d.c (not source-compatible: types and
 * some names differ -- vec3->fxv3, mat4->fxm4, fx_round returns fx), hardened:
 *
 *   - saturating arithmetic (fx_mul/div never trap or wrap silently; the
 *     vector/matrix combining sums saturate too, via int64 accumulation),
 *   - a real Newton fx_rsqrt (not 1/sqrt(x) round-tripped),
 *   - from-scratch CORDIC fx_atan2/asin/acos (no tables of floats, no libm),
 *   - overflow-safe magnitudes (fx_len2/fx_len3),
 *   - column-major fxm3/fxm4 with an affine inverse for normal transforms.
 *
 * DESIGN CONSTRAINTS (identical rationale to g3d.c / asteroids.c):
 *   - value = real * 65536, carried in int32 with int64 intermediates.
 *   - Our own fixed-width typedefs (no <stdint.h>).
 *   - The freestanding build links no soft-float/libc helpers and passes a
 *     STRICTER gate than the games' canary-only grep: fpm.o must objdump to
 *     0 xmm AND 0 fs:0x28 (see build_test/mathtest_check.sh and the pragma
 *     block at the top of fpm.c).
 *
 * DUAL COMPILE: this same source compiles under the freestanding ring-3
 * toolchain (link fpm.o into an app) AND under a normal host gcc so the pure
 * math can be KAT-tested on the host -- see tests/fpm_hosttest.c, which #includes
 * fpm.c as one TU with FPM_HOSTTEST defined (skips the freestanding #pragma).
 *
 * Freestanding build flags (same as scripts/build_all.sh CF):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/fpm/fpm.c -o /tmp/fpm.o
 *
 * NOTE ON COEXISTENCE WITH g3d: fpm defines the same short names (fx, fx_mul,
 * fx_sin, ...). It is an INDEPENDENT library -- do not link fpm.o and g3d.o into
 * the same program (duplicate symbols). Apps use one or the other.
 */
#ifndef FPM_H
#define FPM_H

/* ---- fixed-width typedefs (no stdint in freestanding ring 3) ---- */
typedef unsigned int        fpm_u32;
typedef int                 fpm_i32;
typedef unsigned long long  fpm_u64;
typedef long long           fpm_i64;

/* ====================================================================== *
 *  Q16.16 fixed-point scalar type
 * ====================================================================== */
typedef fpm_i32 fx;                     /* Q16.16 fixed-point scalar */

#define FX_SHIFT    16
#define FX_ONE      (1 << FX_SHIFT)             /* 1.0                 */
#define FX_HALF     (FX_ONE >> 1)               /* 0.5                 */
#define FX_MAX      ((fx)0x7FFFFFFF)            /* +32767.99998  (sat) */
#define FX_MIN      ((fx)(-0x7FFFFFFF - 1))     /* -32768.0      (sat) */

/* Precomputed transcendental constants, round(k * 65536). */
#define FX_PI       205887                      /* round(pi   * 65536) */
#define FX_TAU      411775                      /* round(2*pi * 65536) */
#define FX_HALF_PI  102944                      /* round(pi/2 * 65536) */

/* Brad angle unit: 1024 steps == one full turn (matches g3d). */
#define FPM_ANG_STEPS  1024
#define FPM_ANG_MASK   (FPM_ANG_STEPS - 1)

/* Clamp a 64-bit intermediate into the fx range. This is THE saturation
 * primitive: every combining operation (incl. the vector/matrix inlines below)
 * funnels its int64 intermediate through here so no int32 add/negate can wrap
 * or hit signed-overflow UB. */
static inline fx fpm_sat_i64(fpm_i64 v) {
    if (v > FX_MAX) return FX_MAX;
    if (v < FX_MIN) return FX_MIN;
    return (fx)v;
}

/* ---- int <-> fx ---- */
/* Saturates outside [-32768, 32767] (32768.0 is not representable). */
static inline fx fx_from_int(fpm_i32 i) {
    return fpm_sat_i64((fpm_i64)i << FX_SHIFT);
}
/* Floor toward -inf (arithmetic shift; NOT truncate-toward-zero for negatives)
 * -- same shift g3d's fx_to_int does. */
static inline fpm_i32 fx_to_int(fx a)        { return (fpm_i32)(a >> FX_SHIFT); }
/* Fractional part in [0,1) consistent with floor: fx_floor(a) + fx_frac(a) == a. */
static inline fx      fx_frac(fx a)          { return (fx)((fpm_u32)a & 0xFFFFu); }

/* |FX_MIN| = 32768.0 is unrepresentable -> saturates to FX_MAX (negating
 * INT32_MIN directly would be signed-overflow UB). */
static inline fx fx_abs(fx a)        { return a < 0 ? fpm_sat_i64(-(fpm_i64)a) : a; }
static inline fx fx_min(fx a, fx b)  { return a < b ? a : b; }
static inline fx fx_max(fx a, fx b)  { return a > b ? a : b; }
static inline fx fx_clamp(fx v, fx lo, fx hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* fx from a rational n/d (d==0 -> saturate by sign of n). Inline: used to build
 * literals like 3/2 without FP; deterministic and always available.
 * (n * FX_ONE rather than n << 16: identical value/codegen, but the multiply is
 * defined for negative n in strict C, where the left shift is not.) */
static inline fx fx_ratio(fpm_i32 n, fpm_i32 d) {
    if (d == 0) return n < 0 ? FX_MIN : FX_MAX;
    return fpm_sat_i64(((fpm_i64)n * FX_ONE) / (fpm_i64)d);
}

/* ---- scalar core (implemented in fpm.c; saturating) ---- */
fx fx_mul(fx a, fx b);          /* (a*b) via int64, saturates on overflow      */
fx fx_div(fx a, fx b);          /* a/b via int64; b==0 -> saturate (no trap)    */
fx fx_lerp(fx a, fx b, fx t);   /* a + (b-a)*t, t in Q16.16 (unclamped)         */
fx fx_sat_add(fx a, fx b);      /* saturating add                               */
fx fx_sat_sub(fx a, fx b);      /* saturating subtract                          */
fx fx_floor(fx a);              /* largest integer <= a, as fx                  */
fx fx_ceil(fx a);               /* smallest integer >= a, as fx                 */
fx fx_round(fx a);              /* round half up (toward +inf), as fx           */

/* ---- roots ---- */
fx fx_sqrt(fx a);               /* sqrt of Q16.16, digit-by-digit isqrt         */
fx fx_rsqrt(fx a);              /* 1/sqrt via isqrt seed + Newton (see fpm.c)   */
fx fx_len2(fx x, fx y);         /* hypot(x,y), overflow-safe                    */
fx fx_len3(fx x, fx y, fx z);   /* |(x,y,z)|, overflow-safe                     */

/* ---- trig (brads; wraps &0x3FF). Tables self-init on first use. ---- */
void fpm_init(void);            /* generate the sine table (idempotent)         */
fx fx_sin(fpm_i32 brad);        /* sin(2*pi*brad/1024), Q16.16 in [-1,1]        */
fx fx_cos(fpm_i32 brad);        /* cos(2*pi*brad/1024)                          */
/* CORDIC (from scratch, no libm): results in brads. */
fx fx_atan2(fx y, fx x);        /* angle of (x,y) in brads, all quadrants; (0,0)->0 */
fx fx_asin(fx a);               /* asin(a), a in [-1,1], result in brads        */
fx fx_acos(fx a);               /* acos(a), a in [-1,1], result in brads        */

/* Convert integer degrees to brads. */
static inline fpm_i32 fpm_deg(fpm_i32 deg) {
    return (fpm_i32)(((fpm_i64)deg * FPM_ANG_STEPS) / 360);
}

/* ====================================================================== *
 *  Vectors  (Q16.16 components)
 * ====================================================================== */
typedef struct { fx x, y; }    fxv2;
typedef struct { fx x, y, z; } fxv3;

/* fpm_mul_i64: one Q16.16 product term kept UNSATURATED in int64 -- bit-
 * identical to fx_mul for any in-range result (same >>16 on the same int64
 * product), but lets dot/cross/matrix rows accumulate WITHOUT the int32 wrap
 * that plain `fx_mul(..) + fx_mul(..)` suffers (e.g. dot((181,181),(181,181)):
 * each term fits, the int32 sum does not). Sums of <=4 terms stay < 2^48. */
static inline fpm_i64 fpm_mul_i64(fx a, fx b) {
    return ((fpm_i64)a * (fpm_i64)b) >> FX_SHIFT;
}

static inline fxv2 fxv2_mk(fx x, fx y)        { fxv2 r; r.x=x; r.y=y; return r; }
static inline fxv2 fxv2_add(fxv2 a, fxv2 b)   { return fxv2_mk(fpm_sat_i64((fpm_i64)a.x+b.x), fpm_sat_i64((fpm_i64)a.y+b.y)); }
static inline fxv2 fxv2_sub(fxv2 a, fxv2 b)   { return fxv2_mk(fpm_sat_i64((fpm_i64)a.x-b.x), fpm_sat_i64((fpm_i64)a.y-b.y)); }
static inline fxv2 fxv2_scale(fxv2 a, fx s)   { return fxv2_mk(fx_mul(a.x,s), fx_mul(a.y,s)); }
static inline fx   fxv2_dot(fxv2 a, fxv2 b)   { return fpm_sat_i64(fpm_mul_i64(a.x,b.x) + fpm_mul_i64(a.y,b.y)); }
fx   fxv2_len(fxv2 a);                          /* overflow-safe */
fxv2 fxv2_normalize(fxv2 a);                    /* a/|a| via rsqrt; zero->zero */

static inline fxv3 fxv3_mk(fx x, fx y, fx z)  { fxv3 r; r.x=x; r.y=y; r.z=z; return r; }
static inline fxv3 fxv3_add(fxv3 a, fxv3 b)   { return fxv3_mk(fpm_sat_i64((fpm_i64)a.x+b.x), fpm_sat_i64((fpm_i64)a.y+b.y), fpm_sat_i64((fpm_i64)a.z+b.z)); }
static inline fxv3 fxv3_sub(fxv3 a, fxv3 b)   { return fxv3_mk(fpm_sat_i64((fpm_i64)a.x-b.x), fpm_sat_i64((fpm_i64)a.y-b.y), fpm_sat_i64((fpm_i64)a.z-b.z)); }
static inline fxv3 fxv3_scale(fxv3 a, fx s)   { return fxv3_mk(fx_mul(a.x,s), fx_mul(a.y,s), fx_mul(a.z,s)); }
static inline fx   fxv3_dot(fxv3 a, fxv3 b) {
    return fpm_sat_i64(fpm_mul_i64(a.x,b.x) + fpm_mul_i64(a.y,b.y) + fpm_mul_i64(a.z,b.z));
}
static inline fxv3 fxv3_cross(fxv3 a, fxv3 b) {
    return fxv3_mk(fpm_sat_i64(fpm_mul_i64(a.y,b.z) - fpm_mul_i64(a.z,b.y)),
                   fpm_sat_i64(fpm_mul_i64(a.z,b.x) - fpm_mul_i64(a.x,b.z)),
                   fpm_sat_i64(fpm_mul_i64(a.x,b.y) - fpm_mul_i64(a.y,b.x)));
}
fx   fxv3_len(fxv3 a);                          /* overflow-safe */
/* normalize precision note: the reciprocal length is a Q16.16 quantity, so the
 * relative error grows like |v|/65536 -- keep |v| <= ~1000 for <1% length
 * error (the KAT-proven envelope). Zero vector -> zero. */
fxv3 fxv3_normalize(fxv3 a);                    /* a/|a| via rsqrt; zero->zero */

/* ====================================================================== *
 *  Matrices (column-major: m[col*N + row], so M transforms v as M*v)
 * ====================================================================== */
typedef struct { fx m[9];  } fxm3;      /* 3x3 */
typedef struct { fx m[16]; } fxm4;      /* 4x4 */

/* --- 3x3 --- */
fxm3 fxm3_identity(void);
fxm3 fxm3_mul(fxm3 a, fxm3 b);          /* a*b (apply b first)  */
fxv3 fxm3_mul_vec(fxm3 m, fxv3 v);
fxm3 fxm3_transpose(fxm3 a);
/* General 3x3 inverse (adjugate/det with power-of-two magnitude normalization,
 * so it stays accurate from tiny (~0.01) through large (~1000+) uniform scales
 * -- see fpm.c). Singular -> identity. */
fxm3 fxm3_inverse(fxm3 a);

/* --- 4x4 --- */
fxm4 fxm4_identity(void);
fxm4 fxm4_mul(fxm4 a, fxm4 b);          /* a*b (apply b first)  */
fxv3 fxm4_mul_point(fxm4 m, fxv3 p);    /* transform a point (w=1), no divide */
fxv3 fxm4_mul_dir(fxm4 m, fxv3 v);      /* transform a direction (w=0)        */
fxm4 fxm4_translate(fx x, fx y, fx z);
fxm4 fxm4_scale(fx sx, fx sy, fx sz);
fxm4 fxm4_rotate_x(fpm_i32 brad);
fxm4 fxm4_rotate_y(fpm_i32 brad);
fxm4 fxm4_rotate_z(fpm_i32 brad);
fxm4 fxm4_transpose(fxm4 a);
/* Right-handed perspective (camera looks down -Z). fov in brads, aspect=w/h. */
fxm4 fxm4_perspective(fpm_i32 fov, fx aspect, fx znear, fx zfar);
/* Right-handed lookAt. */
fxm4 fxm4_lookat(fxv3 eye, fxv3 target, fxv3 up);
/* Inverse of an AFFINE 4x4 (upper-left 3x3 invertible, bottom row 0,0,0,1).
 * Handles rotation + (possibly non-uniform) scale + translation -- exactly the
 * class used for model/view matrices and normal transforms. NOT valid for a
 * projection matrix (perspective). Singular 3x3 -> identity rotation block
 * with translation -t (not a full identity matrix). */
fxm4 fxm4_inverse_affine(fxm4 a);

#endif /* FPM_H */
