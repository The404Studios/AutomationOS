// neguptr -- NEGUPTR-0 regression proof for the copy_user_string unaligned-
// unmapped-user-pointer bug (kernel HIGH). Self-contained (no libs, own
// _start, direct syscalls). Prints to fd 1 (serial). The smoke greps
// "NEGUPTR: PASS".
//
// BUG (pre-fix, kernel/core/mem/vmm.c copy_user_string, commit 55926d4): the
// mapped-page check only fired when `(cur_addr & 0xFFF) == 0`, i.e. only at
// 4K-ALIGNED addresses. copy_user_string starts its byte walk at
// `copied == 0` with whatever address userspace handed it -- if that address
// is NOT page-aligned the very first byte is dereferenced in ring 0 BEFORE
// any accessibility check ever runs. An unmapped, non-aligned user pointer
// therefore produced a ring-0 #PF -> kernel_panic() -> the whole system goes
// down from one bad syscall argument (unprivileged DoS).
//
// FIX (kernel/core/mem/vmm.c HEAD, commit 6aacaeb): the guard now also fires
// on the very first byte -- `if (copied == 0 || (cur_addr & 0xFFFULL) == 0)`
// -- so a bad pointer is caught by user_page_is_accessible() before the first
// dereference and copy_user_string returns COPY_EFAULT -> the syscall handler
// returns a negative errno (EFAULT = -14) to userspace instead of faulting in
// kernel mode.
//
// CHOOSING A POINTER THAT ACTUALLY DISCRIMINATES
// ----------------------------------------------
// The bad pointer must be UNMAPPED in the caller's live CR3, otherwise the
// pre-fix kernel's ring-0 read simply succeeds and NO panic occurs (the test
// would then pass on BOTH commits and prove nothing). Low addresses like
// 0x1234 do NOT work: paging_create_address_space() deep-copies PML4[0] (the
// boot identity map, virt 0..RAM, flags 0x83 = Present|Write|Huge, SUPERVISOR)
// into every user address space, so 0x1234 is PRESENT -- and a CPL0 read of a
// supervisor page never faults. The only PML4 slots present in a user CR3 are
// 0 (identity + ELF @ 0x800000), 255 (user stack @ 0x7FFFFFFFE000), and
// 256..511 (kernel/direct-map). Slots 1..254 are guaranteed NON-present.
// So we hand each syscall a NON-page-aligned VA in slot 1/2/3 -- below
// USER_SPACE_END (0x0000800000000000, slot 256) so is_user_address() is true
// and copy_user_string enters its byte loop, but truly unmapped so the pre-fix
// deref #PFs in ring 0.
//
// This test invokes THREE different syscalls (SYS_OPEN, SYS_STAT,
// SYS_UNLINK) that each route their path argument through copy_user_string
// (verified in kernel/core/syscall/handlers.c: sys_open, sys_stat,
// sys_unlink). On the FIXED kernel each call returns a negative EFAULT and the
// process survives to print the verdict + a valid getpid(). On the PRE-FIX
// kernel the first such call (SYS_OPEN) panics the kernel in ring 0 -- boot
// dies, this app never reaches its print, and the "NEGUPTR: PASS" marker never
// appears in the serial log (smoke FAILs on the missing marker, and
// independently on the PANIC / uncontained-CPU-exception checks).

typedef unsigned long size_t;

#define SYS_EXIT    0
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_GETPID  8
#define SYS_STAT    33
#define SYS_UNLINK  34

// Non-page-aligned, UNMAPPED user VAs. Each sits in a PML4 slot (1/2/3) that
// paging_create_address_space() never populates, so every one is guaranteed
// absent in the caller's live CR3, yet all are < USER_SPACE_END
// (0x0000800000000000) so the kernel treats them as user pointers. None is
// 4K-aligned, so the pre-fix alignment-only guard would let the very first
// byte through unchecked and dereference it in ring 0.
#define BADP_OPEN    0x0000008000001234UL   // PML4 slot 1  (512GB + 0x1234)
#define BADP_STAT    0x0000010000002abcUL   // PML4 slot 2  (1TB   + 0x2abc)
#define BADP_UNLINK  0x000001800000137fUL   // PML4 slot 3  (1.5TB + 0x137f)

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

void _start(void) {
    int ok = 1;
    out("NEGUPTR: start\n");

    // --- Check 1: SYS_OPEN with a bad unaligned unmapped path pointer ---
    out("NEGUPTR: [1] SYS_OPEN path=slot1+0x234\n");
    long r1 = sc6(SYS_OPEN, (long)BADP_OPEN, 0 /*flags*/, 0 /*mode*/, 0, 0);
    if (r1 < 0) {
        out("NEGUPTR: [1] PASS efault_returned=1\n");
    } else {
        out("NEGUPTR: [1] FAIL open did not report EFAULT\n");
        ok = 0;
    }

    // --- Check 2: SYS_STAT with a different bad unaligned unmapped path
    //     pointer. The stat-buf output pointer IS valid/mapped (a real stack
    //     buffer) so only the path argument under test is bad. ---
    out("NEGUPTR: [2] SYS_STAT path=slot2+0xabc\n");
    char statbuf[256];
    long r2 = sc3(SYS_STAT, (long)BADP_STAT, (long)statbuf, 0);
    if (r2 < 0) {
        out("NEGUPTR: [2] PASS efault_returned=1\n");
    } else {
        out("NEGUPTR: [2] FAIL stat did not report EFAULT\n");
        ok = 0;
    }

    // --- Check 3: SYS_UNLINK with yet another bad unaligned unmapped path
    //     pointer, exercising a third distinct handler that funnels through
    //     copy_user_string. ---
    out("NEGUPTR: [3] SYS_UNLINK path=slot3+0x37f\n");
    long r3 = sc3(SYS_UNLINK, (long)BADP_UNLINK, 0, 0);
    if (r3 < 0) {
        out("NEGUPTR: [3] PASS efault_returned=1\n");
    } else {
        out("NEGUPTR: [3] FAIL unlink did not report EFAULT\n");
        ok = 0;
    }

    // --- Check 4: the process (and kernel) is still alive after all three
    //     bad-pointer syscalls -- a real pid comes back, no panic occurred. ---
    long pid = sc0(SYS_GETPID);
    if (pid > 0) {
        out("NEGUPTR: [4] PASS kernel_alive=1 getpid_ok=1\n");
    } else {
        out("NEGUPTR: [4] FAIL getpid broken after bad-pointer syscalls\n");
        ok = 0;
    }

    out(ok ? "NEGUPTR: PASS\n" : "NEGUPTR: FAIL\n");
    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}
