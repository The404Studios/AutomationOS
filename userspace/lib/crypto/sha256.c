/*
 * sha256.c -- freestanding SHA-256 (FIPS 180-4).
 * ==============================================
 * No libc, no headers. Big-endian message schedule, 64-round compression.
 */

#include "sha256.h"

/* ---- local memory helpers (no libc) ---------------------------------- */
static void sha256_memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}
static void sha256_memset(void *d, int v, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--) *dp++ = (unsigned char)v;
}

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define BSIG1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SSIG0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static const unsigned int K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_compress(sha256_ctx *c, const unsigned char *p) {
    unsigned int w[64];
    unsigned int a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)p[i * 4] << 24) |
               ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) |
               ((unsigned int)p[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = c->s[0]; b = c->s[1]; cc = c->s[2]; d = c->s[3];
    e = c->s[4]; f = c->s[5]; g = c->s[6]; h = c->s[7];

    for (i = 0; i < 64; i++) {
        t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        t2 = BSIG0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->s[0] += a; c->s[1] += b; c->s[2] += cc; c->s[3] += d;
    c->s[4] += e; c->s[5] += f; c->s[6] += g; c->s[7] += h;
}

void sha256_init(sha256_ctx *c) {
    c->s[0] = 0x6a09e667u; c->s[1] = 0xbb67ae85u;
    c->s[2] = 0x3c6ef372u; c->s[3] = 0xa54ff53au;
    c->s[4] = 0x510e527fu; c->s[5] = 0x9b05688cu;
    c->s[6] = 0x1f83d9abu; c->s[7] = 0x5be0cd19u;
    c->len = 0;
    c->blen = 0;
}

void sha256_update(sha256_ctx *c, const void *data, unsigned long len) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += len;

    if (c->blen) {
        while (len && c->blen < 64) {
            c->buf[c->blen++] = *p++;
            len--;
        }
        if (c->blen == 64) {
            sha256_compress(c, c->buf);
            c->blen = 0;
        }
    }
    while (len >= 64) {
        sha256_compress(c, p);
        p += 64;
        len -= 64;
    }
    while (len--) {
        c->buf[c->blen++] = *p++;
    }
}

void sha256_final(sha256_ctx *c, unsigned char out[32]) {
    unsigned long long bits = c->len * 8ull;
    unsigned char pad = 0x80;
    int i;

    sha256_update(c, &pad, 1);
    pad = 0x00;
    while (c->blen != 56) {
        sha256_update(c, &pad, 1);
    }

    /* append 64-bit big-endian length (not via update, to avoid len drift) */
    for (i = 7; i >= 0; i--) {
        c->buf[c->blen++] = (unsigned char)(bits >> (i * 8));
    }
    sha256_compress(c, c->buf);
    c->blen = 0;

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(c->s[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->s[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->s[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->s[i]);
    }
    sha256_memset(c, 0, sizeof(*c));
}

void sha256(const void *data, unsigned long len, unsigned char out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}
