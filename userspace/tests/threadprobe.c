/*
 * threadprobe.c -- SMP-THREAD-INHERIT-0: a THREADED BATCH workload proving that
 * worker threads INHERIT the parent's CPU placement (one address space, one
 * execution CPU until per-mm TLB shootdown exists).
 * ==========================================================================
 *
 * Kernel-spawned BATCH -> CPU1 (the batchdemo pattern), pointedly NOT on the
 * sys_spawn allowlist (no_allowlist_expansion). The parent creates 2 PERSISTENT
 * worker threads, then all three (parent + 2 workers) loop COOPERATIVELY
 * forever -- so the kernel's live [THREADINHERIT] observation reliably catches
 * them all on CPU1 throughout the 30-minute soak, and each periodic dispatch
 * re-stamps ran_on_cpus bit 1. The proof is KERNEL-SIDE (ran_on_cpus + the
 * shared mm accumulator are the ground truth), so there is no getcpu here.
 *
 * Serial evidence (the smoke greps these):
 *   THREADPROBE: start ...            (the parent reached ring 3 on CPU1)
 *   THREADPROBE: 2 workers created
 * plus kernel-side: the seam placement print (threadprobe -> cpu1), the
 * [THREADINHERIT] observation (parent + both workers ran=0x2, mm single-CPU).
 *
 * NO libc, NO stdio (the cpu1hello/batchdemo house pattern; crt0 -> _start ->
 * main -> SYS_EXIT, though main here never returns).
 */

#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_THREAD_CREATE 79

static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static void print(const char *m)
{
    unsigned long len = 0;
    while (m[len]) len++;
    sc(SYS_WRITE, 1, (long)m, (long)len);
}

#define NWORKERS    2
#define TSTACK_SIZE (16 * 1024)

/* Distinct per-worker stacks. They live in the SHARED address space (BSS) the
 * parent and both workers run on -- the very thing that makes a stray CPU0
 * dispatch a cross-CPU TLB hazard, and the thing inheritance prevents. */
static __attribute__((aligned(16))) unsigned char tstack[NWORKERS][TSTACK_SIZE];

/* A worker: light cooperative work forever, yielding each round so all three
 * tasks of the address space get to dispatch (each dispatch stamps ran_on_cpus
 * bit 1 == CPU1). A pure spin would monopolize CPU1's cooperative scheduler and
 * starve its siblings, so the yield is load-bearing for the proof. */
static void worker(void *arg)
{
    (void)arg;
    volatile unsigned long acc = 0;
    for (;;) {
        for (int k = 0; k < 2000; k++) acc += (unsigned long)k;
        sc(SYS_YIELD, 0, 0, 0);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    print("THREADPROBE: start (2 workers inherit CPU placement)\n");

    for (int i = 0; i < NWORKERS; i++) {
        void *top = (void *)(tstack[i] + TSTACK_SIZE);
        long tid = sc(SYS_THREAD_CREATE, (long)worker, (long)0, (long)top);
        if (tid <= 0) {
            print("THREADPROBE: FAIL(thread_create)\n");
        }
    }

    print("THREADPROBE: 2 workers created\n");

    /* The parent stays a live CPU1 resident too (cooperative loop forever), so
     * the observation sees the WHOLE address space -- leader + workers -- on
     * CPU1 for the duration. */
    volatile unsigned long acc = 0;
    for (;;) {
        for (int k = 0; k < 2000; k++) acc += (unsigned long)k;
        sc(SYS_YIELD, 0, 0, 0);
    }

    return 0;   /* unreachable */
}
