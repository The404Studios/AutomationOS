// forktest -- SYS_FORK copy/CoW semantics + FORK-PROOF-GAPS-0 additions.
// Self-contained: talks to the kernel directly via `syscall`, prints to fd 1
// (serial), exits. The smoke greps "FORKTEST RESULT: PASS".
//
// What it proves:
//   * fork() returns 0 in the child and the child PID in the parent.
//   * CoW isolation, BOTH directions:
//       - child mutates .data + .bss; the PARENT's copies stay UNCHANGED
//         (parent isolation -- original test).
//       - the child first VERIFIES it inherited the parent's pre-fork values
//         (it can READ the CoW-shared pages), THEN writes its OWN values and
//         reads them back (it can WRITE its private copy independently)
//         [fork_child_cow_write].
//   * fork_wait_sigchld_still_ok: a child that exits with a DISTINCTIVE status
//     wakes a parent blocked in waitpid, which reads back exactly that status --
//     i.e. the child-exit -> parent-wake path is intact after the new fork ABI.

typedef unsigned long size_t;

#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_WRITE   3
#define SYS_WAITPID 6
#define SYS_GETPID  8
#define SYS_YIELD   15

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { syscall3(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void outnum(long v) {
    char buf[24]; char* p = buf + 23; *p = 0;
    if (v < 0) { out("-"); v = -v; }
    if (v == 0) { *--p = '0'; }
    else { while (v > 0) { *--p = (char)('0' + (v % 10)); v /= 10; } }
    out(p);
}

// A pre-initialized buffer (lands in .data, a writable RW segment page) and a
// zero buffer (lands in .bss, lazily-faulted anon page). CoW must isolate both.
static char data_buf[32] = "PARENT-DATA-ORIGINAL";
static char bss_buf[32];

static int streq(const char* a, const char* b) {
    size_t i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static void copy(char* dst, const char* src) {
    size_t i = 0; for (; src[i]; i++) dst[i] = src[i]; dst[i] = 0;
}

// One fork iteration: returns 1 if isolation held BOTH ways, 0 on a failure,
// -1 on fork error. Re-dirties the parent's pages first (forces CoW of the prior
// iteration's shared frames), forks, the child verifies inheritance + its own
// writes (exit 0 iff both), the parent reaps and checks the child's status AND
// that the parent's own copies are untouched.
static int one_fork(void) {
    copy(data_buf, "PARENT-DATA-ORIGINAL");
    copy(bss_buf,  "PARENT-BSS-ORIGINAL");

    long pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        // child: prove it INHERITED the parent's pre-fork copies (reads the
        // CoW-shared pages), then write its OWN values + read them back (proves
        // an independent private write). exit 0 iff both hold.
        int inherited = streq(data_buf, "PARENT-DATA-ORIGINAL") &&
                        streq(bss_buf,  "PARENT-BSS-ORIGINAL");
        copy(data_buf, "CHILD-DATA-CHANGED");
        copy(bss_buf,  "CHILD-BSS-CHANGED");
        int own_write = streq(data_buf, "CHILD-DATA-CHANGED") &&
                        streq(bss_buf,  "CHILD-BSS-CHANGED");
        syscall3(SYS_EXIT, (inherited && own_write) ? 0 : 7, 0, 0);
        for (;;) {}
    } else if (pid > 0) {
        // waitpid() is non-blocking here (returns 0 until the child terminates),
        // so REAP by looping + yielding. Reaping each child before the next fork
        // is essential -- otherwise zombies pile up and starve the scheduler.
        int status = -1;
        long reaped = -1;
        for (int tries = 0; tries < 2000000; tries++) {
            long w = syscall3(SYS_WAITPID, pid, (long)&status, 0);
            if (w == pid) { reaped = w; break; }
            syscall3(SYS_YIELD, 0, 0, 0);
        }
        return reaped == pid && status == 0 &&
               streq(data_buf, "PARENT-DATA-ORIGINAL") &&
               streq(bss_buf,  "PARENT-BSS-ORIGINAL");
    }
    return -1;  // fork error
}

// fork_wait_sigchld_still_ok: fork a child that exits with a DISTINCTIVE code;
// the parent blocks/polls in waitpid and must be woken with exactly that status.
#define WAIT_MAGIC 42
static int wait_status_test(void) {
    long pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) return -1;
    if (pid == 0) { syscall3(SYS_EXIT, WAIT_MAGIC, 0, 0); for (;;) {} }
    int status = -1; long w = -1;
    for (int t = 0; t < 2000000 && w != pid; t++) {
        w = syscall3(SYS_WAITPID, pid, (long)&status, 0);
        if (w != pid) syscall3(SYS_YIELD, 0, 0, 0);
    }
    return (w == pid && status == WAIT_MAGIC) ? 1 : 0;
}

#define FORK_ITERS 20

void _start(void) {
    out("FORKTEST: start (CoW isolation both ways, 20 iters + wait/status)\n");

    int passes = 0;
    for (int it = 0; it < FORK_ITERS; it++) {
        int r = one_fork();
        if (r < 0) { out("FORKTEST RESULT: FAIL fork-returned-error\n");
                     syscall3(SYS_EXIT, 2, 0, 0); for (;;) {} }
        if (r) passes++;
    }

    int wait_ok = wait_status_test();

    out("FORKTEST PARENT: final data='"); out(data_buf);
    out("' bss='"); out(bss_buf); out("'\n");
    out("FORKTEST: cow_both_ways="); outnum(passes);
    out("/"); outnum(FORK_ITERS);
    out(" wait_status_ok="); outnum(wait_ok); out("\n");

    if (passes == FORK_ITERS && wait_ok == 1) {
        out("FORKTEST RESULT: PASS isolation-preserved (20/20 forks) + wait/status\n");
        syscall3(SYS_EXIT, 0, 0, 0);
    } else {
        out("FORKTEST RESULT: FAIL\n");
        syscall3(SYS_EXIT, 1, 0, 0);
    }
    for (;;) {}
}
