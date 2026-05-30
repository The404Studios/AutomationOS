/*
 * sha256.h -- freestanding SHA-256 (FIPS 180-4).
 * ==============================================
 *
 * Pure computation: no libc, no syscalls, no malloc. Operates on caller
 * supplied buffers only. Used by both the TLS userspace client and,
 * potentially, the kernel.
 *
 * Streaming usage:
 *     sha256_ctx c; sha256_init(&c);
 *     sha256_update(&c, data, len);   // any number of times
 *     unsigned char digest[32]; sha256_final(&c, digest);
 *
 * One-shot:
 *     unsigned char digest[32]; sha256(data, len, digest);
 */

#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

typedef struct {
    unsigned int       s[8];   /* running state H0..H7                 */
    unsigned long long len;    /* total message length in bytes        */
    unsigned char      buf[64];/* partial block buffer                 */
    unsigned int       blen;   /* bytes currently buffered (0..63)     */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, unsigned long len);
void sha256_final(sha256_ctx *c, unsigned char out[32]);
void sha256(const void *data, unsigned long len, unsigned char out[32]);

#endif /* CRYPTO_SHA256_H */
