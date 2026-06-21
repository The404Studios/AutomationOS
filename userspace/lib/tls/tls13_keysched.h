/*
 * tls13_keysched.h -- TLS 1.3 key schedule (RFC 8446 Section 7.1).
 * ================================================================
 *
 * Freestanding (no libc/syscalls/malloc). Builds the TLS 1.3 secret ladder on
 * top of the HKDF primitives in crypto/hkdf.h (HKDF-Extract + the already-
 * implemented tls13_hkdf_expand_label) and SHA-256/384.
 *
 *   Early Secret      = HKDF-Extract(0, PSK)                  (PSK = 0 if none)
 *   Derived           = Derive-Secret(Early, "derived", "")
 *   Handshake Secret  = HKDF-Extract(Derived, (EC)DHE)
 *   c/s hs traffic    = Derive-Secret(Handshake, "c hs traffic"/"s hs traffic",
 *                                     H(ClientHello..ServerHello))
 *   Derived2          = Derive-Secret(Handshake, "derived", "")
 *   Master Secret     = HKDF-Extract(Derived2, 0)
 *   c/s ap traffic    = Derive-Secret(Master, "c ap traffic"/"s ap traffic",
 *                                     H(ClientHello..server Finished))
 *
 * Per-secret record-protection keys:
 *   key = HKDF-Expand-Label(secret, "key", "", key_len)
 *   iv  = HKDF-Expand-Label(secret, "iv",  "", 12)
 * Finished MAC key:
 *   finished_key = HKDF-Expand-Label(secret, "finished", "", HashLen)
 *
 * hash_id: 0 = SHA-256 (HashLen 32), 1 = SHA-384 (HashLen 48).
 */
#ifndef TLS13_KEYSCHED_H
#define TLS13_KEYSCHED_H

/* HashLen for the chosen hash (32 or 48). */
unsigned long tls13_hashlen(int hash_id);

/* Transcript-Hash of the empty string (used by the "derived" steps). */
int tls13_empty_hash(int hash_id, unsigned char *out);

/* HKDF-Extract wrapper (salt may be NULL/0 -> zero salt). */
int tls13_extract(int hash_id,
                  const unsigned char *salt, unsigned long saltlen,
                  const unsigned char *ikm,  unsigned long ikmlen,
                  unsigned char *prk);

/* Derive-Secret(secret, label, Messages) -- context = Transcript-Hash(Messages)
 * passed in `thash` (thlen bytes, == HashLen). Writes HashLen bytes to out. */
int tls13_derive_secret(int hash_id,
                        const unsigned char *secret,
                        const char *label,
                        const unsigned char *thash, unsigned long thlen,
                        unsigned char *out);

/* Expand a traffic secret into a record-protection key + iv. */
int tls13_traffic_keyiv(int hash_id, const unsigned char *secret,
                        unsigned char *key, unsigned long keylen,
                        unsigned char *iv,  unsigned long ivlen);

/* finished_key = HKDF-Expand-Label(secret, "finished", "", HashLen). */
int tls13_finished_key(int hash_id, const unsigned char *secret,
                       unsigned char *out);

/* Finished verify_data = HMAC(finished_key, transcript_hash), where
 * finished_key is derived from the handshake traffic secret of the sender
 * (RFC 8446 4.4.4). Writes HashLen bytes to out. */
int tls13_finished_verify(int hash_id, const unsigned char *traffic_secret,
                          const unsigned char *transcript_hash,
                          unsigned long thlen, unsigned char *out);

/*
 * tls13_keysched_selftest -- KAT against RFC 8448 Section 3 (Simple 1-RTT
 * Handshake): drives the full ladder from the published ECDHE shared secret +
 * transcript hashes and checks every intermediate secret/key against the RFC.
 * Returns 0 on pass, or a positive step id (1..12) of the first mismatch.
 */
int tls13_keysched_selftest(void);

#endif /* TLS13_KEYSCHED_H */
