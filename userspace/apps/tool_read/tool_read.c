/* tool_read -- TOOLSET-0 read_file. argv=[path]. ONE bounded read:
 *   - reject empty path / a path containing ".." (defense in depth; the host
 *     also enforces this)
 *   - SYS_STAT first; if st_size > READ_CAP, REJECT (exit 3) -- never truncate
 *   - else open O_RDONLY, read the exact bytes, write them to stdout (fd1)
 * Errors go to fd2 (serial), never to fd1 (the capability channel). The host
 * reads the exact file bytes from fd1 and the exit code from TOOL_RESULT. */
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_STAT  33
#define O_RDONLY   0
#define READ_CAP 256

typedef struct {
    unsigned long long st_dev, st_ino;
    unsigned int st_mode, st_nlink, st_uid, st_gid;
    unsigned long long st_rdev, st_size, st_blksize, st_blocks, st_atime, st_mtime, st_ctime;
} k_stat_t;

static long sc(long n,long a,long b,long c){ long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void err(const char* s){ sc(SYS_WRITE,2,(long)s,(long)slen(s)); }
static int has_dotdot(const char* p){ for(unsigned i=0;p[i];i++) if(p[i]=='.'&&p[i+1]=='.') return 1; return 0; }

int main(int argc,char** argv){
    if(argc<2 || !argv[1] || !argv[1][0]){ err("ERR no_path\n"); return 2; }
    const char* p = argv[1];
    if(has_dotdot(p)){ err("ERR traversal\n"); return 2; }
    k_stat_t st;
    if(sc(SYS_STAT,(long)p,(long)&st,0) < 0){ err("ERR stat\n"); return 2; }
    if(st.st_size > READ_CAP){ err("ERR oversize\n"); return 3; }      /* reject, no read */
    long fd = sc(SYS_OPEN,(long)p,O_RDONLY,0);
    if(fd<0){ err("ERR open\n"); return 2; }
    char buf[READ_CAP]; int total=0; long n;
    while(total<READ_CAP && (n=sc(SYS_READ,fd,(long)(buf+total),READ_CAP-total))>0) total+=(int)n;
    sc(SYS_CLOSE,fd,0,0);
    sc(SYS_WRITE,1,(long)buf,total);                                   /* exact bytes -> stdout */
    return 0;
}
