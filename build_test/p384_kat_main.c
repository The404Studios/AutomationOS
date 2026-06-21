/* Host harness for the P-384 ECDSA verify KAT (build_test/p384_kat.sh).
 * Compiles the REAL crypto sources (bignum.c + sha512.c + p384.c) and runs
 * p384_selftest() (RFC 6979 A.2.6, "sample", SHA-384) plus extra edge checks. */
#include <stdio.h>
#include "../userspace/lib/crypto/p384.h"
#include "../userspace/lib/crypto/sha512.h"

/* hexval/parse for the extra checks. */
static int hx(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; }
static void parse48(const char*h, unsigned char*o){ for(int i=0;i<48;i++) o[i]=(unsigned char)((hx(h[2*i])<<4)|hx(h[2*i+1])); }

int main(void)
{
    int pass = 1;

    /* 1) The built-in selftest (valid verifies + tampered-r rejected). */
    if (p384_selftest() != 0) { printf("  FAIL selftest\n"); pass = 0; }
    else printf("  ok   selftest (RFC6979 A.2.6 sample/SHA-384: valid verifies, tampered r rejected)\n");

    /* 2) Independent re-check with explicit vectors + a tampered-s case and an
     *    out-of-range s case, to exercise the validation branches. */
    const char *QX="ec3a4e415b4e19a4568618029f427fa5da9a8bc4ae92e02e06aae5286b300c64def8f0ea9055866064a254515480bc13";
    const char *QY="8015d9b72d7d57244ea8ef9ac0c621896708a59367f9dfb9f54ca84b3f1c9db1288b231c3ae0d4fe7344fd2533264720";
    const char *R ="94edbb92a5ecb8aad4736e56c691916b3f88140666ce9fa73d64c4ea95ad133c81a648152e44acf96e36dd1e80fabe46";
    const char *S ="99ef4aeb15f178cea1fe40db2603138f130e740a19624526203b6351d0a3a94fa329c145786e679e7b82c71a38628ac8";
    unsigned char pub[97],r[48],s[48],h[48];
    pub[0]=0x04; parse48(QX,pub+1); parse48(QY,pub+49); parse48(R,r); parse48(S,s);
    static const unsigned char msg[6]={'s','a','m','p','l','e'};
    sha384(msg,6,h);

    if (p384_ecdsa_verify(pub,h,48,r,s)==0) printf("  ok   valid signature verifies\n");
    else { printf("  FAIL valid signature rejected\n"); pass=0; }

    { unsigned char s2[48]; for(int i=0;i<48;i++) s2[i]=s[i]; s2[47]^=0x02;
      if (p384_ecdsa_verify(pub,h,48,r,s2)!=0) printf("  ok   tampered s rejected\n");
      else { printf("  FAIL tampered s accepted\n"); pass=0; } }

    { unsigned char hh[48]; for(int i=0;i<48;i++) hh[i]=h[i]; hh[47]^=0x01;
      if (p384_ecdsa_verify(pub,hh,48,r,s)!=0) printf("  ok   wrong message-hash rejected\n");
      else { printf("  FAIL wrong hash accepted\n"); pass=0; } }

    { unsigned char s0[48]; for(int i=0;i<48;i++) s0[i]=0;
      if (p384_ecdsa_verify(pub,h,48,r,s0)!=0) printf("  ok   s==0 rejected\n");
      else { printf("  FAIL s==0 accepted\n"); pass=0; } }

    { unsigned char pb[97]; for(int i=0;i<97;i++) pb[i]=pub[i]; pb[1]^=0x01; /* off-curve X */
      if (p384_ecdsa_verify(pb,h,48,r,s)!=0) printf("  ok   off-curve pubkey rejected\n");
      else { printf("  FAIL off-curve pubkey accepted\n"); pass=0; } }

    printf(pass ? "P384KAT: PASS\n" : "P384KAT: FAIL\n");
    return pass ? 0 : 1;
}
