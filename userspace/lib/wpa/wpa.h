/*
 * wpa.h -- freestanding WPA2 4-way-handshake key primitives.
 * ==========================================================
 *
 * Pure computation: no libc, no syscalls, no malloc, no float, no standard
 * headers. Fixed-size stack buffers only. These are the supplicant-side
 * cryptographic primitives of the WPA2 (RSN/CCMP) 4-way handshake:
 *
 *   PMK  --  pre-shared master key  (from pbkdf2.h: wpa_pmk()).
 *   PTK  --  pairwise transient key, derived here from PMK + the two MAC
 *            addresses + the two nonces, via the IEEE 802.11 PRF (HMAC-SHA1).
 *            PTK = KCK[16] || KEK[16] || TK[16]  for CCMP (ptk_len = 48).
 *   MIC  --  EAPOL-Key message integrity code = HMAC-SHA1-128 under the KCK
 *            (the SHA1 HMAC truncated to 16 bytes), computed over the EAPOL
 *            frame with its own MIC field zeroed.
 *   GTK  --  group temporal key, delivered wrapped in EAPOL msg3 under the KEK
 *            via AES Key Unwrap (RFC 3394).
 *
 * IEEE 802.11-2020 12.7.1.2 (Pairwise key hierarchy):
 *   PTK = PRF-X(PMK, "Pairwise key expansion",
 *               Min(AA,SPA) || Max(AA,SPA) || Min(ANonce,SNonce) || Max(...))
 * where PRF-X(K, A, B) for i = 0,1,...:
 *   R = R || HMAC-SHA1(K, A || 0x00 || B || i_byte),  truncate to X bytes.
 *
 * Build flags (matching the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Depends on:
 *   ../crypto/hmac.h     -- hmac_sha1()        (PTK PRF + MIC)
 *   ../crypto/keywrap.h  -- aes_key_unwrap()   (GTK decrypt)
 */

#ifndef WPA_WPA_H
#define WPA_WPA_H

/* Freestanding fixed-width type. The crypto lib pulls in no standard headers;
 * define the one stdint name this API exposes, guarded so a translation unit
 * that already typedef'd it (pbkdf2.h / keywrap.h / clib.h) does not collide. */
#ifndef CRYPTO_STDINT_DEFINED
#define CRYPTO_STDINT_DEFINED
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
#endif

/*
 * wpa_ptk -- derive the Pairwise Transient Key.
 *
 *   pmk      -- 32-byte pairwise master key.
 *   aa       -- Authenticator (AP) MAC address, 6 bytes.
 *   spa      -- Supplicant (station) MAC address, 6 bytes.
 *   anonce   -- Authenticator nonce, 32 bytes.
 *   snonce   -- Supplicant nonce, 32 bytes.
 *   ptk      -- output buffer, ptk_len bytes.
 *   ptk_len  -- 48 for CCMP (KCK[16] || KEK[16] || TK[16]).
 *
 * data = Min(AA,SPA) || Max(AA,SPA) || Min(ANonce,SNonce) || Max(ANonce,SNonce)
 * PTK  = PRF-X(PMK, "Pairwise key expansion", data) truncated to ptk_len.
 */
void wpa_ptk(const uint8_t pmk[32],
             const uint8_t *aa, const uint8_t *spa,
             const uint8_t *anonce, const uint8_t *snonce,
             uint8_t *ptk, int ptk_len);

/*
 * wpa_eapol_mic -- compute the EAPOL-Key MIC (HMAC-SHA1-128).
 *
 *   kck          -- 16-byte Key Confirmation Key (PTK[0..16]).
 *   eapol_frame  -- the full EAPOL-Key frame.
 *   len          -- frame length in bytes.
 *   mic          -- 16-byte output.
 *
 * The MIC is HMAC-SHA1(KCK, frame) truncated to 16 bytes. Per IEEE 802.11 the
 * MIC field inside the frame MUST be zero while the MIC is computed; this
 * routine does NOT modify the caller's buffer, so the caller is responsible
 * for passing a frame whose MIC field is already zeroed.
 */
void wpa_eapol_mic(const uint8_t *kck, const uint8_t *eapol_frame, int len,
                   uint8_t mic[16]);

/*
 * wpa_decrypt_gtk -- recover the GTK from its AES-Key-Wrapped form (msg3).
 *
 *   kek          -- 16-byte Key Encryption Key (PTK[16..32]).
 *   wrapped      -- wrapped key data, (n+1)*8 bytes.
 *   wrapped_len  -- length of `wrapped`; must be a multiple of 8 and >= 24
 *                   (one IV block + at least two key blocks = a 16-byte GTK).
 *   gtk_out      -- receives wrapped_len-8 bytes of recovered GTK.
 *
 * Returns the recovered GTK length in bytes (>0) on success, or < 0 on bad
 * arguments or an AES-Key-Unwrap integrity failure (RFC 3394). On failure
 * gtk_out is unspecified and MUST NOT be used.
 */
int wpa_decrypt_gtk(const uint8_t *kek, const uint8_t *wrapped, int wrapped_len,
                    uint8_t *gtk_out);

/*
 * wpa_selftest -- known-answer + self-consistency self-test.
 * Returns 0 on PASS, non-zero (a distinct code per failing stage) on FAIL.
 */
int wpa_selftest(void);

#endif /* WPA_WPA_H */
