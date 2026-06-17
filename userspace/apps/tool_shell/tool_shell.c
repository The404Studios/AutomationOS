/* tool_shell -- gated agent tool: run ONE bare /bin command by name.
 * =====================================================================
 *
 * The agent rail (sbin/agentd) invokes this with:
 *     argv = [ cmd, arg1, arg2, ... ]
 * i.e. argv[1] is the command NAME (a bare /bin program, no path), and
 * argv[2..] are the arguments to hand that command as ITS own argv[1..].
 *
 * THE GATE (the model is HOSTILE TEXT -- it picks `cmd`): we only ever run a
 * bare command name resolved under /bin. So `cmd` must
 *   - contain NO '/'  (no absolute paths, no escaping /bin), and
 *   - NOT contain ".." (defense in depth against traversal tricks).
 * Anything else is a visible refusal: "DENY shell <cmd>\n" to fd 1, exit 2.
 * On allow we build the path "bin/<cmd>" into a bounded buffer, spawn it with
 * the remaining args, wait, and report "SHELL <cmd> exit=<n>\n" to fd 1.
 *
 * FD CONVENTION: fd 1 is the capability channel the agent CAPTURES as the
 * result the model reads -- so the ONE-LINE OUTCOME (DENY.../SHELL...) goes to
 * fd 1. The spawned child inherits our fds, so the command's own output flows
 * out to serial. Diagnostics (spawn failure) go to fd 2, never fd 1.
 *
 * FREESTANDING ring-3 tool: NO libc, NO headers, single self-contained .c.
 * crt0 provides _start and calls main(argc,argv) -- we do NOT define _start.
 * Build flags add -fno-stack-protector; the .o has ZERO fs:0x28 references.
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/tool_shell/tool_shell.c -o tool_shell.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       tool_shell.o crt0.o -o build/tool_shell
 *   objdump -d build/tool_shell | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WAITPID        6
#define SYS_WRITE          3
#define SYS_SPAWN_EX_ARGV  106

/* -----------------------------------------------------------------------
 * Syscall wrappers (copied verbatim from the ABI kit).
 * --------------------------------------------------------------------- */
static long sc(long n,long a,long b,long c){ long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
static long sc6(long n,long a,long b,long c,long d,long e,long f){ long r;
    register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e; register long r9 __asm__("r9")=f;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory"); return r; }

/* -----------------------------------------------------------------------
 * Freestanding string/number/output helpers.
 * --------------------------------------------------------------------- */
static unsigned slen(const char*s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void out(int fd,const char*s){ sc(SYS_WRITE,fd,(long)s,(long)slen(s)); }
/* signed decimal -> fd (exit status may be negative). */
static void out_d(int fd,long v){
    char b[24]; int i=0; char t[24]; int j=0;
    int neg = (v<0);
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if(u==0) t[j++]='0';
    while(u){ t[j++]=(char)('0'+u%10); u/=10; }   /* const /10 only */
    if(neg) b[i++]='-';
    while(j) b[i++]=t[--j];
    b[i]=0; out(fd,b);
}

/* THE PATH GATE primitive: does `p` contain the substring `s`? (copied
 * verbatim from the ABI kit -- used here to reject ".." in the command name). */
static int has_sub(const char*p,const char*s){
    for(int i=0;p[i];i++){ int j=0; while(s[j]&&p[i+j]==s[j]) j++; if(!s[j]) return 1; }
    return 0;
}

/* -----------------------------------------------------------------------
 * Spawn + wait (copied verbatim from the ABI kit). Packs args[0..n-1] into a
 * NUL-separated argv buffer and hands them to the child as its argv[1..].
 * Returns the child's exit status, or -1 (spawn) / -2 (wait) on failure.
 * --------------------------------------------------------------------- */
static int argv_pack(char*buf,int cap,const char*const*args,int n){
    int p=0;
    for(int i=0;i<n;i++){ const char*a=args[i]; for(int j=0;a[j]&&p<cap-1;j++) buf[p++]=a[j]; if(p<cap) buf[p++]=0; }
    return p;
}
static long spawn_wait(const char*path,const char*const*args,int nargs){
    char av[512];
    int al=argv_pack(av,512,args,nargs);
    long pid=sc6(SYS_SPAWN_EX_ARGV,(long)path,(long)av,al,0,0,0);
    if(pid<0) return -1;
    long st=0;
    long w=sc(SYS_WAITPID,pid,(long)&st,0);
    return w<0 ? -2 : st;
}

int main(int argc,char**argv){
    /* Validate argc/argv before any deref: we need at least the command name. */
    if(argc<2 || !argv[1] || !argv[1][0]){ out(1,"DENY shell \n"); return 2; }
    const char* cmd = argv[1];

    /* THE GATE: `cmd` must be a BARE /bin command name --
     *   - no '/' (no absolute paths, no escaping the /bin namespace), and
     *   - no ".." anywhere (defense in depth against traversal tricks).
     * Reject visibly on fd 1; the model reads this line as the result. */
    int bad = has_sub(cmd,"..");
    for(int i=0; cmd[i] && !bad; i++) if(cmd[i]=='/') bad=1;
    if(bad){ out(1,"DENY shell "); out(1,cmd); out(1,"\n"); return 2; }

    /* Build the resolved path "bin/<cmd>" into a bounded buffer. The spawn
     * path is relative ("bin/cmd"), matching how the kit spawns "bin/cc". */
    char path[256];
    int p=0;
    path[p++]='b'; path[p++]='i'; path[p++]='n'; path[p++]='/';
    for(int i=0; cmd[i] && p < (int)sizeof(path)-1; i++) path[p++]=cmd[i];
    path[p]=0;

    /* The command's own argv[1..] = our argv[2..] (argc-2 of them). When the
     * agent passes only a bare command (argc==2) this is 0 args; the helper
     * and argv_pack both handle n<=0 without dereferencing anything. */
    int nargs = argc-2;
    if(nargs<0) nargs=0;
    long st = spawn_wait(path, (const char*const*)&argv[2], nargs);

    if(st==-1){ out(2,"ERR spawn\n"); }     /* diagnostic -> fd 2, not the result channel */

    /* ONE-LINE OUTCOME -> fd 1 (the capability channel the model reads). */
    out(1,"SHELL "); out(1,cmd); out(1," exit="); out_d(1,st); out(1,"\n");
    return 0;
}
