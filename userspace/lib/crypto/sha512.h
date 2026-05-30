/*
 * sha512.h -- freestanding SHA-512 / SHA-384 (FIPS 180-4).
 * =========================================================
 *
 * Pure computation: no libc, no syscalls, no malloc.  Operates on caller-
 * supplied buffers only.  Used by the TLS userspace stack for
 * TLS_AES_256_GCM_SHA384 cipher suites and HKDF-SHA-384/512.
 *
 * Build flags (no stack-protector, no libc, no PIC):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Streaming usage:
 *   sha512_ctx c; sha512_init(&c);
 *   sha512_update(&c, data, len);   // any number of times
 *   unsigned char digest[64]; sha512_final(&c, digest);
 *
 * One-shot:
 *   unsigned char digest[64]; sha512(data, len, digest);
 *
 * SHA-384 is SHA-512 with a different IV, output truncated to 48 bytes:
 *   sha384_ctx c; sha384_init(&c);
 *   sha384_update(&c, data, len);
 *   unsigned char digest[48]; sha384_final(&c, digest);
 *
 * Self-test (returns 0 on pass, -1 on fail):
 *   int ok = sha512_selftest();
 */

#ifndef CRYPTO_SHA512_H
#define CRYPTO_SHA512_H

/* Context for SHA-512 (and SHA-384, which reuses the same structure). */
typedef struct {
    unsigned long long h[8];      /* running state H0..H7 (64-bit words)   */
    unsigned long long len_lo;    /* message length in bytes, low 64 bits  */
    unsigned long long len_hi;    /* message length in bytes, high 64 bits */
    unsigned char      buf[128];  /* partial block buffer (512-bit block)  */
    unsigned int       blen;      /* bytes currently buffered (0..127)     */
} sha512_ctx;

/* SHA-384 shares the context type; only the IV and output length differ. */
typedef sha512_ctx sha384_ctx;

/* --- SHA-512 ------------------------------------------------------------ */
void sha512_init  (sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, unsigned long len);
void sha512_final (sha512_ctx *c, unsigned char out[64]);
void sha512       (const void *data, unsigned long len, unsigned char out[64]);

/* --- SHA-384 ------------------------------------------------------------ */
void sha384_init  (sha384_ctx *c);
void sha384_update(sha384_ctx *c, const void *data, unsigned long len);
void sha384_final (sha384_ctx *c, unsigned char out[48]);
void sha384       (const void *data, unsigned long len, unsigned char out[48]);

/* --- Self-test ---------------------------------------------------------- */
/* Returns 0 if all FIPS/NIST vectors pass, -1 on the first mismatch. */
int sha512_selftest(void);

#endif /* CRYPTO_SHA512_H */
