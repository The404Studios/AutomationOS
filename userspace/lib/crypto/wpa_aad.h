/*
 * wpa_aad.h -- IEEE 802.11 AAD + CCM/GCM nonce builder (shared by CCMP/GCMP).
 * ===========================================================================
 *
 * Pure computation: no libc, no syscalls, no malloc, no float, no standard
 * headers. Fixed-size stack buffers only. Suitable for freestanding
 * kernel/userspace crypto without a C runtime.
 *
 * Build flags (matching the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * -------------------------------------------------------------------------
 * Additional Authentication Data (IEEE 802.11-2020 sec. 12.5.3.3.3 for CCMP,
 * 12.5.5.3.3 for GCMP).
 * -------------------------------------------------------------------------
 * The AAD is built from the (unencrypted) MPDU MAC header, with the mutable
 * bits masked so that retransmission / power-management churn does not break
 * the MIC:
 *
 *   AAD = FC' || A1 || A2 || A3 || SC' [ || A4 ] [ || QC' ]
 *
 *   FC'  = Frame Control with these subfields masked to 0:
 *            - Subtype bits (for Data frames): mask 0x0070
 *            - Order/+HTC bit (for QoS Data frames): mask 0x8000
 *            - Retry (0x0800), Pwr Mgmt (0x1000), More Data (0x2000)
 *          and with the Protected Frame bit (0x4000) set to 1 (the AAD always
 *          describes the protected MPDU).
 *   A1,A2,A3 = the three address fields (6 bytes each).
 *   SC'  = Sequence Control with the Sequence Number masked to 0 (0xFFF0)
 *          and the Fragment Number (low 4 bits) preserved.
 *   A4   = present only when both ToDS and FromDS are set (6 bytes).
 *   QC'  = QoS Control, present for QoS Data frames (2 bytes); the first byte
 *          has the TID-unrelated bits masked (0x70 and the SPP A-MSDU 0x80
 *          bit), the second byte is forced to 0.
 *
 * Resulting AAD length is 22, 24, 28 or 30 bytes.
 *
 * -------------------------------------------------------------------------
 * Nonce (IEEE 802.11-2020 sec. 12.5.3.3.4 for CCMP, 12.5.5.3.4 for GCMP).
 * -------------------------------------------------------------------------
 *   CCMP (13 bytes): Flags(1) || A2(6) || PN(6, big-endian, PN5..PN0)
 *       Flags = priority(TID) for QoS Data, |0x10 for Management, else 0.
 *   GCMP (12 bytes): A2(6) || PN(6, big-endian, PN5..PN0)
 *       (GCMP has no Flags octet.)
 *
 * The PN is supplied as a 6-byte big-endian array pn6[0..5] where pn6[0] is
 * the most-significant octet (PN5) -- the same ordering used by the IEEE
 * Annex J / hostap test vectors.
 * ====================================================================== */

#ifndef CRYPTO_WPA_AAD_H
#define CRYPTO_WPA_AAD_H

/* Freestanding fixed-width types. The crypto lib pulls in no standard
 * headers, so define the few stdint names this API exposes. Guarded with the
 * shared CRYPTO_STDINT_DEFINED macro so this header may be included alongside
 * the other crypto headers (pbkdf2.h, ccm.h, ...) without redefinition. */
#ifndef CRYPTO_STDINT_DEFINED
#define CRYPTO_STDINT_DEFINED
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
#endif

/* Maximum AAD length (A4 + QoS present): FC(2)+A1..A3(18)+SC(2)+A4(6)+QC(2). */
#define WPA_AAD_MAXLEN 30

/*
 * wpa_build_aad -- build the 802.11 CCMP/GCMP AAD from a MAC header.
 *
 *   mac_hdr  : the MPDU MAC header bytes (FC, Dur, A1, A2, A3, SC, [A4], [QC]).
 *   hdr_len  : length of mac_hdr in bytes (>= 24). A4/QoS presence is derived
 *              from the Frame Control / has_qos rather than from hdr_len, but
 *              hdr_len must cover whatever fields are present.
 *   has_qos  : non-zero if a QoS Control field is present (QoS Data frame).
 *              The QoS Control is read from the header at the correct offset
 *              (after A4 when ToDS+FromDS are both set).
 *   aad      : output buffer, at least WPA_AAD_MAXLEN bytes.
 *   aad_len  : receives the number of AAD bytes written (22/24/28/30).
 *
 * Returns 0 on success, -1 on invalid arguments (NULL ptr, hdr_len < 24, or
 * a header too short to contain the A4/QoS fields it claims).
 */
int wpa_build_aad(const uint8_t *mac_hdr, int hdr_len, int has_qos,
                  uint8_t *aad, int *aad_len);

/*
 * wpa_build_nonce -- build the CCM / GCM nonce.
 *
 *   a2          : the A2 (transmitter) address, 6 bytes.
 *   pn6         : the 6-byte Packet Number, big-endian (pn6[0] = PN5 = MSB).
 *   nonce_flags : the CCMP Flags octet (priority/TID bits and/or 0x10 for
 *                 Management frames). Ignored when gcmp != 0.
 *   gcmp        : 0 -> CCMP (13-byte nonce, with leading Flags octet);
 *                 non-zero -> GCMP (12-byte nonce, no Flags octet).
 *   out         : output buffer (>= 13 bytes for CCMP, >= 12 for GCMP).
 */
void wpa_build_nonce(const uint8_t *a2, const uint8_t *pn6,
                     uint8_t nonce_flags, int gcmp, uint8_t *out);

#endif /* CRYPTO_WPA_AAD_H */
