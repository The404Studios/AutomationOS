/*
 * screenshot.c -- Framebuffer capture tool (freestanding, ring 3).
 * ================================================================
 *
 * Opens a 420x240 window titled "Screenshot".  The UI has:
 *   - A "Capture Screen" button
 *   - A delay selector: "Now" / "3s" toggle
 *   - A status line showing the saved filename and dimensions
 *   - A thumbnail preview of the last capture (downscaled into the
 *     lower portion of the window)
 *
 * Capture flow:
 *   1. Optionally wait 3 seconds (SYS_GET_TICKS_MS polling).
 *   2. Call SYS_FB_ACQUIRE(39) → fills fb_acquire_t {vaddr,w,h,pitch,bpp}.
 *   3. Open /tmp/shotN.ppm (O_WRONLY|O_CREAT|O_TRUNC, 0644).
 *   4. Write binary PPM header: "P6\n<w> <h>\n255\n".
 *   5. For each pixel, read ARGB32/BGRA 4-byte word from framebuffer and
 *      emit 3 bytes (R, G, B) — byte order: framebuffer stores 0xAARRGGBB
 *      so byte[2]=R, byte[1]=G, byte[0]=B (little-endian u32).
 *   6. Close file.  Increment N.  Build thumbnail from captured data.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/screenshot/screenshot.c -o /tmp/screenshot.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/screenshot.o /tmp/wlc.o /tmp/bf.o -o /tmp/screenshot.elf
 *   objdump -d /tmp/screenshot.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline helper (no libc, no stack canary).
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_OPEN         4
#define SYS_CLOSE        5
#define SYS_YIELD        15
#define SYS_FB_ACQUIRE   39
#define SYS_GET_TICKS_MS 40

/* Open flags (matching kernel ABI in userspace/libc/syscall.h). */
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

static inline long sc(long n, long a1, long a2, long a3,
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

/* -----------------------------------------------------------------------
 * Framebuffer acquire info (must match kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef int                i32;

typedef struct {
    u64 vaddr;
    u32 width;
    u32 height;
    u32 pitch;  /* bytes per row */
    u32 bpp;    /* bits per pixel */
} fb_acquire_t;

/* -----------------------------------------------------------------------
 * Minimal freestanding helpers (no libc).
 * --------------------------------------------------------------------- */

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

/* Unsigned 32-bit integer to decimal string.
 * Writes into buf (must be at least 11 bytes), returns pointer to buf. */
static char *u32_to_str(u32 v, char *buf, int *out_len)
{
    char tmp[11];
    int  i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    /* reverse */
    int len = i;
    for (int j = 0; j < len; j++) buf[j] = tmp[len - 1 - j];
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Write a NUL-terminated string to an open fd. */
static void fd_write_str(int fd, const char *s)
{
    sc(SYS_WRITE, fd, (long)s, (long)k_strlen(s), 0, 0, 0);
}

/* Write raw bytes to an open fd. */
static void fd_write_bytes(int fd, const void *buf, long len)
{
    sc(SYS_WRITE, fd, (long)buf, len, 0, 0, 0);
}

/* k_memset (no libc). */
static void k_memset(void *dst, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
}

/* k_memcpy (no libc). */
static void k_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

/* -----------------------------------------------------------------------
 * Window / layout constants.
 * --------------------------------------------------------------------- */
#define WIN_W  420
#define WIN_H  240

/* Toolbar / control area at the top. */
#define CTRL_H   56

/* Thumbnail preview area below the control bar. */
#define THUMB_Y  (CTRL_H + 4)
#define THUMB_W  (WIN_W - 8)
#define THUMB_H  (WIN_H - CTRL_H - 8)

/* Capture button geometry. */
#define BTN_X    8
#define BTN_Y    8
#define BTN_W    140
#define BTN_H    26

/* Delay toggle button (Now / 3s). */
#define DLY_X    (BTN_X + BTN_W + 10)
#define DLY_Y    8
#define DLY_W    70
#define DLY_H    26

/* Status text row. */
#define STS_X    8
#define STS_Y    40

/* Colors. */
#define C_BG        0xFF1E1E2Eu   /* dark background                    */
#define C_CTRL_BG   0xFF2D2D3Fu   /* control bar background             */
#define C_CTRL_SEP  0xFF44445Au   /* separator line                     */
#define C_BTN       0xFF4A7FBBu   /* capture button face                */
#define C_BTN_HOV   0xFF5A8FCBu   /* hover                              */
#define C_BTN_CLK   0xFF2A5F9Bu   /* pressed                            */
#define C_DLY_OFF   0xFF3A3A50u   /* delay button inactive              */
#define C_DLY_ON    0xFF7A4ABBu   /* delay button active (3s selected)  */
#define C_BORDER    0xFF666688u   /* generic border                     */
#define C_TEXT      0xFFEEEEFFu   /* primary text                       */
#define C_TEXT_DIM  0xFF9999BBu   /* dimmed text                        */
#define C_THUMB_BG  0xFF111120u   /* thumbnail background               */
#define C_THUMB_BRD 0xFF444466u   /* thumbnail border                   */
#define C_STATUS_OK 0xFF55DD55u   /* success status text                */

/* -----------------------------------------------------------------------
 * Drawing primitives (same style as paint.c).
 * --------------------------------------------------------------------- */

static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = color;
    }
}

static void draw_border(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                        i32 x, i32 y, i32 w, i32 h, u32 color)
{
    fill_rect(buf, bw, bh, stride_px, x,         y,         w, 1, color);
    fill_rect(buf, bw, bh, stride_px, x,         y + h - 1, w, 1, color);
    fill_rect(buf, bw, bh, stride_px, x,         y,         1, h, color);
    fill_rect(buf, bw, bh, stride_px, x + w - 1, y,         1, h, color);
}

/* -----------------------------------------------------------------------
 * Filename construction.
 * shot_filename[] is a zeroed static buffer; we fill it each time.
 * Max filename "/tmp/shot4294967295.ppm" = 24 chars + NUL → 32 bytes safe.
 * --------------------------------------------------------------------- */
static char shot_filename[32];

static void build_filename(u32 n)
{
    k_memset(shot_filename, 0, sizeof(shot_filename));
    char nbuf[12];
    int  nlen;
    u32_to_str(n, nbuf, &nlen);

    const char *prefix = "/tmp/shot";
    const char *suffix = ".ppm";
    unsigned long pi = 0, si = 0, ni = 0, di = 0;

    while (prefix[pi]) shot_filename[di++] = prefix[pi++];
    for (ni = 0; ni < (unsigned long)nlen; ni++) shot_filename[di++] = nbuf[ni];
    while (suffix[si]) shot_filename[di++] = suffix[si++];
    shot_filename[di] = '\0';
}

/* -----------------------------------------------------------------------
 * Status line text buffer (shown after successful capture).
 * "Saved /tmp/shotN.ppm (WWWWxHHHH)"
 * --------------------------------------------------------------------- */
static char status_buf[64];
static int  status_ok = 0;   /* 0 = idle, 1 = success, -1 = error */

/* -----------------------------------------------------------------------
 * Thumbnail storage.
 * We keep a small ARGB32 copy of the downscaled framebuffer here.
 * Max thumbnail area = 412 x 180 pixels = ~296 KB.  We keep a fixed-size
 * static buffer at maximum size so no heap is needed.
 * --------------------------------------------------------------------- */
#define THUMB_MAX_W  412
#define THUMB_MAX_H  180

static u32 thumb_pixels[THUMB_MAX_W * THUMB_MAX_H];
static u32 thumb_actual_w = 0;
static u32 thumb_actual_h = 0;

/* Downscale src (fbw x fbh, pitch bytes/row, ARGB32) into
 * dst (dw x dh) using nearest-neighbour sampling. */
static void downscale_fb(const u8 *src_bytes, u32 fbw, u32 fbh, u32 pitch,
                         u32 *dst, u32 dw, u32 dh)
{
    for (u32 dy = 0; dy < dh; dy++) {
        u32 sy = dy * fbh / dh;
        for (u32 dx = 0; dx < dw; dx++) {
            u32 sx = dx * fbw / dw;
            /* Each pixel is 4 bytes (ARGB32 / BGRA little-endian). */
            const u8 *p = src_bytes + sy * pitch + sx * 4u;
            /* Reconstruct as opaque ARGB32 — keep the bytes as-is,
             * set alpha to 0xFF so the thumbnail renders correctly. */
            u32 pixel = ((u32)p[3] << 24) |
                        ((u32)p[2] << 16) |
                        ((u32)p[1] <<  8) |
                        ((u32)p[0]);
            pixel |= 0xFF000000u;  /* force opaque */
            dst[dy * dw + dx] = pixel;
        }
    }
}

/* -----------------------------------------------------------------------
 * PPM write helper.
 * Uses a small stack-local write buffer to batch single-row emissions so
 * we don't need heap.  Writes in chunks of CHUNK_PX pixels (3 bytes each).
 * --------------------------------------------------------------------- */
#define CHUNK_PX 256   /* 256 pixels = 768 bytes per write call */

static void write_ppm(int fd, const u8 *fb_bytes, u32 fbw, u32 fbh, u32 pitch)
{
    /* --- Header --- */
    /* "P6\n" */
    fd_write_str(fd, "P6\n");

    /* width */
    char tmp[12];
    int  tlen;
    u32_to_str(fbw, tmp, &tlen);
    fd_write_bytes(fd, tmp, tlen);
    fd_write_str(fd, " ");
    u32_to_str(fbh, tmp, &tlen);
    fd_write_bytes(fd, tmp, tlen);
    fd_write_str(fd, "\n255\n");

    /* --- Pixel data: ARGB32 → RGB24 --- */
    /* The framebuffer stores pixels as 32-bit little-endian words:
     *   byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = A  (BGRA / ARGB32).
     * PPM P6 wants: R G B per pixel.
     */
    u8 chunk[CHUNK_PX * 3];

    for (u32 row = 0; row < fbh; row++) {
        const u8 *src_row = fb_bytes + row * pitch;
        u32 col = 0;
        while (col < fbw) {
            u32 n = fbw - col;
            if (n > CHUNK_PX) n = CHUNK_PX;
            for (u32 i = 0; i < n; i++) {
                const u8 *px = src_row + (col + i) * 4u;
                chunk[i * 3 + 0] = px[2]; /* R */
                chunk[i * 3 + 1] = px[1]; /* G */
                chunk[i * 3 + 2] = px[0]; /* B */
            }
            fd_write_bytes(fd, chunk, (long)(n * 3));
            col += n;
        }
    }
}

/* -----------------------------------------------------------------------
 * String helpers for status message construction (no sprintf).
 * --------------------------------------------------------------------- */
static void build_status(u32 shot_n, u32 fw, u32 fh)
{
    k_memset(status_buf, 0, sizeof(status_buf));
    unsigned long i = 0;

    /* "Saved " */
    const char *s1 = "Saved ";
    for (unsigned long j = 0; s1[j]; j++) status_buf[i++] = s1[j];

    /* filename already in shot_filename */
    for (unsigned long j = 0; shot_filename[j]; j++) status_buf[i++] = shot_filename[j];

    /* " (" */
    status_buf[i++] = ' ';
    status_buf[i++] = '(';

    /* width */
    char tmp[12]; int tlen;
    u32_to_str(fw, tmp, &tlen);
    for (int j = 0; j < tlen; j++) status_buf[i++] = tmp[j];
    status_buf[i++] = 'x';
    u32_to_str(fh, tmp, &tlen);
    for (int j = 0; j < tlen; j++) status_buf[i++] = tmp[j];

    /* ")" */
    status_buf[i++] = ')';
    status_buf[i] = '\0';

    (void)shot_n;
}

/* -----------------------------------------------------------------------
 * Delay countdown helper — poll ticks, yield each iteration.
 * --------------------------------------------------------------------- */
static void delay_ms(long ms)
{
    long start = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    long end   = start + ms;
    while (sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0) < end)
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Capture routine: acquires FB, writes PPM, builds thumbnail.
 * --------------------------------------------------------------------- */
static u32 shot_counter = 0;   /* incremented each successful capture */

static void do_capture(void)
{
    /* ---- 1. SYS_FB_ACQUIRE ---- */
    fb_acquire_t fb;
    k_memset(&fb, 0, sizeof(fb));
    long ret = sc(SYS_FB_ACQUIRE, (long)&fb, 0, 0, 0, 0, 0);
    if (ret < 0 || fb.vaddr == 0 || fb.width == 0 || fb.height == 0) {
        status_ok = -1;
        const char *err = "Error: FB_ACQUIRE failed";
        k_memset(status_buf, 0, sizeof(status_buf));
        for (int i = 0; err[i]; i++) status_buf[i] = err[i];
        print("[SCREENSHOT] FB_ACQUIRE failed\n");
        return;
    }

    print("[SCREENSHOT] FB acquired\n");

    /* ---- 2. Build filename ---- */
    shot_counter++;
    build_filename(shot_counter);

    /* ---- 3. Open file ---- */
    int fd = (int)sc(SYS_OPEN, (long)shot_filename,
                     O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) {
        status_ok = -1;
        const char *err = "Error: cannot open file";
        k_memset(status_buf, 0, sizeof(status_buf));
        for (int i = 0; err[i]; i++) status_buf[i] = err[i];
        print("[SCREENSHOT] open failed\n");
        return;
    }

    /* ---- 4+5. Write PPM ---- */
    const u8 *fb_bytes = (const u8 *)fb.vaddr;
    write_ppm(fd, fb_bytes, fb.width, fb.height, fb.pitch);

    /* ---- 6. Close ---- */
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

    /* ---- Build thumbnail ---- */
    u32 tw = THUMB_MAX_W, th = THUMB_MAX_H;
    if (tw > fb.width)  tw = fb.width;
    if (th > fb.height) th = fb.height;
    /* Further constrain to THUMB_W x THUMB_H display area. */
    if (tw > (u32)THUMB_W) tw = (u32)THUMB_W;
    if (th > (u32)THUMB_H) th = (u32)THUMB_H;
    downscale_fb(fb_bytes, fb.width, fb.height, fb.pitch,
                 thumb_pixels, tw, th);
    thumb_actual_w = tw;
    thumb_actual_h = th;

    /* ---- Build status string ---- */
    build_status(shot_counter, fb.width, fb.height);
    status_ok = 1;

    print("[SCREENSHOT] saved ");
    print(shot_filename);
    print("\n");
}

/* -----------------------------------------------------------------------
 * UI render.
 * --------------------------------------------------------------------- */
static void render(wl_window *win, int btn_state, int delay_3s,
                   int counting_down, long countdown_end)
{
    u32 *buf       = win->pixels;
    u32  bw        = win->w;
    u32  bh        = win->h;
    u32  stride_px = win->stride / 4u;

    /* ---- Background ---- */
    fill_rect(buf, bw, bh, stride_px, 0, 0, (i32)bw, (i32)bh, C_BG);

    /* ---- Control bar background ---- */
    fill_rect(buf, bw, bh, stride_px, 0, 0, (i32)bw, CTRL_H, C_CTRL_BG);
    fill_rect(buf, bw, bh, stride_px, 0, CTRL_H - 1, (i32)bw, 1, C_CTRL_SEP);

    /* ---- Capture button ---- */
    u32 btn_col = (btn_state == 1) ? C_BTN_HOV :
                  (btn_state == 2) ? C_BTN_CLK : C_BTN;
    fill_rect(buf, bw, bh, stride_px, BTN_X, BTN_Y, BTN_W, BTN_H, btn_col);
    draw_border(buf, bw, bh, stride_px, BTN_X, BTN_Y, BTN_W, BTN_H, C_BORDER);

    if (counting_down) {
        /* Show "Capturing..." */
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         BTN_X + 14, BTN_Y + 5, "Capturing...", C_TEXT);
        (void)countdown_end;
    } else {
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         BTN_X + 10, BTN_Y + 5, "Capture Screen", C_TEXT);
    }

    /* ---- Delay toggle button ---- */
    u32 dly_col = delay_3s ? C_DLY_ON : C_DLY_OFF;
    fill_rect(buf, bw, bh, stride_px, DLY_X, DLY_Y, DLY_W, DLY_H, dly_col);
    draw_border(buf, bw, bh, stride_px, DLY_X, DLY_Y, DLY_W, DLY_H, C_BORDER);
    font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                     DLY_X + 8, DLY_Y + 5,
                     delay_3s ? "Delay 3s" : "Delay:Now",
                     C_TEXT);

    /* ---- Status line ---- */
    if (status_ok == 1) {
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         STS_X, STS_Y, status_buf, C_STATUS_OK);
    } else if (status_ok == -1) {
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         STS_X, STS_Y, status_buf, 0xFFFF4444u);
    } else {
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         STS_X, STS_Y, "Press 'Capture Screen' to take a screenshot",
                         C_TEXT_DIM);
    }

    /* ---- Thumbnail area ---- */
    fill_rect(buf, bw, bh, stride_px,
              4, THUMB_Y, (i32)bw - 8, THUMB_H, C_THUMB_BG);
    draw_border(buf, bw, bh, stride_px,
                4, THUMB_Y, (i32)bw - 8, THUMB_H, C_THUMB_BRD);

    if (thumb_actual_w > 0 && thumb_actual_h > 0) {
        /* Center thumbnail in the preview box. */
        i32 tx = 4 + ((i32)(bw - 8) - (i32)thumb_actual_w) / 2;
        i32 ty = THUMB_Y + (THUMB_H - (i32)thumb_actual_h) / 2;
        for (u32 py = 0; py < thumb_actual_h; py++) {
            i32 dst_y = ty + (i32)py;
            if (dst_y < 0 || dst_y >= (i32)bh) continue;
            u32 *dst_row = buf + (u32)dst_y * stride_px;
            u32 *src_row = thumb_pixels + py * thumb_actual_w;
            for (u32 px2 = 0; px2 < thumb_actual_w; px2++) {
                i32 dst_x = tx + (i32)px2;
                if (dst_x < 0 || dst_x >= (i32)bw) continue;
                dst_row[dst_x] = src_row[px2];
            }
        }
        /* "Preview:" label above thumbnail. */
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         8, THUMB_Y - FONT_H - 1, "Preview:", C_TEXT_DIM);
    } else {
        /* Placeholder text. */
        const char *ph = "No capture yet";
        i32 ph_x = 4 + ((i32)(bw - 8) - font_text_width(ph)) / 2;
        i32 ph_y = THUMB_Y + (THUMB_H - FONT_H) / 2;
        font_draw_string(buf, (int)stride_px, (int)bw, (int)bh,
                         ph_x, ph_y, ph, C_TEXT_DIM);
    }
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    print("[SCREENSHOT] starting\n");

    if (wl_connect() != 0) {
        print("[SCREENSHOT] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Screenshot");
    if (!win) {
        print("[SCREENSHOT] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[SCREENSHOT] window created\n");

    /* ---- State ---- */
    int  btn_state    = 0;   /* 0=normal,1=hover,2=pressed */
    int  delay_3s     = 0;   /* 0=Now, 1=3-second delay   */
    int  counting_down = 0;
    long countdown_end = 0;

    /* ---- Frame loop ---- */
    for (;;) {
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea;
                i32 my = (i32)eb;
                int lbtn = ec & 1;

                /* Hit-test capture button. */
                int over_btn = (mx >= BTN_X && mx < BTN_X + BTN_W &&
                                my >= BTN_Y && my < BTN_Y + BTN_H);
                /* Hit-test delay toggle. */
                int over_dly = (mx >= DLY_X && mx < DLY_X + DLY_W &&
                                my >= DLY_Y && my < DLY_Y + DLY_H);

                if (lbtn) {
                    if (over_btn && !counting_down) {
                        btn_state = 2;
                        if (delay_3s) {
                            counting_down = 1;
                            countdown_end = sc(SYS_GET_TICKS_MS,
                                               0, 0, 0, 0, 0, 0) + 3000;
                        } else {
                            /* Capture immediately on release (set flag). */
                            /* We capture below after button-up to let the
                             * frame render "pressed" state once first.    */
                            counting_down = 2; /* sentinel: capture next tick */
                            countdown_end = 0;
                        }
                    } else if (over_dly) {
                        delay_3s = !delay_3s;
                    }
                } else {
                    if (btn_state == 2)
                        btn_state = over_btn ? 1 : 0;
                    else
                        btn_state = over_btn ? 1 : 0;
                }
            }
            /* Key events unused. */
        }

        /* ---- Handle countdown / immediate capture ---- */
        if (counting_down == 1) {
            /* 3-second countdown: check if elapsed. */
            long now = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            if (now >= countdown_end) {
                counting_down = 0;
                btn_state = 0;
                do_capture();
            }
        } else if (counting_down == 2) {
            /* Immediate: do capture this frame (after one rendered frame). */
            counting_down = 0;
            btn_state = 0;
            do_capture();
        }

        /* ---- Render ---- */
        render(win, btn_state, delay_3s, counting_down == 1, countdown_end);

        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
