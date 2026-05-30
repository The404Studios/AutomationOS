/*
 * sha1.h -- freestanding SHA-1 (FIPS 180-4 / RFC 3174).
 * =====================================================
 *
 * Pure computation: no libc, no syscalls, no malloc. Provided for TLS 1.0/1.1
 * PRF and HMAC-SHA1 record MAC. (SHA-1 is cryptographically weak; included
 * only for protocol compatibility, not for new security decisions.)
 *
 * Streaming:
 *     sha1_ctx c; sha1_init(&c); sha1_update(&c, data, len);
 *     unsigned char d[20]; sha1_final(&c, d);
 * One-shot:
 *     unsigned char d[20]; sha1(data, len, d);
 */

#ifndef CRYPTO_SHA1_H
#define CRYPTO_SHA1_H

typedef struct {
    unsigned int       s[5];
    unsigned long long len;
    unsigned char      buf[64];
    unsigned int       blen;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, unsigned long len);
void sha1_final(sha1_ctx *c, unsigned char out[20]);
void sha1(const void *data, unsigned long len, unsigned char out[20]);

#endif /* CRYPTO_SHA1_H */
