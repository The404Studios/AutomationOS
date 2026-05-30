/*
 * x509.c -- freestanding X.509 RSA public-key extractor.
 * ======================================================
 * No libc, no headers, no allocation. Walks a DER certificate with the minimal
 * reader in asn1.c and pulls out the RSA modulus and exponent. See x509.h.
 *
 * Robustness: every descent and every read goes through the bounds-checked
 * asn1_* helpers, which never read past the cursor's `end`. A malformed,
 * truncated, or hostile certificate yields a non-zero error return, never an
 * out-of-bounds access.
 */

#include "asn1.h"
#include "x509.h"

/* ---- local memory helpers (no libc) ---------------------------------- */
static void x509_memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}
static void x509_memset(void *d, int v, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--) *dp++ = (unsigned char)v;
}

/* rsaEncryption: 1.2.840.113549.1.1.1 */
static const unsigned char OID_RSA_ENCRYPTION[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01
};

/* commonName: 2.5.4.3 */
static const unsigned char OID_COMMON_NAME[3] = { 0x55, 0x04, 0x03 };

/* sha256WithRSAEncryption: 1.2.840.113549.1.1.11 */
static const unsigned char OID_SHA256_RSA[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B
};

/* sha1WithRSAEncryption: 1.2.840.113549.1.1.5 */
static const unsigned char OID_SHA1_RSA[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x05
};

/* sha384WithRSAEncryption: 1.2.840.113549.1.1.12 */
static const unsigned char OID_SHA384_RSA[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C
};

/* sha512WithRSAEncryption: 1.2.840.113549.1.1.13 */
static const unsigned char OID_SHA512_RSA[9] = {
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

/* id-ecPublicKey: 1.2.840.10045.2.1 */
static const unsigned char OID_EC_PUBLIC_KEY[7] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01
};

/* id-ce-subjectAltName: 2.5.29.17 */
static const unsigned char OID_SAN[3] = { 0x55, 0x1D, 0x11 };

/* ---- INTEGER copy with leading-zero strip ---------------------------- */
/*
 * Copy a DER INTEGER content view (big-endian, may carry a leading 0x00 sign
 * byte) into the caller's buffer with any single leading 0x00 removed. Returns
 * 0 on success, non-zero if it does not fit `cap`.
 */
static int copy_integer_be(const unsigned char *v, unsigned long vlen,
                           unsigned char *out, unsigned long *out_len,
                           unsigned long cap) {
    /* Strip leading zero bytes. DER uses at most one for the sign, but tolerate
     * more defensively. Keep one byte if the value is all zeros. */
    while (vlen > 1 && v[0] == 0x00) {
        v++;
        vlen--;
    }
    if (vlen > cap) return -1;
    x509_memcpy(out, v, vlen);
    *out_len = vlen;
    return 0;
}

/* ---- SPKI -> RSA modulus/exponent ------------------------------------ */

int x509_spki_extract_rsa(const unsigned char *spki, unsigned long len,
                          unsigned char *mod, unsigned long *mod_len,
                          unsigned char *exp, unsigned long *exp_len) {
    asn1_cur top, spki_seq, algid, rsapub;
    const unsigned char *oid;
    unsigned long oid_len;
    const unsigned char *bits;
    unsigned long bits_len;
    int unused;
    const unsigned char *iv;
    unsigned long il;

    if (!spki || !mod || !mod_len || !exp || !exp_len) return -1;

    top.p = spki;
    top.end = spki + len;

    /* SubjectPublicKeyInfo ::= SEQUENCE { algorithm, subjectPublicKey } */
    if (asn1_enter(&top, ASN1_SEQUENCE, &spki_seq) != 0) return -2;

    /* algorithm AlgorithmIdentifier ::= SEQUENCE { OID, params } */
    if (asn1_enter(&spki_seq, ASN1_SEQUENCE, &algid) != 0) return -3;
    if (asn1_get_oid(&algid, &oid, &oid_len) != 0) return -4;
    if (!asn1_oid_equals(oid, oid_len,
                         OID_RSA_ENCRYPTION, sizeof OID_RSA_ENCRYPTION)) {
        /* Not an RSA key (e.g. EC) -- unsupported. */
        return -5;
    }
    /* Remaining algid params (NULL for RSA) are ignored. */

    /* subjectPublicKey BIT STRING -- payload is DER of RSAPublicKey. */
    if (asn1_get_bitstring(&spki_seq, &bits, &bits_len, &unused) != 0) return -6;
    if (unused != 0) return -7;   /* RSAPublicKey DER is byte-aligned */

    /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
    rsapub.p = bits;
    rsapub.end = bits + bits_len;
    {
        asn1_cur rsaseq;
        if (asn1_enter(&rsapub, ASN1_SEQUENCE, &rsaseq) != 0) return -8;

        /* modulus */
        if (asn1_get_integer(&rsaseq, &iv, &il) != 0) return -9;
        if (copy_integer_be(iv, il, mod, mod_len, 512) != 0) return -10;

        /* publicExponent */
        if (asn1_get_integer(&rsaseq, &iv, &il) != 0) return -11;
        if (copy_integer_be(iv, il, exp, exp_len, 512) != 0) return -12;
    }

    return 0;
}

/* ---- full certificate -> SPKI ---------------------------------------- */

/*
 * Navigate Certificate -> tbsCertificate -> subjectPublicKeyInfo, returning a
 * cursor positioned at the SPKI SEQUENCE TLV (i.e. ready for asn1_enter with
 * ASN1_SEQUENCE) via *spki_out. Returns 0 on success.
 *
 * tbsCertificate fields in order:
 *   [0] version  (OPTIONAL, EXPLICIT -- context-constructed tag 0xA0)
 *   serialNumber INTEGER
 *   signature    AlgorithmIdentifier (SEQUENCE)
 *   issuer       Name (SEQUENCE)
 *   validity     Validity (SEQUENCE)
 *   subject      Name (SEQUENCE)
 *   subjectPublicKeyInfo  SEQUENCE   <-- target
 */
static int navigate_to_spki(const unsigned char *der, unsigned long len,
                            asn1_cur *spki_out) {
    asn1_cur top, cert, tbs;
    int tag;

    if (!der || !spki_out) return -1;

    top.p = der;
    top.end = der + len;

    /* Certificate ::= SEQUENCE */
    if (asn1_enter(&top, ASN1_SEQUENCE, &cert) != 0) return -2;

    /* tbsCertificate ::= SEQUENCE */
    if (asn1_enter(&cert, ASN1_SEQUENCE, &tbs) != 0) return -3;

    /* Optional [0] version: skip if present. */
    if (asn1_peek_tlv(&tbs, &tag, (const unsigned char **)0,
                      (unsigned long *)0) != 0)
        return -4;
    if (tag == ASN1_CONTEXT_CONSTRUCTED(0)) {
        if (asn1_skip(&tbs) != 0) return -5;   /* version */
    }

    if (asn1_skip(&tbs) != 0) return -6;   /* serialNumber */
    if (asn1_skip(&tbs) != 0) return -7;   /* signature AlgorithmIdentifier */
    if (asn1_skip(&tbs) != 0) return -8;   /* issuer */
    if (asn1_skip(&tbs) != 0) return -9;   /* validity */
    if (asn1_skip(&tbs) != 0) return -10;  /* subject */

    /* Next element is subjectPublicKeyInfo. Hand back the cursor positioned at
     * it so the caller can enter the SPKI SEQUENCE. */
    *spki_out = tbs;
    return 0;
}

int x509_extract_pubkey(const unsigned char *der, unsigned long len,
                        unsigned char *mod, unsigned long *mod_len,
                        unsigned char *exp, unsigned long *exp_len) {
    asn1_cur spki_cur;
    const unsigned char *spki_tlv;
    unsigned long spki_len;
    asn1_cur probe;
    const unsigned char *inner_val;
    unsigned long inner_len;
    int inner_tag;

    if (!der || !mod || !mod_len || !exp || !exp_len) return -1;

    /*
     * Accept either a full Certificate or a bare SubjectPublicKeyInfo. We
     * distinguish by peeking inside the outermost SEQUENCE: an SPKI's first
     * inner element is an AlgorithmIdentifier (SEQUENCE) whose first element is
     * an OID; a Certificate's first inner element is tbsCertificate, also a
     * SEQUENCE, but its first element is [0] version or an INTEGER serial --
     * never an OID. So: enter outer SEQUENCE, enter first inner SEQUENCE, and
     * if that begins with an OID we are looking at an SPKI directly.
     */
    probe.p = der;
    probe.end = der + len;
    if (asn1_enter(&probe, ASN1_SEQUENCE, &spki_cur) == 0) {
        asn1_cur first;
        asn1_cur save = spki_cur;
        if (asn1_enter(&spki_cur, ASN1_SEQUENCE, &first) == 0) {
            if (asn1_peek_tlv(&first, &inner_tag, &inner_val, &inner_len) == 0 &&
                inner_tag == ASN1_OID) {
                /* Looks like SPKI's AlgorithmIdentifier -> treat whole input
                 * as SPKI directly. */
                return x509_spki_extract_rsa(der, len, mod, mod_len,
                                             exp, exp_len);
            }
        }
        (void)save;
    }

    /* Otherwise treat as a full certificate. */
    if (navigate_to_spki(der, len, &spki_cur) != 0) return -20;

    /* spki_cur is positioned at the SPKI SEQUENCE TLV. Grab that TLV's whole
     * extent (tag..end of value) and re-parse it through the SPKI path. */
    {
        const unsigned char *tlv_start = spki_cur.p;
        if (asn1_peek_tlv(&spki_cur, &inner_tag, &spki_tlv, &spki_len) != 0)
            return -21;
        if (inner_tag != ASN1_SEQUENCE) return -22;
        /* Whole TLV spans from tag start through end of content. */
        return x509_spki_extract_rsa(tlv_start,
                                     (unsigned long)((spki_tlv + spki_len)
                                                     - tlv_start),
                                     mod, mod_len, exp, exp_len);
    }
}

/* ---- subject CN ------------------------------------------------------- */

/*
 * Search a Name (RDNSequence) cursor for the first commonName attribute and
 * copy its string value into out (NUL-terminated). Returns 0 if found.
 *
 * Name ::= SEQUENCE OF RelativeDistinguishedName
 * RelativeDistinguishedName ::= SET OF AttributeTypeAndValue
 * AttributeTypeAndValue ::= SEQUENCE { type OID, value ANY }
 */
static int find_cn_in_name(asn1_cur *name_seq, char *out, unsigned long out_cap) {
    while (!asn1_at_end(name_seq)) {
        asn1_cur rdn;
        if (asn1_enter(name_seq, ASN1_SET, &rdn) != 0) return -1;
        while (!asn1_at_end(&rdn)) {
            asn1_cur atav;
            const unsigned char *oid, *val;
            unsigned long oid_len, val_len;
            int vtag;
            if (asn1_enter(&rdn, ASN1_SEQUENCE, &atav) != 0) return -1;
            if (asn1_get_oid(&atav, &oid, &oid_len) != 0) return -1;
            /* value is the next TLV: a directory string of some flavour. */
            if (asn1_get_tlv(&atav, &vtag, &val, &val_len) != 0) return -1;
            if (asn1_oid_equals(oid, oid_len,
                                OID_COMMON_NAME, sizeof OID_COMMON_NAME)) {
                unsigned long n = val_len;
                if (out_cap == 0) return -1;
                if (n > out_cap - 1) n = out_cap - 1;
                x509_memcpy(out, val, n);
                out[n] = '\0';
                return 0;
            }
        }
    }
    return -1;   /* no CN found */
}

int x509_get_subject_cn(const unsigned char *der, unsigned long len,
                        char *out, unsigned long out_cap) {
    asn1_cur top, cert, tbs, subject;
    int tag;

    if (out && out_cap >= 1) out[0] = '\0';
    if (!der || !out || out_cap == 0) return -1;

    top.p = der;
    top.end = der + len;

    if (asn1_enter(&top, ASN1_SEQUENCE, &cert) != 0) return -1;
    if (asn1_enter(&cert, ASN1_SEQUENCE, &tbs) != 0) return -1;

    /* optional [0] version */
    if (asn1_peek_tlv(&tbs, &tag, (const unsigned char **)0,
                      (unsigned long *)0) != 0)
        return -1;
    if (tag == ASN1_CONTEXT_CONSTRUCTED(0)) {
        if (asn1_skip(&tbs) != 0) return -1;
    }
    if (asn1_skip(&tbs) != 0) return -1;   /* serial */
    if (asn1_skip(&tbs) != 0) return -1;   /* signature */
    if (asn1_skip(&tbs) != 0) return -1;   /* issuer */
    if (asn1_skip(&tbs) != 0) return -1;   /* validity */

    /* subject Name ::= SEQUENCE */
    if (asn1_enter(&tbs, ASN1_SEQUENCE, &subject) != 0) return -1;

    return find_cn_in_name(&subject, out, out_cap);
}

/* ---- validity --------------------------------------------------------- */

static int copy_time_string(const unsigned char *v, unsigned long vlen,
                            char *out, unsigned long cap) {
    unsigned long n = vlen;
    if (!out || cap == 0) return 0;   /* skip silently if no buffer */
    if (n > cap - 1) n = cap - 1;
    x509_memcpy(out, v, n);
    out[n] = '\0';
    return 0;
}

int x509_get_validity(const unsigned char *der, unsigned long len,
                      char *not_before, unsigned long nb_cap,
                      char *not_after, unsigned long na_cap) {
    asn1_cur top, cert, tbs, validity;
    int tag;
    const unsigned char *v;
    unsigned long vl;

    if (not_before && nb_cap >= 1) not_before[0] = '\0';
    if (not_after && na_cap >= 1) not_after[0] = '\0';
    if (!der) return -1;

    top.p = der;
    top.end = der + len;

    if (asn1_enter(&top, ASN1_SEQUENCE, &cert) != 0) return -1;
    if (asn1_enter(&cert, ASN1_SEQUENCE, &tbs) != 0) return -1;

    if (asn1_peek_tlv(&tbs, &tag, (const unsigned char **)0,
                      (unsigned long *)0) != 0)
        return -1;
    if (tag == ASN1_CONTEXT_CONSTRUCTED(0)) {
        if (asn1_skip(&tbs) != 0) return -1;
    }
    if (asn1_skip(&tbs) != 0) return -1;   /* serial */
    if (asn1_skip(&tbs) != 0) return -1;   /* signature */
    if (asn1_skip(&tbs) != 0) return -1;   /* issuer */

    /* validity ::= SEQUENCE { notBefore Time, notAfter Time } */
    if (asn1_enter(&tbs, ASN1_SEQUENCE, &validity) != 0) return -1;

    /* notBefore: UTCTime or GeneralizedTime. */
    if (asn1_get_tlv(&validity, &tag, &v, &vl) != 0) return -1;
    if (tag != (ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME) &&
        tag != (ASN1_CLASS_UNIVERSAL | ASN1_TAG_GENERALIZEDTIME))
        return -1;
    copy_time_string(v, vl, not_before, nb_cap);

    /* notAfter */
    if (asn1_get_tlv(&validity, &tag, &v, &vl) != 0) return -1;
    if (tag != (ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME) &&
        tag != (ASN1_CLASS_UNIVERSAL | ASN1_TAG_GENERALIZEDTIME))
        return -1;
    copy_time_string(v, vl, not_after, na_cap);

    return 0;
}

/* ====================================================================== */
/* Internal shared navigation helpers                                     */
/* ====================================================================== */

/*
 * Navigate to the start of tbsCertificate's content, returning a cursor that
 * spans the tbs content.  Also optionally returns the pointer to the TBS TLV's
 * first byte (the 0x30 tag) and the full TLV length (tag+length octets +
 * content) so callers can produce a raw view of the signed region.
 *
 * tbs_content_cur : populated with a cursor over the tbs content bytes
 * tbs_tlv_start   : if non-NULL, set to the address of the TBS tag byte
 * tbs_tlv_len     : if non-NULL, set to the byte count of the whole TLV
 */
static int navigate_tbs(const unsigned char *der, unsigned long len,
                        asn1_cur *tbs_content_cur,
                        const unsigned char **tbs_tlv_start,
                        unsigned long *tbs_tlv_len) {
    asn1_cur top, cert;
    const unsigned char *tbs_tag_ptr;
    const unsigned char *tbs_val;
    unsigned long tbs_vlen;

    if (!der || !tbs_content_cur) return -1;

    top.p = der;
    top.end = der + len;

    if (asn1_enter(&top, ASN1_SEQUENCE, &cert) != 0) return -2;

    /* Remember where the TBS TLV starts. */
    tbs_tag_ptr = cert.p;

    /* Read the TBS SEQUENCE TLV (consumes tag+length+content from cert). */
    if (asn1_expect(&cert, ASN1_SEQUENCE, &tbs_val, &tbs_vlen) != 0) return -3;

    if (tbs_tlv_start) *tbs_tlv_start = tbs_tag_ptr;
    if (tbs_tlv_len)   *tbs_tlv_len   = (unsigned long)(cert.p - tbs_tag_ptr);

    tbs_content_cur->p   = tbs_val;
    tbs_content_cur->end = tbs_val + tbs_vlen;
    return 0;
}

/*
 * Skip the optional [0] version, then skip serialNumber, signature-alg,
 * and the fields up to (and optionally including) a target field index.
 *
 * Field indices inside tbsCertificate (after optional version):
 *   0  serialNumber
 *   1  signature AlgorithmIdentifier
 *   2  issuer
 *   3  validity
 *   4  subject
 *   5  subjectPublicKeyInfo
 *   6  [3] extensions (OPTIONAL, context-constructed tag 3)
 *
 * On entry *tbs is a cursor over the tbs content. On success *tbs is
 * positioned at the start of field index `target`. `skip_version` is set
 * by the caller (always 1 for the fields above). Returns 0 on success.
 */
static int tbs_advance_to(asn1_cur *tbs, int target) {
    int tag;
    int i;

    /* Optional [0] EXPLICIT version */
    if (asn1_peek_tlv(tbs, &tag,
                      (const unsigned char **)0, (unsigned long *)0) != 0)
        return -1;
    if (tag == ASN1_CONTEXT_CONSTRUCTED(0)) {
        if (asn1_skip(tbs) != 0) return -1;
    }

    /* Skip fields 0 .. target-1 */
    for (i = 0; i < target; i++) {
        if (asn1_skip(tbs) != 0) return -1;
    }
    return 0;
}

/* ====================================================================== */
/* x509_get_tbs                                                           */
/* ====================================================================== */

int x509_get_tbs(const unsigned char *der, unsigned long len,
                 const unsigned char **tbs, unsigned long *tbslen) {
    asn1_cur tbs_cur;
    const unsigned char *tlv_start;
    unsigned long tlv_len;

    if (!der || !tbs || !tbslen) return -1;
    if (navigate_tbs(der, len, &tbs_cur, &tlv_start, &tlv_len) != 0) return -2;

    *tbs    = tlv_start;
    *tbslen = tlv_len;
    return 0;
}

/* ====================================================================== */
/* x509_get_signature                                                     */
/* ====================================================================== */

/*
 * Map an OID content view to one of the X509_SIGALG_* constants.
 */
static int map_sigalg_oid(const unsigned char *oid, unsigned long olen) {
    if (asn1_oid_equals(oid, olen, OID_SHA256_RSA, sizeof OID_SHA256_RSA))
        return X509_SIGALG_RSA_PKCS1_SHA256;
    if (asn1_oid_equals(oid, olen, OID_SHA1_RSA, sizeof OID_SHA1_RSA))
        return X509_SIGALG_RSA_PKCS1_SHA1;
    if (asn1_oid_equals(oid, olen, OID_SHA384_RSA, sizeof OID_SHA384_RSA))
        return X509_SIGALG_RSA_PKCS1_SHA384;
    if (asn1_oid_equals(oid, olen, OID_SHA512_RSA, sizeof OID_SHA512_RSA))
        return X509_SIGALG_RSA_PKCS1_SHA512;
    if (asn1_oid_equals(oid, olen, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256))
        return X509_SIGALG_ECDSA_SHA256;
    if (asn1_oid_equals(oid, olen, OID_ECDSA_SHA384, sizeof OID_ECDSA_SHA384))
        return X509_SIGALG_ECDSA_SHA384;
    return X509_SIGALG_UNKNOWN;
}

int x509_get_signature(const unsigned char *der, unsigned long len,
                       const unsigned char **sig, unsigned long *siglen,
                       int *sigalg) {
    asn1_cur top, cert, algid_seq;
    const unsigned char *bits;
    unsigned long bits_len;
    const unsigned char *oid;
    unsigned long oid_len;
    int tag;

    if (!der || !sig || !siglen || !sigalg) return -1;

    top.p = der;
    top.end = der + len;

    if (asn1_enter(&top, ASN1_SEQUENCE, &cert) != 0) return -2;

    /* Skip tbsCertificate */
    if (asn1_skip(&cert) != 0) return -3;

    /* signatureAlgorithm AlgorithmIdentifier */
    if (asn1_enter(&cert, ASN1_SEQUENCE, &algid_seq) != 0) return -4;
    if (asn1_get_oid(&algid_seq, &oid, &oid_len) != 0) return -5;
    *sigalg = map_sigalg_oid(oid, oid_len);

    /* signatureValue BIT STRING */
    if (asn1_get_bitstring(&cert, &bits, &bits_len, &tag) != 0) return -6;
    /* tag here is the unused-bits count; for RSA/ECDSA it must be 0 */
    if (tag != 0) return -7;

    *sig    = bits;
    *siglen = bits_len;
    return 0;
}

/* ====================================================================== */
/* x509_ecdsa_sig_to_rs                                                   */
/* ====================================================================== */

/*
 * Copy a DER INTEGER content (big-endian, may have leading 0x00 sign byte)
 * into a fixed 32-byte right-justified big-endian buffer (zero-padded on the
 * left). Returns 0 on success, non-zero if the magnitude exceeds 32 bytes.
 */
static int copy_int_to_fixed32(const unsigned char *v, unsigned long vlen,
                               unsigned char out[32]) {
    unsigned long i;
    /* Strip leading sign/padding zeros. */
    while (vlen > 1 && v[0] == 0x00) { v++; vlen--; }
    if (vlen > 32) return -1;
    /* Zero-fill the output, then right-justify. */
    for (i = 0; i < 32; i++) out[i] = 0;
    x509_memcpy(out + (32 - vlen), v, vlen);
    return 0;
}

int x509_ecdsa_sig_to_rs(const unsigned char *sig, unsigned long siglen,
                         unsigned char r[32], unsigned char s[32]) {
    asn1_cur cur, seq;
    const unsigned char *iv;
    unsigned long il;

    if (!sig || !r || !s) return -1;

    cur.p   = sig;
    cur.end = sig + siglen;

    /* SEQUENCE { INTEGER r, INTEGER s } */
    if (asn1_enter(&cur, ASN1_SEQUENCE, &seq) != 0) return -2;

    if (asn1_get_integer(&seq, &iv, &il) != 0) return -3;
    if (copy_int_to_fixed32(iv, il, r) != 0) return -4;

    if (asn1_get_integer(&seq, &iv, &il) != 0) return -5;
    if (copy_int_to_fixed32(iv, il, s) != 0) return -6;

    return 0;
}

/* ====================================================================== */
/* x509_get_issuer_dn / x509_get_subject_dn                              */
/* ====================================================================== */

/*
 * Return a raw DER view (pointer + byte count) of the Name SEQUENCE TLV at
 * field index `field_idx` inside tbsCertificate.  The pointer points into the
 * original der buffer.
 *
 * field_idx: 2 = issuer, 4 = subject  (zero-based, after version is skipped)
 */
static int get_name_dn(const unsigned char *der, unsigned long len,
                       int field_idx,
                       const unsigned char **dn, unsigned long *dnlen) {
    asn1_cur tbs;
    const unsigned char *name_tag_ptr;
    const unsigned char *nval;
    unsigned long nvlen;

    if (!der || !dn || !dnlen) return -1;
    if (navigate_tbs(der, len, &tbs, (const unsigned char **)0,
                     (unsigned long *)0) != 0)
        return -2;

    if (tbs_advance_to(&tbs, field_idx) != 0) return -3;

    /* Record where the Name TLV tag byte is. */
    name_tag_ptr = tbs.p;

    /* Consume the SEQUENCE TLV to advance cursor past it. */
    if (asn1_expect(&tbs, ASN1_SEQUENCE, &nval, &nvlen) != 0) return -4;

    /* The full TLV spans from name_tag_ptr to tbs.p (post-advance). */
    *dn    = name_tag_ptr;
    *dnlen = (unsigned long)(tbs.p - name_tag_ptr);
    return 0;
}

int x509_get_issuer_dn(const unsigned char *der, unsigned long len,
                       const unsigned char **dn, unsigned long *dnlen) {
    return get_name_dn(der, len, 2, dn, dnlen);
}

int x509_get_subject_dn(const unsigned char *der, unsigned long len,
                        const unsigned char **dn, unsigned long *dnlen) {
    return get_name_dn(der, len, 4, dn, dnlen);
}

/* ====================================================================== */
/* x509_get_san_dns                                                       */
/* ====================================================================== */

/*
 * Walk the [3] EXPLICIT extensions wrapper looking for the SubjectAltName
 * extension (OID 2.5.29.17), then iterate its GeneralNames SEQUENCE collecting
 * [2] IMPLICIT dNSName entries.
 *
 * Extension ::= SEQUENCE { extnID OID, critical BOOLEAN OPTIONAL,
 *                           extnValue OCTET STRING }
 * The extnValue OCTET STRING wraps the extension-specific DER value.
 * For SAN that inner value is:
 *   GeneralNames ::= SEQUENCE OF GeneralName
 *   GeneralName  ::= CHOICE { ... dNSName [2] IA5String ... }
 */
int x509_get_san_dns(const unsigned char *der, unsigned long len,
                     char names[][256], int max) {
    asn1_cur tbs, exts_ctx, exts_seq;
    int tag;
    int count = 0;

    if (!der) return -1;

    if (navigate_tbs(der, len, &tbs,
                     (const unsigned char **)0, (unsigned long *)0) != 0)
        return -1;

    /* Advance past version(opt), serial(0), sigalg(1), issuer(2),
     * validity(3), subject(4), spki(5) -- that is 6 fields. */
    if (tbs_advance_to(&tbs, 6) != 0) return 0;  /* no extensions possible */

    /* Check for [3] EXPLICIT extensions. */
    if (asn1_at_end(&tbs)) return 0;
    if (asn1_peek_tlv(&tbs, &tag,
                      (const unsigned char **)0, (unsigned long *)0) != 0)
        return 0;
    if (tag != ASN1_CONTEXT_CONSTRUCTED(3)) return 0;  /* no extensions */

    /* Enter [3] wrapper, then the SEQUENCE OF Extension. */
    if (asn1_enter(&tbs, ASN1_CONTEXT_CONSTRUCTED(3), &exts_ctx) != 0)
        return 0;
    if (asn1_enter(&exts_ctx, ASN1_SEQUENCE, &exts_seq) != 0) return 0;

    /* Walk each Extension SEQUENCE. */
    while (!asn1_at_end(&exts_seq)) {
        asn1_cur ext_seq;
        const unsigned char *oid;
        unsigned long oid_len;
        const unsigned char *oct_val;
        unsigned long oct_vlen;
        int ext_tag;

        if (asn1_enter(&exts_seq, ASN1_SEQUENCE, &ext_seq) != 0) return -1;

        /* extnID OID */
        if (asn1_get_oid(&ext_seq, &oid, &oid_len) != 0) return -1;

        if (!asn1_oid_equals(oid, oid_len, OID_SAN, sizeof OID_SAN)) {
            /* Not SAN -- skip the rest of this Extension. */
            continue;
        }

        /* Found SAN. Skip optional critical BOOLEAN if present. */
        if (!asn1_at_end(&ext_seq)) {
            if (asn1_peek_tlv(&ext_seq, &ext_tag,
                              (const unsigned char **)0,
                              (unsigned long *)0) == 0) {
                if (ext_tag == (ASN1_CLASS_UNIVERSAL | ASN1_TAG_BOOLEAN)) {
                    if (asn1_skip(&ext_seq) != 0) return -1;
                }
            }
        }

        /* extnValue OCTET STRING wrapping the GeneralNames SEQUENCE. */
        if (asn1_expect(&ext_seq, ASN1_CLASS_UNIVERSAL | ASN1_TAG_OCTET_STRING,
                        &oct_val, &oct_vlen) != 0) return -1;

        /* Descend into the OCTET STRING payload as a new cursor. */
        {
            asn1_cur gen_names;
            asn1_cur gen_seq;

            gen_names.p   = oct_val;
            gen_names.end = oct_val + oct_vlen;

            if (asn1_enter(&gen_names, ASN1_SEQUENCE, &gen_seq) != 0) return -1;

            while (!asn1_at_end(&gen_seq) && (max < 0 || count < max)) {
                const unsigned char *gval;
                unsigned long gvlen;
                int gtag;

                if (asn1_get_tlv(&gen_seq, &gtag, &gval, &gvlen) != 0)
                    return -1;

                /* [2] IMPLICIT IA5String = dNSName */
                if (gtag == ASN1_CONTEXT_PRIMITIVE(2) && names) {
                    unsigned long n = gvlen;
                    if (n > 255) n = 255;
                    x509_memcpy(names[count], gval, n);
                    names[count][n] = '\0';
                    count++;
                }
            }
        }
        /* SAN found and processed; no need to keep searching. */
        break;
    }

    return count;
}

/* ====================================================================== */
/* x509_pubkey_alg                                                        */
/* ====================================================================== */

int x509_pubkey_alg(const unsigned char *der, unsigned long len, int *alg) {
    asn1_cur spki_cur, spki_seq, algid;
    const unsigned char *oid;
    unsigned long oid_len;

    if (!der || !alg) return -1;

    if (navigate_to_spki(der, len, &spki_cur) != 0) return -2;

    /* Enter SubjectPublicKeyInfo SEQUENCE */
    if (asn1_enter(&spki_cur, ASN1_SEQUENCE, &spki_seq) != 0) return -3;

    /* AlgorithmIdentifier SEQUENCE */
    if (asn1_enter(&spki_seq, ASN1_SEQUENCE, &algid) != 0) return -4;

    if (asn1_get_oid(&algid, &oid, &oid_len) != 0) return -5;

    if (asn1_oid_equals(oid, oid_len,
                        OID_RSA_ENCRYPTION, sizeof OID_RSA_ENCRYPTION)) {
        *alg = 0;
    } else if (asn1_oid_equals(oid, oid_len,
                               OID_EC_PUBLIC_KEY, sizeof OID_EC_PUBLIC_KEY)) {
        *alg = 1;
    } else {
        *alg = -1;
    }
    return 0;
}

/* ====================================================================== */
/* x509_get_ec_pubkey                                                     */
/* ====================================================================== */

int x509_get_ec_pubkey(const unsigned char *der, unsigned long len,
                       unsigned char point65[65]) {
    asn1_cur spki_cur, spki_seq, algid;
    const unsigned char *oid;
    unsigned long oid_len;
    const unsigned char *bits;
    unsigned long bits_len;
    int unused;
    unsigned long i;

    if (!der || !point65) return -1;

    if (navigate_to_spki(der, len, &spki_cur) != 0) return -2;

    if (asn1_enter(&spki_cur, ASN1_SEQUENCE, &spki_seq) != 0) return -3;

    /* Verify algorithm is id-ecPublicKey */
    if (asn1_enter(&spki_seq, ASN1_SEQUENCE, &algid) != 0) return -4;
    if (asn1_get_oid(&algid, &oid, &oid_len) != 0) return -5;
    if (!asn1_oid_equals(oid, oid_len,
                         OID_EC_PUBLIC_KEY, sizeof OID_EC_PUBLIC_KEY))
        return -6;

    /* subjectPublicKey BIT STRING -- payload is the EC point. */
    if (asn1_get_bitstring(&spki_seq, &bits, &bits_len, &unused) != 0)
        return -7;
    if (unused != 0) return -8;

    /* Require exactly 65 bytes (uncompressed 0x04 || X || Y, P-256). */
    if (bits_len != 65) return -9;
    if (bits[0] != 0x04) return -10;

    for (i = 0; i < 65; i++) point65[i] = bits[i];
    return 0;
}

/* ====================================================================== */
/* x509_get_validity_utc                                                  */
/* ====================================================================== */

/*
 * Convert a UTCTime or GeneralizedTime content view to a 14-character
 * "YYYYMMDDHHMMSS" string (NUL-terminated in a 15-byte buffer).
 *
 * UTCTime format (RFC 5280):  YYMMDDHHMMSS[Z]
 *   If YY >= 50 -> 19YY, else 20YY.
 * GeneralizedTime format:     YYYYMMDDHHMMSS[Z...]
 *   First 14 chars copied directly.
 */
static int convert_time_to_utc14(const unsigned char *v, unsigned long vlen,
                                  int is_utc, char out[15]) {
    unsigned long i;
    if (is_utc) {
        /* UTCTime: at least 11 chars YYMMDDHHMMSS (without 'Z') */
        if (vlen < 12) return -1;
        /* 2-digit year */
        int yy = (int)((v[0] - '0') * 10 + (v[1] - '0'));
        int century = (yy >= 50) ? 19 : 20;
        out[0] = (char)('0' + century / 10);
        out[1] = (char)('0' + century % 10);
        out[2] = v[0];
        out[3] = v[1];
        /* Copy MMDDHHMMSS (10 chars from v[2..11]) */
        for (i = 0; i < 10; i++) out[4 + i] = (char)v[2 + i];
        out[14] = '\0';
    } else {
        /* GeneralizedTime: at least 14 chars YYYYMMDDHHMMSS */
        if (vlen < 14) return -1;
        for (i = 0; i < 14; i++) out[i] = (char)v[i];
        out[14] = '\0';
    }
    return 0;
}

int x509_get_validity_utc(const unsigned char *der, unsigned long len,
                          char not_before[15], char not_after[15]) {
    asn1_cur tbs, validity;
    const unsigned char *v;
    unsigned long vl;
    int tag;

    if (!der) return -1;

    if (navigate_tbs(der, len, &tbs,
                     (const unsigned char **)0, (unsigned long *)0) != 0)
        return -2;

    /* Skip version(opt), serial(0), sigalg(1), issuer(2) -- 3 fields. */
    if (tbs_advance_to(&tbs, 3) != 0) return -3;

    /* validity ::= SEQUENCE { notBefore Time, notAfter Time } */
    if (asn1_enter(&tbs, ASN1_SEQUENCE, &validity) != 0) return -4;

    /* notBefore */
    if (asn1_get_tlv(&validity, &tag, &v, &vl) != 0) return -5;
    if (tag == (int)(ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME)) {
        if (not_before) {
            if (convert_time_to_utc14(v, vl, 1, not_before) != 0) return -6;
        }
    } else if (tag == (int)(ASN1_CLASS_UNIVERSAL | ASN1_TAG_GENERALIZEDTIME)) {
        if (not_before) {
            if (convert_time_to_utc14(v, vl, 0, not_before) != 0) return -6;
        }
    } else {
        return -7;
    }

    /* notAfter */
    if (asn1_get_tlv(&validity, &tag, &v, &vl) != 0) return -8;
    if (tag == (int)(ASN1_CLASS_UNIVERSAL | ASN1_TAG_UTCTIME)) {
        if (not_after) {
            if (convert_time_to_utc14(v, vl, 1, not_after) != 0) return -9;
        }
    } else if (tag == (int)(ASN1_CLASS_UNIVERSAL | ASN1_TAG_GENERALIZEDTIME)) {
        if (not_after) {
            if (convert_time_to_utc14(v, vl, 0, not_after) != 0) return -9;
        }
    } else {
        return -10;
    }

    return 0;
}

/* ====================================================================== */
/* Self-test                                                              */
/* ====================================================================== */

/*
 * Embedded v3 self-signed Certificate with a SubjectAltName extension.
 * Exercises x509_get_tbs, x509_get_issuer_dn/subject_dn, x509_get_signature,
 * x509_get_san_dns, and x509_get_validity_utc. Uses the same RSA key as
 * X509_SELFTEST_CERT. Subject CN = issuer CN = "server.automationos.test".
 * signatureAlgorithm: sha256WithRSAEncryption (sigalg enum 0).
 * SAN dNSName: "server.automationos.test".
 * Validity: UTCTime notBefore=260101000000Z, notAfter=360101000000Z.
 * tbsCertificate is 264 bytes (TLV at offset 4, content at offset 8).
 */
static const unsigned char X509_SELFTEST_CERT2[350] = {
    0x30, 0x82, 0x01, 0x5a, 0x30, 0x82, 0x01, 0x04, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x01, 0x01, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
    0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x1f, 0x31, 0x1d, 0x30,
    0x1b, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x14, 0x41, 0x75, 0x74, 0x6f,
    0x6d, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x4f, 0x53, 0x20, 0x54, 0x65, 0x73,
    0x74, 0x20, 0x43, 0x41, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x36, 0x30, 0x31,
    0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x17, 0x0d, 0x33,
    0x36, 0x30, 0x31, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a,
    0x30, 0x23, 0x31, 0x21, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c,
    0x18, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0x2e, 0x61, 0x75, 0x74, 0x6f,
    0x6d, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x6f, 0x73, 0x2e, 0x74, 0x65, 0x73,
    0x74, 0x30, 0x5c, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
    0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x4b, 0x00, 0x30, 0x48, 0x02,
    0x41, 0x00, 0x89, 0x9c, 0xe8, 0xa5, 0xfa, 0x69, 0x53, 0x30, 0x65, 0x9a,
    0x4f, 0x6a, 0x74, 0x5e, 0xf2, 0xce, 0xa0, 0x7e, 0x75, 0x98, 0x05, 0x63,
    0xbf, 0xb4, 0x5b, 0x70, 0x07, 0xed, 0x0f, 0x1f, 0x58, 0x3a, 0xe1, 0x46,
    0xb4, 0xd1, 0x93, 0x42, 0x67, 0x8d, 0x80, 0xda, 0xe5, 0xa7, 0x3a, 0xc2,
    0x83, 0x2d, 0xb9, 0xa0, 0x86, 0xe1, 0x17, 0x46, 0x86, 0x68, 0xdd, 0xe4,
    0x77, 0x48, 0xaa, 0xf7, 0xf6, 0x46, 0x02, 0x03, 0x01, 0x00, 0x01, 0xa3,
    0x27, 0x30, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55, 0x1d, 0x11, 0x04, 0x1c,
    0x30, 0x1a, 0x82, 0x18, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0x2e, 0x61,
    0x75, 0x74, 0x6f, 0x6d, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x6f, 0x73, 0x2e,
    0x74, 0x65, 0x73, 0x74, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
    0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x03, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

/*
 * Embedded DER SubjectPublicKeyInfo for a 512-bit RSA key (modulus top bit set
 * so the INTEGER carries a 0x00 sign byte that must be stripped). Exercises the
 * pure SPKI path. Generated deterministically; see x509_selftest().
 *
 * Layout:
 *   30 5C                                  SEQUENCE (SPKI)
 *     30 0D                                  SEQUENCE (AlgorithmIdentifier)
 *       06 09 2A 86 48 86 F7 0D 01 01 01     OID rsaEncryption
 *       05 00                                NULL
 *     03 4B 00                              BIT STRING (0 unused bits)
 *       30 48                                SEQUENCE (RSAPublicKey)
 *         02 41 00 <64 bytes>                INTEGER modulus (0x00 + 64 magnitude)
 *         02 03 01 00 01                     INTEGER exponent 65537
 */
static const unsigned char X509_SELFTEST_SPKI[94] = {
    0x30, 0x5c, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
    0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x4b, 0x00, 0x30, 0x48, 0x02, 0x41,
    0x00, 0x89, 0x9c, 0xe8, 0xa5, 0xfa, 0x69, 0x53, 0x30, 0x65, 0x9a, 0x4f,
    0x6a, 0x74, 0x5e, 0xf2, 0xce, 0xa0, 0x7e, 0x75, 0x98, 0x05, 0x63, 0xbf,
    0xb4, 0x5b, 0x70, 0x07, 0xed, 0x0f, 0x1f, 0x58, 0x3a, 0xe1, 0x46, 0xb4,
    0xd1, 0x93, 0x42, 0x67, 0x8d, 0x80, 0xda, 0xe5, 0xa7, 0x3a, 0xc2, 0x83,
    0x2d, 0xb9, 0xa0, 0x86, 0xe1, 0x17, 0x46, 0x86, 0x68, 0xdd, 0xe4, 0x77,
    0x48, 0xaa, 0xf7, 0xf6, 0x46, 0x02, 0x03, 0x01, 0x00, 0x01,
};

/*
 * Embedded minimal self-signed Certificate wrapping the same key. Exercises the
 * full Certificate -> tbsCertificate -> SPKI walk including [0] version, serial,
 * issuer/subject Names, UTCTime validity, and the multi-byte (0x82 / 0x81) DER
 * length forms in the outer/tbs lengths. Subject CN = "server.automationos.test".
 */
static const unsigned char X509_SELFTEST_CERT[310] = {
    0x30, 0x82, 0x01, 0x32, 0x30, 0x81, 0xdd, 0xa0, 0x03, 0x02, 0x01, 0x02,
    0x02, 0x03, 0x13, 0x37, 0x42, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48,
    0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x1f, 0x31, 0x1d,
    0x30, 0x1b, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x14, 0x41, 0x75, 0x74,
    0x6f, 0x6d, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x4f, 0x53, 0x20, 0x54, 0x65,
    0x73, 0x74, 0x20, 0x43, 0x41, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x36, 0x30,
    0x31, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x17, 0x0d,
    0x33, 0x36, 0x30, 0x31, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x5a, 0x30, 0x23, 0x31, 0x21, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x04, 0x03,
    0x0c, 0x18, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0x2e, 0x61, 0x75, 0x74,
    0x6f, 0x6d, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x6f, 0x73, 0x2e, 0x74, 0x65,
    0x73, 0x74, 0x30, 0x5c, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
    0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x4b, 0x00, 0x30, 0x48,
    0x02, 0x41, 0x00, 0x89, 0x9c, 0xe8, 0xa5, 0xfa, 0x69, 0x53, 0x30, 0x65,
    0x9a, 0x4f, 0x6a, 0x74, 0x5e, 0xf2, 0xce, 0xa0, 0x7e, 0x75, 0x98, 0x05,
    0x63, 0xbf, 0xb4, 0x5b, 0x70, 0x07, 0xed, 0x0f, 0x1f, 0x58, 0x3a, 0xe1,
    0x46, 0xb4, 0xd1, 0x93, 0x42, 0x67, 0x8d, 0x80, 0xda, 0xe5, 0xa7, 0x3a,
    0xc2, 0x83, 0x2d, 0xb9, 0xa0, 0x86, 0xe1, 0x17, 0x46, 0x86, 0x68, 0xdd,
    0xe4, 0x77, 0x48, 0xaa, 0xf7, 0xf6, 0x46, 0x02, 0x03, 0x01, 0x00, 0x01,
    0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
    0x0b, 0x05, 0x00, 0x03, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Expected modulus magnitude (after sign-byte strip), first/last bytes. */
static const unsigned char SELFTEST_MOD_FIRST4[4] = { 0x89, 0x9c, 0xe8, 0xa5 };
static const unsigned char SELFTEST_MOD_LAST4[4]  = { 0xaa, 0xf7, 0xf6, 0x46 };

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int x509_selftest(void) {
    unsigned char mod[512];
    unsigned char exp[512];
    unsigned long mod_len, exp_len;
    int rc;
    int i;

    /* ---- 1. Bare SPKI path ---- */
    x509_memset(mod, 0, sizeof mod);
    x509_memset(exp, 0, sizeof exp);
    mod_len = exp_len = 0;
    rc = x509_extract_pubkey(X509_SELFTEST_SPKI, sizeof X509_SELFTEST_SPKI,
                             mod, &mod_len, exp, &exp_len);
    if (rc != 0) return -1;
    if (mod_len != 64) return -2;                 /* 0x00 sign byte stripped  */
    for (i = 0; i < 4; i++)
        if (mod[i] != SELFTEST_MOD_FIRST4[i]) return -3;
    for (i = 0; i < 4; i++)
        if (mod[64 - 4 + i] != SELFTEST_MOD_LAST4[i]) return -4;
    if (exp_len != 3) return -5;
    if (exp[0] != 0x01 || exp[1] != 0x00 || exp[2] != 0x01) return -6;

    /* ---- 2. Also test the dedicated SPKI entry point ---- */
    x509_memset(mod, 0, sizeof mod);
    mod_len = exp_len = 0;
    rc = x509_spki_extract_rsa(X509_SELFTEST_SPKI, sizeof X509_SELFTEST_SPKI,
                               mod, &mod_len, exp, &exp_len);
    if (rc != 0) return -7;
    if (mod_len != 64) return -8;

    /* ---- 3. Full certificate path ---- */
    x509_memset(mod, 0, sizeof mod);
    x509_memset(exp, 0, sizeof exp);
    mod_len = exp_len = 0;
    rc = x509_extract_pubkey(X509_SELFTEST_CERT, sizeof X509_SELFTEST_CERT,
                             mod, &mod_len, exp, &exp_len);
    if (rc != 0) return -9;
    if (mod_len != 64) return -10;
    for (i = 0; i < 4; i++)
        if (mod[i] != SELFTEST_MOD_FIRST4[i]) return -11;
    if (exp_len != 3 || exp[0] != 0x01 || exp[1] != 0x00 || exp[2] != 0x01)
        return -12;

    /* ---- 4. Subject CN extraction ---- */
    {
        char cn[64];
        rc = x509_get_subject_cn(X509_SELFTEST_CERT, sizeof X509_SELFTEST_CERT,
                                 cn, sizeof cn);
        if (rc != 0) return -13;
        if (!str_eq(cn, "server.automationos.test")) return -14;
    }

    /* ---- 5. Validity dates (raw UTCTime strings) ---- */
    {
        char nb[32], na[32];
        rc = x509_get_validity(X509_SELFTEST_CERT, sizeof X509_SELFTEST_CERT,
                               nb, sizeof nb, na, sizeof na);
        if (rc != 0) return -15;
        if (!str_eq(nb, "260101000000Z")) return -16;
        if (!str_eq(na, "360101000000Z")) return -17;
    }

    /* ---- 6. Robustness: truncated input must fail gracefully ---- */
    {
        unsigned long n;
        for (n = 0; n < sizeof X509_SELFTEST_SPKI; n++) {
            mod_len = exp_len = 0;
            rc = x509_extract_pubkey(X509_SELFTEST_SPKI, n,
                                     mod, &mod_len, exp, &exp_len);
            if (rc == 0) return -18;   /* truncation must never "succeed" */
        }
    }

    /* ================================================================
     * Extended tests using X509_SELFTEST_CERT (no extensions, v3-like)
     * and X509_SELFTEST_CERT2 (v3 with SAN extension).
     * ================================================================ */

    /* ---- 7. x509_get_tbs on CERT1 ---- */
    {
        const unsigned char *tbs;
        unsigned long tbslen;
        rc = x509_get_tbs(X509_SELFTEST_CERT, sizeof X509_SELFTEST_CERT,
                          &tbs, &tbslen);
        if (rc != 0) return -19;
        /* TBS must be non-empty and must point inside the cert buffer. */
        if (tbslen == 0) return -20;
        if (tbs < X509_SELFTEST_CERT) return -21;
        if (tbs + tbslen > X509_SELFTEST_CERT + sizeof X509_SELFTEST_CERT)
            return -22;
        /* TBS TLV starts with a SEQUENCE tag (0x30). */
        if (tbs[0] != 0x30) return -23;
    }

    /* ---- 8. x509_get_tbs on CERT2: check expected offset and length ---- */
    {
        const unsigned char *tbs;
        unsigned long tbslen;
        rc = x509_get_tbs(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                          &tbs, &tbslen);
        if (rc != 0) return -24;
        /* Outer cert has 4-byte TL header -> TBS TLV starts at offset 4. */
        if (tbs != X509_SELFTEST_CERT2 + 4) return -25;
        /* TBS TLV length = 4 (hdr) + 260 (content) = 264. */
        if (tbslen != 264) return -26;
    }

    /* ---- 9. x509_get_issuer_dn on CERT2 ---- */
    {
        const unsigned char *dn;
        unsigned long dnlen;
        rc = x509_get_issuer_dn(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                                &dn, &dnlen);
        if (rc != 0) return -27;
        if (dnlen == 0) return -28;
        /* Must point into the cert buffer. */
        if (dn < X509_SELFTEST_CERT2) return -29;
        if (dn + dnlen > X509_SELFTEST_CERT2 + sizeof X509_SELFTEST_CERT2)
            return -30;
        /* Issuer is a SEQUENCE (0x30). */
        if (dn[0] != 0x30) return -31;
    }

    /* ---- 10. x509_get_subject_dn on CERT2 ---- */
    {
        const unsigned char *dn;
        unsigned long dnlen;
        rc = x509_get_subject_dn(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                                 &dn, &dnlen);
        if (rc != 0) return -32;
        if (dnlen == 0) return -33;
        if (dn[0] != 0x30) return -34;
    }

    /* ---- 11. x509_get_validity_utc on CERT2 ---- */
    {
        char nb[15], na[15];
        rc = x509_get_validity_utc(X509_SELFTEST_CERT2,
                                   sizeof X509_SELFTEST_CERT2, nb, na);
        if (rc != 0) return -35;
        /* UTCTime "260101000000Z": year 26 < 50 -> 2026 */
        if (!str_eq(nb, "20260101000000")) return -36;
        if (!str_eq(na, "20360101000000")) return -37;
    }

    /* ---- 12. x509_get_validity_utc on CERT1 (same dates) ---- */
    {
        char nb[15], na[15];
        rc = x509_get_validity_utc(X509_SELFTEST_CERT,
                                   sizeof X509_SELFTEST_CERT, nb, na);
        if (rc != 0) return -38;
        if (!str_eq(nb, "20260101000000")) return -39;
        if (!str_eq(na, "20360101000000")) return -40;
    }

    /* ---- 13. x509_get_signature on CERT2 ---- */
    {
        const unsigned char *sig;
        unsigned long siglen;
        int sigalg;
        rc = x509_get_signature(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                                &sig, &siglen, &sigalg);
        if (rc != 0) return -41;
        /* Signature is the 64 zero-byte dummy payload. */
        if (siglen == 0) return -42;
        /* OID is sha256WithRSAEncryption -> enum 0. */
        if (sigalg != X509_SIGALG_RSA_PKCS1_SHA256) return -43;
        /* Sig must point inside the buffer. */
        if (sig < X509_SELFTEST_CERT2) return -44;
        if (sig + siglen > X509_SELFTEST_CERT2 + sizeof X509_SELFTEST_CERT2)
            return -45;
    }

    /* ---- 14. x509_pubkey_alg on CERT2 (RSA) ---- */
    {
        int alg;
        rc = x509_pubkey_alg(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                             &alg);
        if (rc != 0) return -46;
        if (alg != 0) return -47;  /* 0 = RSA */
    }

    /* ---- 15. x509_get_san_dns on CERT2 ---- */
    {
        char san[4][256];
        rc = x509_get_san_dns(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                              san, 4);
        /* Must have found exactly 1 dNSName entry. */
        if (rc != 1) return -48;
        if (!str_eq(san[0], "server.automationos.test")) return -49;
    }

    /* ---- 16. x509_get_san_dns on CERT1 (no extensions) ---- */
    {
        char san[4][256];
        rc = x509_get_san_dns(X509_SELFTEST_CERT, sizeof X509_SELFTEST_CERT,
                              san, 4);
        /* CERT1 has no extensions -> 0 entries, not an error. */
        if (rc < 0) return -50;
        if (rc != 0) return -51;
    }

    /* ---- 17. x509_get_ec_pubkey on RSA cert must fail ---- */
    {
        unsigned char pt[65];
        rc = x509_get_ec_pubkey(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                                pt);
        if (rc == 0) return -52;   /* must fail: this is an RSA cert */
    }

    /* ---- 18. Chain-matching check: CERT2's issuer DN == CERT1's issuer DN,
     *          and CERT2's subject DN != CERT2's issuer DN (leaf cert). ---- */
    {
        const unsigned char *issuer2, *subject2, *issuer1;
        unsigned long issuer2_len, subject2_len, issuer1_len;
        unsigned long k;

        rc = x509_get_issuer_dn(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                                &issuer2, &issuer2_len);
        if (rc != 0) return -53;
        rc = x509_get_subject_dn(X509_SELFTEST_CERT2, sizeof X509_SELFTEST_CERT2,
                                 &subject2, &subject2_len);
        if (rc != 0) return -54;

        /* The leaf cert's issuer must differ from its own subject. */
        if (issuer2_len == subject2_len) {
            int same = 1;
            for (k = 0; k < issuer2_len; k++)
                if (issuer2[k] != subject2[k]) { same = 0; break; }
            if (same) return -55;   /* issuer == subject would be wrong */
        }

        /* CERT1 and CERT2 share the same issuer DN (both signed by the CA). */
        rc = x509_get_issuer_dn(X509_SELFTEST_CERT, sizeof X509_SELFTEST_CERT,
                                &issuer1, &issuer1_len);
        if (rc != 0) return -56;
        if (issuer1_len != issuer2_len) return -57;
        for (k = 0; k < issuer1_len; k++)
            if (issuer1[k] != issuer2[k]) return -58;
    }

    return 0;
}
