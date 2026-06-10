/*
 * cpu1hello.c -- SMP-F3-5: the FIRST ring-3 process on CPU1 (ring 3).
 * ====================================================================
 *
 * The smallest possible user workload that exercises the complete CPU1
 * ring-3 lifecycle the scheduler policy layer's law 8 demands:
 *
 *   dispatch  -> first ring-3 instruction on CPU1 (the trampoline iretq)
 *   syscalls  -> SYS_WRITE marks (counted by the smoke) + SYS_YIELD
 *                (bounces through ap_cooperative_schedule and back)
 *   exit      -> return 42 (crt0 -> SYS_EXIT) -- THE dangerous boundary:
 *                per-CPU current resolution, cross-CPU parent wake, CR3
 *                back to idle, teardown only after CPU1 is off the stack.
 *
 * Spawned by the KERNEL (kernel.c, SMP_SCHED_DISPATCH gate) pinned to CPU1
 * with init as the reaping parent. Shipped in every initrd (tiny, inert
 * without the gated kernel spawn).
 *
 * NO libc, NO stdio. Inline syscalls only (house pattern; crt0 provides
 * _start and turns main's return into SYS_EXIT).
 */

#define SYS_WRITE  3
#define SYS_YIELD  7

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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    for (int i = 0; i < 5; i++) {
        print("CPU1HELLO mark\n");   /* the smoke counts these (markers=<n>) */
        sc(SYS_YIELD, 0, 0, 0);      /* voluntary yield: AP cooperative path */
    }
    return 42;                       /* crt0 -> SYS_EXIT(42): the exit proof */
}
