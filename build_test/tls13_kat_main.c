/* Host harness for the TLS 1.3 key-schedule KAT (build_test/tls13_kat.sh).
 * Compiles the real tls13_keysched.c + hkdf.c + sha/hmac sources and runs the
 * RFC 8448 Section 3 key-schedule KAT. */
#include <stdio.h>
#include "../userspace/lib/tls/tls13_keysched.h"

int main(void)
{
    int rc = tls13_keysched_selftest();
    if (rc == 0) {
        printf("  ok   RFC 8448 key schedule: early/derived/handshake/c+s hs traffic/"
               "keys+iv/master/c+s ap traffic all match\n");
        printf("TLS13KAT: PASS\n");
        return 0;
    }
    printf("  FAIL key-schedule step %d mismatched the RFC 8448 vector\n", rc);
    printf("TLS13KAT: FAIL step=%d\n", rc);
    return 1;
}
