/*
 * game.c -- Kernel game framework implementation (freestanding, ring 3).
 * =======================================================================
 *
 * Sits on top of wl_client (windowing + events) and bitfont (8x16 text).
 * No libc: pure freestanding C with inline syscalls.
 *
 * Build (object only; link into your game's final elf):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/game/game.c -o /tmp/game.o
 *   objdump -d /tmp/game.o | grep fs:0x28   # must be empty
 *
 * Include paths needed by consumer:
 *   -I<root>/userspace/lib/game
 *   -I<root>/userspace/lib/wl
 *   -I<root>/userspace/lib/font
 */

#include "game.h"
#include "../wl/wl_client.h"
#include "../font/bitfont.h"

/* =========================================================================
 * Internal inline syscall (6-argument form).
 * ========================================================================= */
static inline long _sc(long n, long a1, long a2, long a3,
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
 * Backbuffer storage.
 *
 * The backbuffer is a statically-allocated pixel array large enough for
 * up to GAME_BB_MAX_W x GAME_BB_MAX_H pixels.  We keep it as a separate
 * buffer (not the wl_window->pixels directly) so that:
 *   - The game draws cleanly into a local buffer.
 *   - game_present() copies it to the compositor-shared window buffer.
 * This avoids partial-frame tearing visible via the compositor.
 *
 * Memory layout: ARGB32, w*h pixels, stride = w (no row padding).
 * ========================================================================= */
#define BB_MAX_PIXELS  (1920 * 1200)

/* Static backbuffer; declared here so it ends up in BSS (zero-initialized).
 * Freestanding loaders must zero BSS; if yours does not, call game_open()
 * which writes via g_clear() before the first frame anyway.              */
static u32 _backbuf[BB_MAX_PIXELS];

/* =========================================================================
 * game_t definition.
 * ========================================================================= */
struct game_t {
    wl_window *win;         /* compositor window                          */
    u32       *bb;          /* backbuffer pointer (into _backbuf)         */
    int        w, h;        /* window / backbuffer dimensions             */

    u64        frame_start; /* ticks_ms at start of current frame         */
    u64        prev_start;  /* ticks_ms at start of previous frame        */
    int        dt_ms;       /* elapsed ms for current frame               */

    /* Target frame time for game_sync(). */
    int        target_ms;   /* default 16 ms (~60 fps)                    */

    /* Keyboard state: 256 slots. */
    u8         key_held[GAME_NUM_KEYS];     /* currently held             */
    u8         key_edge_dn[GAME_NUM_KEYS];  /* pressed this frame         */
    u8         key_edge_up[GAME_NUM_KEYS];  /* released this frame        */

    /* Mouse state. */
    int        mouse_x, mouse_y, mouse_buttons;

    /* Alive flag: set to 0 if the server destroys the window. */
    int        alive;
};

/* Single static instance (games create one game_t). */
static struct game_t _game_ctx;

/* =========================================================================
 * Raw ticks helper.
 * ========================================================================= */
u64 game_ticks(void)
{
    return (u64)_sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
}

/* =========================================================================
 * Lifecycle.
 * ========================================================================= */
game_t *game_open(int w, int h, const char *title)
{
    if (wl_connect() != 0) return (game_t *)0;

    /* Bounds check BEFORE creating the window: a too-large/invalid size must not
     * leak the compositor window + SHM segment (the old code created the window
     * first, then bailed without destroying it). Use 64-bit for w*h so the
     * comparison can't be defeated by 32-bit overflow wrapping to a negative. */
    if (w <= 0 || h <= 0 || (long long)w * (long long)h > BB_MAX_PIXELS) {
        return (game_t *)0;
    }

    wl_window *win = wl_create_window((wl_u32)w, (wl_u32)h, title);
    if (!win) return (game_t *)0;

    struct game_t *g = &_game_ctx;

    /* Zero state. */
    for (int i = 0; i < GAME_NUM_KEYS; i++) {
        g->key_held[i] = 0;
        g->key_edge_dn[i] = 0;
        g->key_edge_up[i] = 0;
    }

    g->win            = win;
    g->bb             = _backbuf;
    g->w              = w;
    g->h              = h;
    g->mouse_x        = 0;
    g->mouse_y        = 0;
    g->mouse_buttons  = 0;
    g->alive          = 1;
    g->target_ms      = 16;   /* ~60 fps */

    u64 now = game_ticks();
    g->frame_start = now;
    g->prev_start  = now;
    g->dt_ms       = 16;

    /* Seed RNG from SYS_RANDOM; fallback to ticks if unavailable. */
    long rv = _sc(SYS_RANDOM, 0, 0, 0, 0, 0, 0);
    u32 seed = (rv > 0) ? (u32)rv : (u32)now;
    if (seed == 0) seed = 0xDEADBEEFu;
    g_srand(seed);

    /* Clear backbuffer. */
    g_clear(g, 0xFF000000u);

    return (game_t *)g;
}

int game_frame_begin(game_t *g)
{
    struct game_t *ctx = (struct game_t *)g;

    /* Timestamp. */
    u64 now = game_ticks();
    ctx->prev_start  = ctx->frame_start;
    ctx->frame_start = now;

    u64 diff = now - ctx->prev_start;
    ctx->dt_ms = (diff < 1) ? 1 : (diff > 500 ? 500 : (int)diff);

    /* Clear per-frame edge arrays. */
    for (int i = 0; i < GAME_NUM_KEYS; i++) {
        ctx->key_edge_dn[i] = 0;
        ctx->key_edge_up[i] = 0;
    }

    /* Drain all pending events. */
    int kind, a, b, c;
    while (wl_poll_event(ctx->win, &kind, &a, &b, &c)) {
        if (kind == WL_EVENT_KEY) {
            int kc = a;
            int pressed = (b != 0);
            if (kc >= 0 && kc < GAME_NUM_KEYS) {
                if (pressed && !ctx->key_held[kc]) {
                    ctx->key_edge_dn[kc] = 1;
                }
                if (!pressed && ctx->key_held[kc]) {
                    ctx->key_edge_up[kc] = 1;
                }
                ctx->key_held[kc] = (u8)pressed;
            }
        } else if (kind == WL_EVENT_POINTER) {
            ctx->mouse_x       = a;
            ctx->mouse_y       = b;
            ctx->mouse_buttons = c;
        }
    }

    return ctx->alive;
}

void game_present(game_t *g)
{
    struct game_t *ctx = (struct game_t *)g;
    /* Copy backbuffer -> compositor-shared window pixels. */
    u32 stride_px = ctx->win->stride / 4u;
    const u32 *src = ctx->bb;
    u32       *dst = ctx->win->pixels;
    int w = ctx->w, h = ctx->h;

    for (int y = 0; y < h; y++) {
        const u32 *srow = src + y * w;
        u32       *drow = dst + y * stride_px;
        for (int x = 0; x < w; x++) {
            drow[x] = srow[x];
        }
    }
    wl_commit(ctx->win);
}

int game_dt_ms(game_t *g)
{
    return ((struct game_t *)g)->dt_ms;
}

void game_sync(game_t *g)
{
    struct game_t *ctx = (struct game_t *)g;
    int target = ctx->target_ms;
    /* Yield at least once, then keep yielding until target elapsed. */
    _sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    for (;;) {
        u64 now = game_ticks();
        u64 elapsed = now - ctx->frame_start;
        if (elapsed >= (u64)target) break;
        _sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}

/* =========================================================================
 * Input.
 * ========================================================================= */
int game_key_down(game_t *g, int keycode)
{
    struct game_t *ctx = (struct game_t *)g;
    if (keycode < 0 || keycode >= GAME_NUM_KEYS) return 0;
    return ctx->key_held[keycode] != 0;
}

int game_key_pressed(game_t *g, int keycode)
{
    struct game_t *ctx = (struct game_t *)g;
    if (keycode < 0 || keycode >= GAME_NUM_KEYS) return 0;
    return ctx->key_edge_dn[keycode] != 0;
}

int game_key_released(game_t *g, int keycode)
{
    struct game_t *ctx = (struct game_t *)g;
    if (keycode < 0 || keycode >= GAME_NUM_KEYS) return 0;
    return ctx->key_edge_up[keycode] != 0;
}

void game_mouse(game_t *g, int *x, int *y, int *buttons)
{
    struct game_t *ctx = (struct game_t *)g;
    if (x)       *x       = ctx->mouse_x;
    if (y)       *y       = ctx->mouse_y;
    if (buttons) *buttons = ctx->mouse_buttons;
}

/* =========================================================================
 * Drawing.
 * ========================================================================= */

static inline void _put(u32 *bb, int stride, int bw, int bh,
                         int x, int y, u32 color)
{
    if ((unsigned)x < (unsigned)bw && (unsigned)y < (unsigned)bh)
        bb[y * stride + x] = color;
}

void g_clear(game_t *g, u32 argb)
{
    struct game_t *ctx = (struct game_t *)g;
    u32 *bb = ctx->bb;
    int n = ctx->w * ctx->h;
    for (int i = 0; i < n; i++) bb[i] = argb;
}

void g_pixel(game_t *g, int x, int y, u32 argb)
{
    struct game_t *ctx = (struct game_t *)g;
    _put(ctx->bb, ctx->w, ctx->w, ctx->h, x, y, argb);
}

void g_fill_rect(game_t *g, int x, int y, int w, int h, u32 argb)
{
    struct game_t *ctx = (struct game_t *)g;
    int bw = ctx->w, bh = ctx->h;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w; if (x2 > bw) x2 = bw;
    int y2 = y + h; if (y2 > bh) y2 = bh;
    if (x1 >= x2 || y1 >= y2) return;
    u32 *bb = ctx->bb;
    int stride = bw;
    for (int yy = y1; yy < y2; yy++) {
        u32 *row = bb + yy * stride;
        for (int xx = x1; xx < x2; xx++) row[xx] = argb;
    }
}

void g_rect(game_t *g, int x, int y, int w, int h, u32 argb)
{
    /* Top and bottom edges. */
    g_fill_rect(g, x,         y,         w, 1, argb);
    g_fill_rect(g, x,         y + h - 1, w, 1, argb);
    /* Left and right edges (exclude corners already drawn). */
    g_fill_rect(g, x,         y + 1,     1, h - 2, argb);
    g_fill_rect(g, x + w - 1, y + 1,     1, h - 2, argb);
}

void g_rounded_rect(game_t *g, int x, int y, int w, int h, int r, u32 argb)
{
    int max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r < 1) { g_fill_rect(g, x, y, w, h, argb); return; }

    /* Fill three non-corner rectangles. */
    g_fill_rect(g, x + r,     y,         w - 2*r, h,         argb); /* center   */
    g_fill_rect(g, x,         y + r,     r,       h - 2*r,   argb); /* left bar */
    g_fill_rect(g, x + w - r, y + r,     r,       h - 2*r,   argb); /* right bar*/

    /* Fill four corner quarter-discs. */
    struct game_t *ctx = (struct game_t *)g;
    int bw = ctx->w, bh = ctx->h;
    u32 *bb = ctx->bb;
    int stride = bw;
    int r2 = r * r;

    int corners[4][2] = {
        { x + r - 1,     y + r - 1     }, /* top-left   */
        { x + w - r,     y + r - 1     }, /* top-right  */
        { x + r - 1,     y + h - r     }, /* bottom-left*/
        { x + w - r,     y + h - r     }, /* bottom-right */
    };

    for (int cy_idx = 0; cy_idx < 4; cy_idx++) {
        int cx = corners[cy_idx][0];
        int cy = corners[cy_idx][1];
        for (int dy = -r; dy <= 0; dy++) {
            /* Use symmetric fill: draw for dy and -dy-1. */
            for (int dx = -r; dx <= 0; dx++) {
                if (dx*dx + dy*dy <= r2) {
                    /* Which quadrant is this corner? */
                    int fx = (cy_idx & 1) ? (cx + (-dx - 1)) : (cx + dx);
                    int fy = (cy_idx >= 2) ? (cy + (-dy - 1)) : (cy + dy);
                    if ((unsigned)fx < (unsigned)bw && (unsigned)fy < (unsigned)bh)
                        bb[fy * stride + fx] = argb;
                }
            }
        }
    }
}

void g_line(game_t *g, int x0, int y0, int x1, int y1, u32 argb)
{
    struct game_t *ctx = (struct game_t *)g;
    int bw = ctx->w, bh = ctx->h;
    u32 *bb = ctx->bb;

    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x1 >= x0) ? 1 : -1;
    int sy = (y1 >= y0) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if ((unsigned)x0 < (unsigned)bw && (unsigned)y0 < (unsigned)bh)
            bb[y0 * bw + x0] = argb;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void g_circle(game_t *g, int cx, int cy, int r, u32 argb)
{
    struct game_t *ctx = (struct game_t *)g;
    int bw = ctx->w, bh = ctx->h;
    u32 *bb = ctx->bb;
    int r2 = r * r;

    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= bh) continue;
        /* x range: dx*dx <= r2 - dy*dy */
        int rem = r2 - dy*dy;
        if (rem < 0) continue;
        int dx = r;
        while (dx * dx > rem) dx--;
        int x0 = cx - dx; if (x0 < 0)  x0 = 0;
        int x1 = cx + dx; if (x1 >= bw) x1 = bw - 1;
        u32 *row = bb + y * bw;
        for (int x = x0; x <= x1; x++) row[x] = argb;
    }
}

void g_circle_outline(game_t *g, int cx, int cy, int r, u32 argb)
{
    /* Midpoint circle algorithm. */
    struct game_t *ctx = (struct game_t *)g;
    int bw = ctx->w, bh = ctx->h;
    u32 *bb = ctx->bb;
    int x = 0, y = r, d = 1 - r;

    while (x <= y) {
        int pts[8][2] = {
            {cx+x, cy+y}, {cx-x, cy+y}, {cx+x, cy-y}, {cx-x, cy-y},
            {cx+y, cy+x}, {cx-y, cy+x}, {cx+y, cy-x}, {cx-y, cy-x},
        };
        for (int i = 0; i < 8; i++) {
            int px = pts[i][0], py = pts[i][1];
            if ((unsigned)px < (unsigned)bw && (unsigned)py < (unsigned)bh)
                bb[py * bw + px] = argb;
        }
        if (d < 0) {
            d += 2*x + 3;
        } else {
            d += 2*(x - y) + 5;
            y--;
        }
        x++;
    }
}

void g_text(game_t *g, int x, int y, const char *s, u32 argb)
{
    struct game_t *ctx = (struct game_t *)g;
    font_draw_string(ctx->bb, ctx->w, ctx->w, ctx->h, x, y, s, argb);
}

void g_text_center(game_t *g, int cx, int cy, const char *s, u32 argb)
{
    int tw = font_text_width(s);
    int tx = cx - tw / 2;
    int ty = cy - FONT_H / 2;
    g_text(g, tx, ty, s, argb);
}

void g_blit(game_t *g, int x, int y, const game_sprite_t *spr)
{
    g_blit_raw(g, x, y, spr->pixels, spr->w, spr->h,
               spr->key, spr->has_key);
}

void g_blit_raw(game_t *g, int x, int y,
                const u32 *pixels, int sw, int sh,
                u32 key_color, int has_key)
{
    struct game_t *ctx = (struct game_t *)g;
    int bw = ctx->w, bh = ctx->h;
    u32 *bb = ctx->bb;

    for (int sy = 0; sy < sh; sy++) {
        int dy = y + sy;
        if (dy < 0 || dy >= bh) continue;
        const u32 *srow = pixels + sy * sw;
        u32       *drow = bb + dy * bw;
        for (int sx = 0; sx < sw; sx++) {
            int dx = x + sx;
            if (dx < 0 || dx >= bw) continue;
            u32 p = srow[sx];
            if (has_key && p == key_color) continue;
            drow[dx] = p;
        }
    }
}

/* =========================================================================
 * Fixed-point sin/cos table.
 *
 * 256-entry table covering a full circle (angle 0..255 maps to 0..2*pi).
 * Values are i32 scaled by 65536 (16.16 fixed-point).
 * Generated by: round(sin(i * 2*pi / 256) * 65536) for i in 0..255.
 *
 * Using a compile-time table avoids any floating-point or libm dependency.
 * ========================================================================= */
static const i32 _sin_table[256] = {
       0,   1608,   3216,   4821,   6424,   8022,   9616,  11204,
   12785,  14359,  15924,  17479,  19024,  20557,  22078,  23586,
   25080,  26558,  28020,  29464,  30890,  32297,  33683,  35048,
   36391,  37711,  39007,  40278,  41524,  42743,  43935,  45098,
   46233,  47337,  48411,  49452,  50460,  51436,  52377,  53283,
   54154,  54988,  55786,  56545,  57267,  57951,  58594,  59198,
   59760,  60281,  60761,  61199,  61594,  61946,  62255,  62520,
   62741,  62917,  63050,  63138,  63182,  63182,  63138,  63050,
   62917,  62741,  62520,  62255,  61946,  61594,  61199,  60761,
   60281,  59760,  59198,  58594,  57951,  57267,  56545,  55786,
   54988,  54154,  53283,  52377,  51436,  50460,  49452,  48411,
   47337,  46233,  45098,  43935,  42743,  41524,  40278,  39007,
   37711,  36391,  35048,  33683,  32297,  30890,  29464,  28020,
   26558,  25080,  23586,  22078,  20557,  19024,  17479,  15924,
   14359,  12785,  11204,   9616,   8022,   6424,   4821,   3216,
    1608,      0,  -1608,  -3216,  -4821,  -6424,  -8022,  -9616,
  -11204, -12785, -14359, -15924, -17479, -19024, -20557, -22078,
  -23586, -25080, -26558, -28020, -29464, -30890, -32297, -33683,
  -35048, -36391, -37711, -39007, -40278, -41524, -42743, -43935,
  -45098, -46233, -47337, -48411, -49452, -50460, -51436, -52377,
  -53283, -54154, -54988, -55786, -56545, -57267, -57951, -58594,
  -59198, -59760, -60281, -60761, -61199, -61594, -61946, -62255,
  -62520, -62741, -62917, -63050, -63138, -63182, -63182, -63138,
  -63050, -62917, -62741, -62520, -62255, -61946, -61594, -61199,
  -60761, -60281, -59760, -59198, -58594, -57951, -57267, -56545,
  -55786, -54988, -54154, -53283, -52377, -51436, -50460, -49452,
  -48411, -47337, -46233, -45098, -43935, -42743, -41524, -40278,
  -39007, -37711, -36391, -35048, -33683, -32297, -30890, -29464,
  -28020, -26558, -25080, -23586, -22078, -20557, -19024, -17479,
  -15924, -14359, -12785, -11204,  -9616,  -8022,  -6424,  -4821,
   -3216,  -1608,      0,   1608,   3216,   4821,   6424,   8022,  /* wrap */
   9616,  11204  /* extra two to silence array-size padding -- unused */
};

i32 g_sin(int angle)
{
    return _sin_table[(unsigned char)angle];
}

i32 g_cos(int angle)
{
    /* cos(x) = sin(x + 64) -- quarter-circle offset in 256-step circle */
    return _sin_table[(unsigned char)(angle + 64)];
}

/* =========================================================================
 * PRNG (LCG, seeded via SYS_RANDOM on game_open).
 * ========================================================================= */
static u32 _rand_state = 12345u;

void g_srand(u32 seed)
{
    _rand_state = seed ? seed : 0x12345678u;
}

u32 g_rand(void)
{
    _rand_state = _rand_state * 1664525u + 1013904223u;
    return _rand_state;
}

/* =========================================================================
 * Audio.
 * ========================================================================= */
void g_beep(int freq_hz, int ms)
{
    long r = _sc(SYS_BEEP, (long)freq_hz, (long)ms, 0, 0, 0, 0);
    (void)r; /* silently ignore negative (unsupported) */
}

/* =========================================================================
 * String / HUD helpers.
 * ========================================================================= */
int g_uitoa(u32 v, char *buf)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12];
    int i = 0;
    while (v > 0) { tmp[i++] = (char)('0' + v % 10u); v /= 10u; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
    return i;
}

int g_itoa(int v, char *buf)
{
    if (v < 0) {
        buf[0] = '-';
        int len = g_uitoa((u32)(-v), buf + 1);
        return len + 1;
    }
    return g_uitoa((u32)v, buf);
}

void g_draw_int(game_t *g, int x, int y, int v, u32 argb)
{
    char buf[14];
    g_itoa(v, buf);
    g_text(g, x, y, buf, argb);
}

void g_draw_uint(game_t *g, int x, int y, u32 v, u32 argb)
{
    char buf[12];
    g_uitoa(v, buf);
    g_text(g, x, y, buf, argb);
}

/*
 * g_draw_score: draw "LABEL: N" at (x, y).
 * Concatenates label + ": " + decimal(score) into a local buffer.
 */
void g_draw_score(game_t *g, int x, int y,
                  const char *label, int score, u32 argb)
{
    char buf[64];
    int i = 0;

    /* Copy label. */
    for (; label[i] && i < 48; i++) buf[i] = label[i];

    /* Append ": ". */
    if (i < 62) buf[i++] = ':';
    if (i < 62) buf[i++] = ' ';

    /* Append decimal score. */
    char num[12];
    int nlen = g_itoa(score, num);
    for (int j = 0; j < nlen && i < 62; j++) buf[i++] = num[j];

    buf[i] = '\0';
    g_text(g, x, y, buf, argb);
}

/* =========================================================================
 * Backbuffer access.
 * ========================================================================= */
u32 *game_backbuffer(game_t *g)
{
    return ((struct game_t *)g)->bb;
}

int game_width(game_t *g)
{
    return ((struct game_t *)g)->w;
}

int game_height(game_t *g)
{
    return ((struct game_t *)g)->h;
}
