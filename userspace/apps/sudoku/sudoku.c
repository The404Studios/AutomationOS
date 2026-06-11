/*
 * sudoku.c -- Classic Sudoku (freestanding, ring 3).
 * ===================================================
 *
 * A polished 9x9 Sudoku for AutomationOS.  Generates a fully-solved board
 * via randomized backtracking (seeded from SYS_GET_TICKS_MS), then removes
 * cells while guaranteeing the puzzle keeps a UNIQUE solution (the remover
 * re-runs a bounded solution-counter after each tentative removal).
 *
 *   - 9x9 grid with 3x3 boxes drawn with bold dividers.
 *   - Given cells are bold/dark; user-entered cells are a lighter blue.
 *   - Mouse: click a cell to select.
 *   - Keyboard: 1-9 enter a digit, 0 / Backspace clear.  Arrow keys move
 *     the selection.  'N' = new game, 'D' = cycle difficulty, 'P' = toggle
 *     pencil-mark (notes) mode, ESC = quit.
 *   - Selected cell is highlighted; cells sharing the selected value are
 *     softly highlighted; any cell that breaks a row/col/box rule is red.
 *   - Header: live timer (mm:ss from SYS_GET_TICKS_MS), a "New Game" button,
 *     a difficulty cycle button, and a win banner when the board is solved.
 *   - Pencil marks: 9 mini candidate digits per empty cell (bonus feature).
 *
 * No libc: pure inline syscalls + wl_client + bitfont.  Board state is
 * malloc'd from a static arena (no kernel heap dependency) per the brief.
 *
 * Window: 500 x 560 fixed.
 *
 * Build (matches scripts/build_all.sh cc()/LD):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/sudoku/sudoku.c -o /tmp/sudoku.o
 *   gcc <same flags> -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc <same flags> -c userspace/lib/font/bitfont.c  -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/sudoku.o /tmp/wlc.o /tmp/bf.o -o /tmp/sudoku.elf
 *   objdump -d /tmp/sudoku.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [SUDOKU] starting
 *   [SUDOKU] new game (difficulty N)
 *   [SUDOKU] solved in Ns
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers (AutomationOS ABI -- see kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40
#define SYS_RANDOM        43

/* -----------------------------------------------------------------------
 * Key codes (Linux-style keycodes delivered via WL_EVENT_KEY a=keycode).
 * The PS/2 driver maps top-row digits to KEY_1=2 .. KEY_0=11 and
 * Backspace=14; keypad/extended keys are not delivered, so clears use 0 or
 * Backspace.  Arrow keys arrive as the standard 103/105/106/108 set.
 * --------------------------------------------------------------------- */
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_BACKSPACE  14
#define KEY_N          49
#define KEY_D          32
#define KEY_P          25
#define KEY_UP         103
#define KEY_LEFT       105
#define KEY_RIGHT      106
#define KEY_DOWN       108

/* -----------------------------------------------------------------------
 * Fixed-width types.
 * --------------------------------------------------------------------- */
typedef unsigned int        u32;
typedef int                 i32;
typedef unsigned char       u8;
typedef unsigned long       u64;
typedef unsigned short      u16;

/* -----------------------------------------------------------------------
 * Inline syscalls (3-arg and 6-arg forms).
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

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

/* -----------------------------------------------------------------------
 * Freestanding helpers.
 * --------------------------------------------------------------------- */
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc6(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }

static void print_i32(i32 v)
{
    char b[12]; i32 i = 0;
    if (v < 0) { sc6(SYS_WRITE, 1, (long)"-", 1, 0, 0, 0); v = -v; }
    if (v == 0) { sc6(SYS_WRITE, 1, (long)"0", 1, 0, 0, 0); return; }
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (i32 j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = '\0';
    sc6(SYS_WRITE, 1, (long)b, i, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Tiny static-arena malloc (the brief asks to malloc the board state, but
 * a freestanding ELF has no heap -- so we carve from a static bump arena).
 * Only ever allocates the board struct once; no free needed.
 * --------------------------------------------------------------------- */
static u8 g_arena[16384];
static u32 g_arena_off = 0;

static void *kmalloc(u32 size)
{
    size = (size + 15u) & ~15u;                 /* 16-byte align */
    if (g_arena_off + size > sizeof(g_arena)) return (void *)0;
    void *p = &g_arena[g_arena_off];
    g_arena_off += size;
    return p;
}

/* -----------------------------------------------------------------------
 * PRNG: seed from SYS_GET_TICKS_MS (topped up with SYS_RANDOM if available).
 * xorshift32 -- fast and good enough for puzzle generation.
 * --------------------------------------------------------------------- */
static u32 g_rng = 0x2545F491u;

static u32 rng_next(void)
{
    u32 x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rng = x;
    return x;
}

static void rng_seed(void)
{
    long t = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    u32 s = (u32)t ^ 0x9E3779B9u;
    /* Mix in CSPRNG bytes when the kernel offers them. */
    u32 extra = 0;
    if (sc(SYS_RANDOM, (long)&extra, (long)sizeof(extra), 0) >= 0) s ^= extra;
    if (s == 0) s = 0xDEADBEEFu;
    g_rng = s;
    /* Warm up. */
    for (int i = 0; i < 16; i++) rng_next();
}

/* Uniform integer in [0, n). */
static u32 rng_below(u32 n) { return n ? (rng_next() % n) : 0; }

/* -----------------------------------------------------------------------
 * Board model.
 *   given[81]   -- the puzzle clues (0 = blank).
 *   solution[81]-- the unique completed solution (for win-check + hints).
 *   value[81]   -- current cell value (0 = empty), includes given clues.
 *   notes[81]   -- bitmask of pencil-marked candidates (bit d => digit d).
 * --------------------------------------------------------------------- */
typedef struct {
    u8  given[81];
    u8  solution[81];
    u8  value[81];
    u16 notes[81];
} board_t;

static board_t *B;            /* malloc'd board state */

#define IDX(r, c)  ((r) * 9 + (c))

/* -----------------------------------------------------------------------
 * Sudoku rule check: can `v` (1..9) legally go at (r,c) given `grid`?
 * Used by both the generator/solver and the live conflict highlighter.
 * --------------------------------------------------------------------- */
static int can_place(const u8 *grid, int r, int c, int v)
{
    for (int i = 0; i < 9; i++) {
        if (grid[IDX(r, i)] == v) return 0;        /* row   */
        if (grid[IDX(i, c)] == v) return 0;        /* col   */
    }
    int br = (r / 3) * 3, bc = (c / 3) * 3;         /* box   */
    for (int dr = 0; dr < 3; dr++)
        for (int dc = 0; dc < 3; dc++)
            if (grid[IDX(br + dr, bc + dc)] == v) return 0;
    return 1;
}

/* -----------------------------------------------------------------------
 * Solver / solution-counter (backtracking).  count_solutions fills a
 * scratch grid and counts up to `limit` solutions (we only ever need to
 * know "exactly 1" vs ">1", so limit=2 keeps it cheap).
 * --------------------------------------------------------------------- */
static int count_solutions(u8 *grid, int limit)
{
    /* Find the first empty cell. */
    int idx = -1;
    for (int i = 0; i < 81; i++) {
        if (grid[i] == 0) { idx = i; break; }
    }
    if (idx < 0) return 1;                          /* full -> one solution */

    int r = idx / 9, c = idx % 9;
    int total = 0;
    for (int v = 1; v <= 9; v++) {
        if (can_place(grid, r, c, v)) {
            grid[idx] = (u8)v;
            total += count_solutions(grid, limit - total);
            grid[idx] = 0;
            if (total >= limit) return total;       /* early-out */
        }
    }
    return total;
}

/* -----------------------------------------------------------------------
 * Generator: fill a solved board with randomized backtracking, then carve
 * out clues while keeping a unique solution.
 * --------------------------------------------------------------------- */

/* Recursive randomized fill of `grid` (which must start all-zero). */
static int fill_grid(u8 *grid)
{
    int idx = -1;
    for (int i = 0; i < 81; i++) {
        if (grid[i] == 0) { idx = i; break; }
    }
    if (idx < 0) return 1;                          /* solved */

    int r = idx / 9, c = idx % 9;

    /* Shuffle digits 1..9 for variety. */
    u8 digits[9] = {1,2,3,4,5,6,7,8,9};
    for (int i = 8; i > 0; i--) {
        int j = (int)rng_below((u32)(i + 1));
        u8 t = digits[i]; digits[i] = digits[j]; digits[j] = t;
    }
    for (int k = 0; k < 9; k++) {
        int v = digits[k];
        if (can_place(grid, r, c, v)) {
            grid[idx] = (u8)v;
            if (fill_grid(grid)) return 1;
            grid[idx] = 0;
        }
    }
    return 0;
}

/*
 * Build a puzzle at the given difficulty.
 *   difficulty: number of CLUES to TRY to leave (lower = harder).
 * We remove cells in a random order; a removal is kept only if the puzzle
 * still has a unique solution.  We stop removing once `target_clues` is hit
 * or the removal order is exhausted (so the resulting clue count is always
 * >= target_clues and the puzzle is always uniquely solvable).
 */
static void generate(int target_clues)
{
    /* 1. Fully-solved board. */
    for (int i = 0; i < 81; i++) B->solution[i] = 0;
    fill_grid(B->solution);

    /* 2. Start the puzzle as the full solution, then carve. */
    for (int i = 0; i < 81; i++) B->given[i] = B->solution[i];

    /* Random removal order over all 81 cells. */
    u8 order[81];
    for (int i = 0; i < 81; i++) order[i] = (u8)i;
    for (int i = 80; i > 0; i--) {
        int j = (int)rng_below((u32)(i + 1));
        u8 t = order[i]; order[i] = order[j]; order[j] = t;
    }

    int clues = 81;
    u8 scratch[81];
    for (int k = 0; k < 81 && clues > target_clues; k++) {
        int cell = order[k];
        if (B->given[cell] == 0) continue;
        u8 saved = B->given[cell];
        B->given[cell] = 0;

        /* Does it still have a unique solution? */
        for (int i = 0; i < 81; i++) scratch[i] = B->given[i];
        if (count_solutions(scratch, 2) != 1) {
            B->given[cell] = saved;                 /* revert: not unique */
        } else {
            clues--;                                /* keep the removal */
        }
    }

    /* 3. Initialise the live working board from the clues. */
    for (int i = 0; i < 81; i++) {
        B->value[i] = B->given[i];
        B->notes[i] = 0;
    }
}

/* -----------------------------------------------------------------------
 * Difficulty levels (clue targets).  Fewer clues => harder.
 * --------------------------------------------------------------------- */
static const char *DIFF_NAME[4] = { "Easy", "Medium", "Hard", "Expert" };
static const int   DIFF_CLUES[4] = { 40, 33, 28, 24 };
static int g_difficulty = 0;                        /* index into DIFF_* */

/* -----------------------------------------------------------------------
 * Game / UI state.
 * --------------------------------------------------------------------- */
static int  g_sel = -1;            /* selected cell index, -1 = none */
static int  g_notes_mode = 0;      /* pencil-mark entry mode */
static int  g_won = 0;             /* board solved correctly */
static u64  g_start_ms = 0;        /* timer start (ms) */
static u64  g_elapsed_ms = 0;      /* frozen elapsed on win */

/* Edge-detect pointer buttons. */
static i32  prev_buttons = 0;

/* -----------------------------------------------------------------------
 * Window / layout geometry.  Fixed 500 x 560.
 * --------------------------------------------------------------------- */
#define WIN_W        500
#define WIN_H        560

/* Live clip bounds = the CURRENT window surface (refreshed on WL_EVENT_RESIZE).
 * The board canvas itself stays a fixed 500x560 -- this is a fixed-canvas app,
 * so on resize we letterbox: clear the whole new surface and blit the fixed
 * canvas at (0,0), clamped to the live surface so a SMALLER window can never
 * overflow the (lib-reallocated) buffer. */
static i32 g_clip_w = WIN_W;
static i32 g_clip_h = WIN_H;

#define HEADER_H     60                       /* top header strip */
#define MARGIN_X     10                       /* left/right margin around grid */
#define GRID_TOP     (HEADER_H + 10)          /* y of grid top edge */
#define GRID_SIZE    (WIN_W - 2 * MARGIN_X)   /* 480 px square grid */
#define CELL         (GRID_SIZE / 9)          /* 53 px per cell */
#define GRID_LEFT    MARGIN_X
#define GRID_RIGHT   (GRID_LEFT + 9 * CELL)
#define GRID_BOTTOM  (GRID_TOP  + 9 * CELL)

/* Header buttons. */
#define BTN_NEW_X    300
#define BTN_NEW_Y    14
#define BTN_NEW_W    90
#define BTN_NEW_H    32

#define BTN_DIFF_X   398
#define BTN_DIFF_Y   14
#define BTN_DIFF_W   92
#define BTN_DIFF_H   32

/* -----------------------------------------------------------------------
 * Colour palette -- clean classic look (light paper + ink).
 * --------------------------------------------------------------------- */
#define C_BG          0xFFF4F1EAu   /* warm paper background        */
#define C_HEADER      0xFF2C3E50u   /* slate header bar             */
#define C_GRIDBG      0xFFFFFFFFu   /* grid cell background         */
#define C_LINE_THIN   0xFFC8C8C0u   /* thin cell separators         */
#define C_LINE_BOLD   0xFF34495Eu   /* bold 3x3 box dividers        */
#define C_GIVEN       0xFF1A1A1Au   /* given clue digits: near-black */
#define C_USER        0xFF2563EBu   /* user-entered digits: blue    */
#define C_NOTE        0xFF8A8A8Au   /* pencil marks: grey           */
#define C_SEL         0xFFCFE3FBu   /* selected cell highlight      */
#define C_PEER        0xFFE8EFF7u   /* same row/col/box of selection */
#define C_SAMEVAL     0xFFBFE0C8u   /* same value as selection      */
#define C_CONFLICT    0xFFF7C7C7u   /* conflicting cell background  */
#define C_CONFLICT_FG 0xFFC02020u   /* conflicting digit ink        */
#define C_BTN         0xFF3D566Eu   /* button face                  */
#define C_BTN_HL      0xFF4E6A86u   /* button highlight edge        */
#define C_WHITE       0xFFFFFFFFu
#define C_WIN         0xFF27AE60u   /* win banner green             */
#define C_HDR_TEXT    0xFFECF0F1u   /* header text                  */
#define C_DIM         0xFFB7C0C8u

/* -----------------------------------------------------------------------
 * Drawing primitives.
 * --------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 stride, i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x, y1 = y < 0 ? 0 : y;
    i32 x2 = x + w, y2 = y + h;
    /* Clamp to the LIVE surface (refreshed on resize), never the fixed canvas:
     * a smaller window must not write past the reallocated buffer. */
    if (x2 > g_clip_w) x2 = g_clip_w;
    if (y2 > g_clip_h) y2 = g_clip_h;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = col;
    }
}

static void hline(u32 *buf, u32 stride, i32 x, i32 y, i32 len, u32 col)
{ fill_rect(buf, stride, x, y, len, 1, col); }

static void vline(u32 *buf, u32 stride, i32 x, i32 y, i32 len, u32 col)
{ fill_rect(buf, stride, x, y, 1, len, col); }

/* Thick line: a w-pixel-wide horizontal/vertical bar. */
static void thick_h(u32 *buf, u32 stride, i32 x, i32 y, i32 len, i32 t, u32 col)
{ fill_rect(buf, stride, x, y - t / 2, len, t, col); }

static void thick_v(u32 *buf, u32 stride, i32 x, i32 y, i32 len, i32 t, u32 col)
{ fill_rect(buf, stride, x - t / 2, y, t, len, col); }

/*
 * Draw a digit (1..9) centred in cell (r,c), scaled up 2x from the 8x16
 * bitfont for a crisp, large numeral.  We render the glyph into a tiny
 * mask and blow it up to 2x with nearest-neighbour.
 */
static void draw_big_digit(u32 *buf, u32 stride, i32 cx, i32 cy, char ch, u32 col)
{
    /* Render the single glyph 2x by sampling the font's 1x output. We do
     * this by drawing to a scratch 8x16 buffer then upscaling. */
    u32 glyph[FONT_W * FONT_H];
    for (int i = 0; i < FONT_W * FONT_H; i++) glyph[i] = 0;
    /* Draw char into scratch at (0,0) with a sentinel colour. */
    font_draw_char(glyph, FONT_W, FONT_W, FONT_H, 0, 0, ch, 0xFFFFFFFFu);

    const int S = 2;                                /* scale factor */
    i32 ox = cx - (FONT_W * S) / 2;
    i32 oy = cy - (FONT_H * S) / 2;
    for (int gy = 0; gy < FONT_H; gy++) {
        for (int gx = 0; gx < FONT_W; gx++) {
            if (glyph[gy * FONT_W + gx] == 0) continue;
            for (int sy = 0; sy < S; sy++) {
                i32 py = oy + gy * S + sy;
                if (py < 0 || py >= g_clip_h) continue;
                u32 *row = buf + (u32)py * stride;
                for (int sx = 0; sx < S; sx++) {
                    i32 px = ox + gx * S + sx;
                    if (px < 0 || px >= g_clip_w) continue;
                    row[px] = col;
                }
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Conflict detection for the live board: does value[idx] violate a rule
 * against any *other* filled cell?  (Empty cells never conflict.)
 * --------------------------------------------------------------------- */
static int cell_conflicts(int idx)
{
    int v = B->value[idx];
    if (v == 0) return 0;
    int r = idx / 9, c = idx % 9;
    for (int i = 0; i < 9; i++) {
        int ri = IDX(r, i), ci = IDX(i, c);
        if (ri != idx && B->value[ri] == v) return 1;
        if (ci != idx && B->value[ci] == v) return 1;
    }
    int br = (r / 3) * 3, bc = (c / 3) * 3;
    for (int dr = 0; dr < 3; dr++)
        for (int dc = 0; dc < 3; dc++) {
            int bi = IDX(br + dr, bc + dc);
            if (bi != idx && B->value[bi] == v) return 1;
        }
    return 0;
}

/* Is the live board completely and correctly solved? */
static int is_solved(void)
{
    for (int i = 0; i < 81; i++) {
        if (B->value[i] == 0) return 0;
        if (B->value[i] != B->solution[i]) return 0;
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * Cell background classification for highlighting.
 * --------------------------------------------------------------------- */
static u32 cell_bg(int idx)
{
    if (cell_conflicts(idx)) return C_CONFLICT;
    if (g_sel >= 0) {
        if (idx == g_sel) return C_SEL;
        int sr = g_sel / 9, sc_ = g_sel % 9;
        int r  = idx / 9,  c  = idx % 9;
        int selv = B->value[g_sel];
        /* Same value as selection (and non-zero) -> green wash. */
        if (selv != 0 && B->value[idx] == selv) return C_SAMEVAL;
        /* Same row / col / box -> faint blue wash. */
        int sameBox = (sr / 3 == r / 3) && (sc_ / 3 == c / 3);
        if (r == sr || c == sc_ || sameBox) return C_PEER;
    }
    return C_GRIDBG;
}

/* -----------------------------------------------------------------------
 * Render: pencil marks (3x3 mini-grid of candidate digits) in a cell.
 * --------------------------------------------------------------------- */
static void draw_notes(u32 *buf, u32 stride, int r, int c)
{
    u16 mask = B->notes[IDX(r, c)];
    if (!mask) return;
    i32 x0 = GRID_LEFT + c * CELL;
    i32 y0 = GRID_TOP  + r * CELL;
    int sub = CELL / 3;                             /* ~17 px sub-cell */
    for (int d = 1; d <= 9; d++) {
        if (!(mask & (1u << d))) continue;
        int sr = (d - 1) / 3, sc2 = (d - 1) % 3;
        i32 cx = x0 + sc2 * sub + sub / 2;
        i32 cy = y0 + sr  * sub + sub / 2;
        char ch = (char)('0' + d);
        font_draw_char(buf, (int)stride, g_clip_w, g_clip_h,
                       cx - FONT_W / 2, cy - FONT_H / 2, ch, C_NOTE);
    }
}

/* -----------------------------------------------------------------------
 * Render the 9x9 grid (backgrounds, digits, notes, lines).
 * --------------------------------------------------------------------- */
static void render_grid(u32 *buf, u32 stride)
{
    /* 1. Cell backgrounds. */
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            int idx = IDX(r, c);
            i32 x = GRID_LEFT + c * CELL;
            i32 y = GRID_TOP  + r * CELL;
            fill_rect(buf, stride, x, y, CELL, CELL, cell_bg(idx));
        }
    }

    /* 2. Thin separators between every cell. */
    for (int i = 0; i <= 9; i++) {
        hline(buf, stride, GRID_LEFT, GRID_TOP + i * CELL, 9 * CELL, C_LINE_THIN);
        vline(buf, stride, GRID_LEFT + i * CELL, GRID_TOP, 9 * CELL, C_LINE_THIN);
    }

    /* 3. Digits + notes. */
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            int idx = IDX(r, c);
            i32 cx = GRID_LEFT + c * CELL + CELL / 2;
            i32 cy = GRID_TOP  + r * CELL + CELL / 2;
            int v = B->value[idx];
            if (v == 0) {
                draw_notes(buf, stride, r, c);
            } else {
                u32 col;
                if (cell_conflicts(idx))       col = C_CONFLICT_FG;
                else if (B->given[idx])        col = C_GIVEN;
                else                           col = C_USER;
                draw_big_digit(buf, stride, cx, cy, (char)('0' + v), col);
            }
        }
    }

    /* 4. Bold 3x3 box dividers + outer frame (drawn last, on top). */
    for (int i = 0; i <= 9; i += 3) {
        thick_h(buf, stride, GRID_LEFT, GRID_TOP + i * CELL, 9 * CELL, 3, C_LINE_BOLD);
        thick_v(buf, stride, GRID_LEFT + i * CELL, GRID_TOP, 9 * CELL, 3, C_LINE_BOLD);
    }
}

/* -----------------------------------------------------------------------
 * Render a header button with a centred label.
 * --------------------------------------------------------------------- */
static void draw_button(u32 *buf, u32 stride, i32 x, i32 y, i32 w, i32 h,
                        const char *label, u32 face)
{
    fill_rect(buf, stride, x, y, w, h, face);
    /* Light top/left edge, subtle bottom shade. */
    hline(buf, stride, x, y, w, C_BTN_HL);
    vline(buf, stride, x, y, h, C_BTN_HL);
    hline(buf, stride, x, y + h - 1, w, 0xFF24323Eu);
    vline(buf, stride, x + w - 1, y, h, 0xFF24323Eu);
    int lw = font_text_width(label);
    font_draw_string(buf, (int)stride, g_clip_w, g_clip_h,
                     x + (w - lw) / 2, y + (h - FONT_H) / 2, label, C_WHITE);
}

/* Format elapsed milliseconds as "mm:ss" into buf (>=6 bytes). */
static void fmt_time(char *buf, u64 ms)
{
    u32 secs = (u32)(ms / 1000u);
    u32 mm = secs / 60u, ssv = secs % 60u;
    if (mm > 99) mm = 99;
    buf[0] = (char)('0' + (mm / 10) % 10);
    buf[1] = (char)('0' + mm % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + (ssv / 10) % 10);
    buf[4] = (char)('0' + ssv % 10);
    buf[5] = '\0';
}

/* -----------------------------------------------------------------------
 * Render the header (title, timer, buttons, status/win banner).
 * --------------------------------------------------------------------- */
static void render_header(u32 *buf, u32 stride, u64 now_ms)
{
    fill_rect(buf, stride, 0, 0, WIN_W, HEADER_H, C_HEADER);

    /* Title. */
    font_draw_string(buf, (int)stride, g_clip_w, g_clip_h, 12, 8, "SUDOKU", C_HDR_TEXT);

    /* Timer (mm:ss) under the title. */
    {
        u64 elapsed = g_won ? g_elapsed_ms : (now_ms - g_start_ms);
        char tbuf[8];
        fmt_time(tbuf, elapsed);
        font_draw_string(buf, (int)stride, g_clip_w, g_clip_h, 12, 32, tbuf,
                         g_won ? C_WIN : C_DIM);
    }

    /* Pencil-mode indicator. */
    if (g_notes_mode) {
        font_draw_string(buf, (int)stride, g_clip_w, g_clip_h, 78, 32, "PENCIL", 0xFFF1C40Fu);
    }

    /* Buttons. */
    draw_button(buf, stride, BTN_NEW_X, BTN_NEW_Y, BTN_NEW_W, BTN_NEW_H,
                "New Game", C_BTN);
    draw_button(buf, stride, BTN_DIFF_X, BTN_DIFF_Y, BTN_DIFF_W, BTN_DIFF_H,
                DIFF_NAME[g_difficulty], C_BTN);

    /* Win banner -- a green ribbon across the bottom of the grid area. */
    if (g_won) {
        const char *msg = "SOLVED!  Press New Game";
        int mw = font_text_width(msg);
        i32 by = GRID_BOTTOM + 6;
        fill_rect(buf, stride, GRID_LEFT, by, 9 * CELL, 24, C_WIN);
        font_draw_string(buf, (int)stride, g_clip_w, g_clip_h,
                         GRID_LEFT + (9 * CELL - mw) / 2, by + 4, msg, C_WHITE);
    }
}

/* -----------------------------------------------------------------------
 * Full redraw.
 * --------------------------------------------------------------------- */
static void render_all(u32 *buf, u32 stride, u64 now_ms)
{
    /* Clear the WHOLE live surface (letterbox margins included) so a window
     * larger than the fixed 500x560 canvas shows no stale garbage.  fill_rect
     * clamps to g_clip_w/g_clip_h, so this is bounded to the current buffer. */
    fill_rect(buf, stride, 0, 0, g_clip_w, g_clip_h, C_BG);
    render_grid(buf, stride);
    render_header(buf, stride, now_ms);
}

/* -----------------------------------------------------------------------
 * Hit testing.
 * --------------------------------------------------------------------- */
static int hit_rect(i32 mx, i32 my, i32 x, i32 y, i32 w, i32 h)
{ return mx >= x && mx < x + w && my >= y && my < y + h; }

/* Map a pointer (mx,my) to a grid cell index, or -1 if outside the grid. */
static int px_to_cell(i32 mx, i32 my)
{
    if (mx < GRID_LEFT || mx >= GRID_RIGHT) return -1;
    if (my < GRID_TOP  || my >= GRID_BOTTOM) return -1;
    int c = (mx - GRID_LEFT) / CELL;
    int r = (my - GRID_TOP)  / CELL;
    if (c < 0 || c > 8 || r < 0 || r > 8) return -1;
    return IDX(r, c);
}

/* -----------------------------------------------------------------------
 * Actions.
 * --------------------------------------------------------------------- */
static void new_game(void)
{
    generate(DIFF_CLUES[g_difficulty]);
    g_sel = -1;
    g_won = 0;
    g_start_ms = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0);
    g_elapsed_ms = 0;
    prev_buttons = 0;
    print("[SUDOKU] new game (difficulty ");
    print_i32(g_difficulty);
    print(")\n");
}

/* Enter digit `d` (1..9) into the selected cell, or 0 to clear. */
static void enter_digit(int d)
{
    if (g_sel < 0 || g_won) return;
    if (B->given[g_sel]) return;                    /* can't edit a clue */

    if (g_notes_mode && d != 0) {
        /* Toggle pencil mark; only meaningful when the cell is empty. */
        if (B->value[g_sel] == 0)
            B->notes[g_sel] ^= (u16)(1u << d);
        return;
    }

    if (d == 0) {
        B->value[g_sel] = 0;
        B->notes[g_sel] = 0;
    } else {
        B->value[g_sel] = (u8)d;
        B->notes[g_sel] = 0;                        /* digit clears notes */
    }

    if (is_solved()) {
        g_won = 1;
        g_elapsed_ms = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0) - g_start_ms;
        print("[SUDOKU] solved in ");
        print_i32((i32)(g_elapsed_ms / 1000u));
        print("s\n");
    }
}

/* Move the selection by (dr,dc), wrapping/clamping inside the grid. */
static void move_sel(int dr, int dc)
{
    if (g_sel < 0) { g_sel = IDX(4, 4); return; }
    int r = g_sel / 9 + dr;
    int c = g_sel % 9 + dc;
    if (r < 0) r = 0;
    if (r > 8) r = 8;
    if (c < 0) c = 0;
    if (c > 8) c = 8;
    g_sel = IDX(r, c);
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    print("[SUDOKU] starting\n");

    if (wl_connect() != 0) {
        print("[SUDOKU] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Sudoku");
    if (!win) {
        print("[SUDOKU] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    u32 stride = win->stride / 4u;
    g_clip_w = (i32)win->w;          /* live clip = actual surface size */
    g_clip_h = (i32)win->h;

    /* Allocate the board from the static arena. */
    B = (board_t *)kmalloc((u32)sizeof(board_t));
    if (!B) {
        print("[SUDOKU] board alloc FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    rng_seed();
    new_game();

    u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0);
    render_all(win->pixels, stride, now);
    wl_commit(win);

    u64 last_clock = now;

    for (;;) {
        int kind, ea, eb, ec;
        int dirty = 0;

        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea, my = (i32)eb, buttons = (i32)ec;
                int left_now  = (buttons & 1);
                int left_prev = (prev_buttons & 1);

                if (left_now && !left_prev) {       /* rising edge = click */
                    if (hit_rect(mx, my, BTN_NEW_X, BTN_NEW_Y, BTN_NEW_W, BTN_NEW_H)) {
                        new_game();
                        dirty = 1;
                    } else if (hit_rect(mx, my, BTN_DIFF_X, BTN_DIFF_Y,
                                        BTN_DIFF_W, BTN_DIFF_H)) {
                        g_difficulty = (g_difficulty + 1) & 3;
                        new_game();
                        dirty = 1;
                    } else {
                        int cell = px_to_cell(mx, my);
                        if (cell >= 0) { g_sel = cell; dirty = 1; }
                    }
                }
                prev_buttons = buttons;
            } else if (kind == WL_EVENT_KEY) {
                int code = ea, pressed = eb;
                if (!pressed) continue;
                switch (code) {
                case KEY_ESC:
                    sc(SYS_EXIT, 0, 0, 0);
                    break;
                case KEY_1: enter_digit(1); dirty = 1; break;
                case KEY_2: enter_digit(2); dirty = 1; break;
                case KEY_3: enter_digit(3); dirty = 1; break;
                case KEY_4: enter_digit(4); dirty = 1; break;
                case KEY_5: enter_digit(5); dirty = 1; break;
                case KEY_6: enter_digit(6); dirty = 1; break;
                case KEY_7: enter_digit(7); dirty = 1; break;
                case KEY_8: enter_digit(8); dirty = 1; break;
                case KEY_9: enter_digit(9); dirty = 1; break;
                case KEY_0:
                case KEY_BACKSPACE: enter_digit(0); dirty = 1; break;
                case KEY_UP:    move_sel(-1, 0); dirty = 1; break;
                case KEY_DOWN:  move_sel( 1, 0); dirty = 1; break;
                case KEY_LEFT:  move_sel( 0,-1); dirty = 1; break;
                case KEY_RIGHT: move_sel( 0, 1); dirty = 1; break;
                case KEY_N: new_game(); dirty = 1; break;
                case KEY_D:
                    g_difficulty = (g_difficulty + 1) & 3;
                    new_game();
                    dirty = 1;
                    break;
                case KEY_P:
                    g_notes_mode = !g_notes_mode;
                    dirty = 1;
                    break;
                default: break;
                }
            } else if (kind == WL_EVENT_RESIZE) {
                /* The lib has ALREADY reallocated and updated win->{w,h,stride,
                 * pixels}.  Refresh our cached stride and the live clip bounds so
                 * every subsequent write is bounded to the new surface, then force
                 * a full redraw (clears the letterbox margins). */
                stride   = win->stride / 4u;
                g_clip_w = (i32)win->w;
                g_clip_h = (i32)win->h;
                dirty = 1;
            }
        }

        /* Tick the on-screen clock about twice a second while playing. */
        now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0);
        if (!g_won && (now - last_clock) >= 500u) {
            last_clock = now;
            dirty = 1;
        }

        if (dirty) {
            render_all(win->pixels, stride, now);
            wl_commit(win);
        }

        sc(SYS_YIELD, 0, 0, 0);
    }
}
