/*
 * md5.h -- freestanding MD5 (RFC 1321).
 * =====================================
 *
 * Pure computation: no libc, no syscalls, no malloc. Included for legacy TLS
 * (the TLS 1.0/1.1 PRF combines MD5+SHA-1) and miscellaneous checksumming.
 * MD5 is cryptographically broken; do NOT use for security decisions.
 *
 * Streaming:
 *     md5_ctx c; md5_init(&c); md5_update(&c, data, len);
 *     unsigned char d[16]; md5_final(&c, d);
 * One-shot:
 *     unsigned char d[16]; md5(data, len, d);
 */

#ifndef CRYPTO_MD5_H
#define CRYPTO_MD5_H

typedef struct {
    unsigned int       s[4];
    unsigned long long len;
    unsigned char      buf[64];
    unsigned int       blen;
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, unsigned long len);
void md5_final(md5_ctx *c, unsigned char out[16]);
void md5(const void *data, unsigned long len, unsigned char out[16]);

#endif /* CRYPTO_MD5_H */
