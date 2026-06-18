/*
 * ccmp.c -- IEEE 802.11 CCMP (CTR + CBC-MAC Protocol) wrapper.
 * ============================================================
 * No libc, no headers beyond ccmp.h / ccm.h / wpa_aad.h / aes.h. Implements
 * IEEE 802.11-2020 sec. 12.5.3: build the AAD and nonce from the MAC header,
 * run AES-CCM (L=2, M=8 for CCMP-128 / M=16 for CCMP-256), and frame the
 * result as CCMP-header || ciphertext || MIC.
 */

#include "ccmp.h"
#include "ccm.h"
#include "wpa_aad.h"

#define CCMP_HDR_LEN   8
#define CCMP_NONCE_LEN 13
#define ETH_ALEN       6

/* Derive the CCMP nonce Flags octet from the MAC header (TID for QoS Data,
 * 0x10 for Management, else 0). Mirrors ccmp_aad_nonce. */
static uint8_t ccmp_nonce_flags(const uint8_t *mac_hdr, int has_qos) {
    unsigned int fc = (unsigned int)mac_hdr[0] | ((unsigned int)mac_hdr[1] << 8);
    unsigned int type = (fc >> 2) & 0x3u;
    int addr4 = ((fc & 0x0300u) == 0x0300u);   /* ToDS && FromDS */
    uint8_t flags = 0;

    if (type == 2 /* DATA */) {
        if (has_qos) {
            int qos_off = 24 + (addr4 ? ETH_ALEN : 0);
            flags = (uint8_t)(mac_hdr[qos_off] & 0x0f);   /* priority/TID */
        }
    } else if (type == 0 /* MGMT */) {
        flags |= 0x10;
    }
    return flags;
}

static int ccmp_miclen(int tkbits) {
    if (tkbits == 256) return 16;   /* CCMP-256 */
    if (tkbits == 128) return 8;    /* CCMP-128 */
    return -1;
}

int ccmp_encrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *pn6,
                 const uint8_t *pt, int ptlen,
                 uint8_t *out, int *out_len) {
    uint8_t aad[WPA_AAD_MAXLEN];
    uint8_t nonce[CCMP_NONCE_LEN];
    int aad_len;
    int miclen;
    const uint8_t *a2;

    miclen = ccmp_miclen(tkbits);
    if (miclen < 0) return -1;
    if (tk == 0 || mac_hdr == 0 || pn6 == 0 || out == 0 || out_len == 0)
        return -1;
    if (ptlen > 0 && pt == 0) return -1;
    if (hdr_len < 24) return -1;

    if (wpa_build_aad(mac_hdr, hdr_len, has_qos, aad, &aad_len) != 0)
        return -1;

    a2 = mac_hdr + 4 + ETH_ALEN;    /* A2 follows FC(2)+Dur(2)+A1(6) */
    wpa_build_nonce(a2, pn6, ccmp_nonce_flags(mac_hdr, has_qos), 0, nonce);

    /* CCMP header: PN0, PN1, Rsvd(0), KeyID|ExtIV(0x20), PN2..PN5.
     * pn6 is big-endian (pn6[0]=PN5 .. pn6[5]=PN0). */
    out[0] = pn6[5];                /* PN0 */
    out[1] = pn6[4];                /* PN1 */
    out[2] = 0x00;                  /* Rsvd */
    out[3] = 0x20;                  /* ExtIV=1, KeyID=0 */
    out[4] = pn6[3];                /* PN2 */
    out[5] = pn6[2];                /* PN3 */
    out[6] = pn6[1];                /* PN4 */
    out[7] = pn6[0];                /* PN5 */

    if (aes_ccm_encrypt(tk, tkbits, nonce, CCMP_NONCE_LEN, aad, aad_len,
                        pt, ptlen, miclen,
                        out + CCMP_HDR_LEN,                 /* ciphertext */
                        out + CCMP_HDR_LEN + ptlen) != 0)   /* MIC        */
        return -1;

    *out_len = CCMP_HDR_LEN + ptlen + miclen;
    return 0;
}

int ccmp_decrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *in, int in_len,
                 uint8_t *pt_out, int *pt_len) {
    uint8_t aad[WPA_AAD_MAXLEN];
    uint8_t nonce[CCMP_NONCE_LEN];
    uint8_t pn6[6];
    int aad_len, miclen, ctlen;
    const uint8_t *a2;

    miclen = ccmp_miclen(tkbits);
    if (miclen < 0) return -1;
    if (tk == 0 || mac_hdr == 0 || in == 0 || pt_out == 0 || pt_len == 0)
        return -1;
    if (hdr_len < 24) return -1;
    if (in_len < CCMP_HDR_LEN + miclen) return -1;
    ctlen = in_len - CCMP_HDR_LEN - miclen;

    /* Recover the big-endian PN from the CCMP header. */
    pn6[0] = in[7];                 /* PN5 */
    pn6[1] = in[6];                 /* PN4 */
    pn6[2] = in[5];                 /* PN3 */
    pn6[3] = in[4];                 /* PN2 */
    pn6[4] = in[1];                 /* PN1 */
    pn6[5] = in[0];                 /* PN0 */

    if (wpa_build_aad(mac_hdr, hdr_len, has_qos, aad, &aad_len) != 0)
        return -1;

    a2 = mac_hdr + 4 + ETH_ALEN;
    wpa_build_nonce(a2, pn6, ccmp_nonce_flags(mac_hdr, has_qos), 0, nonce);

    if (aes_ccm_decrypt(tk, tkbits, nonce, CCMP_NONCE_LEN, aad, aad_len,
                        in + CCMP_HDR_LEN, ctlen,
                        in + CCMP_HDR_LEN + ctlen, miclen,
                        pt_out) != 0)
        return -1;

    *pt_len = ctlen;
    return 0;
}

/* ===================================================================== *
 *  Self-test: IEEE 802.11 CCMP-128 (802.11-2012 M.6.4 / 802.11-2020     *
 *  J.5.4) and CCMP-256 known-answer vectors + round-trip + tamper.      *
 * ===================================================================== */

static int ccmp_eq(const unsigned char *a, const unsigned char *b, int n) {
    int i, d = 0;
    for (i = 0; i < n; i++) d |= (a[i] ^ b[i]);
    return d == 0;
}

int ccmp_selftest(void) {
    /* Shared 802.11 MAC header (24 bytes, non-QoS Data) and PN from the
     * IEEE CCMP test vector. */
    static const unsigned char hdr[24] = {
        0x08,0x48,0xc3,0x2c,0x0f,0xd2,0xe1,0x28,
        0xa5,0x7c,0x50,0x30,0xf1,0x84,0x44,0x08,
        0xab,0xae,0xa5,0xb8,0xfc,0xba,0x80,0x33
    };
    /* pn6 big-endian (pn6[0]=PN5 .. pn6[5]=PN0); on-wire pn[] = B5 03 97 76 E7 0C. */
    static const unsigned char pn6[6] = { 0xB5,0x03,0x97,0x76,0xE7,0x0C };
    static const unsigned char plain[20] = {
        0xf8,0xba,0x1a,0x55,0xd0,0x2f,0x85,0xae,
        0x96,0x7b,0xb6,0x2f,0xb6,0xcd,0xa8,0xeb,
        0x7e,0x78,0xa0,0x50
    };

    unsigned char out[64];
    unsigned char rt[64];
    int out_len, rt_len;

    /* ---- CCMP-128 (TK = 16 bytes, MIC = 8) -------------------------- *
     * IEEE Std 802.11-2012 M.6.4. Expected encrypted MPDU body (CCMP    *
     * header || ciphertext || 8-byte MIC), 8 + 20 + 8 = 36 bytes.       */
    {
        static const unsigned char tk[16] = {
            0xc9,0x7c,0x1f,0x67,0xce,0x37,0x11,0x85,
            0x51,0x4a,0x8a,0x19,0xf2,0xbd,0xd5,0x2f
        };
        static const unsigned char expect[36] = {
            /* CCMP header */
            0x0c,0xe7,0x00,0x20,0x76,0x97,0x03,0xb5,
            /* ciphertext */
            0xf3,0xd0,0xa2,0xfe,0x9a,0x3d,0xbf,0x23,
            0x42,0xa6,0x43,0xe4,0x32,0x46,0xe8,0x0c,
            0x3c,0x04,0xd0,0x19,
            /* MIC */
            0x78,0x45,0xce,0x0b,0x16,0xf9,0x76,0x23
        };
        if (ccmp_encrypt(tk, 128, hdr, 24, 0, pn6, plain, 20, out, &out_len) != 0)
            return 1;
        if (out_len != 36) return 2;
        if (!ccmp_eq(out, expect, 36)) return 3;

        /* round-trip */
        if (ccmp_decrypt(tk, 128, hdr, 24, 0, out, out_len, rt, &rt_len) != 0)
            return 4;
        if (rt_len != 20 || !ccmp_eq(rt, plain, 20)) return 5;

        /* tamper: flip a ciphertext byte -> MIC must fail */
        out[10] ^= 0x20;
        if (ccmp_decrypt(tk, 128, hdr, 24, 0, out, out_len, rt, &rt_len) == 0)
            return 6;
        out[10] ^= 0x20;
        /* tamper: flip a MIC byte -> must fail */
        out[out_len - 1] ^= 0x01;
        if (ccmp_decrypt(tk, 128, hdr, 24, 0, out, out_len, rt, &rt_len) == 0)
            return 7;
        out[out_len - 1] ^= 0x01;
    }

    /* ---- CCMP-256 (TK = 32 bytes, MIC = 16) ------------------------- *
     * IEEE P802.11ac/D7.0 M.6.4. Full embedded KAT (hostap test_vectors).*/
    {
        static const unsigned char tk[32] = {
            0xc9,0x7c,0x1f,0x67,0xce,0x37,0x11,0x85,
            0x51,0x4a,0x8a,0x19,0xf2,0xbd,0xd5,0x2f,
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
        };
        /* Expected encrypted MPDU body = encr[24..] from the published vector:
         * CCMP header(8) || ciphertext(20) || MIC(16) = 44 bytes. */
        static const unsigned char expect[44] = {
            /* CCMP header */
            0x0c,0xe7,0x00,0x20,0x76,0x97,0x03,0xb5,
            /* ciphertext */
            0x6d,0x15,0x5d,0x88,0x32,0x66,0x82,0x56,
            0xd6,0xa9,0x2b,0x78,0xe1,0x1d,0x8e,0x54,
            0x49,0x5d,0xd1,0x74,
            /* MIC */
            0x80,0xaa,0x56,0xc9,0x49,0x2e,0x88,0x2b,
            0x97,0x64,0x2f,0x80,0xd5,0x0f,0xe9,0x7b
        };
        if (ccmp_encrypt(tk, 256, hdr, 24, 0, pn6, plain, 20, out, &out_len) != 0)
            return 8;
        if (out_len != 44) return 9;
        if (!ccmp_eq(out, expect, 44)) return 10;

        if (ccmp_decrypt(tk, 256, hdr, 24, 0, out, out_len, rt, &rt_len) != 0)
            return 11;
        if (rt_len != 20 || !ccmp_eq(rt, plain, 20)) return 12;

        out[out_len - 3] ^= 0x08;   /* tamper a MIC byte */
        if (ccmp_decrypt(tk, 256, hdr, 24, 0, out, out_len, rt, &rt_len) == 0)
            return 13;
        out[out_len - 3] ^= 0x08;
    }

    return 0;
}
