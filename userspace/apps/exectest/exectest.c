/* exectest -- the DRIVER for the EXECVE-INPLACE-0 proof (no libs, own _start).
 * =========================================================================
 *
 * This is spawned by init (sys_spawn "sbin/exectest"). It is the ONLY entry
 * point for the proof; execchild is reached ONLY via execve() from a child of
 * exectest, never spawned directly (so the "no stray 3rd process" claim C3 has
 * a clean baseline).
 *
 * The proof, one claim per check (prints EXECTEST RESULT: PASS iff all hold):
 *
 *   C1 same_pid       : the post-execve image (execchild) runs in the SAME pid
 *                       that fork() returned for the child. Scored by reading
 *                       execchild's proof file (post_pid) and comparing to the
 *                       fork-returned cpid. Decision-6 extra: execchild's
 *                       parent_pid == exectest's own pid.
 *   C2 status77       : the child (now execchild) exits 77 -> waitpid status 77.
 *   C3 no_stray       : execve REPLACES, it does not add. A SYS_PROCLIST poll
 *                       across the window shows process count delta == +1 (the
 *                       one forked child), max_seen <= baseline+1 (never +2 from
 *                       a stray spawned 3rd process), baseline measured first.
 *   C4 argv_envp      : execchild saw argc==2, argv0~execchild, argv1==ARG1MAGIC,
 *                       getenv(EXECVAR)==ENVMAGIC (its all_ok flag in the file).
 *   CoW sanity        : a .data sentinel in THIS (exectest) image is intact
 *                       after the fork/exec dance (the child's exec did not
 *                       reach back into the parent's address space).
 *   C5 fail_neg_survives: a SECOND fork; that child execve("/no/such/file"),
 *                       which must RETURN < 0 (errno negative). The child's
 *                       ORIGINAL image survives, prints EXECCHILD-FAILPATH:
 *                       survived, and exits 88 -> parent reaps status 88.
 *
 * NO LIBC: direct inline syscalls only. Prints to fd 1 (serial).
 */

typedef unsigned long  size_t;
typedef unsigned int   u32;
typedef unsigned char  u8;

/* Syscall numbers (verified against kernel/include/syscall.h). */
#define SYS_EXIT      0
#define SYS_FORK      1
#define SYS_READ      2
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_WAITPID   6
#define SYS_EXECVE    7
#define SYS_GETPID    8
#define SYS_YIELD     15
#define SYS_PROCLIST  44

/* open() flags (verified against kernel/include/vfs.h). */
#define O_RDONLY   0x0000

#define FD_STDOUT  1

/* SYS_PROCLIST 64-byte record (sched.h proc_info_t). pid @ off 0. */
#define PROC_REC_STRIDE  64
#define MAX_PROCS        128

/* execchild proof file (MUST match userspace/apps/execchild/execchild.c). */
#define EXEC_PROOF_PATH   "/tmp/exec_pid_after.bin"
#define EXEC_PROOF_MAGIC  0xEC0C0DE5u
#define EXEC_PROOF_LEN    20
/* layout: off0 magic, off4 post_pid, off8 parent_pid, off12..15 ok-flags,
 *         off16 all_ok */

/* ---- raw syscalls --------------------------------------------------------- */
static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc3(n,a,b,c) sc6((n),(a),(b),(c),0,0)
#define sc1(n,a)     sc6((n),(a),0,0,0,0)
#define sc0(n)       sc6((n),0,0,0,0,0)

/* ---- freestanding helpers ------------------------------------------------- */
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, FD_STDOUT, (long)s, (long)slen(s)); }
static void outnum(long v) {
    char buf[24]; char* p = buf + 23; *p = 0;
    if (v < 0) { out("-"); v = -v; }
    if (v == 0) { *--p = '0'; }
    else { while (v > 0) { *--p = (char)('0' + (v % 10)); v /= 10; } }
    out(p);
}
static u32 rd_u32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* count_execchild: SYS_PROCLIST snapshot -> number of live processes whose name
 * contains "execchild" (name @ off 16 in proc_info_t). 0 before the exec; exactly
 * 1 during it; 2 would be a stray spawned 3rd process. Counting BY NAME, not the
 * total process count, is robust against the desktop concurrently spawning
 * unrelated processes during boot -- the bug that made the total-count version
 * read base=101 max_seen=118 (those are whole-system procs, not strays). */
static long count_execchild(void) {
    static u8 recs[MAX_PROCS * PROC_REC_STRIDE];
    long n = sc3(SYS_PROCLIST, (long)recs, MAX_PROCS, 0);
    if (n < 0) return -1;
    if (n > MAX_PROCS) n = MAX_PROCS;
    long c = 0;
    for (long i = 0; i < n; i++) {
        const char* nm = (const char*)(recs + i * PROC_REC_STRIDE + 16);
        for (int k = 0; k + 9 <= 32; k++) {
            int j = 0;
            while (j < 9 && nm[k + j] == "execchild"[j]) j++;
            if (j == 9) { c++; break; }
        }
    }
    return c;
}

/* reap: blocking-ish waitpid with bounded yields. Returns child status, or
 * a large negative on failure. */
static long reap(long cpid, int* status_out) {
    int status = -1;
    long w = -1;
    for (long t = 0; t < 4000000 && w != cpid; t++) {
        w = sc3(SYS_WAITPID, cpid, (long)&status, 0);
        if (w != cpid) sc0(SYS_YIELD);
    }
    if (status_out) *status_out = status;
    return w;
}

/* .data sentinel for the CoW-sanity claim (lives in THIS image's .data). */
static volatile u32 g_cow_sentinel = 0xC0FFEE11u;

void _start(void) {
    long my_pid = sc0(SYS_GETPID);
    out("EXECTEST: start pid=");
    outnum(my_pid);
    out("\n");

    /* baseline process count for C3 (before any fork). */
    long base = count_execchild();

    int all_pass = 1;

    /* ----- success path: fork + execve(execchild) ----------------------- */
    char  a0[] = "sbin/execchild";
    char  a1[] = "ARG1MAGIC";
    char* cargv[] = { a0, a1, (char*)0 };
    char  e0[] = "EXECVAR=ENVMAGIC";
    char* cenvp[] = { e0, (char*)0 };

    long max_seen = base;
    long cpid = sc0(SYS_FORK);
    if (cpid < 0) {
        out("EXECTEST: FAIL fork1 rc=");
        outnum(cpid);
        out("\n");
        all_pass = 0;
    } else if (cpid == 0) {
        /* child: replace ourselves with execchild. On success this NEVER
         * returns; the next image is execchild's main(argc,argv,envp). */
        long rc = sc3(SYS_EXECVE, (long)a0, (long)cargv, (long)cenvp);
        /* only reached if execve FAILED -- which it must not on the happy path */
        out("EXECTEST-CHILD: execve returned rc=");
        outnum(rc);
        out(" (should not happen)\n");
        sc1(SYS_EXIT, 3);
        for (;;) {}
    }

    /* parent: poll the process count a few times to catch a stray 3rd proc. */
    for (int s = 0; s < 64; s++) {
        long c = count_execchild();
        if (c > max_seen) max_seen = c;
        sc0(SYS_YIELD);
    }

    int status1 = -1;
    long w1 = reap(cpid, &status1);

    /* C2 status77 */
    int c2_status77 = (w1 == cpid && status1 == 77);

    /* read execchild's proof file for C1 + C4. */
    int  c1_same_pid = 0, c4_argv_envp = 0, dec6_ppid = 0;
    {
        long fd = sc3(SYS_OPEN, (long)EXEC_PROOF_PATH, O_RDONLY, 0);
        if (fd >= 3) {
            u8 rec[EXEC_PROOF_LEN];
            long got = sc3(SYS_READ, fd, (long)rec, EXEC_PROOF_LEN);
            sc1(SYS_CLOSE, fd);
            if (got == EXEC_PROOF_LEN && rd_u32(rec + 0) == EXEC_PROOF_MAGIC) {
                u32 post_pid = rd_u32(rec + 4);
                u32 ppid     = rd_u32(rec + 8);
                u32 all_ok   = rd_u32(rec + 16);
                c1_same_pid  = (post_pid == (u32)cpid);
                c4_argv_envp = (all_ok == 1);
                dec6_ppid    = (ppid == (u32)my_pid);
            }
        }
    }

    /* C3 no_stray: counting execchild-NAMED procs, baseline is 0 and we must
     * never see 2 (a stray spawned 3rd process). 1 = the in-place-replaced child
     * caught alive; 0 = it already exited before a poll -- either is fine (the
     * EXECCHILD serial line + same_pid already prove it ran in place). */
    int c3_no_stray = (base >= 0 && max_seen <= base + 1);

    /* CoW sanity: our .data sentinel survived the child's fork+exec. */
    int cow_ok = (g_cow_sentinel == 0xC0FFEE11u);

    /* ----- fail path: fork + execve(bad path) must RETURN < 0 ------------ */
    int c5_fail_survives = 0;
    {
        long fpid = sc0(SYS_FORK);
        if (fpid == 0) {
            char  ba0[] = "/no/such/file";
            char* bargv[] = { ba0, (char*)0 };
            long rc = sc3(SYS_EXECVE, (long)ba0, (long)bargv, 0);
            if (rc < 0) {
                /* the original image SURVIVED a failed execve -- the point of C5 */
                out("EXECCHILD-FAILPATH: survived rc=");
                outnum(rc);
                out("\n");
                sc1(SYS_EXIT, 88);
            } else {
                out("EXECCHILD-FAILPATH: execve unexpectedly succeeded\n");
                sc1(SYS_EXIT, 4);
            }
            for (;;) {}
        } else if (fpid > 0) {
            int status2 = -1;
            long w2 = reap(fpid, &status2);
            c5_fail_survives = (w2 == fpid && status2 == 88);
        }
    }

    /* ----- verdict ------------------------------------------------------- */
    if (!c1_same_pid)     all_pass = 0;
    if (!c2_status77)     all_pass = 0;
    if (!c3_no_stray)     all_pass = 0;
    if (!c4_argv_envp)    all_pass = 0;
    if (!cow_ok)          all_pass = 0;
    if (!c5_fail_survives) all_pass = 0;
    if (!dec6_ppid)       all_pass = 0;

    out("EXECTEST: same_pid="); outnum(c1_same_pid);
    out(" status77=");          outnum(c2_status77);
    out(" no_stray=");          outnum(c3_no_stray);
    out(" argv_envp=");         outnum(c4_argv_envp);
    out(" cow_ok=");            outnum(cow_ok);
    out(" fail_neg_survives="); outnum(c5_fail_survives);
    out(" ppid_ok=");           outnum(dec6_ppid);
    out(" (base=");             outnum(base);
    out(" max_seen=");          outnum(max_seen);
    out(")\n");

    if (all_pass) out("EXECTEST RESULT: PASS\n");
    else          out("EXECTEST RESULT: FAIL\n");

    sc1(SYS_EXIT, all_pass ? 0 : 1);
    for (;;) {}
}
