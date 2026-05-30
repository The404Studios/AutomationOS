/*
 * hkdf.c -- freestanding HKDF (RFC 5869) + TLS 1.3 Expand-Label.
 * ================================================================
 *
 * No libc, no syscalls, no malloc, no standard headers.
 * Fixed stack buffers only.
 *
 * Implements:
 *   - HKDF-Extract / HKDF-Expand for SHA-256 (HashLen = 32, block = 64)
 *   - HKDF-Extract / HKDF-Expand for SHA-384 (HashLen = 48, block = 128)
 *   - HMAC-SHA-384 (internal; hmac.h only ships hmac_sha256)
 *   - TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1)
 *   - Self-test: RFC 5869 Appendix A cases 1 and 3 (SHA-256)
 *
 * Build flags:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#include "hkdf.h"
#include "hmac.h"      /* hmac_sha256 */
#include "sha512.h"    /* sha384_ctx, sha384_init/update/final */

/* =========================================================================
 * Utility helpers (no libc)
 * ====================================================================== */

static void hkdf_memset(void *dst, int val, unsigned long n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)val;
}

static void hkdf_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static unsigned long hkdf_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static int hkdf_memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

/* =========================================================================
 * HMAC-SHA-384 (internal)
 * -----------------------------------------------------------------------
 * SHA-384 is a truncated SHA-512; its HMAC block size (B) is 128 bytes
 * (same as SHA-512), output length (L) is 48 bytes.
 *
 * RFC 2104:
 *   key' = H(key)          if len(key) > B
 *         key || 0x00*pad  otherwise
 *   HMAC = H( key'^opad || H( key'^ipad || msg ) )
 * ====================================================================== */

#define HMAC384_BLOCK  128   /* SHA-384/512 block size in bytes */
#define HMAC384_DIGEST  48   /* SHA-384 output length in bytes  */

static void hk_hmac_sha384(const unsigned char *key, unsigned long klen,
                         const unsigned char *msg, unsigned long mlen,
                         unsigned char out[48])
{
    unsigned char k[HMAC384_BLOCK];
    unsigned char ipad[HMAC384_BLOCK];
    unsigned char opad[HMAC384_BLOCK];
    unsigned char inner[HMAC384_DIGEST];
    sha384_ctx ctx;
    int i;

    /* Prepare key block */
    hkdf_memset(k, 0, HMAC384_BLOCK);
    if (klen > HMAC384_BLOCK) {
        /* Hash long key down to 48 bytes */
        sha384((const void *)key, klen, k);
    } else {
        hkdf_memcpy(k, key, klen);
    }

    /* Build ipad / opad */
    for (i = 0; i < HMAC384_BLOCK; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    /* Inner: H(ipad || msg) */
    sha384_init(&ctx);
    sha384_update(&ctx, ipad, HMAC384_BLOCK);
    sha384_update(&ctx, msg, mlen);
    sha384_final(&ctx, inner);

    /* Outer: H(opad || inner) */
    sha384_init(&ctx);
    sha384_update(&ctx, opad, HMAC384_BLOCK);
    sha384_update(&ctx, inner, HMAC384_DIGEST);
    sha384_final(&ctx, out);

    /* Scrub sensitive data */
    hkdf_memset(k,     0, HMAC384_BLOCK);
    hkdf_memset(ipad,  0, HMAC384_BLOCK);
    hkdf_memset(opad,  0, HMAC384_BLOCK);
    hkdf_memset(inner, 0, HMAC384_DIGEST);
}

/* =========================================================================
 * HMAC-SHA-384 over two-part message: H(key, part1 || part2)
 * Used during HKDF-Expand to avoid assembling T(i-1)||info||counter
 * into a single contiguous buffer.
 * ====================================================================== */
static void hmac_sha384_2(const unsigned char *key, unsigned long klen,
                           const unsigned char *p1,  unsigned long p1len,
                           const unsigned char *p2,  unsigned long p2len,
                           unsigned char out[48])
{
    unsigned char k[HMAC384_BLOCK];
    unsigned char ipad[HMAC384_BLOCK];
    unsigned char opad[HMAC384_BLOCK];
    unsigned char inner[HMAC384_DIGEST];
    sha384_ctx ctx;
    int i;

    hkdf_memset(k, 0, HMAC384_BLOCK);
    if (klen > HMAC384_BLOCK) {
        sha384((const void *)key, klen, k);
    } else {
        hkdf_memcpy(k, key, klen);
    }

    for (i = 0; i < HMAC384_BLOCK; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    sha384_init(&ctx);
    sha384_update(&ctx, ipad, HMAC384_BLOCK);
    if (p1len) sha384_update(&ctx, p1, p1len);
    if (p2len) sha384_update(&ctx, p2, p2len);
    sha384_final(&ctx, inner);

    sha384_init(&ctx);
    sha384_update(&ctx, opad, HMAC384_BLOCK);
    sha384_update(&ctx, inner, HMAC384_DIGEST);
    sha384_final(&ctx, out);

    hkdf_memset(k,     0, HMAC384_BLOCK);
    hkdf_memset(ipad,  0, HMAC384_BLOCK);
    hkdf_memset(opad,  0, HMAC384_BLOCK);
    hkdf_memset(inner, 0, HMAC384_DIGEST);
}

/* =========================================================================
 * HMAC-SHA-256 over two-part message: H(key, p1 || p2)
 * -----------------------------------------------------------------------
 * p1 is T(i-1) (0 or 32 bytes), p2 is info||counter (infolen+1 bytes).
 * Rather than carry a streaming sha256 context here, we assemble into a
 * fixed 8193-byte stack buffer and forward to the existing hmac_sha256().
 * hkdf_expand_sha256 already caps infolen <= 8192, so p1+p2 <= 8225 but
 * in practice info fits easily.  We mirror the 8192 guard for safety.
 * ====================================================================== */
#define HMAC256_DIGEST  32

static void hmac_sha256_2(const unsigned char *key, unsigned long klen,
                           const unsigned char *p1,  unsigned long p1len,
                           const unsigned char *p2,  unsigned long p2len,
                           unsigned char out[32])
{
    /*
     * Max: p1 = 32 bytes (T(i-1)), p2 = infolen+1 <= 8193 bytes.
     * Total <= 8225.  Use 8225-byte buffer; guarded below.
     */
    unsigned char tmp[32 + 8192 + 1]; /* T(i-1) + info + counter */
    unsigned long tlen = p1len + p2len;

    if (tlen > sizeof(tmp)) {
        /* Defensive: caller guarantees this never happens. */
        hkdf_memset(out, 0, HMAC256_DIGEST);
        return;
    }
    if (p1len) hkdf_memcpy(tmp,         p1, p1len);
    if (p2len) hkdf_memcpy(tmp + p1len, p2, p2len);

    hmac_sha256(key, klen, tmp, tlen, out);

    hkdf_memset(tmp, 0, tlen);
}

/* =========================================================================
 * HKDF-Extract (RFC 5869 §2.2)
 *
 * PRK = HMAC-Hash(salt, IKM)
 *
 * "Note that in the extract step, 'IKM' is used as the HMAC input, not
 * as the HMAC key."
 *
 * Default salt: if saltlen == 0, use a HashLen-byte string of zero octets.
 * ====================================================================== */

void hkdf_extract_sha256(const unsigned char *salt, unsigned long saltlen,
                          const unsigned char *ikm,  unsigned long ikmlen,
                          unsigned char prk[32])
{
    unsigned char zero_salt[32];

    if (saltlen == 0) {
        hkdf_memset(zero_salt, 0, 32);
        salt    = zero_salt;
        saltlen = 32;
    }
    hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
}

void hkdf_extract_sha384(const unsigned char *salt, unsigned long saltlen,
                          const unsigned char *ikm,  unsigned long ikmlen,
                          unsigned char prk[48])
{
    unsigned char zero_salt[48];

    if (saltlen == 0) {
        hkdf_memset(zero_salt, 0, 48);
        salt    = zero_salt;
        saltlen = 48;
    }
    hk_hmac_sha384(salt, saltlen, ikm, ikmlen, prk);
}

/* =========================================================================
 * HKDF-Expand (RFC 5869 §2.3)
 *
 * T(0) = ""
 * T(i) = HMAC-Hash(PRK, T(i-1) || info || i)   i = 1,2,...
 * OKM  = T(1) || T(2) || ...  (first outlen bytes)
 *
 * Constraint: outlen <= 255 * HashLen.
 * ====================================================================== */

int hkdf_expand_sha256(const unsigned char prk[32],
                        const unsigned char *info, unsigned long infolen,
                        unsigned char *out,  unsigned long outlen)
{
    /* Max OKM = 255 * 32 = 8160 bytes */
    const unsigned long hashlen = 32;
    unsigned char t[32];          /* T(i) accumulator */
    unsigned char prev[32];       /* T(i-1) */
    unsigned char counter;
    unsigned char info_ctr[8192 + 1]; /* info || counter byte */
    unsigned long written = 0;
    unsigned long chunk;
    int first = 1;

    if (outlen > 255 * hashlen) return -1;
    if (outlen == 0) return 0;

    /* Limit info to fit in our stack buffer + 1 byte counter */
    if (infolen > 8192) return -1;

    hkdf_memset(prev, 0, hashlen);

    for (counter = 1; written < outlen; counter++) {
        /*
         * Build message = T(i-1) || info || counter
         * For first iteration T(0) = "" so skip the prev part.
         */
        unsigned long plen = first ? 0 : hashlen;

        /* Copy info then counter into info_ctr */
        if (infolen) hkdf_memcpy(info_ctr, info, infolen);
        info_ctr[infolen] = counter;

        hmac_sha256_2(prk, hashlen,
                      prev, plen,
                      info_ctr, infolen + 1,
                      t);

        /* Copy min(hashlen, remaining) bytes to output */
        chunk = outlen - written;
        if (chunk > hashlen) chunk = hashlen;
        hkdf_memcpy(out + written, t, chunk);
        written += chunk;

        /* T(i) becomes T(i-1) for next round */
        hkdf_memcpy(prev, t, hashlen);
        first = 0;
    }

    hkdf_memset(t,        0, hashlen);
    hkdf_memset(prev,     0, hashlen);
    hkdf_memset(info_ctr, 0, infolen + 1);
    return 0;
}

int hkdf_expand_sha384(const unsigned char prk[48],
                        const unsigned char *info, unsigned long infolen,
                        unsigned char *out,  unsigned long outlen)
{
    const unsigned long hashlen = 48;
    unsigned char t[48];
    unsigned char prev[48];
    unsigned char info_ctr[8192 + 1]; /* info || counter byte (hoisted) */
    unsigned char counter;
    unsigned long written = 0;
    unsigned long chunk;
    int first = 1;

    if (outlen > 255 * hashlen) return -1;
    if (outlen == 0) return 0;
    if (infolen > 8192) return -1;

    hkdf_memset(prev, 0, hashlen);

    for (counter = 1; written < outlen; counter++) {
        unsigned long plen = first ? 0 : hashlen;

        if (infolen) hkdf_memcpy(info_ctr, info, infolen);
        info_ctr[infolen] = counter;

        hmac_sha384_2(prk, hashlen,
                      prev, plen,
                      info_ctr, infolen + 1,
                      t);

        chunk = outlen - written;
        if (chunk > hashlen) chunk = hashlen;
        hkdf_memcpy(out + written, t, chunk);
        written += chunk;

        hkdf_memcpy(prev, t, hashlen);
        first = 0;

        hkdf_memset(info_ctr, 0, infolen + 1);
    }

    hkdf_memset(t,        0, hashlen);
    hkdf_memset(prev,     0, hashlen);
    hkdf_memset(info_ctr, 0, infolen + 1);
    return 0;
}

/* =========================================================================
 * TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1)
 *
 * HKDF-Expand-Label(Secret, Label, Context, Length) =
 *     HKDF-Expand(Secret, HkdfLabel, Length)
 *
 * HkdfLabel (wire encoding):
 *   uint16   length;          -- Length (big-endian)
 *   opaque   label<7..255>;   -- 1-byte length prefix + "tls13 " + Label
 *   opaque   context<0..255>; -- 1-byte length prefix + Context
 *
 * Total info byte string fed to HKDF-Expand:
 *   [ length_hi, length_lo, label_len, 't','l','s','1','3',' ', ...label...,
 *     ctx_len, ...context... ]
 *
 * Constraints:
 *   - "tls13 " prefix (6 bytes) + label <= 249 bytes  (label_len fits u8)
 *   - ctxlen <= 255 (context_len fits u8)
 *   - secretlen == HashLen for hash_id
 *   - outlen <= 255 * HashLen
 * ====================================================================== */

#define TLS13_LABEL_PREFIX      "tls13 "
#define TLS13_LABEL_PREFIX_LEN  6

int tls13_hkdf_expand_label(int hash_id,
                             const unsigned char *secret,
                             unsigned long        secretlen,
                             const char          *label,
                             const unsigned char *context,
                             unsigned long        ctxlen,
                             unsigned char       *out,
                             unsigned long        outlen)
{
    unsigned long hashlen;
    unsigned long labellen;
    unsigned long full_label_len;  /* "tls13 " + label */
    unsigned char hkdf_label[2 + 1 + 249 + 1 + 255]; /* max wire size */
    unsigned long hkdf_label_len = 0;
    unsigned long i;

    /* Determine hash length */
    if (hash_id == 0)      hashlen = 32;
    else if (hash_id == 1) hashlen = 48;
    else return -1;

    /* Validate secret length matches HashLen */
    if (secretlen != hashlen) return -1;

    /* Validate outlen */
    if (outlen > 255 * hashlen) return -1;
    if (outlen == 0) return 0;

    /* Validate label */
    labellen      = hkdf_strlen(label);
    full_label_len = TLS13_LABEL_PREFIX_LEN + labellen;
    if (full_label_len > 249) return -1;  /* label_len must fit in uint8 */
    if (full_label_len < 1)   return -1;  /* must be at least 1 byte */

    /* Validate context length */
    if (ctxlen > 255) return -1;

    /*
     * Build HkdfLabel wire encoding:
     *   [0..1] : uint16 outlen (big-endian)
     *   [2]    : uint8  label_len = len("tls13 " + label)
     *   [3..3+full_label_len-1] : "tls13 " + label
     *   [3+full_label_len] : uint8 context_len
     *   [3+full_label_len+1 ..] : context bytes
     */
    hkdf_label[hkdf_label_len++] = (unsigned char)((outlen >> 8) & 0xff);
    hkdf_label[hkdf_label_len++] = (unsigned char)(outlen & 0xff);
    hkdf_label[hkdf_label_len++] = (unsigned char)full_label_len;

    /* "tls13 " */
    for (i = 0; i < TLS13_LABEL_PREFIX_LEN; i++)
        hkdf_label[hkdf_label_len++] = (unsigned char)TLS13_LABEL_PREFIX[i];

    /* label */
    for (i = 0; i < labellen; i++)
        hkdf_label[hkdf_label_len++] = (unsigned char)label[i];

    /* context length + context */
    hkdf_label[hkdf_label_len++] = (unsigned char)ctxlen;
    for (i = 0; i < ctxlen; i++)
        hkdf_label[hkdf_label_len++] = context[i];

    /* Call the appropriate HKDF-Expand */
    if (hash_id == 0) {
        return hkdf_expand_sha256(secret, hkdf_label, hkdf_label_len,
                                  out, outlen);
    } else {
        return hkdf_expand_sha384(secret, hkdf_label, hkdf_label_len,
                                  out, outlen);
    }
}

/* =========================================================================
 * Self-test: RFC 5869 Appendix A, Test Cases 1 and 3 (SHA-256)
 *
 * Case 1:
 *   Hash     = SHA-256
 *   IKM      = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (22 octets)
 *   salt     = 0x000102030405060708090a0b0c (13 octets)
 *   info     = 0xf0f1f2f3f4f5f6f7f8f9 (10 octets)
 *   L        = 42
 *
 *   PRK      = 0x077709362c2e32df0ddc3f0dc47bba63
 *              90b6c73bb50f9c3122ec844ad7c2b3e5 (32 octets)
 *   OKM      = 0x3cb25f25faacd57a90434f64d0362f2a
 *              2d2d0a90cf1a5a4c5db02d56ecc4c5bf
 *              34007208d5b887185865 (42 octets)
 *
 * Case 3:
 *   Hash     = SHA-256
 *   IKM      = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (22 octets)
 *   salt     = not provided (zero-length / default)
 *   info     = not provided (zero-length)
 *   L        = 42
 *
 *   PRK      = 0x19ef24a32c717b167f33a91d6f648bdf
 *              96596776afdb6377ac434c1c293ccb04 (32 octets)
 *   OKM      = 0x8da4e775a563c18f715f802a063c5a31
 *              b8a11f5c5ee1879ec3454e5f3c738d2d
 *              9d201395faa4b61a96c8 (42 octets)
 * ====================================================================== */

int hkdf_selftest(void)
{
    /* ------------------------------------------------------------------ */
    /* Test Case 1                                                         */
    /* ------------------------------------------------------------------ */
    static const unsigned char tc1_ikm[22] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b
    };
    static const unsigned char tc1_salt[13] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,
        0x07,0x08,0x09,0x0a,0x0b,0x0c
    };
    static const unsigned char tc1_info[10] = {
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9
    };
    static const unsigned char tc1_prk[32] = {
        0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,
        0x0d,0xdc,0x3f,0x0d,0xc4,0x7b,0xba,0x63,
        0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,
        0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5
    };
    static const unsigned char tc1_okm[42] = {
        0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,
        0x90,0x43,0x4f,0x64,0xd0,0x36,0x2f,0x2a,
        0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,
        0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,
        0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,
        0x58,0x65
    };

    /* ------------------------------------------------------------------ */
    /* Test Case 3                                                         */
    /* ------------------------------------------------------------------ */
    static const unsigned char tc3_ikm[22] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b
    };
    /* salt = "", info = "" */
    static const unsigned char tc3_prk[32] = {
        0x19,0xef,0x24,0xa3,0x2c,0x71,0x7b,0x16,
        0x7f,0x33,0xa9,0x1d,0x6f,0x64,0x8b,0xdf,
        0x96,0x59,0x67,0x76,0xaf,0xdb,0x63,0x77,
        0xac,0x43,0x4c,0x1c,0x29,0x3c,0xcb,0x04
    };
    static const unsigned char tc3_okm[42] = {
        0x8d,0xa4,0xe7,0x75,0xa5,0x63,0xc1,0x8f,
        0x71,0x5f,0x80,0x2a,0x06,0x3c,0x5a,0x31,
        0xb8,0xa1,0x1f,0x5c,0x5e,0xe1,0x87,0x9e,
        0xc3,0x45,0x4e,0x5f,0x3c,0x73,0x8d,0x2d,
        0x9d,0x20,0x13,0x95,0xfa,0xa4,0xb6,0x1a,
        0x96,0xc8
    };

    unsigned char prk[32];
    unsigned char okm[42];

    /* -- Case 1: Extract -- */
    hkdf_extract_sha256(tc1_salt, 13, tc1_ikm, 22, prk);
    if (hkdf_memcmp(prk, tc1_prk, 32) != 0) return -1;

    /* -- Case 1: Expand -- */
    hkdf_memset(okm, 0, 42);
    if (hkdf_expand_sha256(prk, tc1_info, 10, okm, 42) != 0) return -1;
    if (hkdf_memcmp(okm, tc1_okm, 42) != 0) return -1;

    /* -- Case 3: Extract (no salt, no info) -- */
    hkdf_extract_sha256(((void*)0), 0, tc3_ikm, 22, prk);
    if (hkdf_memcmp(prk, tc3_prk, 32) != 0) return -1;

    /* -- Case 3: Expand (no info) -- */
    hkdf_memset(okm, 0, 42);
    if (hkdf_expand_sha256(prk, ((void*)0), 0, okm, 42) != 0) return -1;
    if (hkdf_memcmp(okm, tc3_okm, 42) != 0) return -1;

    /* -- TLS 1.3 Expand-Label sanity check: must produce outlen bytes -- */
    {
        unsigned char tls_out[32];
        unsigned char dummy_secret[32];
        unsigned char dummy_ctx[32];
        hkdf_memset(dummy_secret, 0xab, 32);
        hkdf_memset(dummy_ctx,    0xcd, 32);
        if (tls13_hkdf_expand_label(0,
                                    dummy_secret, 32,
                                    "key",
                                    dummy_ctx, 32,
                                    tls_out, 32) != 0) return -1;
        /* Output must be non-zero (vanishingly unlikely to be all-zero) */
        {
            int all_zero = 1, k;
            for (k = 0; k < 32; k++) if (tls_out[k]) { all_zero = 0; break; }
            if (all_zero) return -1;
        }
        /* Verify length constraint rejection */
        if (tls13_hkdf_expand_label(0,
                                    dummy_secret, 32,
                                    "key",
                                    dummy_ctx, 32,
                                    tls_out,
                                    255 * 32 + 1) != -1) return -1;
        /* Verify bad hash_id rejection */
        if (tls13_hkdf_expand_label(2,
                                    dummy_secret, 32,
                                    "key",
                                    dummy_ctx, 32,
                                    tls_out, 16) != -1) return -1;
    }

    return 0; /* all tests passed */
}
