/* Differential test: runs many generated vectors through the REAL p384 verify.
 * Catches bugs the single RFC6979 vector cannot (malleability, long/short hash,
 * out-of-range r/s, off-curve, x>=n reduction, random valid sigs). */
#include <stdio.h>
#include <string.h>
#include "../userspace/lib/crypto/p384.h"
#include "p384_vectors.h"

static int hx(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; }
static int parse(const char*h, unsigned char*o, int maxbytes){
    int len = (int)(strlen(h)/2);
    if (len>maxbytes) len=maxbytes;
    for(int i=0;i<len;i++) o[i]=(unsigned char)((hx(h[2*i])<<4)|hx(h[2*i+1]));
    return len;
}

int main(void){
    int pass=1, nfa=0, nfr=0;
    for(int t=0;t<P384_TV_N;t++){
        const p384_tv *v=&P384_TV[t];
        unsigned char pub[97], r[48], s[48], h[64];
        pub[0]=0x04;
        parse(v->qx, pub+1, 48);
        parse(v->qy, pub+49, 48);
        parse(v->r, r, 48);
        parse(v->s, s, 48);
        int hlen = parse(v->h, h, 64);
        int rc = p384_ecdsa_verify(pub, h, (unsigned long)hlen, r, s);
        int got_valid = (rc==0);
        if (got_valid != (v->expect!=0)) {
            pass=0;
            if (v->expect) nfr++; else nfa++;
            printf("  MISMATCH [%s] #%d: expect=%d got_valid=%d %s\n",
                   v->label, t, v->expect, got_valid,
                   (!v->expect && got_valid) ? "<<< FALSE-ACCEPT (forgery verified!)" :
                   (v->expect && !got_valid) ? "<<< FALSE-REJECT" : "");
        }
    }
    printf("Ran %d vectors. false_accepts=%d false_rejects=%d\n", P384_TV_N, nfa, nfr);
    printf(pass ? "P384DIFF: PASS\n" : "P384DIFF: FAIL\n");
    return pass?0:1;
}
