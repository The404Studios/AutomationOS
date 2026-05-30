/*
 * imageviewer.c -- "Image Viewer" desktop app (ring 3, freestanding).
 * ===================================================================
 *
 * Loads a PNG (or JPEG) from the initrd, zero-copy, via SYS_MAP_FILE, decodes
 * it with the de-POSIX'd image module (stb_image, STBI_NO_STDIO +
 * stbi_load_from_memory), converts RGBA -> the compositor window's ARGB32
 * (0x00RRGGBB) buffer, and scale-to-fits + centers it. If no image can be
 * loaded it draws a "No image" message so the app still runs visibly.
 *
 * No libc startup is assumed beyond _start; this TU defines its own _start.
 * All I/O is raw syscalls. Diagnostics go to serial (SYS_WRITE=3, fd 1):
 *   "[IMGVIEW] loaded WxH"        on success
 *   "[IMGVIEW] ... no image ..."  on failure (still shows a window)
 *
 * Build (flags DIRECT on the command line -- never via a shell variable, or
 * -fno-stack-protector can be dropped and you fault with CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -DIMAGE_FREESTANDING \
 *       -c imageviewer.c -o imageviewer.o
 *   ...link with image.o wl_client.o bitfont.o + libc objects, -T userspace.ld.
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/*
 * Pull in the image module's public API. IMAGE_FREESTANDING makes image.h use
 * its own fixed-width typedefs and hide the fopen/icon entry points, exposing
 * exactly the memory loader + helpers we need.
 */
#ifndef IMAGE_FREESTANDING
#define IMAGE_FREESTANDING
#endif
#include "../../lib/image/image.h"

/* ---- syscall numbers (mirror kernel/include/syscall.h) ---- */
#define SYS_WRITE     3
#define SYS_YIELD     15
#define SYS_MAP_FILE  17

typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long ulong;

/* 6-argument inline syscall (args rdi/rsi/rdx/r10/r8/r9). */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny serial diagnostics (fd 1) ---- */
static ulong sv_strlen(const char *s) { ulong n = 0; while (s && s[n]) n++; return n; }
static void  print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)sv_strlen(m), 0, 0, 0); }
static void  print_num(long n) {
    char b[24]; int i = 0;
    if (n < 0) { print("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}

/* SYS_MAP_FILE wrapper: returns 0 and writes addr+size out-params on success. */
static int map_file(const char *path, void **addr, ulong *size) {
    return (int)sc(SYS_MAP_FILE, (long)path, (long)addr, (long)size, 0, 0, 0);
}

/* ---- window geometry / palette ---- */
#define WIN_W       640
#define WIN_H       480
#define BG_COLOR    0xFF1B1F26u   /* dark slate canvas        */
#define MAT_COLOR   0xFF11141Au   /* slightly darker matte    */
#define INK_WHITE   0xFFFFFFFFu
#define INK_AMBER   0xFFF1C40Fu
#define INK_GREY    0xFFA0A8B4u

/*
 * Candidate initrd paths, tried in order. The integrator must place ONE test
 * image here; "/usr/share/wallpapers/test.png" is the documented default.
 */
static const char *const IMAGE_CANDIDATES[] = {
    "/usr/share/wallpapers/test.png",
    "/usr/share/images/test.png",
    "/test.png",
    "/image.png",
    "/usr/share/wallpapers/test.jpg",
    "/test.jpg",
    0
};

/* Fill a clipped rectangle into the ARGB32, stride-addressed window buffer. */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color) {
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w, y2 = y + h;
    if (x2 > (i32)bw) x2 = (i32)bw;
    if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/*
 * Sample a decoded image_t at fractional source coords using the nearest
 * pixel and return ARGB32 (0xAARRGGBB). image_get_pixel already packs to
 * (A<<24)|(R<<16)|(G<<8)|B regardless of source channel count, which is
 * exactly the framebuffer's order, so no per-pixel channel swap is needed.
 */
static inline u32 sample_argb(const image_t *img, u32 sx, u32 sy) {
    return image_get_pixel(img, sx, sy);
}

/*
 * Draw the image scaled-to-fit (preserving aspect ratio) and centered inside
 * the window. dst region is [0,bw) x [0,bh). Uses nearest-neighbor sampling so
 * we avoid any float math beyond a couple of ratios. Alpha is composited over
 * the matte so transparent PNGs look correct.
 */
static void blit_fit_centered(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                              const image_t *img) {
    if (!img || img->width == 0 || img->height == 0) return;

    /* Compute scaled size that fits inside (bw,bh) keeping aspect ratio. */
    u32 iw = img->width, ih = img->height;
    /* dst = min over the two axes; use 64-bit-safe integer ratio in floats. */
    double sx = (double)bw / (double)iw;
    double sy = (double)bh / (double)ih;
    double s  = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;                 /* never upscale past 1:1           */

    u32 dw = (u32)((double)iw * s);
    u32 dh = (u32)((double)ih * s);
    if (dw == 0) dw = 1;
    if (dh == 0) dh = 1;

    i32 ox = (i32)((bw - dw) / 2);
    i32 oy = (i32)((bh - dh) / 2);

    double x_ratio = (double)iw / (double)dw;
    double y_ratio = (double)ih / (double)dh;

    for (u32 dy = 0; dy < dh; dy++) {
        i32 fy = oy + (i32)dy;
        if (fy < 0 || fy >= (i32)bh) continue;
        u32 *row = buf + (u32)fy * stride_px;
        u32 syi = (u32)((double)dy * y_ratio);
        if (syi >= ih) syi = ih - 1;
        for (u32 dx = 0; dx < dw; dx++) {
            i32 fx = ox + (i32)dx;
            if (fx < 0 || fx >= (i32)bw) continue;
            u32 sxi = (u32)((double)dx * x_ratio);
            if (sxi >= iw) sxi = iw - 1;

            u32 px = sample_argb(img, sxi, syi);
            u32 a  = (px >> 24) & 0xFF;
            if (a == 0xFF) {
                row[fx] = (px & 0x00FFFFFFu) | 0xFF000000u;
            } else if (a == 0) {
                /* fully transparent -> leave matte */
            } else {
                /* alpha-over the existing destination pixel */
                u32 d = row[fx];
                u32 sr = (px >> 16) & 0xFF, sg = (px >> 8) & 0xFF, sb = px & 0xFF;
                u32 dr = (d  >> 16) & 0xFF, dg = (d  >> 8) & 0xFF, db = d  & 0xFF;
                u32 r = (sr * a + dr * (255 - a)) / 255;
                u32 g = (sg * a + dg * (255 - a)) / 255;
                u32 b = (sb * a + db * (255 - a)) / 255;
                row[fx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}

/* Centered single-line text helper. */
static void draw_centered(u32 *buf, u32 stride_px, u32 bw, u32 bh,
                          i32 cy, const char *s, u32 color) {
    int tw = font_text_width(s);
    int x  = (i32)bw / 2 - tw / 2;
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh, x, cy, s, color);
}

void _start(void) {
    print("[IMGVIEW] starting\n");

    if (wl_connect() != 0) {
        print("[IMGVIEW] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Image Viewer");
    if (!win) {
        print("[IMGVIEW] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    print("[IMGVIEW] window ");
    print_num(win->win_id);
    print(" created\n");

    /* Try each candidate path: map the file, then decode from memory. */
    image_t *img = 0;
    const char *loaded_path = 0;
    for (int i = 0; IMAGE_CANDIDATES[i]; i++) {
        void *data = 0; ulong size = 0;
        print("[IMGVIEW] trying ");
        print(IMAGE_CANDIDATES[i]);
        print("\n");

        if (map_file(IMAGE_CANDIDATES[i], &data, &size) != 0 || !data || size == 0) {
            continue;   /* not present in initrd; try next */
        }

        /* Decode to RGBA so alpha is always available. */
        img = image_load_from_memory(data, (size_t)size, IMAGE_LOAD_RGBA);
        if (img) {
            loaded_path = IMAGE_CANDIDATES[i];
            break;
        }
        print("[IMGVIEW] decode failed: ");
        print(image_get_error());
        print("\n");
    }

    if (img) {
        print("[IMGVIEW] loaded ");
        print_num((long)img->width);
        print("x");
        print_num((long)img->height);
        print(" from ");
        print(loaded_path);
        print("\n");
    } else {
        print("[IMGVIEW] no image found in initrd "
              "(place a PNG at /usr/share/wallpapers/test.png)\n");
    }

    u32 stride_px = win->stride / 4u;
    ulong frame = 0;

    for (;;) {
        /* Matte/canvas clear each frame. */
        fill_rect(win->pixels, win->w, win->h, stride_px,
                  0, 0, (i32)win->w, (i32)win->h, MAT_COLOR);

        if (img) {
            blit_fit_centered(win->pixels, win->w, win->h, stride_px, img);

            /* Small caption strip with the dimensions. */
            char cap[48];
            int p = 0;
            const char *pre = "Image Viewer  ";
            for (const char *q = pre; *q && p < 40; q++) cap[p++] = *q;
            /* append WxH manually (no libc printf) */
            char tmp[24]; int t = 0; long n = (long)img->width;
            do { tmp[t++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
            while (t > 0 && p < 46) cap[p++] = tmp[--t];
            if (p < 46) cap[p++] = 'x';
            t = 0; n = (long)img->height;
            do { tmp[t++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
            while (t > 0 && p < 47) cap[p++] = tmp[--t];
            cap[p] = '\0';

            fill_rect(win->pixels, win->w, win->h, stride_px,
                      0, (i32)win->h - FONT_H - 6, (i32)win->w, FONT_H + 6, BG_COLOR);
            draw_centered(win->pixels, stride_px, win->w, win->h,
                          (i32)win->h - FONT_H - 2, cap, INK_GREY);
        } else {
            /* "No image" placeholder so the app is clearly alive. */
            draw_centered(win->pixels, stride_px, win->w, win->h,
                          (i32)win->h / 2 - FONT_H, "No image", INK_AMBER);
            draw_centered(win->pixels, stride_px, win->w, win->h,
                          (i32)win->h / 2 + 4,
                          "Add a PNG: /usr/share/wallpapers/test.png", INK_GREY);
        }

        /* Drain input without blocking (no interactions yet). */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            (void)kind; (void)a; (void)b; (void)c;
        }

        wl_commit(win);

        frame++;
        if ((frame % 240) == 0) {
            print("[IMGVIEW] committed frame ");
            print_num((long)frame);
            print("\n");
        }

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
