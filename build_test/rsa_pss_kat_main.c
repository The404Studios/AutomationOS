/* Host harness for the RSA-PSS verify KAT (build_test/rsa_pss_kat.sh).
 * Verifies a real openssl-generated rsa_pss_rsae_sha256 signature and rejects a
 * tampered one. API: rsa_pss_verify(pk, sig, siglen, mHash, mHashLen, alg). */
#include <stdio.h>
#include "../userspace/lib/crypto/rsa.h"
#include "../userspace/lib/crypto/rsa_pss.h"
#include "../userspace/lib/crypto/sha256.h"
#include "rsa_pss_vec.h"

int main(void)
{
    rsa_pubkey pk;
    rsa_pubkey_from_bytes(&pk, rsapss_modulus, rsapss_modulus_len,
                          rsapss_exponent, rsapss_exponent_len);

    unsigned char mh[32];
    sha256(rsapss_msg, rsapss_msg_len, mh);   /* mHash = SHA-256(message) */

    int pass = 1;
    int rc = rsa_pss_verify(&pk, rsapss_sig, rsapss_sig_len, mh, 32, RSA_PSS_SHA256);
    if (rc != 0) { printf("  FAIL valid openssl PSS sig rejected rc=%d\n", rc); pass = 0; }
    else printf("  ok   valid openssl RSA-PSS-SHA256 signature verifies\n");

    { unsigned char bad[512];
      for (unsigned long i = 0; i < rsapss_sig_len; i++) bad[i] = rsapss_sig[i];
      bad[100] ^= 0x01;
      if (rsa_pss_verify(&pk, bad, rsapss_sig_len, mh, 32, RSA_PSS_SHA256) == 0) {
          printf("  FAIL tampered signature accepted\n"); pass = 0;
      } else printf("  ok   tampered signature rejected\n"); }

    { unsigned char bh[32];
      for (int i = 0; i < 32; i++) bh[i] = mh[i];
      bh[0] ^= 0x01;
      if (rsa_pss_verify(&pk, rsapss_sig, rsapss_sig_len, bh, 32, RSA_PSS_SHA256) == 0) {
          printf("  FAIL wrong message-hash accepted\n"); pass = 0;
      } else printf("  ok   wrong message-hash rejected\n"); }

    printf(pass ? "RSAPSSKAT: PASS\n" : "RSAPSSKAT: FAIL\n");
    return pass ? 0 : 1;
}
