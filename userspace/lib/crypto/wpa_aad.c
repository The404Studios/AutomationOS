/*
 * wpa_aad.c -- IEEE 802.11 AAD + CCM/GCM nonce builder (shared by CCMP/GCMP).
 * ===========================================================================
 * No libc, no headers. Pure byte manipulation over the 802.11 MAC header.
 *
 * Mirrors the AAD/nonce construction in IEEE 802.11-2020 sec. 12.5.3.3.3 /
 * 12.5.5.3.3 (and the reference wlantest ccmp_aad_nonce / gcmp_aad_nonce).
 */

#include "wpa_aad.h"

/* ---- Frame Control / Sequence Control bit masks (little-endian fc) ---- */
#define FC_TODS      0x0100u
#define FC_FROMDS    0x0200u
#define FC_RETRY     0x0800u
#define FC_PWRMGT    0x1000u
#define FC_MOREDATA  0x2000u
#define FC_ISWEP     0x4000u
#define FC_ORDER     0x8000u

#define FC_TYPE_MGMT 0u
#define FC_TYPE_DATA 2u

#define ETH_ALEN     6

static void wa_memcpy(unsigned char *d, const unsigned char *s, int n) {
    int i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

int wpa_build_aad(const uint8_t *mac_hdr, int hdr_len, int has_qos,
                  uint8_t *aad, int *aad_len) {
    uint32_t fc, type, seq;
    int addr4 = 0;
    int pos;
    int qos_off;

    if (mac_hdr == 0 || aad == 0 || aad_len == 0) return -1;
    if (hdr_len < 24) return -1;

    /* Frame Control is the first two header octets, little-endian on the air. */
    fc = (uint32_t)mac_hdr[0] | ((uint32_t)mac_hdr[1] << 8);

    if ((fc & (FC_TODS | FC_FROMDS)) == (FC_TODS | FC_FROMDS))
        addr4 = 1;

    type = (fc >> 2) & 0x3u;
    if (type == FC_TYPE_DATA) {
        fc &= ~0x0070u;            /* mask Subtype bits */
        if (has_qos)
            fc &= ~FC_ORDER;       /* mask Order/+HTC for QoS Data */
    }

    /* Mutable bits that must not enter the MIC. */
    fc &= ~(FC_RETRY | FC_PWRMGT | FC_MOREDATA);
    /* The AAD always describes the protected MPDU. */
    fc |= FC_ISWEP;

    /* Bounds: ensure the header actually contains the fields we will read. */
    if (addr4 && hdr_len < 24 + ETH_ALEN) return -1;
    qos_off = 24 + (addr4 ? ETH_ALEN : 0);
    if (has_qos && hdr_len < qos_off + 2) return -1;

    /* FC' (2, little-endian) */
    aad[0] = (uint8_t)(fc & 0xff);
    aad[1] = (uint8_t)((fc >> 8) & 0xff);
    pos = 2;

    /* A1 || A2 || A3  (header offsets 4..21) */
    wa_memcpy(aad + pos, mac_hdr + 4, 3 * ETH_ALEN);
    pos += 3 * ETH_ALEN;

    /* SC': Sequence Control with Sequence Number masked, Fragment Number kept.
     * Sequence Control is at header offset 22..23 (little-endian). */
    seq = (uint32_t)mac_hdr[22] | ((uint32_t)mac_hdr[23] << 8);
    seq &= ~0xfff0u;
    aad[pos]     = (uint8_t)(seq & 0xff);
    aad[pos + 1] = (uint8_t)((seq >> 8) & 0xff);
    pos += 2;

    /* A4, present only with ToDS+FromDS (header offset 24..29). */
    if (addr4) {
        wa_memcpy(aad + pos, mac_hdr + 24, ETH_ALEN);
        pos += ETH_ALEN;
    }

    /* QoS Control, present for QoS Data frames. First octet masks the
     * TID-unrelated bits (0x70) and the SPP A-MSDU (0x80) bit; second is 0. */
    if (has_qos) {
        aad[pos]     = (uint8_t)(mac_hdr[qos_off] & ~0x70 & ~0x80);
        aad[pos + 1] = 0x00;
        pos += 2;
    }

    *aad_len = pos;
    return 0;
}

void wpa_build_nonce(const uint8_t *a2, const uint8_t *pn6,
                     uint8_t nonce_flags, int gcmp, uint8_t *out) {
    int base;

    if (gcmp) {
        base = 0;                  /* GCMP nonce has no Flags octet */
    } else {
        out[0] = nonce_flags;      /* CCMP Flags octet */
        base = 1;
    }

    /* A2 (6 bytes) */
    out[base + 0] = a2[0];
    out[base + 1] = a2[1];
    out[base + 2] = a2[2];
    out[base + 3] = a2[3];
    out[base + 4] = a2[4];
    out[base + 5] = a2[5];

    /* PN (6 bytes, big-endian: PN5 .. PN0). pn6[0] is the MSB (PN5). */
    out[base + 6]  = pn6[0];
    out[base + 7]  = pn6[1];
    out[base + 8]  = pn6[2];
    out[base + 9]  = pn6[3];
    out[base + 10] = pn6[4];
    out[base + 11] = pn6[5];
}
