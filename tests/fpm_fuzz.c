/*
 * fpm_fuzz.c -- differential fuzz of the fpm Q16.16 library vs libm doubles.
 * ==========================================================================
 * Fixed-seed xorshift64, ~5.7M mixed-distribution random inputs across every
 * function family, asserting the DOCUMENTED contracts:
 *
 *   scalars    2M   mul/div vs clamped double (<=1.5 LSB in range, EXACT
 *                   FX_MAX/FX_MIN saturation out of range, /0 by numerator
 *                   sign), floor+frac==a, abs>=0, floor<=a<=ceil
 *   trig       2M   sin/cos wrap consistency for ANY int32 brad,
 *                   sin^2+cos^2 within 1e-3; atan2 <= 1 brad vs libm
 *                   (wrapped), INCLUDING tiny raw integer coords -- the
 *                   round-2 audit bug class (was 57 brads before the
 *                   pre-normalization fix)
 *   asin/acos  500k asin(a)+acos(a) == 256 brads within 2
 *   normalize  1M   |len-1| <= 1.2e-2 over the documented envelope
 *                   |v| in [0.01, 655]; nonzero input never maps to zero
 *   inverse    200k random affine (rotY*rotX * non-uniform scale 0.05..50 *
 *                   translation +-2000): M*inverse_affine(M) ~= I within
 *                   2e-2 -- SKIPPING inputs whose true (double) inverse
 *                   translation or entries exceed ~30000, i.e. cases that
 *                   are unrepresentable in Q16.16 and correctly saturate
 *                   per contract (the first fuzz run flagged exactly those;
 *                   verification showed range limits, not bugs)
 *
 * Build & run (host, ~1 s):
 *   gcc -std=gnu11 -O2 -DFPM_HOSTTEST -I userspace/lib/fpm \
 *       tests/fpm_fuzz.c -o /tmp/fpm_fuzz -lm && /tmp/fpm_fuzz
 * Prints per-family worst cases and "FPM FUZZ: PASS" / "FAIL"; exit 0/1.
 * Deterministic: any violation prints reproducer inputs as raw int32.
 */
#include <stdio.h>
#include <math.h>

#include "fpm.c"

static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static unsigned long long xr(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}
static fx rfx(void) {           /* mixed distribution: full / small / near-limit */
    unsigned long long r = xr();
    switch (r % 4) {
    case 0:  return (fx)(r >> 32);                          /* full range   */
    case 1:  return (fx)((int)(r >> 32) % 65536);           /* ~[-1,1]      */
    case 2:  return (fx)((int)(r >> 32) % (200 * 65536));   /* ~[-200,200]  */
    default: { fx lim[] = { FX_MAX, FX_MIN, 0, 1, -1, FX_ONE, -FX_ONE };
               return lim[r % 7]; }
    }
}
static double fxd(fx a) { return (double)a / 65536.0; }
static const double HI = 2147483647.0 / 65536.0, LO = -2147483648.0 / 65536.0;
static double wrapb(double d) {
    while (d >  512.0) d -= 1024.0;
    while (d < -512.0) d += 1024.0;
    return d;
}
static long viol = 0;
#define V(cond, fam, ...) do { if (!(cond)) { \
    if (viol < 8) { printf("VIOLATION %s: ", fam); printf(__VA_ARGS__); printf("\n"); } \
    viol++; } } while (0)

int main(void) {
    fpm_init();
    const double BR = 1024.0 / (2.0 * M_PI);
    double w_mul = 0, w_atan = 0, w_norm = 0, w_inv = 0, w_trig = 0;

    /* --- scalars: 2M --- */
    for (long i = 0; i < 2000000; i++) {
        fx a = rfx(), b = rfx();
        double ar = fxd(a), br = fxd(b);
        double p = ar * br;
        fx g = fx_mul(a, b);
        if (p >= HI) V(g == FX_MAX, "mul", "sat+ a=%d b=%d g=%d", a, b, g);
        else if (p <= LO) V(g == FX_MIN, "mul", "sat- a=%d b=%d g=%d", a, b, g);
        else { double e = fabs(fxd(g) - p) * 65536.0; if (e > w_mul) w_mul = e;
               V(e <= 1.5, "mul", "a=%d b=%d e=%.2f", a, b, e); }
        g = fx_div(a, b);
        if (b == 0) V(g == (a < 0 ? FX_MIN : FX_MAX), "div0", "a=%d g=%d", a, g);
        else { double q = ar / br;
               if (q >= HI) V(g == FX_MAX, "div", "sat+ a=%d b=%d", a, b);
               else if (q <= LO) V(g == FX_MIN, "div", "sat- a=%d b=%d", a, b);
               else V(fabs(fxd(g) - q) * 65536.0 <= 1.5, "div", "a=%d b=%d", a, b); }
        V(fx_floor(a) + fx_frac(a) == a, "frac", "a=%d", a);
        V(fx_abs(a) >= 0, "abs", "a=%d", a);
        fx fl = fx_floor(a), ce = fx_ceil(a);
        V(fl <= a && (ce >= a || ce == FX_MAX), "floorceil", "a=%d", a);
    }

    /* --- trig: 2M random brads + atan2 pairs --- */
    for (long i = 0; i < 2000000; i++) {
        int br2 = (int)xr();
        fx s = fx_sin(br2), c = fx_cos(br2);
        V(s == fx_sin(br2 & 1023) && c == fx_cos(br2 & 1023), "wrap", "b=%d", br2);
        double id = fxd(s)*fxd(s) + fxd(c)*fxd(c);
        if (fabs(id - 1.0) > w_trig) w_trig = fabs(id - 1.0);
        V(fabs(id - 1.0) <= 1e-3, "sin2cos2", "b=%d id=%f", br2, id);
        fx y = rfx(), x = rfx();
        if ((xr() & 3) == 0) { y = (fx)((int)(xr() % 17) - 8); x = (fx)((int)(xr() % 17) - 8); }
        if (!(x == 0 && y == 0)) {
            fx a2 = fx_atan2(y, x);
            double ref = atan2((double)y, (double)x) * BR;
            double e = fabs(wrapb((double)a2 / 65536.0 - ref));
            if (e > w_atan) w_atan = e;
            V(e <= 1.0, "atan2", "y=%d x=%d e=%.3f", y, x, e);
        }
    }

    /* --- asin+acos identity: 500k --- */
    for (long i = 0; i < 500000; i++) {
        fx a = (fx)((int)(xr() % (2 * 65536 + 1)) - 65536);
        fx sum = fx_asin(a) + fx_acos(a);          /* == 90 deg == 256 brads */
        V(fabs((double)sum / 65536.0 - 256.0) <= 2.0, "asinacos", "a=%d", a);
    }

    /* --- normalize: 1M within the documented envelope |v| in [0.01, 655] --- */
    for (long i = 0; i < 1000000; i++) {
        double mag = 0.01 * pow(655.0 / 0.01, (double)(xr() % 10000) / 10000.0);
        double th = (double)(xr() % 62832) / 10000.0, ph = (double)(xr() % 31416) / 10000.0;
        fxv3 v = fxv3_mk((fx)llround(mag * cos(th) * sin(ph) * 65536.0),
                         (fx)llround(mag * sin(th) * sin(ph) * 65536.0),
                         (fx)llround(mag * cos(ph) * 65536.0));
        if (v.x == 0 && v.y == 0 && v.z == 0) continue;
        fxv3 n = fxv3_normalize(v);
        double len = sqrt(fxd(n.x)*fxd(n.x) + fxd(n.y)*fxd(n.y) + fxd(n.z)*fxd(n.z));
        double e = fabs(len - 1.0);
        if (e > w_norm) w_norm = e;
        V(e <= 1.2e-2, "normalize", "v=(%d,%d,%d) len=%f", v.x, v.y, v.z, len);
        V(!(n.x == 0 && n.y == 0 && n.z == 0), "normzero", "v=(%d,%d,%d)", v.x, v.y, v.z);
    }

    /* --- affine inverse roundtrip: 200k random rot+scale+trans --- */
    for (long i = 0; i < 200000; i++) {
        int b1 = (int)(xr() & 1023), b2 = (int)(xr() & 1023);
        double s1 = 0.05 * pow(1000.0, (double)(xr() % 1000) / 1000.0);   /* 0.05..50 */
        double s2 = 0.05 * pow(1000.0, (double)(xr() % 1000) / 1000.0);
        double s3 = 0.05 * pow(1000.0, (double)(xr() % 1000) / 1000.0);
        fx t1 = (fx)((int)(xr() >> 32) % (2000 * 65536));
        fx t2 = (fx)((int)(xr() >> 32) % (2000 * 65536));
        fx t3 = (fx)((int)(xr() >> 32) % (2000 * 65536));
        fxm4 M = fxm4_mul(fxm4_translate(t1, t2, t3),
                  fxm4_mul(fxm4_rotate_y(b1), fxm4_mul(fxm4_rotate_x(b2),
                           fxm4_scale((fx)llround(s1 * 65536.0),
                                      (fx)llround(s2 * 65536.0),
                                      (fx)llround(s3 * 65536.0)))));
        /* representability guard: skip cases whose TRUE inverse exceeds the
         * Q16.16 range (fpm saturates those per contract; see header) */
        {
            double dU[3][3], dt[3] = { fxd(M.m[12]), fxd(M.m[13]), fxd(M.m[14]) };
            for (int cc = 0; cc < 3; cc++) for (int rr = 0; rr < 3; rr++)
                dU[rr][cc] = fxd(M.m[cc*4+rr]);
            double det = dU[0][0]*(dU[1][1]*dU[2][2]-dU[1][2]*dU[2][1])
                       - dU[0][1]*(dU[1][0]*dU[2][2]-dU[1][2]*dU[2][0])
                       + dU[0][2]*(dU[1][0]*dU[2][1]-dU[1][1]*dU[2][0]);
            if (fabs(det) < 1e-6) continue;
            double inv[3][3];
            inv[0][0] =  (dU[1][1]*dU[2][2]-dU[1][2]*dU[2][1])/det;
            inv[0][1] = -(dU[0][1]*dU[2][2]-dU[0][2]*dU[2][1])/det;
            inv[0][2] =  (dU[0][1]*dU[1][2]-dU[0][2]*dU[1][1])/det;
            inv[1][0] = -(dU[1][0]*dU[2][2]-dU[1][2]*dU[2][0])/det;
            inv[1][1] =  (dU[0][0]*dU[2][2]-dU[0][2]*dU[2][0])/det;
            inv[1][2] = -(dU[0][0]*dU[1][2]-dU[0][2]*dU[1][0])/det;
            inv[2][0] =  (dU[1][0]*dU[2][1]-dU[1][1]*dU[2][0])/det;
            inv[2][1] = -(dU[0][0]*dU[2][1]-dU[0][1]*dU[2][0])/det;
            inv[2][2] =  (dU[0][0]*dU[1][1]-dU[0][1]*dU[1][0])/det;
            int skip = 0;
            for (int rr = 0; rr < 3 && !skip; rr++) {
                double nt = -(inv[rr][0]*dt[0] + inv[rr][1]*dt[1] + inv[rr][2]*dt[2]);
                if (fabs(nt) > 30000.0) skip = 1;
                for (int cc = 0; cc < 3; cc++)
                    if (fabs(inv[rr][cc]) > 30000.0) skip = 1;
            }
            if (skip) continue;
        }
        fxm4 I = fxm4_mul(M, fxm4_inverse_affine(M));
        for (int c2 = 0; c2 < 4; c2++) for (int r2 = 0; r2 < 4; r2++) {
            double e = fabs(fxd(I.m[c2*4+r2]) - (c2 == r2 ? 1.0 : 0.0));
            if (e > w_inv) w_inv = e;
            V(e <= 2e-2, "inv", "i=%ld s=(%.2f,%.2f,%.2f) e=%.4f", i, s1, s2, s3, e);
        }
    }

    printf("worst: mul=%.2f LSB atan2=%.4f brad sin2cos2=%.2e norm=%.2e inv=%.2e\n",
           w_mul, w_atan, w_trig, w_norm, w_inv);
    printf(viol ? "FPM FUZZ: FAIL violations=%ld\n" : "FPM FUZZ: PASS (0 violations)\n", viol);
    return viol ? 1 : 0;
}
