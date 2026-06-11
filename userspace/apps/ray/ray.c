/*
 * ray.c -- Wolfenstein-style software raycaster (freestanding, ring 3).
 * =====================================================================
 *
 * A first-person view of a 16x16 grid maze, rendered by classic DDA
 * raycasting: for each screen column we march a ray from the player through
 * the grid until it hits a wall cell, compute the perpendicular distance, and
 * draw a single vertical wall strip whose height is inversely proportional to
 * that distance. Walls are distance-shaded (closer = brighter) and the two
 * grid axes get slightly different shades so corners read clearly. A simple
 * floor/ceiling gradient fills the rest of the column.
 *
 * Pure CPU, ARGB32 output, NO GPU. All math is Q16.16 fixed-point (we reuse
 * g3d's fx type + sin/cos table); no libc, no libm, no floating point.
 *
 * Controls:
 *   W / Up         -- move forward
 *   S / Down       -- move backward
 *   A / D          -- strafe left / right
 *   Left / Right   -- turn
 *   ESC            -- quit
 *
 * Build (matches build_all.sh, links wl + bitfont + g3d):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/ray/ray.c -o /tmp/ray.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/ray.o /tmp/wlc.o /tmp/bf.o /tmp/g3d.o -o /tmp/ray.elf
 *
 * Serial output:
 *   RAY: ready
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/g3d/g3d.h"      /* reuse fx, fx_sin/cos, fx_mul/div, fx_sqrt */

/* See asteroids.c: silence the gcc-15 stack-protector + loop-distribution
 * rewrites that break the freestanding link. */
#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")

/* ---- syscall numbers (AOS, NOT Linux) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- key scancodes ---- */
#define KEY_ESC     1
#define KEY_UP    103
#define KEY_DOWN  108
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_W      17
#define KEY_A      30
#define KEY_S      31
#define KEY_D      32

typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long u64;

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

/* ---- window ---- */
#define WIN_W  640
#define WIN_H  480

/* ====================================================================== *
 *  The map: 16x16 grid. 1..N = wall (cell value picks a color), 0 = empty.
 * ====================================================================== */
#define MAP_W 16
#define MAP_H 16
static const unsigned char MAP[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,2,0,0,0,0,0,1},
    {1,0,2,2,0,0,3,3,0,2,0,4,4,4,0,1},
    {1,0,2,0,0,0,0,3,0,0,0,0,0,4,0,1},
    {1,0,2,0,0,0,0,3,0,0,0,0,0,4,0,1},
    {1,0,0,0,0,5,5,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,5,0,0,0,0,3,3,3,0,0,1},
    {1,0,0,0,0,5,0,0,0,0,3,0,0,0,0,1},
    {1,0,4,4,0,0,0,0,2,2,0,0,0,0,0,1},
    {1,0,4,0,0,0,0,0,2,0,0,5,5,5,0,1},
    {1,0,4,0,0,0,0,0,0,0,0,5,0,0,0,1},
    {1,0,0,0,0,3,3,0,0,0,0,5,0,0,0,1},
    {1,0,0,0,0,3,0,0,0,2,2,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,2,0,0,0,4,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

/* Base wall colors indexed by cell value 1..5. */
static const u32 WALL_COL[6] = {
    0xFF000000,  /* 0 unused */
    0xFFB5651D,  /* 1 brick   */
    0xFF2A6F97,  /* 2 blue     */
    0xFF588157,  /* 3 green    */
    0xFFBC4749,  /* 4 red      */
    0xFFB8860B,  /* 5 gold     */
};

/* Shade an ARGB color by a Q16.16 intensity [0..1]. */
static u32 shade(u32 base, fx inten)
{
    if (inten < 0) inten = 0;
    if (inten > FX_ONE) inten = FX_ONE;
    i32 r = (i32)((base >> 16) & 0xFF);
    i32 g = (i32)((base >> 8)  & 0xFF);
    i32 b = (i32)( base        & 0xFF);
    r = fx_to_int(fx_mul(fx_from_int(r), inten));
    g = fx_to_int(fx_mul(fx_from_int(g), inten));
    b = fx_to_int(fx_mul(fx_from_int(b), inten));
    return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

/* Draw a vertical span at column x, clamped to the live buffer height (cw_h)
 * so a shrunk window can never write past the reallocated surface. */
static inline void vline(u32 *buf, i32 stride, i32 x, i32 y0, i32 y1, u32 c, i32 cw_h)
{
    if (y0 < 0) y0 = 0;
    if (y1 > cw_h) y1 = cw_h;
    for (i32 y = y0; y < y1; y++) buf[(i32)y * stride + x] = c;
}

void _start(void)
{
    if (wl_connect() != 0) {
        print("RAY: wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Raycaster");
    if (!win) {
        print("RAY: wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    i32 stride = (i32)(win->stride / 4u);

    /* ---- player state (Q16.16 world coords; 1 unit == 1 grid cell) ---- */
    fx  px = fx_ratio(3, 1) + FX_HALF;     /* start at (3.5, 3.5)  */
    fx  py = fx_ratio(3, 1) + FX_HALF;
    i32 ang = g3d_deg(0);                  /* facing +X initially  */

    /* FOV: half-plane length. The camera plane is perpendicular to the
     * direction, length tan(fov/2). For ~66 deg FOV, plane ~= 0.66. */
    fx plane_len = fx_ratio(66, 100);

    /* movement tuning (per frame) */
    fx move_step = fx_ratio(6, 100);       /* 0.06 cell/frame      */
    i32 turn_step = 8;                     /* brads/frame          */
    i32 hold_fwd = 0, hold_back = 0, hold_left = 0, hold_right = 0;
    i32 hold_turn_l = 0, hold_turn_r = 0;

    print("RAY: ready\n");

    for (;;) {
        /* ---- input ---- */
        int kind, a, b, cev;
        while (wl_poll_event(win, &kind, &a, &b, &cev)) {
            if (kind == WL_EVENT_RESIZE) {
                /* The library has ALREADY reallocated the buffer and updated
                 * win->{w,h,stride,pixels}. Refresh our cached row stride so
                 * every subsequent write uses the live surface geometry. */
                stride = (i32)(win->stride / 4u);
                continue;
            }
            if (kind != WL_EVENT_KEY) continue;
            i32 down = (b == 1);
            switch (a) {
            case KEY_W: case KEY_UP:   hold_fwd    = down; break;
            case KEY_S: case KEY_DOWN: hold_back   = down; break;
            case KEY_A:                hold_left   = down; break;
            case KEY_D:                hold_right  = down; break;
            case KEY_LEFT:             hold_turn_l = down; break;
            case KEY_RIGHT:            hold_turn_r = down; break;
            case KEY_ESC: if (down) sc(SYS_EXIT, 0, 0, 0, 0, 0, 0); break;
            }
        }

        /* ---- live surface geometry (letterbox a fixed WIN_WxWIN_H canvas) ----
         * stride is refreshed on resize above; re-read w/h every frame. The
         * fixed canvas is drawn at the top-left, clamped to the live buffer so
         * a smaller window never overflows and a larger one shows clean margins. */
        i32 surf_w = (i32)win->w;
        i32 surf_h = (i32)win->h;
        i32 cw = (WIN_W < surf_w) ? WIN_W : surf_w;   /* canvas blit width  */
        i32 ch = (WIN_H < surf_h) ? WIN_H : surf_h;   /* canvas blit height */

        /* ---- turning ---- */
        if (hold_turn_l) ang = (ang - turn_step) & G3D_ANG_MASK;
        if (hold_turn_r) ang = (ang + turn_step) & G3D_ANG_MASK;

        /* direction + camera-plane vectors */
        fx dirx = fx_cos(ang),  diry = fx_sin(ang);
        /* plane is dir rotated -90 deg, scaled by plane_len */
        fx planex = fx_mul(diry,  plane_len);
        fx planey = fx_mul(-dirx, plane_len);

        /* ---- movement with simple wall collision ---- */
        fx ndx = 0, ndy = 0;
        if (hold_fwd)  { ndx += fx_mul(dirx, move_step);  ndy += fx_mul(diry, move_step); }
        if (hold_back) { ndx -= fx_mul(dirx, move_step);  ndy -= fx_mul(diry, move_step); }
        if (hold_left) { ndx += fx_mul(diry, move_step);  ndy -= fx_mul(dirx, move_step); }
        if (hold_right){ ndx -= fx_mul(diry, move_step);  ndy += fx_mul(dirx, move_step); }
        /* try X then Y so we slide along walls instead of sticking */
        {
            fx tx = px + ndx;
            i32 cx = fx_to_int(tx), cy = fx_to_int(py);
            if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H && MAP[cy][cx] == 0) px = tx;
        }
        {
            fx ty = py + ndy;
            i32 cx = fx_to_int(px), cy = fx_to_int(ty);
            if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H && MAP[cy][cx] == 0) py = ty;
        }

        /* ---- letterbox: clear the FULL live surface to black first so any
         * margins around the fixed canvas (a grown window) are never stale. ---- */
        u32 *buf = win->pixels;
        for (i32 y = 0; y < surf_h; y++) {
            u32 *row = buf + (i32)y * stride;
            for (i32 x = 0; x < surf_w; x++) row[x] = 0xFF000000u;
        }

        /* ---- render floor/ceiling background gradient (clamped to canvas) ---- */
        i32 chalf = WIN_H / 2;
        for (i32 y = 0; y < chalf && y < ch; y++) {  /* ceiling: dark -> mid */
            u32 sh = (u32)(20 + (y * 40) / (WIN_H / 2));
            u32 c = 0xFF000000u | (sh << 16) | (sh << 8) | (sh + 10);
            u32 *row = buf + (i32)y * stride;
            for (i32 x = 0; x < cw; x++) row[x] = c;
        }
        for (i32 y = chalf; y < WIN_H && y < ch; y++) {  /* floor: mid -> dark */
            i32 d = WIN_H - y;
            u32 sh = (u32)(20 + (d * 40) / (WIN_H / 2));
            /* warm brownish floor that fades to dark with distance */
            u32 c = 0xFF000000u | ((sh * 2 / 3) << 16) | ((sh * 2 / 3) << 8) | (sh / 2);
            u32 *row = buf + (i32)y * stride;
            for (i32 x = 0; x < cw; x++) row[x] = c;
        }

        /* ====================================================== *
         *  DDA raycast: one ray per screen column.
         * ====================================================== */
        for (i32 x = 0; x < WIN_W && x < cw; x++) {
            /* camera x in [-1, 1] across the screen */
            fx camx = fx_sub(fx_ratio(2 * x, WIN_W), FX_ONE);
            fx rdx = dirx + fx_mul(planex, camx);
            fx rdy = diry + fx_mul(planey, camx);

            i32 mapx = fx_to_int(px);
            i32 mapy = fx_to_int(py);

            /* distance the ray travels to cross one full cell in x / y.
             * deltaDist = |1 / rd|. Guard against near-zero components. */
            fx ddx = (rdx == 0) ? fx_from_int(1 << 14) : fx_abs(fx_div(FX_ONE, rdx));
            fx ddy = (rdy == 0) ? fx_from_int(1 << 14) : fx_abs(fx_div(FX_ONE, rdy));

            i32 stepx, stepy;
            fx sidex, sidey;   /* distance from current pos to next x/y grid line */
            if (rdx < 0) { stepx = -1; sidex = fx_mul(px - fx_from_int(mapx), ddx); }
            else         { stepx =  1; sidex = fx_mul(fx_from_int(mapx) + FX_ONE - px, ddx); }
            if (rdy < 0) { stepy = -1; sidey = fx_mul(py - fx_from_int(mapy), ddy); }
            else         { stepy =  1; sidey = fx_mul(fx_from_int(mapy) + FX_ONE - py, ddy); }

            /* DDA march */
            i32 hit = 0, side = 0, cell = 0, guard = 0;
            while (!hit && guard++ < 64) {
                if (sidex < sidey) { sidex += ddx; mapx += stepx; side = 0; }
                else               { sidey += ddy; mapy += stepy; side = 1; }
                if (mapx < 0 || mapx >= MAP_W || mapy < 0 || mapy >= MAP_H) { hit = 1; cell = 1; break; }
                cell = MAP[mapy][mapx];
                if (cell > 0) hit = 1;
            }

            /* perpendicular distance (avoids fisheye): the side distance minus
             * one delta step gives the distance to the wall along the ray. */
            fx perp = (side == 0) ? (sidex - ddx) : (sidey - ddy);
            if (perp < fx_ratio(1, 100)) perp = fx_ratio(1, 100);

            /* wall strip height = WIN_H / perp (perp in cell units). */
            i32 line_h = fx_to_int(fx_div(fx_from_int(WIN_H), perp));
            i32 y0 = WIN_H / 2 - line_h / 2;
            i32 y1 = WIN_H / 2 + line_h / 2;

            /* distance shading: bright near, dark far; y-sides a touch darker. */
            fx inten = fx_div(FX_ONE, FX_ONE + fx_mul(perp, fx_ratio(35, 100)));
            if (side == 1) inten = fx_mul(inten, fx_ratio(70, 100));
            u32 base = WALL_COL[(cell >= 1 && cell <= 5) ? cell : 1];
            u32 col = shade(base, inten);

            vline(buf, stride, x, y0, y1, col, ch);
        }

        /* HUD */
        font_draw_string(win->pixels, stride, WIN_W, WIN_H, 8, 6,
                         "RAY  WASD move  Left/Right turn  ESC quit", 0xFFFFFFFF);

        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
