/*
 * md5.c -- freestanding MD5 (RFC 1321).
 * =====================================
 * No libc, no headers. Little-endian, 64-round compression.
 */

#include "md5.h"

static void md5_memset(void *d, int v, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--) *dp++ = (unsigned char)v;
}

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static const unsigned int T[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const unsigned char S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_compress(md5_ctx *c, const unsigned char *p) {
    unsigned int m[16];
    unsigned int a, b, cc, d, f, g, t;
    int i;

    for (i = 0; i < 16; i++) {
        m[i] = ((unsigned int)p[i * 4]) |
               ((unsigned int)p[i * 4 + 1] << 8) |
               ((unsigned int)p[i * 4 + 2] << 16) |
               ((unsigned int)p[i * 4 + 3] << 24);
    }

    a = c->s[0]; b = c->s[1]; cc = c->s[2]; d = c->s[3];

    for (i = 0; i < 64; i++) {
        if (i < 16)      { f = (b & cc) | (~b & d);     g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);     g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d;              g = (3 * i + 5) & 15; }
        else             { f = cc ^ (b | ~d);           g = (7 * i) & 15; }

        t = d;
        d = cc;
        cc = b;
        b = b + ROL(a + f + T[i] + m[g], S[i]);
        a = t;
    }

    c->s[0] += a; c->s[1] += b; c->s[2] += cc; c->s[3] += d;
}

void md5_init(md5_ctx *c) {
    c->s[0] = 0x67452301u; c->s[1] = 0xefcdab89u;
    c->s[2] = 0x98badcfeu; c->s[3] = 0x10325476u;
    c->len = 0;
    c->blen = 0;
}

void md5_update(md5_ctx *c, const void *data, unsigned long len) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += len;

    if (c->blen) {
        while (len && c->blen < 64) {
            c->buf[c->blen++] = *p++;
            len--;
        }
        if (c->blen == 64) {
            md5_compress(c, c->buf);
            c->blen = 0;
        }
    }
    while (len >= 64) {
        md5_compress(c, p);
        p += 64;
        len -= 64;
    }
    while (len--) {
        c->buf[c->blen++] = *p++;
    }
}

void md5_final(md5_ctx *c, unsigned char out[16]) {
    unsigned long long bits = c->len * 8ull;
    unsigned char pad = 0x80;
    int i;

    md5_update(c, &pad, 1);
    pad = 0x00;
    while (c->blen != 56) {
        md5_update(c, &pad, 1);
    }

    /* append 64-bit little-endian length */
    for (i = 0; i < 8; i++) {
        c->buf[c->blen++] = (unsigned char)(bits >> (i * 8));
    }
    md5_compress(c, c->buf);
    c->blen = 0;

    for (i = 0; i < 4; i++) {
        out[i * 4]     = (unsigned char)(c->s[i]);
        out[i * 4 + 1] = (unsigned char)(c->s[i] >> 8);
        out[i * 4 + 2] = (unsigned char)(c->s[i] >> 16);
        out[i * 4 + 3] = (unsigned char)(c->s[i] >> 24);
    }
    md5_memset(c, 0, sizeof(*c));
}

void md5(const void *data, unsigned long len, unsigned char out[16]) {
    md5_ctx c;
    md5_init(&c);
    md5_update(&c, data, len);
    md5_final(&c, out);
}
