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
        if (rel > 1e-3) sfail++;
    }
    printf("  fx_sqrt: worst rel err = %.2e\n", smax);
    CHECK(sfail == 0, "fx_sqrt spot values rel err <= 1e-3");

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
        if (es > 0.002 || ec > 0.002) tfail++;
    }
    printf("  fx_sin/cos: worst abs err over all 1024 brads = %.5f\n", tmax);
    CHECK(tfail == 0, "fx_sin/fx_cos abs err <= 0.002 for every brad");

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
        if (e > 1.0) afail++;
        apairs++;
    }
    printf("  fx_atan2: worst err = %.4f brad over %ld pairs\n", amax, apairs);
    CHECK(afail == 0, "fx_atan2 err <= 1 brad (all quadrants + axes)");

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
        if (eas > 2.0) asfail++;
        if (eac > 2.0) acfail++;
    }
    printf("  fx_asin: worst err = %.4f brad; fx_acos: worst = %.4f brad\n", asmax, acmax);
    CHECK(asfail == 0, "fx_asin err <= 2 brads over [-1,1]");
    CHECK(acfail == 0, "fx_acos err <= 2 brads over [-1,1]");
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

int main(void)
{
    BRAD_PER_RAD = 1024.0 / (2.0 * M_PI);
    printf("=== fpm host KAT battery ===\n");
    test_scalars();
    test_roots();
    test_trig();
    test_vectors();
    test_matrices();
    if (g_fail) printf("\nFPM HOSTTEST: FAIL n=%d\n", g_fail);
    else        printf("\nFPM HOSTTEST: PASS\n");
    return g_fail ? 1 : 0;
}
