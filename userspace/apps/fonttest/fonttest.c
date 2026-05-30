/*
 * fonttest.c -- Prove the anti-aliased scalable TrueType module (ring 3).
 * =======================================================================
 *
 * Freestanding _start. Connects to the compositor (wl_client), loads a .ttf
 * from the initrd via ttf_load() (SYS_MAP_FILE under the hood), then renders
 * "AutomationOS 0123" at 12px, 18px and 32px into a window to demonstrate
 * anti-aliasing + arbitrary scaling. Commits once per frame and loops.
 *
 * Diagnostics go to serial (SYS_WRITE=3, fd 1):
 *   "[FONTTEST] ttf loaded"  on success, or an error line otherwise.
 *
 * Build (flags DIRECT on the command line -- see fonttest's section in the
 * task report; never pass them through an unquoted shell variable or the
 * -fno-stack-protector can be dropped and you get a CR2=0x28 canary fault):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -c fonttest.c -o fonttest.o
 *   ...link with ttf.o wl_client.o + libc/libm objects, -T userspace.ld...
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/ttf.h"

/* ---- syscall numbers ---- */
#define SYS_WRITE  3
#define SYS_YIELD  15

typedef unsigned int u32;
typedef int          i32;

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
static unsigned long k_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }
static void print_num(long n) {
    char b[24]; int i = 0;
    if (n < 0) { print("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}

/* Fill a clipped rectangle in the ARGB32, stride-addressed window buffer. */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color) {
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w;
    i32 y2 = y + h;
    if (x2 > (i32)bw) x2 = (i32)bw;
    if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

#define WIN_W   520
#define WIN_H   260
#define BG_COLOR   0xFF202830u   /* dark slate background */
#define INK_WHITE  0xFFFFFFFFu   /* white text            */
#define INK_AMBER  0xFFF1C40Fu   /* amber text            */
#define INK_CYAN   0xFF4FD0E0u   /* cyan text             */

/*
 * Candidate initrd paths for a TrueType font. ttf_load() tries each until one
 * maps + initialises. The integrator should place ONE of these .ttf files in
 * the initrd; "/fonts/DejaVuSans.ttf" is the documented default.
 */
static const char *const FONT_CANDIDATES[] = {
    "/fonts/DejaVuSans.ttf",
    "/fonts/font.ttf",
    "/DejaVuSans.ttf",
    "/font.ttf",
    0
};

static const char *SAMPLE = "AutomationOS 0123";

void _start(void) {
    print("[FONTTEST] starting\n");

    if (wl_connect() != 0) {
        print("[FONTTEST] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "fonttest");
    if (!win) {
        print("[FONTTEST] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    print("[FONTTEST] window ");
    print_num(win->win_id);
    print(" created\n");

    /* Try the candidate font paths in order. */
    int loaded = 0;
    for (int i = 0; FONT_CANDIDATES[i]; i++) {
        print("[FONTTEST] trying font ");
        print(FONT_CANDIDATES[i]);
        print("\n");
        if (ttf_load(FONT_CANDIDATES[i]) == 0) {
            print("[FONTTEST] ttf loaded\n");   /* success marker */
            loaded = 1;
            break;
        }
    }
    if (!loaded) {
        print("[FONTTEST] ERROR: no usable .ttf in initrd "
              "(place one at /fonts/DejaVuSans.ttf)\n");
        /* Keep the window alive so the failure is visible on screen too. */
    }

    u32 stride_px = win->stride / 4u;
    unsigned long frame = 0;

    for (;;) {
        /* Clear to background each frame. */
        fill_rect(win->pixels, win->w, win->h, stride_px,
                  0, 0, (i32)win->w, (i32)win->h, BG_COLOR);

        if (loaded) {
            /*
             * Three sizes to prove AA + scaling. y is the BASELINE, so each
             * line's baseline is placed below the previous by roughly its
             * size to avoid overlap.
             */
            ttf_draw_text(win->pixels, (int)stride_px, (int)win->w, (int)win->h,
                          16, 40,  SAMPLE, 12, INK_WHITE);
            ttf_draw_text(win->pixels, (int)stride_px, (int)win->w, (int)win->h,
                          16, 80,  SAMPLE, 18, INK_AMBER);
            ttf_draw_text(win->pixels, (int)stride_px, (int)win->w, (int)win->h,
                          16, 150, SAMPLE, 32, INK_CYAN);
        } else {
            /* Visible marker block if the font is missing. */
            fill_rect(win->pixels, win->w, win->h, stride_px,
                      16, 16, 200, 24, 0xFFCC3333u);
        }

        /* Drain input without blocking. */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            /* fonttest just acknowledges input; nothing interactive yet. */
            (void)kind; (void)a; (void)b; (void)c;
        }

        wl_commit(win);

        frame++;
        if ((frame % 120) == 0) {
            print("[FONTTEST] committed frame ");
            print_num((long)frame);
            print("\n");
        }

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
