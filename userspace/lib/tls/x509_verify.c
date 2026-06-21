/*
 * x509_verify.c -- freestanding X.509 certificate chain validation.
 * =================================================================
 * No libc, no syscalls, no malloc, no standard headers. See x509_verify.h for
 * the full contract and the SECURITY HONESTY notes (no revocation; only
 * RSA-PKCS1-SHA256 and ECDSA-P256-SHA256 signature algorithms).
 *
 * Implementation strategy
 * -----------------------
 * We do all certificate parsing here with the bounds-checked DER reader in
 * asn1.h. We deliberately do NOT rely on the optional extended accessors that
 * may or may not be present in x509.h (x509_get_tbs / x509_get_signature /
 * x509_get_*_dn / x509_get_san / ...): another engineer is adding those
 * concurrently, and binding to symbols that might not exist yet would break the
 * build. We DO reuse the confirmed, stable x509.h helpers where they fit:
 *   - x509_extract_pubkey()  to pull an RSA modulus/exponent, and
 *   - x509_get_subject_cn()  for the CN hostname fallback.
 * Everything else (TBS bytes, signatureAlgorithm, signatureValue, issuer/subject
 * DN raw bytes, validity normalisation, SAN dNSNames, EC public key, ECDSA r/s)
 * is parsed locally below, so this file is self-contained and correct
 * regardless of when the extended x509.c accessors land.
 */

#include "asn1.h"
#include "x509.h"

#include "../crypto/sha256.h"
#include "../crypto/sha512.h"
#include "../crypto/rsa.h"
#include "../crypto/p256.h"
#include "../crypto/p384.h"

#include "x509_verify.h"

/* ====================================================================== */
/* Local memory helpers (no libc)                                         */
/* ====================================================================== */

static void v_memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}
static void v_memset(void *d, int v, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--) *dp++ = (unsigned char)v;
}
static int v_memeq(const unsigned char *a, const unsigned char *b,
                   unsigned long n) {
    while (n--) { if (*a++ != *b++) return 0; }
    return 1;
}
static unsigned long v_strlen(const char *s) {
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}
static int ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

/* ====================================================================== */
/* OID constants (packed content bytes, no tag/length)                    */
/* ====================================================================== */

/* sha256WithRSAEncryption: 1.2.840.113549.1.1.11 */
static const unsigned char OID_RSA_SHA256[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B
};
/* sha384WithRSAEncryption: 1.2.840.113549.1.1.12 */
static const unsigned char OID_RSA_SHA384[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C
};
/* sha512WithRSAEncryption: 1.2.840.113549.1.1.13 */
static const unsigned char OID_RSA_SHA512[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D
};
/* ecdsa-with-SHA256: 1.2.840.10045.4.3.2 */
static const unsigned char OID_ECDSA_SHA256[8] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02
};
/* ecdsa-with-SHA384: 1.2.840.10045.4.3.3 */
static const unsigned char OID_ECDSA_SHA384[8] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03
};
/* ecdsa-with-SHA512: 1.2.840.10045.4.3.4 */
static const unsigned char OID_ECDSA_SHA512[8] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04
};
/* id-ecPublicKey: 1.2.840.10045.2.1 */
static const unsigned char OID_EC_PUBLIC_KEY[7] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01
};
/* prime256v1 / secp256r1 (P-256): 1.2.840.10045.3.1.7 */
static const unsigned char OID_P256[8] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};
/* secp384r1 (P-384): 1.3.132.0.34 */
static const unsigned char OID_P384[5] = {
    0x2B, 0x81, 0x04, 0x00, 0x22
};
/* rsaEncryption: 1.2.840.113549.1.1.1 */
static const unsigned char OID_RSA_ENC[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01
};
/* id-ce-subjectAltName: 2.5.29.17 */
static const unsigned char OID_SUBJECT_ALT_NAME[3] = { 0x55, 0x1D, 0x11 };
/* commonName: 2.5.4.3 */
static const unsigned char OID_COMMON_NAME[3] = { 0x55, 0x04, 0x03 };

/* Internal signature-algorithm selector. */
#define SIGALG_RSA_SHA256    1
#define SIGALG_ECDSA_SHA256  2
#define SIGALG_RSA_SHA384    3
#define SIGALG_RSA_SHA512    4
#define SIGALG_ECDSA_SHA384  5
#define SIGALG_ECDSA_SHA512  6

/* Public-key algorithm selector. */
#define PKALG_RSA   0
#define PKALG_EC256  1
#define PKALG_EC384  2

/* ====================================================================== */
/* DER navigation helpers                                                 */
/* ====================================================================== */

/*
 * Grab the whole TLV (identifier byte through end of content) at cursor `c`
 * without consuming it conceptually -- but we DO advance `c` so callers can keep
 * walking. *start/*total describe the full TLV extent; *tagv its identifier.
 */
static int tlv_extent(asn1_cur *c, int *tagv,
                      const unsigned char **start, unsigned long *total) {
    const unsigned char *s = c->p;
    const unsigned char *val;
    unsigned long vlen;
    int tag;
    if (asn1_get_tlv(c, &tag, &val, &vlen) != 0) return -1;
    if (tagv) *tagv = tag;
    if (start) *start = s;
    if (total) *total = (unsigned long)((val + vlen) - s);
    return 0;
}

/*
 * Enter Certificate, returning sub-cursors / views for the pieces we need:
 *   tbs_bytes / tbs_len  : exact DER of tbsCertificate (tag..end) -- to hash.
 *   sigalg_oid/len       : the OID inside the outer signatureAlgorithm.
 *   sig_bits/sig_bitlen  : signatureValue BIT STRING payload (after unused byte).
 *   tbs_cur (out)        : a fresh cursor positioned at the start of the
 *                          tbsCertificate CONTENTS (for field walking).
 * Returns 0 on success.
 */
static int parse_cert_top(const unsigned char *der, unsigned long len,
                          const unsigned char **tbs_bytes,
                          unsigned long *tbs_len,
                          const unsigned char **sigalg_oid,
                          unsigned long *sigalg_oidlen,
                          const unsigned char **sig_bits,
                          unsigned long *sig_bitlen,
                          asn1_cur *tbs_cur_out) {
    asn1_cur top, cert, algid;
    int tag, unused;

    if (!der || len == 0) return -1;
    top.p = der; top.end = der + len;

    /* Certificate ::= SEQUENCE */
    if (asn1_enter(&top, ASN1_SEQUENCE, &cert) != 0) return -1;

    /* tbsCertificate ::= SEQUENCE -- capture its whole TLV extent for hashing,
     * and also produce a cursor over its contents. */
    {
        const unsigned char *tbs_start;
        const unsigned char *tbs_val;
        unsigned long tbs_vlen;
        asn1_cur peek = cert;
        if (asn1_peek_tlv(&peek, &tag, &tbs_val, &tbs_vlen) != 0) return -1;
        if (tag != ASN1_SEQUENCE) return -1;
        tbs_start = cert.p;
        if (tbs_bytes) *tbs_bytes = tbs_start;
        if (tbs_len)
            *tbs_len = (unsigned long)((tbs_val + tbs_vlen) - tbs_start);
        if (tbs_cur_out) { tbs_cur_out->p = tbs_val; tbs_cur_out->end = tbs_val + tbs_vlen; }
        /* advance `cert` past tbsCertificate */
        if (asn1_skip(&cert) != 0) return -1;
    }

    /* signatureAlgorithm ::= AlgorithmIdentifier ::= SEQUENCE { OID, params } */
    if (asn1_enter(&cert, ASN1_SEQUENCE, &algid) != 0) return -1;
    if (asn1_get_oid(&algid, sigalg_oid, sigalg_oidlen) != 0) return -1;

    /* signatureValue BIT STRING */
    if (asn1_get_bitstring(&cert, sig_bits, sig_bitlen, &unused) != 0) return -1;
    if (unused != 0) return -1;

    return 0;
}

/*
 * From a tbsCertificate-contents cursor, walk to the named fields, returning
 * raw-TLV views for issuer and subject DN, plus a cursor positioned at the SPKI
 * and a cursor positioned at the extensions (or at end if none), and the
 * validity Time TLVs.
 *
 * TBSCertificate fields:
 *   [0] version (OPTIONAL, EXPLICIT, 0xA0)
 *   serialNumber INTEGER
 *   signature AlgorithmIdentifier   (the INNER copy, we just skip it)
 *   issuer Name (SEQUENCE)          <- raw DER captured
 *   validity SEQUENCE {nb,na}       <- both Time TLVs captured
 *   subject Name (SEQUENCE)         <- raw DER captured
 *   subjectPublicKeyInfo SEQUENCE   <- spki cursor captured
 *   [1] issuerUniqueID  (OPTIONAL)
 *   [2] subjectUniqueID (OPTIONAL)
 *   [3] extensions (OPTIONAL, EXPLICIT) <- ext cursor captured
 */
typedef struct {
    const unsigned char *issuer_dn;  unsigned long issuer_dn_len;
    const unsigned char *subject_dn; unsigned long subject_dn_len;
    const unsigned char *spki;       unsigned long spki_len;
    /* validity raw Time TLV values + tags */
    int   nb_tag;   const unsigned char *nb_val; unsigned long nb_len;
    int   na_tag;   const unsigned char *na_val; unsigned long na_len;
    /* extensions [3] contents, if present (may have len 0) */
    int   have_exts; asn1_cur exts;
} tbs_fields;

static int parse_tbs_fields(asn1_cur tbs, tbs_fields *f) {
    int tag;
    const unsigned char *val;
    unsigned long vlen;
    asn1_cur validity;

    v_memset(f, 0, sizeof *f);

    /* optional [0] version */
    if (asn1_peek_tlv(&tbs, &tag, &val, &vlen) != 0) return -1;
    if (tag == ASN1_CONTEXT_CONSTRUCTED(0)) {
        if (asn1_skip(&tbs) != 0) return -1;
    }
    if (asn1_skip(&tbs) != 0) return -1;   /* serialNumber */
    if (asn1_skip(&tbs) != 0) return -1;   /* signature AlgorithmIdentifier */

    /* issuer Name -- capture whole TLV */
    if (tlv_extent(&tbs, &tag, &f->issuer_dn, &f->issuer_dn_len) != 0) return -1;
    if (tag != ASN1_SEQUENCE) return -1;

    /* validity SEQUENCE { notBefore Time, notAfter Time } */
    if (asn1_enter(&tbs, ASN1_SEQUENCE, &validity) != 0) return -1;
    if (asn1_get_tlv(&validity, &f->nb_tag, &f->nb_val, &f->nb_len) != 0) return -1;
    if (asn1_get_tlv(&validity, &f->na_tag, &f->na_val, &f->na_len) != 0) return -1;

    /* subject Name -- capture whole TLV */
    if (tlv_extent(&tbs, &tag, &f->subject_dn, &f->subject_dn_len) != 0) return -1;
    if (tag != ASN1_SEQUENCE) return -1;

    /* subjectPublicKeyInfo -- capture whole TLV */
    if (tlv_extent(&tbs, &tag, &f->spki, &f->spki_len) != 0) return -1;
    if (tag != ASN1_SEQUENCE) return -1;

    /* optional [1] issuerUniqueID, [2] subjectUniqueID, [3] extensions */
    while (!asn1_at_end(&tbs)) {
        if (asn1_peek_tlv(&tbs, &tag, &val, &vlen) != 0) return -1;
        if (tag == ASN1_CONTEXT_CONSTRUCTED(3)) {
            /* [3] EXPLICIT { Extensions ::= SEQUENCE OF Extension } */
            asn1_cur ext_wrap;
            if (asn1_enter(&tbs, ASN1_CONTEXT_CONSTRUCTED(3), &ext_wrap) != 0)
                return -1;
            if (asn1_enter(&ext_wrap, ASN1_SEQUENCE, &f->exts) != 0) return -1;
            f->have_exts = 1;
            break;
        }
        /* [1]/[2] unique IDs or anything else -- skip. */
        if (asn1_skip(&tbs) != 0) return -1;
    }
    return 0;
}

/* ====================================================================== */
/* Validity time normalisation                                            */
/* ====================================================================== */

/*
 * Normalise a DER Time value to a 14-byte "YYYYMMDDHHMMSS" (no NUL, exactly 14
 * chars written to out[0..13]) suitable for lexicographic compare against the
 * caller's `now`. UTCTime is "YYMMDDHHMMSSZ" (RFC 5280: YY < 50 => 20YY, else
 * 19YY). GeneralizedTime is "YYYYMMDDHHMMSSZ". We accept a trailing 'Z' and
 * require all-digit fields. Returns 0 on success.
 */
static int normalise_time(int tag, const unsigned char *v, unsigned long vlen,
                          char out[14]) {
    unsigned long i;
    const unsigned char *digits;
    unsigned long ndig;
    char yy[2];

    /* strip a single trailing 'Z' if present for the length checks below */
    if (tag == (ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME)) {
        /* "YYMMDDHHMMSSZ" -> 13 chars, or "YYMMDDHHMMSS" -> 12 */
        if (vlen == 13) { if (v[12] != 'Z') return -1; vlen = 12; }
        if (vlen != 12) return -1;
        digits = v; ndig = 12;
        /* validate digits */
        for (i = 0; i < ndig; i++)
            if (v[i] < '0' || v[i] > '9') return -1;
        /* RFC 5280 UTCTime: YY in [0,49] -> 20YY, [50,99] -> 19YY. The first
         * digit decides: '0'..'4' => 20YY, '5'..'9' => 19YY. */
        yy[0] = (char)v[0]; yy[1] = (char)v[1];
        if (yy[0] >= '5') { out[0] = '1'; out[1] = '9'; }
        else              { out[0] = '2'; out[1] = '0'; }
        out[2] = yy[0]; out[3] = yy[1];
        for (i = 0; i < 10; i++) out[4 + i] = (char)digits[2 + i];
        return 0;
    } else if (tag == (ASN1_CLASS_UNIVERSAL | ASN1_TAG_GENERALIZEDTIME)) {
        /* "YYYYMMDDHHMMSSZ" -> 15, or "YYYYMMDDHHMMSS" -> 14 */
        if (vlen == 15) { if (v[14] != 'Z') return -1; vlen = 14; }
        if (vlen != 14) return -1;
        for (i = 0; i < 14; i++)
            if (v[i] < '0' || v[i] > '9') return -1;
        for (i = 0; i < 14; i++) out[i] = (char)v[i];
        return 0;
    }
    return -1;
}

/* Lexicographic compare of two 14-char time strings: <0,0,>0. */
static int time14_cmp(const char *a, const char *b) {
    int i;
    for (i = 0; i < 14; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

/* Validate that `now` is a 14-digit string. Returns 0 if ok. */
static int check_now_format(const char *now) {
    int i;
    if (!now) return -1;
    for (i = 0; i < 14; i++)
        if (now[i] < '0' || now[i] > '9') return -1;
    if (now[14] != '\0') return -1;
    return 0;
}

/*
 * Check a cert's validity window against `now`. f must already be parsed.
 * Returns X509V_OK if notBefore <= now <= notAfter, else X509V_ERR_*.
 */
static int check_validity(const tbs_fields *f, const char *now) {
    char nb[14], na[14];
    if (normalise_time(f->nb_tag, f->nb_val, f->nb_len, nb) != 0)
        return X509V_ERR_TIME_FMT;
    if (normalise_time(f->na_tag, f->na_val, f->na_len, na) != 0)
        return X509V_ERR_TIME_FMT;
    if (time14_cmp(now, nb) < 0) return X509V_ERR_EXPIRED;  /* not yet valid */
    if (time14_cmp(now, na) > 0) return X509V_ERR_EXPIRED;  /* expired       */
    return X509V_OK;
}

/* ====================================================================== */
/* Signature algorithm + public key                                       */
/* ====================================================================== */

/* Map a signatureAlgorithm OID to our internal selector, or 0 if unsupported. */
static int sigalg_from_oid(const unsigned char *oid, unsigned long oidlen) {
    if (asn1_oid_equals(oid, oidlen, OID_RSA_SHA256, sizeof OID_RSA_SHA256))
        return SIGALG_RSA_SHA256;
    if (asn1_oid_equals(oid, oidlen, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256))
        return SIGALG_ECDSA_SHA256;
    if (asn1_oid_equals(oid, oidlen, OID_RSA_SHA384, sizeof OID_RSA_SHA384))
        return SIGALG_RSA_SHA384;
    if (asn1_oid_equals(oid, oidlen, OID_RSA_SHA512, sizeof OID_RSA_SHA512))
        return SIGALG_RSA_SHA512;
    if (asn1_oid_equals(oid, oidlen, OID_ECDSA_SHA384, sizeof OID_ECDSA_SHA384))
        return SIGALG_ECDSA_SHA384;
    if (asn1_oid_equals(oid, oidlen, OID_ECDSA_SHA512, sizeof OID_ECDSA_SHA512))
        return SIGALG_ECDSA_SHA512;
    return 0;
}

/*
 * Inspect an SPKI SEQUENCE TLV to determine the public-key algorithm.
 * Returns PKALG_RSA, PKALG_EC256, or negative on error / unsupported.
 */
static int spki_alg(const unsigned char *spki, unsigned long spki_len) {
    asn1_cur top, seq, algid;
    const unsigned char *oid;
    unsigned long oidlen;

    top.p = spki; top.end = spki + spki_len;
    if (asn1_enter(&top, ASN1_SEQUENCE, &seq) != 0) return -1;
    if (asn1_enter(&seq, ASN1_SEQUENCE, &algid) != 0) return -1;
    if (asn1_get_oid(&algid, &oid, &oidlen) != 0) return -1;

    if (asn1_oid_equals(oid, oidlen, OID_RSA_ENC, sizeof OID_RSA_ENC))
        return PKALG_RSA;
    if (asn1_oid_equals(oid, oidlen, OID_EC_PUBLIC_KEY, sizeof OID_EC_PUBLIC_KEY)) {
        /* Named curve param after the OID: P-256 or P-384 supported. */
        const unsigned char *curve;
        unsigned long curvelen;
        if (asn1_get_oid(&algid, &curve, &curvelen) != 0) return -1;
        if (asn1_oid_equals(curve, curvelen, OID_P256, sizeof OID_P256))
            return PKALG_EC256;
        if (asn1_oid_equals(curve, curvelen, OID_P384, sizeof OID_P384))
            return PKALG_EC384;
        return -1;   /* curve other than P-256/P-384 unsupported */
    }
    return -1;
}

/*
 * Extract an uncompressed EC point (0x04 || X || Y) of exactly `expect_len`
 * bytes from an SPKI (65 for P-256, 97 for P-384). Returns 0 on success.
 */
static int spki_ec_point(const unsigned char *spki, unsigned long spki_len,
                         unsigned char *point, unsigned long expect_len) {
    asn1_cur top, seq;
    const unsigned char *bits;
    unsigned long bits_len;
    int unused;

    top.p = spki; top.end = spki + spki_len;
    if (asn1_enter(&top, ASN1_SEQUENCE, &seq) != 0) return -1;
    if (asn1_skip(&seq) != 0) return -1;   /* AlgorithmIdentifier */
    if (asn1_get_bitstring(&seq, &bits, &bits_len, &unused) != 0) return -1;
    if (unused != 0) return -1;
    if (bits_len != expect_len || bits[0] != 0x04) return -1; /* uncompressed */
    v_memcpy(point, bits, expect_len);
    return 0;
}

/*
 * Decode an ECDSA-Sig-Value DER ::= SEQUENCE { r INTEGER, s INTEGER } from the
 * signature BIT STRING payload into fixed big-endian r and s of `clen` bytes
 * each (left padded): 32 for P-256, 48 for P-384. Returns 0 on success.
 */
static int ec_sig_to_rs(const unsigned char *sig, unsigned long sig_len,
                        unsigned char *r, unsigned char *s,
                        unsigned long clen) {
    asn1_cur top, seq;
    const unsigned char *rv, *sv;
    unsigned long rl, sl;

    top.p = sig; top.end = sig + sig_len;
    if (asn1_enter(&top, ASN1_SEQUENCE, &seq) != 0) return -1;
    if (asn1_get_integer(&seq, &rv, &rl) != 0) return -1;
    if (asn1_get_integer(&seq, &sv, &sl) != 0) return -1;

    /* strip leading 0x00 sign byte(s) */
    while (rl > 1 && rv[0] == 0x00) { rv++; rl--; }
    while (sl > 1 && sv[0] == 0x00) { sv++; sl--; }
    if (rl == 0 || sl == 0 || rl > clen || sl > clen) return -1;

    v_memset(r, 0, clen);
    v_memset(s, 0, clen);
    v_memcpy(r + (clen - rl), rv, rl);
    v_memcpy(s + (clen - sl), sv, sl);
    return 0;
}

/* ====================================================================== */
/* The core: verify cert's signature using issuer's SPKI                  */
/* ====================================================================== */

/*
 * Verify that `cert` (DER) was signed by the key in `issuer_spki`
 * (subjectPublicKeyInfo SEQUENCE TLV). Hashes cert's TBS with the algorithm
 * indicated by cert's signatureAlgorithm and runs RSA or ECDSA verify.
 * Returns X509V_OK or a negative X509V_ERR_*.
 */
static int verify_cert_signed_by(const unsigned char *cert,
                                 unsigned long cert_len,
                                 const unsigned char *issuer_spki,
                                 unsigned long issuer_spki_len) {
    const unsigned char *tbs;       unsigned long tbs_len;
    const unsigned char *sigalg;    unsigned long sigalg_len;
    const unsigned char *sigbits;   unsigned long sigbits_len;
    asn1_cur tbs_cur;
    /* 64 bytes covers SHA-256 (32), SHA-384 (48), and SHA-512 (64). */
    unsigned char hash[64];
    unsigned long hash_len;
    int alg, pkalg;

    if (parse_cert_top(cert, cert_len, &tbs, &tbs_len, &sigalg, &sigalg_len,
                       &sigbits, &sigbits_len, &tbs_cur) != 0)
        return X509V_ERR_PARSE;

    alg = sigalg_from_oid(sigalg, sigalg_len);
    if (alg == 0) return X509V_ERR_SIG_ALG;

    /* Hash the TBS bytes with the algorithm indicated by the signature. */
    if (alg == SIGALG_RSA_SHA256 || alg == SIGALG_ECDSA_SHA256) {
        sha256(tbs, tbs_len, hash);
        hash_len = 32;
    } else if (alg == SIGALG_RSA_SHA384 || alg == SIGALG_ECDSA_SHA384) {
        sha384(tbs, tbs_len, hash);
        hash_len = 48;
    } else { /* RSA_SHA512 or ECDSA_SHA512 */
        sha512(tbs, tbs_len, hash);
        hash_len = 64;
    }

    pkalg = spki_alg(issuer_spki, issuer_spki_len);
    if (pkalg < 0) return X509V_ERR_PUBKEY;

    if (alg == SIGALG_RSA_SHA256 || alg == SIGALG_RSA_SHA384 ||
        alg == SIGALG_RSA_SHA512) {
        unsigned char mod[512], exp[16];
        unsigned long mod_len = 0, exp_len = 0;
        rsa_pubkey pk;
        int rsa_hash_alg;
        if (pkalg != PKALG_RSA) return X509V_ERR_SIG_ALG;
        /* Reuse the confirmed x509.h SPKI extractor for the RSA key. */
        if (x509_extract_pubkey(issuer_spki, issuer_spki_len,
                                mod, &mod_len, exp, &exp_len) != 0)
            return X509V_ERR_PUBKEY;
        rsa_pubkey_from_bytes(&pk, mod, mod_len, exp, exp_len);
        if (alg == SIGALG_RSA_SHA256)
            rsa_hash_alg = RSA_HASH_SHA256;
        else if (alg == SIGALG_RSA_SHA384)
            rsa_hash_alg = RSA_HASH_SHA384;
        else
            rsa_hash_alg = RSA_HASH_SHA512;
        if (rsa_pkcs1_verify(&pk, sigbits, sigbits_len, hash, hash_len,
                             rsa_hash_alg) != 0)
            return X509V_ERR_SIG_INVALID;
        return X509V_OK;
    } else if (pkalg == PKALG_EC384) { /* ECDSA P-384 with SHA-256/384/512 */
        unsigned char point[97];
        unsigned char r[48], s[48];
        if (spki_ec_point(issuer_spki, issuer_spki_len, point, 97) != 0)
            return X509V_ERR_PUBKEY;
        if (ec_sig_to_rs(sigbits, sigbits_len, r, s, 48) != 0)
            return X509V_ERR_SIG_INVALID;
        if (p384_ecdsa_verify(point, hash, hash_len, r, s) != 0)
            return X509V_ERR_SIG_INVALID;
        return X509V_OK;
    } else { /* ECDSA (P-256) with SHA-256/384/512 */
        unsigned char point[65];
        unsigned char r[32], s[32];
        if (pkalg != PKALG_EC256) return X509V_ERR_SIG_ALG;
        if (spki_ec_point(issuer_spki, issuer_spki_len, point, 65) != 0)
            return X509V_ERR_PUBKEY;
        if (ec_sig_to_rs(sigbits, sigbits_len, r, s, 32) != 0)
            return X509V_ERR_SIG_INVALID;
        if (p256_ecdsa_verify(point, hash, hash_len, r, s) != 0)
            return X509V_ERR_SIG_INVALID;
        return X509V_OK;
    }
}

/* ====================================================================== */
/* Hostname matching                                                      */
/* ====================================================================== */

int x509_hostname_match(const char *hostname, const char *cert_name) {
    unsigned long hl, cl;
    if (!hostname || !cert_name) return 0;
    hl = v_strlen(hostname);
    cl = v_strlen(cert_name);
    if (hl == 0 || cl == 0) return 0;

    /* Wildcard: cert_name begins with "*." -- match exactly one left label. */
    if (cert_name[0] == '*' && cert_name[1] == '.') {
        const char *suffix = cert_name + 1;     /* ".example.com"            */
        unsigned long suffix_len = cl - 1;
        const char *dot = (const char *)0;
        const char *p;
        unsigned long label_len, rest_len, i;

        /* hostname must contain a dot: there must be a left label to consume. */
        for (p = hostname; *p; p++) {
            if (*p == '.') { dot = p; break; }
        }
        if (!dot) return 0;                     /* no left label             */
        label_len = (unsigned long)(dot - hostname);
        if (label_len == 0) return 0;           /* leading dot / empty label */

        /* The left label must not itself contain a dot (one label only). The
         * loop above stops at the FIRST dot, so `dot` is the boundary; the
         * remainder (from `dot`) must equal the wildcard suffix exactly. */
        rest_len = hl - label_len;              /* length of ".example.com"  */
        if (rest_len != suffix_len) return 0;
        for (i = 0; i < suffix_len; i++) {
            if (ascii_lower((unsigned char)dot[i]) !=
                ascii_lower((unsigned char)suffix[i]))
                return 0;
        }
        /* Reject a wildcard that would match a bare apex (no left label) -- we
         * already required a non-empty left label above, so we are good. Also
         * guard against the left label containing further dots is impossible
         * because we matched the remainder against the full suffix and the
         * label is everything before the first dot. */
        return 1;
    }

    /* Exact, case-insensitive. */
    if (hl != cl) return 0;
    {
        unsigned long i;
        for (i = 0; i < hl; i++) {
            if (ascii_lower((unsigned char)hostname[i]) !=
                ascii_lower((unsigned char)cert_name[i]))
                return 0;
        }
    }
    return 1;
}

/*
 * Check the leaf certificate's identity against `hostname`: iterate SAN dNSName
 * entries; if at least one SAN dNSName exists, the hostname MUST match one of
 * them (SAN present => CN ignored, per RFC 6125). If no SAN dNSName is present,
 * fall back to the subject CN. Returns X509V_OK or X509V_ERR_HOSTNAME.
 */
static int check_hostname(const unsigned char *leaf, unsigned long leaf_len,
                          const tbs_fields *f, const char *hostname) {
    int saw_dns = 0;

    if (f->have_exts) {
        asn1_cur exts = f->exts;
        while (!asn1_at_end(&exts)) {
            asn1_cur ext;
            const unsigned char *oid;
            unsigned long oidlen;
            int tag;
            const unsigned char *val;
            unsigned long vlen;

            if (asn1_enter(&exts, ASN1_SEQUENCE, &ext) != 0)
                return X509V_ERR_PARSE;
            if (asn1_get_oid(&ext, &oid, &oidlen) != 0) return X509V_ERR_PARSE;

            /* optional BOOLEAN critical */
            if (asn1_peek_tlv(&ext, &tag, &val, &vlen) != 0)
                return X509V_ERR_PARSE;
            if (tag == (ASN1_CLASS_UNIVERSAL | ASN1_TAG_BOOLEAN)) {
                if (asn1_skip(&ext) != 0) return X509V_ERR_PARSE;
            }

            /* extnValue OCTET STRING wrapping the actual extension DER */
            if (asn1_expect(&ext, ASN1_CLASS_UNIVERSAL | ASN1_TAG_OCTET_STRING,
                            &val, &vlen) != 0)
                return X509V_ERR_PARSE;

            if (asn1_oid_equals(oid, oidlen, OID_SUBJECT_ALT_NAME,
                                sizeof OID_SUBJECT_ALT_NAME)) {
                /* GeneralNames ::= SEQUENCE OF GeneralName.
                 * dNSName ::= [2] IA5String (context-primitive tag 0x82). */
                asn1_cur san_wrap, names;
                san_wrap.p = val; san_wrap.end = val + vlen;
                if (asn1_enter(&san_wrap, ASN1_SEQUENCE, &names) != 0)
                    return X509V_ERR_PARSE;
                while (!asn1_at_end(&names)) {
                    int gtag;
                    const unsigned char *gv;
                    unsigned long gl;
                    if (asn1_get_tlv(&names, &gtag, &gv, &gl) != 0)
                        return X509V_ERR_PARSE;
                    if (gtag == ASN1_CONTEXT_PRIMITIVE(2)) {
                        /* dNSName -- copy into a bounded NUL-terminated buffer */
                        char name[256];
                        unsigned long n = gl;
                        saw_dns = 1;
                        if (n > sizeof name - 1) n = sizeof name - 1;
                        v_memcpy(name, gv, n);
                        name[n] = '\0';
                        if (x509_hostname_match(hostname, name))
                            return X509V_OK;
                    }
                    /* other GeneralName kinds (iPAddress, etc.) ignored */
                }
            }
        }
    }

    if (saw_dns) return X509V_ERR_HOSTNAME;   /* SAN present, none matched */

    /* No SAN dNSName -- fall back to subject CN via the confirmed helper. */
    {
        char cn[256];
        if (x509_get_subject_cn(leaf, leaf_len, cn, sizeof cn) != 0)
            return X509V_ERR_HOSTNAME;
        if (x509_hostname_match(hostname, cn)) return X509V_OK;
    }
    return X509V_ERR_HOSTNAME;
}

/* ====================================================================== */
/* CA trust store lookup                                                  */
/* ====================================================================== */

#include "ca_bundle.h"

/*
 * Find a trusted root whose subjectDN equals `issuer_dn` (raw DER bytes),
 * verify `top` (DER) is signed by that root, and check the root's validity.
 * Returns X509V_OK on success, or a negative X509V_ERR_*.
 */
static int verify_against_roots(const unsigned char *top, unsigned long top_len,
                                const unsigned char *issuer_dn,
                                unsigned long issuer_dn_len,
                                const char *now) {
    int count, i;
    int last_err = X509V_ERR_NO_ROOT;

    count = ca_get_count();
    for (i = 0; i < count; i++) {
        unsigned long root_len = 0;
        const unsigned char *root = ca_get_der(i, &root_len);
        const unsigned char *r_tbs, *r_sigalg, *r_sigbits;
        unsigned long r_tbs_len, r_sigalg_len, r_sigbits_len;
        asn1_cur r_tbs_cur;
        tbs_fields rf;
        int rc;

        if (!root || root_len == 0) continue;

        if (parse_cert_top(root, root_len, &r_tbs, &r_tbs_len,
                           &r_sigalg, &r_sigalg_len,
                           &r_sigbits, &r_sigbits_len, &r_tbs_cur) != 0)
            continue;
        if (parse_tbs_fields(r_tbs_cur, &rf) != 0)
            continue;

        /* subjectDN of root must equal issuerDN of the chain top. */
        if (rf.subject_dn_len != issuer_dn_len) continue;
        if (!v_memeq(rf.subject_dn, issuer_dn, issuer_dn_len)) continue;

        /* Candidate root found. The root must itself be time-valid. */
        rc = check_validity(&rf, now);
        if (rc != X509V_OK) { last_err = rc; continue; }

        /* Verify the chain top's signature with this root's public key. */
        rc = verify_cert_signed_by(top, top_len, rf.spki, rf.spki_len);
        if (rc == X509V_OK) return X509V_OK;
        last_err = rc;
        /* keep scanning -- another root with the same subject DN might match */
    }
    return last_err;
}

/* ====================================================================== */
/* Public: verify the whole chain                                          */
/* ====================================================================== */

int x509_verify_chain(const unsigned char *const *certs,
                      const unsigned long *lens, int ncerts,
                      const char *hostname,
                      const char *now_yyyymmddhhmmss) {
    tbs_fields f[X509V_MAX_CHAIN];
    int i, rc;

    if (!certs || !lens || !hostname) return X509V_ERR_ARGS;
    if (ncerts <= 0) return X509V_ERR_CHAIN_LEN;
    if (ncerts > X509V_MAX_CHAIN) return X509V_ERR_CHAIN_LEN;
    if (check_now_format(now_yyyymmddhhmmss) != 0) return X509V_ERR_TIME_FMT;

    /* Parse every cert's TBS fields once (issuer/subject DN, validity, spki,
     * extensions). This also rejects malformed certs early. */
    for (i = 0; i < ncerts; i++) {
        const unsigned char *tbs, *sigalg, *sigbits;
        unsigned long tbs_len, sigalg_len, sigbits_len;
        asn1_cur tbs_cur;
        if (!certs[i] || lens[i] == 0) return X509V_ERR_ARGS;
        if (parse_cert_top(certs[i], lens[i], &tbs, &tbs_len,
                           &sigalg, &sigalg_len,
                           &sigbits, &sigbits_len, &tbs_cur) != 0)
            return X509V_ERR_PARSE;
        if (parse_tbs_fields(tbs_cur, &f[i]) != 0)
            return X509V_ERR_PARSE;
    }

    /* 1. Every cert must be within its validity window. */
    for (i = 0; i < ncerts; i++) {
        rc = check_validity(&f[i], now_yyyymmddhhmmss);
        if (rc != X509V_OK) return rc;
    }

    /* 2. Chain linkage: each cert[i] signed by cert[i+1], and
     *    cert[i].issuerDN == cert[i+1].subjectDN (raw DER). */
    for (i = 0; i + 1 < ncerts; i++) {
        if (f[i].issuer_dn_len != f[i + 1].subject_dn_len ||
            !v_memeq(f[i].issuer_dn, f[i + 1].subject_dn, f[i].issuer_dn_len))
            return X509V_ERR_DN_MISMATCH;
        rc = verify_cert_signed_by(certs[i], lens[i],
                                   f[i + 1].spki, f[i + 1].spki_len);
        if (rc != X509V_OK) return rc;
    }

    /* 3. Trust anchor: the top cert (last in the server-supplied chain) must be
     *    issued by a root in the CA bundle whose subjectDN == top.issuerDN. The
     *    same code path handles ncerts == 1 (leaf issued directly by a root). */
    rc = verify_against_roots(certs[ncerts - 1], lens[ncerts - 1],
                              f[ncerts - 1].issuer_dn,
                              f[ncerts - 1].issuer_dn_len,
                              now_yyyymmddhhmmss);
    if (rc != X509V_OK) return rc;

    /* 4. Hostname: the LEAF must authenticate `hostname` (SAN, CN fallback). */
    rc = check_hostname(certs[0], lens[0], &f[0], hostname);
    if (rc != X509V_OK) return rc;

    return X509V_OK;
}

/* ====================================================================== */
/* Self-test                                                              */
/* ====================================================================== */

/*
 * The chain-level positive/negative tests would require embedding a full,
 * correctly-signed 2-cert chain plus a matching root with real RSA/ECDSA
 * signatures over the exact TBS bytes -- which cannot be authored by hand
 * without running a signing tool, so we do not embed a fake "valid" chain that
 * would not actually verify. Instead we exhaustively unit-test the parts that
 * ARE feasible to check in isolation:
 *
 *   - x509_hostname_match() across the required cases (exact, case-insensitive,
 *     valid wildcard match, and the two wildcard NON-matches), plus
 *   - the time normalisation + comparison used by the validity check (a sane
 *     `now` inside the window passes; an expired one fails), and
 *   - argument / chain-length guards on x509_verify_chain().
 *
 * Returns 0 on full pass, a distinct negative code per failed check.
 */
int x509_verify_selftest(void) {
    /* ---- hostname matching ---- */
    /* exact */
    if (!x509_hostname_match("example.com", "example.com")) return -1;
    /* case-insensitive */
    if (!x509_hostname_match("WWW.Example.COM", "www.example.com")) return -2;
    if (!x509_hostname_match("www.example.com", "WWW.EXAMPLE.COM")) return -3;
    /* valid wildcard: one left label */
    if (!x509_hostname_match("www.youtube.com", "*.youtube.com")) return -4;
    if (!x509_hostname_match("a.example.com", "*.example.com")) return -5;
    /* wildcard NON-match: no left label (apex) */
    if (x509_hostname_match("youtube.com", "*.youtube.com")) return -6;
    /* wildcard NON-match: more than one left label */
    if (x509_hostname_match("a.b.example.com", "*.example.com")) return -7;
    /* mismatched suffix */
    if (x509_hostname_match("www.evil.com", "*.example.com")) return -8;
    if (x509_hostname_match("example.org", "example.com")) return -9;
    /* empties never match */
    if (x509_hostname_match("", "example.com")) return -10;
    if (x509_hostname_match("example.com", "")) return -11;
    if (x509_hostname_match(0, "example.com")) return -12;

    /* ---- time normalisation + window compare ---- */
    {
        char out[14];
        int i;
        const char *exp;
        /* UTCTime "260101000000Z" -> "20260101000000" (YY<50 => 20YY) */
        if (normalise_time(ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME,
                           (const unsigned char *)"260101000000Z", 13, out) != 0)
            return -13;
        exp = "20260101000000";
        for (i = 0; i < 14; i++) if (out[i] != exp[i]) return -14;

        /* UTCTime "960101000000Z" -> "19960101000000" (YY>=50 => 19YY) */
        if (normalise_time(ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME,
                           (const unsigned char *)"960101000000Z", 13, out) != 0)
            return -15;
        exp = "19960101000000";
        for (i = 0; i < 14; i++) if (out[i] != exp[i]) return -16;

        /* GeneralizedTime passthrough */
        if (normalise_time(ASN1_CLASS_UNIVERSAL | ASN1_TAG_GENERALIZEDTIME,
                           (const unsigned char *)"20300101120000Z", 15,
                           out) != 0)
            return -17;
        exp = "20300101120000";
        for (i = 0; i < 14; i++) if (out[i] != exp[i]) return -18;

        /* malformed time rejected */
        if (normalise_time(ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME,
                           (const unsigned char *)"2601", 4, out) == 0)
            return -19;
    }

    /* validity window compare via check_validity */
    {
        tbs_fields f;
        v_memset(&f, 0, sizeof f);
        f.nb_tag = ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME;
        f.nb_val = (const unsigned char *)"260101000000Z"; f.nb_len = 13;
        f.na_tag = ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME;
        f.na_val = (const unsigned char *)"360101000000Z"; f.na_len = 13;
        /* now inside window */
        if (check_validity(&f, "20300615120000") != X509V_OK) return -20;
        /* now before notBefore -> expired/not-yet */
        if (check_validity(&f, "20250101000000") == X509V_OK) return -21;
        /* now after notAfter -> expired */
        if (check_validity(&f, "20400101000000") == X509V_OK) return -22;
    }

    /* ---- argument / length guards ---- */
    {
        const unsigned char dummy = 0x30;
        const unsigned char *cp = &dummy;
        unsigned long dl = 1;
        if (x509_verify_chain(0, &dl, 1, "x", "20300101000000")
                != X509V_ERR_ARGS) return -23;
        if (x509_verify_chain(&cp, &dl, 0, "x", "20300101000000")
                != X509V_ERR_CHAIN_LEN) return -24;
        if (x509_verify_chain(&cp, &dl, X509V_MAX_CHAIN + 1, "x",
                              "20300101000000") != X509V_ERR_CHAIN_LEN)
            return -25;
        /* bad `now` */
        if (x509_verify_chain(&cp, &dl, 1, "x", "nope")
                != X509V_ERR_TIME_FMT) return -26;
        if (x509_verify_chain(&cp, &dl, 1, "x", 0)
                != X509V_ERR_TIME_FMT) return -27;
    }

    return 0;
}
