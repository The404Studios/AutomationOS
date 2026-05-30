/*
 * x25519.c -- X25519 (Curve25519) ECDH, RFC 7748.
 * =================================================
 *
 * Freestanding pure computation: no libc, no syscalls, no malloc, no standard
 * headers. Uses only fixed stack buffers and the types defined below.
 *
 * FIELD REPRESENTATION
 * --------------------
 * GF(2^255-19).  Each field element is stored as 5 limbs of 51 bits in
 * uint64_t arrays:
 *
 *   fe[0] holds bits   0..50
 *   fe[1] holds bits  51..101
 *   fe[2] holds bits 102..152
 *   fe[3] holds bits 153..203
 *   fe[4] holds bits 204..254  (the 255th bit is always 0 after reduction)
 *
 * A "loosely reduced" intermediate may have limbs up to ~54 bits; a fully
 * reduced element has all limbs in [0, 2^51).  Carry propagation brings
 * loosely reduced elements back into range.
 *
 * Multiplication uses unsigned __int128 for the 51x51->102 bit products.
 * GCC supports __int128 fully in -ffreestanding -nostdlib mode on x86-64.
 *
 * MONTGOMERY LADDER
 * -----------------
 * Constant-time double-and-add using the differential addition formula on
 * the Montgomery curve y^2 = x^3 + 486662*x^2 + x over GF(2^255-19).
 * The conditional swap (cswap) is implemented branch-free by XOR-masking
 * all 5 limbs of both coordinates simultaneously.
 *
 * BUILD
 * -----
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *       -c userspace/lib/crypto/x25519.c -o x25519.o
 *   objdump -d x25519.o | grep 'fs:0x28'   # must be empty
 */

#include "x25519.h"

/* ---- basic integer types (no standard headers) ------------------------ */
typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned __int128  u128;
typedef signed long long   i64;

/* ---- local memory helpers (no libc) ----------------------------------- */
static void fe_memset(void *dst, int v, u64 n)
{
    u8 *d = (u8 *)dst;
    while (n--) *d++ = (u8)v;
}

static void fe_memcpy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

static int fe_memcmp(const void *a, const void *b, u64 n)
{
    const u8 *p = (const u8 *)a;
    const u8 *q = (const u8 *)b;
    u8 diff = 0;
    while (n--) diff |= (*p++ ^ *q++);
    return (int)diff;   /* 0 iff equal */
}

/* ---- field element type: 5 x 51-bit limbs in u64 --------------------- */
typedef u64 fe[5];

/* ---- constants ------------------------------------------------------- */
/* p = 2^255 - 19, for Barrett/final reduction */
/* a24 = (486662-2)/4 = 121665 */
#define A24  121665ULL

/* ---- field arithmetic ------------------------------------------------ */

/* fe_0: zero */
static void fe_0(fe h) { h[0]=h[1]=h[2]=h[3]=h[4]=0; }

/* fe_1: one */
static void fe_1(fe h) { h[0]=1; h[1]=h[2]=h[3]=h[4]=0; }

/* fe_copy */
static void fe_copy(fe dst, const fe src)
{
    dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3]; dst[4]=src[4];
}

/*
 * fe_carry -- propagate carries so all limbs are in [0, 2^51).
 *
 * After multiplication the limbs can be up to ~104 bits wide; after add/sub
 * they can be up to ~52 bits.  One full carry sweep suffices for add/sub;
 * two sweeps are needed after mul/square (the second pass handles carry-out
 * from limb 4 being folded back via *19).
 */
static void fe_carry(fe h)
{
    u64 c;
    /* First pass */
    c = h[0] >> 51; h[0] &= 0x7ffffffffffffULL; h[1] += c;
    c = h[1] >> 51; h[1] &= 0x7ffffffffffffULL; h[2] += c;
    c = h[2] >> 51; h[2] &= 0x7ffffffffffffULL; h[3] += c;
    c = h[3] >> 51; h[3] &= 0x7ffffffffffffULL; h[4] += c;
    c = h[4] >> 51; h[4] &= 0x7ffffffffffffULL; h[0] += 19 * c;
    /* Second pass (mop up any carry introduced by the *19 above) */
    c = h[0] >> 51; h[0] &= 0x7ffffffffffffULL; h[1] += c;
    c = h[1] >> 51; h[1] &= 0x7ffffffffffffULL; h[2] += c;
    c = h[2] >> 51; h[2] &= 0x7ffffffffffffULL; h[3] += c;
    c = h[3] >> 51; h[3] &= 0x7ffffffffffffULL; h[4] += c;
    c = h[4] >> 51; h[4] &= 0x7ffffffffffffULL; h[0] += 19 * c;
}

/* fe_add: h = f + g  (loosely reduced; call fe_carry before use in mul) */
static void fe_add(fe h, const fe f, const fe g)
{
    h[0] = f[0] + g[0];
    h[1] = f[1] + g[1];
    h[2] = f[2] + g[2];
    h[3] = f[3] + g[3];
    h[4] = f[4] + g[4];
}

/*
 * fe_sub: h = f - g.
 *
 * We add 4*p (in 51-bit limb form) to f before subtracting g so every output
 * limb stays non-negative even when f is fully reduced and g is maximal.
 *
 * p in 51-bit limbs: {2^51-19, 2^51-1, 2^51-1, 2^51-1, 2^51-1}.
 * 4*p limb values:
 *   limb[0] = 4*(2^51-19) = 2^53-76 = 0x1fffffffffffb4  (fits in 53 bits)
 *   limb[1..4] = 4*(2^51-1) = 2^53-4 = 0x1ffffffffffffc  (fits in 53 bits)
 */
static void fe_sub(fe h, const fe f, const fe g)
{
    h[0] = (f[0] + 0x1fffffffffffb4ULL) - g[0];
    h[1] = (f[1] + 0x1ffffffffffffcULL) - g[1];
    h[2] = (f[2] + 0x1ffffffffffffcULL) - g[2];
    h[3] = (f[3] + 0x1ffffffffffffcULL) - g[3];
    h[4] = (f[4] + 0x1ffffffffffffcULL) - g[4];
}

/*
 * fe_mul: h = f * g  (mod 2^255-19).
 *
 * Schoolbook 5x5 multiply collecting into 128-bit accumulators, then reduce
 * using the identity 2^255 = 19 (mod p), which for 51-bit limbs gives:
 *   limb[i] * limb[j] contributes to output limb[(i+j) mod 5] with a factor
 *   of 19 when i+j >= 5.
 */
static void fe_mul(fe h, const fe f, const fe g)
{
    /* Precompute g[i]*19 for cross-wrap terms */
    u64 g1_19 = g[1] * 19, g2_19 = g[2] * 19;
    u64 g3_19 = g[3] * 19, g4_19 = g[4] * 19;

    u128 t0, t1, t2, t3, t4;

    t0 = (u128)f[0]*g[0]   + (u128)f[1]*g4_19 + (u128)f[2]*g3_19
       + (u128)f[3]*g2_19  + (u128)f[4]*g1_19;
    t1 = (u128)f[0]*g[1]   + (u128)f[1]*g[0]  + (u128)f[2]*g4_19
       + (u128)f[3]*g3_19  + (u128)f[4]*g2_19;
    t2 = (u128)f[0]*g[2]   + (u128)f[1]*g[1]  + (u128)f[2]*g[0]
       + (u128)f[3]*g4_19  + (u128)f[4]*g3_19;
    t3 = (u128)f[0]*g[3]   + (u128)f[1]*g[2]  + (u128)f[2]*g[1]
       + (u128)f[3]*g[0]   + (u128)f[4]*g4_19;
    t4 = (u128)f[0]*g[4]   + (u128)f[1]*g[3]  + (u128)f[2]*g[2]
       + (u128)f[3]*g[1]   + (u128)f[4]*g[0];

    /* Propagate carries using the 51-bit mask */
#define MASK51  0x7ffffffffffffULL
    u64 c;
    h[0] = (u64)(t0 & MASK51); c = (u64)(t0 >> 51);
    t1 += c;
    h[1] = (u64)(t1 & MASK51); c = (u64)(t1 >> 51);
    t2 += c;
    h[2] = (u64)(t2 & MASK51); c = (u64)(t2 >> 51);
    t3 += c;
    h[3] = (u64)(t3 & MASK51); c = (u64)(t3 >> 51);
    t4 += c;
    h[4] = (u64)(t4 & MASK51); c = (u64)(t4 >> 51);
    /* carry out of limb 4 wraps: 2^255 = 19 mod p */
    h[0] += 19 * c;
    /* one final carry from limb 0 in case h[0] overflowed 51 bits */
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
#undef MASK51
}

/*
 * fe_sq: h = f^2  (mod 2^255-19).
 *
 * Identical structure to fe_mul(h, f, f) but saves 5 multiplies by using
 * 2*f[i]*f[j] for i<j cross terms.
 */
static void fe_sq(fe h, const fe f)
{
    /*
     * For squaring with 5x51-bit limbs, output limb k collects:
     *   sum_{i+j=k,   i<=j} (1 + (i!=j)) * f[i]*f[j]            (no wrap)
     *   + 19 * sum_{i+j=k+5, i<=j} (1 + (i!=j)) * f[i]*f[j]    (wrapped)
     *
     * Cross terms (i!=j) get factor 2; diagonal terms (i==j) get factor 1
     * (and wrapped diagonals get factor 19, NOT 38).
     */
    u64 f0_2  = f[0]*2,  f1_2  = f[1]*2;   /* for cross terms */
    u64 f1_38 = f[1]*38, f2_38 = f[2]*38;  /* 2*19 for wrapped cross terms */
    u64 f3_38 = f[3]*38;
    u64 f3_19 = f[3]*19, f4_19 = f[4]*19;  /* 1*19 for wrapped diagonal terms */

    u128 t0, t1, t2, t3, t4;

    /* k=0: f0^2 + 19*(2*f1*f4 + 2*f2*f3) */
    t0 = (u128)f[0]*f[0]  + (u128)f1_38*f[4]  + (u128)f2_38*f[3];

    /* k=1: 2*f0*f1 + 19*(2*f2*f4 + f3^2)
     * f3^2 is a diagonal wrapped term: factor is 19 only, not 38. */
    t1 = (u128)f0_2*f[1]  + (u128)f2_38*f[4]  + (u128)f3_19*f[3];

    /* k=2: 2*f0*f2 + f1^2 + 19*(2*f3*f4)
     * f1^2 is an unwrapped diagonal: no extra factor. */
    t2 = (u128)f0_2*f[2]  + (u128)f[1]*f[1]   + (u128)f3_38*f[4];

    /* k=3: 2*f0*f3 + 2*f1*f2 + 19*(f4^2)
     * f4^2 is a diagonal wrapped term: factor is 19 only. */
    t3 = (u128)f0_2*f[3]  + (u128)f1_2*f[2]   + (u128)f4_19*f[4];

    /* k=4: 2*f0*f4 + 2*f1*f3 + f2^2
     * f2^2 is an unwrapped diagonal: no extra factor. */
    t4 = (u128)f0_2*f[4]  + (u128)f1_2*f[3]   + (u128)f[2]*f[2];

#define MASK51  0x7ffffffffffffULL
    u64 c;
    h[0] = (u64)(t0 & MASK51); c = (u64)(t0 >> 51);
    t1 += c;
    h[1] = (u64)(t1 & MASK51); c = (u64)(t1 >> 51);
    t2 += c;
    h[2] = (u64)(t2 & MASK51); c = (u64)(t2 >> 51);
    t3 += c;
    h[3] = (u64)(t3 & MASK51); c = (u64)(t3 >> 51);
    t4 += c;
    h[4] = (u64)(t4 & MASK51); c = (u64)(t4 >> 51);
    h[0] += 19 * c;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
#undef MASK51
}

/*
 * fe_mul_small: h = f * small  where small fits in 17 bits (e.g. 121665).
 * Avoids the full 5x5 schoolbook; used for the a24 step in the ladder.
 */
static void fe_mul_small(fe h, const fe f, u64 small)
{
    u128 t0 = (u128)f[0]*small;
    u128 t1 = (u128)f[1]*small;
    u128 t2 = (u128)f[2]*small;
    u128 t3 = (u128)f[3]*small;
    u128 t4 = (u128)f[4]*small;

#define MASK51  0x7ffffffffffffULL
    u64 c;
    h[0] = (u64)(t0 & MASK51); c = (u64)(t0 >> 51);
    t1 += c;
    h[1] = (u64)(t1 & MASK51); c = (u64)(t1 >> 51);
    t2 += c;
    h[2] = (u64)(t2 & MASK51); c = (u64)(t2 >> 51);
    t3 += c;
    h[3] = (u64)(t3 & MASK51); c = (u64)(t3 >> 51);
    t4 += c;
    h[4] = (u64)(t4 & MASK51); c = (u64)(t4 >> 51);
    h[0] += 19 * c;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
#undef MASK51
}

/*
 * fe_invert: h = f^(-1) mod p  using Fermat's little theorem: f^(p-2).
 *
 * p - 2 = 2^255 - 21.
 * We use the standard addition chain that computes this with 254 squarings
 * and 11 multiplications (the well-known "djb" chain).
 *
 * Chain overview (exponent bits of p-2 = 2^255-21):
 *   t0 = f^(2^1)
 *   t1 = f^(2^2-1)  = t0 * f^1
 *   t2 = f^(2^3)    = t1^2
 *   ...
 * We follow the NaCl/tweetnacl sequence which is widely audited.
 */
static void fe_invert(fe out, const fe z)
{
    int i;

    /*
     * Compute z^(p-2) = z^(2^255-21) using the standard NaCl/SUPERCOP chain:
     *   z2      = z^2
     *   z9      = z^9        (via z4=z2^2, z8=z4^2, z9=z8*z)
     *   z11     = z9*z2
     *   z2_5_0  = z^(2^5  - 1)
     *   z2_10_0 = z^(2^10 - 1)
     *   z2_20_0 = z^(2^20 - 1)
     *   z2_50_0 = z^(2^50 - 1)
     *   z2_100_0= z^(2^100- 1)
     *   ...     = z^(2^255- 21)
     */
    fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t0;

    /* z2 = z^2 */
    fe_sq(z2, z);
    /* t0 = z^4 */
    fe_sq(t0, z2);
    /* t0 = z^8 */
    fe_sq(t0, t0);
    /* z9 = z^9 */
    fe_mul(z9, t0, z);
    /* z11 = z^11 */
    fe_mul(z11, z9, z2);
    /* t0 = z^22 */
    fe_sq(t0, z11);
    /* z2_5_0 = z^(2^5 - 1) = z^31 */
    fe_mul(z2_5_0, t0, z9);

    /* z2_10_0 = z^(2^10 - 1) */
    fe_sq(t0, z2_5_0);
    for (i = 1; i < 5; i++) fe_sq(t0, t0);   /* t0 = z^(2^10 - 2^5) */
    fe_mul(z2_10_0, t0, z2_5_0);              /* z^(2^10 - 1) */

    /* z2_20_0 = z^(2^20 - 1) */
    fe_sq(t0, z2_10_0);
    for (i = 1; i < 10; i++) fe_sq(t0, t0);  /* t0 = z^(2^20 - 2^10) */
    fe_mul(z2_20_0, t0, z2_10_0);             /* z^(2^20 - 1) */

    /* z2_40_0 = z^(2^40 - 1) */
    fe_sq(t0, z2_20_0);
    for (i = 1; i < 20; i++) fe_sq(t0, t0);  /* t0 = z^(2^40 - 2^20) */
    fe_mul(t0, t0, z2_20_0);                  /* z^(2^40 - 1) */

    /* z2_50_0 = z^(2^50 - 1) */
    fe_sq(t0, t0);
    for (i = 1; i < 10; i++) fe_sq(t0, t0);  /* t0 = z^(2^50 - 2^10) */
    fe_mul(z2_50_0, t0, z2_10_0);             /* z^(2^50 - 1) */

    /* z2_100_0 = z^(2^100 - 1) */
    fe_sq(t0, z2_50_0);
    for (i = 1; i < 50; i++) fe_sq(t0, t0);  /* t0 = z^(2^100 - 2^50) */
    fe_mul(z2_100_0, t0, z2_50_0);            /* z^(2^100 - 1) */

    /* z^(2^200 - 1) */
    fe_sq(t0, z2_100_0);
    for (i = 1; i < 100; i++) fe_sq(t0, t0); /* z^(2^200 - 2^100) */
    fe_mul(t0, t0, z2_100_0);                 /* z^(2^200 - 1) */

    /* z^(2^250 - 1) */
    fe_sq(t0, t0);
    for (i = 1; i < 50; i++) fe_sq(t0, t0);  /* z^(2^250 - 2^50) */
    fe_mul(t0, t0, z2_50_0);                  /* z^(2^250 - 1) */

    /* z^(2^255 - 21) = z^(p-2):
     * At this point t0 = z^(2^250 - 1).  Square 5 more times to reach
     * z^(2^255 - 32), then multiply by z^11 to get z^(2^255-21). */
    fe_sq(t0, t0);                             /* z^(2^251 - 2) */
    fe_sq(t0, t0);                             /* z^(2^252 - 4) */
    fe_sq(t0, t0);                             /* z^(2^253 - 8) */
    fe_sq(t0, t0);                             /* z^(2^254 - 16) */
    fe_sq(t0, t0);                             /* z^(2^255 - 32) */
    fe_mul(out, t0, z11);                      /* z^(2^255 - 32 + 11) = z^(2^255-21) */
}

/*
 * fe_from_bytes -- decode 32 little-endian bytes into a field element.
 * Bit 255 (the top bit of byte 31) is cleared per RFC 7748.
 */
static void fe_from_bytes(fe h, const u8 in[32])
{
    /* Load 256 bits as a little-endian integer, clear bit 255. */
    u64 b[4];
    int i;
    for (i = 0; i < 4; i++) {
        b[i] = (u64)in[i*8+0]        | ((u64)in[i*8+1] <<  8)
             | ((u64)in[i*8+2] << 16) | ((u64)in[i*8+3] << 24)
             | ((u64)in[i*8+4] << 32) | ((u64)in[i*8+5] << 40)
             | ((u64)in[i*8+6] << 48) | ((u64)in[i*8+7] << 56);
    }
    /* Clear the top bit (bit 255) which lives in b[3] bit 63. */
    b[3] &= ~(1ULL << 63);

    /* Distribute into 51-bit limbs from the 256-bit little-endian value.
     *   limb0: bits   0..50  = b[0] & MASK51
     *   limb1: bits  51..101 = (b[0]>>51) | (b[1]<<13) & MASK51
     *   limb2: bits 102..152 = (b[1]>>38) | (b[2]<<26) & MASK51
     *   limb3: bits 153..203 = (b[2]>>25) | (b[3]<<39) & MASK51
     *   limb4: bits 204..254 = b[3]>>12
     */
#define MASK51  0x7ffffffffffffULL
    h[0] =  b[0]                        & MASK51;
    h[1] = ((b[0] >> 51) | (b[1] << 13)) & MASK51;
    h[2] = ((b[1] >> 38) | (b[2] << 26)) & MASK51;
    h[3] = ((b[2] >> 25) | (b[3] << 39)) & MASK51;
    h[4] =   b[3] >> 12;                           /* at most 52 bits, bit255=0 */
#undef MASK51
}

/*
 * fe_to_bytes -- encode a field element as 32 little-endian bytes.
 *
 * First fully reduce the element to [0, p-1] using a conditional subtraction,
 * then pack back into bytes.
 */
static void fe_to_bytes(u8 out[32], const fe h)
{
    fe t;
    u64 carry;
    int i;

    fe_copy(t, h);
    fe_carry(t);

    /* One final carry to get all limbs < 2^51 */
    carry = t[0] >> 51; t[0] &= 0x7ffffffffffffULL; t[1] += carry;
    carry = t[1] >> 51; t[1] &= 0x7ffffffffffffULL; t[2] += carry;
    carry = t[2] >> 51; t[2] &= 0x7ffffffffffffULL; t[3] += carry;
    carry = t[3] >> 51; t[3] &= 0x7ffffffffffffULL; t[4] += carry;
    carry = t[4] >> 51; t[4] &= 0x7ffffffffffffULL; t[0] += 19 * carry;

    /* Conditionally subtract p = 2^255 - 19 if t >= p.
     * We add 19 and see if that overflows limb 4 past 2^51; if so, the result
     * after subtraction is the canonical form. */
    fe r;
    r[0] = t[0] + 19;
    carry = r[0] >> 51; r[0] &= 0x7ffffffffffffULL;
    r[1] = t[1] + carry; carry = r[1] >> 51; r[1] &= 0x7ffffffffffffULL;
    r[2] = t[2] + carry; carry = r[2] >> 51; r[2] &= 0x7ffffffffffffULL;
    r[3] = t[3] + carry; carry = r[3] >> 51; r[3] &= 0x7ffffffffffffULL;
    r[4] = t[4] + carry;

    /* If the top limb overflowed, r is the reduced form; otherwise keep t.
     * Mask is 0xfff...f if r[4] >= 2^51, else 0. */
    u64 mask = (u64)(-(i64)(r[4] >> 51));   /* all-ones if overflow, else 0 */
    for (i = 0; i < 5; i++)
        t[i] ^= mask & (t[i] ^ r[i]);

    /* Clear top bit of t[4] -- it should be 0 already if we subtracted */
    t[4] &= 0x7ffffffffffffULL;

    /* Pack 5 * 51-bit limbs back into 256 bits (little-endian). */
    u64 w0 = t[0] | (t[1] << 51);
    u64 w1 = (t[1] >> 13) | (t[2] << 38);
    u64 w2 = (t[2] >> 26) | (t[3] << 25);
    u64 w3 = (t[3] >> 39) | (t[4] << 12);

    for (i = 0; i < 8; i++) { out[i]    = (u8)(w0 >> (8*i)); }
    for (i = 0; i < 8; i++) { out[8+i]  = (u8)(w1 >> (8*i)); }
    for (i = 0; i < 8; i++) { out[16+i] = (u8)(w2 >> (8*i)); }
    for (i = 0; i < 8; i++) { out[24+i] = (u8)(w3 >> (8*i)); }
}

/* ---- constant-time conditional swap ---------------------------------- */
/*
 * fe_cswap -- swap (a, b) iff swap_bit == 1, constant time.
 *
 * Uses XOR masking; no branches on swap_bit.
 * swap_bit must be exactly 0 or 1.
 */
static void fe_cswap(fe a, fe b, u64 swap_bit)
{
    u64 mask = (u64)(-(i64)swap_bit);  /* 0xfff...f if swap, else 0 */
    u64 x;
    int i;
    for (i = 0; i < 5; i++) {
        x    = mask & (a[i] ^ b[i]);
        a[i] ^= x;
        b[i] ^= x;
    }
}

/* ---- Montgomery ladder ----------------------------------------------- */
/*
 * x25519_raw -- Montgomery-ladder scalar multiplication.
 *
 * Computes out_u = scalar * in_u on the Montgomery curve
 *   y^2 = x^3 + 486662*x^2 + x  over GF(2^255-19).
 *
 * The input point is given as its u-coordinate (already decoded into fe form).
 * The scalar is the clamped 255-bit integer (already byte-clamped, but we
 * iterate bits 254..0).
 *
 * Algorithm: RFC 7748 §5 (Montgomery ladder with differential addition).
 */
static void x25519_raw(u8 out[32], const u8 scalar_bytes[32], const u8 u_bytes[32])
{
    /* Clamp a local copy of the scalar */
    u8 e[32];
    fe_memcpy(e, scalar_bytes, 32);
    e[0]  &= 248;
    e[31] &= 127;
    e[31] |= 64;

    /* Decode u, masking bit 255 */
    fe u;
    fe_from_bytes(u, u_bytes);  /* fe_from_bytes already clears bit 255 */

    /* Ladder state:
     *   x_1  = u (fixed input)
     *   x_2, z_2 = projective coords of R0 (starts at point-at-infinity, 1:0)
     *   x_3, z_3 = projective coords of R1 (starts at u:1)
     */
    fe x_1, x_2, z_2, x_3, z_3;
    fe_copy(x_1, u);
    fe_1(x_2);   fe_0(z_2);   /* R0 = O (projective 1:0) */
    fe_copy(x_3, u); fe_1(z_3); /* R1 = u */

    u64 swap = 0;
    int bit;

    /* Iterate from bit 254 down to bit 0 */
    for (bit = 254; bit >= 0; bit--) {
        u64 k_t = (e[bit / 8] >> (bit & 7)) & 1;

        swap ^= k_t;
        fe_cswap(x_2, x_3, swap);
        fe_cswap(z_2, z_3, swap);
        swap = k_t;

        /* Montgomery differential-addition and doubling step.
         * Follows RFC 7748 Appendix / the standard projective formulas:
         *
         *   A  = x_2 + z_2
         *   AA = A^2
         *   B  = x_2 - z_2
         *   BB = B^2
         *   E  = AA - BB
         *   C  = x_3 + z_3
         *   D  = x_3 - z_3
         *   DA = D*A
         *   CB = C*B
         *   x_3 = (DA + CB)^2
         *   z_3 = x_1 * (DA - CB)^2
         *   x_2 = AA * BB
         *   z_2 = E * (AA + a24*E)
         */
        fe A, AA, B, BB, E, C, D, DA, CB;

        fe_add(A, x_2, z_2);
        fe_sq(AA, A);
        fe_sub(B, x_2, z_2);
        fe_sq(BB, B);
        fe_sub(E, AA, BB);
        fe_add(C, x_3, z_3);
        fe_sub(D, x_3, z_3);
        fe_mul(DA, D, A);
        fe_mul(CB, C, B);

        fe t0, t1;
        fe_add(t0, DA, CB);
        fe_sq(x_3, t0);            /* x_3 = (DA+CB)^2 */

        fe_sub(t1, DA, CB);
        fe_sq(t0, t1);
        fe_mul(z_3, x_1, t0);      /* z_3 = x_1 * (DA-CB)^2 */

        fe_mul(x_2, AA, BB);       /* x_2 = AA*BB */

        fe_mul_small(t0, E, A24);  /* t0  = a24 * E */
        fe_add(t1, AA, t0);        /* t1  = AA + a24*E */
        fe_mul(z_2, E, t1);        /* z_2 = E*(AA + a24*E) */
    }

    /* Final conditional swap */
    fe_cswap(x_2, x_3, swap);
    fe_cswap(z_2, z_3, swap);

    /* Recover affine u-coordinate: result = x_2 * z_2^(p-2) */
    fe inv_z2, result;
    fe_invert(inv_z2, z_2);
    fe_mul(result, x_2, inv_z2);

    fe_to_bytes(out, result);
}

/* ---- public API ------------------------------------------------------ */

int x25519(unsigned char out[32],
           const unsigned char scalar[32],
           const unsigned char point[32])
{
    x25519_raw(out, scalar, point);
    return 0;
}

int x25519_base(unsigned char out[32], const unsigned char scalar[32])
{
    /* Basepoint: u = 9 (little-endian, 32 bytes) */
    static const unsigned char basepoint[32] = { 9 };
    x25519_raw(out, scalar, basepoint);
    return 0;
}

/* ---- self-test ------------------------------------------------------- */
/*
 * RFC 7748 §5.2 test vectors for X25519.
 *
 * Vector 1:
 *   scalar = a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4
 *   u      = e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c
 *   output = c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552
 *
 * Vector 2:
 *   scalar = 4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d
 *   u      = e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493
 *   output = 95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957
 *
 * Iterated test (1 iteration): scalar=u=9 (basepoint) ->
 *   output = 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079
 */

/* Helper to decode a hex string into bytes (length = 2*n hex chars). */
static void hex_to_bytes(u8 *out, const char *hex, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        u8 hi = (u8)hex[2*i];
        u8 lo = (u8)hex[2*i+1];
        hi = (hi >= '0' && hi <= '9') ? hi-'0'
           : (hi >= 'a' && hi <= 'f') ? hi-'a'+10
           : hi-'A'+10;
        lo = (lo >= '0' && lo <= '9') ? lo-'0'
           : (lo >= 'a' && lo <= 'f') ? lo-'a'+10
           : lo-'A'+10;
        out[i] = (u8)((hi << 4) | lo);
    }
}

int x25519_selftest(void)
{
    u8 scalar[32], u_in[32], expected[32], result[32];

    /* ------------------------------------------------------------------ */
    /* Vector 1 (RFC 7748 §5.2, first test vector)                        */
    /* ------------------------------------------------------------------ */
    hex_to_bytes(scalar,
        "a546e36bf0527c9d3b16154b82465edd"
        "62144c0ac1fc5a18506a2244ba449ac4", 32);
    hex_to_bytes(u_in,
        "e6db6867583030db3594c1a424b15f7c"
        "726624ec26b3353b10a903a6d0ab1c4c", 32);
    hex_to_bytes(expected,
        "c3da55379de9c6908e94ea4df28d084f"
        "32eccf03491c71f754b4075577a28552", 32);

    x25519(result, scalar, u_in);
    if (fe_memcmp(result, expected, 32) != 0) return -1;

    /* ------------------------------------------------------------------ */
    /* Vector 2 (RFC 7748 §5.2, second test vector)                       */
    /* ------------------------------------------------------------------ */
    hex_to_bytes(scalar,
        "4b66e9d4d1b4673c5ad22691957d6af5"
        "c11b6421e0ea01d42ca4169e7918ba0d", 32);
    hex_to_bytes(u_in,
        "e5210f12786811d3f4b7959d0538ae2c"
        "31dbe7106fc03c3efc4cd549c715a493", 32);
    hex_to_bytes(expected,
        "95cbde9476e8907d7aade45cb4b873f8"
        "8b595a68799fa152e6f8f7647aac7957", 32);

    x25519(result, scalar, u_in);
    if (fe_memcmp(result, expected, 32) != 0) return -1;

    /* ------------------------------------------------------------------ */
    /* Iterated test: 1 iteration of x25519(k, u) with k=u=9             */
    /* (RFC 7748 §5.2, "After one iteration".)                            */
    /* ------------------------------------------------------------------ */
    fe_memset(scalar,   0, 32); scalar[0]   = 9;
    fe_memset(u_in,     0, 32); u_in[0]     = 9;
    hex_to_bytes(expected,
        "422c8e7a6227d7bca1350b3e2bb7279f"
        "7897b87bb6854b783c60e80311ae3079", 32);

    x25519(result, scalar, u_in);
    if (fe_memcmp(result, expected, 32) != 0) return -1;

    return 0;  /* all vectors passed */
}
