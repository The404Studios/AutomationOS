/*
 * photos.c -- "Photos" desktop app (ring 3, freestanding, NO libc).
 * =================================================================
 *
 * A Windows-11-Photos-style image viewer for AutomationOS:
 *
 *   - Main canvas showing the current image, fit-to-window (aspect preserved),
 *     centered, alpha-composited over a dark matte. Zoom (+/-) re-scales about
 *     the canvas center; the image pans automatically to stay centered.
 *   - A filmstrip / thumbnail strip along the bottom: one tile per image in
 *     the folder, nearest-neighbour downscaled, the current image highlighted
 *     with an accent ring. Click a thumbnail to open it.
 *   - Prev / Next navigation: Left / Right arrow keys AND on-screen rounded
 *     chevron buttons ( < ) and ( > ) floating over the canvas edges.
 *   - On-screen zoom pills ( - ) ( reset ) ( + ) in the top toolbar, plus the
 *     +/- keys and '0' to reset zoom.
 *   - Lists + opens images from a folder. It scans a list of candidate picture
 *     folders (/usr/share/pictures, /Pictures, /Desktop, /tmp ...) for files
 *     ending in .png or .bmp, decodes them on demand with the SHARED freestanding
 *     image codec (userspace/lib/imgcodec: img_decode -> 0xAARRGGBB), and shows
 *     them. If a file fails to decode or no images are found, a clean placeholder
 *     card is drawn (plus the filenames it discovered) so the app is always
 *     visibly alive.
 *
 * Modern dark chrome: rounded toolbar, rounded filmstrip dock, rounded buttons,
 * accent highlight ring. "Aether Dark"-ish palette to match the compositor.
 *
 * No libc startup beyond _start; this TU defines its own _start. All I/O is raw
 * AOS syscalls (NOT Linux numbers): SYS_WRITE=3, SYS_OPEN=4, SYS_READ=2,
 * SYS_CLOSE=5, SYS_OPENDIR=30, SYS_READDIR=31, SYS_CLOSEDIR=32, SYS_YIELD=15.
 * Diagnostics go to serial (fd 1) tagged "[PHOTOS]".
 *
 * Image decoding is REUSED from the shared codec -- we do NOT reimplement PNG/BMP.
 *   img_decode(data,len,out,out_cap,&w,&h)  (userspace/lib/imgcodec/imgcodec.h)
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and you fault with CR2=0x28). Mirror cc() in
 * scripts/build_all.sh:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/photos/photos.c -o /tmp/photos.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/photos.o /tmp/wlc.o /tmp/bf.o \
 *       /tmp/img_codec.o /tmp/img_png.o /tmp/img_bmp.o /tmp/img_gif.o \
 *       /tmp/deflate.o /tmp/lstring.o -o /tmp/photos.elf
 *   objdump -d /tmp/photos.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/imgcodec/imgcodec.h"   /* shared freestanding PNG/BMP/GIF decoder */

/* ------------------------------------------------------------------------- *
 *  Syscall numbers (AOS -- mirror kernel/include/syscall.h) + inline helper. *
 * ------------------------------------------------------------------------- */
#define SYS_READ      2
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_YIELD     15
#define SYS_OPENDIR   30
#define SYS_READDIR   31
#define SYS_CLOSEDIR  32

#define O_RDONLY 0

typedef unsigned int       u32;
typedef int                i32;
typedef unsigned char      u8;
typedef unsigned long      ulong;
typedef unsigned long long u64;

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

/* ---- freestanding string + serial helpers ---- */
static ulong s_len(const char *s) { ulong n = 0; while (s && s[n]) n++; return n; }
static void  print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)s_len(m), 0, 0, 0); }
static void  print_num(long v) {
    char b[24]; int i = 0;
    if (v < 0) { print("-"); v = -v; }
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}
static void s_cpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void s_cat(char *dst, const char *src, int cap) {
    int n = 0; while (dst[n]) n++;
    int i = 0; while (n + i < cap - 1 && src[i]) { dst[n + i] = src[i]; i++; }
    dst[n + i] = '\0';
}
static int s_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* dirent (must match kernel/include/vfs.h). */
#define DT_DIR   4
#define DT_REG   8
#define NAME_MAX 256
struct dirent {
    u64            d_ino;
    long long      d_off;
    unsigned short d_reclen;
    u8             d_type;
    char           d_name[NAME_MAX];
};

/* ------------------------------------------------------------------------- *
 *  Window geometry + Aether-Dark palette.                                    *
 * ------------------------------------------------------------------------- */
#define WIN_W        900
#define WIN_H        640

#define TOOLBAR_H    48               /* top chrome bar                         */
#define STRIP_H      96               /* filmstrip dock at the bottom           */
#define THUMB_W      108
#define THUMB_H      64
#define THUMB_GAP    12

#define COL_BG       0xFF1C1C1Eu      /* window background                      */
#define COL_CANVAS   0xFF121214u      /* darker matte behind the image          */
#define COL_TOOLBAR  0xFF2C2C2Eu      /* toolbar / strip panel                  */
#define COL_PANEL2   0xFF252527u      /* slightly darker panel fill             */
#define COL_HOVER    0xFF3A3A3Cu      /* hovered control                        */
#define COL_BTN      0xFF38383Au      /* idle button fill                       */
#define COL_BORDER   0xFF454547u      /* hairline border                        */
#define COL_TEXT     0xFFFFFFFFu      /* primary text                           */
#define COL_DIM      0xFFAEAEB2u      /* secondary text                         */
#define COL_ACCENT   0xFF0A84FFu      /* selection / accent (system blue)       */
#define COL_AMBER    0xFFF1C40Fu      /* placeholder highlight                  */

/* Picture folders scanned in order; first that opens + has images wins, but we
 * accumulate from ALL of them so images scattered across folders show up too. */
static const char *const PIC_DIRS[] = {
    "/usr/share/pictures",
    "/usr/share/wallpapers",
    "/usr/share/images",
    "/Pictures",
    "/Desktop",
    "/tmp",
    "/tmp/shots",
    0
};

/* ------------------------------------------------------------------------- *
 *  Image catalogue. We keep paths; pixels for the CURRENT image are decoded  *
 *  on demand into g_pix. Thumbnails are cached lazily into g_thumbs.         *
 * ------------------------------------------------------------------------- */
#define MAX_IMAGES   64
#define PATH_LEN     320

/* Full-size decode target: one image at a time. 4096*4096 ARGB would be huge;
 * the codec caps individual PNG dimensions, so 2048x2048 (16 MB) is a safe,
 * generous ceiling and matches malloc-big-buffers guidance. */
#define IMG_MAX_W    2048
#define IMG_MAX_H    2048
#define IMG_MAX_PX   ((ulong)IMG_MAX_W * (ulong)IMG_MAX_H)

static char  g_paths[MAX_IMAGES][PATH_LEN];
static char  g_names[MAX_IMAGES][NAME_MAX];
static int   g_count   = 0;

/* Current full image (decoded) -- big BSS buffer. */
static u32   g_pix[IMG_MAX_PX];
static int   g_cur_w = 0, g_cur_h = 0;
static int   g_cur   = -1;            /* index of decoded image, -1 = none/failed */

/* File-read scratch for raw encoded bytes (PNG/BMP). 8 MiB ceiling. */
#define FILE_CAP  (8u * 1024u * 1024u)
static u8    g_file[FILE_CAP];

/* Thumbnail cache: one THUMB_W*THUMB_H ARGB tile per image, lazily filled. */
static u32   g_thumbs[MAX_IMAGES][THUMB_W * THUMB_H];
static u8    g_thumb_ready[MAX_IMAGES];

/* View state. */
static int   g_zoom_num = 1, g_zoom_den = 1;   /* zoom factor = num/den (>=1 fit) */
static int   g_strip_off = 0;                  /* horizontal scroll of filmstrip  */

/* ------------------------------------------------------------------------- *
 *  Drawing primitives into the ARGB32 window buffer.                         *
 * ------------------------------------------------------------------------- */
static inline void px_put(u32 *buf, u32 stride, i32 bw, i32 bh, i32 x, i32 y, u32 c) {
    if (x < 0 || y < 0 || x >= bw || y >= bh) return;
    buf[(u32)y * stride + (u32)x] = c;
}

static void fill_rect(u32 *buf, u32 stride, i32 bw, i32 bh,
                      i32 x, i32 y, i32 w, i32 h, u32 color) {
    i32 x1 = x < 0 ? 0 : x, y1 = y < 0 ? 0 : y;
    i32 x2 = x + w, y2 = y + h;
    if (x2 > bw) x2 = bw;
    if (y2 > bh) y2 = bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/* Rounded-rect fill (solid). Corners use a circle test per corner cell. */
static void fill_round(u32 *buf, u32 stride, i32 bw, i32 bh,
                       i32 x, i32 y, i32 w, i32 h, i32 r, u32 color) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    for (i32 yy = 0; yy < h; yy++) {
        for (i32 xx = 0; xx < w; xx++) {
            i32 cx = -1, cy = -1;
            if (xx < r && yy < r)             { cx = r;         cy = r;         }
            else if (xx >= w - r && yy < r)   { cx = w - r - 1; cy = r;         }
            else if (xx < r && yy >= h - r)   { cx = r;         cy = h - r - 1; }
            else if (xx >= w - r && yy >= h-r){ cx = w - r - 1; cy = h - r - 1; }
            if (cx >= 0) {
                i32 dx = xx - cx, dy = yy - cy;
                if (dx * dx + dy * dy > r * r) continue;   /* outside corner   */
            }
            px_put(buf, stride, bw, bh, x + xx, y + yy, color);
        }
    }
}

/* 1px rounded-rect outline (drawn as a ring by subtracting an inner fill). */
static void stroke_round(u32 *buf, u32 stride, i32 bw, i32 bh,
                         i32 x, i32 y, i32 w, i32 h, i32 r, u32 color) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (i32 yy = 0; yy < h; yy++) {
        for (i32 xx = 0; xx < w; xx++) {
            i32 cx = -1, cy = -1;
            if (xx < r && yy < r)             { cx = r;         cy = r;         }
            else if (xx >= w - r && yy < r)   { cx = w - r - 1; cy = r;         }
            else if (xx < r && yy >= h - r)   { cx = r;         cy = h - r - 1; }
            else if (xx >= w - r && yy >= h-r){ cx = w - r - 1; cy = h - r - 1; }
            int inside, edge;
            if (cx >= 0) {
                i32 dx = xx - cx, dy = yy - cy, d2 = dx * dx + dy * dy;
                inside = d2 <= r * r;
                edge   = inside && d2 > (r - 1) * (r - 1);
            } else {
                inside = 1;
                edge   = (xx == 0 || xx == w - 1 || yy == 0 || yy == h - 1);
            }
            if (inside && edge) px_put(buf, stride, bw, bh, x + xx, y + yy, color);
        }
    }
}

/* Alpha-over one ARGB source pixel onto a destination pixel value. */
static inline u32 over(u32 dst, u32 src) {
    u32 a = (src >> 24) & 0xFF;
    if (a == 0xFF) return (src & 0x00FFFFFFu) | 0xFF000000u;
    if (a == 0)    return dst;
    u32 sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    u32 dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    u32 r = (sr * a + dr * (255 - a)) / 255;
    u32 g = (sg * a + dg * (255 - a)) / 255;
    u32 b = (sb * a + db * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void text(u32 *buf, u32 stride, i32 bw, i32 bh, i32 x, i32 y,
                 const char *s, u32 color) {
    font_draw_string(buf, (int)stride, bw, bh, x, y, s, color);
}
static void text_center(u32 *buf, u32 stride, i32 bw, i32 bh, i32 cx, i32 y,
                        const char *s, u32 color) {
    int tw = font_text_width(s);
    font_draw_string(buf, (int)stride, bw, bh, cx - tw / 2, y, s, color);
}

/* ------------------------------------------------------------------------- *
 *  Catalogue building: scan picture dirs for *.png / *.bmp.                   *
 * ------------------------------------------------------------------------- */
static int ext_is_image(const char *name) {
    ulong n = s_len(name);
    if (n < 4) return 0;
    const char *t = name + n - 4;
    /* lower-case the candidate extension chars for comparison */
    char e[5];
    for (int i = 0; i < 4; i++) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        e[i] = c;
    }
    e[4] = '\0';
    return s_eq(e, ".png") || s_eq(e, ".bmp");
}

static void path_join(char *dst, int cap, const char *dir, const char *name) {
    s_cpy(dst, dir, cap);
    int n = 0; while (dst[n]) n++;
    if (n == 0 || dst[n - 1] != '/') s_cat(dst, "/", cap);
    s_cat(dst, name, cap);
}

static int already_have(const char *path) {
    for (int i = 0; i < g_count; i++) if (s_eq(g_paths[i], path)) return 1;
    return 0;
}

static void scan_dir(const char *dir) {
    long fd = sc(SYS_OPENDIR, (long)dir, 0, 0, 0, 0, 0);
    if (fd < 0) return;
    struct dirent ent;
    while (g_count < MAX_IMAGES) {
        long r = sc(SYS_READDIR, fd, (long)&ent, 0, 0, 0, 0);
        if (r <= 0) break;
        if (ent.d_type == DT_DIR) continue;
        if (ent.d_name[0] == '.') continue;
        if (!ext_is_image(ent.d_name)) continue;

        char full[PATH_LEN];
        path_join(full, PATH_LEN, dir, ent.d_name);
        if (already_have(full)) continue;

        s_cpy(g_paths[g_count], full, PATH_LEN);
        s_cpy(g_names[g_count], ent.d_name, NAME_MAX);
        g_count++;
    }
    sc(SYS_CLOSEDIR, fd, 0, 0, 0, 0, 0);
}

static void build_catalogue(void) {
    g_count = 0;
    for (int i = 0; PIC_DIRS[i] && g_count < MAX_IMAGES; i++) scan_dir(PIC_DIRS[i]);
    print("[PHOTOS] catalogued ");
    print_num(g_count);
    print(" image(s)\n");
}

/* ------------------------------------------------------------------------- *
 *  File load + decode (REUSES shared imgcodec, no PNG/BMP reimplementation). *
 * ------------------------------------------------------------------------- */
static long read_whole(const char *path, u8 *dst, ulong cap) {
    int fd = (int)sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;
    ulong got = 0;
    for (;;) {
        if (got >= cap) break;                 /* truncate huge files          */
        long n = sc(SYS_READ, fd, (long)(dst + got), (long)(cap - got), 0, 0, 0);
        if (n <= 0) break;
        got += (ulong)n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return (long)got;
}

/* Decode image at catalogue index `idx` into g_pix. Returns 1 on success. */
static int decode_index(int idx) {
    if (idx < 0 || idx >= g_count) return 0;
    long len = read_whole(g_paths[idx], g_file, FILE_CAP);
    if (len <= 0) {
        print("[PHOTOS] read failed: "); print(g_paths[idx]); print("\n");
        return 0;
    }
    int w = 0, h = 0;
    int rc = img_decode(g_file, (ulong)len, g_pix, IMG_MAX_PX, &w, &h);
    if (rc != IMG_OK || w <= 0 || h <= 0) {
        print("[PHOTOS] decode failed ("); print_num(rc);
        print("): "); print(g_paths[idx]); print("\n");
        return 0;
    }
    g_cur_w = w; g_cur_h = h; g_cur = idx;
    print("[PHOTOS] decoded "); print(g_names[idx]); print(" ");
    print_num(w); print("x"); print_num(h); print("\n");
    return 1;
}

/* Build the thumbnail tile for image `idx` (decodes it fresh into g_pix as a
 * side effect, then leaves g_cur pointing at it). Nearest-neighbour downscale,
 * letterboxed inside THUMB_W x THUMB_H over COL_PANEL2. */
static void build_thumb(int idx) {
    if (idx < 0 || idx >= g_count || g_thumb_ready[idx]) return;
    u32 *tile = g_thumbs[idx];
    for (int i = 0; i < THUMB_W * THUMB_H; i++) tile[i] = COL_PANEL2;

    if (!decode_index(idx)) { g_thumb_ready[idx] = 1; return; }  /* leave panel */

    int iw = g_cur_w, ih = g_cur_h;
    /* scaled size keeping aspect, fit inside thumb box */
    int dw = THUMB_W, dh = (int)((u64)dw * ih / (iw ? iw : 1));
    if (dh > THUMB_H) { dh = THUMB_H; dw = (int)((u64)dh * iw / (ih ? ih : 1)); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (THUMB_W - dw) / 2, oy = (THUMB_H - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((u64)y * ih / dh);
        if (sy >= ih) sy = ih - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((u64)x * iw / dw);
            if (sx >= iw) sx = iw - 1;
            u32 sp = g_pix[(u32)sy * iw + sx];
            tile[(oy + y) * THUMB_W + (ox + x)] = over(COL_PANEL2, sp);
        }
    }
    g_thumb_ready[idx] = 1;
}

/* ------------------------------------------------------------------------- *
 *  Canvas render: current image fit-to-window, scaled by the zoom factor.    *
 * ------------------------------------------------------------------------- */
static void draw_canvas(u32 *buf, u32 stride, i32 bw, i32 bh) {
    i32 cx0 = 0, cy0 = TOOLBAR_H;
    i32 cw = bw, ch = bh - TOOLBAR_H - STRIP_H;
    if (ch < 1) ch = 1;

    /* matte */
    fill_rect(buf, stride, bw, bh, cx0, cy0, cw, ch, COL_CANVAS);

    if (g_cur < 0 || g_cur_w <= 0 || g_cur_h <= 0) {
        /* placeholder card */
        i32 pw = 360, ph = 150;
        i32 px = cx0 + (cw - pw) / 2, py = cy0 + (ch - ph) / 2;
        fill_round(buf, stride, bw, bh, px, py, pw, ph, 16, COL_PANEL2);
        stroke_round(buf, stride, bw, bh, px, py, pw, ph, 16, COL_BORDER);
        text_center(buf, stride, bw, bh, cx0 + cw / 2, py + 30,
                    g_count > 0 ? "Couldn't decode this picture" : "No pictures found",
                    COL_AMBER);
        if (g_count > 0) {
            text_center(buf, stride, bw, bh, cx0 + cw / 2, py + 56,
                        "Use the filmstrip below to pick another", COL_DIM);
        } else {
            text_center(buf, stride, bw, bh, cx0 + cw / 2, py + 56,
                        "Add .png / .bmp to /usr/share/pictures", COL_DIM);
        }
        return;
    }

    /* Fit-to-window scale (never below fit), then multiply by zoom = num/den. */
    int iw = g_cur_w, ih = g_cur_h;
    /* fit: dst = floor(min(cw/iw, ch/ih) * dim) computed in integer space */
    u64 fit_w = (u64)cw * ih;     /* compare cw/iw vs ch/ih  via cross-mult     */
    u64 fit_h = (u64)ch * iw;
    int dw, dh;
    if (fit_w < fit_h) { dw = cw;                       dh = (int)((u64)cw * ih / iw); }
    else               { dh = ch;                       dw = (int)((u64)ch * iw / ih); }
    /* apply zoom */
    dw = (int)((u64)dw * g_zoom_num / g_zoom_den);
    dh = (int)((u64)dh * g_zoom_num / g_zoom_den);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    int ox = cx0 + (cw - dw) / 2;
    int oy = cy0 + (ch - dh) / 2;

    /* Clip to the canvas region; iterate destination pixels. */
    int y0 = oy < cy0 ? cy0 : oy;
    int y1 = oy + dh; if (y1 > cy0 + ch) y1 = cy0 + ch;
    int x0 = ox < cx0 ? cx0 : ox;
    int x1 = ox + dw; if (x1 > cx0 + cw) x1 = cx0 + cw;

    for (int y = y0; y < y1; y++) {
        int sy = (int)((u64)(y - oy) * ih / dh);
        if (sy < 0) sy = 0;
        if (sy >= ih) sy = ih - 1;
        u32 *row = buf + (u32)y * stride;
        const u32 *srow = g_pix + (u32)sy * iw;
        for (int x = x0; x < x1; x++) {
            int sx = (int)((u64)(x - ox) * iw / dw);
            if (sx < 0) sx = 0;
            if (sx >= iw) sx = iw - 1;
            row[x] = over(COL_CANVAS, srow[sx]);
        }
    }
}

/* ------------------------------------------------------------------------- *
 *  Hit-testable controls. We recompute rectangles every frame from the live  *
 *  window size and test the cursor / clicks against them.                    *
 * ------------------------------------------------------------------------- */
typedef struct { i32 x, y, w, h; } rect;
static int hit(rect r, i32 px, i32 py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

/* Toolbar zoom-pill geometry (right-aligned trio). */
static rect rc_zoom_out(i32 bw)  { (void)bw; rect r = { bw - 150, 10, 28, 28 }; return r; }
static rect rc_zoom_rst(i32 bw)  { rect r = { bw - 116, 10, 44, 28 }; return r; }
static rect rc_zoom_in(i32 bw)   { rect r = { bw - 66, 10, 28, 28 }; return r; }

/* Chevron nav buttons, vertically centered over the canvas. */
static rect rc_prev(i32 bw, i32 bh) {
    (void)bw; i32 cy = TOOLBAR_H + (bh - TOOLBAR_H - STRIP_H) / 2;
    rect r = { 14, cy - 24, 40, 48 }; return r;
}
static rect rc_next(i32 bw, i32 bh) {
    i32 cy = TOOLBAR_H + (bh - TOOLBAR_H - STRIP_H) / 2;
    rect r = { bw - 54, cy - 24, 40, 48 }; return r;
}

/* Filmstrip tile rect for image i, given current scroll offset. */
static rect rc_thumb(i32 bw, i32 bh, int i) {
    (void)bw;
    i32 sy = bh - STRIP_H;
    i32 ty = sy + (STRIP_H - THUMB_H) / 2;
    i32 tx = THUMB_GAP + i * (THUMB_W + THUMB_GAP) - g_strip_off;
    rect r = { tx, ty, THUMB_W, THUMB_H };
    return r;
}

static void draw_button(u32 *buf, u32 stride, i32 bw, i32 bh, rect r,
                        const char *label, int hot, u32 fill_hot, u32 fill_idle) {
    fill_round(buf, stride, bw, bh, r.x, r.y, r.w, r.h, 9, hot ? fill_hot : fill_idle);
    stroke_round(buf, stride, bw, bh, r.x, r.y, r.w, r.h, 9, COL_BORDER);
    int ty = r.y + (r.h - FONT_H) / 2;
    text_center(buf, stride, bw, bh, r.x + r.w / 2, ty, label, COL_TEXT);
}

/* ------------------------------------------------------------------------- *
 *  Navigation + zoom actions.                                                *
 * ------------------------------------------------------------------------- */
static void ensure_strip_visible(i32 bw) {
    if (g_cur < 0) return;
    int tx = THUMB_GAP + g_cur * (THUMB_W + THUMB_GAP);
    int left = g_strip_off, right = g_strip_off + bw;
    if (tx < left + THUMB_GAP)           g_strip_off = tx - THUMB_GAP;
    else if (tx + THUMB_W > right - THUMB_GAP)
        g_strip_off = tx + THUMB_W + THUMB_GAP - bw;
    int total = THUMB_GAP + g_count * (THUMB_W + THUMB_GAP);
    int max_off = total - bw; if (max_off < 0) max_off = 0;
    if (g_strip_off > max_off) g_strip_off = max_off;
    if (g_strip_off < 0) g_strip_off = 0;
}

static void open_image(int idx, i32 bw) {
    if (idx < 0 || idx >= g_count) return;
    g_zoom_num = 1; g_zoom_den = 1;
    if (!decode_index(idx)) {
        /* Keep g_cur pointing at the selection (so the toolbar/filmstrip still
         * track it) but zero the dimensions so draw_canvas() shows the
         * "couldn't decode" placeholder instead of stale pixels. */
        g_cur = idx; g_cur_w = 0; g_cur_h = 0;
    }
    ensure_strip_visible(bw);
}

static void nav_next(i32 bw) {
    if (g_count == 0) return;
    int n = (g_cur < 0 ? 0 : (g_cur + 1) % g_count);
    open_image(n, bw);
}
static void nav_prev(i32 bw) {
    if (g_count == 0) return;
    int p = (g_cur < 0 ? 0 : (g_cur - 1 + g_count) % g_count);
    open_image(p, bw);
}
static void zoom_in(void)  { if (g_zoom_num < 8 * g_zoom_den) { g_zoom_num = g_zoom_num * 5 / 4 + 1; } }
static void zoom_out(void) { if (g_zoom_num > g_zoom_den)     { g_zoom_num = g_zoom_num * 4 / 5;
                                                                if (g_zoom_num < g_zoom_den) g_zoom_num = g_zoom_den; } }
static void zoom_reset(void) { g_zoom_num = 1; g_zoom_den = 1; }

/* ------------------------------------------------------------------------- *
 *  Keycodes. The compositor forwards raw keycodes via WL_EVENT_KEY. We map a *
 *  small set: arrows for prev/next, +/-/0 for zoom. Both the US set-1 scancode*
 *  arrow values and ASCII fallbacks are accepted so this is robust to the    *
 *  exact keymap the compositor uses.                                         *
 * ------------------------------------------------------------------------- */
#define KEY_LEFT_SC   0x4B
#define KEY_RIGHT_SC  0x4D
#define KEY_PLUS_SC   0x4E   /* keypad +  */
#define KEY_MINUS_SC  0x4A   /* keypad -  */

static void handle_key(int code, i32 bw) {
    switch (code) {
        case KEY_LEFT_SC:  case 'a': case 'A':            nav_prev(bw); break;
        case KEY_RIGHT_SC: case 'd': case 'D': case ' ':  nav_next(bw); break;
        case KEY_PLUS_SC:  case '+': case '=':            zoom_in();    break;
        case KEY_MINUS_SC: case '-': case '_':            zoom_out();   break;
        case '0':                                         zoom_reset(); break;
        default: break;
    }
}

/* ------------------------------------------------------------------------- *
 *  Chrome render: toolbar (title + counter + zoom pills) and filmstrip.      *
 * ------------------------------------------------------------------------- */
static void fmt_int(char *dst, int cap, int v) {
    char tmp[16]; int i = 0;
    if (v == 0) { s_cpy(dst, "0", cap); return; }
    int neg = v < 0; if (neg) v = -v;
    while (v > 0 && i < 15) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; if (neg && j < cap - 1) dst[j++] = '-';
    while (i > 0 && j < cap - 1) dst[j++] = tmp[--i];
    dst[j] = '\0';
}

static void draw_toolbar(u32 *buf, u32 stride, i32 bw, i32 bh, i32 mx, i32 my) {
    fill_rect(buf, stride, bw, bh, 0, 0, bw, TOOLBAR_H, COL_TOOLBAR);
    fill_rect(buf, stride, bw, bh, 0, TOOLBAR_H - 1, bw, 1, COL_BORDER);

    /* Left: app title + current filename. */
    text(buf, stride, bw, bh, 16, (TOOLBAR_H - FONT_H) / 2, "Photos", COL_TEXT);
    if (g_cur >= 0 && g_cur < g_count) {
        text(buf, stride, bw, bh, 16 + font_text_width("Photos") + 18,
             (TOOLBAR_H - FONT_H) / 2, g_names[g_cur], COL_DIM);
    }

    /* Center-ish: "i / N" position + zoom percent. */
    if (g_count > 0) {
        char info[64]; info[0] = '\0';
        char a[16], b[16];
        fmt_int(a, sizeof(a), (g_cur < 0 ? 0 : g_cur + 1));
        fmt_int(b, sizeof(b), g_count);
        s_cat(info, a, sizeof(info)); s_cat(info, " / ", sizeof(info));
        s_cat(info, b, sizeof(info));
        int zp = g_zoom_num * 100 / g_zoom_den;
        char zb[16]; fmt_int(zb, sizeof(zb), zp);
        s_cat(info, "    ", sizeof(info)); s_cat(info, zb, sizeof(info));
        s_cat(info, "%", sizeof(info));
        text_center(buf, stride, bw, bh, bw / 2 - 30, (TOOLBAR_H - FONT_H) / 2, info, COL_DIM);
    }

    /* Right: zoom pills [-] [100%] [+]. */
    rect zo = rc_zoom_out(bw), zr = rc_zoom_rst(bw), zi = rc_zoom_in(bw);
    draw_button(buf, stride, bw, bh, zo, "-",     hit(zo, mx, my), COL_HOVER, COL_BTN);
    draw_button(buf, stride, bw, bh, zr, "Reset", hit(zr, mx, my), COL_HOVER, COL_BTN);
    draw_button(buf, stride, bw, bh, zi, "+",     hit(zi, mx, my), COL_HOVER, COL_BTN);
}

static void draw_filmstrip(u32 *buf, u32 stride, i32 bw, i32 bh, i32 mx, i32 my) {
    i32 sy = bh - STRIP_H;
    fill_rect(buf, stride, bw, bh, 0, sy, bw, STRIP_H, COL_TOOLBAR);
    fill_rect(buf, stride, bw, bh, 0, sy, bw, 1, COL_BORDER);

    if (g_count == 0) {
        text_center(buf, stride, bw, bh, bw / 2, sy + (STRIP_H - FONT_H) / 2,
                    "No images in /usr/share/pictures or /Desktop", COL_DIM);
        return;
    }

    for (int i = 0; i < g_count; i++) {
        rect r = rc_thumb(bw, bh, i);
        if (r.x + r.w < 0 || r.x > bw) continue;   /* offscreen */

        if (!g_thumb_ready[i]) build_thumb(i);      /* lazy build (1 per click ok) */

        /* tile backing card */
        fill_round(buf, stride, bw, bh, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 8, COL_PANEL2);

        /* blit cached thumbnail */
        u32 *tile = g_thumbs[i];
        for (int y = 0; y < THUMB_H; y++) {
            int dy = r.y + y; if (dy < sy || dy >= bh) continue;
            u32 *drow = buf + (u32)dy * stride;
            for (int x = 0; x < THUMB_W; x++) {
                int dx = r.x + x; if (dx < 0 || dx >= bw) continue;
                drow[dx] = tile[y * THUMB_W + x];
            }
        }

        /* selection ring / hover ring */
        if (i == g_cur)        stroke_round(buf, stride, bw, bh, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 8, COL_ACCENT);
        else if (hit(r, mx, my)) stroke_round(buf, stride, bw, bh, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 8, COL_DIM);
    }
}

/* Chevron glyphs drawn as filled triangles (so they read even without fonts). */
static void draw_chevron(u32 *buf, u32 stride, i32 bw, i32 bh, rect r, int left, int hot) {
    fill_round(buf, stride, bw, bh, r.x, r.y, r.w, r.h, 12, hot ? COL_HOVER : COL_PANEL2);
    stroke_round(buf, stride, bw, bh, r.x, r.y, r.w, r.h, 12, COL_BORDER);
    i32 cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (i32 dy = -10; dy <= 10; dy++) {
        i32 run = 10 - (dy < 0 ? -dy : dy);   /* triangle half-width per row */
        for (i32 t = 0; t <= run; t++) {
            i32 x = left ? (cx + 4 - t) : (cx - 4 + t);
            px_put(buf, stride, bw, bh, x,     cy + dy, COL_TEXT);
            px_put(buf, stride, bw, bh, x + 1, cy + dy, COL_TEXT);
        }
    }
}

/* ------------------------------------------------------------------------- *
 *  Entry point.                                                              *
 * ------------------------------------------------------------------------- */
void _start(void) {
    print("[PHOTOS] starting\n");

    if (wl_connect() != 0) {
        print("[PHOTOS] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "Photos");
    if (!win) {
        print("[PHOTOS] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    print("[PHOTOS] window "); print_num(win->win_id); print(" created\n");

    build_catalogue();
    if (g_count > 0) open_image(0, (i32)win->w);     /* show first image      */

    u32 stride = win->stride / 4u;
    i32 mx = 0, my = 0;        /* last known cursor position                  */
    int last_btn = 0;         /* previous pointer button mask (edge detect)   */
    ulong frame = 0;

    for (;;) {
        i32 bw = (i32)win->w, bh = (i32)win->h;

        /* Drain input. */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_POINTER) {
                mx = a; my = b;
                int btn = c & 1;                          /* left button       */
                if (btn && !last_btn) {                   /* click (press edge) */
                    rect zo = rc_zoom_out(bw), zr = rc_zoom_rst(bw), zi = rc_zoom_in(bw);
                    rect pv = rc_prev(bw, bh), nx = rc_next(bw, bh);
                    if      (hit(zo, mx, my)) zoom_out();
                    else if (hit(zr, mx, my)) zoom_reset();
                    else if (hit(zi, mx, my)) zoom_in();
                    else if (hit(pv, mx, my)) nav_prev(bw);
                    else if (hit(nx, mx, my)) nav_next(bw);
                    else {
                        for (int i = 0; i < g_count; i++) {
                            if (hit(rc_thumb(bw, bh, i), mx, my)) { open_image(i, bw); break; }
                        }
                    }
                }
                last_btn = btn;
            } else if (kind == WL_EVENT_KEY) {
                if (b) handle_key(a, bw);                 /* b = pressed       */
            }
        }

        /* Compose the frame. */
        fill_rect(win->pixels, stride, bw, bh, 0, 0, bw, bh, COL_BG);
        draw_canvas(win->pixels, stride, bw, bh);

        /* nav chevrons float over the canvas */
        rect pv = rc_prev(bw, bh), nx = rc_next(bw, bh);
        if (g_count > 1) {
            draw_chevron(win->pixels, stride, bw, bh, pv, 1, hit(pv, mx, my));
            draw_chevron(win->pixels, stride, bw, bh, nx, 0, hit(nx, mx, my));
        }

        draw_toolbar(win->pixels, stride, bw, bh, mx, my);
        draw_filmstrip(win->pixels, stride, bw, bh, mx, my);

        wl_commit(win);

        frame++;
        if ((frame % 600) == 0) { print("[PHOTOS] frame "); print_num((long)frame); print("\n"); }
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
