/*
 * game.h -- Kernel game framework (freestanding, ring 3).
 * ========================================================
 *
 * A clean, ergonomic API for writing games on the custom x86_64 OS.
 * Sits on top of wl_client (windowing) and bitfont (text rendering).
 * No libc, no libm -- pure integer / fixed-point arithmetic.
 *
 * Typical skeleton:
 *
 *   game_t *g = game_open(640, 480, "My Game");
 *   while (game_frame_begin(g)) {
 *       int dt = game_dt_ms(g);
 *       if (game_key_down(g, KEY_LEFT))  player.x -= 2;
 *       g_clear(g, 0xFF111111);
 *       g_fill_rect(g, player.x, player.y, 16, 16, 0xFF00FF00);
 *       g_text(g, 4, 4, "Hello", 0xFFFFFFFF);
 *       game_present(g);
 *       game_sync(g);
 *   }
 *
 * Build flags (freestanding, no canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2
 *       -c game.c -o game.o
 */

#ifndef GAME_H
#define GAME_H

/* =========================================================================
 * Integer types (no stdint.h in freestanding builds).
 * ========================================================================= */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef long               i64;

/* =========================================================================
 * Syscall numbers.
 * ========================================================================= */
#define SYS_WRITE          3
#define SYS_YIELD          15
#define SYS_GET_TICKS_MS   40
#define SYS_BEEP           45
#define SYS_RANDOM         43

/* =========================================================================
 * Key codes (kernel Linux-compatible scancodes, from kernel/include/input.h,
 * confirmed against snake.c and paint.c).
 * ========================================================================= */
#define KEY_ESC       1
#define KEY_1         2
#define KEY_2         3
#define KEY_3         4
#define KEY_4         5
#define KEY_5         6
#define KEY_6         7
#define KEY_7         8
#define KEY_8         9
#define KEY_9         10
#define KEY_0         11
#define KEY_MINUS     12
#define KEY_EQUAL     13
#define KEY_BACKSPACE 14
#define KEY_TAB       15
#define KEY_Q         16
#define KEY_W         17
#define KEY_E         18
#define KEY_R         19
#define KEY_T         20
#define KEY_Y         21
#define KEY_U         22
#define KEY_I         23
#define KEY_O         24
#define KEY_P         25
#define KEY_A         30
#define KEY_S         31
#define KEY_D         32
#define KEY_F         33
#define KEY_G         34
#define KEY_H         35
#define KEY_J         36
#define KEY_K         37
#define KEY_L         38
#define KEY_ENTER     28
#define KEY_Z         44
#define KEY_X         45
#define KEY_C         46
#define KEY_V         47
#define KEY_B         48
#define KEY_N         49
#define KEY_M         50
#define KEY_SPACE     57
#define KEY_UP        103
#define KEY_LEFT      105
#define KEY_RIGHT     106
#define KEY_DOWN      108
#define KEY_F1        59
#define KEY_F2        60
#define KEY_F3        61
#define KEY_F4        62
#define KEY_F5        63
#define KEY_F6        64
#define KEY_F7        65
#define KEY_F8        66
#define KEY_F9        67
#define KEY_F10       68
#define KEY_F11       87
#define KEY_F12       88

/* Total number of tracked keys (must cover all KEY_* above). */
#define GAME_NUM_KEYS 256

/* Mouse button bitmasks (from wl_poll_event pointer events). */
#define MOUSE_LEFT    (1 << 0)
#define MOUSE_RIGHT   (1 << 1)
#define MOUSE_MIDDLE  (1 << 2)

/* =========================================================================
 * Fixed-point math constants.
 * ========================================================================= */

/*
 * Fixed-point sin/cos table: 256 entries for angles 0..255 (full circle).
 * Values are 16.16 fixed-point * 65536 -- call g_sin(angle) / g_cos(angle)
 * where angle is in [0,255].  Return value is i32 in range [-65536, 65536].
 */
#define FP_ONE         65536    /* 1.0 in 16.16 fixed-point */
#define FP_HALF        32768
#define FP_PI_APPROX   205887   /* pi * 65536 */

/* Angle helpers: degrees to table index (0..255), 0..255 units per circle. */
#define DEG_TO_ANGLE(d)  (((d) * 256) / 360)

/* =========================================================================
 * Sprite / image descriptor.
 * ========================================================================= */
typedef struct {
    const u32 *pixels;  /* ARGB32 pixel data, row-major */
    int        w;       /* width in pixels  */
    int        h;       /* height in pixels */
    u32        key;     /* color-key (transparent color); 0 = no key */
    int        has_key; /* 1 if color-key transparency is active */
} game_sprite_t;

/* =========================================================================
 * Opaque game context.
 * ========================================================================= */
#define GAME_BB_MAX_W  1920
#define GAME_BB_MAX_H  1200

typedef struct game_t game_t;

/* =========================================================================
 * Lifecycle.
 * ========================================================================= */

/*
 * game_open - Connect to compositor, create window, allocate backbuffer.
 * Returns a pointer to the game context, or NULL on failure.
 * Internally calls wl_connect() + wl_create_window().
 */
game_t *game_open(int w, int h, const char *title);

/*
 * game_frame_begin - Poll all pending input events (updates key/mouse state),
 * then return 1 to continue or 0 if the window was closed/destroyed.
 * Must be called at the top of every frame loop iteration.
 */
int game_frame_begin(game_t *g);

/*
 * game_present - Blit the backbuffer to the window and call wl_commit().
 */
void game_present(game_t *g);

/*
 * game_dt_ms - Return the elapsed time in milliseconds since the last
 * game_frame_begin() call.  First frame returns 16.  Uses SYS_GET_TICKS_MS.
 */
int game_dt_ms(game_t *g);

/*
 * game_sync - Pace the loop to ~60 fps (16 ms target).  Yields via
 * SYS_YIELD until at least (target_ms - elapsed) ms have passed.
 * Call after game_present().
 */
void game_sync(game_t *g);

/*
 * game_ticks - Return the raw tick count in milliseconds (wraps at 64 bits).
 */
u64 game_ticks(void);

/* =========================================================================
 * Input -- keyboard.
 * ========================================================================= */

/*
 * game_key_down - Return non-zero if the key with the given keycode is
 * currently held down (maintained across frames from wl_poll_event).
 * Also fires on the exact frame the key was pressed (edge detection).
 */
int game_key_down(game_t *g, int keycode);

/*
 * game_key_pressed - Return non-zero only on the first frame the key was
 * pressed (edge detection only, cleared after game_frame_begin).
 */
int game_key_pressed(game_t *g, int keycode);

/*
 * game_key_released - Return non-zero only on the first frame the key was
 * released (edge detection, cleared after game_frame_begin).
 */
int game_key_released(game_t *g, int keycode);

/* =========================================================================
 * Input -- mouse.
 * ========================================================================= */

/*
 * game_mouse - Fill *x, *y, *buttons with the current mouse state.
 * *buttons is a bitmask of MOUSE_LEFT / MOUSE_RIGHT / MOUSE_MIDDLE.
 * Any pointer may be NULL if the caller doesn't need that component.
 */
void game_mouse(game_t *g, int *x, int *y, int *buttons);

/* =========================================================================
 * Drawing -- all operate on the backbuffer.
 * All colors are ARGB32 (0xAARRGGBB).
 * Coordinates are in pixels.  Out-of-bounds operations are silently clipped.
 * ========================================================================= */

/* Fill the entire backbuffer with a solid color. */
void g_clear(game_t *g, u32 argb);

/* Set a single pixel. */
void g_pixel(game_t *g, int x, int y, u32 argb);

/* Draw an axis-aligned unfilled rectangle (1px border). */
void g_rect(game_t *g, int x, int y, int w, int h, u32 argb);

/* Draw a filled axis-aligned rectangle. */
void g_fill_rect(game_t *g, int x, int y, int w, int h, u32 argb);

/*
 * Draw a filled rounded rectangle.
 * r = corner radius in pixels (clamped to min(w,h)/2).
 */
void g_rounded_rect(game_t *g, int x, int y, int w, int h, int r, u32 argb);

/* Draw a line from (x0,y0) to (x1,y1) using Bresenham. */
void g_line(game_t *g, int x0, int y0, int x1, int y1, u32 argb);

/* Draw a filled circle centered at (cx,cy) with radius r. */
void g_circle(game_t *g, int cx, int cy, int r, u32 argb);

/* Draw an unfilled circle (1px border). */
void g_circle_outline(game_t *g, int cx, int cy, int r, u32 argb);

/*
 * Draw a NUL-terminated ASCII string using the 8x16 bitmap font.
 * Uses bitfont's font_draw_string() internally.
 */
void g_text(game_t *g, int x, int y, const char *s, u32 argb);

/*
 * Draw text centered horizontally within the given rectangle.
 * (cx, cy) is the center point; text is centered on it.
 */
void g_text_center(game_t *g, int cx, int cy, const char *s, u32 argb);

/*
 * Blit a sprite at (x, y).
 * If spr->has_key is non-zero, pixels matching spr->key are not drawn
 * (color-key transparency).
 */
void g_blit(game_t *g, int x, int y, const game_sprite_t *spr);

/*
 * Blit raw pixel data at (x, y) with optional color-key transparency.
 * key_color is the transparent color; set has_key=0 to disable transparency.
 */
void g_blit_raw(game_t *g, int x, int y,
                const u32 *pixels, int w, int h,
                u32 key_color, int has_key);

/* =========================================================================
 * Math helpers (integer / fixed-point, no libm).
 * ========================================================================= */

/* Clamp v to [lo, hi]. */
static inline int g_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Integer min / max. */
static inline int g_min(int a, int b) { return a < b ? a : b; }
static inline int g_max(int a, int b) { return a > b ? a : b; }

/* Integer absolute value. */
static inline int g_abs(int v) { return v < 0 ? -v : v; }

/*
 * Fixed-point sin/cos: angle is 0..255 (full circle = 256 steps).
 * Return value is i32 in range [-65536, 65536] (16.16 fixed-point).
 */
i32 g_sin(int angle);
i32 g_cos(int angle);

/* =========================================================================
 * Random number generation (seeded via SYS_RANDOM syscall).
 * ========================================================================= */

/* Seed the PRNG (called automatically by game_open with SYS_RANDOM). */
void g_srand(u32 seed);

/* Return a pseudo-random 32-bit unsigned integer. */
u32 g_rand(void);

/* Return a pseudo-random integer in [0, n). */
static inline int g_rand_range(int n)
{
    if (n <= 1) return 0;
    return (int)(g_rand() % (u32)n);
}

/* =========================================================================
 * Collision detection.
 * ========================================================================= */

/*
 * AABB (axis-aligned bounding box) overlap test.
 * Returns 1 if the two rectangles overlap, 0 otherwise.
 */
static inline int g_aabb(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return (ax < bx + bw) && (ax + aw > bx) &&
           (ay < by + bh) && (ay + ah > by);
}

/* =========================================================================
 * Audio.
 * ========================================================================= */

/*
 * g_beep - Play a tone at the given frequency (Hz) for ms milliseconds.
 * Uses SYS_BEEP.  Silently does nothing if the syscall returns negative
 * (hardware not present / not implemented).
 */
void g_beep(int freq_hz, int ms);

/* =========================================================================
 * String / HUD helpers (no libc sprintf).
 * ========================================================================= */

/*
 * g_itoa - Convert integer v to a NUL-terminated decimal string in buf.
 * buf must be at least 12 bytes.  Returns the number of characters written
 * (not counting the NUL).
 */
int g_itoa(int v, char *buf);

/*
 * g_uitoa - Convert unsigned integer v to decimal string.
 * buf must be at least 12 bytes.
 */
int g_uitoa(u32 v, char *buf);

/*
 * g_draw_int - Convenience: format integer v and draw it at (x, y).
 */
void g_draw_int(game_t *g, int x, int y, int v, u32 argb);

/*
 * g_draw_uint - Convenience: format unsigned integer and draw it.
 */
void g_draw_uint(game_t *g, int x, int y, u32 v, u32 argb);

/*
 * g_draw_score - Draw a score HUD item: "LABEL: N" at (x, y).
 * Typical usage: g_draw_score(g, 4, 4, "SCORE", score, 0xFFFFFFFF);
 */
void g_draw_score(game_t *g, int x, int y,
                  const char *label, int score, u32 argb);

/* =========================================================================
 * Backbuffer direct access (advanced use).
 * ========================================================================= */

/*
 * game_backbuffer - Return a pointer to the raw backbuffer pixel data.
 * Layout: ARGB32, row-major, stride = w pixels (no padding).
 */
u32 *game_backbuffer(game_t *g);

/*
 * game_width / game_height - Return the window dimensions.
 */
int game_width(game_t *g);
int game_height(game_t *g);

#endif /* GAME_H */
