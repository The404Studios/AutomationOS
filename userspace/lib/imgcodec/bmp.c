/*
 * bmp.c -- BMP decoder for AutomationOS imgcodec (freestanding).
 * =============================================================
 *
 * Fully correct support for uncompressed (BI_RGB) BITMAPINFOHEADER BMPs:
 *   - 24-bit  (BGR)
 *   - 32-bit  (BGRA / BGRX)
 *   - 8-bit   palettized (BGRX palette, up to 256 entries)
 * Both bottom-up (positive height) and top-down (negative height).
 *
 * Output pixels are 0xAARRGGBB. 24-bit and 8-bit sources are opaque
 * (alpha forced to 0xFF). 32-bit sources keep their stored alpha byte.
 *
 * No libc: own memory primitives, fixed palette buffer, all loops bounded.
 *
 * Deliberately NOT supported (return negative, never crash):
 *   BITMAPCOREHEADER (12-byte), RLE4/RLE8 compression, bitfields (BI_BITFIELDS),
 *   1/4-bit indexed, 16-bit. These return IMG_ERR_UNSUPPORTED.
 */

#include "imgcodec.h"

/* ---- local fixed-width types (no <stdint.h> in freestanding) ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef int                i32;

/* Cap on total pixels we are ever willing to address, independent of the
 * caller's out_cap. Guards multiplication overflow on w*h. ~67 Mpx. */
#define BMP_MAX_PIXELS   (1u << 26)
#define BMP_MAX_DIM      32768       /* generous per-axis sanity bound  */

/* ---- little-endian readers (BMP is always little-endian on disk) ---- */
static u16 rd16(const u8 *p) {
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static i32 rd32s(const u8 *p) {
    return (i32)rd32(p);
}

int bmp_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h)
{
    if (!data || !out || !w || !h) return IMG_ERR_CORRUPT;
    if (len < 54) return IMG_ERR_TRUNCATED;             /* 14 file + 40 info */

    /* ---- BITMAPFILEHEADER (14 bytes) ---- */
    if (data[0] != 'B' || data[1] != 'M') return IMG_ERR_BADMAGIC;
    u32 pixel_off = rd32(data + 10);

    /* ---- DIB header ---- */
    u32 dib_size = rd32(data + 14);
    if (dib_size < 40) return IMG_ERR_UNSUPPORTED;      /* need BITMAPINFOHEADER+ */

    i32 bw  = rd32s(data + 18);
    i32 bh  = rd32s(data + 22);
    u16 planes = rd16(data + 26);
    u16 bpp = rd16(data + 28);
    u32 comp = rd32(data + 30);
    u32 ncolors = rd32(data + 46);

    if (planes != 1) return IMG_ERR_UNSUPPORTED;
    if (comp != 0)   return IMG_ERR_UNSUPPORTED;        /* only BI_RGB */
    if (bpp != 8 && bpp != 24 && bpp != 32) return IMG_ERR_UNSUPPORTED;

    /* Dimensions: width must be positive; height may be negative (top-down).
     * Negate the height magnitude in u32 to avoid signed-overflow UB when bh
     * is INT_MIN (-bh is undefined in int); the magnitude is then range-checked
     * against BMP_MAX_DIM below and rejected if absurd. */
    int top_down = 0;
    u32 width, height;
    if (bw <= 0) return IMG_ERR_DIMENSIONS;
    if (bh < 0) { top_down = 1; height = -(u32)bh; }
    else        { height = (u32)bh; }
    if (height == 0) return IMG_ERR_DIMENSIONS;
    width = (u32)bw;
    if (width > BMP_MAX_DIM || height > BMP_MAX_DIM) return IMG_ERR_TOOBIG;

    u32 npix   = width * height;
    if (npix > BMP_MAX_PIXELS) return IMG_ERR_TOOBIG;
    if ((unsigned long)npix > out_cap) return IMG_ERR_TOOBIG;

    /* ---- palette (8-bit only) ---- */
    /* Palette sits right after the DIB header. Each entry is BGRX (4 bytes). */
    u32 palette[256];
    u32 pal_count = 0;
    if (bpp == 8) {
        pal_count = ncolors ? ncolors : 256;
        if (pal_count > 256) pal_count = 256;
        unsigned long pal_off = 14ul + dib_size;
        if (pal_off + (unsigned long)pal_count * 4ul > len) return IMG_ERR_TRUNCATED;
        for (u32 i = 0; i < pal_count; i++) {
            const u8 *e = data + pal_off + (unsigned long)i * 4ul;
            u32 b = e[0], g = e[1], r = e[2];
            palette[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        /* Fill any unused slots with opaque black so bad indices are safe. */
        for (u32 i = pal_count; i < 256; i++) palette[i] = 0xFF000000u;
    }

    /* ---- row stride: rows are padded to a 4-byte boundary ---- */
    u32 row_bytes  = (width * (u32)bpp + 7u) / 8u;
    u32 row_stride = (row_bytes + 3u) & ~3u;

    /* Verify the pixel array fits within the input. */
    unsigned long need = (unsigned long)pixel_off
                       + (unsigned long)row_stride * (unsigned long)height;
    if (pixel_off >= len) return IMG_ERR_TRUNCATED;
    if (need > len) return IMG_ERR_TRUNCATED;

    /* ---- decode rows into top-down ARGB ---- */
    for (u32 y = 0; y < height; y++) {
        /* Source row: bottom-up stores last image row first. */
        u32 src_row = top_down ? y : (height - 1u - y);
        const u8 *row = data + pixel_off
                      + (unsigned long)src_row * (unsigned long)row_stride;
        unsigned int *dst = out + (unsigned long)y * (unsigned long)width;

        if (bpp == 24) {
            for (u32 x = 0; x < width; x++) {
                const u8 *px = row + (unsigned long)x * 3ul;
                u32 b = px[0], g = px[1], r = px[2];
                dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        } else if (bpp == 32) {
            for (u32 x = 0; x < width; x++) {
                const u8 *px = row + (unsigned long)x * 4ul;
                u32 b = px[0], g = px[1], r = px[2], a = px[3];
                dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        } else { /* bpp == 8, palettized */
            for (u32 x = 0; x < width; x++) {
                u8 idx = row[x];
                dst[x] = palette[idx];
            }
        }
    }

    *w = (int)width;
    *h = (int)height;
    return IMG_OK;
}
