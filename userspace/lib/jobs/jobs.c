/*
 * userspace/lib/jobs/jobs.c -- bounded MPMC job queue + worker thread pool.
 * =========================================================================
 *
 * See jobs.h for the contract and the single-core / no-speedup framing. This
 * file is FREESTANDING (no libc/libm): it talks to the kernel directly via
 * inline `syscall` wrappers, exactly like the threadtest/futextest probes.
 *
 * ---------------------------------------------------------------------------
 * Why every wait is a BLOCKING futex wait (and never a spin loop)
 * ---------------------------------------------------------------------------
 * The default kernel is COOPERATIVE: a thread runs until it yields or blocks.
 * If any wait here were a busy spin (`while (!cond) {}`), the spinning thread
 * would never relinquish the cpu, the thread that is supposed to satisfy `cond`
 * would never get to run, and the system would DEADLOCK. So:
 *
 *   - The queue mutex (g_lock) is a futex mutex: an uncontended acquire is a
 *     single atomic CAS (no syscall); a contended acquire BLOCKS in FUTEX_WAIT
 *     until the holder releases and FUTEX_WAKEs it.
 *   - A worker with no work to do BLOCKS in FUTEX_WAIT on the monotonic
 *     `submitted` counter. It first reads `submitted` into a local `seen`, then
 *     (under the lock) re-checks whether the ring is really empty, then waits on
 *     `seen`. If a submit happened in between, `submitted != seen`, so FUTEX_WAIT
 *     returns EAGAIN immediately and the worker loops -- the classic
 *     "compare-and-block" that cannot lose a wakeup.
 *   - jobsys_drain BLOCKS in FUTEX_WAIT on the monotonic `completed` counter the
 *     same way (read completed into seen, check completed==submitted, else wait
 *     on seen). A job finishing between the check and the wait bumps `completed`,
 *     making FUTEX_WAIT return EAGAIN -- no lost wakeup.
 *
 * The worker blocking on "no jobs" is precisely what YIELDS the cpu so the
 * submitter (and everything else) runs; the drainer blocking on "not all done"
 * is precisely what yields the cpu so the workers run. This is correct under the
 * cooperative kernel AND under PREEMPT=1.
 */

#include "jobs.h"

/* ---- syscall numbers (must match kernel/include/syscall.h) --------------- */
#define SYS_YIELD         15
#define SYS_FUTEX         70
#define SYS_THREAD_CREATE 79
#define SYS_THREAD_EXIT   80
#define SYS_THREAD_JOIN   81

/* ---- futex op codes (kernel/core/syscall/futex.c) ------------------------ */
#define FUTEX_WAIT   0
#define FUTEX_WAKE   1
#define EAGAIN_NEG  (-11)   /* FUTEX_WAIT returns this when *uaddr != val */

/* 3-arg syscall (thread create/exit/join, yield). */
static inline long sc3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* 6-arg syscall (futex needs r10/r8 for timeout/uaddr2). Mirrors futextest. */
static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

/* Thread wrappers (inline; this lib does not link libc). */
static int  thr_create(void (*entry)(void*), void* arg, void* stack_top) {
    return (int)sc3(SYS_THREAD_CREATE, (long)entry, (long)arg, (long)stack_top);
}
static void thr_exit(int rv) { sc3(SYS_THREAD_EXIT, rv, 0, 0); for (;;) {} }
static int  thr_join(int tid, int* rv) {
    return (int)sc3(SYS_THREAD_JOIN, tid, (long)rv, 0);
}

/* Futex primitives over a 32-bit word. fx_wait blocks iff *uaddr == val. */
static long fx_wait(volatile int* uaddr, int val) {
    return sc6(SYS_FUTEX, (long)uaddr, FUTEX_WAIT, val, 0, 0);
}
static long fx_wake(volatile int* uaddr, int n) {
    return sc6(SYS_FUTEX, (long)uaddr, FUTEX_WAKE, n, 0, 0);
}

/* ====================================================================== *
 *  Futex mutex: 0 = free, 1 = held. Uncontended paths are a single CAS    *
 *  (no syscall); only contention enters the kernel.                       *
 * ====================================================================== */
static void lock_acquire(volatile int* lock) {
    for (;;) {
        int expected = 0;
        /* Fast path: free -> held with no syscall. */
        if (__atomic_compare_exchange_n(lock, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        /* Contended: the lock currently reads `expected` (==1 here). Block until
         * it changes. If the holder releases between the CAS and the wait,
         * *lock != 1 and FUTEX_WAIT returns EAGAIN immediately -> we retry. */
        fx_wait(lock, expected);
    }
}
static void lock_release(volatile int* lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    fx_wake(lock, 1);   /* wake at most one waiter contending for the lock */
}

/* ====================================================================== *
 *  Bounded ring buffer + counters (all shared across the worker threads).  *
 * ====================================================================== */
#define JOBSYS_MAX_WORKERS 8
#define JOB_RING_CAP       256   /* power-of-two slot count */
#define JOB_RING_MASK      (JOB_RING_CAP - 1)

static job_t        g_ring[JOB_RING_CAP];
static volatile int g_lock     = 0;   /* futex mutex over head/tail */
static volatile int g_head     = 0;   /* next index to POP  (consumer side) */
static volatile int g_tail     = 0;   /* next index to PUSH (producer side) */

/* Monotonic counters (never wrap during a run; used as futex wait-words). They
 * are the publish/observe edges that make every wait lose-no-wakeup. */
static volatile int g_submitted = 0;  /* total jobs ever enqueued  */
static volatile int g_completed = 0;  /* total jobs ever finished  */
static volatile int g_stop      = 0;  /* shutdown flag (workers exit when set) */

static int g_n_workers = 0;
static int g_worker_tid[JOBSYS_MAX_WORKERS];

/* Per-worker stacks: distinct 16-aligned static regions (one per worker) so each
 * thread has an INDEPENDENT stack (the kernel 16-aligns the TOP we pass). 32 KiB
 * is plenty -- workers only run small job fns. These live in .bss (shared addr
 * space), but each worker uses ONLY its own row. */
#define WSTACK_SIZE (32 * 1024)
static __attribute__((aligned(16)))
unsigned char g_wstack[JOBSYS_MAX_WORKERS][WSTACK_SIZE];

/* ---- ring helpers (callers hold g_lock) --------------------------------- */
static int ring_empty(void) { return g_head == g_tail; }
static int ring_full(void)  { return (g_tail - g_head) >= JOB_RING_CAP; }

/* ====================================================================== *
 *  Worker loop                                                             *
 * ====================================================================== */
static void worker_main(void* arg) {
    (void)arg;
    for (;;) {
        /* ---- 1. Acquire a job, or block until one is available / we stop ---- */
        job_t job;
        int got = 0;

        /* Observe `submitted` BEFORE inspecting the ring, so that if a submit
         * races in after we decide the ring is empty, `submitted` has already
         * advanced past `seen` and our FUTEX_WAIT(seen) returns EAGAIN at once
         * (no lost wakeup). */
        int seen_submitted = __atomic_load_n(&g_submitted, __ATOMIC_ACQUIRE);

        lock_acquire(&g_lock);
        if (!ring_empty()) {
            job = g_ring[g_head & JOB_RING_MASK];
            g_head++;
            got = 1;
        }
        lock_release(&g_lock);

        if (got) {
            /* ---- 2. Run the job OUTSIDE the lock (the long part) ---- */
            job.fn(job.arg);
            /* ---- 3. Publish completion + wake the drainer ---- */
            __atomic_add_fetch(&g_completed, 1, __ATOMIC_ACQ_REL);
            /* Wake ALL drainers (there is one, but be safe): a single waiter is
             * cheap; INT max would also work. */
            fx_wake(&g_completed, JOBSYS_MAX_WORKERS + 1);
            continue;
        }

        /* Ring was empty. If we're stopping AND it stayed empty, exit cleanly. */
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
            /* Re-check emptiness under the lock: a job submitted just before stop
             * must still be drained, not dropped. */
            lock_acquire(&g_lock);
            int empty = ring_empty();
            lock_release(&g_lock);
            if (empty) thr_exit(0);
            continue;   /* there is still work -- loop back and take it */
        }

        /* ---- 4. Block until a submit (or stop) bumps `submitted`. ----
         * If `submitted` already moved since we sampled it, FUTEX_WAIT returns
         * EAGAIN immediately and we loop (re-check the ring). Otherwise we sleep
         * until jobsys_submit / jobsys_shutdown wakes this word. */
        fx_wait(&g_submitted, seen_submitted);
    }
}

/* ====================================================================== *
 *  Public API                                                             *
 * ====================================================================== */
int jobsys_init(int n_workers) {
    if (n_workers < 1) n_workers = 1;
    if (n_workers > JOBSYS_MAX_WORKERS) n_workers = JOBSYS_MAX_WORKERS;

    /* Reset all shared state (so init can be called again after shutdown). */
    g_head = g_tail = 0;
    g_submitted = g_completed = 0;
    g_stop = 0;
    g_lock = 0;
    g_n_workers = 0;

    for (int i = 0; i < n_workers; i++) {
        void* stack_top = (void*)(g_wstack[i] + WSTACK_SIZE);
        int tid = thr_create(worker_main, (void*)(long)i, stack_top);
        if (tid <= 0) {
            /* A worker failed to spawn. Tear down whatever started so we don't
             * leave half a pool running, then report failure. */
            if (g_n_workers > 0) {
                __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
                __atomic_add_fetch(&g_submitted, 1, __ATOMIC_ACQ_REL); /* perturb wait-word */
                fx_wake(&g_submitted, JOBSYS_MAX_WORKERS);
                for (int j = 0; j < g_n_workers; j++) thr_join(g_worker_tid[j], 0);
                g_n_workers = 0;
            }
            return tid < 0 ? tid : -1;
        }
        g_worker_tid[i] = tid;
        g_n_workers++;
    }
    return g_n_workers;
}

void jobsys_submit(job_fn_t fn, void* arg) {
    /* Push under the lock; if momentarily full, block on a slot opening (the
     * `completed` counter advances as workers finish, freeing slots). This is
     * back-pressure, not an error. */
    for (;;) {
        int seen_completed = __atomic_load_n(&g_completed, __ATOMIC_ACQUIRE);
        lock_acquire(&g_lock);
        if (!ring_full()) {
            g_ring[g_tail & JOB_RING_MASK].fn  = fn;
            g_ring[g_tail & JOB_RING_MASK].arg = arg;
            g_tail++;
            lock_release(&g_lock);
            /* Publish the new job: bump `submitted` THEN wake a worker. A worker
             * that sampled the old `submitted` and is about to FUTEX_WAIT will get
             * EAGAIN (value changed) instead of sleeping -> no lost wakeup. */
            __atomic_add_fetch(&g_submitted, 1, __ATOMIC_ACQ_REL);
            fx_wake(&g_submitted, 1);
            return;
        }
        lock_release(&g_lock);
        /* Full: wait until a job completes (frees a slot). Waiting on the exact
         * observed `completed` value means a completion in between returns EAGAIN
         * immediately. */
        fx_wait(&g_completed, seen_completed);
    }
}

void jobsys_drain(void) {
    for (;;) {
        int seen_completed = __atomic_load_n(&g_completed, __ATOMIC_ACQUIRE);
        int submitted      = __atomic_load_n(&g_submitted, __ATOMIC_ACQUIRE);
        if (seen_completed >= submitted) return;   /* all submitted work is done */
        /* Block until `completed` changes. If a worker finishes between the load
         * above and here, `completed != seen_completed` and FUTEX_WAIT returns
         * EAGAIN at once -> we re-check (no lost wakeup, no spin). */
        fx_wait(&g_completed, seen_completed);
    }
}

void jobsys_shutdown(void) {
    if (g_n_workers == 0) return;

    /* Signal stop, then perturb the `submitted` wait-word and wake EVERY worker
     * that may be blocked waiting for a job, so each observes g_stop and exits.
     * (Bumping submitted guarantees a worker that just sampled the old value and
     * is about to FUTEX_WAIT does not sleep through the wake.) */
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_submitted, 1, __ATOMIC_ACQ_REL);
    fx_wake(&g_submitted, JOBSYS_MAX_WORKERS);

    /* Join every worker so none is orphaned (and the shared address space / its
     * stack region is fully released by the kernel). */
    for (int i = 0; i < g_n_workers; i++) {
        thr_join(g_worker_tid[i], 0);
    }
    g_n_workers = 0;
}
