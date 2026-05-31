/*
 * compositor_m6.c -- Milestone M6 WINDOW MANAGER (ring 3): m5 + WM features.
 *
 * Copied verbatim from compositor_m5.c (verified working animated desktop
 * shell). m5 is left untouched; m6 ADDS true window-manager behavior on top.
 * Keeps ALL m5 behavior: top panel (clock + focused title), bottom dock
 * (launcher spawns sbin/terminal + per-window taskbar), zero-copy SHM client
 * compositing, animation engine (open/close/minimize/restore, fixed-point),
 * rounded corners + soft shadows + wallpaper, font titlebars, mouse window-
 * management (drag/focus/raise/close/minimize), keyboard + pointer forwarding
 * to the focused window, chrome reservation, unconditional SYS_YIELD per frame.
 *
 * NEW IN M6 (window-manager layer)
 * ================================
 *  1. WINDOW SNAPPING. While dragging a window by its titlebar, if the cursor
 *     reaches a screen edge we show a translucent snap PREVIEW of where the
 *     window will land; on release the window SNAPS and animates (geometry
 *     tween, reusing the animation clock) to:
 *       - LEFT  edge  -> left half  of the work area (panel..dock).
 *       - RIGHT edge  -> right half of the work area.
 *       - TOP   edge  -> MAXIMIZED  (fills the whole work area).
 *     Each window stores its pre-snap geometry (saved_x/y/w/h). Dragging a
 *     snapped window away RESTORES that geometry (also animated) before the
 *     normal move resumes, so snapping is fully reversible.
 *
 *  2. ALT+TAB window cycling. We track Left-Alt (KEY_LEFTALT) press/release
 *     and Tab (KEY_TAB) from the forwarded keyboard stream. While Alt is held,
 *     each Tab press cycles focus/raise to the next window in a MOST-RECENTLY-
 *     USED ring (an MRU list maintained on every focus). Alt+Tab is intercepted
 *     in the compositor and is NOT forwarded to the focused client.
 *
 *  3. KEYBOARD SHORTCUTS (intercepted, not forwarded):
 *       - Alt+Q  or  Alt+F4  -> close the focused window (close animation).
 *       - Alt+M              -> minimize the focused window.
 *     Everything else (and every key while Alt is NOT held) is forwarded to
 *     the focused client exactly as in m5.
 *
 *  4. NOTIFICATION TOAST. A transient top-right toast with fade-in/hold/fade-
 *     out, driven by SYS_GET_TICKS_MS. Demoed at startup with
 *     "Welcome to AutomationOS" for a few seconds.
 *
 *  KEY INTERCEPTION CONTRACT: keyboard events are pumped by the compositor
 *  (pump_input, keyboard==1). m6 routes each EV_KEY through wm_handle_key()
 *  FIRST; that function updates Alt state, consumes Alt+Tab / Alt+Q / Alt+F4 /
 *  Alt+M, and returns 1 ("consumed") for those. Only un-consumed keys are
 *  forwarded to the focused client via send_key_to_focus(), so normal client
 *  typing is unaffected.
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
 *       -c userspace/compositor/compositor_m6.c -o /tmp/cm6.o
 *   gcc <same flags> -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/cm6.o /tmp/bf.o -o /tmp/cm6.elf
 *   objdump -d /tmp/cm6.elf | grep "fs:0x28"   # MUST be empty
 */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       size_t;
typedef unsigned long       uint64_t;   /* match <stdint.h> __UINT64_TYPE__ (LP64) */
typedef int                 int32_t;
typedef long                int64_t;    /* match <stdint.h> __INT64_TYPE__  (LP64) */

#include "../lib/icon/icon.h"

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
#define SYS_KILL          26
#define SYS_OPENDIR       30
#define SYS_READDIR       31
#define SYS_CLOSEDIR      32
#define SYS_MMAP          37
#define SYS_FB_ACQUIRE    39
#define SYS_GET_TICKS_MS  40
#define SYS_MKDIR         67

/* ---- directory entry (mirror of kernel struct dirent, kernel/include/vfs.h) ----
 * The kernel copies sizeof(struct dirent) bytes into the user buffer, so the
 * field layout MUST match exactly: d_ino(8) d_off(8) d_reclen(2) d_type(1)
 * d_name[256]. */
#define DESK_NAME_MAX  256
#define DT_DIR         4
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[DESK_NAME_MAX];
};

/* signals for SYS_KILL */
#define SIGKILL           9
#define SIGTERM           15

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

/* ====================================================================== *
 *  PERF: frame-dirty gating + on-screen stats overlay                    *
 * ---------------------------------------------------------------------- *
 *  composite() re-blits every window into the back buffer each frame, so *
 *  its cost scales with the number of open windows. On a slow framebuffer *
 *  (e.g. the ThinkPad T410) the desktop therefore lags more with each app *
 *  opened. The fix is a frame-dirty flag: when GENUINELY nothing changed  *
 *  this frame, we skip composite()+present entirely and burn no CPU.      *
 *                                                                         *
 *  g_dirty is the gate. EVERY change path sets it (input, window lifecycle, *
 *  client surface commits, animations, dock hover). When in any doubt the *
 *  code marks dirty -- a wrongly-SKIPPED frame (stale display) is a worse  *
 *  bug than a wrongly-DRAWN one, so the bias is hard toward marking dirty. *
 *                                                                         *
 *  COMPOSITOR_STATS gates a tiny corner overlay (FPS / frame-time ms /     *
 *  window count / pixels presented) so the owner can measure "1 app vs 5   *
 *  apps" live on the T410. It is cheap and toggleable at runtime with     *
 *  Alt+S (F11/F12 are NOT wired through the kernel PS/2 keymap, so Alt+S    *
 *  -- which the WM already intercepts as a modifier chord -- is the hook).  *
 * ---------------------------------------------------------------------- */
#ifndef COMPOSITOR_STATS
#define COMPOSITOR_STATS 1        /* 1 = draw the stats overlay (ON by default) */
#endif

/* Frame-dirty flag. Seeded to 1 so the very first frame (boot fade + initial
 * desktop) always composites + presents. Set by mark_dirty() on every change
 * path; cleared in the frame loop after a composite+present. */
static int g_dirty = 1;
static inline void mark_dirty(void) { g_dirty = 1; }

/* Stats overlay runtime state. g_stats_on toggles the overlay (Alt+S). The
 * other fields are sampled each presented frame so the overlay shows live nums. */
static int      g_stats_on        = COMPOSITOR_STATS;
static long     g_last_frame_ms   = 0;   /* tick at the last PRESENTED frame    */
static long     g_frame_dt_ms     = 0;   /* ms since the previous presented frame */
static long     g_fps_x10         = 0;   /* FPS * 10 (one decimal), smoothed    */
static uint32_t g_present_px      = 0;   /* pixels written to the FB last present */
static int      g_present_did     = 0;   /* 1 = present_diff actually wrote      */

/* Forward decl: the toast's remaining-visible duration is defined further down
 * (next to render_toast), but anim_tick() -- which is above it -- reads it to
 * keep the frame dirty while a toast is fading in/out. */
static int32_t g_toast_dur_ms;

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

/* M8: simple per-window fade-in duration (ms).  Separate from PH_OPENING
 * so it works for any window regardless of the scale-anim state. */
#define FADE_IN_MS      150

/* animation phases */
#define PH_NONE       0
#define PH_OPENING    1
#define PH_CLOSING    2
#define PH_MINIMIZING 3
#define PH_RESTORING  4
#define PH_SNAPPING   5          /* M6: geometry tween (snap or un-snap)        */

/* M6: snap geometry tween duration (ms) */
#define ANIM_SNAP_MS  170

/* ---------------------------------------------------------------------- *
 *  M6: keyboard scancodes (mirror of kernel/include/input.h)              *
 * ---------------------------------------------------------------------- */
#define KEY_TAB       15
#define KEY_Q         16
#define KEY_S         31      /* PERF: Alt+S toggles the stats overlay */
#define KEY_K         37
#define KEY_M         50
#define KEY_LEFTALT   56
#define KEY_F4        62

/* M6: snap target kinds (also used as the "currently snapped" tag) */
#define SNAP_NONE     0
#define SNAP_LEFT     1
#define SNAP_RIGHT    2
#define SNAP_MAX      3

/* M6: how close (px) the cursor must get to a screen edge to arm a snap. */
#define SNAP_EDGE_PX  12

/* ======================================================================
 * M8: RIGHT-SIDE MACOS-STYLE VERTICAL DOCK
 * ====================================================================== */
#define RDOCK_W          64    /* width of the right dock strip (px)          */
#define RDOCK_ICON_BASE  52    /* base (non-magnified) icon tile size (px)    */
#define RDOCK_PAD         6    /* gap between icon tiles (px)                 */
#define RDOCK_CORNER      8    /* rounded corner radius for icon tiles        */
#define RDOCK_MARGIN_TOP 40    /* top margin inside the dock strip            */

/* Magnification parameters (fixed-point, all in Q8 / integers):
 *   scale = 1 + (MAX_EXTRA/256) * max(0, 1 - dy/INFLUENCE)
 *   MAX_EXTRA/256 ~ 0.9  => max scale ~ 1.9
 *   INFLUENCE ~ 110 px  */
#define RDOCK_MAG_MAX_EXTRA  230   /* (MAX-1)*256 => (1.9-1)*256 = 230        */
#define RDOCK_MAG_INFLUENCE  110   /* pixel radius of magnification field     */
#define RDOCK_SMOOTH_SHIFT     3   /* smooth toward target: >>3 per frame     */

/* Bounce animation on launch click: horizontal (leftward) swing */
#define RDOCK_BOUNCE_MS   420
#define RDOCK_BOUNCE_AMP   18   /* max pixel displacement (left)             */

/* Folder popover (legacy rect popover replaced by the rainbow fan-out below;
 * the ANIM_MS timing is reused to drive the fan open/close animation). */
#define RDOCK_POPOVER_W   160
#define RDOCK_POPOVER_H   200
#define RDOCK_POPOVER_ANIM_MS 180

/* Folder rainbow fan-out (open state). Member app icons sweep OUT along a
 * ~160-degree semicircle into the workspace, floating and twinkling. */
#define RDOCK_FAN_ARC_DEG   160   /* total angular spread of the rainbow      */
#define RDOCK_FAN_RADIUS    140   /* arc radius at full open (px)             */
#define RDOCK_FAN_TILE       44   /* fanned-out member icon tile size (px)    */
#define RDOCK_FAN_BOB_AMP     4   /* vertical floating bob amplitude (px)     */
#define RDOCK_FAN_SPARKLES    8   /* sparkle dots drawn around a hovered icon */

/* Number of app entries and folders */
#define RDOCK_NICONS  23
#define RDOCK_NFOLDERS 2

/* App descriptor */
typedef struct {
    const char *label;   /* 2-char label shown on tile           */
    const char *path;    /* relative spawn path                  */
    uint32_t    color;   /* tile background color (ARGB)         */
} rdock_app_t;

/* Folder descriptor */
typedef struct {
    const char     *label;
    uint32_t        color;
    int             members[4];  /* indices into rdock_apps[], -1=unused */
    int             nmembers;
} rdock_folder_t;

/* ---- App table ---- */
static const rdock_app_t rdock_apps[RDOCK_NICONS] = {
    { "Te", "sbin/terminal",    0xFF1E6FB5u },  /* 0  Terminal   */
    { "Fi", "sbin/filemanager", 0xFF2E8B57u },  /* 1  Files      */
    { "Ed", "sbin/editor",      0xFF8B6914u },  /* 2  Editor     */
    { "No", "sbin/notes",       0xFFB8860Bu },  /* 3  Notes      */
    { "Ca", "sbin/calculator",  0xFF555577u },  /* 4  Calculator */
    { "Cl", "sbin/clock",       0xFF336699u },  /* 5  Clock      */
    { "Pa", "sbin/paint",       0xFF8B3A3Au },  /* 6  Paint      */
    { "Sn", "sbin/snake",       0xFF2D6A2Du },  /* 7  Snake      */
    { "Tt", "sbin/tetris",      0xFF6B2D8Bu },  /* 8  Tetris     */
    { "20", "sbin/game2048",    0xFF8B4513u },  /* 9  2048       */
    { "St", "sbin/settings",    0xFF444466u },  /* 10 Settings   */
    { "Da", "sbin/dateapp",     0xFF336677u },  /* 11 Date       */
    { "Id", "sbin/ide",         0xFF2D6A8Bu },  /* 12 IDE        */
    { "Bd", "sbin/bubbletd",    0xFF1FA87Au },  /* 13 BubbleTD   */
    { "Wb", "sbin/browser",     0xFF1565C0u },  /* 14 Browser    */
    { "Nm", "sbin/netman",      0xFF00897Bu },  /* 15 NetManager */
    { "Ch", "sbin/chess",       0xFF5C3A2Au },  /* 16 Chess      */
    { "As", "sbin/asteroids",   0xFF202840u },  /* 17 Asteroids  */
    { "Su", "sbin/sudoku",      0xFF6D4C9Fu },  /* 18 Sudoku     */
    { "Cc", "sbin/controlcenter", 0xFF0A84FFu },/* 19 ControlCtr */
    { "Ph", "sbin/photos",      0xFF2D8B8Bu },  /* 20 Photos     */
    { "Pm", "sbin/pacman",      0xFFFFD60Au },  /* 21 Pac-Man    */
    { "C+", "sbin/clockapp",    0xFF0067C0u },  /* 22 Clock+     */
};

/* ---- Folder table (Games + Tools) ---- */
static const rdock_folder_t rdock_folders[RDOCK_NFOLDERS] = {
    { "Gm", 0xFF3A3A5Cu, { 7, 8, 9, 21 }, 4 },   /* Games  (+Pac-Man) */
    { "Tl", 0xFF3A5C3Au, { 4, 5, 2, 22 }, 4 },   /* Tools  (+Clock+)  */
};

/* ---- Per-icon animation state ---- */
typedef struct {
    int32_t scale_q8;       /* current magnified scale (Q8, 256=1.0)         */
    int32_t scale_target;   /* target scale this frame (Q8)                  */
    long    bounce_start;   /* SYS_GET_TICKS_MS when bounce started, 0=off   */
    int     bounce_active;
} rdock_icon_state_t;

/* ---- Per-folder state ---- */
typedef struct {
    int     open;           /* 1 = popover expanded                           */
    int32_t anim_t;         /* Q8 progress 0..256 for open/close anim         */
    long    anim_start;
    int     anim_closing;   /* 1 = animating closed                           */
} rdock_folder_state_t;

/* TOTAL items in the dock strip = apps + folders.
 * Folders are inserted after their last member slot conceptually, but we
 * just append them at the bottom of the icon list for simplicity. */
#define RDOCK_TOTAL  (RDOCK_NICONS + RDOCK_NFOLDERS)

/* ====================== Popup menus (start + context) ================== *
 *  Start menu opens above the launcher; right-click opens a context menu  *
 *  on the desktop. Both share one popup with a header + clickable rows.   *
 * ---------------------------------------------------------------------- */
#define MENU_W            190
#define MENU_ROW_H        26
#define MENU_HDR_H        28
#define MENU_MAX          16
#define MACT_NONE          0   /* row spawns g_menu_path */
#define MACT_MINIMIZE_ALL  1
#define MACT_CLOSE_ALL     2
#define MACT_ABOUT         3
#define MACT_NEW_FOLDER    4   /* create /Desktop/NewFolder[N] then rescan */
#define MACT_WIN_MINIMIZE  5   /* minimize g_menu_target_slot (title-bar path) */
#define MACT_WIN_MAXIMIZE  6   /* toggle SNAP_MAX on g_menu_target_slot         */
#define MACT_WIN_CLOSE     7   /* close g_menu_target_slot                      */
#define MACT_REFRESH       8   /* force a full desktop repaint (best-effort)    */
#define MACT_DISPLAY_SETTINGS 9 /* spawn sbin/settings                          */
#define MACT_WIN_SNAP_LEFT  10  /* snap g_menu_target_slot to the left half      */
#define MACT_WIN_SNAP_RIGHT 11  /* snap g_menu_target_slot to the right half     */
static int         g_menu_open   = 0;
static int         g_about_open  = 0;   /* modal About dialog (centered panel) */
static int         g_menu_is_ctx = 0;   /* 0=start menu, 1=context menu */
static int32_t     g_menu_x = 0, g_menu_y = 0;
static int         g_menu_n = 0;
static int         g_menu_hover = -1;
static int         g_menu_target_slot = -1;  /* window the ctx menu acts on, -1=desktop */
static const char *g_menu_label[MENU_MAX];
static const char *g_menu_path[MENU_MAX];
static int         g_menu_action[MENU_MAX];
static int32_t menu_height(void) { return MENU_HDR_H + g_menu_n * MENU_ROW_H + 6; }

static rdock_icon_state_t   g_rdock_icons[RDOCK_TOTAL];
static rdock_folder_state_t g_rdock_folders[RDOCK_NFOLDERS];
static int32_t              g_rdock_hovered = -1;  /* -1 or index 0..TOTAL-1 */

/* Which folder popover is open (-1 = none). Only one at a time. */
static int32_t g_rdock_open_folder = -1;

/* ====================================================================== *
 *  M8: DESKTOP ICONS  ("/Desktop" contents as labeled wallpaper icons)   *
 *                                                                         *
 *  The IDE compiles programs to "/Desktop". We enumerate that directory  *
 *  via SYS_OPENDIR/READDIR/CLOSEDIR (same pattern the file/folder code    *
 *  uses), lay the entries out in a grid at the top-left of the desktop,   *
 *  and CLAMP every tile fully on-screen (icon_for_app / icon_* write      *
 *  UNCLIPPED, so a partially off-screen tile would scribble past the back *
 *  buffer). Double-click runs them; right-click empty space can make new  *
 *  folders.                                                               *
 * ---------------------------------------------------------------------- */
#define DESK_MAX_ICONS   64    /* cap entries we track / draw                 */
#define DESK_NAME_DISP   32    /* truncated name length we store for label    */
#define DESK_TILE        56    /* icon art size (px)                          */
#define DESK_CELL_W      96    /* grid cell width  (tile + label margin)      */
#define DESK_CELL_H      88    /* grid cell height (tile + label + gap)       */
#define DESK_ORIGIN_X    16    /* grid left margin                            */
#define DESK_LABEL_GAP    4    /* gap between tile bottom and label           */
#define DESK_DBLCLICK_MS 400   /* two clicks within this window = double      */
#define DESK_RESCAN_FRAMES 120 /* periodic rescan cadence (frames ~ 2s)       */

typedef struct {
    char name[DESK_NAME_DISP + 1];   /* truncated display/spawn name          */
    int  is_dir;                     /* 1 = directory                         */
} desk_icon_t;

static desk_icon_t g_desk_icons[DESK_MAX_ICONS];
static int         g_desk_count = 0;
/* double-click tracking: last clicked icon index + timestamp */
static int  g_desk_last_idx  = -1;
static long g_desk_last_ms   = 0;

/* M6: snap-armed-during-drag preview. g_snap_armed is SNAP_* (the zone the
 * window will land in if released now), or SNAP_NONE if the cursor is not at an
 * edge. The compositor draws a translucent preview for whatever is armed. */
static int32_t g_snap_armed = SNAP_NONE;

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
    if (den == 0) den = 1;
    int32_t ar = (int32_t)((a >> 16) & 0xFF), ag = (int32_t)((a >> 8) & 0xFF), ab = (int32_t)(a & 0xFF);
    int32_t br = (int32_t)((b >> 16) & 0xFF), bg = (int32_t)((b >> 8) & 0xFF), bb = (int32_t)(b & 0xFF);
    /* Use signed deltas: when a channel of `a` exceeds `b`, (br-ar) is negative.
     * Computed in unsigned it wraps to ~4 billion and the result explodes. */
    int32_t r  = ar + (br - ar) * (int32_t)num / (int32_t)den;
    int32_t g  = ag + (bg - ag) * (int32_t)num / (int32_t)den;
    int32_t bl = ab + (bb - ab) * (int32_t)num / (int32_t)den;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
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

/* ---------------------------------------------------------------------- *
 *  Fixed-point sine, NO FLOAT / NO LIBM.                                  *
 *  sin_q(deg) returns sin(deg) in Q8 (signed, -256..256). Uses a small    *
 *  quarter-wave table (0..90 deg, step 5 deg) with linear interpolation   *
 *  and the usual quadrant symmetry. Degrees are wrapped to [0,360).       *
 *  Reused by the folder fan-out (arc layout), icon floating bob, and the  *
 *  sparkle particle orbits.                                               *
 * ---------------------------------------------------------------------- */
static const int32_t SINQ_TBL[19] = {  /* sin(0..90 by 5 deg) * 256 */
      0,  22,  44,  66,  88, 109, 128, 147, 165, 181,
    196, 209, 221, 231, 240, 247, 252, 255, 256
};
static int32_t sin_q(int32_t deg) {
    /* wrap into [0,360) without %, robust for large/negative inputs */
    deg %= 360;
    if (deg < 0) deg += 360;
    int sign = 1;
    if (deg >= 180) { deg -= 180; sign = -1; }   /* lower half is negated */
    if (deg > 90) deg = 180 - deg;               /* fold 90..180 onto 0..90 */
    /* table lookup with linear interp; index by 5-degree buckets */
    int32_t i = deg / 5;
    int32_t frac = deg - i * 5;                  /* 0..4 */
    int32_t a = SINQ_TBL[i];
    int32_t b = SINQ_TBL[i + 1];
    int32_t v = a + (b - a) * frac / 5;
    return sign * v;
}
/* cos via phase shift */
static int32_t cos_q(int32_t deg) { return sin_q(deg + 90); }

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
    /* IMMUTABLE client buffer extent, captured at create time and NEVER
     * overwritten by snap/maximize (which only rewrite w/h to the maximized
     * drawable rect). The client's SHM is buf_w*buf_h*4 bytes, so any blit
     * MUST clamp its SOURCE read to (buf_w,buf_h) or it walks past the mapped
     * segment and page-faults the compositor. (stride stays == buf_w, so
     * buf_w is redundant with stride but kept explicit for clarity.)        */
    uint32_t  buf_w, buf_h;     /* real SHM pixel extent (clamp source read) */
    int32_t   x, y;             /* placement of window FRAME (titlebar top)*/
    char      title[WL_TITLE_MAX];
    int       dirty;            /* set on commit (informational)           */

    /* ---- M5 animation state ---- */
    int32_t   phase;            /* PH_NONE / OPENING / CLOSING / MINIMIZING / RESTORING / SNAPPING */
    long      anim_start_ms;    /* SYS_GET_TICKS_MS when the phase began    */
    int32_t   anim_dur_ms;      /* phase duration in ms                     */
    int       minimized;        /* sticky: window is parked in the taskbar  */
    int32_t   tb_idx;           /* taskbar slot index captured at minimize  */

    /* ---- M6 window-manager state ---- */
    int32_t   snap_state;       /* SNAP_NONE / LEFT / RIGHT / MAX (current)  */
    int32_t   saved_x, saved_y; /* pre-snap geometry to restore to           */
    uint32_t  saved_w, saved_h; /* pre-snap surface size                     */
    /* PH_SNAPPING geometry tween: animate (from_*) -> (to_*) over the phase. */
    int32_t   from_x, from_y;
    uint32_t  from_w, from_h;
    int32_t   to_x, to_y;
    uint32_t  to_w, to_h;

    /* ---- M8 cheap per-window FADE-IN (compositor-internal) ---- *
     * fade_alpha ramps 0..255 over ~150ms after creation. When it   *
     * reaches 255 it stays there; the fast opaque path is used then.*
     * This is independent of (and additive to) the PH_OPENING       *
     * scale animation so windows smoothly appear from both effects. */
    uint32_t  fade_alpha;       /* 0..255; 255 = fully opaque, use fast path */
    long      fade_start_ms;    /* SYS_GET_TICKS_MS when fade began          */
} window_t;

static window_t g_windows[MAX_WINDOWS];
static int32_t  g_next_win_id = 1;

/* z-order: index list, back (0) to front (count-1). focus = topmost/front. */
static int32_t g_zorder[MAX_WINDOWS];
static int32_t g_zcount = 0;

/* M6: most-recently-used focus ring (front = most recent). Alt+Tab cycles it.
 * Kept distinct from z-order so that Alt-Tabbing through windows during a hold
 * doesn't reshuffle the MRU order until Alt is released. */
static int32_t g_mru[MAX_WINDOWS];
static int32_t g_mru_count = 0;

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
    mark_dirty();    /* PERF: raising/focusing reorders the window stack = repaint */
}

static void z_remove(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_zcount; i++)
        if (g_zorder[i] != slot) g_zorder[w++] = g_zorder[i];
    g_zcount = w;
    mark_dirty();    /* PERF: removing a window from the stack = repaint */
}

/* M6: MRU ring helpers (front = index 0 = most-recently focused). */
static void mru_promote(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_mru_count; i++)
        if (g_mru[i] != slot) g_mru[w++] = g_mru[i];
    g_mru_count = w;
    /* Clamp before the shift: if `slot` was not already in a full ring, the
     * shift's first write (g_mru[g_mru_count]) would land at g_mru[MAX_WINDOWS]
     * — one past the array. Drop the least-recently-used entry to make room. */
    if (g_mru_count >= MAX_WINDOWS) g_mru_count = MAX_WINDOWS - 1;
    /* shift down + insert at front */
    for (int32_t i = g_mru_count; i > 0; i--) g_mru[i] = g_mru[i - 1];
    g_mru[0] = (int32_t)slot;
    g_mru_count++;
}

static void mru_remove(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_mru_count; i++)
        if (g_mru[i] != slot) g_mru[w++] = g_mru[i];
    g_mru_count = w;
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

/* ---------------------------------------------------------------------- *
 *  M6: work area (the chrome-free region between panel and dock) + the     *
 *  snap target rectangles. All of these are in FRAME coordinates: x/y is   *
 *  the top-left of the titlebar; w/h is the CLIENT surface size (the frame  *
 *  adds TITLEBAR_H + borders), matching window_t and render_window_static.  *
 * ---------------------------------------------------------------------- */
#define WORK_MARGIN  4           /* small gap so snapped windows aren't flush */

static int32_t work_x0(void)        { return WORK_MARGIN; }
static int32_t work_y0(void)        { return PANEL_H + WORK_MARGIN; }
/* M8: subtract right dock width so snapping and work area respect the dock */
static int32_t work_x1(void)        { return (int32_t)g_fb_w - RDOCK_W - WORK_MARGIN; }
static int32_t work_y1(void)        { return (int32_t)g_fb_h - DOCK_H - WORK_MARGIN; }
static int32_t work_w(void)         { return work_x1() - work_x0(); }
/* full frame height available (titlebar + client + 2 borders) */
static int32_t work_frame_h(void)   { return work_y1() - work_y0(); }
/* client height for a frame that fills the work area vertically */
static int32_t work_client_h(void)  { int32_t v = work_frame_h() - TITLEBAR_H - 2 * BORDER_W;
                                      return v < 1 ? 1 : v; }

/* Compute the FRAME x/y + CLIENT w/h for a snap target. Returns 0 on success. */
static int snap_target_rect(int32_t kind, int32_t *fx, int32_t *fy,
                            uint32_t *cw, uint32_t *ch) {
    int32_t half = work_w() / 2;
    int32_t cwid = half - BORDER_W;                /* client w fitting one half */
    if (cwid < 1) cwid = 1;
    int32_t chgt = work_client_h();
    switch (kind) {
        case SNAP_LEFT:
            *fx = work_x0() + BORDER_W;
            *fy = work_y0() + BORDER_W;
            *cw = (uint32_t)cwid; *ch = (uint32_t)chgt;
            return 0;
        case SNAP_RIGHT:
            *fx = work_x0() + half + BORDER_W;
            *fy = work_y0() + BORDER_W;
            *cw = (uint32_t)cwid; *ch = (uint32_t)chgt;
            return 0;
        case SNAP_MAX: {
            int32_t fullw = work_w() - 2 * BORDER_W;
            if (fullw < 1) fullw = 1;
            *fx = work_x0() + BORDER_W;
            *fy = work_y0() + BORDER_W;
            *cw = (uint32_t)fullw; *ch = (uint32_t)chgt;
            return 0;
        }
        default:
            return -1;
    }
}

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

/* Eased progress for the VISUAL window animations (open/close/minimize scale).
 * ease-out-cubic: 1 - (1-t)^3, Q8 in [0,256]. Smooth deceleration instead of a
 * linear ramp -> a more satisfying, Windows-like feel. The timing/completion
 * logic keeps using the raw linear t (it must hit exactly 256 at the end). */
static int32_t anim_eased_t(window_t *win, long now) {
    int32_t t = anim_linear_t(win, now);
    if (t <= 0)   return 0;
    if (t >= 256) return 256;
    int32_t inv = 256 - t;
    int64_t i2  = ((int64_t)inv * inv) >> 8;
    int64_t i3  = (i2 * inv) >> 8;
    int64_t f   = 256 - i3;
    if (f < 0)   f = 0;
    if (f > 256) f = 256;
    return (int32_t)f;
}

static void anim_begin(window_t *win, int32_t phase, int32_t dur_ms, long now) {
    win->phase = phase;
    win->anim_dur_ms = dur_ms;
    win->anim_start_ms = now;
    /* PERF: every window animation (open/close/minimize/restore/snap) starts
     * here. Mark dirty so the first animated frame renders even if it began on
     * an otherwise-idle frame; anim_tick keeps it dirty for the rest of the run. */
    mark_dirty();
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

/* M8: Advance each live window's cheap fade-in alpha toward 255.
 * Called once per frame from anim_tick().  Uses only integer math;
 * no floats, no libgcc helpers. */
static void advance_fade_in(long now) {
    for (int s = 0; s < MAX_WINDOWS; s++) {
        window_t *win = &g_windows[s];
        if (!win->used || win->fade_alpha >= 255) continue;
        long dt = now - win->fade_start_ms;
        if (dt <= 0) { win->fade_alpha = 0; continue; }
        if (dt >= FADE_IN_MS) { win->fade_alpha = 255; continue; }
        /* linear ramp: alpha = 255 * dt / FADE_IN_MS */
        win->fade_alpha = (uint32_t)((long)255 * dt / FADE_IN_MS);
        if (win->fade_alpha > 255) win->fade_alpha = 255;
    }
}

static void anim_tick(long now) {
    advance_fade_in(now);
    for (int s = 0; s < MAX_WINDOWS; s++) {
        window_t *win = &g_windows[s];
        if (!win->used || win->phase == PH_NONE) continue;
        int32_t t = anim_linear_t(win, now);
        if (t < 256) continue;                 /* still animating */
        /* phase finished THIS frame: mark dirty so the FINAL settled position
         * renders. Critical for PH_SNAPPING, whose settle step below snaps the
         * geometry to its destination -- without this the gate could skip the
         * last frame and leave the window one tween-step short. Also covers the
         * post-close/minimize/restore repaint (a window vanished/parked). */
        mark_dirty();
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
            case PH_SNAPPING:
                /* settle the geometry at the tween's destination */
                win->x = win->to_x;
                win->y = win->to_y;
                win->w = win->to_w;
                win->h = win->to_h;
                win->phase = PH_NONE;
                break;
            default:
                win->phase = PH_NONE;
                break;
        }
    }

    /* PERF: keep the frame dirty for as long as ANYTHING is animating, so the
     * dirty-gate never skips a frame that would have advanced an animation.
     * This is the single place that covers every per-frame visual motion:
     *   - window open/close/min/restore/snap phases (PH_* != PH_NONE)
     *   - the cheap per-window fade-in (fade_alpha < 255)
     *   - a visible toast (g_toast_dur_ms > 0)
     *   - right-dock folder open/close tween (anim_t in motion)
     *   - right-dock icon bounce
     *   - right-dock magnify scale still settling toward its target
     * If any is live we mark dirty; the gate then composites+presents this
     * frame and re-checks next frame, so motion stays smooth.               */
    for (int s = 0; s < MAX_WINDOWS; s++) {
        window_t *win = &g_windows[s];
        if (!win->used) continue;
        if (win->phase != PH_NONE || win->fade_alpha < 255) { mark_dirty(); break; }
    }
    if (g_toast_dur_ms > 0) mark_dirty();
    for (int fi = 0; fi < RDOCK_NFOLDERS; fi++) {
        rdock_folder_state_t *fs = &g_rdock_folders[fi];
        if (fs->open || fs->anim_closing || fs->anim_t > 0) { mark_dirty(); break; }
    }
    for (int i = 0; i < RDOCK_TOTAL; i++) {
        if (g_rdock_icons[i].bounce_active) { mark_dirty(); break; }
        /* magnify scale still easing toward its target == visible motion */
        if (g_rdock_icons[i].scale_q8 != g_rdock_icons[i].scale_target) { mark_dirty(); break; }
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

/* ====================================================================== *
 *  M8: DESKTOP-ICON scan / layout / render                               *
 * ====================================================================== */

/* (Re)enumerate "/Desktop" into g_desk_icons[]. Mirrors how the dock folder
 * code reads directories: opendir -> readdir loop -> closedir. Skips "." and
 * "..". Robust: bails silently if /Desktop can't be opened (e.g. not mounted
 * yet) and just leaves the previous list. */
static void desk_scan(void) {
    int prev_count = g_desk_count;
    long dfd = syscall(SYS_OPENDIR, (long)"/Desktop", 0, 0);
    if (dfd < 0) {
        if (g_desk_count != 0) mark_dirty();     /* icons vanished -> repaint */
        g_desk_count = 0;
        return;                                  /* no /Desktop -> nothing */
    }

    int n = 0;
    struct dirent ent;
    while (n < DESK_MAX_ICONS) {
        long rr = syscall(SYS_READDIR, dfd, (long)&ent, 0);
        if (rr < 0) break;                        /* end of dir / error */
        /* skip "." and ".." */
        if (ent.d_name[0] == '.' &&
            (ent.d_name[1] == '\0' ||
             (ent.d_name[1] == '.' && ent.d_name[2] == '\0')))
            continue;
        if (ent.d_name[0] == '\0') continue;      /* defensive */

        desk_icon_t *di = &g_desk_icons[n];
        int i = 0;
        while (ent.d_name[i] && i < DESK_NAME_DISP) { di->name[i] = ent.d_name[i]; i++; }
        di->name[i] = '\0';
        di->is_dir  = (ent.d_type == DT_DIR);
        n++;
    }
    syscall(SYS_CLOSEDIR, dfd, 0, 0);
    /* PERF: the periodic /Desktop rescan only changes the screen when the icon
     * set changed. Use the count as a cheap change proxy and mark dirty so a
     * newly-created/removed icon repaints (the common case: IDE output, New
     * Folder). A rename that keeps the same count is rare on /Desktop and is
     * picked up by the next genuine dirty frame; biasing here would force a full
     * recomposite every rescan and defeat the gate. */
    if (n != prev_count) mark_dirty();
    g_desk_count = n;
}

/* Compute the grid cell origin (top-left of the TILE) for desktop icon [idx],
 * laid out in a top-left grid and CLAMPED so the whole DESK_CELL fits between
 * the panel and the dock and left of the right dock. Returns 1 if a valid,
 * fully-on-screen slot exists for idx, else 0 (caller skips it). */
static int desk_icon_origin(int idx, uint32_t W, uint32_t H,
                            int32_t *otx, int32_t *oty) {
    int32_t area_x0 = DESK_ORIGIN_X;
    int32_t area_y0 = PANEL_H + 8;
    int32_t area_x1 = (int32_t)W - RDOCK_W - 4;     /* keep clear of right dock */
    int32_t area_y1 = (int32_t)H - DOCK_H - 4;      /* keep clear of bottom dock */

    int32_t avail_w = area_x1 - area_x0;
    int32_t avail_h = area_y1 - area_y0;
    if (avail_w < DESK_CELL_W || avail_h < DESK_CELL_H) return 0;

    int cols = avail_w / DESK_CELL_W;
    int rows = avail_h / DESK_CELL_H;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (idx >= cols * rows) return 0;               /* no room: skip */

    int col = idx % cols;
    int row = idx / cols;

    int32_t cell_x = area_x0 + col * DESK_CELL_W;
    int32_t cell_y = area_y0 + row * DESK_CELL_H;

    /* center the tile horizontally inside its cell */
    int32_t tx = cell_x + (DESK_CELL_W - DESK_TILE) / 2;
    int32_t ty = cell_y;

    /* Final hard clamp so neither the tile NOR its label can leave the buffer.
     * (icon_* and font_draw both clip-by-cell only; tile must be fully inside.) */
    if (tx < 2) tx = 2;
    if (ty < PANEL_H + 2) ty = PANEL_H + 2;
    if (tx + DESK_TILE > area_x1) return 0;
    if (ty + DESK_TILE + DESK_LABEL_GAP + FONT_H > area_y1) return 0;

    *otx = tx; *oty = ty;
    return 1;
}

/* Draw all desktop icons onto the wallpaper. Called from composite() right
 * after render_desktop(), so they sit beneath windows/chrome. */
static void render_desktop_icons(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                                 int32_t cur_x, int32_t cur_y) {
    for (int i = 0; i < g_desk_count; i++) {
        int32_t tx, ty;
        if (!desk_icon_origin(i, w, h, &tx, &ty)) continue;

        desk_icon_t *di = &g_desk_icons[i];

        /* hover highlight rectangle behind the whole cell */
        int hovered = (cur_x >= tx - 6 && cur_x < tx + DESK_TILE + 6 &&
                       cur_y >= ty - 2 && cur_y < ty + DESK_TILE + DESK_LABEL_GAP + FONT_H + 2);
        if (hovered)
            fill_round_rect(buf, w, h, stride, tx - 6, ty - 2,
                            DESK_TILE + 12, DESK_TILE + DESK_LABEL_GAP + FONT_H + 4,
                            6, 0x40FFFFFFu);

        /* the icon art (drawn FULLY inside the clamped tile) */
        if (di->is_dir) {
            icon_folder(buf, (int)stride, tx, ty, DESK_TILE, 0xFFE0B040u);
        } else {
            /* recognizable icon when the name matches a known app, else a
             * generic text-file tile. icon_for_app falls back to initials. */
            icon_for_app(buf, (int)stride, tx, ty, DESK_TILE, di->name);
        }

        /* centered name label below the tile (truncated to fit the cell) */
        int nlen = 0; while (di->name[nlen]) nlen++;
        int maxch = DESK_CELL_W / FONT_W;
        if (maxch < 1) maxch = 1;
        char lbl[DESK_NAME_DISP + 1];
        int show = nlen > maxch ? maxch : nlen;
        int li;
        for (li = 0; li < show; li++) lbl[li] = di->name[li];
        lbl[show] = '\0';
        int32_t lbl_w = show * FONT_W;
        int32_t lbl_x = tx + DESK_TILE / 2 - lbl_w / 2;
        int32_t lbl_y = ty + DESK_TILE + DESK_LABEL_GAP;
        if (lbl_x < 2) lbl_x = 2;
        font_draw_string(buf, (int)stride, (int)w, (int)h, lbl_x, lbl_y, lbl, COL_TEXT);
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
 *
 * M8 fade-in: if win->fade_alpha < 255 the window is still fading in.
 * The chrome rectangles are drawn as translucent blend_rects, and the
 * client surface is blitted via blit_surface_scaled_alpha at scale 1:1
 * with the fade alpha.  Once fade_alpha == 255 (the fast path), all
 * drawing is the same opaque code as before.
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

    /* M8 fade-in: convert 0..255 alpha to 0..256 Q8 for blend_pixel_a256. */
    uint32_t fa256 = win->fade_alpha;               /* 0..255 */
    if (fa256 >= 255) fa256 = 256;                  /* 255 snaps to fully opaque */
    int fading = (fa256 < 256);

    /* soft drop shadow (always draw, fading shadow is a nice touch) */
    if (!fading) {
        draw_soft_shadow(buf, w, h, stride, outer_x, outer_y, full_w, full_h);
    } else {
        /* Lighter shadow during fade so it doesn't look baked-in while transparent */
        static const uint32_t sha[4] = { 0x22000000u, 0x18000000u, 0x0E000000u, 0x08000000u };
        static const int32_t  sgrow[4] = { 2, 5, 9, 14 };
        static const int32_t  soffy[4] = { 2, 4, 6,  9 };
        for (int si = 3; si >= 0; si--) {
            int32_t sg = sgrow[si];
            int32_t sx2 = outer_x - sg + 2;
            int32_t sy2 = outer_y - sg + soffy[si];
            int32_t sw2 = full_w + 2 * sg;
            int32_t sh2 = full_h + 2 * sg;
            /* alpha-scale by fade fraction */
            uint32_t sa = (((sha[si] >> 24) * fa256) >> 8) << 24;
            uint32_t sc = sa | (sha[si] & 0x00FFFFFFu);
            blend_rect(buf, w, h, stride, sx2, sy2 + WIN_RADIUS, sw2, sh2 - 2 * WIN_RADIUS, sc);
            blend_rect(buf, w, h, stride, sx2 + WIN_RADIUS, sy2, sw2 - 2 * WIN_RADIUS, WIN_RADIUS, sc);
            blend_rect(buf, w, h, stride, sx2 + WIN_RADIUS, sy2 + sh2 - WIN_RADIUS,
                       sw2 - 2 * WIN_RADIUS, WIN_RADIUS, sc);
        }
    }

    if (!fading) {
        /* ---- FAST PATH: fully faded in, use opaque drawing ---- */

        /* rounded border + titlebar */
        fill_round_top_rect(buf, w, h, stride, fx, fy, cw, TITLEBAR_H, WIN_RADIUS, tb_col);

        /* close + minimize boxes */
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

        /* maximize/restore box (left of minimize): a square-outline glyph,
         * font-independent. Clicking it toggles SNAP_MAX (see handle_mouse). */
        int32_t max_x = min_x - MIN_SZ - 6;
        int32_t max_y = fy + (TITLEBAR_H - MIN_SZ) / 2;
        fill_round_rect(buf, w, h, stride, max_x, max_y, MIN_SZ, MIN_SZ, 3, BTN_MIN);
        {
            int32_t gs = (MIN_SZ > 10) ? 8 : ((int32_t)MIN_SZ - 4);
            int32_t gx = max_x + ((int32_t)MIN_SZ - gs) / 2;
            int32_t gy = max_y + ((int32_t)MIN_SZ - gs) / 2;
            fill_round_rect(buf, w, h, stride, gx, gy, gs, gs, 1, 0xFF202020u);
            fill_round_rect(buf, w, h, stride, gx + 1, gy + 2, gs - 2, gs - 3, 1, BTN_MIN);
        }

        /* window title text */
        font_draw_string(buf, (int)stride, (int)w, (int)h,
                         fx + 8, fy + (TITLEBAR_H - FONT_H) / 2, win->title,
                         focused ? COL_TEXT : COL_TEXT_DIM);

        /* client surface. Clamp the SOURCE read to the client's REAL SHM extent
         * (buf_w x buf_h): when a window is maximized, win->w/win->h grow to the
         * maximized rect but the client's buffer stays buf_w x buf_h, so reading
         * win->w x win->h rows here would walk past the mapped segment and fault
         * the compositor. Clamping letterboxes the smaller content instead. */
        if (win->pixels) {
            uint32_t sw = win->w < win->buf_w ? win->w : win->buf_w;
            uint32_t sh = win->h < win->buf_h ? win->h : win->buf_h;
            blit_surface_clip(buf, w, h, stride,
                              win->pixels, sw, sh, win->stride,
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

    } else {
        /* ---- FADE-IN PATH: blend everything at fa256 alpha ---- */

        /* Titlebar: alpha-blended filled rounded-top rect. We approximate by
         * drawing a plain blended rect (the full titlebar band) rather than the
         * rounded variant to keep this path simple and cheap. */
        {
            int32_t ty1 = fy, ty2 = fy + TITLEBAR_H;
            if (ty1 < clip_y0) ty1 = clip_y0;
            if (ty2 > clip_y1) ty2 = clip_y1;
            if (ty2 > ty1) {
                int32_t x1 = fx < 0 ? 0 : fx;
                int32_t x2 = fx + cw; if (x2 > (int32_t)w) x2 = (int32_t)w;
                uint32_t tbr = (tb_col >> 16) & 0xFF;
                uint32_t tbg = (tb_col >>  8) & 0xFF;
                uint32_t tbb = (tb_col      ) & 0xFF;
                for (int32_t yy = ty1; yy < ty2; yy++) {
                    uint32_t *drow = buf + (uint32_t)yy * stride;
                    for (int32_t xx = x1; xx < x2; xx++) {
                        uint32_t d = drow[xx];
                        uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                        uint32_t ia = 256 - fa256;
                        uint32_t or_ = (tbr * fa256 + dr * ia) >> 8;
                        uint32_t og  = (tbg * fa256 + dg * ia) >> 8;
                        uint32_t ob  = (tbb * fa256 + db * ia) >> 8;
                        drow[xx] = 0xFF000000u | (or_ << 16) | (og << 8) | ob;
                    }
                }
            }
        }

        /* Client surface: 1:1 scale blit via blit_surface_scaled_alpha */
        if (win->pixels) {
            /* clamp source read to the real SHM extent (see fast-path note) */
            uint32_t sw = win->w < win->buf_w ? win->w : win->buf_w;
            uint32_t sh = win->h < win->buf_h ? win->h : win->buf_h;
            blit_surface_scaled_alpha(buf, w, h, stride,
                                      win->pixels, sw, sh, win->stride,
                                      client_x, client_y,
                                      256, 256, fa256,
                                      clip_y0, clip_y1);
        } else {
            /* placeholder: blended solid rect */
            int32_t py0 = client_y < clip_y0 ? clip_y0 : client_y;
            int32_t py1 = client_y + ch;
            if (py1 > clip_y1) py1 = clip_y1;
            if (py1 > py0) {
                uint32_t phcol = (fa256 << 24) | (WIN_PLACEHOLDER & 0x00FFFFFFu);
                blend_rect(buf, w, h, stride, client_x, py0, cw, py1 - py0, phcol);
            }
        }

        /* 1px border (alpha-blended) */
        {
            uint32_t ba = (fa256 << 24) | (bd_col & 0x00FFFFFFu);
            blend_rect(buf, w, h, stride, outer_x, outer_y, full_w, 1, ba);
            blend_rect(buf, w, h, stride, outer_x, outer_y + full_h - 1, full_w, 1, ba);
            blend_rect(buf, w, h, stride, outer_x, outer_y, 1, full_h, ba);
            blend_rect(buf, w, h, stride, outer_x + full_w - 1, outer_y, 1, full_h, ba);
        }

        /* No rounded-corner punch during fade: corners remain as-drawn, which is
         * fine since the overall alpha is low and the effect is brief (~150ms). */
        return;
    }  /* end fade-in path */

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

    int32_t lin = anim_eased_t(win, now);

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

    /* Animated client content: scaled + alpha-blended about its center.
     * Clamp the source read to the real SHM extent (see fast-path note). */
    if (win->pixels) {
        uint32_t sw = win->w < win->buf_w ? win->w : win->buf_w;
        uint32_t sh = win->h < win->buf_h ? win->h : win->buf_h;
        blit_surface_scaled_alpha(buf, w, h, stride,
                                  win->pixels, sw, sh, win->stride,
                                  draw_client_x, draw_client_y,
                                  scale_num, scale_den, alpha,
                                  clip_y0, clip_y1);
    }
}

/*
 * M6: SNAP geometry tween. Interpolate (from_*) -> (to_*) with ease_in_out_cubic
 * and draw a normal, fully-opaque, decorated window at the interpolated rect.
 * We do this by temporarily overriding win->x/y/w/h with the tween values and
 * reusing render_window_static (so corners/shadow/titlebar all match), then
 * restoring the settled fields. The blit clips the client surface to the new
 * width/height; if the client buffer is smaller, the placeholder fills the rest.
 */
static void render_window_snapping(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                                   window_t *win, int focused, long now) {
    int32_t lin = anim_eased_t(win, now);
    int32_t e   = ease_in_out_cubic(lin);          /* Q8 progress 0..256 */

    int32_t ix = win->from_x + (win->to_x - win->from_x) * e / 256;
    int32_t iy = win->from_y + (win->to_y - win->from_y) * e / 256;
    int32_t iw = (int32_t)win->from_w + ((int32_t)win->to_w - (int32_t)win->from_w) * e / 256;
    int32_t ih = (int32_t)win->from_h + ((int32_t)win->to_h - (int32_t)win->from_h) * e / 256;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;

    /* override -> draw -> restore */
    int32_t  ox = win->x, oy = win->y;
    uint32_t ow = win->w, oh = win->h;
    win->x = ix; win->y = iy; win->w = (uint32_t)iw; win->h = (uint32_t)ih;
    render_window_static(buf, w, h, stride, win, focused);
    win->x = ox; win->y = oy; win->w = ow; win->h = oh;
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

    /* right: an ethernet/network indicator (4 signal bars) just left of the
     * clock. Neutral color for now (the compositor has no network-status
     * syscall yet); wire it to a SYS_NET_INFO query later to color it by
     * connected/disconnected. This surfaces the requested taskbar network icon. */
    {
        int32_t nx     = (int32_t)w - clk_w - 12 - 26;
        int32_t base_y = PANEL_H / 2 + 5;
        for (int b = 0; b < 4; b++) {
            int32_t bh = 3 + b * 3;
            fill_rect(buf, w, h, stride, nx + b * 5, base_y - bh, 3, bh, COL_TEXT_DIM);
        }
    }
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

/* ---------------------------------------------------------------------- *
 *  M6: snap preview overlay. While a drag has armed a snap zone, draw a    *
 *  translucent accent rect over the area the window will occupy on release.*
 *  (snap_target_rect / g_snap_armed are defined earlier in the file.)      *
 * ---------------------------------------------------------------------- */
static void render_snap_preview(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (g_snap_armed == SNAP_NONE) return;
    int32_t fx, fy; uint32_t cw, ch;
    if (snap_target_rect(g_snap_armed, &fx, &fy, &cw, &ch) != 0) return;
    /* whole-frame rect (titlebar + client + borders), matching a real window */
    int32_t ox = fx - BORDER_W;
    int32_t oy = fy - BORDER_W;
    int32_t fw = (int32_t)cw + 2 * BORDER_W;
    int32_t fh = (int32_t)ch + TITLEBAR_H + 2 * BORDER_W;
    /* translucent accent fill + a brighter 2px border */
    fill_round_rect(buf, w, h, stride, ox, oy, fw, fh, WIN_RADIUS, 0x500A84FFu);
    stroke_rect(buf, w, h, stride, ox, oy, fw, fh, 0xCC0A84FFu);
    stroke_rect(buf, w, h, stride, ox + 1, oy + 1, fw - 2, fh - 2, 0x880A84FFu);
}

/* ---------------------------------------------------------------------- *
 *  M6: notification toast. A single transient top-right message with      *
 *  fade-in / hold / fade-out, all driven by SYS_GET_TICKS_MS.             *
 * ---------------------------------------------------------------------- */
#define TOAST_MAX_LEN   48
#define TOAST_FADE_MS   220
#define TOAST_PAD_X     14
#define TOAST_PAD_Y     10
#define TOAST_MARGIN    12

static char g_toast_text[TOAST_MAX_LEN + 1] = {0};
static long g_toast_start_ms = 0;
static int32_t g_toast_dur_ms = 0;     /* total visible time incl. fades; 0=off */

/* Show a toast for `dur_ms` total (fade-in + hold + fade-out). */
static void toast_show(const char *msg, int32_t dur_ms) {
    int i = 0;
    while (msg[i] && i < TOAST_MAX_LEN) { g_toast_text[i] = msg[i]; i++; }
    g_toast_text[i] = '\0';
    g_toast_start_ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    g_toast_dur_ms   = dur_ms;
    mark_dirty();    /* PERF: a toast appearing is a visible change; anim_tick
                      * then keeps the frame dirty while it fades in/out.        */
}

/* Draw the toast (if active) with its current fade alpha. */
static void render_toast(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride, long now) {
    if (g_toast_dur_ms <= 0 || g_toast_text[0] == '\0') return;
    long dt = now - g_toast_start_ms;
    if (dt < 0) dt = 0;
    if (dt >= g_toast_dur_ms) { g_toast_dur_ms = 0; g_toast_text[0] = '\0'; return; }

    /* fade-in over the first TOAST_FADE_MS, fade-out over the last TOAST_FADE_MS */
    uint32_t alpha = 256;
    if (dt < TOAST_FADE_MS) {
        alpha = (uint32_t)((dt * 256) / TOAST_FADE_MS);
    } else if (dt > g_toast_dur_ms - TOAST_FADE_MS) {
        long rem = g_toast_dur_ms - dt;
        alpha = (uint32_t)((rem * 256) / TOAST_FADE_MS);
    }
    if (alpha > 256) alpha = 256;
    if (alpha == 0) return;

    int len = (int)k_strlen(g_toast_text);
    int32_t tw = len * FONT_W + 2 * TOAST_PAD_X;
    int32_t th = FONT_H + 2 * TOAST_PAD_Y;
    int32_t tx = (int32_t)w - tw - TOAST_MARGIN;
    /* (M8) clamp tx so a very long toast can't produce a negative x and
     * cause the background rect to be drawn off the left edge of the screen. */
    if (tx < 4) tx = 4;
    int32_t ty = PANEL_H + TOAST_MARGIN;            /* just below the panel */

    /* panel background (rounded) blended at the fade alpha */
    {
        /* approximate alpha-fill by blending COL_PANEL with alpha into a rounded
         * rect: do a rounded shadow then a translucent fill + accent border. */
        blend_rect(buf, w, h, stride, tx + 3, ty + 4, tw, th, 0x40000000u);  /* soft shadow */
        /* rounded translucent body */
        uint32_t body = 0xFF000000u | (COL_PANEL & 0x00FFFFFFu);
        /* manual rounded fill with alpha: reuse fill_round_rect by pre-blending
         * not trivial; instead blend a plain rounded rect span set. */
        int32_t r = 8;
        uint32_t ba = (alpha * 0xE0u) >> 8;          /* body ~88% * fade */
        uint32_t bcol = (ba << 24) | (COL_PANEL & 0x00FFFFFFu);
        (void)body;
        blend_rect(buf, w, h, stride, tx, ty + r, tw, th - 2 * r, bcol);
        blend_rect(buf, w, h, stride, tx + r, ty, tw - 2 * r, r, bcol);
        blend_rect(buf, w, h, stride, tx + r, ty + th - r, tw - 2 * r, r, bcol);
        for (int32_t dy = 0; dy < r; dy++)
            for (int32_t dx = 0; dx < r; dx++) {
                int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
                if (off > (r - 1) * (r - 1)) continue;
                blend_rect(buf, w, h, stride, tx + dx, ty + dy, 1, 1, bcol);
                blend_rect(buf, w, h, stride, tx + tw - 1 - dx, ty + dy, 1, 1, bcol);
                blend_rect(buf, w, h, stride, tx + dx, ty + th - 1 - dy, 1, 1, bcol);
                blend_rect(buf, w, h, stride, tx + tw - 1 - dx, ty + th - 1 - dy, 1, 1, bcol);
            }
        /* accent left bar */
        uint32_t acol = (alpha << 24) | (COL_ACCENT & 0x00FFFFFFu);
        blend_rect(buf, w, h, stride, tx, ty + r, 3, th - 2 * r, acol);
    }

    /* text: scale the text color's apparent brightness by alpha (blend toward
     * the body) so it fades together. We approximate by drawing at COL_TEXT --
     * the font has no alpha param, so for partial fades we draw only when the
     * toast is mostly visible to avoid harsh popping. */
    if (alpha >= 64) {
        font_draw_string(buf, (int)stride, (int)w, (int)h,
                         tx + TOAST_PAD_X, ty + TOAST_PAD_Y, g_toast_text, COL_TEXT);
    }
}

/* forward decl for point_in (defined later in the mouse-interaction section) */
static int point_in(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h);

/* ======================================================================
 * M8: RIGHT DOCK HELPERS
 * ====================================================================== */

/* M8 dock-layout fix: the two FOLDERS (data indices RDOCK_NICONS .. TOTAL-1)
 * must ALWAYS be visible/clickable, otherwise the rainbow fan-out is
 * unreachable. We map every data index to a VISUAL slot so the folders render
 * at the TOP of the strip (visual slots 0..NFOLDERS-1) and the apps render
 * below (visual slots NFOLDERS..). The off-screen cull then only drops
 * overflow APPS at the bottom, never the folders. Only the layout/draw/hit-test
 * ORDER changes; the rdock_apps[]/rdock_folders[] data is untouched and every
 * site keys off rdock_ref_cy(), so draw and hit-test stay in lockstep. */
static int rdock_visual_slot(int idx) {
    if (idx >= RDOCK_NICONS)               /* folder -> top */
        return idx - RDOCK_NICONS;
    return RDOCK_NFOLDERS + idx;           /* app -> below the folders */
}

/* Return the un-magnified Y center of icon slot [idx] in the dock strip.
 * The strip starts at y = PANEL_H + RDOCK_MARGIN_TOP and stacks downward,
 * indexed by the VISUAL slot (folders first). We don't pre-compute because
 * magnification shifts everything dynamically; the reference layout is the
 * settled (no magnification) positions. */
static int32_t rdock_ref_cy(int idx) {
    int slot = rdock_visual_slot(idx);
    return PANEL_H + RDOCK_MARGIN_TOP + slot * (RDOCK_ICON_BASE + RDOCK_PAD)
           + RDOCK_ICON_BASE / 2;
}

/* Compute magnification scale (Q8) for a slot given cursor Y. Uses integer
 * fixed-point only:
 *   dy = |cursor_y - ref_cy|
 *   extra = RDOCK_MAG_MAX_EXTRA * max(0, INFLUENCE - dy) / INFLUENCE
 *   scale = 256 + extra                 (256 = 1.0 in Q8)
 */
static int32_t rdock_mag_scale(int idx, int32_t cursor_y) {
    int32_t cy   = rdock_ref_cy(idx);
    int32_t dy   = cursor_y - cy;
    if (dy < 0) dy = -dy;
    if (dy >= RDOCK_MAG_INFLUENCE) return 256;
    int32_t extra = (int32_t)((long)RDOCK_MAG_MAX_EXTRA * (RDOCK_MAG_INFLUENCE - dy)
                              / RDOCK_MAG_INFLUENCE);
    return 256 + extra;
}

/* Given the magnified scale (Q8) and the base size, return the actual pixel size. */
static int32_t rdock_scaled_sz(int32_t scale_q8) {
    return (int32_t)((long)RDOCK_ICON_BASE * scale_q8 / 256);
}

/* Compute the TOP-Y of icon slot [idx] given current scale array.
 * Stacks magnified icons from RDOCK_MARGIN_TOP below the panel; each icon
 * takes max(base, scaled) height + RDOCK_PAD spacing. We keep icons centered
 * on their reference position: top = ref_cy - sz/2. */
static int32_t rdock_icon_top(int idx) {
    int32_t sz = rdock_scaled_sz(g_rdock_icons[idx].scale_q8);
    return rdock_ref_cy(idx) - sz / 2;
}

/* X of the left edge of the dock strip (icon tiles are centered in the strip).*/
static int32_t rdock_strip_x(uint32_t W) {
    return (int32_t)W - RDOCK_W;
}

/* X of the left edge of an icon tile (centered in the strip). */
static int32_t rdock_icon_x(uint32_t W, int32_t sz) {
    int32_t strip_x = rdock_strip_x(W);
    return strip_x + (RDOCK_W - sz) / 2;
}

/* ---- Bounce offset (horizontal, leftward) for a slot.
 * Uses a decaying sine approximation in fixed-point.
 * bounce_phase in [0,256] maps to one full cycle.
 * Offset = AMP * sin(phase * pi) * decay
 * We approximate sin(t*pi) where t in [0,1] as:
 *   4*t*(1-t)  (a parabola, peaks at 0.5)
 * The result is positive (leftward displacement). */
static int32_t rdock_bounce_offset(int slot, long now) {
    rdock_icon_state_t *st = &g_rdock_icons[slot];
    if (!st->bounce_active) return 0;
    long dt = now - st->bounce_start;
    if (dt < 0) dt = 0;
    if (dt >= RDOCK_BOUNCE_MS) { st->bounce_active = 0; return 0; }
    /* linear phase 0..256 */
    int32_t phase = (int32_t)(dt * 256 / RDOCK_BOUNCE_MS);
    /* two half-bounces: t in [0,128] and [128,256] with decaying amplitude */
    int32_t t, amp;
    if (phase < 128) {
        t = phase * 2;               /* 0..256 for first half */
        amp = RDOCK_BOUNCE_AMP;
    } else {
        t = (phase - 128) * 2;
        amp = RDOCK_BOUNCE_AMP / 2;  /* second bounce is smaller */
    }
    /* sin-approx: 4*t*(256-t)/256^2 * amp */
    int32_t s = (int32_t)((long)4 * t * (256 - t) / 65536);  /* 0..256 */
    return (int32_t)((long)amp * s / 256);
}

/* ---- Update magnification scales each frame ---- */
static void rdock_update_scales(int32_t cursor_x, int32_t cursor_y, uint32_t W) {
    int32_t strip_x = rdock_strip_x(W);
    int on_dock = (cursor_x >= strip_x);
    for (int i = 0; i < RDOCK_TOTAL; i++) {
        int32_t target;
        if (on_dock) {
            target = rdock_mag_scale(i, cursor_y);
        } else {
            target = 256;  /* no magnification when cursor off dock */
        }
        g_rdock_icons[i].scale_target = target;
        /* smooth toward target */
        int32_t cur = g_rdock_icons[i].scale_q8;
        int32_t diff = target - cur;
        cur += diff - (diff >> RDOCK_SMOOTH_SHIFT);
        /* clamp */
        if (cur < 256) cur = 256;
        if (cur > 256 + RDOCK_MAG_MAX_EXTRA) cur = 256 + RDOCK_MAG_MAX_EXTRA;
        g_rdock_icons[i].scale_q8 = cur;
    }
}

/* ---- Draw a 2x2 mini-grid inside a tile for folder icons ---- */
static void draw_folder_grid(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                             int32_t tx, int32_t ty, int32_t tsz, int fidx) {
    const rdock_folder_t *f = &rdock_folders[fidx];
    int32_t inner = tsz / 2 - 4;
    if (inner < 4) inner = 4;
    /* 2x2 sub-tiles */
    static const int32_t gox[4] = {0, 1, 0, 1};
    static const int32_t goy[4] = {0, 0, 1, 1};
    static const uint32_t gcols[4] = {
        0xFF5588BBu, 0xFF88BB55u, 0xFFBB5588u, 0xFF55BBBBu
    };
    for (int m = 0; m < (f->nmembers < 4 ? f->nmembers : 4); m++) {
        int32_t gx = tx + 3 + gox[m] * (inner + 2);
        int32_t gy = ty + 3 + goy[m] * (inner + 2);
        /* member procedural icon (draws its own tile background) */
        if (f->members[m] >= 0 && f->members[m] < RDOCK_NICONS) {
            icon_for_app(buf, (int)stride, gx, gy, inner,
                         rdock_apps[f->members[m]].path + 5);  /* skip "sbin/" */
        } else {
            fill_round_rect(buf, bw, bh, stride, gx, gy, inner, inner, 3, gcols[m]);
        }
    }
}

/* ====================================================================== *
 *  M8+: RAINBOW FAN-OUT  (replaces the rectangular folder popover)        *
 *                                                                         *
 *  When a folder is open, its member app icons sweep OUT from the folder  *
 *  tile along a ~160-degree semicircle (a "rainbow") into the workspace.  *
 *  Each icon floats with a gentle vertical bob and twinkles with sparkle  *
 *  particles while the cursor hovers it.                                  *
 *                                                                         *
 *  CRITICAL CLAMP: icon_for_app() clips only against the cell it is told  *
 *  to draw, NOT the screen buffer, so an off-screen cell writes OOB. We   *
 *  compute the arc position then clamp the FULL tile inside the buffer:   *
 *     x in [4, w - RDOCK_W - tile - 4]                                    *
 *     y in [PANEL_H + 4, h - DOCK_H - tile - 4]                           *
 *  Sparkles are drawn with blend_rect (already buffer-clipped) and are    *
 *  additionally guarded so they never start a write off-screen.          *
 * ---------------------------------------------------------------------- */

/* Compute the clamped, on-screen top-left rect of fanned-out member slot [m]
 * of folder [fidx]. anim_t256 is the open progress (0..256). `now` drives the
 * floating bob. Writes the clamped tile origin to *ox/*oy and size to *otile.
 * Returns 1 if the member is valid (and *omi = app index), else 0. */
static int rdock_fan_member_rect(int fidx, int m, int32_t anim_t256, long now,
                                 uint32_t W, uint32_t H,
                                 int32_t *ox, int32_t *oy, int32_t *otile, int *omi) {
    const rdock_folder_t *f = &rdock_folders[fidx];
    if (m < 0 || m >= f->nmembers) return 0;
    int mi = f->members[m];
    if (mi < 0 || mi >= RDOCK_NICONS) return 0;

    int32_t tile = RDOCK_FAN_TILE;
    int dock_idx = RDOCK_NICONS + fidx;
    int32_t sz   = rdock_scaled_sz(g_rdock_icons[dock_idx].scale_q8);

    /* anchor the arc on the folder tile center */
    int32_t cx0 = rdock_strip_x(W) + RDOCK_W / 2;
    int32_t cy0 = rdock_icon_top(dock_idx) + sz / 2;

    /* radius grows with eased open progress */
    int32_t prog = ease_out_cubic(clamp256(anim_t256));
    int32_t radius = (int32_t)((long)RDOCK_FAN_RADIUS * prog / 256);

    /* angle: arc centered on 180 deg (straight LEFT into the workspace),
     * spread over RDOCK_FAN_ARC_DEG. For a single member, sit dead-center. */
    int n = f->nmembers;
    int32_t ang;
    if (n <= 1) {
        ang = 180;
    } else {
        int32_t start = 180 - RDOCK_FAN_ARC_DEG / 2;          /* top of rainbow */
        int32_t step  = RDOCK_FAN_ARC_DEG / (n - 1);
        ang = start + m * step;
    }

    /* arc center-of-tile position (cos negative => leftward) */
    int32_t mx = cx0 + (int32_t)((long)cos_q(ang) * radius / 256);
    int32_t my = cy0 + (int32_t)((long)sin_q(ang) * radius / 256);

    /* floating bob: offset = sin(now/8 + m*40) * AMP   (degrees in sin_q) */
    int32_t bob = (int32_t)((long)sin_q((int32_t)(now / 8) + m * 40)
                            * RDOCK_FAN_BOB_AMP / 256);
    my += bob;

    /* convert center -> top-left */
    int32_t x = mx - tile / 2;
    int32_t y = my - tile / 2;

    /* CRITICAL clamp: keep the whole tile inside the back buffer so the
     * unclipped icon library never scribbles past it. */
    int32_t xmin = 4;
    int32_t xmax = (int32_t)W - RDOCK_W - tile - 4;
    int32_t ymin = PANEL_H + 4;
    int32_t ymax = (int32_t)H - DOCK_H - tile - 4;
    if (xmax < xmin) xmax = xmin;     /* degenerate tiny screen: pin to xmin */
    if (ymax < ymin) ymax = ymin;
    if (x < xmin) x = xmin;
    if (x > xmax) x = xmax;
    if (y < ymin) y = ymin;
    if (y > ymax) y = ymax;

    *ox = x; *oy = y; *otile = tile; *omi = mi;
    return 1;
}

/* Draw ~RDOCK_FAN_SPARKLES twinkling sparkle dots orbiting a hovered tile.
 * Cheap: tiny blend_rects (buffer-clipped) at sin/cos-animated offsets with a
 * twinkling alpha. Each sparkle is additionally skipped if it would begin a
 * write off-screen. */
static void rdock_draw_sparkles(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                                int32_t tx, int32_t ty, int32_t tile, long now) {
    int32_t cx = tx + tile / 2;
    int32_t cy = ty + tile / 2;
    int32_t orbit = tile / 2 + 6;
    for (int s = 0; s < RDOCK_FAN_SPARKLES; s++) {
        int32_t phase = s * (360 / RDOCK_FAN_SPARKLES);
        /* orbit angle advances with time; radius pulses a little */
        int32_t ang = (int32_t)(now / 4) + phase;
        int32_t rr  = orbit + (int32_t)((long)sin_q((int32_t)(now / 3) + phase) * 4 / 256);
        int32_t sx = cx + (int32_t)((long)cos_q(ang) * rr / 256);
        int32_t sy = cy + (int32_t)((long)sin_q(ang) * rr / 256);
        /* twinkle alpha 0x40..0xF0 from a sine of time */
        int32_t tw = sin_q((int32_t)(now / 2) + s * 67);   /* -256..256 */
        int32_t a  = 0x90 + tw * 0x60 / 256;               /* 0x30..0xF0 */
        if (a < 0x20) a = 0x20;
        uint32_t col = ((uint32_t)a << 24) | 0x00FFF4C0u;  /* warm white spark */
        /* dot size pulses 1..2 px; only draw fully on-screen */
        int32_t dsz = 1 + ((tw > 0) ? 1 : 0);
        if (sx < 0 || sy < 0 ||
            sx + dsz > (int32_t)bw || sy + dsz > (int32_t)bh) continue;
        blend_rect(buf, bw, bh, stride, sx, sy, dsz, dsz, col);
        /* tiny cross arms for a "star" feel (each still buffer-clipped) */
        blend_rect(buf, bw, bh, stride, sx - 1, sy, 1, 1, (col & 0x00FFFFFFu) | 0x50000000u);
        blend_rect(buf, bw, bh, stride, sx + dsz, sy, 1, 1, (col & 0x00FFFFFFu) | 0x50000000u);
    }
}

/* ---- Draw a folder's rainbow fan-out (open state) ---- */
static void draw_folder_fanout(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                               int fidx, uint32_t W, uint32_t H,
                               int32_t anim_t256, int32_t cur_x, int32_t cur_y, long now) {
    const rdock_folder_t *f = &rdock_folders[fidx];
    if (anim_t256 <= 0) return;

    for (int m = 0; m < f->nmembers; m++) {
        int32_t x, y, tile; int mi;
        if (!rdock_fan_member_rect(fidx, m, anim_t256, now, W, H, &x, &y, &tile, &mi))
            continue;

        int hovered = (cur_x >= x && cur_x < x + tile &&
                       cur_y >= y && cur_y < y + tile);

        /* sparkles UNDER the icon while hovered */
        if (hovered) rdock_draw_sparkles(buf, bw, bh, stride, x, y, tile, now);

        /* floating member icon (draws its own tile background, now safely
         * clamped fully inside the buffer) */
        icon_for_app(buf, (int)stride, x, y, tile, rdock_apps[mi].path + 5);

        /* hover ring to make the focused member pop */
        if (hovered)
            stroke_rect(buf, bw, bh, stride, x - 1, y - 1, tile + 2, tile + 2, COL_ACCENT);
    }
}

/* ---- Main right-dock render function ---- */
static void render_right_dock(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                               int32_t cur_x, int32_t cur_y, long now) {
    int32_t strip_x = rdock_strip_x(w);

    /* dock strip background */
    fill_rect(buf, w, h, stride, strip_x, PANEL_H, RDOCK_W,
              (int32_t)h - PANEL_H - DOCK_H, 0xE02C2C2Eu);
    /* left border */
    fill_rect(buf, w, h, stride, strip_x, PANEL_H, 1,
              (int32_t)h - PANEL_H - DOCK_H, COL_BORDER);

    g_rdock_hovered = -1;

    for (int i = 0; i < RDOCK_TOTAL; i++) {
        int32_t sz   = rdock_scaled_sz(g_rdock_icons[i].scale_q8);
        int32_t ty   = rdock_icon_top(i);
        int32_t tx   = rdock_icon_x(w, sz);
        /* bounce: shift left */
        int32_t boff = rdock_bounce_offset(i, now);
        tx -= boff;

        /* Keep the (possibly hover-magnified) tile fully inside the buffer WIDTH.
         * icon_for_app / ic_fill_rect draw their rows UNCLIPPED, and a real-HW
         * framebuffer whose pitch == width*4 (e.g. the T410: 1024 wide, pitch 4096)
         * has NO stride slack -- so a tile overflowing the RIGHT edge wraps onto the
         * next row's LEFT edge: the "left bleed" (#77). Anchor the magnified tile to
         * the right edge so it grows LEFTWARD (into the screen) instead of off-screen.
         * Clamp BEFORE the hover test so detection matches the drawn position. QEMU
         * hides this only because its VBE reports a padded pitch > width*4. */
        if (tx + sz > (int32_t)w) tx = (int32_t)w - sz;
        if (tx < 0) tx = 0;

        /* Skip any icon whose (possibly hover-magnified) tile is not FULLY
         * inside the drawable strip. icon_for_app / ic_fill_rect write their
         * tile rows UNCLIPPED, so a partially-off-screen tile would scribble
         * past the back buffer and fault the compositor. With more dock items
         * than fit vertically, the overflow ones are simply not drawn here
         * (they remain reachable via the start menu). */
        if (ty < PANEL_H || ty + sz > (int32_t)h - DOCK_H) continue;

        /* hover detection (magnified rect) */
        int hovered = (cur_x >= tx && cur_x < tx + sz &&
                       cur_y >= ty && cur_y < ty + sz);
        if (hovered) g_rdock_hovered = i;

        uint32_t tile_col;
        int is_folder = (i >= RDOCK_NICONS);

        if (is_folder) {
            int fi = i - RDOCK_NICONS;
            tile_col = rdock_folders[fi].color;
        } else {
            tile_col = rdock_apps[i].color;
        }

        /* hover highlight: lighten the tile */
        if (hovered) {
            uint32_t hr = ((tile_col >> 16) & 0xFF) + 0x30;
            uint32_t hg = ((tile_col >>  8) & 0xFF) + 0x30;
            uint32_t hb = ( tile_col        & 0xFF) + 0x30;
            if (hr > 0xFF) hr = 0xFF;
            if (hg > 0xFF) hg = 0xFF;
            if (hb > 0xFF) hb = 0xFF;
            tile_col = 0xFF000000u | (hr << 16) | (hg << 8) | hb;
        }

        if (is_folder) {
            /* tile background + 2x2 mini-grid */
            fill_round_rect(buf, w, h, stride, tx, ty, sz, sz, RDOCK_CORNER, tile_col);
            draw_folder_grid(buf, w, h, stride, tx, ty, sz, i - RDOCK_NICONS);
        } else {
            /* recognizable procedural icon (draws its own tile background) */
            icon_for_app(buf, (int)stride, tx, ty, sz, rdock_apps[i].path + 5);
        }

        /* tooltip: show full name to the LEFT when hovered */
        if (hovered && !is_folder) {
            const char *tip = rdock_apps[i].path + 5;  /* skip "sbin/" */
            int tip_len = 0;
            while (tip[tip_len]) tip_len++;
            int32_t tip_w = tip_len * FONT_W + 16;
            int32_t tip_h = FONT_H + 8;
            int32_t tip_x = strip_x - tip_w - 6;
            int32_t tip_y = ty + sz / 2 - tip_h / 2;
            if (tip_x < 4) tip_x = 4;
            fill_round_rect(buf, w, h, stride, tip_x, tip_y, tip_w, tip_h, 5, 0xF0111111u);
            stroke_rect(buf, w, h, stride, tip_x, tip_y, tip_w, tip_h, COL_BORDER);
            font_draw_string(buf, (int)stride, (int)w, (int)h,
                             tip_x + 8, tip_y + 4, tip, COL_TEXT);
        }
    }

    /* ---- draw open folders as rainbow fan-outs ---- */
    for (int fi = 0; fi < RDOCK_NFOLDERS; fi++) {
        rdock_folder_state_t *fs = &g_rdock_folders[fi];
        if (!fs->open && fs->anim_t <= 0) continue;

        /* advance fan open/close animation (anim_t Q8 0..256) */
        long dt = now - fs->anim_start;
        int32_t target_t = fs->anim_closing ? 0 : 256;
        int32_t speed = (int32_t)(dt * 256 / RDOCK_POPOVER_ANIM_MS);
        if (speed > 256) speed = 256;
        if (fs->anim_closing) {
            fs->anim_t = 256 - speed;
            if (fs->anim_t <= 0) { fs->anim_t = 0; fs->open = 0; }
        } else {
            fs->anim_t = speed;
            if (fs->anim_t > 256) fs->anim_t = 256;
        }
        (void)target_t;

        draw_folder_fanout(buf, w, h, stride, fi, w, h, fs->anim_t,
                           cur_x, cur_y, now);
    }
}

/* ---- Hit-test + spawn for the right dock on left-click ---- */
/* Returns 1 if the click was consumed by the right dock, else 0. */
static int rdock_handle_click(int32_t cx, int32_t cy, uint32_t W, long now) {
    int32_t strip_x = rdock_strip_x(W);

    /* Click outside the dock strip entirely? The rainbow fan-out lives out in
     * the workspace, so member icons are hit-tested here. */
    if (cx < strip_x) {
        int any_open = 0;

        /* 1) Did the click land on a fanned-out member icon? Use the IDENTICAL
         * clamped geometry as the renderer so hit-test == what is drawn. */
        for (int fi = 0; fi < RDOCK_NFOLDERS; fi++) {
            rdock_folder_state_t *fs = &g_rdock_folders[fi];
            if (!fs->open && fs->anim_t <= 0) continue;
            any_open = 1;
            const rdock_folder_t *f = &rdock_folders[fi];
            for (int m = 0; m < f->nmembers; m++) {
                int32_t x, y, tile; int mi;
                if (!rdock_fan_member_rect(fi, m, fs->anim_t, now,
                                           g_fb_w, g_fb_h, &x, &y, &tile, &mi))
                    continue;
                if (point_in(cx, cy, x, y, tile, tile)) {
                    print("[SHELL] dock folder spawn: ");
                    print(rdock_apps[mi].path);
                    print("\n");
                    long r = syscall(SYS_SPAWN, (long)rdock_apps[mi].path, 0, 0);
                    if (r < 0) { print("[SHELL] spawn fail r="); print_num(r); print("\n"); }
                    /* bounce the folder tile to acknowledge the launch */
                    g_rdock_icons[RDOCK_NICONS + fi].bounce_active = 1;
                    g_rdock_icons[RDOCK_NICONS + fi].bounce_start  = now;
                    return 1;
                }
            }
        }

        /* 2) Click elsewhere while a fan is open: close it (reverse anim). */
        if (any_open) {
            for (int fi = 0; fi < RDOCK_NFOLDERS; fi++) {
                rdock_folder_state_t *fs = &g_rdock_folders[fi];
                if (fs->open || fs->anim_t > 0) {
                    fs->anim_closing = 1;
                    fs->anim_start   = now;
                }
            }
            g_rdock_open_folder = -1;
            return 1;   /* consume: dismissed the fan rather than hitting desktop */
        }
        return 0;
    }

    /* Click on the dock strip: hit-test each icon using magnified rect */
    for (int i = 0; i < RDOCK_TOTAL; i++) {
        int32_t sz   = rdock_scaled_sz(g_rdock_icons[i].scale_q8);
        int32_t ty   = rdock_icon_top(i);
        int32_t tx   = rdock_icon_x(W, sz);
        int32_t boff = rdock_bounce_offset(i, now);
        tx -= boff;

        if (!point_in(cx, cy, tx, ty, sz, sz)) continue;

        int is_folder = (i >= RDOCK_NICONS);
        if (is_folder) {
            int fi = i - RDOCK_NICONS;
            rdock_folder_state_t *fs = &g_rdock_folders[fi];
            if (fs->open || (!fs->anim_closing && fs->anim_t > 0)) {
                /* collapse */
                fs->anim_closing = 1;
                fs->anim_start   = now;
                g_rdock_open_folder = -1;
            } else {
                /* close any other open folder */
                if (g_rdock_open_folder >= 0 && g_rdock_open_folder != fi) {
                    rdock_folder_state_t *ofs = &g_rdock_folders[g_rdock_open_folder];
                    ofs->anim_closing = 1;
                    ofs->anim_start   = now;
                }
                /* open this folder */
                fs->open         = 1;
                fs->anim_closing = 0;
                fs->anim_start   = now;
                fs->anim_t       = 0;
                g_rdock_open_folder = fi;
            }
            /* bounce */
            g_rdock_icons[i].bounce_active = 1;
            g_rdock_icons[i].bounce_start  = now;
            print("[SHELL] dock folder toggle fi="); print_num(fi); print("\n");
        } else {
            /* spawn the app */
            print("[SHELL] dock launch: "); print(rdock_apps[i].path); print("\n");
            long r = syscall(SYS_SPAWN, (long)rdock_apps[i].path, 0, 0);
            if (r < 0) { print("[SHELL] spawn fail r="); print_num(r); print("\n"); }
            /* start bounce */
            g_rdock_icons[i].bounce_active = 1;
            g_rdock_icons[i].bounce_start  = now;
        }
        return 1;
    }
    return 1;  /* consumed: any click on the strip is dock territory */
}

/* Render the popup menu (start / context) on top of the scene, under cursor. */
static void draw_menu(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (!g_menu_open) return;
    int32_t mh = menu_height();
    blend_rect(buf, w, h, stride, g_menu_x + 5, g_menu_y + 6, MENU_W, mh, 0x60000000u); /* shadow */
    fill_round_rect(buf, w, h, stride, g_menu_x, g_menu_y, MENU_W, mh, 8, 0xFF1C2230u); /* panel */
    const char *title = g_menu_is_ctx ? "Actions" : "AutomationOS";
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     g_menu_x + 12, g_menu_y + (MENU_HDR_H - FONT_H) / 2, title, COL_ACCENT);
    blend_rect(buf, w, h, stride, g_menu_x + 8, g_menu_y + MENU_HDR_H - 1,
               MENU_W - 16, 1, 0x40FFFFFFu); /* divider */
    for (int i = 0; i < g_menu_n; i++) {
        int32_t ry = g_menu_y + MENU_HDR_H + i * MENU_ROW_H;
        if (i == g_menu_hover)
            fill_round_rect(buf, w, h, stride, g_menu_x + 4, ry + 1,
                            MENU_W - 8, MENU_ROW_H - 2, 4, COL_ACCENT);
        font_draw_string(buf, (int)stride, (int)w, (int)h,
                         g_menu_x + 14, ry + (MENU_ROW_H - FONT_H) / 2,
                         g_menu_label[i], COL_TEXT);
    }
}

static int about_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Modal "About" panel: a centered card with the OS name + credits. Dismissed by
 * any click (handled in handle_mouse). Drawn above the menu, beneath the cursor. */
static void draw_about(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (!g_about_open) return;

    /* dim the whole screen so it reads as a modal dialog */
    blend_rect(buf, w, h, stride, 0, 0, (int32_t)w, (int32_t)h, 0x88000000u);

    int32_t pw = 440, ph = 210;
    int32_t px = ((int32_t)w - pw) / 2;
    int32_t py = ((int32_t)h - ph) / 2;
    blend_rect(buf, w, h, stride, px + 6, py + 10, pw, ph, 0x60000000u);          /* shadow */
    fill_round_rect(buf, w, h, stride, px, py, pw, ph, 12, 0xFF1C2230u);          /* panel  */
    fill_round_rect(buf, w, h, stride, px, py, pw, 44, 12, 0xFF2A3552u);          /* header */

    const char *l_title = "AutomationOS";
    const char *l_ver   = "v0.1.0  -  from-scratch x86_64 OS";
    const char *l_cred  = "created by fourzerofour & claude";
    const char *l_hint  = "(click anywhere to close)";

    int32_t tx_title = px + (pw - about_slen(l_title) * FONT_W) / 2;
    int32_t tx_ver   = px + (pw - about_slen(l_ver)   * FONT_W) / 2;
    int32_t tx_cred  = px + (pw - about_slen(l_cred)  * FONT_W) / 2;
    int32_t tx_hint  = px + (pw - about_slen(l_hint)  * FONT_W) / 2;

    font_draw_string(buf, (int)stride, (int)w, (int)h, tx_title, py + (44 - FONT_H) / 2, l_title, COL_ACCENT);
    font_draw_string(buf, (int)stride, (int)w, (int)h, tx_ver,   py + 78,  l_ver,  COL_TEXT_DIM);
    font_draw_string(buf, (int)stride, (int)w, (int)h, tx_cred,  py + 118, l_cred, COL_TEXT);
    font_draw_string(buf, (int)stride, (int)w, (int)h, tx_hint,  py + 168, l_hint, COL_TEXT_DIM);
}

/* ====================================================================== *
 *  PERF: on-screen stats overlay (the "measure first" piece)              *
 * ---------------------------------------------------------------------- *
 *  A tiny top-left box drawn each presented frame showing live FPS, frame *
 *  time (ms), composited window count, and pixels pushed to the FB by the *
 *  last present_diff. This is what lets the owner watch "1 app vs 5 apps"  *
 *  on the T410 and see where the time goes. Kept cheap: a handful of small *
 *  fill_rects + ~4 short strings, so measuring never dominates the frame.  *
 *  Toggle at runtime with Alt+S (see wm_handle_key); default-on via        *
 *  COMPOSITOR_STATS.                                                       *
 * ---------------------------------------------------------------------- */

/* Append unsigned `v` (base 10) to dst[] at *pos, bounded by cap. */
static void stat_putu(char *dst, int *pos, int cap, long v) {
    char tmp[16]; int n = 0;
    if (v < 0) v = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0 && n < 16);
    while (n > 0 && *pos < cap - 1) dst[(*pos)++] = tmp[--n];
}
/* Append a literal string. */
static void stat_puts(char *dst, int *pos, int cap, const char *s) {
    while (*s && *pos < cap - 1) dst[(*pos)++] = *s++;
}

/* Count windows that composite() would actually blit this frame (live, not
 * minimized-and-parked). Mirrors the visibility test in the window loop so the
 * overlay's "win" number matches the real per-frame compositing cost. */
static int stats_visible_windows(void) {
    int n = 0;
    for (int32_t i = 0; i < g_zcount; i++) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
        if (win->phase == PH_NONE && win->minimized) continue;  /* parked: skipped */
        n++;
    }
    return n;
}

/* Draw the overlay into the back buffer (so present_diff picks it up). Reads the
 * sampled g_fps_x10 / g_frame_dt_ms / g_present_px globals (updated once per
 * presented frame in the frame loop). */
static void render_stats_overlay(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (!g_stats_on) return;

    /* Build two short lines so the box stays small.
     *   line1: "FPS 59.4  3.2ms"
     *   line2: "win 5  px 18240"                                            */
    char l1[40]; int p1 = 0;
    stat_puts(l1, &p1, (int)sizeof(l1), "FPS ");
    stat_putu(l1, &p1, (int)sizeof(l1), g_fps_x10 / 10);
    stat_puts(l1, &p1, (int)sizeof(l1), ".");
    stat_putu(l1, &p1, (int)sizeof(l1), g_fps_x10 % 10);
    stat_puts(l1, &p1, (int)sizeof(l1), "  ");
    stat_putu(l1, &p1, (int)sizeof(l1), g_frame_dt_ms);
    stat_puts(l1, &p1, (int)sizeof(l1), "ms");
    l1[p1] = '\0';

    char l2[40]; int p2 = 0;
    stat_puts(l2, &p2, (int)sizeof(l2), "win ");
    stat_putu(l2, &p2, (int)sizeof(l2), stats_visible_windows());
    stat_puts(l2, &p2, (int)sizeof(l2), "  px ");
    stat_putu(l2, &p2, (int)sizeof(l2), (long)g_present_px);
    l2[p2] = '\0';

    /* Box geometry: top-left, just below the panel so it never fights the panel
     * title. Width sized to the longer line. */
    int len = p1 > p2 ? p1 : p2;
    int32_t bx = 8;
    int32_t by = PANEL_H + 6;
    int32_t bw = len * FONT_W + 12;
    int32_t bh = 2 * FONT_H + 10;

    /* translucent dark plate + thin accent border (cheap, readable) */
    blend_rect(buf, w, h, stride, bx, by, bw, bh, 0xC0101418u);
    stroke_rect(buf, w, h, stride, bx, by, bw, bh, 0x800A84FFu);

    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     bx + 6, by + 5, l1, 0xFF6FE26Fu);            /* green-ish */
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     bx + 6, by + 5 + FONT_H, l2, 0xFFE2E27Au);   /* amber-ish */
}

/* ====================================================================== */
static void composite(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                      int32_t cursor_x, int32_t cursor_y, long now) {
    render_desktop(buf, w, h, stride);

    /* M8: /Desktop contents as labeled icons (beneath windows + chrome) */
    render_desktop_icons(buf, w, h, stride, cursor_x, cursor_y);

    /* windows, back to front */
    int top = focused_slot();
    for (int32_t i = 0; i < g_zcount; i++) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
        if (win->phase == PH_SNAPPING) {
            render_window_snapping(buf, w, h, stride, win, slot == top, now);
        } else if (win->phase != PH_NONE) {
            render_window_anim(buf, w, h, stride, win, slot == top, now);
        } else if (win->minimized) {
            continue;                       /* parked: only its taskbar button shows */
        } else {
            render_window_static(buf, w, h, stride, win, slot == top);
        }
    }

    /* M6: snap preview over windows (below chrome) */
    render_snap_preview(buf, w, h, stride);

    /* always-on-top chrome (drawn AFTER windows) */
    render_panel(buf, w, h, stride);
    render_dock(buf, w, h, stride, cursor_x, cursor_y);

    /* M8: right vertical dock (drawn after bottom dock so it paints over the
     * bottom-right corner where they meet) */
    rdock_update_scales(cursor_x, cursor_y, w);
    render_right_dock(buf, w, h, stride, cursor_x, cursor_y, now);

    /* M6: notification toast above the chrome */
    render_toast(buf, w, h, stride, now);

    /* popup menu (start / context) above the chrome, beneath the cursor */
    draw_menu(buf, w, h, stride);

    /* modal About dialog above everything except the cursor */
    draw_about(buf, w, h, stride);

    /* PERF: stats overlay above the chrome, beneath the cursor (so the cursor
     * is never occluded by it). Cheap; gated by g_stats_on / COMPOSITOR_STATS. */
    render_stats_overlay(buf, w, h, stride);

    /* cursor on very top */
    draw_cursor(buf, w, h, stride, cursor_x, cursor_y);
}

static void present(uint32_t *fb, uint32_t *back, uint32_t h, uint32_t stride) {
    uint32_t total = h * stride;
    for (uint32_t i = 0; i < total; i++) fb[i] = back[i];
}

/* Dirty-rectangle present: copy only the bounding box of pixels that changed
 * since the previous frame (back vs prev) into the hardware framebuffer, then
 * update prev. The back-vs-prev scan is normal CACHED RAM (fast); only the
 * changed rectangle is written to the SLOW linear framebuffer. On a static or
 * lightly-animated desktop this turns a full ~3MB MMIO write per frame into a
 * tiny one -- the dominant cost on a slow framebuffer like the ThinkPad T410's.
 * Worst case (everything changed) it degrades to a full copy == present().
 * `w` is the visible width (<= stride); columns in [w,stride) are padding and
 * are left untouched (they're never displayed). */
static uint32_t present_diff(uint32_t *fb, uint32_t *back, uint32_t *prev,
                             uint32_t w, uint32_t h, uint32_t stride) {
    uint32_t minx = w, miny = h, maxx = 0, maxy = 0;
    int any = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t off = y * stride;
        for (uint32_t x = 0; x < w; x++) {
            if (back[off + x] != prev[off + x]) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                any = 1;
            }
        }
    }
    if (!any) return 0;               /* nothing changed: skip the fb write    */
    for (uint32_t y = miny; y <= maxy; y++) {
        uint32_t off = y * stride;
        for (uint32_t x = minx; x <= maxx; x++) {
            fb[off + x]   = back[off + x];
            prev[off + x] = back[off + x];
        }
    }
    /* Pixels actually pushed to the slow FB = the changed bounding box area.
     * Returned so the stats overlay can show present cost per frame. */
    return (maxx - minx + 1) * (maxy - miny + 1);
}

/* Boot transition: how long the kernel splash fluidly cross-fades into the
 * desktop, in ms. */
#define BOOT_FADE_MS  900

/* Present a per-channel cross-fade of `splash` -> `back` into `fb`.
 * `t` is the blend amount in [0,256] (0 = all splash, 256 = all desktop).
 * Kept as a fallback; the boot transition uses present_circle_iris() below. */
static void present_crossfade(uint32_t *fb, uint32_t *back, uint32_t *splash,
                              uint32_t total, uint32_t t) {
    if (t > 256u) t = 256u;
    uint32_t it = 256u - t;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t a = splash[i], b = back[i];
        uint32_t r = ((((a >> 16) & 0xFFu) * it) + (((b >> 16) & 0xFFu) * t)) >> 8;
        uint32_t g = ((((a >>  8) & 0xFFu) * it) + (((b >>  8) & 0xFFu) * t)) >> 8;
        uint32_t bl= (((( a       & 0xFFu) * it) + ((  b       & 0xFFu) * t))) >> 8;
        fb[i] = (r << 16) | (g << 8) | bl;
    }
}

/* Integer sqrt (Newton) -- used once per frame for the iris radius. */
static uint32_t isqrt32(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n, y = (x + 1u) / 2u;
    while (y < x) { x = y; y = (x + n / x) / 2u; }
    return x;
}

/* Circular IRIS reveal: the desktop (`back`) opens through a growing, eased,
 * soft-edged circle centered on screen, over the captured boot splash. `t` in
 * [0,256] is transition progress; `max_radius` = center-to-corner distance so
 * the circle fully covers the screen at t=256. This is the fluid "Welcome to
 * AutomationOS" -> desktop boot transition. Writes the full frame, but only
 * during the brief BOOT_FADE_MS window (present_diff takes over after). */
static void present_circle_iris(uint32_t *fb, uint32_t *back, uint32_t *splash,
                                uint32_t W, uint32_t H, uint32_t stride,
                                uint32_t max_radius, uint32_t t) {
    if (t > 256u) t = 256u;
    /* smoothstep easing 3t^2 - 2t^3, kept in [0,256] fixed-point. */
    uint32_t eased  = (uint32_t)(((uint64_t)t * t * (768u - 2u * t)) >> 16);
    uint32_t radius = (max_radius * eased) >> 8;          /* px */
    int32_t  cx = (int32_t)W / 2, cy = (int32_t)H / 2;
    const int32_t FEATHER = 28;                           /* soft-edge band */
    int64_t inner  = (int64_t)radius - FEATHER; if (inner < 0) inner = 0;
    int64_t outer  = (int64_t)radius + FEATHER;
    int64_t inner2 = inner * inner, outer2 = outer * outer;
    int64_t band   = outer2 - inner2; if (band < 1) band = 1;
    for (uint32_t y = 0; y < H; y++) {
        int32_t  dy  = (int32_t)y - cy;
        int64_t  dy2 = (int64_t)dy * dy;
        uint32_t off = y * stride;
        for (uint32_t x = 0; x < W; x++) {
            int32_t dx = (int32_t)x - cx;
            int64_t dist2 = (int64_t)dx * dx + dy2;
            uint32_t px;
            if (dist2 <= inner2) {
                px = back[off + x];                        /* fully revealed */
            } else if (dist2 >= outer2) {
                px = splash[off + x];                      /* still splash    */
            } else {
                uint32_t bl = (uint32_t)(((dist2 - inner2) * 256) / band); /* back->splash */
                uint32_t a = back[off + x], b = splash[off + x];
                uint32_t it = 256u - bl;
                uint32_t r = ((((a >> 16) & 0xFFu) * it) + (((b >> 16) & 0xFFu) * bl)) >> 8;
                uint32_t g = ((((a >>  8) & 0xFFu) * it) + (((b >>  8) & 0xFFu) * bl)) >> 8;
                uint32_t c = (((  a        & 0xFFu) * it) + ((  b        & 0xFFu) * bl)) >> 8;
                px = (r << 16) | (g << 8) | c;
            }
            fb[off + x] = px;
        }
    }
}

/* ====================================================================== *
 *  Client requests                                                        *
 * ====================================================================== */

/* Clamp a window frame so its titlebar stays inside the chrome-free region. */
static void clamp_window(window_t *win) {
    int32_t min_y = PANEL_H + 4;
    int32_t max_y = (int32_t)g_fb_h - DOCK_H - TITLEBAR_H - 8;
    /* M8: right edge is screen_w - RDOCK_W so windows don't slide under dock */
    int32_t max_x = (int32_t)g_fb_w - RDOCK_W - (int32_t)win->w - 4;
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

    /* Validate client-supplied geometry BEFORE trusting it for SHM blits. The
     * client's pixel buffer is w*h*4 bytes; a bogus (zero or larger-than-screen)
     * w/h or an inconsistent stride would make every frame's blit read far past
     * the end of the attached segment (OOB read of compositor memory). A window
     * bigger than the display is invalid regardless. */
    if (req->w == 0 || req->h == 0 || req->w > g_fb_w || req->h > g_fb_h) {
        print("[COMP] rejecting create: bad geometry w="); print_num((long)req->w);
        print(" h="); print_num((long)req->h); print(" pid="); print_num(req->pid);
        print("\n");
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
    /* Force stride to the validated width (in pixels). All clients allocate a
     * tightly-packed w*h*4 buffer, so stride == w. Deriving it from the
     * client-supplied (and previously unvalidated) req->stride was an OOB-read
     * vector; pin it to w instead. */
    win->stride     = req->w;
    /* Record the IMMUTABLE SHM extent. snap/maximize rewrite win->w/win->h to
     * the maximized drawable rect, but the client's SHM stays this size (wl-lite
     * has no resize event), so every blit clamps its SOURCE read to buf_w/buf_h
     * to avoid reading past the mapped segment. NEVER overwrite these. */
    win->buf_w      = req->w;
    win->buf_h      = req->h;
    win->dirty      = 1;
    win->phase      = PH_NONE;
    win->minimized  = 0;
    win->tb_idx     = -1;
    win->snap_state = SNAP_NONE;                    /* M6: not snapped yet      */
    /* M8: start fade-in from fully transparent. */
    win->fade_alpha    = 0;
    win->fade_start_ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
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
    mru_promote(slot);             /* M6: front of the MRU ring             */

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
    mru_remove(slot);                              /* M6: drop from MRU ring   */
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
        /* PERF: ANY client request is a visible change -- a new window (CREATE),
         * a fresh surface frame the client drew (COMMIT), or a teardown
         * (DESTROY). Mark dirty unconditionally so the gate recomposites. This
         * is the choke-point for "a client drew a new frame". */
        mark_dirty();
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
/* Per-event click latches. A quick click delivers press+release in the SAME
 * pump_input batch, so the once-per-frame g_buttons edge check misses it. We
 * latch the rising edge as it happens (capturing the cursor position) and
 * consume it in handle_mouse, so every click registers regardless of batching. */
static int32_t g_click_latch  = 0, g_click_cx  = 0, g_click_cy  = 0;  /* left  */
static int32_t g_rclick_latch = 0, g_rclick_cx = 0, g_rclick_cy = 0;  /* right */

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

/* ---------------------------------------------------------------------- *
 *  M6: keyboard shortcut + Alt+Tab interception.                          *
 *                                                                          *
 *  wm_handle_key() is called for every EV_KEY BEFORE forwarding. It tracks *
 *  Left-Alt state and consumes the WM shortcuts so they never reach the    *
 *  client. Returns 1 if the key was consumed (do NOT forward), else 0.     *
 * ---------------------------------------------------------------------- */
static int     g_alt_held    = 0;      /* Left-Alt currently down?          */
static int     g_alttab_live = 0;      /* an Alt+Tab cycle is in progress    */
static int32_t g_alttab_pos  = 0;      /* index into the MRU ring this cycle */

/* fwd decls for WM actions defined later in the file */
static void focus_window(int slot);
static void begin_minimize(int slot);

/* Raise + focus a slot WITHOUT reordering the MRU ring (so repeated Alt+Tab
 * presses keep cycling predictably; the MRU is committed when Alt is released). */
static void alttab_raise(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    z_push_front(slot);                            /* raise visually + focus   */
    print("[SHELL] alt-tab focus win "); print_num(g_windows[slot].win_id); print("\n");
}

/* Pick the next focusable (used, not minimized, not closing) slot when Alt+Tab
 * advances. We walk the MRU ring starting after the current position. */
static void alttab_advance(void) {
    if (g_mru_count == 0) return;
    /* On the first Tab of a cycle, start from the 2nd MRU entry so one tap
     * swaps to the previously-focused window (classic Alt+Tab behavior). */
    int start = g_alttab_live ? (g_alttab_pos + 1) : 1;
    for (int n = 0; n < g_mru_count; n++) {
        int idx  = (start + n) % g_mru_count;
        int slot = (int)g_mru[idx];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        if (g_windows[slot].minimized) continue;
        if (g_windows[slot].phase == PH_CLOSING || g_windows[slot].phase == PH_MINIMIZING)
            continue;
        g_alttab_pos  = idx;
        g_alttab_live = 1;
        alttab_raise(slot);
        return;
    }
}

/* Returns 1 if the key was consumed by the WM (do not forward to client). */
static int wm_handle_key(int32_t keycode, int32_t pressed) {
    /* Track the Alt modifier. */
    if (keycode == KEY_LEFTALT) {
        g_alt_held = pressed ? 1 : 0;
        if (!pressed && g_alttab_live) {
            /* Alt released: commit the focused window to the front of the MRU. */
            int f = focused_slot();
            if (f >= 0) mru_promote(f);
            g_alttab_live = 0;
        }
        return 1;                                  /* never forward the Alt key */
    }

    if (!g_alt_held) return 0;                     /* no modifier -> forward    */

    /* Alt is held: intercept the WM chords on key DOWN only. */
    if (pressed) {
        if (keycode == KEY_TAB) {
            alttab_advance();
            return 1;
        }
        if (keycode == KEY_Q || keycode == KEY_F4) {
            int f = focused_slot();
            if (f >= 0) { int32_t id = g_windows[f].win_id; begin_close(f);
                          print("[SHELL] shortcut close win "); print_num(id); print("\n"); }
            return 1;
        }
        if (keycode == KEY_M) {
            int f = focused_slot();
            if (f >= 0) begin_minimize(f);
            return 1;
        }
        if (keycode == KEY_S) {
            /* PERF: toggle the on-screen stats overlay (FPS/frame-time/windows/
             * pixels). Mark dirty so it appears/disappears on the next frame. */
            g_stats_on = !g_stats_on;
            mark_dirty();
            print("[SHELL] stats overlay ");
            print(g_stats_on ? "ON\n" : "OFF\n");
            return 1;
        }
        if (keycode == KEY_K) {
            /* FORCE QUIT: SIGKILL the focused window's client process, even if
             * it is hung and not draining its event queue. The per-frame liveness
             * sweep (reap_dead_windows) removes the window once the pid is gone;
             * we also start the close animation now for instant feedback. */
            int f = focused_slot();
            if (f >= 0) {
                int32_t pid = g_windows[f].client_pid;
                int32_t id  = g_windows[f].win_id;
                long rc = syscall(SYS_KILL, pid, SIGKILL, 0);
                print("[SHELL] FORCE QUIT win "); print_num(id);
                print(" pid "); print_num(pid);
                print(" rc "); print_num(rc); print("\n");
                begin_close(f);
            }
            return 1;
        }
    }
    /* Consume the key-UP of an intercepted chord too, so the client never sees
     * a dangling release for a press it never got. (Tab/Q/F4/M/K/S while Alt held.) */
    if (keycode == KEY_TAB || keycode == KEY_Q || keycode == KEY_F4 ||
        keycode == KEY_M   || keycode == KEY_K || keycode == KEY_S)
        return 1;

    return 0;                                       /* other Alt+<key>: forward  */
}

/* M6 liveness sweep: a client may exit normally OR be force-killed (Alt+K or
 * the Task Manager) without ever sending WL_REQ_DESTROY. Probe each window's
 * owner pid with kill(pid, 0) (signal 0 = existence check); if it returns
 * ESRCH the process is gone, so animate the orphaned window away. Throttled by
 * the caller to a few times per second. */
static void reap_dead_windows(void) {
    for (int s = 0; s < MAX_WINDOWS; s++) {
        if (!g_windows[s].used) continue;
        if (g_windows[s].phase == PH_CLOSING) continue;   /* already leaving */
        int32_t pid = g_windows[s].client_pid;
        if (pid <= 0) continue;
        long rc = syscall(SYS_KILL, pid, 0, 0);
        /* kill(pid,0) returns 0 for a live process. Any negative return means
         * the process is gone/unsignalable (this single-user kernel has no
         * signal permission check, so the only error is ESRCH). Treat any
         * error as "dead" rather than hardcoding a specific errno value. */
        if (rc < 0) {
            print("[SHELL] reaping dead win "); print_num(g_windows[s].win_id);
            print(" (pid "); print_num(pid); print(" gone)\n");
            begin_close(s);
        }
    }
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
            if (e->type == EV_KEY) {
                int32_t code    = (int32_t)e->code;
                int32_t pressed = e->value != 0 ? 1 : 0;
                /* PERF: any key event may change the display -- a WM chord
                 * (Alt+Tab raise, Alt+Q close, Alt+M minimize) or a key the
                 * focused client will redraw in response to (e.g. typing in an
                 * editor/terminal). Mark dirty for every key event; biasing
                 * toward an extra recomposite is cheaper than a stale screen. */
                mark_dirty();
                /* M6: WM intercepts Alt / Alt+Tab / Alt+Q / Alt+F4 / Alt+M
                 * BEFORE the client ever sees them. Everything else forwards. */
                if (!wm_handle_key(code, pressed))
                    send_key_to_focus(code, pressed);
            }
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
                if (e->value) {
                    /* Latch the rising edge here so a press+release in one batch
                     * still registers as a click (the frame-level edge would be
                     * lost). Capture the cursor position at press time. */
                    if (bit == 0 && !(g_buttons & 1)) {
                        g_click_latch = 1; g_click_cx = g_cursor_x; g_click_cy = g_cursor_y;
                    } else if (bit == 1 && !(g_buttons & 2)) {
                        g_rclick_latch = 1; g_rclick_cx = g_cursor_x; g_rclick_cy = g_cursor_y;
                    }
                    g_buttons |= (1 << bit);
                } else {
                    g_buttons &= ~(1 << bit);
                }
                pointer_changed = 1;
            }
        }
    }

    if (!keyboard && pointer_changed) {
        if (g_cursor_x < 0) g_cursor_x = 0;
        if (g_cursor_y < 0) g_cursor_y = 0;
        if (g_cursor_x >= (int32_t)W) g_cursor_x = (int32_t)W - 1;
        if (g_cursor_y >= (int32_t)H) g_cursor_y = (int32_t)H - 1;
        /* PERF: the compositor draws the cursor, so ANY cursor move or button
         * change must repaint (cursor sprite, dock hover-magnify, drag, menu
         * highlight all key off pointer state). Mark dirty for every pointer
         * change -- the safest, broadest mouse damage source. */
        mark_dirty();
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

/* M6: which snap zone (if any) does the cursor currently arm? Top edge wins
 * (maximize), then left/right. Returns SNAP_NONE if not near any edge. */
static int32_t snap_zone_for_cursor(int32_t cx, int32_t cy, uint32_t W, uint32_t H) {
    (void)H;
    if (cy <= PANEL_H + SNAP_EDGE_PX)               return SNAP_MAX;
    if (cx <= SNAP_EDGE_PX)                          return SNAP_LEFT;
    /* M8: right snap arm fires at the work-area right edge (before the dock) */
    if (cx >= (int32_t)W - RDOCK_W - SNAP_EDGE_PX)  return SNAP_RIGHT;
    return SNAP_NONE;
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
    mru_promote(slot);                             /* M6: keep MRU ring current */
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

/* ---------------------------------------------------------------------- *
 *  M6: window snapping. begin_snap_to() animates a window into a snap zone *
 *  (left/right half or maximized); begin_unsnap() animates it back to the  *
 *  geometry that was saved when it first snapped.                          *
 * ---------------------------------------------------------------------- */

/* Kick off the PH_SNAPPING geometry tween from the window's current frame to
 * (to_x,to_y) + client (to_w,to_h). */
static void start_geom_tween(window_t *win, int32_t to_x, int32_t to_y,
                             uint32_t to_w, uint32_t to_h) {
    win->from_x = win->x;  win->from_y = win->y;
    win->from_w = win->w;  win->from_h = win->h;
    win->to_x   = to_x;    win->to_y   = to_y;
    win->to_w   = to_w;    win->to_h   = to_h;
    anim_begin(win, PH_SNAPPING, ANIM_SNAP_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
}

/* Snap a window to the given zone. Saves the pre-snap geometry the FIRST time
 * (so repeated snaps don't overwrite the real restore target). */
static void begin_snap_to(int slot, int32_t kind) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->phase == PH_CLOSING || win->phase == PH_MINIMIZING) return;
    int32_t fx, fy; uint32_t cw, ch;
    if (snap_target_rect(kind, &fx, &fy, &cw, &ch) != 0) return;
    /* Aspect/black-margin fix: never grow a window's frame past the app's OWN
     * surface (buf_w x buf_h). A fixed-size client (e.g. the 640x400 terminal)
     * can't fill a bigger frame, so the uncovered area letterboxes to the
     * wallpaper -- the "black margin on the right". Clamp the snap/maximize target
     * to the buffer so the window MATCHES the app. (Real grow-to-fill needs a
     * resize/configure protocol event + per-app reflow -- a separate, larger job.) */
    if (cw > win->buf_w) cw = win->buf_w;
    if (ch > win->buf_h) ch = win->buf_h;
    if (win->snap_state == SNAP_NONE) {            /* remember where we came from */
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    win->snap_state = kind;
    start_geom_tween(win, fx, fy, cw, ch);
    print("[SHELL] snap win "); print_num(win->win_id);
    print(" kind="); print_num(kind); print("\n");
}

/*
 * Resolve all left-mouse interaction. Called once per frame AFTER pump_input
 * has updated g_cursor_x/y + g_buttons. Uses edge detection (prev vs current
 * left-button bit) so a click fires exactly once on press.
 */
/* ---------------------------- popup-menu logic ------------------------- */
static void menu_add(const char *label, const char *path, int action) {
    if (g_menu_n >= MENU_MAX) return;
    g_menu_label[g_menu_n]  = label;
    g_menu_path[g_menu_n]   = path;
    g_menu_action[g_menu_n] = action;
    g_menu_n++;
}
static void open_start_menu(uint32_t W, uint32_t H) {
    (void)W;
    g_menu_n = 0; g_menu_is_ctx = 0;
    for (int i = 0; i < RDOCK_NICONS; i++)
        menu_add(rdock_apps[i].path + 5, rdock_apps[i].path, MACT_NONE);  /* skip "sbin/" */
    int32_t mh = menu_height();
    g_menu_x = launcher_x();
    g_menu_y = dock_top(H) - mh - 6;
    if (g_menu_y < PANEL_H + 4) g_menu_y = PANEL_H + 4;
    g_menu_open = 1;
}
/* Open the right-click context menu. `target_slot` is the window under the
 * cursor (so the menu acts on THAT window), or -1 for the bare desktop. The
 * item set is chosen per target so the menu is appropriate to what was clicked. */
static void open_context_menu(int32_t cx, int32_t cy, int target_slot,
                              uint32_t W, uint32_t H) {
    g_menu_n = 0; g_menu_is_ctx = 1;
    g_menu_target_slot = target_slot;
    if (target_slot >= 0 && target_slot < MAX_WINDOWS && g_windows[target_slot].used) {
        /* window context: actions on that specific window (same code paths as
         * the title-bar minimize/maximize/close buttons). */
        menu_add("Minimize", (const char *)0, MACT_WIN_MINIMIZE);
        if (g_windows[target_slot].snap_state == SNAP_MAX)
            menu_add("Restore",  (const char *)0, MACT_WIN_MAXIMIZE);
        else
            menu_add("Maximize", (const char *)0, MACT_WIN_MAXIMIZE);
        menu_add("Snap Left",  (const char *)0, MACT_WIN_SNAP_LEFT);
        menu_add("Snap Right", (const char *)0, MACT_WIN_SNAP_RIGHT);
        menu_add("Close",    (const char *)0, MACT_WIN_CLOSE);
    } else {
        /* desktop context: actions on the workspace. */
        menu_add("New Folder",       (const char *)0, MACT_NEW_FOLDER);
        menu_add("Display Settings", (const char *)0, MACT_DISPLAY_SETTINGS);
        menu_add("Refresh",          (const char *)0, MACT_REFRESH);
    }
    int32_t mh = menu_height();
    g_menu_x = cx; g_menu_y = cy;
    if (g_menu_x + MENU_W > (int32_t)W - RDOCK_W) g_menu_x = (int32_t)W - RDOCK_W - MENU_W - 4;
    if (g_menu_x < 4) g_menu_x = 4;
    if (g_menu_y + mh > (int32_t)H - DOCK_H) g_menu_y = (int32_t)H - DOCK_H - mh - 4;
    if (g_menu_y < PANEL_H + 4) g_menu_y = PANEL_H + 4;
    g_menu_open = 1;
}
/* Append the NUL-terminated `src` to `dst` (which already holds `*plen` chars,
 * room for `cap` total incl NUL). Updates *plen. Bounded. */
static void desk_strcat(char *dst, int *plen, int cap, const char *src) {
    int n = *plen;
    for (int i = 0; src[i] && n < cap - 1; i++) dst[n++] = src[i];
    dst[n] = '\0';
    *plen = n;
}
/* Append a small positive integer as decimal text. */
static void desk_cat_num(char *dst, int *plen, int cap, int v) {
    char tmp[12]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0) { int n = *plen; if (n < cap - 1) { dst[n++] = tmp[--t]; dst[n] = '\0'; *plen = n; } else break; }
}

/* Create "/Desktop/NewFolder" (or NewFolder2, NewFolder3, ... if taken) via
 * SYS_MKDIR, then rescan so the new tile appears. Existence is probed with
 * SYS_OPENDIR (succeeds only on an existing dir) so we pick a fresh name. */
static void desk_new_folder(void) {
    char path[64];
    for (int n = 1; n <= 99; n++) {
        int len = 0;
        path[0] = '\0';
        desk_strcat(path, &len, (int)sizeof(path), "/Desktop/NewFolder");
        if (n > 1) desk_cat_num(path, &len, (int)sizeof(path), n);

        /* does it already exist? opendir returns >=0 only for an existing dir */
        long probe = syscall(SYS_OPENDIR, (long)path, 0, 0);
        if (probe >= 0) { syscall(SYS_CLOSEDIR, probe, 0, 0); continue; }

        long r = syscall(SYS_MKDIR, (long)path, 0755, 0);
        print("[SHELL] new folder "); print(path);
        if (r < 0) { print(" (failed r="); print_num(r); print(")"); }
        print("\n");
        break;
    }
    desk_scan();   /* refresh the desktop icon list */
}

static void menu_run(int idx) {
    if (idx < 0 || idx >= g_menu_n) return;
    if (g_menu_path[idx]) {
        long r = syscall(SYS_SPAWN, (long)g_menu_path[idx], 0, 0);
        print("[SHELL] menu launch "); print(g_menu_path[idx]);
        if (r < 0) print(" (failed)");
        print("\n");
    } else if (g_menu_action[idx] == MACT_CLOSE_ALL) {
        for (int s = 0; s < MAX_WINDOWS; s++)
            if (g_windows[s].used && g_windows[s].phase != PH_CLOSING) begin_close(s);
    } else if (g_menu_action[idx] == MACT_MINIMIZE_ALL) {
        for (int s = 0; s < MAX_WINDOWS; s++)
            if (g_windows[s].used && !g_windows[s].minimized &&
                g_windows[s].phase != PH_CLOSING) begin_minimize(s);
    } else if (g_menu_action[idx] == MACT_ABOUT) {
        g_about_open = 1;   /* show the modal About dialog (not a toast) */
    } else if (g_menu_action[idx] == MACT_NEW_FOLDER) {
        desk_new_folder();
    } else if (g_menu_action[idx] == MACT_DISPLAY_SETTINGS) {
        syscall(SYS_SPAWN, (long)"sbin/settings", 0, 0);
    } else if (g_menu_action[idx] == MACT_REFRESH) {
        desk_scan();        /* re-read desktop icons; scene repaints every frame */
    } else if (g_menu_action[idx] == MACT_WIN_MINIMIZE ||
               g_menu_action[idx] == MACT_WIN_MAXIMIZE ||
               g_menu_action[idx] == MACT_WIN_SNAP_LEFT ||
               g_menu_action[idx] == MACT_WIN_SNAP_RIGHT ||
               g_menu_action[idx] == MACT_WIN_CLOSE) {
        /* Act on the window the menu was opened over, mirroring the title-bar
         * button handlers exactly. Re-validate the slot in case the window was
         * closed/reaped while the menu was open. */
        int slot = g_menu_target_slot;
        if (slot >= 0 && slot < MAX_WINDOWS && g_windows[slot].used &&
            g_windows[slot].phase != PH_CLOSING &&
            g_windows[slot].phase != PH_MINIMIZING) {
            window_t *win = &g_windows[slot];
            if (g_menu_action[idx] == MACT_WIN_CLOSE) {
                int32_t id = win->win_id;
                begin_close(slot);
                print("[SHELL] close win "); print_num(id); print("\n");
            } else if (g_menu_action[idx] == MACT_WIN_MINIMIZE) {
                begin_minimize(slot);
            } else if (g_menu_action[idx] == MACT_WIN_SNAP_LEFT) {
                begin_snap_to(slot, SNAP_LEFT);
            } else if (g_menu_action[idx] == MACT_WIN_SNAP_RIGHT) {
                begin_snap_to(slot, SNAP_RIGHT);
            } else { /* MACT_WIN_MAXIMIZE: toggle SNAP_MAX (same as title button) */
                if (win->snap_state == SNAP_MAX) {
                    start_geom_tween(win, win->saved_x, win->saved_y,
                                     win->saved_w, win->saved_h);
                    win->snap_state = SNAP_NONE;
                } else {
                    begin_snap_to(slot, SNAP_MAX);
                }
            }
        }
    }
}
/* Returns 1 if the click was consumed (selecting a row, or dismissing). */
static int menu_handle_click(int32_t cx, int32_t cy) {
    if (!g_menu_open) return 0;
    int32_t mh = menu_height();
    if (point_in(cx, cy, g_menu_x, g_menu_y, MENU_W, mh)) {
        int32_t ry = cy - (g_menu_y + MENU_HDR_H);
        if (ry >= 0) {
            int row = ry / MENU_ROW_H;
            if (row >= 0 && row < g_menu_n) menu_run(row);
        }
    }
    g_menu_open = 0;   /* any click dismisses the menu */
    return 1;
}

/* Hit-test the desktop-icon grid at (cx,cy). Returns the icon index, or -1. */
static int desk_icon_at(int32_t cx, int32_t cy, uint32_t W, uint32_t H) {
    for (int i = 0; i < g_desk_count; i++) {
        int32_t tx, ty;
        if (!desk_icon_origin(i, W, H, &tx, &ty)) continue;
        /* hit area = the tile plus its label row (matches the hover rect) */
        if (point_in(cx, cy, tx - 6, ty - 2,
                     DESK_TILE + 12, DESK_TILE + DESK_LABEL_GAP + FONT_H + 4))
            return i;
    }
    return -1;
}

/* Handle a left click on a desktop icon: single click selects (arms double),
 * a second click on the SAME icon within DESK_DBLCLICK_MS launches it:
 *   directory  -> spawn sbin/filemanager
 *   regular fl -> SYS_SPAWN "/Desktop/<name>" (runs IDE-compiled programs)
 * Returns 1 if a desktop icon was clicked (consume), else 0. */
static int desk_handle_click(int32_t cx, int32_t cy, uint32_t W, uint32_t H, long now) {
    int idx = desk_icon_at(cx, cy, W, H);
    if (idx < 0) return 0;

    int is_double = (idx == g_desk_last_idx &&
                     (now - g_desk_last_ms) <= DESK_DBLCLICK_MS &&
                     (now - g_desk_last_ms) >= 0);
    g_desk_last_idx = idx;
    g_desk_last_ms  = now;
    if (!is_double) return 1;        /* first click: select only */

    g_desk_last_idx = -1;            /* reset so a 3rd click starts fresh */
    desk_icon_t *di = &g_desk_icons[idx];

    if (di->is_dir) {
        long r = syscall(SYS_SPAWN, (long)"sbin/filemanager", 0, 0);
        print("[SHELL] desktop open dir "); print(di->name);
        if (r < 0) { print(" (spawn fail r="); print_num(r); print(")"); }
        print("\n");
    } else {
        char path[64];
        int len = 0; path[0] = '\0';
        desk_strcat(path, &len, (int)sizeof(path), "/Desktop/");
        desk_strcat(path, &len, (int)sizeof(path), di->name);
        long r = syscall(SYS_SPAWN, (long)path, 0, 0);
        print("[SHELL] desktop run "); print(path);
        if (r < 0) { print(" (spawn fail r="); print_num(r); print(")"); }
        print("\n");
    }
    return 1;
}

static void handle_mouse(uint32_t W, uint32_t H) {
    int32_t left      = g_buttons & 1;
    int32_t prev_left = g_prev_buttons & 1;
    int32_t release   = !left && prev_left;     /* falling edge = end drag  */
    int32_t cx = g_cursor_x, cy = g_cursor_y;   /* LIVE cursor (drag track) */

    /* PERF: handle_mouse has ~20 state-changing exit paths (focus/raise, drag-
     * move, snap, minimize, maximize, close, menu open/close, taskbar + dock +
     * icon clicks, app launch). Rather than instrument each one, mark dirty once
     * here whenever there is ANY interaction in flight this frame:
     *   - a pending left-click latch (g_click_latch)  -> a click will dispatch
     *   - a pending right-click latch (g_rclick_latch) -> a context menu may open
     *   - an active drag (g_drag_slot >= 0)            -> the window is moving
     *   - an open popup menu (g_menu_open)             -> hover row may change
     *   - a held/just-released button                  -> drag end / re-focus
     * This deliberately over-marks (a no-op click still repaints one frame),
     * honoring "bias hard toward dirty". Cursor-move-only frames are already
     * marked in pump_input. */
    if (g_click_latch || g_rclick_latch || g_drag_slot >= 0 ||
        g_menu_open  || g_about_open    || left || prev_left)
        mark_dirty();

    /* Track which menu row the cursor is over (for highlight), every frame. */
    if (g_menu_open) {
        g_menu_hover = -1;
        if (point_in(g_cursor_x, g_cursor_y, g_menu_x, g_menu_y + MENU_HDR_H,
                     MENU_W, g_menu_n * MENU_ROW_H))
            g_menu_hover = (g_cursor_y - (g_menu_y + MENU_HDR_H)) / MENU_ROW_H;
    }

    /* Right-click opens a CONTEXT-AWARE menu. If a live window is under the
     * cursor, the menu acts on THAT window (minimize/maximize/close). Otherwise,
     * on the bare desktop (not over the dock and not over a desktop icon), it
     * offers desktop actions. Right-clicks on the dock/icons fall through. */
    if (g_rclick_latch) {
        g_rclick_latch = 0;
        int in_region = (g_rclick_cx < (int32_t)W - RDOCK_W &&
                         g_rclick_cy > PANEL_H && g_rclick_cy < (int32_t)H - DOCK_H);
        int on_icon = (desk_icon_at(g_rclick_cx, g_rclick_cy, W, H) >= 0);
        int win_slot = -1;   /* topmost live window under the cursor, or -1 */
        for (int32_t i = g_zcount - 1; i >= 0 && win_slot < 0; i--) {
            int slot = (int)g_zorder[i];
            if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
            window_t *win = &g_windows[slot];
            if (win->minimized) continue;
            if (win->phase == PH_CLOSING || win->phase == PH_MINIMIZING) continue;
            if (point_in(g_rclick_cx, g_rclick_cy, win->x, win->y,
                         (int32_t)win->w, (int32_t)win->h + TITLEBAR_H))
                win_slot = slot;
        }
        if (win_slot >= 0) {
            /* over a window: menu acts on that window (raise it first to match
             * the title-bar buttons, which operate on the focused window). */
            focus_window(win_slot);
            open_context_menu(g_rclick_cx, g_rclick_cy, win_slot, W, H);
            g_prev_buttons = g_buttons;
            return;
        }
        if (in_region && !on_icon) {
            open_context_menu(g_rclick_cx, g_rclick_cy, -1, W, H);
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* ---- active drag-move: track cursor while left held ---- */
    if (g_drag_slot >= 0) {
        window_t *dw = &g_windows[g_drag_slot];
        /* The dragged window may have been closed/force-quit (Alt+K) or reaped
         * mid-drag. If it is no longer a live, non-closing window, abandon the
         * drag so we don't ghost-move a dead slot (or a reused one). */
        if (!dw->used || dw->phase == PH_CLOSING || dw->phase == PH_MINIMIZING) {
            g_drag_slot  = -1;
            g_snap_armed = SNAP_NONE;
            g_prev_buttons = g_buttons;
            return;
        }
        if (left && dw->used) {
            /* M6: dragging a snapped window away un-snaps it back to its saved
             * floating SIZE immediately (no tween mid-drag) and re-anchors the
             * grab so the cursor stays on the titlebar as it follows the mouse. */
            if (dw->snap_state != SNAP_NONE) {
                dw->w = dw->saved_w;               /* restore floating size     */
                dw->h = dw->saved_h;
                dw->snap_state = SNAP_NONE;
                if (dw->phase == PH_SNAPPING) dw->phase = PH_NONE;
                /* re-anchor: keep grab proportional but inside the titlebar */
                g_drag_dx = (int32_t)dw->w / 2;
                g_drag_dy = TITLEBAR_H / 2;
            }
            dw->x = cx - g_drag_dx;
            dw->y = cy - g_drag_dy;
            clamp_window(dw);
            /* M6: arm a snap preview if the cursor reached a screen edge */
            g_snap_armed = snap_zone_for_cursor(cx, cy, W, H);
        }
        if (release) {
            if (g_snap_armed != SNAP_NONE && dw->used) {
                begin_snap_to(g_drag_slot, g_snap_armed);
            } else {
                print("[SHELL] move win ");
                print_num(dw->used ? dw->win_id : -1);
                print("\n");
            }
            g_snap_armed = SNAP_NONE;
            g_drag_slot  = -1;
        }
        g_prev_buttons = g_buttons;
        return;                                  /* drag owns the pointer   */
    }

    /* Click dispatch is driven by the per-event latch (set in pump_input), not a
     * frame-level button edge — a quick click whose press+release arrive in one
     * input batch would otherwise be lost. Hit-test at the latched press pos. */
    if (!g_click_latch) { g_prev_buttons = g_buttons; return; }
    g_click_latch = 0;
    cx = g_click_cx; cy = g_click_cy;

    /* ---------- click hit-testing (priority order) ---------- */

    /* modal About dialog is topmost: any click dismisses it and is consumed. */
    if (g_about_open) { g_about_open = 0; g_prev_buttons = g_buttons; return; }

    /* 0a) an open popup menu owns the next click (select a row or dismiss). */
    if (menu_handle_click(cx, cy)) { g_prev_buttons = g_buttons; return; }

    /* 0) M8: right dock -- highest priority for clicks in its x-range OR in
     *    an open popover that extends into the workspace. */
    {
        long now_click = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        /* rdock_handle_click also handles "click outside popover" collapse,
         * so we call it for every click; it returns 1 only if consumed. */
        if (rdock_handle_click(cx, cy, W, now_click)) {
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* 1) dock launcher button */
    {
        int32_t lx = launcher_x(), ly = launcher_y(H);
        if (point_in(cx, cy, lx, ly, LAUNCH_SZ, LAUNCH_SZ)) {
            /* Launch the Windows-11-style start-menu app (replaces the old
             * built-in popup). It renders as its own window and closes itself
             * after launching an app. (void) the old helper to avoid -Wunused. */
            (void)open_start_menu;
            syscall(SYS_SPAWN, (long)"sbin/startmenu", 0, 0);
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

        /* 3a3) titlebar maximize/restore box (left of minimize) -> toggle SNAP_MAX */
        int32_t max_x = min_x - MIN_SZ - 6;
        int32_t max_y = fy + (TITLEBAR_H - MIN_SZ) / 2;
        if (point_in(cx, cy, max_x, max_y, MIN_SZ, MIN_SZ)) {
            if (win->snap_state == SNAP_MAX) {
                /* restore: tween back to the saved pre-maximize geometry */
                start_geom_tween(win, win->saved_x, win->saved_y,
                                 win->saved_w, win->saved_h);
                win->snap_state = SNAP_NONE;
            } else {
                begin_snap_to(slot, SNAP_MAX);   /* maximize to the work area */
            }
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3b) titlebar (not a button) -> focus + begin drag-move */
        if (point_in(cx, cy, fx, fy, cw, TITLEBAR_H)) {
            focus_window(slot);
            if (left) {   /* button still held → begin drag; quick click = focus only */
                g_drag_slot = slot;
                g_drag_dx = cx - win->x;
                g_drag_dy = cy - win->y;
            }
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3c) window body -> focus + raise (let the client handle the click) */
        focus_window(slot);
        send_pointer_to_focus();
        g_prev_buttons = g_buttons;
        return;
    }

    /* 3.5) desktop icons on the bare wallpaper (no window was hit above).
     *      Double-click launches (dir -> filemanager, file -> /Desktop/name). */
    {
        long now_dc = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        if (desk_handle_click(cx, cy, W, H, now_dc)) {
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* 4) clicked empty desktop / chrome with nothing actionable: no-op */
    g_prev_buttons = g_buttons;
}

/* ====================================================================== */
void _start(void) {
    print("[SHELL] M8 right-dock: 12 icons, 2 folders\n");

    for (int i = 0; i < MAX_WINDOWS; i++) { g_windows[i].used = 0; g_windows[i].phase = PH_NONE; }
    g_zcount = 0;
    g_mru_count = 0;                                /* M6: empty MRU ring        */

    /* M8: initialise right-dock state */
    for (int i = 0; i < RDOCK_TOTAL; i++) {
        g_rdock_icons[i].scale_q8      = 256;
        g_rdock_icons[i].scale_target  = 256;
        g_rdock_icons[i].bounce_active = 0;
        g_rdock_icons[i].bounce_start  = 0;
    }
    for (int fi = 0; fi < RDOCK_NFOLDERS; fi++) {
        g_rdock_folders[fi].open         = 0;
        g_rdock_folders[fi].anim_t       = 0;
        g_rdock_folders[fi].anim_start   = 0;
        g_rdock_folders[fi].anim_closing = 0;
    }
    g_rdock_hovered     = -1;
    g_rdock_open_folder = -1;

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

    /* Fluid boot transition: snapshot the kernel's "Welcome to AutomationOS"
     * splash (still on the hardware framebuffer right now) so the first ~700ms
     * of the desktop can cross-fade in over it instead of popping abruptly.
     * Only possible when double-buffered (back != hw). */
    uint32_t *splash = (uint32_t *)0;
    if (back != hw) {
        long sbp = syscall(SYS_MMAP, 0, (long)bb_bytes,
                           VMM_PROT_READ | VMM_PROT_WRITE);
        if (sbp > 0) {
            splash = (uint32_t *)sbp;
            uint32_t npx = stride * H;
            for (uint32_t i = 0; i < npx; i++) splash[i] = hw[i];
            print("[SHELL] boot splash captured for cross-fade\n");
        }
    }

    /* Previous-frame buffer for dirty-rectangle present (perf). present_diff()
     * writes only the changed bounding box to the slow framebuffer each frame.
     * Zero-filled by mmap, so the first present_diff sees an all-different frame
     * and full-copies once, then tracks deltas. Only used when double-buffered. */
    uint32_t *prev = (uint32_t *)0;
    if (back != hw) {
        long pvp = syscall(SYS_MMAP, 0, (long)bb_bytes,
                           VMM_PROT_READ | VMM_PROT_WRITE);
        if (pvp > 0) {
            prev = (uint32_t *)pvp;
            print("[SHELL] dirty-rect present enabled\n");
        }
    }

    /* Center-to-corner distance: the radius the circular boot iris grows to so
     * it fully covers the screen by the end of the transition. */
    uint32_t _half_w = W / 2, _half_h = H / 2;
    uint32_t max_radius = isqrt32(_half_w * _half_w + _half_h * _half_h);

    long boot_ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);

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

    /* M8: initial scan of /Desktop so compiled programs show as icons */
    desk_scan();

    /* M8: welcome notification toast */
    toast_show("AutomationOS M8 — right dock ready", 3500);

    /* 5. Frame loop. */
    long t0 = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    long next = t0;
    uint64_t frame = 0;
    long last_clock_sec = -1;          /* PERF: last second the panel clock showed */
    g_last_frame_ms = t0;              /* seed frame-time measurement              */

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

        /* d2) liveness sweep (~2x/sec): drop windows whose client exited or was
         * force-quit (Alt+K / Task Manager) without a clean WL_REQ_DESTROY. */
        if ((frame % 30) == 0) reap_dead_windows();

        /* d3) periodically rescan /Desktop so newly compiled/created items
         * (e.g. IDE output, New Folder) appear without a restart. */
        if ((frame % DESK_RESCAN_FRAMES) == 0) desk_scan();

        /* d4) PERF: the panel clock shows HH:MM:SS, so it must repaint once per
         * second even on an otherwise-idle desktop. Pulse dirty when the second
         * rolls over; present_diff then pushes only the tiny clock rect. This is
         * the one perpetual "animation" the gate must never starve. */
        {
            long sec = now / 1000;
            if (sec != last_clock_sec) { last_clock_sec = sec; mark_dirty(); }
        }

        /* d5) PERF: while the boot iris cross-fade is still running it changes
         * every frame -- force dirty so the gate can't skip it. */
        int boot_fading = (back != hw && splash && (now - boot_ms) >= 0 &&
                           (now - boot_ms) < BOOT_FADE_MS);
        if (boot_fading) mark_dirty();

        /* e) composite + present -- BUT ONLY WHEN DIRTY. composite() re-blits all
         * windows (cost scales with window count), and present writes the FB, so
         * skipping both on a genuinely-unchanged frame is the main perf win for
         * "many apps open but mostly idle". Every change path sets g_dirty (see
         * mark_dirty() call sites: input, window lifecycle, client commits,
         * animations, dock hover, the per-second clock pulse above). When NOT
         * dirty we present nothing and just yield. For the first BOOT_FADE_MS we
         * fluidly cross-fade the kernel boot splash into the desktop. */
        if (g_dirty) {
            g_dirty = 0;                       /* consume before drawing so a
                                                * change DURING this frame's draw
                                                * (e.g. a late inbox msg next
                                                * iteration) re-arms cleanly.    */
            composite(back, W, H, stride, g_cursor_x, g_cursor_y, now);
            if (back != hw) {
                if (boot_fading) {
                    uint32_t t = (uint32_t)(((now - boot_ms) * 256) / BOOT_FADE_MS);
                    present_circle_iris(hw, back, splash, W, H, stride, max_radius, t);
                    g_present_px = W * H;        /* iris touches the whole screen */
                    g_present_did = 1;
                } else {
                    if (prev) { g_present_px = present_diff(hw, back, prev, W, H, stride); }
                    else      { present(hw, back, H, stride); g_present_px = W * H; }
                    g_present_did = (g_present_px != 0);
                    splash = (uint32_t *)0;      /* fade complete: stop blending */
                }
            }

            /* PERF: sample frame-time + FPS at each PRESENTED frame (idle frames
             * that skip the draw don't count, so FPS reflects real work). Smooth
             * FPS with a simple IIR so the overlay number is readable. */
            g_frame_dt_ms = now - g_last_frame_ms;
            if (g_frame_dt_ms < 0) g_frame_dt_ms = 0;
            g_last_frame_ms = now;
            if (g_frame_dt_ms > 0) {
                long inst_fps_x10 = 10000 / g_frame_dt_ms;   /* (1000/dt)*10 */
                if (g_fps_x10 == 0) g_fps_x10 = inst_fps_x10;
                else g_fps_x10 += (inst_fps_x10 - g_fps_x10) / 4;  /* IIR a=1/4 */
            }
        }

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
