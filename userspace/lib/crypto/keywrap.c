/*
 * keywrap.c -- freestanding AES Key Wrap / Unwrap (RFC 3394).
 * ============================================================
 *
 * No libc, no syscalls, no malloc, no standard headers.
 * Fixed stack buffers only.
 *
 * Implements:
 *   - aes_key_wrap   -- RFC 3394 §2.2.1 key wrap (the 6n-step juggling)
 *   - aes_key_unwrap -- RFC 3394 §2.2.2 inverse, with integrity check
 *   - keywrap_selftest -- RFC 3394 §4.1 known-answer vector (128/128) plus
 *                         a round-trip and a tamper-rejection check
 *
 * Used for EAPOL-Key GTK delivery in WPA2/WPA3 (the GTK is wrapped under
 * the KEK portion of the PTK).
 *
 * Build flags:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#include "keywrap.h"
#include "aes.h"        /* aes_ctx, aes_set_encrypt_key/decrypt_key,
                           aes_encrypt_block/decrypt_block */

/* =========================================================================
 * Utility helpers (no libc)
 * ====================================================================== */

static void kw_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static int kw_memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

/*
 * Maximum number of 64-bit blocks we will wrap in one call.  WPA GTK
 * delivery wraps at most a handful of blocks (the GTK plus key-data
 * padding stays well under this), so a fixed 64-block (512-byte) ceiling
 * is generous and keeps everything on the stack with no allocation.
 */
#define KW_MAX_BLOCKS   64

/* Default integrity check value (RFC 3394 §2.2.3.1): A6A6A6A6A6A6A6A6. */
static const unsigned char KW_DEFAULT_IV[8] = {
    0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6
};

/* =========================================================================
 * aes_key_wrap (RFC 3394 §2.2.1)
 *
 *   Set A = IV, R[i] = P[i]  (i = 1..n)
 *   For j = 0..5:
 *     For i = 1..n:
 *       B    = AES-ENC(KEK, A | R[i])      (16-byte block: A || R[i])
 *       A    = MSB64(B) ^ t                (t = (n*j)+i, big-endian)
 *       R[i] = LSB64(B)
 *   C[0] = A; C[i] = R[i]
 * ====================================================================== */

int aes_key_wrap(const uint8_t *kek, int kek_bits,
                 const uint8_t *in, int n_blocks, uint8_t *out)
{
    aes_ctx       ctx;
    unsigned char a[8];               /* integrity register A            */
    unsigned char r[KW_MAX_BLOCKS][8];/* R[1..n] working registers       */
    unsigned char block[16];          /* A | R[i] fed to AES             */
    int           i, j, k;
    unsigned long t;                  /* counter (n*j)+i                 */

    if (!kek || !in || !out)          return -1;
    if (n_blocks < 1)                 return -1;
    if (n_blocks > KW_MAX_BLOCKS)     return -1;
    if (kek_bits != 128 && kek_bits != 192 && kek_bits != 256) return -1;

    aes_set_encrypt_key(&ctx, kek, kek_bits);

    /* Initialise A = IV and R[i] = P[i]. */
    kw_memcpy(a, KW_DEFAULT_IV, 8);
    for (i = 0; i < n_blocks; i++)
        kw_memcpy(r[i], in + (unsigned long)i * 8, 8);

    /* 6n-step juggling. */
    for (j = 0; j <= 5; j++) {
        for (i = 1; i <= n_blocks; i++) {
            /* B = AES-ENC(KEK, A | R[i]). */
            kw_memcpy(block,     a,       8);
            kw_memcpy(block + 8, r[i - 1], 8);
            aes_encrypt_block(&ctx, block, block);

            /* A = MSB64(B) ^ t, t = (n*j) + i (64-bit, big-endian). */
            t = (unsigned long)n_blocks * (unsigned long)j + (unsigned long)i;
            kw_memcpy(a, block, 8);   /* MSB64(B) */
            for (k = 0; k < 8; k++) {
                /* XOR t into A, most-significant counter byte first. */
                unsigned char tb =
                    (unsigned char)((t >> (8 * (7 - k))) & 0xff);
                a[k] ^= tb;
            }

            /* R[i] = LSB64(B). */
            kw_memcpy(r[i - 1], block + 8, 8);
        }
    }

    /* Output: C[0] = A, C[i] = R[i]. */
    kw_memcpy(out, a, 8);
    for (i = 0; i < n_blocks; i++)
        kw_memcpy(out + 8 + (unsigned long)i * 8, r[i], 8);

    return 0;
}

/* =========================================================================
 * aes_key_unwrap (RFC 3394 §2.2.2)
 *
 *   Set A = C[0], R[i] = C[i]  (i = 1..n)
 *   For j = 5..0:
 *     For i = n..1:
 *       t    = (n*j) + i                   (big-endian)
 *       B    = AES-DEC(KEK, (A ^ t) | R[i])
 *       A    = MSB64(B)
 *       R[i] = LSB64(B)
 *   If A == IV:  P[i] = R[i]  (success)
 *   Else:        integrity failure
 * ====================================================================== */

int aes_key_unwrap(const uint8_t *kek, int kek_bits,
                   const uint8_t *in, int n_blocks, uint8_t *out)
{
    aes_ctx       ctx;
    unsigned char a[8];
    unsigned char r[KW_MAX_BLOCKS][8];
    unsigned char block[16];
    int           i, j, k;
    unsigned long t;

    if (!kek || !in || !out)          return -1;
    if (n_blocks < 1)                 return -1;
    if (n_blocks > KW_MAX_BLOCKS)     return -1;
    if (kek_bits != 128 && kek_bits != 192 && kek_bits != 256) return -1;

    aes_set_decrypt_key(&ctx, kek, kek_bits);

    /* Initialise A = C[0], R[i] = C[i]. */
    kw_memcpy(a, in, 8);
    for (i = 0; i < n_blocks; i++)
        kw_memcpy(r[i], in + 8 + (unsigned long)i * 8, 8);

    /* Reverse the 6n-step juggling. */
    for (j = 5; j >= 0; j--) {
        for (i = n_blocks; i >= 1; i--) {
            t = (unsigned long)n_blocks * (unsigned long)j + (unsigned long)i;

            /* B = AES-DEC(KEK, (A ^ t) | R[i]). */
            kw_memcpy(block, a, 8);
            for (k = 0; k < 8; k++) {
                unsigned char tb =
                    (unsigned char)((t >> (8 * (7 - k))) & 0xff);
                block[k] ^= tb;
            }
            kw_memcpy(block + 8, r[i - 1], 8);
            aes_decrypt_block(&ctx, block, block);

            /* A = MSB64(B); R[i] = LSB64(B). */
            kw_memcpy(a,        block,     8);
            kw_memcpy(r[i - 1], block + 8, 8);
        }
    }

    /* Integrity check: recovered A must equal the default IV. */
    if (kw_memcmp(a, KW_DEFAULT_IV, 8) != 0)
        return -1;

    /* P[i] = R[i]. */
    for (i = 0; i < n_blocks; i++)
        kw_memcpy(out + (unsigned long)i * 8, r[i], 8);

    return 0;
}

/* =========================================================================
 * Self-test: RFC 3394 §4.1
 *
 *   "Wrap 128 bits of Key Data with a 128-bit KEK"
 *
 *   KEK        = 000102030405060708090A0B0C0D0E0F
 *   Key Data   = 00112233445566778899AABBCCDDEEFF
 *   Ciphertext = 1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5
 *
 * Checks:
 *   (a) wrap(Key Data) == Ciphertext
 *   (b) unwrap(Ciphertext) == Key Data, returns 0
 *   (c) unwrap(corrupted Ciphertext) returns -1 (integrity must fail)
 * ====================================================================== */

int keywrap_selftest(void)
{
    static const unsigned char kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
    };
    static const unsigned char pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF
    };
    static const unsigned char wrapped[24] = {
        0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,
        0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
        0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5
    };

    unsigned char out[24];   /* wrap output:   (2+1)*8 = 24 bytes */
    unsigned char back[16];  /* unwrap output:  2*8    = 16 bytes */
    unsigned char bad[24];
    int i;

    /* (a) Wrap PT -> must equal the known Ciphertext. */
    if (aes_key_wrap(kek, 128, pt, 2, out) != 0)        return -1;
    if (kw_memcmp(out, wrapped, 24) != 0)               return -1;

    /* (b) Unwrap Ciphertext -> must equal PT and return 0. */
    if (aes_key_unwrap(kek, 128, wrapped, 2, back) != 0) return -1;
    if (kw_memcmp(back, pt, 16) != 0)                    return -1;

    /* (c) Corrupt one ciphertext byte -> unwrap must reject (-1). */
    kw_memcpy(bad, wrapped, 24);
    bad[10] ^= 0x01;                                     /* flip a bit */
    if (aes_key_unwrap(kek, 128, bad, 2, back) != -1)    return -1;

    /* Extra: also confirm tamper in the integrity register A is caught. */
    kw_memcpy(bad, wrapped, 24);
    bad[0] ^= 0x80;                                      /* flip A[0]  */
    if (aes_key_unwrap(kek, 128, bad, 2, back) != -1)    return -1;

    /* Suppress unused-variable warnings under aggressive optimisation. */
    for (i = 0; i < 0; i++) { /* no-op */ }

    return 0; /* all checks passed */
}
