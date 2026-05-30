/*
 * hmac.h -- freestanding HMAC (RFC 2104).
 * =======================================
 *
 * Pure computation: no libc, no syscalls, no malloc. HMAC-SHA256 and
 * HMAC-SHA1 over caller-supplied key/message buffers. Used for TLS record
 * MACs, the TLS PRF/HKDF, and cookie signing.
 *
 *   out = H( (key' ^ opad) || H( (key' ^ ipad) || msg ) )
 *
 * Keys longer than the block size (64 bytes) are first hashed; shorter keys
 * are zero-padded. Output is the full digest (32 or 20 bytes).
 */

#ifndef CRYPTO_HMAC_H
#define CRYPTO_HMAC_H

void hmac_sha256(const unsigned char *key, unsigned long klen,
                 const unsigned char *msg, unsigned long mlen,
                 unsigned char out[32]);

void hmac_sha1(const unsigned char *key, unsigned long klen,
               const unsigned char *msg, unsigned long mlen,
               unsigned char out[20]);

/* HMAC-SHA384 (128-byte block size) -- used by the TLS 1.2 P_SHA384 PRF for
 * the GCM_SHA384 / ECDHE-SHA384 cipher suites. */
void hmac_sha384(const unsigned char *key, unsigned long klen,
                 const unsigned char *msg, unsigned long mlen,
                 unsigned char out[48]);

#endif /* CRYPTO_HMAC_H */
