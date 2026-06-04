/*
 * g3d.c -- Self-contained software 3D renderer implementation.
 * ============================================================
 *
 * See g3d.h for the API and the design rationale. Everything here is integer
 * Q16.16 fixed-point: no libc, no libm, no floating point. The same source
 * compiles under host gcc (for tests/g3d_hosttest.c) and under the freestanding
 * ring-3 toolchain (for cube3d / ray).
 */

#include "g3d.h"

/*
 * Neutralise the two gcc-15 freestanding quirks exactly like the games do
 * (see asteroids.c): the stack-protector self-spec re-injection emits a
 * %fs:0x28 canary referencing __stack_chk_fail (absent in our link), and the
 * loop-distribution pass can rewrite hand loops into memset/strlen calls.
 * Guarded by __freestanding marker: harmless to also apply on host gcc, but we
 * only enable it when NOT building the host test, so the host build stays plain.
 */
#ifndef G3D_HOSTTEST
#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")
#endif

/* ====================================================================== *
 *  Fixed-point sqrt (Newton-free integer digit-by-digit).
 * ---------------------------------------------------------------------- *
 *  sqrt of a Q16.16 value x = sqrt(real) in Q16.16. We want
 *  out = sqrt(x_real) * 2^16. Since x = x_real * 2^16,
 *  out = sqrt(x * 2^16) = sqrt(x << 16). Compute integer isqrt of (x<<16)
 *  using the classic bit-by-bit method on a 64-bit operand.
 * ====================================================================== */
fx fx_sqrt(fx a)
{
    if (a <= 0) return 0;
    g3d_u64 n = (g3d_u64)(g3d_u32)a << FX_SHIFT;   /* a * 2^16, unsigned */
    g3d_u64 res = 0;
    /* highest power-of-four <= 2^63 */
    g3d_u64 bit = (g3d_u64)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n   -= res + bit;
            res  = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (fx)res;
}

/* ====================================================================== *
 *  Trig table (1024 steps), built once with an integer polynomial.
 * ---------------------------------------------------------------------- *
 *  We approximate sine on a quarter wave with a 5th-order minimax-ish
 *  polynomial evaluated entirely in Q16.16, then mirror it into the full
 *  table. This avoids ANY floating point while staying accurate to < 0.001.
 *
 *  Using the normalized form: for t in [0,1] mapping to [0, pi/2],
 *    sin(t*pi/2) ~= t*(a1 + t^2*(a3 + t^2*a5))  (odd polynomial)
 *  We instead use the well-known Bhaskara-grade quarter approximation refined
 *  by sampling: to keep it exact and simple, we build the table via the
 *  half-angle recurrence from a single seed (cos/sin of one step) computed by
 *  the polynomial. This is deterministic and FP-free.
 * ====================================================================== */
static fx  g_sin_tab[G3D_ANG_STEPS];
static int g_sin_ready = 0;

/* Evaluate sin(theta) for theta in Q16.16 RADIANS, range roughly [-pi,pi],
 * via a 7th-order Taylor-ish odd polynomial in Q16.16. Good to ~1e-4. */
static fx poly_sin_rad(fx x)
{
    /* Reduce nothing here; callers pass small magnitudes (|x| <= pi). The
     * series converges fine up to |x| = pi.
     *
     *   sin x = x - x^3/6 + x^5/120 - x^7/5040 + x^9/362880
     *
     * We build successive odd powers by repeatedly multiplying by x^2, and
     * divide each by its factorial. Signs alternate explicitly. Carrying the
     * running power separately (NOT folding the divisor back in) keeps the
     * recurrence exact. */
    fx x2  = fx_mul(x, x);
    fx pow = x;                  /* current odd power: starts at x^1 */
    fx sum = pow;                /* + x                              */

    pow = fx_mul(pow, x2);       /* x^3  */ sum -= fx_div(pow, fx_from_int(6));
    pow = fx_mul(pow, x2);       /* x^5  */ sum += fx_div(pow, fx_from_int(120));
    pow = fx_mul(pow, x2);       /* x^7  */ sum -= fx_div(pow, fx_from_int(5040));
    pow = fx_mul(pow, x2);       /* x^9  */ sum += fx_div(pow, fx_from_int(362880));
    return sum;
}

static void sin_table_init(void)
{
    if (g_sin_ready) return;
    /* Constants in Q16.16. */
    const fx PI      = 205887;   /* round(pi      * 65536) */
    const fx HALF_PI = 102944;   /* round(pi/2    * 65536) */
    const fx TWO_PI  = 411775;   /* round(2*pi    * 65536) */
    for (int i = 0; i < G3D_ANG_STEPS; i++) {
        /* theta = i / 1024 * 2pi, in Q16.16 radians, in [0, 2pi). */
        fx theta = fx_mul(fx_ratio(i, G3D_ANG_STEPS), TWO_PI);

        /* Range-reduce to [-pi/2, pi/2] using sine symmetries so the Taylor
         * series is only ever evaluated where it is most accurate:
         *   sin(theta) for theta in (pi/2, pi]      == sin(pi - theta)
         *   sin(theta) for theta in (pi, 3pi/2]     == -sin(theta - pi)
         *   sin(theta) for theta in (3pi/2, 2pi)    == -sin(2pi - theta) */
        fx s;
        if (theta <= HALF_PI) {
            s = poly_sin_rad(theta);
        } else if (theta <= PI) {
            s = poly_sin_rad(PI - theta);
        } else if (theta <= PI + HALF_PI) {
            s = -poly_sin_rad(theta - PI);
        } else {
            s = -poly_sin_rad(TWO_PI - theta);
        }
        g_sin_tab[i] = s;
    }
    g_sin_ready = 1;
}

fx fx_sin(g3d_i32 ang)
{
    if (!g_sin_ready) sin_table_init();
    return g_sin_tab[(g3d_u32)ang & G3D_ANG_MASK];
}
fx fx_cos(g3d_i32 ang)
{
    if (!g_sin_ready) sin_table_init();
    return g_sin_tab[((g3d_u32)ang + (G3D_ANG_STEPS / 4)) & G3D_ANG_MASK];
}

/* ====================================================================== *
 *  vec3 length / normalize
 * ====================================================================== */
fx v3_len(vec3 a)
{
    /* |a|^2 in Q16.16, then sqrt. Use 64-bit to avoid overflow in the dot. */
    fx d = v3_dot(a, a);
    return fx_sqrt(d);
}
vec3 v3_normalize(vec3 a)
{
    fx len = v3_len(a);
    if (len == 0) return v3(0, 0, 0);
    return v3(fx_div(a.x, len), fx_div(a.y, len), fx_div(a.z, len));
}

/* ====================================================================== *
 *  mat4 (column-major: m[col*4 + row])
 * ====================================================================== */
mat4 mat4_identity(void)
{
    mat4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = FX_ONE;
    return r;
}

mat4 mat4_mul(mat4 a, mat4 b)
{
    /* r = a * b ; r[col*4+row] = sum_k a[k*4+row] * b[col*4+k] */
    mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            fx s = 0;
            for (int k = 0; k < 4; k++)
                s += fx_mul(a.m[k * 4 + row], b.m[col * 4 + k]);
            r.m[col * 4 + row] = s;
        }
    }
    return r;
}

/* Transform a point (implicit w=1), perspective-divide NOT applied here. */
vec3 mat4_mul_point(mat4 m, vec3 p)
{
    fx x = fx_mul(m.m[0], p.x) + fx_mul(m.m[4], p.y) + fx_mul(m.m[8],  p.z) + m.m[12];
    fx y = fx_mul(m.m[1], p.x) + fx_mul(m.m[5], p.y) + fx_mul(m.m[9],  p.z) + m.m[13];
    fx z = fx_mul(m.m[2], p.x) + fx_mul(m.m[6], p.y) + fx_mul(m.m[10], p.z) + m.m[14];
    return v3(x, y, z);
}

mat4 mat4_translate(fx x, fx y, fx z)
{
    mat4 r = mat4_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

mat4 mat4_scale(fx sx, fx sy, fx sz)
{
    mat4 r = mat4_identity();
    r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
    return r;
}

mat4 mat4_rotate_x(g3d_i32 ang)
{
    fx c = fx_cos(ang), s = fx_sin(ang);
    mat4 r = mat4_identity();
    r.m[5] = c;  r.m[9]  = -s;
    r.m[6] = s;  r.m[10] =  c;
    return r;
}
mat4 mat4_rotate_y(g3d_i32 ang)
{
    fx c = fx_cos(ang), s = fx_sin(ang);
    mat4 r = mat4_identity();
    r.m[0] =  c;  r.m[8]  = s;
    r.m[2] = -s;  r.m[10] = c;
    return r;
}
mat4 mat4_rotate_z(g3d_i32 ang)
{
    fx c = fx_cos(ang), s = fx_sin(ang);
    mat4 r = mat4_identity();
    r.m[0] = c;  r.m[4] = -s;
    r.m[1] = s;  r.m[5] =  c;
    return r;
}

/* Right-handed perspective, camera looks down -Z, clip z in [-w, w]. */
mat4 mat4_perspective(g3d_i32 fov, fx aspect, fx znear, fx zfar)
{
    /* f = 1 / tan(fov/2) = cos(fov/2)/sin(fov/2). */
    g3d_i32 half = fov / 2;
    fx s = fx_sin(half), c = fx_cos(half);
    fx f = fx_div(c, s);                       /* cot(fov/2) */

    mat4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0;
    r.m[0]  = fx_div(f, aspect);               /* x scale */
    r.m[5]  = f;                               /* y scale */
    fx nf   = znear - zfar;                    /* znear - zfar             */
    r.m[10] = fx_div(zfar + znear, nf);        /* (far+near)/(near-far)    */
    r.m[11] = fx_from_int(-1);                 /* -1 -> w = -z_eye         */
    r.m[14] = fx_div(fx_mul(fx_mul(fx_from_int(2), zfar), znear), nf);
    r.m[15] = 0;
    return r;
}

/* Right-handed lookAt. */
mat4 mat4_lookat(vec3 eye, vec3 target, vec3 up)
{
    vec3 f = v3_normalize(v3_sub(target, eye));    /* forward (toward target) */
    vec3 s = v3_normalize(v3_cross(f, up));        /* right                   */
    vec3 u = v3_cross(s, f);                       /* true up                 */

    mat4 r = mat4_identity();
    /* rows of the rotation are s, u, -f (column-major storage) */
    r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -v3_dot(s, eye);
    r.m[13] = -v3_dot(u, eye);
    r.m[14] =  v3_dot(f, eye);
    return r;
}

/* ====================================================================== *
 *  Target clear helpers
 * ====================================================================== */
void g3d_clear(g3d_target *t, g3d_u32 argb)
{
    for (g3d_i32 y = 0; y < t->h; y++) {
        g3d_u32 *row = t->pixels + (g3d_i32)y * t->stride;
        for (g3d_i32 x = 0; x < t->w; x++) row[x] = argb;
    }
}
void g3d_clear_depth(g3d_target *t)
{
    g3d_i32 n = t->w * t->h;
    for (g3d_i32 i = 0; i < n; i++) t->zbuf[i] = G3D_ZMAX;
}

/* ====================================================================== *
 *  Triangle rasterizer (top-left filled, edge-function / barycentric).
 * ---------------------------------------------------------------------- *
 *  Inputs are screen-space X/Y in Q16.16 and an integer depth per vertex.
 *  We bound the triangle, then for each integer pixel center compute the
 *  three edge functions; inside-test by sign, and interpolate depth with the
 *  barycentric weights. Depth-test against the z-buffer (smaller = nearer).
 * ====================================================================== */

/* Edge function in Q16.16: (c - a) x (p - a). Result downshifted to keep it
 * in range. Returns a 64-bit signed area*2 term (in Q16.16-ish units). */
static inline g3d_i64 edge(fx ax, fx ay, fx bx, fx by, fx px, fx py)
{
    /* (bx-ax)*(py-ay) - (by-ay)*(px-ax), all Q16.16; product is Q32.32, we
     * shift back to Q16.16 to keep magnitudes sane. */
    g3d_i64 e = ((g3d_i64)(bx - ax) * (g3d_i64)(py - ay)
               - (g3d_i64)(by - ay) * (g3d_i64)(px - ax));
    return e >> FX_SHIFT;
}

void g3d_raster_tri(g3d_target *t,
                    fx ax, fx ay, g3d_i32 az,
                    fx bx, fx by, g3d_i32 bz,
                    fx cx, fx cy, g3d_i32 cz,
                    g3d_u32 color)
{
    /* Bounding box in integer pixels, clamped to the surface. */
    fx minxf = ax, maxxf = ax, minyf = ay, maxyf = ay;
    if (bx < minxf) minxf = bx; if (bx > maxxf) maxxf = bx;
    if (cx < minxf) minxf = cx; if (cx > maxxf) maxxf = cx;
    if (by < minyf) minyf = by; if (by > maxyf) maxyf = by;
    if (cy < minyf) minyf = cy; if (cy > maxyf) maxyf = cy;

    g3d_i32 minx = fx_to_int(minxf);
    g3d_i32 maxx = fx_to_int(maxxf) + 1;
    g3d_i32 miny = fx_to_int(minyf);
    g3d_i32 maxy = fx_to_int(maxyf) + 1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > t->w) maxx = t->w;
    if (maxy > t->h) maxy = t->h;
    if (minx >= maxx || miny >= maxy) return;

    /* Total signed area (twice). If ~0, degenerate -> skip. */
    g3d_i64 area = edge(ax, ay, bx, by, cx, cy);
    if (area == 0) return;
    /* Ensure counter-clockwise winding test works for both signs: we accept a
     * fragment when all three edge functions share the sign of `area`. */

    for (g3d_i32 y = miny; y < maxy; y++) {
        fx py = fx_from_int(y) + FX_HALF;          /* pixel center */
        g3d_u32 *prow = t->pixels + (g3d_i32)y * t->stride;
        g3d_i32 *zrow = t->zbuf   + (g3d_i32)y * t->w;
        for (g3d_i32 x = minx; x < maxx; x++) {
            fx px = fx_from_int(x) + FX_HALF;
            g3d_i64 w0 = edge(bx, by, cx, cy, px, py);   /* opposite a */
            g3d_i64 w1 = edge(cx, cy, ax, ay, px, py);   /* opposite b */
            g3d_i64 w2 = edge(ax, ay, bx, by, px, py);   /* opposite c */

            int inside;
            if (area > 0) inside = (w0 >= 0 && w1 >= 0 && w2 >= 0);
            else          inside = (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside) continue;

            /* Barycentric weights normalized by area; interpolate depth. */
            /* depth = (w0*az + w1*bz + w2*cz) / area  -- all integer. */
            g3d_i64 znum = w0 * (g3d_i64)az + w1 * (g3d_i64)bz + w2 * (g3d_i64)cz;
            g3d_i32 z = (g3d_i32)(znum / area);

            if (z < zrow[x]) {
                zrow[x] = z;
                prow[x] = color;
            }
        }
    }
}

/* ====================================================================== *
 *  High-level: transform, cull, shade, rasterize a mesh.
 * ====================================================================== */

/* Shade a base ARGB color by an intensity in Q16.16 [0..1] (clamped). */
static g3d_u32 shade_color(g3d_u32 base, fx intensity)
{
    if (intensity < 0)      intensity = 0;
    if (intensity > FX_ONE) intensity = FX_ONE;
    g3d_i32 r = (g3d_i32)((base >> 16) & 0xFF);
    g3d_i32 g = (g3d_i32)((base >> 8)  & 0xFF);
    g3d_i32 b = (g3d_i32)( base        & 0xFF);
    r = fx_to_int(fx_mul(fx_from_int(r), intensity));
    g = fx_to_int(fx_mul(fx_from_int(g), intensity));
    b = fx_to_int(fx_mul(fx_from_int(b), intensity));
    return g3d_rgb(r, g, b);
}

void g3d_draw_mesh(g3d_target *t, const g3d_tri *tris, g3d_i32 count,
                   mat4 model, mat4 mvp, vec3 light_dir)
{
    vec3 L = v3_normalize(light_dir);
    fx halfw = fx_ratio(t->w, 2);
    fx halfh = fx_ratio(t->h, 2);

    for (g3d_i32 i = 0; i < count; i++) {
        const g3d_tri *tr = &tris[i];

        /* --- world-space normal for lighting (model matrix only) --- */
        vec3 w0 = mat4_mul_point(model, tr->v[0]);
        vec3 w1 = mat4_mul_point(model, tr->v[1]);
        vec3 w2 = mat4_mul_point(model, tr->v[2]);
        vec3 nrm = v3_normalize(v3_cross(v3_sub(w1, w0), v3_sub(w2, w0)));

        /* --- project all three vertices through MVP --- */
        /* We need the homogeneous w (= -z_eye), so compute the 4th row too. */
        fx cx[3], cy[3], cw[3];
        for (int k = 0; k < 3; k++) {
            vec3 p = tr->v[k];
            fx X = fx_mul(mvp.m[0], p.x) + fx_mul(mvp.m[4], p.y) + fx_mul(mvp.m[8],  p.z) + mvp.m[12];
            fx Y = fx_mul(mvp.m[1], p.x) + fx_mul(mvp.m[5], p.y) + fx_mul(mvp.m[9],  p.z) + mvp.m[13];
            fx W = fx_mul(mvp.m[3], p.x) + fx_mul(mvp.m[7], p.y) + fx_mul(mvp.m[11], p.z) + mvp.m[15];
            cx[k] = X; cy[k] = Y; cw[k] = W;
        }

        /* Reject if any vertex is at/behind the eye (w <= small). Keeps the
         * perspective divide well-defined for this simple (non-clipping) path. */
        if (cw[0] <= FX_HALF/8 || cw[1] <= FX_HALF/8 || cw[2] <= FX_HALF/8)
            continue;

        /* --- perspective divide -> NDC, then NDC -> screen --- */
        fx sx[3], sy[3]; g3d_i32 sz[3];
        for (int k = 0; k < 3; k++) {
            fx ndcx = fx_div(cx[k], cw[k]);          /* [-1,1] */
            fx ndcy = fx_div(cy[k], cw[k]);
            /* screen: x = (ndcx+1)/2 * w ; y = (1-ndcy)/2 * h (flip Y) */
            sx[k] = halfw + fx_mul(ndcx, halfw);
            sy[k] = halfh - fx_mul(ndcy, halfh);
            /* depth key: nearer (small w) should be smaller. Use w directly
             * scaled: map ~[near..far] world depth into the z-buffer integer
             * range. We use the eye-space distance (cw == -z_eye) for a stable,
             * monotonic key. */
            g3d_i64 zk = (g3d_i64)cw[k];             /* Q16.16 eye distance */
            if (zk < 0) zk = 0;
            sz[k] = (g3d_i32)(zk);                   /* monotonic in depth */
        }

        /* --- backface cull in screen space (signed area sign) --- */
        g3d_i64 area2 = ((g3d_i64)(sx[1] - sx[0]) * (g3d_i64)(sy[2] - sy[0])
                       - (g3d_i64)(sy[1] - sy[0]) * (g3d_i64)(sx[2] - sx[0])) >> FX_SHIFT;
        /* With our Y-flip, front faces (CCW in model) become CW on screen,
         * giving a negative area; cull the positive (back) ones. */
        if (area2 > 0) continue;

        /* --- flat shading: ambient + diffuse from light --- */
        /* intensity = ambient + (1-ambient) * max(0, N . L) */
        fx ndotl = v3_dot(nrm, L);
        if (ndotl < 0) ndotl = 0;
        fx ambient = fx_ratio(30, 100);              /* 0.30 ambient floor */
        fx intensity = ambient + fx_mul(FX_ONE - ambient, ndotl);
        g3d_u32 col = shade_color(tr->color, intensity);

        g3d_raster_tri(t,
                       sx[0], sy[0], sz[0],
                       sx[1], sy[1], sz[1],
                       sx[2], sy[2], sz[2],
                       col);
    }
}
