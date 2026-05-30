/*
 * aes.c -- freestanding AES (FIPS-197) + CBC + GCM.
 * =================================================
 * No libc, no headers. Byte-oriented round transforms (clear and correct);
 * GCM = AES-CTR + GHASH (bitwise carry-less multiply over GF(2^128)).
 *
 * The encrypt path uses the forward S-box; the decrypt path uses the inverse
 * S-box with an "equivalent inverse cipher" round-key schedule (InvMixColumns
 * applied to the interior round keys) so aes_decrypt_block mirrors the encrypt
 * structure.
 */

#include "aes.h"

/* ---- local memory helpers ------------------------------------------- */
static void aes_memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}
static void aes_memset(void *d, int v, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--) *dp++ = (unsigned char)v;
}

/* ---- S-box / inverse S-box (FIPS-197) ------------------------------- */
static const unsigned char sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const unsigned char inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const unsigned char rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

/* ---- GF(2^8) multiply (for MixColumns / key-schedule inv) ----------- */
static unsigned char gmul(unsigned char a, unsigned char b) {
    unsigned char p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        unsigned char hi = (unsigned char)(a & 0x80);
        a = (unsigned char)(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

/* round keys are stored big-endian within each 32-bit word */
static unsigned int rotword(unsigned int w) {
    return (w << 8) | (w >> 24);
}
static unsigned int subword(unsigned int w) {
    return ((unsigned int)sbox[(w >> 24) & 0xff] << 24) |
           ((unsigned int)sbox[(w >> 16) & 0xff] << 16) |
           ((unsigned int)sbox[(w >> 8) & 0xff] << 8) |
           ((unsigned int)sbox[w & 0xff]);
}

void aes_set_encrypt_key(aes_ctx *c, const unsigned char *key, int keybits) {
    int nk, nr, total, i;
    unsigned int *rk = c->rk;

    if (keybits == 256) { nk = 8; nr = 14; }
    else                { nk = 4; nr = 10; }   /* default/128 */
    c->nr = nr;
    total = 4 * (nr + 1);

    for (i = 0; i < nk; i++) {
        rk[i] = ((unsigned int)key[4 * i] << 24) |
                ((unsigned int)key[4 * i + 1] << 16) |
                ((unsigned int)key[4 * i + 2] << 8) |
                ((unsigned int)key[4 * i + 3]);
    }
    for (i = nk; i < total; i++) {
        unsigned int t = rk[i - 1];
        if ((i % nk) == 0) {
            t = subword(rotword(t)) ^ ((unsigned int)rcon[i / nk] << 24);
        } else if (nk > 6 && (i % nk) == 4) {
            t = subword(t);
        }
        rk[i] = rk[i - nk] ^ t;
    }
}

void aes_set_decrypt_key(aes_ctx *c, const unsigned char *key, int keybits) {
    /* Build the encrypt schedule, then apply InvMixColumns to the interior
     * round keys to produce the equivalent inverse-cipher schedule. */
    int nr, i, j;
    aes_set_encrypt_key(c, key, keybits);
    nr = c->nr;

    for (i = 1; i < nr; i++) {
        for (j = 0; j < 4; j++) {
            unsigned int w = c->rk[i * 4 + j];
            unsigned char b0 = (unsigned char)(w >> 24);
            unsigned char b1 = (unsigned char)(w >> 16);
            unsigned char b2 = (unsigned char)(w >> 8);
            unsigned char b3 = (unsigned char)(w);
            unsigned char n0 = (unsigned char)(gmul(b0,0x0e)^gmul(b1,0x0b)^gmul(b2,0x0d)^gmul(b3,0x09));
            unsigned char n1 = (unsigned char)(gmul(b0,0x09)^gmul(b1,0x0e)^gmul(b2,0x0b)^gmul(b3,0x0d));
            unsigned char n2 = (unsigned char)(gmul(b0,0x0d)^gmul(b1,0x09)^gmul(b2,0x0e)^gmul(b3,0x0b));
            unsigned char n3 = (unsigned char)(gmul(b0,0x0b)^gmul(b1,0x0d)^gmul(b2,0x09)^gmul(b3,0x0e));
            c->rk[i * 4 + j] = ((unsigned int)n0 << 24) | ((unsigned int)n1 << 16) |
                               ((unsigned int)n2 << 8) | n3;
        }
    }
}

/* ---- block transforms (state is 16 bytes, column-major per FIPS) ----- */
static void add_round_key(unsigned char st[16], const unsigned int *rk) {
    int col;
    for (col = 0; col < 4; col++) {
        unsigned int w = rk[col];
        st[col * 4]     ^= (unsigned char)(w >> 24);
        st[col * 4 + 1] ^= (unsigned char)(w >> 16);
        st[col * 4 + 2] ^= (unsigned char)(w >> 8);
        st[col * 4 + 3] ^= (unsigned char)(w);
    }
}

static void sub_bytes(unsigned char st[16]) {
    int i;
    for (i = 0; i < 16; i++) st[i] = sbox[st[i]];
}
static void inv_sub_bytes(unsigned char st[16]) {
    int i;
    for (i = 0; i < 16; i++) st[i] = inv_sbox[st[i]];
}

/* State byte index: st[col*4 + row]. ShiftRows shifts row r left by r. */
static void shift_rows(unsigned char st[16]) {
    unsigned char t;
    /* row 1: shift left 1 */
    t = st[1]; st[1] = st[5]; st[5] = st[9]; st[9] = st[13]; st[13] = t;
    /* row 2: shift left 2 */
    t = st[2]; st[2] = st[10]; st[10] = t;
    t = st[6]; st[6] = st[14]; st[14] = t;
    /* row 3: shift left 3 (== right 1) */
    t = st[15]; st[15] = st[11]; st[11] = st[7]; st[7] = st[3]; st[3] = t;
}
static void inv_shift_rows(unsigned char st[16]) {
    unsigned char t;
    /* row 1: shift right 1 */
    t = st[13]; st[13] = st[9]; st[9] = st[5]; st[5] = st[1]; st[1] = t;
    /* row 2: shift right 2 */
    t = st[2]; st[2] = st[10]; st[10] = t;
    t = st[6]; st[6] = st[14]; st[14] = t;
    /* row 3: shift right 3 (== left 1) */
    t = st[3]; st[3] = st[7]; st[7] = st[11]; st[11] = st[15]; st[15] = t;
}

static void mix_columns(unsigned char st[16]) {
    int col;
    for (col = 0; col < 4; col++) {
        unsigned char *s = &st[col * 4];
        unsigned char a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
        s[0] = (unsigned char)(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
        s[1] = (unsigned char)(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
        s[2] = (unsigned char)(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
        s[3] = (unsigned char)(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
    }
}
static void inv_mix_columns(unsigned char st[16]) {
    int col;
    for (col = 0; col < 4; col++) {
        unsigned char *s = &st[col * 4];
        unsigned char a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
        s[0] = (unsigned char)(gmul(a0,0x0e)^gmul(a1,0x0b)^gmul(a2,0x0d)^gmul(a3,0x09));
        s[1] = (unsigned char)(gmul(a0,0x09)^gmul(a1,0x0e)^gmul(a2,0x0b)^gmul(a3,0x0d));
        s[2] = (unsigned char)(gmul(a0,0x0d)^gmul(a1,0x09)^gmul(a2,0x0e)^gmul(a3,0x0b));
        s[3] = (unsigned char)(gmul(a0,0x0b)^gmul(a1,0x0d)^gmul(a2,0x09)^gmul(a3,0x0e));
    }
}

void aes_encrypt_block(const aes_ctx *c, const unsigned char in[16],
                       unsigned char out[16]) {
    unsigned char st[16];
    int r;
    aes_memcpy(st, in, 16);

    add_round_key(st, &c->rk[0]);
    for (r = 1; r < c->nr; r++) {
        sub_bytes(st);
        shift_rows(st);
        mix_columns(st);
        add_round_key(st, &c->rk[r * 4]);
    }
    sub_bytes(st);
    shift_rows(st);
    add_round_key(st, &c->rk[c->nr * 4]);

    aes_memcpy(out, st, 16);
}

void aes_decrypt_block(const aes_ctx *c, const unsigned char in[16],
                       unsigned char out[16]) {
    /* Equivalent inverse cipher (matches the decrypt key schedule). */
    unsigned char st[16];
    int r;
    aes_memcpy(st, in, 16);

    add_round_key(st, &c->rk[c->nr * 4]);
    for (r = c->nr - 1; r >= 1; r--) {
        inv_sub_bytes(st);
        inv_shift_rows(st);
        inv_mix_columns(st);
        add_round_key(st, &c->rk[r * 4]);
    }
    inv_sub_bytes(st);
    inv_shift_rows(st);
    add_round_key(st, &c->rk[0]);

    aes_memcpy(out, st, 16);
}

/* ---- CBC ------------------------------------------------------------- */
void aes_cbc_encrypt(const aes_ctx *c, unsigned char iv[16],
                     const unsigned char *in, unsigned char *out,
                     unsigned long nblocks) {
    unsigned char blk[16];
    unsigned long n, i;
    for (n = 0; n < nblocks; n++) {
        for (i = 0; i < 16; i++) blk[i] = (unsigned char)(in[i] ^ iv[i]);
        aes_encrypt_block(c, blk, out);
        aes_memcpy(iv, out, 16);
        in += 16;
        out += 16;
    }
}

void aes_cbc_decrypt(const aes_ctx *c, unsigned char iv[16],
                     const unsigned char *in, unsigned char *out,
                     unsigned long nblocks) {
    unsigned char blk[16];
    unsigned char next_iv[16];
    unsigned long n, i;
    for (n = 0; n < nblocks; n++) {
        aes_memcpy(next_iv, in, 16);          /* save ct before overwrite */
        aes_decrypt_block(c, in, blk);
        for (i = 0; i < 16; i++) out[i] = (unsigned char)(blk[i] ^ iv[i]);
        aes_memcpy(iv, next_iv, 16);
        in += 16;
        out += 16;
    }
}

/* ===================================================================== *
 *  GCM = AES-CTR for confidentiality + GHASH for authentication.        *
 *  All 128-bit values are big-endian byte arrays (FIPS SP 800-38D).     *
 * ===================================================================== */

/* GHASH multiply: Z = X * Y in GF(2^128), poly = x^128 + x^7 + x^2 + x + 1
 * (R = 0xe1 in the top byte, big-endian bit order). */
static void ghash_mul(unsigned char *x, const unsigned char *y) {
    unsigned char z[16];
    unsigned char v[16];
    int i, bit;

    aes_memset(z, 0, 16);
    aes_memcpy(v, y, 16);

    for (i = 0; i < 16; i++) {
        for (bit = 7; bit >= 0; bit--) {
            if ((x[i] >> bit) & 1) {
                int j;
                for (j = 0; j < 16; j++) z[j] ^= v[j];
            }
            /* v = v >> 1 (big-endian bit shift); if LSB was set, xor R */
            unsigned char lsb = (unsigned char)(v[15] & 1);
            int j;
            for (j = 15; j > 0; j--) {
                v[j] = (unsigned char)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
            }
            v[0] = (unsigned char)(v[0] >> 1);
            if (lsb) v[0] ^= 0xe1;
        }
    }
    aes_memcpy(x, z, 16);
}

/* Fold len(A)*8 || len(C)*8 etc. Helper to xor a 16-byte block into the
 * accumulator and multiply by H. */
static void ghash_block(unsigned char *acc, const unsigned char *blk,
                        const unsigned char *H) {
    int i;
    for (i = 0; i < 16; i++) acc[i] ^= blk[i];
    ghash_mul(acc, H);
}

static void ghash_data(unsigned char *acc, const unsigned char *data,
                       unsigned long len, const unsigned char *H) {
    unsigned char blk[16];
    unsigned long off = 0;
    while (off + 16 <= len) {
        ghash_block(acc, data + off, H);
        off += 16;
    }
    if (off < len) {
        unsigned long rem = len - off, i;
        aes_memset(blk, 0, 16);
        for (i = 0; i < rem; i++) blk[i] = data[off + i];
        ghash_block(acc, blk, H);
    }
}

static void put_be64(unsigned char *p, unsigned long long v) {
    int i;
    for (i = 7; i >= 0; i--) { p[7 - i] = (unsigned char)(v >> (i * 8)); }
}

/* increment the low 32 bits (the counter) of a 16-byte block, big-endian */
static void ctr_inc(unsigned char *ctr) {
    int i;
    for (i = 15; i >= 12; i--) {
        if (++ctr[i] != 0) break;
    }
}

static void gcm_core(const aes_ctx *c, const unsigned char iv[12],
                     const unsigned char *aad, unsigned long aadlen,
                     const unsigned char *src, unsigned long srclen,
                     unsigned char *dst, unsigned char tag[16]) {
    unsigned char H[16];
    unsigned char J0[16];
    unsigned char ctr[16];
    unsigned char ks[16];
    unsigned char acc[16];
    unsigned char lenblk[16];
    unsigned char zero[16];
    unsigned long off;
    int i;

    aes_memset(zero, 0, 16);
    aes_encrypt_block(c, zero, H);           /* H = E(K, 0^128)         */

    /* J0 = IV || 0x00000001  (12-byte IV case) */
    aes_memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    /* CTR encryption starting at inc32(J0) */
    aes_memcpy(ctr, J0, 16);
    off = 0;
    while (off < srclen) {
        unsigned long rem = srclen - off, n, j;
        ctr_inc(ctr);
        aes_encrypt_block(c, ctr, ks);
        n = (rem < 16) ? rem : 16;
        for (j = 0; j < n; j++) dst[off + j] = (unsigned char)(src[off + j] ^ ks[j]);
        off += n;
    }

    /* GHASH over AAD || C || (len(A)||len(C)) */
    aes_memset(acc, 0, 16);
    ghash_data(acc, aad, aadlen, H);
    ghash_data(acc, dst, srclen, H);
    put_be64(lenblk, (unsigned long long)aadlen * 8ull);
    put_be64(lenblk + 8, (unsigned long long)srclen * 8ull);
    ghash_block(acc, lenblk, H);

    /* tag = GHASH ^ E(K, J0) */
    aes_encrypt_block(c, J0, ks);
    for (i = 0; i < 16; i++) tag[i] = (unsigned char)(acc[i] ^ ks[i]);
}

int aes_gcm_encrypt(const aes_ctx *c, const unsigned char iv[12],
                    const unsigned char *aad, unsigned long aadlen,
                    const unsigned char *pt, unsigned long ptlen,
                    unsigned char *ct, unsigned char tag[16]) {
    gcm_core(c, iv, aad, aadlen, pt, ptlen, ct, tag);
    return 0;
}

int aes_gcm_decrypt(const aes_ctx *c, const unsigned char iv[12],
                    const unsigned char *aad, unsigned long aadlen,
                    const unsigned char *ct, unsigned long ctlen,
                    const unsigned char tag[16], unsigned char *pt) {
    /* Recompute the tag over the *ciphertext* and CTR-decrypt; the GHASH in
     * gcm_core runs over dst, but for decryption GHASH must run over the
     * ciphertext (== src here). We therefore GHASH the ct directly. */
    unsigned char H[16];
    unsigned char J0[16];
    unsigned char ctr[16];
    unsigned char ks[16];
    unsigned char acc[16];
    unsigned char lenblk[16];
    unsigned char zero[16];
    unsigned char expect[16];
    unsigned long off;
    int i, diff;

    aes_memset(zero, 0, 16);
    aes_encrypt_block(c, zero, H);

    aes_memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    /* compute expected tag from ciphertext first (constant-time compare) */
    aes_memset(acc, 0, 16);
    ghash_data(acc, aad, aadlen, H);
    ghash_data(acc, ct, ctlen, H);
    put_be64(lenblk, (unsigned long long)aadlen * 8ull);
    put_be64(lenblk + 8, (unsigned long long)ctlen * 8ull);
    ghash_block(acc, lenblk, H);
    aes_encrypt_block(c, J0, ks);
    for (i = 0; i < 16; i++) expect[i] = (unsigned char)(acc[i] ^ ks[i]);

    diff = 0;
    for (i = 0; i < 16; i++) diff |= (expect[i] ^ tag[i]);
    if (diff != 0) {
        return 1;   /* authentication failure: do not release plaintext */
    }

    /* CTR-decrypt */
    aes_memcpy(ctr, J0, 16);
    off = 0;
    while (off < ctlen) {
        unsigned long rem = ctlen - off, n, j;
        ctr_inc(ctr);
        aes_encrypt_block(c, ctr, ks);
        n = (rem < 16) ? rem : 16;
        for (j = 0; j < n; j++) pt[off + j] = (unsigned char)(ct[off + j] ^ ks[j]);
        off += n;
    }
    return 0;
}
