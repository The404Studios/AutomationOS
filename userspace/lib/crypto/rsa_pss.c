/*
 * rsa_pss.c -- freestanding RSASSA-PSS signature VERIFICATION.
 * ===========================================================
 *
 * Implements RSASSA-PSS-VERIFY (RFC 8017 sec 8.1.2) = RSAVP1 + EMSA-PSS-VERIFY
 * (sec 9.1.2), layered on the existing project RSA public-key operation
 * (rsa_pubkey + bignum modexp) and the freestanding SHA-256 / SHA-384.
 *
 * Pure computation: NO libc, NO syscalls, NO malloc, NO standard headers.
 * Everything lives in fixed-size stack buffers bounded by the modulus byte
 * length (<= 512 bytes for a 4096-bit key).
 *
 * Covers the two mandatory TLS 1.3 schemes, with salt length == digest length
 * (matching OpenSSL rsa_pss_saltlen:-1):
 *
 *   RSA_PSS_SHA256   rsa_pss_rsae_sha256   (hLen = sLen = 32)
 *   RSA_PSS_SHA384   rsa_pss_rsae_sha384   (hLen = sLen = 48)
 *
 * The caller passes the message hash mHash = Hash(M) (TLS already has the
 * transcript/handshake hash on hand). Returns 0 iff the signature is valid;
 * a distinct nonzero code per failure point otherwise. Inputs are public
 * (signature + hash off the wire), so this is not written to be constant-time.
 */

#include "rsa_pss.h"
#include "rsa.h"
#include "bignum.h"
#include "sha256.h"
#include "sha512.h"   /* sha384 lives here */

/* Largest modulus we serialise, in bytes (4096-bit). */
#define PSS_MAX_BYTES (BN_WORDS * 4)
/* Largest digest we handle here (SHA-384 = 48; round up to 64). */
#define PSS_MAX_HLEN  64

/* --------------------------------------------------------------------- *
 *  Private memory primitives (no libc).                                   *
 * --------------------------------------------------------------------- */

static void pss_memset(void *dst, int val, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)val;
}

/* Returns 0 iff the n compared bytes are equal. */
static int pss_memcmp(const unsigned char *a, const unsigned char *b,
                      unsigned long n)
{
    unsigned diff = 0;
    while (n--) diff |= (unsigned)(*a++ ^ *b++);
    return diff != 0;
}

/* --------------------------------------------------------------------- *
 *  Tiny hash abstraction so MGF1 + the EMSA-PSS body are written once.    *
 *  alg: RSA_PSS_SHA256 (0) = SHA-256, RSA_PSS_SHA384 (1) = SHA-384.       *
 * --------------------------------------------------------------------- */

/* Digest length for the selected algorithm. */
static unsigned long pss_hlen(int alg)
{
    return (alg == RSA_PSS_SHA256) ? 32UL : 48UL;
}

/* Streaming hash of two concatenated buffers (seed || counter in MGF1). */
static void pss_hash2(int alg,
                      const unsigned char *a, unsigned long alen,
                      const unsigned char *b, unsigned long blen,
                      unsigned char *out)
{
    if (alg == RSA_PSS_SHA256) {
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, a, alen);
        sha256_update(&c, b, blen);
        sha256_final(&c, out);
    } else {
        sha384_ctx c;
        sha384_init(&c);
        sha384_update(&c, a, alen);
        sha384_update(&c, b, blen);
        sha384_final(&c, out);
    }
}

/* Streaming hash of three concatenated buffers. M' = (8 x 0x00) || mHash ||
 * salt is hashed directly so we never build M' in a scratch array. */
static void pss_hash3(int alg,
                      const unsigned char *a, unsigned long alen,
                      const unsigned char *b, unsigned long blen,
                      const unsigned char *c3, unsigned long clen,
                      unsigned char *out)
{
    if (alg == RSA_PSS_SHA256) {
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, a, alen);
        sha256_update(&c, b, blen);
        sha256_update(&c, c3, clen);
        sha256_final(&c, out);
    } else {
        sha384_ctx c;
        sha384_init(&c);
        sha384_update(&c, a, alen);
        sha384_update(&c, b, blen);
        sha384_update(&c, c3, clen);
        sha384_final(&c, out);
    }
}

/* --------------------------------------------------------------------- *
 *  MGF1 (RFC 8017 appendix B.2.1).                                        *
 *                                                                         *
 *  mask = T[0..maskLen) where T = Hash(seed || I2OSP(0,4))               *
 *                                  || Hash(seed || I2OSP(1,4)) || ...     *
 *                                                                         *
 *  The generated mask is XORed directly into `inout` (the DB buffer),     *
 *  block by block, so no maskLen-sized scratch buffer is needed.          *
 * --------------------------------------------------------------------- */

static void mgf1_xor(int alg, const unsigned char *seed, unsigned long seedLen,
                     unsigned char *inout, unsigned long maskLen)
{
    unsigned long hLen = pss_hlen(alg);
    unsigned char block[PSS_MAX_HLEN];
    unsigned char cbuf[4];
    unsigned long counter = 0;
    unsigned long off = 0;

    while (off < maskLen) {
        /* I2OSP(counter, 4): big-endian 4-byte counter. */
        cbuf[0] = (unsigned char)(counter >> 24);
        cbuf[1] = (unsigned char)(counter >> 16);
        cbuf[2] = (unsigned char)(counter >> 8);
        cbuf[3] = (unsigned char)(counter);

        pss_hash2(alg, seed, seedLen, cbuf, 4, block);

        unsigned long take = hLen;
        if (off + take > maskLen) take = maskLen - off;
        for (unsigned long i = 0; i < take; i++)
            inout[off + i] ^= block[i];

        off += take;
        counter++;
    }
}

/* --------------------------------------------------------------------- *
 *  RSASSA-PSS-VERIFY  (RFC 8017 sec 8.1.2 + 9.1.2).                       *
 * --------------------------------------------------------------------- */

int rsa_pss_verify(const rsa_pubkey *pk,
                   const unsigned char *sig, unsigned long sig_len,
                   const unsigned char *mhash, unsigned long mhash_len,
                   int hash_alg)
{
    /* Resolve the hash and its digest length; reject unknown selectors. */
    if (hash_alg != RSA_PSS_SHA256 && hash_alg != RSA_PSS_SHA384)
        return 1;
    unsigned long hLen = pss_hlen(hash_alg);
    unsigned long sLen = hLen;                       /* salt len == digest len */

    /* mHash must be exactly the digest length. */
    if (mhash_len != hLen)                 return 2;

    /* k = modulus byte length; modBits = exact bit length of n. */
    int modBits = bn_bit_length(&pk->n);
    unsigned long k = (unsigned long)((modBits + 7) / 8);
    if (k < 1 || k > PSS_MAX_BYTES)        return 3;

    /* Signature length must equal the modulus byte length (RSAVP1 input). */
    if (sig_len != k)                      return 4;

    /* emBits = modBits - 1 ; emLen = ceil(emBits / 8). (RFC 8017 9.1.2.) */
    unsigned long emBits = (unsigned long)(modBits - 1);
    unsigned long emLen  = (emBits + 7) / 8;

    /* Step 3: if emLen < hLen + sLen + 2, the encoding is inconsistent. */
    if (emLen < hLen + sLen + 2)           return 5;

    /* ---- RSAVP1: s -> m = s^e mod n -------------------------------- */
    bignum s, m;
    bn_from_bytes(&s, sig, sig_len);
    /* Signature representative must be < n (RFC 8017 5.2.2 step 1). */
    if (bn_cmp(&s, &pk->n) >= 0)           return 6;

    bn_mod_exp(&m, &s, &pk->e, &pk->n);

    /* I2OSP(m, emLen): big-endian, exactly emLen bytes. bn_to_bytes
     * left-pads with zeros / truncates high bytes to the requested width.
     * A PSS-valid EM has its leftmost (8*emLen - emBits) bits zero, so it
     * always fits in emLen bytes. */
    unsigned char EM[PSS_MAX_BYTES];
    bn_to_bytes(&m, EM, emLen);

    /* ---- EMSA-PSS-VERIFY (RFC 8017 9.1.2) -------------------------- */

    /* Step 4: rightmost byte of EM must be 0xbc. */
    if (EM[emLen - 1] != 0xbc)             return 7;

    /* Step 5: split EM = maskedDB (emLen-hLen-1) || H (hLen) || 0xbc. */
    unsigned long dbLen = emLen - hLen - 1;
    const unsigned char *maskedDB = EM;
    const unsigned char *H        = EM + dbLen;

    /* Step 6: the leftmost (8*emLen - emBits) bits of maskedDB[0] must be 0. */
    unsigned long zbits = 8 * emLen - emBits;       /* in [0, 8) */
    if (zbits) {
        unsigned char topmask = (unsigned char)(0xFF << (8 - zbits));
        if (maskedDB[0] & topmask)         return 8;
    }

    /* Steps 7-8: DB = maskedDB XOR MGF1(H, dbLen). Work in a local buffer. */
    unsigned char DB[PSS_MAX_BYTES];
    for (unsigned long i = 0; i < dbLen; i++) DB[i] = maskedDB[i];
    mgf1_xor(hash_alg, H, hLen, DB, dbLen);

    /* Step 9: clear the same leftmost bits in DB[0]. */
    if (zbits) {
        unsigned char keep = (unsigned char)(0xFF >> zbits);
        DB[0] &= keep;
    }

    /* Step 10: DB = PS (zeros) || 0x01 || salt, PS having dbLen-sLen-1 bytes.
     * Verify the leading zero run and the 0x01 octet. */
    unsigned long psLen = dbLen - sLen - 1;
    for (unsigned long i = 0; i < psLen; i++)
        if (DB[i] != 0x00)                 return 9;
    if (DB[psLen] != 0x01)                 return 10;

    /* Step 11: salt = DB[dbLen - sLen .. dbLen). */
    const unsigned char *salt = DB + (dbLen - sLen);

    /* Step 12-13: M' = (0x00 x 8) || mHash || salt ; H' = Hash(M').
     * Hashed in one streaming pass (no scratch buffer for M'). */
    static const unsigned char eight_zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    unsigned char Hverify[PSS_MAX_HLEN];
    pss_hash3(hash_alg, eight_zeros, 8, mhash, hLen, salt, sLen, Hverify);

    /* Step 14: accept iff H' == H. */
    int rc = pss_memcmp(Hverify, H, hLen) == 0 ? 0 : 11;

    /* Scrub working buffers. */
    pss_memset(EM, 0, sizeof(EM));
    pss_memset(DB, 0, sizeof(DB));
    return rc;
}
