/*
 * p256_internal.h -- internal P-256 field/group primitives exposed for SAE.
 * =========================================================================
 *
 * The public P-256 API (p256.h) only ships keygen/ecdh/ecdsa_verify/selftest.
 * WPA3 SAE (the "dragonfly" handshake, sae.c) needs direct access to the
 * underlying field arithmetic, the group law, and the curve constants in
 * order to:
 *   - derive the password element PWE by hunting-and-pecking (needs a field
 *     square-root and a quadratic-residue test), and
 *   - run the commit/confirm exchange (needs point add / scalar-mul / negate
 *     and reduction mod the group order n).
 *
 * These symbols are the SAME functions used by the KAT-proven ECDH/ECDSA
 * paths in p256.c -- they were promoted from `static` to external linkage
 * WITHOUT any behavioural change.  This header merely declares them so a
 * second translation unit (sae.c) can call them.
 *
 * IMPORTANT: this is an INTERNAL header.  Field elements are NOT reduced or
 * validated by most of these primitives; callers are responsible for the
 * preconditions documented in p256.c (e.g. inputs to fe_add/fe_sub must be
 * < p).  Do not expose this header outside the crypto library.
 *
 * Build flags (same as the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#ifndef CRYPTO_P256_INTERNAL_H
#define CRYPTO_P256_INTERNAL_H

/* -------------------------------------------------------------------------
 * Limb types and aggregate types.
 *
 * These mirror the definitions in p256.c verbatim.  p256.c includes this
 * header (instead of redefining them locally) so the single definition is
 * shared; sae.c includes it too.
 * ---------------------------------------------------------------------- */

typedef unsigned int       p256_u32;
typedef unsigned long long p256_u64;

/* A 256-bit field element: eight 32-bit limbs, little-endian (w[0] = LSW). */
typedef struct { p256_u32 w[8]; } p256_fe;

/* A projective (Jacobian) point (X:Y:Z); affine = (X/Z^2, Y/Z^3), Z==0 = inf. */
typedef struct { p256_fe X; p256_fe Y; p256_fe Z; } p256_pt;

/* -------------------------------------------------------------------------
 * Curve constants (defined once in p256.c).
 * ---------------------------------------------------------------------- */
extern const p256_fe P256_FIELD_P;   /* field prime p                       */
extern const p256_fe P256_ORDER_N;   /* group order n (a.k.a. r/q)          */
extern const p256_fe P256_CURVE_B;   /* curve coefficient b                 */
extern const p256_fe P256_BASE_GX;   /* generator G x                       */
extern const p256_fe P256_BASE_GY;   /* generator G y                       */

/* -------------------------------------------------------------------------
 * Field arithmetic mod p.  Inputs to add/sub/neg must already be < p.
 * ---------------------------------------------------------------------- */
void p256_fe_zero(p256_fe *out);                                   /* out = 0      */
void p256_fe_copy(p256_fe *out, const p256_fe *a);                 /* out = a      */
int  p256_fe_is_zero(const p256_fe *a);                            /* a == 0 ?     */
int  p256_fe_cmp(const p256_fe *a, const p256_fe *b);              /* -1/0/+1      */
void p256_fe_add(p256_fe *out, const p256_fe *a, const p256_fe *b);/* a+b mod p    */
void p256_fe_sub(p256_fe *out, const p256_fe *a, const p256_fe *b);/* a-b mod p    */
void p256_fe_mul(p256_fe *out, const p256_fe *a, const p256_fe *b);/* a*b mod p    */
void p256_fe_sqr(p256_fe *out, const p256_fe *a);                  /* a^2 mod p    */
void p256_fe_inv(p256_fe *out, const p256_fe *a);                  /* a^-1 mod p   */
void p256_fe_neg(p256_fe *out, const p256_fe *a);                  /* -a mod p     */

/* Byte (de)serialization: 32-byte big-endian <-> fe (no reduction). */
void p256_fe_from_bytes(p256_fe *out, const unsigned char b[32]);
void p256_fe_to_bytes(unsigned char b[32], const p256_fe *in);

/*
 * p256_fe_pow: out = base ^ exp mod p, where exp is an arbitrary 256-bit
 * exponent given as eight 32-bit little-endian limbs (exp[0] = LSW).
 * Square-and-multiply, MSB-first, 256 squarings.  base must be < p.
 */
void p256_fe_pow(p256_fe *out, const p256_fe *base, const p256_u32 exp[8]);

/*
 * p256_fe_sqrt: out = sqrt(a) mod p, valid because p === 3 (mod 4) so the
 * root is the single exponentiation a^((p+1)/4) mod p.  Returns 1 if a is a
 * quadratic residue (out*out == a) and 0 otherwise (out left undefined-ish).
 */
int p256_fe_sqrt(p256_fe *out, const p256_fe *a);

/*
 * p256_fe_is_quadratic_residue: returns 1 if a is a non-zero quadratic
 * residue mod p (a^((p-1)/2) == 1), else 0.  (0 is reported as 0/non-QR.)
 */
int p256_fe_is_quadratic_residue(const p256_fe *a);

/* -------------------------------------------------------------------------
 * Arithmetic mod n (the group order).  Used for SAE scalars.
 * ---------------------------------------------------------------------- */
int  p256_fe_cmp_n(const p256_fe *a);                              /* cmp vs n     */
void p256_fe_cond_sub_n(p256_fe *a);                               /* if a>=n,a-=n */
void p256_fe_mul_n(p256_fe *out, const p256_fe *a, const p256_fe *b); /* a*b mod n */
void p256_fe_inv_n(p256_fe *out, const p256_fe *a);                /* a^-1 mod n   */

/* -------------------------------------------------------------------------
 * Point validation and group law (Jacobian).
 * ---------------------------------------------------------------------- */
int  p256_point_on_curve(const p256_fe *x, const p256_fe *y);      /* on curve?    */
void p256_pt_infinity(p256_pt *P);                                 /* P = O        */
int  p256_pt_is_infinity(const p256_pt *P);                        /* P == O ?     */
void p256_pt_from_affine(p256_pt *P, const p256_fe *x, const p256_fe *y);
void p256_pt_to_affine(p256_fe *ax, p256_fe *ay, const p256_pt *P);/* P != O       */
void p256_pt_add(p256_pt *R, const p256_pt *P, const p256_pt *Q);  /* R = P + Q    */
void p256_pt_dbl(p256_pt *R, const p256_pt *P);                    /* R = 2P       */

/*
 * p256_pt_scalar_mul: R = k * P, where k is a 32-byte big-endian scalar.
 * Left-to-right double-and-add. NOT constant-time: the per-bit branch leaks the
 * scalar via timing. Fine for the public ECDH/ECDSA points, but SAE feeds SECRET
 * scalars (rand/mask/peer_scalar) through this -- a Dragonblood-class side
 * channel. Acceptable on this OS (no remote timing adversary) but NOT hardened;
 * a constant-time ladder is the fix if that ever changes.
 */
void p256_pt_scalar_mul(p256_pt *R, const p256_pt *P, const unsigned char k[32]);

#endif /* CRYPTO_P256_INTERNAL_H */
