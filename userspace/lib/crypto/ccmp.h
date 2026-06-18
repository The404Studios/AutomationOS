/*
 * ccmp.h -- IEEE 802.11 CCMP (CTR + CBC-MAC Protocol) wrapper.
 * ============================================================
 *
 * Pure computation: no libc, no syscalls, no malloc, no float, no standard
 * headers. Fixed-size stack buffers only. Wraps the generic AES-CCM core
 * (ccm.h) with the 802.11 AAD/nonce construction (wpa_aad.h) and the CCMP
 * header / MIC framing, per IEEE 802.11-2020 sec. 12.5.3.
 *
 * Build flags (matching the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Output layout of a protected CCMP MPDU body (what these functions write to
 * `out`, i.e. everything after the MAC header):
 *
 *   CCMP Header (8 octets) || Encrypted Data || MIC
 *   CCMP Header = PN0, PN1, Rsvd(0), KeyID|ExtIV(0x20 | keyid<<6),
 *                 PN2, PN3, PN4, PN5
 *   MIC length  = 8 octets for CCMP-128 (TK = 16 bytes),
 *                 16 octets for CCMP-256 (TK = 32 bytes).
 *
 * Note: these functions write only the encrypted MPDU *body* to `out` (CCMP
 * header + ciphertext + MIC). They do not copy or modify the MAC header; the
 * caller supplies the MAC header separately (for AAD construction) and is
 * responsible for setting the Protected Frame bit in the transmitted header.
 * ====================================================================== */

#ifndef CRYPTO_CCMP_H
#define CRYPTO_CCMP_H

#ifndef CRYPTO_STDINT_DEFINED
#define CRYPTO_STDINT_DEFINED
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
#endif

/*
 * ccmp_encrypt -- protect an MPDU payload with CCMP.
 *
 *   tk/tkbits : Temporal Key (tkbits = 128 for CCMP-128, 256 for CCMP-256).
 *   mac_hdr   : the plaintext MAC header (used to build the AAD/nonce).
 *   hdr_len   : MAC header length in bytes (>= 24).
 *   has_qos   : non-zero for a QoS Data frame (a QoS Control field is present
 *               in mac_hdr; its TID becomes the CCMP nonce priority).
 *   pn6       : 6-byte Packet Number, big-endian (pn6[0] = PN5 = MSB).
 *   pt/ptlen  : plaintext payload (ptlen <= 2304).
 *   out       : receives CCMP-header(8) || ciphertext(ptlen) || MIC(8 or 16).
 *   out_len   : receives the total number of bytes written to out.
 *
 * Returns 0 on success, -1 on invalid arguments.
 */
int ccmp_encrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *pn6,
                 const uint8_t *pt, int ptlen,
                 uint8_t *out, int *out_len);

/*
 * ccmp_decrypt -- recover and verify a CCMP-protected MPDU body.
 *
 *   in/in_len : the encrypted MPDU body: CCMP-header(8) || ct || MIC.
 *   pt_out    : receives the recovered plaintext payload.
 *   pt_len    : receives the plaintext length (in_len - 8 - miclen).
 *
 * The PN is taken from the CCMP header in `in`. Returns 0 when the MIC
 * verifies, -1 on MIC mismatch or invalid arguments. On failure pt_out must
 * be treated as invalid.
 */
int ccmp_decrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *in, int in_len,
                 uint8_t *pt_out, int *pt_len);

/*
 * ccmp_selftest -- IEEE 802.11 CCMP known-answer tests (the 802.11-2020
 * Annex J.5.4 / hostap CCMP-128 vector and the J.5 CCMP-256 vector) plus an
 * encrypt->decrypt round-trip and a tamper-detection check. Returns 0 on
 * PASS, non-zero on the first failure.
 */
int ccmp_selftest(void);

#endif /* CRYPTO_CCMP_H */
