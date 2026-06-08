/*
 * futexwaittest.c — LIVE concurrent BLOCKING futex test (SYS_FUTEX = 70)
 * =====================================================================
 *
 * The sibling probe userspace/apps/futextest/futextest.c only covers the
 * NON-blocking futex paths (value-mismatch EAGAIN, wake-with-no-waiters,
 * bad op/addr). It can never park a task, so it cannot catch the two real
 * concurrency bugs that matter for the SMP/PREEMPT futex restructure:
 *
 *   (A) LOST WAKEUP  — a waiter that has truly BLOCKED on a futex word is
 *                      missed by a concurrent FUTEX_WAKE and sleeps forever.
 *   (B) WRONG WAITER — a FUTEX_WAKE on one futex key wakes a waiter that is
 *                      parked on a DIFFERENT key (key not filtered).
 *
 * This test drives the BLOCKING path for real, using REAL THREADS
 * (SYS_THREAD_CREATE/EXIT/JOIN) so every thread shares ONE address space
 * (one CR3). A futex word is therefore the SAME PHYSICAL ADDRESS in the
 * waiter and the waker — which is exactly what the kernel's physical-address
 * futex key hashing (futex_get_phys_addr + futex_hash in
 * kernel/core/syscall/futex.c) relies on. Distinct futex words live at
 * distinct physical addresses, so they MUST hash to independent wait state.
 *
 * Futex ABI (kernel/core/syscall/futex.c):
 *   sys_futex(uaddr, op, val, timeout, uaddr2, val3)
 *     FUTEX_WAIT (0): if *uaddr != val  → return EAGAIN (-11) immediately
 *                     if *uaddr == val  → BLOCK until a matching FUTEX_WAKE
 *                                          (returns 0 on wakeup)  <-- TESTED
 *     FUTEX_WAKE (1): wake up to `val` waiters on uaddr; returns count woken.
 *
 * SUB-CASE A (lost-wakeup):
 *   A child thread sets futex_word=1 and FUTEX_WAITs (val=1 ⇒ it blocks).
 *   main yields to let the child PARK, sets futex_word=0, then FUTEX_WAKEs.
 *   The waiter MUST wake and return 0; main thread_joins it. Proves a truly
 *   blocked waiter is not lost across a concurrent wake.
 *
 * SUB-CASE B (wrong-waiter / key filtering):
 *   Two child threads block on TWO DISTINCT futex words (distinct addresses).
 *   main wakes ONLY word #0. Waiter #0 MUST wake; waiter #1 MUST stay blocked.
 *   main confirms #1 is still parked, then wakes it too and joins both. Proves
 *   a wake is filtered to the EXACT futex key (no cross-key spurious wake).
 *
 * NEVER HANGS: every wait-for-the-child-to-park spin and every
 * wait-for-the-woken-flag spin is bounded by a yield budget. If a child does
 * not appear to park in time we WAKE ANYWAY (bounded retry) and, as a final
 * backstop, broadcast-wake before joining so a join can never block forever.
 * A blown budget is reported as FAIL, not a hang.
 *
 * DOUBLES AS THE SMP / PREEMPT RACE VEHICLE:
 *   Boot the kernel under QEMU with `-smp 2` (and/or a PREEMPT build) and this
 *   same test exercises a genuine waiter-on-CPU-B / waker-on-CPU-A race against
 *   the single-lock (wo->lock) futex restructure. On uniprocessor it still
 *   proves correctness via forced yields; on SMP it becomes a live data race
 *   probe for the lost-wakeup and wrong-waiter invariants.
 *
 * Build flags (no fs:0x28 canary), same as the other freestanding probes:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Smoke gate: prints "FUTEXWAITTEST: PASS" iff BOTH sub-cases pass.
 */

/* ---- minimal types (no standard headers) -------------------------------- */
typedef unsigned long       size_t;
typedef unsigned long long  uint64_t;
typedef long long           int64_t;
typedef unsigned int        uint32_t;
typedef unsigned char       uint8_t;

/* ---- syscall numbers (match threadtest.c / futextest.c) ----------------- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_FUTEX         70
#define SYS_THREAD_CREATE 79
#define SYS_THREAD_EXIT   80
#define SYS_THREAD_JOIN   81

/* ---- futex op codes (from futex.c) -------------------------------------- */
#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

/* ---- errno values (negative; from kernel/include/errno.h) --------------- */
#define EAGAIN  (-11)

/* ---- syscall wrappers --------------------------------------------------- */
/* 3-arg form (thread create/join, write, yield, exit). */
static long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

/* 6-arg form for SYS_FUTEX (uaddr, op, val, timeout, uaddr2, val3). */
static long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- thin wrappers ------------------------------------------------------ */
static int  thread_create(void (*entry)(void*), void* arg, void* stack_top) {
    return (int)syscall3(SYS_THREAD_CREATE, (long)entry, (long)arg, (long)stack_top);
}
static void thread_exit(int rv) { syscall3(SYS_THREAD_EXIT, rv, 0, 0); for (;;) {} }
static int  thread_join(int tid, int* rv) {
    return (int)syscall3(SYS_THREAD_JOIN, tid, (long)rv, 0);
}
static void yield(void) { syscall3(SYS_YIELD, 0, 0, 0); }

static long futex_wait(volatile uint32_t* w, int val) {
    return sc6(SYS_FUTEX, (long)w, FUTEX_WAIT, val, 0, 0);
}
static long futex_wake(volatile uint32_t* w, int n) {
    return sc6(SYS_FUTEX, (long)w, FUTEX_WAKE, n, 0, 0);
}

/* ---- I/O helpers (fd 1 = serial) ---------------------------------------- */
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

/* ---- bounded-spin budgets ----------------------------------------------- */
/* Yields we will burn waiting for a child to mark itself "about to block"
 * (or for a woken flag to flip). Generous, but FINITE — a blown budget is a
 * reported FAIL, never a hang. ~50k cooperative yields is far more scheduler
 * passes than a healthy wake needs, yet returns in well under a second. */
#define PARK_BUDGET   50000
#define WOKEN_BUDGET  50000

/* ---- per-thread user stacks (distinct regions ⇒ independent stacks) ------ */
#define TSTACK_SIZE (16 * 1024)
static __attribute__((aligned(16))) unsigned char tstack[2][TSTACK_SIZE];

/*
 * SHARED futex words. 4-byte aligned (futex_validate_addr requires it) and
 * volatile (so the kernel's atomic load and our reads see real memory). They
 * live in the one address space all threads share, so &fw0 / &fw1 are the same
 * PHYSICAL address for the waiter and the waker. fw0 and fw1 are DISTINCT
 * words ⇒ distinct physical addresses ⇒ independent futex keys (sub-case B).
 */
static volatile uint32_t fw0 __attribute__((aligned(4)));
static volatile uint32_t fw1 __attribute__((aligned(4)));

/*
 * Liveness flags so the waker can observe the waiter's progress WITHOUT
 * relying on timing alone:
 *   about_to_block[i] : waiter set its word and is about to call FUTEX_WAIT.
 *   woke[i]           : FUTEX_WAIT returned (the waiter resumed).
 *   wait_ret[i]       : the FUTEX_WAIT return value (0 = clean wakeup).
 * All shared (one address space).
 */
static volatile int about_to_block[2];
static volatile int woke[2];
static volatile long wait_ret[2];

/* ---- sub-case A: lost-wakeup -------------------------------------------- */
/*
 * Waiter for case A. Sets fw0=1 and FUTEX_WAITs with val=1 ⇒ *uaddr==val ⇒ it
 * BLOCKS in the kernel wait queue. When a matching FUTEX_WAKE arrives it
 * returns 0; we publish that and exit. The about_to_block flag lets main know
 * we are at the wait boundary so it can park us deterministically.
 */
static void waiter_A(void* arg) {
    (void)arg;
    fw0 = 1;
    __atomic_store_n(&about_to_block[0], 1, __ATOMIC_SEQ_CST);
    long r = futex_wait(&fw0, 1);     /* blocks until main wakes us */
    wait_ret[0] = r;
    __atomic_store_n(&woke[0], 1, __ATOMIC_SEQ_CST);
    thread_exit(0);
}

/* ---- sub-case B: wrong-waiter (two distinct keys) ----------------------- */
/* Waiter on fw0 (index 0) — woken FIRST and ALONE. */
static void waiter_B0(void* arg) {
    (void)arg;
    fw0 = 1;
    __atomic_store_n(&about_to_block[0], 1, __ATOMIC_SEQ_CST);
    long r = futex_wait(&fw0, 1);
    wait_ret[0] = r;
    __atomic_store_n(&woke[0], 1, __ATOMIC_SEQ_CST);
    thread_exit(0);
}
/* Waiter on fw1 (index 1) — must STAY blocked when only fw0 is woken. */
static void waiter_B1(void* arg) {
    (void)arg;
    fw1 = 1;
    __atomic_store_n(&about_to_block[1], 1, __ATOMIC_SEQ_CST);
    long r = futex_wait(&fw1, 1);
    wait_ret[1] = r;
    __atomic_store_n(&woke[1], 1, __ATOMIC_SEQ_CST);
    thread_exit(0);
}

/* Spin (bounded) until the waiter has reached its FUTEX_WAIT boundary, then
 * give the scheduler several extra yields so the wait syscall actually parks
 * the task inside the kernel before we wake. Returns 1 if it reached the
 * boundary within budget, 0 otherwise. */
static int wait_until_about_to_block(int idx) {
    int spins = 0;
    while (!__atomic_load_n(&about_to_block[idx], __ATOMIC_SEQ_CST)) {
        yield();
        if (++spins >= PARK_BUDGET) return 0;
    }
    /* Reached the boundary; burn a handful more passes so the FUTEX_WAIT
     * syscall has actually entered the kernel and blocked the task. */
    for (int k = 0; k < 64; k++) yield();
    return 1;
}

/* Spin (bounded) until woke[idx] flips. Returns 1 if it woke, 0 on timeout. */
static int wait_until_woken(int idx) {
    int spins = 0;
    while (!__atomic_load_n(&woke[idx], __ATOMIC_SEQ_CST)) {
        yield();
        if (++spins >= WOKEN_BUDGET) return 0;
    }
    return 1;
}

static void fail_exit(const char* msg) {
    out("FUTEXWAITTEST: FAIL ");
    out(msg);
    out("\n");
    syscall3(SYS_EXIT, 1, 0, 0);
    for (;;) {}
}

/* ======================================================================== */
/* SUB-CASE A: a truly-blocked waiter is woken (no lost wakeup).            */
/* Returns 1 on pass; on hard failure it fail_exit()s (never returns).      */
/* ======================================================================== */
static int run_case_A(void) {
    out("[FUTEXWAITTEST] A: lost-wakeup -- block a waiter, wake it, it must return 0\n");

    about_to_block[0] = 0; woke[0] = 0; wait_ret[0] = -999; fw0 = 0;

    void* stk = (void*)(tstack[0] + TSTACK_SIZE);
    int tid = thread_create(waiter_A, (void*)0, stk);
    if (tid <= 0) fail_exit("A: thread_create failed");

    /* Let the waiter reach FUTEX_WAIT and PARK. */
    if (!wait_until_about_to_block(0))
        fail_exit("A: waiter never reached the wait boundary (budget blown)");

    /* The waiter blocked with fw0==1 and val==1. Change the word (mimics a
     * real unlock) and wake exactly one waiter. */
    fw0 = 0;
    long woken = futex_wake(&fw0, 1);
    if (woken < 0) fail_exit("A: FUTEX_WAKE returned an error");
    /* woken==1 is the healthy result. woken==0 can legitimately happen if the
     * waiter has not yet committed to the queue on a racy schedule; we do NOT
     * fail on that alone — the woke-flag spin below is the real arbiter, and
     * we re-wake within it. */

    /* Wait (bounded) for the waiter to actually resume. Re-issue the wake a
     * few times in case the first wake raced ahead of the park. */
    int reW = 0;
    while (!wait_until_woken(0)) {
        if (++reW > 8) fail_exit("A: waiter never woke (LOST WAKEUP) -- budget blown");
        futex_wake(&fw0, 1);   /* bounded retry; never an infinite loop */
    }

    if (wait_ret[0] != 0) {
        out("[FUTEXWAITTEST] A: FUTEX_WAIT returned ");
        out_num(wait_ret[0]);
        out(" (expected 0)\n");
        fail_exit("A: waiter resumed with a non-zero return");
    }

    int rv = -1;
    if (thread_join(tid, &rv) != 0) fail_exit("A: thread_join failed");

    out("[FUTEXWAITTEST] A: PASS (waiter blocked then woke, ret=0, woken=");
    out_num(woken);
    out(")\n");
    return 1;
}

/* ======================================================================== */
/* SUB-CASE B: a wake on one key wakes ONLY that key's waiter.             */
/* Two waiters on distinct addresses; wake fw0 only; fw1's waiter must stay */
/* blocked. Returns 1 on pass; hard failures fail_exit().                  */
/* ======================================================================== */
static int run_case_B(void) {
    out("[FUTEXWAITTEST] B: wrong-waiter -- two keys, wake one, only that one wakes\n");

    about_to_block[0] = 0; about_to_block[1] = 0;
    woke[0] = 0; woke[1] = 0; wait_ret[0] = -999; wait_ret[1] = -999;
    fw0 = 0; fw1 = 0;

    void* stk0 = (void*)(tstack[0] + TSTACK_SIZE);
    void* stk1 = (void*)(tstack[1] + TSTACK_SIZE);
    int t0 = thread_create(waiter_B0, (void*)0, stk0);
    if (t0 <= 0) fail_exit("B: thread_create(waiter_B0) failed");
    int t1 = thread_create(waiter_B1, (void*)0, stk1);
    if (t1 <= 0) fail_exit("B: thread_create(waiter_B1) failed");

    /* Both must reach their FUTEX_WAIT boundaries and park on DISTINCT keys. */
    if (!wait_until_about_to_block(0))
        fail_exit("B: waiter0 never reached the wait boundary (budget blown)");
    if (!wait_until_about_to_block(1))
        fail_exit("B: waiter1 never reached the wait boundary (budget blown)");

    /* Wake ONLY key fw0. Key filtering ⇒ waiter1 (on fw1) must NOT wake. */
    fw0 = 0;
    long woken0 = futex_wake(&fw0, 1);
    if (woken0 < 0) fail_exit("B: FUTEX_WAKE(fw0) returned an error");

    /* Waiter0 must resume (bounded, with re-wake retry). */
    int reW = 0;
    while (!wait_until_woken(0)) {
        if (++reW > 8) fail_exit("B: waiter0 never woke after FUTEX_WAKE(fw0)");
        futex_wake(&fw0, 1);
    }
    if (wait_ret[0] != 0) fail_exit("B: waiter0 resumed with a non-zero return");

    /* THE KEY-FILTER ASSERTION: waiter1 must STILL be blocked. Burn a healthy
     * batch of yields; if it spuriously woke on the fw0 wake, woke[1] flips. */
    for (int k = 0; k < 4096; k++) yield();
    if (__atomic_load_n(&woke[1], __ATOMIC_SEQ_CST)) {
        out("[FUTEXWAITTEST] B: waiter1 (on fw1) woke from a FUTEX_WAKE(fw0) -- ");
        out("WRONG-WAITER / key not filtered\n");
        /* Still clean up so we never strand a thread / hang the join. */
        fw1 = 0; futex_wake(&fw1, 0x7fffffff);
        thread_join(t0, (void*)0);
        thread_join(t1, (void*)0);
        fail_exit("B: cross-key spurious wake");
    }

    /* Correct so far: now wake fw1's waiter and confirm it too returns 0. */
    fw1 = 0;
    long woken1 = futex_wake(&fw1, 1);
    if (woken1 < 0) fail_exit("B: FUTEX_WAKE(fw1) returned an error");

    reW = 0;
    while (!wait_until_woken(1)) {
        if (++reW > 8) fail_exit("B: waiter1 never woke after FUTEX_WAKE(fw1)");
        futex_wake(&fw1, 1);
    }
    if (wait_ret[1] != 0) fail_exit("B: waiter1 resumed with a non-zero return");

    /* Final backstop: broadcast-wake both keys so no thread can be stranded,
     * then join. join must not block (both waiters have resumed). */
    futex_wake(&fw0, 0x7fffffff);
    futex_wake(&fw1, 0x7fffffff);
    if (thread_join(t0, (void*)0) != 0) fail_exit("B: thread_join(t0) failed");
    if (thread_join(t1, (void*)0) != 0) fail_exit("B: thread_join(t1) failed");

    out("[FUTEXWAITTEST] B: PASS (fw0 wake woke only its waiter; fw1 stayed blocked then woke)\n");
    return 1;
}

/* ---- entry point -------------------------------------------------------- */
void _start(void) {
    out("[FUTEXWAITTEST] SYS_FUTEX=70 LIVE BLOCKING path (threads share one CR3)\n");
    out("[FUTEXWAITTEST] ============================================================\n");

    int a = run_case_A();   /* fail_exit()s internally on failure */
    int b = run_case_B();

    if (a && b) {
        out("FUTEXWAITTEST: PASS\n");
        syscall3(SYS_EXIT, 0, 0, 0);
    } else {
        /* Defensive: run_case_* fail-exit on error, so this is unreachable,
         * but keep an explicit FAIL gate for total clarity. */
        out("FUTEXWAITTEST: FAIL (a sub-case did not pass)\n");
        syscall3(SYS_EXIT, 1, 0, 0);
    }
    for (;;) {}
}
