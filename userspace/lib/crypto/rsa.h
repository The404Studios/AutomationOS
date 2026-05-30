/*
 * rsa.h -- freestanding RSA public-key operations (PKCS#1 v1.5).
 * =============================================================
 *
 * Pure computation: NO libc, NO syscalls, NO malloc, NO standard headers.
 * Built on bignum.c's Montgomery modular exponentiation. This is the
 * public-key half of RSA only -- enough to drive a TLS 1.2 client:
 *
 *   * rsa_pkcs1_encrypt() -- RSAES-PKCS1-v1_5 encryption with the server's
 *     public key. Used to encrypt the 48-byte TLS premaster secret in the
 *     ClientKeyExchange message.
 *
 *   * rsa_pkcs1_verify() -- RSASSA-PKCS1-v1_5 signature verification. Used to
 *     check certificate signatures (and, for static-RSA-free suites, server
 *     key-exchange signatures). Recovers m = sig^e mod n and validates the
 *     EMSA-PKCS1-v1_5 DigestInfo encoding against the supplied hash.
 *
 * Only the public exponent e is ever used here, so no private key, CRT, or
 * blinding is present. Inputs are public values (ciphertext we produce,
 * signatures and certificates from the wire), so this code is not written to
 * be constant-time.
 */

#ifndef CRYPTO_RSA_H
#define CRYPTO_RSA_H

#include "bignum.h"

/* An RSA public key: modulus n and public exponent e. */
typedef struct {
    bignum n;
    bignum e;
} rsa_pubkey;

/*
 * Hash algorithm selector for rsa_pkcs1_verify().
 *
 *   RSA_HASH_SHA256  (0)  SHA-256,  32-byte digest, 19-byte DigestInfo prefix.
 *   RSA_HASH_SHA1    (1)  SHA-1,    20-byte digest, 15-byte DigestInfo prefix.
 *   RSA_HASH_SHA384  (2)  SHA-384,  48-byte digest, 19-byte DigestInfo prefix.
 *   RSA_HASH_SHA512  (3)  SHA-512,  64-byte digest, 19-byte DigestInfo prefix.
 *
 * Any other value is rejected with error 3 ("unknown hash alg").
 * Passing a hash buffer whose length does not match the expected digest size for
 * the selected alg is rejected with error 4 ("wrong digest size").
 *
 * DigestInfo DER prefixes follow RFC 8017 sec 9.2 / RFC 3447 appendix B.1:
 *
 *   SHA-384: 30 41 30 0d 06 09 60 86 48 01 65 03 04 02 02 05 00 04 30
 *            OID id-sha384 = 2.16.840.1.101.3.4.2.2
 *   SHA-512: 30 51 30 0d 06 09 60 86 48 01 65 03 04 02 03 05 00 04 40
 *            OID id-sha512 = 2.16.840.1.101.3.4.2.3
 */
#define RSA_HASH_SHA256 0
#define RSA_HASH_SHA1   1
#define RSA_HASH_SHA384 2
#define RSA_HASH_SHA512 3

/* Load a public key from big-endian modulus and exponent byte strings. */
void rsa_pubkey_from_bytes(rsa_pubkey *pk,
                           const unsigned char *mod_be, unsigned long mod_len,
                           const unsigned char *exp_be, unsigned long exp_len);

/*
 * RSAES-PKCS1-v1_5 encryption (RFC 8017 sec 7.2.1).
 *
 *   EM = 0x00 || 0x02 || PS || 0x00 || M
 *
 * where PS is at least 8 nonzero padding bytes such that |EM| == k (the
 * modulus byte length). The caller supplies a pool of nonzero random bytes in
 * rnd[0..rnd_len); these are consumed to fill PS (any zero byte in the pool is
 * skipped, so the pool should contain more than the minimum number of nonzero
 * bytes). out_len MUST equal the modulus byte length k.
 *
 * Returns 0 on success; nonzero on error (bad lengths, message too long, or
 * insufficient nonzero randomness to build at least 8 padding bytes).
 */
int rsa_pkcs1_encrypt(const rsa_pubkey *pk,
                      const unsigned char *msg, unsigned long msg_len,
                      const unsigned char *rnd, unsigned long rnd_len,
                      unsigned char *out, unsigned long out_len);

/*
 * RSASSA-PKCS1-v1_5 signature verification (RFC 8017 sec 8.2.2).
 *
 * Computes m = sig^e mod n, then checks the EMSA-PKCS1-v1_5 encoding:
 *
 *   EM = 0x00 || 0x01 || 0xFF...0xFF || 0x00 || DigestInfo
 *
 * where DigestInfo is the DER-encoded algorithm OID prefix for hash_alg
 * followed by the raw hash. The recovered hash is compared against
 * hash[0..hash_len). sig_len must equal the modulus byte length.
 *
 * hash_alg is one of RSA_HASH_SHA256 (0), RSA_HASH_SHA1 (1),
 * RSA_HASH_SHA384 (2), or RSA_HASH_SHA512 (3).
 *
 * Returns 0 if the signature is valid, nonzero otherwise.
 */
int rsa_pkcs1_verify(const rsa_pubkey *pk,
                     const unsigned char *sig, unsigned long sig_len,
                     const unsigned char *hash, unsigned long hash_len,
                     int hash_alg);

/*
 * Small known-answer self-test. Builds a tiny RSA key from two small primes,
 * exercises bn_mod_exp round-trips (m^e^d == m), and checks a PKCS#1 v1.5
 * signature-verify path against a constructed signature. Returns 0 on pass,
 * nonzero on the first failure.
 */
int rsa_selftest(void);

#endif /* CRYPTO_RSA_H */
