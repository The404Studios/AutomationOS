/*
 * g2048.c -- Polished 2048 sliding-tile game (freestanding, ring 3).
 * ==================================================================
 *
 * 400x500 window:
 *   60px header  : title "2048", SCORE + BEST score boxes
 *   400x400 grid : 4x4 tiles, 90x90px each with 10px gaps, 5px margin
 *   40px footer  : hint "Arrows/WASD | R=Restart"
 *
 * Features:
 *   - Arrow keys + WASD to slide tiles
 *   - Tile spawn: 2 (90%) or 4 (10%), seeded via SYS_GET_TICKS_MS xorshift32
 *   - Win at 2048, game-over when no moves remain
 *   - Score = sum of all merges; best score persists until process exits
 *   - R or Enter to restart; win overlay lets you keep playing (press R/Enter)
 *   - Rounded tiles colored by value; centered number text
 *
 * No libc: freestanding inline syscalls + wl_client + bitfont.
 *
 * Build (compile-check only -- do NOT run build_all.sh):
 *   CF="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2"
 *   gcc $CF -c userspace/apps/games/g2048/g2048.c -o /tmp/g2048.o
 *   gcc $CF -c userspace/lib/wl/wl_client.c        -o /tmp/wlc.o
 *   gcc $CF -c userspace/lib/font/bitfont.c         -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/g2048.o /tmp/wlc.o /tmp/bf.o -o /tmp/g2048.elf
 *   objdump -d /tmp/g2048.elf | grep fs:0x28   # MUST be empty
 *
 * build_all.sh lines for lead integrator (add to wl-direct apps section):
 *   build_wl_app userspace/apps/games/g2048/g2048.c  g2048
 *   ...and in the initrd cp loop:
 *   cp /tmp/g2048.elf /tmp/ird/sbin/g2048
 *
 * Dock entry suggestion:
 *   { "name": "2048", "exec": "/sbin/g2048", "color": 0xFFEDC22E }
 *
 * Serial output:
 *   [G2048] starting
 *   [G2048] score N best N (game over / restart)
 */

#include "../../../lib/wl/wl_client.h"
#include "../../../lib/font/bitfont.h"

/* ------------------------------------------------------------------ */
/* Syscall numbers (AOS -- NOT Linux)                                   */
/* ------------------------------------------------------------------ */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

/* ------------------------------------------------------------------ */
/* Key codes (kernel/include/input.h)                                   */
/* ------------------------------------------------------------------ */
#define KEY_UP     103
#define KEY_DOWN   108
#define KEY_LEFT   105
#define KEY_RIGHT  106
#define KEY_W       17
#define KEY_A       30
#define KEY_S       31
#define KEY_D       32
#define KEY_R       19
#define KEY_ENTER   28

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;

/* ------------------------------------------------------------------ */
/* Inline syscall (6-arg, no fs:0x28 canary)                           */
/* ------------------------------------------------------------------ */
static inline long sc6(long n, long a1, long a2, long a3,
                        long a4, long a5, long a6)
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

/* ------------------------------------------------------------------ */
/* Serial helpers                                                        */
/* ------------------------------------------------------------------ */
static u32 g2_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void g2_print(const char *m) { sc6(SYS_WRITE, 1, (long)m, (long)g2_strlen(m), 0, 0, 0); }

static void g2_print_u32(u32 v)
{
    char b[12]; i32 i = 0;
    if (v == 0) { sc6(SYS_WRITE, 1, (long)"0", 1, 0, 0, 0); return; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (i32 j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = '\0';
    sc6(SYS_WRITE, 1, (long)b, i, 0, 0, 0);
}

/* Format u32 into buf (NUL-terminated), return char count. */
static i32 g2_fmt(char *buf, u32 v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12]; i32 i = 0;
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    for (i32 j = 0; j < i; j++) buf[j] = tmp[i-1-j];
    buf[i] = '\0';
    return i;
}

/* ------------------------------------------------------------------ */
/* Window / layout                                                       */
/* ------------------------------------------------------------------ */
#define WIN_W     400
#define WIN_H     500

#define HEADER_H   60   /* title + score strip */
#define FOOTER_H   40   /* hint strip */
#define GRID_SIDE  400  /* full-width square grid area */

#define NCOLS   4
#define NROWS   4
#define TILE_W  90
#define TILE_H  90
#define TILE_GAP 10
/* Grid origin: centered horizontally, just below header.
 * total grid width = 4*90 + 3*10 = 390; left margin = (400-390)/2 = 5 */
#define GRID_X0  5
#define GRID_Y0  (HEADER_H + 5)

#define TILE_PX(col) (GRID_X0 + (col) * (TILE_W + TILE_GAP))
#define TILE_PY(row) (GRID_Y0 + (row) * (TILE_H + TILE_GAP))

/* ------------------------------------------------------------------ */
/* Colours (ARGB32)                                                     */
/* ------------------------------------------------------------------ */
#define C_BG           0xFFFAF8EFu  /* warm off-white background       */
#define C_HEADER_BG    0xFF776E65u  /* dark brown header                */
#define C_GRID_BG      0xFFBBADA0u  /* grid surround                   */
#define C_EMPTY        0xFFCDC1B4u  /* empty cell                      */
#define C_WHITE        0xFFFFFFFFu
#define C_DARK_TEXT    0xFF776E65u  /* text on light tiles (2, 4)       */
#define C_LIGHT_TEXT   0xFFF9F6F2u  /* text on dark tiles               */
#define C_GOLD         0xFFEDC22Eu
#define C_FOOTER_BG    0xFF8F7A6Au

/* Tile colour table: index = log2(value), capped at 11 */
static const u32 TILE_CLR[12] = {
    0xFFCDC1B4u,  /* 0    empty        */
    0xFFEEE4DAu,  /* 2    pale cream   */
    0xFFEDE0C8u,  /* 4    warm tan     */
    0xFFF2B179u,  /* 8    orange       */
    0xFFF59563u,  /* 16   mid-orange   */
    0xFFF67C5Fu,  /* 32   coral        */
    0xFFF65E3Bu,  /* 64   red-orange   */
    0xFFEDCF72u,  /* 128  gold         */
    0xFFEDCC61u,  /* 256  bright gold  */
    0xFFEDC850u,  /* 512  deep gold    */
    0xFFEDC53Fu,  /* 1024 sunflower    */
    0xFFEDC22Eu,  /* 2048 rich gold    */
};

/* ------------------------------------------------------------------ */
/* xorshift32 PRNG (seeded with SYS_GET_TICKS_MS)                      */
/* ------------------------------------------------------------------ */
static u32 g_rng;

static u32 xs32(void)
{
    u32 x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

/* ------------------------------------------------------------------ */
/* Game state                                                           */
/* ------------------------------------------------------------------ */
#define ST_PLAY  0
#define ST_WIN   1
#define ST_OVER  2

static u32 g_board[NROWS][NCOLS];
static u32 g_score;
static u32 g_best;
static i32 g_state;
static i32 g_win_acked; /* 1 after player dismisses win screen */

/* ------------------------------------------------------------------ */
/* Game logic helpers                                                   */
/* ------------------------------------------------------------------ */
static i32 count_empty(void)
{
    i32 n = 0;
    for (i32 r = 0; r < NROWS; r++)
        for (i32 c = 0; c < NCOLS; c++)
            if (g_board[r][c] == 0) n++;
    return n;
}

static void spawn_tile(void)
{
    i32 empty = count_empty();
    if (empty == 0) return;
    i32 target = (i32)(xs32() % (u32)empty);
    u32 val    = ((xs32() % 10u) < 9u) ? 2u : 4u;
    i32 seen   = 0;
    for (i32 r = 0; r < NROWS; r++)
        for (i32 c = 0; c < NCOLS; c++)
            if (g_board[r][c] == 0) {
                if (seen == target) { g_board[r][c] = val; return; }
                seen++;
            }
}

static i32 has_moves(void)
{
    for (i32 r = 0; r < NROWS; r++)
        for (i32 c = 0; c < NCOLS; c++) {
            if (g_board[r][c] == 0) return 1;
            if (r+1 < NROWS && g_board[r][c] == g_board[r+1][c]) return 1;
            if (c+1 < NCOLS && g_board[r][c] == g_board[r][c+1]) return 1;
        }
    return 0;
}

/* Slide/merge one row of 4 left. Returns 1 if changed. */
static i32 slide_left(u32 row[4], u32 *delta)
{
    u32 orig[4], tmp[4], res[4];
    for (i32 i = 0; i < 4; i++) { orig[i] = row[i]; tmp[i] = 0; res[i] = 0; }

    /* pack non-zero left */
    i32 pos = 0;
    for (i32 i = 0; i < 4; i++)
        if (row[i]) tmp[pos++] = row[i];

    /* merge */
    for (i32 i = 0; i < 3; i++) {
        if (tmp[i] && tmp[i] == tmp[i+1]) {
            tmp[i] *= 2;
            *delta += tmp[i];
            tmp[i+1] = 0;
            i++;
        }
    }

    /* pack result */
    pos = 0;
    for (i32 i = 0; i < 4; i++)
        if (tmp[i]) res[pos++] = tmp[i];

    i32 changed = 0;
    for (i32 i = 0; i < 4; i++) {
        row[i] = res[i];
        if (row[i] != orig[i]) changed = 1;
    }
    return changed;
}

#define DIR_LEFT  0
#define DIR_RIGHT 1
#define DIR_UP    2
#define DIR_DOWN  3

static i32 do_move(i32 dir)
{
    i32 moved = 0;
    u32 delta = 0;

    if (dir == DIR_LEFT) {
        for (i32 r = 0; r < NROWS; r++) {
            u32 row[4] = { g_board[r][0], g_board[r][1], g_board[r][2], g_board[r][3] };
            if (slide_left(row, &delta)) {
                moved = 1;
                for (i32 c = 0; c < NCOLS; c++) g_board[r][c] = row[c];
            }
        }
    } else if (dir == DIR_RIGHT) {
        for (i32 r = 0; r < NROWS; r++) {
            u32 row[4] = { g_board[r][3], g_board[r][2], g_board[r][1], g_board[r][0] };
            if (slide_left(row, &delta)) {
                moved = 1;
                g_board[r][0]=row[3]; g_board[r][1]=row[2];
                g_board[r][2]=row[1]; g_board[r][3]=row[0];
            }
        }
    } else if (dir == DIR_UP) {
        for (i32 c = 0; c < NCOLS; c++) {
            u32 row[4] = { g_board[0][c], g_board[1][c], g_board[2][c], g_board[3][c] };
            if (slide_left(row, &delta)) {
                moved = 1;
                for (i32 r = 0; r < NROWS; r++) g_board[r][c] = row[r];
            }
        }
    } else { /* DIR_DOWN */
        for (i32 c = 0; c < NCOLS; c++) {
            u32 row[4] = { g_board[3][c], g_board[2][c], g_board[1][c], g_board[0][c] };
            if (slide_left(row, &delta)) {
                moved = 1;
                g_board[0][c]=row[3]; g_board[1][c]=row[2];
                g_board[2][c]=row[1]; g_board[3][c]=row[0];
            }
        }
    }

    if (moved) {
        g_score += delta;
        if (g_score > g_best) g_best = g_score;
        spawn_tile();
        if (g_state == ST_PLAY)
            for (i32 r = 0; r < NROWS; r++)
                for (i32 c = 0; c < NCOLS; c++)
                    if (g_board[r][c] >= 2048) { g_state = ST_WIN; return 1; }
        if (g_state == ST_PLAY && !has_moves())
            g_state = ST_OVER;
    }
    return moved;
}

static void game_init(u64 seed)
{
    u32 s = (u32)(seed ^ (seed >> 32));
    if (s == 0) s = 0xDEADBEEFu;
    g_rng = s;
    /* extra xorshift mix */
    xs32(); xs32();

    for (i32 r = 0; r < NROWS; r++)
        for (i32 c = 0; c < NCOLS; c++)
            g_board[r][c] = 0;

    g_score    = 0;
    g_state    = ST_PLAY;
    g_win_acked = 0;

    spawn_tile();
    spawn_tile();
}

/* ------------------------------------------------------------------ */
/* Drawing helpers                                                      */
/* ------------------------------------------------------------------ */
static void fill_rect(u32 *buf, i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x, x2 = x+w > WIN_W ? WIN_W : x+w;
    i32 y1 = y < 0 ? 0 : y, y2 = y+h > WIN_H ? WIN_H : y+h;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * WIN_W;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = col;
    }
}

/*
 * Approximated rounded rect: fill center band + inner top/bottom strips,
 * leaving r*r corner squares undrawn against the background.
 */
static void fill_rr(u32 *buf, i32 x, i32 y, i32 w, i32 h, u32 col, i32 r)
{
    if (r < 1) { fill_rect(buf, x, y, w, h, col); return; }
    /* vertical center band */
    fill_rect(buf, x,   y+r, w,   h-2*r, col);
    /* top horizontal strip (inner width) */
    fill_rect(buf, x+r, y,   w-2*r, r,   col);
    /* bottom horizontal strip */
    fill_rect(buf, x+r, y+h-r, w-2*r, r, col);
}

/* Map tile value to colour-table index (log2, capped at 11). */
static i32 val_idx(u32 v)
{
    if (v == 0) return 0;
    i32 idx = 0;
    u32 x = v;
    while (x > 1) { x >>= 1; idx++; }
    return idx > 11 ? 11 : idx;
}

/* Draw horizontally-centered text within a box (tx,ty,tw,th). */
static void draw_centered(u32 *buf, i32 tx, i32 ty, i32 tw, i32 th,
                           const char *s, u32 col)
{
    i32 tw_px = font_text_width(s);
    i32 x = tx + (tw - tw_px) / 2;
    i32 y = ty + (th - FONT_H) / 2;
    font_draw_string(buf, WIN_W, WIN_W, WIN_H, x, y, s, col);
}

/* Draw a score box at (bx,by,bw,bh) with a label row and a value row. */
static void draw_score_box(u32 *buf, i32 bx, i32 by, i32 bw, i32 bh,
                            const char *label, u32 val)
{
    fill_rr(buf, bx, by, bw, bh, 0xFF9E9285u, 4);
    /* label (small, near top) */
    {
        i32 lw = font_text_width(label);
        font_draw_string(buf, WIN_W, WIN_W, WIN_H,
                         bx + (bw - lw)/2, by + 4,
                         label, 0xFFEEE4DAu);
    }
    /* value (centered in lower 2/3) */
    {
        char vbuf[12];
        g2_fmt(vbuf, val);
        i32 vw = font_text_width(vbuf);
        font_draw_string(buf, WIN_W, WIN_W, WIN_H,
                         bx + (bw - vw)/2, by + FONT_H + 6,
                         vbuf, C_WHITE);
    }
}

/* ------------------------------------------------------------------ */
/* Render                                                               */
/* ------------------------------------------------------------------ */
static void render(u32 *buf)
{
    /* ---- 1. Background ---- */
    fill_rect(buf, 0, 0, WIN_W, WIN_H, C_BG);

    /* ---- 2. Header ---- */
    fill_rect(buf, 0, 0, WIN_W, HEADER_H, C_HEADER_BG);

    /* Title "2048" left-aligned */
    font_draw_string(buf, WIN_W, WIN_W, WIN_H,
                     10, (HEADER_H - FONT_H*2) / 2, "2048", C_GOLD);
    /* scale-2 effect: duplicate one pixel right and one down */
    font_draw_string(buf, WIN_W, WIN_W, WIN_H,
                     11, (HEADER_H - FONT_H*2) / 2 + 1, "2048", C_GOLD);

    /* Score + Best boxes right-aligned */
    {
        i32 box_w = 78, box_h = 44;
        i32 box_y = (HEADER_H - box_h) / 2;
        /* BEST box */
        draw_score_box(buf, WIN_W - 10 - box_w,       box_y, box_w, box_h, "BEST",  g_best);
        /* SCORE box */
        draw_score_box(buf, WIN_W - 10 - 2*box_w - 8, box_y, box_w, box_h, "SCORE", g_score);
    }

    /* ---- 3. Grid surround ---- */
    fill_rr(buf,
            GRID_X0 - 5, GRID_Y0 - 5,
            NROWS * (TILE_W + TILE_GAP) + 5,
            NCOLS * (TILE_H + TILE_GAP) + 5,
            C_GRID_BG, 6);

    /* ---- 4. Tiles ---- */
    for (i32 r = 0; r < NROWS; r++) {
        for (i32 c = 0; c < NCOLS; c++) {
            u32 v  = g_board[r][c];
            i32 ix = val_idx(v);
            i32 px = TILE_PX(c), py = TILE_PY(r);

            fill_rr(buf, px, py, TILE_W, TILE_H, TILE_CLR[ix], 8);

            if (v) {
                char vs[8];
                g2_fmt(vs, v);
                u32 tc = (ix <= 2) ? C_DARK_TEXT : C_LIGHT_TEXT;
                draw_centered(buf, px, py, TILE_W, TILE_H, vs, tc);
            }
        }
    }

    /* ---- 5. Footer hint ---- */
    fill_rect(buf, 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H, C_FOOTER_BG);
    {
        const char *hint = "Arrows/WASD  |  R=Restart";
        i32 hw = font_text_width(hint);
        font_draw_string(buf, WIN_W, WIN_W, WIN_H,
                         (WIN_W - hw) / 2, WIN_H - FOOTER_H + (FOOTER_H - FONT_H)/2,
                         hint, 0xFFDDD0C8u);
    }

    /* ---- 6. Win overlay ---- */
    if (g_state == ST_WIN && !g_win_acked) {
        /* stipple overlay (even rows) */
        for (i32 y = GRID_Y0; y < WIN_H - FOOTER_H; y += 2)
            fill_rect(buf, 0, y, WIN_W, 1, 0xCC8F7A50u);

        const char *l1 = "YOU WIN!";
        const char *l2 = "Press R to keep playing";
        const char *l3 = "or Enter to restart";
        i32 cy = WIN_H / 2 - 28;
        draw_centered(buf, 0, cy,      WIN_W, FONT_H, l1, 0xFFFFD700u);
        draw_centered(buf, 0, cy + 22, WIN_W, FONT_H, l2, C_WHITE);
        draw_centered(buf, 0, cy + 38, WIN_W, FONT_H, l3, C_WHITE);
    }

    /* ---- 7. Game-over overlay ---- */
    if (g_state == ST_OVER) {
        for (i32 y = GRID_Y0; y < WIN_H - FOOTER_H; y += 2)
            fill_rect(buf, 0, y, WIN_W, 1, 0xBB111111u);

        const char *l1 = "GAME OVER";
        char sl[32];
        {
            const char *pfx = "Score: ";
            i32 n = 0;
            for (i32 i = 0; pfx[i]; i++) sl[n++] = pfx[i];
            char nv[12]; i32 nl = g2_fmt(nv, g_score);
            for (i32 i = 0; i < nl; i++) sl[n++] = nv[i];
            sl[n] = '\0';
        }
        const char *l3 = "R or Enter to restart";
        i32 cy = WIN_H / 2 - 32;
        draw_centered(buf, 0, cy,      WIN_W, FONT_H, l1, 0xFFFF6B6Bu);
        draw_centered(buf, 0, cy + 22, WIN_W, FONT_H, sl, 0xFFFFD700u);
        draw_centered(buf, 0, cy + 44, WIN_W, FONT_H, l3, C_WHITE);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                                */
/* ------------------------------------------------------------------ */
void _start(void)
{
    g2_print("[G2048] starting\n");

    if (wl_connect() != 0) {
        g2_print("[G2048] wl_connect FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "2048");
    if (!win) {
        g2_print("[G2048] wl_create_window FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    u64 now = (u64)sc6(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    g_best  = 0;
    game_init(now);

    for (;;) {
        int kind, a, b, cev;
        while (wl_poll_event(win, &kind, &a, &b, &cev)) {
            if (kind != WL_EVENT_KEY || b != 1) continue; /* key-down only */

            /* Restart / dismiss win overlay */
            if (a == KEY_R || a == KEY_ENTER) {
                if (g_state == ST_WIN && !g_win_acked) {
                    /* First press: dismiss overlay, keep playing */
                    g_win_acked = 1;
                    g_state     = ST_PLAY;
                } else {
                    /* Full restart (R always forces fresh game) */
                    g2_print("[G2048] score ");
                    g2_print_u32(g_score);
                    g2_print(" best ");
                    g2_print_u32(g_best);
                    g2_print("\n");
                    now = (u64)sc6(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
                    game_init(now);
                }
                continue;
            }

            if (g_state != ST_PLAY) continue;

            i32 dir = -1;
            switch (a) {
            case KEY_LEFT:  case KEY_A: dir = DIR_LEFT;  break;
            case KEY_RIGHT: case KEY_D: dir = DIR_RIGHT; break;
            case KEY_UP:    case KEY_W: dir = DIR_UP;    break;
            case KEY_DOWN:  case KEY_S: dir = DIR_DOWN;  break;
            }
            if (dir >= 0) do_move(dir);
        }

        render(win->pixels);
        wl_commit(win);
        sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
