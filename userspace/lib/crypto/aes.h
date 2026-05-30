/*
 * aes.h -- freestanding AES (FIPS-197) + CBC + GCM.
 * =================================================
 *
 * Pure computation: no libc, no syscalls, no malloc. Supports AES-128 and
 * AES-256 (key schedule, single-block encrypt/decrypt, CBC chaining, and
 * GCM authenticated encryption for TLS_*_GCM cipher suites).
 *
 * Key schedule layout: rk[] holds the expanded round keys as 32-bit words
 * (Nr+1 round keys * 4 words). nr is the number of rounds (10 for 128-bit,
 * 14 for 256-bit). 60 words is enough for AES-256 (15*4).
 *
 *   aes_ctx c;
 *   aes_set_encrypt_key(&c, key, 128);
 *   aes_encrypt_block(&c, in16, out16);
 *
 * CBC: iv[16] is updated in place to the last ciphertext block (encrypt) or
 * the last input ciphertext block (decrypt), so chained calls continue the
 * stream. nblocks is the number of 16-byte blocks.
 *
 * GCM: iv is the 12-byte nonce (the common TLS case; J0 = IV || 0x00000001).
 * Returns 0 on success. decrypt returns nonzero if the tag does not verify
 * (and in that case the plaintext output must be treated as invalid).
 */

#ifndef CRYPTO_AES_H
#define CRYPTO_AES_H

typedef struct {
    unsigned int rk[60];   /* expanded round keys (up to 15*4 words)   */
    int          nr;       /* number of rounds (10 or 14)              */
} aes_ctx;

void aes_set_encrypt_key(aes_ctx *c, const unsigned char *key, int keybits);
void aes_set_decrypt_key(aes_ctx *c, const unsigned char *key, int keybits);

void aes_encrypt_block(const aes_ctx *c, const unsigned char in[16],
                       unsigned char out[16]);
void aes_decrypt_block(const aes_ctx *c, const unsigned char in[16],
                       unsigned char out[16]);

void aes_cbc_encrypt(const aes_ctx *c, unsigned char iv[16],
                     const unsigned char *in, unsigned char *out,
                     unsigned long nblocks);
void aes_cbc_decrypt(const aes_ctx *c, unsigned char iv[16],
                     const unsigned char *in, unsigned char *out,
                     unsigned long nblocks);

int aes_gcm_encrypt(const aes_ctx *c, const unsigned char iv[12],
                    const unsigned char *aad, unsigned long aadlen,
                    const unsigned char *pt, unsigned long ptlen,
                    unsigned char *ct, unsigned char tag[16]);
int aes_gcm_decrypt(const aes_ctx *c, const unsigned char iv[12],
                    const unsigned char *aad, unsigned long aadlen,
                    const unsigned char *ct, unsigned long ctlen,
                    const unsigned char tag[16], unsigned char *pt);

#endif /* CRYPTO_AES_H */
