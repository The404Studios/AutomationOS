/*
 * p384.h -- NIST P-384 (secp384r1) ECDSA signature verification.
 * ===============================================================
 *
 * Pure freestanding computation: NO syscalls, NO libc, NO malloc, NO standard
 * headers. Fixed stack buffers only. Build flags must include -ffreestanding
 * -nostdlib -fno-builtin -fno-stack-protector.
 *
 * Curve: y^2 = x^3 - 3x + b  over  GF(p)
 *   p = 2^384 - 2^128 - 2^96 + 2^32 - 1   (NIST P-384 prime, FIPS 186-4)
 *   n = FIPS 186-4 group order
 *   G = standard base point
 *
 * This is the companion to p256.c: same role (ECDSA verify for X.509 chains),
 * but built on the GENERIC bignum modular arithmetic (bn_mod_mul/add/sub +
 * bn_mod_exp for inversion) rather than a P-384-specific fast reduction --
 * correctness-first, since certificate verification is not a hot path. Point
 * arithmetic uses Jacobian projective coordinates (one inversion per verify).
 *
 * Public-key wire format: 97-byte uncompressed point 0x04 || X[48] || Y[48],
 * big-endian. r and s are big-endian 48-byte arrays.
 */

#ifndef CRYPTO_P384_H
#define CRYPTO_P384_H

/*
 * p384_ecdsa_verify -- verify an ECDSA-P384 signature.
 *
 *   pub97[97] : uncompressed signer public key (0x04 || X || Y)
 *   hash      : pre-computed message digest; the leftmost 384 bits (48 bytes)
 *               are used (FIPS bits2int); shorter hashes are used whole
 *   hlen      : byte length of hash
 *   r[48]     : big-endian signature r component
 *   s[48]     : big-endian signature s component
 *
 *   Returns 0 if the signature is valid, -1 otherwise.
 *   Rejects r or s outside [1, n-1]; rejects a public key not on the curve.
 */
int p384_ecdsa_verify(const unsigned char pub97[97],
                      const unsigned char *hash, unsigned long hlen,
                      const unsigned char r[48], const unsigned char s[48]);

/*
 * p384_selftest -- run a built-in known-answer test (RFC 6979 A.2.6 vector
 * for message "sample" with SHA-384). Returns 0 if it passes, -1 otherwise.
 * Requires a SHA-384 implementation to be linked (sha384 from sha512.c).
 */
int p384_selftest(void);

#endif /* CRYPTO_P384_H */
