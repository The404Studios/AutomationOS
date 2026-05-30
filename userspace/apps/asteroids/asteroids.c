/*
 * asteroids.c -- Classic vector "Asteroids" shooter (freestanding, ring 3).
 * =========================================================================
 *
 * A 640x480 black-space window. You pilot a triangular ship that you rotate
 * (LEFT / RIGHT) and thrust (UP) with real momentum/inertia; fire bullets
 * (SPACE); drifting asteroids wrap around the screen edges and split into
 * smaller rocks when shot. Score, lives, and game-over / restart (R).
 *
 * Everything is drawn as vector OUTLINES (Bresenham line segments) for the
 * authentic 1979 arcade look: the ship is a triangle, asteroids are jagged
 * closed polygons, bullets are tiny squares, and the thrust flame flickers.
 *
 * No libc, no libm, NO floating point (ring 3 has no FP): all motion uses
 * Q8 fixed-point integers and a 256-entry integer sin table for rotation.
 *
 * No libc: freestanding inline syscalls + wl_client + bitfont.
 *
 * Build (matches scripts/build_all.sh build_wl_app -- wl + bitfont only):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/asteroids/asteroids.c -o /tmp/asteroids.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/asteroids.o /tmp/wlc.o /tmp/bf.o -o /tmp/asteroids.elf
 *   objdump -d /tmp/asteroids.elf | grep fs:0x28      # MUST be empty
 *
 * Serial output:
 *   [ASTEROIDS] starting
 *   [ASTEROIDS] wave N
 *   [ASTEROIDS] game over score N
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/*
 * Two gcc-15 quirks are neutralised at TU scope here (the -O2 keeps the normal
 * optimisation level, so this only toggles the two problem passes):
 *
 *  - "no-stack-protector": Arch's gcc self-spec re-injects
 *    -fstack-protector-strong AFTER the build's single -fno-stack-protector for
 *    TUs it deems "vulnerable" (local arrays whose address escapes). That emits
 *    a %fs:0x28 canary referencing __stack_chk_fail, which does NOT exist in our
 *    freestanding ring-3 link and trips the build's `objdump | grep fs:0x28`
 *    gate.
 *  - "no-tree-loop-distribute-patterns": at -O2 gcc rewrites the hand-rolled
 *    k_strlen() byte loop (and any clear loop) into a call to libc strlen/memset,
 *    which also don't exist freestanding -> "undefined reference to strlen".
 *
 * Disabling both at file scope keeps every function self-contained regardless of
 * which ones the heuristics would have flagged.
 */
#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")

/* ---- syscall numbers (AOS, NOT Linux) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- key scancodes (kernel/include/input.h) ---- */
#define KEY_ESC     1
#define KEY_R       19
#define KEY_P       25
#define KEY_UP     103
#define KEY_DOWN   108
#define KEY_LEFT   105
#define KEY_RIGHT  106
#define KEY_W       17
#define KEY_A       30
#define KEY_S       31
#define KEY_D       32
#define KEY_SPACE   57
#define KEY_ENTER   28

/* ---- types ---- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;
typedef long           i64;

/* ---- inline syscall (6-arg form) ---- */
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

/* ---- serial helpers ---- */
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }
static void print_u32(u32 v)
{
    char b[12]; i32 i = 0;
    if (v == 0) { sc(SYS_WRITE, 1, (long)"0", 1, 0, 0, 0); return; }
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (i32 j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = '\0';
    sc(SYS_WRITE, 1, (long)b, i, 0, 0, 0);
}

/* Format u32 into buf (NUL-terminated), return length. */
static i32 fmt_u32(char *buf, u32 v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    i32 i = 0; char tmp[12];
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (i32 j = 0; j < i; j++) buf[j] = tmp[i-1-j];
    buf[i] = '\0';
    return i;
}

/* ====================================================================== *
 *  Fixed-point math (Q8: value = real * 256) + integer trig
 * ---------------------------------------------------------------------- *
 *  Angles are measured in 256 "brads" (binary radians) around the circle,
 *  so a full turn = 256 and angle arithmetic wraps with a simple & 0xFF.
 *  SIN_Q8[a] = round(sin(a / 256 * 2*pi) * 256), i.e. a Q8 sine value in
 *  [-256, 256]. Cosine is just sin shifted by a quarter-turn (64 brads).
 * ====================================================================== */
#define FP_SHIFT  8
#define FP_ONE    256
#define ANG_MASK  0xFF        /* 256 angle steps */

/* Q8 sine for the 256 angle steps. Precomputed (no FP at runtime). */
static const i32 SIN_Q8[256] = {
       0,   6,  13,  19,  25,  31,  38,  44,  50,  56,  62,  68,  74,  80,  86,  92,
      98, 104, 109, 115, 121, 126, 132, 137, 142, 147, 152, 157, 162, 167, 172, 177,
     181, 185, 190, 194, 198, 202, 206, 209, 213, 216, 220, 223, 226, 229, 231, 234,
     237, 239, 241, 243, 245, 247, 248, 250, 251, 252, 253, 254, 255, 255, 256, 256,
     256, 256, 256, 255, 255, 254, 253, 252, 251, 250, 248, 247, 245, 243, 241, 239,
     237, 234, 231, 229, 226, 223, 220, 216, 213, 209, 206, 202, 198, 194, 190, 185,
     181, 177, 172, 167, 162, 157, 152, 147, 142, 137, 132, 126, 121, 115, 109, 104,
      98,  92,  86,  80,  74,  68,  62,  56,  50,  44,  38,  31,  25,  19,  13,   6,
       0,  -6, -13, -19, -25, -31, -38, -44, -50, -56, -62, -68, -74, -80, -86, -92,
     -98,-104,-109,-115,-121,-126,-132,-137,-142,-147,-152,-157,-162,-167,-172,-177,
    -181,-185,-190,-194,-198,-202,-206,-209,-213,-216,-220,-223,-226,-229,-231,-234,
    -237,-239,-241,-243,-245,-247,-248,-250,-251,-252,-253,-254,-255,-255,-256,-256,
    -256,-256,-256,-255,-255,-254,-253,-252,-251,-250,-248,-247,-245,-243,-241,-239,
    -237,-234,-231,-229,-226,-223,-220,-216,-213,-209,-206,-202,-198,-194,-190,-185,
    -181,-177,-172,-167,-162,-157,-152,-147,-142,-137,-132,-126,-121,-115,-109,-104,
     -98, -92, -86, -80, -74, -68, -62, -56, -50, -44, -38, -31, -25, -19, -13,  -6,
};
static inline i32 fp_sin(i32 a) { return SIN_Q8[(a) & ANG_MASK]; }
static inline i32 fp_cos(i32 a) { return SIN_Q8[((a) + 64) & ANG_MASK]; }

/* ---- window constants ---- */
#define WIN_W   640
#define WIN_H   480

/* ---- colours (ARGB32) ---- */
#define COL_BG       0xFF000008u   /* near-black space            */
#define COL_SHIP     0xFFFFFFFFu   /* white ship outline          */
#define COL_THRUST   0xFFFF8C1Au   /* orange thrust flame         */
#define COL_ROCK     0xFFB0C4DEu   /* light steel-blue rocks      */
#define COL_BULLET   0xFFFFE45Cu   /* warm yellow bullet          */
#define COL_HUD      0xFFFFFFFFu
#define COL_DIM      0xFF7080A0u
#define COL_TITLE    0xFF4CC9F0u
#define COL_RED      0xFFFF4D6D

/* ---- LCG random (seeded from ticks) ---- */
static u32 g_rand = 0x1234567u;
static inline u32 rng(void) { g_rand = g_rand * 1664525u + 1013904223u; return g_rand; }
/* random in [lo, hi] inclusive */
static i32 rng_range(i32 lo, i32 hi) { return lo + (i32)(rng() % (u32)(hi - lo + 1)); }

/* ====================================================================== *
 *  Vector draw helpers (outlines, no fills) -- self-contained Bresenham
 * ====================================================================== */
static inline void put_px(u32 *buf, u32 stride, i32 x, i32 y, u32 c)
{
    if (x < 0 || x >= WIN_W || y < 0 || y >= WIN_H) return;
    buf[(u32)y * stride + (u32)x] = c;
}

/* clear: fill full window with one colour */
static void clear(u32 *buf, u32 stride, u32 c)
{
    for (i32 y = 0; y < WIN_H; y++) {
        u32 *row = buf + (u32)y * stride;
        for (i32 x = 0; x < WIN_W; x++) row[x] = c;
    }
}

/* fill a small rect (bullets / HUD blips) */
static void fill_rect(u32 *buf, u32 stride, i32 x, i32 y, i32 w, i32 h, u32 c)
{
    i32 x1 = x < 0 ? 0 : x, y1 = y < 0 ? 0 : y;
    i32 x2 = x + w, y2 = y + h;
    if (x2 > WIN_W) x2 = WIN_W;
    if (y2 > WIN_H) y2 = WIN_H;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = c;
    }
}

/* Bresenham line. */
static void draw_line(u32 *buf, u32 stride, i32 x0, i32 y0, i32 x1, i32 y1, u32 c)
{
    i32 dx = x1 - x0; if (dx < 0) dx = -dx;
    i32 dy = y1 - y0; if (dy < 0) dy = -dy;
    i32 sx = x0 < x1 ? 1 : -1;
    i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx - dy;
    for (;;) {
        put_px(buf, stride, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Draw a closed polygon outline from N points (pixel coords). */
static void draw_poly(u32 *buf, u32 stride, const i32 *xs, const i32 *ys, i32 n, u32 c)
{
    for (i32 i = 0; i < n; i++) {
        i32 j = (i + 1) % n;
        draw_line(buf, stride, xs[i], ys[i], xs[j], ys[j], c);
    }
}

/* ====================================================================== *
 *  Game objects
 * ====================================================================== */
/* Positions/velocities are Q8 fixed-point in *pixel* units.
 * px (pixel) = q >> 8.  Screen wraps modulo (WIN_W<<8) / (WIN_H<<8). */
#define WORLD_W   (WIN_W << FP_SHIFT)
#define WORLD_H   (WIN_H << FP_SHIFT)

typedef struct {
    i32 x, y;        /* Q8 position           */
    i32 vx, vy;      /* Q8 velocity per tick  */
    i32 ang;         /* heading 0..255 brads  */
    i32 alive;       /* 1 = flying            */
    i32 thrusting;   /* draw flame this frame */
    i32 invuln;      /* spawn-protection ticks */
} ship_t;

#define MAX_ASTEROIDS  32
typedef struct {
    i32 x, y;        /* Q8 position           */
    i32 vx, vy;      /* Q8 velocity           */
    i32 size;        /* 3=large 2=med 1=small 0=dead */
    i32 ang;         /* current spin angle    */
    i32 spin;        /* spin delta per tick   */
    i32 shape;       /* which jagged radius set (0..2) */
} asteroid_t;

#define MAX_BULLETS  6
typedef struct {
    i32 x, y;        /* Q8 position           */
    i32 vx, vy;      /* Q8 velocity           */
    i32 life;        /* ticks remaining, 0=dead */
} bullet_t;

static ship_t     g_ship;
static asteroid_t g_rocks[MAX_ASTEROIDS];
static bullet_t   g_bullets[MAX_BULLETS];

static u32 g_score;
static i32 g_lives;
static i32 g_wave;
static i32 g_game_over;
static i32 g_paused;

/* Jagged radius templates (Q8 pixels) for asteroid silhouettes. 8 verts. */
#define ROCK_VERTS  8
static const i32 ROCK_SHAPE[3][ROCK_VERTS] = {
    { 256, 200, 256, 180, 240, 170, 256, 210 },
    { 220, 256, 190, 256, 210, 180, 256, 200 },
    { 256, 230, 200, 256, 170, 220, 240, 256 },
};
/* Base radius by size class (pixels). */
static const i32 ROCK_RADIUS[4] = { 0, 12, 24, 44 };  /* idx by size */
/* Score by size class. */
static const u32 ROCK_SCORE[4]  = { 0, 100, 50, 20 };  /* small worth more */

/* ---- physics tuning (all Q8) ---- */
#define SHIP_R        14           /* ship triangle radius (pixels)        */
#define THRUST_ACC    20           /* Q8 accel per thrust tick             */
#define MAX_SPEED     (7 << FP_SHIFT)
#define FRICTION_NUM  252          /* velocity *= 252/256 per tick (drag)  */
#define FRICTION_DEN  256
#define TURN_RATE     5            /* brads per tick                        */
#define BULLET_SPEED  (9 << FP_SHIFT)
#define BULLET_LIFE   45           /* ticks (~0.7s at ~60fps loop)         */
#define INVULN_TICKS  90

/* ---- helpers ---- */
static void wrap_pos(i32 *x, i32 *y)
{
    if (*x < 0)        *x += WORLD_W;
    if (*x >= WORLD_W) *x -= WORLD_W;
    if (*y < 0)        *y += WORLD_H;
    if (*y >= WORLD_H) *y -= WORLD_H;
}

/* spawn one asteroid of given size at (qx,qy) with random-ish drift */
static void spawn_rock(i32 idx, i32 qx, i32 qy, i32 size)
{
    asteroid_t *r = &g_rocks[idx];
    r->x = qx; r->y = qy;
    r->size = size;
    r->shape = (i32)(rng() % 3u);
    /* Speed scales up for smaller rocks (more frantic). */
    i32 base = (4 - size);                 /* 1..3 */
    i32 spd  = (base << (FP_SHIFT - 2)) + (i32)(rng() % (u32)(1 << (FP_SHIFT - 1)));
    i32 dir  = (i32)(rng() & ANG_MASK);
    r->vx = (fp_cos(dir) * spd) >> FP_SHIFT;
    r->vy = (fp_sin(dir) * spd) >> FP_SHIFT;
    r->ang  = (i32)(rng() & ANG_MASK);
    r->spin = rng_range(-3, 3);
    if (r->spin == 0) r->spin = 1;
}

static i32 count_rocks(void)
{
    i32 n = 0;
    for (i32 i = 0; i < MAX_ASTEROIDS; i++)
        if (g_rocks[i].size > 0) n++;
    return n;
}

static i32 free_rock_slot(void)
{
    for (i32 i = 0; i < MAX_ASTEROIDS; i++)
        if (g_rocks[i].size == 0) return i;
    return -1;
}

/* Begin a wave: N large asteroids drifting in from the edges. */
static void start_wave(i32 wave)
{
    for (i32 i = 0; i < MAX_ASTEROIDS; i++) g_rocks[i].size = 0;
    i32 n = 3 + wave;            /* wave 1 -> 4 rocks, grows each wave */
    if (n > 9) n = 9;
    for (i32 i = 0; i < n; i++) {
        /* Spawn near a screen edge, away from the ship centre. */
        i32 qx, qy;
        if (rng() & 1u) { qx = (rng() & 1u) ? (16 << FP_SHIFT) : (WORLD_W - (16 << FP_SHIFT));
                          qy = (i32)(rng() % (u32)WORLD_H); }
        else            { qy = (rng() & 1u) ? (16 << FP_SHIFT) : (WORLD_H - (16 << FP_SHIFT));
                          qx = (i32)(rng() % (u32)WORLD_W); }
        spawn_rock(i, qx, qy, 3);
    }
}

/* Reset ship to centre, stationary, with spawn invulnerability. */
static void reset_ship(void)
{
    g_ship.x = WORLD_W / 2;
    g_ship.y = WORLD_H / 2;
    g_ship.vx = g_ship.vy = 0;
    g_ship.ang = 192;            /* pointing "up" (screen -Y)            */
    g_ship.alive = 1;
    g_ship.thrusting = 0;
    g_ship.invuln = INVULN_TICKS;
}

static void game_init(u64 ticks)
{
    g_rand = (u32)(ticks ^ (ticks >> 17));
    if (g_rand == 0) g_rand = 0xC0FFEEu;
    g_score = 0;
    g_lives = 3;
    g_wave  = 1;
    g_game_over = 0;
    g_paused = 0;
    for (i32 i = 0; i < MAX_BULLETS; i++) g_bullets[i].life = 0;
    reset_ship();
    start_wave(g_wave);
    print("[ASTEROIDS] wave "); print_u32((u32)g_wave); print("\n");
}

/* ---- fire a bullet from the ship nose ---- */
static void fire_bullet(void)
{
    for (i32 i = 0; i < MAX_BULLETS; i++) {
        if (g_bullets[i].life == 0) {
            bullet_t *b = &g_bullets[i];
            i32 c = fp_cos(g_ship.ang), s = fp_sin(g_ship.ang);
            /* spawn at nose: ship.pos + dir * SHIP_R */
            b->x = g_ship.x + c * SHIP_R;
            b->y = g_ship.y + s * SHIP_R;
            b->vx = (c * (BULLET_SPEED >> FP_SHIFT)) + g_ship.vx;
            b->vy = (s * (BULLET_SPEED >> FP_SHIFT)) + g_ship.vy;
            b->life = BULLET_LIFE;
            return;
        }
    }
}

/* squared distance between two Q8 points, returned in pixel^2 (downshifted) */
static i64 dist2_px(i32 ax, i32 ay, i32 bx, i32 by)
{
    i64 dx = (ax - bx) >> FP_SHIFT;
    i64 dy = (ay - by) >> FP_SHIFT;
    return dx * dx + dy * dy;
}

/* Split a rock when hit: spawn two smaller children (if any), award score. */
static void split_rock(i32 idx)
{
    asteroid_t *r = &g_rocks[idx];
    i32 sz = r->size;
    i32 qx = r->x, qy = r->y;
    g_score += ROCK_SCORE[sz];
    r->size = 0;                 /* kill parent */
    if (sz > 1) {
        for (i32 k = 0; k < 2; k++) {
            i32 slot = (k == 0) ? idx : free_rock_slot();
            if (slot < 0) continue;
            spawn_rock(slot, qx, qy, sz - 1);
        }
    }
}

/* ====================================================================== *
 *  Per-tick simulation
 * ====================================================================== */
static i32 g_left, g_right, g_thrust;   /* held-key state (edge+hold) */

static void game_tick(void)
{
    if (g_game_over || g_paused) return;

    /* ---- ship rotation ---- */
    if (g_left)  g_ship.ang = (g_ship.ang - TURN_RATE) & ANG_MASK;
    if (g_right) g_ship.ang = (g_ship.ang + TURN_RATE) & ANG_MASK;

    /* ---- thrust ---- */
    g_ship.thrusting = 0;
    if (g_thrust) {
        g_ship.vx += (fp_cos(g_ship.ang) * THRUST_ACC) >> FP_SHIFT;
        g_ship.vy += (fp_sin(g_ship.ang) * THRUST_ACC) >> FP_SHIFT;
        g_ship.thrusting = 1;
    }
    /* drag + speed clamp */
    g_ship.vx = (g_ship.vx * FRICTION_NUM) / FRICTION_DEN;
    g_ship.vy = (g_ship.vy * FRICTION_NUM) / FRICTION_DEN;
    {
        i64 sp2 = (i64)g_ship.vx * g_ship.vx + (i64)g_ship.vy * g_ship.vy;
        i64 mx  = (i64)MAX_SPEED * MAX_SPEED;
        if (sp2 > mx && sp2 > 0) {
            /* integer scale-down: v *= MAX/|v| via coarse steps */
            while (((i64)g_ship.vx*g_ship.vx + (i64)g_ship.vy*g_ship.vy) > mx) {
                g_ship.vx = (g_ship.vx * 240) / 256;
                g_ship.vy = (g_ship.vy * 240) / 256;
            }
        }
    }
    g_ship.x += g_ship.vx;
    g_ship.y += g_ship.vy;
    wrap_pos(&g_ship.x, &g_ship.y);
    if (g_ship.invuln > 0) g_ship.invuln--;

    /* ---- bullets ---- */
    for (i32 i = 0; i < MAX_BULLETS; i++) {
        bullet_t *b = &g_bullets[i];
        if (b->life == 0) continue;
        b->x += b->vx;
        b->y += b->vy;
        wrap_pos(&b->x, &b->y);
        b->life--;
    }

    /* ---- asteroids: move + spin ---- */
    for (i32 i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *r = &g_rocks[i];
        if (r->size == 0) continue;
        r->x += r->vx;
        r->y += r->vy;
        wrap_pos(&r->x, &r->y);
        r->ang = (r->ang + r->spin) & ANG_MASK;
    }

    /* ---- bullet vs asteroid collisions ---- */
    for (i32 bi = 0; bi < MAX_BULLETS; bi++) {
        bullet_t *b = &g_bullets[bi];
        if (b->life == 0) continue;
        for (i32 ri = 0; ri < MAX_ASTEROIDS; ri++) {
            asteroid_t *r = &g_rocks[ri];
            if (r->size == 0) continue;
            i32 rad = ROCK_RADIUS[r->size];
            i64 rr  = (i64)rad * rad;
            if (dist2_px(b->x, b->y, r->x, r->y) <= rr) {
                b->life = 0;
                split_rock(ri);
                break;
            }
        }
    }

    /* ---- ship vs asteroid collisions ---- */
    if (g_ship.invuln == 0) {
        for (i32 ri = 0; ri < MAX_ASTEROIDS; ri++) {
            asteroid_t *r = &g_rocks[ri];
            if (r->size == 0) continue;
            i32 rad = ROCK_RADIUS[r->size] + (SHIP_R - 4);
            i64 rr  = (i64)rad * rad;
            if (dist2_px(g_ship.x, g_ship.y, r->x, r->y) <= rr) {
                g_lives--;
                if (g_lives <= 0) {
                    g_game_over = 1;
                    print("[ASTEROIDS] game over score ");
                    print_u32(g_score); print("\n");
                } else {
                    reset_ship();
                }
                break;
            }
        }
    }

    /* ---- wave cleared? ---- */
    if (count_rocks() == 0 && !g_game_over) {
        g_wave++;
        start_wave(g_wave);
        print("[ASTEROIDS] wave "); print_u32((u32)g_wave); print("\n");
    }
}

/* ====================================================================== *
 *  Rendering (vector outlines)
 * ====================================================================== */

/* Draw the ship as a white triangle + optional thrust flame.
 * Triangle: nose forward, two rear corners spread +/- ~140 brads. */
static void draw_ship(u32 *buf, u32 stride)
{
    if (!g_ship.alive) return;
    /* Blink while invulnerable. */
    if (g_ship.invuln > 0 && ((g_ship.invuln >> 2) & 1)) return;

    i32 cx = g_ship.x >> FP_SHIFT;
    i32 cy = g_ship.y >> FP_SHIFT;
    i32 a  = g_ship.ang;

    /* Three hull vertices around heading a. */
    i32 nx = cx + ((fp_cos(a) * SHIP_R) >> FP_SHIFT);
    i32 ny = cy + ((fp_sin(a) * SHIP_R) >> FP_SHIFT);
    i32 al = (a + 110) & ANG_MASK;       /* rear-left  */
    i32 ar = (a - 110) & ANG_MASK;       /* rear-right */
    i32 lx = cx + ((fp_cos(al) * SHIP_R) >> FP_SHIFT);
    i32 ly = cy + ((fp_sin(al) * SHIP_R) >> FP_SHIFT);
    i32 rx = cx + ((fp_cos(ar) * SHIP_R) >> FP_SHIFT);
    i32 ry = cy + ((fp_sin(ar) * SHIP_R) >> FP_SHIFT);

    draw_line(buf, stride, nx, ny, lx, ly, COL_SHIP);
    draw_line(buf, stride, nx, ny, rx, ry, COL_SHIP);
    /* Rear notch: connect the two rear corners through an inset point. */
    i32 bx = cx - ((fp_cos(a) * (SHIP_R/2)) >> FP_SHIFT);
    i32 by = cy - ((fp_sin(a) * (SHIP_R/2)) >> FP_SHIFT);
    draw_line(buf, stride, lx, ly, bx, by, COL_SHIP);
    draw_line(buf, stride, rx, ry, bx, by, COL_SHIP);

    /* Thrust flame: flickering tail behind the notch. */
    if (g_ship.thrusting && (rng() & 1u)) {
        i32 fx = cx - ((fp_cos(a) * (SHIP_R + 6)) >> FP_SHIFT);
        i32 fy = cy - ((fp_sin(a) * (SHIP_R + 6)) >> FP_SHIFT);
        draw_line(buf, stride, lx, ly, fx, fy, COL_THRUST);
        draw_line(buf, stride, rx, ry, fx, fy, COL_THRUST);
    }
}

/* Draw one asteroid as a jagged closed polygon (8 verts). */
static void draw_rock(u32 *buf, u32 stride, const asteroid_t *r)
{
    i32 cx = r->x >> FP_SHIFT;
    i32 cy = r->y >> FP_SHIFT;
    i32 base = ROCK_RADIUS[r->size];
    i32 xs[ROCK_VERTS], ys[ROCK_VERTS];
    const i32 *tmpl = ROCK_SHAPE[r->shape];
    for (i32 i = 0; i < ROCK_VERTS; i++) {
        /* vertex angle evenly spaced (256/8 = 32 brads), plus spin */
        i32 va = (r->ang + i * (256 / ROCK_VERTS)) & ANG_MASK;
        /* radius = base * tmpl(Q8) >> 8  (tmpl in [170..256] Q8 fraction) */
        i32 rad = (base * tmpl[i]) >> FP_SHIFT;
        xs[i] = cx + ((fp_cos(va) * rad) >> FP_SHIFT);
        ys[i] = cy + ((fp_sin(va) * rad) >> FP_SHIFT);
    }
    draw_poly(buf, stride, xs, ys, ROCK_VERTS, COL_ROCK);
}

static void draw_bullets(u32 *buf, u32 stride)
{
    for (i32 i = 0; i < MAX_BULLETS; i++) {
        if (g_bullets[i].life == 0) continue;
        i32 x = g_bullets[i].x >> FP_SHIFT;
        i32 y = g_bullets[i].y >> FP_SHIFT;
        fill_rect(buf, stride, x - 1, y - 1, 3, 3, COL_BULLET);
    }
}

/* Mini ship icon for the lives indicator. */
static void draw_life_icon(u32 *buf, u32 stride, i32 px, i32 py)
{
    i32 a = 192;                 /* pointing up */
    i32 r = 7;
    i32 nx = px + ((fp_cos(a) * r) >> FP_SHIFT);
    i32 ny = py + ((fp_sin(a) * r) >> FP_SHIFT);
    i32 al = (a + 110) & ANG_MASK, ar = (a - 110) & ANG_MASK;
    i32 lx = px + ((fp_cos(al) * r) >> FP_SHIFT);
    i32 ly = py + ((fp_sin(al) * r) >> FP_SHIFT);
    i32 rx = px + ((fp_cos(ar) * r) >> FP_SHIFT);
    i32 ry = py + ((fp_sin(ar) * r) >> FP_SHIFT);
    draw_line(buf, stride, nx, ny, lx, ly, COL_DIM);
    draw_line(buf, stride, nx, ny, rx, ry, COL_DIM);
    draw_line(buf, stride, lx, ly, rx, ry, COL_DIM);
}

static void draw_hud(u32 *buf, u32 stride)
{
    /* Score top-left. */
    char line[40]; i32 n = 0;
    const char *pfx = "SCORE ";
    for (i32 i = 0; pfx[i]; i++) line[n++] = pfx[i];
    char num[12]; i32 nl = fmt_u32(num, g_score);
    for (i32 i = 0; i < nl; i++) line[n++] = num[i];
    line[n] = '\0';
    font_draw_string(buf, (i32)stride, WIN_W, WIN_H, 8, 6, line, COL_HUD);

    /* Wave top-right. */
    char wbuf[24]; n = 0;
    const char *wp = "WAVE ";
    for (i32 i = 0; wp[i]; i++) wbuf[n++] = wp[i];
    nl = fmt_u32(num, (u32)g_wave);
    for (i32 i = 0; i < nl; i++) wbuf[n++] = num[i];
    wbuf[n] = '\0';
    i32 ww = font_text_width(wbuf);
    font_draw_string(buf, (i32)stride, WIN_W, WIN_H, WIN_W - ww - 8, 6, wbuf, COL_DIM);

    /* Lives: small ship icons under the score. */
    for (i32 i = 0; i < g_lives; i++)
        draw_life_icon(buf, stride, 16 + i * 22, 34);
}

static void center_text(u32 *buf, u32 stride, i32 y, const char *s, u32 c)
{
    i32 w = font_text_width(s);
    font_draw_string(buf, (i32)stride, WIN_W, WIN_H, (WIN_W - w) / 2, y, s, c);
}

static void render(u32 *buf, u32 stride)
{
    clear(buf, stride, COL_BG);

    for (i32 i = 0; i < MAX_ASTEROIDS; i++)
        if (g_rocks[i].size > 0) draw_rock(buf, stride, &g_rocks[i]);

    draw_bullets(buf, stride);
    draw_ship(buf, stride);
    draw_hud(buf, stride);

    if (g_game_over) {
        /* scanline dim */
        for (i32 y = 0; y < WIN_H; y += 2)
            fill_rect(buf, stride, 0, y, WIN_W, 1, 0xBB000010);
        center_text(buf, stride, WIN_H/2 - 28, "GAME OVER", COL_RED);
        {
            char fs[40]; i32 n = 0; const char *p = "FINAL SCORE ";
            for (i32 i = 0; p[i]; i++) fs[n++] = p[i];
            char num[12]; i32 nl = fmt_u32(num, g_score);
            for (i32 i = 0; i < nl; i++) fs[n++] = num[i];
            fs[n] = '\0';
            center_text(buf, stride, WIN_H/2 - 4, fs, COL_HUD);
        }
        center_text(buf, stride, WIN_H/2 + 24, "PRESS R TO RESTART", COL_DIM);
    } else if (g_paused) {
        for (i32 y = 0; y < WIN_H; y += 2)
            fill_rect(buf, stride, 0, y, WIN_W, 1, 0xBB000010);
        center_text(buf, stride, WIN_H/2 - 8, "PAUSED", COL_TITLE);
        center_text(buf, stride, WIN_H/2 + 14, "PRESS P TO RESUME", COL_DIM);
    }
}

/* ====================================================================== *
 *  Entry point + main loop (~60fps via SYS_GET_TICKS_MS)
 * ====================================================================== */
void _start(void)
{
    print("[ASTEROIDS] starting\n");

    if (wl_connect() != 0) {
        print("[ASTEROIDS] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Asteroids");
    if (!win) {
        print("[ASTEROIDS] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    u32 stride = win->stride / 4u;

    u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    game_init(now);

    u64 last = now;
    #define FRAME_MS  16u            /* ~60 fps */

    for (;;) {
        u64 t = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);

        /* ---- drain input (track held keys for rotate/thrust) ---- */
        int kind, a, b, cev;
        while (wl_poll_event(win, &kind, &a, &b, &cev)) {
            if (kind != WL_EVENT_KEY) continue;
            i32 down = (b == 1);
            switch (a) {
            case KEY_LEFT:  case KEY_A: g_left   = down; break;
            case KEY_RIGHT: case KEY_D: g_right  = down; break;
            case KEY_UP:    case KEY_W: g_thrust = down; break;
            case KEY_SPACE:
                if (down && !g_game_over && !g_paused) fire_bullet();
                else if (down && g_game_over) {        /* restart on SPACE too */
                    now = (u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);
                    game_init(now); last = now;
                }
                break;
            case KEY_P:
                if (down && !g_game_over) g_paused = !g_paused;
                break;
            case KEY_R:
                if (down) {
                    now = (u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);
                    game_init(now); last = now;
                }
                break;
            case KEY_ESC:
                if (down) sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
                break;
            }
        }

        /* ---- fixed-step simulation: catch up missed frames ---- */
        i32 guard = 0;
        while ((t - last) >= FRAME_MS && guard < 4) {
            game_tick();
            last += FRAME_MS;
            guard++;
        }
        if (guard == 0 && (t - last) > (FRAME_MS * 8)) last = t; /* resync */

        render(win->pixels, stride);
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
