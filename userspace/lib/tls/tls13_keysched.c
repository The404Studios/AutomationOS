/*
 * tls13_keysched.c -- TLS 1.3 key schedule (RFC 8446 Section 7.1).
 * ================================================================
 * Freestanding. See tls13_keysched.h. KAT'd against RFC 8448 Section 3.
 */
#include "tls13_keysched.h"
#include "../crypto/hkdf.h"
#include "../crypto/hmac.h"
#include "../crypto/sha256.h"
#include "../crypto/sha512.h"   /* sha384 */

unsigned long tls13_hashlen(int hash_id) { return hash_id ? 48u : 32u; }

int tls13_empty_hash(int hash_id, unsigned char *out)
{
    if (hash_id) sha384((const unsigned char *)"", 0, out);
    else         sha256((const unsigned char *)"", 0, out);
    return 0;
}

int tls13_extract(int hash_id,
                  const unsigned char *salt, unsigned long saltlen,
                  const unsigned char *ikm,  unsigned long ikmlen,
                  unsigned char *prk)
{
    if (hash_id) hkdf_extract_sha384(salt, saltlen, ikm, ikmlen, prk);
    else         hkdf_extract_sha256(salt, saltlen, ikm, ikmlen, prk);
    return 0;
}

int tls13_derive_secret(int hash_id,
                        const unsigned char *secret,
                        const char *label,
                        const unsigned char *thash, unsigned long thlen,
                        unsigned char *out)
{
    unsigned long hlen = tls13_hashlen(hash_id);
    return tls13_hkdf_expand_label(hash_id, secret, hlen, label,
                                   thash, thlen, out, hlen);
}

int tls13_traffic_keyiv(int hash_id, const unsigned char *secret,
                        unsigned char *key, unsigned long keylen,
                        unsigned char *iv,  unsigned long ivlen)
{
    unsigned long hlen = tls13_hashlen(hash_id);
    if (tls13_hkdf_expand_label(hash_id, secret, hlen, "key", (const unsigned char *)"", 0, key, keylen) != 0) return -1;
    if (tls13_hkdf_expand_label(hash_id, secret, hlen, "iv",  (const unsigned char *)"", 0, iv,  ivlen)  != 0) return -1;
    return 0;
}

int tls13_finished_key(int hash_id, const unsigned char *secret,
                       unsigned char *out)
{
    unsigned long hlen = tls13_hashlen(hash_id);
    return tls13_hkdf_expand_label(hash_id, secret, hlen, "finished",
                                   (const unsigned char *)"", 0, out, hlen);
}

int tls13_finished_verify(int hash_id, const unsigned char *traffic_secret,
                          const unsigned char *transcript_hash,
                          unsigned long thlen, unsigned char *out)
{
    unsigned char fk[48];
    unsigned long hlen = tls13_hashlen(hash_id);
    if (tls13_finished_key(hash_id, traffic_secret, fk) != 0) return -1;
    if (hash_id) hmac_sha384(fk, hlen, transcript_hash, thlen, out);
    else         hmac_sha256(fk, hlen, transcript_hash, thlen, out);
    return 0;
}

/* ===================================================================== *
 *  KAT: RFC 8448 Section 3 (Simple 1-RTT Handshake), SHA-256.            *
 * ===================================================================== */
static int hx(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static void unhex(const char *h, unsigned char *o, int olen)
{
    for (int i = 0; i < olen; i++)
        o[i] = (unsigned char)((hx(h[2 * i]) << 4) | hx(h[2 * i + 1]));
}
static int eq(const unsigned char *a, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int tls13_keysched_selftest(void)
{
    /* RFC 8448 Section 3 published values. */
    const char *IKM_H   = "8bd4054fb55b9d63fdfbacf9f04b9f0d35e6d63f537563efd46272900f89492d";
    const char *EARLY_H = "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a";
    const char *DERIV_H = "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba";
    const char *HS_H    = "1dc826e93606aa6fdc0aadc12f741b01046aa6b99f691ed221a9f0ca043fbeac";
    const char *CHS_H   = "b3eddb126e067f35a780b3abf45e2d8f3b1a950738f52e9600746a0e27a55a21";
    const char *SHS_H   = "b67b7d690cc16c4e75e54213cb2d37b4e9c912bcded9105d42befd59d391ad38";
    const char *THSH_H  = "860c06edc07858ee8e78f0e7428c58edd6b43f2ca3e6e95f02ed063cf0e1cad8";
    const char *SKEY_H  = "3fce516009c21727d0f2e4e86ee403bc";
    const char *SIV_H   = "5d313eb2671276ee13000b30";
    const char *MS_H    = "18df06843d13a08bf2a449844c5f8a478001bc4d4c627984d5a41da8d0402919";
    const char *CAP_H   = "9e40646ce79a7f9dc05af8889bce6552875afa0b06df0087f792ebb7c17504a5";
    const char *SAP_H   = "a11af9f05531f856ad47116b45a950328204b4f44bfb6b3a4b4f1f3fcb631643";
    const char *THAP_H  = "9608102a0f1ccc6db6250b7b7e417b1a000eaada3daae4777a7686c9ff83df13";

    unsigned char ikm[32], th_chsh[32], th_chsf[32];
    unsigned char want[48], got[48];
    unsigned char early[32], derived[32], hs[32], c_hs[32], s_hs[32];
    unsigned char derived2[32], master[32], c_ap[32], s_ap[32];
    unsigned char zero[32]; for (int i = 0; i < 32; i++) zero[i] = 0;

    unhex(IKM_H, ikm, 32);
    unhex(THSH_H, th_chsh, 32);
    unhex(THAP_H, th_chsf, 32);

    /* 1: Early Secret = Extract(0, PSK=0) */
    tls13_extract(0, 0, 0, zero, 32, early);
    unhex(EARLY_H, want, 32); if (!eq(early, want, 32)) return 1;

    /* 2: Derived = Derive-Secret(Early, "derived", "") */
    { unsigned char eh[32]; tls13_empty_hash(0, eh);
      tls13_derive_secret(0, early, "derived", eh, 32, derived); }
    unhex(DERIV_H, want, 32); if (!eq(derived, want, 32)) return 2;

    /* 3: Handshake Secret = Extract(Derived, ECDHE) */
    tls13_extract(0, derived, 32, ikm, 32, hs);
    unhex(HS_H, want, 32); if (!eq(hs, want, 32)) return 3;

    /* 4: client handshake traffic secret */
    tls13_derive_secret(0, hs, "c hs traffic", th_chsh, 32, c_hs);
    unhex(CHS_H, want, 32); if (!eq(c_hs, want, 32)) return 4;

    /* 5: server handshake traffic secret */
    tls13_derive_secret(0, hs, "s hs traffic", th_chsh, 32, s_hs);
    unhex(SHS_H, want, 32); if (!eq(s_hs, want, 32)) return 5;

    /* 6: server handshake key + iv */
    { unsigned char key[16], iv[12];
      tls13_traffic_keyiv(0, s_hs, key, 16, iv, 12);
      unhex(SKEY_H, want, 16); if (!eq(key, want, 16)) return 6;
      unhex(SIV_H, want, 12);  if (!eq(iv, want, 12))  return 7; }

    /* 8: Derived2 + Master Secret = Extract(Derived2, 0) */
    { unsigned char eh[32]; tls13_empty_hash(0, eh);
      tls13_derive_secret(0, hs, "derived", eh, 32, derived2); }
    tls13_extract(0, derived2, 32, zero, 32, master);
    unhex(MS_H, want, 32); if (!eq(master, want, 32)) return 8;

    /* 9: client application traffic secret */
    tls13_derive_secret(0, master, "c ap traffic", th_chsf, 32, c_ap);
    unhex(CAP_H, want, 32); if (!eq(c_ap, want, 32)) return 9;

    /* 10: server application traffic secret */
    tls13_derive_secret(0, master, "s ap traffic", th_chsf, 32, s_ap);
    unhex(SAP_H, want, 32); if (!eq(s_ap, want, 32)) return 10;

    /* 11: server Finished verify_data = HMAC(finished_key(s_hs), H(CH..CertVerify)) */
    {
        const char *THSF_H = "edb7725fa7a3473b031ec8ef65a2485493900138a2b91291407d7951a06110ed";
        const char *SFIN_H = "9b9b141d906337fbd2cbdce71df4deda4ab42c309572cb7fffee5454b78f0718";
        unsigned char thsf[32], vd[32];
        unhex(THSF_H, thsf, 32);
        tls13_finished_verify(0, s_hs, thsf, 32, vd);
        unhex(SFIN_H, want, 32); if (!eq(vd, want, 32)) return 11;
    }

    /* 12: client Finished verify_data = HMAC(finished_key(c_hs), H(CH..serverFin)),
     *     where that transcript hash is the same CH..serverFinished hash (th_chsf). */
    {
        const char *CFIN_H = "a8ec436d677634ae525ac1fcebe11a039ec17694fac6e98527b642f2edd5ce61";
        unsigned char vd[32];
        tls13_finished_verify(0, c_hs, th_chsf, 32, vd);
        unhex(CFIN_H, want, 32); if (!eq(vd, want, 32)) return 12;
    }

    (void)got;
    return 0;
}
