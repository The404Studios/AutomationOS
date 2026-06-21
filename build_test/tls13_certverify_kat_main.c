/* Host harness for the TLS 1.3 CertificateVerify KAT.
 * Verifies the real RFC 8448 Section 3 server CertificateVerify (rsa_pss_rsae_
 * sha256 over the CH..Certificate transcript hash) and rejects a tampered one. */
#include <stdio.h>
#include "../userspace/lib/tls/tls13_certverify.h"

int main(void)
{
    int rc = tls13_certverify_selftest();
    if (rc == 0) {
        printf("  ok   RFC 8448 server CertificateVerify (rsa_pss_rsae_sha256) verifies; "
               "tampered signature rejected\n");
        printf("TLS13CVKAT: PASS\n");
        return 0;
    }
    printf("  FAIL tls13_certverify_selftest rc=%d\n", rc);
    printf("TLS13CVKAT: FAIL\n");
    return 1;
}
