/*
 * pbkdf2.h -- freestanding PBKDF2-HMAC-SHA1 (RFC 2898 / PKCS #5 v2.0).
 * ====================================================================
 *
 * Pure computation: no libc, no syscalls, no malloc, no float, no standard
 * headers. Fixed-size stack buffers only. The PRF is HMAC-SHA-1 (reused from
 * hmac.h / sha1.h); SHA-1 is weak as a hash, but PBKDF2-HMAC-SHA1 remains the
 * mandated KDF for WPA/WPA2 PSK -> PMK derivation (IEEE 802.11i /
 * 802.11-2020 J.4).
 *
 * RFC 2898 PBKDF2:
 *   DK = T(1) || T(2) || ... || T(ceil(dklen/hlen))   (truncated to dklen)
 *   T(i) = U(1) ^ U(2) ^ ... ^ U(c)
 *     U(1) = PRF(P, S || INT32BE(i))
 *     U(j) = PRF(P, U(j-1))                            j = 2..c
 * where PRF = HMAC-SHA1 (hlen = 20).
 *
 * Build flags (matching the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#ifndef CRYPTO_PBKDF2_H
#define CRYPTO_PBKDF2_H

/* Freestanding fixed-width types. The crypto lib pulls in no standard
 * headers, so define the few stdint names this API exposes. Guarded so a
 * translation unit that already typedef'd them (e.g. clib.h, image.h) does
 * not double-define. */
#ifndef CRYPTO_STDINT_DEFINED
#define CRYPTO_STDINT_DEFINED
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
#endif

/*
 * PBKDF2 with PRF = HMAC-SHA1.
 *
 *   pass/passlen  -- password / passphrase octets (P)
 *   salt/saltlen  -- salt octets (S)
 *   iters         -- iteration count (c); treated as >= 1 (0 behaves as 1)
 *   out/dklen     -- derived key buffer; dklen output bytes are written
 *
 * dklen <= 0 writes nothing. Negative passlen/saltlen are treated as 0.
 */
void pbkdf2_hmac_sha1(const uint8_t *pass, int passlen,
                      const uint8_t *salt, int saltlen,
                      uint32_t iters,
                      uint8_t *out, int dklen);

/*
 * WPA/WPA2 PMK derivation (IEEE 802.11-2020 J.4):
 *   PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 256 bits)
 *
 *   passphrase -- ASCII passphrase, NUL-terminated (its strlen is the length)
 *   ssid       -- SSID octets (1..32 bytes)
 *   pmk        -- 32-byte output
 */
void wpa_pmk(const char *passphrase,
             const uint8_t *ssid, int ssid_len,
             uint8_t pmk[32]);

/* Known-answer self-test. Returns 0 on PASS, non-zero on FAIL. */
int pbkdf2_selftest(void);

#endif /* CRYPTO_PBKDF2_H */
