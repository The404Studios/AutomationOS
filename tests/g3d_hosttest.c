/*
 * g3d_hosttest.c -- Host-side sanity test for the g3d fixed-point math
 *                   and rasterizer. Compile + run with the SYSTEM gcc
 *                   (NOT freestanding); this validates the pure logic.
 * ====================================================================
 *
 * Build & run:
 *   gcc -std=gnu11 -O2 -DG3D_HOSTTEST -I userspace/lib/g3d \
 *       tests/g3d_hosttest.c -o /tmp/g3d_hosttest
 *   /tmp/g3d_hosttest
 *
 * We pull in the g3d implementation directly (single TU) with G3D_HOSTTEST
 * defined so the freestanding-only #pragma GCC optimize is skipped. stdio is
 * used ONLY by the test harness, never by g3d itself.
 *
 * The test asserts:
 *   1. fixed-point sqrt / sin / cos are accurate vs the libm reference.
 *   2. a known point transformed by a perspective*rotation MVP lands on the
 *      expected screen pixel (centered, in front of the camera).
 *   3. a known triangle rasterizes so its centroid pixel is filled with the
 *      shaded color and the depth buffer is written there (and NOT outside).
 *
 * Prints "G3D HOSTTEST: PASS" / "FAIL" and exits 0 / 1 accordingly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* include the library implementation as one translation unit */
#include "g3d.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
        if (!(cond)) { printf("  FAIL: %s  (%s)\n", msg, #cond); g_fail = 1; } \
        else         { printf("  ok:   %s\n", msg); } \
    } while (0)

/* Convert a Q16.16 back to double for comparison/printing. */
static double fxd(fx a) { return (double)a / 65536.0; }

/* ------------------------------------------------------------------ */
/* 1. Fixed-point scalar accuracy                                      */
/* ------------------------------------------------------------------ */
static void test_scalars(void)
{
    printf("[scalars]\n");

    /* sqrt(2) ~= 1.41421356 */
    double s2 = fxd(fx_sqrt(fx_from_int(2)));
    CHECK(fabs(s2 - 1.4142135) < 0.001, "fx_sqrt(2) ~= 1.41421");

    /* sqrt(16) == 4 exactly */
    double s16 = fxd(fx_sqrt(fx_from_int(16)));
    CHECK(fabs(s16 - 4.0) < 0.001, "fx_sqrt(16) ~= 4.0");

    /* sin/cos at a handful of angles vs libm */
    int worst_ok = 1;
    double maxerr = 0.0;
    for (int deg = 0; deg < 360; deg += 15) {
        int brad = g3d_deg(deg);
        double rs = sin(deg * M_PI / 180.0);
        double rc = cos(deg * M_PI / 180.0);
        double gs = fxd(fx_sin(brad));
        double gc = fxd(fx_cos(brad));
        double es = fabs(gs - rs), ec = fabs(gc - rc);
        if (es > maxerr) maxerr = es;
        if (ec > maxerr) maxerr = ec;
        if (es > 0.01 || ec > 0.01) worst_ok = 0;
    }
    printf("  (max sin/cos error over 0..345 deg = %.5f)\n", maxerr);
    CHECK(worst_ok, "fx_sin/fx_cos within 0.01 of libm");

    /* mul / div round-trip */
    fx a = fx_ratio(7, 3);             /* 2.333... */
    fx b = fx_mul(a, fx_from_int(3));  /* ~7.0     */
    CHECK(fabs(fxd(b) - 7.0) < 0.001, "fx_mul: (7/3)*3 ~= 7");
}

/* ------------------------------------------------------------------ */
/* 2. Matrix transform of a known point                                */
/* ------------------------------------------------------------------ */
static void test_transform(void)
{
    printf("[transform]\n");

    const int W = 640, H = 480;
    fx aspect = fx_ratio(W, H);
    mat4 proj = mat4_perspective(g3d_deg(90), aspect, fx_ratio(1, 10), fx_from_int(100));
    /* camera 5 units back on +Z, looking at origin */
    mat4 view = mat4_translate(0, 0, fx_from_int(-5));

    /* The origin point should project to the exact screen center. */
    vec3 p0 = v3(0, 0, 0);
    /* clip = proj * view * p */
    mat4 pv = mat4_mul(proj, view);
    fx X = fx_mul(pv.m[0], p0.x) + fx_mul(pv.m[4], p0.y) + fx_mul(pv.m[8],  p0.z) + pv.m[12];
    fx Y = fx_mul(pv.m[1], p0.x) + fx_mul(pv.m[5], p0.y) + fx_mul(pv.m[9],  p0.z) + pv.m[13];
    fx Wc= fx_mul(pv.m[3], p0.x) + fx_mul(pv.m[7], p0.y) + fx_mul(pv.m[11], p0.z) + pv.m[15];

    double ndcx = fxd(X) / fxd(Wc);
    double ndcy = fxd(Y) / fxd(Wc);
    double sxd = (ndcx + 1.0) * 0.5 * W;
    double syd = (1.0 - ndcy) * 0.5 * H;
    printf("  origin -> ndc(%.4f, %.4f) -> screen(%.1f, %.1f)\n", ndcx, ndcy, sxd, syd);
    CHECK(fabs(sxd - W / 2.0) < 1.0, "origin maps to screen center X (320)");
    CHECK(fabs(syd - H / 2.0) < 1.0, "origin maps to screen center Y (240)");
    CHECK(Wc > 0, "origin is in front of the camera (w > 0)");

    /* A point offset +1 in X (world) must land to the RIGHT of center. */
    vec3 pr = v3(FX_ONE, 0, 0);
    fx Xr = fx_mul(pv.m[0], pr.x) + fx_mul(pv.m[4], pr.y) + fx_mul(pv.m[8],  pr.z) + pv.m[12];
    fx Wr = fx_mul(pv.m[3], pr.x) + fx_mul(pv.m[7], pr.y) + fx_mul(pv.m[11], pr.z) + pv.m[15];
    double sxr = (fxd(Xr) / fxd(Wr) + 1.0) * 0.5 * W;
    printf("  (+1,0,0) -> screen X %.1f\n", sxr);
    CHECK(sxr > W / 2.0 + 5.0, "+X world point projects right of center");

    /* A 90-degree Y rotation of (1,0,0) should give ~(0,0,-1). */
    mat4 ry = mat4_rotate_y(g3d_deg(90));
    vec3 rp = mat4_mul_point(ry, v3(FX_ONE, 0, 0));
    printf("  rotY90 * (1,0,0) = (%.3f, %.3f, %.3f)\n", fxd(rp.x), fxd(rp.y), fxd(rp.z));
    CHECK(fabs(fxd(rp.x) - 0.0) < 0.01, "rotY90 x ~= 0");
    CHECK(fabs(fxd(rp.z) + 1.0) < 0.01, "rotY90 z ~= -1");
}

/* ------------------------------------------------------------------ */
/* 3. Triangle rasterization: centroid filled, outside untouched       */
/* ------------------------------------------------------------------ */
static void test_raster(void)
{
    printf("[raster]\n");

    const int W = 64, H = 64;
    static g3d_u32 pix[64 * 64];
    static g3d_i32 zbf[64 * 64];
    g3d_target t = { pix, zbf, W, H, W };

    g3d_clear(&t, 0xFF000000);
    g3d_clear_depth(&t);

    /* A big triangle: (10,10) (54,10) (32,54). Centroid ~ (32, 24.7). */
    fx ax = fx_from_int(10), ay = fx_from_int(10);
    fx bx = fx_from_int(54), by = fx_from_int(10);
    fx cx = fx_from_int(32), cy = fx_from_int(54);
    g3d_u32 col = 0xFF20C040;
    g3d_i32 depth = 1000;
    g3d_raster_tri(&t, ax, ay, depth, bx, by, depth, cx, cy, depth, col);

    int cxp = 32, cyp = 25;
    g3d_u32 got = pix[cyp * W + cxp];
    printf("  centroid pixel (%d,%d) = 0x%08X (want 0x%08X)\n", cxp, cyp, got, col);
    CHECK(got == col, "triangle centroid pixel is filled with the color");
    CHECK(zbf[cyp * W + cxp] == depth, "z-buffer written at centroid");

    /* A corner well OUTSIDE the triangle must remain background. */
    g3d_u32 corner = pix[2 * W + 2];
    CHECK(corner == 0xFF000000, "pixel outside triangle untouched");
    CHECK(zbf[2 * W + 2] == G3D_ZMAX, "z-buffer untouched outside triangle");

    /* Depth test: draw a NEARER triangle (smaller z) over the same centroid
     * -> it should win; then a FARTHER one -> it should NOT overwrite. */
    g3d_raster_tri(&t, ax, ay, 500, bx, by, 500, cx, cy, 500, 0xFFFF0000);
    CHECK(pix[cyp * W + cxp] == 0xFFFF0000, "nearer triangle overwrites (z-test)");
    g3d_raster_tri(&t, ax, ay, 900, bx, by, 900, cx, cy, 900, 0xFF0000FF);
    CHECK(pix[cyp * W + cxp] == 0xFFFF0000, "farther triangle is rejected (z-test)");
}

/* ------------------------------------------------------------------ */
/* 4. Full mesh path: a single front-facing triangle gets drawn shaded */
/*    and a back-facing one gets culled.                                */
/* ------------------------------------------------------------------ */
static void test_mesh_cull(void)
{
    printf("[mesh+cull]\n");

    const int W = 128, H = 128;
    static g3d_u32 pix[128 * 128];
    static g3d_i32 zbf[128 * 128];
    g3d_target t = { pix, zbf, W, H, W };

    fx aspect = fx_ratio(W, H);
    mat4 proj = mat4_perspective(g3d_deg(60), aspect, fx_ratio(1, 10), fx_from_int(100));
    mat4 view = mat4_translate(0, 0, fx_from_int(-3));
    mat4 mvp  = mat4_mul(proj, view);
    vec3 light = v3(0, 0, FX_ONE);   /* light toward +Z (camera) */

    /* Front-facing triangle (CCW when viewed from +Z / the camera). */
    g3d_tri front;
    front.v[0] = v3(fx_from_int(-1), fx_from_int(-1), 0);
    front.v[1] = v3(fx_from_int( 1), fx_from_int(-1), 0);
    front.v[2] = v3(0,               fx_from_int( 1), 0);
    front.color = 0xFFFFFFFF;

    g3d_clear(&t, 0xFF000000);
    g3d_clear_depth(&t);
    g3d_draw_mesh(&t, &front, 1, mat4_identity(), mvp, light);

    /* center pixel should now be non-black (the lit triangle). */
    g3d_u32 cpix = pix[(H/2) * W + (W/2)];
    printf("  front tri center pixel = 0x%08X\n", cpix);
    CHECK(cpix != 0xFF000000, "front-facing triangle is rasterized");

    /* Back-facing triangle: reverse winding -> should be culled (all black). */
    g3d_tri back = front;
    back.v[1] = front.v[2];
    back.v[2] = front.v[1];

    g3d_clear(&t, 0xFF000000);
    g3d_clear_depth(&t);
    g3d_draw_mesh(&t, &back, 1, mat4_identity(), mvp, light);

    int any = 0;
    for (int i = 0; i < W * H; i++) if (pix[i] != 0xFF000000) { any = 1; break; }
    CHECK(any == 0, "back-facing triangle is culled (nothing drawn)");
}

int main(void)
{
    printf("=== g3d host test ===\n");
    test_scalars();
    test_transform();
    test_raster();
    test_mesh_cull();
    printf("\nG3D HOSTTEST: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
