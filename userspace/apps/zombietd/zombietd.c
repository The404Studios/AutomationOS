/*
 * zombietd.c -- "ZOMBIE BASTION" : Zombie Survival Tower Defense / Base-Defense
 *               Tycoon (freestanding, ring 3).
 * ===========================================================================
 *
 * Defend a central BASE from endless, escalating waves of zombies. Zombies
 * spawn from the field edges and seek the base with greedy navigation + light
 * separation (so they swarm without perfectly stacking). Earn cash per kill,
 * spend it to PLACE auto-firing turrets, then UPGRADE or SELL them between
 * waves. Survive as long as you can -- the waves never stop and only get
 * harder. Score = (waves survived * 100) + kills.
 *
 * Zombie types (unlock + scale with wave number):
 *   - Walker  (w1+)  : normal hp / speed, the baseline shambler.
 *   - Runner  (w3+)  : fast, low hp -- rushes the wall.
 *   - Brute   (w6+)  : slow, very high hp, hits the base hard.
 *   - Horde   (w10+) : medium, comes in big packs at high waves.
 * Each type's hp and speed scale up with the wave so late waves are brutal.
 *
 * Turrets (tycoon: buy / upgrade / sell, each has targeting AI):
 *   1 Gun     ($100): cheap, balanced single-target.
 *   2 MachGun ($170): very fast fire, low damage.
 *   3 Cannon  ($260): slow, high damage, SPLASH area damage.
 *   4 Sniper  ($300): very long range, high damage, slow fire.
 *   5 Frost   ($190): medium, applies a SLOW to zombies it hits.
 * Each turret levels up (cost rises per level; boosts dmg/range/fire-rate).
 *
 * Mouse-first controls (mirrors the genre):
 *   - LEFT CLICK a palette turret  -> select that type (range preview follows
 *     the cursor over the field; red = can't place / can't afford).
 *   - LEFT CLICK an empty field spot -> place the selected turret, deduct cash.
 *   - LEFT CLICK a PLACED turret (with nothing selected) -> open its panel:
 *       click UPGRADE to level it up, SELL to remove it for a partial refund.
 *   - LEFT CLICK "START WAVE" (or Space/Enter) -> launch the next wave early.
 *   - Keys 1..5 select turret types; 'r' restart after game over; Esc/'q' quit.
 *   - Hover a placed turret -> shows its range ring.
 *
 * No libc/malloc/stdio/libm: pure inline syscalls + wl_client + bitfont,
 * static buffers, integer / fixed-point math only.
 *
 * Build (wl-direct app = app + wl_client + bitfont -- matches build_all.sh):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -Iuserspace/lib/wl -Iuserspace/lib/font -Iuserspace/lib/game \
 *       -c userspace/apps/zombietd/zombietd.c -o /tmp/ztd.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/ztd.o /tmp/wlc.o /tmp/bf.o -o /tmp/zombietd.elf
 *   objdump -d /tmp/zombietd.elf | grep fs:0x28      # MUST be empty
 *
 * Serial output:
 *   [ZOMBIETD] starting
 *   [ZOMBIETD] game over wave N kills K   (on base destroyed)
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/*
 * Defensively disable the stack-protector for this whole translation unit.
 * The real build uses the freestanding x86_64-elf cross-compiler where
 * -fno-stack-protector already yields canary-free code, but some HOST gccs
 * (Arch's, built --enable-default-ssp) ignore the command-line flag for
 * functions with local arrays; this pragma forces it off regardless so the
 * linked ELF stays free of fs:0x28 canary references.
 */
#pragma GCC optimize ("no-stack-protector")

/* ---- syscall numbers (AOS, see kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- key codes (from kernel/include/input.h) ---- */
#define KEY_ESC     1
#define KEY_1       2
#define KEY_2       3
#define KEY_3       4
#define KEY_4       5
#define KEY_5       6
#define KEY_Q       16
#define KEY_R       19
#define KEY_S       31
#define KEY_U       22
#define KEY_SPACE   57
#define KEY_ENTER   28

/* ---- types ---- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;

/* ---- inline syscall (6-arg generic form) ---- */
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
/* gcc -O2 loop-idiom recognition can rewrite length loops into a call to libc
 * `strlen`; provide one (with that pass disabled so it cannot recurse into
 * itself) to satisfy the freestanding link. */
__attribute__((used, optimize("no-tree-loop-distribute-patterns")))
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }

/* Format u32 into buf (NUL-terminated), return length. tmp[] is sized for a
 * full 64-bit value with margin so it can never overrun. */
static i32 fmt_u32(char *buf, u32 v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    i32 i = 0;
    char tmp[24];
    while (v > 0 && i < (i32)sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (i32 j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
    return i;
}

/* Append helpers for building HUD strings without sprintf. */
static void str_append(char *dst, i32 *pos, const char *src)
{
    for (i32 i = 0; src[i]; i++) dst[(*pos)++] = src[i];
    dst[*pos] = '\0';
}
static void num_append(char *dst, i32 *pos, u32 v)
{
    char num[24];
    i32 n = fmt_u32(num, v);
    for (i32 i = 0; i < n; i++) dst[(*pos)++] = num[i];
    dst[*pos] = '\0';
}

/* ---- misc math ---- */
static i32 i_abs(i32 v) { return v < 0 ? -v : v; }

/* integer sqrt (floor) */
static i32 i_sqrt(i32 v)
{
    if (v <= 0) return 0;
    i32 x = v, r = 0, b = 1 << 30;
    while (b > x) b >>= 2;
    while (b != 0) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* ---- LCG random (seeded from ticks) ---- */
static u32 g_rand_state = 0x1234ABCDu;
static u32 lcg_rand(void)
{
    g_rand_state = g_rand_state * 1664525u + 1013904223u;
    return g_rand_state;
}
static i32 rand_range(i32 n) { if (n <= 1) return 0; return (i32)(lcg_rand() % (u32)n); }

/* ===================================================================== */
/* Window / layout                                                       */
/* ===================================================================== */

#define WIN_W   960
#define WIN_H   680

#define HUD_H        44          /* top HUD bar height                  */
#define PALETTE_H    78          /* bottom palette bar height           */
#define FIELD_Y      HUD_H
#define FIELD_H      (WIN_H - HUD_H - PALETTE_H)   /* playfield height   */
#define PALETTE_Y    (WIN_H - PALETTE_H)

/* ---- colours (ARGB32, 0xAARRGGBB) ---- */
#define COL_GROUND    0xFF20351F   /* dark mossy ground            */
#define COL_GROUND2   0xFF263D24   /* checker accent               */
#define COL_HUD_BG    0xFF12233A
#define COL_PAL_BG    0xFF0E1B2E
#define COL_WHITE     0xFFFFFFFF
#define COL_BLACK     0xFF000000
#define COL_YELLOW    0xFFFFD60A
#define COL_RED       0xFFFF4D6D
#define COL_GREEN     0xFF52E07A
#define COL_GREY      0xFF8A93A0
#define COL_DKGREY    0xFF44505E
#define COL_BTN       0xFF1E7A46
#define COL_BTN_HI    0xFF2BA75F
#define COL_BTN_RED   0xFF9A2B3A
#define COL_BTN_REDHI 0xFFC4374B
#define COL_SEL       0xFFFFD60A
#define COL_BASE      0xFF3A6EA5   /* base structure body          */
#define COL_BASE_HI   0xFF5C9AD6
#define COL_BLOOD     0xFFB01818

/* ===================================================================== */
/* Canvas + draw primitives                                              */
/* ===================================================================== */

typedef struct {
    u32 *buf;
    i32  stride;   /* pixels per row */
    i32  w, h;
} Canvas;

static void fill_rect(Canvas *c, i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > c->w) x2 = c->w;
    i32 y2 = y + h; if (y2 > c->h) y2 = c->h;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = c->buf + (u32)yy * c->stride;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void put_px(Canvas *c, i32 x, i32 y, u32 color)
{
    /* Single-compare unsigned bounds check: negative x/y become huge unsigned
     * values and are rejected too. Every higher-level primitive ultimately
     * relies on (or mirrors) this clamp so NOTHING is written out of bounds. */
    if ((u32)x >= (u32)c->w || (u32)y >= (u32)c->h) return;
    c->buf[(u32)y * c->stride + (u32)x] = color;
}

/* Midpoint filled circle. Every horizontal span is clamped to [0,w) and every
 * row to [0,h) so a center far off-screen can never write outside the buffer. */
static void fill_circle(Canvas *c, i32 cx, i32 cy, i32 r, u32 color)
{
    if (r <= 0) { put_px(c, cx, cy, color); return; }
    i32 r2 = r * r;
    i32 dy0 = -r; if (cy + dy0 < 0)        dy0 = -cy;
    i32 dy1 =  r; if (cy + dy1 > c->h - 1) dy1 = (c->h - 1) - cy;
    for (i32 dy = dy0; dy <= dy1; dy++) {
        i32 yy = cy + dy;
        if (yy < 0 || yy >= c->h) continue;
        i32 span = i_sqrt(r2 - dy * dy);
        i32 x1 = cx - span; if (x1 < 0) x1 = 0; if (x1 > c->w - 1) continue;
        i32 x2 = cx + span; if (x2 >= c->w) x2 = c->w - 1; if (x2 < 0) continue;
        if (x1 > x2) continue;
        u32 *row = c->buf + (u32)yy * c->stride;
        for (i32 x = x1; x <= x2; x++) row[x] = color;
    }
}

/* Circle outline (range preview). */
static void circle_outline(Canvas *c, i32 cx, i32 cy, i32 r, u32 color)
{
    if (r <= 0) return;
    i32 x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put_px(c, cx + x, cy + y, color);
        put_px(c, cx + y, cy + x, color);
        put_px(c, cx - y, cy + x, color);
        put_px(c, cx - x, cy + y, color);
        put_px(c, cx - x, cy - y, color);
        put_px(c, cx - y, cy - x, color);
        put_px(c, cx + y, cy - x, color);
        put_px(c, cx + x, cy - y, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* Bresenham line with optional thickness. */
static void draw_line_thick(Canvas *c, i32 x0, i32 y0, i32 x1, i32 y1,
                            i32 t, u32 color)
{
    i32 dx = i_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    i32 dy = -i_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;
    for (;;) {
        if (t <= 1) put_px(c, x0, y0, color);
        else fill_rect(c, x0 - t / 2, y0 - t / 2, t, t, color);
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Text helpers. */
static void draw_text(Canvas *c, i32 x, i32 y, const char *s, u32 color)
{
    font_draw_string(c->buf, c->stride, c->w, c->h, x, y, s, color);
}
static void draw_text_centered(Canvas *c, i32 cx, i32 y, const char *s, u32 color)
{
    i32 w = font_text_width(s);
    font_draw_string(c->buf, c->stride, c->w, c->h, cx - w / 2, y, s, color);
}

/* A small horizontal HP/progress bar with a dark frame. */
static void draw_bar(Canvas *c, i32 x, i32 y, i32 w, i32 h,
                     i32 cur, i32 max, u32 fg, u32 bg)
{
    if (max <= 0) max = 1;
    if (cur < 0) cur = 0; if (cur > max) cur = max;
    fill_rect(c, x - 1, y - 1, w + 2, h + 2, COL_BLACK);
    fill_rect(c, x, y, w, h, bg);
    i32 fw = (w * cur) / max;
    if (fw > 0) fill_rect(c, x, y, fw, h, fg);
}

/* ===================================================================== */
/* Fixed-point: positions in 1/FP_ONE pixel units                        */
/* ===================================================================== */

#define FP_SHIFT  8
#define FP_ONE    (1 << FP_SHIFT)
#define TO_FP(px) ((px) << FP_SHIFT)
#define TO_PX(fp) ((fp) >> FP_SHIFT)

/* ===================================================================== */
/* The base                                                              */
/* ===================================================================== */

#define BASE_R       46            /* base structure radius (px)         */
#define BASE_HP_MAX  1000

static i32 g_base_x, g_base_y;     /* center of the base (px)            */
static i32 g_base_hp;
static i32 g_base_flash;           /* frames of damage-flash remaining   */

/* ===================================================================== */
/* Zombies (AI seek-to-base)                                             */
/* ===================================================================== */

#define MAX_ZOMBIES 400

#define ZT_WALKER  0
#define ZT_RUNNER  1
#define ZT_BRUTE   2
#define ZT_HORDE   3
#define ZT_COUNT   4

typedef struct {
    const char *name;
    i32 base_hp;       /* hp at wave 1                                  */
    i32 hp_per_wave;   /* extra hp per wave above 1                     */
    i32 speed;         /* movement speed (FP px / tick)                 */
    i32 radius;        /* body radius (px)                              */
    i32 dmg;           /* damage dealt to base per hit                  */
    i32 reward;        /* cash on kill                                  */
    u32 col;           /* body colour                                   */
    u32 col2;          /* accent colour                                 */
} ZombieType;

static const ZombieType ZTYPE[ZT_COUNT] = {
    /* name      hp  hp/w spd  rad dmg rew  col         col2       */
    { "Walker",  6,  3,   46,  11, 6,  3,   0xFF6FA24B, 0xFF45662F },
    { "Runner",  4,  2,   96,  9,  4,  4,   0xFFD8A23B, 0xFF8A6320 },
    { "Brute",   28, 9,   30,  17, 16, 10,  0xFFB04BB0, 0xFF6B2F6B },
    { "Horde",   8,  3,   58,  10, 7,  4,   0xFF4BB0A2, 0xFF2F6B63 },
};

typedef struct {
    i32 alive;
    i32 type;
    i32 hp, hp_max;
    i32 x, y;          /* position (FP)                                 */
    i32 speed;         /* current base speed (FP/tick) for this zombie  */
    i32 slow_ticks;    /* remaining ticks of slow effect                */
    i32 atk_cd;        /* ticks until next hit on the base              */
    i32 hurt;          /* frames of hit-flash remaining                 */
    i32 wob;           /* per-zombie phase for a little shamble wobble  */
} Zombie;

static Zombie g_zom[MAX_ZOMBIES];
static i32    g_zom_count = 0;      /* current live count                 */

static i32 spawn_zombie(i32 type, i32 wave, i32 px, i32 py)
{
    if (g_zom_count >= MAX_ZOMBIES) return -1;
    for (i32 i = 0; i < MAX_ZOMBIES; i++) {
        if (!g_zom[i].alive) {
            const ZombieType *zt = &ZTYPE[type];
            i32 hp = zt->base_hp + zt->hp_per_wave * (wave - 1);
            /* gentle speed ramp: +1 FP/tick every 4 waves, capped */
            i32 spd = zt->speed + (wave / 4) * 4;
            if (spd > zt->speed + 60) spd = zt->speed + 60;
            g_zom[i].alive = 1;
            g_zom[i].type = type;
            g_zom[i].hp = hp;
            g_zom[i].hp_max = hp;
            g_zom[i].x = TO_FP(px);
            g_zom[i].y = TO_FP(py);
            g_zom[i].speed = spd;
            g_zom[i].slow_ticks = 0;
            g_zom[i].atk_cd = 0;
            g_zom[i].hurt = 0;
            g_zom[i].wob = (i32)(lcg_rand() & 255u);
            g_zom_count++;
            return i;
        }
    }
    return -1;
}

/* ===================================================================== */
/* Turrets                                                               */
/* ===================================================================== */

#define MAX_TURRETS 80

#define TU_GUN     0
#define TU_MGUN    1
#define TU_CANNON  2
#define TU_SNIPER  3
#define TU_FROST   4
#define TU_COUNT   5

typedef struct {
    const char *name;
    i32 cost;
    i32 range;       /* px                                            */
    i32 fire_cd;     /* ticks between shots                           */
    i32 dmg;
    i32 splash;      /* splash radius px (0 = single target)          */
    i32 slow;        /* slow ticks applied on hit (0 = none)          */
    i32 proj_speed;  /* projectile speed (px/tick)                    */
    u32 col;         /* base colour                                   */
    u32 barrel;      /* barrel / projectile colour                    */
} TurretType;

static const TurretType TTYPE[TU_COUNT] = {
    /* name      cost range  cd  dmg splash slow pspd  col         barrel     */
    { "Gun",     100,  120,   9,  4,  0,     0,  16,  0xFFB0BEC8, 0xFFE6EDF2 },
    { "MachGun", 170,  108,   3,  2,  0,     0,  18,  0xFFC9A23B, 0xFFFFE08A },
    { "Cannon",  260,  130,  34, 14, 52,     0,  11,  0xFFC76A2A, 0xFF8A3F12 },
    { "Sniper",  300,  240,  40, 30,  0,     0,  30,  0xFF7E5BD0, 0xFFCBB6FF },
    { "Frost",   190,  112,  14,  3,  0,    72,  15,  0xFF59C8E0, 0xFF2A96B5 },
};

typedef struct {
    i32 alive;
    i32 type;
    i32 level;       /* 1..MAX_LEVEL                                  */
    i32 x, y;        /* center px                                     */
    i32 cd;          /* ticks until next shot                         */
    i32 invested;    /* total cash sunk in (for sell refund calc)     */
    i32 aim_x, aim_y;/* last aim point (barrel rotation)              */
    i32 kills;
} Turret;

static Turret g_tur[MAX_TURRETS];
static i32    g_tur_count = 0;
static i32    g_sel_placed = -1;    /* index of a placed turret being inspected */

#define TURRET_R   17               /* turret body radius / overlap test   */
#define MAX_LEVEL  5

/* Effective stats scale with level: each level +25% dmg, +8% range, faster cd. */
static i32 tur_dmg(const Turret *t)
{
    i32 base = TTYPE[t->type].dmg;
    return base + (base * 25 * (t->level - 1)) / 100;
}
static i32 tur_range(const Turret *t)
{
    i32 base = TTYPE[t->type].range;
    return base + (base * 8 * (t->level - 1)) / 100;
}
static i32 tur_fire_cd(const Turret *t)
{
    i32 base = TTYPE[t->type].fire_cd;
    i32 cd = base - (t->level - 1);          /* a touch faster each level */
    i32 floor = (base / 2 > 2) ? base / 2 : 2;
    if (cd < floor) cd = floor;
    return cd;
}
static i32 tur_splash(const Turret *t)
{
    i32 base = TTYPE[t->type].splash;
    if (base == 0) return 0;
    return base + 4 * (t->level - 1);
}
/* Cost to upgrade a turret to its next level (rises with level). */
static i32 tur_upgrade_cost(const Turret *t)
{
    return (TTYPE[t->type].cost * (3 + t->level)) / 4;
}
/* Refund when selling (60% of total invested). */
static i32 tur_sell_value(const Turret *t)
{
    return (t->invested * 60) / 100;
}

/* ===================================================================== */
/* Projectiles                                                           */
/* ===================================================================== */

#define MAX_PROJ 512

typedef struct {
    i32 alive;
    i32 x, y;        /* current pos (FP) */
    i32 vx, vy;      /* velocity (FP/tick) */
    i32 target;      /* homing target zombie index (-1 = dumb) */
    i32 ttl;         /* ticks before auto-expire */
    i32 dmg;
    i32 splash;
    i32 slow;
    u32 col;
} Proj;

static Proj g_proj[MAX_PROJ];

static void spawn_proj(i32 sx, i32 sy, i32 tgt, i32 tx, i32 ty,
                       i32 dmg, i32 splash, i32 slow, i32 spd, u32 col)
{
    i32 dx = tx - sx, dy = ty - sy;
    i32 d = i_sqrt(dx * dx + dy * dy);
    if (d < 1) d = 1;
    for (i32 i = 0; i < MAX_PROJ; i++) {
        if (!g_proj[i].alive) {
            g_proj[i].alive = 1;
            g_proj[i].x = TO_FP(sx);
            g_proj[i].y = TO_FP(sy);
            g_proj[i].vx = (dx * TO_FP(spd)) / d;
            g_proj[i].vy = (dy * TO_FP(spd)) / d;
            g_proj[i].target = tgt;
            g_proj[i].ttl = 80;
            g_proj[i].dmg = dmg;
            g_proj[i].splash = splash;
            g_proj[i].slow = slow;
            g_proj[i].col = col;
            return;
        }
    }
}

/* ===================================================================== */
/* Particles (cosmetic hit/kill bursts)                                  */
/* ===================================================================== */

#define MAX_PART 256

typedef struct {
    i32 alive;
    i32 x, y;        /* FP */
    i32 vx, vy;      /* FP/tick */
    i32 ttl;
    i32 r;
    u32 col;
} Particle;

static Particle g_part[MAX_PART];

static void spawn_burst(i32 px, i32 py, i32 n, u32 col, i32 power)
{
    for (i32 k = 0; k < n; k++) {
        for (i32 i = 0; i < MAX_PART; i++) {
            if (!g_part[i].alive) {
                g_part[i].alive = 1;
                g_part[i].x = TO_FP(px);
                g_part[i].y = TO_FP(py);
                i32 ang = (i32)(lcg_rand() & 255u);
                i32 sp = power + rand_range(power);
                /* crude direction from a tiny LUT-free trig: use sign pattern */
                i32 vx = ((ang & 1) ? sp : -sp);
                i32 vy = ((ang & 2) ? sp : -sp);
                vx = (vx * (1 + rand_range(3))) / 2;
                vy = (vy * (1 + rand_range(3))) / 2;
                g_part[i].vx = vx;
                g_part[i].vy = vy;
                g_part[i].ttl = 10 + rand_range(12);
                g_part[i].r = 1 + rand_range(2);
                g_part[i].col = col;
                break;
            }
        }
    }
}

/* ===================================================================== */
/* Spawn points (field edges)                                            */
/* ===================================================================== */

#define MAX_SPAWNS 6
static i32 g_spawn_x[MAX_SPAWNS];
static i32 g_spawn_y[MAX_SPAWNS];
static i32 g_spawn_n = 0;

static void build_spawns(void)
{
    /* Spread spawn points around the field perimeter (just outside the edges
     * so zombies walk in). Top-left, top-right, mid-left, mid-right, bottom
     * pair. The base is centered; zombies seek it from all sides. */
    i32 left = 8, right = WIN_W - 8;
    i32 top = FIELD_Y + 8, bot = PALETTE_Y - 8;
    i32 i = 0;
    g_spawn_x[i] = left;             g_spawn_y[i] = top;                 i++;
    g_spawn_x[i] = right;            g_spawn_y[i] = top;                 i++;
    g_spawn_x[i] = left;             g_spawn_y[i] = (top + bot) / 2;     i++;
    g_spawn_x[i] = right;            g_spawn_y[i] = (top + bot) / 2;     i++;
    g_spawn_x[i] = left;             g_spawn_y[i] = bot;                 i++;
    g_spawn_x[i] = right;            g_spawn_y[i] = bot;                 i++;
    g_spawn_n = i;
}

/* ===================================================================== */
/* Waves (endless, procedurally escalating)                              */
/* ===================================================================== */

#define ST_BUILD     0   /* between waves, placing/upgrading turrets */
#define ST_WAVE      1   /* wave in progress                         */
#define ST_GAMEOVER  2

static i32 g_state;
static i32 g_cash;
static i32 g_wave;           /* current wave number (1-based)            */
static i32 g_kills;
static i32 g_sel_turret;     /* selected palette turret (-1 none)        */
static i32 g_mx, g_my;       /* current mouse pos                        */

/* Spawn scheduling for the in-progress wave. */
static i32 g_to_spawn;       /* zombies left to spawn this wave          */
static i32 g_spawn_cd;       /* ticks until next spawn                   */
static i32 g_spawn_gap;      /* ticks between spawns this wave           */
static i32 g_build_timer;    /* auto-start countdown in BUILD (ticks)    */

/* Zombie count for wave N: grows steadily and never stops. */
static i32 wave_zombie_count(i32 w)
{
    return 8 + w * 3 + (w * w) / 6;
}
/* Ticks between spawns for wave N: shrinks (faster) at higher waves. */
static i32 wave_spawn_gap(i32 w)
{
    i32 g = 26 - w;
    if (g < 5) g = 5;
    return g;
}

/* Pick a zombie type for this wave using weighted rolls; tougher types unlock
 * at thresholds and become more common as the wave number climbs. */
static i32 roll_zombie_type(i32 w)
{
    /* Build a weight table that depends on the wave. */
    i32 wk = 100;                       /* walker baseline               */
    i32 rn = (w >= 3)  ? (10 + w * 2)  : 0;
    i32 br = (w >= 6)  ? (6 + w)       : 0;
    i32 hd = (w >= 10) ? (8 + w)       : 0;
    /* walkers thin out a little once the others arrive */
    if (w >= 6) wk = 60;
    if (w >= 12) wk = 40;
    i32 total = wk + rn + br + hd;
    if (total <= 0) return ZT_WALKER;
    i32 r = rand_range(total);
    if (r < wk) return ZT_WALKER; r -= wk;
    if (r < rn) return ZT_RUNNER; r -= rn;
    if (r < br) return ZT_BRUTE;  r -= br;
    return ZT_HORDE;
}

/* ===================================================================== */
/* Init / reset                                                          */
/* ===================================================================== */

static void reset_game(u64 ticks)
{
    g_rand_state = (u32)(ticks ^ (ticks >> 13) ^ 0x9E3779B9u);
    if (g_rand_state == 0) g_rand_state = 0xC0FFEEu;

    for (i32 i = 0; i < MAX_ZOMBIES; i++) g_zom[i].alive  = 0;
    for (i32 i = 0; i < MAX_TURRETS; i++) g_tur[i].alive  = 0;
    for (i32 i = 0; i < MAX_PROJ; i++)    g_proj[i].alive = 0;
    for (i32 i = 0; i < MAX_PART; i++)    g_part[i].alive = 0;
    g_zom_count = 0;
    g_tur_count = 0;

    g_base_x = WIN_W / 2;
    g_base_y = FIELD_Y + FIELD_H / 2;
    g_base_hp = BASE_HP_MAX;
    g_base_flash = 0;

    g_state = ST_BUILD;
    g_cash = 320;
    g_wave = 1;
    g_kills = 0;
    g_sel_turret = -1;
    g_sel_placed = -1;
    g_to_spawn = 0;
    g_spawn_cd = 0;
    g_spawn_gap = wave_spawn_gap(1);
    g_build_timer = 60 * 12;     /* ~12s to set up before wave 1 auto-starts */
}

static void start_wave(void)
{
    if (g_state == ST_WAVE) return;
    g_to_spawn = wave_zombie_count(g_wave);
    g_spawn_gap = wave_spawn_gap(g_wave);
    g_spawn_cd = 6;
    g_state = ST_WAVE;
    g_sel_placed = -1;
}

/* ===================================================================== */
/* Update -- AI & simulation                                             */
/* ===================================================================== */

/* Acquire the best target for a turret: the zombie in range that is CLOSEST
 * to the base (most urgent threat). Returns index or -1. */
static i32 acquire_target(i32 tx, i32 ty, i32 range)
{
    i32 r2 = range * range;
    i32 best = -1;
    i32 best_base_d2 = 0x7FFFFFFF;
    for (i32 i = 0; i < MAX_ZOMBIES; i++) {
        if (!g_zom[i].alive) continue;
        i32 zx = TO_PX(g_zom[i].x), zy = TO_PX(g_zom[i].y);
        i32 dx = zx - tx, dy = zy - ty;
        if (dx * dx + dy * dy <= r2) {
            i32 bx = zx - g_base_x, by = zy - g_base_y;
            i32 bd2 = bx * bx + by * by;
            if (bd2 < best_base_d2) { best_base_d2 = bd2; best = i; }
        }
    }
    return best;
}

static void kill_zombie(i32 idx, i32 by_turret)
{
    Zombie *z = &g_zom[idx];
    if (!z->alive) return;
    g_cash += ZTYPE[z->type].reward;
    g_kills++;
    if (by_turret >= 0 && by_turret < MAX_TURRETS && g_tur[by_turret].alive)
        g_tur[by_turret].kills++;
    spawn_burst(TO_PX(z->x), TO_PX(z->y), 8, COL_BLOOD, 3);
    spawn_burst(TO_PX(z->x), TO_PX(z->y), 4, ZTYPE[z->type].col, 2);
    z->alive = 0;
    g_zom_count--;
}

/* Apply damage to a zombie; returns 1 if it died. */
static i32 damage_zombie(i32 idx, i32 dmg, i32 slow, i32 by_turret)
{
    Zombie *z = &g_zom[idx];
    if (!z->alive) return 0;
    if (slow > 0 && z->slow_ticks < slow) z->slow_ticks = slow;
    z->hp -= dmg;
    z->hurt = 4;
    if (z->hp <= 0) { kill_zombie(idx, by_turret); return 1; }
    return 0;
}

static void update_zombies(void)
{
    for (i32 i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &g_zom[i];
        if (!z->alive) continue;
        if (z->hurt > 0) z->hurt--;

        i32 zx = TO_PX(z->x), zy = TO_PX(z->y);
        i32 dxb = g_base_x - zx, dyb = g_base_y - zy;
        i32 distb = i_sqrt(dxb * dxb + dyb * dyb);
        if (distb < 1) distb = 1;

        const ZombieType *zt = &ZTYPE[z->type];
        i32 reach = BASE_R + zt->radius;

        if (distb <= reach) {
            /* At the wall: stop and gnaw the base on a cooldown. */
            if (z->atk_cd > 0) z->atk_cd--;
            else {
                g_base_hp -= zt->dmg;
                z->atk_cd = 28;
                g_base_flash = 5;
                spawn_burst(zx, zy, 5, COL_BASE_HI, 2);
                if (g_base_hp <= 0) { g_base_hp = 0; g_state = ST_GAMEOVER; }
            }
            continue;
        }

        /* Seek the base. Speed (halved while slowed), plus a tiny shamble. */
        i32 spd = z->speed;
        if (z->slow_ticks > 0) { spd = spd / 2; z->slow_ticks--; }
        i32 mvx = (dxb * spd) / distb;
        i32 mvy = (dyb * spd) / distb;

        /* Light separation: push away from a few nearby zombies so they swarm
         * without perfectly overlapping. Sample a sparse stride for speed. */
        i32 sepx = 0, sepy = 0;
        i32 sep_r = zt->radius + 7;
        for (i32 j = (i & 7); j < MAX_ZOMBIES; j += 8) {
            if (j == i || !g_zom[j].alive) continue;
            i32 ox = zx - TO_PX(g_zom[j].x);
            i32 oy = zy - TO_PX(g_zom[j].y);
            i32 d2 = ox * ox + oy * oy;
            if (d2 > 0 && d2 < sep_r * sep_r) {
                sepx += ox;
                sepy += oy;
            }
        }
        /* Scale separation into FP velocity units (weak nudge). */
        mvx += (sepx * (FP_ONE / 6)) >> 4;
        mvy += (sepy * (FP_ONE / 6)) >> 4;

        z->x += mvx;
        z->y += mvy;
        z->wob = (z->wob + 7) & 255;
    }
}

static void update_turrets(void)
{
    for (i32 i = 0; i < MAX_TURRETS; i++) {
        Turret *t = &g_tur[i];
        if (!t->alive) continue;
        if (t->cd > 0) t->cd--;
        i32 range = tur_range(t);
        i32 tgt = acquire_target(t->x, t->y, range);
        if (tgt >= 0) {
            i32 zx = TO_PX(g_zom[tgt].x), zy = TO_PX(g_zom[tgt].y);
            t->aim_x = zx; t->aim_y = zy;
            if (t->cd == 0) {
                const TurretType *tt = &TTYPE[t->type];
                spawn_proj(t->x, t->y, tgt, zx, zy,
                           tur_dmg(t), tur_splash(t), tt->slow,
                           tt->proj_speed, tt->barrel);
                t->cd = tur_fire_cd(t);
            }
        }
    }
}

static void update_projectiles(void)
{
    for (i32 i = 0; i < MAX_PROJ; i++) {
        Proj *pr = &g_proj[i];
        if (!pr->alive) continue;

        /* Light homing: steer toward the live target so fast zombies still get
         * hit. If the target died, the projectile flies straight and detonates
         * on proximity / expiry. */
        if (pr->target >= 0 && g_zom[pr->target].alive) {
            i32 zx = TO_PX(g_zom[pr->target].x), zy = TO_PX(g_zom[pr->target].y);
            i32 px = TO_PX(pr->x), py = TO_PX(pr->y);
            i32 dx = zx - px, dy = zy - py;
            i32 d = i_sqrt(dx * dx + dy * dy);
            if (d < 1) d = 1;
            i32 spd = i_sqrt((TO_PX(pr->vx)) * (TO_PX(pr->vx)) +
                             (TO_PX(pr->vy)) * (TO_PX(pr->vy)));
            if (spd < 8) spd = 8;
            pr->vx = (dx * TO_FP(spd)) / d;
            pr->vy = (dy * TO_FP(spd)) / d;
        }

        pr->x += pr->vx;
        pr->y += pr->vy;
        pr->ttl--;

        i32 px = TO_PX(pr->x), py = TO_PX(pr->y);
        if (px < -40 || px > WIN_W + 40 || py < -40 || py > WIN_H + 40 ||
            pr->ttl <= 0) {
            pr->alive = 0;
            continue;
        }

        /* Hit detection: find a zombie whose body the projectile is inside. */
        i32 hit = -1;
        for (i32 z = 0; z < MAX_ZOMBIES; z++) {
            if (!g_zom[z].alive) continue;
            i32 zx = TO_PX(g_zom[z].x), zy = TO_PX(g_zom[z].y);
            i32 dx = px - zx, dy = py - zy;
            i32 rad = ZTYPE[g_zom[z].type].radius + 3;
            if (dx * dx + dy * dy <= rad * rad) { hit = z; break; }
        }
        if (hit < 0) continue;

        if (pr->splash > 0) {
            /* Splash: damage all zombies near the impact point. */
            i32 r2 = pr->splash * pr->splash;
            spawn_burst(px, py, 12, 0xFFFFB347, 4);
            for (i32 z = 0; z < MAX_ZOMBIES; z++) {
                if (!g_zom[z].alive) continue;
                i32 zx = TO_PX(g_zom[z].x), zy = TO_PX(g_zom[z].y);
                i32 dx = px - zx, dy = py - zy;
                if (dx * dx + dy * dy <= r2)
                    damage_zombie(z, pr->dmg, pr->slow, -1);
            }
        } else {
            spawn_burst(px, py, 3, pr->col, 2);
            damage_zombie(hit, pr->dmg, pr->slow, -1);
        }
        pr->alive = 0;
    }
}

static void update_particles(void)
{
    for (i32 i = 0; i < MAX_PART; i++) {
        Particle *p = &g_part[i];
        if (!p->alive) continue;
        p->x += p->vx;
        p->y += p->vy;
        p->vy += 6;            /* gravity */
        p->ttl--;
        if (p->ttl <= 0) p->alive = 0;
    }
}

static void update_game(void)
{
    if (g_state == ST_GAMEOVER) {
        update_particles();   /* let the last burst settle */
        return;
    }

    if (g_base_flash > 0) g_base_flash--;

    /* ---- BUILD phase: auto-start countdown ---- */
    if (g_state == ST_BUILD) {
        if (g_build_timer > 0) g_build_timer--;
        else start_wave();
    }

    /* ---- spawn from the wave budget ---- */
    if (g_state == ST_WAVE && g_to_spawn > 0) {
        if (g_spawn_cd > 0) g_spawn_cd--;
        else {
            i32 type = roll_zombie_type(g_wave);
            i32 s = rand_range(g_spawn_n);
            spawn_zombie(type, g_wave, g_spawn_x[s], g_spawn_y[s]);
            g_to_spawn--;
            g_spawn_cd = g_spawn_gap;
        }
    }

    update_zombies();
    update_turrets();
    update_projectiles();
    update_particles();

    /* ---- wave clear: all spawned and none left alive ---- */
    if (g_state == ST_WAVE && g_to_spawn == 0 && g_zom_count == 0) {
        g_cash += 60 + g_wave * 20;          /* end-of-wave bonus */
        g_wave++;
        g_state = ST_BUILD;
        g_build_timer = 60 * 14;             /* ~14s build window */
        g_sel_placed = -1;
    }
}

/* ===================================================================== */
/* Placement / hit-testing                                               */
/* ===================================================================== */

static i32 in_field(i32 x, i32 y)
{
    return x >= 6 && x < WIN_W - 6 && y >= FIELD_Y + 6 && y < PALETTE_Y - 6;
}

/* Can a turret be placed at (x,y)? not on the base, not on a turret, in-field,
 * and not too close to a spawn point (so you can't fully wall them in). */
static i32 can_place(i32 x, i32 y)
{
    if (!in_field(x, y)) return 0;
    /* base clearance */
    i32 dxb = x - g_base_x, dyb = y - g_base_y;
    i32 clr = BASE_R + TURRET_R + 4;
    if (dxb * dxb + dyb * dyb < clr * clr) return 0;
    /* turret overlap */
    for (i32 i = 0; i < MAX_TURRETS; i++) {
        if (!g_tur[i].alive) continue;
        i32 dx = x - g_tur[i].x, dy = y - g_tur[i].y;
        i32 md = TURRET_R * 2 + 2;
        if (dx * dx + dy * dy < md * md) return 0;
    }
    return 1;
}

static void place_turret(i32 type, i32 x, i32 y)
{
    if (g_tur_count >= MAX_TURRETS) return;
    if (g_cash < TTYPE[type].cost) return;
    if (!can_place(x, y)) return;
    for (i32 i = 0; i < MAX_TURRETS; i++) {
        if (!g_tur[i].alive) {
            g_tur[i].alive = 1;
            g_tur[i].type = type;
            g_tur[i].level = 1;
            g_tur[i].x = x;
            g_tur[i].y = y;
            g_tur[i].cd = 0;
            g_tur[i].invested = TTYPE[type].cost;
            g_tur[i].aim_x = x;
            g_tur[i].aim_y = y - 30;
            g_tur[i].kills = 0;
            g_tur_count++;
            g_cash -= TTYPE[type].cost;
            return;
        }
    }
}

static void upgrade_turret(i32 idx)
{
    if (idx < 0 || idx >= MAX_TURRETS || !g_tur[idx].alive) return;
    Turret *t = &g_tur[idx];
    if (t->level >= MAX_LEVEL) return;
    i32 cost = tur_upgrade_cost(t);
    if (g_cash < cost) return;
    g_cash -= cost;
    t->invested += cost;
    t->level++;
}

static void sell_turret(i32 idx)
{
    if (idx < 0 || idx >= MAX_TURRETS || !g_tur[idx].alive) return;
    Turret *t = &g_tur[idx];
    g_cash += tur_sell_value(t);
    spawn_burst(t->x, t->y, 10, COL_GREY, 3);
    t->alive = 0;
    g_tur_count--;
    g_sel_placed = -1;
}

/* Find a placed turret under the cursor. */
static i32 turret_under(i32 x, i32 y)
{
    for (i32 i = 0; i < MAX_TURRETS; i++) {
        if (!g_tur[i].alive) continue;
        i32 dx = x - g_tur[i].x, dy = y - g_tur[i].y;
        if (dx * dx + dy * dy <= (TURRET_R + 2) * (TURRET_R + 2)) return i;
    }
    return -1;
}

/* ---- palette / button rectangles ---- */
#define PAL_SLOT_W 124
#define PAL_SLOT_X0 10
#define PAL_SLOT_GAP 6
static void pal_slot_rect(i32 i, i32 *x, i32 *y, i32 *w, i32 *h)
{
    *x = PAL_SLOT_X0 + i * (PAL_SLOT_W + PAL_SLOT_GAP);
    *y = PALETTE_Y + 8;
    *w = PAL_SLOT_W;
    *h = PALETTE_H - 16;
}
static void startbtn_rect(i32 *x, i32 *y, i32 *w, i32 *h)
{
    *w = 132; *h = PALETTE_H - 16;
    *x = WIN_W - *w - 10;
    *y = PALETTE_Y + 8;
}
static i32 point_in(i32 px, i32 py, i32 x, i32 y, i32 w, i32 h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Inspector panel (appears when a placed turret is selected). Anchored near
 * the turret but kept on-screen. UPGRADE button = top, SELL = bottom. */
#define PANEL_W 168
#define PANEL_H 96
static void panel_rect(i32 idx, i32 *x, i32 *y)
{
    i32 px = g_tur[idx].x + TURRET_R + 8;
    i32 py = g_tur[idx].y - PANEL_H / 2;
    if (px + PANEL_W > WIN_W - 6) px = g_tur[idx].x - TURRET_R - 8 - PANEL_W;
    if (px < 6) px = 6;
    if (py < FIELD_Y + 4) py = FIELD_Y + 4;
    if (py + PANEL_H > PALETTE_Y - 4) py = PALETTE_Y - 4 - PANEL_H;
    *x = px; *y = py;
}
static void panel_upgrade_rect(i32 idx, i32 *x, i32 *y, i32 *w, i32 *h)
{
    i32 px, py; panel_rect(idx, &px, &py);
    *x = px + 8; *y = py + 44; *w = PANEL_W - 16; *h = 20;
}
static void panel_sell_rect(i32 idx, i32 *x, i32 *y, i32 *w, i32 *h)
{
    i32 px, py; panel_rect(idx, &px, &py);
    *x = px + 8; *y = py + 68; *w = PANEL_W - 16; *h = 20;
}

/* ===================================================================== */
/* Rendering                                                             */
/* ===================================================================== */

static void draw_base(Canvas *c)
{
    i32 bx = g_base_x, by = g_base_y;
    /* shadow ring + walls */
    fill_circle(c, bx, by, BASE_R + 4, 0xFF101A10);
    u32 body = (g_base_flash > 0) ? 0xFFE56A6A : COL_BASE;
    fill_circle(c, bx, by, BASE_R, body);
    fill_circle(c, bx, by, BASE_R - 6, 0xFF2B527A);
    /* a little fortress: crenellated inner block */
    fill_rect(c, bx - 20, by - 20, 40, 40, COL_BASE_HI);
    fill_rect(c, bx - 14, by - 14, 28, 28, 0xFF223A55);
    /* corner battlement nubs */
    fill_rect(c, bx - 26, by - 26, 8, 8, COL_BASE_HI);
    fill_rect(c, bx + 18, by - 26, 8, 8, COL_BASE_HI);
    fill_rect(c, bx - 26, by + 18, 8, 8, COL_BASE_HI);
    fill_rect(c, bx + 18, by + 18, 8, 8, COL_BASE_HI);
    /* "HQ" mark */
    draw_text_centered(c, bx, by - FONT_H / 2, "HQ", COL_WHITE);
    /* base HP bar floating above */
    draw_bar(c, bx - 40, by - BASE_R - 14, 80, 7, g_base_hp, BASE_HP_MAX,
             COL_GREEN, 0xFF402020);
}

static void draw_zombie(Canvas *c, Zombie *z)
{
    const ZombieType *zt = &ZTYPE[z->type];
    i32 zx = TO_PX(z->x), zy = TO_PX(z->y);
    i32 r = zt->radius;
    /* shamble wobble: tiny vertical bob via the wob phase (no trig needed). */
    i32 bob = ((z->wob & 64) ? 1 : 0) - ((z->wob & 128) ? 1 : 0);
    zy += bob;

    u32 body = (z->hurt > 0) ? 0xFFFFFFFF : zt->col;
    /* drop shadow */
    fill_circle(c, zx, zy + r - 1, r - 2, 0x40000000 | (COL_BLACK & 0xFFFFFF));
    fill_circle(c, zx, zy, r, body);
    fill_circle(c, zx, zy, r - 3, zt->col2);
    /* eyes */
    fill_circle(c, zx - r / 3, zy - r / 4, 2, COL_RED);
    fill_circle(c, zx + r / 3, zy - r / 4, 2, COL_RED);
    /* slow shimmer ring */
    if (z->slow_ticks > 0) circle_outline(c, zx, zy, r + 1, 0xFFAEE8FF);
    /* hp bar above (only if damaged) */
    if (z->hp < z->hp_max)
        draw_bar(c, zx - r, zy - r - 6, r * 2, 3, z->hp, z->hp_max,
                 COL_GREEN, 0xFF3A1414);
}

static void draw_turret(Canvas *c, Turret *t)
{
    const TurretType *tt = &TTYPE[t->type];
    /* base */
    fill_circle(c, t->x, t->y, TURRET_R, 0xFF1A2630);
    fill_circle(c, t->x, t->y, TURRET_R - 3, tt->col);
    /* barrel toward aim */
    i32 dx = t->aim_x - t->x, dy = t->aim_y - t->y;
    i32 d = i_sqrt(dx * dx + dy * dy);
    if (d < 1) { dx = 0; dy = -1; d = 1; }
    i32 bx = t->x + (dx * (TURRET_R + 9)) / d;
    i32 by = t->y + (dy * (TURRET_R + 9)) / d;
    draw_line_thick(c, t->x, t->y, bx, by, 6, tt->barrel);
    fill_circle(c, bx, by, 3, tt->barrel);
    /* center hub */
    fill_circle(c, t->x, t->y, 4, 0xFF0C1218);
    /* level pips (small dots around the top) */
    for (i32 i = 0; i < t->level; i++)
        fill_circle(c, t->x - 8 + i * 4, t->y + TURRET_R - 3, 1, COL_YELLOW);
}

static void draw_field(Canvas *c)
{
    fill_rect(c, 0, FIELD_Y, WIN_W, FIELD_H, COL_GROUND);
    for (i32 yy = FIELD_Y; yy < PALETTE_Y; yy += 48) {
        for (i32 xx = ((yy / 48) & 1) ? 48 : 0; xx < WIN_W; xx += 96)
            fill_rect(c, xx, yy, 48, 48, COL_GROUND2);
    }
    /* spawn markers (subtle red gates at the edges) */
    for (i32 i = 0; i < g_spawn_n; i++) {
        fill_circle(c, g_spawn_x[i], g_spawn_y[i], 6, 0x80000000 | 0x551010);
        circle_outline(c, g_spawn_x[i], g_spawn_y[i], 8, COL_BLOOD);
    }
}

static void draw_hud(Canvas *c)
{
    fill_rect(c, 0, 0, WIN_W, HUD_H, COL_HUD_BG);
    char line[96]; i32 n;

    n = 0; str_append(line, &n, "CASH $"); num_append(line, &n, (u32)g_cash);
    draw_text(c, 12, 14, line, COL_YELLOW);

    n = 0; str_append(line, &n, "WAVE "); num_append(line, &n, (u32)g_wave);
    draw_text(c, 200, 14, line, COL_WHITE);

    n = 0; str_append(line, &n, "KILLS "); num_append(line, &n, (u32)g_kills);
    draw_text(c, 330, 14, line, COL_GREEN);

    /* score = waves*100 + kills */
    n = 0; str_append(line, &n, "SCORE ");
    num_append(line, &n, (u32)((g_wave - 1) * 100 + g_kills));
    draw_text(c, 470, 14, line, COL_WHITE);

    /* base HP readout + bar in the HUD */
    n = 0; str_append(line, &n, "BASE ");
    num_append(line, &n, (u32)g_base_hp);
    draw_text(c, 640, 14, line, COL_RED);
    draw_bar(c, 760, 16, 180, 12, g_base_hp, BASE_HP_MAX, COL_GREEN, 0xFF402020);

    /* state / countdown hint */
    if (g_state == ST_BUILD) {
        char h[48]; i32 hn = 0;
        str_append(h, &hn, "BUILD - SPACE to start (");
        num_append(h, &hn, (u32)((g_build_timer / 60) + 1));
        str_append(h, &hn, "s)");
        draw_text_centered(c, WIN_W / 2, 28, h, 0xFFBFE8C8);
    }
}

static void draw_palette(Canvas *c)
{
    fill_rect(c, 0, PALETTE_Y, WIN_W, PALETTE_H, COL_PAL_BG);
    fill_rect(c, 0, PALETTE_Y, WIN_W, 2, COL_DKGREY);

    for (i32 i = 0; i < TU_COUNT; i++) {
        i32 x, y, w, h; pal_slot_rect(i, &x, &y, &w, &h);
        const TurretType *tt = &TTYPE[i];
        i32 afford = g_cash >= tt->cost;
        u32 frame = (g_sel_turret == i) ? COL_SEL : COL_DKGREY;
        fill_rect(c, x, y, w, h, 0xFF18283A);
        fill_rect(c, x, y, w, 2, frame);
        fill_rect(c, x, y + h - 2, w, 2, frame);
        fill_rect(c, x, y, 2, h, frame);
        fill_rect(c, x + w - 2, y, 2, h, frame);

        /* icon */
        i32 icx = x + 22, icy = y + h / 2;
        fill_circle(c, icx, icy, 12, afford ? tt->col : COL_DKGREY);
        fill_circle(c, icx, icy, 9, afford ? 0xFF1A2630 : 0xFF2A3340);
        draw_line_thick(c, icx, icy, icx, icy - 15, 5,
                        afford ? tt->barrel : COL_GREY);

        /* name + cost */
        u32 tcol = afford ? COL_WHITE : COL_GREY;
        char nm[24]; i32 nn = 0;
        str_append(nm, &nn, "["); num_append(nm, &nn, (u32)(i + 1));
        str_append(nm, &nn, "] "); str_append(nm, &nn, tt->name);
        draw_text(c, x + 40, y + 8, nm, tcol);
        char cost[24]; i32 cn = 0;
        str_append(cost, &cn, "$"); num_append(cost, &cn, (u32)tt->cost);
        draw_text(c, x + 40, y + 30, cost, afford ? COL_YELLOW : COL_GREY);
        /* quick stat line */
        char st[24]; i32 sn = 0;
        if (tt->splash > 0)      str_append(st, &sn, "splash");
        else if (tt->slow > 0)   str_append(st, &sn, "slows");
        else { str_append(st, &sn, "dmg "); num_append(st, &sn, (u32)tt->dmg); }
        draw_text(c, x + 40, y + 50, st, COL_GREY);
    }

    /* Start Wave button */
    {
        i32 x, y, w, h; startbtn_rect(&x, &y, &w, &h);
        i32 hot = point_in(g_mx, g_my, x, y, w, h);
        u32 bg;
        const char *label;
        if (g_state == ST_GAMEOVER) { bg = COL_DKGREY; label = "PRESS R"; }
        else if (g_state == ST_WAVE) {
            bg = COL_DKGREY; label = "FIGHTING";
        } else {
            bg = hot ? COL_BTN_HI : COL_BTN; label = "START WAVE";
        }
        fill_rect(c, x, y, w, h, bg);
        draw_text_centered(c, x + w / 2, y + 14, label, COL_WHITE);
        if (g_state == ST_WAVE) {
            char l[24]; i32 ln = 0;
            str_append(l, &ln, "left: ");
            num_append(l, &ln, (u32)(g_to_spawn + g_zom_count));
            draw_text_centered(c, x + w / 2, y + 38, l, COL_GREY);
        }
    }
}

static void draw_panel(Canvas *c)
{
    if (g_sel_placed < 0 || g_sel_placed >= MAX_TURRETS) return;
    if (!g_tur[g_sel_placed].alive) { g_sel_placed = -1; return; }
    Turret *t = &g_tur[g_sel_placed];
    const TurretType *tt = &TTYPE[t->type];

    i32 px, py; panel_rect(g_sel_placed, &px, &py);
    /* highlight the selected turret's range */
    circle_outline(c, t->x, t->y, tur_range(t), 0xFFFFFFFF);

    fill_rect(c, px, py, PANEL_W, PANEL_H, 0xFF0B1626);
    fill_rect(c, px, py, PANEL_W, 2, COL_SEL);
    fill_rect(c, px, py + PANEL_H - 2, PANEL_W, 2, COL_SEL);
    fill_rect(c, px, py, 2, PANEL_H, COL_SEL);
    fill_rect(c, px + PANEL_W - 2, py, 2, PANEL_H, COL_SEL);

    /* title: name + level */
    char ttl[32]; i32 tn = 0;
    str_append(ttl, &tn, tt->name);
    str_append(ttl, &tn, "  L");
    num_append(ttl, &tn, (u32)t->level);
    draw_text(c, px + 8, py + 6, ttl, COL_WHITE);
    /* stats */
    char st[32]; i32 sn = 0;
    str_append(st, &sn, "dmg "); num_append(st, &sn, (u32)tur_dmg(t));
    str_append(st, &sn, " rng "); num_append(st, &sn, (u32)tur_range(t));
    draw_text(c, px + 8, py + 24, st, COL_GREY);

    /* upgrade button */
    {
        i32 bx, by, bw, bh; panel_upgrade_rect(g_sel_placed, &bx, &by, &bw, &bh);
        i32 maxed = (t->level >= MAX_LEVEL);
        i32 cost = tur_upgrade_cost(t);
        i32 hot = point_in(g_mx, g_my, bx, by, bw, bh);
        i32 afford = g_cash >= cost;
        u32 bg = maxed ? COL_DKGREY
                       : (afford ? (hot ? COL_BTN_HI : COL_BTN) : 0xFF334155);
        fill_rect(c, bx, by, bw, bh, bg);
        char l[28]; i32 ln = 0;
        if (maxed) str_append(l, &ln, "MAX LEVEL");
        else { str_append(l, &ln, "UPGRADE $"); num_append(l, &ln, (u32)cost); }
        draw_text_centered(c, bx + bw / 2, by + 2, l, COL_WHITE);
    }
    /* sell button */
    {
        i32 bx, by, bw, bh; panel_sell_rect(g_sel_placed, &bx, &by, &bw, &bh);
        i32 hot = point_in(g_mx, g_my, bx, by, bw, bh);
        fill_rect(c, bx, by, bw, bh, hot ? COL_BTN_REDHI : COL_BTN_RED);
        char l[28]; i32 ln = 0;
        str_append(l, &ln, "SELL $"); num_append(l, &ln, (u32)tur_sell_value(t));
        draw_text_centered(c, bx + bw / 2, by + 2, l, COL_WHITE);
    }
}

static void render(Canvas *c)
{
    draw_field(c);

    /* hover range for a placed turret when nothing is being placed/inspected */
    if (g_sel_turret < 0 && g_sel_placed < 0 && in_field(g_mx, g_my)) {
        i32 ht = turret_under(g_mx, g_my);
        if (ht >= 0)
            circle_outline(c, g_tur[ht].x, g_tur[ht].y,
                           tur_range(&g_tur[ht]), 0xFFFFFFFF);
    }

    /* turrets */
    for (i32 i = 0; i < MAX_TURRETS; i++)
        if (g_tur[i].alive) draw_turret(c, &g_tur[i]);

    /* the base */
    draw_base(c);

    /* zombies */
    for (i32 i = 0; i < MAX_ZOMBIES; i++)
        if (g_zom[i].alive) draw_zombie(c, &g_zom[i]);

    /* projectiles */
    for (i32 i = 0; i < MAX_PROJ; i++) {
        if (!g_proj[i].alive) continue;
        i32 px = TO_PX(g_proj[i].x), py = TO_PX(g_proj[i].y);
        fill_circle(c, px, py, 3, g_proj[i].col);
        put_px(c, px, py, COL_WHITE);
    }

    /* particles */
    for (i32 i = 0; i < MAX_PART; i++) {
        if (!g_part[i].alive) continue;
        i32 px = TO_PX(g_part[i].x), py = TO_PX(g_part[i].y);
        fill_circle(c, px, py, g_part[i].r, g_part[i].col);
    }

    /* placement preview */
    if (g_sel_turret >= 0 && in_field(g_mx, g_my)) {
        const TurretType *tt = &TTYPE[g_sel_turret];
        i32 ok = can_place(g_mx, g_my) && g_cash >= tt->cost;
        u32 ring = ok ? 0xFFFFFFFF : COL_RED;
        circle_outline(c, g_mx, g_my, tt->range, ring);
        fill_circle(c, g_mx, g_my, TURRET_R - 4, ok ? 0xFF66CC88 : 0xFFCC5566);
        fill_circle(c, g_mx, g_my, TURRET_R - 8, tt->col);
    }

    /* inspector panel for a selected placed turret */
    draw_panel(c);

    draw_hud(c);
    draw_palette(c);

    /* center help line just above the palette */
    if (g_state != ST_GAMEOVER)
        draw_text_centered(c, WIN_W / 2, PALETTE_Y - 18,
            "Pick a turret (1-5) & click to build. Click a turret to upgrade/sell.",
            0xFF9FB8CF);

    /* GAME OVER overlay */
    if (g_state == ST_GAMEOVER) {
        for (i32 y = FIELD_Y; y < PALETTE_Y; y += 2)
            fill_rect(c, 0, y, WIN_W, 1, COL_BLACK);
        i32 cy = FIELD_Y + FIELD_H / 2;
        draw_text_centered(c, WIN_W / 2, cy - 44, "THE BASE HAS FALLEN", COL_RED);
        char l[64]; i32 n = 0;
        str_append(l, &n, "Survived to wave ");
        num_append(l, &n, (u32)g_wave);
        draw_text_centered(c, WIN_W / 2, cy - 10, l, COL_WHITE);
        n = 0;
        str_append(l, &n, "Kills ");  num_append(l, &n, (u32)g_kills);
        str_append(l, &n, "   Score "); num_append(l, &n,
            (u32)((g_wave - 1) * 100 + g_kills));
        draw_text_centered(c, WIN_W / 2, cy + 14, l, COL_YELLOW);
        draw_text_centered(c, WIN_W / 2, cy + 44, "Press R to defend again", COL_GREEN);
    }
}

/* ===================================================================== */
/* Input handling                                                        */
/* ===================================================================== */

static void handle_click(i32 x, i32 y)
{
    if (g_state == ST_GAMEOVER) {
        u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        reset_game(now);
        return;
    }

    /* 1) inspector-panel buttons take priority while a turret is selected */
    if (g_sel_placed >= 0 && g_sel_placed < MAX_TURRETS && g_tur[g_sel_placed].alive) {
        i32 bx, by, bw, bh;
        panel_upgrade_rect(g_sel_placed, &bx, &by, &bw, &bh);
        if (point_in(x, y, bx, by, bw, bh)) { upgrade_turret(g_sel_placed); return; }
        panel_sell_rect(g_sel_placed, &bx, &by, &bw, &bh);
        if (point_in(x, y, bx, by, bw, bh)) { sell_turret(g_sel_placed); return; }
    }

    /* 2) palette slots */
    for (i32 i = 0; i < TU_COUNT; i++) {
        i32 rx, ry, rw, rh; pal_slot_rect(i, &rx, &ry, &rw, &rh);
        if (point_in(x, y, rx, ry, rw, rh)) {
            g_sel_turret = (g_sel_turret == i) ? -1 : i;
            g_sel_placed = -1;
            return;
        }
    }

    /* 3) start-wave button */
    {
        i32 rx, ry, rw, rh; startbtn_rect(&rx, &ry, &rw, &rh);
        if (point_in(x, y, rx, ry, rw, rh)) {
            if (g_state == ST_BUILD) start_wave();
            return;
        }
    }

    /* 4) field interaction */
    if (in_field(x, y)) {
        if (g_sel_turret >= 0) {
            /* placing a new turret */
            if (g_cash >= TTYPE[g_sel_turret].cost && can_place(x, y)) {
                place_turret(g_sel_turret, x, y);
                if (g_cash < TTYPE[g_sel_turret].cost) g_sel_turret = -1;
            }
            return;
        }
        /* selecting a placed turret to inspect/upgrade/sell */
        i32 ht = turret_under(x, y);
        g_sel_placed = (ht == g_sel_placed) ? -1 : ht;
        return;
    }

    /* clicked empty chrome: clear selections */
    g_sel_placed = -1;
}

static void handle_key(i32 code)
{
    switch (code) {
    case KEY_1: g_sel_turret = (g_sel_turret == 0) ? -1 : 0; g_sel_placed = -1; break;
    case KEY_2: g_sel_turret = (g_sel_turret == 1) ? -1 : 1; g_sel_placed = -1; break;
    case KEY_3: g_sel_turret = (g_sel_turret == 2) ? -1 : 2; g_sel_placed = -1; break;
    case KEY_4: g_sel_turret = (g_sel_turret == 3) ? -1 : 3; g_sel_placed = -1; break;
    case KEY_5: g_sel_turret = (g_sel_turret == 4) ? -1 : 4; g_sel_placed = -1; break;
    case KEY_U:
        if (g_sel_placed >= 0) upgrade_turret(g_sel_placed);
        break;
    case KEY_S:
        if (g_sel_placed >= 0) sell_turret(g_sel_placed);
        break;
    case KEY_SPACE:
    case KEY_ENTER:
        if (g_state == ST_BUILD) start_wave();
        else if (g_state == ST_GAMEOVER) {
            u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            reset_game(now);
        }
        break;
    case KEY_R:
        if (g_state == ST_GAMEOVER) {
            u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            reset_game(now);
        }
        break;
    case KEY_ESC:
    case KEY_Q:
        print("[ZOMBIETD] exit\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
        break;
    }
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

void _start(void)
{
    print("[ZOMBIETD] starting\n");

    if (wl_connect() != 0) {
        print("[ZOMBIETD] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Zombie Bastion");
    if (!win) {
        print("[ZOMBIETD] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    Canvas cv;
    cv.buf = win->pixels;
    cv.stride = (i32)(win->stride / 4u);
    cv.w = (i32)win->w;
    cv.h = (i32)win->h;

    build_spawns();

    u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    reset_game(now);

    g_mx = WIN_W / 2;
    g_my = FIELD_Y + FIELD_H / 2;

    u64 last_tick = now;
    const u64 TICK_MS = 16;   /* ~60 logic ticks/sec */
    i32 prev_btn = 0;

    for (;;) {
        u64 t = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);

        /* ---- drain input ---- */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                g_mx = ea;
                g_my = eb;
                i32 btn = (ec & 1);
                if (btn && !prev_btn) handle_click(g_mx, g_my);  /* rising edge */
                prev_btn = btn;
            } else if (kind == WL_EVENT_KEY) {
                if (eb == 1) handle_key(ea);   /* eb = pressed */
            }
        }

        /* ---- fixed-step update with bounded catch-up ---- */
        if ((t - last_tick) >= TICK_MS) {
            i32 steps = (i32)((t - last_tick) / TICK_MS);
            if (steps > 4) steps = 4;
            for (i32 s = 0; s < steps; s++) update_game();
            last_tick = t;
        }

        render(&cv);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
