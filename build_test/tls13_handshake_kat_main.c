/* Offline full TLS 1.3 handshake KAT: drives the entire client handshake logic
 * over the RFC 8448 Section 3 recorded bytes and verifies every intermediate
 * against the trace -- ECDHE, transcript, key schedule, flight decrypt, message
 * walk, CertificateVerify, server Finished, client Finished. Proves the B6 state
 * machine logic end-to-end before any socket wiring. */
#include <stdio.h>
#include <string.h>
#include "../userspace/lib/tls/tls13_handshake.h"
#include "../userspace/lib/tls/tls13_keysched.h"
#include "../userspace/lib/tls/tls13_record.h"
#include "../userspace/lib/tls/tls13_certverify.h"
#include "../userspace/lib/crypto/rsa.h"
#include "../userspace/lib/crypto/x25519.h"
#include "../userspace/lib/crypto/sha256.h"
#include "rfc8448_record_vec.h"   /* rfc8448_server_flight_{record,plain} */

static int hx(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; }
static unsigned long uh(const char*h, unsigned char*o){ unsigned long n=0; for(;h[2*n];n++) o[n]=(unsigned char)((hx(h[2*n])<<4)|hx(h[2*n+1])); return n; }
static int eq(const unsigned char*a,const unsigned char*b,unsigned long n){ for(unsigned long i=0;i<n;i++) if(a[i]!=b[i])return 0; return 1; }
static unsigned int rd16(const unsigned char*p){ return ((unsigned)p[0]<<8)|p[1]; }

/* RFC 8448 Section 3 vectors. */
static const char CH_H[]="010000c00303cb34ecb1e78163ba1c38c6dacb196a6dffa21a8d9912ec18a2ef6283024dece7000006130113031302010000910000000b0009000006736572766572ff01000100000a00140012001d0017001800190100010101020103010400230000003300260024001d002099381de560e4bd43d23d8e435a7dbafeb3c06e51c13cae4d5413691e529aaf2c002b0003020304000d0020001e040305030603020308040805080604010501060102010402050206020202002d00020101001c00024001";
static const char SH_H[]="020000560303a6af06a4121860dc5e6e60249cd34c95930c8ac5cb1434dac155772ed3e2692800130100002e00330024001d0020c9828876112095fe66762bdbf7c672e156d6cc253b833df1dd69b1b04e751f0f002b00020304";
static const char CPRIV_H[]="49af42ba7f7994852d713ef2784bcbcaa7911de26adc5642cb634540e7ea5005";
static const char IKM_H[]="8bd4054fb55b9d63fdfbacf9f04b9f0d35e6d63f537563efd46272900f89492d";
static const char THSH_H[]="860c06edc07858ee8e78f0e7428c58edd6b43f2ca3e6e95f02ed063cf0e1cad8";
static const char THCERT_H[]="764d6632b3c35c3f3205e3499ac3edbaabb88295fba751461d3678e2e5ea0687";
static const char THCV_H[]="edb7725fa7a3473b031ec8ef65a2485493900138a2b91291407d7951a06110ed";
static const char THSF_H[]="9608102a0f1ccc6db6250b7b7e417b1a000eaada3daae4777a7686c9ff83df13";
static const char SKEY_H[]="3fce516009c21727d0f2e4e86ee403bc";
static const char SIV_H[]="5d313eb2671276ee13000b30";
static const char SFIN_H[]="9b9b141d906337fbd2cbdce71df4deda4ab42c309572cb7fffee5454b78f0718";
static const char CFIN_H[]="a8ec436d677634ae525ac1fcebe11a039ec17694fac6e98527b642f2edd5ce61";
static const char MOD_H[]="b4bb498f8279303d980836399b36c6988c0c68de55e1bdb826d3901a2461eafd2de49a91d015abbc9a95137ace6c1af19eaa6af98c7ced43120998e187a80ee0ccb0524b1b018c3e0b63264d449a6d38e22a5fda430846748030530ef0461c8ca9d9efbfae8ea6d1d03e2bd193eff0ab9a8002c47428a6d35a8d88d79f7f1e3f";

#define CK(cond,msg) do{ if(!(cond)){ printf("  FAIL %s\n",msg); return 1; } }while(0)

/* transcript hash = SHA256(CH || SH || flight[0..n)) */
static void th(const unsigned char*ch,unsigned long chl,const unsigned char*sh,unsigned long shl,
               const unsigned char*fl,unsigned long fln,unsigned char*out){
    sha256_ctx c; sha256_init(&c); sha256_update(&c,ch,chl); sha256_update(&c,sh,shl);
    if(fln) sha256_update(&c,fl,fln); sha256_final(&c,out);
}

int main(void){
    unsigned char CH[256],SH[128],cpriv[32],ikm[32],want[64];
    unsigned char th_chsh[32],th_chcert[32],th_chcv[32],th_chsf[32];
    unsigned char skey_w[16],siv_w[12],sfin[32],cfin[32],mod[128];
    unsigned long chl=uh(CH_H,CH), shl=uh(SH_H,SH);
    uh(CPRIV_H,cpriv); uh(IKM_H,ikm);
    uh(THSH_H,th_chsh); uh(THCERT_H,th_chcert); uh(THCV_H,th_chcv); uh(THSF_H,th_chsf);
    uh(SKEY_H,skey_w); uh(SIV_H,siv_w); uh(SFIN_H,sfin); uh(CFIN_H,cfin); uh(MOD_H,mod);

    /* parse ServerHello */
    unsigned short cipher,group; int is13; unsigned char ppub[64]; unsigned long ppublen;
    CK(tls13_parse_server_hello(SH,shl,&cipher,&is13,&group,ppub,sizeof ppub,&ppublen)==0,"parse SH");
    CK(is13==1,"detect TLS1.3"); CK(cipher==TLS13_AES_128_GCM_SHA256,"cipher 1301");
    CK(group==TLS13_GROUP_X25519 && ppublen==32,"key_share x25519");
    printf("  ok   ServerHello: TLS1.3 negotiated, AES_128_GCM_SHA256, x25519 key_share\n");

    /* 1. ECDHE */
    unsigned char ecdhe[32];
    x25519(ecdhe,cpriv,ppub);
    CK(eq(ecdhe,ikm,32),"X25519 ECDHE == RFC IKM");
    printf("  ok   X25519(client_priv, server_pub) == RFC 8448 shared secret\n");

    /* 2. transcript CH..SH */
    unsigned char t[32]; th(CH,chl,SH,shl,0,0,t); CK(eq(t,th_chsh,32),"H(CH||SH)");
    printf("  ok   transcript H(ClientHello..ServerHello) matches\n");

    /* 3. key schedule from computed IKM + transcript */
    unsigned char zero[32]={0}, eh[32], early[32], derived[32], hs[32], s_hs[32], c_hs[32], skey[16], siv[12];
    tls13_empty_hash(0,eh);
    tls13_extract(0,0,0,zero,32,early);
    tls13_derive_secret(0,early,"derived",eh,32,derived);
    tls13_extract(0,derived,32,ikm,32,hs);
    tls13_derive_secret(0,hs,"s hs traffic",t,32,s_hs);
    tls13_derive_secret(0,hs,"c hs traffic",t,32,c_hs);
    tls13_traffic_keyiv(0,s_hs,skey,16,siv,12);
    CK(eq(skey,skey_w,16)&&eq(siv,siv_w,12),"derived server hs key/iv == RFC");
    printf("  ok   key schedule from computed ECDHE -> server hs key/iv match RFC\n");

    /* 4. decrypt the server flight record */
    unsigned char fl[1024]; unsigned long fln; unsigned char itype;
    CK(tls13_record_open(TLS13_AEAD_AES128_GCM,skey,siv,0,
        rfc8448_server_flight_record,rfc8448_server_flight_record_len,
        fl,sizeof fl,&fln,&itype)==0,"decrypt flight");
    CK(itype==0x16 && fln==rfc8448_server_flight_plain_len,"flight inner=handshake 657");
    CK(eq(fl,rfc8448_server_flight_plain,fln),"flight plaintext == RFC");
    printf("  ok   server flight decrypts to RFC 8448 plaintext (%lu octets)\n",fln);

    /* 5. walk EE / Cert / CertVerify / Finished */
    unsigned long off=0; const unsigned char *b; unsigned long bl;
    const unsigned char *cv_body=0; unsigned long cv_len=0; const unsigned char *fin_body=0; unsigned long fin_len=0;
    int m;
    m=tls13_next_handshake_msg(fl,fln,&off,&b,&bl); CK(m==0x08,"EncryptedExtensions");
    m=tls13_next_handshake_msg(fl,fln,&off,&b,&bl); CK(m==0x0b,"Certificate");
    unsigned long off_after_cert=off;
    m=tls13_next_handshake_msg(fl,fln,&off,&b,&bl); CK(m==0x0f,"CertificateVerify"); cv_body=b; cv_len=bl;
    unsigned long off_after_cv=off;
    m=tls13_next_handshake_msg(fl,fln,&off,&b,&bl); CK(m==0x14,"Finished"); fin_body=b; fin_len=bl;
    CK(off==fln,"flight fully consumed");
    printf("  ok   flight walk: EncryptedExtensions, Certificate, CertificateVerify, Finished\n");

    /* 6. transcript CH..Certificate */
    unsigned char t2[32]; th(CH,chl,SH,shl,fl,off_after_cert,t2); CK(eq(t2,th_chcert,32),"H(CH..Cert)");

    /* 7. CertificateVerify: sigalg(2)||siglen(2)||sig */
    unsigned short sigalg=(unsigned short)rd16(cv_body); unsigned int siglen=rd16(cv_body+2);
    const unsigned char *sig=cv_body+4;
    CK(sigalg==0x0804 && siglen==128 && cv_len==132,"CertVerify sigalg/len");
    rsa_pubkey pk; unsigned char exp[3]={1,0,1}; rsa_pubkey_from_bytes(&pk,mod,128,exp,3);
    CK(tls13_certverify_rsapss(sigalg,1,t2,32,sig,siglen,&pk)==0,"CertificateVerify signature");
    printf("  ok   CertificateVerify (rsa_pss_rsae_sha256 over H(CH..Cert)) verifies\n");

    /* 8. transcript CH..CertificateVerify */
    unsigned char t3[32]; th(CH,chl,SH,shl,fl,off_after_cv,t3); CK(eq(t3,th_chcv,32),"H(CH..CertVerify)");

    /* 9. server Finished verify_data */
    unsigned char vd[32]; tls13_finished_verify(0,s_hs,t3,32,vd);
    CK(fin_len==32 && eq(vd,fin_body,32) && eq(vd,sfin,32),"server Finished verify_data");
    printf("  ok   server Finished verify_data recomputes + matches the message + RFC\n");

    /* 10. transcript CH..server Finished */
    unsigned char t4[32]; th(CH,chl,SH,shl,fl,fln,t4); CK(eq(t4,th_chsf,32),"H(CH..serverFin)");

    /* 11. client Finished verify_data */
    unsigned char cvd[32]; tls13_finished_verify(0,c_hs,t4,32,cvd);
    CK(eq(cvd,cfin,32),"client Finished verify_data");
    printf("  ok   client Finished verify_data == RFC 8448\n");

    printf("TLS13HSKAT: PASS (full RFC 8448 handshake driven end-to-end)\n");
    return 0;
}
