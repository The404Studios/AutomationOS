/*
 * pacman.c -- PAC-MAN-style maze game (freestanding, ring 3).
 * ==========================================================
 *
 * A hand-drawn tile maze (walls + dots + power pellets), Pac-Man steered
 * with arrow keys (continuous direction, only turns where the grid allows),
 * four ghosts with chase/scatter AI, score, lives, power-pellet "frightened"
 * mode (eat ghosts for bonus), win when all dots are eaten, game-over when a
 * ghost catches Pac-Man with no lives left.
 *
 * NO libc: freestanding inline syscalls + wl_client (window/input/commit) +
 * bitfont (HUD text). NO floating point in ring 3 -- pure integer/fixed math.
 * The maze, Pac-Man and ghosts are drawn with fill_rect primitives (a coarse
 * arc/circle for Pac-Man via a per-cell bitmap, animated open/close mouth).
 *
 * Grid:  MAZE_COLS x MAZE_ROWS tiles, CELL_PX pixels each, under a HUD strip.
 * Movement: one grid step per ~110ms tick (SYS_GET_TICKS_MS), ~9 fps -- the
 * smooth, readable cadence the classic uses; mouth animates every other tick.
 *
 * Build (compile-check; matches scripts/build_all.sh cc()/LD flags):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/pacman/pacman.c -o /tmp/pacman.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/pacman.o /tmp/wlc.o /tmp/bf.o -o /tmp/pacman.elf
 *   objdump -d /tmp/pacman.elf | grep fs:0x28      # MUST be empty
 *
 * Serial output:
 *   [PACMAN] starting
 *   [PACMAN] score N        (on game over / win)
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* ---- syscall numbers (AOS, see kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- key codes (from kernel/include/input.h) ---- */
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

/* ============================================================
 *  Maze layout
 *  ----------------------------------------------------------
 *  Each char is one tile:
 *    '#' wall      '.' dot       'o' power pellet
 *    ' ' empty     '-' ghost-house door (ghosts pass, Pac-Man blocked)
 *    'P' Pac-Man spawn          'G' ghost spawn (in the house)
 *  Tunnels: row with empty edge tiles wraps left<->right.
 *  21 cols x 23 rows -- a compact but authentic Pac-Man feel.
 * ============================================================ */
#define MAZE_COLS 21
#define MAZE_ROWS 23
static const char *const MAZE[MAZE_ROWS] = {
/*           1111111111222 */
/* 0123456789012345678901 */
  "#####################",  /* 0  */
  "#........#........#  #",  /* 1  */
  "#o##.###.#.###.##.#o #",  /* 2  */
  "#.................#  #",  /* 3  */
  "#.##.#.#####.#.##.#  #",  /* 4  */
  "#....#...#...#....#  #",  /* 5  */
  "####.### # # ###.####",  /* 6  */
  "   #.#   ---   #.#   ",  /* 7  */
  "####.# ##---## #.####",  /* 8  */
  "    .  #GG GG#  .    ",  /* 9  tunnel row */
  "####.# ####### #.####",  /* 10 */
  "   #.#         #.#   ",  /* 11 */
  "####.# ####### #.####",  /* 12 */
  "#........#........#  #",  /* 13 */
  "#.##.###.#.###.##.#  #",  /* 14 */
  "#o.#.....P.....#..#o #",  /* 15 */
  "##.#.#.#####.#.#.#.  #",  /* 16 */
  "#....#...#...#....#  #",  /* 17 */
  "#.######.#.######.#  #",  /* 18 */
  "#.................#  #",  /* 19 */
  "#.###.###.#.###.##.#  ",  /* 20 */
  "#.................#  #",  /* 21 */
  "#####################",  /* 22 */
};

/* Mutable tile grid: what remains on each cell (dots get eaten). */
#define T_WALL  0
#define T_EMPTY 1
#define T_DOT   2
#define T_PWR   3
#define T_DOOR  4
static unsigned char g_grid[MAZE_ROWS][MAZE_COLS];
static i32 g_dots_left;

/* ---- window / grid geometry ---- */
#define CELL_PX   22
#define HUD_H     24
#define WIN_W     (MAZE_COLS * CELL_PX)              /* 21*22 = 462 */
#define WIN_H     (HUD_H + MAZE_ROWS * CELL_PX)      /* 24+506 = 530 */

#define CELL_X(col)  ((col) * CELL_PX)
#define CELL_Y(row)  (HUD_H + (row) * CELL_PX)

/* ---- colours (ARGB32) ---- */
#define COL_BG        0xFF000000u   /* black play field          */
#define COL_WALL      0xFF2121DEu   /* classic maze blue         */
#define COL_WALL_HI   0xFF4444FFu   /* wall inner highlight      */
#define COL_DOOR      0xFFFFB8DEu   /* ghost-house door (pink)   */
#define COL_DOT       0xFFFFE0A8u   /* pac dots (warm cream)     */
#define COL_PWR       0xFFFFE0A8u   /* power pellet              */
#define COL_PAC       0xFFFFD60Au   /* Pac-Man yellow            */
#define COL_HUD_BG    0xFF101020u
#define COL_WHITE     0xFFFFFFFFu
#define COL_YELLOW    0xFFFFD60Au
#define COL_RED       0xFFFF4D6Du
#define COL_GREEN     0xFF4CD964u
#define COL_FRIGHT    0xFF2233CCu   /* frightened ghost body     */
#define COL_FRIGHT2   0xFFFFFFFFu   /* frightened blink (white)  */

/* Ghost body colours: Blinky red, Pinky pink, Inky cyan, Clyde orange. */
static const u32 GHOST_COL[4] = {
    0xFFFF0000u, 0xFFFFB8FFu, 0xFF00FFFFu, 0xFFFFB852u
};

/* ---- directions: 0=right 1=down 2=left 3=up, -1 = none ---- */
static const i32 DX[4] = { 1, 0, -1, 0 };
static const i32 DY[4] = { 0, 1, 0, -1 };

/* ---- actors ---- */
typedef struct { i32 cx, cy; i32 dir; } pac_t;
typedef struct {
    i32 cx, cy;          /* tile position                          */
    i32 dir;             /* current heading                        */
    i32 home_cx, home_cy;/* scatter target / respawn anchor        */
    i32 spawn_cx, spawn_cy;
    i32 eaten;           /* 1 while returning to house (eyes)      */
} ghost_t;

#define NGHOSTS 4
static pac_t   g_pac;
static ghost_t g_ghost[NGHOSTS];

static u32 g_score;
static i32 g_lives;
static i32 g_game_over;     /* 1 = lost                          */
static i32 g_win;           /* 1 = all dots eaten                */
static i32 g_paused;
static i32 g_fright_ticks;  /* >0 while ghosts are frightened    */
static i32 g_eat_chain;     /* consecutive ghosts eaten this pwr */
static u32 g_tick;          /* global step counter (for AI mode) */
static i32 g_mouth;         /* 0/1 animated mouth state          */
static i32 g_ready_ticks;   /* brief "READY!" pause after spawn  */

/* ---- LCG random ---- */
static u32 g_rand = 0x1234567u;
static u32 lcg(void) { g_rand = g_rand * 1664525u + 1013904223u; return g_rand; }

/* ============================================================
 *  Grid helpers
 * ============================================================ */
static i32 in_bounds(i32 cx, i32 cy)
{
    return cx >= 0 && cx < MAZE_COLS && cy >= 0 && cy < MAZE_ROWS;
}

/* Is the tile walkable for Pac-Man? (door blocks him). */
static i32 walkable_pac(i32 cx, i32 cy)
{
    if (!in_bounds(cx, cy)) return 0;
    unsigned char t = g_grid[cy][cx];
    return t != T_WALL && t != T_DOOR;
}
/* Is the tile walkable for a ghost? (door allowed). */
static i32 walkable_ghost(i32 cx, i32 cy)
{
    if (!in_bounds(cx, cy)) return 0;
    return g_grid[cy][cx] != T_WALL;
}

/* Horizontal tunnel wrap: returns wrapped column for actors leaving an edge. */
static i32 wrap_col(i32 cx) {
    if (cx < 0) return MAZE_COLS - 1;
    if (cx >= MAZE_COLS) return 0;
    return cx;
}

/* ============================================================
 *  Init / reset
 * ============================================================ */
static void place_actors(void)
{
    /* Pac-Man spawn from layout ('P'), ghosts from 'G'. */
    i32 gi = 0;
    g_pac.dir = 2;  /* face left, classic */
    for (i32 r = 0; r < MAZE_ROWS; r++) {
        for (i32 c = 0; c < MAZE_COLS; c++) {
            char ch = MAZE[r][c];
            if (ch == 'P') { g_pac.cx = c; g_pac.cy = r; }
            else if (ch == 'G' && gi < NGHOSTS) {
                g_ghost[gi].cx = c; g_ghost[gi].cy = r;
                g_ghost[gi].spawn_cx = c; g_ghost[gi].spawn_cy = r;
                g_ghost[gi].dir = 3; g_ghost[gi].eaten = 0;
                gi++;
            }
        }
    }
    /* If the layout lacked enough 'G', stack remaining ghosts at center. */
    while (gi < NGHOSTS) {
        g_ghost[gi].cx = MAZE_COLS / 2; g_ghost[gi].cy = MAZE_ROWS / 2;
        g_ghost[gi].spawn_cx = g_ghost[gi].cx; g_ghost[gi].spawn_cy = g_ghost[gi].cy;
        g_ghost[gi].dir = 3; g_ghost[gi].eaten = 0;
        gi++;
    }
    /* Scatter home corners (one per ghost). */
    g_ghost[0].home_cx = MAZE_COLS - 2; g_ghost[0].home_cy = 0;               /* top-right */
    g_ghost[1].home_cx = 1;             g_ghost[1].home_cy = 0;               /* top-left  */
    g_ghost[2].home_cx = MAZE_COLS - 2; g_ghost[2].home_cy = MAZE_ROWS - 1;  /* bot-right */
    g_ghost[3].home_cx = 1;             g_ghost[3].home_cy = MAZE_ROWS - 1;  /* bot-left  */
}

/* Build the mutable grid from the const layout, counting dots. */
static void build_grid(void)
{
    g_dots_left = 0;
    for (i32 r = 0; r < MAZE_ROWS; r++) {
        for (i32 c = 0; c < MAZE_COLS; c++) {
            char ch = MAZE[r][c];
            unsigned char t;
            switch (ch) {
            case '#': t = T_WALL;  break;
            case '.': t = T_DOT;  g_dots_left++; break;
            case 'o': t = T_PWR;  g_dots_left++; break;
            case '-': t = T_DOOR; break;
            default:  t = T_EMPTY; break;  /* ' ', 'P', 'G' */
            }
            g_grid[r][c] = t;
        }
    }
}

static void reset_round(void)
{
    place_actors();
    g_pac.dir       = 2;
    g_fright_ticks  = 0;
    g_eat_chain     = 0;
    g_ready_ticks   = 6;   /* ~0.6s "READY!" */
    for (i32 i = 0; i < NGHOSTS; i++) g_ghost[i].eaten = 0;
}

static void game_init(u64 ticks)
{
    g_rand = (u32)(ticks ^ (ticks >> 16));
    if (g_rand == 0) g_rand = 0xDEADBEEFu;
    g_score      = 0;
    g_lives      = 3;
    g_game_over  = 0;
    g_win        = 0;
    g_paused     = 0;
    g_tick       = 0;
    g_mouth      = 0;
    build_grid();
    reset_round();
}

/* Buffered desired direction (applied when the turn becomes legal). */
static i32 g_want_dir = 2;

/* ============================================================
 *  Ghost AI
 *  ----------------------------------------------------------
 *  Classic rules, integer-only:
 *   - frightened: pick a random legal direction (no reversal)
 *   - eaten (eyes): head straight back to the house spawn tile
 *   - else alternate scatter (target = home corner) and chase
 *     (target = Pac-Man tile), choosing at each tile the legal
 *     neighbour that minimises squared distance to the target,
 *     never reversing unless boxed in. Blinky chases directly;
 *     the others use light personality offsets toward Pac-Man.
 * ============================================================ */
static i32 dist2(i32 ax, i32 ay, i32 bx, i32 by)
{
    i32 dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

/* Scatter for ~20 ticks, chase for ~60 ticks, repeating. */
static i32 is_scatter(void)
{
    u32 phase = g_tick % 80u;
    return phase < 20u;
}

static void ghost_target(i32 i, i32 *tx, i32 *ty)
{
    ghost_t *g = &g_ghost[i];
    if (g->eaten) {                 /* return to house */
        *tx = g->spawn_cx; *ty = g->spawn_cy; return;
    }
    if (is_scatter()) {             /* flee to home corner */
        *tx = g->home_cx; *ty = g->home_cy; return;
    }
    /* Chase: aim at (or just ahead of) Pac-Man, with per-ghost flavour. */
    i32 px = g_pac.cx, py = g_pac.cy;
    switch (i) {
    case 0:  /* Blinky: straight at Pac-Man */
        *tx = px; *ty = py; break;
    case 1:  /* Pinky: 4 tiles ahead of Pac-Man's heading */
        *tx = px + DX[g_pac.dir] * 4; *ty = py + DY[g_pac.dir] * 4; break;
    case 2:  /* Inky: mirror a bit past Pac-Man */
        *tx = px + DX[g_pac.dir] * 2; *ty = py - DY[g_pac.dir] * 2; break;
    default: /* Clyde: chase when far, scatter to corner when near */
        if (dist2(g->cx, g->cy, px, py) > 36) { *tx = px; *ty = py; }
        else { *tx = g->home_cx; *ty = g->home_cy; }
        break;
    }
}

static void ghost_step(i32 i)
{
    ghost_t *g = &g_ghost[i];

    /* Eyes that reached the house turn back into a live ghost. */
    if (g->eaten && g->cx == g->spawn_cx && g->cy == g->spawn_cy) {
        g->eaten = 0;
        g->dir = 3;
    }

    i32 frightened = (g_fright_ticks > 0) && !g->eaten;
    i32 opp = (g->dir + 2) & 3;

    if (frightened) {
        /* Random walk: shuffle a direction list, take first legal non-reverse. */
        i32 order[4] = { 0, 1, 2, 3 };
        for (i32 k = 3; k > 0; k--) {
            i32 j = (i32)(lcg() % (u32)(k + 1));
            i32 t = order[k]; order[k] = order[j]; order[j] = t;
        }
        for (i32 k = 0; k < 4; k++) {
            i32 d = order[k];
            if (d == opp) continue;
            i32 nx = wrap_col(g->cx + DX[d]), ny = g->cy + DY[d];
            if (walkable_ghost(nx, ny)) { g->dir = d; break; }
        }
    } else {
        /* Greedy toward target: minimise squared distance, no reversal. */
        i32 tx, ty; ghost_target(i, &tx, &ty);
        i32 best = -1, bestcost = 0x7fffffff;
        for (i32 d = 0; d < 4; d++) {
            if (d == opp) continue;
            i32 nx = wrap_col(g->cx + DX[d]), ny = g->cy + DY[d];
            if (!walkable_ghost(nx, ny)) continue;
            i32 cost = dist2(nx, ny, tx, ty);
            if (cost < bestcost) { bestcost = cost; best = d; }
        }
        if (best < 0) best = opp;  /* dead-end: allow reverse */
        g->dir = best;
    }

    /* Advance one tile (with tunnel wrap). */
    i32 nx = wrap_col(g->cx + DX[g->dir]), ny = g->cy + DY[g->dir];
    if (walkable_ghost(nx, ny)) { g->cx = nx; g->cy = ny; }
}

/* ============================================================
 *  Collisions: Pac-Man vs ghosts (called twice per step so a
 *  same-tile swap is still caught).
 * ============================================================ */
static void handle_collisions(void)
{
    for (i32 i = 0; i < NGHOSTS; i++) {
        ghost_t *g = &g_ghost[i];
        if (g->cx != g_pac.cx || g->cy != g_pac.cy) continue;
        if (g->eaten) continue;
        if (g_fright_ticks > 0) {
            /* Eat ghost: chain bonus 200/400/800/1600. */
            g_eat_chain++;
            i32 bonus = 200;
            for (i32 k = 1; k < g_eat_chain && k < 4; k++) bonus *= 2;
            g_score += (u32)bonus;
            g->eaten = 1;
            g->dir = (g->dir + 2) & 3;  /* turn the eyes around */
        } else {
            /* Lose a life. */
            g_lives--;
            if (g_lives <= 0) { g_game_over = 1; }
            else { reset_round(); }
            return;  /* stop after a death this step */
        }
    }
}

/* ============================================================
 *  One game step (a grid tick)
 * ============================================================ */
static void game_step(void)
{
    if (g_game_over || g_win || g_paused) return;
    if (g_ready_ticks > 0) { g_ready_ticks--; return; }

    g_tick++;
    g_mouth ^= 1;

    /* --- Pac-Man movement: honour buffered turn at junctions --- */
    {
        i32 wx = wrap_col(g_pac.cx + DX[g_want_dir]);
        i32 wy = g_pac.cy + DY[g_want_dir];
        if (walkable_pac(wx, wy)) g_pac.dir = g_want_dir;  /* turn now */

        i32 nx = wrap_col(g_pac.cx + DX[g_pac.dir]);
        i32 ny = g_pac.cy + DY[g_pac.dir];
        if (walkable_pac(nx, ny)) { g_pac.cx = nx; g_pac.cy = ny; }
        /* else: blocked by wall -> stay put until a turn opens up. */
    }

    /* --- Eat dot / power pellet under Pac-Man --- */
    {
        unsigned char *t = &g_grid[g_pac.cy][g_pac.cx];
        if (*t == T_DOT) {
            *t = T_EMPTY; g_score += 10; g_dots_left--;
        } else if (*t == T_PWR) {
            *t = T_EMPTY; g_score += 50; g_dots_left--;
            g_fright_ticks = 50;   /* ~5.5s of vulnerability */
            g_eat_chain = 0;
        }
        if (g_dots_left <= 0) { g_win = 1; return; }
    }

    /* Collision check after Pac-Man moves (catch head-on swaps). */
    handle_collisions();
    if (g_game_over || g_win) return;

    /* --- Ghosts move; ghosts in frightened mode move every other tick
     *     (the classic slow-down), eyes always move. --- */
    for (i32 i = 0; i < NGHOSTS; i++) {
        if (g_fright_ticks > 0 && !g_ghost[i].eaten && (g_tick & 1)) continue;
        ghost_step(i);
    }

    handle_collisions();

    if (g_fright_ticks > 0) g_fright_ticks--;
    if (g_fright_ticks == 0) g_eat_chain = 0;
}

/* ============================================================
 *  Rendering
 * ============================================================ */
static void fill_rect(u32 *buf, u32 stride, i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > WIN_W) x2 = WIN_W;
    i32 y2 = y + h; if (y2 > WIN_H) y2 = WIN_H;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/* Filled circle (Bresenham-free, integer test r^2). */
static void fill_circle(u32 *buf, u32 stride, i32 cx, i32 cy, i32 r, u32 color)
{
    for (i32 dy = -r; dy <= r; dy++) {
        i32 yy = cy + dy;
        if (yy < 0 || yy >= WIN_H) continue;
        i32 span = r * r - dy * dy;
        if (span < 0) continue;
        /* integer sqrt of span */
        i32 dx = 0; while ((dx + 1) * (dx + 1) <= span) dx++;
        i32 x1 = cx - dx, x2 = cx + dx;
        if (x1 < 0) x1 = 0;
        if (x2 >= WIN_W) x2 = WIN_W - 1;
        u32 *row = buf + (u32)yy * stride;
        for (i32 xx = x1; xx <= x2; xx++) row[xx] = color;
    }
}

/* Draw Pac-Man: a yellow disc with a wedge "mouth" cut toward g_pac.dir.
 * Mouth open/close animated via g_mouth; integer slope test for the wedge. */
static void draw_pacman(u32 *buf, u32 stride)
{
    i32 cx = CELL_X(g_pac.cx) + CELL_PX / 2;
    i32 cy = CELL_Y(g_pac.cy) + CELL_PX / 2;
    i32 r  = CELL_PX / 2 - 2;
    i32 dir = g_pac.dir;
    /* mouth half-angle: wide when g_mouth, closed (full disc) otherwise */
    i32 open = g_mouth;  /* 1 = mouth open */

    for (i32 dy = -r; dy <= r; dy++) {
        i32 yy = cy + dy;
        if (yy < 0 || yy >= WIN_H) continue;
        i32 span = r * r - dy * dy;
        if (span < 0) continue;
        i32 dxm = 0; while ((dxm + 1) * (dxm + 1) <= span) dxm++;
        u32 *row = buf + (u32)yy * stride;
        for (i32 dx = -dxm; dx <= dxm; dx++) {
            i32 xx = cx + dx;
            if (xx < 0 || xx >= WIN_W) continue;
            if (open) {
                /* Cut a 90-degree wedge facing `dir`. The wedge is the
                 * region where |perpendicular| < along, measured along the
                 * facing axis -- pure integer comparison, no trig. */
                i32 along, perp;
                switch (dir) {
                case 0: along =  dx; perp = dy; break;  /* right */
                case 1: along =  dy; perp = dx; break;  /* down  */
                case 2: along = -dx; perp = dy; break;  /* left  */
                default:along = -dy; perp = dx; break;  /* up    */
                }
                if (along > 0 && (perp <= along && perp >= -along)) continue; /* mouth gap */
            }
            row[xx] = COL_PAC;
        }
    }
}

/* Draw one ghost: rounded body + two eyes; frightened -> blue + blinking. */
static void draw_ghost(u32 *buf, u32 stride, i32 i)
{
    ghost_t *g = &g_ghost[i];
    i32 x = CELL_X(g->cx), y = CELL_Y(g->cy);
    i32 cx = x + CELL_PX / 2;
    i32 r  = CELL_PX / 2 - 2;

    u32 body;
    if (g->eaten) {
        body = 0; /* eyes only */
    } else if (g_fright_ticks > 0) {
        /* Blink white in the last ~1.5s of frightened time. */
        i32 blink = (g_fright_ticks <= 14) && (g_fright_ticks & 1);
        body = blink ? COL_FRIGHT2 : COL_FRIGHT;
    } else {
        body = GHOST_COL[i];
    }

    if (body) {
        /* Dome (top half circle) + rectangular skirt. */
        fill_circle(buf, stride, cx, y + CELL_PX / 2 - 1, r, body);
        fill_rect(buf, stride, cx - r, y + CELL_PX / 2 - 1, 2 * r + 1, r, body);
        /* Three little feet (notched bottom) by punching gaps. */
        i32 footw = (2 * r + 1) / 3;
        for (i32 f = 0; f < 3; f++) {
            if (f & 1) continue;  /* leave gaps between feet */
            fill_rect(buf, stride, cx - r + f * footw,
                      y + CELL_PX - 3, footw, 3, COL_BG);
        }
    }

    /* Eyes (always visible). Pupils look toward heading. */
    i32 eyer = r / 2; if (eyer < 2) eyer = 2;
    i32 ey   = y + CELL_PX / 2 - 1;
    i32 elx  = cx - r / 2, erx = cx + r / 2;
    u32 white = COL_WHITE;
    u32 frightface = (g_fright_ticks > 0 && !g->eaten) ? 0xFFFFB8FFu : 0xFF1030FFu;
    fill_circle(buf, stride, elx, ey, eyer, white);
    fill_circle(buf, stride, erx, ey, eyer, white);
    i32 pdx = DX[g->dir], pdy = DY[g->dir];
    fill_circle(buf, stride, elx + pdx, ey + pdy, eyer / 2 + 1, frightface);
    fill_circle(buf, stride, erx + pdx, ey + pdy, eyer / 2 + 1, frightface);
}

/* Draw the maze: walls (blue, rounded inner highlight), dots, pellets, door. */
static void draw_maze(u32 *buf, u32 stride)
{
    for (i32 r = 0; r < MAZE_ROWS; r++) {
        for (i32 c = 0; c < MAZE_COLS; c++) {
            i32 x = CELL_X(c), y = CELL_Y(r);
            unsigned char t = g_grid[r][c];
            if (t == T_WALL) {
                fill_rect(buf, stride, x, y, CELL_PX, CELL_PX, COL_WALL);
                /* inner highlight for a slight 3-D bevel */
                fill_rect(buf, stride, x + 3, y + 3,
                          CELL_PX - 6, CELL_PX - 6, COL_WALL_HI);
                fill_rect(buf, stride, x + 5, y + 5,
                          CELL_PX - 10, CELL_PX - 10, COL_WALL);
            } else if (t == T_DOOR) {
                fill_rect(buf, stride, x, y + CELL_PX / 2 - 2,
                          CELL_PX, 4, COL_DOOR);
            } else if (t == T_DOT) {
                fill_rect(buf, stride, x + CELL_PX / 2 - 2,
                          y + CELL_PX / 2 - 2, 4, 4, COL_DOT);
            } else if (t == T_PWR) {
                /* Power pellet: a pulsing larger blob. */
                i32 pr = (g_tick & 1) ? CELL_PX / 2 - 4 : CELL_PX / 2 - 6;
                if (pr < 3) pr = 3;
                fill_circle(buf, stride, x + CELL_PX / 2,
                            y + CELL_PX / 2, pr, COL_PWR);
            }
        }
    }
}

static void draw_centered(u32 *buf, u32 stride, i32 y, const char *s, u32 color)
{
    i32 w = font_text_width(s);
    font_draw_string(buf, (i32)stride, WIN_W, WIN_H,
                     (WIN_W - w) / 2, y, s, color);
}

static void render(u32 *buf, u32 stride)
{
    /* Field + HUD. */
    fill_rect(buf, stride, 0, 0, WIN_W, WIN_H, COL_BG);
    fill_rect(buf, stride, 0, 0, WIN_W, HUD_H, COL_HUD_BG);

    /* HUD: score (left) + lives as small pac icons (right). */
    {
        char s[40]; i32 n = 0;
        const char *pfx = "SCORE ";
        for (i32 i = 0; pfx[i]; i++) s[n++] = pfx[i];
        char num[12]; i32 nl = fmt_u32(num, g_score);
        for (i32 i = 0; i < nl; i++) s[n++] = num[i];
        s[n] = '\0';
        font_draw_string(buf, (i32)stride, WIN_W, WIN_H, 6, 4, s, COL_WHITE);
    }
    for (i32 i = 0; i < g_lives && i < 6; i++) {
        i32 lx = WIN_W - 16 - i * 18;
        fill_circle(buf, stride, lx, HUD_H / 2, HUD_H / 2 - 4, COL_PAC);
    }

    draw_maze(buf, stride);
    for (i32 i = 0; i < NGHOSTS; i++) draw_ghost(buf, stride, i);
    draw_pacman(buf, stride);

    /* READY! banner on (re)spawn. */
    if (g_ready_ticks > 0 && !g_game_over && !g_win) {
        draw_centered(buf, stride, CELL_Y(MAZE_ROWS / 2 + 1), "READY!", COL_YELLOW);
    }

    /* Pause overlay. */
    if (g_paused && !g_game_over && !g_win) {
        for (i32 y = HUD_H; y < WIN_H; y += 2)
            fill_rect(buf, stride, 0, y, WIN_W, 1, 0xBB000020u);
        draw_centered(buf, stride, WIN_H / 2 - 10, "PAUSED", COL_YELLOW);
        draw_centered(buf, stride, WIN_H / 2 + 10, "PRESS P TO RESUME", COL_WHITE);
    }

    /* Win overlay. */
    if (g_win) {
        for (i32 y = HUD_H; y < WIN_H; y += 2)
            fill_rect(buf, stride, 0, y, WIN_W, 1, 0xBB001000u);
        draw_centered(buf, stride, WIN_H / 2 - 30, "YOU WIN!", COL_GREEN);
        char s[40]; i32 n = 0; const char *pfx = "SCORE ";
        for (i32 i = 0; pfx[i]; i++) s[n++] = pfx[i];
        char num[12]; i32 nl = fmt_u32(num, g_score);
        for (i32 i = 0; i < nl; i++) s[n++] = num[i];
        s[n] = '\0';
        draw_centered(buf, stride, WIN_H / 2 - 6, s, COL_YELLOW);
        draw_centered(buf, stride, WIN_H / 2 + 18, "PRESS R TO PLAY AGAIN", COL_WHITE);
    }

    /* Game-over overlay. */
    if (g_game_over) {
        for (i32 y = HUD_H; y < WIN_H; y += 2)
            fill_rect(buf, stride, 0, y, WIN_W, 1, 0xBB000000u);
        draw_centered(buf, stride, WIN_H / 2 - 30, "GAME OVER", COL_RED);
        char s[40]; i32 n = 0; const char *pfx = "SCORE ";
        for (i32 i = 0; pfx[i]; i++) s[n++] = pfx[i];
        char num[12]; i32 nl = fmt_u32(num, g_score);
        for (i32 i = 0; i < nl; i++) s[n++] = num[i];
        s[n] = '\0';
        draw_centered(buf, stride, WIN_H / 2 - 6, s, COL_YELLOW);
        draw_centered(buf, stride, WIN_H / 2 + 18, "PRESS R TO RESTART", COL_WHITE);
    }
}

/* ============================================================
 *  Entry point
 * ============================================================ */
void _start(void)
{
    print("[PACMAN] starting\n");

    if (wl_connect() != 0) {
        print("[PACMAN] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Pac-Man");
    if (!win) {
        print("[PACMAN] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    u32 stride = win->stride / 4u;

    u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    game_init(now);
    g_want_dir = g_pac.dir;
    u64 last_step = now;

    #define STEP_MS 110u   /* ~9 fps grid cadence */

    for (;;) {
        u64 t = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);

        /* Drain input. */
        int kind, a, b, c_ev;
        while (wl_poll_event(win, &kind, &a, &b, &c_ev)) {
            if (kind != WL_EVENT_KEY || b != 1) continue;  /* key-down only */
            switch (a) {
            case KEY_ESC:
                print("[PACMAN] score "); print_u32(g_score); print("\n");
                sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
                break;
            case KEY_P:
                if (!g_game_over && !g_win) g_paused = !g_paused;
                break;
            case KEY_RIGHT: case KEY_D: g_want_dir = 0; break;
            case KEY_DOWN:  case KEY_S: g_want_dir = 1; break;
            case KEY_LEFT:  case KEY_A: g_want_dir = 2; break;
            case KEY_UP:    case KEY_W: g_want_dir = 3; break;
            case KEY_R:
            case KEY_SPACE: case KEY_ENTER:
                if (g_game_over || g_win) {
                    now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
                    game_init(now);
                    g_want_dir = g_pac.dir;
                    last_step = now;
                }
                break;
            }
        }

        /* Advance on the fixed grid cadence. */
        if (!g_paused && !g_game_over && !g_win && (t - last_step) >= STEP_MS) {
            i32 was_done = g_game_over || g_win;
            game_step();
            last_step = t;
            if (!was_done && (g_game_over || g_win)) {
                print("[PACMAN] score "); print_u32(g_score); print("\n");
            }
        }

        render(win->pixels, stride);
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
