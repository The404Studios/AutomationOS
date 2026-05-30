/*
 * asn1.c -- freestanding minimal DER reader.
 * ==========================================
 * No libc, no headers, no allocation. Every read is bounds-checked against the
 * cursor's `end`. See asn1.h for the contract.
 *
 * DER length encoding handled:
 *   - short form:  0x00..0x7F           (length is the byte itself)
 *   - long form:   0x81 LL              (1 length byte follows)
 *                  0x82 LL LL           (2 length bytes follow)
 *                  0x83 LL LL LL        (3 length bytes follow)
 *                  0x84 LL LL LL LL     (4 length bytes follow)
 *   - indefinite (0x80) is rejected -- DER forbids it.
 *
 * Only single-octet identifier tags (tag number <= 30) are accepted; the
 * high-tag-number form (low 5 bits == 0x1F) is rejected. Nothing in an X.509
 * certificate needs more than that.
 */

#include "asn1.h"

/* ---- local memory helper (no libc) ----------------------------------- */
static int asn1_memeq(const unsigned char *a, const unsigned char *b,
                      unsigned long n) {
    while (n--) {
        if (*a++ != *b++) return 0;
    }
    return 1;
}

/*
 * Decode a DER length starting at *pp (which must be < end). On success
 * advances *pp past the length octets and stores the value in *out. Returns 0
 * on success, non-zero on malformed/overrunning length.
 */
static int asn1_read_len(const unsigned char **pp, const unsigned char *end,
                         unsigned long *out) {
    const unsigned char *p = *pp;
    unsigned char first;
    unsigned int nbytes, i;
    unsigned long len;

    if (p >= end) return -1;
    first = *p++;

    if ((first & 0x80) == 0) {
        /* short form: value is the byte itself (0..127) */
        *out = (unsigned long)first;
        *pp = p;
        return 0;
    }

    /* long form: low 7 bits = number of subsequent length octets */
    nbytes = (unsigned int)(first & 0x7F);

    /* 0x80 == indefinite length: not allowed in DER. */
    if (nbytes == 0) return -1;

    /* Cap at 4 length octets; we never parse anything 4 GiB+. This also keeps
     * the accumulation inside an unsigned long on a 64-bit target without
     * overflow concerns. */
    if (nbytes > 4) return -1;

    /* Must have all length octets available. */
    if ((unsigned long)(end - p) < (unsigned long)nbytes) return -1;

    len = 0;
    for (i = 0; i < nbytes; i++) {
        len = (len << 8) | (unsigned long)(*p++);
    }

    *out = len;
    *pp = p;
    return 0;
}

/*
 * Core TLV reader shared by peek and consume. If `advance` is non-NULL it
 * receives the position just past the whole TLV (so the caller can advance its
 * cursor). The value view is written to val/vlen and the identifier to tag.
 */
static int asn1_read_tlv(const unsigned char *p, const unsigned char *end,
                         int *tag, const unsigned char **val,
                         unsigned long *vlen, const unsigned char **advance) {
    unsigned char id;
    unsigned long len;
    const unsigned char *content;

    if (p >= end) return -1;

    id = *p++;

    /* Reject high-tag-number form (low 5 bits all set). We only support the
     * single-octet identifiers that appear in X.509. */
    if ((id & 0x1F) == 0x1F) return -1;

    if (asn1_read_len(&p, end, &len) != 0) return -1;

    /* The content must fit within the remaining buffer. */
    if ((unsigned long)(end - p) < len) return -1;

    content = p;

    if (tag)  *tag  = (int)id;
    if (val)  *val  = content;
    if (vlen) *vlen = len;
    if (advance) *advance = content + len;
    return 0;
}

int asn1_get_tlv(asn1_cur *c, int *tag, const unsigned char **val,
                 unsigned long *vlen) {
    const unsigned char *next;
    if (!c) return -1;
    if (asn1_read_tlv(c->p, c->end, tag, val, vlen, &next) != 0) return -1;
    c->p = next;
    return 0;
}

int asn1_peek_tlv(const asn1_cur *c, int *tag, const unsigned char **val,
                  unsigned long *vlen) {
    if (!c) return -1;
    return asn1_read_tlv(c->p, c->end, tag, val, vlen, (const unsigned char **)0);
}

int asn1_expect(asn1_cur *c, int want, const unsigned char **val,
                unsigned long *vlen) {
    int tag;
    asn1_cur tmp;
    const unsigned char *v;
    unsigned long l;

    if (!c) return -1;
    tmp = *c;
    if (asn1_get_tlv(&tmp, &tag, &v, &l) != 0) return -1;
    if (tag != want) return -1;

    *c = tmp;            /* commit advance only on a match */
    if (val)  *val  = v;
    if (vlen) *vlen = l;
    return 0;
}

int asn1_enter(asn1_cur *c, int want, asn1_cur *inner) {
    const unsigned char *v;
    unsigned long l;

    if (!c || !inner) return -1;
    if (asn1_expect(c, want, &v, &l) != 0) return -1;

    inner->p   = v;
    inner->end = v + l;
    return 0;
}

int asn1_skip(asn1_cur *c) {
    return asn1_get_tlv(c, (int *)0, (const unsigned char **)0,
                        (unsigned long *)0);
}

int asn1_at_end(const asn1_cur *c) {
    if (!c) return 1;
    return c->p >= c->end;
}

int asn1_get_integer(asn1_cur *c, const unsigned char **val,
                     unsigned long *vlen) {
    return asn1_expect(c, ASN1_INTEGER, val, vlen);
}

int asn1_get_oid(asn1_cur *c, const unsigned char **val, unsigned long *vlen) {
    return asn1_expect(c, ASN1_OID, val, vlen);
}

int asn1_get_bitstring(asn1_cur *c, const unsigned char **val,
                       unsigned long *vlen, int *unused) {
    const unsigned char *v;
    unsigned long l;

    if (asn1_expect(c, ASN1_BIT_STRING, &v, &l) != 0) return -1;

    /* A BIT STRING always begins with the "number of unused bits" octet. */
    if (l < 1) return -1;

    if (unused) *unused = (int)v[0];
    if (val)    *val    = v + 1;
    if (vlen)   *vlen   = l - 1;
    return 0;
}

int asn1_oid_equals(const unsigned char *oid, unsigned long oid_len,
                    const unsigned char *want, unsigned long want_len) {
    if (oid_len != want_len) return 0;
    return asn1_memeq(oid, want, oid_len);
}
