/*
 * ccm.h -- freestanding generic AES-CCM (NIST SP 800-38C), L = 2.
 * ===============================================================
 *
 * Pure computation: no libc, no syscalls, no malloc, no float, no standard
 * headers. Fixed-size stack buffers only. Builds on the single-block AES API
 * from aes.h (aes_set_encrypt_key / aes_encrypt_block); CCM uses only the
 * forward AES block transform (for both the CBC-MAC tag and CTR keystream).
 *
 * Build flags (matching the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * -------------------------------------------------------------------------
 * CCM = Counter mode (confidentiality) + CBC-MAC (authentication).
 * -------------------------------------------------------------------------
 * This is the "Generation-Encryption" / "Decryption-Verification" process of
 * NIST SP 800-38C with the length-of-length parameter L = 2 (so the maximum
 * payload is 2^16 - 1 = 65535 bytes and the nonce length Nlen = 15 - L = 13).
 * That parameterization is exactly what IEEE 802.11 CCMP uses.
 *
 * The CBC-MAC is computed over the formatted blocks B0 || B1 || ... where:
 *   B0  = Flags || Nonce || Q          (Q = message length, L = 2 octets BE)
 *         Flags = 64*Adata + 8*((Tlen-2)/2) + (L-1)
 *   B1.. = encode(Alen) || AAD || pad, then payload || pad   (16-byte blocks)
 * The keystream blocks are CTR(Ctr_i) with Ctr_0 = Flags' || Nonce || i,
 * Flags' = L-1; S0 = E(K, Ctr_0) is XORed with the CBC-MAC to form the tag.
 *
 * Tag length (taglen) is a parameter: CCMP-128 uses M = 8, CCMP-256 uses
 * M = 16. Valid taglen values are the even numbers 4,6,8,10,12,14,16.
 * ====================================================================== */

#ifndef CRYPTO_CCM_H
#define CRYPTO_CCM_H

#ifndef CRYPTO_STDINT_DEFINED
#define CRYPTO_STDINT_DEFINED
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
#endif

/*
 * aes_ccm_encrypt -- CCM generation-encryption.
 *
 *   key/keybits : AES key (keybits = 128 or 256).
 *   nonce/nlen  : the CCM nonce N. With L = 2, nlen must be 13.
 *   aad/aadlen  : associated data to authenticate (not encrypted). aadlen may
 *                 be 0. With this implementation aadlen must be < 2^16 - 2^8.
 *   pt/ptlen    : plaintext payload (ptlen <= 65535).
 *   taglen      : MIC length in bytes (even, 4..16).
 *   ct_out      : receives ptlen ciphertext bytes (may alias pt).
 *   tag_out     : receives taglen MIC bytes.
 *
 * Returns 0 on success, -1 on invalid arguments.
 */
int aes_ccm_encrypt(const uint8_t *key, int keybits,
                    const uint8_t *nonce, int nlen,
                    const uint8_t *aad, int aadlen,
                    const uint8_t *pt, int ptlen,
                    int taglen,
                    uint8_t *ct_out, uint8_t *tag_out);

/*
 * aes_ccm_decrypt -- CCM decryption-verification.
 *
 *   ct/ctlen    : ciphertext payload.
 *   tag         : the received MIC (taglen bytes).
 *   pt_out      : receives ctlen recovered plaintext bytes (may alias ct).
 *
 * The MIC is recomputed over the recovered plaintext and compared (in
 * constant time) against tag. Returns 0 when the tag verifies, -1 on tag
 * mismatch or invalid arguments. On mismatch pt_out must be treated as
 * invalid.
 */
int aes_ccm_decrypt(const uint8_t *key, int keybits,
                    const uint8_t *nonce, int nlen,
                    const uint8_t *aad, int aadlen,
                    const uint8_t *ct, int ctlen,
                    const uint8_t *tag, int taglen,
                    uint8_t *pt_out);

/*
 * ccm_selftest -- NIST SP 800-38C Appendix C known-answer tests (examples
 * 1, 2 and 3) plus an encrypt->decrypt round-trip and a tamper-detection
 * check. Returns 0 on PASS, non-zero on the first failure.
 */
int ccm_selftest(void);

#endif /* CRYPTO_CCM_H */
