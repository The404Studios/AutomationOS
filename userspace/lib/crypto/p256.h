/*
 * p256.h -- NIST P-256 (secp256r1) ECDH + ECDSA verification.
 * =============================================================
 *
 * Pure freestanding computation: NO syscalls, NO libc, NO malloc,
 * NO standard headers.  Fixed stack buffers only.  Build flags must
 * include -ffreestanding -nostdlib -fno-builtin -fno-stack-protector.
 *
 * Curve: y^2 = x^3 - 3x + b  over  GF(p)
 *   p = 2^256 - 2^224 + 2^192 + 2^96 - 1  (NIST P-256 prime, FIPS 186-4)
 *   n = FIPS 186-4 group order
 *   G = standard base point (FIPS 186-4 / SEC2)
 *
 * Public-key wire format: 65-byte uncompressed point: 0x04 || X[32] || Y[32],
 * all coordinates big-endian.  Scalars and r/s values are also big-endian
 * 32-byte arrays.
 *
 * API:
 *   p256_keygen        -- given a 32-byte private scalar, compute the public key
 *   p256_ecdh          -- ECDH shared-secret (X coordinate of priv * peer)
 *   p256_ecdsa_verify  -- verify an ECDSA signature over a pre-hashed message
 *   p256_selftest      -- run built-in NIST CAVP / RFC test vectors; 0 = pass
 */

#ifndef CRYPTO_P256_H
#define CRYPTO_P256_H

/*
 * p256_keygen -- compute the uncompressed public key for a given private key.
 *
 *   priv[32]   : big-endian private scalar (must be in [1, n-1]; callers
 *                must supply a uniformly random value from a CSPRNG)
 *   pub65[65]  : output -- 0x04 || Qx[32] || Qy[32], big-endian
 *
 *   Returns 0 on success, -1 if priv is zero or >= n.
 */
int p256_keygen(unsigned char priv[32], unsigned char pub65[65]);

/*
 * p256_ecdh -- ECDH raw shared secret (X coordinate of priv * peer_pub).
 *
 *   out_x[32]     : big-endian X coordinate of the shared point
 *   priv[32]      : our private scalar (big-endian)
 *   peer_pub65[65]: uncompressed peer public key (0x04 || X || Y)
 *
 *   Returns 0 on success, -1 if the peer key is invalid (wrong prefix,
 *   coordinates not on the curve, point at infinity).
 */
int p256_ecdh(unsigned char out_x[32],
              const unsigned char priv[32],
              const unsigned char peer_pub65[65]);

/*
 * p256_ecdsa_verify -- verify an ECDSA-P256 signature.
 *
 *   pub65[65]  : uncompressed signer public key
 *   hash       : pre-computed message digest (any length; leftmost 256 bits
 *                used, zero-padded if shorter)
 *   hlen       : byte length of hash
 *   r[32]      : big-endian signature r component
 *   s[32]      : big-endian signature s component
 *
 *   Returns 0 if the signature is valid, -1 otherwise.
 *   Rejects r or s outside [1, n-1]; rejects the public key if not on curve.
 */
int p256_ecdsa_verify(const unsigned char pub65[65],
                      const unsigned char *hash, unsigned long hlen,
                      const unsigned char r[32], const unsigned char s[32]);

/*
 * p256_selftest -- run built-in known-answer tests.
 *
 *   Uses a NIST CAVP ECDSA-P256 verification vector (SHA-256, from the
 *   NIST SigVer test suite) and a scalar-multiplication consistency check.
 *
 *   Returns 0 if all tests pass, -1 if any test fails.
 */
int p256_selftest(void);

#endif /* CRYPTO_P256_H */
