// forktest -- verifies SYS_FORK copy semantics (and, once CoW lands, that
// copy-on-write preserves address-space isolation). Self-contained: talks to
// the kernel directly via `syscall`, prints to fd 1 (serial), exits.
//
// What it proves:
//   * fork() returns 0 in the child and the child PID in the parent.
//   * After the child mutates both a .data buffer and a .bss buffer, the
//     PARENT's copies are UNCHANGED -> address spaces are isolated. This must
//     hold for both eager-copy fork and copy-on-write fork.
//
// The smoke harness greps the serial log for "FORKTEST RESULT: PASS".

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
static void outn(const char* s, size_t n) { syscall3(SYS_WRITE, 1, (long)s, (long)n); }

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

// One fork iteration: returns 1 if the parent's pages stayed isolated, 0 on a
// failure. Each iteration re-dirties the parent's pages first (which, under CoW,
// forces a copy-on-write of the previous iteration's shared frames), then forks,
// has the (silent) child mutate its copies and exit, waits, and checks that the
// PARENT's copies are untouched.
static int one_fork(void) {
    copy(data_buf, "PARENT-DATA-ORIGINAL");
    copy(bss_buf,  "PARENT-BSS-ORIGINAL");

    long pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        // child: dirty both shared pages (triggers CoW), then leave.
        copy(data_buf, "CHILD-DATA-CHANGED");
        copy(bss_buf,  "CHILD-BSS-CHANGED");
        syscall3(SYS_EXIT, 0, 0, 0);
        for (;;) {}
    } else if (pid > 0) {
        // waitpid() is non-blocking (returns 0 until the child has terminated),
        // so REAP by looping: yield to let the cooperative child run, retry until
        // waitpid returns the child's pid. Reaping each child before the next
        // fork is essential — otherwise zombies pile up and starve the scheduler.
        int status = 0;
        for (int tries = 0; tries < 1000000; tries++) {
            long w = syscall3(SYS_WAITPID, pid, (long)&status, 0);
            if (w == pid) break;               // child reaped
            syscall3(SYS_YIELD, 0, 0, 0);
        }
        return streq(data_buf, "PARENT-DATA-ORIGINAL") &&
               streq(bss_buf,  "PARENT-BSS-ORIGINAL");
    }
    return -1;  // fork error
}

#define FORK_ITERS 20

void _start(void) {
    out("FORKTEST: start (CoW isolation stress, 20 iterations)\n");

    int passes = 0;
    for (int it = 0; it < FORK_ITERS; it++) {
        int r = one_fork();
        if (r < 0) { out("FORKTEST RESULT: FAIL fork-returned-error\n");
                     syscall3(SYS_EXIT, 2, 0, 0); for (;;) {} }
        if (r) passes++;
    }

    // Final sanity print of the parent's (still-original) buffers.
    out("FORKTEST PARENT: final data='"); out(data_buf);
    out("' bss='"); out(bss_buf); out("'\n");

    if (passes == FORK_ITERS) {
        out("FORKTEST RESULT: PASS isolation-preserved (20/20 forks)\n");
        syscall3(SYS_EXIT, 0, 0, 0);
    } else {
        out("FORKTEST RESULT: FAIL isolation-broken\n");
        syscall3(SYS_EXIT, 1, 0, 0);
    }
    for (;;) {}
}
