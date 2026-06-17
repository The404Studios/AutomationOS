// fairwake -- SCHED-FAIRNESS-0 proof. Spawned alongside sbin/pureburn (pure
// non-syscalling ring-3 spinners) under a desktop-less boot, so NOTHING else
// offers a cooperative-switch boundary once init is blocked in waitpid on the
// never-exiting burners.
//
// fairwake SLEEPS, which blocks it; the timer wakes it -> it becomes a
// RESUME_CRETURN ready task. The OLD schedule_from_irq could only iretq-resume a
// RESUME_IRETQ successor, so the IRQ path would pick fairwake, REJECT it, and
// resume a pureburn -> fairwake starves forever and never prints. With the
// fairness fix the IRQ path hands the CPU to fairwake and it wakes. The smoke
// greps "FAIRWAKE: PASS" (present == fixed, absent == starved/buggy).
typedef unsigned long size_t;
#define SYS_EXIT   0
#define SYS_WRITE  3
#define SYS_SLEEP  9

static inline long sc(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}
static size_t slen(const char* s){ size_t n=0; while(s[n]) n++; return n; }
static void out(const char* s){ sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }

void _start(void) {
    out("FAIRWAKE: start (sleeping 2000ms under pure burners)\n");
    // Block on a real sleep. We are woken by the timer's sleep_list_wake_due()
    // and re-readied as RESUME_CRETURN -- the exact class the IRQ path starved.
    sc(SYS_SLEEP, 2000, 0, 0);
    // Reaching here means the scheduler DISPATCHED a woken RESUME_CRETURN task
    // from the IRQ path while only pure burners were running -> fix works.
    out("FAIRWAKE: PASS woke_after_sleep_under_pure_burn=1\n");
    sc(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}
