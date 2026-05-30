/*
 * imgcodec.h -- freestanding image-decoder library for AutomationOS.
 * =================================================================
 *
 * Userspace, ring-3, PURE decoding. No libc / stdio / malloc / standard
 * headers. All scratch buffers are fixed-size or caller-provided; the
 * library brings its own memcpy/memset. Every loop is bounded and no
 * decoder ever writes past the caller's out_cap.
 *
 * Decodes into a caller-provided 32-bit pixel buffer. Each pixel is
 * 0xAARRGGBB (alpha in the high byte). Opaque sources get alpha = 0xFF.
 *
 * Build flags (objdump must show NO fs:0x28 stack canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Supported formats / modes (see the .c files for the authoritative list):
 *   BMP : uncompressed 24-bit and 32-bit BITMAPINFOHEADER (top-down and
 *         bottom-up), plus 8-bit palettized. Fully correct.
 *   PNG : 8-bit grayscale / grayscale+alpha / RGB / RGBA, non-interlaced.
 *         Filters None/Sub/Up/Average/Paeth. IDAT is zlib-wrapped DEFLATE.
 *         Unsupported (graceful error): interlaced (Adam7), 16-bit depth,
 *         palette (color type 3), bit depths < 8.
 *   GIF : GIF87a / GIF89a, single image (first frame only for animations).
 *         LZW decompress, global or local color table, transparent index.
 *
 * Return convention for all decoders:
 *   0            success (*w,*h set; out[0..w*h) written)
 *   negative     error (bad data, unsupported mode, or output too small)
 */

#ifndef IMGCODEC_H
#define IMGCODEC_H

/* Simple width/height descriptor. */
typedef struct {
    int w;
    int h;
} img_info;

/* Negative error codes shared by all decoders. */
#define IMG_OK              0
#define IMG_ERR_BADMAGIC   (-1)   /* signature did not match            */
#define IMG_ERR_TRUNCATED  (-2)   /* input ended before a needed field  */
#define IMG_ERR_UNSUPPORTED (-3)  /* valid file, mode we do not decode  */
#define IMG_ERR_TOOBIG     (-4)   /* w*h exceeds out_cap                */
#define IMG_ERR_CORRUPT    (-5)   /* malformed / inconsistent data      */
#define IMG_ERR_DECOMPRESS (-6)   /* inflate / LZW failure              */
#define IMG_ERR_DIMENSIONS (-7)   /* zero/negative/insane dimensions    */

/*
 * Auto-detect BMP / PNG / GIF by signature and decode into `out`
 * (out_cap pixels, each 0xAARRGGBB). Sets *w and *h on success.
 * Returns 0 on success, negative on error / unsupported / too big.
 */
int img_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h);

/* Individual decoders. Each detects its own magic before decoding. */
int bmp_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h);
int png_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h);
int gif_decode(const unsigned char *data, unsigned long len,
               unsigned int *out, unsigned long out_cap, int *w, int *h);

/*
 * Built-in known-answer self test. Decodes embedded tiny images and
 * checks dimensions / pixel values. Returns 0 only if all KATs pass
 * (the embedded BMP is the must-pass case). Negative on failure.
 */
int imgcodec_selftest(void);

#endif /* IMGCODEC_H */
