/*
 * game2048.c -- 2048 sliding-tile game (freestanding, ring 3).
 * =============================================================
 *
 * A 420x500 window: 50px header (score), 420x420 grid area (4x4 tiles,
 * each 100x100px with 8px gaps), and a 30px bottom strip.
 *
 * Controls:
 *   Arrow keys / WASD : slide tiles
 *   R / Enter         : restart
 *
 * No libc: freestanding inline syscalls + wl_client + bitfont.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/game2048/game2048.c -o /tmp/g2048.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c     -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c     -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/g2048.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/g2048.elf
 *   objdump -d /tmp/g2048.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [2048] starting
 *   [2048] score N (game over)
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* ---- syscall numbers ---- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

/* ---- key codes (from kernel/include/input.h) ---- */
#define KEY_UP    103
#define KEY_DOWN  108
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_W      17
#define KEY_A      30
#define KEY_S      31
#define KEY_D      32
#define KEY_R      19
#define KEY_ENTER  28

/* ---- types ---- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;

/* ---- inline syscall ---- */
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
    char b[12];
    i32 i = 0;
    if (v == 0) { sc(SYS_WRITE, 1, (long)"0", 1, 0, 0, 0); return; }
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (i32 j = 0; j < i / 2; j++) {
        char tmp = b[j]; b[j] = b[i-1-j]; b[i-1-j] = tmp;
    }
    b[i] = '\0';
    sc(SYS_WRITE, 1, (long)b, i, 0, 0, 0);
}

/* Format u32 into buf (NUL-terminated), return length. */
static i32 fmt_u32(char *buf, u32 v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    i32 i = 0;
    char tmp[12];
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (i32 j = 0; j < i; j++) buf[j] = tmp[i-1-j];
    buf[i] = '\0';
    return i;
}

/* ---- window / layout constants ---- */
#define WIN_W       420
#define WIN_H       500

#define HEADER_H     50          /* score / title header strip                */
#define FOOTER_H     30          /* bottom strip                              */
#define GRID_AREA    420         /* square grid area height/width             */
#define GRID_COLS    4
#define GRID_ROWS    4
#define TILE_GAP     10          /* gap between tiles (px)                    */
/* Each tile: (420 - 5*gap) / 4 = (420 - 50) / 4 = 92.5 → use 92px tiles
   with adjusted gaps to fit: 5*10 = 50, 4*92 = 368, total = 418 → shift by 1
   Let's do: margin=11, gap=10, tile=92. 11 + 4*92 + 3*10 = 11+368+30 = 409 → nope
   Clean: margin=9, tile=93, gap=9: 9 + 4*93 + 3*9 = 9+372+27 = 408 → no
   Easiest: tile=96, gap=8, margin=4: 4 + 4*96 + 3*8 = 4+384+24 = 412 → no
   Best fit: gap=10, tile=92, side_margin=9: 9+9 + 4*92 + 3*10 = 18+368+30=416 → close
   Use: GRID_X0=8, tile=96, gap=8: 8 + 4*96 + 3*8 = 8+384+24 = 416 → no
   Simple: GRID_X0=10, tile=90, gap=10: 10 + 4*90 + 3*10 = 10+360+30 = 400 → 20 left
   Simplest clean look: GRID_X0=10, tile=90, gap=10 (total used=400, center with 10 margin each side)
*/
#define TILE_W       90          /* tile width in pixels                      */
#define TILE_H       90          /* tile height in pixels                     */
#define GRID_X0      10          /* left margin of grid                       */
#define GRID_Y0      (HEADER_H + 10)  /* top of grid (below header + 10px pad) */

/* Pixel origin of tile (col, row) */
#define TILE_PX(col) (GRID_X0 + (col) * (TILE_W + TILE_GAP))
#define TILE_PY(row) (GRID_Y0 + (row) * (TILE_H + TILE_GAP))

/* ---- colours ---- */
#define COL_BG          0xFFBBADA0u   /* classic 2048 beige background      */
#define COL_HEADER_BG   0xFF776E65u   /* dark header strip                  */
#define COL_GRID_BG     0xFFCDC1B4u   /* empty cell colour                  */
#define COL_WHITE       0xFFFFFFFFu
#define COL_DARK_TEXT   0xFF776E65u   /* dark text for light tiles          */
#define COL_LIGHT_TEXT  0xFFF9F6F2u   /* light text for dark tiles          */
#define COL_OVERLAY     0xCC000000u   /* semi-opaque black overlay (approx) */
#define COL_WIN_OVERLAY 0xCC8F7A50u   /* gold-ish win overlay               */

/* Tile colours by value index (0=empty, 1=2, 2=4, 3=8, ... 11=2048) */
static const u32 TILE_COLORS[12] = {
    0xFFCDC1B4u,   /* 0     = empty         */
    0xFFEEE4DAu,   /* 2     = pale cream    */
    0xFFEDE0C8u,   /* 4     = warm tan      */
    0xFFF2B179u,   /* 8     = orange        */
    0xFFF59563u,   /* 16    = medium orange */
    0xFFF67C5Fu,   /* 32    = coral         */
    0xFFF65E3Bu,   /* 64    = red-orange    */
    0xFFEDCF72u,   /* 128   = gold          */
    0xFFEDCC61u,   /* 256   = bright gold   */
    0xFFEDC850u,   /* 512   = deep gold     */
    0xFFEDC53Fu,   /* 1024  = sunflower     */
    0xFFEDC22Eu,   /* 2048  = rich gold     */
};

/* Text colours: tiles <= 4 use dark text, rest use light */
static u32 tile_text_color(i32 val_idx)
{
    return (val_idx <= 2) ? COL_DARK_TEXT : COL_LIGHT_TEXT;
}

/* ---- LCG random ---- */
static u32 g_rand_state = 12345u;

static u32 lcg_rand(void)
{
    g_rand_state = g_rand_state * 1664525u + 1013904223u;
    return g_rand_state;
}

/* ---- game state ---- */
#define GAME_PLAYING  0
#define GAME_WIN      1
#define GAME_OVER     2

static u32 g_board[GRID_ROWS][GRID_COLS];  /* tile values (0 = empty) */
static u32 g_score;
static i32 g_state;          /* GAME_PLAYING / GAME_WIN / GAME_OVER */
static i32 g_win_acked;      /* set when player dismisses win screen */

/* ---- helpers ---- */

/* Count empty cells. */
static i32 count_empty(void)
{
    i32 n = 0;
    for (i32 r = 0; r < GRID_ROWS; r++)
        for (i32 c = 0; c < GRID_COLS; c++)
            if (g_board[r][c] == 0) n++;
    return n;
}

/* Spawn a 2 (90%) or 4 (10%) in a random empty cell. */
static void spawn_tile(void)
{
    i32 empty = count_empty();
    if (empty == 0) return;

    /* Pick a random empty slot index. */
    i32 target = (i32)(lcg_rand() % (u32)empty);
    i32 val = ((lcg_rand() % 10u) < 9u) ? 2u : 4u;

    i32 seen = 0;
    for (i32 r = 0; r < GRID_ROWS; r++) {
        for (i32 c = 0; c < GRID_COLS; c++) {
            if (g_board[r][c] == 0) {
                if (seen == target) {
                    g_board[r][c] = (u32)val;
                    return;
                }
                seen++;
            }
        }
    }
}

/* Check if any moves are possible (any empty cell or any adjacent merge). */
static i32 has_moves(void)
{
    for (i32 r = 0; r < GRID_ROWS; r++) {
        for (i32 c = 0; c < GRID_COLS; c++) {
            if (g_board[r][c] == 0) return 1;
            if (r + 1 < GRID_ROWS && g_board[r][c] == g_board[r+1][c]) return 1;
            if (c + 1 < GRID_COLS && g_board[r][c] == g_board[r][c+1]) return 1;
        }
    }
    return 0;
}

/* ---- slide logic ---- */

/*
 * Slide and merge a single row of 4 values towards index 0.
 * Returns 1 if anything moved or merged, 0 otherwise.
 * Accumulates merge score into *score_delta.
 */
static i32 slide_row_left(u32 row[4], u32 *score_delta)
{
    u32 orig[4];
    for (i32 i = 0; i < 4; i++) orig[i] = row[i];

    /* Pack non-zero values to the left. */
    u32 tmp[4] = {0, 0, 0, 0};
    i32 pos = 0;
    for (i32 i = 0; i < 4; i++)
        if (row[i] != 0) tmp[pos++] = row[i];

    /* Merge adjacent equal values. */
    for (i32 i = 0; i < 3; i++) {
        if (tmp[i] != 0 && tmp[i] == tmp[i+1]) {
            tmp[i] *= 2;
            *score_delta += tmp[i];
            tmp[i+1] = 0;
            i++;  /* skip merged cell */
        }
    }

    /* Pack again to remove holes from merges. */
    u32 result[4] = {0, 0, 0, 0};
    pos = 0;
    for (i32 i = 0; i < 4; i++)
        if (tmp[i] != 0) result[pos++] = tmp[i];

    /* Copy back and check if anything changed. */
    i32 changed = 0;
    for (i32 i = 0; i < 4; i++) {
        row[i] = result[i];
        if (row[i] != orig[i]) changed = 1;
    }
    return changed;
}

/* Direction enum */
#define DIR_LEFT  0
#define DIR_RIGHT 1
#define DIR_UP    2
#define DIR_DOWN  3

/*
 * Perform a full board slide in direction d.
 * Returns 1 if the board changed, 0 if no move was possible.
 */
static i32 do_move(i32 d)
{
    i32 moved = 0;
    u32 score_delta = 0;

    if (d == DIR_LEFT) {
        for (i32 r = 0; r < GRID_ROWS; r++) {
            u32 row[4] = { g_board[r][0], g_board[r][1],
                           g_board[r][2], g_board[r][3] };
            if (slide_row_left(row, &score_delta)) {
                moved = 1;
                for (i32 c = 0; c < GRID_COLS; c++) g_board[r][c] = row[c];
            }
        }
    } else if (d == DIR_RIGHT) {
        for (i32 r = 0; r < GRID_ROWS; r++) {
            /* Reverse, slide left, reverse back. */
            u32 row[4] = { g_board[r][3], g_board[r][2],
                           g_board[r][1], g_board[r][0] };
            if (slide_row_left(row, &score_delta)) {
                moved = 1;
                g_board[r][0] = row[3]; g_board[r][1] = row[2];
                g_board[r][2] = row[1]; g_board[r][3] = row[0];
            }
        }
    } else if (d == DIR_UP) {
        for (i32 c = 0; c < GRID_COLS; c++) {
            u32 row[4] = { g_board[0][c], g_board[1][c],
                           g_board[2][c], g_board[3][c] };
            if (slide_row_left(row, &score_delta)) {
                moved = 1;
                for (i32 r = 0; r < GRID_ROWS; r++) g_board[r][c] = row[r];
            }
        }
    } else { /* DIR_DOWN */
        for (i32 c = 0; c < GRID_COLS; c++) {
            u32 row[4] = { g_board[3][c], g_board[2][c],
                           g_board[1][c], g_board[0][c] };
            if (slide_row_left(row, &score_delta)) {
                moved = 1;
                g_board[0][c] = row[3]; g_board[1][c] = row[2];
                g_board[2][c] = row[1]; g_board[3][c] = row[0];
            }
        }
    }

    if (moved) {
        g_score += score_delta;
        spawn_tile();

        /* Check win (2048 tile present). */
        if (g_state == GAME_PLAYING) {
            for (i32 r = 0; r < GRID_ROWS; r++)
                for (i32 c = 0; c < GRID_COLS; c++)
                    if (g_board[r][c] >= 2048) g_state = GAME_WIN;
        }

        /* Check game over (no moves left). */
        if (g_state == GAME_PLAYING && !has_moves())
            g_state = GAME_OVER;
    }

    return moved;
}

/* ---- game init ---- */
static void game_init(u64 ticks)
{
    g_rand_state = (u32)(ticks ^ (ticks >> 16));
    if (g_rand_state == 0) g_rand_state = 0xCAFEBABEu;

    for (i32 r = 0; r < GRID_ROWS; r++)
        for (i32 c = 0; c < GRID_COLS; c++)
            g_board[r][c] = 0;

    g_score    = 0;
    g_state    = GAME_PLAYING;
    g_win_acked = 0;

    /* Start with two tiles. */
    spawn_tile();
    spawn_tile();
}

/* ---- draw helpers ---- */

static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > WIN_W) x2 = WIN_W;
    i32 y2 = y + h; if (y2 > WIN_H) y2 = WIN_H;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/*
 * Draw a rounded rectangle by leaving 4px corner squares empty.
 * This gives a simple "rounded" appearance without heavy math.
 */
static void fill_rounded_rect(u32 *buf, u32 stride_px,
                               i32 x, i32 y, i32 w, i32 h,
                               u32 color, i32 radius)
{
    if (radius < 1) {
        fill_rect(buf, stride_px, x, y, w, h, color);
        return;
    }
    /* Center rectangle (full width, inner height). */
    fill_rect(buf, stride_px, x,          y + radius, w,          h - 2*radius, color);
    /* Top strip (inner width). */
    fill_rect(buf, stride_px, x + radius, y,          w - 2*radius, radius,     color);
    /* Bottom strip (inner width). */
    fill_rect(buf, stride_px, x + radius, y + h - radius, w - 2*radius, radius, color);
}

/*
 * Map a tile value to a colour-table index (capped at index 11 for >=2048).
 * 0 → 0, 2 → 1, 4 → 2, 8 → 3, 16 → 4, ..., 2048 → 11
 */
static i32 val_to_idx(u32 v)
{
    if (v == 0) return 0;
    i32 idx = 0;
    u32 x = v;
    while (x > 1) { x >>= 1; idx++; }  /* log2 */
    if (idx > 11) idx = 11;
    return idx;
}

/* Draw a centered string inside a tile at pixel rect (tx, ty, tw, th). */
static void draw_centered_str(u32 *buf, u32 stride_px,
                               i32 tx, i32 ty, i32 tw, i32 th,
                               const char *s, u32 color)
{
    i32 tw_text = font_text_width(s);
    i32 x = tx + (tw - tw_text) / 2;
    i32 y = ty + (th - FONT_H) / 2;
    font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H, x, y, s, color);
}

/* ---- render ---- */
static void render(u32 *buf, u32 stride_px)
{
    /* 1. Background */
    fill_rect(buf, stride_px, 0, 0, WIN_W, WIN_H, COL_BG);

    /* 2. Header strip */
    fill_rect(buf, stride_px, 0, 0, WIN_W, HEADER_H, COL_HEADER_BG);

    /* Title "2048" on left */
    {
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         8, (HEADER_H - FONT_H) / 2, "2048", COL_WHITE);
    }

    /* Score on right area */
    {
        char score_str[32];
        const char *prefix = "SCORE: ";
        i32 n = 0;
        for (i32 i = 0; prefix[i]; i++) score_str[n++] = prefix[i];
        char num[12];
        i32 nlen = fmt_u32(num, g_score);
        for (i32 i = 0; i < nlen; i++) score_str[n++] = num[i];
        score_str[n] = '\0';
        i32 sw = font_text_width(score_str);
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         WIN_W - sw - 8, (HEADER_H - FONT_H) / 2,
                         score_str, COL_WHITE);
    }

    /* 3. Grid background */
    fill_rounded_rect(buf, stride_px,
                      GRID_X0 - 4, GRID_Y0 - 4,
                      WIN_W - 2*(GRID_X0-4), GRID_ROWS*(TILE_H+TILE_GAP)+4,
                      COL_GRID_BG, 4);

    /* 4. Draw each tile */
    for (i32 r = 0; r < GRID_ROWS; r++) {
        for (i32 c = 0; c < GRID_COLS; c++) {
            u32 val  = g_board[r][c];
            i32 idx  = val_to_idx(val);
            u32 tclr = TILE_COLORS[idx];
            i32 px   = TILE_PX(c);
            i32 py   = TILE_PY(r);

            fill_rounded_rect(buf, stride_px, px, py, TILE_W, TILE_H, tclr, 6);

            if (val != 0) {
                /* Format value string. */
                char vstr[8];
                fmt_u32(vstr, val);
                u32 tcolor = tile_text_color(idx);
                draw_centered_str(buf, stride_px, px, py, TILE_W, TILE_H, vstr, tcolor);
            }
        }
    }

    /* 5. Win overlay */
    if (g_state == GAME_WIN && !g_win_acked) {
        /* Stipple overlay (every-other-row) for transparency effect */
        for (i32 y = GRID_Y0; y < WIN_H; y += 2)
            fill_rect(buf, stride_px, 0, y, WIN_W, 1, 0xBB8F7A50u);

        const char *msg1 = "YOU WIN!";
        const char *msg2 = "Press R or Enter";
        const char *msg3 = "to continue";
        i32 cy = WIN_H / 2 - 30;
        i32 w1 = font_text_width(msg1);
        i32 w2 = font_text_width(msg2);
        i32 w3 = font_text_width(msg3);
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         (WIN_W - w1)/2, cy,      msg1, 0xFFFFD700u);
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         (WIN_W - w2)/2, cy + 20, msg2, COL_WHITE);
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         (WIN_W - w3)/2, cy + 36, msg3, COL_WHITE);
    }

    /* 6. Game over overlay */
    if (g_state == GAME_OVER) {
        for (i32 y = GRID_Y0; y < WIN_H; y += 2)
            fill_rect(buf, stride_px, 0, y, WIN_W, 1, 0xBB000000u);

        const char *msg1 = "GAME OVER";
        const char *msg2 = "Press R or Enter";
        const char *msg3 = "to restart";

        char score_line[32];
        const char *sp = "Score: ";
        i32 n = 0;
        for (i32 i = 0; sp[i]; i++) score_line[n++] = sp[i];
        char num[12];
        i32 nlen = fmt_u32(num, g_score);
        for (i32 i = 0; i < nlen; i++) score_line[n++] = num[i];
        score_line[n] = '\0';

        i32 cy  = WIN_H / 2 - 36;
        i32 w1  = font_text_width(msg1);
        i32 wsl = font_text_width(score_line);
        i32 w2  = font_text_width(msg2);
        i32 w3  = font_text_width(msg3);

        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         (WIN_W - w1)/2,  cy,      msg1,       0xFFFF6B6Bu);
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         (WIN_W - wsl)/2, cy + 20, score_line, 0xFFFFD700u);
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         (WIN_W - w2)/2,  cy + 40, msg2,       COL_WHITE);
        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         (WIN_W - w3)/2,  cy + 56, msg3,       COL_WHITE);
    }
}

/* ---- entry point ---- */
void _start(void)
{
    print("[2048] starting\n");

    if (wl_connect() != 0) {
        print("[2048] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "2048");
    if (!win) {
        print("[2048] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    u32 stride_px = win->stride / 4u;

    /* Seed RNG and init game. */
    u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    game_init(now);

    i32 prev_state = GAME_PLAYING;

    for (;;) {
        /* Drain input. */
        int kind, a, b, c_ev;
        while (wl_poll_event(win, &kind, &a, &b, &c_ev)) {
            if (kind != WL_EVENT_KEY || b != 1) continue;  /* key-down only */

            /* Restart key works in any state. */
            if (a == KEY_R || a == KEY_ENTER) {
                if (g_state != GAME_PLAYING || g_win_acked == 0) {
                    /* If on win screen, first press acknowledges the win
                     * and lets user keep playing; if game over or user
                     * presses R explicitly, fully restart. */
                    if (g_state == GAME_WIN && !g_win_acked) {
                        g_win_acked = 1;
                        g_state = GAME_PLAYING;
                    } else {
                        now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
                        game_init(now);
                        prev_state = GAME_PLAYING;
                    }
                }
                continue;
            }

            /* Tile moves only allowed while playing. */
            if (g_state != GAME_PLAYING) continue;

            i32 dir = -1;
            switch (a) {
            case KEY_LEFT:  case KEY_A: dir = DIR_LEFT;  break;
            case KEY_RIGHT: case KEY_D: dir = DIR_RIGHT; break;
            case KEY_UP:    case KEY_W: dir = DIR_UP;    break;
            case KEY_DOWN:  case KEY_S: dir = DIR_DOWN;  break;
            }

            if (dir >= 0) {
                do_move(dir);
                /* Print to serial on state change. */
                if (g_state != prev_state) {
                    if (g_state == GAME_OVER) {
                        print("[2048] score ");
                        print_u32(g_score);
                        print(" (game over)\n");
                    }
                    prev_state = g_state;
                }
            }
        }

        /* Render and commit. */
        render(win->pixels, stride_px);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
