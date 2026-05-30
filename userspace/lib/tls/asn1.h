/*
 * asn1.h -- freestanding minimal DER (ASN.1 Distinguished Encoding Rules) reader.
 * ==============================================================================
 *
 * Pure computation: NO libc, NO syscalls, NO malloc, NO standard headers.
 * Everything operates on a caller-provided, immutable byte buffer. The reader
 * never copies or allocates; helpers return pointers and lengths into the
 * original DER buffer. Bounds are checked against an explicit `end` pointer on
 * every read, so a malformed or truncated certificate can never cause a read
 * past the buffer.
 *
 * This is intentionally tiny: it implements just enough DER to walk an X.509
 * certificate and pull out the RSA SubjectPublicKeyInfo (see x509.h). It is a
 * READER only -- there is no encoder.
 *
 * Cursor model
 * ------------
 * An `asn1_cur` is a half-open range [p, end). asn1_get_tlv() reads one
 * Tag-Length-Value at `p`, hands back the tag and a view of the value bytes,
 * and advances `p` past the whole TLV so the next call reads the following
 * element at the same nesting level. To descend into a constructed element
 * (SEQUENCE / SET / context-tagged) use asn1_enter(), which produces a new
 * cursor scoped to that element's contents.
 *
 * Typical walk:
 *     asn1_cur top = { der, der + len };
 *     asn1_cur cert;
 *     if (asn1_enter(&top, ASN1_SEQUENCE, &cert)) error;   // Certificate
 *     asn1_cur tbs;
 *     if (asn1_enter(&cert, ASN1_SEQUENCE, &tbs)) error;   // tbsCertificate
 *     ... walk fields of tbs ...
 *
 * All functions return 0 on success and non-zero on any error (malformed
 * encoding, length overrun, unexpected tag, etc.).
 */

#ifndef TLS_ASN1_H
#define TLS_ASN1_H

/* ---- ASN.1 universal tag numbers (low 5 bits) and class/PC bits ------- */

#define ASN1_TAG_BOOLEAN          0x01
#define ASN1_TAG_INTEGER          0x02
#define ASN1_TAG_BIT_STRING       0x03
#define ASN1_TAG_OCTET_STRING     0x04
#define ASN1_TAG_NULL             0x05
#define ASN1_TAG_OID              0x06
#define ASN1_TAG_UTF8STRING       0x0C
#define ASN1_TAG_PRINTABLESTRING  0x13
#define ASN1_TAG_TELETEXSTRING    0x14
#define ASN1_TAG_IA5STRING        0x16
#define ASN1_TAG_UTCTIME          0x17
#define ASN1_TAG_GENERALIZEDTIME  0x18
#define ASN1_TAG_SEQUENCE         0x10  /* on the wire as 0x30 (constructed) */
#define ASN1_TAG_SET              0x11  /* on the wire as 0x31 (constructed) */

/* Class / primitive-constructed bits in the identifier octet. */
#define ASN1_CLASS_UNIVERSAL      0x00
#define ASN1_CLASS_APPLICATION    0x40
#define ASN1_CLASS_CONTEXT        0x80
#define ASN1_CLASS_PRIVATE        0xC0
#define ASN1_CONSTRUCTED          0x20

/* Full identifier octets for the structures we actually navigate. The whole
 * (one-byte) identifier is returned by asn1_get_tlv() in *tag, so callers
 * compare against these complete values, e.g. (*tag == ASN1_SEQUENCE). */
#define ASN1_SEQUENCE   (ASN1_CLASS_UNIVERSAL | ASN1_CONSTRUCTED | ASN1_TAG_SEQUENCE) /* 0x30 */
#define ASN1_SET        (ASN1_CLASS_UNIVERSAL | ASN1_CONSTRUCTED | ASN1_TAG_SET)      /* 0x31 */
#define ASN1_INTEGER    (ASN1_CLASS_UNIVERSAL | ASN1_TAG_INTEGER)                     /* 0x02 */
#define ASN1_BIT_STRING (ASN1_CLASS_UNIVERSAL | ASN1_TAG_BIT_STRING)                  /* 0x03 */
#define ASN1_OID        (ASN1_CLASS_UNIVERSAL | ASN1_TAG_OID)                         /* 0x06 */
#define ASN1_NULL       (ASN1_CLASS_UNIVERSAL | ASN1_TAG_NULL)                        /* 0x05 */

/* Context-tagged [n] EXPLICIT (constructed) identifiers, e.g. [0] version. */
#define ASN1_CONTEXT_CONSTRUCTED(n) \
    ((unsigned char)(ASN1_CLASS_CONTEXT | ASN1_CONSTRUCTED | ((n) & 0x1F)))
#define ASN1_CONTEXT_PRIMITIVE(n) \
    ((unsigned char)(ASN1_CLASS_CONTEXT | ((n) & 0x1F)))

/* ---- cursor ----------------------------------------------------------- */

typedef struct {
    const unsigned char *p;   /* next byte to read                         */
    const unsigned char *end; /* one-past-the-last readable byte           */
} asn1_cur;

/*
 * Read one TLV at the cursor. On success:
 *   *tag  receives the (single-octet) identifier byte,
 *   *val  receives a pointer to the first content byte,
 *   *vlen receives the content length in bytes,
 * and the cursor is advanced past the entire TLV. Any of tag/val/vlen may be
 * NULL if the caller does not need that output. Returns 0 on success, non-zero
 * if the buffer is exhausted or the length encoding is malformed/overruns end.
 *
 * Only single-octet identifiers are supported (tag numbers 0..30), which covers
 * everything appearing in an X.509 certificate. Indefinite-length form (0x80)
 * is rejected (DER forbids it).
 */
int asn1_get_tlv(asn1_cur *c, int *tag, const unsigned char **val,
                 unsigned long *vlen);

/*
 * Like asn1_get_tlv() but does NOT advance the cursor -- it peeks at the next
 * element. Useful for OPTIONAL fields where the tag decides whether to consume.
 * Returns 0 on success, non-zero on error.
 */
int asn1_peek_tlv(const asn1_cur *c, int *tag, const unsigned char **val,
                  unsigned long *vlen);

/*
 * Read the next TLV and verify its identifier equals `want`. On a match the
 * value view is returned and the cursor advanced (same semantics as
 * asn1_get_tlv). Returns 0 on success, non-zero on mismatch or error.
 */
int asn1_expect(asn1_cur *c, int want, const unsigned char **val,
                unsigned long *vlen);

/*
 * Descend into a constructed element of identifier `want` (e.g. ASN1_SEQUENCE).
 * Reads the TLV at the cursor, verifies the tag, advances the parent cursor
 * past it, and writes a sub-cursor spanning just that element's contents into
 * *inner. Returns 0 on success, non-zero on tag mismatch or bounds error.
 */
int asn1_enter(asn1_cur *c, int want, asn1_cur *inner);

/*
 * Skip exactly one TLV at the cursor regardless of its tag (the cursor is
 * advanced past it). Returns 0 on success, non-zero on error.
 */
int asn1_skip(asn1_cur *c);

/* Non-zero if the cursor has no more bytes to read. */
int asn1_at_end(const asn1_cur *c);

/*
 * Read an INTEGER and return a view of its raw content bytes (big-endian,
 * including any leading 0x00 sign byte). Returns 0 on success.
 */
int asn1_get_integer(asn1_cur *c, const unsigned char **val,
                     unsigned long *vlen);

/*
 * Read an OBJECT IDENTIFIER and return a view of its encoded content bytes
 * (the packed sub-identifier bytes, NOT including tag/length). Returns 0 on
 * success.
 */
int asn1_get_oid(asn1_cur *c, const unsigned char **val, unsigned long *vlen);

/*
 * Read a BIT STRING and return a view of its payload AFTER the leading
 * "unused bits" octet. *unused (may be NULL) receives that octet's value.
 * For DER the unused-bits count is 0 for the byte-aligned strings we parse.
 * Returns 0 on success, non-zero on error (e.g. empty BIT STRING).
 */
int asn1_get_bitstring(asn1_cur *c, const unsigned char **val,
                       unsigned long *vlen, int *unused);

/*
 * Compare an OID content view against an expected packed-byte template.
 * Returns 1 if equal, 0 otherwise. (Pure byte compare; no allocation.)
 */
int asn1_oid_equals(const unsigned char *oid, unsigned long oid_len,
                    const unsigned char *want, unsigned long want_len);

#endif /* TLS_ASN1_H */
