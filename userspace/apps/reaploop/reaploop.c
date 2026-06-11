// reaploop -- LIVE PROOF of PID-SLOT RECYCLING after the zombie-reap (#9) fix.
//
// The kernel process table is fixed at MAX_PROCESSES == 256 (process.c:18). A PID
// slot returns to the free pool only when a terminated child is fully reaped --
// i.e. when BOTH the creation ref (claim-based reap) AND the scheduler/running ref
// (KILL-FIX-002) are released so ref_count reaches 0 and process_unref clears
// pid_used[]. If EITHER ref leaks (the pre-#9 state), pid_used[] is never cleared,
// allocate_pid() (lowest-free scan, process.c:43) never re-hands-out that slot, and
// child PIDs climb monotonically until the pool exhausts.
//
// PROOF STRATEGY (reuse detection -- rigorous AND fast): fork+reap a trivial child
// in a loop and count how many times each PID is handed to our child. allocate_pid
// returns the LOWEST free PID, so once a slot is freed by a reap it is the slot the
// NEXT fork reuses. A PID handed to our child RECYCLE_PROOF (==12) times therefore
// proves its slot was freed and reused 11 times -- IMPOSSIBLE under the #9 leak
// (where the slot is never freed, so no PID is ever reused). This lands in ~12
// cycles, far cheaper than literally cycling >256 times (which the cooperative
// scheduler + per-fork serial logging make impractically slow), while proving the
// exact property #9 is about. If NO slot is reused RECYCLE_PROOF times within
// MAX_CYCLES, the pool is leaking -> REAPLOOP: FAIL.
//
// Self-contained (no libs): talks to the kernel directly via `syscall`, prints to
// fd 1 (serial), exits. Mirrors userspace/apps/forktest/forktest.c (same syscall
// numbers + wrapper). The child touches NOTHING (cheapest fork + teardown).

typedef unsigned long size_t;

#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_WRITE   3
#define SYS_WAITPID 6
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
static void out_num(long v) {
    char b[24]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i++] = '-';
    char r[24]; int j = 0;
    while (i > 0) r[j++] = b[--i];
    r[j] = '\0';
    out(r);
}

// Cap on total fork+reap cycles before we declare the pool non-recycling (a leak).
// With the #9 fix the proof lands far sooner (see RECYCLE_PROOF); this is only a
// runaway/leak backstop. > 256 preserves the historical "past the 256-slot" intent.
#define MAX_CYCLES 600

// A child PID handed out this many times proves its slot was freed and REUSED
// (RECYCLE_PROOF - 1) times -- definitive recycling, impossible under the #9 leak.
#define RECYCLE_PROOF 12

// Reap one child by PID. sys_waitpid BLOCKS until the child terminates (then
// returns its pid), so this normally returns after one call; the yield+retry is a
// bounded belt-and-suspenders so a kernel bug can't wedge us forever. Returns 1 if
// reaped, 0 on timeout.
static int reap(long pid) {
    int status = 0;
    for (long tries = 0; tries < 2000000; tries++) {
        long w = syscall3(SYS_WAITPID, pid, (long)&status, 0);
        if (w == pid) return 1;            // child reaped -> its PID slot is freed
        syscall3(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}

// Per-PID allocation counter (pid -> times handed to OUR fork child). In .bss; we
// zero it explicitly since this bare _start binary does not run crt0.
static unsigned char pcount[256];

void _start(void) {
    out("REAPLOOP: start (fork+reap; proves PID-slot recycling -- the #9 fix)\n");

    for (int i = 0; i < 256; i++) pcount[i] = 0;

    long fork_retries = 0;
    for (long it = 0; it < MAX_CYCLES; it++) {
        long pid;
        // Retry fork on TRANSIENT ENOMEM (-12): during the boot self-test storm the
        // 256-PID pool can momentarily fill with not-yet-reaped boot zombies (init
        // only harvests them once it reaches its waitpid(-1) loop). Yield to let it
        // reap one, then retry. A real, never-recovering leak exhausts the bounded
        // budget and FAILs rather than looping forever.
        for (;;) {
            pid = syscall3(SYS_FORK, 0, 0, 0);
            if (pid >= 0) break;                 // 0 = child, > 0 = parent
            if (++fork_retries > 1000000) {
                out("REAPLOOP: FAIL fork ENOMEM persisted at cycle "); out_num(it);
                out(" -- PID pool never recovered (reaping leaks slots)\n");
                syscall3(SYS_EXIT, 1, 0, 0); for (;;) {}
            }
            syscall3(SYS_YIELD, 0, 0, 0);
        }

        if (pid == 0) {                          // child: do nothing, exit immediately
            syscall3(SYS_EXIT, 0, 0, 0); for (;;) {}
        }

        // parent: this PID is currently allocated to our child. Count it; reaching
        // RECYCLE_PROOF hand-outs of the SAME pid proves its slot recycles.
        int reused = 0;
        if (pid >= 1 && pid < 256) {
            if (pcount[pid] < 255) pcount[pid]++;
            reused = (pcount[pid] >= RECYCLE_PROOF);
        }

        if (!reap(pid)) {                        // reap to free the slot for reuse
            out("REAPLOOP: FAIL reap-timeout at cycle "); out_num(it);
            out(" (child pid "); out_num(pid); out(" never terminated)\n");
            syscall3(SYS_EXIT, 2, 0, 0); for (;;) {}
        }

        if (reused) {
            out("REAPLOOP: recycled PID "); out_num(pid);
            out(" handed out "); out_num(pcount[pid]);
            out(" times over "); out_num(it + 1); out(" cycles\n");
            out("REAPLOOP: PASS\n");
            syscall3(SYS_EXIT, 0, 0, 0); for (;;) {}
        }

        if ((it % 64) == 63) {
            out("REAPLOOP: ... "); out_num(it + 1);
            out(" cycles, no slot reused "); out_num(RECYCLE_PROOF); out("x yet\n");
        }
    }

    // MAX_CYCLES reached with NO slot reused RECYCLE_PROOF times -> pool not
    // recycling (a real zombie/PID leak: slots are never returned to the pool).
    out("REAPLOOP: FAIL no PID recycled after "); out_num((long)MAX_CYCLES);
    out(" cycles -- slots are NOT being freed (zombie/PID leak)\n");
    syscall3(SYS_EXIT, 1, 0, 0); for (;;) {}
}
