/*
 * snake.c -- Classic Snake game (freestanding, ring 3).
 * =====================================================
 *
 * A 480x480 window with a 24x24 grid of 20x20-pixel cells.
 * The top 20 pixels show the score via bitfont; the grid occupies
 * rows 20..479 (460px = 23 rows of 20px each -- the top row is the
 * HUD strip, so the play area is rows 1..23 in cell space).
 *
 * Actually we give a full 480px height and place the score in a
 * 20px HUD at the very top; the snake grid starts at pixel y=20.
 * Grid: GRID_COLS=24 columns, GRID_ROWS=23 rows (pixels 20..479).
 *
 * No libc: freestanding inline syscalls + wl_client + bitfont.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/snake/snake.c   -o /tmp/snake.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c   -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c   -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/snake.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/snake.elf
 *   objdump -d /tmp/snake.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [SNAKE] starting
 *   [SNAKE] score N          (on death)
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* ---- syscall numbers ---- */
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

/* ---- inline syscall (6-arg form for full generality) ---- */
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

/* Print unsigned decimal integer to serial. */
static void print_u32(u32 v)
{
    char b[12];
    i32 i = 0;
    if (v == 0) { sc(SYS_WRITE, 1, (long)"0", 1, 0, 0, 0); return; }
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    /* reverse */
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

/* ---- window / grid constants ---- */
#define WIN_W       480
#define WIN_H       480

#define HUD_H        20          /* pixels reserved for score bar at top   */
#define CELL_PX      20          /* pixels per cell                        */
#define GRID_COLS    24          /* cells across                           */
#define GRID_ROWS    23          /* cells down (460 / 20 = 23)             */

/* Pixel origin of cell (col, row) */
#define CELL_X(col)  ((col) * CELL_PX)
#define CELL_Y(row)  (HUD_H + (row) * CELL_PX)

/* ---- colours ---- */
#define COL_BG        0xFF1A1A2E   /* dark navy background          */
#define COL_GRID      0xFF16213E   /* faint grid shade              */
#define COL_HUD_BG    0xFF0F3460   /* HUD strip background          */
#define COL_SNAKE_HD  0xFF4CC9F0   /* snake head: bright cyan       */
#define COL_SNAKE_BD  0xFF4361EE   /* snake body: blue-violet       */
#define COL_FOOD      0xFFF72585   /* food: hot pink                */
#define COL_WHITE     0xFFFFFFFF
#define COL_YELLOW    0xFFFFD60A
#define COL_RED       0xFFFF4D6D

/* ---- snake ring-buffer ---- */
#define MAX_SNAKE  (GRID_COLS * GRID_ROWS)   /* 552 cells max */

typedef struct { i32 x, y; } cell_t;

static cell_t g_snake[MAX_SNAKE];   /* ring buffer of body cells      */
static i32    g_head;               /* index of head in ring          */
static i32    g_len;                /* current snake length           */
static cell_t g_food;               /* current food cell              */
static u32    g_score;              /* apples eaten                   */

/* Direction: 0=right,1=down,2=left,3=up */
static i32    g_dir;
static i32    g_next_dir;           /* buffered next direction        */
static i32    g_game_over;          /* non-zero when dead             */
static i32    g_paused;             /* non-zero when paused           */

/* ---- LCG random (seeded from ticks at first food placement) ---- */
static u32 g_rand_state = 12345u;

static u32 lcg_rand(void)
{
    g_rand_state = g_rand_state * 1664525u + 1013904223u;
    return g_rand_state;
}

/* ---- draw helpers ---- */

/*
 * Letterbox clamp bounds, refreshed every frame from the live window so a
 * SMALLER window cannot overflow the buffer and a LARGER window's margins
 * are bounded by the real surface. The fixed 480x480 canvas is blitted at
 * the top-left; writes are clamped to min(canvas, window).
 */
static i32 g_clip_w = WIN_W;   /* = min(WIN_W, win->w) */
static i32 g_clip_h = WIN_H;   /* = min(WIN_H, win->h) */

/* fill_rect: pixel rectangle into ARGB32 buffer; stride_px = pixels/row.
 * Clamped to the current window via g_clip_w/g_clip_h (set per frame). */
static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > g_clip_w) x2 = g_clip_w;
    i32 y2 = y + h; if (y2 > g_clip_h) y2 = g_clip_h;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/* Draw a single grid cell with a 1px inner margin for grid-line effect. */
static void draw_cell(u32 *buf, u32 stride_px, i32 col, i32 row, u32 color)
{
    i32 px = CELL_X(col) + 1;
    i32 py = CELL_Y(row) + 1;
    fill_rect(buf, stride_px, px, py, CELL_PX - 2, CELL_PX - 2, color);
}

/* ring-buffer accessor: index i relative to tail (0 = tail, len-1 = head) */
static cell_t snake_at(i32 i)
{
    i32 idx = (g_head - (g_len - 1) + i + MAX_SNAKE * 2) % MAX_SNAKE;
    return g_snake[idx];
}

/* ---- food placement ---- */
static void place_food(void)
{
    /* Try random positions until one is not occupied by the snake. */
    for (i32 attempts = 0; attempts < 1024; attempts++) {
        i32 fx = (i32)(lcg_rand() % (u32)GRID_COLS);
        i32 fy = (i32)(lcg_rand() % (u32)GRID_ROWS);
        /* Check collision with snake body */
        i32 occupied = 0;
        for (i32 j = 0; j < g_len; j++) {
            cell_t c = snake_at(j);
            if (c.x == fx && c.y == fy) { occupied = 1; break; }
        }
        if (!occupied) {
            g_food.x = fx;
            g_food.y = fy;
            return;
        }
    }
    /* Fallback: place at (0,0) if grid is nearly full */
    g_food.x = 0;
    g_food.y = 0;
}

/* ---- game init / reset ---- */
static void game_init(u64 ticks)
{
    /* Seed RNG with current ticks for variety between games. */
    g_rand_state = (u32)(ticks ^ (ticks >> 16));
    if (g_rand_state == 0) g_rand_state = 0xDEADBEEFu;

    /* Start snake at center, length 3, moving right. */
    g_len  = 3;
    g_head = 2;
    g_dir  = 0;
    g_next_dir = 0;
    g_score    = 0;
    g_game_over = 0;
    g_paused    = 0;

    i32 cx = GRID_COLS / 2;
    i32 cy = GRID_ROWS / 2;
    g_snake[0].x = cx - 2;  g_snake[0].y = cy;   /* tail  */
    g_snake[1].x = cx - 1;  g_snake[1].y = cy;   /* body  */
    g_snake[2].x = cx;      g_snake[2].y = cy;   /* head  */

    place_food();
}

/* ---- advance one tick ---- */
static void game_tick(void)
{
    if (g_game_over || g_paused) return;

    /* Apply buffered direction (ignore reversals). */
    i32 d = g_next_dir;
    /* Reversal test: opposite directions differ by 2 mod 4 */
    i32 opposite = (g_dir + 2) & 3;
    if (d != opposite) g_dir = d;

    /* Compute new head position. */
    cell_t head = g_snake[g_head];
    cell_t nh = head;
    switch (g_dir) {
    case 0: nh.x++; break;  /* right */
    case 1: nh.y++; break;  /* down  */
    case 2: nh.x--; break;  /* left  */
    case 3: nh.y--; break;  /* up    */
    }

    /* Wall collision */
    if (nh.x < 0 || nh.x >= GRID_COLS || nh.y < 0 || nh.y >= GRID_ROWS) {
        g_game_over = 1;
        return;
    }

    /* Self collision: check all cells except tail (tail moves away). */
    for (i32 j = 1; j < g_len; j++) {
        cell_t c = snake_at(j);
        if (c.x == nh.x && c.y == nh.y) {
            g_game_over = 1;
            return;
        }
    }

    /* Check food. */
    i32 ate = (nh.x == g_food.x && nh.y == g_food.y);

    /* Advance head in ring buffer. */
    g_head = (g_head + 1) % MAX_SNAKE;
    g_snake[g_head] = nh;

    if (ate) {
        g_len++;
        if (g_len > MAX_SNAKE) g_len = MAX_SNAKE;
        g_score++;
        place_food();
    }
    /* If not ate, the tail naturally disappears because g_len stays the
     * same -- the ring-buffer head moved forward, so the effective tail
     * index is now one further along. */
}

/* ---- render ---- */
/*
 * win_w/win_h/stride_px are the LIVE window geometry (re-read each frame).
 * The play canvas is a fixed 480x480 grid blitted at the top-left; on a
 * resized (maximized/snapped) window we letterbox: clear the WHOLE surface
 * to the background so the margins around the canvas are never stale garbage.
 */
static void render(u32 *buf, u32 stride_px, i32 win_w, i32 win_h)
{
    /* 0. Letterbox: paint the ENTIRE live surface to the background first so
     *    margins beyond the fixed 480x480 canvas are clean. Bounded to the
     *    live window using the live stride -- no overflow on a smaller window. */
    {
        i32 fw = win_w < 0 ? 0 : win_w;
        i32 fh = win_h < 0 ? 0 : win_h;
        for (i32 yy = 0; yy < fh; yy++) {
            u32 *row = buf + (u32)yy * stride_px;
            for (i32 xx = 0; xx < fw; xx++) row[xx] = COL_BG;
        }
    }

    /* 1. Clear full window (canvas region, clamped to min(canvas,window)). */
    fill_rect(buf, stride_px, 0, 0, WIN_W, WIN_H, COL_BG);

    /* 2. HUD strip. */
    fill_rect(buf, stride_px, 0, 0, WIN_W, HUD_H, COL_HUD_BG);

    /* Score text in HUD. */
    {
        char score_str[32];
        i32 n = 0;
        /* "SCORE: " */
        const char *prefix = "SCORE: ";
        for (i32 i = 0; prefix[i]; i++) score_str[n++] = prefix[i];
        char num[12];
        i32 nlen = fmt_u32(num, g_score);
        for (i32 i = 0; i < nlen; i++) score_str[n++] = num[i];
        score_str[n] = '\0';
        font_draw_string(buf, (i32)stride_px, g_clip_w, g_clip_h,
                         4, 2, score_str, COL_WHITE);
    }

    /* 3. Draw grid lines (faint alternate background for cells). */
    for (i32 row = 0; row < GRID_ROWS; row++) {
        for (i32 col = 0; col < GRID_COLS; col++) {
            /* Only draw the 1px border of each cell to create grid effect. */
            i32 px = CELL_X(col);
            i32 py = CELL_Y(row);
            /* Top edge */
            fill_rect(buf, stride_px, px, py, CELL_PX, 1, COL_GRID);
            /* Left edge */
            fill_rect(buf, stride_px, px, py, 1, CELL_PX, COL_GRID);
        }
    }

    /* 4. Draw food. */
    draw_cell(buf, stride_px, g_food.x, g_food.y, COL_FOOD);

    /* 5. Draw snake (body first, then head on top). */
    for (i32 j = 0; j < g_len - 1; j++) {
        cell_t c = snake_at(j);
        draw_cell(buf, stride_px, c.x, c.y, COL_SNAKE_BD);
    }
    /* Head (last cell = snake_at(g_len-1)). */
    {
        cell_t hd = snake_at(g_len - 1);
        draw_cell(buf, stride_px, hd.x, hd.y, COL_SNAKE_HD);
    }

    /* 6. Game over overlay. */
    if (g_game_over) {
        /* Dim the play area with a semi-transparent dark overlay via stipple:
         * draw every-other pixel dark for a tint effect. We do a simple full
         * dark overlay with alpha blending not available, so just draw a
         * translucent-looking rect by drawing every other row. */
        for (i32 y = HUD_H; y < WIN_H; y += 2) {
            fill_rect(buf, stride_px, 0, y, WIN_W, 1, 0xBB000000);
        }

        /* "GAME OVER" centered. */
        const char *over_msg  = "GAME OVER";
        const char *score_msg2 = "SCORE: ";
        const char *restart_msg = "PRESS R TO RESTART";
        i32 ow = font_text_width(over_msg);
        i32 rw = font_text_width(restart_msg);
        i32 ry = WIN_H / 2;

        font_draw_string(buf, (i32)stride_px, g_clip_w, g_clip_h,
                         (WIN_W - ow) / 2, ry - 20, over_msg, COL_RED);

        /* Final score line. */
        {
            char final_score[32];
            i32 n = 0;
            for (i32 i = 0; score_msg2[i]; i++) final_score[n++] = score_msg2[i];
            char num[12];
            i32 nlen = fmt_u32(num, g_score);
            for (i32 i = 0; i < nlen; i++) final_score[n++] = num[i];
            final_score[n] = '\0';
            i32 sw = font_text_width(final_score);
            font_draw_string(buf, (i32)stride_px, g_clip_w, g_clip_h,
                             (WIN_W - sw) / 2, ry, final_score, COL_YELLOW);
        }

        font_draw_string(buf, (i32)stride_px, g_clip_w, g_clip_h,
                         (WIN_W - rw) / 2, ry + 20, restart_msg, COL_WHITE);
    }

    /* 7. Pause overlay. */
    if (g_paused && !g_game_over) {
        for (i32 y = HUD_H; y < WIN_H; y += 2) {
            fill_rect(buf, stride_px, 0, y, WIN_W, 1, 0xBB000020);
        }
        const char *pause_msg  = "PAUSED";
        const char *resume_msg = "PRESS P TO RESUME";
        i32 pm_w = font_text_width(pause_msg);
        i32 rm_w = font_text_width(resume_msg);
        i32 mid_y = WIN_H / 2;
        font_draw_string(buf, (i32)stride_px, g_clip_w, g_clip_h,
                         (WIN_W - pm_w) / 2, mid_y - 10, pause_msg, COL_YELLOW);
        font_draw_string(buf, (i32)stride_px, g_clip_w, g_clip_h,
                         (WIN_W - rm_w) / 2, mid_y + 10, resume_msg, COL_WHITE);
    }
}

/* ---- entry point ---- */
void _start(void)
{
    print("[SNAKE] starting\n");

    if (wl_connect() != 0) {
        print("[SNAKE] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Snake");
    if (!win) {
        print("[SNAKE] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    u32 stride_px = win->stride / 4u;

    /* Letterbox clamp = min(fixed canvas, live window). Refreshed each frame. */
    g_clip_w = (i32)win->w < WIN_W ? (i32)win->w : WIN_W;
    g_clip_h = (i32)win->h < WIN_H ? (i32)win->h : WIN_H;

    /* Seed and init the game. */
    u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    game_init(now);

    u64 last_tick_ms = now;
    /* Base interval 120ms (~8 fps); shrinks 5ms per apple, min 40ms (~25 fps). */
    #define TICK_BASE   120u
    #define TICK_MIN     40u
    #define TICK_STEP     5u

    for (;;) {
        u64 t = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);

        /* Drain input events. */
        int kind, a, b, c_ev;
        while (wl_poll_event(win, &kind, &a, &b, &c_ev)) {
            if (kind == WL_EVENT_KEY && b == 1) {   /* b=pressed */
                switch (a) {
                /* Exit on ESC */
                case KEY_ESC:
                    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
                    break;
                /* Pause toggle on P (not in game-over state) */
                case KEY_P:
                    if (!g_game_over) g_paused = !g_paused;
                    break;
                /* Arrow keys / WASD (only when not paused) */
                case KEY_RIGHT: case KEY_D:
                    if (!g_paused) g_next_dir = 0; break;
                case KEY_DOWN:  case KEY_S:
                    if (!g_paused) g_next_dir = 1; break;
                case KEY_LEFT:  case KEY_A:
                    if (!g_paused) g_next_dir = 2; break;
                case KEY_UP:    case KEY_W:
                    if (!g_paused) g_next_dir = 3; break;
                /* Restart on R (game over) or SPACE/ENTER */
                case KEY_R:
                    if (g_game_over) {
                        now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
                        game_init(now);
                        last_tick_ms = now;
                    }
                    break;
                case KEY_SPACE: case KEY_ENTER:
                    if (g_game_over) {
                        now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
                        game_init(now);
                        last_tick_ms = now;
                    }
                    break;
                }
            } else if (kind == WL_EVENT_RESIZE) {
                /* The library has ALREADY reallocated the buffer and updated
                 * win->{w,h,stride,pixels}. We keep the fixed 480x480 canvas
                 * and letterbox it: just refresh our cached stride + clamp
                 * bounds so the next render clears the full new surface and
                 * never writes past the (possibly smaller) buffer. */
                stride_px = win->stride / 4u;
                g_clip_w = (i32)win->w < WIN_W ? (i32)win->w : WIN_W;
                g_clip_h = (i32)win->h < WIN_H ? (i32)win->h : WIN_H;
            }
        }

        /* Advance game state on interval (faster at higher scores). */
        u64 speed_reduction = (u64)(g_score * TICK_STEP);
        u64 tick_interval = TICK_BASE > (TICK_MIN + speed_reduction)
                            ? TICK_BASE - speed_reduction : TICK_MIN;
        if (!g_game_over && !g_paused && (t - last_tick_ms) >= tick_interval) {
            i32 was_alive = !g_game_over;
            game_tick();
            last_tick_ms = t;
            /* Print score to serial on death. */
            if (was_alive && g_game_over) {
                print("[SNAKE] score ");
                print_u32(g_score);
                print("\n");
            }
        }

        /* Render and commit. Re-read live geometry every frame so all pixel
         * writes are bounded to the CURRENT win->w/h using the CURRENT stride
         * (defends even if a resize lands without a polled event this frame). */
        stride_px = win->stride / 4u;
        g_clip_w = (i32)win->w < WIN_W ? (i32)win->w : WIN_W;
        g_clip_h = (i32)win->h < WIN_H ? (i32)win->h : WIN_H;
        render(win->pixels, stride_px, (i32)win->w, (i32)win->h);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
