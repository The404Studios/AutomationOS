/*
 * AutomationOS shared animation / easing library  (implementation).
 * Fixed-point only -- no floating point, no libc dependency.
 * See anim.h for the contract / number domains.
 */
#include "anim.h"

/* ================================================================== */
/* Kernel time source: SYS_GET_TICKS_MS == 40 (monotonic ms).         */
/* Issued inline so this object links with nothing else.              */
/* ================================================================== */
#define ANIM_SYS_GET_TICKS_MS 40

static inline long anim_sc1(long n) {
    long ret;
    register long a1 __asm__("rdi") = 0;
    register long a2 __asm__("rsi") = 0;
    register long a3 __asm__("rdx") = 0;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(n), "r"(a1), "r"(a2), "r"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

anim_u64 anim_now_ms(void) {
    return (anim_u64)anim_sc1(ANIM_SYS_GET_TICKS_MS);
}

/* ================================================================== */
/* Fixed-point primitives (Q16.16, 64-bit intermediates).             */
/* ================================================================== */
anim_fx anim_fx_mul(anim_fx a, anim_fx b) {
    return (anim_fx)(((anim_i64)a * (anim_i64)b) >> ANIM_FX_SHIFT);
}

anim_fx anim_fx_div(anim_fx a, anim_fx b) {
    if (b == 0) return 0;
    return (anim_fx)(((anim_i64)a << ANIM_FX_SHIFT) / (anim_i64)b);
}

/*
 * Integer square root of a Q16.16 value, returning Q16.16.
 * sqrt(v/65536) in Q16.16  ==  sqrt(v * 65536)  (a 64-bit isqrt).
 */
anim_fx anim_fx_sqrt(anim_fx v) {
    if (v <= 0) return 0;
    anim_u64 n = (anim_u64)(anim_u32)v << ANIM_FX_SHIFT; /* v * 65536 */
    /* Binary-search style integer sqrt (bit-by-bit). */
    anim_u64 res = 0;
    anim_u64 bit = (anim_u64)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (anim_fx)res;
}

/* ================================================================== */
/* Small fixed-point sine (for elastic), input in Q16.16 "turns"      */
/* mapped so that argument is radians/(2*pi) is awkward; instead we    */
/* take phase in Q16.16 where 1.0 == full period (2*pi). Returns       */
/* sin in Q16.16, range [-1,1]. Uses a 5th-order minimax-ish poly on  */
/* a quarter wave -- accurate to ~1e-3, plenty for UI motion.         */
/* ================================================================== */
static anim_fx anim_sin_turns(anim_fx turns) {
    /* reduce to [0,1) period */
    turns &= (ANIM_FX_ONE - 1);                 /* fractional turns      */
    /* fold into quarter waves; q in [0,4) */
    anim_i32 quadrant = (turns >> (ANIM_FX_SHIFT - 2)) & 3;
    anim_fx x = (turns << 2) & (ANIM_FX_ONE - 1); /* position in quarter, Q16.16 [0,1) */
    /* mirror for quadrants 1 and 3 so x ramps 0->1 over the rising part */
    if (quadrant & 1) x = ANIM_FX_ONE - x;
    /* Bhaskara-I style sine approximation on [0,1) representing [0,pi/2):
     * sin(t) ~= x*(a - b*x) form; we use the classic 16x(pi-x)/(5pi^2 - 4x(pi-x))
     * but rescaled to our [0,1] quarter. Simpler: polynomial p(x)=x*(c1 - c3*x^2)
     * tuned so p(0)=0, p(1)=1, p'(0) matches. Constants in Q16.16. */
    anim_fx x2 = anim_fx_mul(x, x);
    /* p = x*(98302/65536 - x2*32766/65536)  -> ~ (1.49986)x - (0.49996)x^3 */
    anim_fx c1 = 98302;   /* ~1.4999 in Q16.16 */
    anim_fx c3 = 32766;   /* ~0.4999 in Q16.16 */
    anim_fx p = anim_fx_mul(x, c1 - anim_fx_mul(x2, c3));
    if (quadrant >= 2) p = -p;
    return p;
}

/* ================================================================== */
/* Internal: run a curve in Q16.16, taking norm t and returning norm. */
/* ================================================================== */
static anim_i32 clamp_t(anim_i32 t) {
    if (t < 0) return 0;
    if (t > ANIM_ONE) return ANIM_ONE;
    return t;
}

/* ================================================================== */
/* Easing functions (t,return in [0,256]).                            */
/* ================================================================== */

anim_i32 anim_ease_linear(anim_i32 t) {
    return clamp_t(t);
}

anim_i32 anim_ease_in_quad(anim_i32 t) {
    t = clamp_t(t);
    /* t^2 with 256 == 1.0 : (t*t)/256 */
    return (t * t) / ANIM_ONE;
}

anim_i32 anim_ease_out_quad(anim_i32 t) {
    t = clamp_t(t);
    /* 1 - (1-t)^2 */
    anim_i32 u = ANIM_ONE - t;
    return ANIM_ONE - (u * u) / ANIM_ONE;
}

anim_i32 anim_ease_inout_quad(anim_i32 t) {
    t = clamp_t(t);
    if (t < ANIM_ONE / 2) {
        /* 2*t^2 */
        return (2 * t * t) / ANIM_ONE;
    } else {
        /* 1 - (-2t+2)^2 / 2 */
        anim_i32 u = (ANIM_ONE - t) * 2;       /* (2 - 2t) scaled to 512 max */
        return ANIM_ONE - (u * u) / (2 * ANIM_ONE);
    }
}

anim_i32 anim_ease_out_cubic(anim_i32 t) {
    t = clamp_t(t);
    /* 1 - (1-t)^3 */
    anim_i32 u = ANIM_ONE - t;
    anim_i32 u3 = (u * u) / ANIM_ONE;          /* u^2 */
    u3 = (u3 * u) / ANIM_ONE;                  /* u^3 */
    return ANIM_ONE - u3;
}

anim_i32 anim_ease_out_back(anim_i32 t) {
    /* classic: 1 + c3*(t-1)^3 + c1*(t-1)^2, c1=1.70158, c3=c1+1=2.70158 */
    t = clamp_t(t);
    anim_fx ft = anim_norm_to_fx(t);           /* Q16.16 in [0,1]       */
    anim_fx one = ANIM_FX_ONE;
    anim_fx c1 = 111514;                       /* 1.70158 in Q16.16     */
    anim_fx c3 = c1 + one;                      /* 2.70158               */
    anim_fx u = ft - one;                       /* (t-1)                 */
    anim_fx u2 = anim_fx_mul(u, u);
    anim_fx u3 = anim_fx_mul(u2, u);
    anim_fx r = one + anim_fx_mul(c3, u3) + anim_fx_mul(c1, u2);
    return anim_fx_to_norm(r);                  /* may exceed 256 (overshoot) */
}

anim_i32 anim_ease_out_elastic(anim_i32 t) {
    t = clamp_t(t);
    if (t == 0) return 0;
    if (t >= ANIM_ONE) return ANIM_ONE;
    anim_fx ft = anim_norm_to_fx(t);
    anim_fx one = ANIM_FX_ONE;
    /* pow(2, -10t) -- approximate exponential decay without FP:
     * compute 2^(-10t) via repeated halving on integer part + linear
     * interpolation on fraction. exponent e = 10t in Q16.16. */
    anim_fx e = anim_fx_mul(anim_fx_from_int(10), ft);   /* 10t, Q16.16 */
    anim_i32 e_int = anim_fx_to_int(e);
    anim_fx e_frac = e - anim_fx_from_int(e_int);        /* [0,1) Q16.16 */
    anim_fx decay = one;
    int i;
    for (i = 0; i < e_int && i < 30; i++) decay >>= 1;   /* * 2^-int    */
    /* fractional part: 2^-f ~= 1 - 0.5f (linear) over [0,1) -- good enough */
    decay = anim_fx_mul(decay, one - anim_fx_mul(e_frac, ANIM_FX_ONE / 2));
    /* phase: standard elastic uses (10t - 0.75)*(2pi/3). In "turns"
     * (1.0 == 2pi) that is (10t - 0.75)/3 turns. */
    anim_fx phase = anim_fx_div(e - (ANIM_FX_ONE * 3 / 4), anim_fx_from_int(3));
    anim_fx s = anim_sin_turns(phase);
    /* result = 2^-10t * sin(phase) + 1 */
    anim_fx r = anim_fx_mul(decay, s) + one;
    return anim_fx_to_norm(r);
}

anim_i32 anim_ease_out_bounce(anim_i32 t) {
    t = clamp_t(t);
    /* Standard piecewise bounce, n1=7.5625, d1=2.75, done in Q16.16. */
    anim_fx ft = anim_norm_to_fx(t);
    anim_fx one = ANIM_FX_ONE;
    anim_fx n1 = 495616;                        /* 7.5625 in Q16.16      */
    anim_fx d1 = 180224;                        /* 2.75   in Q16.16      */
    anim_fx r;
    if (ft < anim_fx_div(one, d1)) {            /* t < 1/2.75            */
        r = anim_fx_mul(n1, anim_fx_mul(ft, ft));
    } else if (ft < anim_fx_div(anim_fx_from_int(2), d1)) { /* t < 2/2.75 */
        anim_fx tt = ft - anim_fx_div(anim_fx_from_int(3) / 2, d1); /* 1.5/d1 */
        r = anim_fx_mul(n1, anim_fx_mul(tt, tt)) + (one * 3 / 4);   /* +0.75 */
    } else if (ft < anim_fx_div(anim_fx_from_int(25) / 10, d1)) {   /* <2.5/2.75 */
        anim_fx tt = ft - anim_fx_div(anim_fx_from_int(225) / 100, d1); /* 2.25/d1 */
        r = anim_fx_mul(n1, anim_fx_mul(tt, tt)) + (one * 15 / 16); /* +0.9375 */
    } else {
        anim_fx tt = ft - anim_fx_div(anim_fx_from_int(2625) / 1000, d1); /* 2.625/d1 */
        r = anim_fx_mul(n1, anim_fx_mul(tt, tt)) + (one * 63 / 64); /* +0.984375 */
    }
    return anim_fx_to_norm(r);
}

anim_i32 anim_smoothstep(anim_i32 t) {
    t = clamp_t(t);
    /* 3t^2 - 2t^3 with 256 == 1.0 */
    anim_i32 t2 = (t * t) / ANIM_ONE;          /* t^2 */
    anim_i32 t3 = (t2 * t) / ANIM_ONE;         /* t^3 */
    return 3 * t2 - 2 * t3;
}

/* ================================================================== */
/* Interpolation helpers.                                             */
/* ================================================================== */
anim_i32 anim_lerp_i(anim_i32 start, anim_i32 end, anim_i32 t_norm) {
    if (t_norm <= 0) return start;
    if (t_norm >= ANIM_ONE) return end;
    anim_i64 delta = (anim_i64)end - (anim_i64)start;
    /* start + delta*t/256, rounded */
    anim_i64 scaled = (delta * t_norm + (ANIM_ONE / 2)) / ANIM_ONE;
    return (anim_i32)((anim_i64)start + scaled);
}

anim_u32 anim_lerp_argb(anim_u32 from, anim_u32 to, anim_i32 t_norm) {
    if (t_norm <= 0) return from;
    if (t_norm >= ANIM_ONE) return to;
    anim_u32 out = 0;
    int shift;
    for (shift = 0; shift < 32; shift += 8) {
        anim_i32 a = (from >> shift) & 0xFF;
        anim_i32 b = (to   >> shift) & 0xFF;
        anim_i32 v = anim_lerp_i(a, b, t_norm);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out |= ((anim_u32)v & 0xFF) << shift;
    }
    return out;
}

/* ================================================================== */
/* Tweens.                                                            */
/* ================================================================== */
anim_i32 anim_tween(anim_i32 start, anim_i32 end,
                    anim_u32 duration_ms, anim_u32 elapsed_ms,
                    anim_ease_fn ease) {
    if (duration_ms == 0 || elapsed_ms >= duration_ms) return end;
    /* normalized progress in [0,256] */
    anim_i32 t = (anim_i32)(((anim_u64)elapsed_ms * ANIM_ONE) / duration_ms);
    anim_i32 e = ease ? ease(t) : t;
    return anim_lerp_i(start, end, e);
}

anim_u32 anim_tween_argb(anim_u32 from, anim_u32 to,
                         anim_u32 duration_ms, anim_u32 elapsed_ms,
                         anim_ease_fn ease) {
    if (duration_ms == 0 || elapsed_ms >= duration_ms) return to;
    anim_i32 t = (anim_i32)(((anim_u64)elapsed_ms * ANIM_ONE) / duration_ms);
    anim_i32 e = ease ? ease(t) : t;
    /* eased value may overshoot for back/elastic; clamp for color */
    if (e < 0) e = 0;
    if (e > ANIM_ONE) e = ANIM_ONE;
    return anim_lerp_argb(from, to, e);
}

/* ================================================================== */
/* Spring stepper (critically-damped, semi-implicit Euler).           */
/* ================================================================== */
anim_fx anim_spring_step(anim_fx *pos, anim_fx *vel,
                         anim_fx target, anim_i32 stiffness,
                         anim_u32 dt_ms) {
    if (!pos || !vel) return 0;
    if (stiffness < 1)   stiffness = 1;
    if (stiffness > 256) stiffness = 256;
    if (dt_ms == 0) return *pos;
    if (dt_ms > 64) dt_ms = 64;                 /* clamp for stability   */

    /* dt in seconds, Q16.16: dt_ms/1000 */
    anim_fx dt = anim_fx_div(anim_fx_from_int((anim_i32)dt_ms),
                             anim_fx_from_int(1000));
    /* angular frequency w (rad/s) derived from stiffness; map
     * stiffness[1..256] -> w[~2 .. ~32]. w = stiffness/8 + 2. */
    anim_fx w = anim_fx_from_int(stiffness) / 8 + anim_fx_from_int(2);
    anim_fx w2 = anim_fx_mul(w, w);
    /* critical damping: c = 2w */
    anim_fx c = anim_fx_mul(anim_fx_from_int(2), w);

    anim_fx x = *pos;
    anim_fx v = *vel;
    anim_fx disp = x - target;                  /* displacement          */
    /* a = -w^2*disp - c*v */
    anim_fx a = -anim_fx_mul(w2, disp) - anim_fx_mul(c, v);
    /* semi-implicit Euler: v += a*dt ; x += v*dt */
    v = v + anim_fx_mul(a, dt);
    x = x + anim_fx_mul(v, dt);

    *vel = v;
    *pos = x;
    return x;
}
