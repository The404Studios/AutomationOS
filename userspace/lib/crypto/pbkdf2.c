/*
 * pbkdf2.c -- freestanding PBKDF2-HMAC-SHA1 (RFC 2898 / PKCS #5 v2.0).
 * ====================================================================
 *
 * No libc, no syscalls, no malloc, no float, no standard headers.
 * Fixed stack buffers only. PRF = HMAC-SHA1 (hlen = 20).
 *
 * Implements:
 *   - pbkdf2_hmac_sha1()  generic RFC 2898 derivation
 *   - wpa_pmk()           WPA/WPA2 PMK = PBKDF2(passphrase, SSID, 4096, 32)
 *   - pbkdf2_selftest()   RFC 6070 (dkLen=20) + IEEE 802.11-2020 J.4.2 (PMK)
 *
 * RFC 2898 §5.2:
 *   DK = T(1) || T(2) || ... || T(l)       (concatenate, truncate to dkLen)
 *   T(i) = U(1) ^ U(2) ^ ... ^ U(c)
 *     U(1) = PRF(P, S || INT(i))           INT(i) = 4-byte big-endian i (1..)
 *     U(j) = PRF(P, U(j-1))                j = 2..c
 *
 * Build flags:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#include "pbkdf2.h"
#include "hmac.h"   /* hmac_sha1(key,klen,msg,mlen,out[20]) */
#include "sha1.h"   /* sha1_ctx, sha1_init/update/final -- for two-part HMAC */

#define PBKDF2_HLEN 20   /* HMAC-SHA1 output length */
#define HMAC_BLOCK  64   /* SHA-1 HMAC block size   */

/* ------------------------------------------------------------------ *
 * Local helpers (no libc)
 * ------------------------------------------------------------------ */

static void pb_memset(void *dst, int val, unsigned long n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)val;
}

static void pb_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static int pb_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ------------------------------------------------------------------ *
 * HMAC-SHA1 over a two-part message: out = HMAC(key, p1 || p2)
 *
 * Used for U(1) = HMAC(P, S || INT32BE(i)) so the salt (p1, arbitrary
 * length) never has to be copied into a fixed-size buffer alongside the
 * 4-byte counter (p2). Streams both parts through the SHA-1 context,
 * mirroring the construction in hmac.c / hkdf.c.
 * ------------------------------------------------------------------ */
static void hmac_sha1_2(const unsigned char *key, unsigned long klen,
                        const unsigned char *p1,  unsigned long p1len,
                        const unsigned char *p2,  unsigned long p2len,
                        unsigned char out[20])
{
    unsigned char k[HMAC_BLOCK];
    unsigned char ipad[HMAC_BLOCK];
    unsigned char opad[HMAC_BLOCK];
    unsigned char inner[PBKDF2_HLEN];
    sha1_ctx c;
    int i;

    pb_memset(k, 0, HMAC_BLOCK);
    if (klen > HMAC_BLOCK) {
        sha1(key, klen, k);          /* k = H(key), rest stays zero */
    } else {
        for (i = 0; (unsigned long)i < klen; i++) k[i] = key[i];
    }

    for (i = 0; i < HMAC_BLOCK; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    /* inner = H(ipad || p1 || p2) */
    sha1_init(&c);
    sha1_update(&c, ipad, HMAC_BLOCK);
    if (p1len) sha1_update(&c, p1, p1len);
    if (p2len) sha1_update(&c, p2, p2len);
    sha1_final(&c, inner);

    /* out = H(opad || inner) */
    sha1_init(&c);
    sha1_update(&c, opad, HMAC_BLOCK);
    sha1_update(&c, inner, PBKDF2_HLEN);
    sha1_final(&c, out);

    pb_memset(k,     0, HMAC_BLOCK);
    pb_memset(ipad,  0, HMAC_BLOCK);
    pb_memset(opad,  0, HMAC_BLOCK);
    pb_memset(inner, 0, PBKDF2_HLEN);
}

/* ------------------------------------------------------------------ *
 * pbkdf2_hmac_sha1 -- RFC 2898 §5.2
 * ------------------------------------------------------------------ */
void pbkdf2_hmac_sha1(const uint8_t *pass, int passlen,
                      const uint8_t *salt, int saltlen,
                      uint32_t iters,
                      uint8_t *out, int dklen)
{
    unsigned char u[PBKDF2_HLEN];   /* current U(j)                  */
    unsigned char t[PBKDF2_HLEN];   /* running XOR accumulator T(i)  */
    unsigned char ctr[4];           /* INT32BE(block index)          */
    unsigned long plen, slen;
    uint32_t block;
    int written;
    int i, j;

    if (dklen <= 0) return;
    if (iters == 0) iters = 1;      /* defensive: a valid c is >= 1  */
    plen = (passlen > 0) ? (unsigned long)passlen : 0UL;
    slen = (saltlen  > 0) ? (unsigned long)saltlen : 0UL;

    written = 0;
    block   = 1;
    while (written < dklen) {
        /* INT32BE(block) -- 1-based block index, big-endian */
        ctr[0] = (unsigned char)((block >> 24) & 0xff);
        ctr[1] = (unsigned char)((block >> 16) & 0xff);
        ctr[2] = (unsigned char)((block >>  8) & 0xff);
        ctr[3] = (unsigned char)( block        & 0xff);

        /* U(1) = HMAC(P, S || INT32BE(block)) */
        hmac_sha1_2(pass, plen, salt, slen, ctr, 4, u);
        pb_memcpy(t, u, PBKDF2_HLEN);   /* T = U(1) */

        /* U(j) = HMAC(P, U(j-1));  T ^= U(j),  j = 2..c */
        for (j = 1; (uint32_t)j < iters; j++) {
            hmac_sha1(pass, plen, u, PBKDF2_HLEN, u);
            for (i = 0; i < PBKDF2_HLEN; i++)
                t[i] = (unsigned char)(t[i] ^ u[i]);
        }

        /* Copy min(hlen, remaining) bytes of T into the output */
        {
            int remaining = dklen - written;
            int chunk = (remaining < PBKDF2_HLEN) ? remaining : PBKDF2_HLEN;
            for (i = 0; i < chunk; i++)
                out[written + i] = t[i];
            written += chunk;
        }

        block++;
    }

    pb_memset(u,   0, PBKDF2_HLEN);
    pb_memset(t,   0, PBKDF2_HLEN);
    pb_memset(ctr, 0, sizeof(ctr));
}

/* ------------------------------------------------------------------ *
 * wpa_pmk -- IEEE 802.11-2020 J.4
 *   PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 256 bits)
 * ------------------------------------------------------------------ */
void wpa_pmk(const char *passphrase,
             const uint8_t *ssid, int ssid_len,
             uint8_t pmk[32])
{
    pbkdf2_hmac_sha1((const uint8_t *)passphrase, pb_strlen(passphrase),
                     ssid, ssid_len,
                     4096u, pmk, 32);
}

/* ------------------------------------------------------------------ *
 * Self-test
 * ------------------------------------------------------------------
 * KAT 1 -- RFC 6070:
 *   P = "password", S = "salt", c = 4096, dkLen = 20
 *   DK = 4b007901 b765489a bead49d9 26f721d0 65a429c1
 *
 * KAT 2 -- IEEE 802.11-2020 J.4.2 (test vector 1):
 *   passphrase = "password", SSID = "IEEE" (4 bytes), c = 4096, 32 bytes
 *   PMK = f42c6fc5 2df0ebef 9ebb4b90 b38a5f90
 *         2e83fe1b 135a70e2 3aed762e 9710a12e
 * ------------------------------------------------------------------ */

/* constant-time-ish fixed-length compare; returns 0 if equal, nonzero if not */
static int pb_diff(const unsigned char *a, const unsigned char *b, int n)
{
    unsigned char d = 0;
    int i;
    for (i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return (int)d;
}

int pbkdf2_selftest(void)
{
    /* ---- KAT 1: RFC 6070 (P="password", S="salt", c=4096, dkLen=20) ---- */
    {
        static const unsigned char pass[8] = { 'p','a','s','s','w','o','r','d' };
        static const unsigned char salt[4] = { 's','a','l','t' };
        static const unsigned char exp[20] = {
            0x4b,0x00,0x79,0x01,0xb7,0x65,0x48,0x9a,
            0xbe,0xad,0x49,0xd9,0x26,0xf7,0x21,0xd0,
            0x65,0xa4,0x29,0xc1
        };
        unsigned char dk[20];
        pb_memset(dk, 0, sizeof(dk));
        pbkdf2_hmac_sha1(pass, 8, salt, 4, 4096u, dk, 20);
        if (pb_diff(dk, exp, 20) != 0) return 1;
    }

    /* ---- KAT 2: IEEE 802.11-2020 J.4.2 WPA PMK ------------------------- *
     * passphrase = "password", SSID = "IEEE", c = 4096, 32 bytes          */
    {
        static const unsigned char ssid[4] = { 'I','E','E','E' };
        static const unsigned char exp[32] = {
            0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,
            0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
            0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,
            0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e
        };
        unsigned char pmk[32];
        pb_memset(pmk, 0, sizeof(pmk));
        wpa_pmk("password", ssid, 4, pmk);
        if (pb_diff(pmk, exp, 32) != 0) return 2;
    }

    return 0; /* both known-answer vectors matched */
}
