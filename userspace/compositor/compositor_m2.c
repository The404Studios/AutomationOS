/*
 * compositor_m2.c -- Milestone M2 minimal compositor (freestanding, ring 3).
 *
 * Draws the FIRST real graphics on this OS: a desktop background (vertical
 * gradient), ONE window with a titlebar + 1px border, and a mouse cursor,
 * presented to the hardware framebuffer in a ~60 FPS frame loop.
 *
 * No libc, no Linux deps -- pure inline-syscall + self-contained pixel math
 * (alpha-blend + rect fill adapted from userspace/compositor/blit.c).
 *
 * Build (EXACT):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/compositor/compositor_m2.c -o compositor_m2.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       compositor_m2.o -o compositor_m2.elf
 *   objdump -d compositor_m2.elf | grep "fs:0x28"   # MUST be empty
 */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       size_t;
typedef unsigned long long  uint64_t;
typedef int                 int32_t;

/* ---- syscall numbers ---- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_YIELD         15
#define SYS_MMAP          37
#define SYS_FB_ACQUIRE    39
#define SYS_GET_TICKS_MS  40

#define VMM_PROT_READ   0x01
#define VMM_PROT_WRITE  0x02

static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
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
 *  Pixel math (self-contained, adapted from blit.c)                       *
 * ====================================================================== */

/* Alpha blend ARGB32 src over dst: result = src*a + dst*(1-a). */
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

/* Fill a clipped rectangle in a stride-addressed buffer with a solid color. */
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

/* Alpha-blended filled rectangle (for shadow / cursor with transparency). */
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

/* 1px-thick rectangle outline (border). */
static void stroke_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                        int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fill_rect(buf, bw, bh, stride, x, y, w, 1, color);             /* top    */
    fill_rect(buf, bw, bh, stride, x, y + h - 1, w, 1, color);     /* bottom */
    fill_rect(buf, bw, bh, stride, x, y, 1, h, color);            /* left   */
    fill_rect(buf, bw, bh, stride, x + w - 1, y, 1, h, color);    /* right  */
}

/* ====================================================================== *
 *  Theme (colors proven good in composition.c)                           *
 * ====================================================================== */
#define DESK_TOP        0x00203A5Au   /* gradient top    (deep blue)   */
#define DESK_BOT        0x000E1B2Bu   /* gradient bottom (near black)  */
#define WIN_BODY        0xFFECF0F1u   /* window client area (light)    */
#define TITLEBAR_COLOR  0xFF2C3E50u   /* dark slate titlebar           */
#define BORDER_COLOR    0xFF3498DBu   /* blue accent border            */
#define SHADOW_COLOR    0x40000000u   /* 25% black drop shadow         */
#define CURSOR_FILL     0xFFFFFFFFu   /* white cursor                  */
#define CURSOR_EDGE     0xFF000000u   /* black cursor outline          */
#define BTN_CLOSE       0xFFE74C3Cu   /* red close button              */
#define BTN_MIN         0xFFF1C40Fu   /* yellow minimize button        */

#define TITLEBAR_H  28
#define BORDER_W    1

/* Linear interpolate one channel of two packed ARGB colors. */
static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t r = ar + (br - ar) * num / den;
    uint32_t g = ag + (bg - ag) * num / den;
    uint32_t bl = ab + (bb - ab) * num / den;
    return (r << 16) | (g << 8) | bl;
}

/* ====================================================================== *
 *  Cursor: a classic top-left pointing arrow drawn from a bitmap mask.   *
 *  '#' = black edge, '.' = white fill, ' ' = transparent.                *
 * ====================================================================== */
#define CUR_W 12
#define CUR_H 19
static const char *CURSOR[CUR_H] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#......#####",
    "#...#..#    ",
    "#..# #..#   ",
    "#.#  #..#   ",
    "##    #..#  ",
    "#     #..#  ",
    "       #..# ",
    "       ####  ",
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
 *  Scene render: paints one full frame into the back buffer.             *
 * ====================================================================== */
static void render_frame(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                         int32_t cursor_x, int32_t cursor_y, int32_t win_x, int32_t win_y) {
    /* 1. Desktop background: vertical gradient (recomputed per row). */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t c = lerp_color(DESK_TOP, DESK_BOT, y, h ? h - 1 : 1);
        uint32_t *r = buf + y * stride;
        for (uint32_t x = 0; x < w; x++) r[x] = c;
    }

    /* 2. One window. geometry = client area; titlebar sits above it. */
    int32_t win_w = 520;
    int32_t win_h = 320;
    int32_t client_x = win_x;
    int32_t client_y = win_y + TITLEBAR_H;     /* client below titlebar */

    /* Drop shadow (alpha-blended, offset down-right). */
    blend_rect(buf, w, h, stride, win_x + 6, win_y + 6, win_w, win_h + TITLEBAR_H, SHADOW_COLOR);

    /* Border frame around whole window (titlebar + client). */
    stroke_rect(buf, w, h, stride,
                win_x - BORDER_W, win_y - BORDER_W,
                win_w + 2 * BORDER_W, win_h + TITLEBAR_H + 2 * BORDER_W, BORDER_COLOR);

    /* Titlebar bar. */
    fill_rect(buf, w, h, stride, win_x, win_y, win_w, TITLEBAR_H, TITLEBAR_COLOR);

    /* Titlebar buttons (close + minimize) at right edge. */
    fill_rect(buf, w, h, stride, win_x + win_w - 22, win_y + 9, 11, 11, BTN_CLOSE);
    fill_rect(buf, w, h, stride, win_x + win_w - 40, win_y + 9, 11, 11, BTN_MIN);

    /* A simple "title" glyph block on the left so the bar reads as a titlebar. */
    fill_rect(buf, w, h, stride, win_x + 10, win_y + 11, 80, 6, 0xFFECF0F1u);

    /* Client area. */
    fill_rect(buf, w, h, stride, client_x, client_y, win_w, win_h, WIN_BODY);

    /* A bit of content inside the client area: an accent header strip + boxes. */
    fill_rect(buf, w, h, stride, client_x + 16, client_y + 16, win_w - 32, 40, BORDER_COLOR);
    fill_rect(buf, w, h, stride, client_x + 16, client_y + 72, 160, 110, 0xFFBDC3C7u);
    fill_rect(buf, w, h, stride, client_x + 192, client_y + 72, 160, 110, 0xFFBDC3C7u);
    fill_rect(buf, w, h, stride, client_x + 368, client_y + 72, win_w - 384, 110, 0xFFBDC3C7u);

    /* 3. Mouse cursor on top of everything. */
    draw_cursor(buf, w, h, stride, cursor_x, cursor_y);
}

/* Copy the back buffer to the hardware framebuffer, honoring pitch. */
static void present(uint32_t *fb, uint32_t *back, uint32_t h, uint32_t stride) {
    /* Both buffers share the same stride (we allocated back at fb pitch),
       so a single linear copy covers all rows. */
    uint32_t total = h * stride;
    for (uint32_t i = 0; i < total; i++) fb[i] = back[i];
}

void _start(void) {
    print("[COMP] starting\n");

    /* Acquire the framebuffer. */
    fb_acquire_t fb;
    long r = syscall(SYS_FB_ACQUIRE, (long)&fb, 0, 0);
    if (r != 0) {
        print("[COMP] fb_acquire FAILED r="); print_num(r); print("\n");
        for (;;) syscall(SYS_YIELD, 0, 0, 0);
    }
    uint32_t W = fb.width, H = fb.height;
    uint32_t stride = fb.pitch / 4;               /* pixels per row */
    print("[COMP] fb "); print_num(W); print("x"); print_num(H);
    print(" pitch="); print_num(fb.pitch);
    print(" bpp="); print_num(fb.bpp); print("\n");

    /* Allocate the back buffer (pitch*height bytes -- ~3MB at 1024x768). */
    size_t bb_bytes = (size_t)fb.pitch * H;
    long bbp = syscall(SYS_MMAP, 0, (long)bb_bytes, VMM_PROT_READ | VMM_PROT_WRITE);
    uint32_t *back;
    uint32_t *hw = (uint32_t *)fb.vaddr;
    if (bbp > 0) {
        back = (uint32_t *)bbp;
        print("[COMP] back buffer OK bytes="); print_num((long)bb_bytes);
        print(" va="); print_num(bbp); print("\n");
    } else {
        /* Fallback: no back buffer -- render straight to the framebuffer. */
        back = hw;
        print("[COMP] back buffer mmap FAILED ("); print_num(bbp);
        print(") -- rendering direct to fb\n");
    }

    /* Frame loop state. */
    int32_t win_x = (int32_t)(W / 2) - 260;       /* center the 520px window */
    int32_t win_y = (int32_t)(H / 2) - 174;
    int32_t cx = (int32_t)(W / 2);                /* cursor start: center    */
    int32_t cy = (int32_t)(H / 2);

    long t0 = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    long next = t0;                               /* next frame deadline (ms)*/
    uint64_t frame = 0;

    print("[COMP] entering frame loop\n");
    for (;;) {
        long now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);

        /* Gentle motion so consecutive frames differ visibly: the cursor
           traces a small Lissajous-ish path via integer steps; the window
           drifts slightly. Purely cosmetic / proof-of-liveness. */
        long phase = (now - t0) / 16;             /* ~frame index by time   */
        int32_t dx = (int32_t)((phase % 240) - 120);   /* -120..119 */
        int32_t dy = (int32_t)(((phase / 2) % 160) - 80);
        cx = (int32_t)(W / 2) + dx;
        cy = (int32_t)(H / 2) + dy;
        if (cx < 0) cx = 0; if (cx >= (int32_t)W) cx = (int32_t)W - 1;
        if (cy < 0) cy = 0; if (cy >= (int32_t)H) cy = (int32_t)H - 1;

        render_frame(back, W, H, stride, cx, cy, win_x, win_y);

        if (back != hw) present(hw, back, H, stride);

        frame++;
        if ((frame % 60) == 0) {
            print("[COMP] frame "); print_num((long)frame); print(" presented\n");
        }

        /* Pace to ~16ms/frame. Yield repeatedly until the deadline. */
        next += 16;
        for (;;) {
            now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
            if (now >= next) break;
            syscall(SYS_YIELD, 0, 0, 0);
        }
        /* If we fell badly behind (e.g. clock jump), resync the deadline. */
        if (next < now - 64) next = now;
    }
}
