/* ledgerver -- verify the C4 tamper-evident hash-chain on the agent audit ledger.
 * =============================================================================
 * Re-reads /var/log/ai/actions.log and recomputes the FNV-1a chain that aibroker's
 * ledger_record writes. For each line: hash = FNV1a( prev_hash_hex || " " || <line up
 * to " hash="> ); compare to the stored hash; carry it forward. Any edit to a past line
 * breaks the chain. Prints exactly one greppable verdict:
 *     LEDGER: VERIFIED records=<n>
 *     LEDGER: TAMPERED line=<k>
 *     LEDGER: EMPTY            (no ledger / zero records)
 * Freestanding (crt0 main, direct syscalls, no libc). /var is ramfs => in-session only.
 *
 * Build: cc userspace/apps/ledgerver/ledgerver.c /tmp/ledgerver.o; $LD /tmp/crt0.o ... */

typedef unsigned long      u64;
typedef unsigned long long ull;

#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define O_RDONLY   0

static long sc(long n, long a, long b, long c){
    long r; register long r10 __asm__("r10")=0; register long r8 __asm__("r8")=0;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");
    return r;
}
static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void out(const char* s){ sc(SYS_WRITE,1,(long)s,(long)slen(s)); }
static void out_u(unsigned v){ char b[12]; int i=0; if(!v)b[i++]='0'; while(v){b[i++]=(char)('0'+v%10);v/=10;}
    char o[12]; int j=0; while(i)o[j++]=b[--i]; o[j]=0; out(o); }

/* FNV-1a 64-bit -- MUST match aibroker.c byte-for-byte. */
static u64 fnv1a(const char* p, int n){
    u64 h = 0xcbf29ce484222325UL;
    for(int i=0;i<n;i++){ h ^= (unsigned char)p[i]; h *= 0x100000001b3UL; }
    return h;
}
static void u64_to_hex16(u64 v, char* buf){
    static const char hx[]="0123456789abcdef";
    for(int i=15;i>=0;i--){ buf[i]=hx[v & 0xf]; v >>= 4; }
    buf[16]=0;
}
/* find the LAST occurrence of " hash=" in [s, s+len). returns index or -1. */
static int find_hash(const char* s, int len){
    for(int i=len-6;i>=0;i--)
        if(s[i]==' '&&s[i+1]=='h'&&s[i+2]=='a'&&s[i+3]=='s'&&s[i+4]=='h'&&s[i+5]=='=') return i;
    return -1;
}

int main(void){
    static char buf[65536];
    long fd = sc(SYS_OPEN, (long)"/var/log/ai/actions.log", O_RDONLY, 0);
    if(fd < 0){ out("LEDGER: EMPTY\n"); return 0; }
    int total=0;
    for(;;){ long r=sc(SYS_READ, fd, (long)(buf+total), (long)(sizeof(buf)-1-total));
        if(r<=0) break; total+=(int)r; if(total>=(int)sizeof(buf)-1) break; }
    sc(SYS_CLOSE, fd, 0, 0);
    buf[total]=0;
    if(total==0){ out("LEDGER: EMPTY\n"); return 0; }

    u64 prev = 0xcbf29ce484222325UL;   /* same seed as aibroker */
    unsigned recs=0, lineno=0;
    int start=0;
    static char chain[2048];
    for(int i=0;i<=total;i++){
        if(i<total && buf[i]!='\n') continue;
        int linelen = i - start;
        if(linelen <= 0){ start=i+1; continue; }   /* skip blank lines */
        lineno++;
        const char* line = buf + start;
        int hpos = find_hash(line, linelen);
        if(hpos < 0){ out("LEDGER: TAMPERED line="); out_u(lineno); out(" (no hash field)\n"); return 1; }
        /* stored hash = the 16 hex chars after " hash=" */
        const char* hstr = line + hpos + 6;
        /* body = line[0..hpos) ; chain = prev_hex || " " || body */
        char prevh[17]; u64_to_hex16(prev, prevh);
        int c=0; for(int k=0;k<16;k++) chain[c++]=prevh[k]; chain[c++]=' ';
        for(int k=0;k<hpos && c<(int)sizeof(chain)-1;k++) chain[c++]=line[k];
        chain[c]=0;
        char want[17]; u64 h=fnv1a(chain,c); u64_to_hex16(h,want);
        int ok=1; for(int k=0;k<16;k++) if(hstr[k]!=want[k]){ ok=0; break; }
        if(!ok){ out("LEDGER: TAMPERED line="); out_u(lineno); out("\n"); return 1; }
        prev=h; recs++;
        start=i+1;
    }
    out("LEDGER: VERIFIED records="); out_u(recs); out("\n");
    return 0;
}
