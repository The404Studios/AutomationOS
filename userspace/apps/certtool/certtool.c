/*
 * certtool.c -- freestanding X.509 certificate inspector for AutomationOS.
 * =========================================================================
 *
 * Usage:  certtool FILE
 *         certtool          (no args: self-test)
 *
 * Reads FILE into a static buffer. If the content starts with "-----BEGIN"
 * it is treated as PEM and decoded to DER via pem_extract_der(). Otherwise
 * DER is assumed. Then prints:
 *
 *   Subject CN
 *   Subject DN (raw DER length, hex preview)
 *   Issuer DN  (raw DER length, hex preview)
 *   Validity notBefore / notAfter
 *   Public-key algorithm (RSA / EC) + RSA modulus bit-length
 *   SAN dNSName entries (if any)
 *
 * Self-test (argc <= 1): calls x509_selftest() and parses an embedded
 * minimal DER SubjectPublicKeyInfo to verify x509_extract_pubkey().
 * Prints "CERTTOOL SELFTEST: PASS" or "CERTTOOL SELFTEST: FAIL".
 * Returns 0 on pass, 1 on fail.
 *
 * Build flags (no fs:0x28):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Linked with crt0.o (calls main(argc, argv), then SYS_EXIT).
 * No libc / no malloc / no standard headers.
 */

#include "../../lib/tls/x509.h"
#include "../../lib/crypto/base64.h"

/* -------------------------------------------------------------------------
 * Syscall numbers (kernel/include/syscall.h)
 * ---------------------------------------------------------------------- */
#define SYS_EXIT  0
#define SYS_READ  2
#define SYS_WRITE 3
#define SYS_OPEN  4
#define SYS_CLOSE 5

#define O_RDONLY  0

/* -------------------------------------------------------------------------
 * 6-argument syscall wrapper
 * ---------------------------------------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* -------------------------------------------------------------------------
 * Minimal I/O helpers
 * ---------------------------------------------------------------------- */
static unsigned long ct_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void ct_write(const char *s)
{
    unsigned long len = ct_strlen(s);
    sc(SYS_WRITE, 1, (long)s, (long)len, 0, 0, 0);
}

/* Write a single character */
static void ct_putc(char c)
{
    sc(SYS_WRITE, 1, (long)&c, 1, 0, 0, 0);
}

/* -------------------------------------------------------------------------
 * Number formatters (decimal and hex) -- no libc sprintf/itoa
 * ---------------------------------------------------------------------- */

/* Write unsigned long as decimal string */
static void ct_put_ulong(unsigned long v)
{
    char buf[21]; /* max 20 digits for 64-bit + NUL */
    int i = 20;
    buf[i] = '\0';
    if (v == 0) {
        ct_putc('0');
        return;
    }
    while (v && i > 0) {
        buf[--i] = '0' + (char)(v % 10);
        v /= 10;
    }
    ct_write(buf + i);
}

/* Write byte as 2-digit lowercase hex */
static void ct_put_hex8(unsigned char b)
{
    static const char hx[] = "0123456789abcdef";
    ct_putc(hx[(b >> 4) & 0xf]);
    ct_putc(hx[b & 0xf]);
}

/* Write first `n` bytes of buf as hex, separated by spaces */
static void ct_put_hex_preview(const unsigned char *buf, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) {
        if (i) ct_putc(' ');
        ct_put_hex8(buf[i]);
    }
}

/* -------------------------------------------------------------------------
 * Static buffers
 * ---------------------------------------------------------------------- */
#define FILE_BUF_SIZE  (64 * 1024)  /* 64 KiB -- enough for most certs      */
#define DER_BUF_SIZE   (64 * 1024)
#define MOD_BUF_SIZE   512          /* up to 4096-bit modulus = 512 bytes    */
#define EXP_BUF_SIZE   16
#define CN_BUF_SIZE    256
#define TIME_BUF_SIZE  15           /* 14 chars + NUL                        */
#define SAN_MAX        16           /* max SAN DNS entries to print          */

static unsigned char g_filebuf[FILE_BUF_SIZE];
static unsigned char g_derbuf[DER_BUF_SIZE];
static unsigned char g_mod[MOD_BUF_SIZE];
static unsigned char g_exp[EXP_BUF_SIZE];
static char          g_cn[CN_BUF_SIZE];
static char          g_not_before[TIME_BUF_SIZE];
static char          g_not_after[TIME_BUF_SIZE];
static char          g_san[SAN_MAX][256];

/* -------------------------------------------------------------------------
 * Utility: compare first n chars of two strings
 * ---------------------------------------------------------------------- */
static int ct_strncmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) {
        if ((unsigned char)a[i] != (unsigned char)b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Self-test embedded SPKI (RSA-512 for minimal size; real usage targets 2048+)
 *
 * This is a valid DER SubjectPublicKeyInfo carrying a 512-bit RSA key
 * (generated offline, deterministic). x509_extract_pubkey() accepts a bare
 * SPKI as documented in x509.h.
 *
 * SEQUENCE {
 *   SEQUENCE { OID rsaEncryption, NULL }
 *   BIT STRING {
 *     SEQUENCE {
 *       INTEGER (modulus, 64 bytes + sign byte)
 *       INTEGER (exponent = 65537)
 *     }
 *   }
 * }
 * ---------------------------------------------------------------------- */
static const unsigned char k_test_spki[] = {
    /* SEQUENCE (outer SPKI wrapper) */
    0x30, 0x5c,
      /* SEQUENCE (AlgorithmIdentifier) */
      0x30, 0x0d,
        /* OID rsaEncryption 1.2.840.113549.1.1.1 */
        0x06, 0x09,
          0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01,
        /* NULL */
        0x05, 0x00,
      /* BIT STRING (0 unused bits) */
      0x03, 0x4b, 0x00,
        /* SEQUENCE (RSAPublicKey) */
        0x30, 0x48,
          /* INTEGER modulus (64 bytes, no sign byte needed -- MSB=0xb3 < 0x80,
             so no 0x00 prefix).
             Value is arbitrary non-zero 512-bit number used purely for
             self-test; not a real key. */
          0x02, 0x41,
            0x00,  /* leading 0x00 sign byte since 0xb3 >= 0x80 */
            0xb3, 0x51, 0x0a, 0x7d, 0x2c, 0x9e, 0xf4, 0x6a,
            0xa1, 0x33, 0xdc, 0x7b, 0x55, 0x8f, 0x0c, 0xee,
            0xd9, 0x42, 0x17, 0x83, 0xfe, 0x0b, 0x6c, 0x30,
            0x9a, 0x47, 0x6e, 0x22, 0xca, 0xf1, 0x88, 0xbd,
            0x11, 0x3d, 0x5a, 0x92, 0xc6, 0x04, 0xe8, 0x71,
            0xfa, 0xb7, 0x29, 0xd5, 0x0c, 0x3b, 0x7f, 0x44,
            0x87, 0x65, 0xdc, 0x21, 0x9e, 0x33, 0xc1, 0x07,
            0x5d, 0x92, 0x3a, 0x10, 0x4f, 0x6e, 0xd7, 0x23,
          /* INTEGER exponent = 65537 = 0x010001 */
          0x02, 0x03,
            0x01, 0x00, 0x01,
};

/* -------------------------------------------------------------------------
 * run_selftest(): calls x509_selftest() + parses embedded SPKI.
 * Returns 0 on pass, 1 on fail.
 * ---------------------------------------------------------------------- */
static int run_selftest(void)
{
    int pass = 1; /* optimistic */

    /* -- Part 1: library self-test ---------------------------------------- */
    int lib_result = x509_selftest();
    if (lib_result != 0) {
        ct_write("[certtool] x509_selftest() FAIL (code ");
        if (lib_result < 0) {
            ct_putc('-');
            ct_put_ulong((unsigned long)(-lib_result));
        } else {
            ct_put_ulong((unsigned long)lib_result);
        }
        ct_write(")\n");
        pass = 0;
    } else {
        ct_write("[certtool] x509_selftest(): PASS\n");
    }

    /* -- Part 2: parse embedded SPKI -------------------------------------- */
    unsigned long mod_len = 0;
    unsigned long exp_len = 0;
    int rc = x509_extract_pubkey(k_test_spki, sizeof(k_test_spki),
                                 g_mod, &mod_len,
                                 g_exp, &exp_len);
    if (rc != 0) {
        ct_write("[certtool] embedded SPKI parse FAIL (rc=");
        ct_put_ulong((unsigned long)rc);
        ct_write(")\n");
        pass = 0;
    } else {
        /* Expect 64-byte modulus (leading 0x00 sign byte stripped) */
        if (mod_len != 64) {
            ct_write("[certtool] embedded SPKI: unexpected mod_len=");
            ct_put_ulong(mod_len);
            ct_write(" (want 64)\n");
            pass = 0;
        }
        /* Expect exponent = 0x010001 */
        if (exp_len != 3 ||
            g_exp[0] != 0x01 || g_exp[1] != 0x00 || g_exp[2] != 0x01) {
            ct_write("[certtool] embedded SPKI: unexpected exponent\n");
            pass = 0;
        }
        if (pass) {
            ct_write("[certtool] embedded SPKI parse: PASS (mod_len=");
            ct_put_ulong(mod_len);
            ct_write(", exp=010001)\n");
        }
    }

    if (pass) {
        ct_write("CERTTOOL SELFTEST: PASS\n");
        return 0;
    } else {
        ct_write("CERTTOOL SELFTEST: FAIL\n");
        return 1;
    }
}

/* -------------------------------------------------------------------------
 * inspect_cert(): print all accessible fields of a DER certificate.
 * der/len: the DER buffer.
 * ---------------------------------------------------------------------- */
static void inspect_cert(const unsigned char *der, unsigned long len)
{
    int rc;

    ct_write("=== X.509 Certificate Inspector ===\n");

    /* -- Subject CN ------------------------------------------------------- */
    g_cn[0] = '\0';
    rc = x509_get_subject_cn(der, len, g_cn, CN_BUF_SIZE);
    ct_write("Subject CN : ");
    if (rc == 0 && g_cn[0] != '\0') {
        ct_write(g_cn);
    } else {
        ct_write("(not found)");
    }
    ct_putc('\n');

    /* -- Subject DN (raw DER view) ---------------------------------------- */
    {
        const unsigned char *dn = 0;
        unsigned long dnlen = 0;
        rc = x509_get_subject_dn(der, len, &dn, &dnlen);
        ct_write("Subject DN : ");
        if (rc == 0 && dn && dnlen > 0) {
            ct_put_ulong(dnlen);
            ct_write(" bytes  [");
            ct_put_hex_preview(dn, dnlen < 12 ? dnlen : 12);
            if (dnlen > 12) ct_write(" ...");
            ct_write("]");
        } else {
            ct_write("(unavailable)");
        }
        ct_putc('\n');
    }

    /* -- Issuer DN (raw DER view) ----------------------------------------- */
    {
        const unsigned char *dn = 0;
        unsigned long dnlen = 0;
        rc = x509_get_issuer_dn(der, len, &dn, &dnlen);
        ct_write("Issuer DN  : ");
        if (rc == 0 && dn && dnlen > 0) {
            ct_put_ulong(dnlen);
            ct_write(" bytes  [");
            ct_put_hex_preview(dn, dnlen < 12 ? dnlen : 12);
            if (dnlen > 12) ct_write(" ...");
            ct_write("]");
        } else {
            ct_write("(unavailable)");
        }
        ct_putc('\n');
    }

    /* -- Validity ---------------------------------------------------------- */
    {
        g_not_before[0] = '\0';
        g_not_after[0]  = '\0';
        rc = x509_get_validity_utc(der, len, g_not_before, g_not_after);
        ct_write("Not Before : ");
        if (rc == 0 && g_not_before[0] != '\0') {
            ct_write(g_not_before);
        } else {
            /* fall back to raw x509_get_validity which returns the original string */
            char nb_raw[32];
            char na_raw[32];
            nb_raw[0] = na_raw[0] = '\0';
            int rc2 = x509_get_validity(der, len, nb_raw, 32, na_raw, 32);
            ct_write(rc2 == 0 && nb_raw[0] != '\0' ? nb_raw : "(unavailable)");
        }
        ct_putc('\n');

        ct_write("Not After  : ");
        if (rc == 0 && g_not_after[0] != '\0') {
            ct_write(g_not_after);
        } else {
            char nb_raw[32];
            char na_raw[32];
            nb_raw[0] = na_raw[0] = '\0';
            int rc2 = x509_get_validity(der, len, nb_raw, 32, na_raw, 32);
            ct_write(rc2 == 0 && na_raw[0] != '\0' ? na_raw : "(unavailable)");
        }
        ct_putc('\n');
    }

    /* -- Public-key algorithm --------------------------------------------- */
    {
        int alg = -1;
        rc = x509_pubkey_alg(der, len, &alg);
        ct_write("PubKey Alg : ");
        if (rc != 0) {
            ct_write("(parse error)");
        } else if (alg == 0) {
            ct_write("RSA");
        } else if (alg == 1) {
            ct_write("EC");
        } else {
            ct_write("unknown (");
            ct_put_ulong((unsigned long)(alg < 0 ? 0 : (unsigned)alg));
            ct_putc(')');
        }
        ct_putc('\n');

        /* RSA: extract modulus and report bit-length */
        if (rc == 0 && alg == 0) {
            unsigned long mod_len = 0;
            unsigned long exp_len = 0;
            int prc = x509_extract_pubkey(der, len,
                                          g_mod, &mod_len,
                                          g_exp, &exp_len);
            ct_write("RSA Modulus: ");
            if (prc == 0 && mod_len > 0) {
                /* bit length = byte count * 8
                   (strip leading zero bytes the library already removes) */
                unsigned long bits = mod_len * 8;
                ct_put_ulong(bits);
                ct_write(" bits (");
                ct_put_ulong(mod_len);
                ct_write(" bytes)");
            } else {
                ct_write("(unavailable)");
            }
            ct_putc('\n');
        }

        /* EC: print uncompressed point first byte for quick sanity */
        if (rc == 0 && alg == 1) {
            unsigned char pt[65];
            int erc = x509_get_ec_pubkey(der, len, pt);
            ct_write("EC PubKey  : ");
            if (erc == 0) {
                ct_write("65-byte uncompressed point [04 ");
                ct_put_hex8(pt[1]);
                ct_write(" ");
                ct_put_hex8(pt[2]);
                ct_write(" ...]");
            } else {
                ct_write("(not 65-byte uncompressed point)");
            }
            ct_putc('\n');
        }
    }

    /* -- SAN dNSName entries ---------------------------------------------- */
    {
        int n = x509_get_san_dns(der, len, g_san, SAN_MAX);
        ct_write("SAN DNS    : ");
        if (n < 0) {
            ct_write("(parse error)");
        } else if (n == 0) {
            ct_write("(none)");
        } else {
            ct_put_ulong((unsigned long)n);
            ct_write(" entr");
            ct_write(n == 1 ? "y" : "ies");
            int i;
            for (i = 0; i < n; i++) {
                ct_write("\n             [");
                ct_put_ulong((unsigned long)i);
                ct_write("] ");
                ct_write(g_san[i]);
            }
        }
        ct_putc('\n');
    }
}

/* -------------------------------------------------------------------------
 * main()
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    /* -- Self-test when called with no arguments -------------------------- */
    if (argc <= 1) {
        return run_selftest();
    }

    /* -- Open and read the file ------------------------------------------- */
    const char *path = argv[1];
    long fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) {
        ct_write("certtool: cannot open '");
        ct_write(path);
        ct_write("'\n");
        return 1;
    }

    long total = 0;
    long n;
    while (total < (long)FILE_BUF_SIZE) {
        n = sc(SYS_READ, fd,
               (long)(g_filebuf + total),
               (long)(FILE_BUF_SIZE - (unsigned long)total),
               0, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

    if (total <= 0) {
        ct_write("certtool: file is empty or unreadable\n");
        return 1;
    }

    /* -- Detect PEM vs DER ------------------------------------------------ */
    const unsigned char *der;
    unsigned long derlen;

    /* PEM starts with "-----BEGIN" */
    if (total >= 10 &&
        ct_strncmp((const char *)g_filebuf, "-----BEGIN", 10) == 0)
    {
        /* NUL-terminate the file buffer for pem_extract_der */
        if (total < (long)FILE_BUF_SIZE)
            g_filebuf[total] = '\0';
        else
            g_filebuf[FILE_BUF_SIZE - 1] = '\0';

        b64_long dlen = pem_extract_der((const char *)g_filebuf,
                                        (b64_ulong)total,
                                        "CERTIFICATE",
                                        g_derbuf, DER_BUF_SIZE);
        if (dlen < 0) {
            ct_write("certtool: PEM decode failed\n");
            return 1;
        }
        der    = g_derbuf;
        derlen = (unsigned long)dlen;
        ct_write("[PEM input: ");
        ct_put_ulong((unsigned long)total);
        ct_write(" bytes -> ");
        ct_put_ulong(derlen);
        ct_write(" bytes DER]\n");
    } else {
        /* Assume raw DER */
        der    = g_filebuf;
        derlen = (unsigned long)total;
        ct_write("[DER input: ");
        ct_put_ulong(derlen);
        ct_write(" bytes]\n");
    }

    /* -- Parse and print -------------------------------------------------- */
    inspect_cert(der, derlen);

    return 0;
}
