/*
 * tls13_certverify.c -- TLS 1.3 CertificateVerify verification (RFC 8446 4.4.3).
 * Freestanding. See tls13_certverify.h.
 */
#include "tls13_certverify.h"
#include "../crypto/rsa_pss.h"
#include "../crypto/p256.h"
#include "../crypto/p384.h"
#include "../crypto/sha256.h"
#include "../crypto/sha512.h"   /* sha384 */

static const char CTX_SERVER[] = "TLS 1.3, server CertificateVerify";
static const char CTX_CLIENT[] = "TLS 1.3, client CertificateVerify";

unsigned long tls13_certverify_content(int is_server,
                                       const unsigned char *transcript_hash,
                                       unsigned long thlen, unsigned char *out)
{
    const char *ctx = is_server ? CTX_SERVER : CTX_CLIENT;
    unsigned long n = 0;
    for (int i = 0; i < 64; i++) out[n++] = 0x20;          /* 64 spaces      */
    for (const char *p = ctx; *p; p++) out[n++] = (unsigned char)*p; /* context */
    out[n++] = 0x00;                                       /* separator      */
    for (unsigned long i = 0; i < thlen; i++) out[n++] = transcript_hash[i];
    return n;
}

int tls13_certverify_rsapss(unsigned short sigalg, int is_server,
                            const unsigned char *transcript_hash, unsigned long thlen,
                            const unsigned char *sig, unsigned long sig_len,
                            const rsa_pubkey *pk)
{
    unsigned char content[64 + 34 + 1 + 64];
    unsigned long clen = tls13_certverify_content(is_server, transcript_hash, thlen, content);
    unsigned char mh[64];
    int alg;
    if (sigalg == TLS13_SIG_RSA_PSS_SHA256)      { sha256(content, clen, mh); alg = RSA_PSS_SHA256; }
    else if (sigalg == TLS13_SIG_RSA_PSS_SHA384) { sha384(content, clen, mh); alg = RSA_PSS_SHA384; }
    else return -1;
    unsigned long hlen = (alg == RSA_PSS_SHA384) ? 48u : 32u;
    return rsa_pss_verify(pk, sig, sig_len, mh, hlen, alg) == 0 ? 0 : -2;
}

/* Decode a DER ECDSA-Sig-Value SEQUENCE { INTEGER r, INTEGER s } into fixed
 * big-endian r and s of clen bytes each. Returns 0 on success. */
static int der_sig_to_rs(const unsigned char *sig, unsigned long sig_len,
                         unsigned char *r, unsigned char *s, unsigned long clen)
{
    unsigned long p = 0;
    if (sig_len < 8 || sig[p++] != 0x30) return -1;
    unsigned long seqlen = sig[p++];
    if (seqlen & 0x80) return -1;                 /* only short form expected */
    if (p + seqlen > sig_len) return -1;
    for (int comp = 0; comp < 2; comp++) {
        if (p >= sig_len || sig[p++] != 0x02) return -1;   /* INTEGER */
        unsigned long ilen = sig[p++];
        if (ilen & 0x80) return -1;
        if (p + ilen > sig_len || ilen == 0) return -1;
        const unsigned char *iv = sig + p; p += ilen;
        while (ilen > 1 && iv[0] == 0x00) { iv++; ilen--; }  /* strip sign byte */
        if (ilen > clen) return -1;
        unsigned char *dst = comp ? s : r;
        for (unsigned long i = 0; i < clen; i++) dst[i] = 0;
        for (unsigned long i = 0; i < ilen; i++) dst[clen - ilen + i] = iv[i];
    }
    return 0;
}

int tls13_certverify_ecdsa(unsigned short sigalg, int is_server,
                           const unsigned char *transcript_hash, unsigned long thlen,
                           const unsigned char *sig, unsigned long sig_len,
                           const unsigned char *ec_point, unsigned long ec_point_len)
{
    unsigned char content[64 + 34 + 1 + 64];
    unsigned long clen = tls13_certverify_content(is_server, transcript_hash, thlen, content);
    unsigned char mh[64];
    if (sigalg == TLS13_SIG_ECDSA_P256_SHA256) {
        if (ec_point_len != 65) return -1;
        unsigned char r[32], s[32];
        if (der_sig_to_rs(sig, sig_len, r, s, 32) != 0) return -1;
        sha256(content, clen, mh);
        return p256_ecdsa_verify(ec_point, mh, 32, r, s) == 0 ? 0 : -2;
    } else if (sigalg == TLS13_SIG_ECDSA_P384_SHA384) {
        if (ec_point_len != 97) return -1;
        unsigned char r[48], s[48];
        if (der_sig_to_rs(sig, sig_len, r, s, 48) != 0) return -1;
        sha384(content, clen, mh);
        return p384_ecdsa_verify(ec_point, mh, 48, r, s) == 0 ? 0 : -2;
    }
    return -1;
}

/* ===================================================================== *
 *  RFC 8448 Section 3 KAT: server CertificateVerify (rsa_pss_rsae_sha256).
 * ===================================================================== */
static int chx(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; }
static void uh(const char*h, unsigned char*o, int n){ for(int i=0;i<n;i++) o[i]=(unsigned char)((chx(h[2*i])<<4)|chx(h[2*i+1])); }

int tls13_certverify_selftest(void)
{
    /* RFC 8448 Section 3 server CertificateVerify over Transcript-Hash(CH..Cert). */
    static const char TH_H[]  = "764d6632b3c35c3f3205e3499ac3edbaabb88295fba751461d3678e2e5ea0687";
    static const char SIG_H[] = "5a747c5d88fa9bd2e55ab085a61015b7211f824cd484145ab3ff52f1fda8477b"
                                "0b7abc90db78e2d33a5c141a078653fa6bef780c5ea248eeaaa785c4f394cab6"
                                "d30bbe8d4859ee511f602957b15411ac027671459e46445c9ea58c181e818e95"
                                "b8c3fb0bf3278409d3be152a3da5043e063dda65cdf5aea20d53dfacd42f74f3";
    static const char MOD_H[] = "b4bb498f8279303d980836399b36c6988c0c68de55e1bdb826d3901a2461eafd"
                                "2de49a91d015abbc9a95137ace6c1af19eaa6af98c7ced43120998e187a80ee0"
                                "ccb0524b1b018c3e0b63264d449a6d38e22a5fda430846748030530ef0461c8c"
                                "a9d9efbfae8ea6d1d03e2bd193eff0ab9a8002c47428a6d35a8d88d79f7f1e3f";
    unsigned char th[32], sig[128], mod[128];
    unsigned char exp[3] = { 0x01, 0x00, 0x01 };
    uh(TH_H, th, 32);
    uh(SIG_H, sig, 128);
    uh(MOD_H, mod, 128);

    rsa_pubkey pk;
    rsa_pubkey_from_bytes(&pk, mod, 128, exp, 3);

    /* Valid server CertificateVerify must verify. */
    if (tls13_certverify_rsapss(TLS13_SIG_RSA_PSS_SHA256, 1, th, 32, sig, 128, &pk) != 0)
        return 1;
    /* A tampered signature must be rejected. */
    sig[64] ^= 0x01;
    if (tls13_certverify_rsapss(TLS13_SIG_RSA_PSS_SHA256, 1, th, 32, sig, 128, &pk) == 0)
        return 2;
    return 0;
}
