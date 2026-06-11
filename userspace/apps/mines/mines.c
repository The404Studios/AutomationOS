/*
 * mines.c -- Classic Minesweeper (freestanding, ring 3).
 * =======================================================
 *
 * 9x9 grid, 10 mines.  Left-click reveals; right-click (button bit 1)
 * or shift-click toggles a flag.  First-click safety: mines are placed
 * after the first reveal so the clicked cell is never a mine.
 * Flood-fill auto-reveals all contiguous zero cells on reveal.
 * Win when all non-mine cells are revealed.
 *
 * Window: CELL_SIZE=30 px cells → 270 px wide, HEADER_H=50 px header,
 * total 270 x 320.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/mines/mines.c -o /tmp/mn.o
 *   gcc <same flags> -c userspace/lib/wl/wl_client.c  -o /tmp/wlc.o
 *   gcc <same flags> -c userspace/lib/font/bitfont.c   -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/mn.o /tmp/wlc.o /tmp/bf.o -o /tmp/mn.elf
 *   objdump -d /tmp/mn.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [MINES] starting
 *   [MINES] win
 *   [MINES] boom
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall helpers.
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_RANDOM       43

static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* sc6 for WRITE (needs 3 user args, rest 0) */
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
typedef unsigned int  u32;
typedef int           i32;
typedef unsigned char u8;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc6(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

/* Integer-to-decimal string (up to 10 digits).  Returns pointer to buf. */
static char *itoa(i32 v, char *buf)
{
    if (v < 0) { buf[0] = '-'; itoa(-v, buf + 1); return buf; }
    char tmp[12]; int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/* -----------------------------------------------------------------------
 * Randomness: SYS_RANDOM=43, fallback LCG.
 * --------------------------------------------------------------------- */
static u32 lcg_state = 0x12345678u;

static u32 rand_u32(void)
{
    /* Lehmer LCG */
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

static void seed_lcg(void)
{
    long t = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    if (t > 0) lcg_state = (u32)t ^ 0xDEADBEEFu;
}

/* Fill buf[len] with random bytes; fallback to LCG if syscall fails. */
static void get_random(u8 *buf, int len)
{
    long r = sc(SYS_RANDOM, (long)buf, len, 0);
    if (r < 0) {
        seed_lcg();
        for (int i = 0; i < len; i++) {
            if ((i & 3) == 0) rand_u32();
            buf[i] = (u8)(lcg_state >> ((i & 3) * 8));
        }
    }
}

/* -----------------------------------------------------------------------
 * Game constants.
 * --------------------------------------------------------------------- */
#define COLS        9
#define ROWS        9
#define NUM_MINES   10

#define CELL_SIZE   30   /* px per cell */
#define HEADER_H    50   /* px for header bar */

#define WIN_W   (COLS * CELL_SIZE)           /* 270 */
#define WIN_H   (ROWS * CELL_SIZE + HEADER_H) /* 320 */

/* Cell state flags (stored in cell_state[]). */
#define CS_MINE      0x01
#define CS_REVEALED  0x02
#define CS_FLAGGED   0x04

/* Game state enum. */
#define GS_IDLE    0   /* waiting for first click */
#define GS_PLAYING 1
#define GS_WIN     2
#define GS_LOSE    3

/* -----------------------------------------------------------------------
 * Colour palette.
 * --------------------------------------------------------------------- */
#define C_BG_HEADER  0xFF3C3C3Cu
#define C_CELL_UP    0xFFC0C0C0u   /* unrevealed face */
#define C_CELL_DN    0xFFB0B0B0u   /* revealed face */
#define C_MINE_BG    0xFFFF2020u   /* mine revealed = red */
#define C_FLAG       0xFFFF4400u
#define C_TEXT       0xFF202020u
#define C_WHITE      0xFFFFFFFFu
#define C_BLACK      0xFF000000u
#define C_SHADOW     0xFF808080u
#define C_HIGHLIGHT  0xFFEEEEEEu
#define C_BTN_FACE   0xFF909090u

/* Classic minesweeper number colours. */
static const u32 NUM_COLOR[9] = {
    0xFF000000u,   /* 0 – unused */
    0xFF0000FFu,   /* 1 blue */
    0xFF007B00u,   /* 2 dark green */
    0xFFFF0000u,   /* 3 red */
    0xFF00007Bu,   /* 4 dark blue */
    0xFF7B0000u,   /* 5 dark red */
    0xFF007B7Bu,   /* 6 teal */
    0xFF000000u,   /* 7 black */
    0xFF808080u,   /* 8 grey */
};

/* -----------------------------------------------------------------------
 * Game state (globals – no dynamic allocation).
 * --------------------------------------------------------------------- */
static u8  cell_state[ROWS * COLS];   /* bitmask: CS_MINE|CS_REVEALED|CS_FLAGGED */
static u8  cell_num  [ROWS * COLS];   /* adjacent mine count (0-8) */

static int game_state  = GS_IDLE;
static int flags_placed = 0;
static int revealed_count = 0;        /* non-mine cells revealed */

/* Previous pointer info for right-click edge detection. */
static i32 prev_buttons = 0;

/* Live surface geometry, refreshed from win->{w,h,stride} every frame so a
 * compositor resize (Maximize/snap) never leaves us writing past the buffer
 * with a stale stride.  The board is a FIXED WIN_W x WIN_H canvas; on a larger
 * window we letterbox (clear the margins) and on a smaller window every write
 * is clipped to these live bounds so we cannot overflow. */
static i32 g_clip_w = WIN_W;   /* current buffer width  in pixels */
static i32 g_clip_h = WIN_H;   /* current buffer height in pixels */

/* -----------------------------------------------------------------------
 * Drawing primitives.
 * --------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    for (i32 yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= g_clip_h) continue;
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= g_clip_w) continue;
            row[xx] = color;
        }
    }
}

static void hline(u32 *buf, u32 stride_px,
                  i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, stride_px, x, y, len, 1, color);
}

static void vline(u32 *buf, u32 stride_px,
                  i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, stride_px, x, y, 1, len, color);
}

/*
 * Draw a raised 3-D beveled rectangle (light top/left, dark bottom/right).
 * depth = number of bevel pixels.
 */
static void draw_raised(u32 *buf, u32 stride_px,
                         i32 x, i32 y, i32 w, i32 h)
{
    /* Fill face */
    fill_rect(buf, stride_px, x + 2, y + 2, w - 4, h - 4, C_CELL_UP);
    /* Light edges (top, left) */
    for (i32 d = 0; d < 2; d++) {
        hline(buf, stride_px, x + d, y + d, w - d * 2, C_HIGHLIGHT);
        vline(buf, stride_px, x + d, y + d, h - d * 2, C_HIGHLIGHT);
    }
    /* Dark edges (bottom, right) */
    for (i32 d = 0; d < 2; d++) {
        hline(buf, stride_px, x + d, y + h - 1 - d, w - d * 2, C_SHADOW);
        vline(buf, stride_px, x + w - 1 - d, y + d, h - d * 2, C_SHADOW);
    }
}

/*
 * Draw a sunken (revealed) cell face.
 */
static void draw_sunken(u32 *buf, u32 stride_px,
                          i32 x, i32 y, i32 w, i32 h)
{
    fill_rect(buf, stride_px, x + 1, y + 1, w - 2, h - 2, C_CELL_DN);
    /* Dark top/left (sunken look) */
    hline(buf, stride_px, x, y, w, C_SHADOW);
    vline(buf, stride_px, x, y, h, C_SHADOW);
    /* Light bottom/right */
    hline(buf, stride_px, x + 1, y + h - 1, w - 1, C_HIGHLIGHT);
    vline(buf, stride_px, x + w - 1, y + 1, h - 1, C_HIGHLIGHT);
}

/* Draw a small mine symbol (circle with spikes) at cell pixel origin. */
static void draw_mine_symbol(u32 *buf, u32 stride_px, i32 cx, i32 cy)
{
    /* Core circle radius 5 */
    i32 r = 5;
    for (i32 dy = -r; dy <= r; dy++) {
        for (i32 dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r) {
                i32 px = cx + dx, py = cy + dy;
                if (px >= 0 && px < g_clip_w && py >= 0 && py < g_clip_h)
                    buf[(u32)py * stride_px + (u32)px] = C_BLACK;
            }
        }
    }
    /* 4 cardinal spikes */
    for (i32 i = 1; i <= r + 2; i++) {
        i32 pts[4][2] = {
            {cx, cy - i}, {cx, cy + i}, {cx - i, cy}, {cx + i, cy}
        };
        for (int p = 0; p < 4; p++) {
            i32 px = pts[p][0], py = pts[p][1];
            if (px >= 0 && px < g_clip_w && py >= 0 && py < g_clip_h)
                buf[(u32)py * stride_px + (u32)px] = C_BLACK;
        }
    }
    /* Diagonal spikes */
    for (i32 i = 1; i <= r - 1; i++) {
        i32 pts[4][2] = {
            {cx + i, cy - i}, {cx - i, cy - i},
            {cx + i, cy + i}, {cx - i, cy + i}
        };
        for (int p = 0; p < 4; p++) {
            i32 px = pts[p][0], py = pts[p][1];
            if (px >= 0 && px < g_clip_w && py >= 0 && py < g_clip_h)
                buf[(u32)py * stride_px + (u32)px] = C_BLACK;
        }
    }
    /* Highlight dot */
    i32 hx = cx - 2, hy = cy - 2;
    if (hx >= 0 && hy >= 0 && hx < g_clip_w && hy < g_clip_h)
        buf[(u32)hy * stride_px + (u32)hx] = C_WHITE;
}

/* Draw a flag at cell pixel origin. */
static void draw_flag_symbol(u32 *buf, u32 stride_px, i32 cx, i32 cy)
{
    /* Pole: vertical line */
    for (i32 i = -10; i <= 6; i++) {
        i32 py = cy + i;
        if (py >= 0 && py < g_clip_h && cx >= 0 && cx < g_clip_w)
            buf[(u32)py * stride_px + (u32)cx] = C_BLACK;
    }
    /* Flag triangle */
    for (i32 row = 0; row < 8; row++) {
        i32 py = cy - 10 + row;
        for (i32 col = 0; col <= row && col < 8; col++) {
            i32 px = cx + 1 + col;
            if (px >= 0 && px < g_clip_w && py >= 0 && py < g_clip_h)
                buf[(u32)py * stride_px + (u32)px] = C_FLAG;
        }
    }
    /* Base */
    for (i32 dx = -4; dx <= 4; dx++) {
        i32 px = cx + dx, py = cy + 7;
        if (px >= 0 && px < g_clip_w && py >= 0 && py < g_clip_h)
            buf[(u32)py * stride_px + (u32)px] = C_BLACK;
    }
}

/* -----------------------------------------------------------------------
 * Render one cell.
 * --------------------------------------------------------------------- */
static void render_cell(u32 *buf, u32 stride_px, int row, int col, int show_mines)
{
    i32 x = col * CELL_SIZE;
    i32 y = HEADER_H + row * CELL_SIZE;
    u8  state = cell_state[row * COLS + col];

    if (state & CS_REVEALED) {
        /* Sunken face */
        if (state & CS_MINE) {
            /* Mine revealed: red background */
            draw_sunken(buf, stride_px, x, y, CELL_SIZE, CELL_SIZE);
            fill_rect(buf, stride_px, x + 1, y + 1, CELL_SIZE - 2, CELL_SIZE - 2, C_MINE_BG);
            draw_mine_symbol(buf, stride_px, x + CELL_SIZE/2, y + CELL_SIZE/2);
        } else {
            draw_sunken(buf, stride_px, x, y, CELL_SIZE, CELL_SIZE);
            u8 num = cell_num[row * COLS + col];
            if (num > 0) {
                char nbuf[3];
                nbuf[0] = (char)('0' + num);
                nbuf[1] = '\0';
                /* Center text: FONT_W=8, FONT_H=16 */
                i32 tx = x + (CELL_SIZE - FONT_W) / 2;
                i32 ty = y + (CELL_SIZE - FONT_H) / 2;
                font_draw_string(buf, (int)stride_px, g_clip_w, g_clip_h,
                                 tx, ty, nbuf, NUM_COLOR[num]);
            }
        }
    } else if (state & CS_FLAGGED) {
        draw_raised(buf, stride_px, x, y, CELL_SIZE, CELL_SIZE);
        draw_flag_symbol(buf, stride_px, x + CELL_SIZE/2, y + CELL_SIZE/2);
    } else if (show_mines && (state & CS_MINE)) {
        /* Game over: reveal unflagged mines */
        draw_sunken(buf, stride_px, x, y, CELL_SIZE, CELL_SIZE);
        draw_mine_symbol(buf, stride_px, x + CELL_SIZE/2, y + CELL_SIZE/2);
    } else {
        draw_raised(buf, stride_px, x, y, CELL_SIZE, CELL_SIZE);
    }
}

/* -----------------------------------------------------------------------
 * Render header bar.
 * --------------------------------------------------------------------- */
#define BTN_W  80
#define BTN_H  30
#define BTN_X  (WIN_W/2 - BTN_W/2)
#define BTN_Y  10

static void render_header(u32 *buf, u32 stride_px)
{
    fill_rect(buf, stride_px, 0, 0, WIN_W, HEADER_H, C_BG_HEADER);

    /* Mine counter (left): mines - flags */
    int remaining = NUM_MINES - flags_placed;
    char cbuf[12];
    itoa(remaining, cbuf);
    font_draw_string(buf, (int)stride_px, g_clip_w, g_clip_h,
                     6, HEADER_H/2 - FONT_H/2, cbuf, 0xFFFF4444u);

    /* Status / restart button (center) */
    const char *label;
    u32 btn_text_color = C_TEXT;
    if (game_state == GS_WIN) {
        label = "You win!";
        btn_text_color = 0xFF006600u;
    } else if (game_state == GS_LOSE) {
        label = "Boom!";
        btn_text_color = 0xFFCC0000u;
    } else {
        label = "Playing";
    }

    /* Draw button */
    fill_rect(buf, stride_px, BTN_X, BTN_Y, BTN_W, BTN_H, C_BTN_FACE);
    /* Raised border for button */
    hline(buf, stride_px, BTN_X,         BTN_Y,          BTN_W, C_HIGHLIGHT);
    hline(buf, stride_px, BTN_X,         BTN_Y + BTN_H-1, BTN_W, C_SHADOW);
    vline(buf, stride_px, BTN_X,         BTN_Y,          BTN_H, C_HIGHLIGHT);
    vline(buf, stride_px, BTN_X+BTN_W-1, BTN_Y,          BTN_H, C_SHADOW);

    int lw = font_text_width(label);
    font_draw_string(buf, (int)stride_px, g_clip_w, g_clip_h,
                     BTN_X + (BTN_W - lw) / 2,
                     BTN_Y + (BTN_H - FONT_H) / 2,
                     label, btn_text_color);
}

/* -----------------------------------------------------------------------
 * Full redraw.
 *
 * Re-reads the live surface geometry from `win` so a compositor resize is
 * handled with zero stale state: stride is recomputed, the clip bounds are
 * refreshed, and the WHOLE buffer is cleared to black first (letterbox) so
 * the margins around the fixed WIN_W x WIN_H board are never garbage.  The
 * board is then drawn at the top-left; every primitive clips to g_clip_w/h.
 * --------------------------------------------------------------------- */
static void render_all(wl_window *win)
{
    u32 stride_px = win->stride / 4u;

    /* Live clip bounds: clamp so a SMALLER window can never overflow the
     * fixed-canvas blit, and a LARGER window gets its full surface cleared. */
    g_clip_w = (i32)win->w;
    g_clip_h = (i32)win->h;

    u32 *buf = win->pixels;

    /* Clear the FULL current surface (letterbox margins) before drawing. */
    fill_rect(buf, stride_px, 0, 0, g_clip_w, g_clip_h, C_BLACK);

    int show_mines = (game_state == GS_LOSE);
    render_header(buf, stride_px);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            render_cell(buf, stride_px, r, c, show_mines);
}

/* -----------------------------------------------------------------------
 * Game logic.
 * --------------------------------------------------------------------- */

static void board_reset(void)
{
    for (int i = 0; i < ROWS * COLS; i++) {
        cell_state[i] = 0;
        cell_num[i]   = 0;
    }
    flags_placed   = 0;
    revealed_count = 0;
    game_state     = GS_IDLE;
    prev_buttons   = 0;
}

/* Count adjacent mines for all cells. */
static void compute_numbers(void)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (cell_state[r * COLS + c] & CS_MINE) continue;
            int cnt = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr, nc = c + dc;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                    if (cell_state[nr * COLS + nc] & CS_MINE) cnt++;
                }
            }
            cell_num[r * COLS + c] = (u8)cnt;
        }
    }
}

/* Place NUM_MINES mines randomly, avoiding (safe_r, safe_c). */
static void place_mines(int safe_r, int safe_c)
{
    u8 rnd_buf[NUM_MINES * 4];
    get_random(rnd_buf, (int)sizeof(rnd_buf));
    int placed = 0;
    int attempts = 0;
    /* Build list of candidate cells */
    int candidates[ROWS * COLS];
    int ncand = 0;
    for (int i = 0; i < ROWS * COLS; i++) {
        int r = i / COLS, c = i % COLS;
        /* Exclude the safe cell and its 8 neighbors */
        int dr = r - safe_r, dc = c - safe_c;
        if (dr >= -1 && dr <= 1 && dc >= -1 && dc <= 1) continue;
        candidates[ncand++] = i;
    }
    /* Fisher-Yates shuffle first NUM_MINES elements */
    /* Re-use rnd_buf, top up with more random if needed */
    u8 shuf_buf[ROWS * COLS * 2];
    get_random(shuf_buf, (int)sizeof(shuf_buf));
    for (int i = 0; i < ncand && placed < NUM_MINES; i++) {
        /* Pick random j in [i, ncand) */
        u8 rv = shuf_buf[i < (int)sizeof(shuf_buf) ? i : i % (int)sizeof(shuf_buf)];
        int j = i + (int)(rv % (u8)(ncand - i));
        if (j >= ncand) j = ncand - 1;
        /* Swap */
        int tmp = candidates[i]; candidates[i] = candidates[j]; candidates[j] = tmp;
        cell_state[candidates[i]] |= CS_MINE;
        placed++;
    }
    (void)attempts;
    (void)rnd_buf;
    compute_numbers();
}

/* Flood-fill reveal (BFS with explicit stack to avoid recursion depth). */
#define STACK_MAX (ROWS * COLS)
static int bfs_stack[STACK_MAX];

static void flood_reveal(int start_r, int start_c)
{
    int head = 0, tail = 0;
    bfs_stack[tail++] = start_r * COLS + start_c;

    while (head < tail) {
        int idx = bfs_stack[head++];
        int r = idx / COLS, c = idx % COLS;

        if (cell_state[idx] & CS_REVEALED) continue;
        if (cell_state[idx] & CS_FLAGGED)  continue;
        if (cell_state[idx] & CS_MINE)     continue;

        cell_state[idx] |= CS_REVEALED;
        revealed_count++;

        /* Only spread from zero cells */
        if (cell_num[idx] != 0) continue;

        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                int ni = nr * COLS + nc;
                if (!(cell_state[ni] & CS_REVEALED) &&
                    !(cell_state[ni] & CS_FLAGGED) &&
                    !(cell_state[ni] & CS_MINE)) {
                    if (tail < STACK_MAX)
                        bfs_stack[tail++] = ni;
                }
            }
        }
    }
}

static void check_win(void)
{
    /* Win: every non-mine cell is revealed. */
    int target = ROWS * COLS - NUM_MINES;
    if (revealed_count >= target) {
        game_state = GS_WIN;
        print("[MINES] win\n");
    }
}

/* Handle left-click on cell (row, col). */
static void cell_left_click(int row, int col)
{
    if (game_state == GS_WIN || game_state == GS_LOSE) return;

    int idx = row * COLS + col;
    if (cell_state[idx] & CS_REVEALED) return;
    if (cell_state[idx] & CS_FLAGGED)  return;

    if (game_state == GS_IDLE) {
        /* First click: place mines avoiding this cell, then start game. */
        place_mines(row, col);
        game_state = GS_PLAYING;
    }

    if (cell_state[idx] & CS_MINE) {
        /* Hit a mine */
        cell_state[idx] |= CS_REVEALED;
        game_state = GS_LOSE;
        print("[MINES] boom\n");
        return;
    }

    flood_reveal(row, col);
    check_win();
}

/* Handle right-click (flag toggle) on cell (row, col). */
static void cell_right_click(int row, int col)
{
    if (game_state == GS_WIN || game_state == GS_LOSE) return;

    int idx = row * COLS + col;
    if (cell_state[idx] & CS_REVEALED) return;

    if (cell_state[idx] & CS_FLAGGED) {
        cell_state[idx] &= (u8)~CS_FLAGGED;
        flags_placed--;
    } else {
        cell_state[idx] |= CS_FLAGGED;
        flags_placed++;
    }
}

/* -----------------------------------------------------------------------
 * Hit test: is click on restart button?
 * --------------------------------------------------------------------- */
static int hit_restart(i32 mx, i32 my)
{
    return (mx >= BTN_X && mx < BTN_X + BTN_W &&
            my >= BTN_Y && my < BTN_Y + BTN_H);
}

/* Convert pixel (mx, my) in grid area to (row, col), or -1 if out of grid. */
static int px_to_cell(i32 mx, i32 my, int *out_row, int *out_col)
{
    if (my < HEADER_H) return 0;
    i32 gy = my - HEADER_H;
    i32 col = mx / CELL_SIZE;
    i32 row = gy / CELL_SIZE;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return 0;
    *out_row = (int)row;
    *out_col = (int)col;
    return 1;
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    print("[MINES] starting\n");

    if (wl_connect() != 0) {
        print("[MINES] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Minesweeper");
    if (!win) {
        print("[MINES] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    board_reset();
    render_all(win);   /* reads live win->{w,h,stride,pixels} */
    wl_commit(win);

    /* Main event loop. */
    for (;;) {
        int kind, ea, eb, ec;
        int dirty = 0;

        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_RESIZE) {
                /* The library already reallocated the buffer and updated
                 * win->{w,h,stride,pixels}.  We just need a full redraw:
                 * render_all() re-reads the live geometry, clears the WHOLE
                 * new surface (letterbox margins) and re-blits the board. */
                dirty = 1;
                continue;
            }
            if (kind != WL_EVENT_POINTER) continue;

            i32 mx      = (i32)ea;
            i32 my      = (i32)eb;
            i32 buttons = (i32)ec;

            /* Left button: bit 0. */
            int left_now  = (buttons & 1);
            int left_prev = (prev_buttons & 1);
            /* Right button: bit 1. */
            int right_now  = (buttons & 2);
            int right_prev = (prev_buttons & 2);
            /* Shift key not visible here; right button (bit 1) used for flag. */

            /* Left click (rising edge). */
            if (left_now && !left_prev) {
                if (hit_restart(mx, my)) {
                    board_reset();
                    dirty = 1;
                } else {
                    int row, col;
                    if (px_to_cell(mx, my, &row, &col)) {
                        cell_left_click(row, col);
                        dirty = 1;
                    }
                }
            }

            /* Right click (rising edge). */
            if (right_now && !right_prev) {
                int row, col;
                if (px_to_cell(mx, my, &row, &col)) {
                    cell_right_click(row, col);
                    dirty = 1;
                }
            }

            prev_buttons = buttons;
        }

        if (dirty) {
            render_all(win);   /* re-reads live geometry; safe after resize */
            wl_commit(win);
        }

        sc(SYS_YIELD, 0, 0, 0);
    }
}
