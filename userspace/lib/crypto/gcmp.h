/*
 * gcmp.h -- IEEE 802.11 GCMP (GCM with GMAC Protocol) wrapper.
 * ============================================================
 *
 * Pure computation: no libc, no syscalls, no malloc, no float, no standard
 * headers. Fixed-size stack buffers only. Wraps the existing AES-GCM core
 * (aes.h: aes_gcm_encrypt / aes_gcm_decrypt) with the 802.11 AAD/nonce
 * construction (wpa_aad.h) and the GCMP header / MIC framing, per IEEE
 * 802.11-2020 sec. 12.5.5.
 *
 * Build flags (matching the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * GCMP reuses AES-GCM directly: the 12-byte GCMP nonce (A2 || PN) is exactly
 * the GCM IV, and the GCM construction here uses J0 = IV || 0x00000001 -- the
 * 802.11 GCMP nonce model. The MIC is always the 16-byte GCM tag.
 *
 * Output layout of a protected GCMP MPDU body (what these functions write to
 * `out`, i.e. everything after the MAC header):
 *
 *   GCMP Header (8 octets) || Encrypted Data || MIC (16 octets)
 *   GCMP Header = PN0, PN1, Rsvd(0), KeyID|ExtIV(0x20 | keyid<<6),
 *                 PN2, PN3, PN4, PN5
 *
 * tkbits = 128 for GCMP-128 (TK = 16 bytes) or 256 for GCMP-256 (TK = 32).
 * ====================================================================== */

#ifndef CRYPTO_GCMP_H
#define CRYPTO_GCMP_H

#ifndef CRYPTO_STDINT_DEFINED
#define CRYPTO_STDINT_DEFINED
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
#endif

/*
 * gcmp_encrypt -- protect an MPDU payload with GCMP.
 *
 *   tk/tkbits : Temporal Key (tkbits = 128 or 256).
 *   mac_hdr   : the plaintext MAC header (used to build the AAD/nonce).
 *   hdr_len   : MAC header length in bytes (>= 24).
 *   has_qos   : non-zero for a QoS Data frame (a QoS Control field is present).
 *   pn6       : 6-byte Packet Number, big-endian (pn6[0] = PN5 = MSB).
 *   pt/ptlen  : plaintext payload (ptlen <= 2304).
 *   out       : receives GCMP-header(8) || ciphertext(ptlen) || MIC(16).
 *   out_len   : receives the total number of bytes written to out.
 *
 * Returns 0 on success, -1 on invalid arguments.
 */
int gcmp_encrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *pn6,
                 const uint8_t *pt, int ptlen,
                 uint8_t *out, int *out_len);

/*
 * gcmp_decrypt -- recover and verify a GCMP-protected MPDU body.
 *
 *   in/in_len : the encrypted MPDU body: GCMP-header(8) || ct || MIC(16).
 *   pt_out    : receives the recovered plaintext payload.
 *   pt_len    : receives the plaintext length (in_len - 8 - 16).
 *
 * The PN is taken from the GCMP header in `in`. Returns 0 when the MIC
 * verifies, -1 on MIC mismatch or invalid arguments.
 */
int gcmp_decrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *in, int in_len,
                 uint8_t *pt_out, int *pt_len);

/*
 * gcmp_selftest -- IEEE 802.11 GCMP known-answer tests (the 802.11ad-2012
 * M.11.1 / 802.11-2020 J.6 GCMP-128 vector and the GCMP-256 vector) plus an
 * encrypt->decrypt round-trip and a tamper-detection check. Returns 0 on
 * PASS, non-zero on the first failure.
 */
int gcmp_selftest(void);

#endif /* CRYPTO_GCMP_H */
