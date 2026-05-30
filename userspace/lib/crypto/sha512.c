/*
 * sha512.c -- freestanding SHA-512 / SHA-384 (FIPS 180-4).
 * =========================================================
 * No libc, no headers other than our own sha512.h.
 * 64-bit words, 80-round compression, 128-byte (1024-bit) blocks.
 * Message length appended as 128-bit big-endian (tracked via len_lo/len_hi).
 *
 * Compile with:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#include "sha512.h"

/* =========================================================================
 * Local memory helpers (no libc).
 * ========================================================================= */

static void s5_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static void s5_memset(void *dst, int val, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)val;
}

static int s5_memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

/* =========================================================================
 * SHA-512 round constants K[0..79] (FIPS 180-4 §4.2.3).
 * First 64 bits of the fractional parts of the cube roots of the first
 * 80 prime numbers.
 * ========================================================================= */

static const unsigned long long K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

/* =========================================================================
 * Bit-manipulation macros (64-bit).
 * All names prefixed with S5_ to avoid collisions if both sha256 and sha512
 * translation units are linked together.
 * ========================================================================= */

#define S5_ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

#define S5_CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define S5_MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* SHA-512 Σ0, Σ1 (capital sigma, mix of state words) */
#define S5_BSIG0(x) (S5_ROR64(x, 28) ^ S5_ROR64(x, 34) ^ S5_ROR64(x, 39))
#define S5_BSIG1(x) (S5_ROR64(x, 14) ^ S5_ROR64(x, 18) ^ S5_ROR64(x, 41))

/* SHA-512 σ0, σ1 (lowercase sigma, message schedule expansion) */
#define S5_SSIG0(x) (S5_ROR64(x, 1)  ^ S5_ROR64(x, 8)  ^ ((x) >> 7))
#define S5_SSIG1(x) (S5_ROR64(x, 19) ^ S5_ROR64(x, 61) ^ ((x) >> 6))

/* =========================================================================
 * Core compression function: process one 128-byte (1024-bit) block.
 * ========================================================================= */

static void sha512_compress(sha512_ctx *c, const unsigned char *p)
{
    unsigned long long w[80];
    unsigned long long a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    /* Load 16 big-endian 64-bit words from the block. */
    for (i = 0; i < 16; i++) {
        w[i] =
            ((unsigned long long)p[i * 8 + 0] << 56) |
            ((unsigned long long)p[i * 8 + 1] << 48) |
            ((unsigned long long)p[i * 8 + 2] << 40) |
            ((unsigned long long)p[i * 8 + 3] << 32) |
            ((unsigned long long)p[i * 8 + 4] << 24) |
            ((unsigned long long)p[i * 8 + 5] << 16) |
            ((unsigned long long)p[i * 8 + 6] <<  8) |
            ((unsigned long long)p[i * 8 + 7]);
    }

    /* Extend to 80 words (message schedule). */
    for (i = 16; i < 80; i++) {
        w[i] = S5_SSIG1(w[i - 2]) + w[i - 7] +
               S5_SSIG0(w[i - 15]) + w[i - 16];
    }

    /* Initialise working variables from current hash state. */
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g  = c->h[6]; h = c->h[7];

    /* 80 rounds. */
    for (i = 0; i < 80; i++) {
        t1 = h + S5_BSIG1(e) + S5_CH(e, f, g) + K512[i] + w[i];
        t2 = S5_BSIG0(a)     + S5_MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    /* Add compressed chunk to current hash state. */
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] +=  g; c->h[7] += h;
}

/* =========================================================================
 * SHA-512 public API.
 * ========================================================================= */

/* FIPS 180-4 §5.3.5 – SHA-512 initial hash value. */
void sha512_init(sha512_ctx *c)
{
    c->h[0] = 0x6a09e667f3bcc908ULL;
    c->h[1] = 0xbb67ae8584caa73bULL;
    c->h[2] = 0x3c6ef372fe94f82bULL;
    c->h[3] = 0xa54ff53a5f1d36f1ULL;
    c->h[4] = 0x510e527fade682d1ULL;
    c->h[5] = 0x9b05688c2b3e6c1fULL;
    c->h[6] = 0x1f83d9abfb41bd6bULL;
    c->h[7] = 0x5be0cd19137e2179ULL;
    c->len_lo = 0;
    c->len_hi = 0;
    c->blen   = 0;
}

void sha512_update(sha512_ctx *c, const void *data, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)data;

    /* Accumulate length (byte count, 128-bit). */
    c->len_lo += (unsigned long long)len;
    if (c->len_lo < (unsigned long long)len)
        c->len_hi++;   /* carry */

    /* Drain any partial block first. */
    if (c->blen) {
        while (len && c->blen < 128) {
            c->buf[c->blen++] = *p++;
            len--;
        }
        if (c->blen == 128) {
            sha512_compress(c, c->buf);
            c->blen = 0;
        }
    }

    /* Process full blocks directly from the caller's buffer. */
    while (len >= 128) {
        sha512_compress(c, p);
        p   += 128;
        len -= 128;
    }

    /* Buffer any trailing bytes. */
    while (len--) {
        c->buf[c->blen++] = *p++;
    }
}

void sha512_final(sha512_ctx *c, unsigned char out[64])
{
    /*
     * Message length in bits as 128-bit big-endian.
     * len_lo / len_hi are in bytes; convert to bits with << 3.
     * bits_hi picks up the top bit shifted out of len_lo, if any.
     */
    unsigned long long bits_hi = (c->len_hi << 3) | (c->len_lo >> 61);
    unsigned long long bits_lo =  c->len_lo << 3;

    /* Append the 0x80 padding byte. */
    unsigned char pad = 0x80;
    sha512_update(c, &pad, 1);

    /* Zero-pad until buf is at position 112 (leaving 16 bytes for length). */
    pad = 0x00;
    while (c->blen != 112) {
        sha512_update(c, &pad, 1);
    }

    /*
     * Append the 128-bit big-endian bit-length directly into the buffer
     * without going through sha512_update (which would corrupt len_lo/hi).
     */
    int i;
    for (i = 7; i >= 0; i--) {
        c->buf[c->blen++] = (unsigned char)(bits_hi >> (i * 8));
    }
    for (i = 7; i >= 0; i--) {
        c->buf[c->blen++] = (unsigned char)(bits_lo >> (i * 8));
    }
    sha512_compress(c, c->buf);

    /* Serialise the 512-bit digest (8 × 64-bit big-endian words). */
    for (i = 0; i < 8; i++) {
        out[i * 8 + 0] = (unsigned char)(c->h[i] >> 56);
        out[i * 8 + 1] = (unsigned char)(c->h[i] >> 48);
        out[i * 8 + 2] = (unsigned char)(c->h[i] >> 40);
        out[i * 8 + 3] = (unsigned char)(c->h[i] >> 32);
        out[i * 8 + 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 8 + 5] = (unsigned char)(c->h[i] >> 16);
        out[i * 8 + 6] = (unsigned char)(c->h[i] >>  8);
        out[i * 8 + 7] = (unsigned char)(c->h[i]);
    }

    /* Scrub sensitive state. */
    s5_memset(c, 0, sizeof(*c));
}

void sha512(const void *data, unsigned long len, unsigned char out[64])
{
    sha512_ctx c;
    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}

/* =========================================================================
 * SHA-384 public API.
 * SHA-384 = SHA-512 with a different IV; output is the first 48 bytes.
 * FIPS 180-4 §5.3.4.
 * ========================================================================= */

void sha384_init(sha384_ctx *c)
{
    c->h[0] = 0xcbbb9d5dc1059ed8ULL;
    c->h[1] = 0x629a292a367cd507ULL;
    c->h[2] = 0x9159015a3070dd17ULL;
    c->h[3] = 0x152fecd8f70e5939ULL;
    c->h[4] = 0x67332667ffc00b31ULL;
    c->h[5] = 0x8eb44a8768581511ULL;
    c->h[6] = 0xdb0c2e0d64f98fa7ULL;
    c->h[7] = 0x47b5481dbefa4fa4ULL;
    c->len_lo = 0;
    c->len_hi = 0;
    c->blen   = 0;
}

/* sha384_update and sha384_compress are identical to the SHA-512 variants;
 * reuse them directly through the shared context type. */
void sha384_update(sha384_ctx *c, const void *data, unsigned long len)
{
    sha512_update(c, data, len);
}

void sha384_final(sha384_ctx *c, unsigned char out[48])
{
    unsigned char tmp[64];
    sha512_final(c, tmp);          /* produces the full 512-bit intermediate */
    s5_memcpy(out, tmp, 48);       /* SHA-384 output = first 384 bits        */
    s5_memset(tmp, 0, sizeof(tmp));
}

void sha384(const void *data, unsigned long len, unsigned char out[48])
{
    sha384_ctx c;
    sha384_init(&c);
    sha384_update(&c, data, len);
    sha384_final(&c, out);
}

/* =========================================================================
 * Self-test using FIPS 180-4 / NIST CAVS vectors.
 * Returns 0 on pass, -1 on first mismatch.
 * ========================================================================= */

int sha512_selftest(void)
{
    unsigned char digest[64];

    /* ------------------------------------------------------------------
     * Vector 1: SHA-512("abc")
     * Expected (FIPS 180-4 example B.1):
     *   ddaf35a1 93617aba cc417349 ae204131 12e6fa4e 89a97ea2 0a9eeee6 4b55d39a
     *   2192992a 274fc1a8 36ba3c23 a3feebbd 454d4423 643ce80e 2a9ac94f a54ca49f
     * ------------------------------------------------------------------ */
    {
        static const unsigned char msg1[] = { 'a', 'b', 'c' };
        static const unsigned char exp1[64] = {
            0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba,
            0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31,
            0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2,
            0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a,
            0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8,
            0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd,
            0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e,
            0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f
        };
        sha512(msg1, sizeof(msg1), digest);
        if (s5_memcmp(digest, exp1, 64) != 0) return -1;
    }

    /* ------------------------------------------------------------------
     * Vector 2: SHA-512("") -- empty message
     * Expected (FIPS 180-4 example / NIST):
     *   cf83e135 7eefb8bd f1542850 d66d8007 d620e405 0b5715dc 83f4a921 d36ce9ce
     *   47d0d13c 5d85f2b0 ff8318d2 877eec2f 63b931bd 47417a81 a538327a f927da3e
     * ------------------------------------------------------------------ */
    {
        static const unsigned char exp2[64] = {
            0xcf, 0x83, 0xe1, 0x35, 0x7e, 0xef, 0xb8, 0xbd,
            0xf1, 0x54, 0x28, 0x50, 0xd6, 0x6d, 0x80, 0x07,
            0xd6, 0x20, 0xe4, 0x05, 0x0b, 0x57, 0x15, 0xdc,
            0x83, 0xf4, 0xa9, 0x21, 0xd3, 0x6c, 0xe9, 0xce,
            0x47, 0xd0, 0xd1, 0x3c, 0x5d, 0x85, 0xf2, 0xb0,
            0xff, 0x83, 0x18, 0xd2, 0x87, 0x7e, 0xec, 0x2f,
            0x63, 0xb9, 0x31, 0xbd, 0x47, 0x41, 0x7a, 0x81,
            0xa5, 0x38, 0x32, 0x7a, 0xf9, 0x27, 0xda, 0x3e
        };
        sha512("", 0, digest);
        if (s5_memcmp(digest, exp2, 64) != 0) return -1;
    }

    /* ------------------------------------------------------------------
     * Vector 3: SHA-512 multi-block (112-byte message, FIPS 180-4 §B.2)
     * Message: "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
     *          "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"
     * (spans two 128-byte blocks after padding is considered)
     * Expected:
     *   8e959b75 dae313da 8cf4f728 14fc143f 8f7779c6 eb9f7fa1 7299aead b6889018
     *   501d289e 4900f7e4 331b99de c4b5433a c7d329ee b6dd2654 5e96e55b 874be909
     * ------------------------------------------------------------------ */
    {
        static const unsigned char msg3[] =
            "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
            "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        static const unsigned char exp3[64] = {
            0x8e, 0x95, 0x9b, 0x75, 0xda, 0xe3, 0x13, 0xda,
            0x8c, 0xf4, 0xf7, 0x28, 0x14, 0xfc, 0x14, 0x3f,
            0x8f, 0x77, 0x79, 0xc6, 0xeb, 0x9f, 0x7f, 0xa1,
            0x72, 0x99, 0xae, 0xad, 0xb6, 0x88, 0x90, 0x18,
            0x50, 0x1d, 0x28, 0x9e, 0x49, 0x00, 0xf7, 0xe4,
            0x33, 0x1b, 0x99, 0xde, 0xc4, 0xb5, 0x43, 0x3a,
            0xc7, 0xd3, 0x29, 0xee, 0xb6, 0xdd, 0x26, 0x54,
            0x5e, 0x96, 0xe5, 0x5b, 0x87, 0x4b, 0xe9, 0x09
        };
        /* Use streaming API to also exercise sha512_update. */
        sha512_ctx ctx;
        sha512_init(&ctx);
        /* Feed in two chunks to exercise the buffering logic. */
        sha512_update(&ctx, msg3, 56);
        sha512_update(&ctx, msg3 + 56, sizeof(msg3) - 1 - 56);
        sha512_final(&ctx, digest);
        if (s5_memcmp(digest, exp3, 64) != 0) return -1;
    }

    /* ------------------------------------------------------------------
     * Vector 4: SHA-384("abc")
     * Expected (FIPS 180-4 §B.3):
     *   cb00753f 45a35e8b b5a03d69 9ac65007 272c32ab 0eded163 1a8b605a 43ff5bed
     *   8086072b a1e7cc23 58baeca1 34c825a7
     * ------------------------------------------------------------------ */
    {
        static const unsigned char msg4[] = { 'a', 'b', 'c' };
        static const unsigned char exp4[48] = {
            0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b,
            0xb5, 0xa0, 0x3d, 0x69, 0x9a, 0xc6, 0x50, 0x07,
            0x27, 0x2c, 0x32, 0xab, 0x0e, 0xde, 0xd1, 0x63,
            0x1a, 0x8b, 0x60, 0x5a, 0x43, 0xff, 0x5b, 0xed,
            0x80, 0x86, 0x07, 0x2b, 0xa1, 0xe7, 0xcc, 0x23,
            0x58, 0xba, 0xec, 0xa1, 0x34, 0xc8, 0x25, 0xa7
        };
        unsigned char out384[48];
        sha384(msg4, sizeof(msg4), out384);
        if (s5_memcmp(out384, exp4, 48) != 0) return -1;
    }

    /* ------------------------------------------------------------------
     * Vector 5: SHA-384("") -- empty message (NIST CAVS)
     * Expected:
     *   38b060a7 51ac9638 4cd9327e b1b1e36a 21fdb711 14be0743 4c0cc7bf 63f6e1da
     *   274edebf e76f65fb d51ad2f1 4898b95b
     * ------------------------------------------------------------------ */
    {
        static const unsigned char exp5[48] = {
            0x38, 0xb0, 0x60, 0xa7, 0x51, 0xac, 0x96, 0x38,
            0x4c, 0xd9, 0x32, 0x7e, 0xb1, 0xb1, 0xe3, 0x6a,
            0x21, 0xfd, 0xb7, 0x11, 0x14, 0xbe, 0x07, 0x43,
            0x4c, 0x0c, 0xc7, 0xbf, 0x63, 0xf6, 0xe1, 0xda,
            0x27, 0x4e, 0xde, 0xbf, 0xe7, 0x6f, 0x65, 0xfb,
            0xd5, 0x1a, 0xd2, 0xf1, 0x48, 0x98, 0xb9, 0x5b
        };
        unsigned char out384[48];
        sha384("", 0, out384);
        if (s5_memcmp(out384, exp5, 48) != 0) return -1;
    }

    /* All vectors passed. */
    return 0;
}
