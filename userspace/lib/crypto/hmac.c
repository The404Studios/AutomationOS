/*
 * hmac.c -- freestanding HMAC (RFC 2104) over SHA-256 and SHA-1.
 * =============================================================
 * No libc, no headers. Block size for both underlying hashes is 64 bytes.
 */

#include "hmac.h"
#include "sha256.h"
#include "sha1.h"
#include "sha512.h"

#define HMAC_BLOCK 64
#define HMAC_BLOCK_384 128   /* SHA-384/512 use a 128-byte block */

static void hmac_memset(void *d, int v, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--) *dp++ = (unsigned char)v;
}

void hmac_sha256(const unsigned char *key, unsigned long klen,
                 const unsigned char *msg, unsigned long mlen,
                 unsigned char out[32]) {
    unsigned char k[HMAC_BLOCK];
    unsigned char ipad[HMAC_BLOCK];
    unsigned char opad[HMAC_BLOCK];
    unsigned char inner[32];
    sha256_ctx c;
    int i;

    hmac_memset(k, 0, HMAC_BLOCK);
    if (klen > HMAC_BLOCK) {
        sha256(key, klen, k);          /* k = H(key), rest stays zero */
    } else {
        for (i = 0; (unsigned long)i < klen; i++) k[i] = key[i];
    }

    for (i = 0; i < HMAC_BLOCK; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    sha256_init(&c);
    sha256_update(&c, ipad, HMAC_BLOCK);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, HMAC_BLOCK);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);

    hmac_memset(k, 0, HMAC_BLOCK);
    hmac_memset(ipad, 0, HMAC_BLOCK);
    hmac_memset(opad, 0, HMAC_BLOCK);
    hmac_memset(inner, 0, sizeof(inner));
}

void hmac_sha1(const unsigned char *key, unsigned long klen,
               const unsigned char *msg, unsigned long mlen,
               unsigned char out[20]) {
    unsigned char k[HMAC_BLOCK];
    unsigned char ipad[HMAC_BLOCK];
    unsigned char opad[HMAC_BLOCK];
    unsigned char inner[20];
    sha1_ctx c;
    int i;

    hmac_memset(k, 0, HMAC_BLOCK);
    if (klen > HMAC_BLOCK) {
        sha1(key, klen, k);
    } else {
        for (i = 0; (unsigned long)i < klen; i++) k[i] = key[i];
    }

    for (i = 0; i < HMAC_BLOCK; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    sha1_init(&c);
    sha1_update(&c, ipad, HMAC_BLOCK);
    sha1_update(&c, msg, mlen);
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, HMAC_BLOCK);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);

    hmac_memset(k, 0, HMAC_BLOCK);
    hmac_memset(ipad, 0, HMAC_BLOCK);
    hmac_memset(opad, 0, HMAC_BLOCK);
    hmac_memset(inner, 0, sizeof(inner));
}

void hmac_sha384(const unsigned char *key, unsigned long klen,
                 const unsigned char *msg, unsigned long mlen,
                 unsigned char out[48]) {
    unsigned char k[HMAC_BLOCK_384];
    unsigned char ipad[HMAC_BLOCK_384];
    unsigned char opad[HMAC_BLOCK_384];
    unsigned char inner[48];
    sha384_ctx c;
    int i;

    hmac_memset(k, 0, HMAC_BLOCK_384);
    if (klen > HMAC_BLOCK_384) {
        sha384(key, klen, k);          /* k = H(key), rest stays zero */
    } else {
        for (i = 0; (unsigned long)i < klen; i++) k[i] = key[i];
    }

    for (i = 0; i < HMAC_BLOCK_384; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    sha384_init(&c);
    sha384_update(&c, ipad, HMAC_BLOCK_384);
    sha384_update(&c, msg, mlen);
    sha384_final(&c, inner);

    sha384_init(&c);
    sha384_update(&c, opad, HMAC_BLOCK_384);
    sha384_update(&c, inner, 48);
    sha384_final(&c, out);

    hmac_memset(k, 0, HMAC_BLOCK_384);
    hmac_memset(ipad, 0, HMAC_BLOCK_384);
    hmac_memset(opad, 0, HMAC_BLOCK_384);
    hmac_memset(inner, 0, sizeof(inner));
}
