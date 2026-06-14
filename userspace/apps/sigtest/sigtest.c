// sigtest -- SIG-FULL-0 (B8) proof. Self-contained (no libs, own _start, direct
// syscalls). Prints to fd 1 (serial). The smoke greps "SIGTEST RESULT: PASS".
//
// Proves: a user handler runs and returns cleanly, execution resumes after the
// kill, a BLOCKED signal does not deliver (but shows pending), and unblocking
// delivers it. (Default-action + bad-pointer fail-safe are exercised via a
// child so the parent survives to print the verdict.)

typedef unsigned long size_t;

#define SYS_EXIT            0
#define SYS_FORK            1
#define SYS_WRITE           3
#define SYS_WAITPID         6
#define SYS_GETPID          8
#define SYS_YIELD           15
#define SYS_KILL            26
#define SYS_RT_SIGACTION    107
#define SYS_RT_SIGPROCMASK  108
#define SYS_RT_SIGRETURN    109
#define SYS_SIGPENDING      110

#define SIGUSR1 10
#define SIGTERM 15
#define SIGCHLD 17

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

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }

static volatile int g_ran = 0;
static volatile int g_sig = 0;

static void handler(int sig) {
    g_ran++;
    g_sig = sig;
    out("SIGTEST:   >> handler entered\n");
}

static volatile int g_sigchld = 0;
static void chld_handler(int sig) {
    g_sigchld = sig;
    out("SIGTEST:   >> SIGCHLD handler entered\n");
}

// The handler returns HERE; issue SYS_RT_SIGRETURN to restore the interrupted
// context. naked => no prologue/epilogue (it never returns normally).
__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile("mov $109, %rax\n\t"
                     "syscall\n\t"
                     "ud2\n\t");
}

void _start(void) {
    out("SIGTEST: start\n");
    long pid = sc0(SYS_GETPID);
    int ok = 1;

    // Install a SIGUSR1 handler with our restorer.
    sc6(SYS_RT_SIGACTION, SIGUSR1, (long)&handler, (long)&sig_restorer, 0, 0);

    // --- Check 1: handler runs, returns cleanly, execution resumes ---
    out("SIGTEST: [1] raise SIGUSR1 to self\n");
    sc3(SYS_KILL, pid, SIGUSR1, 0);
    if (g_ran == 1 && g_sig == SIGUSR1) {
        out("SIGTEST: [1] PASS handler_ran=1 resumed=1\n");
    } else { out("SIGTEST: [1] FAIL\n"); ok = 0; }

    // --- Check 2: a BLOCKED signal does not deliver, but is pending ---
    {
        long set = (1UL << SIGUSR1);
        int before = g_ran;
        sc3(SYS_RT_SIGPROCMASK, 0 /*BLOCK*/, (long)&set, 0);
        sc3(SYS_KILL, pid, SIGUSR1, 0);
        long pend = sc0(SYS_SIGPENDING);
        if (g_ran == before && (pend & (1UL << SIGUSR1))) {
            out("SIGTEST: [2] PASS blocked_not_delivered=1 pending=1\n");
        } else { out("SIGTEST: [2] FAIL\n"); ok = 0; }

        // --- Check 3: unblocking delivers the pending signal ---
        sc3(SYS_RT_SIGPROCMASK, 1 /*UNBLOCK*/, (long)&set, 0);
        if (g_ran == before + 1) {
            out("SIGTEST: [3] PASS delivered_after_unblock=1\n");
        } else { out("SIGTEST: [3] FAIL\n"); ok = 0; }
    }

    // --- Check 4: default action (terminate) on an uncaught signal, in a child
    //     so the parent survives. Child installs NO handler; parent SIGTERMs it. ---
    {
        long cpid = sc3(SYS_FORK, 0, 0, 0);
        if (cpid == 0) {
            for (;;) sc0(SYS_YIELD);            // child: spin until signalled
        } else if (cpid > 0) {
            sc3(SYS_KILL, cpid, SIGTERM, 0);    // default action: terminate
            int status = 0;
            long w = 0;
            for (int t = 0; t < 2000000 && w != cpid; t++) {
                w = sc3(SYS_WAITPID, cpid, (long)&status, 0);
                if (w != cpid) sc0(SYS_YIELD);
            }
            if (w == cpid) out("SIGTEST: [4] PASS default_terminate_child=1\n");
            else { out("SIGTEST: [4] FAIL\n"); ok = 0; }
        }
    }

    // --- Check 5: a bad handler pointer fails SAFE (the child dies, kernel
    //     survives), again in a child. ---
    {
        long cpid = sc3(SYS_FORK, 0, 0, 0);
        if (cpid == 0) {
            sc6(SYS_RT_SIGACTION, SIGUSR1, 0x4000, (long)&sig_restorer, 0, 0); // bogus handler VA
            sc3(SYS_KILL, sc0(SYS_GETPID), SIGUSR1, 0);    // -> faults in ring 3
            for (;;) sc0(SYS_YIELD);
        } else if (cpid > 0) {
            int status = 0; long w = 0;
            for (int t = 0; t < 2000000 && w != cpid; t++) {
                w = sc3(SYS_WAITPID, cpid, (long)&status, 0);
                if (w != cpid) sc0(SYS_YIELD);
            }
            if (w == cpid) out("SIGTEST: [5] PASS bad_handler_killed_child_kernel_alive=1\n");
            else { out("SIGTEST: [5] FAIL\n"); ok = 0; }
        }
    }

    // --- Check 6: a terminating child raises SIGCHLD on the parent. Install a
    //     SIGCHLD handler, fork a child that exits immediately, reap it, and
    //     confirm the handler ran (SIGCHLD became pending on exit and delivered
    //     at the parent's next return-to-user). ---
    {
        g_sigchld = 0;
        sc6(SYS_RT_SIGACTION, SIGCHLD, (long)&chld_handler, (long)&sig_restorer, 0, 0);
        long cpid = sc3(SYS_FORK, 0, 0, 0);
        if (cpid == 0) {
            sc3(SYS_EXIT, 0, 0, 0);             // child: exit at once -> SIGCHLD
            for (;;) {}
        } else if (cpid > 0) {
            int status = 0; long w = 0;
            for (int t = 0; t < 2000000 && w != cpid; t++) {
                w = sc3(SYS_WAITPID, cpid, (long)&status, 0);
                if (w != cpid) sc0(SYS_YIELD);
            }
            // SIGCHLD is delivered at a return-to-user boundary; spin a few
            // syscalls to give it one if it has not arrived during the reap.
            for (int t = 0; t < 16 && !g_sigchld; t++) sc0(SYS_YIELD);
            if (w == cpid && g_sigchld == SIGCHLD) {
                out("SIGTEST: [6] PASS sigchld_on_child_exit=1\n");
            } else { out("SIGTEST: [6] FAIL\n"); ok = 0; }
        }
    }

    // --- Check 7 (SECURITY, P0): a NON-CANONICAL handler address must be
    //     REJECTED by sigaction. Otherwise it becomes the sysret RIP and #GPs in
    //     ring 0 on a user-controlled stack (CVE-2012-0217 class). 0x8000...0000
    //     is non-canonical (bit 63 set, bits 48-62 clear). Expect a negative
    //     (EINVAL) return; the legitimate handler is untouched. ---
    {
        long r = sc6(SYS_RT_SIGACTION, SIGUSR1, (long)0x8000000000000000UL,
                     (long)&sig_restorer, 0, 0);
        if (r < 0) out("SIGTEST: [7] PASS noncanonical_handler_rejected=1\n");
        else { out("SIGTEST: [7] FAIL\n"); ok = 0; }
    }

    out(ok ? "SIGTEST RESULT: PASS\n" : "SIGTEST RESULT: FAIL\n");
    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}
