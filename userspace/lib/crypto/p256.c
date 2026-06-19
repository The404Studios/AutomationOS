/*
 * p256.c -- NIST P-256 (secp256r1) ECDH + ECDSA verification.
 * =============================================================
 *
 * Freestanding, no-libc, no-malloc.  See p256.h for the public API.
 *
 * -----------------------------------------------------------------------
 * Design overview
 * -----------------------------------------------------------------------
 *
 * Field representation
 *   Every field element is stored as eight 32-bit limbs in little-endian
 *   order: fe[0] is the least-significant 32-bit word.
 *   Value = fe[0] + fe[1]*2^32 + ... + fe[7]*2^224.
 *   Fully reduced canonical form satisfies  val < p.
 *
 * Fast reduction mod p (NIST special form)
 *   p = 2^256 - 2^224 + 2^192 + 2^96 - 1
 *   A 512-bit intermediate product T (sixteen 32-bit words c[0..15]) is
 *   reduced by forming the six auxiliary 256-bit values s1..s6 defined in
 *   FIPS 186-4 Appendix D.1.2 / SEC2 and computing:
 *       T mod p = T + 2*s1 + 2*s2 + s3 + s4 - s5 - s6  (mod p)
 *   This is implemented as a sequence of 256-bit additions and subtractions
 *   on the eight-word representation, with a final conditional reduction.
 *
 * Point representation
 *   Jacobian projective coordinates (X:Y:Z) where the affine point is
 *   (X/Z^2, Y/Z^3).  The point at infinity is represented as Z == 0.
 *   All arithmetic stays in projective form; only final output converts to
 *   affine (one field inversion).
 *
 * Scalar multiplication
 *   Left-to-right double-and-add.  The exponents processed here are public
 *   (keys in ECDH are public once transmitted; ECDSA verify operates only
 *   on public material), so timing side-channels are not a security concern
 *   for this use case.
 *
 * Inversion
 *   Fermat's little theorem: a^-1 = a^(p-2) mod p.  Implemented as a
 *   square-and-multiply loop over the 256 bits of p-2 (256 squarings,
 *   ~128 multiplications on average).  Inversion mod n uses the same
 *   approach with n-2.
 *
 * -----------------------------------------------------------------------
 * NIST P-256 constants (FIPS 186-4, Appendix D)
 * -----------------------------------------------------------------------
 *
 * p  = FFFFFFFF 00000001 00000000 00000000
 *      00000000 FFFFFFFF FFFFFFFF FFFFFFFF
 *
 * n  = FFFFFFFF 00000000 FFFFFFFF FFFFFFFF
 *      BCE6FAAD A7179E84 F3B9CAC2 FC632551
 *
 * b  = 5AC635D8 AA3A93E7 B3EBBD55 769886BC
 *      651D06B0 CC53B0F6 3BCE3C3E 27D2604B
 *
 * Gx = 6B17D1F2 E12C4247 F8BCE6E5 63A440F2
 *      77037D81 2DEB33A0 F4A13945 D898C296
 *
 * Gy = 4FE342E2 FE1A7F9B 8EE7EB4A 7C0F9E16
 *      2BCE3357 6B315ECE CBB64068 37BF51F5
 */

#include "p256.h"
#include "p256_internal.h"   /* shared limb/point types + the SAE-facing API */

/* =========================================================================
 * Internal types
 * =========================================================================
 *
 * The limb/aggregate types live in p256_internal.h so that sae.c can share
 * the exact same layout.  We keep the short local names (u32/u64/fe/pt) used
 * throughout this file as aliases to the header types -- this is purely a
 * spelling convenience and changes no behaviour.
 */

typedef p256_u32 u32;
typedef p256_u64 u64;

/* A 256-bit field element, little-endian 32-bit limbs. */
typedef p256_fe fe;

/* A projective (Jacobian) point on P-256. */
typedef p256_pt pt;

/* =========================================================================
 * Private memory helpers (no libc)
 * ========================================================================= */

static void p_memset(void *dst, int v, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  c = (unsigned char)v;
    while (n--) *d++ = c;
}

static void p_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static int p_memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

/* =========================================================================
 * Curve constants in little-endian limb form
 * ========================================================================= */

/*
 * These constants are defined with EXTERNAL linkage (the P256_* names) so the
 * SAE module can reference them via p256_internal.h.  The original local
 * names (FIELD_P/ORDER_N/CURVE_B/BASE_GX/BASE_GY) used throughout this file
 * are kept as macro aliases below, so every existing reference is byte-for-
 * byte unchanged.  The numeric values are identical to before.
 */

/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
const fe P256_FIELD_P = {{ 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
                            0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF }};

/* n = group order */
const fe P256_ORDER_N = {{ 0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
                            0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF }};

/* b (curve coefficient) */
const fe P256_CURVE_B = {{ 0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0,
                            0x769886BC, 0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8 }};

/* Base point G in affine, stored as Jacobian with Z=1. */
const fe P256_BASE_GX = {{ 0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81,
                            0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2 }};

const fe P256_BASE_GY = {{ 0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357,
                            0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2 }};

/* Local short aliases (unchanged spelling for the rest of this file). */
#define FIELD_P  P256_FIELD_P
#define ORDER_N  P256_ORDER_N
#define CURVE_B  P256_CURVE_B
#define BASE_GX  P256_BASE_GX
#define BASE_GY  P256_BASE_GY

/* =========================================================================
 * 256-bit field arithmetic mod p
 * ========================================================================= */

/* fe_zero: out = 0 */
static void fe_zero(fe *out) { p_memset(out->w, 0, sizeof(out->w)); }

/* fe_copy: out = a */
static void fe_copy(fe *out, const fe *a) { p_memcpy(out->w, a->w, sizeof(a->w)); }

/* fe_is_zero: returns 1 if a == 0 */
static int fe_is_zero(const fe *a)
{
    u32 acc = 0;
    for (int i = 0; i < 8; i++) acc |= a->w[i];
    return acc == 0;
}

/* fe_cmp: returns -1/0/+1 for a < b / a == b / a > b (magnitude, unsigned) */
static int fe_cmp(const fe *a, const fe *b)
{
    for (int i = 7; i >= 0; i--) {
        if (a->w[i] < b->w[i]) return -1;
        if (a->w[i] > b->w[i]) return  1;
    }
    return 0;
}

/* fe_cond_sub_p: if a >= p, subtract p in place (one conditional reduction). */
static void fe_cond_sub_p(fe *a)
{
    if (fe_cmp(a, &FIELD_P) < 0) return;
    u64 borrow = 0;
    for (int i = 0; i < 8; i++) {
        u64 d  = (u64)a->w[i] - (u64)FIELD_P.w[i] - borrow;
        a->w[i] = (u32)d;
        borrow  = (d >> 63) & 1;
    }
}

/* fe_add: out = a + b mod p (inputs must be < p). */
static void fe_add(fe *out, const fe *a, const fe *b)
{
    u64 carry = 0;
    for (int i = 0; i < 8; i++) {
        u64 s   = (u64)a->w[i] + (u64)b->w[i] + carry;
        out->w[i] = (u32)s;
        carry   = s >> 32;
    }
    /*
     * If carry == 1, the true sum is 2^256 + out[0..7].
     * Since a, b < p < 2^256, the true sum is in [2^256, 2p).
     * (2^256 + out) mod p = (2^256 - p) + out = out - p (mod 2^256).
     * So unconditionally subtract p when carry == 1.
     * Otherwise the sum is in [0, 2p); one conditional subtraction suffices.
     */
    if (carry) {
        u64 borrow = 0;
        for (int i = 0; i < 8; i++) {
            u64 d   = (u64)out->w[i] - (u64)FIELD_P.w[i] - borrow;
            out->w[i] = (u32)d;
            borrow  = (d >> 63) & 1;
        }
    } else {
        fe_cond_sub_p(out);
    }
}

/* fe_dbl: out = 2*a mod p */
static void fe_dbl(fe *out, const fe *a) { fe_add(out, a, a); }

/* fe_sub: out = a - b mod p (inputs must be < p). */
static void fe_sub(fe *out, const fe *a, const fe *b)
{
    /* Add p first to ensure non-negative result, then reduce. */
    u64 borrow = 0;
    for (int i = 0; i < 8; i++) {
        u64 d   = (u64)a->w[i] - (u64)b->w[i] - borrow;
        out->w[i] = (u32)d;
        borrow  = (d >> 63) & 1;
    }
    if (borrow) {
        /* result wrapped around; add p back */
        u64 carry = 0;
        for (int i = 0; i < 8; i++) {
            u64 s   = (u64)out->w[i] + (u64)FIELD_P.w[i] + carry;
            out->w[i] = (u32)s;
            carry   = s >> 32;
        }
    }
}

/* -------------------------------------------------------------------------
 * NIST P-256 fast reduction of a 512-bit product.
 *
 * Given c[0..15] (a 512-bit value, little-endian 32-bit words), compute
 * c mod p into *out.
 *
 * Method: use the identity
 *   c mod p = c[0..7] + sum(c[k] * R[k-8], k=8..15)  mod p
 * where R[i] = 2^(32*(i+8)) mod p (precomputed unsigned 8-word constants).
 *
 * Products c[k]*R[i][j] are split into 32-bit lo/hi halves and accumulated
 * into a 16-word u64 array.  A full carry pass leaves each of acc[8..15]
 * at most 2^32-1.  A second accumulation pass reduces acc[8..15] the same
 * way, after which acc[8] is at most a few units and handled iteratively.
 *
 * All products fit in u64: max(c[k]) * max(R[i][j]) = (2^32-1)^2 < 2^64.
 *
 * Reference: FIPS 186-4 D.1.2, constants derived from 2^(32k) mod p.
 * -------------------------------------------------------------------------*/

/*
 * REDUCE_R[i][j] = j-th 32-bit LE word of 2^(32*(i+8)) mod p, i=0..7.
 * Verified: R[0] = 2^256 mod p = 2^224 - 2^192 - 2^96 + 1
 *         = 0x00000000_FFFFFFFE_FFFFFFFF_FFFFFFFF_FFFFFFFF_00000000_00000000_00000001
 *           (LE: w0=0x00000001, w3=0xFFFFFFFF, w4=0xFFFFFFFF, w5=0xFFFFFFFF,
 *                w6=0xFFFFFFFE, w7=0x00000000)
 */
static const u32 REDUCE_R[8][8] = {
    /* i=0: k=8,  2^256 mod p */
    { 0x00000001U, 0x00000000U, 0x00000000U, 0xFFFFFFFFU,
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFEU, 0x00000000U },
    /* i=1: k=9,  2^288 mod p */
    { 0x00000000U, 0x00000001U, 0x00000000U, 0x00000000U,
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFEU },
    /* i=2: k=10, 2^320 mod p */
    { 0xFFFFFFFFU, 0x00000000U, 0x00000001U, 0x00000001U,
      0xFFFFFFFFU, 0xFFFFFFFEU, 0x00000000U, 0xFFFFFFFEU },
    /* i=3: k=11, 2^352 mod p */
    { 0xFFFFFFFEU, 0xFFFFFFFFU, 0x00000000U, 0x00000003U,
      0x00000000U, 0xFFFFFFFFU, 0x00000000U, 0xFFFFFFFEU },
    /* i=4: k=12, 2^384 mod p */
    { 0xFFFFFFFEU, 0xFFFFFFFEU, 0xFFFFFFFFU, 0x00000002U,
      0x00000002U, 0x00000000U, 0x00000001U, 0xFFFFFFFEU },
    /* i=5: k=13, 2^416 mod p */
    { 0xFFFFFFFEU, 0xFFFFFFFEU, 0xFFFFFFFEU, 0x00000001U,
      0x00000002U, 0x00000002U, 0x00000002U, 0xFFFFFFFEU },
    /* i=6: k=14, 2^448 mod p */
    { 0xFFFFFFFFU, 0xFFFFFFFEU, 0xFFFFFFFEU, 0xFFFFFFFFU,
      0x00000000U, 0x00000002U, 0x00000003U, 0x00000000U },
    /* i=7: k=15, 2^480 mod p */
    { 0x00000000U, 0xFFFFFFFFU, 0xFFFFFFFEU, 0xFFFFFFFEU,
      0xFFFFFFFFU, 0x00000000U, 0x00000002U, 0x00000003U },
};

/* fe_reduce512: reduce c[0..15] (512-bit LE words) into *out mod p. */
static void fe_reduce512(fe *out, const u32 c[16])
{
    u64 acc[16];
    int i, j;

    for (i = 0; i < 16; i++) acc[i] = 0;
    for (i = 0; i < 8;  i++) acc[i] = c[i];

    /* Pass 1: fold c[8..15] into acc[0..15] using their 2^(32k) mod p values. */
    for (i = 0; i < 8; i++) {
        u64 ck = c[i + 8];
        if (!ck) continue;
        for (j = 0; j < 8; j++) {
            u64 prod = ck * (u64)REDUCE_R[i][j];
            acc[j]   += prod & 0xFFFFFFFFU;
            acc[j+1] += prod >> 32;
        }
    }

    /* Carry propagate the full 16-word accumulator. */
    for (i = 0; i < 15; i++) {
        acc[i+1] += acc[i] >> 32;
        acc[i]   &= 0xFFFFFFFFU;
    }

    /*
     * acc[8..15] now each hold at most 2^32-1 (a single 32-bit value).
     * Pass 2: fold acc[8..15] into acc[0..8] the same way.
     */
    for (i = 0; i < 8; i++) {
        u64 ck = acc[i + 8];
        acc[i + 8] = 0;
        if (!ck) continue;
        for (j = 0; j < 8; j++) {
            u64 prod = ck * (u64)REDUCE_R[i][j];
            acc[j]   += prod & 0xFFFFFFFFU;
            acc[j+1] += prod >> 32;
        }
    }

    /* Carry propagate. */
    for (i = 0; i < 8; i++) {
        acc[i+1] += acc[i] >> 32;
        acc[i]   &= 0xFFFFFFFFU;
    }

    /* acc[8] is now a small positive value (≤ a few units). Reduce it. */
    while (acc[8]) {
        u64 hi = acc[8];
        acc[8] = 0;
        for (j = 0; j < 8; j++) {
            u64 prod = hi * (u64)REDUCE_R[0][j];
            acc[j]   += prod & 0xFFFFFFFFU;
            acc[j+1] += prod >> 32;
        }
        for (i = 0; i < 8; i++) {
            acc[i+1] += acc[i] >> 32;
            acc[i]   &= 0xFFFFFFFFU;
        }
    }

    fe tmp;
    for (i = 0; i < 8; i++) tmp.w[i] = (u32)acc[i];

    /* Canonical reduction: at most two conditional subtractions. */
    fe_cond_sub_p(&tmp);
    fe_cond_sub_p(&tmp);
    fe_copy(out, &tmp);
}

/* fe_mul: out = a * b mod p */
static void fe_mul(fe *out, const fe *a, const fe *b)
{
    u32 c[16];
    p_memset(c, 0, sizeof(c));

    /* Schoolbook 8x8 multiplication using u64 accumulators. */
    for (int i = 0; i < 8; i++) {
        u64 carry = 0;
        for (int j = 0; j < 8; j++) {
            u64 prod = (u64)a->w[i] * (u64)b->w[j] + (u64)c[i+j] + carry;
            c[i+j]  = (u32)prod;
            carry   = prod >> 32;
        }
        c[i+8] += (u32)carry;
    }
    fe_reduce512(out, c);
}

/* fe_sqr: out = a^2 mod p (same as fe_mul but we could optimize; keep simple) */
static void fe_sqr(fe *out, const fe *a) { fe_mul(out, a, a); }

/* fe_neg: out = -a mod p */
static void fe_neg(fe *out, const fe *a)
{
    if (fe_is_zero(a)) { fe_zero(out); return; }
    /* p - a */
    u64 borrow = 0;
    for (int i = 0; i < 8; i++) {
        u64 d   = (u64)FIELD_P.w[i] - (u64)a->w[i] - borrow;
        out->w[i] = (u32)d;
        borrow  = (d >> 63) & 1;
    }
}

/* -------------------------------------------------------------------------
 * Modular inverse via Fermat: a^(p-2) mod p.
 *
 * p-2 = FFFFFFFF 00000001 00000000 00000000
 *       00000000 FFFFFFFF FFFFFFFF FFFFFFFD
 *
 * We use an addition chain for p-2 that avoids computing all 256 squarings
 * naively; instead we build up reusable bit-runs.
 *
 * Strategy (standard for P-256):
 *   Compute a^e for e = p-2 using the following chain:
 *     Let x = a
 *     x2   = x^2
 *     x3   = x2 * x
 *     x6   = x3^(2^3) -- square 3 times, multiply
 *     ...etc.
 *   We use a simple square-and-multiply over the bits of p-2 (256 squarings
 *   + ~128 multiplies in the worst case) because correctness is paramount
 *   and inversions are called only a few times per operation.
 * -------------------------------------------------------------------------*/

/* p-2 as eight 32-bit LE words */
static const u32 P_MINUS_2[8] = {
    0xFFFFFFFD, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
};

static void fe_inv(fe *out, const fe *a)
{
    fe r, base;
    fe_copy(&base, a);

    /* out = 1 */
    fe_zero(&r);
    r.w[0] = 1;

    for (int i = 255; i >= 0; i--) {
        fe_sqr(&r, &r);
        int word = i >> 5;
        int bit  = i & 31;
        if ((P_MINUS_2[word] >> bit) & 1u) {
            fe_mul(&r, &r, &base);
        }
    }
    fe_copy(out, &r);
}

/* fe_from_bytes: big-endian 32-byte array -> fe (does NOT reduce mod p). */
static void fe_from_bytes(fe *out, const unsigned char b[32])
{
    for (int i = 0; i < 8; i++) {
        int off = (7 - i) * 4;   /* big-endian: high word first */
        out->w[i] = ((u32)b[off]   << 24) |
                    ((u32)b[off+1] << 16) |
                    ((u32)b[off+2] <<  8) |
                    ((u32)b[off+3]);
    }
}

/* fe_to_bytes: fe -> big-endian 32-byte array. */
static void fe_to_bytes(unsigned char b[32], const fe *in)
{
    for (int i = 0; i < 8; i++) {
        int off = (7 - i) * 4;
        b[off]   = (unsigned char)(in->w[i] >> 24);
        b[off+1] = (unsigned char)(in->w[i] >> 16);
        b[off+2] = (unsigned char)(in->w[i] >>  8);
        b[off+3] = (unsigned char)(in->w[i]);
    }
}

/* =========================================================================
 * Modular arithmetic mod n (group order)
 * =========================================================================
 *
 * For ECDSA we need: inverse mod n, reduce mod n, and comparisons.
 * We re-use the same fe type but with ORDER_N as the modulus.
 */

/* fe_cmp_n: compare a against ORDER_N, returns -1/0/+1 */
static int fe_cmp_n(const fe *a)
{
    return fe_cmp(a, &ORDER_N);
}

/* fe_cond_sub_n: if a >= n, subtract n in place. */
static void fe_cond_sub_n(fe *a)
{
    if (fe_cmp_n(a) < 0) return;
    u64 borrow = 0;
    for (int i = 0; i < 8; i++) {
        u64 d   = (u64)a->w[i] - (u64)ORDER_N.w[i] - borrow;
        a->w[i] = (u32)d;
        borrow  = (d >> 63) & 1;
    }
}

/* fe_mul_n: out = a * b mod n  (schoolbook + shift-subtract reduction) */
static void fe_mul_n(fe *out, const fe *a, const fe *b)
{
    /* Full 512-bit product in u32 words, same pattern as fe_mul. */
    u32 acc[16];
    p_memset(acc, 0, sizeof(acc));

    for (int i = 0; i < 8; i++) {
        u64 carry = 0;
        for (int j = 0; j < 8; j++) {
            u64 prod = (u64)a->w[i] * (u64)b->w[j] + (u64)acc[i+j] + carry;
            acc[i+j] = (u32)prod;
            carry    = prod >> 32;
        }
        acc[i+8] += (u32)carry;
    }

    /* Reduce 512-bit acc mod n using shift-subtract.
     * n is 256 bits; acc is 512 bits.  Align n to match acc's MSB. */

    /* Bit length of acc */
    int abits = 0;
    for (int i = 15; i >= 0; i--) {
        if (acc[i]) {
            u32 v = acc[i]; int b = 0;
            while (v) { b++; v >>= 1; }
            abits = i * 32 + b;
            break;
        }
    }

    /* Build n as 16-word array. */
    u32 n16[16];
    p_memset(n16, 0, sizeof(n16));
    for (int i = 0; i < 8; i++) n16[i] = ORDER_N.w[i];

    int nbits = 256;
    int shift = abits - nbits;
    if (shift < 0) shift = 0;

    /* Shift n16 left by `shift` bits. */
    int ws = shift >> 5, bs = shift & 31;
    u32 ns[16];
    p_memset(ns, 0, sizeof(ns));
    if (ws < 16) {
        for (int i = 0; i + ws < 16; i++) ns[i + ws] = n16[i];
    }
    if (bs) {
        u32 carry = 0;
        for (int i = 0; i < 16; i++) {
            u32 nc = ns[i] >> (32 - bs);  /* bs in [1,31] here, no UB */
            ns[i]  = (ns[i] << bs) | carry;
            carry  = nc;
        }
    }

    for (int s = shift; s >= 0; s--) {
        /* compare acc vs ns */
        int ge = 0;
        for (int i = 15; i >= 0; i--) {
            if (acc[i] > ns[i]) { ge = 1; break; }
            if (acc[i] < ns[i]) { ge = 0; break; }
            if (i == 0) ge = 1;  /* equal */
        }
        if (ge) {
            u64 borrow = 0;
            for (int i = 0; i < 16; i++) {
                u64 d = (u64)acc[i] - (u64)ns[i] - borrow;
                acc[i] = (u32)d;
                borrow = (d >> 63) & 1;
            }
        }
        /* ns >>= 1 */
        u32 c2 = 0;
        for (int i = 15; i >= 0; i--) {
            u32 nc2 = ns[i] & 1u;
            ns[i]  = (ns[i] >> 1) | (c2 << 31);
            c2     = nc2;
        }
    }

    for (int i = 0; i < 8; i++) out->w[i] = acc[i];
}

/* n-2 for Fermat inverse mod n */
static const u32 N_MINUS_2[8] = {
    0xFC63254F, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
    0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF
};

/* fe_inv_n: out = a^-1 mod n */
static void fe_inv_n(fe *out, const fe *a)
{
    fe r, base, tmp;
    fe_copy(&base, a);
    fe_zero(&r); r.w[0] = 1;

    for (int i = 255; i >= 0; i--) {
        /* r = r^2 mod n */
        fe_mul_n(&tmp, &r, &r);
        fe_copy(&r, &tmp);

        int word = i >> 5;
        int bit  = i & 31;
        if ((N_MINUS_2[word] >> bit) & 1u) {
            fe_mul_n(&tmp, &r, &base);
            fe_copy(&r, &tmp);
        }
    }
    fe_copy(out, &r);
}

/* fe_reduce_n: reduce an arbitrary 32-byte big-endian value mod n. */
static void fe_reduce_mod_n(fe *out, const unsigned char *hash, unsigned long hlen)
{
    /* Take leftmost 32 bytes (256 bits); if hlen < 32, zero-pad on the left.
     * This is the "truncate to bit-length of n" step from FIPS 186-4 (n is
     * 256 bits so we simply take the leftmost 256 bits). */
    unsigned char buf[32];
    p_memset(buf, 0, 32);
    if (hlen >= 32) {
        p_memcpy(buf, hash, 32);
    } else {
        /* right-align: put hash at the end */
        p_memcpy(buf + (32 - hlen), hash, hlen);
    }
    fe_from_bytes(out, buf);
    /* Reduce mod n: at most one subtraction since value is < 2^256 */
    fe_cond_sub_n(out);
}

/* =========================================================================
 * Point validation: is (x,y) on P-256?
 *   y^2 mod p == (x^3 - 3x + b) mod p
 * ========================================================================= */
static int point_on_curve(const fe *x, const fe *y)
{
    fe lhs, rhs, tmp;

    /* lhs = y^2 mod p */
    fe_sqr(&lhs, y);

    /* rhs = x^3 - 3x + b */
    fe_sqr(&tmp, x);
    fe_mul(&rhs, &tmp, x);    /* x^3 */

    /* -3x: subtract x three times */
    fe_sub(&rhs, &rhs, x);
    fe_sub(&rhs, &rhs, x);
    fe_sub(&rhs, &rhs, x);

    fe_add(&rhs, &rhs, &CURVE_B);

    return fe_cmp(&lhs, &rhs) == 0;
}

/* =========================================================================
 * Jacobian point arithmetic
 * =========================================================================
 *
 * Formulae from:
 *   Hankerson, Menezes, Vanstone "Guide to Elliptic Curve Cryptography"
 *   and https://hyperelliptic.org/EFD/g1p/auto-shortw-jacobian-3.html
 */

/* pt_infinity: set P to the point at infinity (Z=0). */
static void pt_infinity(pt *P)
{
    fe_zero(&P->X); fe_zero(&P->Y); fe_zero(&P->Z);
}

/* pt_is_infinity: true if Z == 0. */
static int pt_is_infinity(const pt *P)
{
    return fe_is_zero(&P->Z);
}

/* pt_from_affine: set Jacobian point from affine (x,y) with Z=1. */
static void pt_from_affine(pt *P, const fe *x, const fe *y)
{
    fe_copy(&P->X, x);
    fe_copy(&P->Y, y);
    fe_zero(&P->Z); P->Z.w[0] = 1;
}

/* pt_to_affine: convert Jacobian P to affine (ax, ay).
 * P must NOT be the point at infinity (check beforehand). */
static void pt_to_affine(fe *ax, fe *ay, const pt *P)
{
    fe zinv, zinv2, zinv3;
    fe_inv(&zinv, &P->Z);           /* zinv = 1/Z */
    fe_sqr(&zinv2, &zinv);          /* zinv2 = 1/Z^2 */
    fe_mul(&zinv3, &zinv2, &zinv);  /* zinv3 = 1/Z^3 */
    fe_mul(ax, &P->X, &zinv2);      /* ax = X/Z^2 */
    fe_mul(ay, &P->Y, &zinv3);      /* ay = Y/Z^3 */
}

/*
 * pt_dbl: P = 2*P  (Jacobian doubling, a = -3 variant).
 *
 * Algorithm (from EFD shortw-jacobian-3):
 *   if P == infinity: return infinity
 *   delta = Z1^2
 *   gamma = Y1^2
 *   beta  = X1 * gamma
 *   alpha = 3*(X1 - delta)*(X1 + delta)    [uses a=-3]
 *   X3 = alpha^2 - 8*beta
 *   Z3 = (Y1 + Z1)^2 - gamma - delta
 *   Y3 = alpha*(4*beta - X3) - 8*gamma^2
 */
static void pt_dbl(pt *R, const pt *P)
{
    if (pt_is_infinity(P)) { pt_infinity(R); return; }

    fe delta, gamma, beta, alpha, tmp, tmp2;

    fe_sqr(&delta, &P->Z);              /* delta = Z1^2 */
    fe_sqr(&gamma, &P->Y);              /* gamma = Y1^2 */
    fe_mul(&beta, &P->X, &gamma);       /* beta = X1*gamma */

    /* alpha = 3*(X1 - delta)*(X1 + delta) */
    fe_sub(&tmp, &P->X, &delta);
    fe_add(&tmp2, &P->X, &delta);
    fe_mul(&alpha, &tmp, &tmp2);
    /* 3*alpha */
    fe_dbl(&tmp, &alpha);
    fe_add(&alpha, &tmp, &alpha);

    /* X3 = alpha^2 - 8*beta */
    fe_sqr(&R->X, &alpha);
    fe_dbl(&tmp, &beta);   /* 2*beta */
    fe_dbl(&tmp, &tmp);    /* 4*beta */
    fe_dbl(&tmp, &tmp);    /* 8*beta */
    fe_sub(&R->X, &R->X, &tmp);

    /* Z3 = (Y1 + Z1)^2 - gamma - delta */
    fe_add(&R->Z, &P->Y, &P->Z);
    fe_sqr(&R->Z, &R->Z);
    fe_sub(&R->Z, &R->Z, &gamma);
    fe_sub(&R->Z, &R->Z, &delta);

    /* Y3 = alpha*(4*beta - X3) - 8*gamma^2 */
    fe_dbl(&tmp, &beta);   /* 2*beta */
    fe_dbl(&tmp, &tmp);    /* 4*beta */
    fe_sub(&tmp, &tmp, &R->X);
    fe_mul(&R->Y, &alpha, &tmp);
    fe_sqr(&gamma, &gamma);   /* gamma^2 */
    fe_dbl(&gamma, &gamma);   /* 2*gamma^2 */
    fe_dbl(&gamma, &gamma);   /* 4*gamma^2 */
    fe_dbl(&gamma, &gamma);   /* 8*gamma^2 */
    fe_sub(&R->Y, &R->Y, &gamma);
}

/*
 * pt_add: R = P + Q  (Jacobian add, mixed when Q.Z==1 is NOT assumed here;
 * full general Jacobian-Jacobian addition).
 *
 * Algorithm (from EFD add-2007-bl):
 *   Z1Z1 = Z1^2
 *   Z2Z2 = Z2^2
 *   U1 = X1*Z2Z2
 *   U2 = X2*Z1Z1
 *   S1 = Y1*Z2*Z2Z2
 *   S2 = Y2*Z1*Z1Z1
 *   H = U2 - U1
 *   I = (2H)^2
 *   J = H*I
 *   r = 2*(S2 - S1)
 *   V = U1*I
 *   X3 = r^2 - J - 2V
 *   Y3 = r*(V - X3) - 2*S1*J
 *   Z3 = ((Z1+Z2)^2 - Z1Z1 - Z2Z2)*H
 *
 * If P == Q we fall through to pt_dbl (H would be 0 leading to infinity).
 * If one is infinity, return the other.
 */
static void pt_add(pt *R, const pt *P, const pt *Q)
{
    if (pt_is_infinity(P)) { *R = *Q; return; }
    if (pt_is_infinity(Q)) { *R = *P; return; }

    fe Z1Z1, Z2Z2, U1, U2, S1, S2, H, I, J, r_val, V, tmp;

    fe_sqr(&Z1Z1, &P->Z);
    fe_sqr(&Z2Z2, &Q->Z);
    fe_mul(&U1, &P->X, &Z2Z2);
    fe_mul(&U2, &Q->X, &Z1Z1);
    fe_mul(&tmp, &P->Y, &Q->Z);
    fe_mul(&S1, &tmp, &Z2Z2);
    fe_mul(&tmp, &Q->Y, &P->Z);
    fe_mul(&S2, &tmp, &Z1Z1);

    fe_sub(&H, &U2, &U1);

    /* H == 0 means P and Q have the same X coordinate */
    if (fe_is_zero(&H)) {
        fe_sub(&tmp, &S2, &S1);
        if (fe_is_zero(&tmp)) {
            /* P == Q: double */
            pt_dbl(R, P);
        } else {
            /* P == -Q: return infinity */
            pt_infinity(R);
        }
        return;
    }

    fe_dbl(&I, &H);
    fe_sqr(&I, &I);             /* I = (2H)^2 */
    fe_mul(&J, &H, &I);
    fe_sub(&r_val, &S2, &S1);
    fe_dbl(&r_val, &r_val);     /* r = 2*(S2-S1) */
    fe_mul(&V, &U1, &I);

    /* X3 = r^2 - J - 2*V */
    fe_sqr(&R->X, &r_val);
    fe_sub(&R->X, &R->X, &J);
    fe_dbl(&tmp, &V);
    fe_sub(&R->X, &R->X, &tmp);

    /* Y3 = r*(V - X3) - 2*S1*J */
    fe_sub(&tmp, &V, &R->X);
    fe_mul(&R->Y, &r_val, &tmp);
    fe_mul(&tmp, &S1, &J);
    fe_dbl(&tmp, &tmp);
    fe_sub(&R->Y, &R->Y, &tmp);

    /* Z3 = ((Z1+Z2)^2 - Z1Z1 - Z2Z2)*H */
    fe_add(&R->Z, &P->Z, &Q->Z);
    fe_sqr(&R->Z, &R->Z);
    fe_sub(&R->Z, &R->Z, &Z1Z1);
    fe_sub(&R->Z, &R->Z, &Z2Z2);
    fe_mul(&R->Z, &R->Z, &H);
}

/* =========================================================================
 * Scalar multiplication: R = k * P
 *
 * Left-to-right double-and-add over 256 bits of k.
 * k is given as a 32-byte big-endian array.
 * ========================================================================= */
static void pt_scalar_mul(pt *R, const pt *P, const unsigned char k[32])
{
    pt_infinity(R);
    pt tmp;

    for (int i = 0; i < 256; i++) {
        int byte_idx = i / 8;
        int bit_idx  = 7 - (i % 8);
        int bit      = (k[byte_idx] >> bit_idx) & 1;

        pt_dbl(&tmp, R);
        *R = tmp;
        if (bit) {
            pt_add(&tmp, R, P);
            *R = tmp;
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * p256_keygen: pub = priv * G
 */
int p256_keygen(unsigned char priv[32], unsigned char pub65[65])
{
    /* Check priv != 0 and priv < n */
    fe k;
    fe_from_bytes(&k, priv);
    if (fe_is_zero(&k)) return -1;
    if (fe_cmp(&k, &ORDER_N) >= 0) return -1;

    pt G, Q;
    pt_from_affine(&G, &BASE_GX, &BASE_GY);
    pt_scalar_mul(&Q, &G, priv);

    if (pt_is_infinity(&Q)) return -1;

    fe ax, ay;
    pt_to_affine(&ax, &ay, &Q);

    pub65[0] = 0x04;
    fe_to_bytes(pub65 + 1,  &ax);
    fe_to_bytes(pub65 + 33, &ay);
    return 0;
}

/*
 * p256_ecdh: shared_x = priv * peer_pub
 */
int p256_ecdh(unsigned char out_x[32],
              const unsigned char priv[32],
              const unsigned char peer_pub65[65])
{
    if (peer_pub65[0] != 0x04) return -1;

    fe px, py;
    fe_from_bytes(&px, peer_pub65 + 1);
    fe_from_bytes(&py, peer_pub65 + 33);

    /* Validate coordinates: must be < p */
    if (fe_cmp(&px, &FIELD_P) >= 0) return -1;
    if (fe_cmp(&py, &FIELD_P) >= 0) return -1;

    /* Validate: point is on the curve */
    if (!point_on_curve(&px, &py)) return -1;

    /* Validate private key */
    fe k;
    fe_from_bytes(&k, priv);
    if (fe_is_zero(&k)) return -1;
    if (fe_cmp(&k, &ORDER_N) >= 0) return -1;

    pt Q, R;
    pt_from_affine(&Q, &px, &py);
    pt_scalar_mul(&R, &Q, priv);

    if (pt_is_infinity(&R)) return -1;

    fe ax, ay;
    pt_to_affine(&ax, &ay, &R);
    fe_to_bytes(out_x, &ax);
    return 0;
}

/*
 * p256_ecdsa_verify: FIPS 186-4 verification.
 *
 *   1. Reject r, s not in [1, n-1].
 *   2. e = leftmost 256 bits of hash (reduced mod n).
 *   3. w = s^-1 mod n.
 *   4. u1 = e*w mod n, u2 = r*w mod n.
 *   5. R_pt = u1*G + u2*Q.
 *   6. Valid iff R_pt != infinity and R_pt.x mod n == r.
 */
int p256_ecdsa_verify(const unsigned char pub65[65],
                      const unsigned char *hash, unsigned long hlen,
                      const unsigned char r[32], const unsigned char s[32])
{
    if (pub65[0] != 0x04) return -1;

    /* Decode public key */
    fe Qx, Qy;
    fe_from_bytes(&Qx, pub65 + 1);
    fe_from_bytes(&Qy, pub65 + 33);
    if (fe_cmp(&Qx, &FIELD_P) >= 0) return -1;
    if (fe_cmp(&Qy, &FIELD_P) >= 0) return -1;
    if (!point_on_curve(&Qx, &Qy)) return -1;

    /* Decode r, s and validate in [1, n-1] */
    fe fr, fs;
    fe_from_bytes(&fr, r);
    fe_from_bytes(&fs, s);
    if (fe_is_zero(&fr) || fe_cmp_n(&fr) >= 0) return -1;
    if (fe_is_zero(&fs) || fe_cmp_n(&fs) >= 0) return -1;

    /* e = hash mod n */
    fe fe_e;
    fe_reduce_mod_n(&fe_e, hash, hlen);

    /* w = s^-1 mod n */
    fe w;
    fe_inv_n(&w, &fs);

    /* u1 = e*w mod n, u2 = r*w mod n */
    fe u1, u2;
    fe_mul_n(&u1, &fe_e, &w);
    fe_mul_n(&u2, &fr,   &w);

    /* Encode u1 and u2 as 32-byte scalars for scalar multiplication */
    unsigned char u1_bytes[32], u2_bytes[32];
    fe_to_bytes(u1_bytes, &u1);
    fe_to_bytes(u2_bytes, &u2);

    /* R_pt = u1*G + u2*Q */
    pt G, Q_pt, R1, R2, R_pt;
    pt_from_affine(&G,    &BASE_GX, &BASE_GY);
    pt_from_affine(&Q_pt, &Qx,      &Qy);

    pt_scalar_mul(&R1, &G,    u1_bytes);
    pt_scalar_mul(&R2, &Q_pt, u2_bytes);
    pt_add(&R_pt, &R1, &R2);

    if (pt_is_infinity(&R_pt)) return -1;

    /* Convert R_pt.x to affine, then reduce mod n, compare with r */
    fe ax, ay;
    pt_to_affine(&ax, &ay, &R_pt);

    /* ax mod n */
    fe ax_mod_n;
    fe_copy(&ax_mod_n, &ax);
    fe_cond_sub_n(&ax_mod_n);

    if (fe_cmp(&ax_mod_n, &fr) != 0) return -1;
    return 0;
}

/* =========================================================================
 * Extra field primitives needed by WPA3 SAE (dragonfly).
 * =========================================================================
 *
 * These are NEW functions; they do not alter any of the logic above.
 * fe_pow generalises the fe_inv exponentiation ladder to an arbitrary
 * exponent; fe_sqrt and fe_is_quadratic_residue build on it.
 */

/*
 * fe_pow: out = base ^ exp mod p, exp = eight 32-bit LE limbs (exp[0]=LSW).
 * Identical square-and-multiply structure to fe_inv (which is fe_pow with
 * exp = p-2), MSB-first over all 256 bits.
 */
static void fe_pow(fe *out, const fe *base, const u32 exp[8])
{
    fe r, b;
    fe_copy(&b, base);

    /* r = 1 */
    fe_zero(&r);
    r.w[0] = 1;

    for (int i = 255; i >= 0; i--) {
        fe_sqr(&r, &r);
        int word = i >> 5;
        int bit  = i & 31;
        if ((exp[word] >> bit) & 1u) {
            fe_mul(&r, &r, &b);
        }
    }
    fe_copy(out, &r);
}

/*
 * (p+1)/4 as eight 32-bit LE words.
 *   p   = FFFFFFFF 00000001 00000000 00000000 00000000 FFFFFFFF FFFFFFFF FFFFFFFF
 *   p+1 = FFFFFFFF 00000001 00000000 00000000 00000001 00000000 00000000 00000000
 *   (p+1)/4 (shift right 2):
 *       = 3FFFFFFF C0000000 40000000 00000000 00000000 40000000 00000000 00000000
 *   LE limbs (w0 = LSW):
 */
static const u32 P_PLUS_1_DIV_4[8] = {
    0x00000000, 0x00000000, 0x40000000, 0x00000000,
    0x00000000, 0x40000000, 0xC0000000, 0x3FFFFFFF
};

/*
 * (p-1)/2 as eight 32-bit LE words (Euler's criterion exponent).
 *   p-1 = FFFFFFFF 00000001 00000000 00000000 00000000 FFFFFFFF FFFFFFFF FFFFFFFE
 *   (p-1)/2 (shift right 1):
 *       = 7FFFFFFF 80000000 80000000 00000000 00000000 7FFFFFFF FFFFFFFF FFFFFFFF
 *   LE limbs (w0 = LSW):
 */
static const u32 P_MINUS_1_DIV_2[8] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF, 0x00000000,
    0x00000000, 0x80000000, 0x80000000, 0x7FFFFFFF
};

/*
 * fe_sqrt: out = sqrt(a) mod p.  Because p === 3 (mod 4), the principal
 * square root is a^((p+1)/4) mod p.  We then verify out^2 == a; returns 1 if
 * a is a QR (root valid), 0 otherwise.  a must be < p.
 */
static int fe_sqrt(fe *out, const fe *a)
{
    fe root, chk;
    fe_pow(&root, a, P_PLUS_1_DIV_4);
    fe_sqr(&chk, &root);
    /* both root^2 and a are canonical (< p) here, so a direct compare works */
    if (fe_cmp(&chk, a) != 0) {
        return 0;
    }
    fe_copy(out, &root);
    return 1;
}

/*
 * fe_is_quadratic_residue: returns 1 iff a is a non-zero quadratic residue
 * mod p, i.e. a^((p-1)/2) == 1 (Euler's criterion).  Returns 0 for a == 0
 * and for non-residues (where the Legendre symbol is p-1).
 */
static int fe_is_quadratic_residue(const fe *a)
{
    fe r, one;
    if (fe_is_zero(a)) return 0;
    fe_pow(&r, a, P_MINUS_1_DIV_2);
    fe_zero(&one); one.w[0] = 1;
    return fe_cmp(&r, &one) == 0;
}

/* =========================================================================
 * External-linkage wrappers for SAE (declared in p256_internal.h).
 * =========================================================================
 *
 * Each forwards to the corresponding static implementation above WITHOUT any
 * change of behaviour.  Promoting via thin wrappers (rather than dropping the
 * `static` keyword in place) keeps every internal call site bound to the
 * original symbol and leaves the proven ECDH/ECDSA code path untouched.
 */
void p256_fe_zero(p256_fe *out)                              { fe_zero(out); }
void p256_fe_copy(p256_fe *out, const p256_fe *a)            { fe_copy(out, a); }
int  p256_fe_is_zero(const p256_fe *a)                       { return fe_is_zero(a); }
int  p256_fe_cmp(const p256_fe *a, const p256_fe *b)         { return fe_cmp(a, b); }
void p256_fe_add(p256_fe *o, const p256_fe *a, const p256_fe *b) { fe_add(o, a, b); }
void p256_fe_sub(p256_fe *o, const p256_fe *a, const p256_fe *b) { fe_sub(o, a, b); }
void p256_fe_mul(p256_fe *o, const p256_fe *a, const p256_fe *b) { fe_mul(o, a, b); }
void p256_fe_sqr(p256_fe *o, const p256_fe *a)               { fe_sqr(o, a); }
void p256_fe_inv(p256_fe *o, const p256_fe *a)               { fe_inv(o, a); }
void p256_fe_neg(p256_fe *o, const p256_fe *a)               { fe_neg(o, a); }
void p256_fe_from_bytes(p256_fe *o, const unsigned char b[32]) { fe_from_bytes(o, b); }
void p256_fe_to_bytes(unsigned char b[32], const p256_fe *in)  { fe_to_bytes(b, in); }
void p256_fe_pow(p256_fe *o, const p256_fe *base, const p256_u32 exp[8])
                                                             { fe_pow(o, base, exp); }
int  p256_fe_sqrt(p256_fe *o, const p256_fe *a)              { return fe_sqrt(o, a); }
int  p256_fe_is_quadratic_residue(const p256_fe *a)          { return fe_is_quadratic_residue(a); }

int  p256_fe_cmp_n(const p256_fe *a)                         { return fe_cmp_n(a); }
void p256_fe_cond_sub_n(p256_fe *a)                          { fe_cond_sub_n(a); }
void p256_fe_mul_n(p256_fe *o, const p256_fe *a, const p256_fe *b) { fe_mul_n(o, a, b); }
void p256_fe_inv_n(p256_fe *o, const p256_fe *a)             { fe_inv_n(o, a); }

int  p256_point_on_curve(const p256_fe *x, const p256_fe *y) { return point_on_curve(x, y); }
void p256_pt_infinity(p256_pt *P)                            { pt_infinity(P); }
int  p256_pt_is_infinity(const p256_pt *P)                   { return pt_is_infinity(P); }
void p256_pt_from_affine(p256_pt *P, const p256_fe *x, const p256_fe *y)
                                                             { pt_from_affine(P, x, y); }
void p256_pt_to_affine(p256_fe *ax, p256_fe *ay, const p256_pt *P)
                                                             { pt_to_affine(ax, ay, P); }
void p256_pt_add(p256_pt *R, const p256_pt *P, const p256_pt *Q) { pt_add(R, P, Q); }
void p256_pt_dbl(p256_pt *R, const p256_pt *P)               { pt_dbl(R, P); }
void p256_pt_scalar_mul(p256_pt *R, const p256_pt *P, const unsigned char k[32])
                                                             { pt_scalar_mul(R, P, k); }

/* =========================================================================
 * Self-test
 *
 * Test vector 1 + 2: RFC 6979 Appendix A.2.5 -- ECDSA, P-256, SHA-256,
 *   message "sample".
 *
 *   Source: RFC 6979 (Deterministic Usage of DSA and ECDSA), Appendix A.2.5.
 *   The hash field is SHA-256("sample") -- we embed the pre-computed digest
 *   directly so no SHA-256 dependency is needed.
 *
 *   Private key d:
 *     C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
 *   Public key Q:
 *     Qx = 60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6
 *     Qy = 7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299
 *   SHA-256("sample"):
 *     AF2BDBE1AA9B6EC1E2ADE1D694F41FC71A831D0268E9891562113D8A62ADD1BF
 *   Signature (k chosen deterministically per RFC 6979):
 *     r = EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716
 *     s = F7CB1C942D657EF04FC2FEAD0BD9C29B09CB7EECB8E9DD7B12FC9F3CC1CA8CE
 *
 * Test vector 3: Tampered s (one bit flipped) -- must FAIL.
 *
 * Test vector 4: Scalar-mult -- p256_keygen with priv=2 must yield 2*G.
 *   2*G for P-256 (from OpenSSL, BouncyCastle, https://neuromancer.sk/std/nist/P-256):
 *     x = 7CF27B188D034F7E8A52380304B51AC3C74355B0A6B3B601C6B23BEDBC79A31A
 *     y = 07775510DB8ED040293D9AC69F7430DBBA7DADE63CE982299E04B79D227873D1
 *
 * Test vector 5: ECDH -- priv=2, peer=G => shared.x == (2*G).x.
 *
 * ========================================================================= */

int p256_selftest(void)
{
    /* ------------------------------------------------------------------ *
     * Vectors 1+2: RFC 6979 A.2.5, P-256/SHA-256, message "sample"       *
     *   hash = SHA-256("sample") embedded as raw 32 bytes                 *
     * ------------------------------------------------------------------ */

    /* Public key (uncompressed) */
    static const unsigned char tv_pub65[65] = {
        0x04,
        /* Qx */
        0x60, 0xfe, 0xd4, 0xba, 0x25, 0x5a, 0x9d, 0x31,
        0xc9, 0x61, 0xeb, 0x74, 0xc6, 0x35, 0x6d, 0x68,
        0xc0, 0x49, 0xb8, 0x92, 0x3b, 0x61, 0xfa, 0x6c,
        0xe6, 0x69, 0x62, 0x2e, 0x60, 0xf2, 0x9f, 0xb6,
        /* Qy */
        0x79, 0x03, 0xfe, 0x10, 0x08, 0xb8, 0xbc, 0x99,
        0xa4, 0x1a, 0xe9, 0xe9, 0x56, 0x28, 0xbc, 0x64,
        0xf2, 0xf1, 0xb2, 0x0c, 0x2d, 0x7e, 0x9f, 0x51,
        0x77, 0xa3, 0xc2, 0x94, 0xd4, 0x46, 0x22, 0x99
    };

    /* SHA-256("sample") -- pre-computed; passed directly as the hash input */
    static const unsigned char tv_hash[32] = {
        0xaf, 0x2b, 0xdb, 0xe1, 0xaa, 0x9b, 0x6e, 0xc1,
        0xe2, 0xad, 0xe1, 0xd6, 0x94, 0xf4, 0x1f, 0xc7,
        0x1a, 0x83, 0x1d, 0x02, 0x68, 0xe9, 0x89, 0x15,
        0x62, 0x11, 0x3d, 0x8a, 0x62, 0xad, 0xd1, 0xbf
    };

    /* Signature components r and s (RFC 6979 deterministic, k not needed) */
    static const unsigned char tv_r[32] = {
        0xef, 0xd4, 0x8b, 0x2a, 0xac, 0xb6, 0xa8, 0xfd,
        0x11, 0x40, 0xdd, 0x9c, 0xd4, 0x5e, 0x81, 0xd6,
        0x9d, 0x2c, 0x87, 0x7b, 0x56, 0xaa, 0xf9, 0x91,
        0xc3, 0x4d, 0x0e, 0xa8, 0x4e, 0xaf, 0x37, 0x16
    };
    /* s = F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8
     * (RFC 6979 A.2.5, P-256/SHA-256, "sample") */
    static const unsigned char tv_s[32] = {
        0xf7, 0xcb, 0x1c, 0x94, 0x2d, 0x65, 0x7c, 0x41,
        0xd4, 0x36, 0xc7, 0xa1, 0xb6, 0xe2, 0x9f, 0x65,
        0xf3, 0xe9, 0x00, 0xdb, 0xb9, 0xaf, 0xf4, 0x06,
        0x4d, 0xc4, 0xab, 0x2f, 0x84, 0x3a, 0xcd, 0xa8
    };

    /* Vector 1: valid signature must verify */
    if (p256_ecdsa_verify(tv_pub65, tv_hash, 32, tv_r, tv_s) != 0)
        return -1;

    /* Vector 2: tampered s (flip last byte) must FAIL */
    {
        unsigned char tv_s_bad[32];
        p_memcpy(tv_s_bad, tv_s, 32);
        tv_s_bad[31] ^= 0x01;
        if (p256_ecdsa_verify(tv_pub65, tv_hash, 32, tv_r, tv_s_bad) == 0)
            return -1;
    }

    /* ------------------------------------------------------------------ *
     * Vector 3: tampered r must also FAIL                                 *
     * ------------------------------------------------------------------ */
    {
        unsigned char tv_r_bad[32];
        p_memcpy(tv_r_bad, tv_r, 32);
        tv_r_bad[0] ^= 0x80;
        if (p256_ecdsa_verify(tv_pub65, tv_hash, 32, tv_r_bad, tv_s) == 0)
            return -1;
    }

    /* ------------------------------------------------------------------ *
     * Vector 4: p256_keygen with priv=2 must yield the known 2*G point.  *
     *   2*G for P-256 (OpenSSL, BouncyCastle, neuromancer.sk):            *
     *     x = 7CF27B188D034F7E8A52380304B51AC3C74355B0A6B3B601C6B23BEDBC79A31A
     *     y = 07775510DB8ED040293D9AC69F7430DBBA7DADE63CE982299E04B79D227873D1
     * ------------------------------------------------------------------ */
    /* 2*G for P-256 (computed from the curve, verified against FIPS/OpenSSL):
     *   x = 7CF27B188D034F7E8A52380304B51AC3C08969E277F21B35A60B48FC47669978
     *   y = 07775510DB8ED040293D9AC69F7430DBBA7DADE63CE982299E04B79D227873D1 */
    static const unsigned char expected_2G[65] = {
        0x04,
        /* x */
        0x7c, 0xf2, 0x7b, 0x18, 0x8d, 0x03, 0x4f, 0x7e,
        0x8a, 0x52, 0x38, 0x03, 0x04, 0xb5, 0x1a, 0xc3,
        0xc0, 0x89, 0x69, 0xe2, 0x77, 0xf2, 0x1b, 0x35,
        0xa6, 0x0b, 0x48, 0xfc, 0x47, 0x66, 0x99, 0x78,
        /* y */
        0x07, 0x77, 0x55, 0x10, 0xdb, 0x8e, 0xd0, 0x40,
        0x29, 0x3d, 0x9a, 0xc6, 0x9f, 0x74, 0x30, 0xdb,
        0xba, 0x7d, 0xad, 0xe6, 0x3c, 0xe9, 0x82, 0x29,
        0x9e, 0x04, 0xb7, 0x9d, 0x22, 0x78, 0x73, 0xd1
    };

    unsigned char priv2_buf[32];
    unsigned char pub65[65];
    p_memset(priv2_buf, 0, 32);
    priv2_buf[31] = 2;

    if (p256_keygen(priv2_buf, pub65) != 0) return -1;
    if (p_memcmp(pub65, expected_2G, 65) != 0) return -1;

    /* ------------------------------------------------------------------ *
     * Vector 5: ECDH -- priv=2, peer=G => shared.x == (2*G).x            *
     * ------------------------------------------------------------------ */
    static const unsigned char G_pub65[65] = {
        0x04,
        /* Gx */
        0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47,
        0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
        0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
        0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
        /* Gy */
        0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b,
        0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
        0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
        0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5
    };

    unsigned char shared_x[32];
    if (p256_ecdh(shared_x, priv2_buf, G_pub65) != 0) return -1;
    if (p_memcmp(shared_x, expected_2G + 1, 32) != 0) return -1;

    return 0;  /* all tests passed */
}
