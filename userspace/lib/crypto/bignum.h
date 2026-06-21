/*
 * bignum.h -- freestanding fixed-capacity unsigned big integers.
 * =============================================================
 *
 * Pure computation: NO libc, NO syscalls, NO malloc, NO standard headers.
 * Everything operates on fixed-size, caller- or stack-allocated `bignum`
 * structures. The only "memory" primitives used are private memcpy/memset
 * defined inside bignum.c.
 *
 * Numbers are UNSIGNED magnitudes stored little-endian in an array of
 * 32-bit words: w[0] is the least significant word. `n` is the count of
 * significant words (the number is normalised so that w[n-1] != 0, except
 * that zero has n == 0). All words at index >= n are guaranteed to be 0.
 *
 * Capacity is BN_WORDS * 32 bits. With BN_WORDS == 128 that is 4096 bits of
 * value storage, which is enough to hold a 4096-bit RSA modulus and also the
 * intermediate products that arise during modular reduction (a full
 * schoolbook product of two N-word numbers needs 2N words, so callers that
 * need a 2048-bit modulus should keep operands comfortably within capacity;
 * see bignum.c for the internal wide-product handling that keeps everything
 * within fixed buffers).
 *
 * The headline routine is bn_mod_exp(), which computes base^exp mod mod using
 * Montgomery multiplication (for odd moduli, which all RSA moduli are) with a
 * square-and-multiply ladder. That is what backs RSA in rsa.c.
 */

#ifndef CRYPTO_BIGNUM_H
#define CRYPTO_BIGNUM_H

/*
 * 128 32-bit words = 4096 bits of value capacity. This sizes the public
 * `bignum` so that a 4096-bit modulus fits. Internal routines that form
 * double-width products use a wider scratch type defined privately in
 * bignum.c, so this capacity bounds the *values* callers manipulate, not the
 * transient products.
 */
#define BN_WORDS 128

typedef struct {
    unsigned int w[BN_WORDS]; /* little-endian 32-bit words, w[0] = LSW */
    int          n;           /* number of significant words (w[n-1]!=0) */
} bignum;

/* ---- construction / serialisation ------------------------------------ */

/* Load a big-endian byte string be[0..len) into x. */
void bn_from_bytes(bignum *x, const unsigned char *be, unsigned long len);

/* Store x as a big-endian byte string of exactly `len` bytes, left-padded
 * with zeros. If x does not fit in `len` bytes the high bytes are truncated
 * (callers should always pass len >= the modulus length). */
void bn_to_bytes(const bignum *x, unsigned char *be, unsigned long len);

/* x = v (a single 32-bit value). */
void bn_set_u32(bignum *x, unsigned int v);

/* x = 0. */
void bn_set_zero(bignum *x);

/* Copy: dst = src. */
void bn_copy(bignum *dst, const bignum *src);

/* ---- comparison / predicates ----------------------------------------- */

/* Returns -1 if a<b, 0 if a==b, +1 if a>b (unsigned magnitude compare). */
int bn_cmp(const bignum *a, const bignum *b);

/* Returns nonzero if x == 0. */
int bn_is_zero(const bignum *x);

/* Bit length of x (0 for zero, otherwise index of MSB + 1). */
int bn_bit_length(const bignum *x);

/* ---- arithmetic ------------------------------------------------------- */

/* The headline operation: out = base^exp mod mod.
 *
 * `mod` must be odd and nonzero (true for every RSA modulus). Uses
 * Montgomery multiplication internally for efficiency at 2048/4096 bits.
 * All operands are reduced mod `mod` as needed; out may alias none of the
 * inputs (callers pass a distinct destination). */
void bn_mod_exp(bignum *out, const bignum *base, const bignum *exp,
                const bignum *mod);

/* ---- generic modular arithmetic (any modulus; used by the EC code) ---- *
 *
 * Correctness-first helpers built on shift/subtract reduction. bn_mod()
 * reduces any input; bn_mod_add/sub/mul assume operands already in [0, m).
 * Results are fully reduced into [0, m). Modular inverse for a PRIME modulus
 * is obtained via Fermat: bn_mod_exp(out, a, m-2, m). */
void bn_mod    (bignum *r, const bignum *a, const bignum *m);
void bn_mod_add(bignum *r, const bignum *a, const bignum *b, const bignum *m);
void bn_mod_sub(bignum *r, const bignum *a, const bignum *b, const bignum *m);
void bn_mod_mul(bignum *r, const bignum *a, const bignum *b, const bignum *m);

#endif /* CRYPTO_BIGNUM_H */
