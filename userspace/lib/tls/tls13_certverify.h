/*
 * tls13_certverify.h -- TLS 1.3 CertificateVerify verification (RFC 8446 4.4.3).
 * =============================================================================
 *
 * The signed content is:
 *   octet 0x20 repeated 64 times
 *   || the context string ("TLS 1.3, server CertificateVerify" for a server,
 *      "TLS 1.3, client CertificateVerify" for a client)
 *   || a single 0x00
 *   || Transcript-Hash(Handshake Context .. Certificate)
 *
 * The peer signs that content under the certificate key with the negotiated
 * SignatureScheme. We rebuild it, hash it, and verify the signature:
 *   0x0804 rsa_pss_rsae_sha256  -> RSA-PSS(SHA-256) over the content
 *   0x0805 rsa_pss_rsae_sha384  -> RSA-PSS(SHA-384)
 *   0x0403 ecdsa_secp256r1_sha256 -> P-256 ECDSA over SHA-256(content)
 *   0x0503 ecdsa_secp384r1_sha384 -> P-384 ECDSA over SHA-384(content)
 *
 * Freestanding; built on rsa_pss, p256/p384, and sha256/384.
 */
#ifndef TLS13_CERTVERIFY_H
#define TLS13_CERTVERIFY_H

#include "../crypto/rsa.h"

/* SignatureScheme codes used in TLS 1.3 CertificateVerify. */
#define TLS13_SIG_RSA_PSS_SHA256  0x0804
#define TLS13_SIG_RSA_PSS_SHA384  0x0805
#define TLS13_SIG_ECDSA_P256_SHA256 0x0403
#define TLS13_SIG_ECDSA_P384_SHA384 0x0503

/* Build the CertificateVerify signed content into out (caller buffer >= 98 +
 * thlen). is_server selects the context string. Returns the content length. */
unsigned long tls13_certverify_content(int is_server,
                                       const unsigned char *transcript_hash,
                                       unsigned long thlen, unsigned char *out);

/* RSA-PSS CertificateVerify (sigalg 0x0804 / 0x0805). Returns 0 iff valid. */
int tls13_certverify_rsapss(unsigned short sigalg, int is_server,
                            const unsigned char *transcript_hash, unsigned long thlen,
                            const unsigned char *sig, unsigned long sig_len,
                            const rsa_pubkey *pk);

/* ECDSA CertificateVerify (sigalg 0x0403 / 0x0503). ec_point is the
 * uncompressed point (65 for P-256, 97 for P-384). Returns 0 iff valid. */
int tls13_certverify_ecdsa(unsigned short sigalg, int is_server,
                           const unsigned char *transcript_hash, unsigned long thlen,
                           const unsigned char *sig, unsigned long sig_len,
                           const unsigned char *ec_point, unsigned long ec_point_len);

/* RFC 8448 Section 3 KAT (server CertificateVerify, rsa_pss_rsae_sha256).
 * Returns 0 on pass, nonzero on failure. */
int tls13_certverify_selftest(void);

#endif /* TLS13_CERTVERIFY_H */
