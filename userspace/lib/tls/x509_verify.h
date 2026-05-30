/*
 * x509_verify.h -- freestanding X.509 certificate CHAIN VALIDATION.
 * ================================================================
 *
 * Pure computation: NO libc, NO syscalls, NO malloc, NO standard headers.
 * Everything operates on caller-provided, immutable DER buffers using fixed
 * stack/static buffers and our own memcpy/memcmp. No allocation, no I/O. Every
 * read is bounds-checked through the minimal DER reader in asn1.h, so a
 * malformed/truncated/hostile certificate can never cause an out-of-bounds
 * access -- it yields a negative error code instead.
 *
 * WHAT THIS IS FOR
 * ----------------
 * A TLS client that merely extracts the server's public key (see x509.h) gets
 * ENCRYPTION but not AUTHENTICATION: an active man-in-the-middle can present any
 * certificate and the client cannot tell. This module closes that gap. Given
 * the certificate chain the server sent in its Certificate handshake message,
 * it answers the only question that makes https:// trustworthy:
 *
 *     "Is this certificate chain issued by a CA I trust, still valid in time,
 *      and actually for the host I asked to connect to?"
 *
 * It performs the four classic checks:
 *
 *   1. CHAIN LINKAGE + SIGNATURES. For each adjacent pair (cert[i], cert[i+1]),
 *      cert[i] must be signed by cert[i+1]'s key: we hash cert[i]'s TBS bytes
 *      with the signature's hash algorithm and verify the signature with
 *      cert[i+1]'s public key. We also require cert[i].issuerDN to equal
 *      cert[i+1].subjectDN (raw DER byte compare).
 *
 *   2. TRUST ANCHOR. The top of the server-supplied chain must be issued by a
 *      root in the CA trust store (ca_bundle.h). We find a root whose subjectDN
 *      equals the top cert's issuerDN, verify the top cert's signature with that
 *      root's key, and check the root is itself within its validity window. (If
 *      the leaf is itself directly issued by a trusted root, a 1-cert chain is
 *      accepted by the same logic.)
 *
 *   3. VALIDITY WINDOW. Every cert in the chain (and the matching root) must
 *      satisfy notBefore <= now <= notAfter. Times are normalised to
 *      "YYYYMMDDHHMMSS" so a plain lexicographic compare is correct for UTC.
 *
 *   4. HOSTNAME. The leaf's subjectAltName dNSName entries must match the
 *      requested hostname (case-insensitive exact, or "*." wildcard matching
 *      exactly one left label). If the leaf has no SAN we fall back to the
 *      subject CommonName.
 *
 * SECURITY HONESTY -- READ THIS
 * -----------------------------
 *   * This checks SIGNATURES + CHAIN + VALIDITY + HOSTNAME. That is enough to
 *     authenticate a normally-behaving server against a trusted root.
 *   * It does NOT do REVOCATION. There is no CRL fetching and no OCSP
 *     (stapled or otherwise). A certificate that has been revoked but is still
 *     within its validity window and chains to a trusted root will be ACCEPTED.
 *   * Supported signature algorithms are ONLY:
 *         RSA PKCS#1 v1.5 with SHA-256   ("rsa-sha256")
 *         RSA PKCS#1 v1.5 with SHA-384   ("rsa-sha384")
 *         RSA PKCS#1 v1.5 with SHA-512   ("rsa-sha512")
 *         ECDSA on P-256 with SHA-256    ("ecdsa-sha256")
 *     Any other signature algorithm (RSA/ECDSA with SHA-1, RSA-PSS, EdDSA,
 *     EC curves other than P-256, ECDSA-SHA384/512, ...) is REJECTED, not
 *     silently accepted. SHA-1 is intentionally not accepted (it is broken for
 *     signing). ECDSA-SHA384/512 requires P-384/P-521 which are not
 *     implemented; those OIDs are also rejected.
 *   * It does NOT enforce basicConstraints CA:TRUE / pathLen, key usage, or
 *     name constraints on intermediates. The chain is authenticated by issuer/
 *     subject DN matching and signature verification only. This is a deliberate
 *     minimalism trade-off; document it to anyone relying on it.
 *   * It does NOT verify the leaf cert's public-key-to-handshake binding -- that
 *     (using the authenticated leaf key in the TLS key exchange / CertVerify) is
 *     the TLS layer's job. This module only authenticates the chain itself.
 */

#ifndef TLS_X509_VERIFY_H
#define TLS_X509_VERIFY_H

/* ---- error codes (all negative; 0 == trusted, valid, hostname matches) ---- */
#define X509V_OK                 0
#define X509V_ERR_ARGS          -1   /* NULL / nonsensical arguments          */
#define X509V_ERR_CHAIN_LEN     -2   /* ncerts <= 0 or exceeds the cap        */
#define X509V_ERR_PARSE         -3   /* a cert failed to parse (bad DER)      */
#define X509V_ERR_DN_MISMATCH   -4   /* issuerDN != next subjectDN in chain   */
#define X509V_ERR_SIG_ALG       -5   /* unsupported signature algorithm       */
#define X509V_ERR_SIG_INVALID   -6   /* signature did not verify              */
#define X509V_ERR_EXPIRED       -7   /* a cert is outside its validity window */
#define X509V_ERR_NO_ROOT       -8   /* no trusted root matches chain top     */
#define X509V_ERR_HOSTNAME      -9   /* leaf name does not match the hostname */
#define X509V_ERR_PUBKEY       -10   /* could not extract issuer's public key */
#define X509V_ERR_TIME_FMT     -11   /* `now` / a cert time was malformed     */
#define X509V_ERR_INTERNAL     -12   /* buffer too small / unexpected state   */

/* Maximum certificates accepted in one chain (leaf + intermediates). */
#define X509V_MAX_CHAIN  8

/*
 * Verify a server certificate chain.
 *
 *   certs   array of `ncerts` DER certificate pointers. certs[0] is the leaf
 *           (server) certificate; certs[1..] are intermediates in issuing order
 *           (each signed by the next), exactly as a TLS server sends them.
 *   lens    parallel array of DER lengths, lens[i] for certs[i].
 *   ncerts  number of certificates (1 .. X509V_MAX_CHAIN).
 *   hostname        the host the client intended to reach (NUL-terminated). The
 *                   leaf's SAN dNSName entries (or CN fallback) must match it.
 *   now_yyyymmddhhmmss  current UTC time as a 14-char "YYYYMMDDHHMMSS" string
 *                   (e.g. derived from SYS_GETTIME). Used for the validity
 *                   window comparison.
 *
 * Returns X509V_OK (0) iff the chain is TRUSTED (chains to a CA-bundle root via
 * verified signatures and matching DNs) AND every cert is within its validity
 * window AND the leaf authenticates `hostname`. Otherwise a negative
 * X509V_ERR_* describing the first failure. Never reads past any cert buffer.
 */
int x509_verify_chain(const unsigned char *const *certs,
                      const unsigned long *lens, int ncerts,
                      const char *hostname,
                      const char *now_yyyymmddhhmmss);

/*
 * Hostname matcher, exported for reuse (e.g. the TLS layer, tests).
 *
 *   hostname    the requested host (NUL-terminated, no trailing dot expected).
 *   cert_name   a single name from the certificate: a SAN dNSName or a CN. May
 *               be a wildcard of the form "*.suffix".
 *
 * Returns 1 on match, 0 on no match. Matching rules:
 *   - case-insensitive (ASCII) exact comparison, OR
 *   - a leading "*." in cert_name matches EXACTLY ONE left label of hostname:
 *       "*.example.com" matches "www.example.com" but NOT "example.com"
 *       (no left label) and NOT "a.b.example.com" (more than one left label).
 *   - the wildcard must be the entire left-most label ("*." only); embedded
 *     wildcards like "w*.example.com" are not honoured.
 *   - empty hostname / empty cert_name never match.
 */
int x509_hostname_match(const char *hostname, const char *cert_name);

/*
 * Built-in self-test. Verifies hostname matching across the required cases
 * (exact, case-insensitive, valid wildcard, and the wildcard NON-matches), and
 * -- if an embedded mini chain is compiled in -- a positive chain verification
 * plus negative tests (wrong hostname, expired). Returns 0 if all checks pass,
 * a distinct negative code per failed check otherwise. No I/O.
 */
int x509_verify_selftest(void);

#endif /* TLS_X509_VERIFY_H */
