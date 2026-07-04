/*
 * negrsp.c -- KERNEL-ROBUST-0 NEGATIVE regression proof (bug "negrsp").
 * ========================================================================
 * Bug (pre-fix, kernel/core/signal/kill.c deliver_pending_signals): the saved
 * syscall frame's user_rsp was validated only for non-zero, never checked to
 * be a canonical user VA. An RSP near 2^64 let the CoW pre-resolve loop
 * ("for (pg = lo; ; pg += 0x1000) ... if (pg >= hi) break;") wrap 64-bit and
 * spin forever with IF=0 -- a PERMANENT core hang (nothing else can ever run,
 * not even the timer interrupt, because IF=0).
 *
 * Fix (at HEAD): deliver_pending_signals now requires user_rsp itself to be
 * sig_va_user() (< 0x0000800000000000, non-zero) before touching it; a bad
 * one fails SAFE to the default action (process terminated), and the loop
 * is wrap-safe besides.
 *
 * Test strategy (mirrors sigtest.c's "fork a child that does something fatal
 * so the parent survives to print the verdict" idiom):
 *
 *   child:
 *     1. rt_sigaction(SIGUSR1, valid_handler, valid_restorer)   -- both
 *        canonical user VAs, so ONLY user_rsp is the variable under test.
 *     2. rt_sigprocmask(BLOCK, {SIGUSR1})                       -- mask it.
 *     3. kill(getpid, SIGUSR1)                                  -- now
 *        pending-but-masked (deliver_pending_signals sees nothing to do).
 *     4. PURE INLINE ASM, no C in between: load all SYS_RT_SIGPROCMASK
 *        UNBLOCK argument registers, THEN clobber rsp to 0xFFFFFFFFFFFFFFFF
 *        (non-canonical), THEN `syscall` immediately. The `syscall`
 *        instruction itself does not need a valid user stack (entry switches
 *        to the kernel stack via the LSTAR mechanism) -- it is only the
 *        POST-dispatch deliver_pending_signals() path, using the RSP value
 *        syscall.asm captured at syscall entry, that is exposed to user_rsp.
 *        Unblocking makes SIGUSR1 deliverable at that exact post-dispatch
 *        point, with user_rsp == ~0.
 *
 *        FIXED kernel:  user_rsp fails sig_va_user() -> signal_default_action
 *                        -> child TERMINATED (never resumes to user code, so
 *                        it never touches its wrecked stack). The child's
 *                        syscall does not return to our C frame either way.
 *        BUGGY kernel:  the CoW pre-resolve loop wraps and spins forever with
 *                        IF=0. The ENTIRE machine (single ring-3 CPU on this
 *                        path) freezes permanently -- no further instruction,
 *                        including the parent's, ever executes again.
 *
 *   parent:
 *     waits (bounded, yielding) for the child via SYS_WAITPID.
 *       FIXED:  waitpid returns promptly (child died on SIGUSR1) -> PASS.
 *       BUGGY:  the whole VM is wedged (IF=0 spin) before the parent even
 *               gets scheduled again -- NEGRSP: PASS is never printed, and
 *               nothing spawned after negrsp in init's list runs either.
 *               scripts/smoke_boot.sh's outer `timeout` kills the wedged
 *               QEMU; the grep for the PASS marker then correctly reports
 *               the regression (hang == detected regression, the bug's own
 *               signature -- there is no softer failure mode to catch here,
 *               by construction: IF=0 means even the PIT/LAPIC tick that
 *               would otherwise let a watchdog intervene cannot fire).
 *
 * Discriminates fixed-vs-buggy: the ONLY variable under test is user_rsp's
 * canonicality. handler and restorer are both valid, so on the fixed kernel
 * this exercises exactly the "user_rsp non-canonical -> default action" path
 * added by the fix (not the pre-existing handler/restorer canonical checks,
 * which existed before the fix too).
 * ======================================================================== */

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

#define SIGUSR1 10

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

/* Valid handler: never actually meant to run (user_rsp is what fails), but
 * must be a canonical VA so it alone can't trigger the default action. */
static void handler(int sig) {
    (void)sig;
    out("NEGRSP:   >> handler unexpectedly entered (should never happen)\n");
}

/* Valid restorer, naked (SYS_RT_SIGRETURN), same idiom as sigtest.c. */
__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile("mov $109, %rax\n\t"
                     "syscall\n\t"
                     "ud2\n\t");
}

/* Mask/set operand for the UNBLOCK call. Must be a GLOBAL (not on-stack)
 * address: by the time the kernel's copy_from_user reads *set, the child's
 * user RSP is already garbage, but `set` is an independent pointer into
 * static data, which is unaffected by the RSP clobber. */
static volatile unsigned long g_unblock_set = (1UL << SIGUSR1);

/* The delicate part: load every SYS_RT_SIGPROCMASK(UNBLOCK) argument into its
 * ABI register, THEN clobber rsp, THEN `syscall` -- with NOTHING else (no C,
 * no stack access) between the rsp clobber and the syscall instruction. All
 * operand setup (the address-of g_unblock_set) is resolved by the compiler
 * into a register BEFORE this asm block's instructions execute, i.e. while
 * rsp is still valid, so it never needs the stack once inside. rax/rdi/rsi/
 * rdx are clobbered explicitly with immediates/the operand -- listing them
 * (along with rcx/r11, clobbered by `syscall` itself, and memory) in the
 * clobber list forces the compiler to keep the %0 operand in a DIFFERENT
 * register, so there is no allocation conflict. */
__attribute__((noreturn, noinline)) static void clobber_rsp_then_unblock(void) {
    unsigned long set_addr = (unsigned long)&g_unblock_set;
    __asm__ volatile(
        "mov $108, %%rax\n\t"                      /* SYS_RT_SIGPROCMASK   */
        "mov $1, %%rdi\n\t"                         /* how = UNBLOCK        */
        "mov %0, %%rsi\n\t"                         /* set = &g_unblock_set */
        "xor %%rdx, %%rdx\n\t"                      /* oldset = NULL        */
        "movabs $0xFFFFFFFFFFFFFFFF, %%rsp\n\t"     /* NON-CANONICAL rsp    */
        "syscall\n\t"                               /* immediately -- no C  */
        :
        : "r"(set_addr)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    /* FIXED kernel: default_action terminates us before any code after
     * `syscall` could run. BUGGY kernel: never gets here (permanent hang in
     * the kernel). Either way this must not fall through to C on a wrecked
     * stack; trap if it somehow does. */
    __asm__ volatile("ud2");
    __builtin_unreachable();
}

void _start(void) {
    out("NEGRSP: start\n");
    long pid = sc0(SYS_GETPID);

    long cpid = sc3(SYS_FORK, 0, 0, 0);
    if (cpid == 0) {
        /* --- child: set up a fully valid handler/restorer so ONLY user_rsp
         * is under test, mask+raise SIGUSR1 so it is pending, then wreck its
         * own rsp immediately before the syscall that makes it deliverable. */
        long cp = sc0(SYS_GETPID);
        sc6(SYS_RT_SIGACTION, SIGUSR1, (long)&handler, (long)&sig_restorer, 0, 0);
        {
            unsigned long block_set = (1UL << SIGUSR1);
            sc3(SYS_RT_SIGPROCMASK, 0 /*BLOCK*/, (long)&block_set, 0);
        }
        sc3(SYS_KILL, cp, SIGUSR1, 0);   /* pending, masked -- not delivered yet */
        clobber_rsp_then_unblock();      /* -> deliverable with user_rsp == ~0   */
        for (;;) { }                     /* unreachable */
    } else if (cpid > 0) {
        /* --- parent: bounded, yielding wait. On the FIXED kernel the child
         * is terminated by the default action and this returns promptly. On
         * the BUGGY kernel the whole machine wedges (IF=0 spin) before we
         * ever get scheduled again, so this loop (and the PASS print below)
         * never executes -- the outer smoke harness's timeout is what
         * ultimately reports the regression. */
        int status = 0;
        long w = 0;
        for (int t = 0; t < 2000000 && w != cpid; t++) {
            w = sc3(SYS_WAITPID, cpid, (long)&status, 0);
            if (w != cpid) sc0(SYS_YIELD);
        }
        if (w == cpid) {
            out("NEGRSP: PASS noncanonical_user_rsp_defaulted_child_terminated=1\n");
            sc3(SYS_EXIT, 0, 0, 0);
        } else {
            out("NEGRSP: FAIL waitpid_never_returned (child stuck?)\n");
            sc3(SYS_EXIT, 1, 0, 0);
        }
    } else {
        out("NEGRSP: FAIL fork\n");
        sc3(SYS_EXIT, 1, 0, 0);
    }
    (void)pid;
    for (;;) { }
}
