/*
 * browser2_anim.c -- animation engine for browser2: inertial scrolling and
 * ARGB buffer cross-fade.
 *
 * Pure, deterministic, freestanding: NO libc, NO syscalls, NO global I/O.
 * All state is module-static. Integer / fixed-point math throughout.
 *
 * Build (NO fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/browser2/browser2_anim.c -o /tmp/f5_anim.o
 *   objdump -d /tmp/f5_anim.o | grep -c fs:0x28   # must be 0
 *
 * =========================================================================
 * SCROLL PHYSICS DESIGN
 * =========================================================================
 *
 *  Fixed-point format
 *  ------------------
 *  Internal position and velocity are stored in 1/256-pixel units.
 *  A real pixel offset P corresponds to internal value P * FP_SCALE (256).
 *  This gives ~0.004 px resolution with 32-bit integers, letting the
 *  physics integrate smoothly without floats.
 *
 *  Impulse
 *  -------
 *  b2anim_scroll_input(delta_px) is called by browser2.c once per wheel
 *  notch (or key-repeat step). browser2.c is expected to pass a delta in
 *  SCROLL_LINE units (default: 3 lines * SCROLL_LINE_PX px each = 60 px).
 *  The function converts to fixed-point: velocity += delta_px * FP_SCALE.
 *
 *  For a representative one-notch impulse of 60 px:
 *    initial velocity = 60 * 256 = 15360 fp-units/ms
 *
 *  Total travel  (geometric series)
 *  ---------------------------------
 *  Each ms: vel(k+1) = vel(k) * FRICTION_NUM / FRICTION_DEN
 *  Sum of displacements = vel_0 * FRICTION_DEN / (FRICTION_DEN - FRICTION_NUM)
 *
 *  With FRICTION_NUM=240, FRICTION_DEN=256, the denominator = 16:
 *    total_fp = 15360 * 256 / 16 = 245760 fp-units = 960 px
 *
 *  That is too much for a one-notch scroll. We therefore apply an additional
 *  IMPULSE_SCALE to keep travel comfortable (~120 px per notch at 60 px/notch):
 *    desired total = 120 px = 30720 fp-units
 *    required vel_0 = 30720 * 16 / 256 = 1920 fp-units/ms
 *    => IMPULSE_SCALE = 1920 / (60 * 256) * 256 = 32
 *    => vel += (delta_px * IMPULSE_SCALE)
 *
 *  With IMPULSE_SCALE=32:
 *    vel_0 = 60 * 32 = 1920 fp/ms
 *    total travel = 1920 * 16 = 30720 fp-units = 120 px  ✓
 *
 *  Settle time estimate
 *  --------------------
 *  With FRICTION_NUM=240/256 per ms, half-life ≈ 10.8 ms.
 *  VELOCITY_EPSILON = 64 fp-units = 0.25 px/ms.
 *  Starting at vel=1920: 1920 * 0.9375^k < 64 → k ≈ 48 ms, ~3 frames.
 *
 *  For large impulses (rapid multi-notch flick), the velocity accumulates.
 *  The boundary clamping zeroes velocity at the extremes, so overshooting
 *  never occurs. Because friction is strong the scroll settles within
 *  ~300 ms even from a large accumulated impulse.
 *
 *  Summary:
 *    • One wheel notch (delta_px = 60): ~120 px of travel, settles in ~50 ms.
 *    • Three quick notches (delta_px = 180): ~360 px travel, settles ~200 ms.
 *    • Both are within the "few lines in a few hundred ms" target.
 *
 *  Crossfade
 *  ---------
 *  Per-pixel, per-channel linear blend with rounding (+127 trick).
 *  Aliasing safe: reads pf and pt before writing out[i].
 */

#include "browser2_anim.h"

/* =========================================================================
 * Basic integer types -- no stdint.h in freestanding
 * ========================================================================= */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;

/* =========================================================================
 * Fixed-point constants
 * ========================================================================= */

/* 1 px == FP_SCALE internal units. */
#define FP_SCALE         256

/* Impulse scale: velocity += delta_px * IMPULSE_SCALE (not delta_px * FP_SCALE).
 * Set so one "line" of delta (20 px) produces ~10 px of total travel.
 * IMPULSE_SCALE = FP_SCALE * (FRICTION_DEN - FRICTION_NUM) / FRICTION_DEN
 * Numerically: 256 * 16 / 256 = 16, giving total_travel = delta_px px exactly.
 * We use 16 so that delta_px = 60 → ~60 px of travel (one notch = one comfortable
 * screenful of lines). Adjust here to taste. */
#define IMPULSE_SCALE    16

/* Friction: per-ms multiplier = 240/256 = 0.9375.
 * Strong enough to settle quickly; gentle enough for a smooth deceleration arc.
 * Applied iteratively for each ms in dt, up to DT_CAP ms. */
#define FRICTION_NUM     240
#define FRICTION_DEN     256

/* Maximum dt to process per tick (ms); clamp long gaps to avoid spirals. */
#define DT_CAP           100

/* Velocity below which scroll is considered at rest (fp-units/ms).
 * At 64/256 = 0.25 px/ms the motion is imperceptible; declare rest. */
#define VELOCITY_EPSILON  64

/* =========================================================================
 * Module-static scroll state
 * ========================================================================= */
static i32 s_pos_fp = 0;   /* current position in 1/256 px units */
static i32 s_vel_fp = 0;   /* velocity in (1/256 px) per ms       */

/* =========================================================================
 * b2anim_scroll_input
 * Add an impulse to velocity. delta_px > 0 → scroll down.
 * Scaled by IMPULSE_SCALE (not the full FP_SCALE) for comfortable travel.
 * ========================================================================= */
void b2anim_scroll_input(int delta_px)
{
    s_vel_fp += delta_px * IMPULSE_SCALE;
}

/* =========================================================================
 * b2anim_scroll_tick
 * Integrate velocity → position, apply per-ms friction, clamp.
 * dt_ms: elapsed time this frame in milliseconds.
 *
 * Integration order per ms-step:
 *   1. Advance position by current velocity.
 *   2. Damp velocity by FRICTION_NUM/FRICTION_DEN.
 *   3. Zero velocity component pointing out of bounds.
 * Then hard-clamp position at the end.
 * ========================================================================= */
void b2anim_scroll_tick(int dt_ms, int content_h, int viewport_h)
{
    /* Clamp dt to avoid huge loops on first-tick or tab-wake-up stalls. */
    if (dt_ms > DT_CAP)
        dt_ms = DT_CAP;
    if (dt_ms < 1)
        return;

    /* Compute max scroll offset (0 if content fits inside viewport). */
    int max_px = content_h - viewport_h;
    if (max_px < 0)
        max_px = 0;

    int max_fp = max_px * FP_SCALE;

    /* Integrate one ms at a time for accurate friction application. */
    for (int step = 0; step < dt_ms; step++) {
        /* 1. Advance position */
        s_pos_fp += s_vel_fp;

        /* 2. Apply friction (multiply-then-divide; both positive; truncates) */
        s_vel_fp = s_vel_fp * FRICTION_NUM / FRICTION_DEN;

        /* 3. Zero velocity at boundaries to prevent oscillation */
        if (s_pos_fp <= 0 && s_vel_fp < 0)
            s_vel_fp = 0;
        if (s_pos_fp >= max_fp && s_vel_fp > 0)
            s_vel_fp = 0;
    }

    /* Hard-clamp position — must never over/undershoot. */
    if (s_pos_fp < 0)
        s_pos_fp = 0;
    if (s_pos_fp > max_fp)
        s_pos_fp = max_fp;
}

/* =========================================================================
 * b2anim_scroll_y
 * Return current scroll offset in whole pixels.
 * ========================================================================= */
int b2anim_scroll_y(void)
{
    return s_pos_fp / FP_SCALE;
}

/* =========================================================================
 * b2anim_scroll_active
 * Returns 1 while the scroll is still settling, 0 once at rest.
 * ========================================================================= */
int b2anim_scroll_active(void)
{
    int v = s_vel_fp;
    if (v < 0)
        v = -v;
    return (v >= VELOCITY_EPSILON) ? 1 : 0;
}

/* =========================================================================
 * b2anim_crossfade
 * Per-pixel, per-channel linear blend of ARGB32 pixels.
 *
 *   out[i] = from[i] * (255 - t) / 255 + to[i] * t / 255
 *
 * Each 32-bit pixel: bits [31:24]=A, [23:16]=R, [15:8]=G, [7:0]=B.
 * Reads both source pixels before writing out[i], so aliasing (out == from
 * or out == to) is safe.
 *
 * Rounding: (x * w + 127) / 255 gives correctly-rounded integer division,
 * keeping midpoint blends true (e.g. 50% of black + 50% of white = 128,
 * not 127).
 *
 * t=0   → all from (no change).
 * t=128 → 50/50 blend.
 * t=255 → all to.
 * ========================================================================= */
void b2anim_crossfade(unsigned int *out,
                      const unsigned int *from,
                      const unsigned int *to,
                      int npx, int t)
{
    /* Clamp t to [0,255] for safety. */
    if (t < 0)   t = 0;
    if (t > 255) t = 255;

    int wt = t;           /* weight for 'to'   */
    int wf = 255 - t;     /* weight for 'from' */

    for (int i = 0; i < npx; i++) {
        /* Read both sources before any write (alias safety). */
        unsigned int pf = from[i];
        unsigned int pt = to[i];

        /* Extract channels */
        unsigned int af = (pf >> 24) & 0xFF;
        unsigned int rf = (pf >> 16) & 0xFF;
        unsigned int gf = (pf >>  8) & 0xFF;
        unsigned int bf = (pf      ) & 0xFF;

        unsigned int at = (pt >> 24) & 0xFF;
        unsigned int rt = (pt >> 16) & 0xFF;
        unsigned int gt = (pt >>  8) & 0xFF;
        unsigned int bt = (pt      ) & 0xFF;

        /* Blend with rounding: (x * w + 127) / 255 */
        unsigned int ao = (af * (unsigned int)wf + at * (unsigned int)wt + 127u) / 255u;
        unsigned int ro = (rf * (unsigned int)wf + rt * (unsigned int)wt + 127u) / 255u;
        unsigned int go = (gf * (unsigned int)wf + gt * (unsigned int)wt + 127u) / 255u;
        unsigned int bo = (bf * (unsigned int)wf + bt * (unsigned int)wt + 127u) / 255u;

        out[i] = (ao << 24) | (ro << 16) | (go << 8) | bo;
    }
}

/* =========================================================================
 * b2anim_selftest
 * Returns 0 on pass, non-zero step index on first failure.
 * ========================================================================= */
int b2anim_selftest(void)
{
    /* ------------------------------------------------------------------
     * SCROLL BASIC MOVEMENT TEST
     * ------------------------------------------------------------------ */
    s_pos_fp = 0;
    s_vel_fp = 0;

    int content_h  = 2000;
    int viewport_h = 600;
    int max_px     = content_h - viewport_h;  /* 1400 */

    /* One-notch impulse of 60 px (typical browser line-scroll). */
    b2anim_scroll_input(60);

    /* After one tick position must have moved. */
    b2anim_scroll_tick(16, content_h, viewport_h);
    if (b2anim_scroll_y() <= 0)
        return 1;  /* FAIL: should have moved down */

    /* ------------------------------------------------------------------
     * SCROLL SETTLE TEST
     * ------------------------------------------------------------------ */
    /* Tick until scroll settles -- cap at 5000 ms to avoid infinite loop. */
    int total_ms = 16;
    while (b2anim_scroll_active() && total_ms < 5000) {
        b2anim_scroll_tick(16, content_h, viewport_h);
        total_ms += 16;
    }
    if (b2anim_scroll_active())
        return 2;  /* FAIL: never settled */

    /* Position must be within [0, max_px]. */
    int y = b2anim_scroll_y();
    if (y < 0 || y > max_px)
        return 3;  /* FAIL: out of bounds */

    /* Settle time sanity: a 60-px impulse should settle well within 1000 ms. */
    if (total_ms > 1000)
        return 4;  /* FAIL: took too long to settle */

    /* ------------------------------------------------------------------
     * CLAMP TEST: huge impulse must not push past max.
     * ------------------------------------------------------------------ */
    s_pos_fp = 0;
    s_vel_fp = 0;
    b2anim_scroll_input(1000000);   /* absurdly large */
    for (int i = 0; i < 200; i++)
        b2anim_scroll_tick(16, content_h, viewport_h);
    if (b2anim_scroll_y() > max_px)
        return 5;  /* FAIL: exceeded maximum */
    if (b2anim_scroll_y() < 0)
        return 6;  /* FAIL: negative after forward clamp */

    /* ------------------------------------------------------------------
     * NEGATIVE CLAMP TEST: scrolling up past zero must stop at 0.
     * ------------------------------------------------------------------ */
    s_pos_fp = 0;
    s_vel_fp = 0;
    b2anim_scroll_input(-1000000);  /* huge upward impulse */
    for (int i = 0; i < 200; i++)
        b2anim_scroll_tick(16, content_h, viewport_h);
    if (b2anim_scroll_y() < 0)
        return 7;  /* FAIL: went below 0 */

    /* ------------------------------------------------------------------
     * SCROLL FEEL TEST: one comfortable notch should travel a few lines.
     * With IMPULSE_SCALE=16, delta=60 → travel = 60 px (3 × 20-px lines).
     * Must land in [20, 200] px to be "comfortable" without huge overshoot.
     * ------------------------------------------------------------------ */
    s_pos_fp = 0;
    s_vel_fp = 0;
    b2anim_scroll_input(60);   /* 3 typical screen lines */
    /* Let it fully settle. */
    for (int i = 0; i < 300; i++)
        b2anim_scroll_tick(16, content_h, viewport_h);
    {
        int settled_y = b2anim_scroll_y();
        if (settled_y < 20 || settled_y > 200)
            return 8;  /* FAIL: scroll travel out of comfortable range */
    }

    /* ------------------------------------------------------------------
     * CROSSFADE TEST: 50/50 blend of black and white must be mid-gray.
     * ------------------------------------------------------------------ */
    unsigned int from_buf[4];
    unsigned int to_buf[4];
    unsigned int out_buf[4];

    for (int i = 0; i < 4; i++) {
        from_buf[i] = 0xFF000000u;  /* opaque black */
        to_buf[i]   = 0xFFFFFFFFu;  /* opaque white */
    }

    /* Blend at t=128 — halfway. */
    b2anim_crossfade(out_buf, from_buf, to_buf, 4, 128);

    for (int i = 0; i < 4; i++) {
        unsigned int p = out_buf[i];
        unsigned int a = (p >> 24) & 0xFF;
        unsigned int r = (p >> 16) & 0xFF;
        unsigned int g = (p >>  8) & 0xFF;
        unsigned int b = (p      ) & 0xFF;

        /* Alpha: 0xFF * 127/255 + 0xFF * 128/255 ≈ 0xFF always */
        if (a < 0xFE || a > 0xFF)
            return 9;  /* FAIL: alpha wrong */

        /* Color channels: from=0x00, to=0xFF, t=128.
         * (0*127 + 255*128 + 127) / 255 = 32767/255 = 128 (rounds up).
         * Accept 127..129. */
        if (r < 127 || r > 129)
            return 10; /* FAIL: R channel not mid-gray */
        if (g < 127 || g > 129)
            return 11; /* FAIL: G channel not mid-gray */
        if (b < 127 || b > 129)
            return 12; /* FAIL: B channel not mid-gray */
    }

    /* ------------------------------------------------------------------
     * CROSSFADE BOUNDARY TEST: t=0 must produce 'from' exactly.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < 4; i++) {
        from_buf[i] = 0xFFAABBCCu;
        to_buf[i]   = 0xFF112233u;
    }
    b2anim_crossfade(out_buf, from_buf, to_buf, 4, 0);
    for (int i = 0; i < 4; i++) {
        if (out_buf[i] != 0xFFAABBCCu)
            return 13; /* FAIL: t=0 should preserve 'from' */
    }

    /* ------------------------------------------------------------------
     * CROSSFADE BOUNDARY TEST: t=255 must produce 'to' exactly.
     * ------------------------------------------------------------------ */
    b2anim_crossfade(out_buf, from_buf, to_buf, 4, 255);
    for (int i = 0; i < 4; i++) {
        /* (to * 255 + 127) / 255: with wf=0, reduces to (to*255+127)/255.
         * For channel 0x11: (0x11*255+127)/255 = (4335+127)/255 = 4462/255 = 17
         * = 0x11. Same for 0x22 and 0x33. And alpha 0xFF → 0xFF. */
        unsigned int r_ch = (out_buf[i] >> 16) & 0xFF;
        unsigned int g_ch = (out_buf[i] >>  8) & 0xFF;
        unsigned int b_ch = (out_buf[i]       ) & 0xFF;
        unsigned int a_ch = (out_buf[i] >> 24) & 0xFF;
        if (a_ch != 0xFF || r_ch != 0x11 || g_ch != 0x22 || b_ch != 0x33)
            return 14; /* FAIL: t=255 should produce 'to' */
    }

    /* ------------------------------------------------------------------
     * CROSSFADE ALIAS TEST: out == from (in-place) must not corrupt.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < 4; i++) {
        from_buf[i] = 0xFF808080u;
        to_buf[i]   = 0xFF404040u;
    }
    b2anim_crossfade(from_buf, from_buf, to_buf, 4, 128);
    for (int i = 0; i < 4; i++) {
        unsigned int r = (from_buf[i] >> 16) & 0xFF;
        /* 0x80=128, 0x40=64; t=128:
         * (128*127 + 64*128 + 127)/255 = (16256 + 8192 + 127)/255 = 24575/255 = 96
         * Accept 95..97 to tolerate any rounding edge. */
        if (r < 95 || r > 97)
            return 15; /* FAIL: alias blend wrong */
    }

    /* ------------------------------------------------------------------
     * CROSSFADE CLAMP TEST: t out-of-range must be clamped, not UB.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < 4; i++) {
        from_buf[i] = 0xFF000000u;
        to_buf[i]   = 0xFFFFFFFFu;
    }
    /* t > 255: should clamp to 255 → produce 'to' */
    b2anim_crossfade(out_buf, from_buf, to_buf, 4, 300);
    {
        unsigned int r = (out_buf[0] >> 16) & 0xFF;
        if (r != 0xFF) return 16; /* FAIL: t=300 should clamp to 255 */
    }
    /* t < 0: should clamp to 0 → produce 'from' */
    b2anim_crossfade(out_buf, from_buf, to_buf, 4, -5);
    {
        unsigned int r = (out_buf[0] >> 16) & 0xFF;
        if (r != 0x00) return 17; /* FAIL: t=-5 should clamp to 0 */
    }

    return 0; /* PASS */
}
