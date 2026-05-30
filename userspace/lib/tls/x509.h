/*
 * x509.h -- freestanding X.509 certificate parser (RSA public-key extraction).
 * ============================================================================
 *
 * Pure computation: NO libc, NO syscalls, NO malloc, NO standard headers.
 * Operates on a caller-provided DER buffer using the minimal DER reader in
 * asn1.h. Nothing is allocated; outputs are written into caller-provided fixed
 * buffers.
 *
 * Why this exists
 * ---------------
 * A TLS 1.2 client receives the server's certificate chain (DER-encoded X.509)
 * in the Certificate handshake message. To perform RSA key exchange the client
 * needs the server's RSA PUBLIC KEY -- the modulus (n) and public exponent (e)
 * -- which live in the certificate's subjectPublicKeyInfo. This module walks
 * the cert and extracts exactly that.
 *
 * Scope: best-effort. Supports certificates whose SPKI algorithm is
 * rsaEncryption (OID 1.2.840.113549.1.1.1) carrying an
 *     RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }.
 * It does NOT verify signatures, check validity dates, or validate the chain;
 * that is the caller's / TLS layer's responsibility.
 *
 * Structure being parsed (RFC 5280):
 *
 *   Certificate ::= SEQUENCE {
 *       tbsCertificate       TBSCertificate,
 *       signatureAlgorithm   AlgorithmIdentifier,
 *       signatureValue       BIT STRING }
 *
 *   TBSCertificate ::= SEQUENCE {
 *       [0] version          (EXPLICIT, OPTIONAL, default v1),
 *       serialNumber         INTEGER,
 *       signature            AlgorithmIdentifier,
 *       issuer               Name,
 *       validity             Validity,
 *       subject              Name,
 *       subjectPublicKeyInfo SubjectPublicKeyInfo,
 *       ... }
 *
 *   SubjectPublicKeyInfo ::= SEQUENCE {
 *       algorithm            AlgorithmIdentifier,
 *       subjectPublicKey     BIT STRING }     -- DER of RSAPublicKey
 */

#ifndef TLS_X509_H
#define TLS_X509_H

/*
 * Extract the RSA public key from a DER X.509 certificate.
 *
 *   der/len        the certificate, DER-encoded.
 *   mod / mod_len  on success, mod[0..*mod_len) holds the big-endian modulus
 *                  with any leading 0x00 sign byte stripped. Caller supplies
 *                  the buffer (>= 512 bytes recommended for up to 4096-bit
 *                  keys). *mod_len is set to the byte count written.
 *   exp / exp_len  on success, exp[0..*exp_len) holds the big-endian public
 *                  exponent (typically 3 bytes: 01 00 01). Caller supplies the
 *                  buffer (16 bytes is ample). *exp_len is the byte count.
 *
 * Returns 0 on success. Non-zero on any parse error, unsupported algorithm
 * (non-RSA SPKI), or if a value does not fit the provided buffer. On failure
 * the output buffers/lengths are left in an unspecified state.
 *
 * The function also accepts a bare DER-encoded SubjectPublicKeyInfo (i.e. an
 * SPKI SEQUENCE whose first element is an AlgorithmIdentifier) in place of a
 * full certificate -- handy for the TLS layer and for testing.
 */
int x509_extract_pubkey(const unsigned char *der, unsigned long len,
                        unsigned char *mod, unsigned long *mod_len,
                        unsigned char *exp, unsigned long *exp_len);

/*
 * Extract the RSA public key from a bare DER SubjectPublicKeyInfo (not a full
 * certificate). Same output contract as x509_extract_pubkey(). Exposed so the
 * TLS layer can reuse it; x509_extract_pubkey() falls back to this path.
 */
int x509_spki_extract_rsa(const unsigned char *spki, unsigned long len,
                          unsigned char *mod, unsigned long *mod_len,
                          unsigned char *exp, unsigned long *exp_len);

/*
 * Convenience: copy the subject Common Name (CN, OID 2.5.4.3) into out as a
 * NUL-terminated string (truncated to out_cap-1 chars). Best-effort: searches
 * the subject Name's RDNSequence for the first CN AttributeTypeAndValue.
 * Returns 0 on success (CN found and copied), non-zero otherwise. out_cap must
 * be >= 1; on any non-zero return out[0] is set to '\0' when out_cap >= 1.
 */
int x509_get_subject_cn(const unsigned char *der, unsigned long len,
                        char *out, unsigned long out_cap);

/*
 * Convenience: copy the validity notBefore / notAfter raw time strings (UTCTime
 * "YYMMDDHHMMSSZ" or GeneralizedTime "YYYYMMDDHHMMSSZ") into the caller's
 * buffers as NUL-terminated strings. Either pointer may be NULL to skip it.
 * Returns 0 on success, non-zero on parse error. Buffers should be >= 32 bytes.
 */
int x509_get_validity(const unsigned char *der, unsigned long len,
                      char *not_before, unsigned long nb_cap,
                      char *not_after, unsigned long na_cap);

/*
 * Built-in self-test. Parses an embedded, known-good DER RSA
 * SubjectPublicKeyInfo (and a tiny self-signed certificate) and verifies that
 * x509_extract_pubkey() returns the expected modulus length and leading bytes
 * and the expected exponent (0x010001). Also exercises all new accessors below.
 * Returns 0 if all checks pass, non-zero (a distinct negative code per failed
 * check) otherwise. No I/O.
 */
int x509_selftest(void);

/* =========================================================================
 * Extended accessors required by the certificate-chain validator.
 * All functions take (der, len) -- the full DER certificate buffer.
 * All return 0 on success, non-zero on any parse or bounds error.
 * Pointer outputs point INTO the der buffer (no copy, no allocation).
 * ========================================================================= */

/*
 * Return a view of the raw DER bytes that form the tbsCertificate SEQUENCE TLV
 * (tag + length + contents -- exactly the bytes that are signed). *tbs points
 * into der and *tbslen is the byte count of the whole TLV.
 */
int x509_get_tbs(const unsigned char *der, unsigned long len,
                 const unsigned char **tbs, unsigned long *tbslen);

/*
 * Signature algorithm codes returned by x509_get_signature():
 *   0  rsa_pkcs1_sha256   (sha256WithRSAEncryption  OID 1.2.840.113549.1.1.11)
 *   1  rsa_pkcs1_sha1     (sha1WithRSAEncryption    OID 1.2.840.113549.1.1.5)
 *   2  rsa_pkcs1_sha384   (sha384WithRSAEncryption  OID 1.2.840.113549.1.1.12)
 *   3  rsa_pkcs1_sha512   (sha512WithRSAEncryption  OID 1.2.840.113549.1.1.13)
 *   4  ecdsa_sha256       (ecdsa-with-SHA256         OID 1.2.840.10045.4.3.2)
 *   5  ecdsa_sha384       (ecdsa-with-SHA384         OID 1.2.840.10045.4.3.3)
 *  -1  unknown
 */
#define X509_SIGALG_RSA_PKCS1_SHA256  0
#define X509_SIGALG_RSA_PKCS1_SHA1    1
#define X509_SIGALG_RSA_PKCS1_SHA384  2
#define X509_SIGALG_RSA_PKCS1_SHA512  3
#define X509_SIGALG_ECDSA_SHA256      4
#define X509_SIGALG_ECDSA_SHA384      5
#define X509_SIGALG_UNKNOWN          -1

/*
 * Return the raw signatureValue bytes (for RSA: the PKCS#1 signature bytes;
 * for ECDSA: the DER SEQUENCE { INTEGER r, INTEGER s }). *sig points into der,
 * *siglen is the byte count. *sigalg is set to one of the X509_SIGALG_* codes.
 * The BIT STRING's leading "unused bits" octet is consumed; *sig starts at the
 * first payload byte.
 */
int x509_get_signature(const unsigned char *der, unsigned long len,
                       const unsigned char **sig, unsigned long *siglen,
                       int *sigalg);

/*
 * Decode an ECDSA DER signature SEQUENCE { INTEGER r, INTEGER s } into
 * fixed-width 32-byte big-endian r and s (right-justified, zero-padded on the
 * left, leading sign bytes stripped). Returns 0 on success.
 */
int x509_ecdsa_sig_to_rs(const unsigned char *sig, unsigned long siglen,
                         unsigned char r[32], unsigned char s[32]);

/*
 * Return a view of the raw DER bytes of the issuer Name SEQUENCE TLV.
 * *dn points into der, *dnlen is the byte count including the SEQUENCE tag and
 * length. Suitable for byte-for-byte comparison with another certificate's
 * subject Name when walking a chain.
 */
int x509_get_issuer_dn(const unsigned char *der, unsigned long len,
                       const unsigned char **dn, unsigned long *dnlen);

/*
 * Return a view of the raw DER bytes of the subject Name SEQUENCE TLV.
 * Same pointer-into-der / byte-count contract as x509_get_issuer_dn().
 */
int x509_get_subject_dn(const unsigned char *der, unsigned long len,
                        const unsigned char **dn, unsigned long *dnlen);

/*
 * Walk the SubjectAltName extension (OID 2.5.29.17) and collect dNSName
 * entries. Each entry is copied as a NUL-terminated ASCII string into
 * names[i] (truncated to 255 chars + NUL). Up to `max` entries are written.
 * Returns the number of dNSName entries found (may be 0 if the extension is
 * absent or contains only non-DNS name types). Returns -1 on a hard parse
 * error before the extension is reached.
 */
int x509_get_san_dns(const unsigned char *der, unsigned long len,
                     char names[][256], int max);

/*
 * Return the public-key algorithm in the certificate's subjectPublicKeyInfo:
 *   0  RSA   (rsaEncryption OID 1.2.840.113549.1.1.1)
 *   1  EC    (id-ecPublicKey OID 1.2.840.10045.2.1, any curve)
 *  -1  other / unknown
 * The result is written to *alg. Returns 0 on success, non-zero on parse error.
 */
int x509_pubkey_alg(const unsigned char *der, unsigned long len, int *alg);

/*
 * For EC certificates: copy the 65-byte uncompressed public point
 * (0x04 || X || Y) from subjectPublicKeyInfo into point65[0..64].
 * Returns 0 on success, non-zero if the SPKI algorithm is not EC or if the
 * point is not exactly 65 bytes (i.e. compressed or wrong curve).
 */
int x509_get_ec_pubkey(const unsigned char *der, unsigned long len,
                       unsigned char point65[65]);

/*
 * Return the notBefore and notAfter validity times as compact 14-character
 * UTC strings "YYYYMMDDHHMMSS" (no trailing 'Z' or other suffix).
 * UTCTime YYMMDDHHMMSS[Z]: 2-digit year >= 50 maps to 19xx, else 20xx.
 * GeneralizedTime YYYYMMDDHHMMSS[Z]: first four digits are the year.
 * Either out pointer may be NULL to skip that field.
 * Buffers must be >= 15 bytes (14 chars + NUL).
 * Returns 0 on success, non-zero on parse error.
 */
int x509_get_validity_utc(const unsigned char *der, unsigned long len,
                          char not_before[15], char not_after[15]);

#endif /* TLS_X509_H */
