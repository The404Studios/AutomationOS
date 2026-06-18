/*
 * wpa.c -- freestanding WPA2 4-way-handshake key primitives.
 * ==========================================================
 *
 * No libc, no syscalls, no malloc, no float, no standard headers. Fixed stack
 * buffers only. Implements the supplicant-side cryptography of the WPA2
 * (RSN/CCMP) 4-way handshake:
 *
 *   wpa_ptk()          PTK = PRF-X(PMK, "Pairwise key expansion", data)
 *   wpa_eapol_mic()    MIC = HMAC-SHA1(KCK, frame)[0..16]
 *   wpa_decrypt_gtk()  GTK = AES-Key-Unwrap(KEK, wrapped)   (RFC 3394)
 *   wpa_selftest()     KAT (PTK + MIC) + self-consistency backstops
 *
 * IEEE 802.11-2020 12.7.1.2:
 *   data = Min(AA,SPA) || Max(AA,SPA) || Min(ANonce,SNonce) || Max(ANonce,SNonce)
 *   PRF-X(K, A, B): for i = 0,1,...:  R || HMAC-SHA1(K, A || 0x00 || B || i),
 *                   concatenated then truncated to X bytes.  X = 48 for CCMP.
 *
 * Build flags:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#include "wpa.h"
#include "../crypto/hmac.h"     /* hmac_sha1(key,klen,msg,mlen,out[20]) */
#include "../crypto/keywrap.h"  /* aes_key_unwrap(kek,kek_bits,in,n_blocks,out) */

/* The KAT backstop also exercises the wrap direction; pull it in so the
 * self-test can produce a wrapped GTK without an external reference. The
 * declaration lives in keywrap.h (aes_key_wrap). */

#define WPA_SHA1_LEN  20
#define WPA_MIC_LEN   16

/* ------------------------------------------------------------------ *
 * Local helpers (no libc)
 * ------------------------------------------------------------------ */

static void wpa_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static void wpa_memset(void *dst, int v, unsigned long n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)v;
}

/* lexicographic compare of n bytes: <0, 0, >0 like memcmp */
static int wpa_cmp(const unsigned char *a, const unsigned char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

/* constant-ish fixed-length difference; 0 iff equal */
static int wpa_diff(const unsigned char *a, const unsigned char *b, int n)
{
    unsigned char d = 0;
    int i;
    for (i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return (int)d;
}

/* ------------------------------------------------------------------ *
 * wpa_ptk -- IEEE 802.11-2020 12.7.1.2 pairwise key derivation.
 * ------------------------------------------------------------------ */
void wpa_ptk(const uint8_t pmk[32],
             const uint8_t *aa, const uint8_t *spa,
             const uint8_t *anonce, const uint8_t *snonce,
             uint8_t *ptk, int ptk_len)
{
    /* "Pairwise key expansion" -- 22 bytes, NOT NUL-terminated in the PRF. */
    static const unsigned char label[22] = {
        'P','a','i','r','w','i','s','e',' ',
        'k','e','y',' ',
        'e','x','p','a','n','s','i','o','n'
    };
    /* The per-iteration PRF message is fixed except for its last byte (the
     * counter i):  msg = label(22) || 0x00 || data(76) || i.   100 bytes. */
    unsigned char msg[100];
    unsigned char digest[WPA_SHA1_LEN];
    int off, i, written;
    unsigned char counter;

    if (!ptk || ptk_len <= 0) return;

    /* msg[0..22]  = "Pairwise key expansion" || 0x00 (the single NUL the
     * 802.11 PRF places between the label A and the data B). */
    for (i = 0; i < 22; i++) msg[i] = label[i];
    msg[22] = 0x00;

    /* msg[23..99) = Min(AA,SPA) || Max(AA,SPA) || Min(ANonce,SNonce)
     *               || Max(ANonce,SNonce)  -- 76 bytes of PRF data B. */
    if (wpa_cmp(aa, spa, 6) < 0) {
        wpa_memcpy(msg + 23, aa,  6);
        wpa_memcpy(msg + 29, spa, 6);
    } else {
        wpa_memcpy(msg + 23, spa, 6);
        wpa_memcpy(msg + 29, aa,  6);
    }
    if (wpa_cmp(anonce, snonce, 32) < 0) {
        wpa_memcpy(msg + 35, anonce, 32);
        wpa_memcpy(msg + 67, snonce, 32);
    } else {
        wpa_memcpy(msg + 35, snonce, 32);
        wpa_memcpy(msg + 67, anonce, 32);
    }
    /* msg[99] = the 1-byte counter i, set per iteration below. */

    /* R = R || HMAC-SHA1(PMK, label || 0x00 || data || i_byte), i = 0,1,... */
    written = 0;
    counter = 0;
    while (written < ptk_len) {
        int chunk, remaining;

        msg[99] = counter;
        hmac_sha1(pmk, 32, msg, 100, digest);

        remaining = ptk_len - written;
        chunk = (remaining < WPA_SHA1_LEN) ? remaining : WPA_SHA1_LEN;
        for (off = 0; off < chunk; off++) ptk[written + off] = digest[off];
        written += chunk;
        counter++;
    }

    wpa_memset(msg, 0, sizeof(msg));
    wpa_memset(digest, 0, sizeof(digest));
}

/* ------------------------------------------------------------------ *
 * wpa_eapol_mic -- HMAC-SHA1-128 over the EAPOL frame.
 * The caller must have zeroed the MIC field already.
 * ------------------------------------------------------------------ */
void wpa_eapol_mic(const uint8_t *kck, const uint8_t *eapol_frame, int len,
                   uint8_t mic[16])
{
    unsigned char full[WPA_SHA1_LEN];
    int i;

    if (len < 0) len = 0;
    hmac_sha1(kck, 16, eapol_frame, (unsigned long)len, full);
    for (i = 0; i < WPA_MIC_LEN; i++) mic[i] = full[i];   /* truncate to 16 */
    wpa_memset(full, 0, sizeof(full));
}

/* ------------------------------------------------------------------ *
 * wpa_decrypt_gtk -- AES Key Unwrap of the GTK (RFC 3394, KEK = 128-bit).
 * ------------------------------------------------------------------ */
int wpa_decrypt_gtk(const uint8_t *kek, const uint8_t *wrapped, int wrapped_len,
                    uint8_t *gtk_out)
{
    int n_blocks;

    if (!kek || !wrapped || !gtk_out) return -1;
    /* wrapped = (n+1) 8-byte blocks, n >= 2 (smallest GTK = 16 bytes). */
    if (wrapped_len < 24 || (wrapped_len & 7) != 0) return -1;

    n_blocks = (wrapped_len / 8) - 1;       /* recovered plaintext blocks */
    if (aes_key_unwrap(kek, 128, wrapped, n_blocks, gtk_out) != 0)
        return -2;                          /* integrity check failed */

    return n_blocks * 8;                    /* recovered GTK length in bytes */
}

/* ================================================================== *
 * Self-test
 * ==================================================================
 *
 * KAT (mandatory) -- a deterministic IEEE-802.11-PRF 4-way vector. The inputs
 * are fixed and the expected PTK / MIC were produced by an independent,
 * reference HMAC-SHA1 implementation (Python's hmac+hashlib), so this is a
 * genuine known-answer test of the PRF and the MIC, not a tautology:
 *
 *   PMK    = f42c6fc5 2df0ebef 9ebb4b90 b38a5f90
 *            2e83fe1b 135a70e2 3aed762e 9710a12e   (= 802.11 J.4.2 vector-1 PMK)
 *   AA     = 02:00:ca:fe:00:02
 *   SPA    = 02:00:ca:fe:00:10
 *   ANonce = 01 02 .. 20      (bytes 0x01..0x20)
 *   SNonce = 21 22 .. 40      (bytes 0x21..0x40)
 *
 *   PTK(48) = 4f3b945d e369629f a2e2ac90 c8ad639a   (KCK)
 *             d1097159 a05f9c1f a0ab15fb cc56cee0   (KEK)
 *             eebdce0d 2da820e8 cc544b44 b87fa992   (TK)
 *
 *   EAPOL frame (99 bytes, header + SNonce, MIC field zeroed) -> MIC =
 *             7189bba9 943e8eaf 3ceac259 6e6f1e16
 *
 * Self-consistency backstops (catch regressions even if a vector is mis-typed):
 *   - recompute the MIC and verify it equals itself (and a 1-bit-flipped
 *     frame yields a *different* MIC);
 *   - wrap a known GTK with aes_key_wrap, then wpa_decrypt_gtk() it back and
 *     verify round-trip equality (and that a tampered wrap is rejected).
 * ------------------------------------------------------------------ */
int wpa_selftest(void)
{
    static const unsigned char pmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,
        0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,
        0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e
    };
    static const unsigned char aa[6]  = { 0x02,0x00,0xca,0xfe,0x00,0x02 };
    static const unsigned char spa[6] = { 0x02,0x00,0xca,0xfe,0x00,0x10 };
    unsigned char anonce[32], snonce[32];
    unsigned char ptk[48];
    int i;

    static const unsigned char exp_ptk[48] = {
        /* KCK */
        0x4f,0x3b,0x94,0x5d,0xe3,0x69,0x62,0x9f,
        0xa2,0xe2,0xac,0x90,0xc8,0xad,0x63,0x9a,
        /* KEK */
        0xd1,0x09,0x71,0x59,0xa0,0x5f,0x9c,0x1f,
        0xa0,0xab,0x15,0xfb,0xcc,0x56,0xce,0xe0,
        /* TK  */
        0xee,0xbd,0xce,0x0d,0x2d,0xa8,0x20,0xe8,
        0xcc,0x54,0x4b,0x44,0xb8,0x7f,0xa9,0x92
    };

    /* ANonce = 0x01..0x20, SNonce = 0x21..0x40 */
    for (i = 0; i < 32; i++) anonce[i] = (unsigned char)(0x01 + i);
    for (i = 0; i < 32; i++) snonce[i] = (unsigned char)(0x21 + i);

    /* ---- KAT 1: PTK derivation -------------------------------------- */
    wpa_memset(ptk, 0, sizeof(ptk));
    wpa_ptk(pmk, aa, spa, anonce, snonce, ptk, 48);
    if (wpa_diff(ptk, exp_ptk, 48) != 0) return 1;

    /* ---- KAT 2: EAPOL MIC ------------------------------------------- *
     * 99-byte frame; MIC field (offset 81..97) zeroed in the message.   */
    {
        static const unsigned char exp_mic[16] = {
            0x71,0x89,0xbb,0xa9,0x94,0x3e,0x8e,0xaf,
            0x3c,0xea,0xc2,0x59,0x6e,0x6f,0x1e,0x16
        };
        unsigned char frame[99];
        unsigned char mic[16];
        unsigned char mic2[16];

        wpa_memset(frame, 0, sizeof(frame));
        /* header: 0203 0075 02 010a 0000 (version/type/len + key info etc.) */
        frame[0]=0x02; frame[1]=0x03; frame[2]=0x00; frame[3]=0x75;
        frame[4]=0x02; frame[5]=0x01; frame[6]=0x0a;
        /* key nonce field carries SNonce (0x21..0x40) at offset 17..48 */
        for (i = 0; i < 32; i++) frame[17 + i] = (unsigned char)(0x21 + i);
        /* offsets 49..98 stay zero (IV, RSC, ID, zeroed-MIC, key-data-len) */

        wpa_eapol_mic(ptk /*KCK = PTK[0..16]*/, frame, 99, mic);
        if (wpa_diff(mic, exp_mic, 16) != 0) return 2;

        /* self-consistency: recompute -> identical; flip a bit -> different. */
        wpa_eapol_mic(ptk, frame, 99, mic2);
        if (wpa_diff(mic, mic2, 16) != 0) return 3;
        frame[17] ^= 0x01;
        wpa_eapol_mic(ptk, frame, 99, mic2);
        if (wpa_diff(mic, mic2, 16) == 0) return 4;  /* must differ */
    }

    /* ---- KAT 3: GTK unwrap round-trip ------------------------------- *
     * Wrap a known 16-byte GTK under the derived KEK (PTK[16..32]) with the
     * RFC-3394 wrapper, then recover it through wpa_decrypt_gtk and check.  */
    {
        static const unsigned char gtk[16] = {
            0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
            0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
        };
        const unsigned char *kek = ptk + 16;
        unsigned char wrapped[24];   /* (2 + 1) * 8 */
        unsigned char out[16];
        int rc;

        if (aes_key_wrap(kek, 128, gtk, 2, wrapped) != 0) return 5;

        rc = wpa_decrypt_gtk(kek, wrapped, 24, out);
        if (rc != 16) return 6;
        if (wpa_diff(out, gtk, 16) != 0) return 7;

        /* tamper -> unwrap MUST reject (RFC 3394 integrity). */
        wrapped[0] ^= 0x01;
        rc = wpa_decrypt_gtk(kek, wrapped, 24, out);
        if (rc >= 0) return 8;

        /* bad-length guards. */
        if (wpa_decrypt_gtk(kek, wrapped, 23, out) >= 0) return 9;  /* not /8 */
        if (wpa_decrypt_gtk(kek, wrapped, 8,  out) >= 0) return 10; /* too short */
    }

    return 0; /* all known-answer + consistency checks passed */
}
