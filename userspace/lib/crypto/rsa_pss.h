/*
 * rsa_pss.h -- freestanding RSASSA-PSS signature verification.
 * ===========================================================
 *
 * Pure computation: NO libc, NO syscalls, NO malloc, NO standard headers.
 * Built on the existing project RSA (rsa.h / rsa.c) and the project SHA
 * implementations (sha256.c / sha512.c). This is the public-key
 * verification half of RSASSA-PSS only -- enough to authenticate the
 * server CertificateVerify and certificate signatures in a TLS 1.3 client
 * for the mandatory schemes:
 *
 *   rsa_pss_rsae_sha256  -- MGF1-SHA256, salt length = 32 (= digest length)
 *   rsa_pss_rsae_sha384  -- MGF1-SHA384, salt length = 48 (= digest length)
 *
 * The verify performs:
 *
 *   1. RSAVP1: m = sig^e mod n  (reuses rsa.c's modexp path via the bignum
 *      layer); reject sig >= n. Recover EM as emLen = ceil((modBits-1)/8)
 *      big-endian bytes.
 *   2. EMSA-PSS-VERIFY (RFC 8017 sec 9.1.2):
 *        - last byte of EM must be 0xbc
 *        - EM = maskedDB(emLen-hLen-1) || H(hLen) || 0xbc
 *        - dbMask = MGF1(H, emLen-hLen-1) using the same hash
 *        - DB = maskedDB XOR dbMask
 *        - clear the leftmost (8*emLen - (modBits-1)) bits of DB[0]
 *        - DB must be PS(zeros) || 0x01 || salt(hLen) (sLen == hLen here)
 *        - M' = (0x00 * 8) || mHash || salt ; H' = Hash(M')
 *        - accept iff H' == H
 *
 * sLen is fixed to hLen for these TLS schemes (matching OpenSSL's
 * rsa_pss_saltlen:-1, i.e. "salt length = digest length").
 *
 * Inputs are public values from the wire, so this code is not constant-time.
 */

#ifndef CRYPTO_RSA_PSS_H
#define CRYPTO_RSA_PSS_H

#include "rsa.h"   /* rsa_pubkey, bignum (via bignum.h) */

/* Hash selector for the PSS verify. Salt length is always = digest length. */
#define RSA_PSS_SHA256 0   /* rsa_pss_rsae_sha256: MGF1-SHA256, sLen=32 */
#define RSA_PSS_SHA384 1   /* rsa_pss_rsae_sha384: MGF1-SHA384, sLen=48 */

/*
 * RSASSA-PSS-VERIFY (RFC 8017 sec 8.1.2) for the two TLS 1.3 RSA-PSS schemes.
 *
 *   pk        the signer's RSA public key (n, e).
 *   sig       the signature octet string, big-endian, length sig_len.
 *   sig_len   length of sig; MUST equal the modulus byte length k.
 *   mhash     the message hash mHash = Hash(M), length mhash_len.
 *   mhash_len MUST equal the digest length for hash_alg (32 or 48).
 *   hash_alg  RSA_PSS_SHA256 or RSA_PSS_SHA384.
 *
 * Returns 0 on a VALID signature, nonzero (a distinct code per failure
 * point, see rsa_pss.c) otherwise. A return of 0 means the signature
 * authenticates mhash under pk.
 */
int rsa_pss_verify(const rsa_pubkey *pk,
                   const unsigned char *sig, unsigned long sig_len,
                   const unsigned char *mhash, unsigned long mhash_len,
                   int hash_alg);

#endif /* CRYPTO_RSA_PSS_H */
