/*
 * rsa.c -- freestanding RSA public-key operations (PKCS#1 v1.5).
 * =============================================================
 *
 * See rsa.h. Built entirely on bignum.c. No libc, no syscalls, no malloc;
 * everything lives in fixed-size stack buffers, capped by the modulus byte
 * length (<= BN_WORDS*4 == 512 bytes for a 4096-bit key).
 */

#include "rsa.h"
#include "bignum.h"

/* Largest modulus we serialise/deserialise, in bytes (4096-bit). */
#define RSA_MAX_BYTES (BN_WORDS * 4)

/* --------------------------------------------------------------------- *
 *  Private memory primitives (no libc).                                   *
 * --------------------------------------------------------------------- */

static void rsa_memset(void *dst, int val, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)val;
}

static void rsa_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

/* Constant-ish memcmp: returns 0 iff equal. (Not a security boundary here;
 * the data compared is the recovered hash of public material.) */
static int rsa_memcmp(const unsigned char *a, const unsigned char *b,
                      unsigned long n)
{
    unsigned diff = 0;
    while (n--) diff |= (unsigned)(*a++ ^ *b++);
    return diff != 0;
}

/* --------------------------------------------------------------------- *
 *  DigestInfo DER prefixes (RFC 8017 sec 9.2 note 1).                     *
 *                                                                         *
 *  EMSA-PKCS1-v1_5 DigestInfo = <prefix> || <raw hash>.                   *
 * --------------------------------------------------------------------- */

/* SHA-256: 19-byte prefix, 32-byte hash. */
static const unsigned char DIGINFO_SHA256[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

/* SHA-1: 15-byte prefix, 20-byte hash. */
static const unsigned char DIGINFO_SHA1[] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
    0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14
};

/* SHA-384: 19-byte prefix, 48-byte hash (RFC 8017 sec 9.2 note 1).
 *   SEQUENCE(65) { SEQUENCE(13) { OID id-sha384, NULL }, OCTET STRING(48) }
 *   id-sha384 = 2.16.840.1.101.3.4.2.2  (60 86 48 01 65 03 04 02 02) */
static const unsigned char DIGINFO_SHA384[] = {
    0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05,
    0x00, 0x04, 0x30
};

/* SHA-512: 19-byte prefix, 64-byte hash (RFC 8017 sec 9.2 note 1).
 *   SEQUENCE(81) { SEQUENCE(13) { OID id-sha512, NULL }, OCTET STRING(64) }
 *   id-sha512 = 2.16.840.1.101.3.4.2.3  (60 86 48 01 65 03 04 02 03) */
static const unsigned char DIGINFO_SHA512[] = {
    0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05,
    0x00, 0x04, 0x40
};

/* --------------------------------------------------------------------- *
 *  Public key construction.                                              *
 * --------------------------------------------------------------------- */

void rsa_pubkey_from_bytes(rsa_pubkey *pk,
                           const unsigned char *mod_be, unsigned long mod_len,
                           const unsigned char *exp_be, unsigned long exp_len)
{
    bn_from_bytes(&pk->n, mod_be, mod_len);
    bn_from_bytes(&pk->e, exp_be, exp_len);
}

/* Modulus length in bytes (number of bytes needed to hold n, i.e. its
 * big-endian length). For RSA this is the key size, e.g. 256 for 2048-bit. */
static unsigned long rsa_modulus_bytes(const rsa_pubkey *pk)
{
    int bits = bn_bit_length(&pk->n);
    return (unsigned long)((bits + 7) / 8);
}

/* --------------------------------------------------------------------- *
 *  Core: raw public-key operation  m = c^e mod n.                        *
 * --------------------------------------------------------------------- */

static void rsa_pub_op(const rsa_pubkey *pk, const bignum *in, bignum *out)
{
    bn_mod_exp(out, in, &pk->e, &pk->n);
}

/* --------------------------------------------------------------------- *
 *  RSAES-PKCS1-v1_5 encryption.                                          *
 * --------------------------------------------------------------------- */

int rsa_pkcs1_encrypt(const rsa_pubkey *pk,
                      const unsigned char *msg, unsigned long msg_len,
                      const unsigned char *rnd, unsigned long rnd_len,
                      unsigned char *out, unsigned long out_len)
{
    unsigned long k = rsa_modulus_bytes(pk);

    if (k < 11 || k > RSA_MAX_BYTES) return 1;     /* bad / huge modulus */
    if (out_len != k)               return 2;      /* caller must size out=k */
    /* PKCS#1 v1.5 type 2: EM = 00 02 PS 00 M, |PS| >= 8. So |M| <= k - 11. */
    if (msg_len + 11 > k)           return 3;      /* message too long */

    unsigned long ps_len = k - msg_len - 3;        /* >= 8 by the check above */

    unsigned char em[RSA_MAX_BYTES];
    em[0] = 0x00;
    em[1] = 0x02;

    /* Fill PS with nonzero random bytes drawn from the caller's pool. We skip
     * any zero bytes in the pool (PS must be all nonzero). If the pool runs
     * out before PS is full, fail rather than weaken the padding. */
    unsigned long ri = 0;       /* index into rnd */
    for (unsigned long i = 0; i < ps_len; i++) {
        unsigned char b = 0;
        while (ri < rnd_len) {
            unsigned char cand = rnd[ri++];
            if (cand != 0) { b = cand; break; }
        }
        if (b == 0) {
            rsa_memset(em, 0, sizeof(em));         /* scrub partial padding */
            return 4;                              /* not enough randomness */
        }
        em[2 + i] = b;
    }

    em[2 + ps_len] = 0x00;
    rsa_memcpy(em + 2 + ps_len + 1, msg, msg_len);

    /* c = EM^e mod n. */
    bignum m, c;
    bn_from_bytes(&m, em, k);
    rsa_pub_op(pk, &m, &c);
    bn_to_bytes(&c, out, k);

    rsa_memset(em, 0, sizeof(em));                 /* scrub */
    return 0;
}

/* --------------------------------------------------------------------- *
 *  RSASSA-PKCS1-v1_5 verification.                                       *
 * --------------------------------------------------------------------- */

/* Map a hash_alg selector to its DigestInfo prefix and digest length.
 * Returns 0 on success, nonzero for an unknown selector. */
static int pkcs1_digestinfo(int hash_alg, const unsigned char **prefix,
                            unsigned long *prefix_len, unsigned long *hlen)
{
    if (hash_alg == RSA_HASH_SHA256) {
        *prefix = DIGINFO_SHA256; *prefix_len = sizeof(DIGINFO_SHA256);
        *hlen = 32; return 0;
    }
    if (hash_alg == RSA_HASH_SHA1) {
        *prefix = DIGINFO_SHA1;   *prefix_len = sizeof(DIGINFO_SHA1);
        *hlen = 20; return 0;
    }
    if (hash_alg == RSA_HASH_SHA384) {
        *prefix = DIGINFO_SHA384; *prefix_len = sizeof(DIGINFO_SHA384);
        *hlen = 48; return 0;
    }
    if (hash_alg == RSA_HASH_SHA512) {
        *prefix = DIGINFO_SHA512; *prefix_len = sizeof(DIGINFO_SHA512);
        *hlen = 64; return 0;
    }
    return 1;
}

/*
 * Validate an EMSA-PKCS1-v1_5 encoded message em[0..k) against the expected
 * hash. Layout (RFC 8017 sec 9.2):
 *     EM = 0x00 || 0x01 || PS(0xFF...) || 0x00 || DigestInfo || hash
 * with |PS| >= 8. Returns 0 if the structure and embedded hash match,
 * nonzero (a distinct code per failure point) otherwise. This is the byte
 * checker shared by rsa_pkcs1_verify and rsa_selftest.
 */
static int pkcs1_check_em(const unsigned char *em, unsigned long k,
                          const unsigned char *prefix, unsigned long prefix_len,
                          const unsigned char *hash, unsigned long hash_len)
{
    unsigned long tlen = prefix_len + hash_len;
    if (tlen + 11 > k) return 6;                   /* no room for valid EM */

    unsigned long pslen = k - 3 - tlen;
    if (pslen < 8) return 7;

    if (em[0] != 0x00) return 8;
    if (em[1] != 0x01) return 9;

    unsigned long idx = 2;
    for (unsigned long i = 0; i < pslen; i++, idx++) {
        if (em[idx] != 0xFF) return 10;
    }

    if (em[idx] != 0x00) return 11;                /* separator */
    idx++;

    if (rsa_memcmp(em + idx, prefix, prefix_len) != 0) return 12;
    idx += prefix_len;

    if (rsa_memcmp(em + idx, hash, hash_len) != 0) return 13;
    idx += hash_len;

    if (idx != k) return 14;                        /* must consume all k */

    return 0;
}

int rsa_pkcs1_verify(const rsa_pubkey *pk,
                     const unsigned char *sig, unsigned long sig_len,
                     const unsigned char *hash, unsigned long hash_len,
                     int hash_alg)
{
    unsigned long k = rsa_modulus_bytes(pk);
    if (k < 11 || k > RSA_MAX_BYTES) return 1;
    if (sig_len != k)               return 2;      /* sig must be modulus-len */

    const unsigned char *prefix;
    unsigned long        prefix_len;
    unsigned long        expect_hlen;
    if (pkcs1_digestinfo(hash_alg, &prefix, &prefix_len, &expect_hlen) != 0)
        return 3;                                  /* unknown hash alg */
    if (hash_len != expect_hlen) return 4;         /* wrong digest size */

    /* m = sig^e mod n. */
    bignum s, m;
    bn_from_bytes(&s, sig, k);

    /* Reject sig >= n (RFC 8017: signature representative out of range). */
    if (bn_cmp(&s, &pk->n) >= 0) return 5;

    rsa_pub_op(pk, &s, &m);

    /* Serialise the recovered EM, big-endian, exactly k bytes, then check. */
    unsigned char em[RSA_MAX_BYTES];
    bn_to_bytes(&m, em, k);

    return pkcs1_check_em(em, k, prefix, prefix_len, hash, hash_len);
}

/* --------------------------------------------------------------------- *
 *  Self-test.                                                            *
 * --------------------------------------------------------------------- */

/*
 * Known-answer test using a tiny RSA key.
 *
 *   p = 61, q = 53  ->  n = 3233,  phi = 3120
 *   e = 17,  d = 2753   (17 * 2753 = 46801 = 15*3120 + 1, so e*d = 1 mod phi)
 *
 * This is the textbook RSA example. We verify:
 *   1. bn_mod_exp round-trips:  (m^e mod n)^d mod n == m  for several m.
 *   2. The known ciphertext for m=65 is 2790 (the canonical worked example).
 *   3. A larger pseudo-2048-ish exponentiation sanity check (m^1 == m).
 *   4. The PKCS#1 verify EM-structure checker accepts a hand-built valid EM
 *      and rejects a tampered one, using a real-size (here small) modulus.
 */
int rsa_selftest(void)
{
    bignum n, e, d, m, c, r;

    bn_set_u32(&n, 3233);
    bn_set_u32(&e, 17);
    bn_set_u32(&d, 2753);

    /* 1. Canonical example: 65^17 mod 3233 == 2790. */
    bn_set_u32(&m, 65);
    bn_mod_exp(&c, &m, &e, &n);
    {
        bignum want;
        bn_set_u32(&want, 2790);
        if (bn_cmp(&c, &want) != 0) return 1;
    }
    /* ...and 2790^2753 mod 3233 == 65 (decrypt back). */
    bn_mod_exp(&r, &c, &d, &n);
    if (bn_cmp(&r, &m) != 0) return 2;

    /* 2. Round-trip a spread of messages. */
    for (unsigned v = 2; v < 3233; v += 257) {
        bn_set_u32(&m, v);
        bn_mod_exp(&c, &m, &e, &n);
        bn_mod_exp(&r, &c, &d, &n);
        if (bn_cmp(&r, &m) != 0) return 3;
    }

    /* 3. m^1 mod n == m mod n; m^0 mod n == 1. */
    {
        bignum one, zero;
        bn_set_u32(&one, 1);
        bn_set_zero(&zero);
        bn_set_u32(&m, 1234);
        bn_mod_exp(&r, &m, &one, &n);
        {
            bignum mm; bn_copy(&mm, &m);  /* m < n already */
            if (bn_cmp(&r, &mm) != 0) return 4;
        }
        bn_mod_exp(&r, &m, &zero, &n);
        if (bn_cmp(&r, &one) != 0) return 5;
    }

    /* 4. Sanity-check the DigestInfo prefix tables. */
    if (sizeof(DIGINFO_SHA256) != 19) return 6;
    if (sizeof(DIGINFO_SHA1)   != 15) return 7;
    if (sizeof(DIGINFO_SHA384) != 19) return 15;
    if (sizeof(DIGINFO_SHA512) != 19) return 16;

    /*
     * 5. Exercise the EMSA-PKCS1-v1_5 byte parser shared by the real verify
     *    path. Build a well-formed EM for a 2048-bit modulus (k = 256) over a
     *    known SHA-256 digest, confirm pkcs1_check_em accepts it, then corrupt
     *    each significant field in turn and confirm it is rejected. This
     *    validates the exact structure rsa_pkcs1_verify enforces after the
     *    modexp, without needing a private key to produce a signature.
     */
    {
        const unsigned long k = 256;               /* 2048-bit modulus bytes */
        unsigned char       digest[32];
        unsigned char       em[256];

        for (int i = 0; i < 32; i++) digest[i] = (unsigned char)(i * 7 + 1);

        unsigned long plen = sizeof(DIGINFO_SHA256);   /* 19 */
        unsigned long tlen = plen + 32;                 /* DigestInfo + hash */
        unsigned long ps   = k - 3 - tlen;              /* 0xFF run length */

        /* Assemble EM = 00 01 FF..FF 00 prefix hash. */
        unsigned long idx = 0;
        em[idx++] = 0x00;
        em[idx++] = 0x01;
        for (unsigned long i = 0; i < ps; i++) em[idx++] = 0xFF;
        em[idx++] = 0x00;
        for (unsigned long i = 0; i < plen; i++) em[idx++] = DIGINFO_SHA256[i];
        for (int i = 0; i < 32; i++) em[idx++] = digest[i];
        if (idx != k) return 8;                          /* assembly bug */

        /* Must accept the well-formed EM. */
        if (pkcs1_check_em(em, k, DIGINFO_SHA256, plen, digest, 32) != 0)
            return 9;

        /* Corrupt the leading 0x00 -> reject. */
        em[0] = 0x11;
        if (pkcs1_check_em(em, k, DIGINFO_SHA256, plen, digest, 32) == 0)
            return 10;
        em[0] = 0x00;

        /* Corrupt the 0x01 type byte -> reject. */
        em[1] = 0x02;
        if (pkcs1_check_em(em, k, DIGINFO_SHA256, plen, digest, 32) == 0)
            return 11;
        em[1] = 0x01;

        /* Corrupt a padding byte -> reject. */
        em[5] = 0xFE;
        if (pkcs1_check_em(em, k, DIGINFO_SHA256, plen, digest, 32) == 0)
            return 12;
        em[5] = 0xFF;

        /* Corrupt the embedded hash -> reject. */
        em[k - 1] ^= 0xFF;
        if (pkcs1_check_em(em, k, DIGINFO_SHA256, plen, digest, 32) == 0)
            return 13;
        em[k - 1] ^= 0xFF;

        /* Final re-accept to prove the restores were exact. */
        if (pkcs1_check_em(em, k, DIGINFO_SHA256, plen, digest, 32) != 0)
            return 14;
    }

    /*
     * 6. Exercise pkcs1_check_em with a SHA-384 DigestInfo EM (k = 256,
     *    48-byte digest). Validates prefix selection for RSA_HASH_SHA384.
     */
    {
        const unsigned long k = 256;
        unsigned char       digest384[48];
        unsigned char       em384[256];
        unsigned long plen384 = sizeof(DIGINFO_SHA384);   /* 19 */
        unsigned long tlen384 = plen384 + 48;
        unsigned long ps384   = k - 3 - tlen384;
        unsigned long idx = 0;

        for (int i = 0; i < 48; i++) digest384[i] = (unsigned char)(i * 5 + 3);

        em384[idx++] = 0x00;
        em384[idx++] = 0x01;
        for (unsigned long i = 0; i < ps384; i++) em384[idx++] = 0xFF;
        em384[idx++] = 0x00;
        for (unsigned long i = 0; i < plen384; i++) em384[idx++] = DIGINFO_SHA384[i];
        for (int i = 0; i < 48; i++) em384[idx++] = digest384[i];
        if (idx != k) return 17;                          /* assembly bug */

        /* Well-formed EM with SHA-384 prefix must be accepted. */
        if (pkcs1_check_em(em384, k, DIGINFO_SHA384, plen384, digest384, 48) != 0)
            return 18;

        /* Corrupt the hash tail -> must be rejected. */
        em384[k - 1] ^= 0xFF;
        if (pkcs1_check_em(em384, k, DIGINFO_SHA384, plen384, digest384, 48) == 0)
            return 19;
        em384[k - 1] ^= 0xFF;

        /* pkcs1_digestinfo must map RSA_HASH_SHA384 to prefix + hlen=48. */
        {
            const unsigned char *p384;
            unsigned long plen_out, hlen_out;
            if (pkcs1_digestinfo(RSA_HASH_SHA384, &p384, &plen_out, &hlen_out) != 0)
                return 20;
            if (plen_out != 19 || hlen_out != 48) return 21;
        }

        /* pkcs1_digestinfo must map RSA_HASH_SHA512 to prefix + hlen=64. */
        {
            const unsigned char *p512;
            unsigned long plen_out, hlen_out;
            if (pkcs1_digestinfo(RSA_HASH_SHA512, &p512, &plen_out, &hlen_out) != 0)
                return 22;
            if (plen_out != 19 || hlen_out != 64) return 23;
        }

        /* Unknown hash_alg must be rejected. */
        {
            const unsigned char *px;
            unsigned long plen_out, hlen_out;
            if (pkcs1_digestinfo(99, &px, &plen_out, &hlen_out) == 0)
                return 24;
        }
    }

    return 0;
}
