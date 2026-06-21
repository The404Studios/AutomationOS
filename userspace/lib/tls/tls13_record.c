/*
 * tls13_record.c -- TLS 1.3 record protection (RFC 8446 Section 5.2).
 * Freestanding. See tls13_record.h.
 */
#include "tls13_record.h"
#include "../crypto/aes.h"
#include "../crypto/chacha20poly1305.h"

#define REC_MAX 16640   /* 2^14 plaintext + overhead */

/* nonce = static_iv XOR seq, seq big-endian in the trailing 8 octets. */
static void rec_nonce(const unsigned char static_iv[12], unsigned long long seq,
                      unsigned char nonce[12])
{
    for (int i = 0; i < 12; i++) nonce[i] = static_iv[i];
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (unsigned char)(seq >> (8 * i));
}

static int aead_open(int aead_id, const unsigned char *key,
                     const unsigned char nonce[12],
                     const unsigned char *aad, unsigned long aadlen,
                     const unsigned char *ct, unsigned long ctlen,
                     const unsigned char tag[16], unsigned char *pt)
{
    if (aead_id == TLS13_AEAD_CHACHA20)
        return chacha20poly1305_decrypt(key, nonce, aad, aadlen, ct, ctlen, tag, pt);
    aes_ctx c;
    aes_set_encrypt_key(&c, key, aead_id == TLS13_AEAD_AES256_GCM ? 256 : 128);
    return aes_gcm_decrypt(&c, nonce, aad, aadlen, ct, ctlen, tag, pt);
}

static int aead_seal(int aead_id, const unsigned char *key,
                     const unsigned char nonce[12],
                     const unsigned char *aad, unsigned long aadlen,
                     const unsigned char *pt, unsigned long ptlen,
                     unsigned char *ct, unsigned char tag[16])
{
    if (aead_id == TLS13_AEAD_CHACHA20)
        return chacha20poly1305_encrypt(key, nonce, aad, aadlen, pt, ptlen, ct, tag);
    aes_ctx c;
    aes_set_encrypt_key(&c, key, aead_id == TLS13_AEAD_AES256_GCM ? 256 : 128);
    return aes_gcm_encrypt(&c, nonce, aad, aadlen, pt, ptlen, ct, tag);
}

int tls13_record_open(int aead_id, const unsigned char *key,
                      const unsigned char static_iv[12], unsigned long long seq,
                      const unsigned char *record, unsigned long record_len,
                      unsigned char *out, unsigned long out_cap,
                      unsigned long *out_len, unsigned char *inner_type)
{
    if (record_len < 5u + 16u + 1u) return -1;
    if (record[0] != 0x17) return -1;                 /* opaque_type app_data   */
    unsigned long body = ((unsigned long)record[3] << 8) | record[4];
    if (5u + body != record_len) return -1;
    if (body < 16u + 1u) return -1;
    unsigned long ctlen = body - 16u;                 /* inner pt incl. type    */
    if (ctlen > out_cap) return -1;
    const unsigned char *ct  = record + 5;
    const unsigned char *tag = record + 5 + ctlen;
    unsigned char nonce[12]; rec_nonce(static_iv, seq, nonce);
    unsigned char aad[5]; for (int i = 0; i < 5; i++) aad[i] = record[i];

    if (aead_open(aead_id, key, nonce, aad, 5, ct, ctlen, tag, out) != 0)
        return -2;                                    /* authentication failed  */

    /* Strip zero padding from the end; the last non-zero byte is the type. */
    long i = (long)ctlen - 1;
    while (i >= 0 && out[i] == 0x00) i--;
    if (i < 0) return -3;                             /* all padding: malformed */
    *inner_type = out[i];
    *out_len = (unsigned long)i;
    return 0;
}

int tls13_record_seal(int aead_id, const unsigned char *key,
                      const unsigned char static_iv[12], unsigned long long seq,
                      const unsigned char *content, unsigned long content_len,
                      unsigned char content_type,
                      unsigned char *out_record, unsigned long out_cap,
                      unsigned long *out_len)
{
    unsigned long inner = content_len + 1u;           /* content || type        */
    unsigned long body  = inner + 16u;                /* + AEAD tag             */
    unsigned long rec_len = 5u + body;
    if (rec_len > out_cap || rec_len > REC_MAX) return -1;

    out_record[0] = 0x17; out_record[1] = 0x03; out_record[2] = 0x03;
    out_record[3] = (unsigned char)(body >> 8);
    out_record[4] = (unsigned char)(body & 0xff);

    /* Assemble inner plaintext (content || type) in place, then encrypt in
     * place (GCM/ChaCha are stream ciphers -> ct may alias pt). */
    unsigned char *pt = out_record + 5;
    for (unsigned long k = 0; k < content_len; k++) pt[k] = content[k];
    pt[content_len] = content_type;

    unsigned char nonce[12]; rec_nonce(static_iv, seq, nonce);
    unsigned char aad[5]; for (int i = 0; i < 5; i++) aad[i] = out_record[i];

    if (aead_seal(aead_id, key, nonce, aad, 5, pt, inner, pt, out_record + 5 + inner) != 0)
        return -2;
    *out_len = rec_len;
    return 0;
}

/* Round-trip self-test across the three AEADs. */
int tls13_record_selftest(void)
{
    unsigned char key[32], iv[12], content[40], rec[256], out[256];
    unsigned long rlen, olen;
    unsigned char itype;
    for (int i = 0; i < 32; i++) key[i] = (unsigned char)(0x40 + i);
    for (int i = 0; i < 12; i++) iv[i] = (unsigned char)(0x10 + i);
    for (int i = 0; i < 40; i++) content[i] = (unsigned char)i;

    int aeads[3] = { TLS13_AEAD_AES128_GCM, TLS13_AEAD_AES256_GCM, TLS13_AEAD_CHACHA20 };
    for (int a = 0; a < 3; a++) {
        for (unsigned long long seq = 0; seq <= 1; seq++) {
            if (tls13_record_seal(aeads[a], key, iv, seq, content, 40, 0x16,
                                  rec, sizeof(rec), &rlen) != 0) return 1;
            if (tls13_record_open(aeads[a], key, iv, seq, rec, rlen,
                                  out, sizeof(out), &olen, &itype) != 0) return 2;
            if (olen != 40 || itype != 0x16) return 3;
            for (int k = 0; k < 40; k++) if (out[k] != content[k]) return 4;
            /* a wrong seq must fail authentication */
            if (tls13_record_open(aeads[a], key, iv, seq + 5, rec, rlen,
                                  out, sizeof(out), &olen, &itype) == 0) return 5;
        }
    }
    return 0;
}
