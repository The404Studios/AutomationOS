/*
 * host_test.c — test malloc + snprintf logic from clib against glibc
 * Compile on Linux: gcc -std=gnu11 -O2 -Wall host_test.c -o host_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

/* ── Minimal types ── */
typedef unsigned long size_t_t;
typedef unsigned long uintptr_t_t;
typedef __builtin_va_list va_list_t;
#define MY_NULL ((void*)0)

/* ═══ Re-implement Sink + vsnprintf from clib.c for host testing ══════════ */

typedef struct { char *buf; size_t cap; size_t pos; } Sink;
static void sink_put(Sink *s, char c)  { if(s->buf && s->pos < s->cap-1) s->buf[s->pos]=c; s->pos++; }
static void sink_pad(Sink *s, char pad, int n) { for(int i=0;i<n;i++) sink_put(s,pad); }
static void sink_str(Sink *s, const char *str, int width, char pad) {
    if(!str) str="(null)";
    int len=0; const char *p=str; while(*p) len++,p++;
    if(width-len>0) sink_pad(s,pad,width-len);
    for(int i=0;i<len;i++) sink_put(s,str[i]);
}
static void sink_uint(Sink *s, unsigned long long v, int base, int up, int width, char pad) {
    char tmp[66]; int i=0;
    const char *lo="0123456789abcdef";
    const char *hi="0123456789ABCDEF";
    const char *t=up?hi:lo;
    if(v==0){tmp[i++]='0';}else{unsigned long long b=base;while(v){tmp[i++]=t[(int)(v%b)];v/=b;}}
    if(width-i>0) sink_pad(s,pad,width-i);
    while(i>0) sink_put(s,tmp[--i]);
}
static void sink_sint(Sink *s, long long v, int base, int up, int width, char pad) {
    if(v<0){
        if(pad=='0'&&width>0){sink_put(s,'-');width--;sink_uint(s,(unsigned long long)(-(v+1))+1ULL,base,up,width,pad);}
        else{sink_put(s,'-');sink_uint(s,(unsigned long long)(-(v+1))+1ULL,base,up,width-1,pad);}
    }else{sink_uint(s,(unsigned long long)v,base,up,width,pad);}
}
static int my_vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap) {
    Sink s; s.buf=buf; s.cap=(buf&&size>0)?size:1; s.pos=0;
    for(const char *p=fmt;*p;p++){
        if(*p!='%'){sink_put(&s,*p);continue;}
        p++; if(!*p) break;
        char pad=' '; if(*p=='0'){pad='0';p++;}
        int width=0; while(*p>='0'&&*p<='9'){width=width*10+(*p-'0');p++;}
        if(*p=='.'){p++;while(*p>='0'&&*p<='9')p++;}
        int lmod=0; while(*p=='l'){lmod++;p++;}
        switch(*p){
        case 'd':case 'i':{long long v=(lmod>=2)?__builtin_va_arg(ap,long long):(lmod==1)?(long long)__builtin_va_arg(ap,long):(long long)__builtin_va_arg(ap,int);sink_sint(&s,v,10,0,width,pad);break;}
        case 'u':{unsigned long long v=(lmod>=2)?__builtin_va_arg(ap,unsigned long long):(lmod==1)?(unsigned long long)__builtin_va_arg(ap,unsigned long):(unsigned long long)__builtin_va_arg(ap,unsigned int);sink_uint(&s,v,10,0,width,pad);break;}
        case 'x':{unsigned long long v=(lmod>=2)?__builtin_va_arg(ap,unsigned long long):(lmod==1)?(unsigned long long)__builtin_va_arg(ap,unsigned long):(unsigned long long)__builtin_va_arg(ap,unsigned int);sink_uint(&s,v,16,0,width,pad);break;}
        case 'X':{unsigned long long v=(lmod>=2)?__builtin_va_arg(ap,unsigned long long):(lmod==1)?(unsigned long long)__builtin_va_arg(ap,unsigned long):(unsigned long long)__builtin_va_arg(ap,unsigned int);sink_uint(&s,v,16,1,width,pad);break;}
        case 'p':{unsigned long long addr=(unsigned long long)(uintptr_t)__builtin_va_arg(ap,void*);sink_put(&s,'0');sink_put(&s,'x');sink_uint(&s,addr,16,0,16,'0');break;}
        case 's':{const char *sv=__builtin_va_arg(ap,const char*);sink_str(&s,sv,width,pad);break;}
        case 'c':{char cv=(char)__builtin_va_arg(ap,int);if(width>1)sink_pad(&s,' ',width-1);sink_put(&s,cv);break;}
        case '%':sink_put(&s,'%');break;
        default:sink_put(&s,'%');sink_put(&s,*p);break;
        }
    }
    if(buf&&size>0){size_t idx=(s.pos<size)?s.pos:size-1;buf[idx]='\0';}
    return(int)s.pos;
}
static int my_snprintf(char *buf, size_t size, const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    int r=my_vsnprintf(buf,size,fmt,ap); __builtin_va_end(ap); return r;
}

/* ═══ Malloc re-implementation using host mmap ═══════════════════════════ */

#define HEAP_MAGIC  0xA110CA7EUL
#define HEAP_ALIGN  16u
#define HEAP_SPLIT  32u

typedef struct Block Block;
struct Block { size_t size; unsigned free; unsigned magic; Block *next; unsigned char _pad[8]; };
typedef struct Arena Arena;
struct Arena { Arena *next; size_t capacity; Block *blocks; unsigned char _pad[8]; };
static Arena *arenas = NULL;

static void *blk_pay(Block *b) { return (void*)((unsigned char*)b+sizeof(Block)); }
static Block *pay_blk(void *p) { return (Block*)((unsigned char*)p-sizeof(Block)); }
static size_t aup(size_t n) { return (n+(HEAP_ALIGN-1u))&~(HEAP_ALIGN-1u); }
static void coal(Block *b) { while(b->next&&b->next->free){b->size+=sizeof(Block)+b->next->size;b->next=b->next->next;} }

static Arena *anew(size_t need) {
    size_t tot=need+sizeof(Arena)+sizeof(Block);
    if(tot<2*1024*1024) tot=2*1024*1024;
    tot=(tot+4095u)&~4095u;
    void *p=mmap(NULL,tot,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED) return NULL;
    Arena *a=(Arena*)p; a->next=NULL; a->capacity=tot-sizeof(Arena);
    Block *b=(Block*)((unsigned char*)p+sizeof(Arena));
    b->size=a->capacity-sizeof(Block);b->free=1;b->magic=HEAP_MAGIC;b->next=NULL;
    a->blocks=b; return a;
}
static void *my_malloc(size_t size) {
    if(!size) return NULL;
    size=aup(size);
    for(Arena *a=arenas;a;a=a->next)
        for(Block *b=a->blocks;b;b=b->next){
            if(!b->free||b->size<size) continue;
            if(b->size>=size+sizeof(Block)+HEAP_SPLIT){
                Block *r=(Block*)((unsigned char*)blk_pay(b)+size);
                r->size=b->size-size-sizeof(Block);r->free=1;r->magic=HEAP_MAGIC;r->next=b->next;b->next=r;b->size=size;
            }
            b->free=0; return blk_pay(b);
        }
    Arena *a=anew(size+sizeof(Block)); if(!a) return NULL;
    if(!arenas)arenas=a; else{Arena *t=arenas;while(t->next)t=t->next;t->next=a;}
    Block *b=a->blocks;
    if(b->size>=size+sizeof(Block)+HEAP_SPLIT){
        Block *r=(Block*)((unsigned char*)blk_pay(b)+size);
        r->size=b->size-size-sizeof(Block);r->free=1;r->magic=HEAP_MAGIC;r->next=b->next;b->next=r;b->size=size;
    }
    b->free=0; return blk_pay(b);
}
static void my_free(void *ptr) {
    if(!ptr) return;
    Block *b=pay_blk(ptr);
    if(b->magic!=HEAP_MAGIC||b->free) return;
    b->free=1; coal(b);
    for(Arena *a=arenas;a;a=a->next){
        Block *prev=NULL;
        for(Block *cur=a->blocks;cur;cur=cur->next){
            if(cur==b){if(prev&&prev->free)coal(prev);goto done;}
            prev=cur;
        }
    }
done:;
}
static void *my_calloc(size_t n,size_t sz){
    if(n&&sz>(size_t)-1/n)return NULL;
    size_t t=n*sz; void *p=my_malloc(t); if(p)memset(p,0,t); return p;
}
static void *my_realloc(void *ptr,size_t size){
    if(!ptr) return my_malloc(size);
    if(!size){my_free(ptr);return NULL;}
    Block *b=pay_blk(ptr);
    if(b->magic!=HEAP_MAGIC) return NULL;
    if(b->size>=aup(size)) return ptr;
    void *np=my_malloc(size); if(!np) return NULL;
    size_t copy=(b->size<size)?b->size:size;
    memcpy(np,ptr,copy); my_free(ptr); return np;
}

/* ═══ Test harness ════════════════════════════════════════════════════════ */
static int pass=0, fail=0;
#define OK(cond, msg) do { if(cond){pass++;}else{fail++;fprintf(stderr,"  FAIL: %s\n",msg);} }while(0)

static void test_snprintf(void) {
    char got[256], ref[256]; int r, rr;

    r=my_snprintf(got,256,"%d",42); rr=snprintf(ref,256,"%d",42);
    OK(r==rr&&!strcmp(got,ref), "%%d 42");

    r=my_snprintf(got,256,"%d",-999); rr=snprintf(ref,256,"%d",-999);
    OK(r==rr&&!strcmp(got,ref), "%%d -999");

    r=my_snprintf(got,256,"%08x",0xdeadbeef); rr=snprintf(ref,256,"%08x",0xdeadbeef);
    OK(r==rr&&!strcmp(got,ref), "%%08x deadbeef");

    r=my_snprintf(got,256,"%X",0xCAFE); rr=snprintf(ref,256,"%X",0xCAFE);
    OK(r==rr&&!strcmp(got,ref), "%%X CAFE");

    r=my_snprintf(got,256,"%u",4294967295u); rr=snprintf(ref,256,"%u",4294967295u);
    OK(r==rr&&!strcmp(got,ref), "%%u UINT_MAX");

    r=my_snprintf(got,256,"%10s","hi"); rr=snprintf(ref,256,"%10s","hi");
    OK(r==rr&&!strcmp(got,ref), "%%10s hi");

    r=my_snprintf(got,256,"%s",NULL);
    OK(!strcmp(got,"(null)"), "%%s NULL");

    r=my_snprintf(got,256,"%c",'Z'); rr=snprintf(ref,256,"%c",'Z');
    OK(r==rr&&!strcmp(got,ref), "%%c Z");

    r=my_snprintf(got,256,"100%%"); rr=snprintf(ref,256,"100%%");
    OK(r==rr&&!strcmp(got,ref), "%%%%");

    r=my_snprintf(got,256,"%ld",-1234567890L); rr=snprintf(ref,256,"%ld",-1234567890L);
    OK(r==rr&&!strcmp(got,ref), "%%ld");

    r=my_snprintf(got,256,"%lld",-9000000000LL); rr=snprintf(ref,256,"%lld",-9000000000LL);
    OK(r==rr&&!strcmp(got,ref), "%%lld");

    r=my_snprintf(got,256,"%llu",18446744073709551615ULL); rr=snprintf(ref,256,"%llu",18446744073709551615ULL);
    OK(r==rr&&!strcmp(got,ref), "%%llu ULLONG_MAX");

    r=my_snprintf(got,5,"hello world");
    OK(r==11 && !strcmp(got,"hell"), "truncation len+NUL");

    r=my_snprintf(got,256,"");
    OK(r==0&&got[0]=='\0', "empty fmt");

    r=my_snprintf(got,256,"[%05d][%s][%x]",7,"abc",255);
    rr=snprintf(ref,256,"[%05d][%s][%x]",7,"abc",255);
    OK(r==rr&&!strcmp(got,ref), "combined");

    r=my_snprintf(got,256,"%d",0); rr=snprintf(ref,256,"%d",0);
    OK(r==rr&&!strcmp(got,ref), "%%d 0");

    r=my_snprintf(got,256,"%x",0u); rr=snprintf(ref,256,"%x",0u);
    OK(r==rr&&!strcmp(got,ref), "%%x 0");
}

static void test_malloc(void) {
    void *p1=my_malloc(1);
    OK(p1!=NULL, "malloc(1) not null");
    OK(((uintptr_t)p1&15)==0, "malloc(1) 16-byte aligned");

    void *p2=my_malloc(128);
    OK(p2!=NULL, "malloc(128)");
    OK(((uintptr_t)p2&15)==0, "malloc(128) aligned");

    /* no overlap */
    OK((unsigned char*)p2>=(unsigned char*)p1+16 ||
       (unsigned char*)p1>=(unsigned char*)p2+128, "no overlap");

    /* calloc zeroed */
    unsigned char *c=(unsigned char*)my_calloc(100,4);
    OK(c!=NULL, "calloc(100,4)");
    int zero=1; for(int i=0;i<400;i++) if(c[i])zero=0;
    OK(zero, "calloc all zeros");

    /* realloc preserves data */
    unsigned char *r=(unsigned char*)my_malloc(32);
    for(int i=0;i<32;i++) r[i]=(unsigned char)i;
    unsigned char *r2=(unsigned char*)my_realloc(r,128);
    OK(r2!=NULL, "realloc 32->128");
    int ok=1; for(int i=0;i<32;i++) if(r2[i]!=(unsigned char)i)ok=0;
    OK(ok, "realloc data preserved");
    my_free(r2);

    /* alloc after free */
    void *a=my_malloc(64); void *b=my_malloc(64);
    OK(a&&b, "two allocs");
    my_free(a);
    void *a2=my_malloc(64);
    OK(a2!=NULL, "alloc after free");
    my_free(a2); my_free(b);

    /* free(NULL) safe */
    my_free(NULL);
    OK(1, "free(NULL) safe");

    /* stress: 2000 random alloc/free cycles */
    #define NP 256
    void *ptrs[NP];
    for(int i=0;i<NP;i++) ptrs[i]=NULL;
    int errs=0;
    for(int iter=0;iter<2000;iter++){
        int idx=iter%NP;
        if(ptrs[idx]){my_free(ptrs[idx]);ptrs[idx]=NULL;}
        size_t sz=(size_t)(((idx*17+iter*31)%200)+1)*8;
        ptrs[idx]=my_malloc(sz);
        if(!ptrs[idx]) errs++;
        if(ptrs[idx]&&((uintptr_t)ptrs[idx]&15)) errs++;
        if(ptrs[idx]) memset(ptrs[idx],0xAB,sz);
    }
    for(int i=0;i<NP;i++) if(ptrs[i]) my_free(ptrs[i]);
    OK(errs==0, "stress: alignment+no-null");
    #undef NP
}

int main(void) {
    printf("=== clib mini-libc host tests ===\n");
    printf("-- snprintf --\n");
    test_snprintf();
    printf("-- malloc --\n");
    test_malloc();
    printf("Results: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
