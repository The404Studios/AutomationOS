/*
 * platformer.c -- Side-scrolling platformer (freestanding, ring 3).
 * ==================================================================
 *
 * Window: 720x440 "Platformer"
 * 3 levels, tile-based, parallax background, particle dust, SFX.
 *
 * Physics (fixed-point, 8 fractional bits = 256 sub-pixels):
 *   Gravity:    +22 sub-px/frame  (~0.086 px/ms at 60fps)
 *   Max fall:   +320 sub-px/frame
 *   Jump vel:   -320 sub-px/frame (held up to 12 frames for variable height)
 *   Accel:      +28 sub-px/frame  horizontal
 *   Friction:   *210/256 per frame (grounded), *248/256 (airborne)
 *   Max run:    +/-256 sub-px/frame (~1 px/frame @ 60fps)
 *
 * Tile format: each level is a 2D u8 array [LEVEL_ROWS][LEVEL_COLS].
 *   0 = air
 *   1 = solid ground
 *   2 = platform (solid only from above)
 *   3 = spike (hazard)
 *   4 = coin
 *   5 = goal flag
 *
 * Camera: smooth follow with left-edge clamping.
 * Coyote time: 8 frames. Jump buffer: 8 frames.
 * Controls: Left/A, Right/D, Space/Up = jump.
 * 3 lives. Enter = restart level / title.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace/lib/game \
 *       -c userspace/apps/platformer/platformer.c -o /tmp/platformer.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/game/game.c   -o /tmp/game.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c  -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/platformer.o /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/platformer.elf
 *   objdump -d /tmp/platformer.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/game/game.h"

/* =========================================================================
 * Window dimensions
 * ========================================================================= */
#define WIN_W   720
#define WIN_H   440
#define HUD_H   28

/* Play area height (below HUD) */
#define PLAY_H  (WIN_H - HUD_H)

/* =========================================================================
 * Tile system
 * ========================================================================= */
#define TILE_W      20
#define TILE_H      20
#define LEVEL_ROWS  22   /* tiles tall */
#define LEVEL_COLS  60   /* tiles wide per level (levels 2 & 3 use all 60) */

/* Tile type IDs */
#define T_AIR   0
#define T_SOLID 1
#define T_PLAT  2
#define T_SPIKE 3
#define T_COIN  4
#define T_FLAG  5

/* Tile colors */
#define COL_SKY_TOP   0xFF1a1a3e
#define COL_SKY_BOT   0xFF2d2d5e
#define COL_GROUND    0xFF5a3e28
#define COL_GROUND_T  0xFF7a5a38
#define COL_PLAT      0xFF4a6a3a
#define COL_PLAT_T    0xFF6a9a5a
#define COL_SPIKE     0xFFc0c0c0
#define COL_COIN      0xFFffd700
#define COL_FLAG_P    0xFFcccccc
#define COL_FLAG_F    0xFFff4444
#define COL_BG1       0xFF252550
#define COL_BG2       0xFF1e1e42

/* =========================================================================
 * Level data (tile maps)
 * G=ground/solid, P=platform, S=spike, C=coin, F=flag, ' '=air
 * Encoded as u8 arrays (row-major, top to bottom).
 * ========================================================================= */

/* LEVEL 1 - 36 columns */
#define L1_COLS 36
#define L1_ROWS 22

static const u8 level1[L1_ROWS][LEVEL_COLS] = {
/* row  0 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  1 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  2 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  3 */ {0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  4 */ {0,0,0,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  5 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  6 */ {0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  7 */ {2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  8 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  9 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 10 */ {0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 11 */ {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 12 */ {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 13 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 14 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
/* row 15 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
/* row 16 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,5},
/* row 17 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 18 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 19 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 20 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 21 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* LEVEL 2 - cave / underground feel */
static const u8 level2[L1_ROWS][LEVEL_COLS] = {
/* row  0 */ {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
/* row  1 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row  2 */ {1,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row  3 */ {1,0,0,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row  4 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,4,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row  5 */ {1,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row  6 */ {1,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row  7 */ {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row  8 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0,1},
/* row  9 */ {1,0,0,0,3,3,3,0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,0,0,0,0,0,0,0,1},
/* row 10 */ {1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 11 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 12 */ {1,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,5,0,0,0,1},
/* row 13 */ {1,0,2,2,2,2,2,2,2,0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1},
/* row 14 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 15 */ {1,0,0,0,3,3,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 16 */ {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 17 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 18 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 19 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 20 */ {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
/* row 21 */ {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

/* LEVEL 3 - sky/cloud theme, more vertical challenge */
static const u8 level3[L1_ROWS][LEVEL_COLS] = {
/* row  0 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  1 */ {0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  2 */ {0,0,0,0,0,0,2,2,2,2,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  3 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  4 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  5 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  6 */ {2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  7 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,4,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  8 */ {0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row  9 */ {1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 10 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 11 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0},
/* row 12 */ {0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0},
/* row 13 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 14 */ {0,0,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,5,0,0,0,0},
/* row 15 */ {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
/* row 16 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 17 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 18 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 19 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 20 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
/* row 21 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* Pointer table for levels */
static const u8 (*g_levels[3])[LEVEL_COLS] = {
    level1, level2, level3
};

/* Mutable coin/flag state overlay: 1 = collected/done */
static u8 g_tile_state[L1_ROWS][LEVEL_COLS];

/* =========================================================================
 * Physics constants (fixed-point: 8 fractional bits, 1 FP = 1/256 pixel)
 * ========================================================================= */
#define FP              256       /* 1.0 in fixed-point */
#define GRAVITY         22        /* sub-px/frame gravity acceleration */
#define MAX_FALL        (FP * 5)  /* terminal velocity */
#define JUMP_VEL        (-FP * 5) /* initial jump velocity */
#define JUMP_HOLD_EXTRA (-FP / 3) /* extra upward vel per frame while held */
#define MAX_JUMP_HOLD   10        /* frames of extra jump force */
#define ACCEL           28        /* horizontal acceleration */
#define MAX_SPEED_X     (FP + FP/2) /* max horizontal speed */
#define FRIC_GROUND_N   210       /* friction numerator (out of 256) */
#define FRIC_AIR_N      248

#define COYOTE_FRAMES   8
#define JUMP_BUFFER_FRAMES 8

/* =========================================================================
 * Player
 * ========================================================================= */
#define PLAYER_W    14
#define PLAYER_H    20

typedef struct {
    int px, py;         /* position in sub-pixels (fixed-point) */
    int vx, vy;         /* velocity in sub-pixels/frame */
    int on_ground;      /* boolean */
    int coyote;         /* coyote-time frames remaining */
    int jump_buffer;    /* jump buffer frames remaining */
    int jump_hold;      /* frames left for variable jump height */
    int is_jumping;     /* currently in a held jump */
    int facing;         /* 1=right, -1=left */
    int respawn_x;      /* sub-pixel respawn X */
    int respawn_y;      /* sub-pixel respawn Y */
    int dead;           /* currently in death flash */
    int dead_timer;     /* frames remaining in death flash */
    int landed_prev;    /* was on ground last frame (for dust trigger) */
} player_t;

/* =========================================================================
 * Particles
 * ========================================================================= */
#define MAX_PARTICLES 64

typedef struct {
    int  x, y;       /* pixel coords */
    int  vx, vy;     /* sub-pixel velocity */
    int  life;       /* frames remaining */
    int  max_life;
    u32  color;
} particle_t;

static particle_t g_particles[MAX_PARTICLES];
static int        g_num_particles;

static void spawn_dust(int px_world, int py_world)
{
    for (int i = 0; i < 5; i++) {
        if (g_num_particles >= MAX_PARTICLES) break;
        particle_t *p = &g_particles[g_num_particles++];
        p->x    = px_world;
        p->y    = py_world;
        p->vx   = g_rand_range(200) - 100;  /* -100..+99 sub-px/frame */
        p->vy   = -(g_rand_range(60) + 20); /* upward */
        p->life = p->max_life = 12 + g_rand_range(8);
        p->color = 0xFFd0c090;
    }
}

static void spawn_coin_burst(int px_world, int py_world)
{
    for (int i = 0; i < 6; i++) {
        if (g_num_particles >= MAX_PARTICLES) break;
        particle_t *p = &g_particles[g_num_particles++];
        p->x    = px_world;
        p->y    = py_world;
        p->vx   = g_rand_range(300) - 150;
        p->vy   = -(g_rand_range(80) + 30);
        p->life = p->max_life = 20 + g_rand_range(10);
        p->color = 0xFFffd700;
    }
}

static void update_particles(void)
{
    int i = 0;
    while (i < g_num_particles) {
        particle_t *p = &g_particles[i];
        p->x  += p->vx >> 7;
        p->y  += p->vy >> 7;
        p->vy += 8;  /* gravity on particles */
        p->life--;
        if (p->life <= 0) {
            /* Remove by swapping with last */
            g_particles[i] = g_particles[--g_num_particles];
        } else {
            i++;
        }
    }
}

/* =========================================================================
 * Game state
 * ========================================================================= */
#define STATE_TITLE  0
#define STATE_PLAY   1
#define STATE_DEAD   2
#define STATE_WIN    3
#define STATE_GAMEOVER 4

static int       g_state;
static int       g_level;        /* 0-based */
static int       g_lives;
static int       g_score;
static player_t  g_player;
static int       g_cam_x;        /* camera X in pixels (world space) */
static int       g_win_timer;    /* countdown for win flash */
static int       g_title_timer;

/* =========================================================================
 * Tile helpers
 * ========================================================================= */
static u8 get_tile(int col, int row)
{
    if (col < 0 || col >= LEVEL_COLS || row < 0 || row >= LEVEL_ROWS)
        return T_SOLID; /* out of bounds = solid wall */
    return g_levels[g_level][row][col];
}

static int tile_solid(u8 t)  { return t == T_SOLID; }
static int tile_plat(u8 t)   { return t == T_PLAT; }
static int tile_hazard(u8 t) { return t == T_SPIKE; }
static int tile_coin(u8 t)   { return t == T_COIN; }
static int tile_flag(u8 t)   { return t == T_FLAG; }

/* =========================================================================
 * Spawn point detection
 * ========================================================================= */
static void find_spawn(int *out_px, int *out_py)
{
    /* Default: top-left area */
    *out_px = 2 * TILE_W * FP;
    *out_py = (LEVEL_ROWS - 4) * TILE_H * FP;
    /* Search for an explicit start: first open ground tile */
    for (int row = LEVEL_ROWS - 2; row >= 1; row--) {
        for (int col = 0; col < LEVEL_COLS; col++) {
            u8 below = get_tile(col, row + 1);
            u8 here  = get_tile(col, row);
            u8 above = get_tile(col, row - 1);
            if (here == T_AIR && above == T_AIR &&
                (tile_solid(below) || tile_plat(below))) {
                *out_px = col * TILE_W * FP;
                *out_py = row * TILE_H * FP;
                return;
            }
        }
    }
}

/* =========================================================================
 * Level initialisation
 * ========================================================================= */
static void level_init(void)
{
    /* Clear coin/flag collected state */
    for (int r = 0; r < L1_ROWS; r++)
        for (int c = 0; c < LEVEL_COLS; c++)
            g_tile_state[r][c] = 0;

    g_num_particles = 0;
    g_cam_x = 0;

    /* Place player at spawn */
    int sx, sy;
    find_spawn(&sx, &sy);

    player_t *pl = &g_player;
    pl->px          = sx;
    pl->py          = sy;
    pl->vx          = 0;
    pl->vy          = 0;
    pl->on_ground   = 0;
    pl->coyote      = 0;
    pl->jump_buffer = 0;
    pl->jump_hold   = 0;
    pl->is_jumping  = 0;
    pl->facing      = 1;
    pl->respawn_x   = sx;
    pl->respawn_y   = sy;
    pl->dead        = 0;
    pl->dead_timer  = 0;
    pl->landed_prev = 0;
}

/* =========================================================================
 * Collision resolution (sweep AABB against tile grid)
 * ========================================================================= */

/* Returns 1 if the player AABB at (px,py) in pixels overlaps any solid/plat.
 * For platforms: only solid from above (vy >= 0, bottom edge crossing). */
static int player_px_to_tile_px(int fp_val) { return fp_val / FP; }

/* Resolve horizontal movement: move px by dx (pixels), push out of solids. */
static void resolve_x(player_t *pl, int dx)
{
    int px = player_px_to_tile_px(pl->px) + dx;
    int py = player_px_to_tile_px(pl->py);

    int left  = px;
    int right = px + PLAYER_W - 1;
    int top   = py;
    int bot   = py + PLAYER_H - 1;

    /* Check columns on the leading edge */
    int edge_x = (dx > 0) ? right : left;
    int tile_col = edge_x / TILE_W;

    int push = 0;
    for (int ty = top / TILE_H; ty <= bot / TILE_H; ty++) {
        u8 t = get_tile(tile_col, ty);
        if (tile_solid(t)) {
            if (dx > 0) {
                /* Push left: right edge to tile left boundary */
                int tile_left = tile_col * TILE_W;
                push = tile_left - (player_px_to_tile_px(pl->px) + PLAYER_W);
            } else {
                /* Push right */
                int tile_right = (tile_col + 1) * TILE_W;
                push = tile_right - player_px_to_tile_px(pl->px);
            }
            pl->vx = 0;
            break;
        }
    }
    pl->px += (dx + push) * FP;
}

/* Resolve vertical movement.  Returns 1 if landed this frame. */
static int resolve_y(player_t *pl, int dy)
{
    int px = player_px_to_tile_px(pl->px);
    int py = player_px_to_tile_px(pl->py) + dy;

    int left  = px;
    int right = px + PLAYER_W - 1;
    int top   = py;
    int bot   = py + PLAYER_H - 1;

    int landed = 0;

    if (dy > 0) {
        /* Moving down: check bottom edge */
        int tile_row = bot / TILE_H;
        for (int tx = left / TILE_W; tx <= right / TILE_W; tx++) {
            u8 t = get_tile(tx, tile_row);
            if (tile_solid(t)) {
                int tile_top = tile_row * TILE_H;
                int new_py   = tile_top - PLAYER_H;
                pl->py = new_py * FP;
                pl->vy = 0;
                landed = 1;
                break;
            }
            /* Platform: only if moving down and feet are at/near top */
            if (tile_plat(t)) {
                int tile_top   = tile_row * TILE_H;
                int prev_bot   = player_px_to_tile_px(pl->py) + PLAYER_H - 1;
                if (prev_bot <= tile_top + 1 && dy > 0) {
                    int new_py = tile_top - PLAYER_H;
                    pl->py = new_py * FP;
                    pl->vy = 0;
                    landed = 1;
                    break;
                }
            }
        }
    } else if (dy < 0) {
        /* Moving up: check top edge */
        int tile_row = top / TILE_H;
        for (int tx = left / TILE_W; tx <= right / TILE_W; tx++) {
            u8 t = get_tile(tx, tile_row);
            if (tile_solid(t)) {
                int tile_bot = (tile_row + 1) * TILE_H;
                pl->py = tile_bot * FP;
                pl->vy = 0;
                break;
            }
        }
    }

    if (!landed) {
        pl->py += dy * FP;
    }

    return landed;
}

/* =========================================================================
 * Physics update (one frame)
 * ========================================================================= */
static void physics_update(game_t *g, int dt)
{
    player_t *pl = &g_player;

    if (pl->dead) return;

    /* --- Horizontal input --- */
    int move_left  = game_key_down(g, KEY_LEFT)  || game_key_down(g, KEY_A);
    int move_right = game_key_down(g, KEY_RIGHT) || game_key_down(g, KEY_D);
    int jump_pressed = game_key_pressed(g, KEY_SPACE) || game_key_pressed(g, KEY_UP)
                    || game_key_pressed(g, KEY_W);
    int jump_held    = game_key_down(g, KEY_SPACE)    || game_key_down(g, KEY_UP)
                    || game_key_down(g, KEY_W);

    /* Horizontal acceleration */
    if (move_right) { pl->vx += ACCEL; pl->facing =  1; }
    if (move_left)  { pl->vx -= ACCEL; pl->facing = -1; }
    if (!move_left && !move_right) {
        /* Friction */
        int fric = pl->on_ground ? FRIC_GROUND_N : FRIC_AIR_N;
        pl->vx = (pl->vx * fric) / FP;
        if (pl->vx > -4 && pl->vx < 4) pl->vx = 0;
    }
    pl->vx = g_clamp(pl->vx, -MAX_SPEED_X, MAX_SPEED_X);

    /* --- Jump buffer --- */
    if (jump_pressed) pl->jump_buffer = JUMP_BUFFER_FRAMES;
    if (pl->jump_buffer > 0) pl->jump_buffer--;

    /* --- Coyote time --- */
    if (pl->on_ground) {
        pl->coyote = COYOTE_FRAMES;
    } else if (pl->coyote > 0) {
        pl->coyote--;
    }

    /* --- Initiate jump --- */
    if (pl->jump_buffer > 0 && pl->coyote > 0 && !pl->is_jumping) {
        pl->vy          = JUMP_VEL;
        pl->jump_hold   = MAX_JUMP_HOLD;
        pl->is_jumping  = 1;
        pl->jump_buffer = 0;
        pl->coyote      = 0;
        pl->on_ground   = 0;
        g_beep(440, 50);
    }

    /* --- Variable jump height (hold jump key) --- */
    if (pl->is_jumping && pl->jump_hold > 0 && jump_held && pl->vy < 0) {
        pl->vy += JUMP_HOLD_EXTRA;
        pl->jump_hold--;
    } else if (!jump_held || pl->vy >= 0) {
        pl->jump_hold  = 0;
        pl->is_jumping = 0;
    }

    /* --- Gravity --- */
    pl->vy += GRAVITY;
    if (pl->vy > MAX_FALL) pl->vy = MAX_FALL;

    /* --- Move & Collide --- */
    int prev_on_ground = pl->on_ground;
    pl->on_ground = 0;

    /* Apply velocity in sub-pixels, resolve in pixels */
    int dx_fp = pl->vx;
    int dy_fp = pl->vy;

    /* Step x */
    int dx_px = dx_fp / FP;
    if (dx_px != 0) {
        resolve_x(pl, dx_px);
    } else {
        pl->px += dx_fp;
        /* Snap to grid contribution */
    }

    /* Step y */
    int dy_px = dy_fp / FP;
    int landed = 0;
    if (dy_px != 0) {
        landed = resolve_y(pl, dy_px);
        if (landed) pl->on_ground = 1;
    } else {
        pl->py += dy_fp;
        /* Check if standing on something (sub-pixel hasn't moved a full pixel) */
        int bx  = player_px_to_tile_px(pl->px);
        int by  = player_px_to_tile_px(pl->py) + PLAYER_H;
        int tc  = bx / TILE_W;
        int tr  = by / TILE_H;
        for (int tx = tc; tx <= (bx + PLAYER_W - 1) / TILE_W; tx++) {
            u8 t = get_tile(tx, tr);
            if (tile_solid(t)) { pl->on_ground = 1; break; }
            if (tile_plat(t))  { pl->on_ground = 1; break; }
        }
    }

    /* Dust on landing */
    if (pl->on_ground && !prev_on_ground) {
        int world_px = player_px_to_tile_px(pl->px) + PLAYER_W / 2;
        int world_py = player_px_to_tile_px(pl->py) + PLAYER_H;
        spawn_dust(world_px, world_py);
        if (!pl->landed_prev)
            g_beep(120, 30);
    }
    pl->landed_prev = pl->on_ground;

    /* --- Tile interactions --- */
    int pcol_l = player_px_to_tile_px(pl->px) / TILE_W;
    int pcol_r = (player_px_to_tile_px(pl->px) + PLAYER_W - 1) / TILE_W;
    int prow_t = player_px_to_tile_px(pl->py) / TILE_H;
    int prow_b = (player_px_to_tile_px(pl->py) + PLAYER_H - 1) / TILE_H;

    for (int tr2 = prow_t; tr2 <= prow_b; tr2++) {
        for (int tc2 = pcol_l; tc2 <= pcol_r; tc2++) {
            if (tr2 < 0 || tr2 >= LEVEL_ROWS || tc2 < 0 || tc2 >= LEVEL_COLS) continue;
            u8 t = g_levels[g_level][tr2][tc2];
            if (g_tile_state[tr2][tc2]) continue; /* already consumed */

            if (tile_hazard(t)) {
                /* Hit spike */
                pl->dead = 1;
                pl->dead_timer = 30;
                g_lives--;
                g_beep(80, 200);
                return;
            }
            if (tile_coin(t)) {
                g_tile_state[tr2][tc2] = 1; /* collect */
                g_score += 10;
                spawn_coin_burst(tc2 * TILE_W + TILE_W/2, tr2 * TILE_H);
                g_beep(880, 60);
            }
            if (tile_flag(t)) {
                g_tile_state[tr2][tc2] = 1;
                g_state = STATE_WIN;
                g_win_timer = 90;
                g_beep(660, 80);
                g_beep(880, 80);
                return;
            }
        }
    }

    /* Pit death: fell below level */
    if (player_px_to_tile_px(pl->py) > LEVEL_ROWS * TILE_H + 40) {
        pl->dead = 1;
        pl->dead_timer = 30;
        g_lives--;
        g_beep(80, 200);
    }
}

/* =========================================================================
 * Camera
 * ========================================================================= */
static void update_camera(void)
{
    /* Target: center player horizontally in window */
    int world_px  = player_px_to_tile_px(g_player.px);
    int target_cx = world_px + PLAYER_W/2 - WIN_W/2;

    /* Max scroll */
    int max_cam   = LEVEL_COLS * TILE_W - WIN_W;
    if (max_cam < 0) max_cam = 0;

    target_cx = g_clamp(target_cx, 0, max_cam);

    /* Smooth follow: lerp by 1/8 */
    g_cam_x += (target_cx - g_cam_x) / 8;
    g_cam_x  = g_clamp(g_cam_x, 0, max_cam);
}

/* =========================================================================
 * Rendering helpers
 * ========================================================================= */

/* Draw tile at world position (tx,ty) in tile coords, offset by camera */
static void draw_tile(game_t *g, int col, int row, u8 tile, int state)
{
    int sx = col * TILE_W - g_cam_x;
    int sy = row * TILE_H + HUD_H;

    if (sx + TILE_W < 0 || sx >= WIN_W) return;  /* off screen */

    switch (tile) {
    case T_SOLID: {
        /* Check if top row (draw grass top) */
        u8 above = get_tile(col, row - 1);
        if (above == T_AIR || above == T_COIN || above == T_FLAG) {
            g_fill_rect(g, sx, sy, TILE_W, 4,           COL_GROUND_T);
            g_fill_rect(g, sx, sy + 4, TILE_W, TILE_H - 4, COL_GROUND);
        } else {
            g_fill_rect(g, sx, sy, TILE_W, TILE_H, COL_GROUND);
        }
        break;
    }
    case T_PLAT:
        g_fill_rect(g, sx, sy, TILE_W, 4,  COL_PLAT_T);
        g_fill_rect(g, sx, sy+4, TILE_W, 4, COL_PLAT);
        break;
    case T_SPIKE:
        if (!state) {
            /* Draw triangular spike */
            int mid = sx + TILE_W / 2;
            int bot = sy + TILE_H - 1;
            int tip = sy + 2;
            for (int py = tip; py <= bot; py++) {
                int half = ((py - tip) * (TILE_W/2)) / (bot - tip + 1);
                g_fill_rect(g, mid - half, py, half * 2, 1, COL_SPIKE);
            }
        }
        break;
    case T_COIN:
        if (!state) {
            g_circle(g, sx + TILE_W/2, sy + TILE_H/2, 5, COL_COIN);
            g_circle(g, sx + TILE_W/2, sy + TILE_H/2, 3, 0xFF886600);
        }
        break;
    case T_FLAG: {
        if (!state) {
            /* Pole */
            g_fill_rect(g, sx + TILE_W/2 - 1, sy + 2, 2, TILE_H - 4, COL_FLAG_P);
            /* Flag banner */
            g_fill_rect(g, sx + TILE_W/2 + 1, sy + 2, 8, 6, COL_FLAG_F);
        }
        break;
    }
    default: break;
    }
}

/* Draw the player character (primitive art) */
static void draw_player(game_t *g)
{
    player_t *pl = &g_player;
    int sx = player_px_to_tile_px(pl->px) - g_cam_x;
    int sy = player_px_to_tile_px(pl->py) + HUD_H;

    /* Death flash */
    if (pl->dead && (pl->dead_timer / 4) % 2) return;

    int f = pl->facing;  /* 1=right, -1=left */

    /* Body */
    u32 body_col = 0xFF2255cc;
    u32 head_col = 0xFFffcc88;
    u32 shoe_col = 0xFF442211;
    u32 eye_col  = 0xFF111111;

    /* Shoes */
    g_fill_rect(g, sx + 1, sy + PLAYER_H - 5, PLAYER_W - 2, 5, shoe_col);

    /* Body/shirt */
    g_fill_rect(g, sx + 2, sy + 9, PLAYER_W - 4, PLAYER_H - 14, body_col);

    /* Head */
    g_rounded_rect(g, sx + 2, sy, PLAYER_W - 4, 10, 3, head_col);

    /* Eye */
    int eye_x = (f > 0) ? sx + PLAYER_W - 6 : sx + 3;
    g_fill_rect(g, eye_x, sy + 3, 2, 2, eye_col);

    /* Hat */
    g_fill_rect(g, sx + 1, sy - 4, PLAYER_W - 2, 4, 0xFFcc2222);
    g_fill_rect(g, sx,     sy - 2, PLAYER_W,     2, 0xFFcc2222);

    /* Running legs animation: bobble when on ground & moving */
    int bob = 0;
    if (pl->on_ground && (pl->vx > 16 || pl->vx < -16)) {
        bob = (player_px_to_tile_px(pl->px) / 4) & 1;
    }
    if (bob) {
        g_fill_rect(g, sx + 2,          sy + PLAYER_H - 10, 4, 5, 0xFF334477);
        g_fill_rect(g, sx + PLAYER_W-6, sy + PLAYER_H - 8,  4, 3, 0xFF334477);
    } else {
        g_fill_rect(g, sx + 2,          sy + PLAYER_H - 9, 4, 4, 0xFF334477);
        g_fill_rect(g, sx + PLAYER_W-6, sy + PLAYER_H - 9, 4, 4, 0xFF334477);
    }
}

/* =========================================================================
 * Parallax background
 * ========================================================================= */
static void draw_background(game_t *g)
{
    /* Sky gradient (two bands) */
    g_fill_rect(g, 0, HUD_H, WIN_W, PLAY_H / 2, COL_SKY_TOP);
    g_fill_rect(g, 0, HUD_H + PLAY_H / 2, WIN_W, PLAY_H / 2, COL_SKY_BOT);

    /* Layer 1: distant "mountains" (scrolls at 0.25x camera) */
    int off1 = g_cam_x / 4;
    for (int i = 0; i < 12; i++) {
        int mx  = (i * 80 - off1) % (WIN_W + 80) - 40;
        int mh  = 40 + (i * 37) % 60;
        int mw  = 60 + (i * 23) % 40;
        int my  = HUD_H + PLAY_H - 80 - mh;
        u32 mc  = (i % 2 == 0) ? 0xFF3a3a5a : 0xFF444468;
        /* Simple triangle via filled rects */
        for (int row = 0; row < mh; row++) {
            int hw = (mw * (mh - row)) / (mh * 2);
            g_fill_rect(g, mx + mw/2 - hw, my + row, hw * 2, 1, mc);
        }
    }

    /* Layer 2: nearer hills (scrolls at 0.5x camera) */
    int off2 = g_cam_x / 2;
    for (int i = 0; i < 8; i++) {
        int hx   = (i * 110 - off2) % (WIN_W + 120) - 60;
        int hh   = 30 + (i * 41) % 40;
        int hw   = 80 + (i * 17) % 50;
        int hy   = HUD_H + PLAY_H - 60;
        u32 hc   = 0xFF2a4a2a;
        /* Draw semicircle-ish hill */
        for (int row = 0; row < hh; row++) {
            int halfW = (hw * (hh - row)) / (hh);
            g_fill_rect(g, hx + hw/2 - halfW/2, hy - row, halfW, 1, hc);
        }
    }
}

/* =========================================================================
 * HUD
 * ========================================================================= */
static void draw_hud(game_t *g)
{
    /* HUD background */
    g_fill_rect(g, 0, 0, WIN_W, HUD_H, 0xFF111122);

    /* Lives (hearts) */
    g_text(g, 4, 6, "LIVES:", 0xFFffffff);
    for (int i = 0; i < 3; i++) {
        u32 col = (i < g_lives) ? 0xFFff4444 : 0xFF442222;
        int hx = 56 + i * 14;
        int hy = 7;
        g_circle(g, hx,     hy, 4, col);
        g_circle(g, hx + 6, hy, 4, col);
        g_fill_rect(g, hx - 3, hy, 13, 6, col);
    }

    /* Score */
    g_text(g, WIN_W/2 - 48, 6, "SCORE:", 0xFFffd700);
    g_draw_int(g, WIN_W/2 + 8, 6, g_score, 0xFFffd700);

    /* Level */
    char lbuf[16];
    lbuf[0] = 'L'; lbuf[1] = 'V'; lbuf[2] = 'L'; lbuf[3] = ':';
    lbuf[4] = ' '; lbuf[5] = '0' + (g_level + 1); lbuf[6] = '\0';
    g_text(g, WIN_W - 72, 6, lbuf, 0xFF88ffff);
}

/* =========================================================================
 * Full render
 * ========================================================================= */
static void render_play(game_t *g)
{
    draw_background(g);

    /* Tiles */
    for (int row = 0; row < LEVEL_ROWS; row++) {
        for (int col = 0; col < LEVEL_COLS; col++) {
            u8 t = g_levels[g_level][row][col];
            if (t == T_AIR) continue;
            draw_tile(g, col, row, t, g_tile_state[row][col]);
        }
    }

    /* Particles */
    for (int i = 0; i < g_num_particles; i++) {
        particle_t *p = &g_particles[i];
        int alpha = (p->life * 255) / p->max_life;
        u32 col = (p->color & 0x00FFFFFF) | ((u32)alpha << 24);
        /* Simple 2px dot */
        g_fill_rect(g, p->x - g_cam_x, p->y + HUD_H, 2, 2, col);
    }

    draw_player(g);
    draw_hud(g);
}

/* =========================================================================
 * Title / overlay screens
 * ========================================================================= */
static void render_title(game_t *g)
{
    g_clear(g, 0xFF0a0a1e);

    /* Animated starfield */
    int t = g_title_timer;
    for (int i = 0; i < 40; i++) {
        int sx = (i * 37 + t / 2) % WIN_W;
        int sy = (i * 53 + t / 3) % WIN_H;
        g_fill_rect(g, sx, sy, 2, 2, 0xFFffffff);
    }

    g_text_center(g, WIN_W/2, WIN_H/2 - 60, "PLATFORMER", 0xFFffd700);
    g_text_center(g, WIN_W/2, WIN_H/2 - 30, "A/D or LEFT/RIGHT : move", 0xFFcccccc);
    g_text_center(g, WIN_W/2, WIN_H/2 - 14, "SPACE/UP/W : jump (hold for height)", 0xFFcccccc);
    g_text_center(g, WIN_W/2, WIN_H/2 + 6,  "Collect coins, reach the flag!", 0xFF88ff88);
    g_text_center(g, WIN_W/2, WIN_H/2 + 28, "3 lives  -  3 levels", 0xFFaaaaff);
    g_text_center(g, WIN_W/2, WIN_H/2 + 60, "PRESS SPACE OR ENTER TO START", 0xFFffffff);
}

static void render_win(game_t *g)
{
    render_play(g);
    /* Flash overlay */
    if ((g_win_timer / 6) % 2) {
        g_fill_rect(g, 0, HUD_H, WIN_W, PLAY_H, 0x44ffffff);
    }
    g_text_center(g, WIN_W/2, WIN_H/2 - 16, "LEVEL COMPLETE!", 0xFFffd700);
    if (g_level < 2) {
        g_text_center(g, WIN_W/2, WIN_H/2 + 8, "Get ready...", 0xFFffffff);
    } else {
        g_text_center(g, WIN_W/2, WIN_H/2 + 8, "YOU WIN!  ALL LEVELS DONE!", 0xFF88ff88);
        char sc_buf[32];
        /* "SCORE: XXXX" */
        const char *pre = "FINAL SCORE: ";
        int i = 0;
        while (pre[i]) { sc_buf[i] = pre[i]; i++; }
        g_itoa(g_score, sc_buf + i);
        g_text_center(g, WIN_W/2, WIN_H/2 + 28, sc_buf, 0xFFffd700);
        g_text_center(g, WIN_W/2, WIN_H/2 + 50, "PRESS ENTER TO RESTART", 0xFFaaaaaa);
    }
}

static void render_gameover(game_t *g)
{
    render_play(g);
    g_fill_rect(g, 0, HUD_H, WIN_W, PLAY_H, 0x88000000);
    g_text_center(g, WIN_W/2, WIN_H/2 - 24, "GAME OVER", 0xFFff4444);
    char sc_buf[32];
    const char *pre = "SCORE: ";
    int i = 0;
    while (pre[i]) { sc_buf[i] = pre[i]; i++; }
    g_itoa(g_score, sc_buf + i);
    g_text_center(g, WIN_W/2, WIN_H/2 + 4,  sc_buf, 0xFFffd700);
    g_text_center(g, WIN_W/2, WIN_H/2 + 28, "PRESS ENTER TO RESTART", 0xFFffffff);
}

/* =========================================================================
 * Game restart
 * ========================================================================= */
static void start_game(void)
{
    g_level = 0;
    g_lives = 3;
    g_score = 0;
    level_init();
    g_state = STATE_PLAY;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
void _start(void)
{
    game_t *g = game_open(WIN_W, WIN_H, "Platformer");
    if (!g) {
        /* Can't open window; spin */
        for (;;) { /* yield */ }
    }

    g_state       = STATE_TITLE;
    g_title_timer = 0;
    g_level       = 0;
    g_lives       = 3;
    g_score       = 0;

    while (game_frame_begin(g)) {
        int dt = game_dt_ms(g);
        if (dt < 1)  dt = 1;
        if (dt > 50) dt = 50;   /* cap at 50ms to avoid spiral on lag */

        switch (g_state) {

        /* ---- TITLE ---- */
        case STATE_TITLE:
            g_title_timer++;
            if (game_key_pressed(g, KEY_SPACE) || game_key_pressed(g, KEY_ENTER)) {
                start_game();
            }
            render_title(g);
            break;

        /* ---- PLAY ---- */
        case STATE_PLAY: {
            player_t *pl = &g_player;

            /* Handle death recovery */
            if (pl->dead) {
                pl->dead_timer--;
                if (pl->dead_timer <= 0) {
                    if (g_lives <= 0) {
                        g_state = STATE_GAMEOVER;
                        break;
                    }
                    /* Respawn */
                    pl->px       = pl->respawn_x;
                    pl->py       = pl->respawn_y;
                    pl->vx       = 0;
                    pl->vy       = 0;
                    pl->dead     = 0;
                    pl->coyote   = 0;
                    pl->jump_buffer = 0;
                    pl->is_jumping = 0;
                    g_num_particles = 0;
                }
            }

            physics_update(g, dt);
            update_particles();
            update_camera();
            render_play(g);
            break;
        }

        /* ---- WIN ---- */
        case STATE_WIN:
            g_win_timer--;
            update_particles();
            if (g_win_timer <= 0) {
                if (g_level < 2) {
                    g_level++;
                    level_init();
                    g_state = STATE_PLAY;
                } else {
                    /* All levels done — stay on win screen */
                    g_win_timer = 1; /* keep showing win */
                    if (game_key_pressed(g, KEY_ENTER) || game_key_pressed(g, KEY_SPACE)) {
                        start_game();
                    }
                }
            }
            render_win(g);
            break;

        /* ---- GAME OVER ---- */
        case STATE_GAMEOVER:
            if (game_key_pressed(g, KEY_ENTER) || game_key_pressed(g, KEY_SPACE)) {
                start_game();
            }
            render_gameover(g);
            break;

        default:
            break;
        }

        game_present(g);
        game_sync(g);
    }
}
