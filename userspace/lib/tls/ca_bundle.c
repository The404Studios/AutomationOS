/*
 * ca_bundle.c -- CA root certificate trust store.
 * ================================================
 *
 * Freestanding pure data + code.
 * NO syscalls. NO libc. NO malloc. NO standard headers.
 *
 * Build flags (NO fs:0x28 stack canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * ================================================================
 * TRUST STORE -- 7 EMBEDDED ROOT CAs
 * ================================================================
 * This module ships with 7 real, bit-perfect root CA certificates,
 * compiled in from ca_roots_data.h (ISRG Root X1, GTS Root R1,
 * DigiCert Global Root CA, DigiCert Global Root G2, GlobalSign Root CA,
 * USERTrust RSA Certification Authority, Amazon Root CA 1). Together
 * they anchor the large majority of public HTTPS certificates.
 *
 * With these roots present the TLS layer can both ENCRYPT traffic and
 * AUTHENTICATE the remote peer: x509_verify_chain() can chain a server
 * certificate to one of these roots, set cert_trusted == 1 in
 * tls_conn_t, and make tls_cert_trusted() return 1. A server whose
 * chain does NOT anchor here yields X509V_ERR_NO_ROOT -> cert_trusted
 * == 0 (encrypted-but-unauthenticated).
 *
 * Callers that display a "secure connection" indicator MUST gate it
 * on tls_cert_trusted() returning 1, not merely on the TLS handshake
 * completing.
 *
 * Every embedded root is bit-exact DER obtained from an authoritative
 * source via the openssl pipeline below -- never hand-typed -- because a
 * single flipped bit produces a root that silently fails to match real
 * server chains.
 *
 * TO POPULATE THE TRUST STORE
 * ----------------------------
 * Build-time (permanent, no recompile of this file):
 *   Edit userspace/lib/tls/ca_roots_data.h following the instructions
 *   in that file.  The one-liner is:
 *     openssl x509 -in root.pem -outform DER | xxd -i -n my_root_der
 *   Paste the output into ca_roots_data.h and add a CA_ROOTS_DATA entry.
 *
 * Runtime (no recompile at all):
 *   Call ca_add_root_pem() at startup before any TLS connections are
 *   made.  Suitable for an update-ca tool that reads roots from a disk
 *   file or a kernel config block.
 * ================================================================
 *
 * EMBEDDED ROOTS: 7 (see ca_roots_data.h for the exact set).
 *   Reproducing multi-hundred-byte DER blobs from memory is unreliable;
 *   a single flipped bit produces a root that silently fails to match
 *   real server chains.  Each embedded root was obtained via the openssl
 *   pipeline above to guarantee bit-perfect bytes from an authoritative
 *   source.
 */

#include "ca_bundle.h"
#include "../crypto/base64.h"

/* ======================================================================= */
/* Internal entry type                                                      */
/* ======================================================================= */

typedef struct {
    const char          *name;  /* human label, NUL-terminated                */
    const unsigned char *der;   /* DER bytes (static or slab-pooled)           */
    unsigned long        len;   /* byte count of DER blob                      */
} ca_entry_t;

/* ======================================================================= */
/* Build-time static roots (populated from ca_roots_data.h)                */
/* ======================================================================= */

/*
 * ca_roots_data.h defines CA_ROOTS_DATA as a comma-separated list of
 * { name, der, len } initializers, or nothing when the bundle is empty.
 * It also defines any static const unsigned char arrays for the DER blobs.
 */
#include "ca_roots_data.h"

static ca_entry_t s_roots[CA_MAX_ROOTS] = {
#ifdef CA_ROOTS_DATA
    CA_ROOTS_DATA
#endif
    /* sentinel to avoid zero-length array when CA_ROOTS_DATA is empty */
    { (const char *)0, (const unsigned char *)0, 0 }
};

/* Count of static (build-time) entries; computed once at init. */
static int s_static_count = -1;   /* -1 = not yet counted */

/* ======================================================================= */
/* Runtime DER slab pool (for ca_add_root_pem)                             */
/* ======================================================================= */

static unsigned char s_pool[CA_POOL_BYTES];
static unsigned long s_pool_used = 0;

/* ======================================================================= */
/* One-time initialisation (lazy, called from every public function)        */
/* ======================================================================= */

static void bundle_init(void)
{
    int i;
    if (s_static_count >= 0)
        return;   /* already done */

    s_static_count = 0;
    for (i = 0; i < CA_MAX_ROOTS; i++) {
        /* The sentinel { NULL, NULL, 0 } terminates the static section. */
        if (s_roots[i].name == (const char *)0)
            break;
        s_static_count++;
    }
    /* s_roots[s_static_count] onward is available for runtime entries.
     * The sentinel slot at the end of the static section is reused for
     * the first runtime entry, so we start writing at s_static_count. */
}

/* Total number of valid entries (static + runtime). */
static int bundle_count(void)
{
    int i;
    bundle_init();
    /* Count upward until we hit a NULL name or reach the hard cap. */
    for (i = 0; i < CA_MAX_ROOTS; i++) {
        if (s_roots[i].name == (const char *)0)
            return i;
    }
    return CA_MAX_ROOTS;
}

/* ======================================================================= */
/* Public API                                                               */
/* ======================================================================= */

int ca_get_count(void)
{
    return bundle_count();
}

const unsigned char *ca_get_der(int i, unsigned long *out_len)
{
    int count;
    if (!out_len)
        return (const unsigned char *)0;
    count = bundle_count();
    if (i < 0 || i >= count)
        return (const unsigned char *)0;
    *out_len = s_roots[i].len;
    return s_roots[i].der;
}

const char *ca_get_name(int i)
{
    int count = bundle_count();
    if (i < 0 || i >= count)
        return (const char *)0;
    return s_roots[i].name;
}

/*
 * ca_bundle_selftest -- verify internal consistency of every stored entry.
 *
 * Checks:
 *   - name is non-NULL
 *   - der is non-NULL
 *   - len > 0
 *   - der[0] == 0x30  (DER SEQUENCE tag; all X.509 certs start with this)
 *
 * Passes trivially with an empty bundle (nothing to check).
 * Returns 0 on pass, -1 on the first inconsistency found.
 */
int ca_bundle_selftest(void)
{
    int i, count;
    count = ca_get_count();

    for (i = 0; i < count; i++) {
        const unsigned char *der;
        unsigned long len = 0;
        const char *name;

        name = ca_get_name(i);
        if (!name)
            return -1;

        der = ca_get_der(i, &len);
        if (!der)
            return -1;
        if (len == 0)
            return -1;

        /* Every DER-encoded X.509 certificate begins with 0x30 (SEQUENCE). */
        if (der[0] != 0x30)
            return -1;
    }

    return 0;   /* OK -- empty bundle or all entries consistent */
}

/*
 * ca_add_root_pem -- install a root CA certificate from a PEM string.
 *
 * Locates "-----BEGIN CERTIFICATE-----" in [pem, pem+pemlen), base64-decodes
 * the body into the internal static slab, and appends the entry to the bundle.
 *
 * Return values:
 *   0   success
 *  -1   bundle full (CA_MAX_ROOTS reached; no space for another entry)
 *  -2   slab pool full (CA_POOL_BYTES exhausted; DER won't fit)
 *  -3   pem_extract_der() failed (malformed PEM, bad base64, or output too small)
 *  -4   name is NULL
 */
int ca_add_root_pem(const char *name, const char *pem, unsigned long pemlen)
{
    int count;
    b64_long der_len;
    unsigned long avail;

    if (!name)
        return -4;

    bundle_init();
    count = bundle_count();
    if (count >= CA_MAX_ROOTS)
        return -1;

    /* How many bytes remain in the slab? */
    avail = (unsigned long)CA_POOL_BYTES - s_pool_used;

    /*
     * Decode the PEM block directly into the tail of the slab.
     * pem_extract_der() (from ../crypto/base64.h) locates:
     *   -----BEGIN CERTIFICATE-----
     *   ... base64 body ...
     *   -----END CERTIFICATE-----
     * and writes the raw DER bytes into the provided output buffer.
     */
    der_len = pem_extract_der(pem, pemlen, "CERTIFICATE",
                              s_pool + s_pool_used, avail);
    if (der_len <= 0)
        return -3;

    /* Append the new entry.  We write at `count`; the sentinel that was
     * there is overwritten, so we must ensure the slot after it is still
     * NULL -- which is guaranteed because s_roots was zero-initialised as
     * a static array and we checked count < CA_MAX_ROOTS above. */
    s_roots[count].name = name;
    s_roots[count].der  = s_pool + s_pool_used;
    s_roots[count].len  = (unsigned long)der_len;

    /* Advance pool cursor.  Align to 8 bytes for any future DER reads. */
    s_pool_used += (unsigned long)der_len;
    s_pool_used  = (s_pool_used + 7u) & ~7u;

    return 0;
}
