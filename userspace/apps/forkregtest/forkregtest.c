// forkregtest -- FORK-REGS-INHERIT-0 proof. Verifies a fork()ed child resumes
// with the PARENT's callee-saved registers (rbx, r12-r15) intact -- i.e. fork
// inherits the full register set, not just rax=0. Self-contained (no libc, own
// _start, direct syscalls). The smoke greps "FORKREGTEST RESULT: PASS".
//
// The magic-store / fork / read-back is ONE inline-asm block: the compiler cannot
// reload the registers between the store and the check, and the child resumes at
// the instruction right after `syscall`, reading the SAME registers. They equal
// the magic values IFF fork() inherited them. Without register inheritance the
// child reads kernel garbage (exactly the bug forkfdtest first surfaced).

typedef unsigned long size_t;

#define SYS_EXIT     0
#define SYS_WRITE    3
#define SYS_WAITPID  6
#define SYS_YIELD    15

#define M12 0x1212121212121212UL
#define M13 0x1313131313131313UL
#define M14 0x1414141414141414UL
#define M15 0x1515151515151515UL
#define MBX 0x0B0B0B0B0B0B0B0BUL

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
    long fail_detail = 0;   // bitmask of which regs were wrong in the child
    int i;

    for (i = 0; i < ITERS; i++) {
        unsigned long fork_ret = 1, r12v = 0, r13v = 0, r14v = 0, r15v = 0, rbxv = 0;

        // SYS_FORK = 1. Set magics, fork, read the same registers back -- atomic.
        __asm__ volatile(
            "movabs $0x1212121212121212, %%r12\n\t"
            "movabs $0x1313131313131313, %%r13\n\t"
            "movabs $0x1414141414141414, %%r14\n\t"
            "movabs $0x1515151515151515, %%r15\n\t"
            "movabs $0x0B0B0B0B0B0B0B0B, %%rbx\n\t"
            "mov $1, %%eax\n\t"          // SYS_FORK
            "syscall\n\t"                // parent + child resume at the next insn
            "mov %%rax, %0\n\t"          // fork() return value
            "mov %%r12, %1\n\t"
            "mov %%r13, %2\n\t"
            "mov %%r14, %3\n\t"
            "mov %%r15, %4\n\t"
            "mov %%rbx, %5\n\t"
            : "=m"(fork_ret), "=m"(r12v), "=m"(r13v),
              "=m"(r14v), "=m"(r15v), "=m"(rbxv)
            :
            : "rax", "rcx", "r11", "r12", "r13", "r14", "r15", "rbx", "memory");

        if ((long)fork_ret == 0) {
            // CHILD: every callee-saved register must still hold its magic value.
            long bad = 0;
            if (r12v != M12) bad |= 1;
            if (r13v != M13) bad |= 2;
            if (r14v != M14) bad |= 4;
            if (r15v != M15) bad |= 8;
            if (rbxv != MBX) bad |= 16;
            sc3(SYS_EXIT, bad ? (100 + bad) : 0, 0, 0);
            for (;;) {}
        }

        if ((long)fork_ret < 0) {           // fork failed
            ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = -1; }
            continue;
        }

        // PARENT: reap the child; a 0 status means it saw all magics.
        int status = -1; long w = -1; int t;
        for (t = 0; t < 2000000 && w != (long)fork_ret; t++) {
            w = sc3(SYS_WAITPID, (long)fork_ret, (long)&status, 0);
            if (w != (long)fork_ret) sc0(SYS_YIELD);
        }
        if (w != (long)fork_ret) { ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = -2; } continue; }
        if (status != 0) { ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = status; } }

        // Sanity: the PARENT's own callee-saved regs must also survive the syscall.
        if (r12v != M12 || r13v != M13 || r14v != M14 || r15v != M15 || rbxv != MBX) {
            ok = 0; if (first_fail < 0) { first_fail = i; fail_detail = -3; }
        }
    }

    out("FORKREGTEST: iters="); outnum(i);
    out(" callee_saved_inherited="); outnum(ok);
    if (!ok) { out(" first_fail_iter="); outnum(first_fail);
               out(" detail="); outnum(fail_detail); }
    out("\n");
    out(ok ? "FORKREGTEST RESULT: PASS\n" : "FORKREGTEST RESULT: FAIL\n");
    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}
