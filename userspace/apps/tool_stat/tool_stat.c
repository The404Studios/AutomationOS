/* tool_stat -- TOOLSET-0 stat. argv=[path]. Writes "size=<n> type=<f|d>\n" to
 * stdout. Type is probed via SYS_OPENDIR (st_mode carries no POSIX type bit on
 * this kernel). Errors go to fd2. */
#define SYS_WRITE     3
#define SYS_OPENDIR  30
#define SYS_CLOSEDIR 32
#define SYS_STAT     33

typedef struct {
    unsigned long long st_dev, st_ino;
    unsigned int st_mode, st_nlink, st_uid, st_gid;
    unsigned long long st_rdev, st_size, st_blksize, st_blocks, st_atime, st_mtime, st_ctime;
} k_stat_t;

static long sc(long n,long a,long b,long c){ long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void out(int fd,const char* s){ sc(SYS_WRITE,fd,(long)s,(long)slen(s)); }
static void out_u(int fd,unsigned long long v){ char b[24]; int i=0;
    if(!v){ out(fd,"0"); return; } while(v){ b[i++]=(char)('0'+v%10); v/=10; }
    char r[24]; int j=0; while(i) r[j++]=b[--i]; r[j]=0; out(fd,r); }
static int has_dotdot(const char* p){ for(unsigned i=0;p[i];i++) if(p[i]=='.'&&p[i+1]=='.') return 1; return 0; }

int main(int argc,char** argv){
    if(argc<2 || !argv[1] || !argv[1][0]){ out(2,"ERR no_path\n"); return 2; }
    const char* p = argv[1];
    if(has_dotdot(p)){ out(2,"ERR traversal\n"); return 2; }
    k_stat_t st;
    if(sc(SYS_STAT,(long)p,(long)&st,0) < 0){ out(2,"ERR stat\n"); return 2; }
    long dfd = sc(SYS_OPENDIR,(long)p,0,0);
    int is_dir = (dfd>=0); if(is_dir) sc(SYS_CLOSEDIR,dfd,0,0);
    out(1,"size="); out_u(1,st.st_size); out(1," type="); out(1,is_dir?"d":"f"); out(1,"\n");
    return 0;
}
