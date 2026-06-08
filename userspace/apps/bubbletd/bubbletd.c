/*
 * bubbletd.c -- "Bubble Defense" (freestanding, ring 3).
 * ======================================================
 *
 * An original tower-defense game (same genre as balloon tower defense, but
 * with wholly original names/art): colored "bubbles" travel a winding path,
 * you place auto-firing towers that pop them, popped bubbles SPLIT into two
 * weaker ones, you earn cash and survive 18 waves.
 *
 * Towers:
 *   - Pin    (cost 100): long range, fast fire, 1 dmg, single target.
 *   - Boomer (cost 220): med range, slow fire, 2 dmg, SPLASH area damage.
 *   - Froster(cost 160): med range, med fire,  1 dmg, SLOWS hit bubbles.
 *
 * Enemy bubbles: tiers 1..5 (red/blue/green/yellow/purple), higher tier =
 * more health / faster / more reward / more lives lost on leak. Popping a
 * tier>1 bubble spawns two tier-1 bubbles at its position.
 *
 * Mouse-first controls (mirrors the genre):
 *   - LEFT CLICK a palette tower -> select that type (preview range follows
 *     the cursor over the field).
 *   - LEFT CLICK a valid empty field spot -> place tower, deduct cash.
 *   - LEFT CLICK "Start Wave" button (or Space/Enter) -> launch next wave.
 *   - Keys 1/2/3 select tower types; 'r' restart after game over/victory;
 *     Esc / 'q' quit.
 *   - Hover a placed tower -> shows its range circle.
 *
 * No libc/malloc/stdio/libm: pure inline syscalls + wl_client + bitfont,
 * static buffers, integer/fixed-point math only.
 *
 * Build (wl-direct app = app + wl_client + bitfont):
 *   gcc <freestanding flags> -c userspace/apps/bubbletd/bubbletd.c -o btd.o
 *   gcc <freestanding flags> -c userspace/lib/wl/wl_client.c        -o wlc.o
 *   gcc <freestanding flags> -c userspace/lib/font/bitfont.c        -o bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       btd.o wlc.o bf.o -o bubbletd.elf
 *   objdump -d bubbletd.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/*
 * Defensively disable the stack-protector for this whole translation unit.
 * The real build uses the freestanding x86_64-elf cross-compiler (see
 * docs/TOOLCHAIN.md) where -fno-stack-protector already yields canary-free
 * code. Some HOST gccs (e.g. Arch's, built --enable-default-ssp) ignore the
 * command-line -fno-stack-protector for functions with local arrays; this
 * pragma forces it off regardless, so the linked ELF stays free of fs:0x28
 * canary references as required by freestanding ring-3 apps.
 */
#pragma GCC optimize ("no-stack-protector")

/* ---- syscall numbers ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- key codes (from kernel/include/input.h) ---- */
#define KEY_ESC     1
#define KEY_1       2
#define KEY_2       3
#define KEY_3       4
#define KEY_Q       16
#define KEY_R       19
#define KEY_SPACE   57
#define KEY_ENTER   28

/* ---- types ---- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;

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
/* gcc -O2 loop-idiom recognition rewrites length loops into a call to the libc
 * `strlen`; provide one (with that pass disabled so it cannot recurse into
 * itself) to satisfy the freestanding link. */
__attribute__((used, optimize("no-tree-loop-distribute-patterns")))
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }

/* Format u32 into buf (NUL-terminated), return length.
 * tmp[] is sized for a full 64-bit value (20 digits) with margin so it can
 * never be overrun even if a wider value is ever passed in; the digit count
 * for a u32 is at most 10. */
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

/* Append src to dst at *pos, advancing *pos. */
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

/* ===================================================================== */
/* Window / layout                                                       */
/* ===================================================================== */

#define WIN_W   900
#define WIN_H   660

#define HUD_H        40          /* top HUD bar height                  */
#define PALETTE_H    72          /* bottom palette bar height           */
#define FIELD_Y      HUD_H
#define FIELD_H      (WIN_H - HUD_H - PALETTE_H)   /* playfield height   */
#define PALETTE_Y    (WIN_H - PALETTE_H)

/* ---- colours (ARGB32, 0xAARRGGBB) ---- */
#define COL_GRASS     0xFF2F6B3A
#define COL_GRASS2    0xFF35763F   /* checker accent              */
#define COL_PATH      0xFF8A6A3A   /* path band (brown dirt)      */
#define COL_PATH_EDGE 0xFF6E5128
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
#define COL_SEL       0xFFFFD60A
#define COL_RANGE     0x66FFFFFF   /* (alpha unused by writer; drawn as outline) */

/* tier colours: index 0 unused, 1..5 */
static const u32 TIER_COL[6] = {
    0xFF000000,
    0xFFE23B3B,   /* 1 red    */
    0xFF3B7BE2,   /* 2 blue   */
    0xFF3BC55A,   /* 3 green  */
    0xFFE2C13B,   /* 4 yellow */
    0xFFA24BE2,   /* 5 purple */
};

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
     * values and are rejected too. This is the lowest-level pixel write; every
     * higher-level primitive ultimately relies on (or mirrors) this clamp so
     * NOTHING is ever written outside the [0,w) x [0,h) window framebuffer. */
    if ((u32)x >= (u32)c->w || (u32)y >= (u32)c->h) return;
    c->buf[(u32)y * c->stride + (u32)x] = color;
}

/* Midpoint filled circle. Every horizontal span is clamped to [0,w) and every
 * row to [0,h) so a center far off-screen (e.g. range preview chasing the
 * cursor past an edge) can never write outside the framebuffer. */
static void fill_circle(Canvas *c, i32 cx, i32 cy, i32 r, u32 color)
{
    if (r <= 0) { put_px(c, cx, cy, color); return; }
    i32 r2 = r * r;
    /* clamp the vertical iteration range so dy never visits off-buffer rows */
    i32 dy0 = -r; if (cy + dy0 < 0)        dy0 = -cy;
    i32 dy1 =  r; if (cy + dy1 > c->h - 1) dy1 = (c->h - 1) - cy;
    for (i32 dy = dy0; dy <= dy1; dy++) {
        i32 yy = cy + dy;
        if (yy < 0 || yy >= c->h) continue;   /* belt-and-suspenders */
        i32 span = i_sqrt(r2 - dy * dy);
        i32 x1 = cx - span; if (x1 < 0) x1 = 0; if (x1 > c->w - 1) continue;
        i32 x2 = cx + span; if (x2 >= c->w) x2 = c->w - 1; if (x2 < 0) continue;
        if (x1 > x2) continue;
        u32 *row = c->buf + (u32)yy * c->stride;
        for (i32 x = x1; x <= x2; x++) row[x] = color;
    }
}

/* Circle outline (range preview). thick = ring thickness in px. */
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

/* Bresenham line, drawn with a small thickness for visibility. */
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

/* Draw a string centered horizontally at (cx, y). */
static void draw_text_centered(Canvas *c, i32 cx, i32 y, const char *s, u32 color)
{
    i32 w = font_text_width(s);
    font_draw_string(c->buf, c->stride, c->w, c->h, cx - w / 2, y, s, color);
}
static void draw_text(Canvas *c, i32 x, i32 y, const char *s, u32 color)
{
    font_draw_string(c->buf, c->stride, c->w, c->h, x, y, s, color);
}

/* ===================================================================== */
/* Fixed-point: positions in 1/FP_ONE pixel units                        */
/* ===================================================================== */

#define FP_SHIFT  8
#define FP_ONE    (1 << FP_SHIFT)
#define TO_FP(px) ((px) << FP_SHIFT)
#define TO_PX(fp) ((fp) >> FP_SHIFT)

/* ===================================================================== */
/* Path                                                                  */
/* ===================================================================== */

#define MAX_WAYPOINTS 10
typedef struct { i32 x, y; } Pt;

/* Winding S/zig-zag path across the field (pixel coords).               */
static Pt g_path[MAX_WAYPOINTS];
static i32 g_path_n = 0;

/* cumulative length (in pixels) up to each waypoint */
static i32 g_seg_len[MAX_WAYPOINTS];   /* length of segment i..i+1 */
static i32 g_total_len = 0;

#define PATH_HALF 18    /* path band half-width in px */

static void build_path(void)
{
    /* y-band of field: FIELD_Y .. FIELD_Y+FIELD_H */
    i32 top = FIELD_Y + 40;
    i32 bot = FIELD_Y + FIELD_H - 40;
    i32 midy = (top + bot) / 2;
    (void)midy;

    i32 i = 0;
    g_path[i].x = -20;        g_path[i].y = top + 20;          i++;  /* enter left */
    g_path[i].x = 230;        g_path[i].y = top + 20;          i++;
    g_path[i].x = 230;        g_path[i].y = bot - 30;          i++;
    g_path[i].x = 470;        g_path[i].y = bot - 30;          i++;
    g_path[i].x = 470;        g_path[i].y = top + 30;          i++;
    g_path[i].x = 690;        g_path[i].y = top + 30;          i++;
    g_path[i].x = 690;        g_path[i].y = bot - 20;          i++;
    g_path[i].x = WIN_W + 20; g_path[i].y = bot - 20;          i++;  /* exit right */
    g_path_n = i;

    g_total_len = 0;
    for (i32 s = 0; s < g_path_n - 1; s++) {
        i32 dx = g_path[s + 1].x - g_path[s].x;
        i32 dy = g_path[s + 1].y - g_path[s].y;
        i32 L = i_sqrt(dx * dx + dy * dy);
        if (L < 1) L = 1;
        g_seg_len[s] = L;
        g_total_len += L;
    }
}

/* Convert a distance-along-path (px) to a pixel point. */
static Pt path_point(i32 dist)
{
    Pt p;
    if (dist <= 0) { return g_path[0]; }
    i32 acc = 0;
    for (i32 s = 0; s < g_path_n - 1; s++) {
        i32 L = g_seg_len[s];
        if (dist <= acc + L) {
            i32 into = dist - acc;
            i32 dx = g_path[s + 1].x - g_path[s].x;
            i32 dy = g_path[s + 1].y - g_path[s].y;
            p.x = g_path[s].x + (dx * into) / L;
            p.y = g_path[s].y + (dy * into) / L;
            return p;
        }
        acc += L;
    }
    return g_path[g_path_n - 1];
}

/* Distance (squared) from a point to the path polyline (for placement test). */
static i32 dist2_to_path(i32 px, i32 py)
{
    i32 best = 0x7FFFFFFF;
    for (i32 s = 0; s < g_path_n - 1; s++) {
        i32 ax = g_path[s].x, ay = g_path[s].y;
        i32 bx = g_path[s + 1].x, by = g_path[s + 1].y;
        i32 vx = bx - ax, vy = by - ay;
        i32 wx = px - ax, wy = py - ay;
        i32 vv = vx * vx + vy * vy;
        i32 t = 0;
        if (vv > 0) {
            /* clamp projection param to [0,1], scaled by 1024 */
            i32 dot = wx * vx + wy * vy;
            t = (dot * 1024) / vv;
            if (t < 0) t = 0;
            if (t > 1024) t = 1024;
        }
        i32 cx = ax + (vx * t) / 1024;
        i32 cy = ay + (vy * t) / 1024;
        i32 ddx = px - cx, ddy = py - cy;
        i32 d2 = ddx * ddx + ddy * ddy;
        if (d2 < best) best = d2;
    }
    return best;
}

/* ===================================================================== */
/* Enemies (bubbles)                                                     */
/* ===================================================================== */

#define MAX_ENEMIES 256

typedef struct {
    i32 alive;
    i32 tier;        /* 1..5 */
    i32 hp;
    i32 dist;        /* distance along path, in fixed-point px (FP) */
    i32 speed;       /* base px-per-tick (FP)                       */
    i32 slow_ticks;  /* remaining ticks of slow effect              */
} Enemy;

static Enemy g_enemy[MAX_ENEMIES];
static i32   g_enemy_count = 0;   /* current live count (for fast checks) */

/* per-tier stats (bounds-guarded: t outside [0,5] returns 0, never an OOB read) */
static i32 tier_hp(i32 t)    { static const i32 v[6] = {0,1,2,3,5,8};   return ((unsigned)t < 6) ? v[t] : 0; }
static i32 tier_speed(i32 t) { static const i32 v[6] = {0,80,110,140,170,210}; return ((unsigned)t < 6) ? v[t] : 0; } /* FP/tick-ish */
static i32 tier_reward(i32 t){ static const i32 v[6] = {0,2,3,5,8,12};  return ((unsigned)t < 6) ? v[t] : 0; }
static i32 tier_lives(i32 t) { static const i32 v[6] = {0,1,1,2,3,4};   return ((unsigned)t < 6) ? v[t] : 0; }
static i32 tier_radius(i32 t){ static const i32 v[6] = {0,10,12,14,16,18}; return ((unsigned)t < 6) ? v[t] : 0; }

static i32 spawn_enemy(i32 tier, i32 dist_fp)
{
    if (g_enemy_count >= MAX_ENEMIES) return -1;
    for (i32 i = 0; i < MAX_ENEMIES; i++) {
        if (!g_enemy[i].alive) {
            g_enemy[i].alive = 1;
            g_enemy[i].tier = tier;
            g_enemy[i].hp = tier_hp(tier);
            g_enemy[i].dist = dist_fp;
            g_enemy[i].speed = tier_speed(tier);
            g_enemy[i].slow_ticks = 0;
            g_enemy_count++;
            return i;
        }
    }
    return -1;
}

/* ===================================================================== */
/* Towers                                                                */
/* ===================================================================== */

#define MAX_TOWERS 64

#define TT_PIN     0
#define TT_BOOMER  1
#define TT_FROSTER 2
#define TT_COUNT   3

typedef struct {
    const char *name;
    i32 cost;
    i32 range;       /* px */
    i32 fire_cd;     /* ticks between shots */
    i32 dmg;
    i32 splash;      /* splash radius px (0 = single target) */
    i32 slow;        /* slow ticks applied (0 = none) */
    u32 col;         /* base colour */
    u32 barrel;      /* barrel colour */
} TowerType;

static const TowerType TTYPE[TT_COUNT] = {
    /* name      cost range cd dmg splash slow  col         barrel     */
    { "Pin",     100,  95,  6,  1,  0,     0,   0xFFB0BEC8, 0xFFE6EDF2 },
    { "Boomer",  220,  85, 26,  2, 46,     0,   0xFFC76A2A, 0xFF8A3F12 },
    { "Froster", 160,  80, 16,  1,  0,    60,   0xFF59C8E0, 0xFF2A96B5 },
};

typedef struct {
    i32 alive;
    i32 type;
    i32 x, y;        /* center px */
    i32 cd;          /* ticks until next shot */
    i32 target;      /* current target enemy index (-1 none) */
    i32 aim_x, aim_y;/* last aim point (for barrel rotation) */
} Tower;

static Tower g_tower[MAX_TOWERS];
static i32   g_tower_count = 0;

#define TOWER_R 16   /* tower body radius (also used for overlap test) */

/* ===================================================================== */
/* Projectiles                                                           */
/* ===================================================================== */

#define MAX_PROJ 256

typedef struct {
    i32 alive;
    i32 x, y;        /* current pos (FP) */
    i32 tx, ty;      /* target pos (px)  */
    i32 vx, vy;      /* velocity (FP/tick) */
    i32 ttl;         /* ticks before auto-expire */
    i32 dmg;
    i32 splash;
    i32 slow;
    u32 col;
} Proj;

static Proj g_proj[MAX_PROJ];

static void spawn_proj(i32 sx, i32 sy, i32 tx, i32 ty,
                       i32 dmg, i32 splash, i32 slow, u32 col)
{
    /* compute unit-ish velocity toward target */
    i32 dx = tx - sx, dy = ty - sy;
    i32 d = i_sqrt(dx * dx + dy * dy);
    if (d < 1) d = 1;
    i32 spd = 14;   /* px per tick */
    for (i32 i = 0; i < MAX_PROJ; i++) {
        if (!g_proj[i].alive) {
            g_proj[i].alive = 1;
            g_proj[i].x = TO_FP(sx);
            g_proj[i].y = TO_FP(sy);
            g_proj[i].tx = tx;
            g_proj[i].ty = ty;
            g_proj[i].vx = (dx * TO_FP(spd)) / d;
            g_proj[i].vy = (dy * TO_FP(spd)) / d;
            g_proj[i].ttl = 60;
            g_proj[i].dmg = dmg;
            g_proj[i].splash = splash;
            g_proj[i].slow = slow;
            g_proj[i].col = col;
            return;
        }
    }
}

/* ===================================================================== */
/* Waves                                                                 */
/* ===================================================================== */

/* Each wave: a list of (tier, count) groups spawned in order, spaced out.   */
#define MAX_WAVES       18
#define MAX_GROUPS       5

typedef struct { i32 tier; i32 count; } Group;
typedef struct { Group g[MAX_GROUPS]; i32 ngroups; i32 gap; } Wave;

static Wave g_waves[MAX_WAVES];

static void build_waves(void)
{
    /* helper-ish manual fill; gap = ticks between spawns within wave */
    i32 w = 0;
    #define WAVE(GAP) do { g_waves[w].gap = (GAP); g_waves[w].ngroups = 0; } while(0)
    #define GRP(T,C)  do { i32 k = g_waves[w].ngroups++; g_waves[w].g[k].tier=(T); g_waves[w].g[k].count=(C); } while(0)
    #define ENDW()    do { w++; } while(0)

    WAVE(34); GRP(1, 8);                          ENDW(); /* 1  */
    WAVE(30); GRP(1, 14);                          ENDW(); /* 2  */
    WAVE(30); GRP(1, 10); GRP(2, 5);               ENDW(); /* 3  */
    WAVE(26); GRP(2, 14);                          ENDW(); /* 4  */
    WAVE(26); GRP(1, 12); GRP(2, 8);               ENDW(); /* 5  */
    WAVE(24); GRP(2, 10); GRP(3, 4);               ENDW(); /* 6  */
    WAVE(24); GRP(3, 10);                          ENDW(); /* 7  */
    WAVE(22); GRP(2, 12); GRP(3, 8);               ENDW(); /* 8  */
    WAVE(22); GRP(3, 12); GRP(4, 3);               ENDW(); /* 9  */
    WAVE(20); GRP(4, 8);                           ENDW(); /* 10 */
    WAVE(20); GRP(3, 12); GRP(4, 6);               ENDW(); /* 11 */
    WAVE(18); GRP(4, 12);                          ENDW(); /* 12 */
    WAVE(18); GRP(3, 14); GRP(4, 8); GRP(5, 2);    ENDW(); /* 13 */
    WAVE(16); GRP(4, 12); GRP(5, 4);               ENDW(); /* 14 */
    WAVE(16); GRP(5, 8);                           ENDW(); /* 15 */
    WAVE(15); GRP(4, 16); GRP(5, 8);               ENDW(); /* 16 */
    WAVE(14); GRP(5, 14);                          ENDW(); /* 17 */
    WAVE(12); GRP(4, 20); GRP(5, 12); GRP(3, 10);  ENDW(); /* 18 boss-ish */

    #undef WAVE
    #undef GRP
    #undef ENDW
}

/* flattened spawn schedule for the current wave */
#define MAX_SCHED 64
static i32 g_sched_tier[MAX_SCHED];
static i32 g_sched_n = 0;
static i32 g_sched_i = 0;        /* next to spawn */
static i32 g_spawn_cd = 0;       /* ticks until next spawn */
static i32 g_cur_gap = 30;

/* ===================================================================== */
/* Game state                                                            */
/* ===================================================================== */

#define ST_BUILD     0   /* between waves, placing towers */
#define ST_WAVE      1   /* wave in progress              */
#define ST_GAMEOVER  2
#define ST_VICTORY   3

static i32 g_state;
static i32 g_cash;
static i32 g_lives;
static i32 g_wave;       /* 0-based index of current/next wave */
static i32 g_sel_tower;  /* selected palette tower (-1 none) */
static i32 g_mx, g_my;   /* current mouse pos */

static void reset_game(u64 ticks)
{
    g_rand_state = (u32)(ticks ^ (ticks >> 13) ^ 0x9E3779B9u);
    if (g_rand_state == 0) g_rand_state = 0xC0FFEEu;

    for (i32 i = 0; i < MAX_ENEMIES; i++) g_enemy[i].alive = 0;
    for (i32 i = 0; i < MAX_TOWERS; i++)  g_tower[i].alive  = 0;
    for (i32 i = 0; i < MAX_PROJ; i++)    g_proj[i].alive   = 0;
    g_enemy_count = 0;
    g_tower_count = 0;

    g_state = ST_BUILD;
    g_cash = 250;
    g_lives = 40;
    g_wave = 0;
    g_sel_tower = -1;
    g_sched_n = 0;
    g_sched_i = 0;
    g_spawn_cd = 0;
}

/* Prepare the spawn schedule for wave index w (0-based). */
static void start_wave(void)
{
    if (g_wave >= MAX_WAVES) return;
    if (g_state == ST_WAVE) return;   /* already running */

    Wave *wv = &g_waves[g_wave];
    g_sched_n = 0;
    for (i32 gi = 0; gi < wv->ngroups; gi++) {
        i32 t = wv->g[gi].tier;
        i32 ct = wv->g[gi].count;
        for (i32 k = 0; k < ct && g_sched_n < MAX_SCHED; k++) {
            g_sched_tier[g_sched_n++] = t;
        }
    }
    g_sched_i = 0;
    g_cur_gap = wv->gap;
    g_spawn_cd = 4;
    g_state = ST_WAVE;
}

/* ===================================================================== */
/* Update                                                                */
/* ===================================================================== */

/* Find the first enemy (furthest along path) within range of (tx,ty). */
static i32 acquire_target(i32 tx, i32 ty, i32 range)
{
    i32 r2 = range * range;
    i32 best = -1;
    i32 best_dist = -1;
    for (i32 i = 0; i < MAX_ENEMIES; i++) {
        if (!g_enemy[i].alive) continue;
        Pt p = path_point(TO_PX(g_enemy[i].dist));
        i32 dx = p.x - tx, dy = p.y - ty;
        if (dx * dx + dy * dy <= r2) {
            if (g_enemy[i].dist > best_dist) {
                best_dist = g_enemy[i].dist;
                best = i;
            }
        }
    }
    return best;
}

static void pop_enemy(i32 idx)
{
    Enemy *e = &g_enemy[idx];
    if (!e->alive) return;
    i32 tier = e->tier;
    i32 dist = e->dist;
    g_cash += tier_reward(tier);
    e->alive = 0;
    g_enemy_count--;
    /* SIGNATURE: split into two tier-1 bubbles (if tier > 1).
     * Only split if BOTH children fit; spawn_enemy also guards each slot, but
     * this combined check keeps the count honest and avoids partial splits. */
    if (tier > 1 && g_enemy_count + 2 <= MAX_ENEMIES) {
        /* fan the two children apart by a small random gap along the path */
        i32 jitter = (i32)(lcg_rand() % (u32)(FP_ONE * 4));
        spawn_enemy(1, dist + jitter);
        spawn_enemy(1, dist - (FP_ONE * 6) - jitter);
    }
}

/* Apply damage to an enemy; handle pop+split. Returns 1 if it died. */
static i32 damage_enemy(i32 idx, i32 dmg, i32 slow)
{
    Enemy *e = &g_enemy[idx];
    if (!e->alive) return 0;
    if (slow > 0) {
        if (e->slow_ticks < slow) e->slow_ticks = slow;
    }
    e->hp -= dmg;
    if (e->hp <= 0) {
        pop_enemy(idx);
        return 1;
    }
    return 0;
}

static void update_game(void)
{
    if (g_state != ST_WAVE) {
        /* still animate projectiles between waves (cosmetic) */
    }

    /* ---- spawn from schedule ---- */
    if (g_state == ST_WAVE && g_sched_i < g_sched_n) {
        if (g_spawn_cd > 0) g_spawn_cd--;
        else {
            spawn_enemy(g_sched_tier[g_sched_i], TO_FP(0));
            g_sched_i++;
            g_spawn_cd = g_cur_gap;
        }
    }

    /* ---- move enemies ---- */
    i32 end_fp = TO_FP(g_total_len);
    for (i32 i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g_enemy[i];
        if (!e->alive) continue;
        i32 spd = e->speed;
        if (e->slow_ticks > 0) { spd = spd / 2; e->slow_ticks--; }
        e->dist += spd;
        if (e->dist >= end_fp) {
            /* leaked: lose lives = tier cost */
            g_lives -= tier_lives(e->tier);
            e->alive = 0;
            g_enemy_count--;
            if (g_lives <= 0) {
                g_lives = 0;
                g_state = ST_GAMEOVER;
            }
        }
    }

    /* ---- towers fire ---- */
    for (i32 i = 0; i < MAX_TOWERS; i++) {
        Tower *t = &g_tower[i];
        if (!t->alive) continue;
        const TowerType *tt = &TTYPE[t->type];
        if (t->cd > 0) t->cd--;

        i32 tgt = acquire_target(t->x, t->y, tt->range);
        t->target = tgt;
        if (tgt >= 0) {
            Pt p = path_point(TO_PX(g_enemy[tgt].dist));
            t->aim_x = p.x;
            t->aim_y = p.y;
            if (t->cd == 0) {
                spawn_proj(t->x, t->y, p.x, p.y,
                           tt->dmg, tt->splash, tt->slow, tt->barrel);
                t->cd = tt->fire_cd;
            }
        }
    }

    /* ---- projectiles ---- */
    for (i32 i = 0; i < MAX_PROJ; i++) {
        Proj *pr = &g_proj[i];
        if (!pr->alive) continue;
        pr->x += pr->vx;
        pr->y += pr->vy;
        pr->ttl--;
        i32 px = TO_PX(pr->x), py = TO_PX(pr->y);
        i32 ddx = px - pr->tx, ddy = py - pr->ty;
        i32 arrived = (ddx * ddx + ddy * ddy) <= (12 * 12);
        if (pr->ttl <= 0) arrived = 1;   /* expire = detonate at current pos */
        if (px < -40 || px > WIN_W + 40 || py < -40 || py > WIN_H + 40)
            { pr->alive = 0; continue; }

        if (arrived) {
            pr->alive = 0;
            if (pr->splash > 0) {
                /* splash: damage all enemies near impact */
                i32 r2 = pr->splash * pr->splash;
                i32 ix = pr->tx, iy = pr->ty;
                for (i32 e = 0; e < MAX_ENEMIES; e++) {
                    if (!g_enemy[e].alive) continue;
                    Pt p = path_point(TO_PX(g_enemy[e].dist));
                    i32 dx = p.x - ix, dy = p.y - iy;
                    if (dx * dx + dy * dy <= r2)
                        damage_enemy(e, pr->dmg, pr->slow);
                }
            } else {
                /* single target: hit nearest enemy to impact point */
                i32 best = -1, bestd2 = 0x7FFFFFFF;
                for (i32 e = 0; e < MAX_ENEMIES; e++) {
                    if (!g_enemy[e].alive) continue;
                    Pt p = path_point(TO_PX(g_enemy[e].dist));
                    i32 dx = p.x - pr->tx, dy = p.y - pr->ty;
                    i32 d2 = dx * dx + dy * dy;
                    i32 rad = tier_radius(g_enemy[e].tier) + 8;
                    if (d2 <= rad * rad && d2 < bestd2) { bestd2 = d2; best = e; }
                }
                if (best >= 0) damage_enemy(best, pr->dmg, pr->slow);
            }
        }
    }

    /* ---- wave clear check ---- */
    if (g_state == ST_WAVE && g_sched_i >= g_sched_n && g_enemy_count == 0) {
        /* award bonus and advance */
        g_cash += 50 + g_wave * 15;
        g_wave++;
        if (g_wave >= MAX_WAVES) {
            g_state = ST_VICTORY;
        } else {
            g_state = ST_BUILD;
        }
    }
}

/* ===================================================================== */
/* Placement / hit-testing                                               */
/* ===================================================================== */

static i32 in_field(i32 x, i32 y)
{
    return x >= 6 && x < WIN_W - 6 && y >= FIELD_Y + 6 && y < PALETTE_Y - 6;
}

/* Can a tower be placed at (x,y)? not on path, not on tower, in field. */
static i32 can_place(i32 x, i32 y)
{
    if (!in_field(x, y)) return 0;
    /* path clearance */
    i32 clear = PATH_HALF + TOWER_R - 2;
    if (dist2_to_path(x, y) < clear * clear) return 0;
    /* tower overlap */
    for (i32 i = 0; i < MAX_TOWERS; i++) {
        if (!g_tower[i].alive) continue;
        i32 dx = x - g_tower[i].x, dy = y - g_tower[i].y;
        i32 md = TOWER_R * 2;
        if (dx * dx + dy * dy < md * md) return 0;
    }
    return 1;
}

static void place_tower(i32 type, i32 x, i32 y)
{
    if (g_tower_count >= MAX_TOWERS) return;
    if (g_cash < TTYPE[type].cost) return;
    if (!can_place(x, y)) return;
    for (i32 i = 0; i < MAX_TOWERS; i++) {
        if (!g_tower[i].alive) {
            g_tower[i].alive = 1;
            g_tower[i].type = type;
            g_tower[i].x = x;
            g_tower[i].y = y;
            g_tower[i].cd = 0;
            g_tower[i].target = -1;
            g_tower[i].aim_x = x;
            g_tower[i].aim_y = y - 30;
            g_tower_count++;
            g_cash -= TTYPE[type].cost;
            return;
        }
    }
}

/* Palette slot rectangles. */
#define PAL_SLOT_W 150
#define PAL_SLOT_X0 12
#define PAL_SLOT_GAP 8
static void pal_slot_rect(i32 i, i32 *x, i32 *y, i32 *w, i32 *h)
{
    *x = PAL_SLOT_X0 + i * (PAL_SLOT_W + PAL_SLOT_GAP);
    *y = PALETTE_Y + 8;
    *w = PAL_SLOT_W;
    *h = PALETTE_H - 16;
}

/* Start Wave button rect (right side of palette). */
static void startbtn_rect(i32 *x, i32 *y, i32 *w, i32 *h)
{
    *w = 150; *h = PALETTE_H - 16;
    *x = WIN_W - *w - 12;
    *y = PALETTE_Y + 8;
}
static i32 point_in(i32 px, i32 py, i32 x, i32 y, i32 w, i32 h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Find a placed tower under the cursor (for hover-range display). */
static i32 tower_under(i32 x, i32 y)
{
    for (i32 i = 0; i < MAX_TOWERS; i++) {
        if (!g_tower[i].alive) continue;
        i32 dx = x - g_tower[i].x, dy = y - g_tower[i].y;
        if (dx * dx + dy * dy <= TOWER_R * TOWER_R) return i;
    }
    return -1;
}

/* ===================================================================== */
/* Rendering                                                             */
/* ===================================================================== */

static void draw_path(Canvas *c)
{
    /* Draw path as a thick band: edge color then inner fill along segments. */
    for (i32 s = 0; s < g_path_n - 1; s++) {
        draw_line_thick(c, g_path[s].x, g_path[s].y,
                        g_path[s + 1].x, g_path[s + 1].y,
                        PATH_HALF * 2 + 6, COL_PATH_EDGE);
    }
    for (i32 s = 0; s < g_path_n - 1; s++) {
        draw_line_thick(c, g_path[s].x, g_path[s].y,
                        g_path[s + 1].x, g_path[s + 1].y,
                        PATH_HALF * 2, COL_PATH);
    }
    /* round the joints */
    for (i32 s = 1; s < g_path_n - 1; s++) {
        fill_circle(c, g_path[s].x, g_path[s].y, PATH_HALF + 3, COL_PATH_EDGE);
        fill_circle(c, g_path[s].x, g_path[s].y, PATH_HALF, COL_PATH);
    }
}

static void draw_tower(Canvas *c, Tower *t)
{
    const TowerType *tt = &TTYPE[t->type];
    /* base */
    fill_circle(c, t->x, t->y, TOWER_R, 0xFF20303E);
    fill_circle(c, t->x, t->y, TOWER_R - 3, tt->col);
    /* barrel pointed toward aim */
    i32 ax = t->aim_x, ay = t->aim_y;
    i32 dx = ax - t->x, dy = ay - t->y;
    i32 d = i_sqrt(dx * dx + dy * dy);
    if (d < 1) { dx = 0; dy = -1; d = 1; }
    i32 bx = t->x + (dx * (TOWER_R + 8)) / d;
    i32 by = t->y + (dy * (TOWER_R + 8)) / d;
    draw_line_thick(c, t->x, t->y, bx, by, 6, tt->barrel);
    fill_circle(c, bx, by, 3, tt->barrel);
    /* tiny center hub */
    fill_circle(c, t->x, t->y, 4, 0xFF101820);
}

static void draw_enemy(Canvas *c, Enemy *e)
{
    Pt p = path_point(TO_PX(e->dist));
    i32 r = tier_radius(e->tier);
    u32 col = TIER_COL[e->tier];
    /* slow tint: lighten/blue shimmer */
    fill_circle(c, p.x, p.y, r, col);
    /* highlight */
    fill_circle(c, p.x - r / 3, p.y - r / 3, r / 3, 0x88FFFFFF);
    /* outline ring if slowed */
    if (e->slow_ticks > 0) circle_outline(c, p.x, p.y, r + 1, 0xFFAEE8FF);
    /* tier number centered */
    char num[4];
    fmt_u32(num, (u32)e->tier);
    i32 tw = font_text_width(num);
    font_draw_string(c->buf, c->stride, c->w, c->h,
                     p.x - tw / 2, p.y - FONT_H / 2, num, COL_WHITE);
}

static void draw_hud(Canvas *c)
{
    fill_rect(c, 0, 0, WIN_W, HUD_H, COL_HUD_BG);
    char line[96];
    i32 n;

    /* Cash */
    n = 0; str_append(line, &n, "CASH $"); num_append(line, &n, (u32)g_cash);
    draw_text(c, 12, 12, line, COL_YELLOW);

    /* Lives */
    n = 0; str_append(line, &n, "LIVES "); num_append(line, &n, (u32)g_lives);
    draw_text(c, 230, 12, line, COL_RED);

    /* Wave */
    n = 0;
    str_append(line, &n, "WAVE ");
    i32 shown = g_wave + 1; if (shown > MAX_WAVES) shown = MAX_WAVES;
    num_append(line, &n, (u32)shown);
    str_append(line, &n, "/");
    num_append(line, &n, (u32)MAX_WAVES);
    draw_text(c, 400, 12, line, COL_WHITE);

    /* State / hint */
    const char *st;
    if (g_state == ST_BUILD)        st = "BUILD: place towers, then Start Wave";
    else if (g_state == ST_WAVE)    st = "WAVE IN PROGRESS";
    else if (g_state == ST_GAMEOVER)st = "GAME OVER";
    else                            st = "VICTORY!";
    draw_text(c, 560, 12, st, COL_GREEN);
}

static void draw_palette(Canvas *c)
{
    fill_rect(c, 0, PALETTE_Y, WIN_W, PALETTE_H, COL_PAL_BG);
    fill_rect(c, 0, PALETTE_Y, WIN_W, 2, COL_DKGREY);

    for (i32 i = 0; i < TT_COUNT; i++) {
        i32 x, y, w, h;
        pal_slot_rect(i, &x, &y, &w, &h);
        const TowerType *tt = &TTYPE[i];
        i32 affordable = g_cash >= tt->cost;
        u32 frame = (g_sel_tower == i) ? COL_SEL : COL_DKGREY;
        fill_rect(c, x, y, w, h, 0xFF18283A);
        /* frame */
        fill_rect(c, x, y, w, 2, frame);
        fill_rect(c, x, y + h - 2, w, 2, frame);
        fill_rect(c, x, y, 2, h, frame);
        fill_rect(c, x + w - 2, y, 2, h, frame);

        /* icon: little tower */
        i32 icx = x + 24, icy = y + h / 2;
        fill_circle(c, icx, icy, 13, affordable ? tt->col : COL_DKGREY);
        fill_circle(c, icx, icy, 10, affordable ? 0xFF20303E : 0xFF2A3340);
        draw_line_thick(c, icx, icy, icx, icy - 16, 5,
                        affordable ? tt->barrel : COL_GREY);

        /* name + cost */
        u32 tcol = affordable ? COL_WHITE : COL_GREY;
        char nm[24]; i32 nn = 0;
        str_append(nm, &nn, "[");
        num_append(nm, &nn, (u32)(i + 1));
        str_append(nm, &nn, "] ");
        str_append(nm, &nn, tt->name);
        draw_text(c, x + 44, y + 8, nm, tcol);

        char cost[24]; i32 cn = 0;
        str_append(cost, &cn, "$");
        num_append(cost, &cn, (u32)tt->cost);
        draw_text(c, x + 44, y + 28, cost,
                  affordable ? COL_YELLOW : COL_GREY);
    }

    /* Start Wave button */
    {
        i32 x, y, w, h;
        startbtn_rect(&x, &y, &w, &h);
        i32 hot = point_in(g_mx, g_my, x, y, w, h);
        i32 can_start = (g_state == ST_BUILD);
        u32 bg = can_start ? (hot ? COL_BTN_HI : COL_BTN) : COL_DKGREY;
        fill_rect(c, x, y, w, h, bg);
        const char *label = (g_state == ST_WAVE) ? "FIGHTING..." : "START WAVE";
        if (g_state == ST_GAMEOVER || g_state == ST_VICTORY) label = "PRESS R";
        draw_text_centered(c, x + w / 2, y + h / 2 - FONT_H / 2, label, COL_WHITE);
    }

    /* center help text */
    draw_text_centered(c, WIN_W / 2 - 40, PALETTE_Y - 20,
        "Click a tower, then place it.  Space = next wave.", 0xFFBFE8C8);
}

static void render(Canvas *c)
{
    /* Letterbox: clear the WHOLE current surface (which may be larger than the
     * fixed WIN_W x WIN_H canvas after a Maximize/snap) so no stale garbage is
     * left in the margins around the fixed game canvas. All draws below are
     * clamped to c->w/c->h by the primitives, so the fixed-layout content is
     * simply blitted at the top-left and any extra space stays black. */
    if (c->w > WIN_W || c->h > WIN_H)
        fill_rect(c, 0, 0, c->w, c->h, COL_BLACK);

    /* field background: grass checker */
    fill_rect(c, 0, FIELD_Y, WIN_W, FIELD_H, COL_GRASS);
    for (i32 yy = FIELD_Y; yy < PALETTE_Y; yy += 40) {
        for (i32 xx = ((yy / 40) & 1) ? 40 : 0; xx < WIN_W; xx += 80) {
            fill_rect(c, xx, yy, 40, 40, COL_GRASS2);
        }
    }

    draw_path(c);

    /* hover range for placed tower under cursor */
    if (g_sel_tower < 0 && in_field(g_mx, g_my)) {
        i32 ht = tower_under(g_mx, g_my);
        if (ht >= 0)
            circle_outline(c, g_tower[ht].x, g_tower[ht].y,
                           TTYPE[g_tower[ht].type].range, 0xFFFFFFFF);
    }

    /* towers */
    for (i32 i = 0; i < MAX_TOWERS; i++)
        if (g_tower[i].alive) draw_tower(c, &g_tower[i]);

    /* enemies */
    for (i32 i = 0; i < MAX_ENEMIES; i++)
        if (g_enemy[i].alive) draw_enemy(c, &g_enemy[i]);

    /* projectiles */
    for (i32 i = 0; i < MAX_PROJ; i++) {
        if (!g_proj[i].alive) continue;
        i32 px = TO_PX(g_proj[i].x), py = TO_PX(g_proj[i].y);
        fill_circle(c, px, py, 3, g_proj[i].col);
        fill_circle(c, px, py, 1, COL_WHITE);
    }

    /* placement preview when a tower type is selected */
    if (g_sel_tower >= 0 && in_field(g_mx, g_my)) {
        const TowerType *tt = &TTYPE[g_sel_tower];
        i32 ok = can_place(g_mx, g_my) && g_cash >= tt->cost;
        u32 ring = ok ? 0xFFFFFFFF : COL_RED;
        circle_outline(c, g_mx, g_my, tt->range, ring);
        fill_circle(c, g_mx, g_my, TOWER_R - 4,
                    ok ? 0x99FFFFFF : 0x99FF4D6D);
        fill_circle(c, g_mx, g_my, TOWER_R - 8, tt->col);
    }

    draw_hud(c);
    draw_palette(c);

    /* overlays */
    if (g_state == ST_GAMEOVER || g_state == ST_VICTORY) {
        /* dim playfield with striped overlay (no alpha blend available) */
        for (i32 y = FIELD_Y; y < PALETTE_Y; y += 2)
            fill_rect(c, 0, y, WIN_W, 1, 0xFF000000);
        i32 cy = FIELD_Y + FIELD_H / 2;
        if (g_state == ST_GAMEOVER) {
            draw_text_centered(c, WIN_W / 2, cy - 30, "GAME  OVER", COL_RED);
        } else {
            draw_text_centered(c, WIN_W / 2, cy - 30, "VICTORY!", COL_GREEN);
        }
        char l[48]; i32 n = 0;
        str_append(l, &n, "Reached wave ");
        i32 shown = g_wave; if (shown > MAX_WAVES) shown = MAX_WAVES;
        if (g_state == ST_VICTORY) shown = MAX_WAVES;
        num_append(l, &n, (u32)shown);
        str_append(l, &n, " / ");
        num_append(l, &n, (u32)MAX_WAVES);
        draw_text_centered(c, WIN_W / 2, cy, l, COL_WHITE);
        draw_text_centered(c, WIN_W / 2, cy + 26,
                           "Press R to play again", COL_YELLOW);
    }
}

/* ===================================================================== */
/* Input handling                                                        */
/* ===================================================================== */

static void handle_click(i32 x, i32 y)
{
    /* palette clicks */
    for (i32 i = 0; i < TT_COUNT; i++) {
        i32 rx, ry, rw, rh;
        pal_slot_rect(i, &rx, &ry, &rw, &rh);
        if (point_in(x, y, rx, ry, rw, rh)) {
            g_sel_tower = (g_sel_tower == i) ? -1 : i;
            return;
        }
    }
    /* start-wave button */
    {
        i32 rx, ry, rw, rh;
        startbtn_rect(&rx, &ry, &rw, &rh);
        if (point_in(x, y, rx, ry, rw, rh)) {
            if (g_state == ST_GAMEOVER || g_state == ST_VICTORY) {
                u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
                reset_game(now);
            } else if (g_state == ST_BUILD) {
                start_wave();
            }
            return;
        }
    }
    /* field click: place tower if one is selected */
    if (g_sel_tower >= 0 && in_field(x, y)) {
        if (g_cash >= TTYPE[g_sel_tower].cost && can_place(x, y)) {
            place_tower(g_sel_tower, x, y);
            /* keep selection so player can place several quickly */
            if (g_cash < TTYPE[g_sel_tower].cost) g_sel_tower = -1;
        }
        return;
    }
}

static void handle_key(i32 code)
{
    switch (code) {
    case KEY_1: g_sel_tower = (g_sel_tower == 0) ? -1 : 0; break;
    case KEY_2: g_sel_tower = (g_sel_tower == 1) ? -1 : 1; break;
    case KEY_3: g_sel_tower = (g_sel_tower == 2) ? -1 : 2; break;
    case KEY_SPACE:
    case KEY_ENTER:
        if (g_state == ST_BUILD) start_wave();
        else if (g_state == ST_GAMEOVER || g_state == ST_VICTORY) {
            u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            reset_game(now);
        }
        break;
    case KEY_R:
        if (g_state == ST_GAMEOVER || g_state == ST_VICTORY) {
            u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            reset_game(now);
        }
        break;
    case KEY_ESC:
    case KEY_Q:
        print("[BUBBLETD] exit\n");
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
    print("[BUBBLETD] starting\n");

    if (wl_connect() != 0) {
        print("[BUBBLETD] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Bubble Defense");
    if (!win) {
        print("[BUBBLETD] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    Canvas cv;
    cv.buf = win->pixels;
    cv.stride = (i32)(win->stride / 4u);
    cv.w = (i32)win->w;
    cv.h = (i32)win->h;

    build_path();
    build_waves();

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
                if (btn && !prev_btn) handle_click(g_mx, g_my);  /* edge */
                prev_btn = btn;
            } else if (kind == WL_EVENT_KEY) {
                if (eb == 1) handle_key(ea);   /* eb = pressed */
            } else if (kind == WL_EVENT_RESIZE) {
                /* The library has ALREADY reallocated the buffer and updated
                 * win->{w,h,stride,pixels}. Refresh the cached canvas so every
                 * subsequent pixel write is bounded to the CURRENT surface with
                 * the CURRENT stride (a smaller window clamps; a larger one gets
                 * its full surface cleared each frame -- see render()). This is a
                 * fixed-canvas game, so the layout is NOT scaled (letterboxed). */
                cv.buf    = win->pixels;
                cv.stride = (i32)(win->stride / 4u);
                cv.w      = (i32)win->w;
                cv.h      = (i32)win->h;
            }
        }

        /* ---- fixed-step update ---- */
        if ((t - last_tick) >= TICK_MS) {
            /* run at most a few catch-up steps to stay responsive */
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
