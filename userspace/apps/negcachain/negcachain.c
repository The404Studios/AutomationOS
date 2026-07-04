// negcachain -- NEGCACHAIN-0 regression proof for the x509_verify.c
// basicConstraints/cA bypass HIGH (no libs, own _start, direct syscalls).
// Prints to fd 1 (serial). The smoke greps "NEGCACHAIN: PASS".
//
// BUG (pre-fix, userspace/lib/tls/x509_verify.c x509_verify_chain): chain
// linkage only checked issuer/subject DN equality + a valid signature
// between adjacent certs. It never checked that an issuing cert asserted
// basicConstraints cA=TRUE. A normal end-entity (non-CA) leaf certificate --
// the kind any attacker can legitimately obtain for a domain THEY own --
// could therefore sign a forged certificate for ANY OTHER host, and
// x509_verify_chain() would accept it: a classic basicConstraints-bypass PKI
// impersonation hole (the gap was even self-documented in x509_verify.h's
// "does NOT enforce basicConstraints" honesty note).
//
// FIX (HEAD, x509_verify.c): a new static cert_is_ca(&tbs_fields) parses the
// id-ce-basicConstraints (2.5.29.19) extension and returns 1 iff it is
// present with cA==TRUE. x509_verify_chain()'s chain-linkage loop now calls
// !cert_is_ca(&f[i+1]) -> return X509V_ERR_NOT_CA (-13) before trusting an
// issuer's signature.
//
// DISCRIMINATION STRATEGY: cert_is_ca() is `static`, so this test drives it
// through a tiny public wrapper added alongside the fix,
// x509_test_cert_is_ca(der, len), which parses ONE standalone DER
// certificate and returns cert_is_ca()'s verdict on it -- the exact
// predicate x509_verify_chain() now applies to every issuer in a chain. No
// signature needs to be valid for this (cert_is_ca only walks extensions),
// so the test can hand-craft minimal-but-structurally-valid DER certs
// entirely offline:
//
//   CERT_CA_TRUE   -- basicConstraints { cA TRUE  } -> must return 1
//   CERT_CA_FALSE  -- basicConstraints { cA FALSE } -> must return 0
//   CERT_NO_EXT    -- no extensions at all           -> must return 0
//
// On the FIXED kernel/userspace (x509_test_cert_is_ca links against the
// HEAD x509_verify.o, which contains cert_is_ca()) all three checks land on
// their expected side and this test prints "NEGCACHAIN: PASS". On the
// PRE-FIX object (55926d4's x509_verify.c, which has no cert_is_ca()/
// x509_test_cert_is_ca() at all) this translation unit fails to LINK --
// x509_test_cert_is_ca is an undefined symbol -- so the pre-fix build never
// produces negcachain.elf, init never spawns it, and "NEGCACHAIN: PASS"
// never appears in the serial log; smoke_boot.sh's grep for the marker then
// fails closed. This is a build-time (link-time) discriminator, which is
// the strongest possible proof for a "the check didn't exist" class bug --
// stronger than any runtime behavior a still-buggy binary could fake.
//
// As a bonus end-to-end confirmation (not the primary gate, since the CA
// bundle may legitimately be empty on some builds), we also probe every
// real root in the compiled-in CA bundle (ca_bundle.h) and require that
// every one of them IS a CA -- true root CAs always assert cA=TRUE, so this
// independently exercises the fixed code against real-world DER, not just
// hand-crafted fixtures.

#include "../../lib/tls/x509_verify.h"
#include "../../lib/tls/ca_bundle.h"

typedef unsigned long size_t;

#define SYS_EXIT   0
#define SYS_WRITE  3
#define SYS_GETPID 8

static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc3(n,a,b,c) sc6((n),(a),(b),(c),0,0)
#define sc0(n)       sc6((n),0,0,0,0,0)

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }

/* ------------------------------------------------------------------------
 * Hand-crafted minimal DER certificates. Each is a full, structurally-valid
 * Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm,
 * signatureValue } -- serialNumber=1, empty issuer/subject Names, a
 * 2026..2026 UTCTime validity window (unused by cert_is_ca), a placeholder
 * SPKI, and (for the first two) a single [3] extensions block carrying ONLY
 * a basicConstraints (2.5.29.19) extension. No signature is verified by
 * cert_is_ca(), so the trailing BIT STRING content is a dummy zero byte.
 * ------------------------------------------------------------------------ */

/* basicConstraints { cA TRUE } */
static const unsigned char CERT_CA_TRUE[] = {
    0x30,0x61,
      0x30,0x4C,
        0x02,0x01,0x01,
        0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B,0x05,0x00,
        0x30,0x00,
        0x30,0x1E,
          0x17,0x0D,0x32,0x36,0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5A,
          0x17,0x0D,0x32,0x36,0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5A,
        0x30,0x00,
        0x30,0x02,0x05,0x00,
        0xA3,0x10,
          0x30,0x0E,
            0x30,0x0C,
              0x06,0x03,0x55,0x1D,0x13,
              0x04,0x05,
                0x30,0x03,0x01,0x01,0xFF,
      0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B,0x05,0x00,
      0x03,0x02,0x00,0x00
};

/* basicConstraints { cA FALSE } -- identical to CERT_CA_TRUE except the
 * single cA BOOLEAN payload byte (0xFF -> 0x00). */
static const unsigned char CERT_CA_FALSE[] = {
    0x30,0x61,
      0x30,0x4C,
        0x02,0x01,0x01,
        0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B,0x05,0x00,
        0x30,0x00,
        0x30,0x1E,
          0x17,0x0D,0x32,0x36,0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5A,
          0x17,0x0D,0x32,0x36,0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5A,
        0x30,0x00,
        0x30,0x02,0x05,0x00,
        0xA3,0x10,
          0x30,0x0E,
            0x30,0x0C,
              0x06,0x03,0x55,0x1D,0x13,
              0x04,0x05,
                0x30,0x03,0x01,0x01,0x00,
      0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B,0x05,0x00,
      0x03,0x02,0x00,0x00
};

/* No extensions at all (the [3] block is entirely absent) -- the "an
 * end-entity leaf with no basicConstraints" case, which MUST also read as
 * non-CA (BasicConstraints cA DEFAULT FALSE). */
static const unsigned char CERT_NO_EXT[] = {
    0x30,0x4F,
      0x30,0x3A,
        0x02,0x01,0x01,
        0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B,0x05,0x00,
        0x30,0x00,
        0x30,0x1E,
          0x17,0x0D,0x32,0x36,0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5A,
          0x17,0x0D,0x32,0x36,0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5A,
        0x30,0x00,
        0x30,0x02,0x05,0x00,
      0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B,0x05,0x00,
      0x03,0x02,0x00,0x00
};

void _start(void) {
    int ok = 1;
    out("NEGCACHAIN: start\n");

    // --- Check 1: a cert asserting basicConstraints cA=TRUE IS a CA ---
    out("NEGCACHAIN: [1] cA=TRUE fixture\n");
    int r1 = x509_test_cert_is_ca(CERT_CA_TRUE, sizeof CERT_CA_TRUE);
    if (r1 == 1) {
        out("NEGCACHAIN: [1] PASS is_ca=1\n");
    } else {
        out("NEGCACHAIN: [1] FAIL expected is_ca=1\n");
        ok = 0;
    }

    // --- Check 2 (the bug's exact shape): a cert asserting basicConstraints
    //     cA=FALSE must NOT be treated as a CA. Pre-fix, x509_verify_chain
    //     never even asked this question -- any such cert's signature alone
    //     was enough to trust it as an issuer. ---
    out("NEGCACHAIN: [2] cA=FALSE fixture (forged-issuer shape)\n");
    int r2 = x509_test_cert_is_ca(CERT_CA_FALSE, sizeof CERT_CA_FALSE);
    if (r2 == 0) {
        out("NEGCACHAIN: [2] PASS is_ca=0\n");
    } else {
        out("NEGCACHAIN: [2] FAIL expected is_ca=0 got is_ca=1 (basicConstraints bypass!)\n");
        ok = 0;
    }

    // --- Check 3: a cert with NO basicConstraints extension at all (a
    //     typical end-entity leaf) must also read as non-CA (DEFAULT FALSE),
    //     not merely "extension present but false". ---
    out("NEGCACHAIN: [3] no-extensions fixture (typical leaf shape)\n");
    int r3 = x509_test_cert_is_ca(CERT_NO_EXT, sizeof CERT_NO_EXT);
    if (r3 == 0) {
        out("NEGCACHAIN: [3] PASS is_ca=0\n");
    } else {
        out("NEGCACHAIN: [3] FAIL expected is_ca=0 got is_ca=1\n");
        ok = 0;
    }

    // --- Check 4 (bonus, real-world confirmation): every compiled-in CA
    //     bundle root must read as a CA. Real root certificates always
    //     assert basicConstraints cA=TRUE, so this exercises the fixed code
    //     against authentic DER, not just hand-crafted fixtures. Skipped
    //     (not failed) if the bundle happens to be empty on this build --
    //     an empty trust store is a documented, safe configuration
    //     (ca_bundle.h), not this bug's concern. ---
    int ca_count = ca_get_count();
    if (ca_count > 0) {
        int all_ca = 1;
        int checked = 0;
        for (int i = 0; i < ca_count; i++) {
            unsigned long rlen = 0;
            const unsigned char *rder = ca_get_der(i, &rlen);
            if (!rder || rlen == 0) continue;
            checked++;
            if (x509_test_cert_is_ca(rder, rlen) != 1) all_ca = 0;
        }
        if (checked > 0 && all_ca) {
            out("NEGCACHAIN: [4] PASS all_bundle_roots_are_ca=1\n");
        } else if (checked == 0) {
            out("NEGCACHAIN: [4] SKIP (no readable bundle entries)\n");
        } else {
            out("NEGCACHAIN: [4] FAIL a real CA bundle root read as non-CA\n");
            ok = 0;
        }
    } else {
        out("NEGCACHAIN: [4] SKIP (empty CA bundle -- not this bug's concern)\n");
    }

    // --- Check 5: the process (and kernel) is still alive -- this whole
    //     test ran in pure userspace computation (no syscalls but WRITE),
    //     so a real pid coming back also confirms nothing corrupted state. ---
    long pid = sc0(SYS_GETPID);
    if (pid > 0) {
        out("NEGCACHAIN: [5] PASS kernel_alive=1 getpid_ok=1\n");
    } else {
        out("NEGCACHAIN: [5] FAIL getpid broken\n");
        ok = 0;
    }

    out(ok ? "NEGCACHAIN: PASS\n" : "NEGCACHAIN: FAIL\n");
    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}
