/*
 * raycaster.c -- Wolfenstein-style pseudo-3D raycaster demo (freestanding, ring 3).
 * ==================================================================================
 *
 * A first-person 3D maze using DDA grid traversal raycasting.
 * Pure integer/fixed-point math; no floats anywhere.
 *
 * Features:
 *   - 640x400 window
 *   - Hard-coded 16x16 tile map with multiple rooms and corridors
 *   - DDA (Digital Differential Analysis) ray marching per screen column
 *   - Fish-eye correction via perpendicular wall distance (g_cos applied to
 *     ray angle relative to view direction)
 *   - Wall shading by distance (linear fade to dark) + side shading (N/S vs E/W)
 *   - Four wall colours (two for each map tile type 1–4) for visual variety
 *   - Ceiling gradient (dark top -> mid) and floor gradient (mid -> darker)
 *   - Top-down minimap (80x80, top-right corner) with player dot + FOV lines
 *   - Smooth player movement: W/S forward/back, A/D strafe, Left/Right turn
 *   - Collision detection (step-back on wall hit)
 *   - FPS counter (rolling average over 16 frames)
 *   - ESC to quit
 *
 * Fixed-point scheme:
 *   Positions stored as 20.12 fixed-point (FP_SHIFT = 12 bits).
 *   Map tiles are 1 unit; a cell in world-space = FP_ONE (4096 counts).
 *   g_sin / g_cos return 16.16 values; we right-shift by 4 to produce 16.12.
 *
 * Raycasting algorithm:
 *   For each screen column x (0..WIN_W-1):
 *     1. Compute ray angle = player_angle + ray_offset[x]  (precomputed table)
 *     2. Get ray direction: (dx, dy) = (g_cos(angle), g_sin(angle))   [16.12]
 *     3. DDA: track (map_x, map_y) and advance along ray with delta_dist
 *        to find the first wall hit (H or V side)
 *     4. Perpendicular distance = component along view direction (fish-eye fix)
 *     5. Projected wall height = (WIN_H * FP_ONE) / perp_dist   (clamped)
 *     6. Shade wall: base color for tile type, darken by distance, darken E/W
 *     7. Draw ceiling, wall slice, floor pixels for column x
 *
 * Build (from repo root, inside WSL Arch):
 *   ROOT=$(pwd)
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/raycaster/raycaster.c -o /tmp/rc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/game/game.c -o /tmp/game.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/rc.o /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/raycaster.elf
 *   objdump -d /tmp/raycaster.elf | grep fs:0x28   # MUST be empty
 *
 * Controls:
 *   W / S          -- move forward / backward
 *   A / D          -- strafe left / right
 *   Left / Right   -- turn left / right
 *   ESC            -- exit
 */

#include "../../lib/game/game.h"

/* =========================================================================
 * Window layout
 * ========================================================================= */
#define WIN_W    640
#define WIN_H    400

/* Minimap in top-right corner */
#define MM_X     (WIN_W - MM_W - 4)
#define MM_Y     4
#define MM_W     80
#define MM_H     80
#define MM_COLS  MAP_W
#define MM_ROWS  MAP_H

/* =========================================================================
 * Fixed-point setup  (20.12)
 * Our positions use 20.12 fixed-point (RC_ONE = 4096 per tile unit).
 * game.h defines FP_ONE=65536 for 16.16; we use RC_* names to avoid clash.
 * ========================================================================= */
#define RC_SHIFT  12
#define RC_ONE    (1 << RC_SHIFT)            /* 4096 per tile unit */
#define RC_HALF   (RC_ONE >> 1)              /* 2048 */

/*
 * g_sin/g_cos return 16.16 values in [-65536, 65536].
 * We want 16.12 values for our 20.12 world.  Shift right by 4.
 */
#define SIN12(a)   (g_sin(a) >> 4)
#define COS12(a)   (g_cos(a) >> 4)

/* =========================================================================
 * Map
 * ========================================================================= */
#define MAP_W  16
#define MAP_H  16

/*
 * Tile codes:
 *   0 = floor (walkable)
 *   1 = grey stone
 *   2 = red brick
 *   3 = blue wall
 *   4 = green mossy wall
 */
static const u8 MAP[MAP_H][MAP_W] = {
    { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 },
    { 1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 },
    { 1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 },
    { 1,0,0,2, 2,0,0,0, 0,0,3,3, 0,0,0,1 },
    { 1,0,0,2, 0,0,1,1, 0,0,0,3, 0,0,0,1 },
    { 1,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,0,1 },
    { 1,0,0,0, 0,0,0,0, 0,0,0,0, 0,4,4,1 },
    { 1,1,0,1, 0,0,0,4, 0,0,0,0, 0,4,0,1 },
    { 1,0,0,0, 0,0,0,4, 0,2,2,0, 0,0,0,1 },
    { 1,0,0,0, 0,0,0,0, 0,2,0,0, 0,0,0,1 },
    { 1,0,3,3, 0,0,0,0, 0,0,0,0, 0,0,0,1 },
    { 1,0,0,3, 0,4,4,4, 0,0,0,0, 0,0,0,1 },
    { 1,0,0,0, 0,0,0,0, 0,0,0,0, 2,0,0,1 },
    { 1,0,0,0, 0,0,0,0, 0,0,0,0, 2,0,0,1 },
    { 1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 },
    { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 },
};

static inline int map_solid(int mx, int my)
{
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return 1;
    return MAP[my][mx] != 0;
}

static inline int map_tile(int mx, int my)
{
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return 1;
    return MAP[my][mx];
}

/* =========================================================================
 * Field of view and angle tables
 * ========================================================================= */

/*
 * FOV = 64 angle units (out of 256 = full circle).
 * Half-FOV = 32 units.
 * For column x (0..WIN_W-1), the ray offset relative to view direction:
 *   ray_angle_offset[x] = round( (x / (WIN_W-1) - 0.5) * FOV )
 * stored as signed byte in [-32, +32].
 */
#define FOV_UNITS  64    /* angle units for full FOV (0..255 circle) */
#define HALF_FOV   32

/* Precomputed per-column angle offsets: angle_offset[x] in [-32..32] */
static i8 angle_offset[WIN_W];

/* Build the angle offset table once at startup */
static void build_angle_table(void)
{
    for (int x = 0; x < WIN_W; x++) {
        /*
         * Map x in [0, WIN_W-1] -> offset in [-HALF_FOV, +HALF_FOV].
         * Integer: (x * FOV_UNITS) / (WIN_W - 1) - HALF_FOV
         */
        int off = (x * FOV_UNITS) / (WIN_W - 1) - HALF_FOV;
        angle_offset[x] = (i8)off;
    }
}

/* =========================================================================
 * Player state
 * ========================================================================= */
typedef struct {
    i32 x;       /* 20.12 fixed-point world position */
    i32 y;       /* 20.12 fixed-point world position */
    int angle;   /* 0..255 (8-bit wrap) */
} player_t;

/* =========================================================================
 * Color helpers
 * ========================================================================= */

/* Build ARGB32 from components */
static inline u32 rgb(u8 r, u8 g, u8 b)
{
    return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

/*
 * Darken a color by factor (0 = black, 256 = unchanged).
 * factor > 256 is clamped to 256.
 */
static inline u32 darken(u32 c, int factor)
{
    if (factor <= 0) return 0xFF000000u;
    if (factor >= 256) return c;
    u32 r = ((c >> 16) & 0xFF) * (u32)factor >> 8;
    u32 g = ((c >>  8) & 0xFF) * (u32)factor >> 8;
    u32 b = ( c        & 0xFF) * (u32)factor >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Base wall colors per tile type */
static inline u32 tile_color(int tile)
{
    switch (tile) {
        case 1:  return rgb(160, 160, 160); /* grey stone */
        case 2:  return rgb(180,  60,  60); /* red brick  */
        case 3:  return rgb( 60,  80, 200); /* blue wall  */
        case 4:  return rgb( 50, 140,  60); /* green moss */
        default: return rgb(200, 200, 200);
    }
}

/* =========================================================================
 * Division helper: (a * RC_ONE) / b  with guard against b==0
 * ========================================================================= */
static inline i32 fp_div(i32 a, i32 b)
{
    if (b == 0) b = 1;
    return (i32)(((i64)a << RC_SHIFT) / b);
}

/* =========================================================================
 * Raycasting: cast one ray, return projected wall height and shade color.
 *
 * Parameters:
 *   px, py    - player position (20.12 FP)
 *   ray_ang   - ray angle (0..255)
 *   view_ang  - player view angle (0..255)
 *   out_color - wall color including shading (output)
 *
 * Returns: projected half-wall height in pixels (0 if no hit within max dist)
 * ========================================================================= */
#define MAX_DIST_FP   (16 * RC_ONE)   /* max ray travel (16 map units) */

static int cast_ray(i32 px, i32 py,
                    int ray_ang, int view_ang,
                    u32 *out_color)
{
    /* Ray direction (16.12 fixed-point) */
    i32 rdx = COS12(ray_ang);
    i32 rdy = SIN12(ray_ang);

    /* Map tile the player is in */
    int mx = (int)(px >> RC_SHIFT);
    int my = (int)(py >> RC_SHIFT);

    /* Position within tile (0..RC_ONE-1) */
    i32 fx = px & (RC_ONE - 1);
    i32 fy = py & (RC_ONE - 1);

    /*
     * DDA setup.
     * delta_dist_x = |RC_ONE / rdx|   (how far along ray between vertical grid lines)
     * delta_dist_y = |RC_ONE / rdy|
     *
     * We scale to avoid extreme values: if component is near zero, use large sentinel.
     */
#define LARGE_DIST  (32 * RC_ONE)

    i32 ddx, ddy;
    if (rdx == 0) ddx = LARGE_DIST;
    else          ddx = g_abs(fp_div(RC_ONE, rdx));
    if (rdy == 0) ddy = LARGE_DIST;
    else          ddy = g_abs(fp_div(RC_ONE, rdy));

    /* Step direction and initial side_dist */
    int step_x, step_y;
    i32 sdx, sdy;

    if (rdx < 0) {
        step_x = -1;
        sdx = (i32)(((i64)fx * ddx) >> RC_SHIFT);   /* distance to left edge */
    } else {
        step_x = 1;
        sdx = (i32)(((i64)(RC_ONE - fx) * ddx) >> RC_SHIFT); /* distance to right edge */
    }
    if (rdy < 0) {
        step_y = -1;
        sdy = (i32)(((i64)fy * ddy) >> RC_SHIFT);
    } else {
        step_y = 1;
        sdy = (i32)(((i64)(RC_ONE - fy) * ddy) >> RC_SHIFT);
    }

    /* March until wall hit or max distance */
    int side = 0;  /* 0 = vertical grid line hit (E/W wall), 1 = horizontal (N/S wall) */
    int tile = 0;
    i32 perp_dist = 0;

    for (int step = 0; step < 64; step++) {
        if (sdx < sdy) {
            perp_dist = sdx;
            sdx += ddx;
            mx += step_x;
            side = 0;
        } else {
            perp_dist = sdy;
            sdy += ddy;
            my += step_y;
            side = 1;
        }

        if (perp_dist > MAX_DIST_FP) {
            *out_color = 0xFF000000;
            return 0;
        }

        tile = map_tile(mx, my);
        if (tile != 0) break;
    }

    if (tile == 0) {
        *out_color = 0xFF000000;
        return 0;
    }

    /*
     * Fish-eye correction: project onto view plane.
     * perp_dist is the distance along the ray; we need the perpendicular
     * (orthographic) distance to avoid fish-eye distortion.
     *
     * Correct approach: multiply ray distance by cos of the angle difference
     * between ray and view direction.
     *   perp = ray_dist * cos(ray_ang - view_ang)
     *
     * angle_diff in [-HALF_FOV, +HALF_FOV]; cos of small angles is close to 1.
     */
    int angle_diff = (ray_ang - view_ang) & 0xFF;
    /* Normalise to [-128, 127] */
    if (angle_diff > 127) angle_diff -= 256;
    /* cos of angle_diff */
    i32 cos_diff = COS12((angle_diff + 256) & 0xFF);
    if (cos_diff < RC_HALF) cos_diff = RC_HALF; /* clamp to avoid infinite wall */

    /* perp_dist_corr = perp_dist * cos_diff / RC_ONE */
    i32 perp_corr = (i32)(((i64)perp_dist * cos_diff) >> RC_SHIFT);
    if (perp_corr < 64) perp_corr = 64;  /* avoid divide by zero */

    /* Projected wall height: WIN_H * RC_ONE / perp_corr */
    int wall_h = (int)(((i64)WIN_H * RC_ONE) / perp_corr);
    if (wall_h > WIN_H * 4) wall_h = WIN_H * 4;

    /*
     * Shading:
     *   - Distance fade: the further away, the darker.
     *     factor = 256 - (perp_corr * 200 / MAX_DIST_FP)
     *   - Side shading: E/W walls (side=0) are slightly darker than N/S (side=1).
     */
    int shade = 256 - (int)(((i64)perp_corr * 200) / MAX_DIST_FP);
    if (shade < 30)  shade = 30;
    if (shade > 256) shade = 256;
    if (side == 0) shade = shade * 3 / 4;   /* E/W face 75% brightness */

    u32 base = tile_color(tile);
    *out_color = darken(base, shade);

    return wall_h;  /* half wall height in pixels (center at WIN_H/2) */
}

/* =========================================================================
 * Ceiling and floor pixel colors
 * ========================================================================= */

static inline u32 ceiling_color(int y)
{
    /* Gradient from near-black at top to dark-grey at horizon */
    int t = y * 80 / (WIN_H / 2);  /* 0 at top, 80 at horizon */
    return rgb((u8)t, (u8)t, (u8)(t + 10));
}

static inline u32 floor_color(int y)
{
    /* y is pixels below horizon; gradient from mid-dark to dark */
    int half = WIN_H / 2;
    int t = 40 + (y - half) * 60 / half;  /* 40 at horizon, 100 at bottom */
    if (t > 90) t = 90;
    return rgb((u8)(t / 3), (u8)(t / 2), (u8)(t / 3));
}

/* =========================================================================
 * Minimap rendering
 * ========================================================================= */

static void draw_minimap(game_t *g, const player_t *pl)
{
    int cw = MM_W / MAP_W;   /* cell width  in pixels */
    int ch = MM_H / MAP_H;   /* cell height in pixels */
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    /* Background */
    g_fill_rect(g, MM_X - 1, MM_Y - 1, MM_W + 2, MM_H + 2, 0xFF000000);

    /* Tiles */
    for (int my = 0; my < MAP_H; my++) {
        for (int mx = 0; mx < MAP_W; mx++) {
            int tile = MAP[my][mx];
            u32 col = (tile == 0) ? 0xFF222222 : darken(tile_color(tile), 200);
            g_fill_rect(g,
                        MM_X + mx * cw,
                        MM_Y + my * ch,
                        cw, ch, col);
        }
    }

    /* Player dot */
    int pdx = (int)(pl->x >> RC_SHIFT);
    int pdy = (int)(pl->y >> RC_SHIFT);
    /* Sub-tile position for smoother dot */
    int sub_x = (int)(((pl->x & (RC_ONE - 1)) * cw) >> RC_SHIFT);
    int sub_y = (int)(((pl->y & (RC_ONE - 1)) * ch) >> RC_SHIFT);
    int dot_x = MM_X + pdx * cw + sub_x;
    int dot_y = MM_Y + pdy * ch + sub_y;
    g_fill_rect(g, dot_x - 1, dot_y - 1, 3, 3, 0xFFFFFF00);

    /* FOV indicator lines (two rays at ±HALF_FOV) */
    int fov_len = cw * 3;
    for (int side = -1; side <= 1; side += 2) {
        int fov_ang = (pl->angle + side * HALF_FOV) & 0xFF;
        i32 fdx = COS12(fov_ang);
        i32 fdy = SIN12(fov_ang);
        /* Scale to pixel space: fdx is 16.12, multiply by fov_len, divide by RC_ONE */
        int ex = dot_x + (int)(((i64)fdx * fov_len) >> RC_SHIFT);
        int ey = dot_y + (int)(((i64)fdy * fov_len) >> RC_SHIFT);
        g_line(g, dot_x, dot_y, ex, ey, 0x88FFFF44);
    }
    /* View direction line */
    {
        i32 vdx = COS12(pl->angle);
        i32 vdy = SIN12(pl->angle);
        int ex = dot_x + (int)(((i64)vdx * fov_len * 2) >> RC_SHIFT);
        int ey = dot_y + (int)(((i64)vdy * fov_len * 2) >> RC_SHIFT);
        g_line(g, dot_x, dot_y, ex, ey, 0xFFFFFF44);
    }

    /* Border */
    g_rect(g, MM_X - 1, MM_Y - 1, MM_W + 2, MM_H + 2, 0xFF888888);
}

/* =========================================================================
 * Player movement and collision
 * ========================================================================= */

#define PLAYER_RADIUS  (RC_ONE * 25 / 100)   /* 0.25 tile units */
#define MOVE_SPEED     (RC_ONE * 3 / 100)     /* tiles per ms */
#define TURN_SPEED     1                       /* angle units per ~16ms frame */

/*
 * Try to move the player by (dx, dy) in world-space 20.12 coords.
 * Slides along walls: tries full move, then X-only, then Y-only.
 */
static void player_move(player_t *pl, i32 dx, i32 dy)
{
    i32 nx = pl->x + dx;
    i32 ny = pl->y + dy;
    i32 r  = PLAYER_RADIUS;

    /* Test a position: all four corners must be in empty space */
#define BLOCKED(px, py) \
    (map_solid((int)((px - r) >> RC_SHIFT), (int)((py - r) >> RC_SHIFT)) || \
     map_solid((int)((px + r) >> RC_SHIFT), (int)((py - r) >> RC_SHIFT)) || \
     map_solid((int)((px - r) >> RC_SHIFT), (int)((py + r) >> RC_SHIFT)) || \
     map_solid((int)((px + r) >> RC_SHIFT), (int)((py + r) >> RC_SHIFT)))

    if (!BLOCKED(nx, ny)) {
        pl->x = nx; pl->y = ny;
    } else if (!BLOCKED(nx, pl->y)) {
        pl->x = nx;
    } else if (!BLOCKED(pl->x, ny)) {
        pl->y = ny;
    }
    /* else: fully blocked, don't move */

#undef BLOCKED
}

/* =========================================================================
 * FPS counter (rolling average over 16 frames)
 * ========================================================================= */
#define FPS_HIST  16

static u64 fps_times[FPS_HIST];
static int fps_head = 0;
static int fps_val  = 60;

static void fps_update(int dt_ms)
{
    if (dt_ms < 1) dt_ms = 1;
    fps_times[fps_head] = (u64)dt_ms;
    fps_head = (fps_head + 1) & (FPS_HIST - 1);

    u64 sum = 0;
    for (int i = 0; i < FPS_HIST; i++) sum += fps_times[i];
    if (sum == 0) sum = 1;
    fps_val = (int)((u64)FPS_HIST * 1000 / sum);
}

/* =========================================================================
 * HUD rendering
 * ========================================================================= */

static void draw_hud(game_t *g)
{
    /* FPS in top-left */
    char buf[32];
    int n = 0;
    buf[n++] = 'F'; buf[n++] = 'P'; buf[n++] = 'S'; buf[n++] = ':';
    n += g_itoa(fps_val, buf + n);
    buf[n] = '\0';
    g_text(g, 4, 4, buf, 0xFFFFFFFF);

    /* Controls hint at bottom-left */
    g_text(g, 4, WIN_H - 18,
           "W/S:move  A/D:strafe  Arrows:turn  ESC:quit",
           0xFF888888);
}

/* =========================================================================
 * Main 3D scene rendering
 * ========================================================================= */

static void render_scene(game_t *g, const player_t *pl)
{
    u32 *bb    = game_backbuffer(g);
    int  half  = WIN_H / 2;

    /* Draw ceiling and floor first (background) */
    for (int y = 0; y < WIN_H; y++) {
        u32 bg_col;
        if (y < half)
            bg_col = ceiling_color(y);
        else
            bg_col = floor_color(y);

        u32 *row = bb + y * WIN_W;
        for (int x = 0; x < WIN_W; x++)
            row[x] = bg_col;
    }

    /*
     * Cast one ray per column.
     * Wall slice is drawn as a vertical strip centered at WIN_H/2.
     */
    for (int x = 0; x < WIN_W; x++) {
        int ray_ang = (pl->angle + angle_offset[x] + 256) & 0xFF;

        u32 wall_col;
        int wall_h = cast_ray(pl->x, pl->y, ray_ang, pl->angle, &wall_col);

        if (wall_h <= 0) continue;

        int top    = half - wall_h / 2;
        int bottom = half + wall_h / 2;
        if (top    < 0)     top    = 0;
        if (bottom > WIN_H) bottom = WIN_H;

        /* Draw the wall strip for this column */
        for (int y = top; y < bottom; y++) {
            bb[y * WIN_W + x] = wall_col;
        }
    }
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

void _start(void)
{
    game_t *g = game_open(WIN_W, WIN_H, "Raycaster");
    if (!g) {
        /* Cannot open window; spin-exit */
        for (;;) { }
    }

    /* Build angle lookup table */
    build_angle_table();

    /* Initialise FPS history */
    for (int i = 0; i < FPS_HIST; i++) fps_times[i] = 16;

    /* Player starts near top-left open area, facing right (angle 0 = east) */
    player_t pl;
    pl.x     = (i32)(2 * RC_ONE) + RC_HALF;  /* tile 2.5 */
    pl.y     = (i32)(2 * RC_ONE) + RC_HALF;  /* tile 2.5 */
    pl.angle = 0;                              /* facing east */

    while (game_frame_begin(g)) {
        int dt = game_dt_ms(g);
        if (dt < 1) dt = 1;
        if (dt > 50) dt = 50;   /* cap to avoid large jumps on lag spikes */

        /* ---- Input ---- */
        if (game_key_down(g, KEY_ESC)) break;

        /* Turn */
        if (game_key_down(g, KEY_LEFT))
            pl.angle = (pl.angle - TURN_SPEED + 256) & 0xFF;
        if (game_key_down(g, KEY_RIGHT))
            pl.angle = (pl.angle + TURN_SPEED) & 0xFF;

        /* Forward / backward movement speed scaled by dt */
        i32 speed = (i32)((i64)MOVE_SPEED * dt);  /* 20.12 scaled by ms */

        i32 fwd_dx = COS12(pl.angle);
        i32 fwd_dy = SIN12(pl.angle);

        /* Right direction: angle + 64 (quarter turn) */
        int right_ang = (pl.angle + 64) & 0xFF;
        i32 rgt_dx = COS12(right_ang);
        i32 rgt_dy = SIN12(right_ang);

        /* Scale direction by speed: result is 16.12 * speed(20.12) >> 12 */
        if (game_key_down(g, KEY_W)) {
            i32 dx = (i32)(((i64)fwd_dx * speed) >> RC_SHIFT);
            i32 dy = (i32)(((i64)fwd_dy * speed) >> RC_SHIFT);
            player_move(&pl, dx, dy);
        }
        if (game_key_down(g, KEY_S)) {
            i32 dx = (i32)(((i64)fwd_dx * speed) >> RC_SHIFT);
            i32 dy = (i32)(((i64)fwd_dy * speed) >> RC_SHIFT);
            player_move(&pl, -dx, -dy);
        }
        if (game_key_down(g, KEY_A)) {
            i32 dx = (i32)(((i64)rgt_dx * speed) >> RC_SHIFT);
            i32 dy = (i32)(((i64)rgt_dy * speed) >> RC_SHIFT);
            player_move(&pl, -dx, -dy);
        }
        if (game_key_down(g, KEY_D)) {
            i32 dx = (i32)(((i64)rgt_dx * speed) >> RC_SHIFT);
            i32 dy = (i32)(((i64)rgt_dy * speed) >> RC_SHIFT);
            player_move(&pl, dx, dy);
        }

        /* ---- Render ---- */
        render_scene(g, &pl);
        draw_minimap(g, &pl);
        draw_hud(g);

        game_present(g);
        game_sync(g);

        fps_update(game_dt_ms(g));
    }

    /* Exit via SYS_EXIT equivalent: loop-yield (game_open already connected,
     * returning from _start in freestanding triggers undefined behaviour;
     * game framework loops on window destroy, so we just loop-yield here) */
    for (;;) { }
}
