/*
 * g3d.h -- Self-contained software 3D renderer (freestanding, ring 3).
 * ====================================================================
 *
 * A tiny CPU-only 3D pipeline that writes straight into an ARGB32 window
 * buffer (the wl_window->pixels surface). There is NO GPU here: every pixel
 * is produced on the CPU.
 *
 * Design constraints (mirrors the existing freestanding games, e.g.
 * userspace/apps/asteroids/asteroids.c):
 *   - No libc, no libm, NO floating point. All math is Q16.16 fixed-point
 *     (value = real * 65536) carried in 32/64-bit integers, plus a 1024-entry
 *     integer sine table for trig. This keeps the link free of __stack_chk_fail
 *     / strlen / any soft-float helpers, so the same `objdump | grep fs:0x28`
 *     gate the games pass also passes here.
 *   - Provide our own fixed-width typedefs (no <stdint.h>).
 *
 * Pipeline:
 *   model -> (mat4) -> view -> (mat4) -> clip/NDC -> screen -> rasterize.
 * Triangles are filled with a z-buffer depth test, flat shading from a single
 * directional light, and backface culling. Perspective-correct *enough* for
 * solid opaque models (depth is interpolated linearly in screen space, which
 * is the classic z-buffer approximation and is fine for convex solids).
 *
 * Build (freestanding, same flags as build_all.sh CF; link g3d.o into the app):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/g3d/g3d.c -o /tmp/g3d.o
 *
 * The SAME source also compiles under a normal host gcc (no -ffreestanding) so
 * the pure math/rasterizer logic can be unit-tested on the host -- see
 * tests/g3d_hosttest.c.
 */

#ifndef G3D_H
#define G3D_H

/* ---- fixed-width typedefs (no stdint in freestanding ring 3) ---- */
typedef unsigned int        g3d_u32;
typedef int                 g3d_i32;
typedef unsigned long long  g3d_u64;
typedef long long           g3d_i64;

/* ====================================================================== *
 *  Q16.16 fixed-point scalar type
 * ---------------------------------------------------------------------- *
 *  fx = real * 65536. Addition/subtraction are plain integer ops; multiply
 *  and divide go through 64-bit intermediates to avoid overflow.
 * ====================================================================== */
typedef g3d_i32 fx;                 /* Q16.16 fixed-point scalar */

#define FX_SHIFT   16
#define FX_ONE     (1 << FX_SHIFT)              /* 1.0 in Q16.16   */
#define FX_HALF    (FX_ONE >> 1)                /* 0.5             */

/* int <-> fx conversions */
static inline fx  fx_from_int(g3d_i32 i) { return (fx)(i << FX_SHIFT); }
static inline g3d_i32 fx_to_int(fx a)    { return (g3d_i32)(a >> FX_SHIFT); }
/* round-to-nearest fx -> int */
static inline g3d_i32 fx_round(fx a)     { return (g3d_i32)((a + FX_HALF) >> FX_SHIFT); }

static inline fx fx_add(fx a, fx b) { return a + b; }
static inline fx fx_sub(fx a, fx b) { return a - b; }
static inline fx fx_abs(fx a)       { return a < 0 ? -a : a; }
static inline fx fx_mul(fx a, fx b) { return (fx)(((g3d_i64)a * (g3d_i64)b) >> FX_SHIFT); }
static inline fx fx_div(fx a, fx b) {
    if (b == 0) return 0;
    return (fx)(((g3d_i64)a << FX_SHIFT) / (g3d_i64)b);
}

/* fx from a rational n/d (handy for literals like 3/2 without FP). */
static inline fx fx_ratio(g3d_i32 n, g3d_i32 d) {
    if (d == 0) return 0;
    return (fx)(((g3d_i64)n << FX_SHIFT) / (g3d_i64)d);
}

/* Integer square root of a Q16.16 value, returned as Q16.16. */
fx fx_sqrt(fx a);

/* ====================================================================== *
 *  Trig -- 1024-step circle, Q16.16 results in [-1,1].
 * ---------------------------------------------------------------------- *
 *  Angles are measured in "g3d brads": 1024 steps == full turn, wrapping
 *  with & 0x3FF. The table is generated once at first use (host) -- in the
 *  freestanding build it is a const table baked into .rodata.
 * ====================================================================== */
#define G3D_ANG_STEPS  1024
#define G3D_ANG_MASK   (G3D_ANG_STEPS - 1)

fx fx_sin(g3d_i32 ang);     /* ang in 0..1023 brads (wraps) */
fx fx_cos(g3d_i32 ang);

/* Convert degrees (integer) to g3d brads. */
static inline g3d_i32 g3d_deg(g3d_i32 deg) {
    /* brads = deg * 1024 / 360 */
    return (g3d_i32)(((g3d_i64)deg * G3D_ANG_STEPS) / 360);
}

/* ====================================================================== *
 *  Vector / matrix types
 * ====================================================================== */
typedef struct { fx x, y, z; } vec3;        /* Q16.16 components */

/* Column-major 4x4 matrix: m[col*4 + row], so m transforms v as m*v.
 * (Column-major keeps the multiply/translate math the same as OpenGL.) */
typedef struct { fx m[16]; } mat4;

/* ---- vec3 helpers ---- */
static inline vec3 v3(fx x, fx y, fx z) { vec3 r; r.x = x; r.y = y; r.z = z; return r; }
static inline vec3 v3_add(vec3 a, vec3 b){ return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b){ return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline vec3 v3_scale(vec3 a, fx s){ return v3(fx_mul(a.x,s), fx_mul(a.y,s), fx_mul(a.z,s)); }
static inline fx   v3_dot(vec3 a, vec3 b){ return fx_mul(a.x,b.x) + fx_mul(a.y,b.y) + fx_mul(a.z,b.z); }
static inline vec3 v3_cross(vec3 a, vec3 b){
    return v3(fx_mul(a.y,b.z) - fx_mul(a.z,b.y),
              fx_mul(a.z,b.x) - fx_mul(a.x,b.z),
              fx_mul(a.x,b.y) - fx_mul(a.y,b.x));
}
fx   v3_len(vec3 a);                 /* |a| as Q16.16 */
vec3 v3_normalize(vec3 a);           /* a / |a| (zero vector -> zero) */

/* ---- mat4 builders / ops ---- */
mat4 mat4_identity(void);
mat4 mat4_mul(mat4 a, mat4 b);                 /* a * b (apply b first)        */
vec3 mat4_mul_point(mat4 m, vec3 p);           /* transform a point (w=1)      */
mat4 mat4_translate(fx x, fx y, fx z);
mat4 mat4_scale(fx sx, fx sy, fx sz);
mat4 mat4_rotate_x(g3d_i32 ang);               /* ang in g3d brads             */
mat4 mat4_rotate_y(g3d_i32 ang);
mat4 mat4_rotate_z(g3d_i32 ang);
/* Perspective projection. fov is in g3d brads (e.g. g3d_deg(60)). aspect=w/h
 * as Q16.16. near/far in Q16.16 world units. Maps eye space (camera looking
 * down -Z) to clip space; the renderer does the NDC->screen step. */
mat4 mat4_perspective(g3d_i32 fov, fx aspect, fx znear, fx zfar);
/* Right-handed lookAt: eye position, target point, up vector. */
mat4 mat4_lookat(vec3 eye, vec3 target, vec3 up);

/* ====================================================================== *
 *  Rasterizer target: an ARGB32 surface + matching z-buffer.
 * ---------------------------------------------------------------------- *
 *  The z-buffer is caller-provided (w*h g3d_i32 entries) so the library
 *  performs NO allocation -- there is no malloc in this freestanding world.
 *  Depth stored is the projected NDC z mapped to [0 .. G3D_ZMAX]; smaller =
 *  nearer, and the test keeps the nearest fragment.
 * ====================================================================== */
#define G3D_ZMAX  0x7FFFFFFF

typedef struct {
    g3d_u32 *pixels;     /* ARGB32 framebuffer (0xAARRGGBB)           */
    g3d_i32 *zbuf;       /* depth buffer, w*h entries (caller-owned)  */
    g3d_i32  w, h;       /* dimensions in pixels                      */
    g3d_i32  stride;     /* row stride in PIXELS (== w for wl)        */
} g3d_target;

/* Fill the whole color buffer with one ARGB color. */
void g3d_clear(g3d_target *t, g3d_u32 argb);
/* Reset the z-buffer to "infinitely far" (G3D_ZMAX). */
void g3d_clear_depth(g3d_target *t);

/* Pack/unpack ARGB helpers (A in high byte). */
static inline g3d_u32 g3d_rgb(g3d_i32 r, g3d_i32 g, g3d_i32 b) {
    return 0xFF000000u | ((g3d_u32)(r & 0xFF) << 16) |
           ((g3d_u32)(g & 0xFF) << 8) | (g3d_u32)(b & 0xFF);
}

/* ====================================================================== *
 *  The triangle: three model-space vertices + a base (unlit) color.
 * ====================================================================== */
typedef struct {
    vec3    v[3];        /* model-space positions (Q16.16) */
    g3d_u32 color;       /* base ARGB color (lit by g3d_draw_mesh) */
} g3d_tri;

/* Draw a single screen-space triangle with z-test. (a,b,c) are screen X/Y as
 * Q16.16 with an interpolated depth z in [0..G3D_ZMAX]. Color is final (already
 * shaded). Backface culling is the caller's job here -- used internally. */
void g3d_raster_tri(g3d_target *t,
                    fx ax, fx ay, g3d_i32 az,
                    fx bx, fx by, g3d_i32 bz,
                    fx cx, fx cy, g3d_i32 cz,
                    g3d_u32 color);

/* ---- the convenience high-level entry ---- *
 * Transform `count` model-space triangles by `mvp` (model-view-projection),
 * cull backfaces, flat-shade each surviving face from `light_dir` (a direction
 * in world/eye space; brighter when the face normal points toward the light),
 * and rasterize with depth testing into `t`.
 *
 * `model` is the model matrix alone (used to transform normals for lighting);
 * pass mat4_identity() if your tris are already in world space. */
void g3d_draw_mesh(g3d_target *t, const g3d_tri *tris, g3d_i32 count,
                   mat4 model, mat4 mvp, vec3 light_dir);

#endif /* G3D_H */
