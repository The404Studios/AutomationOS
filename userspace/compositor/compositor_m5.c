/*
 * compositor_m5.c -- Milestone M5 DESKTOP SHELL (ring 3): ANIMATED + POLISHED.
 *
 * Built on compositor_m4.c (verified working desktop shell). Keeps ALL m4
 * behavior: top panel (clock + focused title), bottom dock (launcher spawns
 * sbin/terminal + per-window taskbar), zero-copy SHM client compositing, font
 * titlebars, mouse window-management (drag/focus/raise/close), keyboard +
 * pointer forwarding to the focused window, chrome reservation, unconditional
 * SYS_YIELD per frame.
 *
 * NEW IN M5
 * =========
 *  1. ANIMATION ENGINE (frame-clock driven via SYS_GET_TICKS_MS). Each window
 *     slot carries an animation phase (NONE/OPENING/CLOSING/MINIMIZING/
 *     RESTORING), anim_start_ms, anim_dur_ms, and a per-frame computed scale
 *     (FIXED-POINT, /256) + alpha (0..256).
 *       - OPEN  (~180ms): scale 0.90 -> 1.00 (ease-out-cubic), alpha 0 -> 256.
 *       - CLOSE (~150ms): scale 1.00 -> 0.90 (ease-in-cubic), alpha 256 -> 0;
 *         the slot is destroyed (shmdt + free) only when t>=1.
 *       - MINIMIZE (~220ms): the drawn rect shrinks + slides toward the
 *         window's taskbar button, alpha -> 0; the slot stays alive + hidden
 *         and keeps its taskbar entry.
 *       - RESTORE  (~220ms): reverse of minimize (taskbar rect -> geometry).
 *     A window whose phase != NONE is drawn via blit_surface_scaled_alpha()
 *     (nearest-neighbor scale about a center point + alpha blend); otherwise
 *     the normal 1:1 opaque blit is used.
 *
 *     FIXED-POINT, NOT FLOAT: scale is an int in 1/256 units (Q8). Easing is
 *     evaluated on integer t in [0,256]. This deliberately avoids gcc emitting
 *     libgcc soft-float helpers (__mulsf3 etc.) that won't link under
 *     -nostdlib.
 *
 *  2. VISUAL POLISH
 *       - Rounded window OUTER corners (radius ~8px): corner pixels of the
 *         frame/border/titlebar are clipped so windows read as rounded.
 *       - Soft layered drop shadow (4 translucent rects, growing + fading,
 *         offset down-right) so windows lift off the wallpaper.
 *       - Wallpaper: tasteful full-screen vertical gradient
 *         (0xFF101826 navy top -> 0xFF1B2A3A bottom), per-scanline.
 *       - Dock: rounded top corners + rounded launcher button + subtle hover
 *         highlight on dock/taskbar items under the cursor.
 *
 * Aether Dark palette: desktop 0xFF1C1C1E, panel/dock 0xFF2C2C2E,
 *   hover 0xFF3A3A3C, text 0xFFFFFFFF, text-dim 0xFFAEAEB2, accent 0xFF0A84FF,
 *   border 0xFF38383A, close 0xFFFF5F57, min 0xFFFFBD47.
 *
 * Build (EXACT -- flags DIRECT on the cmdline, never via an unquoted shell
 * var, or -fno-stack-protector is dropped and it faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/compositor/compositor_m5.c -o /tmp/cm5.o
 *   gcc <same flags> -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/cm5.o /tmp/bf.o -o /tmp/cm5.elf
 *   objdump -d /tmp/cm5.elf | grep "fs:0x28"   # MUST be empty
 */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       size_t;
typedef unsigned long long  uint64_t;
typedef int                 int32_t;
typedef long long           int64_t;

/* ---- syscall numbers (from kernel/include/syscall.h) ---- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_YIELD         15
#define SYS_SPAWN         16
#define SYS_SHMAT         19
#define SYS_SHMDT         20
#define SYS_MSGGET        22
#define SYS_MSGSND        23
#define SYS_MSGRCV        24
#define SYS_MMAP          37
#define SYS_FB_ACQUIRE    39
#define SYS_GET_TICKS_MS  40

/* mmap prot bits */
#define VMM_PROT_READ   0x01
#define VMM_PROT_WRITE  0x02

/* open() flags (from kernel/include/vfs.h + evdev O_NONBLOCK) */
#define O_RDONLY    0x0000
#define O_NONBLOCK  0x0800

/* SysV IPC flags (from kernel/include/ipc.h) */
#define IPC_CREAT   0x0200
#define IPC_NOWAIT  0x0800

/* IPC error codes we care about (kernel returns these) */
#define IPC_ENOMSG  -42   /* no message of desired type (NOWAIT, queue empty) */

/* ---- 3-arg and 6-arg inline syscalls ---- */
static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall" : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny serial diagnostics ---- */
static size_t k_strlen(const char *s) { size_t l = 0; while (s[l]) l++; return l; }
static void print(const char *m) { syscall(SYS_WRITE, 1, (long)m, (long)k_strlen(m)); }
static void print_num(long n) {
    char b[24]; int i = 0;
    if (n < 0) { print("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; syscall(SYS_WRITE, 1, (long)&c, 1); }
}

/* ---- framebuffer geometry returned by SYS_FB_ACQUIRE ---- */
typedef struct { uint64_t vaddr; uint32_t width, height, pitch, bpp; } fb_acquire_t;

/* ====================================================================== *
 *  WAYLAND-LIKE PROTOCOL (local mirror of wl_proto.h)                     *
 * ====================================================================== */
#define WL_COMP_INBOX_KEY   0x434F4D50          /* "COMP" -- server inbox  */
#define WL_REPLY_KEY(pid)   (0x52000000 + (pid))/* per-client event queue  */

/* client -> server message types */
#define WL_REQ_CREATE   1
#define WL_REQ_COMMIT   2
#define WL_REQ_DESTROY  3

/* server -> client message types */
#define WL_EVT_CREATED  1
#define WL_EVT_POINTER  2
#define WL_EVT_KEY      3

#define WL_TITLE_MAX    48

/* mtype is the SysV message type; the kernel treats it as 8 bytes (int64_t)
 * and the payload follows it. msgsz passed to msgsnd/msgrcv is the size of
 * the payload AFTER mtype (verified against kernel/ipc/msgqueue.c). */
typedef struct {
    int64_t  mtype;
    int32_t  pid;
    int32_t  shm_id;
    uint32_t w, h, stride;        /* stride in PIXELS (client pixel pitch)  */
    char     title[WL_TITLE_MAX];
} wl_req_create_t;

typedef struct {
    int64_t  mtype;
    int32_t  win_id;
    uint32_t x, y, w, h;          /* committed damage rect (we mark dirty)  */
} wl_req_commit_t;

typedef struct {
    int64_t  mtype;
    int32_t  win_id;
} wl_req_destroy_t;

typedef struct {
    int64_t  mtype;
    int32_t  win_id;
} wl_evt_created_t;

typedef struct {
    int64_t  mtype;
    int32_t  x, y, buttons;
} wl_evt_pointer_t;

typedef struct {
    int64_t  mtype;
    int32_t  keycode, pressed;
} wl_evt_key_t;

/* A receive buffer large enough for the biggest client->server message
 * (WL_REQ_CREATE). We msgrcv() with type 0 (any) into this union. */
typedef union {
    int64_t          mtype;       /* common first field */
    wl_req_create_t  create;
    wl_req_commit_t  commit;
    wl_req_destroy_t destroy;
    char             raw[128];
} wl_inbox_msg_t;

/* ====================================================================== *
 *  Pixel math (self-contained, from compositor_m2/m3.c)                   *
 * ====================================================================== */
static inline uint32_t blend_pixel(uint32_t src, uint32_t dst) {
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0xFF) return src;
    if (a == 0)    return dst;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t ia = 255 - a;
    uint32_t or_ = (sr * a + dr * ia) / 255;
    uint32_t og  = (sg * a + dg * ia) / 255;
    uint32_t ob  = (sb * a + db * ia) / 255;
    return 0xFF000000u | (or_ << 16) | (og << 8) | ob;
}

/* Blend an opaque-RGB src over dst with an explicit 0..256 alpha (Q8). This is
 * the workhorse for animated (scaled) window content where each pixel inherits
 * the window's animation alpha rather than its own A channel. */
static inline uint32_t blend_pixel_a256(uint32_t src, uint32_t dst, uint32_t a256) {
    if (a256 >= 256) return 0xFF000000u | (src & 0x00FFFFFFu);
    if (a256 == 0)   return dst;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t ia = 256 - a256;
    uint32_t r = (sr * a256 + dr * ia) >> 8;
    uint32_t g = (sg * a256 + dg * ia) >> 8;
    uint32_t b = (sb * a256 + db * ia) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void fill_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x1 = x < 0 ? 0 : x;
    int32_t y1 = y < 0 ? 0 : y;
    int32_t x2 = x + w;
    int32_t y2 = y + h;
    if (x2 > (int32_t)bw) x2 = (int32_t)bw;
    if (y2 > (int32_t)bh) y2 = (int32_t)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (int32_t yy = y1; yy < y2; yy++) {
        uint32_t *row = buf + (uint32_t)yy * stride;
        for (int32_t xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void blend_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                       int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x1 = x < 0 ? 0 : x;
    int32_t y1 = y < 0 ? 0 : y;
    int32_t x2 = x + w;
    int32_t y2 = y + h;
    if (x2 > (int32_t)bw) x2 = (int32_t)bw;
    if (y2 > (int32_t)bh) y2 = (int32_t)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (int32_t yy = y1; yy < y2; yy++) {
        uint32_t *row = buf + (uint32_t)yy * stride;
        for (int32_t xx = x1; xx < x2; xx++) row[xx] = blend_pixel(color, row[xx]);
    }
}

static void stroke_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                        int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fill_rect(buf, bw, bh, stride, x, y, w, 1, color);
    fill_rect(buf, bw, bh, stride, x, y + h - 1, w, 1, color);
    fill_rect(buf, bw, bh, stride, x, y, 1, h, color);
    fill_rect(buf, bw, bh, stride, x + w - 1, y, 1, h, color);
}

/* Filled rounded rectangle: a fill_rect with the four corner pixels clipped
 * (cheap, just enough to read as "rounded" at small sizes). */
static void fill_round_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            int32_t r, uint32_t color) {
    if (r < 1) { fill_rect(buf, bw, bh, stride, x, y, w, h, color); return; }
    /* middle band (full width) */
    fill_rect(buf, bw, bh, stride, x, y + r, w, h - 2 * r, color);
    /* top + bottom bands inset by the corner radius */
    fill_rect(buf, bw, bh, stride, x + r, y, w - 2 * r, r, color);
    fill_rect(buf, bw, bh, stride, x + r, y + h - r, w - 2 * r, r, color);
    /* fill the corner quarter-discs */
    for (int32_t dy = 0; dy < r; dy++) {
        for (int32_t dx = 0; dx < r; dx++) {
            int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
            if (off > (r - 1) * (r - 1)) continue;   /* outside the disc */
            /* top-left */
            fill_rect(buf, bw, bh, stride, x + dx, y + dy, 1, 1, color);
            /* top-right */
            fill_rect(buf, bw, bh, stride, x + w - 1 - dx, y + dy, 1, 1, color);
            /* bottom-left */
            fill_rect(buf, bw, bh, stride, x + dx, y + h - 1 - dy, 1, 1, color);
            /* bottom-right */
            fill_rect(buf, bw, bh, stride, x + w - 1 - dx, y + h - 1 - dy, 1, 1, color);
        }
    }
}

/* Rounded-rect TOP corners only (used for the dock: square bottom, soft top). */
static void fill_round_top_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t r, uint32_t color) {
    if (r < 1) { fill_rect(buf, bw, bh, stride, x, y, w, h, color); return; }
    fill_rect(buf, bw, bh, stride, x, y + r, w, h - r, color);   /* everything below the top corner band */
    fill_rect(buf, bw, bh, stride, x + r, y, w - 2 * r, r, color); /* top band between corners */
    for (int32_t dy = 0; dy < r; dy++) {
        for (int32_t dx = 0; dx < r; dx++) {
            int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
            if (off > (r - 1) * (r - 1)) continue;
            fill_rect(buf, bw, bh, stride, x + dx, y + dy, 1, 1, color);            /* top-left */
            fill_rect(buf, bw, bh, stride, x + w - 1 - dx, y + dy, 1, 1, color);    /* top-right */
        }
    }
}

/* Is (lx,ly) -- coords RELATIVE to a rounded WxH rect with corner radius r --
 * outside the rounded shape? Used to punch the 4 corners out of a window so it
 * reads as rounded. */
static inline int round_corner_clipped(int32_t lx, int32_t ly, int32_t w, int32_t h, int32_t r) {
    if (r < 1) return 0;
    int32_t cx, cy;          /* nearest corner-disc center */
    int inx = 0, iny = 0;
    if (lx < r)            { cx = r;         inx = 1; }
    else if (lx >= w - r)  { cx = w - r - 1; inx = 1; }
    else                     cx = lx;
    if (ly < r)            { cy = r;         iny = 1; }
    else if (ly >= h - r)  { cy = h - r - 1; iny = 1; }
    else                     cy = ly;
    if (!inx || !iny) return 0;             /* only the 4 corner squares matter */
    int32_t ddx = lx - cx, ddy = ly - cy;
    return (ddx * ddx + ddy * ddy) > (r * r);
}

/*
 * Blit a client's shared-memory pixel buffer (ARGB32, row stride in pixels)
 * into the back buffer at (dx,dy), clipped to the screen and (optionally) to
 * a vertical band [clip_y0, clip_y1) so windows never overdraw the chrome.
 * Pixels are copied opaque (clients own their content).
 */
static void blit_surface_clip(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                              const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t sstride,
                              int32_t dx, int32_t dy, int32_t clip_y0, int32_t clip_y1) {
    if (!src || sw == 0 || sh == 0) return;
    for (uint32_t sy = 0; sy < sh; sy++) {
        int32_t py = dy + (int32_t)sy;
        if (py < 0 || py >= (int32_t)bh) continue;
        if (py < clip_y0 || py >= clip_y1) continue;   /* keep out of chrome */
        const uint32_t *srow = src + (uint64_t)sy * sstride;
        uint32_t *drow = buf + (uint32_t)py * stride;
        for (uint32_t sx = 0; sx < sw; sx++) {
            int32_t px = dx + (int32_t)sx;
            if (px < 0 || px >= (int32_t)bw) continue;
            drow[px] = srow[sx] | 0xFF000000u;   /* force opaque */
        }
    }
}

/*
 * ANIMATION BLIT: nearest-neighbor scale a client surface and alpha-blend it.
 *
 * The source (sw x sh) is scaled by (scale_num/scale_den) ABOUT the rect's
 * center (the natural 1:1 destination is dst_x..dst_x+sw, dst_y..dst_y+sh).
 * Each emitted pixel is blended with the constant alpha a256 (0..256). Output
 * is clipped to the screen and to the chrome band [clip_y0, clip_y1).
 *
 * Fixed-point only: scale is num/den integers, alpha is a 0..256 integer. No
 * floats -> no libgcc soft-float helpers.
 */
static void blit_surface_scaled_alpha(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                                       const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t sstride,
                                       int32_t dst_x, int32_t dst_y,
                                       int32_t scale_num, int32_t scale_den, uint32_t a256,
                                       int32_t clip_y0, int32_t clip_y1) {
    if (!src || sw == 0 || sh == 0 || scale_den <= 0 || a256 == 0) return;
    if (scale_num <= 0) return;

    /* scaled destination size */
    int32_t dw = (int32_t)((uint64_t)sw * (uint32_t)scale_num / (uint32_t)scale_den);
    int32_t dh = (int32_t)((uint64_t)sh * (uint32_t)scale_num / (uint32_t)scale_den);
    if (dw <= 0 || dh <= 0) return;

    /* keep the rect centered on the natural (unscaled) center */
    int32_t cx = dst_x + (int32_t)sw / 2;
    int32_t cy = dst_y + (int32_t)sh / 2;
    int32_t ox = cx - dw / 2;
    int32_t oy = cy - dh / 2;

    for (int32_t py = 0; py < dh; py++) {
        int32_t sy = oy + py;
        if (sy < 0 || sy >= (int32_t)bh) continue;
        if (sy < clip_y0 || sy >= clip_y1) continue;
        /* map destination row -> source row (nearest) */
        uint32_t srcy = (uint32_t)((uint64_t)py * (uint32_t)scale_den / (uint32_t)scale_num);
        if (srcy >= sh) srcy = sh - 1;
        const uint32_t *srow = src + (uint64_t)srcy * sstride;
        uint32_t *drow = buf + (uint32_t)sy * stride;
        for (int32_t px = 0; px < dw; px++) {
            int32_t sx = ox + px;
            if (sx < 0 || sx >= (int32_t)bw) continue;
            uint32_t srcx = (uint32_t)((uint64_t)px * (uint32_t)scale_den / (uint32_t)scale_num);
            if (srcx >= sw) srcx = sw - 1;
            drow[sx] = blend_pixel_a256(srow[srcx], drow[sx], a256);
        }
    }
}

/* ====================================================================== *
 *  Theme -- Aether Dark                                                    *
 * ====================================================================== */
#define COL_DESKTOP   0xFF1C1C1Eu
#define COL_PANEL     0xFF2C2C2Eu
#define COL_HOVER     0xFF3A3A3Cu
#define COL_TEXT      0xFFFFFFFFu
#define COL_TEXT_DIM  0xFFAEAEB2u
#define COL_ACCENT    0xFF0A84FFu
#define COL_BORDER    0xFF38383Au

/* Wallpaper gradient endpoints (deep blue-navy). */
#define WALL_TOP        0xFF101826u
#define WALL_BOT        0xFF1B2A3Au

#define TITLEBAR_FOCUS  0xFF3A3A3Cu   /* focused window titlebar             */
#define TITLEBAR_UNFOC  0xFF2C2C2Eu   /* unfocused titlebar                  */
#define BORDER_FOCUS    0xFF0A84FFu   /* focused border (accent)             */
#define BORDER_UNFOC    0xFF38383Au   /* unfocused border                    */
#define CURSOR_FILL     0xFFFFFFFFu
#define CURSOR_EDGE     0xFF000000u
#define BTN_CLOSE       0xFFFF5F57u   /* close box (red)                     */
#define BTN_MIN         0xFFFFBD47u   /* minimize box (amber)                */
#define WIN_PLACEHOLDER 0xFF1C1C1Eu   /* shown if a client has no shm yet    */

#define TITLEBAR_H  28
#define BORDER_W    1
#define WIN_RADIUS  8                 /* rounded window outer-corner radius   */

/* chrome geometry */
#define PANEL_H     28
#define DOCK_H      44
#define LAUNCH_SZ   36                 /* launcher button is 36x36            */
#define TASK_W      120                /* taskbar button width                */
#define TASK_H      32                 /* taskbar button height               */
#define CLOSE_SZ    14                 /* titlebar close box hit/visual size  */
#define MIN_SZ      14                 /* titlebar minimize box hit/visual    */

/* animation tunables (ms) */
#define ANIM_OPEN_MS    180
#define ANIM_CLOSE_MS   150
#define ANIM_MIN_MS     220
#define ANIM_RESTORE_MS 220

/* animation phases */
#define PH_NONE       0
#define PH_OPENING    1
#define PH_CLOSING    2
#define PH_MINIMIZING 3
#define PH_RESTORING  4

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t r = ar + (br - ar) * num / den;
    uint32_t g = ag + (bg - ag) * num / den;
    uint32_t bl = ab + (bb - ab) * num / den;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

/* ---------------------------------------------------------------------- *
 *  Easing -- fixed-point: input + output are Q8 in [0,256].               *
 * ---------------------------------------------------------------------- */
static inline int32_t clamp256(int32_t t) { return t < 0 ? 0 : (t > 256 ? 256 : t); }

/* ease_out_cubic(t) = 1 - (1-t)^3   (Q8 in, Q8 out) */
static int32_t ease_out_cubic(int32_t t) {
    t = clamp256(t);
    int32_t u = 256 - t;                       /* (1-t) in Q8 */
    int32_t u3 = (int32_t)(((int64_t)u * u * u) >> 16);  /* u^3 / 256^2 -> Q8 */
    return clamp256(256 - u3);
}

/* ease_in_cubic(t) = t^3   (Q8 in, Q8 out) */
static int32_t ease_in_cubic(int32_t t) {
    t = clamp256(t);
    return clamp256((int32_t)(((int64_t)t * t * t) >> 16));
}

/* ease_in_out_cubic: <0.5 -> 4t^3, else 1 - (-2t+2)^3 / 2  (Q8). */
static int32_t ease_in_out_cubic(int32_t t) {
    t = clamp256(t);
    if (t < 128) {
        /* 4*t^3 = (t^3) << 2 ; t^3 in Q8 = (t*t*t)>>16 */
        int32_t t3 = (int32_t)(((int64_t)t * t * t) >> 16);
        return clamp256(t3 << 2);
    } else {
        int32_t u = 2 * (256 - t);             /* (-2t+2) in Q8 == 2*(1-t) */
        int32_t u3 = (int32_t)(((int64_t)u * u * u) >> 16);
        return clamp256(256 - (u3 >> 1));
    }
}

/* font (linked from userspace/lib/font/bitfont.c) */
extern int font_draw_string(unsigned int *fbuf, int fstride, int fbw, int fbh,
                            int tx, int ty, const char *s, unsigned int color);
#define FONT_W 8
#define FONT_H 16

/* ---- cursor arrow bitmap (from compositor_m2/m3.c) ---- */
#define CUR_W 12
#define CUR_H 19
static const char *CURSOR[CUR_H] = {
    "#           ", "##          ", "#.#         ", "#..#        ",
    "#...#       ", "#....#      ", "#.....#     ", "#......#    ",
    "#.......#   ", "#........#  ", "#.........# ", "#......#####",
    "#...#..#    ", "#..# #..#   ", "#.#  #..#   ", "##    #..#  ",
    "#     #..#  ", "       #..# ", "       ####  ",
};

static void draw_cursor(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                        int32_t cx, int32_t cy) {
    for (int32_t r = 0; r < CUR_H; r++) {
        const char *row = CURSOR[r];
        for (int32_t c = 0; c < CUR_W; c++) {
            char m = row[c];
            uint32_t col;
            if (m == '#')      col = CURSOR_EDGE;
            else if (m == '.') col = CURSOR_FILL;
            else continue;
            int32_t px = cx + c, py = cy + r;
            if (px < 0 || py < 0 || px >= (int32_t)bw || py >= (int32_t)bh) continue;
            buf[(uint32_t)py * stride + (uint32_t)px] = col;
        }
    }
}

/* ====================================================================== *
 *  Window registry                                                        *
 * ====================================================================== */
#define MAX_WINDOWS 16          /* >= 8 concurrent windows required        */

typedef struct {
    int       used;
    int32_t   win_id;           /* server-assigned id (>0)                 */
    int32_t   client_pid;       /* owner pid (for reply queue)             */
    int32_t   reply_qid;        /* cached WL_REPLY_KEY(pid) queue id, or -1 */
    int32_t   shm_id;           /* client's shm segment id                 */
    uint32_t *pixels;           /* shmat() base of client's pixel buffer   */
    uint64_t  shm_vaddr;        /* attach addr (for shmdt)                 */
    uint32_t  w, h, stride;     /* surface dims; stride in PIXELS          */
    int32_t   x, y;             /* placement of window FRAME (titlebar top)*/
    char      title[WL_TITLE_MAX];
    int       dirty;            /* set on commit (informational)           */

    /* ---- M5 animation state ---- */
    int32_t   phase;            /* PH_NONE / OPENING / CLOSING / MINIMIZING / RESTORING */
    long      anim_start_ms;    /* SYS_GET_TICKS_MS when the phase began    */
    int32_t   anim_dur_ms;      /* phase duration in ms                     */
    int       minimized;        /* sticky: window is parked in the taskbar  */
    int32_t   tb_idx;           /* taskbar slot index captured at minimize  */
} window_t;

static window_t g_windows[MAX_WINDOWS];
static int32_t  g_next_win_id = 1;

/* z-order: index list, back (0) to front (count-1). focus = topmost/front. */
static int32_t g_zorder[MAX_WINDOWS];
static int32_t g_zcount = 0;

/* monotonic spawn counter, for staggering new window placement */
static int32_t g_spawn_seq = 0;

static int find_free_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_windows[i].used) return i;
    return -1;
}

static int slot_by_win_id(int32_t win_id) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_windows[i].used && g_windows[i].win_id == win_id) return i;
    return -1;
}

static void z_push_front(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_zcount; i++)
        if (g_zorder[i] != slot) g_zorder[w++] = g_zorder[i];
    g_zcount = w;
    g_zorder[g_zcount++] = (int32_t)slot;
}

static void z_remove(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_zcount; i++)
        if (g_zorder[i] != slot) g_zorder[w++] = g_zorder[i];
    g_zcount = w;
}

/* Topmost focusable window slot (skips minimized/closing), or -1. Used for the
 * panel title and input forwarding so a parked/closing window never "owns" the
 * keyboard. */
static int focused_slot(void) {
    for (int32_t i = g_zcount - 1; i >= 0; i--) {
        int s = (int)g_zorder[i];
        if (s < 0 || s >= MAX_WINDOWS || !g_windows[s].used) continue;
        if (g_windows[s].minimized) continue;
        if (g_windows[s].phase == PH_CLOSING || g_windows[s].phase == PH_MINIMIZING) continue;
        return s;
    }
    return -1;
}

/* Resolve (and cache) a client's reply queue. */
static int32_t client_reply_qid(window_t *win) {
    if (win->reply_qid >= 0) return win->reply_qid;
    long qid = sc6(SYS_MSGGET, (long)WL_REPLY_KEY(win->client_pid),
                   (long)(IPC_CREAT | 0666), 0, 0, 0, 0);
    if (qid >= 0) win->reply_qid = (int32_t)qid;
    return (int32_t)qid;
}

/* ====================================================================== *
 *  Dock / taskbar layout (forward decls -- the animation engine needs the *
 *  taskbar button geometry to compute the minimize/restore target rect).  *
 * ====================================================================== */
static uint32_t g_fb_w = 0, g_fb_h = 0;   /* cached for placement clamping */

static int32_t dock_top(uint32_t h)        { return (int32_t)h - DOCK_H; }
static int32_t launcher_x(void)            { return 8; }
static int32_t launcher_y(uint32_t h)      { return dock_top(h) + (DOCK_H - LAUNCH_SZ) / 2; }
static int32_t taskbtn_x(int idx)          { return launcher_x() + LAUNCH_SZ + 12 + idx * (TASK_W + 8); }
static int32_t taskbtn_y(uint32_t h)       { return dock_top(h) + (DOCK_H - TASK_H) / 2; }

/* Visible taskbar index for a slot (matches render_dock's slot-order layout). */
static int taskbar_index_of(int slot) {
    int idx = 0;
    for (int s = 0; s < MAX_WINDOWS; s++) {
        if (!g_windows[s].used) continue;
        if (s == slot) return idx;
        idx++;
    }
    return -1;
}

/* ====================================================================== *
 *  Animation engine                                                       *
 * ====================================================================== */

/* Compute raw linear t (Q8 in [0,256]) for a slot's current phase at `now`. */
static int32_t anim_linear_t(window_t *win, long now) {
    if (win->anim_dur_ms <= 0) return 256;
    long dt = now - win->anim_start_ms;
    if (dt <= 0) return 0;
    if (dt >= win->anim_dur_ms) return 256;
    return (int32_t)((dt * 256) / win->anim_dur_ms);
}

static void anim_begin(window_t *win, int32_t phase, int32_t dur_ms, long now) {
    win->phase = phase;
    win->anim_dur_ms = dur_ms;
    win->anim_start_ms = now;
}

/*
 * Per-frame animation advancement. Resolves completed phases:
 *   OPENING    -> NONE (settled full-size opaque)
 *   CLOSING    -> destroy the slot (shmdt + free)
 *   MINIMIZING -> NONE + minimized=1 (parked, hidden, taskbar entry kept)
 *   RESTORING  -> NONE + minimized=0
 * Called once per frame BEFORE composite().
 */
static void destroy_slot(int slot);    /* fwd */

static void anim_tick(long now) {
    for (int s = 0; s < MAX_WINDOWS; s++) {
        window_t *win = &g_windows[s];
        if (!win->used || win->phase == PH_NONE) continue;
        int32_t t = anim_linear_t(win, now);
        if (t < 256) continue;                 /* still animating */
        /* phase finished */
        switch (win->phase) {
            case PH_OPENING:
                win->phase = PH_NONE;
                break;
            case PH_CLOSING:
                destroy_slot(s);                /* tears down + clears phase */
                break;
            case PH_MINIMIZING:
                win->phase = PH_NONE;
                win->minimized = 1;             /* park in taskbar */
                break;
            case PH_RESTORING:
                win->phase = PH_NONE;
                win->minimized = 0;
                break;
            default:
                win->phase = PH_NONE;
                break;
        }
    }
}

/* ====================================================================== *
 *  Compositing                                                            *
 * ====================================================================== */

/* Wallpaper: full-screen vertical navy gradient (per scanline). */
static void render_desktop(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t c = lerp_color(WALL_TOP, WALL_BOT, y, h ? h - 1 : 1);
        uint32_t *r = buf + y * stride;
        for (uint32_t x = 0; x < w; x++) r[x] = c;
    }
}

/* Soft layered drop shadow: 4 translucent rounded rects, growing outward and
 * fading, offset down-right, so the window appears to float. */
static void draw_soft_shadow(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                             int32_t fx, int32_t fy, int32_t fw, int32_t fh) {
    static const int32_t grow[4]  = { 2, 5, 9, 14 };
    static const uint32_t a[4]    = { 0x55000000u, 0x33000000u, 0x1E000000u, 0x10000000u };
    static const int32_t offy[4]  = { 2, 4, 6, 9 };
    for (int i = 3; i >= 0; i--) {
        int32_t g = grow[i];
        int32_t sx = fx - g + 2;            /* slight right bias */
        int32_t sy = fy - g + offy[i];      /* down bias */
        int32_t sw = fw + 2 * g;
        int32_t sh = fh + 2 * g;
        /* blended rounded rect (manual: blend the round-rect span set) */
        int32_t r = WIN_RADIUS + g;
        /* middle band */
        blend_rect(buf, w, h, stride, sx, sy + r, sw, sh - 2 * r, a[i]);
        blend_rect(buf, w, h, stride, sx + r, sy, sw - 2 * r, r, a[i]);
        blend_rect(buf, w, h, stride, sx + r, sy + sh - r, sw - 2 * r, r, a[i]);
        /* corner discs */
        for (int32_t dy = 0; dy < r; dy++) {
            for (int32_t dx = 0; dx < r; dx++) {
                int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
                if (off > (r - 1) * (r - 1)) continue;
                blend_rect(buf, w, h, stride, sx + dx, sy + dy, 1, 1, a[i]);
                blend_rect(buf, w, h, stride, sx + sw - 1 - dx, sy + dy, 1, 1, a[i]);
                blend_rect(buf, w, h, stride, sx + dx, sy + sh - 1 - dy, 1, 1, a[i]);
                blend_rect(buf, w, h, stride, sx + sw - 1 - dx, sy + sh - 1 - dy, 1, 1, a[i]);
            }
        }
    }
}

/*
 * Draw a settled (phase==NONE, non-minimized) window with full decorations,
 * rounded outer corners + soft shadow. frame_x/frame_y is the top-left of the
 * titlebar; the client area sits below it. Content is clipped to the chrome-
 * free band [PANEL_H, H-DOCK_H).
 */
static void render_window_static(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                                 window_t *win, int focused) {
    int32_t cw = (int32_t)win->w;
    int32_t ch = (int32_t)win->h;
    int32_t fx = win->x;
    int32_t fy = win->y;
    int32_t client_x = fx;
    int32_t client_y = fy + TITLEBAR_H;

    int32_t clip_y0 = PANEL_H;
    int32_t clip_y1 = (int32_t)h - DOCK_H;

    uint32_t tb_col  = focused ? TITLEBAR_FOCUS : TITLEBAR_UNFOC;
    uint32_t bd_col  = focused ? BORDER_FOCUS   : BORDER_UNFOC;

    int32_t full_w = cw + 2 * BORDER_W;
    int32_t full_h = ch + TITLEBAR_H + 2 * BORDER_W;
    int32_t outer_x = fx - BORDER_W;
    int32_t outer_y = fy - BORDER_W;

    /* soft drop shadow */
    draw_soft_shadow(buf, w, h, stride, outer_x, outer_y, full_w, full_h);

    /* rounded border + titlebar:
     *   1) titlebar fill (rounded TOP corners)
     *   2) body/client fill below it
     *   3) 1px border stroke, then punch rounded corners. */
    fill_round_top_rect(buf, w, h, stride, fx, fy, cw, TITLEBAR_H, WIN_RADIUS, tb_col);

    /* close + minimize boxes (right-aligned: close rightmost, min to its left) */
    int32_t close_x = fx + cw - CLOSE_SZ - 8;
    int32_t close_y = fy + (TITLEBAR_H - CLOSE_SZ) / 2;
    int32_t min_x   = close_x - MIN_SZ - 6;
    int32_t min_y   = fy + (TITLEBAR_H - MIN_SZ) / 2;
    fill_round_rect(buf, w, h, stride, close_x, close_y, CLOSE_SZ, CLOSE_SZ, 3, BTN_CLOSE);
    fill_round_rect(buf, w, h, stride, min_x,   min_y,   MIN_SZ,   MIN_SZ,   3, BTN_MIN);
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     close_x + (CLOSE_SZ - FONT_W) / 2,
                     close_y + (CLOSE_SZ - FONT_H) / 2, "x", COL_TEXT);
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     min_x + (MIN_SZ - FONT_W) / 2,
                     min_y + (MIN_SZ - FONT_H) / 2, "-", 0xFF000000u);

    /* window title text (bitmap font) */
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     fx + 8, fy + (TITLEBAR_H - FONT_H) / 2, win->title,
                     focused ? COL_TEXT : COL_TEXT_DIM);

    /* client surface */
    if (win->pixels) {
        blit_surface_clip(buf, w, h, stride,
                          win->pixels, win->w, win->h, win->stride,
                          client_x, client_y, clip_y0, clip_y1);
    } else {
        int32_t py0 = client_y < clip_y0 ? clip_y0 : client_y;
        int32_t py1 = client_y + ch;
        if (py1 > clip_y1) py1 = clip_y1;
        if (py1 > py0)
            fill_rect(buf, w, h, stride, client_x, py0, cw, py1 - py0, WIN_PLACEHOLDER);
    }

    /* 1px border around the whole frame */
    stroke_rect(buf, w, h, stride, outer_x, outer_y, full_w, full_h, bd_col);

    /* punch rounded OUTER corners: overwrite the corner pixels of the whole
     * frame rect with the wallpaper color underneath. We recompute the
     * wallpaper per scanline so the corners blend into the gradient. */
    for (int32_t ly = 0; ly < full_h; ly++) {
        int32_t py = outer_y + ly;
        if (py < 0 || py >= (int32_t)h) continue;
        uint32_t wall = lerp_color(WALL_TOP, WALL_BOT, (uint32_t)py, h ? h - 1 : 1);
        for (int32_t lx = 0; lx < full_w; lx++) {
            if (!round_corner_clipped(lx, ly, full_w, full_h, WIN_RADIUS)) continue;
            int32_t px = outer_x + lx;
            if (px < 0 || px >= (int32_t)w) continue;
            if (py < PANEL_H || py >= (int32_t)h - DOCK_H) continue;  /* respect chrome band */
            buf[(uint32_t)py * stride + (uint32_t)px] = wall;
        }
    }
}

/*
 * Draw an ANIMATING window (phase != NONE). We compute an eased progress, a
 * scale (Q8) and an alpha (0..256), and a draw origin. The whole window
 * (titlebar + client) is captured conceptually as the client surface plus
 * decorations; for simplicity + speed we animate the CLIENT pixel buffer
 * scaled/alpha-blended, and draw the titlebar chrome at matching alpha at the
 * scaled position. This keeps the effect crisp without an offscreen capture.
 */
static void render_window_anim(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                               window_t *win, int focused, long now) {
    int32_t cw = (int32_t)win->w;
    int32_t ch = (int32_t)win->h;
    int32_t clip_y0 = PANEL_H;
    int32_t clip_y1 = (int32_t)h - DOCK_H;

    int32_t lin = anim_linear_t(win, now);

    /* geometry: settled frame top-left */
    int32_t fx = win->x, fy = win->y;

    int32_t scale_num = 256, scale_den = 256;     /* Q8 scale = 1.0 */
    uint32_t alpha = 256;
    int32_t draw_fx = fx, draw_fy = fy;

    if (win->phase == PH_OPENING) {
        int32_t e = ease_out_cubic(lin);
        /* scale 0.90 -> 1.00 : 230/256 .. 256/256 */
        scale_num = 230 + (256 - 230) * e / 256;
        alpha = (uint32_t)e;                       /* 0 -> 256 */
    } else if (win->phase == PH_CLOSING) {
        int32_t e = ease_in_cubic(lin);
        /* scale 1.00 -> 0.90 */
        scale_num = 256 - (256 - 230) * e / 256;
        alpha = (uint32_t)(256 - e);               /* 256 -> 0 */
    } else if (win->phase == PH_MINIMIZING || win->phase == PH_RESTORING) {
        /* shrink + slide toward (or out of) the taskbar button. */
        int32_t e;
        int32_t idx = (win->tb_idx >= 0) ? win->tb_idx : taskbar_index_of((int)(win - g_windows));
        int32_t tb_x = taskbtn_x(idx >= 0 ? idx : 0);
        int32_t tb_y = taskbtn_y(g_fb_h);
        /* target: small window centered on the taskbar button */
        int32_t tgt_fx = tb_x + (TASK_W - cw / 6) / 2;     /* shrink to ~1/6 then alpha-out */
        int32_t tgt_fy = tb_y;
        int32_t min_num = 96;                              /* shrink to ~0.375 */
        if (win->phase == PH_MINIMIZING) {
            e = ease_in_out_cubic(lin);
            scale_num = 256 - (256 - min_num) * e / 256;
            draw_fx = fx + (tgt_fx - fx) * e / 256;
            draw_fy = fy + (tgt_fy - fy) * e / 256;
            alpha = (uint32_t)(256 - e * 200 / 256);       /* fade toward ~22% near end */
            if (alpha > 256) alpha = 256;
        } else { /* RESTORING: reverse */
            e = ease_in_out_cubic(lin);
            int32_t r = 256 - e;                            /* 256 -> 0 */
            scale_num = 256 - (256 - min_num) * r / 256;
            draw_fx = fx + (tgt_fx - fx) * r / 256;
            draw_fy = fy + (tgt_fy - fy) * r / 256;
            alpha = (uint32_t)(256 - r * 200 / 256);
            if (alpha > 256) alpha = 256;
        }
        if (idx < 0) { /* no taskbar slot (shouldn't happen) -> just fade */
            draw_fx = fx; draw_fy = fy;
        }
    }

    if (alpha == 0) return;

    int32_t draw_client_x = draw_fx;
    int32_t draw_client_y = draw_fy + TITLEBAR_H;

    /* Animated titlebar chrome: scale the titlebar fill the same as content by
     * drawing a scaled rounded rect. For simplicity (and to avoid a second
     * scaler) we draw the titlebar at the *scaled width* anchored at the scaled
     * origin, then alpha-blend with the chrome color. */
    int32_t sw = (int32_t)((uint64_t)cw * (uint32_t)scale_num / (uint32_t)scale_den);
    int32_t sh_tb = (int32_t)((uint64_t)TITLEBAR_H * (uint32_t)scale_num / (uint32_t)scale_den);
    if (sw < 1) sw = 1;
    if (sh_tb < 1) sh_tb = 1;
    /* center the scaled titlebar over the natural titlebar center */
    int32_t tb_cx = draw_fx + cw / 2;
    int32_t tb_cy = draw_fy + TITLEBAR_H / 2;
    int32_t tb_x = tb_cx - sw / 2;
    int32_t tb_y = tb_cy - sh_tb / 2;
    uint32_t tb_col = focused ? TITLEBAR_FOCUS : TITLEBAR_UNFOC;
    /* clamp the titlebar draw into the chrome band */
    {
        int32_t yy0 = tb_y, yy1 = tb_y + sh_tb;
        if (yy0 < clip_y0) yy0 = clip_y0;
        if (yy1 > clip_y1) yy1 = clip_y1;
        if (yy1 > yy0) {
            /* alpha-blend the titlebar fill */
            int32_t x1 = tb_x < 0 ? 0 : tb_x;
            int32_t x2 = tb_x + sw;
            if (x2 > (int32_t)w) x2 = (int32_t)w;
            for (int32_t yy = yy0; yy < yy1; yy++) {
                uint32_t *drow = buf + (uint32_t)yy * stride;
                for (int32_t xx = x1; xx < x2; xx++)
                    drow[xx] = blend_pixel_a256(tb_col, drow[xx], alpha);
            }
        }
    }

    /* Animated client content: scaled + alpha-blended about its center. */
    if (win->pixels) {
        blit_surface_scaled_alpha(buf, w, h, stride,
                                  win->pixels, win->w, win->h, win->stride,
                                  draw_client_x, draw_client_y,
                                  scale_num, scale_den, alpha,
                                  clip_y0, clip_y1);
    }
}

/* ---------------------------------------------------------------------- *
 *  Chrome: top panel + bottom dock (always drawn on top of windows)      *
 * ---------------------------------------------------------------------- */

/* Format ms uptime as HH:MM:SS into out (>=9 bytes). */
static void format_clock(long ms, char *out) {
    long total = ms / 1000;
    long ss = total % 60;
    long mm = (total / 60) % 60;
    long hh = (total / 3600) % 100;   /* wrap hours at 100 to keep 2 digits */
    out[0] = (char)('0' + (hh / 10)); out[1] = (char)('0' + (hh % 10));
    out[2] = ':';
    out[3] = (char)('0' + (mm / 10)); out[4] = (char)('0' + (mm % 10));
    out[5] = ':';
    out[6] = (char)('0' + (ss / 10)); out[7] = (char)('0' + (ss % 10));
    out[8] = '\0';
}

/* Copy a (possibly long) title into a fixed buffer, truncating with an
 * ellipsis-ish ".." if it would overflow `maxchars` glyphs. */
static void truncate_title(const char *src, char *dst, int maxchars) {
    int i = 0;
    while (src[i] && i < maxchars) { dst[i] = src[i]; i++; }
    if (src[i] && maxchars >= 2) { dst[maxchars - 1] = '.'; dst[maxchars - 2] = '.'; }
    dst[i] = '\0';
    if (i >= maxchars) dst[maxchars] = '\0';
}

static void render_panel(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    /* panel background + 1px bottom border */
    fill_rect(buf, w, h, stride, 0, 0, (int32_t)w, PANEL_H, COL_PANEL);
    fill_rect(buf, w, h, stride, 0, PANEL_H - 1, (int32_t)w, 1, COL_BORDER);

    /* left: focused window title (or product name) */
    const char *title = "AutomationOS";
    int slot = focused_slot();
    if (slot >= 0 && g_windows[slot].used && g_windows[slot].title[0])
        title = g_windows[slot].title;
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     12, (PANEL_H - FONT_H) / 2, title, COL_TEXT);

    /* right: clock HH:MM:SS */
    char clk[9];
    long ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    format_clock(ms, clk);
    int clk_w = (int)k_strlen(clk) * FONT_W;
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     (int)w - clk_w - 12, (PANEL_H - FONT_H) / 2, clk, COL_TEXT);
}

static void render_dock(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                        int32_t cur_x, int32_t cur_y) {
    int32_t dy = dock_top(h);
    /* dock background: rounded TOP corners + 1px top border */
    fill_round_top_rect(buf, w, h, stride, 0, dy, (int32_t)w, DOCK_H, 10, COL_PANEL);
    fill_rect(buf, w, h, stride, 0, dy, (int32_t)w, 1, COL_BORDER);

    /* launcher button: rounded accent square labeled "T"; subtle hover lift */
    int32_t lx = launcher_x(), ly = launcher_y(h);
    int launch_hover = (cur_x >= lx && cur_x < lx + LAUNCH_SZ &&
                        cur_y >= ly && cur_y < ly + LAUNCH_SZ);
    if (launch_hover)
        fill_round_rect(buf, w, h, stride, lx - 1, ly - 1, LAUNCH_SZ + 2, LAUNCH_SZ + 2, 9, COL_HOVER);
    fill_round_rect(buf, w, h, stride, lx, ly, LAUNCH_SZ, LAUNCH_SZ, 8, COL_ACCENT);
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     lx + (LAUNCH_SZ - FONT_W) / 2, ly + (LAUNCH_SZ - FONT_H) / 2,
                     "T", COL_TEXT);

    /* taskbar: one button per window (minimized windows keep their button). */
    int focused = focused_slot();
    int idx = 0;
    for (int s = 0; s < MAX_WINDOWS; s++) {
        if (!g_windows[s].used) continue;
        int32_t bx = taskbtn_x(idx);
        int32_t by = taskbtn_y(h);
        if (bx + TASK_W > (int32_t)w) break;        /* ran out of dock space */
        int hover = (cur_x >= bx && cur_x < bx + TASK_W &&
                     cur_y >= by && cur_y < by + TASK_H);
        uint32_t bg = (s == focused) ? COL_HOVER : (hover ? COL_HOVER : COL_PANEL);
        fill_round_rect(buf, w, h, stride, bx, by, TASK_W, TASK_H, 4, bg);
        stroke_rect(buf, w, h, stride, bx, by, TASK_W, TASK_H, COL_BORDER);
        /* minimized windows get a dim accent dot to show they're parked */
        if (g_windows[s].minimized)
            fill_round_rect(buf, w, h, stride, bx + 4, by + TASK_H / 2 - 2, 4, 4, 2, COL_ACCENT);
        char t[20];
        truncate_title(g_windows[s].title[0] ? g_windows[s].title : "window",
                       t, (TASK_W - 12) / FONT_W);
        font_draw_string(buf, (int)stride, (int)w, (int)h,
                         bx + (g_windows[s].minimized ? 12 : 6),
                         by + (TASK_H - FONT_H) / 2, t,
                         (s == focused) ? COL_TEXT : COL_TEXT_DIM);
        idx++;
    }
}

static void composite(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                      int32_t cursor_x, int32_t cursor_y, long now) {
    render_desktop(buf, w, h, stride);

    /* windows, back to front */
    int top = focused_slot();
    for (int32_t i = 0; i < g_zcount; i++) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
        if (win->phase != PH_NONE) {
            render_window_anim(buf, w, h, stride, win, slot == top, now);
        } else if (win->minimized) {
            continue;                       /* parked: only its taskbar button shows */
        } else {
            render_window_static(buf, w, h, stride, win, slot == top);
        }
    }

    /* always-on-top chrome (drawn AFTER windows) */
    render_panel(buf, w, h, stride);
    render_dock(buf, w, h, stride, cursor_x, cursor_y);

    /* cursor on very top */
    draw_cursor(buf, w, h, stride, cursor_x, cursor_y);
}

static void present(uint32_t *fb, uint32_t *back, uint32_t h, uint32_t stride) {
    uint32_t total = h * stride;
    for (uint32_t i = 0; i < total; i++) fb[i] = back[i];
}

/* ====================================================================== *
 *  Client requests                                                        *
 * ====================================================================== */

/* Clamp a window frame so its titlebar stays inside the chrome-free region. */
static void clamp_window(window_t *win) {
    int32_t min_y = PANEL_H + 4;
    int32_t max_y = (int32_t)g_fb_h - DOCK_H - TITLEBAR_H - 8;
    int32_t max_x = (int32_t)g_fb_w - (int32_t)win->w - 4;
    if (win->y < min_y) win->y = min_y;
    if (max_y >= min_y && win->y > max_y) win->y = max_y;
    if (win->x < 4) win->x = 4;
    if (max_x >= 4 && win->x > max_x) win->x = max_x;
}

static void handle_create(const wl_req_create_t *req) {
    int slot = find_free_slot();
    if (slot < 0) {
        print("[COMP] window limit reached, rejecting create from pid=");
        print_num(req->pid); print("\n");
        return;
    }

    window_t *win = &g_windows[slot];
    for (size_t i = 0; i < sizeof(*win); i++) ((char *)win)[i] = 0;
    win->used       = 1;
    win->win_id     = g_next_win_id++;
    win->client_pid = req->pid;
    win->reply_qid  = -1;
    win->shm_id     = req->shm_id;
    win->w          = req->w;
    win->h          = req->h;
    /* protocol carries stride in BYTES; our blit uses stride in PIXELS. */
    win->stride     = req->stride ? (req->stride / 4u) : req->w;
    win->dirty      = 1;
    win->phase      = PH_NONE;
    win->minimized  = 0;
    win->tb_idx     = -1;
    for (int i = 0; i < WL_TITLE_MAX; i++) win->title[i] = req->title[i];
    win->title[WL_TITLE_MAX - 1] = '\0';

    /* zero-copy attach: map the client's shm segment into THIS process. */
    long addr = sc6(SYS_SHMAT, (long)req->shm_id, 0, 0, 0, 0, 0);
    if (addr > 0) {
        win->shm_vaddr = (uint64_t)addr;
        win->pixels    = (uint32_t *)addr;
    } else {
        win->shm_vaddr = 0;
        win->pixels    = 0;
        print("[COMP] shmat FAILED shm_id="); print_num(req->shm_id);
        print(" r="); print_num(addr); print("\n");
    }

    /* staggered placement inside the chrome-free region */
    int32_t step = g_spawn_seq++ % 6;
    win->x = 80 + step * 40;
    win->y = PANEL_H + 24 + step * 36;
    clamp_window(win);

    z_push_front(slot);            /* new window becomes focused (topmost)  */

    /* M5: begin the OPEN animation (scale 0.90->1.00, alpha 0->256). */
    anim_begin(win, PH_OPENING, ANIM_OPEN_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));

    int32_t qid = client_reply_qid(win);
    if (qid >= 0) {
        wl_evt_created_t ev;
        ev.mtype  = WL_EVT_CREATED;
        ev.win_id = win->win_id;
        long r = sc6(SYS_MSGSND, qid, (long)&ev,
                     (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
        if (r < 0) { print("[COMP] msgsnd CREATED failed r="); print_num(r); print("\n"); }
    } else {
        print("[COMP] no reply queue for pid="); print_num(req->pid); print("\n");
    }

    print("[COMP] client connected win="); print_num(win->win_id);
    print(" "); print_num((long)win->w); print("x"); print_num((long)win->h);
    print(" pid="); print_num(win->client_pid); print("\n");
}

static void handle_commit(const wl_req_commit_t *req) {
    int slot = slot_by_win_id(req->win_id);
    if (slot < 0) return;
    window_t *win = &g_windows[slot];
    win->dirty = 1;
    if (!win->pixels && win->shm_id > 0) {
        long addr = sc6(SYS_SHMAT, (long)win->shm_id, 0, 0, 0, 0, 0);
        if (addr > 0) { win->shm_vaddr = (uint64_t)addr; win->pixels = (uint32_t *)addr; }
    }
    (void)req;
}

/* Tear down a window slot: detach shm, remove from z-order, free the slot. */
static void destroy_slot(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->shm_vaddr) {
        sc6(SYS_SHMDT, (long)win->shm_vaddr, 0, 0, 0, 0, 0);
        win->shm_vaddr = 0;
        win->pixels    = 0;
    }
    z_remove(slot);
    win->used       = 0;
    win->phase      = PH_NONE;
    win->minimized  = 0;
}

/* Begin the CLOSE animation; the slot is freed when the animation completes
 * (in anim_tick). If a client sends DESTROY, we honor it the same way so the
 * exit is always animated. */
static void begin_close(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->phase == PH_CLOSING) return;          /* already closing */
    win->minimized = 0;                            /* un-park so it animates from where it is */
    anim_begin(win, PH_CLOSING, ANIM_CLOSE_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
}

static void handle_destroy(const wl_req_destroy_t *req) {
    int slot = slot_by_win_id(req->win_id);
    if (slot < 0) return;
    int32_t id = g_windows[slot].win_id;
    begin_close(slot);
    print("[COMP] client disconnected win="); print_num(id); print("\n");
}

static void drain_inbox(int32_t inbox_qid) {
    wl_inbox_msg_t msg;
    for (;;) {
        long r = sc6(SYS_MSGRCV, inbox_qid, (long)&msg,
                     (long)(sizeof(msg) - sizeof(int64_t)), 0, (long)IPC_NOWAIT, 0);
        if (r < 0) break;
        switch (msg.mtype) {
            case WL_REQ_CREATE:  handle_create(&msg.create);   break;
            case WL_REQ_COMMIT:  handle_commit(&msg.commit);   break;
            case WL_REQ_DESTROY: handle_destroy(&msg.destroy); break;
            default: break;
        }
    }
}

/* ====================================================================== *
 *  Input: /dev/input/event0 (kbd) + event1 (mouse), polled non-blocking  *
 * ====================================================================== */
typedef struct {
    uint64_t timestamp;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input_event_t;

#define EV_KEY  0
#define EV_REL  1
#define REL_X   0
#define REL_Y   1
#define BTN_LEFT_CODE    0x110
#define BTN_RIGHT_CODE   0x111
#define BTN_MIDDLE_CODE  0x112
#define EVENTS_PER_READ  32

static int32_t g_kbd_fd   = -1;
static int32_t g_mouse_fd = -1;

/* live pointer state */
static int32_t g_cursor_x = 0;
static int32_t g_cursor_y = 0;
static int32_t g_buttons  = 0;   /* bit0=left bit1=right bit2=middle */

static void send_pointer_to_focus(void) {
    int slot = focused_slot();
    if (slot < 0) return;
    window_t *win = &g_windows[slot];
    int32_t qid = client_reply_qid(win);
    if (qid < 0) return;
    wl_evt_pointer_t ev;
    ev.mtype   = WL_EVT_POINTER;
    ev.x       = g_cursor_x - win->x;
    ev.y       = g_cursor_y - (win->y + TITLEBAR_H);
    ev.buttons = g_buttons;
    sc6(SYS_MSGSND, qid, (long)&ev, (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
}

static void send_key_to_focus(int32_t keycode, int32_t pressed) {
    int slot = focused_slot();
    if (slot < 0) return;
    window_t *win = &g_windows[slot];
    int32_t qid = client_reply_qid(win);
    if (qid < 0) return;
    wl_evt_key_t ev;
    ev.mtype   = WL_EVT_KEY;
    ev.keycode = keycode;
    ev.pressed = pressed;
    sc6(SYS_MSGSND, qid, (long)&ev, (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
}

/* Read + process all currently-buffered events from one fd. keyboard==1
 * routes EV_KEY presses to the focused window; else the fd is the mouse. */
static void pump_input(int32_t fd, int keyboard, uint32_t W, uint32_t H) {
    if (fd < 0) return;
    input_event_t evs[EVENTS_PER_READ];
    long n = syscall(SYS_READ, fd, (long)evs, (long)sizeof(evs));
    if (n <= 0) return;
    long count = n / (long)sizeof(input_event_t);
    int pointer_changed = 0;

    for (long i = 0; i < count; i++) {
        input_event_t *e = &evs[i];
        if (keyboard) {
            if (e->type == EV_KEY)
                send_key_to_focus((int32_t)e->code, e->value != 0 ? 1 : 0);
            continue;
        }
        if (e->type == EV_REL) {
            if (e->code == REL_X) { g_cursor_x += e->value; pointer_changed = 1; }
            else if (e->code == REL_Y) { g_cursor_y += e->value; pointer_changed = 1; }
        } else if (e->type == EV_KEY) {
            int32_t bit = -1;
            if (e->code == BTN_LEFT_CODE)   bit = 0;
            else if (e->code == BTN_RIGHT_CODE)  bit = 1;
            else if (e->code == BTN_MIDDLE_CODE) bit = 2;
            if (bit >= 0) {
                if (e->value) g_buttons |= (1 << bit);
                else          g_buttons &= ~(1 << bit);
                pointer_changed = 1;
            }
        }
    }

    if (!keyboard && pointer_changed) {
        if (g_cursor_x < 0) g_cursor_x = 0;
        if (g_cursor_y < 0) g_cursor_y = 0;
        if (g_cursor_x >= (int32_t)W) g_cursor_x = (int32_t)W - 1;
        if (g_cursor_y >= (int32_t)H) g_cursor_y = (int32_t)H - 1;
        send_pointer_to_focus();
    }
}

/* ---------------------------------------------------------------------- *
 *  Desktop-shell mouse interaction (hit-testing + drag-move)             *
 * ---------------------------------------------------------------------- */
static int32_t g_prev_buttons = 0;     /* for left-click edge detection    */
static int     g_drag_slot   = -1;     /* slot being drag-moved, or -1     */
static int32_t g_drag_dx = 0, g_drag_dy = 0;  /* cursor-to-frame offset    */

static int point_in(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void spawn_terminal(void) {
    print("[SHELL] launch terminal\n");
    long r = syscall(SYS_SPAWN, (long)"sbin/terminal", 0, 0);
    if (r < 0) { print("[SHELL] spawn failed r="); print_num(r); print("\n"); }
}

/* Focus + raise a window slot. */
static void focus_window(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    z_push_front(slot);
    print("[SHELL] focus win "); print_num(g_windows[slot].win_id); print("\n");
}

/* Begin minimize (park toward taskbar). */
static void begin_minimize(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->minimized || win->phase == PH_MINIMIZING || win->phase == PH_CLOSING) return;
    win->tb_idx = taskbar_index_of(slot);          /* capture target taskbar slot */
    anim_begin(win, PH_MINIMIZING, ANIM_MIN_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
    print("[SHELL] minimize win "); print_num(win->win_id); print("\n");
}

/* Begin restore (un-park from taskbar). */
static void begin_restore(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->phase == PH_RESTORING || win->phase == PH_CLOSING) return;
    win->tb_idx = taskbar_index_of(slot);
    win->minimized = 0;                            /* visible again, animating in */
    anim_begin(win, PH_RESTORING, ANIM_RESTORE_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
    z_push_front(slot);                            /* raise + focus on restore */
    print("[SHELL] restore win "); print_num(win->win_id); print("\n");
}

/*
 * Resolve all left-mouse interaction. Called once per frame AFTER pump_input
 * has updated g_cursor_x/y + g_buttons. Uses edge detection (prev vs current
 * left-button bit) so a click fires exactly once on press.
 */
static void handle_mouse(uint32_t W, uint32_t H) {
    int32_t left      = g_buttons & 1;
    int32_t prev_left = g_prev_buttons & 1;
    int32_t press     = left && !prev_left;     /* rising edge = click      */
    int32_t release   = !left && prev_left;     /* falling edge             */
    int32_t cx = g_cursor_x, cy = g_cursor_y;

    /* ---- active drag-move: track cursor while left held ---- */
    if (g_drag_slot >= 0) {
        if (left && g_windows[g_drag_slot].used) {
            g_windows[g_drag_slot].x = cx - g_drag_dx;
            g_windows[g_drag_slot].y = cy - g_drag_dy;
            clamp_window(&g_windows[g_drag_slot]);
        }
        if (release) {
            print("[SHELL] move win ");
            print_num(g_windows[g_drag_slot].used ? g_windows[g_drag_slot].win_id : -1);
            print("\n");
            g_drag_slot = -1;
        }
        g_prev_buttons = g_buttons;
        return;                                  /* drag owns the pointer   */
    }

    if (!press) { g_prev_buttons = g_buttons; return; }

    /* ---------- click hit-testing (priority order) ---------- */

    /* 1) dock launcher button */
    {
        int32_t lx = launcher_x(), ly = launcher_y(H);
        if (point_in(cx, cy, lx, ly, LAUNCH_SZ, LAUNCH_SZ)) {
            spawn_terminal();
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* 2) taskbar buttons: focus+raise a normal window, or RESTORE a minimized
     *    one. Iterate visible windows in slot order to match render_dock. */
    {
        int idx = 0;
        for (int s = 0; s < MAX_WINDOWS; s++) {
            if (!g_windows[s].used) continue;
            int32_t bx = taskbtn_x(idx), by = taskbtn_y(H);
            if (bx + TASK_W > (int32_t)W) break;
            if (point_in(cx, cy, bx, by, TASK_W, TASK_H)) {
                if (g_windows[s].minimized || g_windows[s].phase == PH_MINIMIZING)
                    begin_restore(s);
                else
                    focus_window(s);
                g_prev_buttons = g_buttons;
                return;
            }
            idx++;
        }
    }

    /* 3) windows, FRONT to back (topmost first). Skip parked/closing windows. */
    for (int32_t i = g_zcount - 1; i >= 0; i--) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
        if (win->minimized) continue;
        if (win->phase == PH_CLOSING || win->phase == PH_MINIMIZING) continue;
        int32_t fx = win->x, fy = win->y;
        int32_t cw = (int32_t)win->w;
        int32_t full_h = (int32_t)win->h + TITLEBAR_H;

        /* outside this window's whole frame? keep searching lower windows */
        if (!point_in(cx, cy, fx, fy, cw, full_h)) continue;

        /* 3a) titlebar close box */
        int32_t close_x = fx + cw - CLOSE_SZ - 8;
        int32_t close_y = fy + (TITLEBAR_H - CLOSE_SZ) / 2;
        if (point_in(cx, cy, close_x, close_y, CLOSE_SZ, CLOSE_SZ)) {
            int32_t id = win->win_id;
            begin_close(slot);
            print("[SHELL] close win "); print_num(id); print("\n");
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3a2) titlebar minimize box (left of the close box) */
        int32_t min_x = close_x - MIN_SZ - 6;
        int32_t min_y = fy + (TITLEBAR_H - MIN_SZ) / 2;
        if (point_in(cx, cy, min_x, min_y, MIN_SZ, MIN_SZ)) {
            begin_minimize(slot);
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3b) titlebar (not a button) -> focus + begin drag-move */
        if (point_in(cx, cy, fx, fy, cw, TITLEBAR_H)) {
            focus_window(slot);
            g_drag_slot = slot;
            g_drag_dx = cx - win->x;
            g_drag_dy = cy - win->y;
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3c) window body -> focus + raise (let the client handle the click) */
        focus_window(slot);
        send_pointer_to_focus();
        g_prev_buttons = g_buttons;
        return;
    }

    /* 4) clicked empty desktop / chrome with nothing actionable: no-op */
    g_prev_buttons = g_buttons;
}

/* ====================================================================== */
void _start(void) {
    print("[SHELL] M5 animated desktop shell starting\n");

    for (int i = 0; i < MAX_WINDOWS; i++) { g_windows[i].used = 0; g_windows[i].phase = PH_NONE; }
    g_zcount = 0;

    /* 1. Acquire the framebuffer. */
    fb_acquire_t fb;
    long r = syscall(SYS_FB_ACQUIRE, (long)&fb, 0, 0);
    if (r != 0) {
        print("[SHELL] fb_acquire FAILED r="); print_num(r); print("\n");
        for (;;) syscall(SYS_YIELD, 0, 0, 0);
    }
    uint32_t W = fb.width, H = fb.height;
    uint32_t stride = fb.pitch / 4;
    g_fb_w = W; g_fb_h = H;
    print("[SHELL] fb "); print_num(W); print("x"); print_num(H);
    print(" pitch="); print_num(fb.pitch);
    print(" bpp="); print_num(fb.bpp); print("\n");

    /* 2. Allocate the back buffer. */
    size_t bb_bytes = (size_t)fb.pitch * H;
    long bbp = syscall(SYS_MMAP, 0, (long)bb_bytes, VMM_PROT_READ | VMM_PROT_WRITE);
    uint32_t *hw   = (uint32_t *)fb.vaddr;
    uint32_t *back;
    if (bbp > 0) {
        back = (uint32_t *)bbp;
        print("[SHELL] back buffer OK bytes="); print_num((long)bb_bytes); print("\n");
    } else {
        back = hw;
        print("[SHELL] back buffer mmap FAILED ("); print_num(bbp);
        print(") -- rendering direct to fb\n");
    }

    /* 3. Create the server command inbox (SysV message queue). */
    long inbox = sc6(SYS_MSGGET, (long)WL_COMP_INBOX_KEY,
                     (long)(IPC_CREAT | 0666), 0, 0, 0, 0);
    if (inbox < 0) {
        print("[SHELL] msgget(inbox) FAILED r="); print_num(inbox);
        print(" -- continuing without IPC (no clients)\n");
    } else {
        print("[SHELL] inbox queue id="); print_num(inbox);
        print(" key=0x434F4D50\n");
    }
    int32_t inbox_qid = (int32_t)inbox;

    /* 4. Open input devices non-blocking. */
    g_kbd_fd   = (int32_t)syscall(SYS_OPEN, (long)"/dev/input/event0",
                                  (long)(O_RDONLY | O_NONBLOCK), 0);
    g_mouse_fd = (int32_t)syscall(SYS_OPEN, (long)"/dev/input/event1",
                                  (long)(O_RDONLY | O_NONBLOCK), 0);
    if (g_kbd_fd < 0)
        print("[SHELL] WARN: /dev/input/event0 (kbd) unavailable -- degraded\n");
    else { print("[SHELL] keyboard fd="); print_num(g_kbd_fd); print("\n"); }
    if (g_mouse_fd < 0)
        print("[SHELL] WARN: /dev/input/event1 (mouse) unavailable -- degraded\n");
    else { print("[SHELL] mouse fd="); print_num(g_mouse_fd); print("\n"); }

    /* cursor starts at screen center */
    g_cursor_x = (int32_t)(W / 2);
    g_cursor_y = (int32_t)(H / 2);

    /* 5. Frame loop. */
    long t0 = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    long next = t0;
    uint64_t frame = 0;

    print("[SHELL] entering frame loop\n");
    for (;;) {
        long now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);

        /* a) drain client requests */
        if (inbox_qid >= 0) drain_inbox(inbox_qid);

        /* b) pump input devices (updates cursor + buttons, forwards kbd/ptr) */
        pump_input(g_kbd_fd,   1, W, H);
        pump_input(g_mouse_fd, 0, W, H);

        /* c) resolve desktop-shell mouse interaction (click/drag/close/launch) */
        handle_mouse(W, H);

        /* d) advance animations (may destroy slots whose CLOSE finished) */
        anim_tick(now);

        /* e) composite + present */
        composite(back, W, H, stride, g_cursor_x, g_cursor_y, now);
        if (back != hw) present(hw, back, H, stride);

        frame++;
        if ((frame % 60) == 0) {
            print("[SHELL] frame "); print_num((long)frame);
            print(" ("); print_num((long)g_zcount);
            print(" windows)\n");
        }

        /* f) ALWAYS yield at least once per frame so init and clients get
         * scheduled even when a frame exceeds the 16ms budget. */
        syscall(SYS_YIELD, 0, 0, 0);
        next += 16;
        for (;;) {
            now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
            if (now >= next) break;
            syscall(SYS_YIELD, 0, 0, 0);
        }
        if (next < now - 64) next = now;     /* resync after a clock jump */
    }
}
