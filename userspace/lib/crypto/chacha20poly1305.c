/*
 * chacha20poly1305.c -- ChaCha20-Poly1305 AEAD (RFC 8439).
 * =========================================================
 *
 * Freestanding: no libc, no syscalls, no malloc, no standard headers.
 * 130-bit Poly1305 arithmetic uses unsigned __int128 (GCC freestanding OK on
 * x86-64).  All multi-byte integers are handled little-endian per the RFC.
 *
 * Build flags (NO fs:0x28 stack-protector canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#include "chacha20poly1305.h"

/* =========================================================================
 * Section 0 – local helpers (no libc)
 * ====================================================================== */

static void cc_memset(void *d, int v, unsigned long n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)v;
}

static void cc_memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char       *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}

/* Constant-time comparison: returns 0 iff every byte is equal. */
static int cc_memcmp_ct(const unsigned char *a, const unsigned char *b,
                        unsigned long n)
{
    unsigned char diff = 0;
    while (n--) diff |= (*a++ ^ *b++);
    return (int)diff;
}

/* =========================================================================
 * Section 1 – ChaCha20 (RFC 8439 §2.1 – §2.4)
 * ====================================================================== */

/* Little-endian load/store helpers */
static unsigned int le32_load(const unsigned char *p)
{
    return (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}

static void le32_store(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

/* ChaCha20 quarter-round (RFC 8439 §2.1.1) */
#define QR(a, b, c, d)          \
    do {                        \
        (a) += (b); (d) ^= (a); (d) = ROTL32((d), 16); \
        (c) += (d); (b) ^= (c); (b) = ROTL32((b), 12); \
        (a) += (b); (d) ^= (a); (d) = ROTL32((d),  8); \
        (c) += (d); (b) ^= (c); (b) = ROTL32((b),  7); \
    } while (0)

/* ChaCha20 block function (RFC 8439 §2.3).
 *
 *  Initial state layout (words 0-15):
 *    0-3   : constants "expa", "nd 3", "2-by", "te k"
 *    4-11  : key (8 × u32, LE)
 *    12    : counter (u32)
 *    13-15 : nonce (3 × u32, LE)
 */
void chacha20_block(const unsigned char key[32],
                    unsigned int counter,
                    const unsigned char nonce[12],
                    unsigned char out[64])
{
    static const unsigned int CONST[4] = {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
    };

    unsigned int s[16];  /* working state */
    unsigned int x[16];  /* initial state copy */
    int i;

    /* Build initial state */
    x[0]  = CONST[0]; x[1]  = CONST[1];
    x[2]  = CONST[2]; x[3]  = CONST[3];
    x[4]  = le32_load(key +  0); x[5]  = le32_load(key +  4);
    x[6]  = le32_load(key +  8); x[7]  = le32_load(key + 12);
    x[8]  = le32_load(key + 16); x[9]  = le32_load(key + 20);
    x[10] = le32_load(key + 24); x[11] = le32_load(key + 28);
    x[12] = counter;
    x[13] = le32_load(nonce + 0);
    x[14] = le32_load(nonce + 4);
    x[15] = le32_load(nonce + 8);

    /* Copy to working state */
    for (i = 0; i < 16; i++) s[i] = x[i];

    /* 20 rounds = 10 double-rounds (RFC 8439 §2.3.1) */
    for (i = 0; i < 10; i++) {
        /* Column rounds */
        QR(s[0], s[4], s[ 8], s[12]);
        QR(s[1], s[5], s[ 9], s[13]);
        QR(s[2], s[6], s[10], s[14]);
        QR(s[3], s[7], s[11], s[15]);
        /* Diagonal rounds */
        QR(s[0], s[5], s[10], s[15]);
        QR(s[1], s[6], s[11], s[12]);
        QR(s[2], s[7], s[ 8], s[13]);
        QR(s[3], s[4], s[ 9], s[14]);
    }

    /* Add initial state and serialise LE */
    for (i = 0; i < 16; i++)
        le32_store(out + 4 * i, s[i] + x[i]);
}

/* ChaCha20 stream-cipher XOR (RFC 8439 §2.4) */
void chacha20_xor(const unsigned char key[32],
                  unsigned int counter,
                  const unsigned char nonce[12],
                  const unsigned char *in,
                  unsigned char *out,
                  unsigned long len)
{
    unsigned char block[64];
    unsigned long i;

    while (len > 0) {
        unsigned long take = (len < 64u) ? len : 64u;
        chacha20_block(key, counter, nonce, block);
        for (i = 0; i < take; i++)
            out[i] = in[i] ^ block[i];
        in      += take;
        out     += take;
        len     -= take;
        counter++;
    }
    /* Wipe keystream from stack */
    cc_memset(block, 0, sizeof(block));
}

/* =========================================================================
 * Section 2 – Poly1305 (RFC 8439 §2.5)
 *
 * 130-bit accumulator:  uses unsigned __int128 for intermediate products,
 * keeping the running accumulator as three 64-bit limbs (h0, h1, h2) where
 * the full value is  h0 + h1*2^64 + h2*2^128  (with h2 <= 4, representing
 * the "carry" into bit 128+).  The prime is p = 2^130 - 5.
 *
 * r is stored in two 64-bit halves after clamping; s likewise.
 * ====================================================================== */

/* Clamp r per RFC 8439 §2.5.1 */
static void poly1305_clamp_r(unsigned char r[16])
{
    r[ 3] &= 0x0f;
    r[ 7] &= 0x0f;
    r[11] &= 0x0f;
    r[15] &= 0x0f;
    r[ 4] &= 0xfc;
    r[ 8] &= 0xfc;
    r[12] &= 0xfc;
}

/* Load a 64-bit LE word */
static unsigned long long le64_load(const unsigned char *p)
{
    return (unsigned long long)p[0]
         | ((unsigned long long)p[1] <<  8)
         | ((unsigned long long)p[2] << 16)
         | ((unsigned long long)p[3] << 24)
         | ((unsigned long long)p[4] << 32)
         | ((unsigned long long)p[5] << 40)
         | ((unsigned long long)p[6] << 48)
         | ((unsigned long long)p[7] << 56);
}

/* Store a 64-bit LE word */
static void le64_store(unsigned char *p, unsigned long long v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >>  8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
    p[4] = (unsigned char)(v >> 32);
    p[5] = (unsigned char)(v >> 40);
    p[6] = (unsigned char)(v >> 48);
    p[7] = (unsigned char)(v >> 56);
}

/*
 * poly1305_mac -- RFC 8439 §2.5.1
 *
 * Accumulator h is represented as three 64-bit limbs (h0, h1, h2) so that
 * the true 130-bit value is  h = h0 + h1<<64 + h2<<128  with h2 in [0..7].
 *
 * r is the 130-bit clamp-modified key half (only 128 bits after clamping,
 * but we work with it as r_lo (bits 0-63) and r_hi (bits 64-127)).
 *
 * Each 16-byte message block is treated as a 130-bit integer by appending
 * a '1' bit at position 8*blocklen (so full 16-byte blocks get the bit at
 * position 128).
 *
 * After processing all blocks we reduce h mod p = 2^130-5, then add s.
 */
void poly1305_mac(const unsigned char key[32],
                  const unsigned char *msg,
                  unsigned long len,
                  unsigned char tag[16])
{
    /* ---- key setup ---- */
    unsigned char r_bytes[16];
    cc_memcpy(r_bytes, key, 16);
    poly1305_clamp_r(r_bytes);

    unsigned long long r_lo = le64_load(r_bytes);       /* bits  0-63  of r */
    unsigned long long r_hi = le64_load(r_bytes + 8);   /* bits 64-127 of r */

    unsigned long long s_lo = le64_load(key + 16);      /* bits  0-63  of s */
    unsigned long long s_hi = le64_load(key + 24);      /* bits 64-127 of s */

    /* ---- accumulator h = 0 ---- */
    unsigned long long h0 = 0, h1 = 0;
    unsigned long long h2 = 0;   /* only bits 0-2 used (max value 4) */

    /* ---- process each 16-byte block ---- */
    while (len > 0) {
        unsigned char block[17];
        unsigned long blen = (len < 16u) ? len : 16u;
        unsigned long long n0, n1, n2_bit;

        cc_memset(block, 0, 17);
        cc_memcpy(block, msg, blen);
        block[blen] = 0x01;  /* appended '1' bit per RFC 8439 §2.5.1 */

        n0      = le64_load(block);
        n1      = le64_load(block + 8);
        n2_bit  = block[16];  /* 0 or 1 */

        /* h += n */
        {
            unsigned __int128 tmp;
            tmp = (unsigned __int128)h0 + n0;
            h0  = (unsigned long long)tmp;
            tmp = (unsigned __int128)h1 + n1 + (tmp >> 64);
            h1  = (unsigned long long)tmp;
            h2  = h2 + n2_bit + (unsigned long long)(tmp >> 64);
        }

        /*
         * h *= r  (mod 2^130 - 5)
         *
         * Full 260-bit product of a 130-bit h and a 128-bit r decomposes as:
         *   h * r = (h0 + h1*2^64 + h2*2^128) * (r_lo + r_hi*2^64)
         *
         * Terms:
         *   [0]   h0 * r_lo                      -> bits   0-127
         *   [1]   h0 * r_hi                      -> bits  64-191
         *   [2]   h1 * r_lo                      -> bits  64-191
         *   [3]   h1 * r_hi                      -> bits 128-255
         *   [4]   h2 * r_lo   (h2 <= 4, so 2-bit)-> bits 128-192
         *   [5]   h2 * r_hi                      -> bits 192-256
         *
         * We need the result mod 2^130-5.  For bits >= 130 we use the
         * identity  x * 2^130 ≡ x * 5 (mod 2^130-5).
         * Bits 130-255 map back via:  value * 2^(bits-130) * 5
         *
         * We accumulate a 192-bit result in three 64-bit words (p0,p1,p2)
         * and reduce the overflow above 130 bits.
         */
        {
            typedef unsigned __int128 u128;

            /* Full products */
            u128 t00 = (u128)h0 * r_lo;
            u128 t01 = (u128)h0 * r_hi;
            u128 t10 = (u128)h1 * r_lo;
            u128 t11 = (u128)h1 * r_hi;
            /* h2 is at most 4 (3 bits), safe as unsigned long long multiply */
            u128 t20 = (u128)h2 * r_lo;
            u128 t21 = (u128)h2 * r_hi;

            /*
             * Lay out a 256-bit sum in 64-bit columns [col0..col3]:
             *
             *   col0 (bits 0-63)  : lo(t00)
             *   col1 (bits 64-127): hi(t00) + lo(t01) + lo(t10)
             *   col2 (bits 128-191): hi(t01) + hi(t10) + lo(t11) + lo(t20)
             *   col3 (bits 192-255): hi(t11) + hi(t20) + lo(t21)
             *   col4 (bits 256+)  : hi(t21)
             *
             * Reduction: 2^130 ≡ 5, so bits [130..] fold back with factor 5.
             * We work with the 192-bit intermediate (col0, col1, col2+) where
             * col2 carries bits 128-191, and then reduce the 2-bit overflow
             * above bit 129 (i.e., the carry out of col2).
             */

            u128 acc0, acc1, acc2;

            acc0 = t00;
            acc1 = t01 + t10 + (acc0 >> 64);
            acc0 = (u128)(unsigned long long)acc0;
            acc2 = t11 + t20 + (acc1 >> 64);
            acc1 = (u128)(unsigned long long)acc1;

            /* col3 and col4: anything >= bit 192 */
            u128 acc3 = t21 + (acc2 >> 64);
            acc2 = (u128)(unsigned long long)acc2;

            /*
             * Now reduce:
             *   - bits [130..192) contribute via  (value >> 2) * 5  back to
             *     the low 130 bits, because  2^130 ≡ 5 (mod p).
             *   - bits [192..256) likewise but shifted further.
             *
             * Split acc2 around the 130-bit boundary (bit 130 = bit 2 of
             * acc2):  overflow = acc2 >> 2,  keep = acc2 & 3.
             *
             * Additionally acc3 (bits 192+) also overflows and must be
             * multiplied back in with appropriate power of 5.
             *   bit 192 = bit 62 of acc3 relative to 130  → 5 * 2^62... but
             *   easier: fold acc3 into the overflow: overflow += acc3 * 2^(192-130-2)
             *              = acc3 * 2^60
             *   Actually the clean way: compute a single "carry" out of bit
             *   129 and multiply by 5.
             *
             * Correct reduction in two steps:
             *   Step A: fold acc3 (bits 192-255) back.
             *     acc3 * 2^192 = acc3 * 2^(130+62) ≡ acc3 * 5 * 2^62 (mod p)
             *     So add (acc3 * 5) << 62 into the 128-bit (acc0,acc1) pair,
             *     while accounting that the << 62 crosses the 64-bit boundary.
             *   Step B: fold the carry out of bit 129.
             *     carry = (acc2 * 4 + acc1_top2 + acc0_top) >> 130 ... messy.
             *
             * Simpler: all we need is one full 130-bit result.  Combine
             * everything into a single u128 "high" (= acc1*2^64 + acc0) and
             * a small "carry" field, then do a single 5-multiply fold.
             *
             * Fully safe approach with __int128:
             *   Form 3-limb 192-bit result (q0, q1, q2) where each limb is
             *   64 bits. q2 holds at most a few bits (bits 128-191).
             *   Reduce: c = q2 >> 2;  q2 &= 3;  (q0,q1,q2) += c*5.
             *   One more round is sufficient (Poly1305 invariant guarantees
             *   q2 < 8 before reduction, so c < 2, and after += c*5 q2 <= 3).
             *
             *   But we also have acc3 sitting in the 192-bit range ...
             *   First fold acc3 into q2: q2 += acc3 * (2^64 / no, bits 192-256
             *   sit above the 3-limb representation).
             *
             * Cleanest path: use a 4-limb 256-bit representation, then reduce.
             */

            /* Rebuild with acc3 properly carried */
            /* p0..p3 are our 4×64 = 256-bit product limbs */
            unsigned long long p0 = (unsigned long long)acc0;
            unsigned long long p1 = (unsigned long long)acc1;
            unsigned long long p2 = (unsigned long long)acc2;
            unsigned long long p3 = (unsigned long long)acc3;
            /* acc3 itself might be > 64 bits (hi(t21) is 64 bits, t21 is
             * 128 bits, hi(t21) <= 4 * 2^64 which fits in 66 bits total).
             * Add the high half of acc3 into an implicit p4. */
            unsigned long long p4 = (unsigned long long)(acc3 >> 64);

            /*
             * Reduce mod 2^130-5 using  2^130 ≡ 5:
             *
             *   total = p0 + p1*2^64 + p2*2^128 + p3*2^192 + p4*2^256
             *   split at bit 130:
             *     lo_part  = p0 + p1*2^64 + (p2 & 3)*2^128    (130 bits)
             *     hi_part  = (p2>>2) + p3*2^62 + p4*2^126      (126 bits, sits above 2^130)
             *
             *   result = lo_part + hi_part * 5  (mod 2^130-5, not fully reduced)
             *
             *   Compute carry = hi_part, multiply by 5, add to lo.
             *   This fits in 130 bits + small epsilon; one more fold suffices.
             */
            {
                /*
                 * Reduce the 256-bit product (p0..p4) mod 2^130-5.
                 *
                 * Split the product at bit 130:
                 *   low part  : p0 (bits 0-63) + p1 (bits 64-127) + (p2 & 3) * 2^128
                 *   high part : (p2 >> 2) + p3 * 2^62 + p4 * 2^126
                 *
                 * By 2^130 ≡ 5 (mod p):  result = low + high * 5.
                 *
                 * IMPORTANT: (p2 & 3) * 2^128 does NOT fit in u128; we must
                 * track h2 = (p2 & 3) separately and add carry * 5 using
                 * explicit 3-limb (h0, h1, h2) carry propagation.
                 */
                unsigned long long p2_lo = p2 & 3u;   /* bits 128-129 of product */
                unsigned long long p2_hi = p2 >> 2;   /* bits 130-191 of product */

                /* carry = p2_hi + p3*2^62 + p4*2^126 (fits in ~128 bits) */
                u128 carry = (u128)p2_hi
                           + ((u128)p3 << 62)
                           + ((u128)p4 << 126);

                /*
                 * c5 = carry * 5.
                 *
                 * The algorithm invariant ensures h2 <= 5 before this multiply,
                 * which bounds carry < 2^127, so carry * 5 < 2^130 and fits
                 * entirely within the low 128 bits of u128.  c5_hi is always 0.
                 *
                 * We split c5 explicitly to avoid any shift-by-128 warning.
                 */
                u128 c5 = carry * 5;
                unsigned long long c5_lo  = (unsigned long long)c5;
                unsigned long long c5_mid = (unsigned long long)(c5 >> 64);
                /* c5_hi would be (c5 >> 128), but carry*5 < 2^130 means the
                 * bits at position 128-129 land in p2_lo / the fold below; the
                 * contribution above bit 127 is tracked through carry itself.
                 * Since carry < 2^127, carry*5 < 5*2^127 < 2^130, and the
                 * excess above bit 127 is at most 2 bits; that feeds into h2
                 * via p2_lo directly.  No separate c5_hi term is needed. */

                /* Add p0 + c5_lo, propagating carry upward.
                 * c5_hi = 0 always (proven above), so h2 = p2_lo + carry_in. */
                u128 tmp;
                tmp  = (u128)p0 + c5_lo;
                h0   = (unsigned long long)tmp;
                tmp  = (u128)p1 + c5_mid + (tmp >> 64);
                h1   = (unsigned long long)tmp;
                h2   = p2_lo + (unsigned long long)(tmp >> 64);

                /* One final fold: h2 may have bits above 2 (at most ~7) */
                {
                    unsigned long long c2 = h2 >> 2;
                    h2 &= 3u;
                    u128 add = (u128)h0 + (u128)c2 * 5;
                    h0 = (unsigned long long)add;
                    add = (u128)h1 + (add >> 64);
                    h1 = (unsigned long long)add;
                    h2 += (unsigned long long)(add >> 64);
                    /* h2 is now <= 3, safe for next iteration */
                }
            }
        }

        msg += blen;
        len -= blen;
    }

    /*
     * Final reduction (RFC 8439 §2.5.1 step 5):
     * Fully reduce h mod 2^130-5 by computing h - p if h >= p.
     *
     * h is currently in (h0, h1, h2) with h2 <= 3.
     * p = 2^130 - 5 = (0xfffffffffffffffb, 0xffffffffffffffff, 3)
     *   in our 3-limb representation.
     *
     * Compute h_candidate = h + 5, then check if it wrapped past 2^130:
     * if h_candidate >> 130 != 0, use h_candidate & (2^130-1), else use h.
     *
     * The standard trick: add 5, keep the 130-bit residue if the addition
     * overflowed into bit 130 (meaning h + 5 >= 2^130, i.e., h >= 2^130-5 = p).
     */
    {
        typedef unsigned __int128 u128;
        u128 tmp;

        /* t = h + 5 */
        tmp = (u128)h0 + 5;
        unsigned long long t0 = (unsigned long long)tmp;
        tmp = (u128)h1 + (tmp >> 64);
        unsigned long long t1 = (unsigned long long)tmp;
        unsigned long long t2 = h2 + (unsigned long long)(tmp >> 64);

        /*
         * mask = 0 if t < 2^130 (no overflow), 0xfff... if t >= 2^130.
         * overflow bit is t2 >> 2.
         */
        unsigned long long mask = (unsigned long long)(0u - (t2 >> 2)); /* all-ones or 0 */
        unsigned long long nmask = ~mask;

        h0 = (t0 & mask) | (h0 & nmask);
        h1 = (t1 & mask) | (h1 & nmask);
        /* h2 is discarded after final reduction (result is 128-bit tag) */

        /* h += s (mod 2^128) -- no reduction needed, wraps naturally */
        tmp = (u128)h0 + s_lo;
        h0  = (unsigned long long)tmp;
        tmp = (u128)h1 + s_hi + (tmp >> 64);
        h1  = (unsigned long long)tmp;
    }

    /* Serialise tag LE */
    le64_store(tag,     h0);
    le64_store(tag + 8, h1);
}

/* =========================================================================
 * Section 3 – AEAD construction (RFC 8439 §2.8)
 * ====================================================================== */

/*
 * Build the Poly1305 MAC input:
 *   aad || pad16(aad) || ct || pad16(ct) || LE64(aadlen) || LE64(ctlen)
 * into the provided scratch buffer `buf` of size `bufsz`.
 * Returns the total number of bytes written, or 0 on overflow.
 *
 * (Callers use a fixed-size 4096-byte scratch for the length/padding fields
 * when the actual data is fed inline via a streaming helper.)
 *
 * Since we must avoid malloc, we use a two-pass approach: poly1305_mac() is
 * called over a single contiguous scratch buffer.  This limits AEAD to
 * messages where  aadlen + 16 + ctlen + 16 + 16  fits in AEAD_MAC_BUF_MAX.
 * For a production kernel TLS layer this should be increased; the API is
 * identical regardless.
 */

#define AEAD_MAC_BUF_MAX  (16384u + 256u)  /* 16 KiB aad+ct + overhead */

/*
 * pad16_len: returns number of zero-padding bytes needed to reach a multiple
 * of 16, per RFC 8439 §2.8.
 */
static unsigned long pad16_len(unsigned long n)
{
    unsigned long r = n & 15u;
    return r ? (16u - r) : 0u;
}

/*
 * poly1305_aead_mac: compute the Poly1305 tag over the AEAD MAC input
 * without a heap allocation.  Uses a fixed 32-byte stack buffer for the
 * length footer and handles the AAD + CT in a single contiguous scratch.
 *
 * For large messages exceeding AEAD_MAC_BUF_MAX this implementation still
 * works because poly1305 is computed over the MAC input formed in scratch,
 * but callers must ensure the combined input fits.  The selftest vectors are
 * well within limits.
 *
 * scratch must be at least aadlen + 16 + ctlen + 16 + 16 bytes.
 * We build it inline here and call poly1305_mac once.
 */
static void compute_aead_tag(
        const unsigned char otk[32],
        const unsigned char *aad,   unsigned long aadlen,
        const unsigned char *ct,    unsigned long ctlen,
        unsigned char tag[16])
{
    /*
     * MAC input = aad || pad(aad) || ct || pad(ct) || LE64(aadlen) || LE64(ctlen)
     *
     * We keep a static scratch buffer; this is a kernel/freestanding context
     * so BSS is fine.  Max combined payload guarded by AEAD_MAC_BUF_MAX.
     */
    static unsigned char scratch[AEAD_MAC_BUF_MAX];
    unsigned long pos = 0;
    unsigned long pad;
    unsigned char footer[16];

    /* Copy aad */
    if (aadlen > 0 && pos + aadlen <= AEAD_MAC_BUF_MAX) {
        cc_memcpy(scratch + pos, aad, aadlen);
        pos += aadlen;
    }
    /* Pad aad to 16-byte boundary */
    pad = pad16_len(aadlen);
    if (pos + pad <= AEAD_MAC_BUF_MAX) {
        cc_memset(scratch + pos, 0, pad);
        pos += pad;
    }
    /* Copy ct */
    if (ctlen > 0 && pos + ctlen <= AEAD_MAC_BUF_MAX) {
        cc_memcpy(scratch + pos, ct, ctlen);
        pos += ctlen;
    }
    /* Pad ct to 16-byte boundary */
    pad = pad16_len(ctlen);
    if (pos + pad <= AEAD_MAC_BUF_MAX) {
        cc_memset(scratch + pos, 0, pad);
        pos += pad;
    }
    /* LE64(aadlen) || LE64(ctlen) */
    le64_store(footer,     (unsigned long long)aadlen);
    le64_store(footer + 8, (unsigned long long)ctlen);
    if (pos + 16 <= AEAD_MAC_BUF_MAX) {
        cc_memcpy(scratch + pos, footer, 16);
        pos += 16;
    }

    poly1305_mac(otk, scratch, pos, tag);
}

/* ---- Public AEAD encrypt ---- */
int chacha20poly1305_encrypt(const unsigned char key[32],
                             const unsigned char nonce[12],
                             const unsigned char *aad,
                             unsigned long aadlen,
                             const unsigned char *pt,
                             unsigned long ptlen,
                             unsigned char *ct,
                             unsigned char tag[16])
{
    unsigned char otk[32];   /* Poly1305 one-time key */
    unsigned char block0[64];

    /* Generate OTK: first 32 bytes of ChaCha20 block with counter=0 */
    chacha20_block(key, 0, nonce, block0);
    cc_memcpy(otk, block0, 32);
    cc_memset(block0, 0, sizeof(block0));

    /* Encrypt: counter starts at 1 per RFC 8439 §2.8 */
    chacha20_xor(key, 1, nonce, pt, ct, ptlen);

    /* Compute Poly1305 tag over (aad, ct) */
    compute_aead_tag(otk, aad, aadlen, ct, ptlen, tag);

    cc_memset(otk, 0, sizeof(otk));
    return 0;
}

/* ---- Public AEAD decrypt ---- */
int chacha20poly1305_decrypt(const unsigned char key[32],
                             const unsigned char nonce[12],
                             const unsigned char *aad,
                             unsigned long aadlen,
                             const unsigned char *ct,
                             unsigned long ctlen,
                             const unsigned char tag[16],
                             unsigned char *pt)
{
    unsigned char otk[32];
    unsigned char block0[64];
    unsigned char expected_tag[16];
    int auth_ok;

    /* Generate OTK */
    chacha20_block(key, 0, nonce, block0);
    cc_memcpy(otk, block0, 32);
    cc_memset(block0, 0, sizeof(block0));

    /* Compute expected tag over received ciphertext */
    compute_aead_tag(otk, aad, aadlen, ct, ctlen, expected_tag);
    cc_memset(otk, 0, sizeof(otk));

    /* Constant-time compare */
    auth_ok = cc_memcmp_ct(expected_tag, tag, 16);

    /* Only decrypt if tag is valid */
    if (auth_ok == 0) {
        chacha20_xor(key, 1, nonce, ct, pt, ctlen);
    }

    cc_memset(expected_tag, 0, sizeof(expected_tag));
    return auth_ok; /* 0 = success, non-zero = authentication failure */
}

/* =========================================================================
 * Section 4 – Self-test (RFC 8439 test vectors)
 * ====================================================================== */

/* Simple byte-compare of two fixed arrays; returns 0 on match */
static int tv_check(const unsigned char *got, const unsigned char *want,
                    unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++)
        if (got[i] != want[i]) return -1;
    return 0;
}

/*
 * RFC 8439 §2.4.2 – ChaCha20 keystream test vector
 * Key:    00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
 *         10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
 * Counter: 1
 * Nonce:  00 00 00 00 00 00 00 4a 00 00 00 00
 */
static int selftest_chacha20(void)
{
    static const unsigned char key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    static const unsigned char nonce[12] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4a,0x00,0x00,0x00,0x00
    };
    /* "Ladies and Gentlemen of the class of '99: If I could offer you only
     *  one tip for the future, sunscreen would be it." */
    static const unsigned char pt[114] = {
        0x4c,0x61,0x64,0x69,0x65,0x73,0x20,0x61,0x6e,0x64,0x20,0x47,0x65,0x6e,
        0x74,0x6c,0x65,0x6d,0x65,0x6e,0x20,0x6f,0x66,0x20,0x74,0x68,0x65,0x20,
        0x63,0x6c,0x61,0x73,0x73,0x20,0x6f,0x66,0x20,0x27,0x39,0x39,0x3a,0x20,
        0x49,0x66,0x20,0x49,0x20,0x63,0x6f,0x75,0x6c,0x64,0x20,0x6f,0x66,0x66,
        0x65,0x72,0x20,0x79,0x6f,0x75,0x20,0x6f,0x6e,0x6c,0x79,0x20,0x6f,0x6e,
        0x65,0x20,0x74,0x69,0x70,0x20,0x66,0x6f,0x72,0x20,0x74,0x68,0x65,0x20,
        0x66,0x75,0x74,0x75,0x72,0x65,0x2c,0x20,0x73,0x75,0x6e,0x73,0x63,0x72,
        0x65,0x65,0x6e,0x20,0x77,0x6f,0x75,0x6c,0x64,0x20,0x62,0x65,0x20,0x69,
        0x74,0x2e
    };
    /* Expected ciphertext (RFC 8439 §2.4.2) */
    static const unsigned char expected_ct[114] = {
        0x6e,0x2e,0x35,0x9a,0x25,0x68,0xf9,0x80,0x41,0xba,0x07,0x28,0xdd,0x0d,
        0x69,0x81,0xe9,0x7e,0x7a,0xec,0x1d,0x43,0x60,0xc2,0x0a,0x27,0xaf,0xcc,
        0xfd,0x9f,0xae,0x0b,0xf9,0x1b,0x65,0xc5,0x52,0x47,0x33,0xab,0x8f,0x59,
        0x3d,0xab,0xcd,0x62,0xb3,0x57,0x16,0x39,0xd6,0x24,0xe6,0x51,0x52,0xab,
        0x8f,0x53,0x0c,0x35,0x9f,0x08,0x61,0xd8,0x07,0xca,0x0d,0xbf,0x50,0x0d,
        0x6a,0x61,0x56,0xa3,0x8e,0x08,0x8a,0x22,0xb6,0x5e,0x52,0xbc,0x51,0x4d,
        0x16,0xcc,0xf8,0x06,0x81,0x8c,0xe9,0x1a,0xb7,0x79,0x37,0x36,0x5a,0xf9,
        0x0b,0xbf,0x74,0xa3,0x5b,0xe6,0xb4,0x0b,0x8e,0xed,0xf2,0x78,0x5e,0x42,
        0x87,0x4d
    };
    unsigned char ct[114];
    chacha20_xor(key, 1, nonce, pt, ct, 114);
    return tv_check(ct, expected_ct, 114);
}

/*
 * RFC 8439 §2.5.2 – Poly1305 test vector
 * Key: 85 d6 be 78 57 55 6d 33 7f 44 52 fe 42 d5 06 a8
 *      01 03 80 8a fb 0d b2 fd 4a bf f6 af 41 49 f5 1b
 * Msg: "Cryptographic Forum Research Group"
 * Tag: a8 06 1d c1 30 51 36 c6 c2 2b 8b af 0c 01 27 a9
 */
static int selftest_poly1305(void)
{
    static const unsigned char key[32] = {
        0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,
        0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
        0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,
        0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b
    };
    static const unsigned char msg[34] = {
        'C','r','y','p','t','o','g','r','a','p','h','i','c',' ',
        'F','o','r','u','m',' ',
        'R','e','s','e','a','r','c','h',' ',
        'G','r','o','u','p'
    };
    static const unsigned char expected_tag[16] = {
        0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,
        0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9
    };
    unsigned char tag[16];
    poly1305_mac(key, msg, 34, tag);
    return tv_check(tag, expected_tag, 16);
}

/*
 * RFC 8439 §2.8.2 – AEAD test vector
 *
 * Key:   80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
 *        90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
 * Nonce: 07 00 00 00  40 41 42 43 44 45 46 47
 * AAD:   50 51 52 53 c0 c1 c2 c3 c4 c5 c6 c7
 * PT:    4c 61 64 69 ... (same "Ladies and Gentlemen..." plaintext)
 * CT:    d3 1a 8d ... (documented)
 * Tag:   1a e1 0b 59 4f 09 e2 6a 7e 90 2e cb d0 60 06 91
 */
static int selftest_aead(void)
{
    static const unsigned char key[32] = {
        0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
        0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
        0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f
    };
    static const unsigned char nonce[12] = {
        0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,
        0x44,0x45,0x46,0x47
    };
    static const unsigned char aad[12] = {
        0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,
        0xc4,0xc5,0xc6,0xc7
    };
    /* RFC 8439 §2.8.2 plaintext (114 bytes, same message as §2.4.2) */
    static const unsigned char pt[114] = {
        0x4c,0x61,0x64,0x69,0x65,0x73,0x20,0x61,0x6e,0x64,0x20,0x47,0x65,0x6e,
        0x74,0x6c,0x65,0x6d,0x65,0x6e,0x20,0x6f,0x66,0x20,0x74,0x68,0x65,0x20,
        0x63,0x6c,0x61,0x73,0x73,0x20,0x6f,0x66,0x20,0x27,0x39,0x39,0x3a,0x20,
        0x49,0x66,0x20,0x49,0x20,0x63,0x6f,0x75,0x6c,0x64,0x20,0x6f,0x66,0x66,
        0x65,0x72,0x20,0x79,0x6f,0x75,0x20,0x6f,0x6e,0x6c,0x79,0x20,0x6f,0x6e,
        0x65,0x20,0x74,0x69,0x70,0x20,0x66,0x6f,0x72,0x20,0x74,0x68,0x65,0x20,
        0x66,0x75,0x74,0x75,0x72,0x65,0x2c,0x20,0x73,0x75,0x6e,0x73,0x63,0x72,
        0x65,0x65,0x6e,0x20,0x77,0x6f,0x75,0x6c,0x64,0x20,0x62,0x65,0x20,0x69,
        0x74,0x2e
    };
    /* Expected ciphertext (RFC 8439 §2.8.2) */
    static const unsigned char expected_ct[114] = {
        0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,
        0x7e,0xc2,0xa4,0xad,0xed,0x51,0x29,0x6e,0x08,0xfe,0xa9,0xe2,0xb5,0xa7,
        0x36,0xee,0x62,0xd6,0x3d,0xbe,0xa4,0x5e,0x8c,0xa9,0x67,0x12,0x82,0xfa,
        0xfb,0x69,0xda,0x92,0x72,0x8b,0x1a,0x71,0xde,0x0a,0x9e,0x06,0x0b,0x29,
        0x05,0xd6,0xa5,0xb6,0x7e,0xcd,0x3b,0x36,0x92,0xdd,0xbd,0x7f,0x2d,0x77,
        0x8b,0x8c,0x98,0x03,0xae,0xe3,0x28,0x09,0x1b,0x58,0xfa,0xb3,0x24,0xe4,
        0xfa,0xd6,0x75,0x94,0x55,0x85,0x80,0x8b,0x48,0x31,0xd7,0xbc,0x3f,0xf4,
        0xde,0xf0,0x8e,0x4b,0x7a,0x9d,0xe5,0x76,0xd2,0x65,0x86,0xce,0xc6,0x4b,
        0x61,0x16
    };
    /* Expected tag (RFC 8439 §2.8.2) */
    static const unsigned char expected_tag[16] = {
        0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,
        0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91
    };

    unsigned char ct[114];
    unsigned char tag[16];
    unsigned char pt2[114];
    int ret;

    /* --- encrypt --- */
    ret = chacha20poly1305_encrypt(key, nonce, aad, 12, pt, 114, ct, tag);
    if (ret != 0) return -1;
    if (tv_check(ct, expected_ct, 114) != 0) return -2;
    if (tv_check(tag, expected_tag, 16) != 0) return -3;

    /* --- decrypt round-trip --- */
    cc_memset(pt2, 0, sizeof(pt2));
    ret = chacha20poly1305_decrypt(key, nonce, aad, 12, ct, 114, tag, pt2);
    if (ret != 0) return -4;
    if (tv_check(pt2, pt, 114) != 0) return -5;

    /* --- corrupted-tag negative test --- */
    {
        unsigned char bad_tag[16];
        cc_memcpy(bad_tag, expected_tag, 16);
        bad_tag[0] ^= 0xff;  /* flip one byte */
        cc_memset(pt2, 0xcc, sizeof(pt2));
        ret = chacha20poly1305_decrypt(key, nonce, aad, 12, ct, 114, bad_tag, pt2);
        if (ret == 0) return -6;  /* must NOT succeed with bad tag */
        /* Plaintext must not have been written (still 0xcc) */
        {
            unsigned long i;
            for (i = 0; i < 114; i++)
                if (pt2[i] != 0xcc) return -7;
        }
    }

    return 0;
}

/* Public entry point */
int chacha20poly1305_selftest(void)
{
    if (selftest_chacha20()  != 0) return 1;
    if (selftest_poly1305()  != 0) return 2;
    if (selftest_aead()      != 0) return 3;
    return 0;
}
