/*
 * x25519.h -- X25519 (Curve25519) ECDH, RFC 7748.
 * =================================================
 *
 * Freestanding pure computation: no libc, no syscalls, no malloc, no standard
 * headers. Fixed buffers; own memset/memcpy. Compatible with the build flags:
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * objdump MUST NOT show any "fs:0x28" canary references.
 *
 * ALGORITHM
 * ---------
 *   Montgomery-ladder scalar multiplication over GF(2^255-19).
 *   Field representation: 5 limbs of 51 bits each, stored in uint64_t.
 *   Mul/square use unsigned __int128 for 64x64->128 intermediate products
 *   (GCC supports __int128 fully freestanding on x86-64).
 *   The ladder is constant-time: branch-free conditional swap (cswap) on a
 *   secret-dependent bit; no secret-dependent memory accesses.
 *
 * SCALAR CLAMPING (RFC 7748 §5)
 * ------------------------------
 *   scalar[0]  &= 248   (clear bits 0-2)
 *   scalar[31] &= 127   (clear bit 255)
 *   scalar[31] |=  64   (set   bit 254)
 *
 * USAGE
 * -----
 *   // ECDH: generate ephemeral keypair + shared secret
 *   unsigned char priv[32], pub[32], peer_pub[32], shared[32];
 *   // ... fill priv with 32 random bytes ...
 *   x25519_base(pub, priv);          // our public key
 *   x25519(shared, priv, peer_pub);  // shared secret
 *
 *   // Self-test (checks RFC 7748 vectors)
 *   if (x25519_selftest() != 0) { ... crypto broken ... }
 */

#ifndef CRYPTO_X25519_H
#define CRYPTO_X25519_H

/*
 * x25519 -- compute shared = scalar * point (Montgomery X-coordinate).
 *
 * scalar : 32-byte little-endian scalar (clamped internally per RFC 7748).
 * point  : 32-byte little-endian u-coordinate of the peer's public key.
 *           Bit 255 is masked to 0 before use, as required by RFC 7748.
 * out    : 32-byte result (little-endian u-coordinate of the output point).
 *
 * Returns 0 on success.  The function never fails in the cryptographic sense
 * (it silently handles the low-order subgroup by design of X25519); the return
 * value is provided for API uniformity.
 */
int x25519(unsigned char out[32],
           const unsigned char scalar[32],
           const unsigned char point[32]);

/*
 * x25519_base -- compute out = scalar * basepoint(9).
 *
 * Equivalent to x25519(out, scalar, {9,0,...,0}).
 * Used to derive a public key from a private scalar.
 *
 * Returns 0 on success.
 */
int x25519_base(unsigned char out[32], const unsigned char scalar[32]);

/*
 * x25519_selftest -- verify the RFC 7748 §5.2 known-answer vectors.
 *
 * Checks both of the two-scalar test vectors from RFC 7748 §5.2 plus the
 * single-iteration iterated test (scalar=u=9 -> known output). Returns 0 if
 * all vectors match, -1 on any mismatch.
 */
int x25519_selftest(void);

#endif /* CRYPTO_X25519_H */
