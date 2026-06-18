/*
 * ccm.c -- freestanding generic AES-CCM (NIST SP 800-38C).
 * ========================================================
 * No libc, no headers beyond aes.h / ccm.h. The CBC-MAC is implemented inline
 * from aes_encrypt_block (the library's AES-CBC mode is NOT reusable here:
 * CBC-MAC has no IV add on the first block and uses the B0 formatted block),
 * and CTR-mode encryption reuses the same forward block transform.
 *
 * The length-of-length parameter L is derived from the nonce length as
 * L = 15 - Nlen, exactly as NIST SP 800-38C formats B0 / Ctr_i. This lets a
 * single core reproduce both the NIST Appendix C examples (Nlen 7/8/12, so
 * L = 8/7/3) and the IEEE 802.11 CCMP case (Nlen = 13, L = 2). The payload is
 * bounded at CCM_MAX_PT octets (the largest 802.11 MSDU), which always fits in
 * the L-octet length field for the L values we accept.
 */

#include "ccm.h"
#include "aes.h"

#define CCM_MIN_NLEN  7          /* L = 8 (largest L we format)            */
#define CCM_MAX_NLEN  13         /* L = 2 (the 802.11 CCMP case)           */
#define CCM_MAX_PT    2304       /* bound the payload (802.11 MSDU cap)    */

static void ccm_memset(unsigned char *d, int v, int n) {
    int i;
    for (i = 0; i < n; i++) d[i] = (unsigned char)v;
}

/* XOR a 16-byte block into the CBC-MAC accumulator, then encrypt in place. */
static void cbcmac_block(const aes_ctx *c, unsigned char *mac,
                         const unsigned char *blk) {
    int i;
    unsigned char tmp[16];
    for (i = 0; i < 16; i++) tmp[i] = (unsigned char)(mac[i] ^ blk[i]);
    aes_encrypt_block(c, tmp, mac);
}

/*
 * Compute the CBC-MAC T over the formatted input (B0 || AAD-encoding || AAD ||
 * pad || payload || pad), leaving the raw 16-byte MAC in mac[16]. The caller
 * XORs S0 and truncates to taglen.  L = 15 - nlen.
 */
static void ccm_cbc_mac(const aes_ctx *c,
                        const unsigned char *nonce, int nlen,
                        const unsigned char *aad, int aadlen,
                        const unsigned char *payload, int plen,
                        int taglen, unsigned char mac[16]) {
    unsigned char blk[16];
    int i, off, L;

    L = 15 - nlen;

    /* ---- B0 = Flags || Nonce(nlen) || Q(L octets, big-endian) ------- */
    ccm_memset(blk, 0, 16);
    blk[0] = (unsigned char)((aadlen > 0 ? 0x40 : 0x00) |    /* Adata */
                             (((taglen - 2) / 2) << 3) |     /* M'    */
                             (L - 1));                       /* L'    */
    for (i = 0; i < nlen; i++) blk[1 + i] = nonce[i];
    /* Q occupies the last L octets, big-endian; plen always fits in 2 octets,
     * so the higher octets stay zero. */
    blk[14] = (unsigned char)((plen >> 8) & 0xff);
    blk[15] = (unsigned char)(plen & 0xff);

    ccm_memset(mac, 0, 16);
    cbcmac_block(c, mac, blk);     /* MAC = E(K, B0) */

    /* ---- Associated data: length encoding then the AAD itself ------- */
    if (aadlen > 0) {
        ccm_memset(blk, 0, 16);
        /* For 0 < aadlen < 2^16 - 2^8 the length is encoded as two octets
         * (big-endian). Larger encodings (0xFFFE.. / 0xFFFF..) are not used
         * by 802.11 or the NIST examples 1-3 and are out of scope here. */
        blk[0] = (unsigned char)((aadlen >> 8) & 0xff);
        blk[1] = (unsigned char)(aadlen & 0xff);
        off = 0;
        i = 2;                     /* first AAD block already holds 2 len bytes */
        while (off < aadlen) {
            blk[i++] = aad[off++];
            if (i == 16) {
                cbcmac_block(c, mac, blk);
                ccm_memset(blk, 0, 16);
                i = 0;
            }
        }
        if (i != 0) {              /* flush a partial (zero-padded) block */
            cbcmac_block(c, mac, blk);
        }
    }

    /* ---- Payload, zero-padded to a block boundary ------------------- */
    off = 0;
    while (off < plen) {
        int n = plen - off;
        if (n > 16) n = 16;
        ccm_memset(blk, 0, 16);
        for (i = 0; i < n; i++) blk[i] = payload[off + i];
        cbcmac_block(c, mac, blk);
        off += 16;
    }
}

/* Build the CTR pre-counter block Ctr_i = Flags(L-1) || Nonce(nlen) ||
 * i(L octets, big-endian). */
static void ccm_ctr_block(const unsigned char *nonce, int nlen,
                          unsigned int i, unsigned char out[16]) {
    int k, L = 15 - nlen;
    ccm_memset(out, 0, 16);
    out[0] = (unsigned char)(L - 1);
    for (k = 0; k < nlen; k++) out[1 + k] = nonce[k];
    /* counter in the low octets, big-endian (i fits in 2 octets for our caps) */
    out[14] = (unsigned char)((i >> 8) & 0xff);
    out[15] = (unsigned char)(i & 0xff);
}

/* CTR-encrypt (or -decrypt) src into dst using counter blocks 1.. */
static void ccm_ctr_crypt(const aes_ctx *c, const unsigned char *nonce, int nlen,
                          const unsigned char *src, int len, unsigned char *dst) {
    unsigned char ctr[16];
    unsigned char ks[16];
    int off = 0;
    unsigned int i = 1;
    while (off < len) {
        int n = len - off, j;
        if (n > 16) n = 16;
        ccm_ctr_block(nonce, nlen, i, ctr);
        aes_encrypt_block(c, ctr, ks);
        for (j = 0; j < n; j++) dst[off + j] = (unsigned char)(src[off + j] ^ ks[j]);
        off += 16;
        i++;
    }
}

static int ccm_args_ok(int keybits, int nlen, int aadlen, int len, int taglen) {
    if (keybits != 128 && keybits != 256) return 0;
    if (nlen < CCM_MIN_NLEN || nlen > CCM_MAX_NLEN) return 0;
    if (taglen < 4 || taglen > 16 || (taglen & 1)) return 0;
    if (aadlen < 0 || len < 0) return 0;
    if (len > CCM_MAX_PT) return 0;
    /* Two-octet AAD length encoding range. */
    if (aadlen >= 0xff00) return 0;
    return 1;
}

int aes_ccm_encrypt(const uint8_t *key, int keybits,
                    const uint8_t *nonce, int nlen,
                    const uint8_t *aad, int aadlen,
                    const uint8_t *pt, int ptlen,
                    int taglen,
                    uint8_t *ct_out, uint8_t *tag_out) {
    aes_ctx c;
    unsigned char mac[16];
    unsigned char s0[16];
    unsigned char ctr0[16];
    int i;

    if (!ccm_args_ok(keybits, nlen, aadlen, ptlen, taglen)) return -1;
    if (key == 0 || nonce == 0 || tag_out == 0) return -1;
    if (ptlen > 0 && (pt == 0 || ct_out == 0)) return -1;
    if (aadlen > 0 && aad == 0) return -1;

    aes_set_encrypt_key(&c, key, keybits);

    /* Tag is computed over the *plaintext*. */
    ccm_cbc_mac(&c, nonce, nlen, aad, aadlen, pt, ptlen, taglen, mac);

    /* S0 = E(K, Ctr_0); tag = (MAC ^ S0) truncated to taglen. */
    ccm_ctr_block(nonce, nlen, 0, ctr0);
    aes_encrypt_block(&c, ctr0, s0);
    for (i = 0; i < taglen; i++) tag_out[i] = (unsigned char)(mac[i] ^ s0[i]);

    /* Encrypt the payload with CTR blocks 1.. */
    if (ptlen > 0) ccm_ctr_crypt(&c, nonce, nlen, pt, ptlen, ct_out);

    return 0;
}

int aes_ccm_decrypt(const uint8_t *key, int keybits,
                    const uint8_t *nonce, int nlen,
                    const uint8_t *aad, int aadlen,
                    const uint8_t *ct, int ctlen,
                    const uint8_t *tag, int taglen,
                    uint8_t *pt_out) {
    aes_ctx c;
    unsigned char mac[16];
    unsigned char s0[16];
    unsigned char ctr0[16];
    unsigned char expect[16];
    unsigned char plain[CCM_MAX_PT];
    int i, diff;

    if (!ccm_args_ok(keybits, nlen, aadlen, ctlen, taglen)) return -1;
    if (key == 0 || nonce == 0 || tag == 0) return -1;
    if (ctlen > 0 && (ct == 0 || pt_out == 0)) return -1;
    if (aadlen > 0 && aad == 0) return -1;

    aes_set_encrypt_key(&c, key, keybits);

    /* CTR-decrypt into a local buffer first (do not release to pt_out until
     * the MIC verifies). */
    if (ctlen > 0) ccm_ctr_crypt(&c, nonce, nlen, ct, ctlen, plain);

    /* Recompute the MAC over the recovered plaintext and form the tag. */
    ccm_cbc_mac(&c, nonce, nlen, aad, aadlen, plain, ctlen, taglen, mac);
    ccm_ctr_block(nonce, nlen, 0, ctr0);
    aes_encrypt_block(&c, ctr0, s0);
    for (i = 0; i < taglen; i++) expect[i] = (unsigned char)(mac[i] ^ s0[i]);

    diff = 0;
    for (i = 0; i < taglen; i++) diff |= (expect[i] ^ tag[i]);
    if (diff != 0) {
        ccm_memset(plain, 0, ctlen > 0 ? ctlen : 1);
        return -1;             /* authentication failure */
    }

    for (i = 0; i < ctlen; i++) pt_out[i] = plain[i];
    return 0;
}

/* ===================================================================== *
 *  Self-test: NIST SP 800-38C Appendix C, Examples 1-3 (known-answer),  *
 *  plus an L=2 round-trip and a tamper test.                            *
 * ===================================================================== */

static int ccm_eq(const unsigned char *a, const unsigned char *b, int n) {
    int i, d = 0;
    for (i = 0; i < n; i++) d |= (a[i] ^ b[i]);
    return d == 0;
}

int ccm_selftest(void) {
    /* NIST SP 800-38C Appendix C examples 1-3 all share this key. */
    static const unsigned char K[16] = {
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,
        0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f
    };
    unsigned char ct[64], tag[16], pt[64];

    /* ---- Example 1: Nlen=56b(7B, L=8), Tlen=32b(4B), Alen=64b(8B),
     *      Plen=32b(4B). Expected C = 7162015b, T = 4dac255d. --------- */
    {
        static const unsigned char N[7] = { 0x10,0x11,0x12,0x13,0x14,0x15,0x16 };
        static const unsigned char A[8] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
        static const unsigned char P[4] = { 0x20,0x21,0x22,0x23 };
        static const unsigned char C[4] = { 0x71,0x62,0x01,0x5b };
        static const unsigned char T[4] = { 0x4d,0xac,0x25,0x5d };
        if (aes_ccm_encrypt(K, 128, N, 7, A, 8, P, 4, 4, ct, tag) != 0) return 1;
        if (!ccm_eq(ct, C, 4)) return 2;
        if (!ccm_eq(tag, T, 4)) return 3;
        if (aes_ccm_decrypt(K, 128, N, 7, A, 8, ct, 4, tag, 4, pt) != 0) return 4;
        if (!ccm_eq(pt, P, 4)) return 5;
    }

    /* ---- Example 2: Nlen=64b(8B, L=7), Tlen=48b(6B), Alen=128b(16B),
     *      Plen=128b(16B). Expected C = d2a1f0e0 51ea5f62 081a7792 073d593d,
     *      T = 1fc64fbf accd. ------------------------------------------ */
    {
        static const unsigned char N[8] = {
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17 };
        static const unsigned char A[16] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
        static const unsigned char P[16] = {
            0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
            0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f };
        static const unsigned char C[16] = {
            0xd2,0xa1,0xf0,0xe0,0x51,0xea,0x5f,0x62,
            0x08,0x1a,0x77,0x92,0x07,0x3d,0x59,0x3d };
        static const unsigned char T[6] = {
            0x1f,0xc6,0x4f,0xbf,0xac,0xcd };
        if (aes_ccm_encrypt(K, 128, N, 8, A, 16, P, 16, 6, ct, tag) != 0) return 6;
        if (!ccm_eq(ct, C, 16)) return 7;
        if (!ccm_eq(tag, T, 6)) return 8;
        if (aes_ccm_decrypt(K, 128, N, 8, A, 16, ct, 16, tag, 6, pt) != 0) return 9;
        if (!ccm_eq(pt, P, 16)) return 10;
    }

    /* ---- Example 3: Nlen=96b(12B, L=3), Tlen=64b(8B), Alen=160b(20B),
     *      Plen=192b(24B). Expected
     *      C = e3b201a9 f5b71a7a 9b1ceaec cd97e70b 6176aad9 a4428aa5,
     *      T = 484392fb c1b09951. -------------------------------------- */
    {
        static const unsigned char N[12] = {
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
            0x18,0x19,0x1a,0x1b };
        static const unsigned char A[20] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
            0x10,0x11,0x12,0x13 };
        static const unsigned char P[24] = {
            0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
            0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
            0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37 };
        static const unsigned char C[24] = {
            0xe3,0xb2,0x01,0xa9,0xf5,0xb7,0x1a,0x7a,
            0x9b,0x1c,0xea,0xec,0xcd,0x97,0xe7,0x0b,
            0x61,0x76,0xaa,0xd9,0xa4,0x42,0x8a,0xa5 };
        static const unsigned char T[8] = {
            0x48,0x43,0x92,0xfb,0xc1,0xb0,0x99,0x51 };
        if (aes_ccm_encrypt(K, 128, N, 12, A, 20, P, 24, 8, ct, tag) != 0) return 11;
        if (!ccm_eq(ct, C, 24)) return 12;
        if (!ccm_eq(tag, T, 8)) return 13;
        if (aes_ccm_decrypt(K, 128, N, 12, A, 20, ct, 24, tag, 8, pt) != 0) return 14;
        if (!ccm_eq(pt, P, 24)) return 15;
    }

    /* ---- Backstop: L=2 (802.11) round-trip + tamper test ------------- */
    {
        static const unsigned char nonce[13] = {
            0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
            0x88,0x99,0xaa,0xbb,0xcc };
        static const unsigned char aad[22] = {
            0x40,0x08,0x00,0x01,0x02,0x03,0x04,0x05,
            0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,
            0x0e,0x0f,0x10,0x11,0x12,0x13 };
        static const unsigned char in[20] = {
            0xde,0xad,0xbe,0xef,0x01,0x23,0x45,0x67,
            0x89,0xab,0xcd,0xef,0x00,0x11,0x22,0x33,
            0x44,0x55,0x66,0x77 };
        if (aes_ccm_encrypt(K, 128, nonce, 13, aad, 22, in, 20, 8, ct, tag) != 0)
            return 16;
        if (aes_ccm_decrypt(K, 128, nonce, 13, aad, 22, ct, 20, tag, 8, pt) != 0)
            return 17;
        if (!ccm_eq(pt, in, 20)) return 18;
        ct[3] ^= 0x10;             /* tamper a ciphertext byte */
        if (aes_ccm_decrypt(K, 128, nonce, 13, aad, 22, ct, 20, tag, 8, pt) == 0)
            return 19;
        ct[3] ^= 0x10;
        tag[7] ^= 0x01;            /* tamper a MIC byte */
        if (aes_ccm_decrypt(K, 128, nonce, 13, aad, 22, ct, 20, tag, 8, pt) == 0)
            return 20;
        tag[7] ^= 0x01;
    }

    return 0;
}
