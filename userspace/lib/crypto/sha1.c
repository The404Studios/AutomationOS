/*
 * sha1.c -- freestanding SHA-1 (FIPS 180-4 / RFC 3174).
 * =====================================================
 * No libc, no headers. Big-endian, 80-round compression.
 */

#include "sha1.h"

static void sha1_memset(void *d, int v, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--) *dp++ = (unsigned char)v;
}

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_compress(sha1_ctx *c, const unsigned char *p) {
    unsigned int w[80];
    unsigned int a, b, cc, d, e, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)p[i * 4] << 24) |
               ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) |
               ((unsigned int)p[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = c->s[0]; b = c->s[1]; cc = c->s[2]; d = c->s[3]; e = c->s[4];

    for (i = 0; i < 80; i++) {
        unsigned int f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5a827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8f1bbcdcu; }
        else             { f = b ^ cc ^ d;                     k = 0xca62c1d6u; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }

    c->s[0] += a; c->s[1] += b; c->s[2] += cc; c->s[3] += d; c->s[4] += e;
}

void sha1_init(sha1_ctx *c) {
    c->s[0] = 0x67452301u; c->s[1] = 0xefcdab89u; c->s[2] = 0x98badcfeu;
    c->s[3] = 0x10325476u; c->s[4] = 0xc3d2e1f0u;
    c->len = 0;
    c->blen = 0;
}

void sha1_update(sha1_ctx *c, const void *data, unsigned long len) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += len;

    if (c->blen) {
        while (len && c->blen < 64) {
            c->buf[c->blen++] = *p++;
            len--;
        }
        if (c->blen == 64) {
            sha1_compress(c, c->buf);
            c->blen = 0;
        }
    }
    while (len >= 64) {
        sha1_compress(c, p);
        p += 64;
        len -= 64;
    }
    while (len--) {
        c->buf[c->blen++] = *p++;
    }
}

void sha1_final(sha1_ctx *c, unsigned char out[20]) {
    unsigned long long bits = c->len * 8ull;
    unsigned char pad = 0x80;
    int i;

    sha1_update(c, &pad, 1);
    pad = 0x00;
    while (c->blen != 56) {
        sha1_update(c, &pad, 1);
    }

    for (i = 7; i >= 0; i--) {
        c->buf[c->blen++] = (unsigned char)(bits >> (i * 8));
    }
    sha1_compress(c, c->buf);
    c->blen = 0;

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(c->s[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->s[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->s[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->s[i]);
    }
    sha1_memset(c, 0, sizeof(*c));
}

void sha1(const void *data, unsigned long len, unsigned char out[20]) {
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}
