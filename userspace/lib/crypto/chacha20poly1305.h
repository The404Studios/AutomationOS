/*
 * chacha20poly1305.h -- ChaCha20-Poly1305 AEAD (RFC 8439).
 * =========================================================
 *
 * Freestanding pure computation: no libc, no syscalls, no malloc, no standard
 * headers.  Uses only caller-supplied fixed buffers and its own memset/memcpy.
 *
 * Build flags required (NO fs:0x28 canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * AEAD API
 * --------
 *   chacha20poly1305_encrypt() -- pt -> ct + tag.
 *   chacha20poly1305_decrypt() -- constant-time tag verification BEFORE
 *                                  writing plaintext; returns non-zero on
 *                                  authentication failure.
 *
 * Primitive API (for TLS / other consumers)
 * ------------------------------------------
 *   chacha20_block()  -- single 64-byte ChaCha20 keystream block.
 *   chacha20_xor()    -- keystream-XOR over an arbitrary-length buffer.
 *   poly1305_mac()    -- Poly1305 one-shot authenticator.
 *
 * Self-test
 * ---------
 *   chacha20poly1305_selftest() -- RFC 8439 §§2.4.2, 2.5.2, 2.8.2 vectors.
 *                                   Returns 0 on success, non-zero on failure.
 */

#ifndef CRYPTO_CHACHA20POLY1305_H
#define CRYPTO_CHACHA20POLY1305_H

/* -------------------------------------------------------------------------
 * Primitive: ChaCha20 block function (RFC 8439 §2.1)
 *
 *   key     32-byte secret key
 *   counter 32-bit block counter (initial value per caller; 0 for OTK, 1 for
 *           encrypt)
 *   nonce   12-byte nonce
 *   out     64-byte output keystream block
 * ---------------------------------------------------------------------- */
void chacha20_block(const unsigned char key[32],
                    unsigned int counter,
                    const unsigned char nonce[12],
                    unsigned char out[64]);

/* -------------------------------------------------------------------------
 * Primitive: ChaCha20 stream-cipher XOR (RFC 8439 §2.4)
 *
 *   Encrypts or decrypts `len` bytes.  The first block counter value is
 *   `counter` (use 1 for AEAD).  in and out may alias.
 * ---------------------------------------------------------------------- */
void chacha20_xor(const unsigned char key[32],
                  unsigned int counter,
                  const unsigned char nonce[12],
                  const unsigned char *in,
                  unsigned char *out,
                  unsigned long len);

/* -------------------------------------------------------------------------
 * Primitive: Poly1305 one-shot MAC (RFC 8439 §2.5)
 *
 *   key   32-byte one-time key (r || s, r clamped internally)
 *   msg   message to authenticate
 *   len   message length in bytes
 *   tag   16-byte output tag
 * ---------------------------------------------------------------------- */
void poly1305_mac(const unsigned char key[32],
                  const unsigned char *msg,
                  unsigned long len,
                  unsigned char tag[16]);

/* -------------------------------------------------------------------------
 * AEAD encrypt (RFC 8439 §2.8)
 *
 *   key     32-byte secret key
 *   nonce   12-byte nonce (must never be reused with the same key)
 *   aad     additional authenticated data (may be NULL if aadlen == 0)
 *   aadlen  length of aad in bytes
 *   pt      plaintext input
 *   ptlen   plaintext length in bytes
 *   ct      ciphertext output  -- caller must supply at least ptlen bytes
 *   tag     16-byte Poly1305 authentication tag output
 *
 *   Returns 0 always (no failure path on encryption).
 * ---------------------------------------------------------------------- */
int chacha20poly1305_encrypt(const unsigned char key[32],
                             const unsigned char nonce[12],
                             const unsigned char *aad,
                             unsigned long aadlen,
                             const unsigned char *pt,
                             unsigned long ptlen,
                             unsigned char *ct,
                             unsigned char tag[16]);

/* -------------------------------------------------------------------------
 * AEAD decrypt (RFC 8439 §2.8)
 *
 *   key     32-byte secret key
 *   nonce   12-byte nonce
 *   aad     additional authenticated data (may be NULL if aadlen == 0)
 *   aadlen  length of aad in bytes
 *   ct      ciphertext input
 *   ctlen   ciphertext length in bytes
 *   tag     16-byte Poly1305 authentication tag to verify
 *   pt      plaintext output -- written ONLY when tag verification passes
 *
 *   Returns 0 on authentication success (pt is valid).
 *   Returns non-zero on authentication failure (pt is NOT written / zeroed).
 * ---------------------------------------------------------------------- */
int chacha20poly1305_decrypt(const unsigned char key[32],
                             const unsigned char nonce[12],
                             const unsigned char *aad,
                             unsigned long aadlen,
                             const unsigned char *ct,
                             unsigned long ctlen,
                             const unsigned char tag[16],
                             unsigned char *pt);

/* -------------------------------------------------------------------------
 * Self-test: RFC 8439 §§2.4.2, 2.5.2, 2.8.2 test vectors.
 *
 *   Returns 0 if every vector matches, non-zero otherwise.
 * ---------------------------------------------------------------------- */
int chacha20poly1305_selftest(void);

#endif /* CRYPTO_CHACHA20POLY1305_H */
