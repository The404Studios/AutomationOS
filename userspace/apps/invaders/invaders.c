/*
 * invaders.c -- Classic Space Invaders (freestanding, ring 3).
 * =============================================================
 *
 * 560x620 window.
 * - 5 rows x 11 cols of aliens in 3 types, marching left/right + descending.
 * - Player ship at bottom; Left/Right to move, Space to fire.
 * - Alien bullets rain down at increasing rate per wave.
 * - 4 destructible shields above the player.
 * - Score, lives, wave counter.  Lives = 3.
 * - Waves speed up; game over at 0 lives or aliens reach player row.
 * - Press Enter to restart after game over.
 *
 * SFX: g_beep for player shot, alien hit, alien shot, player death, wave clear.
 *
 * Serial:
 *   [INVADERS] starting
 *   [INVADERS] score N           (on game over)
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/invaders/invaders.c -o /tmp/invaders.o
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
 *       /tmp/invaders.o /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/invaders.elf
 *   objdump -d /tmp/invaders.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/game/game.h"

/* =========================================================================
 * Serial output helpers.
 * ========================================================================= */
static inline long _inv_sc3(long n, long a, long b, long c)
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
    _inv_sc3(SYS_WRITE, 1, (long)s, (long)len);
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
#define WIN_W          560
#define WIN_H          620

/* HUD strip at top. */
#define HUD_H          30

/* Alien grid. */
#define ALIEN_COLS     11
#define ALIEN_ROWS     5
#define ALIEN_CW       40       /* cell width  */
#define ALIEN_CH       32       /* cell height */
#define ALIEN_GRID_X   28       /* left edge of alien grid */
#define ALIEN_GRID_Y   (HUD_H + 20)

/* Player. */
#define PLAYER_W       36
#define PLAYER_H       16
#define PLAYER_SPEED   3
#define PLAYER_Y       (WIN_H - 50)
#define PLAYER_XMIN    8
#define PLAYER_XMAX    (WIN_W - 8 - PLAYER_W)

/* Bullets. */
#define PLAYER_BULLET_W  3
#define PLAYER_BULLET_H  10
#define PLAYER_BULLET_SPD 8

#define ALIEN_BULLET_W   3
#define ALIEN_BULLET_H   8
#define ALIEN_BULLET_SPD 4

#define MAX_PLAYER_BULLETS  2
#define MAX_ALIEN_BULLETS   6

/* Shields. */
#define SHIELD_COUNT   4
#define SHIELD_W       44
#define SHIELD_H       28
#define SHIELD_Y       (PLAYER_Y - 48)

/* Each shield is a 11x7 bitmask of block health (0=destroyed, 1-3=hp). */
#define SHIELD_BW      11
#define SHIELD_BH      7
#define SHIELD_BLOCK_W (SHIELD_W / SHIELD_BW)   /* ~4 */
#define SHIELD_BLOCK_H (SHIELD_H / SHIELD_BH)   /* 4  */

/* Score values per alien type (rows 0=top, 4=bottom). */
/* Type 0 (rows 0): 30 pts, Type 1 (rows 1-2): 20 pts, Type 2 (rows 3-4): 10 pts. */
static const int ALIEN_SCORE[3] = { 30, 20, 10 };

/* How many alien rows belong to each type. */
/* Row 0 -> type 0, rows 1-2 -> type 1, rows 3-4 -> type 2 */
static inline int row_to_type(int r) {
    if (r == 0) return 0;
    if (r <= 2)  return 1;
    return 2;
}

/* Alien march: initial period (frames between steps). */
#define MARCH_PERIOD_BASE  48
#define MARCH_PERIOD_MIN    6
#define MARCH_STEP_X        8   /* pixels per horizontal step */
#define MARCH_STEP_Y       16   /* pixels to descend per wrap */

/* Alien bullet fire: average frames between a new alien bullet. */
#define ALIEN_FIRE_PERIOD_BASE 60
#define ALIEN_FIRE_PERIOD_MIN  16

/* UFO (mystery ship) at top. */
#define UFO_Y          (HUD_H + 6)
#define UFO_W          36
#define UFO_H          14
#define UFO_SCORE      100
#define UFO_PERIOD     700   /* frames between UFO appearances */

/* Lives. */
#define LIVES_INIT     3

/* =========================================================================
 * Colours.
 * ========================================================================= */
#define COL_BG         0xFF080810
#define COL_STAR       0xFF888888
#define COL_HUD_BG     0xFF101028
#define COL_PLAYER     0xFF40FF40
#define COL_PBULLET    0xFFFFFF40
#define COL_ABULLET    0xFFFF4040
#define COL_SHIELD     0xFF20CC20
#define COL_UFO        0xFFFF44CC
#define COL_TEXT       0xFFDDDDDD
#define COL_SCORE_VAL  0xFFFFD700
#define COL_LIVES      0xFF44FF44
#define COL_GAMEOVER   0xFFFF2020
#define COL_WIN        0xFFFFD700
#define COL_WAVE       0xFF4488FF

/* Alien colours by type. */
static const u32 ALIEN_COLS_[3] = {
    0xFFFF4488,  /* type 0: magenta (top row, rarest/most valuable)    */
    0xFF44BBFF,  /* type 1: cyan    (middle rows)                       */
    0xFF88FF44,  /* type 2: green   (bottom rows, most common)          */
};

/* =========================================================================
 * Data structures.
 * ========================================================================= */
typedef struct {
    int alive;   /* 0 = dead */
    int explode; /* frames of explosion remaining */
} alien_t;

typedef struct {
    int active;
    int x, y;
} bullet_t;

typedef struct {
    int x;        /* top-left x of shield */
    /* 11*7 block hp map; 0=gone, 1-3=hp */
    u8  hp[SHIELD_BW * SHIELD_BH];
} shield_t;

/* =========================================================================
 * Global state.
 * ========================================================================= */
static alien_t   aliens[ALIEN_ROWS][ALIEN_COLS];
static int       alien_count;   /* living aliens */

static int       march_dx;      /* current direction: +1 or -1 */
static int       march_x;       /* pixel offset of grid left edge */
static int       march_y;       /* pixel offset of grid top edge */
static int       march_timer;   /* frames until next march step */
static int       march_period;  /* frames between steps */

static bullet_t  pbullets[MAX_PLAYER_BULLETS];
static bullet_t  abullets[MAX_ALIEN_BULLETS];

static shield_t  shields[SHIELD_COUNT];

static int       player_x;
static int       player_lives;
static int       player_dead_timer; /* >0 = respawn delay */

static int       score;
static int       wave;

static int       fire_timer;    /* countdown to next alien shot */
static int       fire_period;

/* UFO. */
static int       ufo_active;
static int       ufo_x;
static int       ufo_dir;     /* +1 or -1 */
static int       ufo_timer;   /* countdown to next UFO */

typedef enum {
    ISTATE_PLAY,
    ISTATE_GAME_OVER,
    ISTATE_WIN_WAVE
} inv_state_t;

static inv_state_t g_istate;
static int         state_timer;

/* =========================================================================
 * Stars (static random background).
 * ========================================================================= */
#define NUM_STARS 80
static int star_x[NUM_STARS];
static int star_y[NUM_STARS];
static u32 star_col[NUM_STARS];

static void init_stars(void)
{
    for (int i = 0; i < NUM_STARS; i++) {
        star_x[i]   = g_rand_range(WIN_W);
        star_y[i]   = HUD_H + g_rand_range(WIN_H - HUD_H);
        int bright  = 60 + g_rand_range(100);
        star_col[i] = 0xFF000000 | (bright << 16) | (bright << 8) | bright;
    }
}

/* =========================================================================
 * Shield helpers.
 * ========================================================================= */
static void shields_reset(void)
{
    int gap = (WIN_W - SHIELD_COUNT * SHIELD_W) / (SHIELD_COUNT + 1);
    for (int i = 0; i < SHIELD_COUNT; i++) {
        shields[i].x = gap + i * (SHIELD_W + gap);
        for (int j = 0; j < SHIELD_BW * SHIELD_BH; j++) {
            shields[i].hp[j] = 3;
        }
        /* Carve out a notch at the bottom centre for the player to stand under. */
        for (int brow = 4; brow < SHIELD_BH; brow++) {
            for (int bcol = 3; bcol < 8; bcol++) {
                shields[i].hp[brow * SHIELD_BW + bcol] = 0;
            }
        }
    }
}

static void draw_shields(game_t *gx)
{
    for (int si = 0; si < SHIELD_COUNT; si++) {
        int sx0 = shields[si].x;
        for (int brow = 0; brow < SHIELD_BH; brow++) {
            for (int bcol = 0; bcol < SHIELD_BW; bcol++) {
                u8 hp = shields[si].hp[brow * SHIELD_BW + bcol];
                if (hp == 0) continue;
                u32 c;
                if (hp == 3)      c = COL_SHIELD;
                else if (hp == 2) c = 0xFF1A991A;
                else              c = 0xFF0D660D;
                int px = sx0 + bcol * SHIELD_BLOCK_W;
                int py = SHIELD_Y + brow * SHIELD_BLOCK_H;
                g_fill_rect(gx, px, py, SHIELD_BLOCK_W - 1, SHIELD_BLOCK_H - 1, c);
            }
        }
    }
}

/* Damage shield blocks overlapping rect (bx,by,bw,bh). Returns 1 if hit. */
static int shield_damage(int bx_, int by_, int bw_, int bh_)
{
    int hit = 0;
    for (int si = 0; si < SHIELD_COUNT; si++) {
        int sx0 = shields[si].x;
        for (int brow = 0; brow < SHIELD_BH; brow++) {
            for (int bcol = 0; bcol < SHIELD_BW; bcol++) {
                u8 hp = shields[si].hp[brow * SHIELD_BW + bcol];
                if (hp == 0) continue;
                int px = sx0 + bcol * SHIELD_BLOCK_W;
                int py = SHIELD_Y + brow * SHIELD_BLOCK_H;
                if (g_aabb(bx_, by_, bw_, bh_, px, py, SHIELD_BLOCK_W, SHIELD_BLOCK_H)) {
                    shields[si].hp[brow * SHIELD_BW + bcol]--;
                    hit = 1;
                }
            }
        }
    }
    return hit;
}

/* =========================================================================
 * Alien grid helpers.
 * ========================================================================= */
static void aliens_reset(void)
{
    for (int r = 0; r < ALIEN_ROWS; r++) {
        for (int c = 0; c < ALIEN_COLS; c++) {
            aliens[r][c].alive   = 1;
            aliens[r][c].explode = 0;
        }
    }
    alien_count = ALIEN_ROWS * ALIEN_COLS;
}

/* Pixel position of an alien cell (top-left of the alien sprite). */
static int alien_px(int c) { return ALIEN_GRID_X + march_x + c * ALIEN_CW + 4; }
static int alien_py(int r) { return ALIEN_GRID_Y + march_y + r * ALIEN_CH + 2; }

/* Alien sprite width/height (drawn, not full cell). */
#define ALIEN_SW   28
#define ALIEN_SH   20

/* =========================================================================
 * March period computation: decreases as alien count drops and wave rises.
 * ========================================================================= */
static void update_march_period(void)
{
    /* Base: MARCH_PERIOD_BASE, reduce by (55 - alien_count)/55 * (BASE-MIN). */
    int killed = ALIEN_ROWS * ALIEN_COLS - alien_count;
    int total  = ALIEN_ROWS * ALIEN_COLS;
    int span   = MARCH_PERIOD_BASE - MARCH_PERIOD_MIN;
    int wave_bonus = (wave - 1) * 4;
    march_period = MARCH_PERIOD_BASE - (killed * span / total) - wave_bonus;
    if (march_period < MARCH_PERIOD_MIN) march_period = MARCH_PERIOD_MIN;
}

/* =========================================================================
 * Fire period: how often aliens shoot.
 * ========================================================================= */
static void update_fire_period(void)
{
    int wave_bonus = (wave - 1) * 6;
    fire_period = ALIEN_FIRE_PERIOD_BASE - wave_bonus;
    if (fire_period < ALIEN_FIRE_PERIOD_MIN) fire_period = ALIEN_FIRE_PERIOD_MIN;
}

/* =========================================================================
 * Alien bullet firing: pick a random living alien from the bottom of a column.
 * ========================================================================= */
static void alien_fire(void)
{
    if (alien_count == 0) return;

    /* Find a free alien bullet slot. */
    int slot = -1;
    for (int i = 0; i < MAX_ALIEN_BULLETS; i++) {
        if (!abullets[i].active) { slot = i; break; }
    }
    if (slot < 0) return;

    /* Pick a random column and find the bottom-most alive alien. */
    int start_col = g_rand_range(ALIEN_COLS);
    for (int attempt = 0; attempt < ALIEN_COLS; attempt++) {
        int col = (start_col + attempt) % ALIEN_COLS;
        for (int row = ALIEN_ROWS - 1; row >= 0; row--) {
            if (aliens[row][col].alive) {
                abullets[slot].active = 1;
                abullets[slot].x = alien_px(col) + ALIEN_SW / 2 - ALIEN_BULLET_W / 2;
                abullets[slot].y = alien_py(row) + ALIEN_SH;
                g_beep(180, 25);
                return;
            }
        }
    }
}

/* =========================================================================
 * Wave start.
 * ========================================================================= */
static void wave_start(void)
{
    aliens_reset();
    march_x = 0;
    march_y = 0;
    march_dx = 1;
    update_march_period();
    march_timer = march_period;
    update_fire_period();
    fire_timer = fire_period;

    /* Clear bullets. */
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++) pbullets[i].active = 0;
    for (int i = 0; i < MAX_ALIEN_BULLETS;  i++) abullets[i].active  = 0;

    ufo_active = 0;
    ufo_timer  = UFO_PERIOD + g_rand_range(200);
}

/* =========================================================================
 * Full game reset (new game).
 * ========================================================================= */
static void game_reset(game_t *gx)
{
    score        = 0;
    wave         = 1;
    player_lives = LIVES_INIT;
    player_x     = WIN_W / 2 - PLAYER_W / 2;
    player_dead_timer = 0;
    g_istate     = ISTATE_PLAY;
    state_timer  = 0;

    shields_reset();
    wave_start();
    (void)gx;
}

/* =========================================================================
 * Draw alien sprite (three types, two animation frames).
 * Uses small pixel art drawn with g_fill_rect.
 * ========================================================================= */

/* Bitmap rows for each alien type (2 frames each), 7 pixels wide. */
/* Rendered at ALIEN_SW=28 px wide, ALIEN_SH=20 px tall, pixel = 4x4. */

/* Type 0 (top): "UFO crab" shape */
static const u8 AL0_F0[5] = { 0b0001000, 0b0111110, 0b1101011, 0b1111110, 0b0101010 };
static const u8 AL0_F1[5] = { 0b0001000, 0b0111110, 0b1101011, 0b1111110, 0b1010101 };
/* Type 1 (mid): "squid" shape */
static const u8 AL1_F0[5] = { 0b0010000, 0b0111110, 0b1111111, 0b1011010, 0b0101010 };
static const u8 AL1_F1[5] = { 0b0010000, 0b0111110, 0b1111111, 0b1011010, 0b1010101 };
/* Type 2 (btm): "bug" shape */
static const u8 AL2_F0[5] = { 0b1100110, 0b0111110, 0b1111111, 0b1111111, 0b0100100 };
static const u8 AL2_F1[5] = { 0b1100110, 0b0111110, 0b1111111, 0b1111111, 0b1011010 };

static int g_anim_frame = 0; /* 0 or 1, toggled each march step */

static void draw_alien_sprite(game_t *gx, int x, int y, int type, int frame, u32 col)
{
    const u8 *rows;
    if (type == 0) rows = frame ? AL0_F1 : AL0_F0;
    else if (type == 1) rows = frame ? AL1_F1 : AL1_F0;
    else               rows = frame ? AL2_F1 : AL2_F0;

    int ps = 4;  /* pixel scale */
    for (int row = 0; row < 5; row++) {
        for (int bit = 0; bit < 7; bit++) {
            if (rows[row] & (0x40 >> bit)) {
                g_fill_rect(gx,
                            x + bit * ps,
                            y + row * ps,
                            ps, ps, col);
            }
        }
    }
}

/* =========================================================================
 * Draw player ship: simple pixelart triangle+base.
 * ========================================================================= */
static void draw_player(game_t *gx, int x, int y)
{
    /* Cannon barrel */
    g_fill_rect(gx, x + PLAYER_W/2 - 2, y,            4, 6, COL_PLAYER);
    /* Body hull */
    g_fill_rect(gx, x + 4, y + 6,     PLAYER_W - 8, 6,  COL_PLAYER);
    /* Base */
    g_fill_rect(gx, x,     y + 10,    PLAYER_W,     6,  COL_PLAYER);
    /* Engine glows */
    g_fill_rect(gx, x + 4,            y + 14, 6, 2, 0xFF226622);
    g_fill_rect(gx, x + PLAYER_W - 10, y + 14, 6, 2, 0xFF226622);
}

/* =========================================================================
 * Draw UFO.
 * ========================================================================= */
static void draw_ufo(game_t *gx)
{
    if (!ufo_active) return;
    int x = ufo_x;
    int y = UFO_Y;
    /* Saucer shape */
    g_fill_rect(gx, x + 12, y,      12, 4, COL_UFO);
    g_fill_rect(gx, x + 6,  y + 4,  24, 6, COL_UFO);
    g_fill_rect(gx, x,      y + 8,  UFO_W, 6, COL_UFO);
    /* Lights */
    g_fill_rect(gx, x + 4,  y + 9,  4, 4, 0xFFFF88FF);
    g_fill_rect(gx, x + 14, y + 9,  4, 4, 0xFFFF88FF);
    g_fill_rect(gx, x + 24, y + 9,  4, 4, 0xFFFF88FF);
}

/* =========================================================================
 * HUD: score, lives, wave.
 * ========================================================================= */
static void draw_hud(game_t *gx)
{
    g_fill_rect(gx, 0, 0, WIN_W, HUD_H, COL_HUD_BG);
    g_fill_rect(gx, 0, HUD_H - 1, WIN_W, 1, 0xFF2233AA);

    /* Score */
    g_text(gx, 4, 8, "SCORE", COL_TEXT);
    g_draw_int(gx, 52, 8, score, COL_SCORE_VAL);

    /* Wave */
    g_text_center(gx, WIN_W / 2, 14, "WAVE", COL_TEXT);
    g_draw_int(gx, WIN_W/2 + 20, 8, wave, COL_WAVE);

    /* Lives */
    {
        char buf[32];
        buf[0] = 'L'; buf[1] = 'I'; buf[2] = 'V'; buf[3] = 'E'; buf[4] = 'S'; buf[5] = ':'; buf[6] = ' ';
        int n = 7;
        for (int i = 0; i < player_lives; i++) {
            buf[n++] = '<';
            buf[n++] = '3';
            buf[n++] = ' ';
        }
        buf[n] = '\0';
        g_text(gx, WIN_W - 140, 8, buf, COL_LIVES);
    }
}

/* =========================================================================
 * Explosion particle effect (simple crosshair for 1 frame).
 * ========================================================================= */
static void draw_explosion(game_t *gx, int x, int y, int w, int h)
{
    g_fill_rect(gx, x - 4, y + h/2 - 2, w + 8, 4, 0xFFFFAA00);
    g_fill_rect(gx, x + w/2 - 2, y - 4, 4, h + 8, 0xFFFFAA00);
    g_text(gx, x, y, "*", 0xFFFFFF44);
}

/* =========================================================================
 * Update: march aliens.
 * ========================================================================= */
static void update_march(void)
{
    march_timer--;
    if (march_timer > 0) return;
    march_timer = march_period;

    /* Toggle animation frame. */
    g_anim_frame ^= 1;

    /* Compute current bounds of alien grid. */
    int left_col  = ALIEN_COLS;
    int right_col = -1;
    int bot_row   = -1;
    for (int r = 0; r < ALIEN_ROWS; r++) {
        for (int c = 0; c < ALIEN_COLS; c++) {
            if (aliens[r][c].alive) {
                if (c < left_col)  left_col  = c;
                if (c > right_col) right_col = c;
                if (r > bot_row)   bot_row   = r;
            }
        }
    }
    if (right_col < 0) return; /* all dead */

    /* Pixel extents of living alien grid. */
    int grid_left  = ALIEN_GRID_X + march_x + left_col  * ALIEN_CW + 4;
    int grid_right = ALIEN_GRID_X + march_x + right_col * ALIEN_CW + 4 + ALIEN_SW;

    int next_x = march_x + march_dx * MARCH_STEP_X;
    int next_left  = grid_left  + march_dx * MARCH_STEP_X;
    int next_right = grid_right + march_dx * MARCH_STEP_X;

    /* Check if next step would go out of bounds. */
    int wall_l = 4;
    int wall_r = WIN_W - 4;

    if (next_left < wall_l || next_right > wall_r) {
        /* Descend and reverse. */
        march_y  += MARCH_STEP_Y;
        march_dx  = -march_dx;
    } else {
        march_x = next_x;
    }

    /* Update period now (fewer aliens = faster). */
    update_march_period();
    march_timer = march_period;
}

/* =========================================================================
 * Update: player bullets.
 * ========================================================================= */
static void update_player_bullets(void)
{
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++) {
        if (!pbullets[i].active) continue;
        pbullets[i].y -= PLAYER_BULLET_SPD;
        if (pbullets[i].y + PLAYER_BULLET_H < 0) {
            pbullets[i].active = 0;
            continue;
        }

        int bx_ = pbullets[i].x;
        int by_ = pbullets[i].y;

        /* Hit shield. */
        if (shield_damage(bx_, by_, PLAYER_BULLET_W, PLAYER_BULLET_H)) {
            pbullets[i].active = 0;
            g_beep(250, 20);
            continue;
        }

        /* Hit alien. */
        int hit = 0;
        for (int r = 0; r < ALIEN_ROWS && !hit; r++) {
            for (int c = 0; c < ALIEN_COLS && !hit; c++) {
                if (!aliens[r][c].alive) continue;
                int ax = alien_px(c);
                int ay = alien_py(r);
                if (g_aabb(bx_, by_, PLAYER_BULLET_W, PLAYER_BULLET_H,
                           ax, ay, ALIEN_SW, ALIEN_SH)) {
                    aliens[r][c].alive   = 0;
                    aliens[r][c].explode = 6;
                    alien_count--;
                    score += ALIEN_SCORE[row_to_type(r)];
                    pbullets[i].active = 0;
                    g_beep(440, 35);
                    update_march_period();
                    hit = 1;
                }
            }
        }
        if (hit) continue;

        /* Hit UFO. */
        if (ufo_active &&
            g_aabb(bx_, by_, PLAYER_BULLET_W, PLAYER_BULLET_H,
                   ufo_x, UFO_Y, UFO_W, UFO_H)) {
            ufo_active = 0;
            score += UFO_SCORE;
            pbullets[i].active = 0;
            g_beep(1200, 60);
        }
    }
}

/* =========================================================================
 * Update: alien bullets.
 * ========================================================================= */
static void update_alien_bullets(game_t *gx)
{
    for (int i = 0; i < MAX_ALIEN_BULLETS; i++) {
        if (!abullets[i].active) continue;
        abullets[i].y += ALIEN_BULLET_SPD;

        int bx_ = abullets[i].x;
        int by_ = abullets[i].y;

        if (by_ > WIN_H) {
            abullets[i].active = 0;
            continue;
        }

        /* Hit shield. */
        if (shield_damage(bx_, by_, ALIEN_BULLET_W, ALIEN_BULLET_H)) {
            abullets[i].active = 0;
            g_beep(220, 20);
            continue;
        }

        /* Hit player. */
        if (player_dead_timer == 0 &&
            g_aabb(bx_, by_, ALIEN_BULLET_W, ALIEN_BULLET_H,
                   player_x, PLAYER_Y, PLAYER_W, PLAYER_H)) {
            abullets[i].active = 0;
            player_lives--;
            player_dead_timer = 120; /* ~2 second respawn */
            g_beep(100, 300);
            if (player_lives <= 0) {
                g_istate    = ISTATE_GAME_OVER;
                state_timer = 180;
            }
        }
    }
    (void)gx;
}

/* =========================================================================
 * Update: UFO.
 * ========================================================================= */
static void update_ufo(void)
{
    if (!ufo_active) {
        ufo_timer--;
        if (ufo_timer <= 0) {
            ufo_active = 1;
            ufo_dir    = (g_rand_range(2) ? 1 : -1);
            ufo_x      = (ufo_dir > 0) ? -UFO_W : WIN_W;
            ufo_timer  = UFO_PERIOD + g_rand_range(400);
        }
    } else {
        ufo_x += ufo_dir * 2;
        if (ufo_x > WIN_W + UFO_W || ufo_x < -UFO_W * 2)
            ufo_active = 0;
    }
}

/* =========================================================================
 * Check if aliens reached the player row (instant game over).
 * ========================================================================= */
static void check_alien_invasion(void)
{
    for (int r = 0; r < ALIEN_ROWS; r++) {
        for (int c = 0; c < ALIEN_COLS; c++) {
            if (!aliens[r][c].alive) continue;
            if (alien_py(r) + ALIEN_SH >= PLAYER_Y - 4) {
                g_istate    = ISTATE_GAME_OVER;
                state_timer = 180;
                g_beep(80, 500);
                return;
            }
        }
    }
}

/* =========================================================================
 * Main render.
 * ========================================================================= */
static void render(game_t *gx)
{
    g_clear(gx, COL_BG);

    /* Stars */
    for (int i = 0; i < NUM_STARS; i++)
        g_pixel(gx, star_x[i], star_y[i], star_col[i]);

    /* HUD */
    draw_hud(gx);

    /* Bottom border */
    g_fill_rect(gx, 0, PLAYER_Y + PLAYER_H + 4, WIN_W, 2, 0xFF2244AA);

    /* Shields */
    draw_shields(gx);

    /* UFO */
    draw_ufo(gx);

    /* Aliens */
    for (int r = 0; r < ALIEN_ROWS; r++) {
        for (int c = 0; c < ALIEN_COLS; c++) {
            int ax = alien_px(c);
            int ay = alien_py(r);
            int type = row_to_type(r);

            if (aliens[r][c].explode > 0) {
                draw_explosion(gx, ax, ay, ALIEN_SW, ALIEN_SH);
                aliens[r][c].explode--;
            } else if (aliens[r][c].alive) {
                draw_alien_sprite(gx, ax, ay, type, g_anim_frame, ALIEN_COLS_[type]);
            }
        }
    }

    /* Player */
    if (player_dead_timer == 0) {
        draw_player(gx, player_x, PLAYER_Y);
    } else {
        /* Flicker during respawn */
        if ((player_dead_timer >> 2) & 1) {
            draw_player(gx, player_x, PLAYER_Y);
        }
        /* Explosion effect */
        if (player_dead_timer > 80) {
            g_circle(gx, player_x + PLAYER_W/2, PLAYER_Y + PLAYER_H/2,
                     (120 - player_dead_timer) / 3 + 4, 0xFFFF4400);
        }
    }

    /* Player bullets */
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++) {
        if (!pbullets[i].active) continue;
        g_fill_rect(gx, pbullets[i].x, pbullets[i].y,
                    PLAYER_BULLET_W, PLAYER_BULLET_H, COL_PBULLET);
        /* Glow tip */
        g_fill_rect(gx, pbullets[i].x - 1, pbullets[i].y,
                    PLAYER_BULLET_W + 2, 2, 0xFFFFFF99);
    }

    /* Alien bullets */
    for (int i = 0; i < MAX_ALIEN_BULLETS; i++) {
        if (!abullets[i].active) continue;
        /* Jagged bolt */
        g_fill_rect(gx, abullets[i].x, abullets[i].y,
                    ALIEN_BULLET_W, ALIEN_BULLET_H, COL_ABULLET);
        g_fill_rect(gx, abullets[i].x + 1, abullets[i].y + 2,
                    2, ALIEN_BULLET_H - 2, 0xFFFF8888);
    }

    /* Overlays */
    if (g_istate == ISTATE_WIN_WAVE) {
        g_fill_rect(gx, 0, WIN_H/2 - 40, WIN_W, 80, 0xCC101030);
        g_text_center(gx, WIN_W/2, WIN_H/2 - 12, "WAVE CLEAR!", COL_WIN);
        g_text_center(gx, WIN_W/2, WIN_H/2 + 8,  "Prepare for next wave...", COL_TEXT);
    }

    if (g_istate == ISTATE_GAME_OVER) {
        g_fill_rect(gx, 0, WIN_H/2 - 56, WIN_W, 112, 0xCC100010);
        g_text_center(gx, WIN_W/2, WIN_H/2 - 26, "GAME OVER", COL_GAMEOVER);
        {
            char buf[32];
            int n = 0;
            const char *sl = "SCORE: ";
            while (sl[n]) buf[n++] = sl[(unsigned)n];
            /* Use g_itoa to append */
            char nb[14]; g_itoa(score, nb);
            for (int j = 0; nb[j]; j++) buf[n++] = nb[j];
            buf[n] = '\0';
            g_text_center(gx, WIN_W/2, WIN_H/2 - 6, buf, COL_SCORE_VAL);
        }
        g_text_center(gx, WIN_W/2, WIN_H/2 + 18, "Press ENTER to play again", COL_TEXT);
    }
}

/* =========================================================================
 * Entry point.
 * ========================================================================= */
void _start(void)
{
    serial_write("[INVADERS] starting\n");

    game_t *gx = game_open(WIN_W, WIN_H, "Invaders");
    if (!gx) {
        for (;;) { /* fatal */ }
    }

    init_stars();
    game_reset(gx);

    while (game_frame_begin(gx)) {

        /* --- Input --- */
        if (g_istate == ISTATE_PLAY && player_dead_timer == 0) {
            if (game_key_down(gx, KEY_LEFT))  player_x -= PLAYER_SPEED;
            if (game_key_down(gx, KEY_RIGHT)) player_x += PLAYER_SPEED;
            player_x = g_clamp(player_x, PLAYER_XMIN, PLAYER_XMAX);

            /* Fire player bullet. */
            if (game_key_pressed(gx, KEY_SPACE)) {
                for (int i = 0; i < MAX_PLAYER_BULLETS; i++) {
                    if (!pbullets[i].active) {
                        pbullets[i].active = 1;
                        pbullets[i].x = player_x + PLAYER_W/2 - PLAYER_BULLET_W/2;
                        pbullets[i].y = PLAYER_Y - PLAYER_BULLET_H;
                        g_beep(700, 30);
                        break;
                    }
                }
            }
        }

        /* Restart on Enter after game over. */
        if (g_istate == ISTATE_GAME_OVER && game_key_pressed(gx, KEY_ENTER)) {
            serial_write("[INVADERS] score ");
            serial_int(score);
            serial_write("\n");
            game_reset(gx);
        }

        /* --- Update --- */
        if (g_istate == ISTATE_PLAY) {
            /* Player dead timer. */
            if (player_dead_timer > 0) {
                player_dead_timer--;
                if (player_dead_timer == 0) {
                    /* Re-centre player after death. */
                    player_x = WIN_W / 2 - PLAYER_W / 2;
                }
            }

            /* March. */
            update_march();

            /* Alien fire. */
            if (alien_count > 0) {
                fire_timer--;
                if (fire_timer <= 0) {
                    fire_timer = fire_period;
                    alien_fire();
                }
            }

            /* Bullets. */
            update_player_bullets();
            update_alien_bullets(gx);

            /* UFO. */
            update_ufo();

            /* Invasion check. */
            check_alien_invasion();

            /* Wave clear. */
            if (alien_count == 0 && g_istate == ISTATE_PLAY) {
                g_istate    = ISTATE_WIN_WAVE;
                state_timer = 120;
                g_beep(880, 200);
            }
        } else if (g_istate == ISTATE_WIN_WAVE) {
            state_timer--;
            if (state_timer <= 0) {
                wave++;
                shields_reset();
                wave_start();
                g_istate = ISTATE_PLAY;
                g_beep(440, 100);
            }
        } else if (g_istate == ISTATE_GAME_OVER) {
            /* Wait for Enter (handled above). */
        }

        /* --- Render --- */
        render(gx);
        game_present(gx);
        game_sync(gx);
    }
}
