/*
 * fpm_hosttest.c -- Host KAT battery for the fpm fixed-point math library.
 * ========================================================================
 * Compile + run with the SYSTEM gcc (NOT freestanding). Pulls in fpm.c as one
 * TU with FPM_HOSTTEST defined so the freestanding-only #pragma is skipped.
 * <math.h>/<stdio.h> are used ONLY by this harness -- never by fpm itself.
 *
 * Build & run:
 *   gcc -std=gnu11 -O2 -DFPM_HOSTTEST -I userspace/lib/fpm \
 *       tests/fpm_hosttest.c -o /tmp/fpm_hosttest -lm && /tmp/fpm_hosttest
 *
 * Every section prints its worst-case observed error vs the libm double
 * reference and asserts an explicit, documented bound. Prints
 *   "FPM HOSTTEST: PASS"  or  "FPM HOSTTEST: FAIL n=<count>"
 * and exits 0/1.
 */
#include <stdio.h>
#include <math.h>

#include "fpm.c"                 /* single-TU include of the implementation */

static int g_fail = 0;           /* count of failed CHECKs                  */
#define CHECK(cond, msg) do { \
        if (!(cond)) { printf("  FAIL: %s  (%s)\n", msg, #cond); g_fail++; } \
        else         { printf("  ok:   %s\n", msg); } \
    } while (0)

static double fxd(fx a) { return (double)a / 65536.0; }
static const double LSB = 1.0 / 65536.0;
static double BRAD_PER_RAD;      /* 1024/(2pi) */

/* wrap a brad difference into [-512,512] so 1023 vs -1 reads as 1 apart */
static double wrap_brad(double d) {
    while (d >  512.0) d -= 1024.0;
    while (d < -512.0) d += 1024.0;
    return d;
}

/* ------------------------------------------------------------------ *
 * 1. Scalar core: mul / div / ratio (grid incl. neg / overflow / sat) *
 *    plus floor/ceil/round/frac/lerp/sat_add/sat_sub                  *
 * ------------------------------------------------------------------ */
static void test_scalars(void)
{
    printf("[scalars: mul/div/ratio + rounding + saturating]\n");
    const double HI = fxd(FX_MAX), LO = fxd(FX_MIN);

    /* A grid spanning small fractions, integers, and near-overflow magnitudes,
     * both signs. fx values built exactly so the reference is exact. */
    fx grid[] = {
        0, 1, -1, FX_HALF, -FX_HALF, FX_ONE, -FX_ONE,
        fx_from_int(2), fx_from_int(-3), fx_from_int(7), fx_from_int(-11),
        fx_from_int(181), fx_from_int(-181),        /* 181^2 ~= 32761 < max */
        fx_from_int(200), fx_from_int(-200),        /* 200^2 = 40000 -> sat */
        fx_from_int(20000), fx_from_int(-20000),    /* huge -> sat          */
        fx_ratio(1,3), fx_ratio(-2,7), fx_ratio(7,3), FX_MAX, FX_MIN,
    };
    const int N = (int)(sizeof(grid)/sizeof(grid[0]));

    /* --- fx_mul: saturating --- */
    double mmax = 0; int mfail = 0;
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
        double ar = fxd(grid[i]), br = fxd(grid[j]);
        double pr = ar * br;
        fx got = fx_mul(grid[i], grid[j]);
        if (pr >= HI)      { if (got != FX_MAX) mfail++; }
        else if (pr <= LO) { if (got != FX_MIN) mfail++; }
        else { double e = fabs(fxd(got) - pr); if (e > mmax) mmax = e;
               if (e > 1.5*LSB) mfail++; }
    }
    printf("  fx_mul: worst in-range err = %.2e LSB, sat/fails=%d\n", mmax/LSB, mfail);
    CHECK(mfail == 0, "fx_mul grid (in-range <=1.5 LSB, overflow saturates)");

    /* --- fx_div: saturating; b==0 -> saturate by sign(a) --- */
    double dmax = 0; int dfail = 0;
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
        fx got = fx_div(grid[i], grid[j]);
        if (grid[j] == 0) { if (got != (grid[i] < 0 ? FX_MIN : FX_MAX)) dfail++; continue; }
        double qr = fxd(grid[i]) / fxd(grid[j]);
        if (qr >= HI)      { if (got != FX_MAX) dfail++; }
        else if (qr <= LO) { if (got != FX_MIN) dfail++; }
        else { double e = fabs(fxd(got) - qr); if (e > dmax) dmax = e;
               if (e > 1.5*LSB) dfail++; }
    }
    printf("  fx_div: worst in-range err = %.2e LSB, sat/fails=%d\n", dmax/LSB, dfail);
    CHECK(dfail == 0, "fx_div grid (in-range <=1.5 LSB, /0 saturates by sign)");

    /* --- fx_ratio --- */
    int rfail = 0;
    for (int n = -20; n <= 20; n++) for (int d = -9; d <= 9; d++) {
        fx got = fx_ratio(n, d);
        if (d == 0) { if (got != (n < 0 ? FX_MIN : FX_MAX)) rfail++; continue; }
        double qr = (double)n / (double)d;
        if (fabs(fxd(got) - qr) > 1.5*LSB) rfail++;
    }
    CHECK(rfail == 0, "fx_ratio n/d (<=1.5 LSB, /0 saturates by sign)");

    /* --- floor / ceil / round / frac --- */
    CHECK(fx_floor(fx_ratio(5,2)) == fx_from_int(2),  "floor(2.5)=2");
    CHECK(fx_floor(fx_ratio(-5,2))== fx_from_int(-3), "floor(-2.5)=-3");
    CHECK(fx_ceil (fx_ratio(5,2)) == fx_from_int(3),  "ceil(2.5)=3");
    CHECK(fx_ceil (fx_ratio(-5,2))== fx_from_int(-2), "ceil(-2.5)=-2");
    CHECK(fx_round(fx_ratio(5,2)) == fx_from_int(3),  "round(2.5)=3 (half up)");
    CHECK(fx_round(fx_ratio(12,5))== fx_from_int(2),  "round(2.4)=2");
    CHECK(fx_round(fx_ratio(-5,2))== fx_from_int(-2), "round(-2.5)=-2 (half up)");
    /* frac(-0.25) = 0.75, and floor + frac reconstructs the value */
    fx v = fx_ratio(-1,4);
    CHECK(fabs(fxd(fx_frac(v)) - 0.75) < 1e-9, "frac(-0.25)=0.75");
    CHECK(fx_floor(v) + fx_frac(v) == v, "floor(a)+frac(a)==a");

    /* --- lerp --- */
    fx lp = fx_lerp(fx_from_int(10), fx_from_int(20), fx_ratio(1,4));
    CHECK(fabs(fxd(lp) - 12.5) < 2*LSB, "lerp(10,20,0.25)=12.5");

    /* --- saturating add/sub --- */
    CHECK(fx_sat_add(FX_MAX, FX_ONE) == FX_MAX, "sat_add(MAX,1)=MAX");
    CHECK(fx_sat_add(FX_MIN, -FX_ONE) == FX_MIN, "sat_add(MIN,-1)=MIN");
    CHECK(fx_sat_sub(FX_MIN, FX_ONE) == FX_MIN, "sat_sub(MIN,1)=MIN");
    CHECK(fx_sat_sub(FX_MAX, -FX_ONE) == FX_MAX, "sat_sub(MAX,-1)=MAX");
    CHECK(fx_sat_add(fx_from_int(3), fx_from_int(4)) == fx_from_int(7), "sat_add(3,4)=7");

    /* --- clamp / min / max / abs --- */
    CHECK(fx_clamp(fx_from_int(5), 0, fx_from_int(3)) == fx_from_int(3), "clamp hi");
    CHECK(fx_clamp(fx_from_int(-5),0, fx_from_int(3)) == 0, "clamp lo");
    CHECK(fx_abs(fx_from_int(-9)) == fx_from_int(9), "abs(-9)=9");
}

/* ------------------------------------------------------------------ *
 * 2. Roots: sqrt + rsqrt                                             *
 * ------------------------------------------------------------------ */
static void test_roots(void)
{
    printf("[roots: sqrt/rsqrt]\n");

    struct { fx in; double want; } sq[] = {
        { 0, 0.0 }, { FX_ONE, 1.0 }, { fx_from_int(2), sqrt(2.0) },
        { fx_ratio(1,4), 0.5 }, { fx_from_int(10000), 100.0 },
        { FX_MAX, sqrt(fxd(FX_MAX)) },
    };
    double smax = 0; int sfail = 0;
    for (int i = 0; i < 6; i++) {
        double got = fxd(fx_sqrt(sq[i].in));
        double rel = sq[i].want > 1e-9 ? fabs(got - sq[i].want)/sq[i].want
                                       : fabs(got - sq[i].want);
        if (rel > smax) smax = rel;
        if (rel > 1e-4) sfail++;    /* spec 1e-3; observed worst 9.7e-6 */
    }
    printf("  fx_sqrt: worst rel err = %.2e\n", smax);
    CHECK(sfail == 0, "fx_sqrt spot values rel err <= 1e-4 (spec 1e-3)");

    /* rsqrt over [1e-3, 30000], geometric sweep. */
    double rmax = 0; int rfail = 0; double rworst_x = 0;
    for (double x = 1e-3; x <= 30000.0; x *= 1.05) {
        fx xf = (fx)llround(x * 65536.0);
        if (xf <= 0) continue;
        double xr = fxd(xf);                 /* the value actually stored     */
        double want = 1.0 / sqrt(xr);
        double got = fxd(fx_rsqrt(xf));
        double rel = fabs(got - want) / want;
        if (rel > rmax) { rmax = rel; rworst_x = xr; }
        if (rel > 5e-3) rfail++;
    }
    printf("  fx_rsqrt: worst rel err = %.2e (at x=%.4g)\n", rmax, rworst_x);
    CHECK(rfail == 0, "fx_rsqrt over [1e-3,30000] rel err <= 5e-3");
}

/* ------------------------------------------------------------------ *
 * 3. Trig: sin/cos (every brad), atan2 (10k+ pairs), asin/acos       *
 * ------------------------------------------------------------------ */
static void test_trig(void)
{
    printf("[trig: sin/cos/atan2/asin/acos]\n");
    fpm_init();

    /* sin/cos: EVERY brad 0..1023, abs err <= 0.002 */
    double tmax = 0; int tfail = 0;
    for (int b = 0; b < 1024; b++) {
        double ang = 2.0 * M_PI * b / 1024.0;
        double es = fabs(fxd(fx_sin(b)) - sin(ang));
        double ec = fabs(fxd(fx_cos(b)) - cos(ang));
        if (es > tmax) tmax = es;
        if (ec > tmax) tmax = ec;
        /* spec bound is 0.002; assert the ~4x-observed regression bound 2e-4
         * (observed worst 3e-5) -- integer math is deterministic, so any
         * loosening beyond this is a real regression, not platform noise */
        if (es > 2e-4 || ec > 2e-4) tfail++;
    }
    printf("  fx_sin/cos: worst abs err over all 1024 brads = %.5f\n", tmax);
    CHECK(tfail == 0, "fx_sin/fx_cos abs err <= 2e-4 (spec 0.002) for every brad");

    /* atan2: >10000 (y,x) pairs, all quadrants + axes, err <= 1 brad */
    double amax = 0; int afail = 0; long apairs = 0;
    for (int yi = -50; yi <= 50; yi++) for (int xi = -50; xi <= 50; xi++) {
        if (xi == 0 && yi == 0) {
            if (fx_atan2(0, 0) != 0) afail++;
            continue;
        }
        fx got = fx_atan2(fx_from_int(yi), fx_from_int(xi));
        double ref = atan2((double)yi, (double)xi) * BRAD_PER_RAD;
        /* got is a brad value in Q16.16; compare in brad units, wrapped */
        double e = fabs(wrap_brad((double)got/65536.0 - ref));
        if (e > amax) amax = e;
        if (e > 0.05) afail++;      /* spec 1 brad; observed worst 0.0052 */
        apairs++;
    }
    printf("  fx_atan2: worst err = %.4f brad over %ld pairs\n", amax, apairs);
    CHECK(afail == 0, "fx_atan2 err <= 0.05 brad (spec 1) all quadrants + axes");

    /* asin/acos: sweep [-1,1] step 1/512, err <= 2 brads */
    double asmax = 0, acmax = 0; int asfail = 0, acfail = 0;
    for (int i = -512; i <= 512; i++) {
        double a = (double)i / 512.0;
        fx af = (fx)llround(a * 65536.0);
        double aref = fxd(af);                  /* stored value */
        double ras = asin(aref) * BRAD_PER_RAD;
        double rac = acos(aref) * BRAD_PER_RAD;
        double eas = fabs(wrap_brad((double)fx_asin(af)/65536.0 - ras));
        double eac = fabs(wrap_brad((double)fx_acos(af)/65536.0 - rac));
        if (eas > asmax) asmax = eas;
        if (eac > acmax) acmax = eac;
        if (eas > 0.1) asfail++;    /* spec 2 brads; observed worst 0.0136 */
        if (eac > 0.1) acfail++;
    }
    printf("  fx_asin: worst err = %.4f brad; fx_acos: worst = %.4f brad\n", asmax, acmax);
    CHECK(asfail == 0, "fx_asin err <= 0.1 brad (spec 2) over [-1,1]");
    CHECK(acfail == 0, "fx_acos err <= 0.1 brad (spec 2) over [-1,1]");

    /* extreme aspect ratios + FX_MIN corners (CORDIC folding stress).
     * References use the RAW int values -- atan2 is scale-invariant. */
    {
        struct { fx y, x; } ex[] = {
            { FX_MAX, 1 }, { 1, FX_MAX }, { FX_MIN, 1 }, { 1, FX_MIN },
            { FX_MIN, FX_MIN }, { FX_MAX, FX_MIN }, { FX_MIN, FX_MAX },
        };
        int exfail = 0; double exmax = 0;
        for (int i = 0; i < 7; i++) {
            fx got = fx_atan2(ex[i].y, ex[i].x);
            double ref = atan2((double)ex[i].y, (double)ex[i].x) * BRAD_PER_RAD;
            double e = fabs(wrap_brad((double)got/65536.0 - ref));
            if (e > exmax) exmax = e;
            if (e > 1.0) exfail++;
        }
        printf("  fx_atan2 extremes (FX_MAX/FX_MIN corners): worst = %.4f brad\n", exmax);
        CHECK(exfail == 0, "fx_atan2 extreme-magnitude corners <= 1 brad");
    }
}

/* ------------------------------------------------------------------ *
 * 4. Vectors: normalize length -> 1                                  *
 * ------------------------------------------------------------------ */
static void test_vectors(void)
{
    printf("[vectors: normalize]\n");
    double nmax = 0; int nfail = 0;
    /* a fixed direction, scaled across magnitudes 1e-2 .. 1e3 */
    for (double mag = 1e-2; mag <= 1e3; mag *= 1.3) {
        double ux = 0.3, uy = -0.6, uz = 0.74162;   /* ~unit-ish direction */
        fxv3 v = fxv3_mk((fx)llround(ux*mag*65536.0),
                         (fx)llround(uy*mag*65536.0),
                         (fx)llround(uz*mag*65536.0));
        fxv3 n = fxv3_normalize(v);
        double len = sqrt(fxd(n.x)*fxd(n.x) + fxd(n.y)*fxd(n.y) + fxd(n.z)*fxd(n.z));
        double e = fabs(len - 1.0);
        if (e > nmax) nmax = e;
        if (e > 1e-2) nfail++;
    }
    printf("  fxv3_normalize: worst |len-1| = %.2e over mag 1e-2..1e3\n", nmax);
    CHECK(nfail == 0, "fxv3_normalize length -> 1 within 1e-2");

    /* 2D normalize spot check */
    fxv2 n2 = fxv2_normalize(fxv2_mk(fx_from_int(3), fx_from_int(4)));
    double l2 = sqrt(fxd(n2.x)*fxd(n2.x) + fxd(n2.y)*fxd(n2.y));
    printf("  fxv2_normalize(3,4) -> (%.4f,%.4f) len=%.5f\n", fxd(n2.x), fxd(n2.y), l2);
    CHECK(fabs(l2 - 1.0) < 1e-2, "fxv2_normalize length -> 1");
    CHECK(fabs(fxd(n2.x) - 0.6) < 1e-2 && fabs(fxd(n2.y) - 0.8) < 1e-2, "fxv2_normalize(3,4)=(0.6,0.8)");

    /* zero vector -> zero (no divide by zero) */
    fxv3 z = fxv3_normalize(fxv3_mk(0,0,0));
    CHECK(z.x == 0 && z.y == 0 && z.z == 0, "normalize(0)=0");
}

/* double-precision column-major 4x4 reference helpers */
static void dmul(const double *a, const double *b, double *r) {
    for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) {
        double s = 0; for (int k = 0; k < 4; k++) s += a[k*4+row]*b[c*4+k];
        r[c*4+row] = s;
    }
}
static void dpoint(const double *m, const double *p, double *o) {
    for (int row = 0; row < 4; row++)
        o[row] = m[0*4+row]*p[0] + m[1*4+row]*p[1] + m[2*4+row]*p[2] + m[3*4+row]*p[3];
}
static void m2d(fxm4 m, double *d) { for (int i = 0; i < 16; i++) d[i] = fxd(m.m[i]); }

/* ------------------------------------------------------------------ *
 * 5. Matrices: mul / point vs double; inverse_affine; persp/lookat    *
 * ------------------------------------------------------------------ */
static void test_matrices(void)
{
    printf("[matrices: mul/point/inverse_affine/perspective/lookat]\n");

    fxm4 T = fxm4_translate(fx_from_int(3), fx_from_int(-2), fx_from_int(5));
    fxm4 R = fxm4_rotate_y(fpm_deg(37));
    fxm4 S = fxm4_scale(fx_from_int(2), fx_ratio(1,2), fx_from_int(3));
    fxm4 M = fxm4_mul(T, fxm4_mul(R, S));   /* translate * rotate * scale */

    /* --- mul vs double reference --- */
    double dT[16], dR[16], dS[16], dRS[16], dM[16];
    m2d(T,dT); m2d(R,dR); m2d(S,dS);
    dmul(dR, dS, dRS); dmul(dT, dRS, dM);
    double mm = 0; int mfail = 0;
    double MM[16]; m2d(M, MM);
    for (int i = 0; i < 16; i++) {
        double e = fabs(MM[i] - dM[i]);
        double rel = fabs(dM[i]) > 1.0 ? e/fabs(dM[i]) : e;
        if (rel > mm) mm = rel;
        if (rel > 1e-2) mfail++;
    }
    printf("  fxm4_mul: worst rel err vs double = %.2e\n", mm);
    CHECK(mfail == 0, "fxm4_mul matches double matrix product within 1e-2");

    /* --- mul_point vs double --- */
    double pm = 0; int pfail = 0;
    fx pts[][3] = { {fx_from_int(1),0,0}, {0,fx_from_int(1),0}, {0,0,fx_from_int(1)},
                    {fx_from_int(2),fx_from_int(-3),fx_from_int(4)} };
    for (int i = 0; i < 4; i++) {
        fxv3 p = fxv3_mk(pts[i][0], pts[i][1], pts[i][2]);
        fxv3 got = fxm4_mul_point(M, p);
        double dp[4] = { fxd(p.x), fxd(p.y), fxd(p.z), 1.0 }, o[4];
        dpoint(dM, dp, o);
        double e = fabs(fxd(got.x)-o[0]) + fabs(fxd(got.y)-o[1]) + fabs(fxd(got.z)-o[2]);
        double sc = fabs(o[0])+fabs(o[1])+fabs(o[2]) + 1.0;
        if (e/sc > pm) pm = e/sc;
        if (e/sc > 1e-2) pfail++;
    }
    printf("  fxm4_mul_point: worst rel err vs double = %.2e\n", pm);
    CHECK(pfail == 0, "fxm4_mul_point matches double within 1e-2");

    /* --- inverse_affine: M * Minv ~= I --- */
    fxm4 Minv = fxm4_inverse_affine(M);
    fxm4 I = fxm4_mul(M, Minv);
    double im = 0; int ifail = 0;
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        double want = (c == r) ? 1.0 : 0.0;
        double e = fabs(fxd(I.m[c*4+r]) - want);
        if (e > im) im = e;
        if (e > 1e-2) ifail++;
    }
    printf("  fxm4_inverse_affine: worst |M*Minv - I| = %.2e\n", im);
    CHECK(ifail == 0, "fxm4_inverse_affine: M*Minv ~= I within 1e-2");

    /* --- perspective vs double reference --- */
    {
        const int W = 640, H = 480;
        fx aspect = fx_ratio(W, H);
        fxm4 P = fxm4_perspective(fpm_deg(90), aspect, fx_ratio(1,10), fx_from_int(100));
        double f = 1.0 / tan((90.0 * M_PI/180.0)/2.0);
        double a = (double)W/H, zn = 0.1, zf = 100.0;
        double pe = 0; int pf = 0;
        double dP[16] = {0};
        dP[0] = f/a; dP[5] = f; dP[10] = (zf+zn)/(zn-zf); dP[11] = -1.0;
        dP[14] = 2.0*zf*zn/(zn-zf);
        double GP[16]; m2d(P, GP);
        for (int i = 0; i < 16; i++) {
            double e = fabs(GP[i] - dP[i]);
            double rel = fabs(dP[i]) > 1.0 ? e/fabs(dP[i]) : e;
            if (rel > pe) pe = rel;
            if (rel > 1e-2) pf++;
        }
        printf("  fxm4_perspective: worst rel err vs double = %.2e\n", pe);
        CHECK(pf == 0, "fxm4_perspective matches double within 1e-2");

        /* origin projects to screen center via P * view */
        fxm4 V = fxm4_translate(0, 0, fx_from_int(-5));
        fxm4 PV = fxm4_mul(P, V);
        fxv3 o = fxv3_mk(0,0,0);
        fx X = fx_mul(PV.m[0],o.x)+fx_mul(PV.m[4],o.y)+fx_mul(PV.m[8],o.z)+PV.m[12];
        fx Wc= fx_mul(PV.m[3],o.x)+fx_mul(PV.m[7],o.y)+fx_mul(PV.m[11],o.z)+PV.m[15];
        double ndcx = fxd(X)/fxd(Wc);
        double sxd = (ndcx + 1.0)*0.5*W;
        printf("  origin -> screen X %.2f (want 320)\n", sxd);
        CHECK(fabs(sxd - 320.0) < 1.0, "perspective: origin maps to screen center X");
    }

    /* --- lookat vs double reference --- */
    {
        fxv3 eye = fxv3_mk(fx_from_int(0), fx_from_int(0), fx_from_int(5));
        fxv3 tgt = fxv3_mk(0,0,0);
        fxv3 up  = fxv3_mk(0, fx_from_int(1), 0);
        fxm4 L = fxm4_lookat(eye, tgt, up);
        /* forward = normalize(tgt-eye) = (0,0,-1); right=(1,0,0); up=(0,1,0).
         * Row layout (column-major): s,u,-f rows; translation = -R*eye. */
        double dL[16] = {0};
        dL[0]=1; dL[4]=0; dL[8]=0;       /* s = (1,0,0) */
        dL[1]=0; dL[5]=1; dL[9]=0;       /* u = (0,1,0) */
        dL[2]=0; dL[6]=0; dL[10]=1;      /* -f = (0,0,1) */
        dL[12]=0; dL[13]=0; dL[14]=-5;   /* -dot(.,eye) */
        dL[15]=1;
        double GL[16]; m2d(L, GL);
        double le = 0; int lf = 0;
        for (int i = 0; i < 16; i++) { double e = fabs(GL[i]-dL[i]); if (e>le) le=e; if (e>1e-2) lf++; }
        printf("  fxm4_lookat: worst abs err vs double = %.2e\n", le);
        CHECK(lf == 0, "fxm4_lookat matches double reference within 1e-2");
    }
}

/* ------------------------------------------------------------------ *
 * 6. Coverage + regression KATs (from the 10-agent audit):            *
 *    - every previously-untested public symbol gets a direct assert   *
 *    - each fixed bug gets a pin (these FAIL against the pre-audit    *
 *      code: fx_abs UB, fx_from_int wrap, round/ceil divergence,      *
 *      dot/matrix int32 accumulation wrap, fxm3_inverse saturation)   *
 * ------------------------------------------------------------------ */
static void test_coverage(void)
{
    printf("[coverage+regression]\n");

    /* --- regression pins for the audit fixes --- */
    CHECK(fx_abs(FX_MIN) == FX_MAX, "REGRESSION: fx_abs(FX_MIN) saturates (was UB negative)");
    CHECK(fx_from_int(32768) == FX_MAX, "REGRESSION: fx_from_int(32768) saturates (was wrap to FX_MIN)");
    CHECK(fx_from_int(-40000) == FX_MIN, "REGRESSION: fx_from_int(-40000) saturates (was sign flip)");
    CHECK(fx_from_int(-32768) == FX_MIN, "fx_from_int(-32768) exact FX_MIN");
    CHECK(fx_round((fx)0x7FFF8000) == FX_MAX, "REGRESSION: round(32767.5) == ceil(32767.5) == FX_MAX");
    {   /* dot wrap: each term fits int32, the sum does not (2*181^2 > 32767) */
        fxv2 v = fxv2_mk(fx_from_int(181), fx_from_int(181));
        CHECK(fxv2_dot(v, v) == FX_MAX, "REGRESSION: fxv2_dot saturates (was negative len^2)");
        fxv3 w = fxv3_mk(fx_from_int(150), fx_from_int(150), fx_from_int(150));
        CHECK(fxv3_dot(w, w) == FX_MAX, "REGRESSION: fxv3_dot saturates (3*150^2 > max)");
        fxv3 big = fxv3_mk(FX_MAX, FX_MAX, FX_MAX);
        fxv3 sum = fxv3_add(big, big);
        CHECK(sum.x == FX_MAX && sum.y == FX_MAX && sum.z == FX_MAX,
              "REGRESSION: fxv3_add saturates (was int32 wrap)");
    }
    {   /* mul_point row wrap: scale-181 point + big translate */
        fxm4 M = fxm4_mul(fxm4_translate(fx_from_int(30000), 0, 0),
                          fxm4_scale(fx_from_int(181), fx_from_int(181), fx_from_int(181)));
        fxv3 q = fxm4_mul_point(M, fxv3_mk(fx_from_int(181), 0, 0));
        CHECK(q.x == FX_MAX, "REGRESSION: fxm4_mul_point row sum saturates (was wrap)");
    }
    {   /* fxm3_inverse beyond the old int32-minor ceiling (scale 200) */
        fxm3 S; for (int i = 0; i < 9; i++) S.m[i] = 0;
        S.m[0] = S.m[4] = S.m[8] = fx_from_int(200);
        fxm3 Si = fxm3_inverse(S);
        double e = fabs(fxd(Si.m[0]) - 0.005);
        printf("  inverse(diag 200).m[0] = %.6f (want 0.005)\n", fxd(Si.m[0]));
        CHECK(e < 1e-4, "REGRESSION: fxm3_inverse handles scale 200 (was saturated det)");
    }

    /* --- scalars with no prior direct coverage --- */
    CHECK(fx_to_int(fx_ratio(5,2)) == 2,   "fx_to_int(2.5) = 2 (floor)");
    CHECK(fx_to_int(fx_ratio(-5,2)) == -3, "fx_to_int(-2.5) = -3 (floor, not truncate)");
    CHECK(fx_min(fx_from_int(3), fx_from_int(-7)) == fx_from_int(-7), "fx_min");
    CHECK(fx_max(fx_from_int(3), fx_from_int(-7)) == fx_from_int(3),  "fx_max");
    CHECK(fx_ratio(40000, 1) == FX_MAX, "fx_ratio saturation (40000/1 > max)");
    CHECK(fx_ceil(fx_from_int(2)) == fx_from_int(2),  "fx_ceil exact-integer path");
    CHECK(fx_floor(fx_from_int(2)) == fx_from_int(2), "fx_floor exact integer");
    CHECK(fx_round(fx_from_int(2)) == fx_from_int(2), "fx_round exact integer");
    CHECK(fpm_deg(360) == 1024 && fpm_deg(90) == 256, "fpm_deg(360)=1024, (90)=256");
    CHECK(fx_lerp(FX_MIN, FX_MAX, FX_ONE) == FX_MAX, "fx_lerp endpoint t=1 across full range");

    /* --- lengths --- */
    CHECK(fx_len2(fx_from_int(3), fx_from_int(4)) == fx_from_int(5), "fx_len2(3,4)=5");
    CHECK(fx_len3(fx_from_int(2), fx_from_int(3), fx_from_int(6)) == fx_from_int(7), "fx_len3(2,3,6)=7");
    CHECK(fxv2_len(fxv2_mk(fx_from_int(3), fx_from_int(4))) == fx_from_int(5), "fxv2_len(3,4)=5");
    CHECK(fxv3_len(fxv3_mk(fx_from_int(2), fx_from_int(3), fx_from_int(6))) == fx_from_int(7), "fxv3_len=7");
    CHECK(fx_len3(FX_MAX, FX_MAX, FX_MAX) == FX_MAX, "fx_len3 saturates at max components");

    /* --- 2D/3D vector ops --- */
    {
        fxv2 a2 = fxv2_mk(fx_from_int(1), fx_from_int(2));
        fxv2 b2 = fxv2_mk(fx_from_int(3), fx_from_int(-1));
        fxv2 s2 = fxv2_add(a2, b2);
        fxv2 d2 = fxv2_sub(a2, b2);
        fxv2 c2 = fxv2_scale(a2, FX_HALF);
        CHECK(s2.x == fx_from_int(4) && s2.y == fx_from_int(1), "fxv2_add");
        CHECK(d2.x == fx_from_int(-2) && d2.y == fx_from_int(3), "fxv2_sub");
        CHECK(c2.x == FX_HALF && c2.y == FX_ONE, "fxv2_scale by 0.5");
        CHECK(fxv2_dot(a2, b2) == fx_from_int(1), "fxv2_dot((1,2),(3,-1))=1");

        fxv3 a3 = fxv3_mk(fx_from_int(1), fx_from_int(2), fx_from_int(3));
        fxv3 b3 = fxv3_mk(fx_from_int(-2), fx_from_int(1), fx_from_int(4));
        fxv3 s3 = fxv3_add(a3, b3);
        fxv3 c3 = fxv3_scale(a3, fx_from_int(2));
        CHECK(s3.x == -FX_ONE && s3.y == fx_from_int(3) && s3.z == fx_from_int(7), "fxv3_add");
        CHECK(c3.x == fx_from_int(2) && c3.y == fx_from_int(4) && c3.z == fx_from_int(6), "fxv3_scale");

        /* basis identities, asserted directly (not via lookat) */
        fxv3 X = fxv3_mk(FX_ONE, 0, 0), Y = fxv3_mk(0, FX_ONE, 0);
        fxv3 Z = fxv3_cross(X, Y);
        CHECK(Z.x == 0 && Z.y == 0 && Z.z == FX_ONE, "cross(x,y) = z");
        CHECK(fxv3_dot(X, Y) == 0, "dot(x,y) = 0");
    }

    /* --- fxm3 family, direct --- */
    {
        /* mul: diag(2,3,4) * diag(0.5, 2, 0.25) = diag(1, 6, 1) */
        fxm3 D1; for (int i = 0; i < 9; i++) D1.m[i] = 0;
        D1.m[0] = fx_from_int(2); D1.m[4] = fx_from_int(3); D1.m[8] = fx_from_int(4);
        fxm3 D2; for (int i = 0; i < 9; i++) D2.m[i] = 0;
        D2.m[0] = FX_HALF; D2.m[4] = fx_from_int(2); D2.m[8] = fx_ratio(1,4);
        fxm3 P = fxm3_mul(D1, D2);
        CHECK(P.m[0] == FX_ONE && P.m[4] == fx_from_int(6) && P.m[8] == FX_ONE
              && P.m[1] == 0 && P.m[3] == 0, "fxm3_mul diag product");

        /* transpose: build asymmetric, check slots + double-transpose identity */
        fxm3 A; for (int i = 0; i < 9; i++) A.m[i] = fx_from_int(i + 1);
        fxm3 At = fxm3_transpose(A);
        CHECK(At.m[1] == A.m[3] && At.m[3] == A.m[1] && At.m[2] == A.m[6], "fxm3_transpose swaps");
        fxm3 Att = fxm3_transpose(At);
        int same = 1; for (int i = 0; i < 9; i++) if (Att.m[i] != A.m[i]) same = 0;
        CHECK(same, "fxm3_transpose is an involution");

        /* mul_vec: diag(2,3,4) * (1,1,1) = (2,3,4) */
        fxv3 mv = fxm3_mul_vec(D1, fxv3_mk(FX_ONE, FX_ONE, FX_ONE));
        CHECK(mv.x == fx_from_int(2) && mv.y == fx_from_int(3) && mv.z == fx_from_int(4), "fxm3_mul_vec");

        /* inverse, direct: inv(diag(2,4,5)) = diag(0.5, 0.25, 0.2) */
        fxm3 D3; for (int i = 0; i < 9; i++) D3.m[i] = 0;
        D3.m[0] = fx_from_int(2); D3.m[4] = fx_from_int(4); D3.m[8] = fx_from_int(5);
        fxm3 D3i = fxm3_inverse(D3);
        CHECK(fabs(fxd(D3i.m[0]) - 0.5)  < 1e-4 &&
              fabs(fxd(D3i.m[4]) - 0.25) < 1e-4 &&
              fabs(fxd(D3i.m[8]) - 0.2)  < 1e-4, "fxm3_inverse diag(2,4,5)");

        /* singular -> identity (fxm3_identity's only reachable path) */
        fxm3 Zm; for (int i = 0; i < 9; i++) Zm.m[i] = 0;
        fxm3 Zi = fxm3_inverse(Zm);
        CHECK(Zi.m[0] == FX_ONE && Zi.m[4] == FX_ONE && Zi.m[8] == FX_ONE && Zi.m[1] == 0,
              "fxm3_inverse(singular) = identity");
    }

    /* --- fxm4: mul_dir / transpose / rotate_x / rotate_z --- */
    {
        fxm4 T = fxm4_translate(fx_from_int(5), fx_from_int(6), fx_from_int(7));
        fxv3 d = fxm4_mul_dir(T, fxv3_mk(FX_ONE, fx_from_int(2), fx_from_int(3)));
        CHECK(d.x == FX_ONE && d.y == fx_from_int(2) && d.z == fx_from_int(3),
              "fxm4_mul_dir ignores translation");

        fxm4 Tt = fxm4_transpose(T);
        CHECK(Tt.m[3] == fx_from_int(5) && Tt.m[7] == fx_from_int(6) && Tt.m[11] == fx_from_int(7),
              "fxm4_transpose moves translation to bottom row");
        fxm4 Ttt = fxm4_transpose(Tt);
        int same4 = 1; for (int i = 0; i < 16; i++) if (Ttt.m[i] != T.m[i]) same4 = 0;
        CHECK(same4, "fxm4_transpose is an involution");

        /* rotate_x / rotate_z vs libm at the brad-quantized angle 105 */
        int brad = fpm_deg(37);                      /* 105 brads */
        double ang = 2.0 * M_PI * brad / 1024.0;     /* the QUANTIZED angle */
        fxv3 ry = fxm4_mul_dir(fxm4_rotate_x(brad), fxv3_mk(0, FX_ONE, 0));
        CHECK(fabs(fxd(ry.y) - cos(ang)) < 1e-3 && fabs(fxd(ry.z) - sin(ang)) < 1e-3,
              "fxm4_rotate_x: (0,1,0) -> (0,cos,sin)");
        fxv3 rx = fxm4_mul_dir(fxm4_rotate_z(brad), fxv3_mk(FX_ONE, 0, 0));
        CHECK(fabs(fxd(rx.x) - cos(ang)) < 1e-3 && fabs(fxd(rx.y) - sin(ang)) < 1e-3,
              "fxm4_rotate_z: (1,0,0) -> (cos,sin,0)");
    }

    /* --- fxm4_mul vs an INDEPENDENT double reference (audit: the original
     * test derived its reference from the fx matrices themselves, which
     * cannot catch builder quantization) --- */
    {
        int brad = fpm_deg(37);
        double ang = 2.0 * M_PI * brad / 1024.0;
        fxm4 M = fxm4_mul(fxm4_translate(fx_from_int(3), fx_from_int(-2), fx_from_int(5)),
                          fxm4_mul(fxm4_rotate_y(brad),
                                   fxm4_scale(fx_from_int(2), fx_ratio(1,2), fx_from_int(3))));
        /* independent column-major reference: T * Ry(ang) * S */
        double dM[16] = {0};
        double cc = cos(ang), ss = sin(ang);
        dM[0] = 2*cc;  dM[2] = 2*-ss; dM[5] = 0.5;
        dM[8] = 3*ss;  dM[10] = 3*cc; dM[15] = 1.0;
        dM[12] = 3; dM[13] = -2; dM[14] = 5;
        double GM[16]; m2d(M, GM);
        double we = 0; int wf = 0;
        for (int i = 0; i < 16; i++) {
            double e = fabs(GM[i] - dM[i]);
            if (e > we) we = e;
            if (e > 1e-3) wf++;
        }
        printf("  fxm4 T*R*S vs independent double ref: worst abs err = %.2e\n", we);
        CHECK(wf == 0, "fxm4_mul chain matches independent double reference (1e-3)");
    }

    /* --- inverse_affine scale sweep (audit: single well-conditioned case) --- */
    {
        double scales[] = { 0.01, 0.5, 2.0, 50.0, 200.0 };
        double wworst = 0; int wfail = 0;
        for (int si = 0; si < 5; si++) {
            fx s = (fx)llround(scales[si] * 65536.0);
            fxm4 M = fxm4_mul(fxm4_translate(fx_from_int(7), fx_from_int(-3), fx_from_int(11)),
                              fxm4_mul(fxm4_rotate_y(105), fxm4_scale(s, s, s)));
            fxm4 I = fxm4_mul(M, fxm4_inverse_affine(M));
            for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
                double want = (c == r) ? 1.0 : 0.0;
                double e = fabs(fxd(I.m[c*4+r]) - want);
                if (e > wworst) wworst = e;
                if (e > 1e-2) wfail++;
            }
        }
        printf("  inverse_affine sweep (scale 0.01..200, rot+T): worst |M*Minv-I| = %.5f\n", wworst);
        CHECK(wfail == 0, "fxm4_inverse_affine round-trips across scales 0.01..200");
    }
}

int main(void)
{
    BRAD_PER_RAD = 1024.0 / (2.0 * M_PI);
    printf("=== fpm host KAT battery ===\n");
    test_scalars();
    test_roots();
    test_trig();
    test_vectors();
    test_matrices();
    test_coverage();
    if (g_fail) printf("\nFPM HOSTTEST: FAIL n=%d\n", g_fail);
    else        printf("\nFPM HOSTTEST: PASS\n");
    return g_fail ? 1 : 0;
}
