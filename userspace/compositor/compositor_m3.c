/*
 * compositor_m3.c -- Milestone M3 multi-client compositing SERVER (ring 3).
 *
 * Extends the M2 compositor (gradient desktop + cursor + one hard-coded
 * window) into a real display SERVER: it accepts client windows over SysV
 * IPC, composites each client's shared-memory pixel buffer on screen with
 * window decorations (titlebar + border), and routes keyboard/pointer input
 * to the focused client.
 *
 * ARCHITECTURE (single-process, cooperative -- one yield-paced frame loop):
 *   - A SysV message queue at WL_COMP_INBOX_KEY is the server's command inbox.
 *   - Clients shmget()+shmat() a pixel buffer, then msgsnd WL_REQ_CREATE with
 *     their shm_id. We shmat() that same segment into THIS process (the kernel
 *     maps it into the caller -- verified), giving zero-copy access to the
 *     client's pixels. We reply WL_EVT_CREATED with the assigned win_id to the
 *     client's per-pid reply queue WL_REPLY_KEY(pid).
 *   - Each frame: drain the inbox, composite the desktop + all windows
 *     back-to-front (blit shm pixels, draw decorations) + cursor, present.
 *   - Input: /dev/input/event0 (keyboard) + event1 (mouse) are polled
 *     non-blocking each frame; relative mouse motion updates the cursor and
 *     key/pointer events are forwarded to the FOCUSED (topmost) window.
 *
 * PROTOCOL (mirrors userspace/include/wl_proto.h, authored by a sibling
 * agent -- redeclared locally so this file builds standalone):
 *   client -> server  (queue WL_COMP_INBOX_KEY):
 *     WL_REQ_CREATE=1  {long mtype; int pid; int shm_id; unsigned w,h,stride; char title[48];}
 *     WL_REQ_COMMIT=2  {long mtype; int win_id; unsigned x,y,w,h;}
 *     WL_REQ_DESTROY=3 {long mtype; int win_id;}
 *   server -> client  (queue WL_REPLY_KEY(client_pid)):
 *     WL_EVT_CREATED=1 {long mtype; int win_id;}
 *     WL_EVT_POINTER=2 {long mtype; int x,y,buttons;}
 *     WL_EVT_KEY=3     {long mtype; int keycode,pressed;}
 *
 * Build (EXACT -- flags DIRECT, never via an unquoted shell var):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/compositor/compositor_m3.c -o compositor_m3.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       compositor_m3.o -o compositor_m3.elf
 *   objdump -d compositor_m3.elf | grep "fs:0x28"   # MUST be empty
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
 *  Pixel math (self-contained, from compositor_m2.c)                      *
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

/*
 * Blit a client's shared-memory pixel buffer (ARGB32, row stride in pixels)
 * into the back buffer at (dx,dy), clipped to the screen. Pixels are copied
 * opaque (clients own their content; alpha could be honored later via
 * blend_pixel -- left opaque for speed and to match a typical CSD client).
 */
static void blit_surface(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                         const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t sstride,
                         int32_t dx, int32_t dy) {
    if (!src || sw == 0 || sh == 0) return;
    for (uint32_t sy = 0; sy < sh; sy++) {
        int32_t py = dy + (int32_t)sy;
        if (py < 0 || py >= (int32_t)bh) continue;
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
 *  Theme                                                                  *
 * ====================================================================== */
#define DESK_TOP        0x00203A5Au
#define DESK_BOT        0x000E1B2Bu
#define TITLEBAR_FOCUS  0xFF2C3E50u   /* focused window titlebar (slate)    */
#define TITLEBAR_UNFOC  0xFF4A5A66u   /* unfocused titlebar (muted)         */
#define BORDER_FOCUS    0xFF3498DBu   /* focused border (blue accent)       */
#define BORDER_UNFOC    0xFF566573u   /* unfocused border (gray)            */
#define SHADOW_COLOR    0x40000000u
#define CURSOR_FILL     0xFFFFFFFFu
#define CURSOR_EDGE     0xFF000000u
#define BTN_CLOSE       0xFFE74C3Cu
#define BTN_MIN         0xFFF1C40Fu
#define WIN_PLACEHOLDER 0xFF1B262Eu   /* shown if a client has no shm yet   */

#define TITLEBAR_H  28
#define BORDER_W    1

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t r = ar + (br - ar) * num / den;
    uint32_t g = ag + (bg - ag) * num / den;
    uint32_t bl = ab + (bb - ab) * num / den;
    return (r << 16) | (g << 8) | bl;
}

/* ---- cursor arrow bitmap (from compositor_m2.c) ---- */
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
    /* remove if already present */
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
 * top-left of the titlebar; the client area sits below it. */
static void render_window(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                          window_t *win, int focused) {
    int32_t cw = (int32_t)win->w;
    int32_t ch = (int32_t)win->h;
    int32_t fx = win->x;                       /* frame (titlebar) x       */
    int32_t fy = win->y;                       /* frame (titlebar) y       */
    int32_t client_x = fx;
    int32_t client_y = fy + TITLEBAR_H;        /* client below titlebar    */

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

    /* titlebar buttons (close + minimize) */
    fill_rect(buf, w, h, stride, fx + cw - 22, fy + 9, 11, 11, BTN_CLOSE);
    fill_rect(buf, w, h, stride, fx + cw - 40, fy + 9, 11, 11, BTN_MIN);

    /* window title text (bitmap font, linked from userspace/lib/font/bitfont.c) */
    extern int font_draw_string(unsigned int *fbuf, int fstride, int fbw, int fbh,
                                int tx, int ty, const char *s, unsigned int color);
    font_draw_string(buf, (int)stride, (int)w, (int)h,
                     fx + 8, fy + (TITLEBAR_H - 16) / 2, win->title, 0xFFECF0F1u);

    /* client surface: blit the client's shared-memory pixels, or a
     * placeholder fill if the client hasn't produced pixels yet. */
    if (win->pixels) {
        blit_surface(buf, w, h, stride,
                     win->pixels, win->w, win->h, win->stride,
                     client_x, client_y);
    } else {
        fill_rect(buf, w, h, stride, client_x, client_y, cw, ch, WIN_PLACEHOLDER);
    }
}

static void composite(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                      int32_t cursor_x, int32_t cursor_y) {
    render_desktop(buf, w, h, stride);

    int top = focused_slot();
    for (int32_t i = 0; i < g_zcount; i++) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        render_window(buf, w, h, stride, &g_windows[slot], slot == top);
    }

    draw_cursor(buf, w, h, stride, cursor_x, cursor_y);
}

static void present(uint32_t *fb, uint32_t *back, uint32_t h, uint32_t stride) {
    uint32_t total = h * stride;
    for (uint32_t i = 0; i < total; i++) fb[i] = back[i];
}

/* ====================================================================== *
 *  Client requests                                                        *
 * ====================================================================== */
static void handle_create(const wl_req_create_t *req) {
    int slot = find_free_slot();
    if (slot < 0) {
        print("[COMP] window limit reached, rejecting create from pid=");
        print_num(req->pid); print("\n");
        return;   /* no slot -> no WL_EVT_CREATED (client will time out) */
    }

    window_t *win = &g_windows[slot];

    /* zero the slot, then populate */
    for (size_t i = 0; i < sizeof(*win); i++) ((char *)win)[i] = 0;
    win->used       = 1;
    win->win_id     = g_next_win_id++;
    win->client_pid = req->pid;
    win->reply_qid  = -1;
    win->shm_id     = req->shm_id;
    win->w          = req->w;
    win->h          = req->h;
    /* The protocol carries stride in BYTES (client sends w*4); our blit uses
     * stride in PIXELS, so convert. Without this the blit reads w*4 pixels per
     * row and runs off the end of the client's shm buffer. */
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
        win->pixels    = 0;        /* placeholder fill until shm is valid  */
        print("[COMP] shmat FAILED shm_id="); print_num(req->shm_id);
        print(" r="); print_num(addr); print("\n");
    }

    /* staggered placement so windows don't fully overlap */
    win->x = 80 + (slot % 6) * 48;
    win->y = 60 + (slot % 6) * 40;

    z_push_front(slot);            /* new window becomes focused (topmost)  */

    /* reply to the client with the assigned win_id */
    int32_t qid = client_reply_qid(win);
    if (qid >= 0) {
        wl_evt_created_t ev;
        ev.mtype  = WL_EVT_CREATED;
        ev.win_id = win->win_id;
        long r = sc6(SYS_MSGSND, qid, (long)&ev,
                     (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
        if (r < 0) {
            print("[COMP] msgsnd CREATED failed r="); print_num(r); print("\n");
        }
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

    /* If the surface had no shm mapping yet (created before shm was ready),
     * try to attach now -- a commit means the client has produced content. */
    if (!win->pixels && win->shm_id > 0) {
        long addr = sc6(SYS_SHMAT, (long)win->shm_id, 0, 0, 0, 0, 0);
        if (addr > 0) {
            win->shm_vaddr = (uint64_t)addr;
            win->pixels    = (uint32_t *)addr;
        }
    }
    (void)req; /* x/y/w/h damage rect accepted; we redraw the whole frame */
}

static void handle_destroy(const wl_req_destroy_t *req) {
    int slot = slot_by_win_id(req->win_id);
    if (slot < 0) return;
    window_t *win = &g_windows[slot];

    if (win->shm_vaddr) {
        sc6(SYS_SHMDT, (long)win->shm_vaddr, 0, 0, 0, 0, 0);
        win->shm_vaddr = 0;
        win->pixels    = 0;
    }
    z_remove(slot);
    print("[COMP] client disconnected win="); print_num(win->win_id); print("\n");
    win->used = 0;
}

/* Drain the inbox queue (non-blocking) and dispatch all pending requests. */
static void drain_inbox(int32_t inbox_qid) {
    wl_inbox_msg_t msg;
    for (;;) {
        /* msgrcv: type 0 = first message of any type; NOWAIT returns ENOMSG
         * when empty. msgsz is the max payload size we can accept. */
        long r = sc6(SYS_MSGRCV, inbox_qid, (long)&msg,
                     (long)(sizeof(msg) - sizeof(int64_t)), 0, (long)IPC_NOWAIT, 0);
        if (r < 0) break;                  /* ENOMSG (empty) or error -> stop */

        switch (msg.mtype) {
            case WL_REQ_CREATE:  handle_create(&msg.create);   break;
            case WL_REQ_COMMIT:  handle_commit(&msg.commit);   break;
            case WL_REQ_DESTROY: handle_destroy(&msg.destroy); break;
            default: /* unknown message type -- ignore */      break;
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

/* input event types (from kernel/include/input.h) */
#define EV_KEY  0
#define EV_REL  1

/* relative axes */
#define REL_X   0
#define REL_Y   1

/* mouse buttons */
#define BTN_LEFT_CODE    0x110
#define BTN_RIGHT_CODE   0x111
#define BTN_MIDDLE_CODE  0x112

#define EVENTS_PER_READ  32

static int32_t g_kbd_fd   = -1;
static int32_t g_mouse_fd = -1;

/* live pointer state forwarded to focused window */
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
    /* report pointer in the focused window's CLIENT-area coordinates */
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

/* Read + process all currently-buffered events from one fd.
 * keyboard==1 routes EV_KEY presses to the focused window; otherwise the
 * fd is treated as the mouse (EV_REL motion + EV_KEY button codes). */
static void pump_input(int32_t fd, int keyboard, uint32_t W, uint32_t H) {
    if (fd < 0) return;
    input_event_t evs[EVENTS_PER_READ];
    long n = syscall(SYS_READ, fd, (long)evs, (long)sizeof(evs));
    if (n <= 0) return;                       /* 0 = no events (never blocks) */
    long count = n / (long)sizeof(input_event_t);
    int pointer_changed = 0;

    for (long i = 0; i < count; i++) {
        input_event_t *e = &evs[i];
        if (keyboard) {
            if (e->type == EV_KEY) {
                /* value: 0=release 1=press 2=repeat -> pressed = value!=0 */
                send_key_to_focus((int32_t)e->code, e->value != 0 ? 1 : 0);
            }
            continue;
        }
        /* mouse */
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

/* ====================================================================== */
void _start(void) {
    print("[COMP] M3 server starting\n");

    /* zero global window registry (no static init guarantee under -nostdlib;
     * .bss IS zeroed by the loader, but be explicit for clarity). */
    for (int i = 0; i < MAX_WINDOWS; i++) g_windows[i].used = 0;
    g_zcount = 0;

    /* 1. Acquire the framebuffer. */
    fb_acquire_t fb;
    long r = syscall(SYS_FB_ACQUIRE, (long)&fb, 0, 0);
    if (r != 0) {
        print("[COMP] fb_acquire FAILED r="); print_num(r); print("\n");
        for (;;) syscall(SYS_YIELD, 0, 0, 0);
    }
    uint32_t W = fb.width, H = fb.height;
    uint32_t stride = fb.pitch / 4;
    print("[COMP] fb "); print_num(W); print("x"); print_num(H);
    print(" pitch="); print_num(fb.pitch);
    print(" bpp="); print_num(fb.bpp); print("\n");

    /* 2. Allocate the back buffer. */
    size_t bb_bytes = (size_t)fb.pitch * H;
    long bbp = syscall(SYS_MMAP, 0, (long)bb_bytes, VMM_PROT_READ | VMM_PROT_WRITE);
    uint32_t *hw   = (uint32_t *)fb.vaddr;
    uint32_t *back;
    if (bbp > 0) {
        back = (uint32_t *)bbp;
        print("[COMP] back buffer OK bytes="); print_num((long)bb_bytes); print("\n");
    } else {
        back = hw;
        print("[COMP] back buffer mmap FAILED ("); print_num(bbp);
        print(") -- rendering direct to fb\n");
    }

    /* 3. Create the server command inbox (SysV message queue). */
    long inbox = sc6(SYS_MSGGET, (long)WL_COMP_INBOX_KEY,
                     (long)(IPC_CREAT | 0666), 0, 0, 0, 0);
    if (inbox < 0) {
        print("[COMP] msgget(inbox) FAILED r="); print_num(inbox);
        print(" -- continuing without IPC (no clients)\n");
    } else {
        print("[COMP] inbox queue id="); print_num(inbox);
        print(" key=0x434F4D50\n");
    }
    int32_t inbox_qid = (int32_t)inbox;

    /* 4. Open input devices non-blocking (degrade gracefully on failure). */
    g_kbd_fd   = (int32_t)syscall(SYS_OPEN, (long)"/dev/input/event0",
                                  (long)(O_RDONLY | O_NONBLOCK), 0);
    g_mouse_fd = (int32_t)syscall(SYS_OPEN, (long)"/dev/input/event1",
                                  (long)(O_RDONLY | O_NONBLOCK), 0);
    if (g_kbd_fd < 0)
        print("[COMP] WARN: /dev/input/event0 (kbd) unavailable -- degraded\n");
    else { print("[COMP] keyboard fd="); print_num(g_kbd_fd); print("\n"); }
    if (g_mouse_fd < 0)
        print("[COMP] WARN: /dev/input/event1 (mouse) unavailable -- degraded\n");
    else { print("[COMP] mouse fd="); print_num(g_mouse_fd); print("\n"); }

    /* cursor starts at screen center */
    g_cursor_x = (int32_t)(W / 2);
    g_cursor_y = (int32_t)(H / 2);

    /* 5. Frame loop. */
    long t0 = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    long next = t0;
    uint64_t frame = 0;

    print("[COMP] entering frame loop\n");
    for (;;) {
        /* a) drain client requests */
        if (inbox_qid >= 0) drain_inbox(inbox_qid);

        /* b) pump input devices, forward to focused window */
        pump_input(g_kbd_fd,   1, W, H);
        pump_input(g_mouse_fd, 0, W, H);

        /* c) composite + present */
        composite(back, W, H, stride, g_cursor_x, g_cursor_y);
        if (back != hw) present(hw, back, H, stride);

        frame++;
        if ((frame % 60) == 0) {
            print("[COMP] frame "); print_num((long)frame);
            print(" presented ("); print_num((long)g_zcount);
            print(" windows)\n");
        }

        /* d) ALWAYS yield at least once per frame so init and clients get
         * scheduled even when a frame exceeds the 16ms budget (full-screen
         * software present is expensive and otherwise starves everyone). */
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
