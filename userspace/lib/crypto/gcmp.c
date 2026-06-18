/*
 * gcmp.c -- IEEE 802.11 GCMP (GCM with GMAC Protocol) wrapper.
 * ============================================================
 * No libc, no headers beyond gcmp.h / wpa_aad.h / aes.h. Implements IEEE
 * 802.11-2020 sec. 12.5.5: build the AAD and the 12-byte nonce (A2 || PN)
 * from the MAC header, then run AES-GCM (reusing aes_gcm_encrypt /
 * aes_gcm_decrypt, whose 12-byte-IV J0 = IV || 0x00000001 is exactly the
 * GCMP nonce model), and frame the result as GCMP-header || ct || MIC(16).
 */

#include "gcmp.h"
#include "wpa_aad.h"
#include "aes.h"

#define GCMP_HDR_LEN    8
#define GCMP_NONCE_LEN  12
#define GCMP_MIC_LEN    16
#define ETH_ALEN        6
#define GCMP_MAX_PT     2304

static int gcmp_keybits_ok(int tkbits) {
    return tkbits == 128 || tkbits == 256;
}

int gcmp_encrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *pn6,
                 const uint8_t *pt, int ptlen,
                 uint8_t *out, int *out_len) {
    aes_ctx c;
    uint8_t aad[WPA_AAD_MAXLEN];
    uint8_t nonce[GCMP_NONCE_LEN];
    int aad_len;
    const uint8_t *a2;

    if (!gcmp_keybits_ok(tkbits)) return -1;
    if (tk == 0 || mac_hdr == 0 || pn6 == 0 || out == 0 || out_len == 0)
        return -1;
    if (ptlen < 0 || ptlen > GCMP_MAX_PT) return -1;
    if (ptlen > 0 && pt == 0) return -1;
    if (hdr_len < 24) return -1;

    if (wpa_build_aad(mac_hdr, hdr_len, has_qos, aad, &aad_len) != 0)
        return -1;

    /* GCMP nonce = A2(6) || PN(6, big-endian); no Flags octet. */
    a2 = mac_hdr + 4 + ETH_ALEN;
    wpa_build_nonce(a2, pn6, 0, 1 /* gcmp */, nonce);

    /* GCMP header (same layout as CCMP). pn6 big-endian. */
    out[0] = pn6[5];                /* PN0 */
    out[1] = pn6[4];                /* PN1 */
    out[2] = 0x00;                  /* Rsvd */
    out[3] = 0x20;                  /* ExtIV=1, KeyID=0 */
    out[4] = pn6[3];                /* PN2 */
    out[5] = pn6[2];                /* PN3 */
    out[6] = pn6[1];                /* PN4 */
    out[7] = pn6[0];                /* PN5 */

    aes_set_encrypt_key(&c, tk, tkbits);
    if (aes_gcm_encrypt(&c, nonce, aad, (unsigned long)aad_len,
                        pt, (unsigned long)ptlen,
                        out + GCMP_HDR_LEN,                  /* ciphertext */
                        out + GCMP_HDR_LEN + ptlen) != 0)    /* MIC (16)   */
        return -1;

    *out_len = GCMP_HDR_LEN + ptlen + GCMP_MIC_LEN;
    return 0;
}

int gcmp_decrypt(const uint8_t *tk, int tkbits,
                 const uint8_t *mac_hdr, int hdr_len, int has_qos,
                 const uint8_t *in, int in_len,
                 uint8_t *pt_out, int *pt_len) {
    aes_ctx c;
    uint8_t aad[WPA_AAD_MAXLEN];
    uint8_t nonce[GCMP_NONCE_LEN];
    uint8_t pn6[6];
    int aad_len, ctlen;
    const uint8_t *a2;

    if (!gcmp_keybits_ok(tkbits)) return -1;
    if (tk == 0 || mac_hdr == 0 || in == 0 || pt_out == 0 || pt_len == 0)
        return -1;
    if (hdr_len < 24) return -1;
    if (in_len < GCMP_HDR_LEN + GCMP_MIC_LEN) return -1;
    ctlen = in_len - GCMP_HDR_LEN - GCMP_MIC_LEN;
    if (ctlen > GCMP_MAX_PT) return -1;

    /* Recover the big-endian PN from the GCMP header. */
    pn6[0] = in[7];                 /* PN5 */
    pn6[1] = in[6];                 /* PN4 */
    pn6[2] = in[5];                 /* PN3 */
    pn6[3] = in[4];                 /* PN2 */
    pn6[4] = in[1];                 /* PN1 */
    pn6[5] = in[0];                 /* PN0 */

    if (wpa_build_aad(mac_hdr, hdr_len, has_qos, aad, &aad_len) != 0)
        return -1;

    a2 = mac_hdr + 4 + ETH_ALEN;
    wpa_build_nonce(a2, pn6, 0, 1 /* gcmp */, nonce);

    aes_set_encrypt_key(&c, tk, tkbits);
    if (aes_gcm_decrypt(&c, nonce, aad, (unsigned long)aad_len,
                        in + GCMP_HDR_LEN, (unsigned long)ctlen,
                        in + GCMP_HDR_LEN + ctlen,           /* MIC */
                        pt_out) != 0)
        return -1;

    *pt_len = ctlen;
    return 0;
}

/* ===================================================================== *
 *  Self-test: IEEE 802.11 GCMP-128 (802.11ad-2012 M.11.1 / 802.11-2020  *
 *  J.6) and GCMP-256 known-answer vectors + round-trip + tamper.        *
 * ===================================================================== */

static int gcmp_eq(const unsigned char *a, const unsigned char *b, int n) {
    int i, d = 0;
    for (i = 0; i < n; i++) d |= (a[i] ^ b[i]);
    return d == 0;
}

int gcmp_selftest(void) {
    /* Shared 26-byte QoS-Data MAC header and PN (from the GCMP vectors). */
    static const unsigned char hdr[26] = {
        0x88,0x48,0x0b,0x00,0x0f,0xd2,0xe1,0x28,
        0xa5,0x7c,0x50,0x30,0xf1,0x84,0x44,0x08,
        0x50,0x30,0xf1,0x84,0x44,0x08,0x80,0x33,
        0x03,0x00                       /* QoS Control */
    };
    /* on-wire pn[] = 00 89 5F 5F 2B 08, big-endian pn6[0]=PN5..pn6[5]=PN0. */
    static const unsigned char pn6[6] = { 0x00,0x89,0x5F,0x5F,0x2B,0x08 };
    static const unsigned char plain[40] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
        0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27
    };

    unsigned char out[128];
    unsigned char rt[64];
    int out_len, rt_len;

    /* ---- GCMP-128 (TK = 16, MIC = 16). IEEE 802.11ad-2012 M.11.1. ---- *
     * Expected ciphertext (40) and MIC (16) from the published vector.   */
    {
        static const unsigned char tk[16] = {
            0xc9,0x7c,0x1f,0x67,0xce,0x37,0x11,0x85,
            0x51,0x4a,0x8a,0x19,0xf2,0xbd,0xd5,0x2f
        };
        static const unsigned char ct[40] = {
            0x60,0xe9,0x70,0x0c,0xc4,0xd4,0x0a,0xc6,
            0xd2,0x88,0xb2,0x01,0xc3,0x8f,0x5b,0xf0,
            0x8b,0x80,0x74,0x42,0x64,0x0a,0x15,0x96,
            0xe5,0xdb,0xda,0xd4,0x1d,0x1f,0x36,0x23,
            0xf4,0x5d,0x7a,0x12,0xdb,0x7a,0xfb,0x23
        };
        static const unsigned char mic[16] = {
            0xde,0xf6,0x19,0xc2,0xa3,0x74,0xb6,0xdf,
            0x66,0xff,0xa5,0x3b,0x6c,0x69,0xd7,0x9e
        };
        if (gcmp_encrypt(tk, 128, hdr, 26, 1, pn6, plain, 40, out, &out_len) != 0)
            return 1;
        if (out_len != GCMP_HDR_LEN + 40 + GCMP_MIC_LEN) return 2;   /* 64 */
        /* ciphertext */
        if (!gcmp_eq(out + GCMP_HDR_LEN, ct, 40)) return 3;
        /* MIC */
        if (!gcmp_eq(out + GCMP_HDR_LEN + 40, mic, 16)) return 4;

        if (gcmp_decrypt(tk, 128, hdr, 26, 1, out, out_len, rt, &rt_len) != 0)
            return 5;
        if (rt_len != 40 || !gcmp_eq(rt, plain, 40)) return 6;

        out[12] ^= 0x04;            /* tamper a ciphertext byte */
        if (gcmp_decrypt(tk, 128, hdr, 26, 1, out, out_len, rt, &rt_len) == 0)
            return 7;
        out[12] ^= 0x04;
        out[out_len - 1] ^= 0x80;   /* tamper a MIC byte */
        if (gcmp_decrypt(tk, 128, hdr, 26, 1, out, out_len, rt, &rt_len) == 0)
            return 8;
        out[out_len - 1] ^= 0x80;
    }

    /* ---- GCMP-256 (TK = 32, MIC = 16). IEEE P802.11ac/D7.0 M.11.1. --- *
     * Full embedded KAT from hostap test_vectors: payload 40 bytes.      */
    {
        static const unsigned char tk[32] = {
            0xc9,0x7c,0x1f,0x67,0xce,0x37,0x11,0x85,
            0x51,0x4a,0x8a,0x19,0xf2,0xbd,0xd5,0x2f,
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
        };
        /* encr[] from the vector, offset 26 (after hdr) + 8 (GCMP hdr):
         * ciphertext(40) then MIC(16). */
        static const unsigned char ct[40] = {
            0x65,0x83,0x43,0xc8,0xb1,0x44,0x47,0xd9,
            0x21,0x1d,0xef,0xd4,0x6a,0xd8,0x9c,0x71,
            0x0c,0x6f,0xc3,0x33,0x33,0x23,0x6e,0x39,
            0x97,0xb9,0x17,0x6a,0x5a,0x8b,0xe7,0x79,
            0xb2,0x12,0x66,0x55,0x5e,0x70,0xad,0x79
        };
        static const unsigned char mic[16] = {
            0x11,0x43,0x16,0x85,0x90,0x95,0x47,0x3d,
            0x5b,0x1b,0xd5,0x96,0xb3,0xde,0xa3,0xbf
        };
        if (gcmp_encrypt(tk, 256, hdr, 26, 1, pn6, plain, 40, out, &out_len) != 0)
            return 9;
        if (out_len != GCMP_HDR_LEN + 40 + GCMP_MIC_LEN) return 10;
        if (!gcmp_eq(out + GCMP_HDR_LEN, ct, 40)) return 11;
        if (!gcmp_eq(out + GCMP_HDR_LEN + 40, mic, 16)) return 12;

        if (gcmp_decrypt(tk, 256, hdr, 26, 1, out, out_len, rt, &rt_len) != 0)
            return 13;
        if (rt_len != 40 || !gcmp_eq(rt, plain, 40)) return 14;

        out[GCMP_HDR_LEN + 40 + 4] ^= 0x02;   /* tamper a MIC byte */
        if (gcmp_decrypt(tk, 256, hdr, 26, 1, out, out_len, rt, &rt_len) == 0)
            return 15;
        out[GCMP_HDR_LEN + 40 + 4] ^= 0x02;
    }

    return 0;
}
