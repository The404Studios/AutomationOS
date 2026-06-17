/* tool_cc -- TOOLSET-0 gated on-device compile. argv=[src.c, out.elf].
 * ==================================================================
 *
 * One GATED action for the agent rail (sbin/agentd): compile a C source file
 * the agent points at into an output ELF, using the on-device cc (/bin/cc).
 *
 * The model is HOSTILE TEXT, so both arguments are policed before anything is
 * spawned:
 *   - src : reject if it contains ".." (path traversal); the host gates this
 *           too, this is defense in depth.
 *   - out : must pass path_write_allowed() (no traversal, no protected system
 *           paths, only the scratch/project allowlist) -- we are creating a
 *           file at that path, so it is a WRITABLE path and must be gated.
 *
 * If both pass, spawn  bin/cc <src> -o <out>  and wait for it. The ONE-LINE
 * outcome goes to fd 1 (the capability channel the agent captures as RESULT):
 *   - "COMPILED <out>\n"  on exit==0
 *   - "CCFAIL <exit>\n"   otherwise (exit is cc's status, or a negative spawn
 *                         error from spawn_wait)
 * Gate rejections also print a one-line "DENY ..." to fd 1 and return 2, so the
 * model can read exactly why it was refused.
 *
 * FREESTANDING ring-3 ELF: NO libc, NO headers, crt0 supplies _start and calls
 * main(argc,argv). Single self-contained .c; the .o must have ZERO fs:0x28
 * references (the orchestrator gates this -- builds add -fno-stack-protector).
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified vs kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE          3
#define SYS_WAITPID        6
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
 * String / output helpers (fd1 = capability channel, fd2 = diagnostics).
 * --------------------------------------------------------------------- */
static unsigned slen(const char*s){unsigned n=0;while(s&&s[n])n++;return n;}
static void out(int fd,const char*s){sc(SYS_WRITE,fd,(long)s,(long)slen(s));}
/* Print a signed decimal directly to a fd (small numbers: exit codes). */
static void out_d(int fd,long v){
  char b[24]; int i=0; char t[24]; int j=0;
  int neg=(v<0); unsigned long u=neg?(unsigned long)(-v):(unsigned long)v;
  if(u==0) t[j++]='0';
  while(u){ t[j++]=(char)('0'+u%10); u/=10; }   /* const /10 -> no libgcc 64-bit div */
  if(neg) b[i++]='-';
  while(j) b[i++]=t[--j];
  b[i]=0; out(fd,b);
}

/* -----------------------------------------------------------------------
 * THE PATH GATE (copied verbatim from the ABI kit -- the model is hostile
 * text, so this exact policy guards every writable/destructive path).
 * --------------------------------------------------------------------- */
static int has_sub(const char*p,const char*s){for(int i=0;p[i];i++){int j=0;while(s[j]&&p[i+j]==s[j])j++;if(!s[j])return 1;}return 0;}
static int starts(const char*p,const char*pre){int j=0;while(pre[j]){if(p[j]!=pre[j])return 0;j++;}return 1;}
static int path_write_allowed(const char*p){
  if(!p||!p[0])return 0;
  if(has_sub(p,".."))return 0;
  if(starts(p,"/boot")||starts(p,"/sbin")||starts(p,"/bin")||starts(p,"/etc")||has_sub(p,"kernel"))return 0;
  if(starts(p,"/tmp")||starts(p,"/home")||starts(p,"/usr/src"))return 1;
  return 0; }

/* -----------------------------------------------------------------------
 * SPAWN + WAIT (copied verbatim from the ABI kit). Packs argv[1..] into a
 * NUL-separated buffer, spawns, waits, returns the child's exit status
 * (or -1 on spawn failure, -2 on waitpid failure).
 * --------------------------------------------------------------------- */
static int argv_pack(char*buf,int cap,const char*const*args,int n){int p=0;for(int i=0;i<n;i++){const char*a=args[i];for(int j=0;a[j]&&p<cap-1;j++)buf[p++]=a[j];if(p<cap)buf[p++]=0;}return p;}
static long spawn_wait(const char*path,const char*const*args,int nargs){char av[512];int al=argv_pack(av,512,args,nargs);long pid=sc6(SYS_SPAWN_EX_ARGV,(long)path,(long)av,al,0,0,0);if(pid<0)return -1;long st=0;long w=sc(SYS_WAITPID,pid,(long)&st,0);return w<0?-2:st;}

/* =======================================================================
 *  Entry point. crt0 supplies _start and calls main(argc,argv); the return
 *  value is fed to SYS_EXIT.
 * ======================================================================= */
int main(int argc,char**argv){
    /* Validate argv before any deref: need both src.c and out.elf. */
    if(argc<3 || !argv[1] || !argv[1][0] || !argv[2] || !argv[2][0]){
        out(2,"ERR usage tool_cc <src.c> <out.elf>\n");
        return 2;
    }
    const char* src = argv[1];
    const char* outp = argv[2];

    /* GATE 1: the source path. The model is HOSTILE TEXT and `src` is the FIRST
     * token we hand to cc, so a weak check here is a gate bypass:
     *   - a leading '-' turns `src` into a cc OPTION (e.g. "-o"), redirecting
     *     cc's write to an un-gated file and demoting the gated `outp` to the
     *     INPUT -- argument injection that defeats GATE 2 entirely;
     *   - an arbitrary absolute/system path lets cc READ a protected file
     *     (/etc/..., kernel...) and leak its contents back through cc's
     *     diagnostics on fd1.
     * So `src` must satisfy the SAME write-policy allowlist as `outp` (sources
     * legitimately live only in the scratch/project dirs the agent can write),
     * and must not start with '-'. This makes GATE 1 as strong as GATE 2. */
    if(src[0]=='-' || !path_write_allowed(src)){
        out(1,"DENY src "); out(1,src); out(1,"\n");
        return 2;
    }
    /* GATE 2: the output path -- must be a policy-allowed writable path, and
     * must not start with '-' (a '-o'-style token would be consumed by cc as an
     * option rather than the intended output file). */
    if(outp[0]=='-' || !path_write_allowed(outp)){
        out(1,"DENY policy "); out(1,outp); out(1,"\n");
        return 2;
    }

    /* Both gates passed: compile with the on-device cc (installed in /bin). */
    const char* cc_args[3] = { src, "-o", outp };
    long st = spawn_wait("bin/cc", cc_args, 3);

    if(st==0){
        out(1,"COMPILED "); out(1,outp); out(1,"\n");
        return 0;
    }
    out(1,"CCFAIL "); out_d(1,st); out(1,"\n");
    return 0;
}
