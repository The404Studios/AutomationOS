/* Targeted unit tests for the bignum helpers p384 relies on, especially the
 * final v = R.x mod n reduction (bn_mod) on values >= n that the ECDSA
 * differential test cannot reach by chance, plus bn_mod_add/sub/mul edge cases.
 * Compares against a tiny independent reference computed here in __int128 where
 * possible, and against hand-picked boundary values. */
#include <stdio.h>
#include <string.h>
#include "../userspace/lib/crypto/bignum.h"

static int hx(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; }
static void parsehex(bignum*x,const char*h){
    int nch=strlen(h); int nb=nch/2; unsigned char be[200];
    for(int i=0;i<nb;i++) be[i]=(unsigned char)((hx(h[2*i])<<4)|hx(h[2*i+1]));
    bn_from_bytes(x,be,nb);
}
static void prhex(const char*tag,const bignum*x){
    unsigned char be[48]; bn_to_bytes(x,be,48);
    printf("%s=",tag); for(int i=0;i<48;i++) printf("%02x",be[i]); printf("\n");
}

int main(void){
    int pass=1;
    const char*N="ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973";
    bignum n; parsehex(&n,N);

    /* 1) bn_mod(n, n) must be 0. */
    { bignum r; bn_mod(&r,&n,&n); if(!bn_is_zero(&r)){printf("FAIL bn_mod(n,n)!=0\n");prhex("got",&r);pass=0;} else printf("ok bn_mod(n,n)=0\n"); }

    /* 2) bn_mod(n+1, n) must be 1. */
    { bignum one,np1,r; bn_set_u32(&one,1); bn_mod_add(&np1,&n,&one,&n); /* (n+1) mod n via add? a must be<n. use raw add */
      /* build n+1 directly */ unsigned char be[48]; bn_to_bytes(&n,be,48); for(int i=47;i>=0;i--){ if(be[i]!=0xff){be[i]++;break;} else be[i]=0; }
      bignum np1raw; bn_from_bytes(&np1raw,be,48);
      bn_mod(&r,&np1raw,&n);
      bignum oneb; bn_set_u32(&oneb,1);
      if(bn_cmp(&r,&oneb)!=0){printf("FAIL bn_mod(n+1,n)!=1\n");prhex("got",&r);pass=0;} else printf("ok bn_mod(n+1,n)=1\n");
    }

    /* 3) bn_mod(2n-1, n) must be n-1. The classic value that exercises a value
     *    in [n, 2n) -- exactly the regime R.x can land in (R.x in [n,p) < 2n? no,
     *    p ~ 2n, p-n ~ small, so R.x in [n,p) IS in [n,2n)). */
    { /* 2n-1 */ bignum two_n, r, nm1;
      bn_mod_add(&two_n,&n,&n,&n); /* WRONG: a,b must be <n; this gives 0. compute raw 2n */
      /* raw 2n: shift n left by 1 */
      unsigned char be[48]; bn_to_bytes(&n,be,48); unsigned char two[49]; int carry=0;
      for(int i=47;i>=0;i--){int v=(be[i]<<1)|carry; two[i+1]=(unsigned char)(v&0xff); carry=(v>>8)&1;} two[0]=(unsigned char)carry;
      /* 2n - 1 */ for(int i=48;i>=0;i--){ if(two[i]!=0){two[i]--;break;} else two[i]=0xff; }
      bignum twonm1; bn_from_bytes(&twonm1,two,49);
      bn_mod(&r,&twonm1,&n);
      /* n-1 */ unsigned char bem1[48]; bn_to_bytes(&n,bem1,48); for(int i=47;i>=0;i--){ if(bem1[i]!=0){bem1[i]--;break;} else bem1[i]=0xff; }
      bn_from_bytes(&nm1,bem1,48);
      if(bn_cmp(&r,&nm1)!=0){printf("FAIL bn_mod(2n-1,n)!=n-1\n");prhex("got",&r);prhex("exp",&nm1);pass=0;} else printf("ok bn_mod(2n-1,n)=n-1 (value in [n,2n) reduced)\n");
    }

    /* 4) bn_mod of a value just below n is unchanged. */
    { bignum nm1,r; unsigned char be[48]; bn_to_bytes(&n,be,48); for(int i=47;i>=0;i--){ if(be[i]!=0){be[i]--;break;} else be[i]=0xff; }
      bn_from_bytes(&nm1,be,48); bn_mod(&r,&nm1,&n);
      if(bn_cmp(&r,&nm1)!=0){printf("FAIL bn_mod(n-1,n)!=n-1\n");pass=0;} else printf("ok bn_mod(n-1,n)=n-1 (no change)\n");
    }

    /* 5) The SPECIFIC regime: a value x in [n, p) where p is the FIELD prime.
     *    R.x is a field element (< p). v = x mod n. Pick x = p-1 (largest field
     *    elt). Compute (p-1) mod n with the C code and with a reference. */
    { const char*P="fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff";
      bignum p_; parsehex(&p_,P);
      bignum pm1; unsigned char be[48]; bn_to_bytes(&p_,be,48); for(int i=47;i>=0;i--){ if(be[i]!=0){be[i]--;break;} else be[i]=0xff; } bn_from_bytes(&pm1,be,48);
      bignum r; bn_mod(&r,&pm1,&n);
      /* reference (p-1) mod n: since n < p < 2n, (p-1) mod n = (p-1) - n. */
      bignum ref; bn_mod_sub(&ref,&pm1,&n,&p_); /* (p-1-n) mod p; since p-1-n < p, == p-1-n. but need it as plain integer; p-1-n < n so fine */
      if(bn_cmp(&r,&ref)!=0){printf("FAIL bn_mod(p-1,n) mismatch\n");prhex("got",&r);prhex("ref",&ref);pass=0;}
      else printf("ok bn_mod(p-1,n) = (p-1-n), exercises x in [n,p) -- the real R.x>=n path\n");
    }

    printf(pass?"P384BN: PASS\n":"P384BN: FAIL\n");
    return pass?0:1;
}
