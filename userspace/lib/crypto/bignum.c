/*
 * bignum.c -- freestanding fixed-capacity unsigned big integers.
 * =============================================================
 *
 * See bignum.h for the model. Implementation notes:
 *
 *   * Words are 32-bit; products and carries are accumulated in 64-bit
 *     (unsigned long long) so each schoolbook step is exact. We assume a
 *     64-bit toolchain (this is an x86_64 OS), so unsigned long long is the
 *     natural double-width type. No __int128 is required.
 *
 *   * Internally we work with FIXED-SIZE word arrays of length BN_TMP_WORDS,
 *     which is wide enough to hold a full schoolbook product of two BN_WORDS
 *     numbers (2*BN_WORDS) plus a little slack for normalisation. Nothing is
 *     heap-allocated; every scratch buffer lives on the stack.
 *
 *   * Modular exponentiation uses Montgomery multiplication. For an odd
 *     modulus m of k words and R = 2^(32k):
 *         - mont(a,b) = a*b*R^-1 mod m   (CIOS Montgomery REDC, fused)
 *         - to enter Montgomery form:  a~ = a*R mod m = mont(a, R^2 mod m)
 *         - to leave:                  a  = mont(a~, 1)
 *     R^2 mod m is computed once via a straightforward (slow but correct)
 *     shift/subtract reduction; the hot inner loop is pure Montgomery.
 *
 *   * Constant-time-ness is NOT a goal here. This backs a TLS client doing
 *     RSA *public-key* operations (encrypt premaster, verify signatures)
 *     where the secret material is the server's, not ours, and the inputs we
 *     process are public ciphertext/signature values. Correctness and being
 *     fast enough for 2048-bit are the goals.
 */

#include "bignum.h"

/*
 * Wide scratch width. A schoolbook product of two BN_WORDS-word operands is
 * 2*BN_WORDS words. We add a couple words of headroom so reduction loops and
 * the Montgomery accumulator (which is one word wider than the modulus) never
 * index out of range.
 */
#define BN_TMP_WORDS (2 * BN_WORDS + 2)

typedef unsigned int       u32;
typedef unsigned long long u64;

/* --------------------------------------------------------------------- *
 *  Private memory primitives (no libc).                                   *
 * --------------------------------------------------------------------- */

static void bn_memset(void *dst, int val, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)val;
    while (n--) *d++ = v;
}

static void bn_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

/* --------------------------------------------------------------------- *
 *  Normalisation helpers.                                                 *
 * --------------------------------------------------------------------- */

/* Recompute x->n by trimming leading zero words. */
static void bn_norm(bignum *x)
{
    int i = BN_WORDS - 1;
    /* Defensive: the value lives in w[0..BN_WORDS); n must not exceed it. */
    while (i >= 0 && x->w[i] == 0) i--;
    x->n = i + 1;
}

void bn_set_zero(bignum *x)
{
    bn_memset(x->w, 0, sizeof(x->w));
    x->n = 0;
}

void bn_copy(bignum *dst, const bignum *src)
{
    bn_memcpy(dst->w, src->w, sizeof(dst->w));
    dst->n = src->n;
}

void bn_set_u32(bignum *x, unsigned int v)
{
    bn_set_zero(x);
    if (v) {
        x->w[0] = v;
        x->n    = 1;
    }
}

int bn_is_zero(const bignum *x)
{
    return x->n == 0;
}

/* --------------------------------------------------------------------- *
 *  Serialisation.                                                        *
 * --------------------------------------------------------------------- */

void bn_from_bytes(bignum *x, const unsigned char *be, unsigned long len)
{
    bn_set_zero(x);
    /* Big-endian: the last byte is the least significant. Walk from the end
     * placing bytes into words, 4 bytes per word. */
    unsigned long bi  = 0;             /* byte index from the LSB end */
    for (long i = (long)len - 1; i >= 0; i--, bi++) {
        unsigned long word = bi >> 2;        /* /4 */
        unsigned      sh   = (unsigned)((bi & 3) * 8);
        if (word >= BN_WORDS) break;         /* input wider than capacity */
        x->w[word] |= ((u32)be[i]) << sh;
    }
    bn_norm(x);
}

void bn_to_bytes(const bignum *x, unsigned char *be, unsigned long len)
{
    /* Emit big-endian, left-padded to exactly len bytes. */
    for (unsigned long i = 0; i < len; i++) {
        unsigned long bi   = len - 1 - i;    /* byte distance from LSB */
        unsigned long word = bi >> 2;
        unsigned      sh   = (unsigned)((bi & 3) * 8);
        unsigned char b    = 0;
        if (word < BN_WORDS)
            b = (unsigned char)((x->w[word] >> sh) & 0xFFu);
        be[i] = b;
    }
}

/* --------------------------------------------------------------------- *
 *  Comparison / bit inspection.                                          *
 * --------------------------------------------------------------------- */

int bn_cmp(const bignum *a, const bignum *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    }
    return 0;
}

int bn_bit_length(const bignum *x)
{
    if (x->n == 0) return 0;
    u32 top = x->w[x->n - 1];
    int bits = (x->n - 1) * 32;
    while (top) { bits++; top >>= 1; }
    return bits;
}

/* Return bit i of x (0 if out of range). */
static int bn_get_bit(const bignum *x, int i)
{
    int word = i >> 5;
    int b    = i & 31;
    if (word >= x->n) return 0;
    return (int)((x->w[word] >> b) & 1u);
}

/* --------------------------------------------------------------------- *
 *  Low-level fixed-width word-array arithmetic.                          *
 *                                                                        *
 *  These operate on raw u32[] arrays of a caller-known length so the     *
 *  Montgomery code can use accumulators one word wider than a bignum.    *
 * --------------------------------------------------------------------- */

/* r[0..len) = a[0..len) (raw copy). */
static void w_copy(u32 *r, const u32 *a, int len)
{
    for (int i = 0; i < len; i++) r[i] = a[i];
}

/* r[0..len) = 0. */
static void w_zero(u32 *r, int len)
{
    for (int i = 0; i < len; i++) r[i] = 0;
}

/* Unsigned compare of a[0..len) vs b[0..len): -1/0/+1. */
static int w_cmp(const u32 *a, const u32 *b, int len)
{
    for (int i = len - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* r = a - b over len words, assuming a >= b. Returns final borrow (0). */
static u32 w_sub(u32 *r, const u32 *a, const u32 *b, int len)
{
    u64 borrow = 0;
    for (int i = 0; i < len; i++) {
        u64 t = (u64)a[i] - (u64)b[i] - borrow;
        r[i]  = (u32)t;
        borrow = (t >> 63) & 1; /* set iff subtraction underflowed */
    }
    return (u32)borrow;
}

/* --------------------------------------------------------------------- *
 *  Shift-and-subtract modular reduction.                                 *
 *                                                                        *
 *  Computes acc mod m for an arbitrary acc (held in a wide buffer). This *
 *  is O(bits) and only used off the hot path: to compute R^2 mod m and to *
 *  reduce the initial base. The Montgomery inner loop does NOT use it.    *
 * --------------------------------------------------------------------- */

/* acc[0..len) %= mod (a bignum). len is the working width in words; it must be
 * large enough to hold acc and >= mod->n. Result left in acc[0..len), with
 * value < mod. */
static void w_mod_bignum(u32 *acc, int len, const bignum *mod)
{
    /* Build a left-aligned copy of the modulus and reduce bit by bit from the
     * top. Classic restoring long division producing only the remainder. */
    int mbits = bn_bit_length(mod);
    if (mbits == 0) return;                 /* modulus 0: undefined, bail */

    /* Find the top set bit of acc. */
    int abits = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (acc[i]) {
            u32 v = acc[i];
            int b = 0;
            while (v) { b++; v >>= 1; }
            abits = i * 32 + b;
            break;
        }
    }
    if (abits < mbits) return;              /* already reduced */

    /* mshift = the modulus shifted up so its MSB lines up with acc's MSB. */
    u32 mshift[BN_TMP_WORDS];
    w_zero(mshift, len);
    for (int i = 0; i < mod->n; i++) mshift[i] = mod->w[i];

    int shift = abits - mbits;
    /* Shift mshift left by `shift` bits (word part then bit part). */
    int ws = shift >> 5;
    int bs = shift & 31;
    if (ws) {
        for (int i = len - 1; i >= 0; i--)
            mshift[i] = (i - ws >= 0) ? mshift[i - ws] : 0;
    }
    if (bs) {
        u32 carry = 0;
        for (int i = 0; i < len; i++) {
            u32 nc   = mshift[i] >> (32 - bs);
            mshift[i] = (mshift[i] << bs) | carry;
            carry    = nc;
        }
    }

    /* For each shifted position from `shift` down to 0: if acc >= mshift,
     * subtract; then shift mshift right by one bit. */
    for (int s = shift; s >= 0; s--) {
        if (w_cmp(acc, mshift, len) >= 0)
            w_sub(acc, acc, mshift, len);
        /* mshift >>= 1 */
        u32 carry = 0;
        for (int i = len - 1; i >= 0; i--) {
            u32 nc    = mshift[i] & 1u;
            mshift[i] = (mshift[i] >> 1) | (carry << 31);
            carry     = nc;
        }
    }
}

/* --------------------------------------------------------------------- *
 *  Generic modular arithmetic (any modulus).                             *
 *                                                                        *
 *  Correctness-first (schoolbook multiply + shift/subtract reduction),   *
 *  NOT the hot RSA path. Used by the elliptic-curve code (p384.c) for     *
 *  field/scalar arithmetic over a prime. Results are fully reduced into   *
 *  [0, m). bn_mod() accepts any input; bn_mod_add/sub/mul assume their    *
 *  operands are already reduced (< m), which the EC code maintains.       *
 * --------------------------------------------------------------------- */

/* Significant word count of a raw word array (index of top nonzero + 1). */
static int w_siglen(const u32 *a, int cap)
{
    int i = cap - 1;
    while (i >= 0 && a[i] == 0) i--;
    return i + 1;
}

/* Reduce acc (cap words) mod m using a tight working width, then store the
 * low BN_WORDS words into r. The working width passed to w_mod_bignum is
 * max(significant-words-of-acc, m->n) so reduction cost tracks the operand
 * size (a P-384 product is ~24 words, not BN_TMP_WORDS). */
static void w_reduce_into(bignum *r, u32 *acc, int cap, const bignum *m)
{
    int len = w_siglen(acc, cap);
    if (len < m->n) len = m->n;
    if (len < 1)    len = 1;
    w_mod_bignum(acc, len, m);
    bn_set_zero(r);
    for (int i = 0; i < BN_WORDS && i < cap; i++) r->w[i] = acc[i];
    bn_norm(r);
}

/* r = a mod m. */
void bn_mod(bignum *r, const bignum *a, const bignum *m)
{
    u32 acc[BN_TMP_WORDS];
    w_zero(acc, BN_TMP_WORDS);
    for (int i = 0; i < a->n && i < BN_WORDS; i++) acc[i] = a->w[i];
    w_reduce_into(r, acc, BN_TMP_WORDS, m);
}

/* r = (a + b) mod m  (a, b assumed < m). */
void bn_mod_add(bignum *r, const bignum *a, const bignum *b, const bignum *m)
{
    u32 acc[BN_TMP_WORDS];
    w_zero(acc, BN_TMP_WORDS);
    u64 carry = 0;
    for (int i = 0; i < BN_WORDS; i++) {
        u64 t = (u64)a->w[i] + (u64)b->w[i] + carry;
        acc[i] = (u32)t;
        carry = t >> 32;
    }
    acc[BN_WORDS] = (u32)carry;
    w_reduce_into(r, acc, BN_TMP_WORDS, m);
}

/* r = (a - b) mod m  (a, b assumed < m). Computes a + m - b (never underflows
 * since b < m <= a + m), then reduces. */
void bn_mod_sub(bignum *r, const bignum *a, const bignum *b, const bignum *m)
{
    u32 acc[BN_TMP_WORDS];
    u32 bb[BN_TMP_WORDS];
    w_zero(acc, BN_TMP_WORDS);
    w_zero(bb, BN_TMP_WORDS);
    u64 carry = 0;
    for (int i = 0; i < BN_WORDS; i++) {
        u64 t = (u64)a->w[i] + (u64)((i < m->n) ? m->w[i] : 0u) + carry;
        acc[i] = (u32)t;
        carry = t >> 32;
    }
    acc[BN_WORDS] = (u32)carry;
    for (int i = 0; i < BN_WORDS; i++) bb[i] = b->w[i];
    w_sub(acc, acc, bb, BN_TMP_WORDS);   /* acc >= a+m > b, no borrow */
    w_reduce_into(r, acc, BN_TMP_WORDS, m);
}

/* r = (a * b) mod m. */
void bn_mod_mul(bignum *r, const bignum *a, const bignum *b, const bignum *m)
{
    u32 prod[BN_TMP_WORDS];
    w_zero(prod, BN_TMP_WORDS);
    for (int i = 0; i < a->n; i++) {
        u64 carry = 0;
        u64 ai = a->w[i];
        for (int j = 0; j < b->n; j++) {
            u64 t = (u64)prod[i + j] + ai * (u64)b->w[j] + carry;
            prod[i + j] = (u32)t;
            carry = t >> 32;
        }
        int k = i + b->n;                 /* propagate the final carry upward */
        while (carry && k < BN_TMP_WORDS) {
            u64 t = (u64)prod[k] + carry;
            prod[k] = (u32)t;
            carry = t >> 32;
            k++;
        }
    }
    w_reduce_into(r, prod, BN_TMP_WORDS, m);
}

/* --------------------------------------------------------------------- *
 *  Montgomery arithmetic.                                                 *
 * --------------------------------------------------------------------- */

/*
 * Montgomery context: the odd modulus m (k words), and n0inv = -m^-1 mod 2^32.
 * R = 2^(32k). rr = R^2 mod m. one_mont = R mod m (Montgomery form of 1).
 */
typedef struct {
    int    k;                  /* number of words in the modulus */
    u32    m[BN_WORDS];        /* the modulus, k significant words */
    u32    n0inv;             /* -m[0]^-1 mod 2^32 */
    u32    rr[BN_TMP_WORDS];   /* R^2 mod m, k words */
} mont_ctx;

/* Compute -inv of a (a odd) modulo 2^32 via Newton iteration on 2-adics. */
static u32 mont_n0inv(u32 a)
{
    /* x = a^-1 mod 2^32 (Hensel lifting; 5 doublings: 2^2,2^4,...,2^32). */
    u32 x = 1;
    for (int i = 0; i < 5; i++)
        x = x * (2u - a * x);     /* doubles the number of correct bits */
    /* We want -a^-1 mod 2^32. */
    return (u32)(0u - x);
}

/* Initialise a Montgomery context from a modulus bignum (must be odd). */
static void mont_init(mont_ctx *ctx, const bignum *mod)
{
    ctx->k = mod->n;
    if (ctx->k < 1) ctx->k = 1;
    for (int i = 0; i < ctx->k; i++) ctx->m[i] = mod->w[i];

    ctx->n0inv = mont_n0inv(ctx->m[0]);

    /* rr = R^2 mod m, with R = 2^(32k).  R^2 = 2^(64k).
     * Build the value 2^(64k) in a wide buffer, then reduce mod m. */
    int width = 2 * ctx->k + 2;          /* words needed for 2^(64k) */
    u32 t[BN_TMP_WORDS];
    w_zero(t, width);
    /* Set bit (64*k): word index = 2k, bit 0. */
    t[2 * ctx->k] = 1u;
    w_mod_bignum(t, width, mod);
    /* Result is < m, so fits in k words; copy out (zero the rest). */
    for (int i = 0; i < ctx->k; i++) ctx->rr[i] = t[i];
}

/*
 * CIOS Montgomery multiplication: r = a * b * R^-1 mod m, all k words.
 * a, b, r are k-word arrays with value < m. Implements the Coarsely
 * Integrated Operand Scanning form with a (k+2)-word accumulator.
 */
static void mont_mul(mont_ctx *ctx, u32 *r, const u32 *a, const u32 *b)
{
    int k = ctx->k;
    u32 t[BN_WORDS + 2];
    w_zero(t, k + 2);

    for (int i = 0; i < k; i++) {
        /* t += a[i] * b */
        u64 carry = 0;
        u32 ai = a[i];
        for (int j = 0; j < k; j++) {
            u64 sum = (u64)t[j] + (u64)ai * (u64)b[j] + carry;
            t[j]    = (u32)sum;
            carry   = sum >> 32;
        }
        u64 s = (u64)t[k] + carry;
        t[k]  = (u32)s;
        t[k + 1] += (u32)(s >> 32);

        /* m_mul = (t[0] * n0inv) mod 2^32; then t += m_mul * m; t >>= word. */
        u32 mword = (u32)((u64)t[0] * (u64)ctx->n0inv);
        carry = 0;
        {
            u64 sum = (u64)t[0] + (u64)mword * (u64)ctx->m[0];
            carry   = sum >> 32;           /* low word becomes 0, discarded */
        }
        for (int j = 1; j < k; j++) {
            u64 sum = (u64)t[j] + (u64)mword * (u64)ctx->m[j] + carry;
            t[j - 1] = (u32)sum;           /* shift down by one word */
            carry    = sum >> 32;
        }
        u64 s2 = (u64)t[k] + carry;
        t[k - 1] = (u32)s2;
        t[k]     = t[k + 1] + (u32)(s2 >> 32);
        t[k + 1] = 0;
    }

    /* Final conditional subtraction: if t >= m, t -= m. t is k+1 words but
     * the high word t[k] is at most 1. */
    if (t[k] != 0 || w_cmp(t, ctx->m, k) >= 0) {
        /* subtract m from the low k words, propagating into t[k]. */
        u64 borrow = 0;
        for (int j = 0; j < k; j++) {
            u64 d = (u64)t[j] - (u64)ctx->m[j] - borrow;
            t[j]  = (u32)d;
            borrow = (d >> 63) & 1;
        }
        /* t[k] -= borrow (now zero). */
    }
    w_copy(r, t, k);
}

/* --------------------------------------------------------------------- *
 *  Modular exponentiation (the headline routine).                        *
 * --------------------------------------------------------------------- */

void bn_mod_exp(bignum *out, const bignum *base, const bignum *exp,
                const bignum *mod)
{
    bn_set_zero(out);

    /* Degenerate moduli. mod == 0 is undefined; mod == 1 => result 0. */
    if (bn_is_zero(mod)) return;
    {
        bignum one;
        bn_set_u32(&one, 1);
        if (bn_cmp(mod, &one) == 0) { bn_set_zero(out); return; }
    }

    /* exp == 0 => result is 1 mod mod (== 1 since mod > 1 here). */
    if (bn_is_zero(exp)) { bn_set_u32(out, 1); return; }

    mont_ctx ctx;
    mont_init(&ctx, mod);
    int k = ctx.k;

    /* Reduce base mod m into a k-word array, then convert to Montgomery form:
     * base_m = base * R mod m = mont(base mod m, rr). */
    u32 base_red[BN_TMP_WORDS];
    w_zero(base_red, 2 * BN_WORDS + 2);
    /* Lay out base value (full width) and reduce mod m. */
    for (int i = 0; i < base->n && i < BN_WORDS; i++) base_red[i] = base->w[i];
    w_mod_bignum(base_red, 2 * BN_WORDS + 2, mod);   /* now < m, fits in k */

    u32 base_m[BN_WORDS];
    mont_mul(&ctx, base_m, base_red, ctx.rr);        /* enter Montgomery */

    /* acc = Montgomery form of 1 = R mod m = mont(1, rr).  Build "1" in k
     * words. */
    u32 oneword[BN_WORDS];
    w_zero(oneword, k);
    oneword[0] = 1u;
    u32 acc[BN_WORDS];
    mont_mul(&ctx, acc, oneword, ctx.rr);            /* acc = R mod m */

    /* Left-to-right square-and-multiply over the exponent bits. */
    int top = bn_bit_length(exp) - 1;
    u32 tmp[BN_WORDS];
    for (int i = top; i >= 0; i--) {
        mont_mul(&ctx, tmp, acc, acc);               /* acc = acc^2 */
        w_copy(acc, tmp, k);
        if (bn_get_bit(exp, i)) {
            mont_mul(&ctx, tmp, acc, base_m);        /* acc *= base */
            w_copy(acc, tmp, k);
        }
    }

    /* Leave Montgomery form: result = mont(acc, 1). */
    u32 res[BN_WORDS];
    mont_mul(&ctx, res, acc, oneword);

    /* Pack into out. */
    bn_set_zero(out);
    for (int i = 0; i < k && i < BN_WORDS; i++) out->w[i] = res[i];
    bn_norm(out);
}
