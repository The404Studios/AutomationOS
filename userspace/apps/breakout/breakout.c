/*
 * breakout.c -- Classic Breakout / Arkanoid game (freestanding, ring 3).
 * ======================================================================
 *
 * 520x600 window.  Paddle at bottom (mouse or Left/Right / A/D keys).
 * Ball bounces off walls, ceiling, paddle, and bricks.  Multiple rows of
 * gradient-colored bricks; ball-paddle "english" (deflection angle depends
 * on where the ball strikes the paddle).  Score, lives (3), progressive
 * ball speed-up, win/next-level, game-over, smooth 60 fps.
 *
 * Uses the game framework (game.h) exclusively -- no raw wl_client calls.
 *
 * Controls:
 *   Mouse        -- move paddle (primary)
 *   Left / A     -- move paddle left
 *   Right / D    -- move paddle right
 *   Space/Enter  -- launch ball (from READY state) / restart (GAME OVER/WIN)
 *   ESC          -- exit
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/breakout/breakout.c -o /tmp/bo.o
 *   gcc <same> -c userspace/lib/game/game.c    -o /tmp/game.o
 *   gcc <same> -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc <same> -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/bo.o /tmp/game.o /tmp/wlc.o /tmp/bf.o -o /tmp/breakout.elf
 *   objdump -d /tmp/breakout.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [BREAKOUT] starting
 *   [BREAKOUT] score N
 */

#include "../../lib/game/game.h"

/* =========================================================================
 * Layout / constants
 * ========================================================================= */

#define WIN_W       520
#define WIN_H       600

/* HUD strip at top */
#define HUD_H        36

/* Play field (below HUD) */
#define FIELD_Y      HUD_H
#define FIELD_H      (WIN_H - HUD_H)

/* Paddle */
#define PAD_W        80
#define PAD_H        12
#define PAD_RADIUS    5
#define PAD_Y        (WIN_H - 28)
#define PAD_SPEED     5   /* pixels per frame when using keyboard */

/* Ball */
#define BALL_R        7

/* Ball fixed-point: velocity stored as signed integer, units = 1/256 pixel/frame.
 * This gives smooth sub-pixel motion without any floating point or libm.
 * Note: game.h defines FP_ONE=65536 (16.16); we use a separate 8.8 scale here. */
#define FP_SHIFT     8          /* 2^8 = 256 sub-pixel units per pixel        */
#define BK_FP_ONE    (1 << FP_SHIFT)

/* Initial ball speed in sub-pixel units (about 3 px/frame) */
#define BALL_SPEED_INIT   (3 * BK_FP_ONE)
/* Speed increment each time a brick row is cleared */
#define BALL_SPEED_INC    (BK_FP_ONE / 4)
/* Max speed cap (about 7 px/frame) */
#define BALL_SPEED_MAX    (7 * BK_FP_ONE)

/* Brick grid */
#define BRICK_COLS    10
#define BRICK_ROWS     7
#define BRICK_W       46
#define BRICK_H       16
#define BRICK_GAP      3
#define BRICK_RADIUS   3
/* Where the brick grid starts inside the play field */
#define BRICK_START_X  ((WIN_W - (BRICK_COLS * (BRICK_W + BRICK_GAP) - BRICK_GAP)) / 2)
#define BRICK_START_Y  (FIELD_Y + 18)

/* Score per brick hit (multiplied by row: bottom row = 1x, top row = BRICK_ROWS x) */
#define SCORE_PER_BRICK  10

/* Lives */
#define LIVES_INIT   3

/* =========================================================================
 * Colors (ARGB32)
 * ========================================================================= */

#define COL_BG         0xFF0D0D1A   /* very dark navy                     */
#define COL_BG2        0xFF111128   /* slightly lighter for field          */
#define COL_HUD_BG     0xFF0A0A18   /* HUD background                     */
#define COL_HUD_LINE   0xFF2A2A55   /* HUD bottom border                  */
#define COL_PADDLE     0xFFE0E8FF   /* bright white-blue paddle            */
#define COL_PADDLE_SH  0xFF8090C0   /* paddle shadow/edge                  */
#define COL_BALL       0xFFFFFFFF   /* white ball                          */
#define COL_BALL_GLOW  0xFFCCDDFF   /* ball glow ring                     */
#define COL_TEXT       0xFFE0E0FF
#define COL_SCORE_CLR  0xFFFFD700   /* gold score text                    */
#define COL_LIFE_CLR   0xFFFF6060   /* red lives dots                     */
#define COL_WIN        0xFF44FF88   /* win message                        */
#define COL_GAMEOVER   0xFFFF4444   /* game over message                  */
#define COL_READY      0xFF88AAFF   /* ready message                      */
#define COL_OVERLAY    0xAA000010   /* semi-dark overlay (uses alpha blending) */

/* Brick row gradient: 7 rows, top-to-bottom */
static const u32 BRICK_COLORS[BRICK_ROWS] = {
    0xFFFF3366,   /* row 0 (top)  -- hot pink / red    */
    0xFFFF6633,   /* row 1        -- orange             */
    0xFFFFCC00,   /* row 2        -- yellow             */
    0xFF44DD44,   /* row 3        -- green              */
    0xFF22CCFF,   /* row 4        -- cyan               */
    0xFF4488FF,   /* row 5        -- blue               */
    0xFF9966FF,   /* row 6 (bot)  -- violet             */
};

/* Highlighted (slightly lighter) versions for the top-edge gleam */
static const u32 BRICK_HI[BRICK_ROWS] = {
    0xFFFF88AA,
    0xFFFFAA88,
    0xFFFFEE88,
    0xFF88FF88,
    0xFF88EEFF,
    0xFF88BBFF,
    0xFFCCAAFF,
};

/* =========================================================================
 * Game state
 * ========================================================================= */

typedef enum {
    STATE_READY,      /* ball on paddle, waiting for launch          */
    STATE_PLAYING,    /* ball in flight                              */
    STATE_BALL_LOST,  /* brief pause after losing a ball             */
    STATE_GAME_OVER,
    STATE_WIN_LEVEL,  /* all bricks cleared, show next-level screen  */
    STATE_WIN_GAME,   /* all levels done                             */
} gs_t;

/* Brick: 0 = gone, 1 = alive */
static u8 bricks[BRICK_ROWS][BRICK_COLS];

/* Paddle position (center x) in pixels, fixed integer */
static int pad_cx;

/* Ball position and velocity in fixed-point (shifted by FP_SHIFT) */
static int ball_fx;   /* x in sub-pixels */
static int ball_fy;   /* y in sub-pixels */
static int ball_vx;   /* vx in sub-pixels/frame */
static int ball_vy;   /* vy in sub-pixels/frame */

/* Game counters */
static int score;
static int lives;
static int level;          /* 0-based, max 2 (3 levels) */
static int bricks_left;

/* State machine */
static gs_t gstate;

/* Pause timer for STATE_BALL_LOST (ms) */
static int lost_timer;

/* Speed accumulator: increases as bricks are destroyed */
static int ball_speed;   /* current base speed in sub-pixels */

/* Track previous mouse x for delta-based paddle movement (smooth) */
static int prev_mouse_x;

/* Serial write helper (uses SYS_WRITE via the framework's internal path;
 * we use the inline approach matching the existing games since game.h
 * doesn't expose a print function).                                         */
static inline long _sc_write(long a1, long a2, long a3)
{
    long r;
    register long r10 asm("r10") = 0;
    register long r8  asm("r8")  = 0;
    register long r9  asm("r9")  = 0;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(3L), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

static unsigned long bk_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void bk_print(const char *m)
{
    _sc_write(1, (long)m, (long)bk_strlen(m));
}

static void bk_print_int(int v)
{
    char buf[16];
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    int i = 0;
    if (v == 0) { buf[i++] = '0'; }
    else { char tmp[12]; int j = 0; while (v > 0) { tmp[j++] = (char)('0' + v % 10); v /= 10; } while (j > 0) buf[i++] = tmp[--j]; }
    if (neg) { /* prepend minus -- shift */ for (int k = i; k > 0; k--) buf[k] = buf[k-1]; buf[0] = '-'; i++; }
    buf[i] = '\0';
    _sc_write(1, (long)buf, i);
}

/* =========================================================================
 * Brick helpers
 * ========================================================================= */

/* Fill all bricks for current level */
static void bricks_init(void)
{
    bricks_left = 0;
    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++) {
            bricks[r][c] = 1;
            bricks_left++;
        }
}

/* Pixel rectangle of brick (r,c) -- top-left corner */
static inline int brick_px(int col) { return BRICK_START_X + col * (BRICK_W + BRICK_GAP); }
static inline int brick_py(int row) { return BRICK_START_Y + row * (BRICK_H + BRICK_GAP); }

/* =========================================================================
 * Game init / reset
 * ========================================================================= */

static void reset_ball(void)
{
    /* Place ball on top of paddle center */
    int px = g_clamp(pad_cx, PAD_W / 2, WIN_W - PAD_W / 2);
    ball_fx = px * BK_FP_ONE;
    ball_fy = (PAD_Y - BALL_R - 1) * BK_FP_ONE;
    ball_vx = 0;
    ball_vy = 0;
}

static void start_level(void)
{
    pad_cx = WIN_W / 2;
    bricks_init();
    ball_speed = BALL_SPEED_INIT + level * (BK_FP_ONE / 2);
    reset_ball();
    gstate = STATE_READY;
}

static void game_init(void)
{
    score  = 0;
    lives  = LIVES_INIT;
    level  = 0;
    start_level();
}

/* =========================================================================
 * Launch ball from paddle (called when player presses Space/Enter in READY)
 * ========================================================================= */
static void launch_ball(void)
{
    /* Launch at ~60 degrees up, slightly random left/right */
    int rdir = (g_rand() & 1) ? 1 : -1;
    ball_vx = rdir * (ball_speed / 2);
    ball_vy = -ball_speed;   /* upward */
    gstate  = STATE_PLAYING;
}

/* =========================================================================
 * Ball-paddle english: reflect ball based on hit position along paddle.
 *
 * The paddle is divided conceptually into 5 zones (left-edge to right-edge).
 * Zone 0 (far left) sends ball left-steeply; zone 4 (far right) right-steeply.
 * Zone 2 (center) sends ball straight up.  Zones 1/3 are intermediate.
 *
 * We preserve the current speed magnitude and remap vx/vy accordingly.
 * ========================================================================= */
static void paddle_reflect(void)
{
    /* Hit offset: -1.0 (far left) to +1.0 (far right), in 256ths */
    int bx  = ball_fx >> FP_SHIFT;
    int off = bx - pad_cx;                      /* pixels from paddle center */
    int half = PAD_W / 2;
    /* Clamp offset to [-half, half] */
    if (off < -half) off = -half;
    if (off >  half) off =  half;

    /* Map offset to vx: max horizontal deflection = ball_speed * 7/8 */
    /* vx = off/half * (7/8 * ball_speed) */
    int new_vx = (off * ball_speed * 7) / (half * 8);

    /* vy is always upward, magnitude = sqrt(speed^2 - vx^2).
     * We approximate: vy = -sqrt approximation via integer:
     *   We want |vx|^2 + |vy|^2 ~ ball_speed^2
     *   Use: vy = -ball_speed * cos(angle) where sin(angle) = vx/speed
     *   For integer: vy^2 = speed^2 - vx^2  -> vy = isqrt(speed^2 - vx^2)
     *   Simple isqrt via Newton iteration.                                  */
    int sp2 = ball_speed * ball_speed;
    int vx2 = new_vx * new_vx;
    int vy2 = sp2 - vx2;
    if (vy2 < (ball_speed / 4) * (ball_speed / 4))
        vy2 = (ball_speed / 4) * (ball_speed / 4);  /* minimum upward speed */

    /* Integer square root (Newton) */
    int est = ball_speed;
    for (int i = 0; i < 8; i++) {
        int next = (est + vy2 / est) / 2;
        if (next >= est) break;
        est = next;
    }
    int new_vy = -est;   /* always upward */

    ball_vx = new_vx;
    ball_vy = new_vy;

    /* Ensure ball is above paddle surface to avoid re-collision */
    ball_fy = (PAD_Y - BALL_R - 1) * BK_FP_ONE;
}

/* =========================================================================
 * Physics update (called once per frame in STATE_PLAYING)
 * ========================================================================= */
static void physics_update(void)
{
    /* Move ball */
    ball_fx += ball_vx;
    ball_fy += ball_vy;

    int bx = ball_fx >> FP_SHIFT;
    int by = ball_fy >> FP_SHIFT;

    /* --- Wall collisions --- */
    /* Left wall */
    if (bx - BALL_R < 0) {
        ball_fx = BALL_R * BK_FP_ONE;
        ball_vx = -ball_vx;
        g_beep(320, 30);
    }
    /* Right wall */
    if (bx + BALL_R >= WIN_W) {
        ball_fx = (WIN_W - BALL_R - 1) * BK_FP_ONE;
        ball_vx = -ball_vx;
        g_beep(320, 30);
    }
    /* Ceiling */
    if (by - BALL_R < FIELD_Y) {
        ball_fy = (FIELD_Y + BALL_R) * BK_FP_ONE;
        ball_vy = -ball_vy;
        g_beep(320, 30);
    }

    /* --- Bottom (ball lost) --- */
    if (by - BALL_R > WIN_H) {
        lives--;
        g_beep(120, 300);
        bk_print("[BREAKOUT] score ");
        bk_print_int(score);
        bk_print("\n");
        if (lives <= 0) {
            gstate = STATE_GAME_OVER;
        } else {
            gstate     = STATE_BALL_LOST;
            lost_timer = 1200;  /* ms to show "ball lost" message */
        }
        return;
    }

    /* Re-read after possible reflection */
    bx = ball_fx >> FP_SHIFT;
    by = ball_fy >> FP_SHIFT;

    /* --- Paddle collision --- */
    {
        int px = pad_cx - PAD_W / 2;
        int py = PAD_Y;
        /* AABB ball-box vs paddle rect */
        if (g_aabb(bx - BALL_R, by - BALL_R, BALL_R * 2, BALL_R * 2,
                   px, py, PAD_W, PAD_H))
        {
            /* Only reflect if ball is moving downward */
            if (ball_vy > 0) {
                paddle_reflect();
                g_beep(480, 40);
            }
        }
    }

    /* --- Brick collisions --- */
    bx = ball_fx >> FP_SHIFT;
    by = ball_fy >> FP_SHIFT;

    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            if (!bricks[row][col]) continue;

            int bpx = brick_px(col);
            int bpy = brick_py(row);

            if (!g_aabb(bx - BALL_R, by - BALL_R, BALL_R * 2, BALL_R * 2,
                        bpx, bpy, BRICK_W, BRICK_H))
                continue;

            /* Destroy brick */
            bricks[row][col] = 0;
            bricks_left--;

            /* Score: higher rows worth more */
            int row_mult = BRICK_ROWS - row;  /* row 0 = top = highest value */
            score += SCORE_PER_BRICK * row_mult;

            /* Sound: pitch varies by row */
            g_beep(400 + row_mult * 80, 60);

            /* Determine which face was hit for reflection.
             * Compare ball center to brick edges + margins.                 */
            int bl = bpx;
            int br = bpx + BRICK_W;
            int bt = bpy;
            int bb_b = bpy + BRICK_H;

            int over_left  = bx < bl;
            int over_right = bx > br;
            int over_top   = by < bt;
            int over_bot   = by > bb_b;

            if (!over_left && !over_right) {
                /* Hit top or bottom face */
                ball_vy = -ball_vy;
            } else if (!over_top && !over_bot) {
                /* Hit left or right face */
                ball_vx = -ball_vx;
            } else {
                /* Corner: reflect both */
                ball_vx = -ball_vx;
                ball_vy = -ball_vy;
            }

            /* Only hit one brick per frame to avoid tunneling */
            goto bricks_done;
        }
    }
bricks_done:;

    /* Speed up slightly over time (every brick destroyed above also helps) */
    /* Already handled: bricks destroyed increase per-level speed via start_level */

    /* Win check */
    if (bricks_left <= 0) {
        g_beep(880, 100);
        bk_print("[BREAKOUT] score ");
        bk_print_int(score);
        bk_print("\n");
        if (level >= 2) {
            gstate = STATE_WIN_GAME;
        } else {
            gstate = STATE_WIN_LEVEL;
        }
    }
}

/* =========================================================================
 * Rendering helpers
 * ========================================================================= */

/* Draw a centered text string at (cx, y). */
static void draw_centered(game_t *g, int cx, int y, const char *s, u32 col)
{
    g_text_center(g, cx, y, s, col);
}

/* Draw HUD: score + lives */
static void draw_hud(game_t *g)
{
    /* Background strip */
    g_fill_rect(g, 0, 0, WIN_W, HUD_H, COL_HUD_BG);
    /* Bottom border line */
    g_fill_rect(g, 0, HUD_H - 1, WIN_W, 1, COL_HUD_LINE);

    /* Score on left */
    g_draw_score(g, 8, (HUD_H - 16) / 2, "SCORE", score, COL_SCORE_CLR);

    /* Level in center */
    {
        char lbuf[24];
        lbuf[0] = 'L'; lbuf[1] = 'E'; lbuf[2] = 'V'; lbuf[3] = 'E';
        lbuf[4] = 'L'; lbuf[5] = ' '; lbuf[6] = (char)('1' + level); lbuf[7] = '\0';
        g_text_center(g, WIN_W / 2, (HUD_H - 16) / 2, lbuf, COL_TEXT);
    }

    /* Lives as small filled circles on the right */
    {
        int lx = WIN_W - 12;
        int ly = HUD_H / 2;
        for (int i = 0; i < lives; i++) {
            g_circle(g, lx - i * 16, ly, 5, COL_LIFE_CLR);
        }
    }
}

/* Draw brick grid */
static void draw_bricks(game_t *g)
{
    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            if (!bricks[row][col]) continue;
            int bpx = brick_px(col);
            int bpy = brick_py(row);
            u32 bc  = BRICK_COLORS[row];
            u32 bhi = BRICK_HI[row];

            /* Main brick body */
            g_rounded_rect(g, bpx, bpy, BRICK_W, BRICK_H, BRICK_RADIUS, bc);
            /* Top gleam highlight (1px line inside top edge) */
            g_fill_rect(g, bpx + BRICK_RADIUS, bpy + 1,
                        BRICK_W - BRICK_RADIUS * 2, 2, bhi);
        }
    }
}

/* Draw paddle */
static void draw_paddle(game_t *g)
{
    int px = pad_cx - PAD_W / 2;
    int py = PAD_Y;
    /* Shadow */
    g_rounded_rect(g, px + 2, py + 2, PAD_W, PAD_H, PAD_RADIUS, 0xFF000033);
    /* Paddle body */
    g_rounded_rect(g, px, py, PAD_W, PAD_H, PAD_RADIUS, COL_PADDLE);
    /* Top gleam */
    g_fill_rect(g, px + PAD_RADIUS, py + 1, PAD_W - PAD_RADIUS * 2, 2, 0xFFFFFFFF);
}

/* Draw ball */
static void draw_ball(game_t *g)
{
    int bx = ball_fx >> FP_SHIFT;
    int by = ball_fy >> FP_SHIFT;
    /* Glow ring */
    g_circle(g, bx, by, BALL_R + 2, 0x44AABBFF);
    /* Ball body */
    g_circle(g, bx, by, BALL_R, COL_BALL);
    /* Specular highlight */
    g_circle(g, bx - 2, by - 2, 2, 0xFFFFFFFF);
}

/* Draw subtle background grid dots */
static void draw_background(game_t *g)
{
    g_clear(g, COL_BG);
    /* Draw a subtle dot grid every 30px for depth */
    for (int y = FIELD_Y + 15; y < WIN_H; y += 30) {
        for (int x = 15; x < WIN_W; x += 30) {
            g_pixel(g, x, y, 0xFF1A1A33);
        }
    }
}

/* Semi-transparent overlay: draw every-other pixel dark to simulate darkening */
static void draw_overlay(game_t *g)
{
    /* Stippled overlay: every other pixel row/col gets a dark tint */
    for (int y = FIELD_Y; y < WIN_H; y += 2) {
        g_fill_rect(g, 0, y, WIN_W, 1, 0x88000010);
    }
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

void _start(void)
{
    bk_print("[BREAKOUT] starting\n");

    game_t *g = game_open(WIN_W, WIN_H, "Breakout");
    if (!g) {
        bk_print("[BREAKOUT] game_open FAILED\n");
        /* Spin forever rather than returning to nothing */
        for (;;) {
            long r;
            register long r10 asm("r10") = 0;
            register long r8  asm("r8")  = 0;
            register long r9  asm("r9")  = 0;
            asm volatile("syscall"
                         : "=a"(r)
                         : "a"(15L), "D"(0L), "S"(0L), "d"(0L),
                           "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
        }
    }

    /* Seed per current ticks (game_open seeds g_srand internally, this is extra) */
    g_srand((u32)game_ticks());

    game_init();

    /* Track previous mouse x for paddle control */
    {
        int mx, my, mb;
        game_mouse(g, &mx, &my, &mb);
        prev_mouse_x = mx;
    }

    while (game_frame_begin(g)) {

        int dt = game_dt_ms(g);
        if (dt <= 0) dt = 16;

        /* ---- Input: ESC to quit ---- */
        if (game_key_pressed(g, KEY_ESC)) break;

        /* ---- Mouse paddle tracking ---- */
        {
            int mx, my, mb;
            game_mouse(g, &mx, &my, &mb);
            if (mx != prev_mouse_x) {
                pad_cx += (mx - prev_mouse_x);
                prev_mouse_x = mx;
            }
        }

        /* ---- Keyboard paddle movement ---- */
        if (game_key_down(g, KEY_LEFT) || game_key_down(g, KEY_A))
            pad_cx -= PAD_SPEED;
        if (game_key_down(g, KEY_RIGHT) || game_key_down(g, KEY_D))
            pad_cx += PAD_SPEED;

        /* Clamp paddle */
        pad_cx = g_clamp(pad_cx, PAD_W / 2, WIN_W - PAD_W / 2);

        /* ---- State machine ---- */
        switch (gstate) {

        case STATE_READY:
            /* Keep ball on paddle */
            reset_ball();
            /* Launch on Space or Enter */
            if (game_key_pressed(g, KEY_SPACE) || game_key_pressed(g, KEY_ENTER)) {
                launch_ball();
            }
            break;

        case STATE_PLAYING:
            /* Run physics (may change gstate) */
            physics_update();
            break;

        case STATE_BALL_LOST:
            lost_timer -= dt;
            if (lost_timer <= 0) {
                reset_ball();
                gstate = STATE_READY;
            }
            break;

        case STATE_GAME_OVER:
            /* Wait for restart key */
            if (game_key_pressed(g, KEY_SPACE) || game_key_pressed(g, KEY_ENTER)) {
                game_init();
            }
            break;

        case STATE_WIN_LEVEL:
            /* Short pause then advance */
            lost_timer -= dt;
            if (lost_timer <= 0 ||
                game_key_pressed(g, KEY_SPACE) || game_key_pressed(g, KEY_ENTER))
            {
                level++;
                /* Increase speed for next level */
                ball_speed = BALL_SPEED_INIT + level * (BK_FP_ONE / 2);
                if (ball_speed > BALL_SPEED_MAX) ball_speed = BALL_SPEED_MAX;
                start_level();
            }
            break;

        case STATE_WIN_GAME:
            if (game_key_pressed(g, KEY_SPACE) || game_key_pressed(g, KEY_ENTER)) {
                game_init();
            }
            break;
        }

        /* ---- Render ---- */
        draw_background(g);
        draw_bricks(g);
        draw_paddle(g);
        draw_ball(g);
        draw_hud(g);

        /* ---- Overlays for non-playing states ---- */
        if (gstate == STATE_READY) {
            draw_centered(g, WIN_W / 2, WIN_H / 2 - 16,
                          "READY", COL_READY);
            draw_centered(g, WIN_W / 2, WIN_H / 2 + 4,
                          "SPACE to launch", COL_TEXT);
        } else if (gstate == STATE_BALL_LOST) {
            draw_overlay(g);
            draw_centered(g, WIN_W / 2, WIN_H / 2 - 10,
                          "BALL LOST!", 0xFFFF8844);
        } else if (gstate == STATE_GAME_OVER) {
            draw_overlay(g);
            draw_centered(g, WIN_W / 2, WIN_H / 2 - 24,
                          "GAME OVER", COL_GAMEOVER);
            g_draw_score(g, WIN_W / 2 - 60, WIN_H / 2,
                         "FINAL SCORE", score, COL_SCORE_CLR);
            draw_centered(g, WIN_W / 2, WIN_H / 2 + 24,
                          "SPACE to restart", COL_TEXT);
        } else if (gstate == STATE_WIN_LEVEL) {
            draw_overlay(g);
            draw_centered(g, WIN_W / 2, WIN_H / 2 - 24,
                          "LEVEL CLEAR!", COL_WIN);
            draw_centered(g, WIN_W / 2, WIN_H / 2 + 4,
                          "SPACE for next level", COL_TEXT);
        } else if (gstate == STATE_WIN_GAME) {
            draw_overlay(g);
            draw_centered(g, WIN_W / 2, WIN_H / 2 - 32,
                          "YOU WIN!", COL_WIN);
            draw_centered(g, WIN_W / 2, WIN_H / 2 - 8,
                          "CONGRATULATIONS!", COL_WIN);
            g_draw_score(g, WIN_W / 2 - 60, WIN_H / 2 + 16,
                         "FINAL SCORE", score, COL_SCORE_CLR);
            draw_centered(g, WIN_W / 2, WIN_H / 2 + 40,
                          "SPACE to play again", COL_TEXT);
        }

        game_present(g);
        game_sync(g);
    }

    /* Print final score to serial on exit */
    bk_print("[BREAKOUT] score ");
    bk_print_int(score);
    bk_print("\n");
}
