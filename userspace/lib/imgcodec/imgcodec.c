/*
 * imgcodec.c -- signature dispatch + known-answer self test.
 * ==========================================================
 *
 * img_decode() sniffs the leading bytes and routes to the right decoder:
 *   "BM"                -> bmp_decode
 *   89 50 4E 47 (\x89PNG) -> png_decode
 *   "GIF8"              -> gif_decode
 *
 * imgcodec_selftest() decodes tiny embedded BMP/PNG/GIF images and checks
 * the results. The 2x2 BMP is the MUST-PASS known-answer test: its four
 * decoded pixels are compared exactly. The PNG and GIF KATs additionally
 * verify dimensions and (for these tiny images) the four pixel values too.
 *
 * Freestanding: no libc. The only external dependency is the existing
 * DEFLATE codec used by png.c.
 */

#include "imgcodec.h"

/* ---- tiny freestanding memory primitives (own, no libc) ---- */
void *img_memset(void *dst, int v, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)v;
    return dst;
}
void *img_memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

int img_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h)
{
    if (!data || !out || !w || !h) return IMG_ERR_CORRUPT;
    if (len < 4) return IMG_ERR_TRUNCATED;

    /* BMP: 'B' 'M' */
    if (data[0] == 'B' && data[1] == 'M')
        return bmp_decode(data, len, out, out_cap, w, h);

    /* PNG: 89 50 4E 47 */
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return png_decode(data, len, out, out_cap, w, h);

    /* GIF: "GIF8" (covers GIF87a and GIF89a) */
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8')
        return gif_decode(data, len, out, out_cap, w, h);

    return IMG_ERR_BADMAGIC;
}

/* ============================================================
 * Embedded known-answer test vectors (generated, byte-exact).
 * All three are 2x2 images whose pixels are, in row-major,
 * top-to-bottom order: RED, GREEN, BLUE, WHITE.
 * ============================================================ */

/* 2x2 24-bit uncompressed BMP (bottom-up). */
static const unsigned char kat_bmp[] = {
    0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00,
    0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00
};

/* 2x2 8-bit RGB non-interlaced PNG (filter 0 scanlines). */
static const unsigned char kat_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xc0,
    0x00, 0xc2, 0x0c, 0xff, 0x81, 0x00, 0x00, 0x1f, 0xee, 0x05, 0xfb, 0xf1,
    0xab, 0xba, 0x77, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82
};

/* 2x2 GIF87a, global color table, single image. */
static const unsigned char kat_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x02, 0x00, 0x02, 0x00, 0x81, 0x00,
    0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x02,
    0x03, 0x44, 0x34, 0x05, 0x00, 0x3b
};

/* Expected decoded pixels (0xAARRGGBB), row-major top-down. */
static const unsigned int kat_expect[4] = {
    0xFFFF0000u,   /* red   */
    0xFF00FF00u,   /* green */
    0xFF0000FFu,   /* blue  */
    0xFFFFFFFFu    /* white */
};

int imgcodec_selftest(void)
{
    unsigned int px[4];
    int w = 0, h = 0;
    int r;

    /* ---- BMP: the must-pass KAT (exact pixel comparison) ---- */
    px[0] = px[1] = px[2] = px[3] = 0;
    r = bmp_decode(kat_bmp, (unsigned long)sizeof(kat_bmp), px, 4, &w, &h);
    if (r != IMG_OK) return -1;
    if (w != 2 || h != 2) return -2;
    for (int i = 0; i < 4; i++)
        if (px[i] != kat_expect[i]) return -3;

    /* Also exercise auto-detect on the BMP. */
    px[0] = px[1] = px[2] = px[3] = 0;
    w = h = 0;
    r = img_decode(kat_bmp, (unsigned long)sizeof(kat_bmp), px, 4, &w, &h);
    if (r != IMG_OK || w != 2 || h != 2) return -4;
    for (int i = 0; i < 4; i++)
        if (px[i] != kat_expect[i]) return -5;

    /* ---- PNG KAT: dimensions + exact pixels (depends on DEFLATE codec) ---- */
    px[0] = px[1] = px[2] = px[3] = 0;
    w = h = 0;
    r = png_decode(kat_png, (unsigned long)sizeof(kat_png), px, 4, &w, &h);
    if (r != IMG_OK) return -6;
    if (w != 2 || h != 2) return -7;
    for (int i = 0; i < 4; i++)
        if (px[i] != kat_expect[i]) return -8;

    /* ---- GIF KAT: dimensions + exact pixels ---- */
    px[0] = px[1] = px[2] = px[3] = 0;
    w = h = 0;
    r = gif_decode(kat_gif, (unsigned long)sizeof(kat_gif), px, 4, &w, &h);
    if (r != IMG_OK) return -9;
    if (w != 2 || h != 2) return -10;
    for (int i = 0; i < 4; i++)
        if (px[i] != kat_expect[i]) return -11;

    return 0;   /* all KATs passed */
}
