/*
 * wltest.c -- M3 "Wayland-lite" test client (freestanding, ring 3).
 * =================================================================
 *
 * Connects to the compositor via the wl_client library, creates a 400x260
 * window, and runs a frame loop: fills the shared ARGB32 buffer with a solid
 * background plus a box that slides across, driven by SYS_GET_TICKS_MS, then
 * commits each frame. Diagnostics go to serial (fd 1).
 *
 * Build (EXACT -- flags passed DIRECTLY, never via an unquoted variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/wltest/wltest.c -o wltest.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o wl_client.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       wltest.o wl_client.o -o wltest
 *   objdump -d wltest | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"

/* ---- syscall numbers (per task spec) ---- */
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

typedef unsigned int   u32;
typedef int            i32;

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

#define WIN_W  400
#define WIN_H  260
#define BG_COLOR   0xFF1E3A5Fu   /* deep blue background        */
#define BOX_COLOR  0xFFF1C40Fu   /* yellow sliding box          */
#define BOX_W      48
#define BOX_H      48

void _start(void) {
    print("[WLTEST] starting\n");

    if (wl_connect() != 0) {
        print("[WLTEST] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "wltest");
    if (!win) {
        print("[WLTEST] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[WLTEST] window ");
    print_num(win->win_id);
    print(" created\n");

    u32 stride_px = win->stride / 4u;   /* pixels per row */
    long t0 = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    unsigned long frame = 0;

    for (;;) {
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        long t = now - t0;

        /* Box slides horizontally, bouncing across the client width. */
        i32 span = (i32)win->w - BOX_W;
        if (span < 0) span = 0;
        i32 period = 2 * (span > 0 ? span : 1);
        i32 p = (i32)((t / 8) % period);          /* triangle wave phase */
        i32 box_x = p <= span ? p : (period - p);
        i32 box_y = (i32)(win->h / 2) - BOX_H / 2;

        /* Render: solid background + moving box. */
        fill_rect(win->pixels, win->w, win->h, stride_px,
                  0, 0, (i32)win->w, (i32)win->h, BG_COLOR);
        fill_rect(win->pixels, win->w, win->h, stride_px,
                  box_x, box_y, BOX_W, BOX_H, BOX_COLOR);

        /* Drain any input events (pointer / key) without blocking. */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            /* M3 test client just acknowledges input on serial. */
            if (kind == WL_EVENT_POINTER) {
                print("[WLTEST] pointer "); print_num(a); print(",");
                print_num(b); print(" btn="); print_num(c); print("\n");
            } else if (kind == WL_EVENT_KEY) {
                print("[WLTEST] key "); print_num(a);
                print(" pressed="); print_num(b); print("\n");
            }
        }

        wl_commit(win);

        frame++;
        if ((frame % 60) == 0) {
            print("[WLTEST] committed frame ");
            print_num((long)frame);
            print("\n");
        }

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
