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

void _start(void) {
    int crypto = crypto_selftest();   /* SHA-256/1/MD5/HMAC/AES KATs */
    int sha512 = sha512_selftest();
    int rsa    = rsa_selftest();
    int x255   = x25519_selftest();
    int p256   = p256_selftest();
    int chacha = chacha20poly1305_selftest();
    int hkdf   = hkdf_selftest();
    int b64    = base64_selftest();
    int x509   = x509_selftest();
    int xverify= x509_verify_selftest();
    int tls    = tls_selftest();

    report("crypto KATs (sha256/sha1/md5/hmac/aes)", crypto);
    report("sha384/512", sha512);
    report("rsa", rsa);
    report("x25519", x255);
    report("p256 (ecdh/ecdsa)", p256);
    report("chacha20-poly1305", chacha);
    report("hkdf", hkdf);
    report("base64", b64);
    report("x509 parse", x509);
    report("x509 chain verify", xverify);
    report("tls PRF", tls);

    int all = crypto | sha512 | rsa | x255 | p256 | chacha | hkdf | b64 | x509 | xverify | tls;
    if (all == 0)
        print("CRYPTOTEST: PASS (full crypto/TLS KAT battery)\n");
    else
        print("CRYPTOTEST: FAIL\n");

    sc(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}
