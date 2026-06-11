/*
 * gif.c -- GIF decoder for AutomationOS imgcodec (freestanding).
 * =============================================================
 *
 * Supports GIF87a and GIF89a, single image (the FIRST image frame of an
 * animation). LZW-decompresses the image data and maps each index through
 * the active color table (local table if present, else the global table)
 * to 0xAARRGGBB. Honors the GIF89a graphic-control transparent index when
 * one is in effect for the first frame (transparent pixels get alpha 0).
 *
 * No libc: own memory primitives, FIXED static LZW tables, all loops
 * bounded, never writes past out_cap.
 *
 * Limitations (graceful negative return, never a crash):
 *   - Only the first image is decoded; subsequent frames are ignored.
 *   - Interlaced GIFs ARE handled (de-interlaced into row order).
 *   - No frame disposal / animation compositing.
 */

#include "imgcodec.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* LZW maximum is 12 bits => 4096 codes. */
#define GIF_MAX_CODES   4096
#define GIF_MAX_DIM     16384

/* Fixed LZW dictionary: each code maps to (prefix code, suffix byte).
 * Decoded strings are emitted via a bounded stack. */
static u16 lzw_prefix[GIF_MAX_CODES];
static u8  lzw_suffix[GIF_MAX_CODES];
static u8  lzw_stack[GIF_MAX_CODES];

/* Bit reader over GIF sub-blocks (LSB-first). Reads codes of `code_size`
 * bits across sub-block boundaries inside the image-data region. */
typedef struct {
    const u8 *data;        /* whole file                */
    unsigned long len;     /* file length               */
    unsigned long pos;     /* cursor (start of a block) */
    /* current sub-block window */
    unsigned long blk_pos; /* offset of current sub-block data byte    */
    u32           blk_rem; /* bytes remaining in current sub-block      */
    /* bit accumulator */
    u32           acc;
    int           bits;
    int           done;    /* hit the 0-length terminator               */
    int           error;
} gif_bits;

/* Load the next sub-block header; sets done when a 0-length block ends it. */
static void gif_next_block(gif_bits *b) {
    if (b->pos >= b->len) { b->done = 1; return; }
    u32 n = b->data[b->pos++];
    if (n == 0) { b->done = 1; return; }
    if (b->pos + n > b->len) { b->error = 1; b->done = 1; return; }
    b->blk_pos = b->pos;
    b->blk_rem = n;
    b->pos += n;
}

static void gif_bits_init(gif_bits *b, const u8 *data, unsigned long len,
                          unsigned long start) {
    b->data = data; b->len = len; b->pos = start;
    b->blk_rem = 0; b->acc = 0; b->bits = 0; b->done = 0; b->error = 0;
    gif_next_block(b);
}

/* Read `n` bits (n <= 12). Returns -1 at end of stream. */
static int gif_read_code(gif_bits *b, int n) {
    while (b->bits < n) {
        if (b->blk_rem == 0) {
            if (b->done) return -1;
            gif_next_block(b);
            if (b->done || b->error) {
                if (b->bits == 0) return -1;
                /* not enough bits but stream ended: pad with zeros */
                break;
            }
        }
        if (b->blk_rem > 0) {
            u8 byte = b->data[b->blk_pos++];
            b->blk_rem--;
            b->acc |= (u32)byte << b->bits;
            b->bits += 8;
        }
    }
    int code = (int)(b->acc & ((1u << n) - 1u));
    b->acc >>= n;
    b->bits -= n;
    if (b->bits < 0) b->bits = 0;
    return code;
}

/* be/le helper: GIF is little-endian. */
static u16 le16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }

int gif_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h)
{
    if (!data || !out || !w || !h) return IMG_ERR_CORRUPT;
    if (len < 13) return IMG_ERR_TRUNCATED;

    /* ---- header: "GIF87a" or "GIF89a" ---- */
    if (data[0]!='G' || data[1]!='I' || data[2]!='F' || data[3]!='8' ||
        (data[4]!='7' && data[4]!='9') || data[5]!='a')
        return IMG_ERR_BADMAGIC;

    /* ---- logical screen descriptor (7 bytes) ---- */
    u8 packed = data[10];
    int gct_flag = (packed >> 7) & 1;
    int gct_size = 2 << (packed & 7);                 /* entries */
    /* data[11] background color index, data[12] aspect ratio: unused */

    unsigned long pos = 13;

    /* ---- global color table ---- */
    u32 gct[256];
    int gct_n = 0;
    if (gct_flag) {
        gct_n = gct_size;
        if (pos + (unsigned long)gct_n * 3ul > len) return IMG_ERR_TRUNCATED;
        for (int i = 0; i < gct_n; i++) {
            const u8 *e = data + pos + (unsigned long)i * 3ul;
            gct[i] = 0xFF000000u | ((u32)e[0] << 16) | ((u32)e[1] << 8) | e[2];
        }
        pos += (unsigned long)gct_n * 3ul;
    }

    /* ---- scan blocks until the first image descriptor ----
     * Track a pending GIF89a transparent index from a Graphic Control Ext. */
    int transparent_index = -1;

    for (;;) {
        if (pos >= len) return IMG_ERR_CORRUPT;       /* no image found */
        u8 sep = data[pos++];

        if (sep == 0x3B) return IMG_ERR_CORRUPT;      /* trailer, no image */

        if (sep == 0x21) {                            /* extension */
            if (pos >= len) return IMG_ERR_TRUNCATED;
            u8 label = data[pos++];
            if (label == 0xF9) {                      /* graphic control */
                if (pos >= len) return IMG_ERR_TRUNCATED;
                u8 bsize = data[pos++];
                if (bsize >= 4 && pos + 4 <= len) {
                    u8 gce_packed = data[pos];
                    if (gce_packed & 0x01)            /* transparency flag */
                        transparent_index = data[pos + 3];
                }
                /* skip this block's data then its sub-block list */
                pos += bsize;
            }
            /* skip remaining sub-blocks of any extension */
            while (pos < len) {
                u8 n = data[pos++];
                if (n == 0) break;
                if (pos + n > len) return IMG_ERR_TRUNCATED;
                pos += n;
            }
            continue;
        }

        if (sep == 0x2C) {                            /* image descriptor */
            break;
        }

        return IMG_ERR_CORRUPT;                        /* unknown separator */
    }

    /* ---- image descriptor (10 bytes after the 0x2C separator) ---- */
    if (pos + 9 > len) return IMG_ERR_TRUNCATED;
    /* left/top at +0..+3 ignored (we render the frame at origin) */
    u16 iw = le16(data + pos + 4);
    u16 ih = le16(data + pos + 6);
    u8  ipacked = data[pos + 8];
    pos += 9;

    int lct_flag = (ipacked >> 7) & 1;
    int interlace = (ipacked >> 6) & 1;
    int lct_size = 2 << (ipacked & 7);

    if (iw == 0 || ih == 0) return IMG_ERR_DIMENSIONS;
    if (iw > GIF_MAX_DIM || ih > GIF_MAX_DIM) return IMG_ERR_TOOBIG;

    u32 width = iw, height = ih;
    u32 npix = width * height;
    if ((unsigned long)npix > out_cap) return IMG_ERR_TOOBIG;

    /* ---- choose active color table ---- */
    u32 lct[256];
    const u32 *ct;
    int ct_n;
    if (lct_flag) {
        ct_n = lct_size;
        if (pos + (unsigned long)ct_n * 3ul > len) return IMG_ERR_TRUNCATED;
        for (int i = 0; i < ct_n; i++) {
            const u8 *e = data + pos + (unsigned long)i * 3ul;
            lct[i] = 0xFF000000u | ((u32)e[0] << 16) | ((u32)e[1] << 8) | e[2];
        }
        pos += (unsigned long)ct_n * 3ul;
        ct = lct;
    } else {
        if (!gct_flag) return IMG_ERR_CORRUPT;        /* no usable table */
        ct = gct;
        ct_n = gct_n;
    }

    /* ---- LZW minimum code size, then the image-data sub-blocks ---- */
    if (pos >= len) return IMG_ERR_TRUNCATED;
    int min_code_size = data[pos++];
    if (min_code_size < 2 || min_code_size > 11) return IMG_ERR_CORRUPT;

    int clear_code = 1 << min_code_size;
    int end_code   = clear_code + 1;

    gif_bits b;
    gif_bits_init(&b, data, len, pos);

    /* ---- decode loop ---- */
    int code_size = min_code_size + 1;
    int next_code = end_code + 1;
    int max_code  = (1 << code_size) - 1;

    /* initialize single-byte dictionary entries */
    for (int i = 0; i < clear_code; i++) {
        lzw_prefix[i] = 0xFFFF;                       /* sentinel: no prefix */
        lzw_suffix[i] = (u8)i;
    }

    int sp = 0;                 /* output stack pointer */
    int old_code = -1;
    int first_byte = 0;

    /* output cursor: write de-interlaced if needed */
    static const int il_start[4] = { 0, 4, 2, 1 };
    static const int il_step[4]  = { 8, 8, 4, 2 };
    int il_pass = 0;
    u32 out_x = 0, out_y = 0;
    unsigned long written = 0;

    /* helper macro replaced by inline emit of one index */
    /* We decode codes into a stack (reverse), then flush in order. */

    for (;;) {
        if (written >= npix) break;                    /* image filled */
        int code = gif_read_code(&b, code_size);
        if (code < 0) break;                           /* end of stream */

        if (code == clear_code) {
            code_size = min_code_size + 1;
            max_code  = (1 << code_size) - 1;
            next_code = end_code + 1;
            old_code  = -1;
            continue;
        }
        if (code == end_code) break;

        if (old_code < 0) {
            /* first code after a clear: must be a literal */
            if (code >= clear_code) return IMG_ERR_CORRUPT;
            first_byte = lzw_suffix[code];
            if (sp >= GIF_MAX_CODES) return IMG_ERR_CORRUPT;   /* guard like 282/288 */
            lzw_stack[sp++] = (u8)first_byte;
            old_code = code;
        } else {
            int in_code = code;
            if (code >= next_code) {
                /* KwKwK case: emit previous string + its first byte */
                if (sp >= GIF_MAX_CODES) return IMG_ERR_CORRUPT;   /* guard like 282/288 */
                lzw_stack[sp++] = (u8)first_byte;
                code = old_code;
            }
            /* walk the prefix chain, pushing suffixes (bounded).
             * `code` must stay a valid, already-defined dictionary index. A
             * prefix that is out of range (e.g. the 0xFFFF literal sentinel,
             * or a stale entry left over from a prior decode with a larger
             * clear_code) means the stream is corrupt -- bail instead of
             * indexing lzw_suffix[]/lzw_prefix[] out of bounds. */
            int guard = 0;
            while (code >= clear_code) {
                if (code >= GIF_MAX_CODES) return IMG_ERR_CORRUPT;
                if (sp >= GIF_MAX_CODES) return IMG_ERR_CORRUPT;
                lzw_stack[sp++] = lzw_suffix[code];
                code = lzw_prefix[code];
                if (++guard > GIF_MAX_CODES) return IMG_ERR_CORRUPT;
            }
            first_byte = lzw_suffix[code];
            if (sp >= GIF_MAX_CODES) return IMG_ERR_CORRUPT;
            lzw_stack[sp++] = (u8)first_byte;

            /* add new dictionary entry: old_code + first_byte */
            if (next_code < GIF_MAX_CODES) {
                lzw_prefix[next_code] = (u16)old_code;
                lzw_suffix[next_code] = (u8)first_byte;
                next_code++;
                if (next_code > max_code && code_size < 12) {
                    code_size++;
                    max_code = (1 << code_size) - 1;
                }
            }
            old_code = in_code;
        }

        /* flush stack (reverse order) to the output image */
        while (sp > 0) {
            if (written >= npix) { sp = 0; break; }
            u8 idx = lzw_stack[--sp];

            u32 argb;
            if (idx == transparent_index)
                argb = 0x00000000u;                    /* transparent */
            else if (idx < ct_n)
                argb = ct[idx];
            else
                argb = 0xFF000000u;                    /* out-of-range -> black */

            /* compute destination (handle interlace) */
            if (interlace) {
                /* out_y / out_x walk in interlaced pass order */
                unsigned long di = (unsigned long)out_y * width + out_x;
                if (di < npix) out[di] = argb;
                out_x++;
                if (out_x >= width) {
                    out_x = 0;
                    out_y += il_step[il_pass];
                    while (out_y >= height && il_pass < 3) {
                        il_pass++;
                        out_y = il_start[il_pass];
                    }
                }
            } else {
                out[written] = argb;
            }
            written++;
        }
    }

    /* If the stream ended early, pad the remainder with transparent black
     * so the caller never reads uninitialized pixels. */
    while (written < npix) {
        if (interlace) {
            unsigned long di = (unsigned long)out_y * width + out_x;
            if (di < npix) out[di] = 0x00000000u;
            out_x++;
            if (out_x >= width) {
                out_x = 0;
                out_y += il_step[il_pass];
                while (out_y >= height && il_pass < 3) {
                    il_pass++;
                    out_y = il_start[il_pass];
                }
            }
        } else {
            out[written] = 0x00000000u;
        }
        written++;
    }

    *w = (int)width;
    *h = (int)height;
    return IMG_OK;
}
