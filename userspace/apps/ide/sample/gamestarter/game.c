/*
 * game.c -- "Block Catcher" game template (freestanding, ring 3).
 * ===============================================================
 *
 * TEACHING TEMPLATE: read the comments tagged "// CHANGE:" to see
 * exactly what to tweak to make the game your own.
 *
 * What it does:
 *   - A paddle at the bottom of the screen moves left/right with arrow keys.
 *   - Blocks fall from random X positions at the top of the screen.
 *   - Catch a block -> score goes up, blocks fall faster.
 *   - Miss a block  -> lives down; 0 lives = GAME OVER.
 *   - Press R on the game-over screen to restart.
 *   - Press ESC to quit.
 *
 * Window: 400 x 480 pixels, ARGB32.
 * HUD   : 24 pixels at the top (score + lives).
 * Play  : 400 x 456 pixels below the HUD.
 *
 * Wl calls used:
 *   wl_connect()           -- open compositor channel
 *   wl_create_window()     -- allocate shm framebuffer + create window
 *   wl_poll_event()        -- drain input events (non-blocking)
 *   wl_commit()            -- push current framebuffer to screen
 *
 * Draw calls:
 *   fill_rect()            -- solid ARGB32 rectangle
 *   font_draw_string()     -- 8x16 bitfont text
 *   font_text_width()      -- measure string width in pixels
 *
 * Timing:
 *   SYS_GET_TICKS_MS (40)  -- millisecond wall clock
 *   SYS_YIELD        (15)  -- give CPU back between frames
 *
 * Build (copy-paste these commands):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/ide/sample/gamestarter/game.c  -o /tmp/game.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c  -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c  -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/gamestarter.elf
 *   objdump -d /tmp/gamestarter.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [GAME] starting
 *   [GAME] game over score N
 */

/* Pull in the freestanding wl client and bitmap font.
 * Paths are relative to this file's location:
 *   userspace/apps/ide/sample/gamestarter/ -> up 4 dirs -> userspace/
 * CHANGE: if you copy this file elsewhere, update these four levels. */
#include "../../../../lib/wl/wl_client.h"
#include "../../../../lib/font/bitfont.h"

/* =========================================================================
 * Syscall numbers.  These are AOS numbers -- NOT Linux!
 * ======================================================================= */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

/* =========================================================================
 * Key codes from kernel/include/input.h.
 * ======================================================================= */
#define KEY_ESC     1
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_R      19

/* =========================================================================
 * Freestanding types -- no libc, no stdint.h.
 * ======================================================================= */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;

/* =========================================================================
 * Inline 6-argument syscall helper (x86-64 sysv ABI).
 * ======================================================================= */
static inline long sc(long n,long a1,long a2,long a3,long a4,long a5,long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8),"r"(r9)
                 : "rcx","r11","memory");
    return r;
}

/* =========================================================================
 * Tiny serial print helpers (no printf, no libc).
 * ======================================================================= */
static u64 k_strlen(const char *s){ u64 n=0; while(s[n]) n++; return n; }
static void print(const char *m){ sc(SYS_WRITE,1,(long)m,(long)k_strlen(m),0,0,0); }

/* Format unsigned int into buf; return char count. */
static i32 fmt_u32(char *buf, u32 v)
{
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    i32 i=0; char tmp[12];
    while (v>0){ tmp[i++]=(char)('0'+(v%10)); v/=10; }
    for (i32 j=0;j<i;j++) buf[j]=tmp[i-1-j];
    buf[i]='\0'; return i;
}

/* =========================================================================
 * Window / layout constants.
 * CHANGE: resize the window here.  Play area = WIN_H - HUD_H.
 * ======================================================================= */
#define WIN_W  400
#define WIN_H  480
#define HUD_H   24    /* pixels reserved for score / lives bar at top */

/* =========================================================================
 * Colour palette (ARGB32 -- 0xFF = fully opaque).
 * CHANGE: swap any colour to retheme the whole game instantly.
 * ======================================================================= */
#define COL_BG        0xFF0D1117   /* near-black background             */
#define COL_HUD       0xFF161B22   /* slightly lighter HUD strip        */
#define COL_PADDLE    0xFF58A6FF   /* bright blue paddle                */
#define COL_BLOCK     0xFFF0883E   /* orange falling block              */
#define COL_BLOCK2    0xFFD2A8FF   /* purple block (harder, faster)     */
#define COL_MISS      0xFFFF4D6D   /* red flash when a block is missed  */
#define COL_TEXT      0xFFFFFFFF   /* white HUD text                    */
#define COL_SCORE_CLR 0xFFFFA657   /* amber score number                */
#define COL_LIVES_CLR 0xFFFF7B72   /* red lives number                  */
#define COL_OVERLAY   0xCC000000   /* darkening overlay on game-over    */

/* =========================================================================
 * Game entity sizes.
 * CHANGE: make the paddle wider for easy mode, narrower for hard.
 * ======================================================================= */
#define PADDLE_W    72    /* paddle width in pixels  -- CHANGE for difficulty */
#define PADDLE_H    12    /* paddle height in pixels */
#define PADDLE_Y   (WIN_H - 32)    /* vertical position from top        */
#define PADDLE_SPD  6     /* pixels per frame the paddle moves           */

#define BLOCK_W     28    /* falling block width  */
#define BLOCK_H     18    /* falling block height */

/* Maximum number of blocks on screen at once.
 * CHANGE: raise MAX_BLOCKS to make it hectic. */
#define MAX_BLOCKS   6

/* Starting number of lives.
 * CHANGE: set to 1 for one-hit mode, 5 for casual. */
#define START_LIVES  3

/* Base drop speed (pixels per game tick).
 * CHANGE: raise BASE_SPEED to start harder. */
#define BASE_SPEED   2

/* How often (in ms) we spawn a new block.
 * CHANGE: lower = more blocks. */
#define SPAWN_MS   900

/* =========================================================================
 * LCG pseudo-random (no stdlib rand).
 * ======================================================================= */
static u32 g_rng = 12345u;
static u32 lcg(void){ g_rng = g_rng*1664525u+1013904223u; return g_rng; }

/* =========================================================================
 * Game state.
 * ======================================================================= */
typedef struct {
    i32 x, y;      /* pixel position (top-left corner) */
    i32 speed;     /* pixels to move down each game tick */
    i32 active;    /* 1 = on screen, 0 = slot free */
    u32 color;     /* colour (orange normal, purple hard) */
} block_t;

static block_t g_blocks[MAX_BLOCKS];
static i32     g_paddle_x;       /* left edge of paddle */
static u32     g_score;
static i32     g_lives;
static i32     g_game_over;
static i32     g_key_left;       /* 1 while left arrow held */
static i32     g_key_right;      /* 1 while right arrow held */
static i32     g_miss_flash;     /* countdown frames for red flash */

/* =========================================================================
 * Pixel drawing helpers (operate directly on the wl framebuffer).
 * ======================================================================= */

/* fill_rect: draw a solid colour rectangle.
 * stride_px = win->stride / 4 (pixels per row, not bytes). */
static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    /* Clip to window bounds so out-of-screen draws are safe. */
    i32 x1 = x<0 ? 0 : x;
    i32 y1 = y<0 ? 0 : y;
    i32 x2 = x+w; if(x2>(i32)WIN_W) x2=WIN_W;
    i32 y2 = y+h; if(y2>(i32)WIN_H) y2=WIN_H;
    if(x1>=x2 || y1>=y2) return;
    for(i32 yy=y1; yy<y2; yy++){
        u32 *row = buf + (u32)yy * stride_px;
        for(i32 xx=x1; xx<x2; xx++) row[xx] = color;
    }
}

/* draw_block_3d: block with a 2px lighter top/left bevel -- feels "solid". */
static void draw_block_3d(u32 *buf, u32 stride_px,
                          i32 x, i32 y, i32 w, i32 h, u32 col)
{
    fill_rect(buf, stride_px, x, y, w, h, col);
    /* Lighter top edge (add ~0x40 to each channel -- simple approximation). */
    u32 bright = col | 0x404040u;
    fill_rect(buf, stride_px, x, y, w, 2, bright);
    fill_rect(buf, stride_px, x, y, 2, h, bright);
    /* Darker bottom/right edge. */
    u32 dark = (col >> 1) & 0x007F7F7Fu | 0xFF000000u;
    fill_rect(buf, stride_px, x, y+h-2, w, 2, dark);
    fill_rect(buf, stride_px, x+w-2, y, 2, h, dark);
}

/* =========================================================================
 * Game initialisation / reset.
 * CHANGE: add power-ups, extra block types, or initial score multiplier.
 * ======================================================================= */
static void game_init(u64 ticks)
{
    /* Seed RNG from startup time so every game is different. */
    g_rng = (u32)(ticks ^ (ticks >> 13));
    if(g_rng == 0) g_rng = 0xC0FFEE;

    /* Clear block pool. */
    for(i32 i=0; i<MAX_BLOCKS; i++) g_blocks[i].active = 0;

    /* Paddle starts centered. */
    g_paddle_x = (WIN_W - PADDLE_W) / 2;
    g_score     = 0;
    g_lives     = START_LIVES;
    g_game_over = 0;
    g_key_left  = 0;
    g_key_right = 0;
    g_miss_flash = 0;
}

/* =========================================================================
 * Find an unused block slot; returns -1 if pool is full.
 * ======================================================================= */
static i32 free_slot(void)
{
    for(i32 i=0; i<MAX_BLOCKS; i++)
        if(!g_blocks[i].active) return i;
    return -1;
}

/* =========================================================================
 * Spawn a new block at a random X position.
 * CHANGE: add patterns (e.g. columns, waves) by changing fx logic.
 * ======================================================================= */
static void spawn_block(void)
{
    i32 s = free_slot();
    if(s < 0) return;   /* pool full -- skip this spawn */

    i32 fx = (i32)(lcg() % (u32)(WIN_W - BLOCK_W));
    if(fx < 0) fx = 0;

    /* Every ~4th block is harder (purple, faster).
     * CHANGE: make this condition more frequent for harder games. */
    i32 hard = ((lcg() % 4u) == 0);

    /* Block speed grows with score (faster as you get better).
     * CHANGE: raise divisor (e.g. /4) to slow the scaling. */
    i32 spd = BASE_SPEED + (i32)(g_score / 3);
    if(hard) spd++;                    /* hard blocks are 1px/tick faster */
    if(spd > 14) spd = 14;            /* cap so it stays catchable        */

    g_blocks[s].x      = fx;
    g_blocks[s].y      = HUD_H;       /* start just below the HUD         */
    g_blocks[s].speed  = spd;
    g_blocks[s].active = 1;
    g_blocks[s].color  = hard ? COL_BLOCK2 : COL_BLOCK;
}

/* =========================================================================
 * Move paddle in response to currently-held keys.
 * ======================================================================= */
static void update_paddle(void)
{
    if(g_key_left){
        g_paddle_x -= PADDLE_SPD;
        if(g_paddle_x < 0) g_paddle_x = 0;
    }
    if(g_key_right){
        g_paddle_x += PADDLE_SPD;
        if(g_paddle_x + PADDLE_W > WIN_W) g_paddle_x = WIN_W - PADDLE_W;
    }
}

/* =========================================================================
 * Axis-aligned rectangle overlap test.
 * ======================================================================= */
static i32 rects_overlap(i32 ax,i32 ay,i32 aw,i32 ah,
                          i32 bx,i32 by,i32 bw,i32 bh)
{
    return ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by;
}

/* =========================================================================
 * Advance all blocks one game tick.
 * ======================================================================= */
static void update_blocks(void)
{
    for(i32 i=0; i<MAX_BLOCKS; i++){
        if(!g_blocks[i].active) continue;

        g_blocks[i].y += g_blocks[i].speed;

        /* Did the block reach the paddle zone? */
        if(rects_overlap(g_blocks[i].x, g_blocks[i].y, BLOCK_W, BLOCK_H,
                         g_paddle_x,    PADDLE_Y,       PADDLE_W, PADDLE_H))
        {
            /* Caught! */
            g_score++;
            g_blocks[i].active = 0;
            continue;
            /* CHANGE: add bonus score for hard blocks, or sound effects here. */
        }

        /* Fell past the bottom of the screen? */
        if(g_blocks[i].y > WIN_H){
            g_blocks[i].active = 0;
            g_lives--;
            g_miss_flash = 8;            /* 8 frames of red flash */
            if(g_lives <= 0){
                g_lives     = 0;
                g_game_over = 1;
            }
        }
    }
}

/* =========================================================================
 * Render the full frame into the wl pixel buffer.
 * ======================================================================= */
static void render(u32 *buf, u32 stride_px)
{
    /* ---- 1. Background ---- */
    u32 bg = (g_miss_flash > 0) ? COL_MISS : COL_BG;
    fill_rect(buf, stride_px, 0, HUD_H, WIN_W, WIN_H - HUD_H, bg);
    if(g_miss_flash > 0) g_miss_flash--;

    /* ---- 2. HUD strip ---- */
    fill_rect(buf, stride_px, 0, 0, WIN_W, HUD_H, COL_HUD);

    /* Build "SCORE: NNN  LIVES: N" string for the HUD.
     * We do this without sprintf -- manual number formatting. */
    {
        char hud[48];
        i32  n = 0;

        /* "SCORE: " */
        const char *sp = "SCORE: ";
        for(i32 j=0; sp[j]; j++) hud[n++] = sp[j];
        char num[12];
        i32 nl = fmt_u32(num, g_score);
        for(i32 j=0; j<nl; j++) hud[n++] = num[j];

        /* "  LIVES: " */
        const char *lp = "  LIVES: ";
        for(i32 j=0; lp[j]; j++) hud[n++] = lp[j];
        nl = fmt_u32(num, (u32)(g_lives < 0 ? 0 : g_lives));
        for(i32 j=0; j<nl; j++) hud[n++] = num[j];
        hud[n] = '\0';

        font_draw_string(buf, (i32)stride_px, WIN_W, WIN_H,
                         4, (HUD_H - FONT_H) / 2 + 1, hud, COL_TEXT);
    }

    /* ---- 3. Falling blocks ---- */
    for(i32 i=0; i<MAX_BLOCKS; i++){
        if(!g_blocks[i].active) continue;
        draw_block_3d(buf, stride_px,
                      g_blocks[i].x, g_blocks[i].y,
                      BLOCK_W, BLOCK_H, g_blocks[i].color);
    }

    /* ---- 4. Paddle ---- */
    /* CHANGE: replace draw_block_3d with your own paddle art here. */
    draw_block_3d(buf, stride_px,
                  g_paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, COL_PADDLE);

    /* ---- 5. Game-over overlay ---- */
    if(g_game_over){
        /* Darken the play area with a row-skip stipple (no real alpha). */
        for(i32 yy=HUD_H; yy<WIN_H; yy+=2)
            fill_rect(buf, stride_px, 0, yy, WIN_W, 1, COL_OVERLAY);

        /* Centre each message line. */
        const char *msg1 = "GAME OVER";
        const char *msg2 = "PRESS R TO RESTART";
        const char *msg3 = "ESC TO QUIT";

        char score_line[24];
        i32 sn = 0;
        const char *sp2 = "SCORE: ";
        for(i32 j=0; sp2[j]; j++) score_line[sn++] = sp2[j];
        char snum[12];
        i32 snl = fmt_u32(snum, g_score);
        for(i32 j=0; j<snl; j++) score_line[sn++] = snum[j];
        score_line[sn] = '\0';

        i32 cy = WIN_H / 2;

        font_draw_string(buf,(i32)stride_px,WIN_W,WIN_H,
            (WIN_W - font_text_width(msg1))/2, cy-28, msg1, COL_MISS);
        font_draw_string(buf,(i32)stride_px,WIN_W,WIN_H,
            (WIN_W - font_text_width(score_line))/2, cy-8, score_line,
            COL_SCORE_CLR);
        font_draw_string(buf,(i32)stride_px,WIN_W,WIN_H,
            (WIN_W - font_text_width(msg2))/2, cy+12, msg2, COL_TEXT);
        font_draw_string(buf,(i32)stride_px,WIN_W,WIN_H,
            (WIN_W - font_text_width(msg3))/2, cy+28, msg3, COL_TEXT);
    }
}

/* =========================================================================
 * _start -- entry point (replaces main(), no CRT).
 * ======================================================================= */
void _start(void)
{
    print("[GAME] starting\n");

    /* Open the compositor channel. */
    if(wl_connect() != 0){
        print("[GAME] wl_connect FAILED\n");
        for(;;) sc(SYS_YIELD,0,0,0,0,0,0);
    }

    /* Create a 400x480 window titled "Block Catcher".
     * CHANGE: the title string shown in the window chrome. */
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Block Catcher");
    if(!win){
        print("[GAME] wl_create_window FAILED\n");
        for(;;) sc(SYS_YIELD,0,0,0,0,0,0);
    }

    /* stride is bytes per row; divide by 4 to get pixels per row. */
    u32 stride_px = win->stride / 4u;

    /* Seed and initialise game state from current time. */
    u64 now = (u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);
    game_init(now);

    u64 last_spawn = now;     /* when we last dropped a block */
    u64 last_tick  = now;     /* when we last ran a game tick */

    /* -----------------------------------------------------------------------
     * MAIN GAME LOOP.
     * Runs at roughly 60 fps (yield after each frame).
     * Game logic ticks at TICK_MS intervals, independent of render rate.
     * CHANGE: TICK_MS controls game physics speed.
     * --------------------------------------------------------------------- */
    #define TICK_MS 16u    /* ~60 physics ticks per second */

    for(;;){
        u64 t = (u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);

        /* ---- Drain input events (non-blocking) ---- */
        int kind, a, b, c_ev;
        while(wl_poll_event(win, &kind, &a, &b, &c_ev)){
            if(kind == WL_EVENT_KEY){
                /* b == 1 means key pressed, b == 0 means key released. */
                if(a == KEY_ESC && b == 1)
                    sc(SYS_EXIT,0,0,0,0,0,0);

                if(a == KEY_R && b == 1 && g_game_over){
                    /* Restart: re-seed from current time for variety. */
                    now = (u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);
                    game_init(now);
                    last_spawn = now;
                    last_tick  = now;
                }

                /* Track held state for smooth paddle movement. */
                if(a == KEY_LEFT)  g_key_left  = b;
                if(a == KEY_RIGHT) g_key_right = b;
                /* CHANGE: also handle KEY_A / KEY_D for WASD layout. */
            }
        }

        /* ---- Game logic at fixed tick rate ---- */
        if(!g_game_over && (t - last_tick) >= TICK_MS){
            last_tick = t;
            update_paddle();
            update_blocks();
        }

        /* ---- Spawn new blocks on a timer ---- */
        if(!g_game_over && (t - last_spawn) >= SPAWN_MS){
            last_spawn = t;
            spawn_block();
        }

        /* ---- Render the current state ---- */
        render(win->pixels, stride_px);
        wl_commit(win);

        /* Yield the CPU so the compositor and other processes get time. */
        sc(SYS_YIELD,0,0,0,0,0,0);
    }
}
