/*
 * AutomationOS shared animation / easing library  (userspace/lib/anim)
 * --------------------------------------------------------------------
 * A tiny, dependency-free, FIXED-POINT animation toolkit for ring-3 apps
 * and the compositor. There is NO floating point anywhere in this lib
 * (ring 3 has no FP), so everything is integer / fixed-point math.
 *
 * Two number domains are used:
 *
 *   1. "norm"  : a normalized progress / output value in [0, 256].
 *                256 == 1.0. This is the domain of every easing fn:
 *                given t in [0,256] it returns eased value in [0,256]
 *                (a few overshoot curves may briefly exceed the range,
 *                 e.g. ease_out_back / elastic -- callers should clamp
 *                 the final pixel result if that matters).
 *
 *   2. Q16.16  : a 32-bit signed fixed-point number (16 integer bits,
 *                16 fraction bits). Used internally by the curves that
 *                need sub-unit precision (cubic/back/elastic/bounce).
 *                ANIM_FX_ONE == (1 << 16).
 *
 * The only kernel dependency is SYS_GET_TICKS_MS (monotonic ms since
 * boot) for anim_now_ms(); the syscall is issued inline so this lib
 * links with zero other objects.
 *
 * Usage is header-light: include "anim.h", link anim.o.
 */
#ifndef AOS_ANIM_H
#define AOS_ANIM_H

/* ------------------------------------------------------------------ */
/* Minimal fixed-width types (freestanding: no <stdint.h> guaranteed) */
/* ------------------------------------------------------------------ */
typedef unsigned int       anim_u32;
typedef int                anim_i32;
typedef long long          anim_i64;
typedef unsigned long long anim_u64;

/* Normalized domain: 256 == 1.0 */
#define ANIM_ONE   256

/* Q16.16 fixed-point */
typedef anim_i32 anim_fx;          /* Q16.16 */
#define ANIM_FX_SHIFT 16
#define ANIM_FX_ONE   (1 << ANIM_FX_SHIFT)

/* ------------------------------------------------------------------ */
/* Fixed-point primitives (exposed so apps can reuse them)            */
/* ------------------------------------------------------------------ */
static inline anim_fx anim_fx_from_int(anim_i32 v) { return v << ANIM_FX_SHIFT; }
static inline anim_i32 anim_fx_to_int(anim_fx v)   { return v >> ANIM_FX_SHIFT; }
/* round-to-nearest when converting back to integer */
static inline anim_i32 anim_fx_round(anim_fx v) {
    return (v + (ANIM_FX_ONE >> 1)) >> ANIM_FX_SHIFT;
}
anim_fx anim_fx_mul(anim_fx a, anim_fx b);   /* (a*b) >> 16, 64-bit safe   */
anim_fx anim_fx_div(anim_fx a, anim_fx b);   /* (a<<16) / b, 64-bit safe   */
anim_fx anim_fx_sqrt(anim_fx v);             /* integer sqrt of Q16.16     */

/* norm[0..256] <-> Q16.16 helpers */
static inline anim_fx anim_norm_to_fx(anim_i32 n) {
    /* n/256 in Q16.16  ==  (n << 16) / 256  ==  n << 8 */
    return n << (ANIM_FX_SHIFT - 8);
}
static inline anim_i32 anim_fx_to_norm(anim_fx f) {
    /* f * 256 then back to int, with rounding */
    return anim_fx_round(f << 8);
}

/* ------------------------------------------------------------------ */
/* Easing functions.                                                  */
/* Signature: t in [0,256] -> eased value in [0,256].                 */
/* (overshoot curves may transiently leave the range)                 */
/* ------------------------------------------------------------------ */
typedef anim_i32 (*anim_ease_fn)(anim_i32 t);

anim_i32 anim_ease_linear(anim_i32 t);
anim_i32 anim_ease_in_quad(anim_i32 t);
anim_i32 anim_ease_out_quad(anim_i32 t);
anim_i32 anim_ease_inout_quad(anim_i32 t);
anim_i32 anim_ease_out_cubic(anim_i32 t);
anim_i32 anim_ease_out_back(anim_i32 t);      /* overshoot then settle      */
anim_i32 anim_ease_out_elastic(anim_i32 t);   /* spring-y oscillation       */
anim_i32 anim_ease_out_bounce(anim_i32 t);    /* bouncing-ball landing      */

/* smoothstep: classic 3t^2 - 2t^3 on the normalized domain          */
anim_i32 anim_smoothstep(anim_i32 t);

/* ------------------------------------------------------------------ */
/* Tweening.                                                          */
/* ------------------------------------------------------------------ */

/*
 * Integer tween. Maps elapsed_ms within [0,duration_ms] through `ease`
 * and interpolates between `start` and `end`. Clamps to `end` once
 * elapsed >= duration. `ease` may be NULL (== linear). If duration is
 * 0, returns `end`.
 */
anim_i32 anim_tween(anim_i32 start, anim_i32 end,
                    anim_u32 duration_ms, anim_u32 elapsed_ms,
                    anim_ease_fn ease);

/*
 * Lower-level: interpolate start..end by an already-eased normalized
 * t in [0,256]. (start + (end-start)*t/256, rounded, 64-bit safe.)
 */
anim_i32 anim_lerp_i(anim_i32 start, anim_i32 end, anim_i32 t_norm);

/*
 * Lerp two packed 0xAARRGGBB colors channel-wise by eased t in [0,256].
 */
anim_u32 anim_lerp_argb(anim_u32 from, anim_u32 to, anim_i32 t_norm);

/* Convenience: full color tween over time (computes t internally).   */
anim_u32 anim_tween_argb(anim_u32 from, anim_u32 to,
                         anim_u32 duration_ms, anim_u32 elapsed_ms,
                         anim_ease_fn ease);

/* ------------------------------------------------------------------ */
/* Spring / critically-damped smoothing.                              */
/* ------------------------------------------------------------------ */

/*
 * A tiny critically-damped "spring toward target" stepper, useful for
 * frame-by-frame following (cursor trails, smooth-scroll, knob slide)
 * without storing a whole animation. State is just current + velocity,
 * both in Q16.16. `stiffness` is in [1..256] (256 ~= snappy). `dt_ms`
 * is the frame delta. Advances *pos / *vel in place; returns *pos.
 *
 * This is an explicit semi-implicit Euler integrator tuned for ~60fps
 * frame deltas; it is stable for dt_ms up to ~64ms.
 */
anim_fx anim_spring_step(anim_fx *pos, anim_fx *vel,
                         anim_fx target, anim_i32 stiffness,
                         anim_u32 dt_ms);

/* ------------------------------------------------------------------ */
/* Time source (monotonic milliseconds since boot via SYS_GET_TICKS). */
/* ------------------------------------------------------------------ */
anim_u64 anim_now_ms(void);

#endif /* AOS_ANIM_H */
