/* tool_spawn -- gated agent tool: spawn a persistent OS app (no wait).
 * ===================================================================
 *
 * FREESTANDING ring-3 ELF (NO libc, NO headers). crt0 provides _start and
 * calls main(argc,argv); we do NOT define _start. Single self-contained file.
 *
 * THE CONTRACT
 *   argv = [app].  This launches a *persistent* app (e.g. a daemon/GUI
 *   process), so we spawn it WITHOUT waiting for it to exit.
 *
 *   The model that drives the agent rail is HOSTILE TEXT, so `app` is gated
 *   before it ever reaches the kernel:
 *     - it MUST start with "sbin/"   (only shipped system apps are spawnable)
 *     - it MUST NOT contain ".."     (no path traversal out of sbin/)
 *   Anything else is denied; we never spawn it.
 *
 * FD CONVENTION
 *   fd 1 is the capability channel the agent (sbin/agentd) CAPTURES as the
 *   RESULT the model reads, so the ONE-LINE OUTCOME goes to fd 1:
 *       DENY spawn <app>          (gate rejected the request)   -> exit 2
 *       SPAWN <app> pid=<pid>     (spawned, not waited on)       -> exit 0
 *       ERR spawn                 (kernel refused the spawn)      -> exit 1
 *
 * Build (flags DIRECTLY on the command line so -fno-stack-protector survives;
 * the resulting .o MUST have ZERO fs:0x28 references -- the orchestrator gates
 * this):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/tool_spawn/tool_spawn.c -o tool_spawn.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       tool_spawn.o crt0.o -o build/tool_spawn
 *   objdump -d build/tool_spawn | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE          3
#define SYS_SPAWN_EX_ARGV  106   /* sc6(106, path, argvbuf, argvlen, 0,0,0) -> pid/<0 */

#define FD_OUT  1   /* capability channel: the agent reads this as the RESULT */

/* -----------------------------------------------------------------------
 * Syscall wrappers (copied verbatim from the ABI kit).
 * SYS_SPAWN_EX_ARGV needs all six register args, so we use sc6().
 * --------------------------------------------------------------------- */
static long sc(long n,long a,long b,long c){ long r;
  __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
static long sc6(long n,long a,long b,long c,long d,long e,long f){ long r;
  register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e; register long r9 __asm__("r9")=f;
  __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory"); return r; }

/* -----------------------------------------------------------------------
 * Freestanding helpers (string length, raw output, decimal output).
 * --------------------------------------------------------------------- */
static unsigned slen(const char*s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void out(int fd,const char*s){ sc(SYS_WRITE,fd,(long)s,(long)slen(s)); }

/* Print an unsigned decimal to fd. Built into a small char[] then emitted;
 * only const /10 division (avoids unlinked 64-bit libgcc divide helpers). */
static void out_u(int fd,unsigned long long v){
  char tmp[24]; int i=0;
  if(v==0){ out(fd,"0"); return; }
  while(v && i<(int)sizeof(tmp)){ tmp[i++]=(char)('0'+(int)(v%10ULL)); v/=10ULL; }
  char rev[24]; int j=0;
  while(i) rev[j++]=tmp[--i];
  rev[j]=0;
  out(fd,rev);
}

/* -----------------------------------------------------------------------
 * THE PATH GATE (model = hostile text). Verbatim primitives from the kit.
 *   has_sub : substring search   -- catches ".." anywhere in the string
 *   starts  : prefix test        -- requires the "sbin/" prefix
 * --------------------------------------------------------------------- */
static int has_sub(const char*p,const char*s){ for(int i=0;p[i];i++){ int j=0; while(s[j]&&p[i+j]==s[j]) j++; if(!s[j]) return 1; } return 0; }
static int starts(const char*p,const char*pre){ int j=0; while(pre[j]){ if(p[j]!=pre[j]) return 0; j++; } return 1; }

/* spawn_allowed -- the spawn policy for this tool. An app may be launched ONLY
 * if it lives under sbin/ and contains no ".." traversal. Returns 1=allow. */
static int spawn_allowed(const char*app){
  if(!app||!app[0])        return 0;   /* missing/empty app */
  if(!starts(app,"sbin/")) return 0;   /* only shipped system apps */
  if(has_sub(app,".."))    return 0;   /* no path traversal */
  return 1;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0 parses argc/argv off the kernel-prepared stack and calls main; its
 *  return value is fed to SYS_EXIT. We validate argv before any deref.
 * ======================================================================= */
int main(int argc,char** argv){
  /* Validate argv before touching it (never deref a missing/empty arg). */
  if(argc<2 || !argv[1] || !argv[1][0]){
    out(FD_OUT,"DENY spawn \n");
    return 2;
  }

  const char* app = argv[1];

  /* GATE: deny anything not under sbin/ or containing traversal. The denial
   * line is the captured RESULT, so it goes to fd 1. */
  if(!spawn_allowed(app)){
    out(FD_OUT,"DENY spawn "); out(FD_OUT,app); out(FD_OUT,"\n");
    return 2;
  }

  /* Spawn WITHOUT waiting: this is a persistent app, so we fire-and-report.
   * An empty argv buffer (len 0) means the child gets no argv[1..]. */
  long pid = sc6(SYS_SPAWN_EX_ARGV,(long)app,(long)"",0,0,0,0);

  if(pid>0){
    out(FD_OUT,"SPAWN "); out(FD_OUT,app); out(FD_OUT," pid="); out_u(FD_OUT,(unsigned long long)pid); out(FD_OUT,"\n");
    return 0;
  }

  out(FD_OUT,"ERR spawn\n");
  return 1;
}
