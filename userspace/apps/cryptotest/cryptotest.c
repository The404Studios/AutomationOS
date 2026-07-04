/*
 * cryptotest.c -- boot-time known-answer-test harness for the crypto/TLS stack.
 * ============================================================================
 * Runs the deterministic, network-free self-tests of every crypto building
 * block behind HTTPS: SHA-256/1/MD5/HMAC/AES KATs, SHA-384/512, RSA (PKCS#1),
 * X25519, P-256 (ECDH+ECDSA), ChaCha20-Poly1305, HKDF, base64, X.509 pubkey
 * extraction, X.509 chain-validation logic, and the TLS 1.2 PRF. Prints per-
 * primitive lines + one summary line the smoke gates on. Freestanding, own
 * _start, no libc.
 */
#include "../../lib/crypto/cryptotest.h"        /* crypto_selftest()  */
#include "../../lib/crypto/rsa.h"               /* rsa_selftest()     */
#include "../../lib/crypto/sha512.h"            /* sha512_selftest()  */
#include "../../lib/crypto/x25519.h"            /* x25519_selftest()  */
#include "../../lib/crypto/p256.h"              /* p256_selftest()    */
#include "../../lib/crypto/chacha20poly1305.h"  /* chacha20poly1305_selftest() */
#include "../../lib/crypto/hkdf.h"              /* hkdf_selftest()    */
#include "../../lib/crypto/base64.h"            /* base64_selftest()  */
#include "../../lib/crypto/pbkdf2.h"            /* pbkdf2_selftest() -- WPA2 PMK */
#include "../../lib/crypto/keywrap.h"           /* keywrap_selftest() -- RFC 3394 GTK */
#include "../../lib/crypto/ccm.h"               /* ccm_selftest() -- NIST 800-38C */
#include "../../lib/crypto/ccmp.h"              /* ccmp_selftest() -- 802.11 CCMP (WPA2) */
#include "../../lib/crypto/gcmp.h"              /* gcmp_selftest() -- 802.11 GCMP (WPA3) */
#include "../../lib/crypto/sae.h"               /* sae_selftest() -- WPA3 SAE dragonfly  */
#include "../../lib/tls/x509.h"                 /* x509_selftest()    */
#include "../../lib/tls/x509_verify.h"          /* x509_verify_selftest() */
#include "../../lib/tls/tls.h"                  /* tls_selftest()     */

#define SYS_EXIT  0
#define SYS_WRITE 3

static long sc(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}
static unsigned long slen(const char* s){unsigned long n=0;while(s[n])n++;return n;}
static void print(const char* m){ sc(SYS_WRITE, 1, (long)m, (long)slen(m)); }
static void report(const char* name, int rc){
    print("[CRYPTOTEST] "); print(name); print(rc == 0 ? ": PASS\n" : ": FAIL\n");
}

/* ============================================================================
 * NEGRSAEXP -- negative regression for the oversized-RSA-publicExponent HIGH.
 * ============================================================================
 * KERNEL-ROBUST-0 fixed x509_spki_extract_rsa() (userspace/lib/tls/x509.c):
 * the publicExponent copy used to reuse the modulus's 512-byte cap even though
 * every real call site hands it a 16-byte exp[16] buffer (see x509.h). A
 * certificate whose publicExponent INTEGER was >16 bytes sailed through
 * copy_integer_be()'s `vlen > cap` gate (e.g. 32 <= 512) and memcpy'd past the
 * end of the caller's 16-byte buffer. The fix dropped the cap to 16, so
 * copy_integer_be() now rejects (returns -1, propagated as -12) before ever
 * touching the buffer.
 *
 * NOTE: x509_selftest() in x509.c does NOT catch this -- its own harness uses
 * exp[512] scratch buffers, not the real 16-byte contract size, so an
 * oversized-exponent overflow is invisible to it. This test uses the REAL
 * 16-byte buffer every live call site uses.
 *
 * negrsaexp_spki is a hand-built, bare SubjectPublicKeyInfo (the DER shape
 * x509_spki_extract_rsa() parses directly):
 *   SEQUENCE {                                             -- SPKI
 *     SEQUENCE { OID rsaEncryption, NULL }                 -- AlgorithmIdentifier
 *     BIT STRING {                                         -- subjectPublicKey
 *       SEQUENCE {                                         -- RSAPublicKey
 *         INTEGER modulus         (3 bytes, arbitrary)
 *         INTEGER publicExponent  (32 nonzero bytes 0x01..0x20 -- NO leading
 *                                   0x00, so copy_integer_be() sees the full
 *                                   32-byte vlen unchanged)
 *       } } }
 * Bytes verified self-consistent (every DER length octet matches its span).
 */
static const unsigned char negrsaexp_spki[61] = {
    0x30, 0x3B,                                                  /* SPKI SEQUENCE, len 59 */
      0x30, 0x0D,                                                /*  AlgorithmIdentifier SEQUENCE, len 13 */
        0x06, 0x09, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01, /*   OID rsaEncryption */
        0x05, 0x00,                                               /*   NULL */
      0x03, 0x2A,                                                /*  BIT STRING, len 42 */
        0x00,                                                     /*   unused-bits octet = 0 */
        0x30, 0x27,                                               /*   RSAPublicKey SEQUENCE, len 39 */
          0x02, 0x03, 0x01, 0x02, 0x03,                            /*     modulus INTEGER, len 3 */
          0x02, 0x20,                                              /*     publicExponent INTEGER, len 32 */
            0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
            0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,
            0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
            0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20
};

/*
 * exp[16] is the REAL contract-sized buffer (matches every live call site).
 * canary[16] sits immediately after it in the same struct -- char[] members
 * have no inter-member padding and are laid out in declaration order (C
 * guarantees increasing addresses for struct members), so this is exactly the
 * 16 bytes a 32-into-16 overflow would spill into: enough to prove the bug
 * deterministically without risking a stack smash past the struct (the
 * overflow, if it happens, is fully absorbed by canary[] and cannot reach a
 * return address or other locals).
 */
struct negrsaexp_frame {
    unsigned char exp[16];
    unsigned char canary[16];
};

static int x509_negrsaexp_check(void) {
    unsigned char mod[512];
    unsigned long mod_len = 0, exp_len = 0xdead;
    struct negrsaexp_frame f;
    int i, rc, canary_ok;

    for (i = 0; i < 512; i++) mod[i] = 0;
    for (i = 0; i < 16; i++)  f.exp[i] = 0;
    for (i = 0; i < 16; i++)  f.canary[i] = 0xA5;

    rc = x509_spki_extract_rsa(negrsaexp_spki, sizeof negrsaexp_spki,
                               mod, &mod_len, f.exp, &exp_len);

    canary_ok = 1;
    for (i = 0; i < 16; i++)
        if (f.canary[i] != 0xA5) canary_ok = 0;

    /* Fixed kernel: parse must fail closed (rc != 0, expect -12) AND the
     * canary must be untouched. Regressed kernel: rc == 0 (the malformed
     * 32-byte exponent "succeeded") and the canary is stomped with the
     * exponent's tail bytes 0x11..0x20. Either symptom alone is a FAIL. */
    if (rc != 0 && canary_ok) return 0;
    return -1;
}

void _start(void) {
    int crypto = crypto_selftest();   /* SHA-256/1/MD5/HMAC/AES KATs */
    int sha512 = sha512_selftest();
    int rsa    = rsa_selftest();
    int x255   = x25519_selftest();
    int p256   = p256_selftest();
    int chacha = chacha20poly1305_selftest();
    int hkdf   = hkdf_selftest();
    int b64    = base64_selftest();
    int pbkdf2 = pbkdf2_selftest();    /* WPA-PROOF: PBKDF2-HMAC-SHA1 (PMK) */
    int keywrap= keywrap_selftest();   /* WPA-PROOF: AES key-wrap RFC 3394  */
    int ccm    = ccm_selftest();       /* WPA-PROOF: AES-CCM (NIST 800-38C) */
    int ccmp   = ccmp_selftest();      /* WPA-PROOF: 802.11 CCMP (WPA2)     */
    int gcmp   = gcmp_selftest();      /* WPA-PROOF: 802.11 GCMP (WPA3)     */
    int sae    = sae_selftest();       /* WPA-PROOF: WPA3 SAE dragonfly     */
    int x509   = x509_selftest();
    int xverify= x509_verify_selftest();
    int tls    = tls_selftest();
    int negrsaexp = x509_negrsaexp_check();  /* NEGRSAEXP regression, see above */

    report("crypto KATs (sha256/sha1/md5/hmac/aes)", crypto);
    report("sha384/512", sha512);
    report("rsa", rsa);
    report("x25519", x255);
    report("p256 (ecdh/ecdsa)", p256);
    report("chacha20-poly1305", chacha);
    report("hkdf", hkdf);
    report("base64", b64);
    report("pbkdf2-hmac-sha1 (wpa pmk)", pbkdf2);
    report("aes key-wrap (rfc 3394)", keywrap);
    report("aes-ccm (nist 800-38c)", ccm);
    report("ccmp (802.11 wpa2)", ccmp);
    report("gcmp (802.11 wpa3)", gcmp);
    report("sae (wpa3 dragonfly)", sae);
    report("x509 parse", x509);
    report("x509 chain verify", xverify);
    report("tls PRF", tls);
    report("x509 oversized RSA publicExponent (NEGRSAEXP)", negrsaexp);

    int all = crypto | sha512 | rsa | x255 | p256 | chacha | hkdf | b64 | pbkdf2 | keywrap
            | ccm | ccmp | gcmp | sae | x509 | xverify | tls | negrsaexp;
    if (all == 0)
        print("CRYPTOTEST: PASS (full crypto/TLS KAT battery)\n");
    else
        print("CRYPTOTEST: FAIL\n");
    if (negrsaexp == 0)
        print("NEGRSAEXP: PASS\n");
    else
        print("NEGRSAEXP: FAIL oversized RSA publicExponent not rejected or canary smashed\n");

    sc(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}
