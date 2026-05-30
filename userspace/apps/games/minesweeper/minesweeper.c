/*
 * minesweeper.c -- Polished Classic Minesweeper (freestanding, ring 3).
 * ======================================================================
 *
 * Beginner: 9x9 grid, 10 mines.
 * Left-click reveals (flood-fill on zero neighbors).
 * Right-click toggles flag.
 * First-click safe: mines placed after first reveal (safe cell + 8 neighbors).
 * Win when all non-mine cells are revealed.
 * R key resets. Smiley/reset button in header.
 * Timer (seconds) and mine counter shown in header via LED-style display.
 *
 * Window: CELL_SIZE=32, grid 9x9=288px wide, HEADER_H=56px, total 296x344.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/games/minesweeper/minesweeper.c -o /tmp/msw.o
 *   gcc <same flags> -c userspace/lib/wl/wl_client.c  -o /tmp/wlc.o
 *   gcc <same flags> -c userspace/lib/font/bitfont.c   -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/msw.o /tmp/wlc.o /tmp/bf.o -o /tmp/minesweeper.elf
 *   objdump -d /tmp/minesweeper.elf | grep fs:0x28   # MUST be empty
 *
 * Mouse events: wl_poll_event(win, &kind, &a, &b, &c)
 *   kind == WL_EVENT_POINTER: a=x, b=y, c=buttons (bit0=left, bit1=right)
 *   Edge detection: compare c with prev_buttons each poll.
 */

#include "../../../lib/wl/wl_client.h"
#include "../../../lib/font/bitfont.h"

/* =========================================================================
 * Syscalls (AOS ABI: SYS_EXIT=0, SYS_WRITE=3, SYS_YIELD=15,
 *           SYS_GET_TICKS_MS=40, SYS_RANDOM=43)
 * ========================================================================= */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_RANDOM       43

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

/* =========================================================================
 * Types
 * ========================================================================= */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef int            i32;
typedef long           i64;

/* =========================================================================
 * Serial helpers
 * ========================================================================= */
static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}
static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

/* Integer to decimal string. Returns buf pointer. */
static char *itoa(i32 v, char *buf)
{
    if (v < 0) { buf[0] = '-'; itoa(-v, buf + 1); return buf; }
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    char tmp[12]; int i = 0;
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/* Zero-padded integer to string of exactly `width` digits. */
static void itoa_pad(i32 v, char *buf, int width)
{
    if (v < 0) v = 0;
    char tmp[12]; int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; } }
    /* Right-justify with leading zeros */
    int j = 0;
    int pad = width - i;
    while (pad-- > 0) buf[j++] = '0';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* =========================================================================
 * Randomness: SYS_RANDOM=43, LCG fallback.
 * ========================================================================= */
static u32 lcg_state = 0xABCD1234u;

static u32 lcg_next(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

static void get_random_bytes(u8 *buf, int len)
{
    long r = sc(SYS_RANDOM, (long)buf, len, 0, 0, 0, 0);
    if (r < 0) {
        /* Fallback: seed from ticks */
        long t = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        if (t > 0) lcg_state = (u32)t ^ 0xDEADBEEFu;
        for (int i = 0; i < len; i++) {
            if ((i & 3) == 0) lcg_next();
            buf[i] = (u8)(lcg_state >> ((i & 3) * 8));
        }
    }
}

/* =========================================================================
 * Game constants
 * ========================================================================= */
#define COLS       9
#define ROWS       9
#define NUM_MINES  10

#define CELL_SIZE  32        /* pixels per cell */
#define HEADER_H   56        /* header bar height */
#define BORDER     4         /* outer border */

#define GRID_W     (COLS * CELL_SIZE)         /* 288 */
#define WIN_W      (GRID_W + BORDER * 2)      /* 296 */
#define WIN_H      (ROWS * CELL_SIZE + HEADER_H + BORDER * 2) /* 352 */

/* Grid pixel origin */
#define GRID_X     BORDER
#define GRID_Y     (HEADER_H + BORDER)

/* Cell state bit flags */
#define CS_MINE      0x01
#define CS_REVEALED  0x02
#define CS_FLAGGED   0x04
#define CS_QUESTION  0x08   /* optional: question mark (unused here) */

/* Game state */
#define GS_IDLE    0   /* before first click */
#define GS_PLAYING 1
#define GS_WIN     2
#define GS_LOSE    3

/* =========================================================================
 * Colour palette
 * ========================================================================= */
#define C_WIN_BG     0xFFBDBDBDu   /* window face / classic grey */
#define C_HEADER_BG  0xFFBDBDBDu
#define C_GRID_BG    0xFFBDBDBDu
#define C_BORDER_LT  0xFFFFFFFFu
#define C_BORDER_DK  0xFF7B7B7Bu
#define C_BORDER_MED 0xFF9E9E9Eu

/* Cell colours */
#define C_CELL_FACE  0xFFBDBDBDu   /* unrevealed face */
#define C_CELL_REV   0xFFC8C8C8u   /* revealed face (slightly lighter) */
#define C_MINE_HIT   0xFFFF3333u   /* the mine that killed you */
#define C_FLAG       0xFFCC2200u
#define C_CROSS      0xFFCC0000u   /* wrong-flag X in lose state */

/* LED display colours (mines/timer) */
#define C_LED_BG     0xFF1A0000u
#define C_LED_ON     0xFFFF3322u
#define C_LED_OFF    0xFF600000u

/* Classic number colours (1-8) */
static const u32 NUM_COLOR[9] = {
    0xFF000000u,   /* 0 unused */
    0xFF0000CCu,   /* 1 blue */
    0xFF007700u,   /* 2 green */
    0xFFCC0000u,   /* 3 red */
    0xFF000077u,   /* 4 dark blue */
    0xFF770000u,   /* 5 dark red */
    0xFF007777u,   /* 6 teal */
    0xFF000000u,   /* 7 black */
    0xFF777777u,   /* 8 grey */
};

/* =========================================================================
 * Drawing primitives into win->pixels
 * ========================================================================= */
static u32 *g_buf;
static u32  g_stride;  /* pixels per row */

static inline void put_pixel(i32 x, i32 y, u32 color)
{
    if ((u32)x < WIN_W && (u32)y < WIN_H)
        g_buf[(u32)y * g_stride + (u32)x] = color;
}

static void fill_rect(i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)WIN_W) x2 = (i32)WIN_W;
    i32 y2 = y + h; if (y2 > (i32)WIN_H) y2 = (i32)WIN_H;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = g_buf + (u32)yy * g_stride;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void hline(i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(x, y, len, 1, color);
}
static void vline(i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(x, y, 1, len, color);
}

/* 3-D raised box (classic Win95 style), depth=2 */
static void draw_raised(i32 x, i32 y, i32 w, i32 h, u32 face)
{
    fill_rect(x + 2, y + 2, w - 4, h - 4, face);
    /* Light: top, left */
    hline(x,   y,   w, C_BORDER_LT);
    hline(x+1, y+1, w-2, 0xFFDDDDDDu);
    vline(x,   y,   h, C_BORDER_LT);
    vline(x+1, y+1, h-2, 0xFFDDDDDDu);
    /* Dark: bottom, right */
    hline(x+1, y+h-2, w-2, C_BORDER_MED);
    hline(x,   y+h-1, w,   C_BORDER_DK);
    vline(x+w-2, y+1, h-2, C_BORDER_MED);
    vline(x+w-1, y,   h,   C_BORDER_DK);
}

/* 3-D sunken box (revealed cell), depth=1 */
static void draw_sunken(i32 x, i32 y, i32 w, i32 h, u32 face)
{
    fill_rect(x + 1, y + 1, w - 2, h - 2, face);
    /* Dark: top, left */
    hline(x, y, w, C_BORDER_DK);
    vline(x, y, h, C_BORDER_DK);
    /* Light: bottom, right */
    hline(x+1, y+h-1, w-1, C_BORDER_LT);
    vline(x+w-1, y+1, h-1, C_BORDER_LT);
}

/* Draw text via bitfont. Returns pixel advance. */
static int draw_text(i32 x, i32 y, const char *s, u32 color)
{
    return font_draw_string((unsigned int*)g_buf, (int)g_stride,
                             WIN_W, WIN_H, x, y, s, color);
}

/* Center text in a rectangle */
static void draw_text_center(i32 rx, i32 ry, i32 rw, i32 rh,
                             const char *s, u32 color)
{
    i32 tw = font_text_width(s);
    i32 tx = rx + (rw - tw) / 2;
    i32 ty = ry + (rh - FONT_H) / 2;
    draw_text(tx, ty, s, color);
}

/* =========================================================================
 * LED / 7-segment style number display (3 digits, right-justified).
 * Draws into a recessed panel of width LED_W, height LED_H.
 * ========================================================================= */
#define LED_DIGIT_W  13   /* pixels per digit */
#define LED_DIGIT_H  23
#define LED_PAD       2
#define LED_DIGITS    3
#define LED_W         (LED_DIGITS * LED_DIGIT_W + LED_PAD * 2)
#define LED_H         (LED_DIGIT_H + LED_PAD * 2)

/*
 * 7-segment encoding: bits [6:0] = segments a-g
 *   0: top, 1: top-right, 2: bottom-right, 3: bottom,
 *   4: bottom-left, 5: top-left, 6: middle
 */
static const u8 SEG7[11] = {
    0x3F, /* 0: abcdef    */
    0x06, /* 1:  bc       */
    0x5B, /* 2: ab de g   */
    0x4F, /* 3: abcd g    */
    0x66, /* 4:  bc fg    */
    0x6D, /* 5: acd fg    */
    0x7D, /* 6: acdefg    */
    0x07, /* 7: abc       */
    0x7F, /* 8: all       */
    0x6F, /* 9: abcdfg    */
    0x40, /* - (dash)     */
};

static void draw_led_digit(i32 dx, i32 dy, int digit)
{
    /* digit=-1 draws blank, digit=10 draws dash '-' */
    u8 segs = (digit < 0) ? 0 : (digit > 10 ? 0 : SEG7[digit]);
    /* Background */
    fill_rect(dx, dy, LED_DIGIT_W, LED_DIGIT_H, C_LED_BG);

    i32 sw  = LED_DIGIT_W - 4;  /* segment width (horizontal) */
    i32 sh  = LED_DIGIT_H / 2 - 2;  /* segment height (vertical half) */
    i32 st  = 2;   /* thickness */
    i32 midY = dy + LED_DIGIT_H / 2;

#define SEG_ON(bit)  ((segs >> (bit)) & 1)
#define SEG_C(b)     (SEG_ON(b) ? C_LED_ON : C_LED_OFF)

    /* a = top horizontal */
    fill_rect(dx+2, dy+1, sw, st, SEG_C(0));
    /* b = top-right vertical */
    fill_rect(dx+LED_DIGIT_W-st-1, dy+2, st, sh-1, SEG_C(1));
    /* c = bottom-right vertical */
    fill_rect(dx+LED_DIGIT_W-st-1, midY+1, st, sh-1, SEG_C(2));
    /* d = bottom horizontal */
    fill_rect(dx+2, dy+LED_DIGIT_H-st-1, sw, st, SEG_C(3));
    /* e = bottom-left vertical */
    fill_rect(dx+1, midY+1, st, sh-1, SEG_C(4));
    /* f = top-left vertical */
    fill_rect(dx+1, dy+2, st, sh-1, SEG_C(5));
    /* g = middle horizontal */
    fill_rect(dx+2, midY-1, sw, st, SEG_C(6));

#undef SEG_ON
#undef SEG_C
}

static void draw_led_number(i32 px, i32 py, int value)
{
    /* Sunken panel */
    draw_sunken(px - LED_PAD, py - LED_PAD,
                LED_W + 2, LED_H + 2, C_LED_BG);

    int neg = (value < 0);
    if (value < 0) value = -value;
    if (value > 999) value = 999;

    int d0 = value / 100;
    int d1 = (value / 10) % 10;
    int d2 = value % 10;

    /* Suppress leading zeros except last digit */
    int show0 = (d0 != 0 || neg);
    int show1 = (show0 || d1 != 0);

    draw_led_digit(px + 0 * LED_DIGIT_W, py, neg ? 10 : (show0 ? d0 : -1));
    draw_led_digit(px + 1 * LED_DIGIT_W, py, show1 ? d1 : -1);
    draw_led_digit(px + 2 * LED_DIGIT_W, py, d2);
}

/* =========================================================================
 * Smiley button
 * ========================================================================= */
#define SMILEY_W   38
#define SMILEY_H   38
#define SMILEY_X   ((WIN_W - SMILEY_W) / 2)
#define SMILEY_Y   ((HEADER_H - SMILEY_H) / 2)

static void draw_circle_aa(i32 cx, i32 cy, i32 r, u32 color)
{
    /* Filled circle */
    for (i32 dy = -r; dy <= r; dy++)
        for (i32 dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                put_pixel(cx+dx, cy+dy, color);
}

static void draw_smiley(int state)
{
    i32 cx = SMILEY_X + SMILEY_W / 2;
    i32 cy = SMILEY_Y + SMILEY_H / 2;
    i32 fr = SMILEY_W / 2 - 3;  /* face radius */

    /* Button face */
    draw_raised(SMILEY_X, SMILEY_Y, SMILEY_W, SMILEY_H, C_WIN_BG);

    /* Yellow face circle */
    draw_circle_aa(cx, cy, fr, 0xFFFFCC00u);
    /* Black outline ring */
    for (i32 dy = -fr; dy <= fr; dy++)
        for (i32 dx = -fr; dx <= fr; dx++) {
            i32 d2 = dx*dx + dy*dy;
            if (d2 <= fr*fr && d2 > (fr-2)*(fr-2))
                put_pixel(cx+dx, cy+dy, 0xFF000000u);
        }

    if (state == GS_LOSE) {
        /* Dead eyes: X marks */
        for (i32 i = -2; i <= 2; i++) {
            put_pixel(cx - 5 + i, cy - 4 + i, 0xFF000000u);
            put_pixel(cx - 5 + i, cy - 4 - i, 0xFF000000u);
            put_pixel(cx + 5 + i, cy - 4 + i, 0xFF000000u);
            put_pixel(cx + 5 + i, cy - 4 - i, 0xFF000000u);
        }
        /* Frowning mouth */
        for (i32 i = -4; i <= 4; i++) {
            i32 curve = (i * i) / 5;
            put_pixel(cx + i, cy + 5 - curve, 0xFF000000u);
        }
    } else if (state == GS_WIN) {
        /* Sunglasses eyes */
        fill_rect(cx - 7, cy - 6, 5, 3, 0xFF000000u);
        fill_rect(cx + 2, cy - 6, 5, 3, 0xFF000000u);
        /* Big smile */
        for (i32 i = -5; i <= 5; i++) {
            i32 curve = -(i * i) / 5;
            put_pixel(cx + i, cy + 4 + curve, 0xFF000000u);
            put_pixel(cx + i, cy + 5 + curve, 0xFF000000u);
        }
    } else {
        /* Normal eyes: filled dots */
        draw_circle_aa(cx - 5, cy - 4, 2, 0xFF000000u);
        draw_circle_aa(cx + 5, cy - 4, 2, 0xFF000000u);
        /* Smile arc */
        for (i32 i = -5; i <= 5; i++) {
            i32 curve = -(i * i) / 6;
            put_pixel(cx + i, cy + 5 + curve, 0xFF000000u);
        }
    }
}

/* =========================================================================
 * Mine symbol (circle + 8 spikes)
 * ========================================================================= */
static void draw_mine(i32 cx, i32 cy, u32 core_color)
{
    i32 r = CELL_SIZE / 2 - 7;
    /* Core circle */
    for (i32 dy = -r; dy <= r; dy++)
        for (i32 dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                put_pixel(cx+dx, cy+dy, core_color);
    /* 8 spikes */
    i32 sp = r + 4;
    for (i32 i = 1; i <= sp; i++) {
        put_pixel(cx, cy-i, core_color);
        put_pixel(cx, cy+i, core_color);
        put_pixel(cx-i, cy, core_color);
        put_pixel(cx+i, cy, core_color);
    }
    for (i32 i = 1; i <= r-1; i++) {
        put_pixel(cx+i, cy-i, core_color);
        put_pixel(cx-i, cy-i, core_color);
        put_pixel(cx+i, cy+i, core_color);
        put_pixel(cx-i, cy+i, core_color);
    }
    /* Highlight */
    put_pixel(cx - r/2, cy - r/2, 0xFFFFFFFFu);
}

/* =========================================================================
 * Flag symbol (pole + triangle flag)
 * ========================================================================= */
static void draw_flag(i32 cx, i32 cy)
{
    /* Pole */
    i32 pole_top = cy - CELL_SIZE/2 + 5;
    i32 pole_bot = cy + CELL_SIZE/2 - 7;
    for (i32 py = pole_top; py <= pole_bot; py++)
        put_pixel(cx, py, 0xFF000000u);
    /* Flag (red triangle to the right of pole top) */
    for (i32 row = 0; row < 9; row++) {
        i32 width = 9 - row;
        i32 py = pole_top + row;
        for (i32 dx = 1; dx < width; dx++)
            put_pixel(cx + dx, py, C_FLAG);
    }
    /* Base */
    for (i32 dx = -4; dx <= 4; dx++)
        put_pixel(cx + dx, pole_bot, 0xFF000000u);
}

/* Draw an X (wrong-flag marker after game over) */
static void draw_cross(i32 cx, i32 cy)
{
    for (i32 i = -5; i <= 5; i++) {
        put_pixel(cx + i, cy + i, C_CROSS);
        put_pixel(cx + i, cy + i + 1, C_CROSS);
        put_pixel(cx + i, cy - i, C_CROSS);
        put_pixel(cx + i, cy - i + 1, C_CROSS);
    }
}

/* =========================================================================
 * Cell rendering
 * ========================================================================= */
static void render_cell(int row, int col, int show_all)
{
    i32 x  = GRID_X + col * CELL_SIZE;
    i32 y  = GRID_Y + row * CELL_SIZE;
    i32 cx = x + CELL_SIZE / 2;
    i32 cy = y + CELL_SIZE / 2;

    extern u8  g_state[ROWS * COLS];
    extern u8  g_num  [ROWS * COLS];
    u8 st  = g_state[row * COLS + col];
    u8 num = g_num  [row * COLS + col];

    if (st & CS_REVEALED) {
        if (st & CS_MINE) {
            /* The mine that was hit */
            draw_sunken(x, y, CELL_SIZE, CELL_SIZE, C_MINE_HIT);
            draw_mine(cx, cy, 0xFF000000u);
        } else {
            draw_sunken(x, y, CELL_SIZE, CELL_SIZE, C_CELL_REV);
            if (num > 0) {
                char nb[3]; nb[0] = (char)('0' + num); nb[1] = '\0';
                i32 tx = cx - FONT_W / 2;
                i32 ty = cy - FONT_H / 2;
                font_draw_string((unsigned int*)g_buf, (int)g_stride,
                                 WIN_W, WIN_H, tx, ty, nb, NUM_COLOR[num]);
            }
        }
    } else if (st & CS_FLAGGED) {
        if (show_all && !(st & CS_MINE)) {
            /* Wrong flag: show mine + X */
            draw_sunken(x, y, CELL_SIZE, CELL_SIZE, C_CELL_REV);
            draw_mine(cx, cy, 0xFF000000u);
            draw_cross(cx, cy);
        } else {
            draw_raised(x, y, CELL_SIZE, CELL_SIZE, C_CELL_FACE);
            draw_flag(cx, cy);
        }
    } else if (show_all && (st & CS_MINE)) {
        /* Unflagged mine exposed at game over */
        draw_sunken(x, y, CELL_SIZE, CELL_SIZE, C_CELL_REV);
        draw_mine(cx, cy, 0xFF000000u);
    } else {
        draw_raised(x, y, CELL_SIZE, CELL_SIZE, C_CELL_FACE);
    }
}

/* =========================================================================
 * Header bar
 * ========================================================================= */
static void render_header(int gs, int mine_count, int elapsed_sec)
{
    /* Background */
    fill_rect(0, 0, WIN_W, HEADER_H + BORDER, C_WIN_BG);
    /* Sunken header panel */
    draw_sunken(BORDER, BORDER, WIN_W - BORDER*2, HEADER_H, C_WIN_BG);

    /* Left LED: remaining mines */
    i32 led_y = BORDER + (HEADER_H - LED_H) / 2;
    draw_led_number(BORDER + 6, led_y, mine_count);

    /* Right LED: elapsed seconds */
    i32 timer_x = WIN_W - BORDER - 6 - LED_W;
    draw_led_number(timer_x, led_y, elapsed_sec);

    /* Smiley button */
    draw_smiley(gs);
}

/* =========================================================================
 * Full scene render
 * ========================================================================= */
static void render_all(int gs, int mine_remaining, int elapsed_sec)
{
    int show_all = (gs == GS_LOSE);

    /* Outer border */
    fill_rect(0, 0, WIN_W, WIN_H, C_WIN_BG);
    /* Raised outer frame */
    hline(0, 0, WIN_W, C_BORDER_LT);
    vline(0, 0, WIN_H, C_BORDER_LT);
    hline(0, WIN_H-1, WIN_W, C_BORDER_DK);
    vline(WIN_W-1, 0, WIN_H, C_BORDER_DK);

    render_header(gs, mine_remaining, elapsed_sec);

    /* Grid sunken panel */
    draw_sunken(BORDER, HEADER_H + BORDER,
                WIN_W - BORDER*2, WIN_H - HEADER_H - BORDER*2,
                C_WIN_BG);

    /* Cells */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            render_cell(r, c, show_all);
}

/* =========================================================================
 * Board state (global; freestanding -- no dynamic allocation)
 * ========================================================================= */
u8 g_state[ROWS * COLS];   /* CS_MINE | CS_REVEALED | CS_FLAGGED */
u8 g_num  [ROWS * COLS];   /* neighbor mine count */

static int g_game_state    = GS_IDLE;
static int g_flags_placed  = 0;
static int g_revealed_cnt  = 0;
static u64 g_start_ticks   = 0;    /* ticks_ms when game started (GS_PLAYING) */
static int g_elapsed_sec   = 0;
static int g_prev_buttons  = 0;

/* =========================================================================
 * Board logic
 * ========================================================================= */
static void board_reset(void)
{
    for (int i = 0; i < ROWS * COLS; i++) {
        g_state[i] = 0;
        g_num[i]   = 0;
    }
    g_game_state   = GS_IDLE;
    g_flags_placed = 0;
    g_revealed_cnt = 0;
    g_start_ticks  = 0;
    g_elapsed_sec  = 0;
    g_prev_buttons = 0;
}

static void compute_numbers(void)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (g_state[r*COLS+c] & CS_MINE) continue;
            int cnt = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r+dr, nc = c+dc;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                    if (g_state[nr*COLS+nc] & CS_MINE) cnt++;
                }
            g_num[r*COLS+c] = (u8)cnt;
        }
    }
}

/* Place mines avoiding safe_r, safe_c and its 8 neighbors (Fisher-Yates). */
static void place_mines(int safe_r, int safe_c)
{
    /* Build candidate list (excludes safe cell + neighbors) */
    int cands[ROWS * COLS];
    int ncand = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int dr = r - safe_r, dc = c - safe_c;
            if (dr >= -1 && dr <= 1 && dc >= -1 && dc <= 1) continue;
            cands[ncand++] = r * COLS + c;
        }

    /* Shuffle first NUM_MINES into front */
    u8 rbuf[ROWS * COLS * 2];
    get_random_bytes(rbuf, (int)sizeof(rbuf));
    int placed = 0;
    for (int i = 0; i < ncand && placed < NUM_MINES; i++) {
        u8 rv = rbuf[i % (int)sizeof(rbuf)];
        int j = i + (int)(rv % (unsigned)(ncand - i));
        if (j >= ncand) j = ncand - 1;
        int tmp = cands[i]; cands[i] = cands[j]; cands[j] = tmp;
        g_state[cands[i]] |= CS_MINE;
        placed++;
    }
    compute_numbers();
}

/* BFS flood reveal. */
#define BFS_MAX (ROWS * COLS)
static int bfs_q[BFS_MAX];

static void flood_reveal(int sr, int sc_)
{
    int head = 0, tail = 0;
    int start = sr * COLS + sc_;
    bfs_q[tail++] = start;

    while (head < tail) {
        int idx = bfs_q[head++];
        if (g_state[idx] & CS_REVEALED) continue;
        if (g_state[idx] & CS_FLAGGED)  continue;
        if (g_state[idx] & CS_MINE)     continue;

        g_state[idx] |= CS_REVEALED;
        g_revealed_cnt++;

        if (g_num[idx] != 0) continue;  /* only expand from zeros */

        int r = idx / COLS, c = idx % COLS;
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r+dr, nc = c+dc;
                if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                int ni = nr * COLS + nc;
                if (!(g_state[ni] & CS_REVEALED) &&
                    !(g_state[ni] & CS_FLAGGED) &&
                    !(g_state[ni] & CS_MINE))
                    if (tail < BFS_MAX) bfs_q[tail++] = ni;
            }
    }
}

static void check_win(void)
{
    if (g_revealed_cnt >= ROWS * COLS - NUM_MINES) {
        g_game_state = GS_WIN;
        print("[MINESWEEPER] win\n");
    }
}

static void cell_left_click(int row, int col)
{
    if (g_game_state == GS_WIN || g_game_state == GS_LOSE) return;
    int idx = row * COLS + col;
    if (g_state[idx] & CS_REVEALED) return;
    if (g_state[idx] & CS_FLAGGED)  return;

    if (g_game_state == GS_IDLE) {
        place_mines(row, col);
        g_game_state  = GS_PLAYING;
        g_start_ticks = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        g_elapsed_sec = 0;
    }

    if (g_state[idx] & CS_MINE) {
        g_state[idx] |= CS_REVEALED;
        g_game_state = GS_LOSE;
        print("[MINESWEEPER] boom\n");
        return;
    }

    flood_reveal(row, col);
    check_win();
}

static void cell_right_click(int row, int col)
{
    if (g_game_state == GS_WIN || g_game_state == GS_LOSE) return;
    int idx = row * COLS + col;
    if (g_state[idx] & CS_REVEALED) return;

    if (g_state[idx] & CS_FLAGGED) {
        g_state[idx] &= (u8)~CS_FLAGGED;
        g_flags_placed--;
    } else {
        g_state[idx] |= CS_FLAGGED;
        g_flags_placed++;
    }
}

/* =========================================================================
 * Hit testing
 * ========================================================================= */
static int hit_smiley(i32 mx, i32 my)
{
    return (mx >= SMILEY_X && mx < SMILEY_X + SMILEY_W &&
            my >= SMILEY_Y && my < SMILEY_Y + SMILEY_H);
}

static int px_to_cell(i32 mx, i32 my, int *out_row, int *out_col)
{
    i32 gx = mx - GRID_X;
    i32 gy = my - GRID_Y;
    if (gx < 0 || gy < 0) return 0;
    i32 col = gx / CELL_SIZE;
    i32 row = gy / CELL_SIZE;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return 0;
    *out_row = (int)row;
    *out_col = (int)col;
    return 1;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
void _start(void)
{
    print("[MINESWEEPER] starting\n");

    if (wl_connect() != 0) {
        print("[MINESWEEPER] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Minesweeper");
    if (!win) {
        print("[MINESWEEPER] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    g_buf    = win->pixels;
    g_stride = win->stride / 4u;  /* stride is bytes; we need pixels */

    board_reset();
    render_all(g_game_state, NUM_MINES - g_flags_placed, 0);
    wl_commit(win);

    /* Main loop */
    for (;;) {
        int kind, ea, eb, ec;
        int dirty = 0;

        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            /* ---- Pointer events ---- */
            if (kind == WL_EVENT_POINTER) {
                i32 mx      = (i32)ea;
                i32 my      = (i32)eb;
                i32 buttons = (i32)ec;

                int left_now  = (buttons & 1);
                int left_prev = (g_prev_buttons & 1);
                int right_now  = (buttons & 2);
                int right_prev = (g_prev_buttons & 2);

                /* Left rising-edge */
                if (left_now && !left_prev) {
                    if (hit_smiley(mx, my)) {
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
                /* Right rising-edge */
                if (right_now && !right_prev) {
                    int row, col;
                    if (px_to_cell(mx, my, &row, &col)) {
                        cell_right_click(row, col);
                        dirty = 1;
                    }
                }

                g_prev_buttons = buttons;
            }

            /* ---- Key events ---- */
            if (kind == WL_EVENT_KEY) {
                int keycode = ea;
                int pressed = eb;
                /* R key (keycode 19) = reset */
                if (pressed && keycode == 19) {
                    board_reset();
                    dirty = 1;
                }
                /* ESC key (keycode 1) = exit */
                if (pressed && keycode == 1) {
                    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
                }
            }
        }

        /* Update timer (only while playing) */
        if (g_game_state == GS_PLAYING) {
            u64 now = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            int secs = (int)((now - g_start_ticks) / 1000u);
            if (secs > 999) secs = 999;
            if (secs != g_elapsed_sec) {
                g_elapsed_sec = secs;
                dirty = 1;
            }
        }

        if (dirty) {
            render_all(g_game_state,
                       NUM_MINES - g_flags_placed,
                       g_elapsed_sec);
            wl_commit(win);
        }

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
