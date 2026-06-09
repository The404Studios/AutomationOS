/* tool_ls -- TOOLSET-0 list_dir. argv=[path]. opendir/readdir up to LS_MAX
 * entries, one name per line to stdout. NO recursion. If the directory has more
 * than LS_MAX entries, REJECT (exit 3, "ERR too_many_entries") -- a visible
 * refusal, not a silent partial list. Errors go to fd2. */
#define SYS_WRITE     3
#define SYS_OPENDIR  30
#define SYS_READDIR  31
#define SYS_CLOSEDIR 32
#define LS_MAX 32
#define NAME_MAX_ 256

typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX_];
} k_dirent_t;

static long sc(long n,long a,long b,long c){ long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void out(int fd,const char* s){ sc(SYS_WRITE,fd,(long)s,(long)slen(s)); }
static int has_dotdot(const char* p){ for(unsigned i=0;p[i];i++) if(p[i]=='.'&&p[i+1]=='.') return 1; return 0; }

int main(int argc,char** argv){
    if(argc<2 || !argv[1] || !argv[1][0]){ out(2,"ERR no_path\n"); return 2; }
    const char* p = argv[1];
    if(has_dotdot(p)){ out(2,"ERR traversal\n"); return 2; }
    long dfd = sc(SYS_OPENDIR,(long)p,0,0);
    if(dfd<0){ out(2,"ERR opendir\n"); return 2; }
    k_dirent_t de; int count=0;
    for(;;){
        long r = sc(SYS_READDIR,dfd,(long)&de,0);
        if(r != 0) break;                          /* 0 = got an entry; nonzero = end/error */
        de.d_name[NAME_MAX_-1]='\0';
        const char* nm = de.d_name;
        if(!nm[0]) continue;
        if((nm[0]=='.'&&!nm[1]) || (nm[0]=='.'&&nm[1]=='.'&&!nm[2])) continue;   /* skip . and .. */
        if(++count > LS_MAX){ sc(SYS_CLOSEDIR,dfd,0,0); out(2,"ERR too_many_entries\n"); return 3; }
        out(1, nm); out(1, "\n");
    }
    sc(SYS_CLOSEDIR,dfd,0,0);
    return 0;
}
