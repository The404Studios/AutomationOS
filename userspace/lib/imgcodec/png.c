/*
 * png.c -- PNG decoder for AutomationOS imgcodec (freestanding).
 * =============================================================
 *
 * Common case only, but fully correct for it:
 *   - bit depth 8, non-interlaced
 *   - color types: 0 (grayscale), 2 (RGB), 4 (grayscale+alpha), 6 (RGBA)
 *   - filter types per scanline: 0 None, 1 Sub, 2 Up, 3 Average, 4 Paeth
 *
 * Pipeline: parse PNG signature -> walk chunks -> concatenate every IDAT
 * payload into a fixed scratch buffer -> the concatenated bytes are a
 * zlib stream (RFC 1950): skip the 2-byte zlib header (and the trailing
 * 4-byte Adler32 is simply ignored) -> raw DEFLATE inflate via the
 * existing codec -> reverse the per-scanline filtering -> expand to
 * 0xAARRGGBB into the caller's buffer.
 *
 * Unsupported (graceful negative return, never a crash):
 *   - interlaced (Adam7)            -> IMG_ERR_UNSUPPORTED
 *   - 16-bit sample depth           -> IMG_ERR_UNSUPPORTED
 *   - palette color type 3          -> IMG_ERR_UNSUPPORTED
 *   - bit depths 1/2/4             -> IMG_ERR_UNSUPPORTED
 *
 * No libc: own memory primitives, FIXED static scratch buffers, all loops
 * bounded, never writes past out_cap. CRC verification is intentionally
 * skipped (optional per the task).
 */

#include "imgcodec.h"
#include "../deflate/deflate.h"

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef int            i32;

/* ---- fixed scratch sizing ----
 * Compressed IDAT scratch and the inflated raw-scanline scratch are both
 * static and fixed. These bound the largest PNG we will decode. The raw
 * buffer must hold height * (1 + width*bytes_per_pixel) bytes. With 4 MiB
 * each, this comfortably covers e.g. 1024x1024 RGBA (~4.2 MB raw) and
 * smaller. Anything larger returns IMG_ERR_TOOBIG rather than overrunning. */
#define PNG_IDAT_CAP   (4u * 1024u * 1024u)   /* concatenated IDAT bytes */
#define PNG_RAW_CAP    (4u * 1024u * 1024u)   /* inflated filtered bytes */
#define PNG_MAX_DIM    8192

static u8 png_idat[PNG_IDAT_CAP];
static u8 png_raw[PNG_RAW_CAP];

/* ---- big-endian readers (PNG is network byte order) ---- */
static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* Paeth predictor (PNG spec). a=left, b=above, c=upper-left. */
static int paeth(int a, int b, int c) {
    int p  = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int png_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h)
{
    if (!data || !out || !w || !h) return IMG_ERR_CORRUPT;
    if (len < 8) return IMG_ERR_TRUNCATED;

    /* ---- 8-byte PNG signature ---- */
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    for (int i = 0; i < 8; i++)
        if (data[i] != sig[i]) return IMG_ERR_BADMAGIC;

    /* ---- walk chunks ---- */
    unsigned long pos = 8;
    u32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    int seen_ihdr = 0, seen_iend = 0;
    unsigned long idat_len = 0;

    while (pos + 8 <= len) {
        u32 clen = be32(data + pos);
        const u8 *ctype = data + pos + 4;
        unsigned long cdata = pos + 8;
        /* Guard: chunk data + 4-byte CRC must fit. */
        if (cdata + (unsigned long)clen + 4ul > len) return IMG_ERR_TRUNCATED;

        if (ctype[0]=='I' && ctype[1]=='H' && ctype[2]=='D' && ctype[3]=='R') {
            if (clen < 13) return IMG_ERR_CORRUPT;
            width      = be32(data + cdata);
            height     = be32(data + cdata + 4);
            bit_depth  = data[cdata + 8];
            color_type = data[cdata + 9];
            interlace  = data[cdata + 12];
            seen_ihdr  = 1;
        } else if (ctype[0]=='I' && ctype[1]=='D' && ctype[2]=='A' && ctype[3]=='T') {
            if (idat_len + (unsigned long)clen > PNG_IDAT_CAP) return IMG_ERR_TOOBIG;
            const u8 *src = data + cdata;
            for (u32 i = 0; i < clen; i++) png_idat[idat_len + i] = src[i];
            idat_len += clen;
        } else if (ctype[0]=='I' && ctype[1]=='E' && ctype[2]=='N' && ctype[3]=='D') {
            seen_iend = 1;
            break;
        }
        /* advance: 4 len + 4 type + data + 4 crc */
        pos = cdata + (unsigned long)clen + 4ul;
    }

    if (!seen_ihdr) return IMG_ERR_CORRUPT;
    (void)seen_iend;

    /* ---- validate the supported subset ---- */
    if (interlace != 0)  return IMG_ERR_UNSUPPORTED;   /* no Adam7 */
    if (bit_depth != 8)  return IMG_ERR_UNSUPPORTED;   /* 8-bit only */
    if (color_type != 0 && color_type != 2 &&
        color_type != 4 && color_type != 6) return IMG_ERR_UNSUPPORTED;

    if (width == 0 || height == 0) return IMG_ERR_DIMENSIONS;
    if (width > PNG_MAX_DIM || height > PNG_MAX_DIM) return IMG_ERR_TOOBIG;

    u32 npix = width * height;
    if ((unsigned long)npix > out_cap) return IMG_ERR_TOOBIG;

    /* bytes per pixel of the *raw* (pre-expansion) samples */
    u32 bpp;
    switch (color_type) {
        case 0: bpp = 1; break;   /* gray        */
        case 2: bpp = 3; break;   /* RGB         */
        case 4: bpp = 2; break;   /* gray+alpha  */
        case 6: bpp = 4; break;   /* RGBA        */
        default: return IMG_ERR_UNSUPPORTED;
    }

    /* raw filtered size = height * (1 filter byte + width*bpp) */
    unsigned long stride = (unsigned long)width * (unsigned long)bpp;
    unsigned long raw_need = (unsigned long)height * (stride + 1ul);
    if (raw_need > PNG_RAW_CAP) return IMG_ERR_TOOBIG;

    if (idat_len < 2) return IMG_ERR_CORRUPT;          /* need zlib header */

    /* ---- skip the 2-byte zlib (RFC 1950) header, then raw inflate ----
     * The 4-byte trailing Adler32 is harmless to the raw inflate (it stops
     * at the final DEFLATE block), so we simply leave it in the input. */
    const u8  *deflate_in  = png_idat + 2;
    unsigned long deflate_len = idat_len - 2;

    long got = inflate_decompress(deflate_in, (long)deflate_len,
                                  png_raw, (long)PNG_RAW_CAP);
    if (got < 0) return IMG_ERR_DECOMPRESS;
    if ((unsigned long)got < raw_need) return IMG_ERR_CORRUPT;

    /* ---- reverse per-scanline filtering, in place in png_raw ----
     * We unfilter into the same buffer's image region. To keep "above"
     * references correct we unfilter row by row; each row's filtered bytes
     * are immediately replaced by their reconstructed values. */
    for (u32 y = 0; y < height; y++) {
        unsigned long row_off = (unsigned long)y * (stride + 1ul);
        u8 filter = png_raw[row_off];
        u8 *cur = png_raw + row_off + 1;                  /* this scanline   */
        u8 *prev = (y == 0) ? 0
                            : png_raw + (unsigned long)(y - 1) * (stride + 1ul) + 1;

        switch (filter) {
            case 0: /* None */
                break;
            case 1: /* Sub: cur[i] += cur[i-bpp] */
                for (unsigned long i = 0; i < stride; i++) {
                    u8 a = (i >= bpp) ? cur[i - bpp] : 0;
                    cur[i] = (u8)(cur[i] + a);
                }
                break;
            case 2: /* Up: cur[i] += prev[i] */
                if (prev) {
                    for (unsigned long i = 0; i < stride; i++)
                        cur[i] = (u8)(cur[i] + prev[i]);
                }
                break;
            case 3: /* Average: cur[i] += (a + b) / 2 */
                for (unsigned long i = 0; i < stride; i++) {
                    int a = (i >= bpp) ? cur[i - bpp] : 0;
                    int b = prev ? prev[i] : 0;
                    cur[i] = (u8)(cur[i] + ((a + b) >> 1));
                }
                break;
            case 4: /* Paeth */
                for (unsigned long i = 0; i < stride; i++) {
                    int a = (i >= bpp) ? cur[i - bpp] : 0;
                    int b = prev ? prev[i] : 0;
                    int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
                    cur[i] = (u8)(cur[i] + paeth(a, b, c));
                }
                break;
            default:
                return IMG_ERR_CORRUPT;                    /* unknown filter */
        }
    }

    /* ---- expand reconstructed samples to 0xAARRGGBB ---- */
    for (u32 y = 0; y < height; y++) {
        unsigned long row_off = (unsigned long)y * (stride + 1ul) + 1ul;
        const u8 *cur = png_raw + row_off;
        unsigned int *dst = out + (unsigned long)y * (unsigned long)width;

        if (color_type == 0) {            /* grayscale */
            for (u32 x = 0; x < width; x++) {
                u32 g = cur[x];
                dst[x] = 0xFF000000u | (g << 16) | (g << 8) | g;
            }
        } else if (color_type == 2) {     /* RGB */
            for (u32 x = 0; x < width; x++) {
                const u8 *p = cur + (unsigned long)x * 3ul;
                dst[x] = 0xFF000000u | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
        } else if (color_type == 4) {     /* gray + alpha */
            for (u32 x = 0; x < width; x++) {
                const u8 *p = cur + (unsigned long)x * 2ul;
                u32 g = p[0], a = p[1];
                dst[x] = (a << 24) | (g << 16) | (g << 8) | g;
            }
        } else {                          /* color_type == 6, RGBA */
            for (u32 x = 0; x < width; x++) {
                const u8 *p = cur + (unsigned long)x * 4ul;
                dst[x] = ((u32)p[3] << 24) | ((u32)p[0] << 16)
                       | ((u32)p[1] << 8) | p[2];
            }
        }
    }

    *w = (int)width;
    *h = (int)height;
    return IMG_OK;
}
