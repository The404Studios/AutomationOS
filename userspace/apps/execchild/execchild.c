/* execchild -- the POST-execve image for the EXECVE-INPLACE-0 proof.
 * ==================================================================
 *
 * This program is REACHED ONLY VIA execve(): exectest forks, the child calls
 *   execve("execchild", {"execchild","ARG1MAGIC",NULL}, {"EXECVAR=ENVMAGIC",NULL})
 * and -- if in-place execve works -- this image replaces the child's image IN
 * THE SAME PID. We therefore PROVE the handoff by checking, from inside this
 * fresh image, that:
 *
 *   1. argc == 2                                   (argc_ok)
 *   2. argv[0] ends with "execchild"               (argv0_ok)
 *   3. argv[1] == "ARG1MAGIC"                       (argv1_ok)
 *   4. getenv("EXECVAR") == "ENVMAGIC"              (envp_ok)
 *
 * and we record our POST-exec pid (must equal the fork-returned pid the parent
 * saw -- C1 "same_pid") plus our parent_pid (decision 6: assert it == exectest's
 * pid) and the four ok-flags into /tmp/exec_pid_after.bin (offset-0, O_TRUNC, no
 * lseek). exectest reads that file back to score C1/C4 and exits 77 here so the
 * parent's waitpid reads status==77 (C2).
 *
 * LINKAGE: linked WITH crt0.o, so the entry is crt0's _start which calls
 *   int main(int argc, char** argv, char** envp);
 * HARD DEPENDENCY: this relies on the crt0 patch (EXECVE-INPLACE-0 plan
 * Step 6) that loads envp into RDX before `call main` -- the UNPATCHED crt0
 * only sets RDI=argc / RSI=argv, leaving RDX (envp) undefined. Without that
 * patch envp_ok is garbage. The kernel argv frame already emits
 *   [argc][argv0..][NULL][envp0..][NULL]
 * so envp == &argv[argc+1] when crt0 forwards it.
 *
 * FAIL-PATH NOTE: execchild is NOT the actor on the C5 fail path. There,
 * exectest's child calls execve("/no/such",...); the execve RETURNS < 0 and the
 * child's ORIGINAL (exectest) image survives, prints "EXECCHILD-FAILPATH:
 * survived" and exits 88. That proof lives in exectest.c -- execchild is never
 * loaded on the fail path (which is the point: a failed execve must not destroy
 * the caller). This file documents that contract; it owns only the success path.
 *
 * FREESTANDING: no libc (getenv/strcmp/etc. are local). Pure inline syscalls.
 */

typedef unsigned long  size_t;
typedef unsigned int   u32;
typedef unsigned char  u8;

/* Syscall numbers (verified against kernel/include/syscall.h). */
#define SYS_EXIT      0
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_GETPID    8
#define SYS_PROCLIST  44

/* open() flags (verified against kernel/include/vfs.h). */
#define O_WRONLY   0x0001
#define O_CREAT    0x0040
#define O_TRUNC    0x0200

#define FD_STDOUT  1

/* SYS_PROCLIST 64-byte record: pid @ off 0, parent_pid @ off 4 (sched.h
 * proc_info_t; mirrored as ps.c procinfo_t). We only read the first 8 bytes. */
#define PROC_REC_STRIDE  64
#define MAX_PROCS        128

/* Proof file written for exectest. Self-describing fixed binary layout, offset 0:
 *   off  0: u32 magic       = EXEC_PROOF_MAGIC
 *   off  4: u32 post_pid    = our pid AFTER execve (C1 same_pid)
 *   off  8: u32 parent_pid  = our parent's pid      (decision 6)
 *   off 12: u8  argc_ok
 *   off 13: u8  argv0_ok
 *   off 14: u8  argv1_ok
 *   off 15: u8  envp_ok
 *   off 16: u32 all_ok      = argc_ok & argv0_ok & argv1_ok & envp_ok
 * Total 20 bytes. */
#define EXEC_PROOF_PATH   "/tmp/exec_pid_after.bin"
#define EXEC_PROOF_MAGIC  0xEC0C0DE5u
#define EXEC_PROOF_LEN    20

/* ---- raw syscalls (need up to 4 args; SYS_OPEN takes path,flags,mode) ------ */
static inline long sc3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc1(n,a)   sc3((n),(a),0,0)
#define sc0(n)     sc3((n),0,0,0)

/* ---- freestanding string/number helpers ----------------------------------- */
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, FD_STDOUT, (long)s, (long)slen(s)); }
static void outnum(long v) {
    char buf[24]; char* p = buf + 23; *p = 0;
    if (v < 0) { out("-"); v = -v; }
    if (v == 0) { *--p = '0'; }
    else { while (v > 0) { *--p = (char)('0' + (v % 10)); v /= 10; } }
    out(p);
}

/* str_eq: 1 iff a and b are equal NUL-terminated strings. */
static int str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    size_t i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];   /* both hit '\0' together */
}

/* ends_with: 1 iff string s ends with suffix suf (NUL-terminated). Used so the
 * argv0 check tolerates either "execchild" or a pathful "sbin/execchild". */
static int ends_with(const char* s, const char* suf) {
    if (!s || !suf) return 0;
    size_t ls = slen(s), lf = slen(suf);
    if (lf > ls) return 0;
    const char* tail = s + (ls - lf);
    for (size_t i = 0; i < lf; i++) if (tail[i] != suf[i]) return 0;
    return 1;
}

/* getenv: scan the SysV envp[] vector ("KEY=VALUE", NULL-terminated) and return
 * a pointer to the VALUE for `key`, or 0 if absent. Local impl -- the freestanding
 * libc here has no getenv. */
static const char* my_getenv(char** envp, const char* key) {
    if (!envp || !key) return 0;
    size_t kl = slen(key);
    for (int i = 0; envp[i]; i++) {
        const char* e = envp[i];
        size_t j = 0;
        while (j < kl && e[j] && e[j] == key[j]) j++;
        if (j == kl && e[j] == '=') return e + kl + 1;   /* point past '=' */
    }
    return 0;
}

/* getppid_via_proclist: there is no SYS_GETPPID, so look ourselves up in the
 * SYS_PROCLIST snapshot and read parent_pid (offset 4 of our 64-byte record).
 * Returns parent_pid, or 0 if we are not found / call failed. */
static u32 getppid_via_proclist(u32 my_pid) {
    static u8 recs[MAX_PROCS * PROC_REC_STRIDE];
    long n = sc3(SYS_PROCLIST, (long)recs, MAX_PROCS, 0);
    if (n < 0) return 0;
    if (n > MAX_PROCS) n = MAX_PROCS;
    for (long i = 0; i < n; i++) {
        u8* r = recs + (size_t)i * PROC_REC_STRIDE;
        u32 pid = *(u32*)(r + 0);
        if (pid == my_pid) return *(u32*)(r + 4);
    }
    return 0;
}

/* store little-endian u32 */
static void put_u32(u8* p, u32 v) { p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }

int main(int argc, char** argv, char** envp) {
    long pid = sc0(SYS_GETPID);

    out("EXECCHILD: start pid=");
    outnum(pid);
    out("\n");

    /* ---- the four claims --------------------------------------------------- */
    int argc_ok  = (argc == 2);
    int argv0_ok = (argv && argv[0] && ends_with(argv[0], "execchild"));
    int argv1_ok = (argc >= 2 && argv && argv[1] && str_eq(argv[1], "ARG1MAGIC"));
    const char* ev = my_getenv(envp, "EXECVAR");
    int envp_ok  = (ev && str_eq(ev, "ENVMAGIC"));

    /* ---- write the proof record (offset 0, O_TRUNC -> fresh, no lseek) ------ */
    u32 ppid = getppid_via_proclist((u32)pid);
    {
        u8 rec[EXEC_PROOF_LEN];
        put_u32(rec + 0,  EXEC_PROOF_MAGIC);
        put_u32(rec + 4,  (u32)pid);
        put_u32(rec + 8,  ppid);
        rec[12] = (u8)(argc_ok  ? 1 : 0);
        rec[13] = (u8)(argv0_ok ? 1 : 0);
        rec[14] = (u8)(argv1_ok ? 1 : 0);
        rec[15] = (u8)(envp_ok  ? 1 : 0);
        put_u32(rec + 16, (u32)((argc_ok && argv0_ok && argv1_ok && envp_ok) ? 1 : 0));

        long fd = sc3(SYS_OPEN, (long)EXEC_PROOF_PATH,
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 3) {
            sc3(SYS_WRITE, fd, (long)rec, EXEC_PROOF_LEN);
            sc1(SYS_CLOSE, fd);
        } else {
            out("EXECCHILD: WARN could not open proof file\n");
        }
    }

    /* ---- human-readable result line (the smoke also greps these flags) ----- */
    out("EXECCHILD: argc_ok=");  outnum(argc_ok);
    out(" argv0_ok=");           outnum(argv0_ok);
    out(" argv1_ok=");           outnum(argv1_ok);
    out(" envp_ok=");            outnum(envp_ok);
    out(" ppid=");               outnum((long)ppid);
    out("\n");

    /* C2: exit 77 so the parent's waitpid reads status==77. */
    sc1(SYS_EXIT, 77);
    for (;;) {}
    return 77;   /* unreachable; crt0 would route a return to SYS_EXIT too */
}
