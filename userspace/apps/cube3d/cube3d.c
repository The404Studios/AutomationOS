/*
 * cube3d.c -- Rotating shaded 3D cube (freestanding, ring 3).
 * ===========================================================
 *
 * Opens a 640x480 window and renders a solid, flat-shaded cube spinning in
 * real time. Pure CPU software rendering via the g3d library (no GPU): every
 * frame transforms the cube's 12 triangles through a model-view-projection
 * matrix, culls backfaces, shades each face by a directional light, and
 * z-buffers the result into the ARGB32 window surface.
 *
 * Rotation speed is driven by wall-clock time (SYS_GET_TICKS_MS) so the spin
 * is smooth and frame-rate independent.
 *
 * Controls:
 *   W / Up         -- pitch up        (rotate +X)
 *   S / Down       -- pitch down      (rotate -X)
 *   A / Left        -- yaw left         (rotate +Y)
 *   D / Right      -- yaw right        (rotate -Y)
 *   Q / E          -- zoom in / out    (dolly the camera)
 *   SPACE          -- toggle auto-spin
 *   ESC            -- quit
 *
 * No libc, no libm, no floating point: all math is Q16.16 fixed-point inside
 * g3d. Mirrors the build/loop conventions of asteroids.c / tetris.c.
 *
 * Build (matches build_all.sh, links wl + bitfont + g3d):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/cube3d/cube3d.c -o /tmp/cube3d.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/cube3d.o /tmp/wlc.o /tmp/bf.o /tmp/g3d.o -o /tmp/cube3d.elf
 *
 * Serial output:
 *   CUBE3D: ready
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/g3d/g3d.h"

/* See asteroids.c: silence the gcc-15 stack-protector re-injection + the
 * loop-distribution-to-memset rewrite that break the freestanding link. */
#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")

/* ---- syscall numbers (AOS, NOT Linux) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- key scancodes (kernel/include/input.h) ---- */
#define KEY_ESC     1
#define KEY_UP    103
#define KEY_DOWN  108
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_W      17
#define KEY_A      30
#define KEY_S      31
#define KEY_D      32
#define KEY_Q      16
#define KEY_E      18
#define KEY_SPACE  57

/* ---- types ---- */
typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long u64;

/* ---- inline syscall (6-arg form; no fs:0x28 canary) ---- */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }

/* ---- window constants ---- */
#define WIN_W  640
#define WIN_H  480

/* ====================================================================== *
 *  Cube geometry: a unit cube centered at the origin, 6 faces -> 12 tris.
 * ---------------------------------------------------------------------- *
 *  Each face gets a distinct base color so the spin is easy to read; the
 *  g3d flat shader then modulates by the light direction per frame.
 *  Winding is counter-clockwise when viewed from OUTSIDE (so backface
 *  culling keeps the front faces).
 * ====================================================================== */
#define N_TRIS 12
static g3d_tri g_cube[N_TRIS];

/* Build the cube once at startup (vertices in Q16.16, +/-1 unit). */
static void build_cube(void)
{
    fx P = FX_ONE, M = -FX_ONE;
    /* 8 corners */
    vec3 v000 = v3(M, M, M), v100 = v3(P, M, M),
         v110 = v3(P, P, M), v010 = v3(M, P, M),
         v001 = v3(M, M, P), v101 = v3(P, M, P),
         v111 = v3(P, P, P), v011 = v3(M, P, P);

    /* Per-face colors (ARGB). */
    u32 cFront = 0xFFE63946, cBack  = 0xFF457B9D,
        cLeft  = 0xFF2A9D8F, cRight = 0xFFE9C46A,
        cTop   = 0xFFF4A261, cBot   = 0xFF8D99AE;

    int i = 0;
    /* Front (+Z): v001 v101 v111 v011, CCW seen from +Z */
    g_cube[i].v[0]=v001; g_cube[i].v[1]=v101; g_cube[i].v[2]=v111; g_cube[i].color=cFront; i++;
    g_cube[i].v[0]=v001; g_cube[i].v[1]=v111; g_cube[i].v[2]=v011; g_cube[i].color=cFront; i++;
    /* Back (-Z): seen from -Z, CCW is v100 v000 v010 v110 */
    g_cube[i].v[0]=v100; g_cube[i].v[1]=v000; g_cube[i].v[2]=v010; g_cube[i].color=cBack; i++;
    g_cube[i].v[0]=v100; g_cube[i].v[1]=v010; g_cube[i].v[2]=v110; g_cube[i].color=cBack; i++;
    /* Left (-X): seen from -X, CCW is v000 v001 v011 v010 */
    g_cube[i].v[0]=v000; g_cube[i].v[1]=v001; g_cube[i].v[2]=v011; g_cube[i].color=cLeft; i++;
    g_cube[i].v[0]=v000; g_cube[i].v[1]=v011; g_cube[i].v[2]=v010; g_cube[i].color=cLeft; i++;
    /* Right (+X): seen from +X, CCW is v101 v100 v110 v111 */
    g_cube[i].v[0]=v101; g_cube[i].v[1]=v100; g_cube[i].v[2]=v110; g_cube[i].color=cRight; i++;
    g_cube[i].v[0]=v101; g_cube[i].v[1]=v110; g_cube[i].v[2]=v111; g_cube[i].color=cRight; i++;
    /* Top (+Y): seen from +Y, CCW is v011 v111 v110 v010 */
    g_cube[i].v[0]=v011; g_cube[i].v[1]=v111; g_cube[i].v[2]=v110; g_cube[i].color=cTop; i++;
    g_cube[i].v[0]=v011; g_cube[i].v[1]=v110; g_cube[i].v[2]=v010; g_cube[i].color=cTop; i++;
    /* Bottom (-Y): seen from -Y, CCW is v000 v100 v101 v001 */
    g_cube[i].v[0]=v000; g_cube[i].v[1]=v100; g_cube[i].v[2]=v101; g_cube[i].color=cBot; i++;
    g_cube[i].v[0]=v000; g_cube[i].v[1]=v101; g_cube[i].v[2]=v001; g_cube[i].color=cBot; i++;
}

/* ---- z-buffer (static so we never allocate) ---- */
static g3d_i32 g_zbuf[WIN_W * WIN_H];

/* Background used for both the canvas clear and the letterbox margins. */
#define BG_COLOR 0xFF101820u

/* Letterbox helper: paint the FULL current window surface to the background so
 * the margins around the fixed WIN_W x WIN_H canvas are never stale garbage
 * after a resize (compositor Maximize/snap). Bounded strictly to the live
 * win->w/h/stride, so a smaller-than-canvas window can never overflow the
 * (re)allocated buffer. */
static void clear_surface(wl_window *win)
{
    u32 stride = win->stride / 4u;          /* pixels per row, current buffer */
    u32 w = win->w, h = win->h;
    for (u32 y = 0; y < h; y++) {
        u32 *row = win->pixels + (u64)y * stride;
        for (u32 x = 0; x < w; x++) row[x] = BG_COLOR;
    }
}

/* Refresh the g3d render target from the live window after a (re)alloc. The
 * canvas stays top-left and is clamped to min(canvas, window) so a SMALLER
 * window can never overflow the (smaller) pixel/z buffers; tgt.w*tgt.h is thus
 * always <= WIN_W*WIN_H (the static z-buffer size). */
static void retarget(g3d_target *tgt, wl_window *win)
{
    tgt->pixels = win->pixels;
    tgt->stride = (g3d_i32)(win->stride / 4u);     /* stride in pixels */
    tgt->w = ((i32)win->w < WIN_W) ? (i32)win->w : WIN_W;
    tgt->h = ((i32)win->h < WIN_H) ? (i32)win->h : WIN_H;
}

void _start(void)
{
    if (wl_connect() != 0) {
        print("CUBE3D: wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Cube 3D");
    if (!win) {
        print("CUBE3D: wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    build_cube();

    g3d_target tgt;
    tgt.zbuf = g_zbuf;
    /* Clamp the fixed canvas to the actual surface in case the compositor
     * honored a different initial size (letterbox if larger, clip if smaller). */
    retarget(&tgt, win);

    /* Projection is constant: 60-degree FOV, 4:3 aspect. */
    fx aspect = fx_ratio(WIN_W, WIN_H);
    mat4 proj = mat4_perspective(g3d_deg(60), aspect, fx_ratio(1, 10), fx_from_int(100));

    /* Light comes from the upper-front-right. */
    vec3 light = v3(fx_ratio(-4,10), fx_ratio(6,10), fx_ratio(7,10));

    /* Camera distance (dolly) and user-driven rotation offsets, in brads. */
    fx  cam_z   = fx_from_int(5);
    i32 user_yaw = 0, user_pitch = 0;
    i32 auto_spin = 1;
    i32 hold_yaw = 0, hold_pitch = 0;  /* held-key direction (-1/0/+1) */

    print("CUBE3D: ready\n");

    u64 start = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);

    for (;;) {
        /* ---- input ---- */
        int kind, a, b, cev;
        while (wl_poll_event(win, &kind, &a, &b, &cev)) {
            if (kind == WL_EVENT_RESIZE) {
                /* The library already reallocated the buffer and updated
                 * win->{w,h,stride,pixels}. Re-point the render target at the
                 * new buffer/stride and re-clamp the canvas to the (possibly
                 * smaller) surface so every later write stays in bounds. The
                 * fixed canvas stays top-left; the new margins are repainted
                 * each frame by clear_surface(). */
                retarget(&tgt, win);
                continue;
            }
            if (kind != WL_EVENT_KEY) continue;
            i32 down = (b == 1);
            switch (a) {
            case KEY_UP:    case KEY_W: hold_pitch = down ? +1 : 0; break;
            case KEY_DOWN:  case KEY_S: hold_pitch = down ? -1 : 0; break;
            case KEY_LEFT:  case KEY_A: hold_yaw   = down ? +1 : 0; break;
            case KEY_RIGHT: case KEY_D: hold_yaw   = down ? -1 : 0; break;
            case KEY_Q: if (down) { cam_z -= fx_ratio(3,10); if (cam_z < fx_from_int(2)) cam_z = fx_from_int(2); } break;
            case KEY_E: if (down) { cam_z += fx_ratio(3,10); if (cam_z > fx_from_int(20)) cam_z = fx_from_int(20); } break;
            case KEY_SPACE: if (down) auto_spin = !auto_spin; break;
            case KEY_ESC:   if (down) sc(SYS_EXIT, 0, 0, 0, 0, 0, 0); break;
            }
        }

        /* ---- apply held rotation (a few brads per frame) ---- */
        user_yaw   = (user_yaw   + hold_yaw   * 6) & G3D_ANG_MASK;
        user_pitch = (user_pitch + hold_pitch * 6) & G3D_ANG_MASK;

        /* ---- time-driven auto spin ---- */
        u64 t = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        u64 ms = t - start;
        /* one full turn (1024 brads) every ~6s of auto-spin on Y. */
        i32 spin_y = auto_spin ? (i32)((ms * G3D_ANG_STEPS / 6000) & G3D_ANG_MASK) : 0;
        i32 spin_x = auto_spin ? (i32)((ms * G3D_ANG_STEPS / 9000) & G3D_ANG_MASK) : 0;

        i32 ry = (spin_y + user_yaw)   & G3D_ANG_MASK;
        i32 rx = (spin_x + user_pitch) & G3D_ANG_MASK;

        /* ---- build matrices: model = Ry * Rx, view = translate camera back ---- */
        mat4 model = mat4_mul(mat4_rotate_y(ry), mat4_rotate_x(rx));
        mat4 view  = mat4_translate(0, 0, -cam_z);
        mat4 mv    = mat4_mul(view, model);
        mat4 mvp   = mat4_mul(proj, mv);

        /* ---- render ---- *
         * Letterbox: paint the whole (possibly larger) surface first so the
         * margins around the fixed canvas are clean, then draw the fixed
         * canvas at top-left. tgt.w/h are already clamped to <= the window. */
        clear_surface(win);
        g3d_clear(&tgt, BG_COLOR);            /* dark slate background */
        g3d_clear_depth(&tgt);
        g3d_draw_mesh(&tgt, g_cube, N_TRIS, model, mvp, light);

        /* HUD -- clip to the live surface (clamped to the canvas) using the
         * live stride so a smaller window can never overflow the buffer. */
        font_draw_string(win->pixels, tgt.stride, tgt.w, tgt.h, 8, 6,
                         "CUBE3D  WASD rotate  Q/E zoom  SPACE spin  ESC quit",
                         0xFFFFFFFF);

        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
