/*
 * tetris.c -- Classic Tetris game (freestanding, ring 3).
 * ========================================================
 *
 * Fixed 360x640 window:
 *   Left panel : 10x20 playfield, each cell 24x24 px, 1px border.
 *   Right panel: score/level/lines + next-piece preview + hold-piece preview.
 *
 * Features:
 *   - All 7 tetrominoes (I O T S Z J L) with SRS-ish rotation + wall-kick.
 *   - Gravity timer (SYS_GET_TICKS_MS); speed increases per level.
 *   - Ghost piece (drop shadow).
 *   - Hold piece (KEY_H / KEY_C).
 *   - Pause (KEY_P).
 *   - Line-clear scoring (NES-style × level+1).
 *   - "GAME OVER — R to restart" overlay.
 *
 * Controls:
 *   Left / A       -- move left
 *   Right / D      -- move right
 *   Down / S       -- soft-drop
 *   Up / X         -- rotate CW
 *   Space          -- hard-drop
 *   H / C          -- hold piece
 *   P              -- pause / resume
 *   R              -- restart (any time; always on game-over)
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/tetris/tetris.c  -o /tmp/tetris.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/wl/wl_client.c    -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/font/bitfont.c    -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/tetris.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/tetris.elf
 *   objdump -d /tmp/tetris.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [TETRIS] starting
 *   [TETRIS] score N          (on game over)
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* ------------------------------------------------------------------ */
/* Syscall numbers                                                       */
/* ------------------------------------------------------------------ */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

/* ------------------------------------------------------------------ */
/* Key codes (kernel/include/input.h; Linux-compatible scancodes)       */
/* ------------------------------------------------------------------ */
#define KEY_UP     103
#define KEY_DOWN   108
#define KEY_LEFT   105
#define KEY_RIGHT  106
#define KEY_W       17
#define KEY_A       30
#define KEY_S       31
#define KEY_D       32
#define KEY_X       45
#define KEY_C       46
#define KEY_H       35
#define KEY_P       25
#define KEY_R       19
#define KEY_SPACE   57
#define KEY_ENTER   28
#define KEY_ESC      1

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;

/* ------------------------------------------------------------------ */
/* Inline syscall (6-arg form; no fs:0x28 canary)                       */
/* ------------------------------------------------------------------ */
static inline long sc(long n, long a1, long a2, long a3,
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
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

/* Minimal itoa for non-negative integers (decimal). */
static void itoa_buf(u32 v, char *buf, int *len)
{
    char tmp[12];
    int  i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } }
    *len = i;
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void print_num(const char *prefix, u32 v)
{
    char buf[64];
    int  idx = 0;
    while (prefix[idx]) { buf[idx] = prefix[idx]; idx++; }
    int nlen;
    itoa_buf(v, buf + idx, &nlen);
    idx += nlen;
    buf[idx++] = '\n';
    buf[idx]   = '\0';
    sc(SYS_WRITE, 1, (long)buf, idx, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* LCG random                                                           */
/* ------------------------------------------------------------------ */
static u32 rng_state = 12345;

static void rng_seed(u32 s)
{
    rng_state = s ^ 0xdeadbeef;
    if (!rng_state) rng_state = 1;
}

static u32 rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* ------------------------------------------------------------------ */
/* Layout constants                                                     */
/* ------------------------------------------------------------------ */
#define WIN_W        360
#define WIN_H        640

#define CELL         24          /* cell size in pixels */
#define BOARD_COLS   10
#define BOARD_ROWS   20

/* Playfield top-left corner */
#define PF_X         8
#define PF_Y         8

/* Pixel dimensions of playfield interior */
#define PF_PXW       (BOARD_COLS * CELL)   /* 240 */
#define PF_PXH       (BOARD_ROWS * CELL)   /* 480 */

/* Right panel */
#define PANEL_X      (PF_X + PF_PXW + 12)  /* 260 */
#define PANEL_W      (WIN_W - PANEL_X - 8)  /* 92 */

/* Preview cell size */
#define PREV_CS      14

/* ------------------------------------------------------------------ */
/* Colors (ARGB32: 0xAARRGGBB)                                          */
/* ------------------------------------------------------------------ */
#define COL_BG        0xFF1A1A2E
#define COL_BORDER    0xFF4A4A6A
#define COL_BORDER_HI 0xFF8080B0
#define COL_EMPTY     0xFF0D0D1A
#define COL_GHOST     0xFF2A2A4A
#define COL_GHOST_BDR 0xFF3A3A6A
#define COL_TEXT      0xFFE0E0E0
#define COL_LABEL     0xFF9090B0
#define COL_SCORE     0xFFFFD700
#define COL_GAMEOVER  0xFFFF4444
#define COL_PAUSE     0xFF44AAFF
#define COL_HOLD_BG   0xFF121220
#define COL_PANEL_BG  0xFF111128

/* One color per tetromino type (I O T S Z J L) */
static const u32 PIECE_COLOR[7] = {
    0xFF00F0F0,  /* I -- cyan   */
    0xFFF0F000,  /* O -- yellow */
    0xFFA000F0,  /* T -- purple */
    0xFF00F000,  /* S -- green  */
    0xFFF00000,  /* Z -- red    */
    0xFF0000F0,  /* J -- blue   */
    0xFFF0A000,  /* L -- orange */
};

/* ------------------------------------------------------------------ */
/* Tetromino rotation tables                                            */
/*   [piece_type][rotation][mino_index][col=0 / row=1]                 */
/* ------------------------------------------------------------------ */
static const i32 PIECES[7][4][4][2] = {
    /* 0: I */
    {
        {{0,1},{1,1},{2,1},{3,1}},
        {{2,0},{2,1},{2,2},{2,3}},
        {{0,2},{1,2},{2,2},{3,2}},
        {{1,0},{1,1},{1,2},{1,3}},
    },
    /* 1: O */
    {
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
    },
    /* 2: T */
    {
        {{0,1},{1,1},{2,1},{1,0}},
        {{1,0},{1,1},{1,2},{2,1}},
        {{0,1},{1,1},{2,1},{1,2}},
        {{1,0},{1,1},{1,2},{0,1}},
    },
    /* 3: S */
    {
        {{0,1},{1,1},{1,0},{2,0}},
        {{1,0},{1,1},{2,1},{2,2}},
        {{0,2},{1,2},{1,1},{2,1}},
        {{0,0},{0,1},{1,1},{1,2}},
    },
    /* 4: Z */
    {
        {{0,0},{1,0},{1,1},{2,1}},
        {{2,0},{2,1},{1,1},{1,2}},
        {{0,1},{1,1},{1,2},{2,2}},
        {{1,0},{1,1},{0,1},{0,2}},
    },
    /* 5: J */
    {
        {{0,0},{0,1},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{1,2}},
        {{0,1},{1,1},{2,1},{2,2}},
        {{1,0},{1,1},{1,2},{0,2}},
    },
    /* 6: L */
    {
        {{2,0},{0,1},{1,1},{2,1}},
        {{1,0},{1,1},{1,2},{2,2}},
        {{0,1},{1,1},{2,1},{0,2}},
        {{0,0},{1,0},{1,1},{1,2}},
    },
};

/* ------------------------------------------------------------------ */
/* Game state                                                           */
/* ------------------------------------------------------------------ */
#define EMPTY_CELL  0xFF  /* sentinel: no block in cell */
#define NO_HOLD    -1

/* board[row][col]: piece-type index 0..6, or EMPTY_CELL */
static u32 board[BOARD_ROWS][BOARD_COLS];

/* Current falling piece */
static i32 cur_type;
static i32 cur_rot;
static i32 cur_x;
static i32 cur_y;

/* Next piece */
static i32 next_type;

/* Hold piece */
static i32 hold_type;      /* NO_HOLD = -1 when empty */
static i32 hold_used;      /* 1 = already used this piece drop */

/* Stats */
static u32 score;
static u32 level;
static u32 lines_cleared;

/* Flags */
static i32 game_over;
static i32 paused;

/* Gravity */
static u64 gravity_ms;
static u64 last_drop_ms;

/* Soft-drop */
static i32 soft_drop;

/* ------------------------------------------------------------------ */
/* Board helpers                                                        */
/* ------------------------------------------------------------------ */
static void board_clear(void)
{
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            board[r][c] = EMPTY_CELL;
}

/* Fill out_c/out_r with the 4 board-space positions of a piece state. */
static void piece_cells(i32 type, i32 rot, i32 px, i32 py,
                        i32 out_c[4], i32 out_r[4])
{
    for (int i = 0; i < 4; i++) {
        out_c[i] = px + PIECES[type][rot][i][0];
        out_r[i] = py + PIECES[type][rot][i][1];
    }
}

/* 1 if position is valid (in-bounds and not blocked). */
static int piece_valid(i32 type, i32 rot, i32 px, i32 py)
{
    i32 cc[4], cr[4];
    piece_cells(type, rot, px, py, cc, cr);
    for (int i = 0; i < 4; i++) {
        if (cc[i] < 0 || cc[i] >= BOARD_COLS) return 0;
        if (cr[i] >= BOARD_ROWS)               return 0;
        if (cr[i] >= 0 && board[cr[i]][cc[i]] != EMPTY_CELL) return 0;
    }
    return 1;
}

/* Lock current piece onto the board. */
static void piece_lock(void)
{
    i32 cc[4], cr[4];
    piece_cells(cur_type, cur_rot, cur_x, cur_y, cc, cr);
    for (int i = 0; i < 4; i++) {
        if (cr[i] >= 0 && cr[i] < BOARD_ROWS &&
            cc[i] >= 0 && cc[i] < BOARD_COLS)
            board[cr[i]][cc[i]] = (u32)cur_type;
    }
}

/* Clear full lines; return count cleared. */
static int lines_clear_board(void)
{
    int cleared = 0;
    for (int r = BOARD_ROWS - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < BOARD_COLS; c++)
            if (board[r][c] == EMPTY_CELL) { full = 0; break; }
        if (full) {
            cleared++;
            for (int rr = r; rr > 0; rr--)
                for (int c = 0; c < BOARD_COLS; c++)
                    board[rr][c] = board[rr - 1][c];
            for (int c = 0; c < BOARD_COLS; c++)
                board[0][c] = EMPTY_CELL;
            r++; /* re-check same row index after shift */
        }
    }
    return cleared;
}

/* NES-style scoring (×(level+1)). */
static const u32 LINE_SCORE[5] = {0, 40, 100, 300, 1200};

static void update_score(int n)
{
    if (n < 0) n = 0;
    if (n > 4) n = 4;
    score         += LINE_SCORE[n] * (level + 1);
    lines_cleared += (u32)n;
    level          = lines_cleared / 10;
    /* Gravity: 800 ms at level 0, −60 ms/level, floor 80 ms */
    u64 dec = (u64)level * 60;
    gravity_ms = (dec < 720) ? (800 - dec) : 80;
    if (gravity_ms < 80) gravity_ms = 80;
}

/* ------------------------------------------------------------------ */
/* Piece spawning                                                       */
/* ------------------------------------------------------------------ */
static void spawn_piece(void)
{
    cur_type  = next_type;
    cur_rot   = 0;
    cur_x     = BOARD_COLS / 2 - 2;
    cur_y     = -1;
    hold_used = 0;

    next_type = (i32)(rng_next() % 7);

    /* Game-over if spawn is immediately blocked */
    if (!piece_valid(cur_type, cur_rot, cur_x, cur_y) &&
        !piece_valid(cur_type, cur_rot, cur_x, cur_y + 1))
        game_over = 1;
}

/* ------------------------------------------------------------------ */
/* Hold piece                                                           */
/* ------------------------------------------------------------------ */
static void do_hold(void)
{
    if (hold_used) return;   /* only one hold per piece drop */
    hold_used = 1;

    if (hold_type == NO_HOLD) {
        hold_type = cur_type;
        spawn_piece();
    } else {
        i32 tmp   = hold_type;
        hold_type = cur_type;
        cur_type  = tmp;
        cur_rot   = 0;
        cur_x     = BOARD_COLS / 2 - 2;
        cur_y     = -1;
        /* If restored piece immediately blocked, game over */
        if (!piece_valid(cur_type, cur_rot, cur_x, cur_y) &&
            !piece_valid(cur_type, cur_rot, cur_x, cur_y + 1))
            game_over = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Ghost piece                                                          */
/* ------------------------------------------------------------------ */
static i32 ghost_y(void)
{
    i32 gy = cur_y;
    while (piece_valid(cur_type, cur_rot, cur_x, gy + 1))
        gy++;
    return gy;
}

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                   */
/* ------------------------------------------------------------------ */
static void fill_rect(u32 *fb, int stride, int x, int y, int w, int h, u32 col)
{
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= WIN_H) continue;
        for (int cx = 0; cx < w; cx++) {
            int px = x + cx;
            if (px < 0 || px >= WIN_W) continue;
            fb[py * stride + px] = col;
        }
    }
}

static void draw_border(u32 *fb, int stride, int x, int y, int w, int h, u32 col)
{
    fill_rect(fb, stride, x,         y,         w, 1, col);
    fill_rect(fb, stride, x,         y + h - 1, w, 1, col);
    fill_rect(fb, stride, x,         y,         1, h, col);
    fill_rect(fb, stride, x + w - 1, y,         1, h, col);
}

/* Draw a Tetris block cell with highlight shading. */
static void draw_cell(u32 *fb, int stride, int px, int py, int cs, u32 col)
{
    fill_rect(fb, stride, px + 1, py + 1, cs - 2, cs - 2, col);

    /* Top-left highlight: clamp each channel +50 */
    u32 r = (col >> 16) & 0xFF; if (r + 50 < 256) r += 50; else r = 255;
    u32 g = (col >>  8) & 0xFF; if (g + 50 < 256) g += 50; else g = 255;
    u32 b = (col      ) & 0xFF; if (b + 50 < 256) b += 50; else b = 255;
    u32 hi = 0xFF000000 | (r << 16) | (g << 8) | b;

    /* Bottom-right shadow: clamp each channel -50 */
    u32 dr = (col >> 16) & 0xFF; if (dr > 50) dr -= 50; else dr = 0;
    u32 dg = (col >>  8) & 0xFF; if (dg > 50) dg -= 50; else dg = 0;
    u32 db = (col      ) & 0xFF; if (db > 50) db -= 50; else db = 0;
    u32 sh = 0xFF000000 | (dr << 16) | (dg << 8) | db;

    /* 1-pixel highlight top edge */
    fill_rect(fb, stride, px + 1, py + 1, cs - 2, 1,      hi);
    /* 1-pixel highlight left edge */
    fill_rect(fb, stride, px + 1, py + 1, 1,      cs - 2, hi);
    /* 1-pixel shadow bottom edge */
    fill_rect(fb, stride, px + 1, py + cs - 2, cs - 2, 1, sh);
    /* 1-pixel shadow right edge */
    fill_rect(fb, stride, px + cs - 2, py + 1, 1, cs - 2, sh);
}

/* Draw a ghost cell (outline only). */
static void draw_ghost_cell(u32 *fb, int stride, int px, int py, int cs, u32 col)
{
    draw_border(fb, stride, px + 1, py + 1, cs - 2, cs - 2, col);
}

/* ------------------------------------------------------------------ */
/* Text helpers                                                         */
/* ------------------------------------------------------------------ */
static void draw_text(u32 *fb, int stride, int x, int y, const char *s, u32 col)
{
    font_draw_string(fb, stride, WIN_W, WIN_H, x, y, s, col);
}

static void draw_num(u32 *fb, int stride, int x, int y, u32 v, u32 col)
{
    char buf[12];
    int  len;
    itoa_buf(v, buf, &len);
    font_draw_string(fb, stride, WIN_W, WIN_H, x, y, buf, col);
}

/* Draw "LABEL\n value" in two lines. */
static void draw_stat(u32 *fb, int stride, int x, int y,
                      const char *label, u32 value)
{
    draw_text(fb, stride, x, y,                label,  COL_LABEL);
    draw_num (fb, stride, x, y + FONT_H + 2,   value,  COL_SCORE);
}

/* ------------------------------------------------------------------ */
/* Piece preview renderer (used for NEXT and HOLD)                      */
/* ------------------------------------------------------------------ */
static void draw_piece_preview(u32 *fb, int stride,
                               int box_x, int box_y, int box_w, int box_h,
                               i32 type, int cs)
{
    /* background */
    fill_rect(fb, stride, box_x, box_y, box_w, box_h, COL_HOLD_BG);
    draw_border(fb, stride, box_x - 1, box_y - 1, box_w + 2, box_h + 2, COL_BORDER);

    if (type == NO_HOLD) {
        draw_text(fb, stride, box_x + 2, box_y + box_h / 2 - FONT_H / 2, "--", COL_LABEL);
        return;
    }

    /* Center the piece in the box (4x4 bounding box, then offset) */
    /* Find bounding min col/row across all 4 minoees at rot=0 */
    int min_c = 4, max_c = 0, min_r = 4, max_r = 0;
    for (int i = 0; i < 4; i++) {
        int dc = PIECES[type][0][i][0];
        int dr = PIECES[type][0][i][1];
        if (dc < min_c) min_c = dc;
        if (dc > max_c) max_c = dc;
        if (dr < min_r) min_r = dr;
        if (dr > max_r) max_r = dr;
    }
    int pw = (max_c - min_c + 1) * cs;
    int ph = (max_r - min_r + 1) * cs;
    int ox = box_x + (box_w - pw) / 2;
    int oy = box_y + (box_h - ph) / 2;

    for (int i = 0; i < 4; i++) {
        int dc = PIECES[type][0][i][0] - min_c;
        int dr = PIECES[type][0][i][1] - min_r;
        draw_cell(fb, stride, ox + dc * cs, oy + dr * cs, cs, PIECE_COLOR[type]);
    }
}

/* ------------------------------------------------------------------ */
/* Main render                                                          */
/* ------------------------------------------------------------------ */
static void render(wl_window *win)
{
    u32 *fb     = win->pixels;
    int  stride = (int)(win->stride / 4);

    /* ---- background ---- */
    fill_rect(fb, stride, 0, 0, WIN_W, WIN_H, COL_BG);

    /* ---- panel background ---- */
    fill_rect(fb, stride, PANEL_X - 4, 0, WIN_W - PANEL_X + 4, WIN_H, COL_PANEL_BG);

    /* ---- playfield border ---- */
    draw_border(fb, stride, PF_X - 1, PF_Y - 1, PF_PXW + 2, PF_PXH + 2, COL_BORDER_HI);

    /* ---- grid cells (empty background) ---- */
    for (int r = 0; r < BOARD_ROWS; r++) {
        for (int c = 0; c < BOARD_COLS; c++) {
            int px = PF_X + c * CELL;
            int py = PF_Y + r * CELL;
            fill_rect(fb, stride, px, py, CELL, CELL, COL_EMPTY);
            /* subtle grid lines at cell edges */
            fill_rect(fb, stride, px, py, CELL, 1, COL_BG);
            fill_rect(fb, stride, px, py, 1, CELL, COL_BG);
        }
    }

    /* ---- locked blocks ---- */
    for (int r = 0; r < BOARD_ROWS; r++) {
        for (int c = 0; c < BOARD_COLS; c++) {
            u32 t = board[r][c];
            if (t == EMPTY_CELL) continue;
            int px = PF_X + c * CELL;
            int py = PF_Y + r * CELL;
            draw_cell(fb, stride, px, py, CELL, PIECE_COLOR[t]);
        }
    }

    if (!game_over && !paused) {
        /* ---- ghost piece ---- */
        i32 gy = ghost_y();
        if (gy != cur_y) {
            i32 cc[4], cr[4];
            piece_cells(cur_type, cur_rot, cur_x, gy, cc, cr);
            for (int i = 0; i < 4; i++) {
                if (cr[i] < 0 || cr[i] >= BOARD_ROWS) continue;
                if (cc[i] < 0 || cc[i] >= BOARD_COLS) continue;
                int px = PF_X + cc[i] * CELL;
                int py = PF_Y + cr[i] * CELL;
                draw_ghost_cell(fb, stride, px, py, CELL, COL_GHOST_BDR);
            }
        }

        /* ---- falling piece ---- */
        i32 fc[4], fr[4];
        piece_cells(cur_type, cur_rot, cur_x, cur_y, fc, fr);
        for (int i = 0; i < 4; i++) {
            if (fr[i] < 0 || fr[i] >= BOARD_ROWS) continue;
            if (fc[i] < 0 || fc[i] >= BOARD_COLS) continue;
            int px = PF_X + fc[i] * CELL;
            int py = PF_Y + fr[i] * CELL;
            draw_cell(fb, stride, px, py, CELL, PIECE_COLOR[cur_type]);
        }
    }

    /* ---- right panel ---- */
    int px0 = PANEL_X;
    int py  = PF_Y;

    /* SCORE */
    draw_stat(fb, stride, px0, py, "SCORE", score);
    py += FONT_H * 2 + 8;

    /* LEVEL */
    draw_stat(fb, stride, px0, py, "LEVEL", level);
    py += FONT_H * 2 + 8;

    /* LINES */
    draw_stat(fb, stride, px0, py, "LINES", lines_cleared);
    py += FONT_H * 2 + 14;

    /* NEXT preview */
    draw_text(fb, stride, px0, py, "NEXT", COL_LABEL);
    py += FONT_H + 4;
    {
        int box_sz = PREV_CS * 4 + 4;
        draw_piece_preview(fb, stride, px0, py, box_sz, box_sz, next_type, PREV_CS);
        py += box_sz + 14;
    }

    /* HOLD preview */
    draw_text(fb, stride, px0, py, "HOLD", COL_LABEL);
    py += FONT_H + 4;
    {
        int box_sz = PREV_CS * 4 + 4;
        /* dim if hold was used this turn */
        u32 hold_col = hold_used ? COL_LABEL : COL_TEXT;
        (void)hold_col;
        draw_piece_preview(fb, stride, px0, py, box_sz, box_sz, hold_type, PREV_CS);
        if (hold_used && hold_type != NO_HOLD) {
            /* overlay dim tint: redraw with darker shade */
            int bx = px0, by = py;
            int bsz = PREV_CS * 4 + 4;
            for (int ry = by; ry < by + bsz; ry++) {
                if (ry < 0 || ry >= WIN_H) continue;
                for (int rx = bx; rx < bx + bsz; rx++) {
                    if (rx < 0 || rx >= WIN_W) continue;
                    u32 p = fb[ry * stride + rx];
                    fb[ry * stride + rx] =
                        0xFF000000 |
                        (((( p >> 17) & 0x7F)) << 16) |
                        (((( p >>  9) & 0x7F)) <<  8) |
                        ((  ( p >>  1) & 0x7F)       );
                }
            }
        }
        py += PREV_CS * 4 + 4 + 14;
    }

    /* Controls hint */
    draw_text(fb, stride, px0, py,              "CTRL", COL_LABEL);
    py += FONT_H + 2;
    draw_text(fb, stride, px0, py,              "<> move",  COL_TEXT); py += FONT_H;
    draw_text(fb, stride, px0, py,              "^/X rot",  COL_TEXT); py += FONT_H;
    draw_text(fb, stride, px0, py,              "v drop",   COL_TEXT); py += FONT_H;
    draw_text(fb, stride, px0, py,              "SPC hard", COL_TEXT); py += FONT_H;
    draw_text(fb, stride, px0, py,              "H hold",   COL_TEXT); py += FONT_H;
    draw_text(fb, stride, px0, py,              "P pause",  COL_TEXT); py += FONT_H;
    draw_text(fb, stride, px0, py,              "R reset",  COL_TEXT);

    /* ---- PAUSE overlay ---- */
    if (paused && !game_over) {
        /* semi-dim the playfield */
        for (int r = 0; r < BOARD_ROWS; r++) {
            for (int c = 0; c < BOARD_COLS; c++) {
                int cpx = PF_X + c * CELL;
                int cpy = PF_Y + r * CELL;
                for (int dy = 0; dy < CELL; dy++) {
                    int ry = cpy + dy;
                    if (ry < 0 || ry >= WIN_H) continue;
                    for (int dx = 0; dx < CELL; dx++) {
                        int rx = cpx + dx;
                        if (rx < 0 || rx >= WIN_W) continue;
                        u32 p = fb[ry * stride + rx];
                        fb[ry * stride + rx] =
                            0xFF000000 |
                            ((((p >> 17) & 0x7F)) << 16) |
                            ((((p >>  9) & 0x7F)) <<  8) |
                            (  ((p >>  1) & 0x7F)      );
                    }
                }
            }
        }
        int go_x = PF_X + (PF_PXW - 6 * FONT_W) / 2;
        int go_y = PF_Y + PF_PXH / 2 - FONT_H;
        draw_text(fb, stride, go_x,              go_y,           "PAUSED",  COL_PAUSE);
        draw_text(fb, stride, go_x - FONT_W * 2, go_y + FONT_H, "P to resume", COL_TEXT);
    }

    /* ---- GAME OVER overlay ---- */
    if (game_over) {
        /* dim playfield */
        for (int r = 0; r < BOARD_ROWS; r++) {
            for (int c = 0; c < BOARD_COLS; c++) {
                int cpx = PF_X + c * CELL;
                int cpy = PF_Y + r * CELL;
                for (int dy = 0; dy < CELL; dy++) {
                    int ry = cpy + dy;
                    if (ry < 0 || ry >= WIN_H) continue;
                    for (int dx = 0; dx < CELL; dx++) {
                        int rx = cpx + dx;
                        if (rx < 0 || rx >= WIN_W) continue;
                        u32 p = fb[ry * stride + rx];
                        fb[ry * stride + rx] =
                            0xFF000000 |
                            ((((p >> 17) & 0x7F)) << 16) |
                            ((((p >>  9) & 0x7F)) <<  8) |
                            (  ((p >>  1) & 0x7F)      );
                    }
                }
            }
        }

        /* Centered overlay text */
        int gx = PF_X + (PF_PXW - 9 * FONT_W) / 2;
        int gy = PF_Y + PF_PXH / 2 - FONT_H * 2;
        draw_text(fb, stride, gx,              gy,            "GAME OVER",   COL_GAMEOVER);
        draw_text(fb, stride, gx + FONT_W * 2, gy + FONT_H,  "score:",       COL_TEXT);
        draw_num (fb, stride, gx + FONT_W * 9, gy + FONT_H,  score,          COL_SCORE);
        draw_text(fb, stride, gx - FONT_W,     gy + FONT_H*2, "R to restart", COL_TEXT);
    }

    wl_commit(win);
}

/* ------------------------------------------------------------------ */
/* Game initialisation                                                  */
/* ------------------------------------------------------------------ */
static void game_init(void)
{
    board_clear();
    score         = 0;
    level         = 0;
    lines_cleared = 0;
    game_over     = 0;
    paused        = 0;
    soft_drop     = 0;
    hold_type     = NO_HOLD;
    hold_used     = 0;
    gravity_ms    = 800;
    last_drop_ms  = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);

    next_type = (i32)(rng_next() % 7);
    spawn_piece();
}

/* ------------------------------------------------------------------ */
/* Input handling                                                       */
/* ------------------------------------------------------------------ */
static void handle_key(int keycode, int pressed)
{
    /* Release events: track soft-drop */
    if (!pressed) {
        if (keycode == KEY_DOWN || keycode == KEY_S)
            soft_drop = 0;
        return;
    }

    /* R = restart at any time */
    if (keycode == KEY_R) {
        rng_seed((u32)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0) ^ (u32)(u64)&keycode);
        game_init();
        return;
    }

    /* ESC = exit */
    if (keycode == KEY_ESC) {
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    }

    /* On game-over only R (handled above) and ESC work */
    if (game_over) return;

    /* P = toggle pause */
    if (keycode == KEY_P) {
        paused = !paused;
        if (!paused)
            last_drop_ms = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        return;
    }

    if (paused) return;

    switch (keycode) {
    case KEY_LEFT:
    case KEY_A:
        if (piece_valid(cur_type, cur_rot, cur_x - 1, cur_y))
            cur_x--;
        break;

    case KEY_RIGHT:
    case KEY_D:
        if (piece_valid(cur_type, cur_rot, cur_x + 1, cur_y))
            cur_x++;
        break;

    case KEY_DOWN:
    case KEY_S:
        soft_drop = 1;
        if (piece_valid(cur_type, cur_rot, cur_x, cur_y + 1))
            cur_y++;
        break;

    case KEY_UP:
    case KEY_X:
    case KEY_W: {
        i32 nr = (cur_rot + 1) & 3;
        /* SRS-ish wall-kick: plain, ±1, ±2 */
        if      (piece_valid(cur_type, nr, cur_x,     cur_y)) cur_rot = nr;
        else if (piece_valid(cur_type, nr, cur_x - 1, cur_y)) { cur_rot = nr; cur_x--; }
        else if (piece_valid(cur_type, nr, cur_x + 1, cur_y)) { cur_rot = nr; cur_x++; }
        else if (piece_valid(cur_type, nr, cur_x - 2, cur_y)) { cur_rot = nr; cur_x -= 2; }
        else if (piece_valid(cur_type, nr, cur_x + 2, cur_y)) { cur_rot = nr; cur_x += 2; }
        /* floor-kick: try one row up */
        else if (piece_valid(cur_type, nr, cur_x, cur_y - 1)) { cur_rot = nr; cur_y--; }
        break;
    }

    case KEY_H:
    case KEY_C:
        do_hold();
        break;

    case KEY_SPACE: {
        /* Hard-drop: fall to ghost position and lock */
        cur_y = ghost_y();
        piece_lock();
        int cl = lines_clear_board();
        update_score(cl);
        last_drop_ms = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        spawn_piece();
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Entry point (_start: no libc, no crt0 for wl-direct apps)           */
/* ------------------------------------------------------------------ */
void _start(void)
{
    print("[TETRIS] starting\n");

    /* Connect to compositor */
    if (wl_connect() != 0)
        print("[TETRIS] wl_connect failed\n");

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Tetris");
    if (!win)
        print("[TETRIS] create_window failed\n");

    /* Seed RNG */
    rng_seed((u32)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0));

    game_init();

    int prev_game_over = 0;

    for (;;) {
        /* ---- poll input ---- */
        int kind, a, b, c;
        while (win && wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY)
                handle_key(a, b);
        }

        /* ---- gravity ---- */
        if (!game_over && !paused) {
            u64 now      = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            u64 interval = soft_drop ? 50 : gravity_ms;

            if (now - last_drop_ms >= interval) {
                last_drop_ms = now;
                if (piece_valid(cur_type, cur_rot, cur_x, cur_y + 1)) {
                    cur_y++;
                } else {
                    piece_lock();
                    int cl = lines_clear_board();
                    update_score(cl);
                    spawn_piece();
                }
            }
        }

        /* Serial: print score on game-over transition */
        if (game_over && !prev_game_over)
            print_num("[TETRIS] score ", score);
        prev_game_over = game_over;

        /* ---- render ---- */
        if (win) render(win);

        /* ---- yield to scheduler ---- */
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
