/* prioritytest -- PROVES that scheduler priority classes actually shift CPU
 * share, measured in real per-process cpu_ticks.
 * ===========================================================================
 *
 * WHAT IT PROVES
 *   The kernel scheduler is an O(1) MLFQ keyed on each process's nice value
 *   (priority field, [-20,+19]); scheduler_add_process() enqueues a process at
 *   runqueue level 100+nice, and scheduler_pick_next() -- used by BOTH the
 *   cooperative yield path AND the preemptive timer-IRQ path (schedule_from_irq)
 *   -- always runs the lowest-numbered non-empty queue first. So a SMALLER nice
 *   should win MORE CPU. This test demonstrates that end to end.
 *
 * DESIGN (works in BOTH the cooperative and the PREEMPT=1 build)
 *   The parent fork()s TWO CPU-bound children:
 *     - child HIGH: sched_setclass(SCHED_CLASS_HIGH)        => nice -10
 *     - child LOW : sched_setclass(SCHED_CLASS_BACKGROUND)  => nice +10
 *   Each child sets its class BEFORE it starts spinning, then runs a compute
 *   loop (integer churn + a little float/SSE, volatile-fed so it can't be folded
 *   away) and calls SYS_YIELD every YIELD_EVERY iterations. The periodic yield
 *   is the crux: each yield re-enqueues the child at its nice-derived priority,
 *   so scheduler_pick_next() re-picks by priority every turn -- in the
 *   cooperative build (where only yields drive switches) AND the preemptive one
 *   (where the timer also preempts). The HIGH child, sitting in a lower-numbered
 *   queue, gets picked ahead of the LOW child far more often => it accrues more
 *   cpu_ticks.
 *
 *   The children spin until KILLED (never exit on their own) so they stay in the
 *   process table for the snapshot. The parent:
 *     1. spins a fixed WALL-CLOCK window (~WINDOW_MS via SYS_GET_TICKS_MS),
 *        yielding throughout so the children actually get the CPU (cooperative);
 *     2. snapshots SYS_PROCLIST(44) WHILE both children are still alive and
 *        reads each child's cpu_ticks (the per-process CPU-time stat);
 *     3. compares: PASS iff high_ticks > low_ticks * 1.3 (HIGH got meaningfully
 *        more CPU); prints the two numbers either way;
 *     4. SYS_KILLs both children (SIGKILL) and reaps them, then exits.
 *
 * OUTPUT (smoke greps for "PRIORITYTEST: PASS")
 *     PRIORITYTEST: high=<ticks> low=<ticks>
 *     PRIORITYTEST: PASS         (or FAIL ...)
 *
 * Modeled on forktest.c (fork/reap pattern) + cpuburn.c (compute loop) +
 * ps.c (the exact 64-byte SYS_PROCLIST procinfo_t ABI). crt0-linked; pure
 * inline syscalls; nostdlib/freestanding.
 */

/* gcc-15 quirks neutralised at TU scope (same rationale as cpuburn.c):
 *  - no-stack-protector: keep the local procinfo[] / digit buffers canary-clean
 *    (__stack_chk_fail does not exist in this freestanding ring-3 link).
 *  - no-tree-loop-distribute-patterns: stop -O2 from rewriting our byte loops
 *    into libc strlen/memcpy calls that don't exist freestanding. */
#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")

#include "../../lib/sched_class.h"   /* SCHED_CLASS_*, sched_setclass()         */

typedef unsigned long      size_t;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* Syscall numbers (verified against kernel/include/syscall.h). */
#define SYS_EXIT          0
#define SYS_FORK          1
#define SYS_WRITE         3
#define SYS_WAITPID       6
#define SYS_GETPID        8
#define SYS_YIELD         15
#define SYS_KILL          26
#define SYS_GET_TICKS_MS  40
#define SYS_PROCLIST      44

#define SIGKILL           9      /* kernel/core/signal/kill.c */
#define FD_STDOUT         1
#define MAX_PROCS         256    /* == kernel MAX_PROCESSES: see ALL live procs */

/* 64-byte SYS_PROCLIST record -- byte-for-byte identical to ps.c / aictl.h /
 * the kernel's proc_info_t. cpu_ticks at offset 48, ctx_switches at 56. */
typedef struct {
    u32  pid;
    u32  parent_pid;
    u32  state;
    u32  flags;
    char name[32];
    u64  cpu_ticks;
    u64  ctx_switches;
} procinfo_t;
typedef char _procinfo_size_assert[sizeof(procinfo_t) == 64 ? 1 : -1];

/* ----- inline syscall helpers ----- */
static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, FD_STDOUT, (long)s, (long)slen(s)); }

/* Emit an unsigned decimal a digit at a time (local fixed array written by
 * index, like cpuburn's out_uint -- stays canary-clean). */
static void out_uint(u64 v) {
    char buf[24];
    int i = 24;
    buf[--i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    out(&buf[i]);
}

/* ===========================================================================
 * CPU-bound child: set the requested class, then spin (with a periodic yield)
 * until killed. The work is volatile-fed integer + float churn so it is really
 * executed and a context switch must save/restore XMM as well as GP regs.
 * Never returns.
 * =========================================================================== */
#define YIELD_EVERY  4000000UL  /* iters of compute between yields (multi-ms bursts) */

static void child_spin(int cls) {
    /* Apply the scheduler class BEFORE doing any work, so the very first time we
     * land back on a runqueue we are already at the class's nice level. */
    (void)sched_setclass(cls);

    volatile float seed = 1.5f;
    unsigned long acc = 0;
    unsigned long lcg = 0x9E3779B1u;
    float facc = (float)seed;
    unsigned long i = 0;

    for (;;) {
        /* integer churn */
        lcg = lcg * 6364136223846793005UL + 1442695040888963407UL;
        acc += (lcg >> 33) ^ i;
        /* float/SSE churn, kept bounded so it never becomes Inf/NaN */
        facc += (float)(acc & 0x3) * 0.25f;
        if (facc >= 100.0f) facc -= 100.0f;

        if (++i >= YIELD_EVERY) {
            i = 0;
            /* Re-enqueue at our nice-derived priority so pick_next favors the
             * higher-priority child each turn (drives BOTH build paths). */
            sc(SYS_YIELD, 0, 0, 0);
        }
    }
}

/* ===========================================================================
 * Parent helpers
 * =========================================================================== */

/* Busy-wait a wall-clock window of `ms`, yielding throughout so the cooperative
 * children get scheduled (and the preemptive ones too -- yielding is harmless). */
static void run_window_ms(unsigned long ms) {
    unsigned long t0 = (unsigned long)sc(SYS_GET_TICKS_MS, 0, 0, 0);
    for (;;) {
        unsigned long now = (unsigned long)sc(SYS_GET_TICKS_MS, 0, 0, 0);
        if (now >= t0 + ms) break;
        sc(SYS_YIELD, 0, 0, 0);
    }
}

/* SYS_PROCLIST writes the snapshot into a caller-provided buffer via the
 * kernel's copy_to_user(), which REQUIRES every destination page to be already
 * present+writable in the page tables -- it does NOT demand-fault them. Our
 * snapshot buffer lives in BSS, whose pages are lazily/demand-zero mapped, so
 * the first ever access faults them in. If we call SYS_PROCLIST before touching
 * the buffer, copy_to_user's page-walk sees the pages NOT-present and returns
 * EFAULT (-14). (This bit us during bring-up: raw_proclist_ret=-14, nproc=0.)
 * So we pre-touch every page of the buffer (one write per 4 KiB) to fault them
 * in BEFORE the syscall. Done once on the shared static buffer below. */
static procinfo_t g_snap[MAX_PROCS];   /* static: avoid a huge stack frame */
static int g_snap_touched = 0;

static void touch_snap(void) {
    if (g_snap_touched) return;
    volatile char* p = (volatile char*)g_snap;
    unsigned long bytes = sizeof(g_snap);
    for (unsigned long off = 0; off < bytes; off += 4096UL) p[off] = 0;
    p[bytes - 1] = 0;                  /* last byte, in case of a partial page */
    g_snap_touched = 1;
}

/* Snapshot the process table and return the cpu_ticks for `pid` (or 0 if the
 * pid is not found in the snapshot / the syscall failed). */
static u64 cpu_ticks_of(int pid) {
    touch_snap();                      /* fault in BSS pages for copy_to_user  */
    long n = sc(SYS_PROCLIST, (long)g_snap, MAX_PROCS, 0);
    if (n <= 0) return 0;
    for (long k = 0; k < n; k++) {
        if ((int)g_snap[k].pid == pid) return g_snap[k].cpu_ticks;
    }
    return 0;
}

/* Reap a specific child (waitpid is non-blocking here: loop+yield until it is
 * reaped). Bounded so a stuck child can't hang the test forever. */
static void reap(int pid) {
    int status = 0;
    for (int tries = 0; tries < 2000000; tries++) {
        long w = sc(SYS_WAITPID, pid, (long)&status, 0);
        if (w == pid) return;
        sc(SYS_YIELD, 0, 0, 0);
    }
}

#define WINDOW_MS  2500UL   /* ~2.5 s measurement window */
#define PASS_RATIO 13       /* PASS iff high*10 > low*PASS_RATIO  (i.e. high > low*1.3) */

int main(void) {
    out("PRIORITYTEST: start (HIGH nice -10 vs BACKGROUND nice +10, ~2.5s window)\n");

    /* Fork child 0 = HIGH. */
    long hpid = sc(SYS_FORK, 0, 0, 0);
    if (hpid == 0) { child_spin(SCHED_CLASS_HIGH); sc(SYS_EXIT, 0, 0, 0); for(;;){} }
    if (hpid < 0) { out("PRIORITYTEST: FAIL fork-high\n"); return 1; }

    /* Fork child 1 = LOW (background). */
    long lpid = sc(SYS_FORK, 0, 0, 0);
    if (lpid == 0) { child_spin(SCHED_CLASS_BACKGROUND); sc(SYS_EXIT, 0, 0, 0); for(;;){} }
    if (lpid < 0) {
        out("PRIORITYTEST: FAIL fork-low\n");
        sc(SYS_KILL, hpid, SIGKILL, 0); reap((int)hpid);
        return 1;
    }

    /* Let both children spin for the measurement window. */
    run_window_ms(WINDOW_MS);

    /* Snapshot per-process CPU time WHILE both children are still alive. */
    u64 high = cpu_ticks_of((int)hpid);
    u64 low  = cpu_ticks_of((int)lpid);

    out("PRIORITYTEST: high="); out_uint(high);
    out(" low=");               out_uint(low);
    out("\n");

    /* Stop the burners and reap them so we don't leak running processes. */
    sc(SYS_KILL, hpid, SIGKILL, 0);
    sc(SYS_KILL, lpid, SIGKILL, 0);
    reap((int)hpid);
    reap((int)lpid);

    /* PASS iff the HIGH-priority child accrued MEANINGFULLY more CPU than LOW.
     * Use integer math: high > low * 1.3  <=>  high*10 > low*13. Also require a
     * non-trivial amount of HIGH CPU so a degenerate all-zero snapshot fails. */
    int pass = (high > 0) && (high * 10ULL > low * (u64)PASS_RATIO);
    out(pass ? "PRIORITYTEST: PASS\n" : "PRIORITYTEST: FAIL\n");
    return pass ? 0 : 1;
}
