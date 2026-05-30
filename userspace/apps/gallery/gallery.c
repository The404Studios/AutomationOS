/*
 * gallery.c -- Image Gallery app (freestanding, ring 3).
 * =======================================================
 *
 * Opens a 720x520 window titled "Gallery". Scans /tmp (and /tmp/shots if
 * present) for files whose names end in ".ppm". Parses binary PPM (P6
 * format: "P6\n<w> <h>\n255\n" then w*h*3 bytes RGB) and renders either:
 *
 *   THUMBNAIL GRID VIEW (default):
 *     4 columns of ~150x110 thumbnail tiles (nearest-neighbour downscale).
 *     Below each tile: the filename (truncated to fit).
 *     Navigation bar at top: "Gallery | N images".
 *     Prev/Next page buttons when there are more than 16 thumbnails.
 *     Click a thumbnail to enter FULL VIEW.
 *     Empty state: "No images yet — use the Screenshot app to capture some."
 *
 *   FULL VIEW (after thumbnail click):
 *     The selected image downscaled-to-fit the canvas (nearest-neighbour).
 *     Top bar: filename + "W x H px".
 *     Bottom bar: [<Prev]  [Back to Grid]  [Next>] buttons.
 *
 * Memory bounds:
 *   PPM pixel data is read into g_imgbuf (static, 1024*768*3 = 2 359 296 B).
 *   Images larger than IMG_BUF_MAX bytes are skipped with a serial warning.
 *   PPM headers larger than HDR_BUF bytes, or malformed headers, are skipped.
 *   Thumbnail pixels are kept in g_thumbs[MAX_IMAGES][THUMB_W*THUMB_H]
 *   (16 slots * 150*110*4 = 9 900 * 4 * 16 = 633 600 B), totalling under 3 MB
 *   of static BSS — fine for a ring-3 process.
 *
 * Syscall numbers (kernel/include/syscall.h):
 *   SYS_READ=2, SYS_WRITE=3, SYS_OPEN=4, SYS_CLOSE=5,
 *   SYS_YIELD=15, SYS_OPENDIR=30, SYS_READDIR=31, SYS_CLOSEDIR=32.
 *
 * Build (flags DIRECTLY on the command line; never via a shell variable
 * or -fno-stack-protector is dropped → fs:0x28 fault):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/gallery/gallery.c -o /tmp/gallery.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/gallery.o /tmp/wlc.o /tmp/bf.o -o /tmp/gallery.elf
 *   objdump -d /tmp/gallery.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -------------------------------------------------------------------------
 * Syscall numbers and inline helper (6-argument form).
 * ----------------------------------------------------------------------- */
#define SYS_READ     2
#define SYS_WRITE    3
#define SYS_OPEN     4
#define SYS_CLOSE    5
#define SYS_YIELD    15
#define SYS_OPENDIR  30
#define SYS_READDIR  31
#define SYS_CLOSEDIR 32

/* O_RDONLY = 0 (matching kernel ABI). */
#define O_RDONLY 0

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

/* -------------------------------------------------------------------------
 * Fixed-width types.
 * ----------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef unsigned char      u8;
typedef int                i32;
typedef unsigned long      ulong;
typedef unsigned long long u64;

/* -------------------------------------------------------------------------
 * Freestanding string / print helpers.
 * ----------------------------------------------------------------------- */
static ulong k_strlen(const char *s)
{
    ulong n = 0;
    while (s[n]) n++;
    return n;
}

static void k_memset(void *dst, int c, ulong n)
{
    u8 *p = (u8 *)dst;
    while (n--) *p++ = (u8)c;
}

static void k_memcpy(void *dst, const void *src, ulong n)
{
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

static int k_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void k_strlcpy(char *dst, const char *src, int n)
{
    int i = 0;
    if (n <= 0) return;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void k_strlcat(char *dst, const char *src, int n)
{
    int len = 0;
    while (dst[len]) len++;
    int i = 0;
    while (len + i < n - 1 && src[i]) { dst[len + i] = src[i]; i++; }
    dst[len + i] = '\0';
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

static void fmt_u32(char *buf, u32 v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (v > 0 && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int lo = 0, hi = i - 1;
    while (lo < hi) { char t = tmp[lo]; tmp[lo] = tmp[hi]; tmp[hi] = t; lo++; hi--; }
    tmp[i] = '\0';
    k_strlcpy(buf, tmp, 12);
}

/* Check if name ends with .ppm (case-insensitive). */
static int ends_ppm(const char *name)
{
    ulong n = k_strlen(name);
    if (n < 4) return 0;
    const char *t = name + n - 4;
    return (t[0] == '.' &&
            (t[1] == 'p' || t[1] == 'P') &&
            (t[2] == 'p' || t[2] == 'P') &&
            (t[3] == 'm' || t[3] == 'M'));
}

/* -------------------------------------------------------------------------
 * Directory listing structures (must match kernel/include/vfs.h).
 * ----------------------------------------------------------------------- */
#define DT_REG   8
#define DT_DIR   4
#define NAME_MAX 256

struct dirent {
    u64            d_ino;
    long long      d_off;
    unsigned short d_reclen;
    u8             d_type;
    char           d_name[NAME_MAX];
};

/* -------------------------------------------------------------------------
 * Window / layout constants.
 * ----------------------------------------------------------------------- */
#define WIN_W     720
#define WIN_H     520

/* Top navigation bar height. */
#define NAV_H     36

/* Thumbnail tile size (displayed). */
#define THUMB_W   150
#define THUMB_H   110

/* Label row below each thumbnail. */
#define LABEL_H   18

/* Grid cell size (tile + label + padding). */
#define CELL_W    (THUMB_W + 8)    /* 158 */
#define CELL_H    (THUMB_H + LABEL_H + 12)  /* 140 */

/* 4 columns; auto-computed left margin. */
#define GRID_COLS    4
#define GRID_MARGIN  ((WIN_W - GRID_COLS * CELL_W) / 2)   /* (720-632)/2=44 */

/* Grid canvas height (below NAV_H). */
#define GRID_H    (WIN_H - NAV_H - 32)   /* 452 */

/* Rows per page. */
#define ROWS_PER_PAGE  3
#define CELLS_PER_PAGE (GRID_COLS * ROWS_PER_PAGE)   /* 12 */

/* Bottom bar for prev/next. */
#define BOT_H     32
#define BOT_Y     (WIN_H - BOT_H)

/* Maximum images we track. */
#define MAX_IMAGES  64

/* PPM read buffer: 1024 * 768 * 3 = 2 359 296 bytes. */
#define IMG_BUF_MAX  (1024 * 768 * 3)

/* PPM header parsing buffer. */
#define HDR_BUF      128

/* Full-view canvas: below NAV bar, above BOT bar. */
#define FULL_CANVAS_Y  NAV_H
#define FULL_CANVAS_H  (WIN_H - NAV_H - BOT_H)

/* -------------------------------------------------------------------------
 * Color palette.
 * ----------------------------------------------------------------------- */
#define C_BG          0xFF1A1A2Eu   /* dark navy background   */
#define C_NAV         0xFF22223Au   /* navigation bar         */
#define C_NAV_SEP     0xFF3A3A5Au   /* nav separator line     */
#define C_CELL_BG     0xFF242438u   /* thumbnail cell bg      */
#define C_CELL_BRD    0xFF3A3A5Au   /* thumbnail border       */
#define C_CELL_HOV    0xFF2E2E50u   /* hovered cell bg        */
#define C_LABEL       0xFFBBBBDDu   /* filename label text    */
#define C_TITLE       0xFFEEEEFFu   /* nav bar title text     */
#define C_DIM         0xFF7777AAu   /* dim text               */
#define C_BTN         0xFF383860u   /* button face            */
#define C_BTN_HOV     0xFF484878u   /* button hovered         */
#define C_BTN_BRD     0xFF555588u   /* button border          */
#define C_BOT         0xFF1E1E38u   /* bottom bar             */
#define C_BOT_SEP     0xFF3A3A5Au   /* bottom bar top line    */
#define C_EMPTY       0xFF555588u   /* empty-state text       */
#define C_FULL_BG     0xFF111122u   /* full-view bg           */
#define C_WHITE       0xFFFFFFFFu
#define C_AMBER       0xFFF0C030u   /* empty-state accent     */

/* -------------------------------------------------------------------------
 * Static global state (.bss).
 * ----------------------------------------------------------------------- */

/* Image catalog. */
typedef struct {
    char name[NAME_MAX];     /* just the filename, no path */
    char path[NAME_MAX + 8]; /* full path (/tmp/name.ppm) */
    u32  img_w;              /* PPM width  (0 = not loaded/invalid) */
    u32  img_h;              /* PPM height */
    int  loaded;             /* 1 = thumbnail decoded OK */
} img_entry_t;

static img_entry_t g_images[MAX_IMAGES];
static int         g_nimg;        /* total found */
static int         g_page;        /* current page (grid view) */
static int         g_hover;       /* hovered cell index on current page, -1=none */

/* View mode. */
#define VIEW_GRID 0
#define VIEW_FULL 1
static int g_view;
static int g_full_idx;   /* index into g_images[] for full view */

/* Thumbnail pixel buffers: ARGB32, THUMB_W * THUMB_H each. */
static u32 g_thumbs[MAX_IMAGES][THUMB_W * THUMB_H];

/* Large single image read buffer (PPM RGB data). */
static u8 g_imgbuf[IMG_BUF_MAX];

/* Header read buffer (for parsing PPM header). */
static char g_hdrbuf[HDR_BUF];

/* Path construction buffer (zeroed before each syscall). */
static char g_pathbuf[NAME_MAX + 16];

/* -------------------------------------------------------------------------
 * Drawing primitives.
 * ----------------------------------------------------------------------- */
static u32  g_stride_px;   /* pixels per row in win->pixels */
static u32 *g_pixels;
static u32  g_bw, g_bh;

static void fill_rect(i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)g_bw) x2 = (i32)g_bw;
    i32 y2 = y + h; if (y2 > (i32)g_bh) y2 = (i32)g_bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = g_pixels + (u32)yy * g_stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = color;
    }
}

static void draw_border(i32 x, i32 y, i32 w, i32 h, u32 color)
{
    fill_rect(x,         y,         w, 1, color);
    fill_rect(x,         y + h - 1, w, 1, color);
    fill_rect(x,         y,         1, h, color);
    fill_rect(x + w - 1, y,         1, h, color);
}

/* Draw text left-aligned at (x,y). */
static void draw_text(i32 x, i32 y, const char *s, u32 color)
{
    font_draw_string(g_pixels, (int)g_stride_px,
                     (int)g_bw, (int)g_bh, x, y, s, color);
}

/* Draw text centered horizontally within [cx-half_w .. cx+half_w] at y. */
static void draw_text_center(i32 cx, i32 y, const char *s, u32 color)
{
    int tw = font_text_width(s);
    draw_text(cx - tw / 2, y, s, color);
}

/* Blit a ARGB32 pixel array (sw x sh) into window at (dx,dy), clipped. */
static void blit_pixels(i32 dx, i32 dy, u32 sw, u32 sh, const u32 *src)
{
    for (u32 row = 0; row < sh; row++) {
        i32 wy = dy + (i32)row;
        if (wy < 0 || wy >= (i32)g_bh) continue;
        u32 *dst_row = g_pixels + (u32)wy * g_stride_px;
        const u32 *src_row = src + row * sw;
        for (u32 col = 0; col < sw; col++) {
            i32 wx = dx + (i32)col;
            if (wx < 0 || wx >= (i32)g_bw) continue;
            dst_row[wx] = src_row[col];
        }
    }
}

/* Truncate string to fit within max_px pixels; result in out (cap must be >= len). */
static void truncate_to_px(char *out, int cap, const char *s, int max_px)
{
    int total = (int)k_strlen(s);
    if (font_text_width(s) <= max_px) {
        k_strlcpy(out, s, cap);
        return;
    }
    /* Binary search for longest prefix that fits with "...". */
    const char *dots = "...";
    int dots_w = font_text_width(dots);
    int avail = max_px - dots_w;
    if (avail <= 0) { k_strlcpy(out, dots, cap); return; }
    int lo = 0, hi = total;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        char tmp[NAME_MAX];
        k_strlcpy(tmp, s, mid + 1);
        tmp[mid] = '\0';
        if (font_text_width(tmp) <= avail) lo = mid;
        else hi = mid - 1;
    }
    k_strlcpy(out, s, lo + 1);
    out[lo] = '\0';
    k_strlcat(out, dots, cap);
}

/* -------------------------------------------------------------------------
 * PPM parsing.
 *
 * Binary PPM (P6) header format:
 *   "P6" whitespace width whitespace height whitespace maxval whitespace
 * The maxval must be 255.  After the last header whitespace byte, raw RGB
 * triples follow.  Comments (#...\n) are allowed in the header.
 *
 * We read the file in two passes:
 *   1. Read up to HDR_BUF bytes to parse the header.
 *   2. Seek (skip) to pixel data start, then read up to IMG_BUF_MAX bytes.
 *
 * Because we have no seek syscall, we read the whole file sequentially:
 *   - For the header, fill g_hdrbuf by reading one byte at a time until
 *     we've parsed "P6 W H 255\n" and note the byte offset.
 *   - Then read the rest into g_imgbuf.
 *
 * To avoid reading one byte at a time into the large buffer, we do:
 *   read(fd, g_hdrbuf, HDR_BUF) -> parse header -> note hdr_len
 *   then read remaining pixels in chunks into g_imgbuf.
 *
 * If the PPM is malformed or oversized, returns 0 (failure).
 * On success: fills *out_w, *out_h, and writes RGB data into g_imgbuf.
 * ----------------------------------------------------------------------- */

/* Skip whitespace and PPM comments in buffer[pos..len].
 * Returns new pos, or -1 if end reached without finding a non-ws byte. */
static int ppm_skip_ws(const char *buf, int pos, int len)
{
    while (pos < len) {
        char c = buf[pos];
        if (c == '#') {
            /* skip to end of line */
            while (pos < len && buf[pos] != '\n') pos++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pos++;
            continue;
        }
        return pos;
    }
    return -1;
}

/* Parse an ASCII unsigned integer from buf[pos..len]; writes result to *val.
 * Returns new pos after the integer, or -1 on failure. */
static int ppm_parse_uint(const char *buf, int pos, int len, u32 *val)
{
    if (pos < 0 || pos >= len) return -1;
    char c = buf[pos];
    if (c < '0' || c > '9') return -1;
    u32 v = 0;
    while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
        v = v * 10u + (u32)(buf[pos] - '0');
        pos++;
    }
    *val = v;
    return pos;
}

/*
 * Read a PPM file identified by path into g_imgbuf.
 * Returns 1 on success (fills *out_w and *out_h), 0 on failure.
 * g_imgbuf will hold raw RGB data (w*h*3 bytes) on success.
 */
static int ppm_load(const char *path, u32 *out_w, u32 *out_h)
{
    /* Open. */
    k_memset(g_pathbuf, 0, sizeof(g_pathbuf));
    k_strlcpy(g_pathbuf, path, (int)sizeof(g_pathbuf));

    int fd = (int)sc(SYS_OPEN, (long)g_pathbuf, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) {
        print("[GALLERY] open failed: ");
        print(g_pathbuf);
        print("\n");
        return 0;
    }

    /* Read up to HDR_BUF bytes to parse header. */
    k_memset(g_hdrbuf, 0, HDR_BUF);
    long hdr_got = sc(SYS_READ, fd, (long)g_hdrbuf, HDR_BUF - 1, 0, 0, 0);
    if (hdr_got < 4) {
        sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
        print("[GALLERY] too short: ");
        print(g_pathbuf);
        print("\n");
        return 0;
    }

    /* Check magic "P6". */
    if (g_hdrbuf[0] != 'P' || g_hdrbuf[1] != '6') {
        sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
        print("[GALLERY] not P6 PPM: ");
        print(g_pathbuf);
        print("\n");
        return 0;
    }

    int pos = 2;
    int hlen = (int)hdr_got;

    /* Skip whitespace after "P6". */
    pos = ppm_skip_ws(g_hdrbuf, pos, hlen);
    if (pos < 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return 0; }

    /* Parse width. */
    u32 img_w = 0;
    pos = ppm_parse_uint(g_hdrbuf, pos, hlen, &img_w);
    if (pos < 0 || img_w == 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return 0; }

    /* Skip whitespace. */
    pos = ppm_skip_ws(g_hdrbuf, pos, hlen);
    if (pos < 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return 0; }

    /* Parse height. */
    u32 img_h = 0;
    pos = ppm_parse_uint(g_hdrbuf, pos, hlen, &img_h);
    if (pos < 0 || img_h == 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return 0; }

    /* Skip whitespace. */
    pos = ppm_skip_ws(g_hdrbuf, pos, hlen);
    if (pos < 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return 0; }

    /* Parse maxval — must be 255. */
    u32 maxval = 0;
    pos = ppm_parse_uint(g_hdrbuf, pos, hlen, &maxval);
    if (pos < 0 || maxval != 255) {
        sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
        print("[GALLERY] maxval != 255: ");
        print(g_pathbuf);
        print("\n");
        return 0;
    }

    /* Skip exactly one whitespace byte (the header terminator). */
    if (pos >= hlen) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return 0; }
    pos++; /* consume the single whitespace byte after "255" */

    /* Validate pixel data size against buffer cap. */
    ulong pixel_bytes = (ulong)img_w * (ulong)img_h * 3UL;
    if (pixel_bytes > (ulong)IMG_BUF_MAX) {
        sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
        print("[GALLERY] image too large (>1024x768), skipping: ");
        print(g_pathbuf);
        print("\n");
        return 0;
    }

    /* Copy the pixel bytes already in g_hdrbuf (after pos) into g_imgbuf. */
    int already = hlen - pos;
    if (already < 0) already = 0;
    ulong need = pixel_bytes;
    ulong got = 0;

    if ((ulong)already > need) already = (int)need;
    if (already > 0) {
        k_memcpy(g_imgbuf, (u8 *)g_hdrbuf + pos, (ulong)already);
        got = (ulong)already;
    }

    /* Read remaining pixel data. */
    while (got < need) {
        ulong want = need - got;
        if (want > 4096) want = 4096;
        long r = sc(SYS_READ, fd, (long)(g_imgbuf + got), (long)want, 0, 0, 0);
        if (r <= 0) break;
        got += (ulong)r;
    }

    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

    if (got < need) {
        print("[GALLERY] short read: ");
        print(g_pathbuf);
        print("\n");
        return 0;
    }

    *out_w = img_w;
    *out_h = img_h;
    return 1;
}

/* -------------------------------------------------------------------------
 * Nearest-neighbour downscale: PPM RGB (g_imgbuf) -> ARGB32 thumbnail.
 * dst must be THUMB_W * THUMB_H u32's.
 * ----------------------------------------------------------------------- */
static void make_thumbnail(u32 src_w, u32 src_h, u32 *dst)
{
    u32 dw = THUMB_W, dh = THUMB_H;
    for (u32 dy = 0; dy < dh; dy++) {
        u32 sy = dy * src_h / dh;
        if (sy >= src_h) sy = src_h - 1;
        for (u32 dx = 0; dx < dw; dx++) {
            u32 sx = dx * src_w / dw;
            if (sx >= src_w) sx = src_w - 1;
            const u8 *px = g_imgbuf + (sy * src_w + sx) * 3;
            dst[dy * dw + dx] = 0xFF000000u
                                | ((u32)px[0] << 16)  /* R */
                                | ((u32)px[1] <<  8)  /* G */
                                | ((u32)px[2]);        /* B */
        }
    }
}

/*
 * Nearest-neighbour scale-to-fit for full view.
 * Renders the image (from g_imgbuf, src_w x src_h) into the window buffer,
 * scaled to fit within (fit_w x fit_h) preserving aspect ratio, centered at
 * (base_x, base_y).
 */
static void blit_fit(i32 base_x, i32 base_y, i32 fit_w, i32 fit_h,
                     u32 src_w, u32 src_h)
{
    if (src_w == 0 || src_h == 0 || fit_w <= 0 || fit_h <= 0) return;

    /* Compute scale (integer fractions: dst*src/dst = src). */
    /* We want dw/dh to fit within fit_w x fit_h. */
    u32 dw, dh;
    /* scale_w = fit_w/src_w, scale_h = fit_h/src_h; pick min. */
    /* Use cross-multiplication to avoid division:
     * scale_w < scale_h  iff  fit_w * src_h < fit_h * src_w */
    u64 cross_w = (u64)(u32)fit_w * (u64)src_h;
    u64 cross_h = (u64)(u32)fit_h * (u64)src_w;
    if (cross_w <= cross_h) {
        /* width-limited */
        dw = (u32)fit_w;
        dh = (u32)((u64)(u32)fit_w * (u64)src_h / (u64)src_w);
    } else {
        /* height-limited */
        dh = (u32)fit_h;
        dw = (u32)((u64)(u32)fit_h * (u64)src_w / (u64)src_h);
    }
    if (dw == 0) dw = 1;
    if (dh == 0) dh = 1;

    /* Center within fit area. */
    i32 ox = base_x + (fit_w - (i32)dw) / 2;
    i32 oy = base_y + (fit_h - (i32)dh) / 2;

    for (u32 dy = 0; dy < dh; dy++) {
        i32 wy = oy + (i32)dy;
        if (wy < 0 || wy >= (i32)g_bh) continue;
        u32 sy = dy * src_h / dh;
        if (sy >= src_h) sy = src_h - 1;
        u32 *dst_row = g_pixels + (u32)wy * g_stride_px;
        for (u32 dx = 0; dx < dw; dx++) {
            i32 wx = ox + (i32)dx;
            if (wx < 0 || wx >= (i32)g_bw) continue;
            u32 sx = dx * src_w / dw;
            if (sx >= src_w) sx = src_w - 1;
            const u8 *pix = g_imgbuf + (sy * src_w + sx) * 3;
            dst_row[wx] = 0xFF000000u
                         | ((u32)pix[0] << 16)
                         | ((u32)pix[1] <<  8)
                         |  (u32)pix[2];
        }
    }
}

/* -------------------------------------------------------------------------
 * Directory scan: collect .ppm files from a directory path.
 * ----------------------------------------------------------------------- */
static void scan_dir(const char *dirpath)
{
    k_memset(g_pathbuf, 0, sizeof(g_pathbuf));
    k_strlcpy(g_pathbuf, dirpath, (int)sizeof(g_pathbuf));

    long dfd = sc(SYS_OPENDIR, (long)g_pathbuf, 0, 0, 0, 0, 0);
    if (dfd < 0) return;   /* directory doesn't exist – silently skip */

    struct dirent ent;
    while (g_nimg < MAX_IMAGES) {
        long r = sc(SYS_READDIR, dfd, (long)&ent, 0, 0, 0, 0);
        if (r != 0) break;
        /* Skip dots and directories. */
        if (ent.d_name[0] == '.') continue;
        if (ent.d_type == DT_DIR) continue;
        if (!ends_ppm(ent.d_name)) continue;

        img_entry_t *e = &g_images[g_nimg];
        k_strlcpy(e->name, ent.d_name, NAME_MAX);
        /* Build full path. */
        k_strlcpy(e->path, dirpath, (int)sizeof(e->path));
        int plen = (int)k_strlen(e->path);
        if (plen > 0 && e->path[plen - 1] != '/') {
            e->path[plen]   = '/';
            e->path[plen+1] = '\0';
        }
        k_strlcat(e->path, ent.d_name, (int)sizeof(e->path));
        e->img_w = 0;
        e->img_h = 0;
        e->loaded = 0;
        g_nimg++;
    }

    sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);
}

/* -------------------------------------------------------------------------
 * Load (or reload) thumbnail for image index i.
 * ----------------------------------------------------------------------- */
static void load_thumb(int i)
{
    if (i < 0 || i >= g_nimg) return;
    img_entry_t *e = &g_images[i];

    u32 w = 0, h = 0;
    if (!ppm_load(e->path, &w, &h)) {
        e->loaded = 0;
        return;
    }
    e->img_w  = w;
    e->img_h  = h;
    make_thumbnail(w, h, g_thumbs[i]);
    e->loaded = 1;
}

/* -------------------------------------------------------------------------
 * Ensure thumbnails for the current page are loaded.
 * ----------------------------------------------------------------------- */
static void ensure_page_thumbs(void)
{
    int start = g_page * CELLS_PER_PAGE;
    int end   = start + CELLS_PER_PAGE;
    if (end > g_nimg) end = g_nimg;
    for (int i = start; i < end; i++) {
        if (!g_images[i].loaded)
            load_thumb(i);
    }
}

/* -------------------------------------------------------------------------
 * Button rendering helper.
 * ----------------------------------------------------------------------- */
static void draw_btn(i32 x, i32 y, i32 w, i32 h,
                     const char *label, int hovered)
{
    u32 bg = hovered ? C_BTN_HOV : C_BTN;
    fill_rect(x, y, w, h, bg);
    draw_border(x, y, w, h, C_BTN_BRD);
    int tw = font_text_width(label);
    draw_text(x + (w - tw) / 2, y + (h - FONT_H) / 2, label, C_WHITE);
}

/* -------------------------------------------------------------------------
 * GRID VIEW rendering.
 * ----------------------------------------------------------------------- */

/* Button hit areas in grid view (bottom bar). */
#define PREV_BTN_X  (WIN_W/2 - 130)
#define PREV_BTN_Y  (BOT_Y + 4)
#define PREV_BTN_W  80
#define PREV_BTN_H  24

#define NEXT_BTN_X  (WIN_W/2 + 50)
#define NEXT_BTN_Y  (BOT_Y + 4)
#define NEXT_BTN_W  80
#define NEXT_BTN_H  24

static int g_prev_hov, g_next_hov;   /* button hover states */

/* Returns page count. */
static int page_count(void)
{
    if (g_nimg == 0) return 1;
    return (g_nimg + CELLS_PER_PAGE - 1) / CELLS_PER_PAGE;
}

static void render_grid(void)
{
    /* Background. */
    fill_rect(0, 0, WIN_W, WIN_H, C_BG);

    /* Navigation bar. */
    fill_rect(0, 0, WIN_W, NAV_H, C_NAV);
    fill_rect(0, NAV_H - 1, WIN_W, 1, C_NAV_SEP);

    /* Title. */
    char title[64];
    k_strlcpy(title, "Gallery", (int)sizeof(title));
    draw_text(10, (NAV_H - FONT_H) / 2, title, C_TITLE);

    /* Count. */
    if (g_nimg > 0) {
        char cnt[48];
        char nb[12];
        fmt_u32(nb, (u32)g_nimg);
        k_strlcpy(cnt, nb, (int)sizeof(cnt));
        k_strlcat(cnt, " image", (int)sizeof(cnt));
        if (g_nimg != 1) k_strlcat(cnt, "s", (int)sizeof(cnt));
        /* page indicator */
        if (page_count() > 1) {
            char pb[24];
            k_strlcpy(pb, "  (page ", (int)sizeof(pb));
            fmt_u32(nb, (u32)(g_page + 1));
            k_strlcat(pb, nb, (int)sizeof(pb));
            k_strlcat(pb, "/", (int)sizeof(pb));
            fmt_u32(nb, (u32)page_count());
            k_strlcat(pb, nb, (int)sizeof(pb));
            k_strlcat(pb, ")", (int)sizeof(pb));
            k_strlcat(cnt, pb, (int)sizeof(cnt));
        }
        int tw = font_text_width(cnt);
        draw_text(WIN_W - tw - 10, (NAV_H - FONT_H) / 2, cnt, C_DIM);
    }

    /* Empty state. */
    if (g_nimg == 0) {
        const char *line1 = "No images yet";
        const char *line2 = "Use the Screenshot app to capture some.";
        draw_text_center(WIN_W / 2, WIN_H / 2 - FONT_H - 4, line1, C_AMBER);
        draw_text_center(WIN_W / 2, WIN_H / 2 + 4,           line2, C_DIM);
        return;
    }

    /* Thumbnail grid. */
    int start = g_page * CELLS_PER_PAGE;
    for (int ci = 0; ci < CELLS_PER_PAGE; ci++) {
        int idx = start + ci;
        if (idx >= g_nimg) break;

        int row = ci / GRID_COLS;
        int col = ci % GRID_COLS;
        i32 cx  = GRID_MARGIN + col * CELL_W;
        i32 cy  = NAV_H + 4 + row * CELL_H;

        /* Cell background. */
        u32 cell_bg = (ci == g_hover) ? C_CELL_HOV : C_CELL_BG;
        fill_rect(cx, cy, CELL_W, CELL_H, cell_bg);

        /* Thumbnail area. */
        i32 tx = cx + 4;
        i32 ty = cy + 4;

        if (g_images[idx].loaded) {
            blit_pixels(tx, ty, THUMB_W, THUMB_H, g_thumbs[idx]);
        } else {
            /* Placeholder: dark rectangle with "?" */
            fill_rect(tx, ty, THUMB_W, THUMB_H, 0xFF181830u);
            draw_text_center(tx + THUMB_W / 2, ty + THUMB_H / 2 - FONT_H / 2,
                             "?", C_DIM);
        }

        /* Border around thumbnail. */
        draw_border(tx, ty, THUMB_W, THUMB_H, C_CELL_BRD);

        /* Filename label below thumbnail. */
        char lbl[NAME_MAX];
        truncate_to_px(lbl, (int)sizeof(lbl), g_images[idx].name,
                       THUMB_W - 4);
        i32 lx = tx + (THUMB_W - font_text_width(lbl)) / 2;
        i32 ly = ty + THUMB_H + 3;
        draw_text(lx, ly, lbl, C_LABEL);
    }

    /* Bottom bar. */
    fill_rect(0, BOT_Y, WIN_W, BOT_H, C_BOT);
    fill_rect(0, BOT_Y, WIN_W, 1, C_BOT_SEP);

    if (page_count() > 1) {
        draw_btn(PREV_BTN_X, PREV_BTN_Y, PREV_BTN_W, PREV_BTN_H,
                 "< Prev", g_prev_hov);
        draw_btn(NEXT_BTN_X, NEXT_BTN_Y, NEXT_BTN_W, NEXT_BTN_H,
                 "Next >", g_next_hov);
    }
}

/* -------------------------------------------------------------------------
 * FULL VIEW rendering.
 * ----------------------------------------------------------------------- */

/* Full-view button geometry. */
#define FB_PREV_X   (WIN_W/2 - 190)
#define FB_BACK_X   (WIN_W/2 - 55)
#define FB_NEXT_X   (WIN_W/2 + 110)
#define FB_BTN_Y    (BOT_Y + 4)
#define FB_BTN_W    110
#define FB_BACK_W   110
#define FB_BTN_H    24

static int g_fb_prev_hov, g_fb_back_hov, g_fb_next_hov;

/* Load the full-view image (if not already in g_imgbuf for this index).
 * We track which index is currently loaded. */
static int g_full_loaded_idx = -1;
static u32 g_full_w, g_full_h;

static void load_full(int idx)
{
    if (g_full_loaded_idx == idx) return;
    g_full_loaded_idx = -1;
    if (idx < 0 || idx >= g_nimg) return;

    u32 w = 0, h = 0;
    if (!ppm_load(g_images[idx].path, &w, &h)) return;
    g_full_w = w;
    g_full_h = h;
    g_full_loaded_idx = idx;
}

static void render_full(void)
{
    /* Background. */
    fill_rect(0, 0, WIN_W, WIN_H, C_FULL_BG);

    /* Nav bar. */
    fill_rect(0, 0, WIN_W, NAV_H, C_NAV);
    fill_rect(0, NAV_H - 1, WIN_W, 1, C_NAV_SEP);

    /* Filename + dimensions in nav bar. */
    if (g_full_idx >= 0 && g_full_idx < g_nimg) {
        img_entry_t *e = &g_images[g_full_idx];
        char info[128];
        k_strlcpy(info, e->name, (int)sizeof(info));
        if (g_full_loaded_idx == g_full_idx) {
            char dims[32];
            char nb[12];
            k_strlcpy(dims, "  (", (int)sizeof(dims));
            fmt_u32(nb, g_full_w);
            k_strlcat(dims, nb, (int)sizeof(dims));
            k_strlcat(dims, " x ", (int)sizeof(dims));
            fmt_u32(nb, g_full_h);
            k_strlcat(dims, nb, (int)sizeof(dims));
            k_strlcat(dims, " px)", (int)sizeof(dims));
            k_strlcat(info, dims, (int)sizeof(info));
        }
        draw_text(10, (NAV_H - FONT_H) / 2, info, C_TITLE);

        /* Index indicator at right. */
        char idx_str[32];
        char nb[12];
        fmt_u32(nb, (u32)(g_full_idx + 1));
        k_strlcpy(idx_str, nb, (int)sizeof(idx_str));
        k_strlcat(idx_str, " / ", (int)sizeof(idx_str));
        fmt_u32(nb, (u32)g_nimg);
        k_strlcat(idx_str, nb, (int)sizeof(idx_str));
        int tw = font_text_width(idx_str);
        draw_text(WIN_W - tw - 10, (NAV_H - FONT_H) / 2, idx_str, C_DIM);
    }

    /* Canvas area. */
    fill_rect(0, FULL_CANVAS_Y, WIN_W, FULL_CANVAS_H, C_FULL_BG);

    /* Image or error. */
    if (g_full_loaded_idx == g_full_idx) {
        blit_fit(0, FULL_CANVAS_Y, WIN_W, FULL_CANVAS_H,
                 g_full_w, g_full_h);
    } else {
        const char *err = "Could not load image";
        draw_text_center(WIN_W / 2, FULL_CANVAS_Y + FULL_CANVAS_H / 2 - FONT_H / 2,
                         err, C_DIM);
    }

    /* Bottom bar. */
    fill_rect(0, BOT_Y, WIN_W, BOT_H, C_BOT);
    fill_rect(0, BOT_Y, WIN_W, 1, C_BOT_SEP);

    draw_btn(FB_PREV_X, FB_BTN_Y, FB_BTN_W, FB_BTN_H,
             "< Prev", g_fb_prev_hov);
    draw_btn(FB_BACK_X, FB_BTN_Y, FB_BACK_W, FB_BTN_H,
             "Grid View", g_fb_back_hov);
    draw_btn(FB_NEXT_X, FB_BTN_Y, FB_BTN_W, FB_BTN_H,
             "Next >", g_fb_next_hov);
}

/* -------------------------------------------------------------------------
 * Hit-test helpers.
 * ----------------------------------------------------------------------- */

/* Returns cell index within the page (0..CELLS_PER_PAGE-1) or -1. */
static int hit_thumb(i32 mx, i32 my)
{
    i32 grid_bot = NAV_H + 4 + ROWS_PER_PAGE * CELL_H;
    if (my < NAV_H || my >= grid_bot) return -1;
    for (int ci = 0; ci < CELLS_PER_PAGE; ci++) {
        int row = ci / GRID_COLS;
        int col = ci % GRID_COLS;
        i32 cx  = GRID_MARGIN + col * CELL_W;
        i32 cy  = NAV_H + 4 + row * CELL_H;
        if (mx >= cx && mx < cx + CELL_W &&
            my >= cy && my < cy + CELL_H)
            return ci;
    }
    return -1;
}

static int hit_rect(i32 mx, i32 my, i32 x, i32 y, i32 w, i32 h)
{
    return (mx >= x && mx < x + w && my >= y && my < y + h);
}

/* -------------------------------------------------------------------------
 * Entry point.
 * ----------------------------------------------------------------------- */
void _start(void)
{
    print("[GALLERY] starting\n");

    /* Connect to compositor. */
    if (wl_connect() != 0) {
        print("[GALLERY] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Gallery");
    if (!win) {
        print("[GALLERY] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[GALLERY] window created\n");

    /* Cache window geometry. */
    g_pixels    = win->pixels;
    g_stride_px = win->stride / 4u;
    g_bw        = win->w;
    g_bh        = win->h;

    /* Scan for PPM files. */
    g_nimg = 0;
    scan_dir("/tmp");
    scan_dir("/tmp/shots");

    {
        char nb[12];
        fmt_u32(nb, (u32)g_nimg);
        print("[GALLERY] found ");
        print(nb);
        print(" PPM files\n");
    }

    /* Pre-load thumbnails for the first page. */
    g_page = 0;
    g_view = VIEW_GRID;
    g_hover = -1;
    g_prev_hov = g_next_hov = 0;
    g_fb_prev_hov = g_fb_back_hov = g_fb_next_hov = 0;
    ensure_page_thumbs();

    /* ---- Frame loop ---- */
    for (;;) {
        /* Process input events. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea;
                i32 my = (i32)eb;
                int lbtn = ec & 1;

                if (g_view == VIEW_GRID) {
                    /* Update hover. */
                    g_hover = hit_thumb(mx, my);
                    /* Check image entries on current page are valid. */
                    if (g_hover >= 0) {
                        int idx = g_page * CELLS_PER_PAGE + g_hover;
                        if (idx >= g_nimg) g_hover = -1;
                    }

                    /* Prev/Next button hover. */
                    g_prev_hov = hit_rect(mx, my, PREV_BTN_X, PREV_BTN_Y,
                                          PREV_BTN_W, PREV_BTN_H);
                    g_next_hov = hit_rect(mx, my, NEXT_BTN_X, NEXT_BTN_Y,
                                          NEXT_BTN_W, NEXT_BTN_H);

                    if (lbtn) {
                        /* Click thumbnail -> open full view. */
                        if (g_hover >= 0) {
                            int idx = g_page * CELLS_PER_PAGE + g_hover;
                            if (idx >= 0 && idx < g_nimg) {
                                g_full_idx  = idx;
                                g_view      = VIEW_FULL;
                                g_hover     = -1;
                                g_fb_prev_hov = g_fb_back_hov = g_fb_next_hov = 0;
                                load_full(g_full_idx);
                            }
                        }
                        /* Prev page. */
                        if (g_prev_hov && g_page > 0) {
                            g_page--;
                            g_hover = -1;
                            ensure_page_thumbs();
                        }
                        /* Next page. */
                        if (g_next_hov && g_page < page_count() - 1) {
                            g_page++;
                            g_hover = -1;
                            ensure_page_thumbs();
                        }
                    }

                } else { /* VIEW_FULL */
                    g_fb_prev_hov = hit_rect(mx, my, FB_PREV_X, FB_BTN_Y,
                                             FB_BTN_W, FB_BTN_H);
                    g_fb_back_hov = hit_rect(mx, my, FB_BACK_X, FB_BTN_Y,
                                             FB_BACK_W, FB_BTN_H);
                    g_fb_next_hov = hit_rect(mx, my, FB_NEXT_X, FB_BTN_Y,
                                             FB_BTN_W, FB_BTN_H);

                    if (lbtn) {
                        /* Back to grid. */
                        if (g_fb_back_hov) {
                            g_view = VIEW_GRID;
                            g_hover = -1;
                            ensure_page_thumbs();
                        }
                        /* Prev image. */
                        if (g_fb_prev_hov && g_full_idx > 0) {
                            g_full_idx--;
                            load_full(g_full_idx);
                        }
                        /* Next image. */
                        if (g_fb_next_hov && g_full_idx < g_nimg - 1) {
                            g_full_idx++;
                            load_full(g_full_idx);
                        }
                    }
                }
            }
            /* Key events not used. */
        }

        /* Render. */
        if (g_view == VIEW_GRID)
            render_grid();
        else
            render_full();

        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
