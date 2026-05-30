/*
 * hkdf.h -- freestanding HKDF (RFC 5869) over SHA-256 and SHA-384.
 * =================================================================
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
 *   ../crypto/hmac.h   -- hmac_sha256(key,klen,msg,mlen,out[32])
 *   ../crypto/sha512.h -- sha384_init/update/final (for HMAC-SHA-384)
 *
 * -------------------------------------------------------------------------
 * HKDF-SHA-256 (hash length HashLen = 32 bytes)
 * -------------------------------------------------------------------------
 *
 * Extract:
 *   HKDF-Extract(salt, IKM) -> PRK (32 bytes)
 *   PRK = HMAC-SHA256(salt, IKM)
 *   If salt is NULL / saltlen == 0, salt defaults to a 32-byte zero block.
 *
 * Expand:
 *   HKDF-Expand(PRK, info, outlen) -> OKM (outlen bytes)
 *   T(0) = empty string
 *   T(i) = HMAC-SHA256(PRK, T(i-1) || info || byte(i))   i = 1,2,...
 *   OKM  = T(1) || T(2) || ...  (first outlen bytes)
 *   Constraint: outlen <= 255 * 32.  Returns -1 if outlen exceeds this.
 *
 * -------------------------------------------------------------------------
 * HKDF-SHA-384 (hash length HashLen = 48 bytes)
 * -------------------------------------------------------------------------
 * Same construction with SHA-384 (HMAC block size 128 bytes, output 48 bytes).
 * PRK is 48 bytes; outlen <= 255 * 48.
 *
 * -------------------------------------------------------------------------
 * TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1)
 * -------------------------------------------------------------------------
 * Builds the HkdfLabel wire structure, then calls the appropriate
 * HKDF-Expand variant:
 *
 *   struct HkdfLabel {
 *       uint16_t length;               // outlen in network byte order
 *       uint8_t  label_len;            // length of "tls13 " + label
 *       uint8_t  label[label_len];     // "tls13 " + label (ASCII)
 *       uint8_t  context_len;          // length of context (0..255)
 *       uint8_t  context[context_len]; // caller-supplied context hash
 *   };
 *
 * hash_id: 0 = SHA-256, 1 = SHA-384.
 * Returns -1 on length overflow or if label + "tls13 " > 249 bytes.
 *
 * -------------------------------------------------------------------------
 * Self-test
 * -------------------------------------------------------------------------
 * hkdf_selftest() runs RFC 5869 Appendix A test cases 1 and 3 (SHA-256).
 * Returns 0 on success, -1 on any mismatch.
 */

#ifndef CRYPTO_HKDF_H
#define CRYPTO_HKDF_H

/* -------------------------------------------------------------------------
 * SHA-256 variants (PRK = 32 bytes)
 * ---------------------------------------------------------------------- */

/*
 * HKDF-Extract-SHA-256 (RFC 5869 §2.2).
 * Writes 32-byte PRK into prk[].
 * If saltlen == 0 (salt may be NULL), the salt is treated as a 32-byte
 * string of zero octets (RFC 5869 §2.2 default).
 */
void hkdf_extract_sha256(const unsigned char *salt, unsigned long saltlen,
                          const unsigned char *ikm,  unsigned long ikmlen,
                          unsigned char prk[32]);

/*
 * HKDF-Expand-SHA-256 (RFC 5869 §2.3).
 * Fills out[0..outlen-1] with keying material derived from prk and info.
 * Returns 0 on success, -1 if outlen > 255*32 = 8160.
 */
int hkdf_expand_sha256(const unsigned char prk[32],
                        const unsigned char *info, unsigned long infolen,
                        unsigned char *out,  unsigned long outlen);

/* -------------------------------------------------------------------------
 * SHA-384 variants (PRK = 48 bytes)
 * ---------------------------------------------------------------------- */

/*
 * HKDF-Extract-SHA-384 (RFC 5869 §2.2 with SHA-384).
 * Writes 48-byte PRK into prk[].
 * Default salt (saltlen == 0): 48-byte zero block.
 */
void hkdf_extract_sha384(const unsigned char *salt, unsigned long saltlen,
                          const unsigned char *ikm,  unsigned long ikmlen,
                          unsigned char prk[48]);

/*
 * HKDF-Expand-SHA-384 (RFC 5869 §2.3 with SHA-384).
 * Returns 0 on success, -1 if outlen > 255*48 = 12240.
 */
int hkdf_expand_sha384(const unsigned char prk[48],
                        const unsigned char *info, unsigned long infolen,
                        unsigned char *out,  unsigned long outlen);

/* -------------------------------------------------------------------------
 * TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1)
 * ---------------------------------------------------------------------- */

/*
 * Derive keying material using the TLS 1.3 label construction.
 *
 *   hash_id  : 0 = SHA-256 (HashLen=32), 1 = SHA-384 (HashLen=48)
 *   secret   : input keying material (secretlen bytes)
 *   label    : ASCII label string (NOT including "tls13 " prefix; that is
 *               prepended automatically).  "tls13 " + label must be <= 249
 *               bytes total so that label_len fits in one byte.
 *   context  : context value (typically a transcript hash), ctxlen bytes
 *   out      : output buffer
 *   outlen   : requested output length
 *
 * Returns 0 on success, -1 on any error (label too long, outlen too large,
 * secretlen != HashLen for expand, etc.).
 *
 * Note: RFC 8446 feeds the full-length secret directly as the PRK parameter
 * of HKDF-Expand; it expects secretlen == HashLen for the chosen hash.
 * This implementation enforces that requirement.
 */
int tls13_hkdf_expand_label(int hash_id,
                             const unsigned char *secret,
                             unsigned long        secretlen,
                             const char          *label,
                             const unsigned char *context,
                             unsigned long        ctxlen,
                             unsigned char       *out,
                             unsigned long        outlen);

/* -------------------------------------------------------------------------
 * Self-test
 * ---------------------------------------------------------------------- */

/*
 * Run built-in known-answer tests (RFC 5869 Appendix A, cases 1 and 3).
 * Returns 0 if all vectors match, -1 on the first mismatch.
 */
int hkdf_selftest(void);

#endif /* CRYPTO_HKDF_H */
