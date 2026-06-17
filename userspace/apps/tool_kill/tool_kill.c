/* tool_kill -- TOOLSET-0 gated kill. argv=[pid].
 *
 * A single GATED action for the agent rail (sbin/agentd): send SIGTERM(15) to a
 * caller-named pid. The model is HOSTILE TEXT, so every input is validated and
 * a hard policy gate is applied BEFORE the syscall:
 *   - parse a strictly-decimal pid (any non-digit byte => reject)
 *   - GATE: pid>2 only. init (pid 1) and the very low system pids (<=2) are
 *     never signallable through this tool, even if they parse cleanly.
 *   - r = SYS_KILL(pid, SIGTERM, 0): r==0 => "KILL <pid>", else "ERR kill".
 *
 * FD CONVENTION: fd1 is the capability channel the agent CAPTURES as the RESULT
 * the model reads, so the ONE-LINE outcome -- success OR the validation/deny
 * reason -- is written to fd1 exactly as the spec requires (a denied/invalid
 * call is itself the observation the model must see). Return code: 0 on a
 * delivered signal, 2 on any reject (bad pid / gated pid).
 *
 * Freestanding ring 3 (NO libc, NO headers); crt0 provides _start and calls
 * main(argc,argv). Built with -fno-stack-protector => the .o must have ZERO
 * fs:0x28 references (the orchestrator gates this). */

#define SYS_WRITE  3
#define SYS_KILL  26   /* sc(26, pid, sig, 0) -> 0 ok / <0 err */

#define FD_OUT     1   /* capability channel: the agent reads this as RESULT */
#define SIGTERM   15

/* Verbatim 3-arg syscall wrapper (see ABI kit). */
static long sc(long n,long a,long b,long c){ long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }

static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void out(int fd,const char* s){ sc(SYS_WRITE,fd,(long)s,(long)slen(s)); }

/* Append an unsigned decimal to fd (small numbers only; const /10, no 64-bit div helper). */
static void out_u(int fd,unsigned long long v){ char b[24]; int i=0;
    if(!v){ out(fd,"0"); return; } while(v){ b[i++]=(char)('0'+v%10); v/=10; }
    char r[24]; int j=0; while(i) r[j++]=b[--i]; r[j]=0; out(fd,r); }

/* Strict decimal parse. Returns 0 and sets *ok=1 on a clean digit string;
 * any non-digit byte (or empty input) leaves *ok=0 and the value undefined. */
static unsigned long long parse_pid(const char* s,int* ok){
    *ok=0;
    if(!s || !s[0]) return 0;
    unsigned long long v=0;
    for(unsigned i=0; s[i]; i++){
        if(s[i] < '0' || s[i] > '9') return 0;   /* *ok stays 0 -> reject */
        v = v*10 + (unsigned long long)(s[i]-'0');
    }
    *ok=1;
    return v;
}

int main(int argc,char** argv){
    /* validate argc/argv before any deref (never touch a missing/empty argv) */
    if(argc<2 || !argv[1] || !argv[1][0]){ out(FD_OUT,"ERR pid\n"); return 2; }

    int ok=0;
    unsigned long long pid = parse_pid(argv[1],&ok);
    if(!ok){ out(FD_OUT,"ERR pid\n"); return 2; }

    /* THE GATE: pid>2 only. Deny init (1) and the low system pids (<=2). */
    if(pid<=2){ out(FD_OUT,"DENY kill pid "); out_u(FD_OUT,pid); out(FD_OUT,"\n"); return 2; }

    long r = sc(SYS_KILL,(long)pid,SIGTERM,0);
    if(r==0){ out(FD_OUT,"KILL "); out_u(FD_OUT,pid); out(FD_OUT,"\n"); return 0; }
    out(FD_OUT,"ERR kill\n");
    return 2;
}
