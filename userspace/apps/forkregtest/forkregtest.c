// forkregtest -- FORK-REGS-INHERIT-0 / FORK-PROOF-GAPS-0 proof. Verifies a
// fork()ed child resumes with the PARENT's FULL general-purpose register set
// intact -- not just rax=0, and not just rbx/r12-r15 (the original test's
// subset). The old bug was exactly "child inherits NO GP regs," so the proof
// must cover every inherited slot:
//   callee-saved : rbx, rbp, r12, r13, r14, r15  (a C caller relies on these
//                  surviving the fork() call; rbp = frame pointer / live locals)
//   arg/scratch  : rdi, rsi, rdx, r10, r8, r9     (fork_do_iretq restores all 15;
//                  the kernel claims a FULL frame, so prove it)
// Self-contained (no libc, own _start, direct syscalls). Smoke greps
// "FORKREGTEST RESULT: PASS".
//
// The magic-store / fork / read-back is ONE inline-asm block: the compiler
// cannot reload the registers between the store and the check, and the child
// resumes at the instruction right after `syscall`, reading the SAME registers.
// They equal the magics IFF fork() inherited them; otherwise the child reads
// kernel garbage (the bug forkfdtest first surfaced).
//
// CHILD checks all 12 (callee-saved + arg). PARENT checks only callee-saved +
// rbp: the arg/scratch registers are caller-clobbered by the syscall for the
// returning parent (that is the ABI, not a fork bug), so asserting them in the
// parent would be a false failure. rcx/r11 are consumed by SYSCALL (RIP/RFLAGS)
// and are untestable here by construction.
//
// rbp handling: this TU is built -O2 (build_all.sh) so the frame pointer is
// omitted and the "=m" operands are rsp-relative -- safe even while rbp holds a
// magic. rbp is manually saved/restored (NOT in the clobber list) so it is net
// unchanged across the asm and never appears as a clobbered frame pointer.

typedef unsigned long size_t;

#define SYS_EXIT     0
#define SYS_WRITE    3
#define SYS_WAITPID  6
#define SYS_YIELD    15

#define MBX 0x0B0B0B0B0B0B0B0BUL
#define MBP 0xB9B9B9B9B9B9B9B9UL
#define M12 0x1212121212121212UL
#define M13 0x1313131313131313UL
#define M14 0x1414141414141414UL
#define M15 0x1515151515151515UL
#define MDI 0xD1D1D1D1D1D1D1D1UL
#define MSI 0x5151515151515151UL
#define MDX 0xDADADADADADADADAUL
#define M10 0xA0A0A0A0A0A0A0A0UL
#define M8  0x0808080808080808UL
#define M9  0x0909090909090909UL

#define ITERS 50

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
#define sc0(n)       sc6((n),0,0,0,0,0)

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void outnum(long v) {
    char buf[24]; char* p = buf + 23; *p = 0;
    if (v < 0) { out("-"); v = -v; }
    if (v == 0) { *--p = '0'; }
    else { while (v > 0) { *--p = (char)('0' + (v % 10)); v /= 10; } }
    out(p);
}

void _start(void) {
    out("FORKREGTEST: start\n");

    int ok = 1;
    long first_fail = -1;
    long fail_detail = 0;   // bitmask of which regs were wrong (child), or <0 code
    int i;

    for (i = 0; i < ITERS; i++) {
        unsigned long fr = 1, sv = 0;
        unsigned long bxv=0, bpv=0, r12v=0, r13v=0, r14v=0, r15v=0;
        unsigned long div=0, siv=0, dxv=0, r10v=0, r8v=0, r9v=0;

        // SYS_FORK = 1. Seed every inheritable GP reg, fork, read them all back
        // -- atomic (the child resumes at the insn after `syscall`).
        __asm__ volatile(
            "mov %%rbp, %[sv]\n\t"                       // save real frame ptr
            "movabs $0x0B0B0B0B0B0B0B0B, %%rbx\n\t"
            "movabs $0xB9B9B9B9B9B9B9B9, %%rbp\n\t"
            "movabs $0x1212121212121212, %%r12\n\t"
            "movabs $0x1313131313131313, %%r13\n\t"
            "movabs $0x1414141414141414, %%r14\n\t"
            "movabs $0x1515151515151515, %%r15\n\t"
            "movabs $0xD1D1D1D1D1D1D1D1, %%rdi\n\t"
            "movabs $0x5151515151515151, %%rsi\n\t"
            "movabs $0xDADADADADADADADA, %%rdx\n\t"
            "movabs $0xA0A0A0A0A0A0A0A0, %%r10\n\t"
            "movabs $0x0808080808080808, %%r8\n\t"
            "movabs $0x0909090909090909, %%r9\n\t"
            "mov $1, %%eax\n\t"                          // SYS_FORK
            "syscall\n\t"                                // parent + child resume here
            "mov %%rax, %[fr]\n\t"
            "mov %%rbx, %[bx]\n\t"
            "mov %%rbp, %[bp]\n\t"
            "mov %%r12, %[r12]\n\t"
            "mov %%r13, %[r13]\n\t"
            "mov %%r14, %[r14]\n\t"
            "mov %%r15, %[r15]\n\t"
            "mov %%rdi, %[di]\n\t"
            "mov %%rsi, %[si]\n\t"
            "mov %%rdx, %[dx]\n\t"
            "mov %%r10, %[r10]\n\t"
            "mov %%r8,  %[r8]\n\t"
            "mov %%r9,  %[r9]\n\t"
            "mov %[sv], %%rbp\n\t"                       // restore real frame ptr
            : [sv]"=m"(sv), [fr]"=m"(fr), [bx]"=m"(bxv), [bp]"=m"(bpv),
              [r12]"=m"(r12v), [r13]"=m"(r13v), [r14]"=m"(r14v), [r15]"=m"(r15v),
              [di]"=m"(div), [si]"=m"(siv), [dx]"=m"(dxv),
              [r10]"=m"(r10v), [r8]"=m"(r8v), [r9]"=m"(r9v)
            :
            : "rax", "rcx", "r11", "rbx", "r12", "r13", "r14", "r15",
              "rdi", "rsi", "rdx", "r10", "r8", "r9", "memory");

        if ((long)fr == 0) {
            // CHILD: every inherited register (callee-saved + arg) must hold its magic.
            long bad = 0;
            if (bxv  != MBX) bad |= 1L << 0;
            if (bpv  != MBP) bad |= 1L << 1;
            if (r12v != M12) bad |= 1L << 2;
            if (r13v != M13) bad |= 1L << 3;
            if (r14v != M14) bad |= 1L << 4;
            if (r15v != M15) bad |= 1L << 5;
            if (div  != MDI) bad |= 1L << 6;
            if (siv  != MSI) bad |= 1L << 7;
            if (dxv  != MDX) bad |= 1L << 8;
            if (r10v != M10) bad |= 1L << 9;
            if (r8v  != M8 ) bad |= 1L << 10;
            if (r9v  != M9 ) bad |= 1L << 11;
            if (bad) { out("FORKREGTEST: child bad_mask="); outnum(bad); out("\n"); }
            sc3(SYS_EXIT, bad ? 1 : 0, 0, 0);
            for (;;) {}
        }

        if ((long)fr < 0) {                 // fork failed
            ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = -1; }
            continue;
        }

        // PARENT: reap the child; status 0 means it saw every magic.
        int status = -1; long w = -1; int t;
        for (t = 0; t < 2000000 && w != (long)fr; t++) {
            w = sc3(SYS_WAITPID, (long)fr, (long)&status, 0);
            if (w != (long)fr) sc0(SYS_YIELD);
        }
        if (w != (long)fr) { ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = -2; } continue; }
        if (status != 0)   { ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = status; } }

        // PARENT sanity: its OWN callee-saved regs + rbp must survive the syscall
        // (arg/scratch regs are caller-clobbered for the parent by ABI -- not checked).
        if (bxv != MBX || bpv != MBP || r12v != M12 || r13v != M13 ||
            r14v != M14 || r15v != M15) {
            ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = -3; }
        }
    }

    out("FORKREGTEST: iters="); outnum(i);
    out(" full_regs_inherited="); outnum(ok);
    if (!ok) { out(" first_fail_iter="); outnum(first_fail);
               out(" detail="); outnum(fail_detail); }
    out("\n");
    out(ok ? "FORKREGTEST RESULT: PASS\n" : "FORKREGTEST RESULT: FAIL\n");
    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}
