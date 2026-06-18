/*
 * keywrap.h -- freestanding AES Key Wrap / Unwrap (RFC 3394).
 * ============================================================
 *
 * Pure computation: no libc, no syscalls, no malloc, no standard headers.
 * Fixed buffers only.  Suitable for use in freestanding kernel/userspace
 * crypto without a C runtime.
 *
 * Build flags (no fs:0x28 stack canary, no libc, no PIC):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Depends on:
 *   ../crypto/aes.h -- aes_ctx, aes_set_encrypt_key / aes_set_decrypt_key,
 *                      aes_encrypt_block / aes_decrypt_block
 *
 * -------------------------------------------------------------------------
 * AES Key Wrap (RFC 3394)
 * -------------------------------------------------------------------------
 *
 * Wraps n 64-bit blocks of key material under a Key-Encryption-Key (KEK)
 * into n+1 64-bit blocks of wrapped output, prepending the integrity
 * register A.  Used in WPA2/WPA3 to deliver the GTK inside an EAPOL-Key
 * frame (the KEK is the KEK portion of the PTK).
 *
 * The wrap process (RFC 3394 §2.2.1):
 *   Inputs:  n 64-bit plaintext blocks P[1]..P[n], KEK.
 *   Set A = IV = A6A6A6A6A6A6A6A6, R[i] = P[i].
 *   For j = 0..5, for i = 1..n:
 *       B    = AES-ENC(KEK, A | R[i])
 *       A    = MSB64(B) ^ t,  where t = (n * j) + i  (64-bit big-endian)
 *       R[i] = LSB64(B)
 *   Output: C[0] = A, C[i] = R[i].   ((n+1)*8 bytes)
 *
 * The unwrap process (RFC 3394 §2.2.2) runs it in reverse with AES-DEC and
 * verifies that the recovered A equals the integrity IV; if it does not, the
 * unwrap fails (returns -1) and the output must be treated as invalid.
 *
 * Constraints: n_blocks must be >= 1.  kek_bits must be 128, 192, or 256
 * (whatever the underlying aes.c key schedule supports -- 128 and 256 are
 * the cases used by WPA).  The caller owns the in/out buffers; they may not
 * overlap.
 * ====================================================================== */

#ifndef CRYPTO_KEYWRAP_H
#define CRYPTO_KEYWRAP_H

/*
 * Freestanding fixed-width types.  The crypto library builds with
 * -ffreestanding -nostdlib, so we cannot include <stdint.h>.  Define the
 * two widths this API needs locally (guarded so a project-wide types
 * header, if later introduced, does not collide).
 */
#ifndef CRYPTO_KEYWRAP_HAVE_FIXED_INTS
#define CRYPTO_KEYWRAP_HAVE_FIXED_INTS
typedef unsigned char uint8_t;
#endif

/*
 * aes_key_wrap -- RFC 3394 wrap of n_blocks 64-bit blocks.
 *
 *   kek       Key-Encryption-Key bytes (kek_bits/8 bytes).
 *   kek_bits  128, 192, or 256.
 *   in        n_blocks * 8 bytes of plaintext key material.
 *   n_blocks  number of 64-bit blocks (>= 1).
 *   out       receives (n_blocks + 1) * 8 bytes of wrapped output.
 *
 * The integrity register uses the default IV A6A6A6A6A6A6A6A6.
 * Returns 0 on success, -1 on invalid arguments.
 */
int aes_key_wrap(const uint8_t *kek, int kek_bits,
                 const uint8_t *in, int n_blocks, uint8_t *out);

/*
 * aes_key_unwrap -- inverse of aes_key_wrap.
 *
 *   kek       Key-Encryption-Key bytes (kek_bits/8 bytes).
 *   kek_bits  128, 192, or 256.
 *   in        (n_blocks + 1) * 8 bytes of wrapped input.
 *   n_blocks  number of 64-bit plaintext blocks expected (>= 1).
 *   out       receives n_blocks * 8 bytes of recovered key material.
 *
 * Verifies the recovered integrity register equals A6A6A6A6A6A6A6A6.
 * Returns 0 on success, -1 on integrity failure or invalid arguments.
 * On integrity failure the contents of out are unspecified and must not
 * be used.
 */
int aes_key_unwrap(const uint8_t *kek, int kek_bits,
                   const uint8_t *in, int n_blocks, uint8_t *out);

/*
 * keywrap_selftest -- RFC 3394 §4.1 known-answer test (128-bit KEK,
 * 128-bit key data) plus a round-trip and a tamper-detection check.
 * Returns 0 on PASS, non-zero on any failure.
 */
int keywrap_selftest(void);

#endif /* CRYPTO_KEYWRAP_H */
