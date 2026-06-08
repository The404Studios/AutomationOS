/*
 * derby.c -- "Demolition Derby 3D" (freestanding, ring 3).
 * ========================================================
 *
 * A real-time 3D demolition derby: you drive a car in a walled arena and ram
 * the AI opponents. Cars take damage proportional to ram impact (the AGGRESSOR
 * -- the car driving INTO the other -- takes far less than the one struck on
 * the flank). Health hits zero -> the car is wrecked and becomes a dead hulk.
 * Last car running wins.
 *
 * Pure CPU software 3D via the g3d library (Q16.16 fixed-point, no GPU, no FP):
 * a checkerboard floor + four arena walls (one static world mesh) and a box per
 * car, z-buffered and flat-shaded. A chase camera follows the player; once the
 * player is wrecked it pulls back to an angled overhead so you can watch the AI
 * finish each other off.
 *
 * The simulation runs on a FIXED 16 ms timestep (frame-rate independent) while
 * rendering every frame. The code is deliberately split into small,
 * single-purpose functions over a few globals (g_cars[], g_state) so the
 * AutomationOS IDE's Semantic Lego Map renders a clean read/write/control graph
 * of the game:
 *
 *   derby_main -> derby_reset
 *              -> input_player                       (writes g_cars[0].ctrl_*)
 *              -> ai_drive -> ai_pick_target         (writes g_cars[i].ctrl_*)
 *              -> physics_step                        (reads ctrl_*, moves cars)
 *              -> collide_cars -> apply_damage        (ram damage)
 *              -> collide_walls -> apply_damage       (wall scrape)
 *              -> update_game_state                   (last-car-wins)
 *              -> render_scene -> draw_world/draw_car (g3d)
 *              -> draw_hud                            (2D overlay)
 *
 * Controls:
 *   W / Up      accelerate        S / Down   reverse / brake
 *   A / Left    steer left        D / Right  steer right
 *   R           restart           ESC        quit
 *
 * Build (matches build_all.sh; links wl + bitfont + g3d like cube3d):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/derby/derby.c -o /tmp/derby.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/derby.o /tmp/wlc.o /tmp/bf.o /tmp/g3d.o -o /tmp/derby.elf
 *
 * Serial output:
 *   DERBY: ready
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/g3d/g3d.h"

/* Silence the gcc-15 stack-protector re-injection + the loop->memset rewrite
 * that break the freestanding link (same guard cube3d/asteroids use). */
#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")

/* ---- syscall numbers (AOS, NOT Linux) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- key scancodes (kernel/include/input.h) ---- */
#define KEY_ESC     1
#define KEY_R      19
#define KEY_UP    103
#define KEY_DOWN  108
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_W      17
#define KEY_A      30
#define KEY_S      31
#define KEY_D      32
#define KEY_SPACE  57

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

/* ====================================================================== *
 *  Tunables (all world distances in Q16.16 world units)
 * ====================================================================== */
#define WIN_W       640
#define WIN_H       480
#define NUM_CARS    5            /* index 0 = the player                  */
#define STEP_MS     16           /* fixed simulation timestep             */

#define ARENA_HALF_I  9          /* arena spans [-9, +9] on X and Z       */
#define CAR_HX      fx_ratio(45,100)   /* half width  (x)                 */
#define CAR_HY      fx_ratio(28,100)   /* half height (y)                 */
#define CAR_HZ      fx_ratio(80,100)   /* half length (z, "forward")      */
#define CAR_R       fx_ratio(82,100)   /* collision radius                */

#define ACCEL       fx_ratio(16,1000)  /* throttle accel per step         */
#define MAX_FWD     fx_ratio(22,100)   /* top forward speed / step        */
#define MAX_REV     fx_ratio(11,100)   /* top reverse speed / step        */
#define FRICTION    fx_ratio(93,100)   /* per-step speed decay            */
#define TURN_RATE   16                 /* brads/step at full steer        */
#define DMG_SCALE   fx_from_int(95)    /* impact closing-speed -> hp      */
#define DMG_MINCLOSE fx_ratio(3,100)   /* below this closing speed: no hp */

/* ---- game state ---- */
typedef struct {
    fx   x, z;            /* position on the floor plane (y is fixed)      */
    i32  head;            /* heading in g3d brads (0..1023), +Z at head=0  */
    fx   speed;           /* signed forward speed per step                 */
    i32  health;          /* 0..100; 0 => wrecked                          */
    i32  alive;           /* 1 while drivable                              */
    u32  color;           /* body ARGB                                     */
    i32  is_player;       /* 1 for g_cars[0]                               */
    i32  ctrl_throttle;   /* -1/0/+1 set by input_player / ai_drive        */
    i32  ctrl_steer;      /* -1/0/+1                                        */
    i32  recover;         /* AI "I'm stuck, reverse out" countdown (steps) */
    i32  hitflash;        /* >0 => flash white briefly after a hit         */
} Car;

static Car g_cars[NUM_CARS];
static int g_alive_count;
static int g_state;        /* 0 = playing, 1 = over                        */
static int g_winner;       /* car index, or -1 for a draw/none            */
static u32 g_rng = 0x1234567u;

/* held-key direction flags for the player (set in the event loop) */
static i32 g_hold_fwd, g_hold_back, g_hold_left, g_hold_right;

/* per-car body colors (player is the bright first one). */
static const u32 CAR_COLORS[NUM_CARS] = {
    0xFF36C5F0u,  /* player  - cyan   */
    0xFFE63946u,  /* red             */
    0xFFF4A261u,  /* orange          */
    0xFF8AC926u,  /* green           */
    0xFFB07CFFu,  /* purple          */
};

static u32 rng_next(void) { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) & 0x7FFFFFu; }

static fx fx_clamp(fx v, fx lo, fx hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* heading -> unit forward direction on the XZ plane. */
static fx car_dirx(i32 head) { return fx_sin(head & G3D_ANG_MASK); }
static fx car_dirz(i32 head) { return fx_cos(head & G3D_ANG_MASK); }

/* ====================================================================== *
 *  Reset
 * ====================================================================== */
static void derby_reset(u64 seed)
{
    g_rng = (u32)(seed ^ 0x9E3779B9u) | 1u;
    for (int i = 0; i < NUM_CARS; i++) {
        Car *c = &g_cars[i];
        i32 ang = (i * G3D_ANG_STEPS) / NUM_CARS;     /* even ring spread   */
        c->x = fx_mul(fx_sin(ang), fx_from_int(5));
        c->z = fx_mul(fx_cos(ang), fx_from_int(5));
        c->head = (ang + (G3D_ANG_STEPS / 2)) & G3D_ANG_MASK;  /* face center */
        c->speed = 0;
        c->health = 100;
        c->alive = 1;
        c->color = CAR_COLORS[i];
        c->is_player = (i == 0);
        c->ctrl_throttle = 0;
        c->ctrl_steer = 0;
        c->recover = 0;
        c->hitflash = 0;
    }
    g_alive_count = NUM_CARS;
    g_state = 0;
    g_winner = -1;
}

/* ====================================================================== *
 *  Damage
 * ====================================================================== */
static void apply_damage(int i, int dmg)
{
    Car *c = &g_cars[i];
    if (!c->alive || dmg <= 0) return;
    c->health -= dmg;
    c->hitflash = 6;
    if (c->health <= 0) {
        c->health = 0;
        c->alive = 0;
        c->speed = 0;
        if (g_alive_count > 0) g_alive_count--;
    }
}

/* ====================================================================== *
 *  AI -- rule-based opponent driver
 * ---------------------------------------------------------------------- *
 *  Rules, in priority order:
 *   R1 RECOVER : if a recover timer is armed (we were stuck / boxed in),
 *               reverse and crank the wheel until it expires.
 *   R2 AVOID   : if heading toward a wall and close to it, steer back inward.
 *   R3 HUNT    : otherwise pick the nearest living rival, steer to face it,
 *               and floor the throttle to ram it.
 *   R4 STUCK   : if we've been crawling for a while, arm a recover burst.
 * ====================================================================== */
static int ai_pick_target(int self)
{
    int best = -1;
    fx  bestd2 = 0;
    Car *s = &g_cars[self];
    for (int j = 0; j < NUM_CARS; j++) {
        if (j == self || !g_cars[j].alive) continue;
        fx dx = g_cars[j].x - s->x;
        fx dz = g_cars[j].z - s->z;
        fx d2 = fx_mul(dx, dx) + fx_mul(dz, dz);
        if (best < 0 || d2 < bestd2) { best = j; bestd2 = d2; }
    }
    return best;
}

static void ai_drive(int i)
{
    Car *c = &g_cars[i];
    if (!c->alive) { c->ctrl_throttle = 0; c->ctrl_steer = 0; return; }

    /* R1 RECOVER: back out of a jam with the wheel turned. */
    if (c->recover > 0) {
        c->recover--;
        c->ctrl_throttle = -1;
        c->ctrl_steer = (i & 1) ? +1 : -1;
        return;
    }

    /* R2 AVOID: bias away from a wall we're driving into. */
    fx half = fx_from_int(ARENA_HALF_I) - fx_from_int(2);
    if (c->x >  half || c->x < -half || c->z > half || c->z < -half) {
        /* steer toward the arena center (0,0) */
        fx tx = -c->x, tz = -c->z;
        fx dirx = car_dirx(c->head), dirz = car_dirz(c->head);
        fx cross = fx_mul(dirx, tz) - fx_mul(dirz, tx);
        c->ctrl_steer = (cross > 0) ? +1 : -1;
        c->ctrl_throttle = +1;
        return;
    }

    /* R3 HUNT: face + ram the nearest living rival. */
    int t = ai_pick_target(i);
    if (t < 0) { c->ctrl_throttle = 0; c->ctrl_steer = 0; return; }
    fx dx = g_cars[t].x - c->x;
    fx dz = g_cars[t].z - c->z;
    fx dirx = car_dirx(c->head), dirz = car_dirz(c->head);
    fx cross = fx_mul(dirx, dz) - fx_mul(dirz, dx);   /* >0 => target is left */
    fx dot   = fx_mul(dirx, dx) + fx_mul(dirz, dz);   /* >0 => target ahead   */

    if (cross > fx_ratio(5,100))      c->ctrl_steer = +1;
    else if (cross < -fx_ratio(5,100)) c->ctrl_steer = -1;
    else                               c->ctrl_steer = 0;
    /* small jitter so packs don't lock into a perfect line */
    if ((rng_next() & 63) == 0) c->ctrl_steer = (rng_next() & 1) ? +1 : -1;
    c->ctrl_throttle = (dot < 0 && (cross > -fx_ratio(5,100) && cross < fx_ratio(5,100)))
                       ? -1   /* dead behind & not turning: reverse to reorient */
                       : +1;  /* otherwise charge */

    /* R4 STUCK: crawling for real -> arm a recover burst. */
    if (c->speed < fx_ratio(3,100) && c->speed > -fx_ratio(3,100))
        if ((rng_next() & 31) == 0) c->recover = 18 + (rng_next() & 15);
}

/* ====================================================================== *
 *  Player input -> control intents
 * ====================================================================== */
static void input_player(void)
{
    Car *c = &g_cars[0];
    if (!c->alive) { c->ctrl_throttle = 0; c->ctrl_steer = 0; return; }
    c->ctrl_throttle = (g_hold_fwd ? 1 : 0) - (g_hold_back ? 1 : 0);
    c->ctrl_steer    = (g_hold_left ? 1 : 0) - (g_hold_right ? 1 : 0);
}

/* ====================================================================== *
 *  Physics integration (one fixed step)
 * ====================================================================== */
static void physics_step(void)
{
    for (int i = 0; i < NUM_CARS; i++) {
        Car *c = &g_cars[i];
        if (c->hitflash > 0) c->hitflash--;
        if (!c->alive) continue;

        /* steering: turn more authority at speed, but allow a little at rest */
        fx sp_abs = c->speed < 0 ? -c->speed : c->speed;
        i32 turn = c->ctrl_steer * (TURN_RATE);
        if (sp_abs < fx_ratio(4,100)) turn = (turn * 5) / 10;   /* sluggish when slow */
        c->head = (c->head + turn) & G3D_ANG_MASK;

        /* throttle + friction */
        c->speed = fx_add(c->speed, c->ctrl_throttle * ACCEL);
        c->speed = fx_mul(c->speed, FRICTION);
        c->speed = fx_clamp(c->speed, -MAX_REV, MAX_FWD);

        /* integrate position */
        c->x = fx_add(c->x, fx_mul(car_dirx(c->head), c->speed));
        c->z = fx_add(c->z, fx_mul(car_dirz(c->head), c->speed));
    }
}

/* ====================================================================== *
 *  Car-vs-car collisions with directional ram damage
 * ====================================================================== */
static void collide_cars(void)
{
    for (int i = 0; i < NUM_CARS; i++) {
        if (!g_cars[i].alive) continue;
        for (int j = i + 1; j < NUM_CARS; j++) {
            if (!g_cars[j].alive) continue;
            Car *a = &g_cars[i], *b = &g_cars[j];

            fx dx = b->x - a->x, dz = b->z - a->z;
            fx d2 = fx_mul(dx, dx) + fx_mul(dz, dz);
            fx mind = CAR_R + CAR_R;
            if (d2 >= fx_mul(mind, mind)) continue;       /* not touching */

            fx dist = fx_sqrt(d2);
            if (dist < fx_ratio(1,100)) { dist = fx_ratio(1,100); dx = dist; dz = 0; }
            fx nx = fx_div(dx, dist), nz = fx_div(dz, dist);   /* a -> b unit */

            /* separate the overlap so they don't grind every frame */
            fx overlap = fx_sub(mind, dist);
            fx push = overlap >> 1;
            a->x = fx_sub(a->x, fx_mul(nx, push));
            a->z = fx_sub(a->z, fx_mul(nz, push));
            b->x = fx_add(b->x, fx_mul(nx, push));
            b->z = fx_add(b->z, fx_mul(nz, push));

            /* velocities along the contact normal */
            fx vax = fx_mul(car_dirx(a->head), a->speed);
            fx vaz = fx_mul(car_dirz(a->head), a->speed);
            fx vbx = fx_mul(car_dirx(b->head), b->speed);
            fx vbz = fx_mul(car_dirz(b->head), b->speed);
            fx closing = fx_mul(vax - vbx, nx) + fx_mul(vaz - vbz, nz);  /* >0 = approaching */
            if (closing < DMG_MINCLOSE) {
                /* resting contact: bleed a little speed, no damage */
                a->speed = fx_mul(a->speed, fx_ratio(7,10));
                b->speed = fx_mul(b->speed, fx_ratio(7,10));
                continue;
            }

            int base = fx_to_int(fx_mul(closing, DMG_SCALE));
            if (base < 1) base = 1;

            /* who is the aggressor? share of the closing each car contributes. */
            fx ca = fx_mul(vax, nx) + fx_mul(vaz, nz);     /* a toward b */
            fx cb = -(fx_mul(vbx, nx) + fx_mul(vbz, nz));  /* b toward a */
            if (ca < 0) ca = 0;
            if (cb < 0) cb = 0;
            fx total = ca + cb; if (total <= 0) total = FX_ONE;
            fx share_a = fx_div(ca, total);                /* 0..1 */
            fx share_b = FX_ONE - share_a;

            /* the aggressor (high share) takes up to 70% less damage */
            fx wa = FX_ONE - fx_mul(share_a, fx_ratio(7,10));
            fx wb = FX_ONE - fx_mul(share_b, fx_ratio(7,10));
            int dmg_a = fx_to_int(fx_mul(fx_from_int(base), wa));
            int dmg_b = fx_to_int(fx_mul(fx_from_int(base), wb));

            apply_damage(i, dmg_a);
            apply_damage(j, dmg_b);

            /* impact bleeds speed off both */
            a->speed = fx_mul(a->speed, fx_ratio(45,100));
            b->speed = fx_mul(b->speed, fx_ratio(45,100));
        }
    }
}

/* ====================================================================== *
 *  Arena wall collisions
 * ====================================================================== */
static void collide_walls(void)
{
    fx lim = fx_from_int(ARENA_HALF_I) - CAR_R;
    for (int i = 0; i < NUM_CARS; i++) {
        Car *c = &g_cars[i];
        if (!c->alive) continue;
        int hit = 0;
        if (c->x >  lim) { c->x =  lim; hit = 1; }
        if (c->x < -lim) { c->x = -lim; hit = 1; }
        if (c->z >  lim) { c->z =  lim; hit = 1; }
        if (c->z < -lim) { c->z = -lim; hit = 1; }
        if (hit) {
            fx sp_abs = c->speed < 0 ? -c->speed : c->speed;
            if (sp_abs > fx_ratio(14,100)) apply_damage(i, 2);   /* scrape */
            c->speed = fx_mul(c->speed, fx_ratio(4,10));
        }
    }
}

/* ====================================================================== *
 *  Win/lose
 * ====================================================================== */
static void update_game_state(void)
{
    if (g_state != 0) return;
    int alive = 0, last = -1;
    for (int i = 0; i < NUM_CARS; i++)
        if (g_cars[i].alive) { alive++; last = i; }
    if (alive <= 1) { g_state = 1; g_winner = (alive == 1) ? last : -1; }
}

/* ====================================================================== *
 *  3D meshes
 * ====================================================================== */
#define MAX_WORLD 160
static g3d_tri g_world[MAX_WORLD];
static int     g_nworld;

static g3d_tri g_carmesh[12];     /* unit-ish car box, recolored per draw */

static void tri_push(g3d_tri *arr, int *n, vec3 a, vec3 b, vec3 c, u32 col)
{
    if (*n >= MAX_WORLD) return;
    arr[*n].v[0] = a; arr[*n].v[1] = b; arr[*n].v[2] = c; arr[*n].color = col;
    (*n)++;
}
/* a quad a-b-c-d (CCW) as two tris; `two_sided` also emits the reverse winding
 * so it is visible from either face (used for floor + walls). */
static void quad_push(g3d_tri *arr, int *n, vec3 a, vec3 b, vec3 c, vec3 d,
                      u32 col, int two_sided)
{
    tri_push(arr, n, a, b, c, col);
    tri_push(arr, n, a, c, d, col);
    if (two_sided) {
        tri_push(arr, n, a, c, b, col);
        tri_push(arr, n, a, d, c, col);
    }
}

/* Build the static arena: a checkerboard floor + four walls, in world space. */
static void build_world(void)
{
    g_nworld = 0;
    int N = 6;                                  /* floor grid cells per side */
    fx half = fx_from_int(ARENA_HALF_I);
    fx cell = fx_div(fx_mul(half, fx_from_int(2)), fx_from_int(N));
    for (int gz = 0; gz < N; gz++) {
        for (int gx = 0; gx < N; gx++) {
            fx x0 = fx_add(-half, fx_mul(cell, fx_from_int(gx)));
            fx x1 = fx_add(x0, cell);
            fx z0 = fx_add(-half, fx_mul(cell, fx_from_int(gz)));
            fx z1 = fx_add(z0, cell);
            u32 col = ((gx + gz) & 1) ? 0xFF26303Au : 0xFF1B232Cu;
            /* top-facing winding (matches cube3d's +Y face order) */
            quad_push(g_world, &g_nworld,
                      v3(x0, 0, z1), v3(x1, 0, z1), v3(x1, 0, z0), v3(x0, 0, z0),
                      col, 0);
        }
    }
    /* four walls, height H, two-sided so they always read */
    fx H = fx_from_int(1);
    u32 wcol = 0xFF3D4C5Au;
    /* +Z wall */
    quad_push(g_world, &g_nworld,
              v3(-half, 0, half), v3(half, 0, half), v3(half, H, half), v3(-half, H, half),
              wcol, 1);
    /* -Z wall */
    quad_push(g_world, &g_nworld,
              v3(-half, 0, -half), v3(half, 0, -half), v3(half, H, -half), v3(-half, H, -half),
              wcol, 1);
    /* +X wall */
    quad_push(g_world, &g_nworld,
              v3(half, 0, -half), v3(half, 0, half), v3(half, H, half), v3(half, H, -half),
              wcol, 1);
    /* -X wall */
    quad_push(g_world, &g_nworld,
              v3(-half, 0, -half), v3(-half, 0, half), v3(-half, H, half), v3(-half, H, -half),
              wcol, 1);
}

/* Build the car box once (centered at origin; +Z is the front). Winding is the
 * cube3d winding (CCW from outside) so backface culling keeps the front faces.
 * Colors are placeholders -- draw_car() recolors per car. */
static void build_carmesh(void)
{
    fx X = CAR_HX, Y = CAR_HY, Z = CAR_HZ;
    vec3 v000 = v3(-X,-Y,-Z), v100 = v3(X,-Y,-Z), v110 = v3(X,Y,-Z), v010 = v3(-X,Y,-Z);
    vec3 v001 = v3(-X,-Y, Z), v101 = v3(X,-Y, Z), v111 = v3(X,Y, Z), v011 = v3(-X,Y, Z);
    int i = 0;
    g_carmesh[i].v[0]=v001; g_carmesh[i].v[1]=v101; g_carmesh[i].v[2]=v111; i++; /* front +Z */
    g_carmesh[i].v[0]=v001; g_carmesh[i].v[1]=v111; g_carmesh[i].v[2]=v011; i++;
    g_carmesh[i].v[0]=v100; g_carmesh[i].v[1]=v000; g_carmesh[i].v[2]=v010; i++; /* back -Z  */
    g_carmesh[i].v[0]=v100; g_carmesh[i].v[1]=v010; g_carmesh[i].v[2]=v110; i++;
    g_carmesh[i].v[0]=v000; g_carmesh[i].v[1]=v001; g_carmesh[i].v[2]=v011; i++; /* left -X  */
    g_carmesh[i].v[0]=v000; g_carmesh[i].v[1]=v011; g_carmesh[i].v[2]=v010; i++;
    g_carmesh[i].v[0]=v101; g_carmesh[i].v[1]=v100; g_carmesh[i].v[2]=v110; i++; /* right +X */
    g_carmesh[i].v[0]=v101; g_carmesh[i].v[1]=v110; g_carmesh[i].v[2]=v111; i++;
    g_carmesh[i].v[0]=v011; g_carmesh[i].v[1]=v111; g_carmesh[i].v[2]=v110; i++; /* top +Y   */
    g_carmesh[i].v[0]=v011; g_carmesh[i].v[1]=v110; g_carmesh[i].v[2]=v010; i++;
    g_carmesh[i].v[0]=v000; g_carmesh[i].v[1]=v100; g_carmesh[i].v[2]=v101; i++; /* bottom -Y*/
    g_carmesh[i].v[0]=v000; g_carmesh[i].v[1]=v101; g_carmesh[i].v[2]=v001; i++;
}

/* ---- render target + z-buffer (static; no malloc) ---- */
static g3d_i32 g_zbuf[WIN_W * WIN_H];
#define BG_COLOR 0xFF0A0E14u

static void clear_surface(wl_window *win)
{
    u32 stride = win->stride / 4u;
    for (u32 y = 0; y < win->h; y++) {
        u32 *row = win->pixels + (u64)y * stride;
        for (u32 x = 0; x < win->w; x++) row[x] = BG_COLOR;
    }
}

/* darken/blend two ARGB colors by a 0..256 factor toward white (for hitflash). */
static u32 tint_white(u32 c, int amt)
{
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += ((255 - r) * amt) >> 8;
    g += ((255 - g) * amt) >> 8;
    b += ((255 - b) * amt) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void draw_car(g3d_target *tgt, mat4 vp, vec3 light, int i)
{
    Car *c = &g_cars[i];
    u32 col;
    if (!c->alive)        col = 0xFF2A2A2Eu;             /* wreck: dark hulk */
    else if (c->hitflash) col = tint_white(c->color, 150);
    else                  col = c->color;
    for (int t = 0; t < 12; t++) g_carmesh[t].color = col;

    /* wrecks sink a touch into the floor and tilt nothing fancy */
    fx ypos = c->alive ? CAR_HY : fx_ratio(18,100);
    mat4 model = mat4_mul(mat4_translate(c->x, ypos, c->z), mat4_rotate_y(c->head));
    mat4 mvp   = mat4_mul(vp, model);
    g3d_draw_mesh(tgt, g_carmesh, 12, model, mvp, light);
}

/* Compute the view matrix: chase the player, or angled-overhead once wrecked. */
static mat4 camera_view(void)
{
    Car *p = &g_cars[0];
    vec3 up = v3(0, FX_ONE, 0);
    if (p->alive) {
        fx dx = car_dirx(p->head), dz = car_dirz(p->head);
        fx back = fx_from_int(6), height = fx_ratio(45,10);
        vec3 eye = v3(fx_sub(p->x, fx_mul(dx, back)), height,
                      fx_sub(p->z, fx_mul(dz, back)));
        vec3 tgt = v3(p->x, fx_from_int(1), p->z);
        return mat4_lookat(eye, tgt, up);
    }
    vec3 eye = v3(0, fx_from_int(15), fx_from_int(11));
    vec3 tgt = v3(0, 0, 0);
    return mat4_lookat(eye, tgt, up);
}

static void render_scene(g3d_target *tgt, mat4 proj, vec3 light)
{
    mat4 view = camera_view();
    mat4 vp   = mat4_mul(proj, view);

    g3d_clear(tgt, BG_COLOR);
    g3d_clear_depth(tgt);
    g3d_draw_mesh(tgt, g_world, g_nworld, mat4_identity(), vp, light);
    /* draw wrecks first, then living cars on top for clarity */
    for (int i = 0; i < NUM_CARS; i++) if (!g_cars[i].alive) draw_car(tgt, vp, light, i);
    for (int i = 0; i < NUM_CARS; i++) if (g_cars[i].alive)  draw_car(tgt, vp, light, i);
}

/* ====================================================================== *
 *  HUD (2D overlay, drawn straight into the surface)
 * ====================================================================== */
static void fill_rect(u32 *px, int stride, int w, int h,
                      int x, int y, int rw, int rh, u32 col)
{
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    for (int yy = 0; yy < rh; yy++) {
        u32 *row = px + (u64)(y + yy) * stride + x;
        for (int xx = 0; xx < rw; xx++) row[xx] = col;
    }
}

static void draw_hud(wl_window *win, int tw, int th)
{
    u32 *px = win->pixels;
    int stride = (int)(win->stride / 4u);
    Car *p = &g_cars[0];

    /* player health bar, top-left */
    int bx = 12, by = 12, bw = 180, bh = 16;
    fill_rect(px, stride, tw, th, bx - 2, by - 2, bw + 4, bh + 4, 0xFF000000u);
    fill_rect(px, stride, tw, th, bx, by, bw, bh, 0xFF333A44u);
    int hpw = (bw * (p->health < 0 ? 0 : p->health)) / 100;
    u32 hpc = p->health > 50 ? 0xFF3CCB5Au : (p->health > 20 ? 0xFFE9C46Au : 0xFFE63946u);
    if (!p->alive) hpc = 0xFF555560u;
    fill_rect(px, stride, tw, th, bx, by, hpw, bh, hpc);
    font_draw_string(px, stride, tw, th, bx + 4, by, p->alive ? "HULL" : "WRECKED",
                     0xFFFFFFFFu);

    /* cars remaining, top-right */
    {
        char buf[24]; int n = 0;
        const char *lbl = "CARS LEFT: ";
        while (lbl[n]) { buf[n] = lbl[n]; n++; }
        int cnt = g_alive_count; if (cnt < 0) cnt = 0;
        buf[n++] = (char)('0' + (cnt % 10)); buf[n] = 0;
        font_draw_string(px, stride, tw, th, tw - 12 - n * FONT_W, 14, buf, 0xFFFFFFFFu);
    }

    /* opponent health pips, under the player bar */
    for (int i = 1; i < NUM_CARS; i++) {
        int oy = by + bh + 8 + (i - 1) * 12;
        fill_rect(px, stride, tw, th, 12, oy, 60, 8, 0xFF333A44u);
        int ow = g_cars[i].alive ? (60 * g_cars[i].health) / 100 : 0;
        fill_rect(px, stride, tw, th, 12, oy, ow, 8,
                  g_cars[i].alive ? g_cars[i].color : 0xFF45454Cu);
    }

    /* controls hint, bottom */
    font_draw_string(px, stride, tw, th, 12, th - FONT_H - 8,
                     "WASD/ARROWS drive   R restart   ESC quit", 0xFFAAB2BFu);

    /* end banner */
    if (g_state == 1) {
        const char *msg = (g_winner == 0) ? "YOU WIN!  Press R" :
                          (g_winner > 0)  ? "WRECKED!  Press R" :
                                            "DRAW!  Press R";
        int mw = (int)k_strlen(msg) * FONT_W * 2;   /* drawn doubled below */
        int mx = (tw - mw) / 2, my = th / 2 - 16;
        fill_rect(px, stride, tw, th, mx - 16, my - 12, mw + 32, FONT_H + 24, 0xCC000000u);
        /* draw the message twice offset-by-1 for a faux-bold, plus a doubled
         * look by stamping each glyph in two adjacent cells */
        for (int s = 0; msg[s]; s++) {
            int gx = mx + s * FONT_W * 2;
            font_draw_char(px, stride, tw, th, gx, my, msg[s], 0xFFFFFFFFu);
            font_draw_char(px, stride, tw, th, gx + 1, my, msg[s], 0xFFFFFFFFu);
        }
    }
}

/* ====================================================================== *
 *  Entry / main loop
 * ====================================================================== */
void _start(void)
{
    if (wl_connect() != 0) {
        print("DERBY: wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Demolition Derby 3D");
    if (!win) {
        print("DERBY: wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    build_world();
    build_carmesh();

    g3d_target tgt;
    tgt.pixels = win->pixels;
    tgt.zbuf   = g_zbuf;
    tgt.stride = (g3d_i32)(win->stride / 4u);
    tgt.w = (i32)win->w < WIN_W ? (i32)win->w : WIN_W;
    tgt.h = (i32)win->h < WIN_H ? (i32)win->h : WIN_H;

    fx aspect = fx_ratio(WIN_W, WIN_H);
    mat4 proj = mat4_perspective(g3d_deg(60), aspect, fx_ratio(1, 10), fx_from_int(120));
    vec3 light = v3(fx_ratio(-3,10), fx_ratio(8,10), fx_ratio(4,10));

    u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    derby_reset(now);
    u64 last = now;
    u64 acc  = 0;

    print("DERBY: ready\n");

    for (;;) {
        /* ---- input ---- */
        int kind, a, b, cev;
        while (wl_poll_event(win, &kind, &a, &b, &cev)) {
            if (kind == WL_EVENT_RESIZE) {
                tgt.pixels = win->pixels;
                tgt.stride = (g3d_i32)(win->stride / 4u);
                tgt.w = (i32)win->w < WIN_W ? (i32)win->w : WIN_W;
                tgt.h = (i32)win->h < WIN_H ? (i32)win->h : WIN_H;
                continue;
            }
            if (kind != WL_EVENT_KEY) continue;
            i32 down = (b == 1);
            switch (a) {
            case KEY_UP:    case KEY_W: g_hold_fwd   = down; break;
            case KEY_DOWN:  case KEY_S: g_hold_back  = down; break;
            case KEY_LEFT:  case KEY_A: g_hold_left  = down; break;
            case KEY_RIGHT: case KEY_D: g_hold_right = down; break;
            case KEY_R: if (down) { g_hold_fwd = g_hold_back = g_hold_left = g_hold_right = 0;
                                    derby_reset((u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0)); } break;
            case KEY_ESC: if (down) sc(SYS_EXIT, 0, 0, 0, 0, 0, 0); break;
            }
        }

        /* ---- fixed-timestep simulation ---- */
        now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        acc += (now - last);
        last = now;
        if (acc > (u64)(STEP_MS * 5)) acc = STEP_MS * 5;   /* cap catch-up */
        int steps = 0;
        while (acc >= STEP_MS && steps < 5) {
            acc -= STEP_MS;
            steps++;
            if (g_state == 0) {
                input_player();
                for (int i = 1; i < NUM_CARS; i++) ai_drive(i);
                physics_step();
                collide_cars();
                collide_walls();
                update_game_state();
            } else {
                /* let hit-flashes fade after the round ends */
                for (int i = 0; i < NUM_CARS; i++)
                    if (g_cars[i].hitflash > 0) g_cars[i].hitflash--;
            }
        }

        /* ---- render ---- */
        clear_surface(win);
        render_scene(&tgt, proj, light);
        draw_hud(win, (int)win->w, (int)win->h);

        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
