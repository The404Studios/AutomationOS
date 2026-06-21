/*
 * tls13_record.h -- TLS 1.3 record protection (RFC 8446 Section 5.2).
 * ====================================================================
 *
 * Freestanding. AEAD-protected records as used after the ServerHello in
 * TLS 1.3. Sits on the existing AES-GCM and ChaCha20-Poly1305 primitives.
 *
 *   nonce            = static_iv XOR seq    (seq big-endian, right-aligned)
 *   additional_data  = the 5-byte record header
 *                      opaque_type(0x17) || legacy_version(0x0303) || length
 *   inner plaintext  = content || real_content_type(1) || zero padding
 *
 * aead_id: 0 = AES-128-GCM (16-byte key), 1 = AES-256-GCM (32-byte key),
 *          2 = ChaCha20-Poly1305 (32-byte key). All use a 12-byte iv + 16-byte tag.
 */
#ifndef TLS13_RECORD_H
#define TLS13_RECORD_H

#define TLS13_AEAD_AES128_GCM 0
#define TLS13_AEAD_AES256_GCM 1
#define TLS13_AEAD_CHACHA20   2

/*
 * Open (decrypt+authenticate) one TLS 1.3 record.
 *   record/record_len : the whole wire record (5-byte header + ciphertext + tag)
 *   out/out_cap       : buffer for the inner content (needs >= record_len-5-16)
 *   *out_len          : set to the inner content length (padding + type stripped)
 *   *inner_type       : set to the real content type (e.g. 0x16 handshake, 0x17 appdata)
 * Returns 0 on success, negative on malformed record or auth failure.
 */
int tls13_record_open(int aead_id, const unsigned char *key,
                      const unsigned char static_iv[12], unsigned long long seq,
                      const unsigned char *record, unsigned long record_len,
                      unsigned char *out, unsigned long out_cap,
                      unsigned long *out_len, unsigned char *inner_type);

/*
 * Seal (encrypt) `content` of type `content_type` into a TLS 1.3 wire record.
 *   out_record/out_cap : buffer for the record (needs >= content_len + 5 + 1 + 16)
 *   *out_len           : total wire record length
 * Returns 0 on success, negative on error. No record padding is added.
 */
int tls13_record_seal(int aead_id, const unsigned char *key,
                      const unsigned char static_iv[12], unsigned long long seq,
                      const unsigned char *content, unsigned long content_len,
                      unsigned char content_type,
                      unsigned char *out_record, unsigned long out_cap,
                      unsigned long *out_len);

/* Round-trip self-test (seal then open) across all three AEADs. 0 = pass. */
int tls13_record_selftest(void);

#endif /* TLS13_RECORD_H */
