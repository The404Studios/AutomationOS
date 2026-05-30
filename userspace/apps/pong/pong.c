/*
 * pong.c -- Classic Pong game (freestanding, ring 3).
 * ====================================================
 *
 * 640x440 window.  Two paddles, one ball, first to 11 wins.
 *
 * Controls:
 *   Left paddle  -- W (up) / S (down) or mouse Y tracking
 *   Right paddle -- Up / Down arrows, or simple AI (speed-capped)
 *   Space        -- serve ball (from centre, towards last scorer)
 *   Esc          -- pause / resume
 *
 * Ball:
 *   - Starts at centre, served with slight random angle variation.
 *   - Speed increases slightly each rally (volley count based).
 *   - Paddle "english": hitting near top/bottom of paddle adds vertical spin.
 *
 * Scoring:
 *   - First to 11 points wins.
 *   - Winner screen; press Space to reset.
 *
 * SFX: g_beep for paddle hits, wall bounces, and scoring.
 *
 * Serial:
 *   [PONG] starting
 *   [PONG] game over left=N right=N   (on win)
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/pong/pong.c -o /tmp/pong.o
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
 *       /tmp/pong.o /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/pong.elf
 *   objdump -d /tmp/pong.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/game/game.h"

/* =========================================================================
 * Serial output helpers (SYS_WRITE, no libc).
 * ========================================================================= */
static inline long _pong_sc3(long n, long a, long b, long c)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return r;
}

static void serial_write(const char *s)
{
    u64 len = 0;
    while (s[len]) len++;
    _pong_sc3(SYS_WRITE, 1, (long)s, (long)len);
}

static void serial_int(int v)
{
    char buf[14];
    g_itoa(v, buf);
    serial_write(buf);
}

/* =========================================================================
 * Layout constants.
 * ========================================================================= */
#define WIN_W        640
#define WIN_H        440

#define PAD_W        10
#define PAD_H        70
#define PAD_MARGIN   14          /* distance from edge */
#define PAD_SPEED    5           /* pixels per frame (keyboard) */
#define PAD_AI_SPEED 4           /* AI max pixels per frame      */

#define BALL_SIZE    10          /* square ball side              */
#define BALL_INIT_VX 4           /* initial horizontal speed (px/frame) */
#define BALL_INIT_VY 3           /* initial vertical speed               */
#define BALL_MAX_VX  12
#define BALL_MAX_VY  10

#define SCORE_WIN    11

/* Fixed-point scale: velocities stored * 256 for sub-pixel precision. */
#define FX           256

/* =========================================================================
 * Colours.
 * ========================================================================= */
#define COL_BG       0xFF0A0A0A
#define COL_NET      0xFF444444
#define COL_PADDLE   0xFFE8E8E8
#define COL_BALL     0xFFF0F040
#define COL_SCORE    0xFFFFFFFF
#define COL_SCORE_DIM 0xFF888888
#define COL_WIN_BG   0xFF1A1A3A
#define COL_WIN_TEXT 0xFFFFD700
#define COL_SHADOW   0xFF333333
#define COL_RED      0xFFFF4444
#define COL_CYAN     0xFF44CCFF
#define COL_PAUSED   0xFF888844

/* =========================================================================
 * Game state.
 * ========================================================================= */
typedef enum {
    STATE_IDLE,      /* waiting for Space to serve */
    STATE_PLAY,
    STATE_SCORE,     /* brief pause after a point  */
    STATE_WIN,
    STATE_PAUSED
} pong_state_t;

/* Paddles: left=0, right=1 */
static int   pad_y[2];          /* top-left y of paddle              */
static int   score[2];          /* scores                            */

/* Ball (fixed-point * FX) */
static int   bx, by;            /* position (integer pixels * FX)    */
static int   bvx, bvy;          /* velocity (pixels * FX per frame)  */
static int   serve_dir;         /* -1 = left, +1 = right             */

static pong_state_t g_state;
static int   score_timer;       /* frames to wait before next serve  */
static int   prev_pause;        /* for edge detection on Esc         */
static pong_state_t pre_pause;  /* state before pausing              */

/* =========================================================================
 * Fixed-point abs / sign helpers.
 * ========================================================================= */
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int isign(int v) { return v < 0 ? -1 : 1; }

/* =========================================================================
 * Ball reset: place at centre, velocity towards serve_dir.
 * ========================================================================= */
static void ball_reset(game_t *gx)
{
    bx = (WIN_W / 2 - BALL_SIZE / 2) * FX;
    by = (WIN_H / 2 - BALL_SIZE / 2) * FX;

    /* Base speed with slight random Y variation. */
    int vx = BALL_INIT_VX * FX * serve_dir;
    /* Random y between -BALL_INIT_VY and +BALL_INIT_VY (non-zero). */
    int vy_mag = BALL_INIT_VY * FX + g_rand_range(FX);
    int vy_sign = g_rand_range(2) ? 1 : -1;
    int vy = vy_mag * vy_sign;

    bvx = vx;
    bvy = vy;
    (void)gx;
}

/* =========================================================================
 * Game initialisation.
 * ========================================================================= */
static void game_reset(game_t *gx)
{
    score[0] = 0;
    score[1] = 0;
    pad_y[0] = WIN_H / 2 - PAD_H / 2;
    pad_y[1] = WIN_H / 2 - PAD_H / 2;
    serve_dir = 1;
    g_state = STATE_IDLE;
    prev_pause = 0;
    ball_reset(gx);
}

/* =========================================================================
 * Draw a large digit (3x scale bitmap font via stacked g_fill_rect).
 * We draw digits using a simple 5x7 segmented approach scaled up.
 * ========================================================================= */

/* 5-wide x 7-tall segment map (each row is 5 bits, MSB=left). */
static const u8 digit_bmp[10][7] = {
    /* 0 */ {0x1C, 0x14, 0x14, 0x14, 0x14, 0x14, 0x1C},
    /* 1 */ {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* 2 */ {0x1C, 0x04, 0x04, 0x1C, 0x10, 0x10, 0x1C},
    /* 3 */ {0x1C, 0x04, 0x04, 0x1C, 0x04, 0x04, 0x1C},
    /* 4 */ {0x14, 0x14, 0x14, 0x1C, 0x04, 0x04, 0x04},
    /* 5 */ {0x1C, 0x10, 0x10, 0x1C, 0x04, 0x04, 0x1C},
    /* 6 */ {0x1C, 0x10, 0x10, 0x1C, 0x14, 0x14, 0x1C},
    /* 7 */ {0x1C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* 8 */ {0x1C, 0x14, 0x14, 0x1C, 0x14, 0x14, 0x1C},
    /* 9 */ {0x1C, 0x14, 0x14, 0x1C, 0x04, 0x04, 0x1C},
};

static void draw_big_digit(game_t *gx, int x, int y, int d, u32 col)
{
    int scale = 6;  /* each bitmap pixel = 6x6 screen pixels */
    if (d < 0 || d > 9) return;
    const u8 *rows = digit_bmp[d];
    for (int row = 0; row < 7; row++) {
        for (int col2 = 0; col2 < 5; col2++) {
            /* bit: MSB is col 0 */
            if (rows[row] & (0x10 >> col2)) {
                g_fill_rect(gx,
                            x + col2 * scale,
                            y + row  * scale,
                            scale - 1, scale - 1, col);
            }
        }
    }
}

/* Draw two-digit score (or one digit if < 10). Centred at cx. */
static void draw_score_big(game_t *gx, int cx, int y, int s, u32 col)
{
    int dig_w  = 5 * 6;  /* width of one digit in pixels */
    int gap    = 4;
    int total  = (s >= 10) ? (dig_w * 2 + gap) : dig_w;
    int x      = cx - total / 2;

    if (s >= 10) {
        draw_big_digit(gx, x,            y, s / 10, col);
        draw_big_digit(gx, x + dig_w + gap, y, s % 10, col);
    } else {
        draw_big_digit(gx, x, y, s, col);
    }
}

/* =========================================================================
 * Centre dashed net.
 * ========================================================================= */
static void draw_net(game_t *gx)
{
    int cx = WIN_W / 2 - 1;
    int dash_h = 12;
    int gap    = 8;
    for (int y = 0; y < WIN_H; y += dash_h + gap) {
        int h = (y + dash_h < WIN_H) ? dash_h : (WIN_H - y);
        g_fill_rect(gx, cx, y, 2, h, COL_NET);
    }
}

/* =========================================================================
 * Paddle drawing: rounded bar with highlight.
 * ========================================================================= */
static void draw_paddle(game_t *gx, int side, u32 col)
{
    int x = (side == 0) ? PAD_MARGIN : (WIN_W - PAD_MARGIN - PAD_W);
    int y = pad_y[side];
    /* Shadow */
    g_fill_rect(gx, x + 2, y + 2, PAD_W, PAD_H, COL_SHADOW);
    /* Main body */
    g_rounded_rect(gx, x, y, PAD_W, PAD_H, 4, col);
    /* Highlight strip */
    g_fill_rect(gx, x + 2, y + 3, 2, PAD_H - 6, 0xFFFFFFFF);
}

/* =========================================================================
 * Ball drawing: bright square with glow outline.
 * ========================================================================= */
static void draw_ball(game_t *gx)
{
    int x = bx / FX;
    int y = by / FX;
    /* Glow */
    g_fill_rect(gx, x - 2, y - 2, BALL_SIZE + 4, BALL_SIZE + 4, 0xFF504020);
    /* Core */
    g_fill_rect(gx, x, y, BALL_SIZE, BALL_SIZE, COL_BALL);
    /* Specular */
    g_fill_rect(gx, x + 1, y + 1, 3, 2, 0xFFFFFFCC);
}

/* =========================================================================
 * Render the full frame.
 * ========================================================================= */
static void render(game_t *gx)
{
    g_clear(gx, COL_BG);

    /* Net */
    draw_net(gx);

    /* Top/bottom borders */
    g_fill_rect(gx, 0, 0, WIN_W, 3, 0xFF303030);
    g_fill_rect(gx, 0, WIN_H - 3, WIN_W, 3, 0xFF303030);

    /* Scores */
    u32 sc0_col = (score[0] >= SCORE_WIN - 1) ? COL_CYAN  : COL_SCORE;
    u32 sc1_col = (score[1] >= SCORE_WIN - 1) ? COL_WIN_TEXT : COL_SCORE;
    draw_score_big(gx, WIN_W / 4,     16, score[0], sc0_col);
    draw_score_big(gx, 3 * WIN_W / 4, 16, score[1], sc1_col);

    /* Paddles */
    draw_paddle(gx, 0, COL_PADDLE);
    draw_paddle(gx, 1, COL_PADDLE);

    /* Ball (only during play) */
    if (g_state == STATE_PLAY || g_state == STATE_PAUSED)
        draw_ball(gx);

    /* State overlays */
    if (g_state == STATE_IDLE) {
        g_text_center(gx, WIN_W / 2, WIN_H - 28, "SPACE to serve", COL_SCORE_DIM);
        g_text_center(gx, WIN_W / 2, WIN_H / 2 + 48, "W/S or mouse = left  UP/DOWN = right", COL_SCORE_DIM);
    }

    if (g_state == STATE_PAUSED) {
        g_fill_rect(gx, WIN_W/2 - 80, WIN_H/2 - 18, 160, 36, 0xCC111122);
        g_text_center(gx, WIN_W / 2, WIN_H / 2, "PAUSED  (ESC to resume)", COL_PAUSED);
    }

    if (g_state == STATE_WIN) {
        /* Overlay */
        g_fill_rect(gx, 0, WIN_H/2 - 50, WIN_W, 110, COL_WIN_BG);
        const char *winner = (score[0] >= SCORE_WIN) ? "LEFT PLAYER WINS!" : "RIGHT PLAYER WINS!";
        g_text_center(gx, WIN_W / 2, WIN_H / 2 - 20, winner, COL_WIN_TEXT);
        g_text_center(gx, WIN_W / 2, WIN_H / 2 + 8,  "SPACE to play again", COL_SCORE_DIM);
    }
}

/* =========================================================================
 * Update paddle: left = W/S or mouse, right = AI or arrows.
 * ========================================================================= */
static void update_paddles(game_t *gx)
{
    /* -- Left paddle: keyboard W/S or mouse -- */
    int mx, my, mb;
    game_mouse(gx, &mx, &my, &mb);

    /* Mouse tracking: map mouse Y to paddle centre. */
    int target_y = my - PAD_H / 2;
    int diff = target_y - pad_y[0];
    /* Only track if mouse moved significantly (avoid jitter). */
    if (diff > PAD_SPEED)       pad_y[0] += PAD_SPEED;
    else if (diff < -PAD_SPEED) pad_y[0] -= PAD_SPEED;

    /* Keyboard overrides (additive preference). */
    if (game_key_down(gx, KEY_W)) pad_y[0] -= PAD_SPEED;
    if (game_key_down(gx, KEY_S)) pad_y[0] += PAD_SPEED;

    /* -- Right paddle: arrows or AI -- */
    int ai_moved = 0;
    if (game_key_down(gx, KEY_UP))   { pad_y[1] -= PAD_SPEED; ai_moved = 1; }
    if (game_key_down(gx, KEY_DOWN)) { pad_y[1] += PAD_SPEED; ai_moved = 1; }

    /* AI: track ball with a speed cap if arrows not pressed */
    if (!ai_moved && g_state == STATE_PLAY) {
        int ball_cy = by / FX + BALL_SIZE / 2;
        int pad_cy  = pad_y[1] + PAD_H / 2;
        int d = ball_cy - pad_cy;
        if (d > PAD_AI_SPEED)       pad_y[1] += PAD_AI_SPEED;
        else if (d < -PAD_AI_SPEED) pad_y[1] -= PAD_AI_SPEED;
        else                         pad_y[1] += d;
    }

    /* Clamp both paddles to playfield. */
    int ymin = 3;
    int ymax = WIN_H - 3 - PAD_H;
    pad_y[0] = g_clamp(pad_y[0], ymin, ymax);
    pad_y[1] = g_clamp(pad_y[1], ymin, ymax);
}

/* =========================================================================
 * Ball physics update: move, bounce off walls and paddles.
 * ========================================================================= */
static void update_ball(game_t *gx)
{
    bx += bvx;
    by += bvy;

    int px = bx / FX;
    int py = by / FX;

    /* --- Top / bottom wall bounce --- */
    if (py <= 3) {
        by = 3 * FX;
        bvy = iabs(bvy);
        g_beep(440, 30);
    }
    if (py + BALL_SIZE >= WIN_H - 3) {
        by = (WIN_H - 3 - BALL_SIZE) * FX;
        bvy = -iabs(bvy);
        g_beep(440, 30);
    }

    /* Refresh pixel coords after wall bounce. */
    px = bx / FX;
    py = by / FX;

    /* --- Left paddle collision --- */
    int lpad_x = PAD_MARGIN;
    int lpad_y = pad_y[0];
    if (bvx < 0 &&
        g_aabb(px, py, BALL_SIZE, BALL_SIZE,
               lpad_x, lpad_y, PAD_W, PAD_H)) {

        /* Reflect horizontal. */
        bx  = (lpad_x + PAD_W) * FX;
        bvx = iabs(bvx);

        /* English: relative hit position on paddle [-1.0, +1.0] * FX. */
        int rel = (py + BALL_SIZE / 2) - (lpad_y + PAD_H / 2);
        /* Scale: max influence = ±3 * FX. */
        int spin = (rel * 3 * FX) / (PAD_H / 2);
        bvy = bvy + spin;

        /* Cap velocities. */
        if (bvx > BALL_MAX_VX * FX) bvx = BALL_MAX_VX * FX;
        if (iabs(bvy) > BALL_MAX_VY * FX) bvy = isign(bvy) * BALL_MAX_VY * FX;

        /* Small speedup each rally. */
        bvx = bvx + FX / 2;

        g_beep(600, 40);
    }

    /* --- Right paddle collision --- */
    int rpad_x = WIN_W - PAD_MARGIN - PAD_W;
    int rpad_y = pad_y[1];
    if (bvx > 0 &&
        g_aabb(px, py, BALL_SIZE, BALL_SIZE,
               rpad_x, rpad_y, PAD_W, PAD_H)) {

        bx  = (rpad_x - BALL_SIZE) * FX;
        bvx = -iabs(bvx);

        int rel  = (py + BALL_SIZE / 2) - (rpad_y + PAD_H / 2);
        int spin = (rel * 3 * FX) / (PAD_H / 2);
        bvy = bvy + spin;

        if (iabs(bvx) > BALL_MAX_VX * FX) bvx = -BALL_MAX_VX * FX;
        if (iabs(bvy) > BALL_MAX_VY * FX) bvy = isign(bvy) * BALL_MAX_VY * FX;

        bvx = bvx - FX / 2;  /* make magnitude larger (it's negative) */

        g_beep(600, 40);
    }

    /* --- Scoring: ball exits left or right --- */
    px = bx / FX;
    if (px + BALL_SIZE < 0) {
        /* Right scores. */
        score[1]++;
        serve_dir = -1;  /* next serve goes left */
        g_beep(200, 120);
        g_state      = STATE_SCORE;
        score_timer  = 90; /* ~1.5 seconds */
        if (score[1] >= SCORE_WIN) {
            g_state = STATE_WIN;
            g_beep(880, 400);
        }
    } else if (px > WIN_W) {
        /* Left scores. */
        score[0]++;
        serve_dir = 1;  /* next serve goes right */
        g_beep(200, 120);
        g_state     = STATE_SCORE;
        score_timer = 90;
        if (score[0] >= SCORE_WIN) {
            g_state = STATE_WIN;
            g_beep(880, 400);
        }
    }
    (void)gx;
}

/* =========================================================================
 * Entry point.
 * ========================================================================= */
void _start(void)
{
    serial_write("[PONG] starting\n");

    game_t *gx = game_open(WIN_W, WIN_H, "Pong");
    if (!gx) {
        for (;;) { /* fatal */ }
    }

    game_reset(gx);

    while (game_frame_begin(gx)) {

        /* --- Pause toggle (Esc edge) --- */
        int esc_now = game_key_pressed(gx, KEY_ESC);
        if (esc_now && !prev_pause) {
            if (g_state == STATE_PLAY) {
                pre_pause = g_state;
                g_state   = STATE_PAUSED;
            } else if (g_state == STATE_PAUSED) {
                g_state   = pre_pause;
            }
        }
        prev_pause = esc_now;

        /* --- State machine --- */
        switch (g_state) {

        case STATE_IDLE:
            update_paddles(gx);
            if (game_key_pressed(gx, KEY_SPACE)) {
                ball_reset(gx);
                g_state = STATE_PLAY;
                g_beep(300, 60);
            }
            break;

        case STATE_PLAY:
            update_paddles(gx);
            update_ball(gx);
            break;

        case STATE_SCORE:
            update_paddles(gx);
            score_timer--;
            if (score_timer <= 0) {
                ball_reset(gx);
                g_state = STATE_IDLE;
            }
            break;

        case STATE_WIN:
            if (game_key_pressed(gx, KEY_SPACE)) {
                /* Print result to serial then reset. */
                serial_write("[PONG] game over left=");
                serial_int(score[0]);
                serial_write(" right=");
                serial_int(score[1]);
                serial_write("\n");
                game_reset(gx);
            }
            break;

        case STATE_PAUSED:
            /* Nothing to update. */
            break;
        }

        render(gx);
        game_present(gx);
        game_sync(gx);
    }
}
