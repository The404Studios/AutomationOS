/*
 * compositor_m4.c -- Milestone M4 DESKTOP SHELL (ring 3).
 *
 * Extends the M3 multi-client compositing server (compositor_m3.c) into a
 * real desktop shell:
 *
 *   - TOP PANEL (full width, 28px): left = focused window title (or
 *     "AutomationOS" when no window is focused); right = a live clock
 *     HH:MM:SS computed from SYS_GET_TICKS_MS (no RTC -- this is uptime).
 *   - BOTTOM DOCK (full width, 44px): a rounded accent LAUNCHER button "T"
 *     that SYS_SPAWNs "sbin/terminal", followed by a TASKBAR with one button
 *     per open client window (focused button highlighted). Clicking a
 *     taskbar button focuses + raises that window.
 *   - MOUSE WINDOW MANAGEMENT (driven by pump_input -> g_cursor_x/y + button
 *     state, with edge detection for a click = press transition):
 *       * dock launcher       -> SYS_SPAWN terminal
 *       * taskbar button      -> focus + raise that window
 *       * titlebar close box  -> destroy (shmdt + free the slot)
 *       * titlebar (drag)     -> grab + move the window while held
 *       * window body         -> focus + raise
 *   - New windows are placed in the chrome-free region (y in [PANEL_H,
 *     H-DOCK_H)) and staggered.
 *   - Panel + dock are drawn AFTER all windows (always-on-top chrome).
 *   - Keeps: unconditional SYS_YIELD per frame, font titlebars, zero-copy
 *     client compositing, input forwarding to the focused window (so the
 *     terminal still gets keystrokes), gradient desktop background.
 *
 * Aether Dark palette:
 *   desktop 0xFF1C1C1E, panel 0xFF2C2C2E, hover 0xFF3A3A3C,
 *   text 0xFFFFFFFF, text-dim 0xFFAEAEB2, accent 0xFF0A84FF, border 0xFF38383A.
 *
 * Serial diagnostics: "[SHELL] launch terminal", "[SHELL] focus win N",
 *   "[SHELL] close win N", "[SHELL] move win N".
 *
 * Build (EXACT -- flags DIRECT on the cmdline, never via an unquoted shell
 * var, or -fno-stack-protector is dropped and it faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/compositor/compositor_m4.c -o /tmp/cm4.o
 *   gcc <same flags> -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/cm4.o /tmp/bf.o -o /tmp/cm4.elf
 *   objdump -d /tmp/cm4.elf | grep "fs:0x28"   # MUST be empty
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

/* Desktop gradient endpoints (subtle -- around the base desktop color). */
#define DESK_TOP        0xFF242428u
#define DESK_BOT        0xFF141416u

#define TITLEBAR_FOCUS  0xFF3A3A3Cu   /* focused window titlebar             */
#define TITLEBAR_UNFOC  0xFF2C2C2Eu   /* unfocused titlebar                  */
#define BORDER_FOCUS    0xFF0A84FFu   /* focused border (accent)             */
#define BORDER_UNFOC    0xFF38383Au   /* unfocused border                    */
#define SHADOW_COLOR    0x60000000u
#define CURSOR_FILL     0xFFFFFFFFu
#define CURSOR_EDGE     0xFF000000u
#define BTN_CLOSE       0xFFFF453Au   /* close box (red)                     */
#define WIN_PLACEHOLDER 0xFF1C1C1Eu   /* shown if a client has no shm yet    */

#define TITLEBAR_H  28
#define BORDER_W    1

/* chrome geometry */
#define PANEL_H     28
#define DOCK_H      44
#define LAUNCH_SZ   36                 /* launcher button is 36x36            */
#define TASK_W      120                /* taskbar button width                */
#define TASK_H      32                 /* taskbar button height               */
#define CLOSE_SZ    14                 /* titlebar close box hit/visual size  */

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t r = ar + (br - ar) * num / den;
    uint32_t g = ag + (bg - ag) * num / den;
    uint32_t bl = ab + (bb - ab) * num / den;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
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

/* Topmost (focused) window slot, or -1 if none. */
static int focused_slot(void) {
    if (g_zcount == 0) return -1;
    return (int)g_zorder[g_zcount - 1];
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
 *  Compositing                                                            *
 * ====================================================================== */
static void render_desktop(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t c = lerp_color(DESK_TOP, DESK_BOT, y, h ? h - 1 : 1);
        uint32_t *r = buf + y * stride;
        for (uint32_t x = 0; x < w; x++) r[x] = c;
    }
}

/* Draw a single window's decorations + client surface. frame_x/frame_y is the
 * top-left of the titlebar; the client area sits below it. Window content is
 * clipped to the chrome-free band [PANEL_H, H-DOCK_H). */
static void render_window(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                          window_t *win, int focused) {
    int32_t cw = (int32_t)win->w;
    int32_t ch = (int32_t)win->h;
    int32_t fx = win->x;                       /* frame (titlebar) x       */
    int32_t fy = win->y;                       /* frame (titlebar) y       */
    int32_t client_x = fx;
    int32_t client_y = fy + TITLEBAR_H;        /* client below titlebar    */

    int32_t clip_y0 = PANEL_H;
    int32_t clip_y1 = (int32_t)h - DOCK_H;

    uint32_t tb_col  = focused ? TITLEBAR_FOCUS : TITLEBAR_UNFOC;
    uint32_t bd_col  = focused ? BORDER_FOCUS   : BORDER_UNFOC;

    /* drop shadow */
    blend_rect(buf, w, h, stride, fx + 6, fy + 6, cw, ch + TITLEBAR_H, SHADOW_COLOR);

    /* border around the whole window (titlebar + client) */
    stroke_rect(buf, w, h, stride,
                fx - BORDER_W, fy - BORDER_W,
                cw + 2 * BORDER_W, ch + TITLEBAR_H + 2 * BORDER_W, bd_col);

    /* titlebar */
    fill_rect(buf, w, h, stride, fx, fy, cw, TITLEBAR_H, tb_col);

    /* titlebar close box (right-aligned) */
    int32_t close_x = fx + cw - CLOSE_SZ - 8;
    int32_t close_y = fy + (TITLEBAR_H - CLOSE_SZ) / 2;
    fill_round_rect(buf, w, h, stride, close_x, close_y, CLOSE_SZ, CLOSE_SZ, 3, BTN_CLOSE);
    /* a small white "x" glyph centered in the close box */
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     close_x + (CLOSE_SZ - FONT_W) / 2,
                     close_y + (CLOSE_SZ - FONT_H) / 2, "x", COL_TEXT);

    /* window title text (bitmap font) */
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     fx + 8, fy + (TITLEBAR_H - FONT_H) / 2, win->title,
                     focused ? COL_TEXT : COL_TEXT_DIM);

    /* client surface: blit the client's shared-memory pixels (clipped to the
     * chrome-free band), or a placeholder fill if no pixels yet. */
    if (win->pixels) {
        blit_surface_clip(buf, w, h, stride,
                          win->pixels, win->w, win->h, win->stride,
                          client_x, client_y, clip_y0, clip_y1);
    } else {
        /* placeholder, clipped to the chrome band manually */
        int32_t py0 = client_y < clip_y0 ? clip_y0 : client_y;
        int32_t py1 = client_y + ch;
        if (py1 > clip_y1) py1 = clip_y1;
        if (py1 > py0)
            fill_rect(buf, w, h, stride, client_x, py0, cw, py1 - py0, WIN_PLACEHOLDER);
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

/* Taskbar layout: launcher at left, then one button per window left-to-right.
 * These helpers return the on-screen rectangle for hit-testing AND drawing so
 * the two never drift apart. */
static int32_t dock_top(uint32_t h)        { return (int32_t)h - DOCK_H; }
static int32_t launcher_x(void)            { return 8; }
static int32_t launcher_y(uint32_t h)      { return dock_top(h) + (DOCK_H - LAUNCH_SZ) / 2; }

/* x of the i-th taskbar button (0-based among visible windows). */
static int32_t taskbtn_x(int idx)          { return launcher_x() + LAUNCH_SZ + 12 + idx * (TASK_W + 8); }
static int32_t taskbtn_y(uint32_t h)       { return dock_top(h) + (DOCK_H - TASK_H) / 2; }

static void render_dock(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    int32_t dy = dock_top(h);
    /* dock background + 1px top border */
    fill_rect(buf, w, h, stride, 0, dy, (int32_t)w, DOCK_H, COL_PANEL);
    fill_rect(buf, w, h, stride, 0, dy, (int32_t)w, 1, COL_BORDER);

    /* launcher button: rounded accent square labeled "T" */
    int32_t lx = launcher_x(), ly = launcher_y(h);
    fill_round_rect(buf, w, h, stride, lx, ly, LAUNCH_SZ, LAUNCH_SZ, 8, COL_ACCENT);
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     lx + (LAUNCH_SZ - FONT_W) / 2, ly + (LAUNCH_SZ - FONT_H) / 2,
                     "T", COL_TEXT);

    /* taskbar: one button per window, in z-order (back to front so the
     * focused/topmost button is consistently positioned by slot order). We
     * iterate slots so positions are stable as focus changes. */
    int focused = focused_slot();
    int idx = 0;
    for (int s = 0; s < MAX_WINDOWS; s++) {
        if (!g_windows[s].used) continue;
        int32_t bx = taskbtn_x(idx);
        int32_t by = taskbtn_y(h);
        if (bx + TASK_W > (int32_t)w) break;        /* ran out of dock space */
        uint32_t bg = (s == focused) ? COL_HOVER : COL_PANEL;
        fill_round_rect(buf, w, h, stride, bx, by, TASK_W, TASK_H, 4, bg);
        stroke_rect(buf, w, h, stride, bx, by, TASK_W, TASK_H, COL_BORDER);
        /* truncated title (button is TASK_W wide -> ~14 glyphs minus padding) */
        char t[20];
        truncate_title(g_windows[s].title[0] ? g_windows[s].title : "window",
                       t, (TASK_W - 12) / FONT_W);
        font_draw_string(buf, (int)stride, (int)w, (int)h,
                         bx + 6, by + (TASK_H - FONT_H) / 2, t,
                         (s == focused) ? COL_TEXT : COL_TEXT_DIM);
        idx++;
    }
}

static void composite(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                      int32_t cursor_x, int32_t cursor_y) {
    render_desktop(buf, w, h, stride);

    /* windows, back to front */
    int top = focused_slot();
    for (int32_t i = 0; i < g_zcount; i++) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        render_window(buf, w, h, stride, &g_windows[slot], slot == top);
    }

    /* always-on-top chrome (drawn AFTER windows) */
    render_panel(buf, w, h, stride);
    render_dock(buf, w, h, stride);

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
static uint32_t g_fb_w = 0, g_fb_h = 0;   /* cached for placement clamping */

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
    win->used = 0;
}

static void handle_destroy(const wl_req_destroy_t *req) {
    int slot = slot_by_win_id(req->win_id);
    if (slot < 0) return;
    int32_t id = g_windows[slot].win_id;
    destroy_slot(slot);
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
        /* Note: we forward pointer to the focused window in the shell layer
         * (handle_mouse) so we can suppress events while dragging/over chrome.
         * Keep this here too so plain motion still reaches the client. */
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

    /* 2) taskbar buttons (focus + raise) -- iterate visible windows in slot
     *    order to match render_dock's layout. */
    {
        int idx = 0;
        for (int s = 0; s < MAX_WINDOWS; s++) {
            if (!g_windows[s].used) continue;
            int32_t bx = taskbtn_x(idx), by = taskbtn_y(H);
            if (bx + TASK_W > (int32_t)W) break;
            if (point_in(cx, cy, bx, by, TASK_W, TASK_H)) {
                focus_window(s);
                g_prev_buttons = g_buttons;
                return;
            }
            idx++;
        }
    }

    /* 3) windows, FRONT to back (topmost first) */
    for (int32_t i = g_zcount - 1; i >= 0; i--) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
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
            destroy_slot(slot);
            print("[SHELL] close win "); print_num(id); print("\n");
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3b) titlebar (not the close box) -> focus + begin drag-move */
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
    print("[SHELL] M4 desktop shell starting\n");

    for (int i = 0; i < MAX_WINDOWS; i++) g_windows[i].used = 0;
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
        /* a) drain client requests */
        if (inbox_qid >= 0) drain_inbox(inbox_qid);

        /* b) pump input devices (updates cursor + buttons, forwards kbd/ptr) */
        pump_input(g_kbd_fd,   1, W, H);
        pump_input(g_mouse_fd, 0, W, H);

        /* c) resolve desktop-shell mouse interaction (click/drag/close/launch) */
        handle_mouse(W, H);

        /* d) composite + present */
        composite(back, W, H, stride, g_cursor_x, g_cursor_y);
        if (back != hw) present(hw, back, H, stride);

        frame++;
        if ((frame % 60) == 0) {
            print("[SHELL] frame "); print_num((long)frame);
            print(" ("); print_num((long)g_zcount);
            print(" windows)\n");
        }

        /* e) ALWAYS yield at least once per frame so init and clients get
         * scheduled even when a frame exceeds the 16ms budget. */
        syscall(SYS_YIELD, 0, 0, 0);
        next += 16;
        long now;
        for (;;) {
            now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
            if (now >= next) break;
            syscall(SYS_YIELD, 0, 0, 0);
        }
        if (next < now - 64) next = now;     /* resync after a clock jump */
    }
}
