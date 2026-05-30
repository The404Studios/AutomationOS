/* sched_class.h -- named scheduler priority classes for AutomationOS userspace.
 *
 * The kernel scheduler is an O(1) MLFQ keyed on each process's `priority` field,
 * which is a POSIX-style *nice* value in the range [-20, +19] (lower nice ==
 * higher priority). scheduler_add_process() enqueues a process at runqueue
 * priority NICE_TO_PRIORITY(nice) = 100 + nice (a 0..139 level), and
 * scheduler_pick_next() (used by BOTH the cooperative yield path AND the
 * preemptive timer IRQ path, schedule_from_irq) always runs the lowest-numbered
 * non-empty queue first. So a smaller nice == more CPU share, in both builds.
 *
 * This header maps a handful of human-readable CLASSES onto that nice axis so
 * apps don't sprinkle magic -10/+10 constants around, plus a tiny inline helper
 * sched_setclass() that pushes the class's nice into the kernel.
 *
 * Self-contained on purpose (no .c, no link step): like the other freestanding
 * probes (cpuburn/sleeptest), an app just #includes this and gets the constants
 * + the helper inlined. Nothing here allocates or touches a stack array, so it
 * stays %fs:0x28 canary-clean in the nostdlib ring-3 link.
 */
#ifndef AUTOMATIONOS_SCHED_CLASS_H
#define AUTOMATIONOS_SCHED_CLASS_H

/* Syscall numbers (must match kernel/include/syscall.h). */
#ifndef SYS_NICE
#define SYS_NICE         27   /* adjust priority: arg1=pid(0=self), arg2=increment */
#endif
#ifndef SYS_SETPRIORITY
#define SYS_SETPRIORITY  29   /* set priority: arg1=which(0=PRIO_PROCESS), arg2=who(0=self), arg3=nice */
#endif

/* ---------------------------------------------------------------------------
 * Named priority classes -> nice value.
 *
 * REALTIME is the most CPU-favored (nice -20 == runqueue level 80); IDLE is the
 * least (nice +19 == level 139). NORMAL (nice 0 == level 100) is the default a
 * freshly-created process already has, so SCHED_CLASS_NORMAL is a no-op shift.
 * These are the canonical five the scheduler-infra item asked for.
 * ------------------------------------------------------------------------- */
#define SCHED_CLASS_REALTIME    (-20)   /* hard-favored; use sparingly          */
#define SCHED_CLASS_HIGH        (-10)   /* clearly above normal                 */
#define SCHED_CLASS_NORMAL        (0)   /* default                              */
#define SCHED_CLASS_BACKGROUND   (10)   /* clearly below normal                 */
#define SCHED_CLASS_IDLE         (19)   /* only runs when nothing else wants CPU */

/* ---------------------------------------------------------------------------
 * sched_setclass(cls) -- set the CURRENT process's scheduler class.
 *
 * `cls` is one of the SCHED_CLASS_* nice values above (any nice in [-20,+19]
 * also works; the kernel clamps out-of-range values). Returns the resulting
 * nice value (>= -20) on success, or a negative errno on failure (e.g. -EPERM
 * if the caller lacks permission to renice itself, though with the single-UID
 * model that never trips).
 *
 * Implementation detail / arg convention (read kernel/core/sched/nice.c):
 *   We use SYS_SETPRIORITY because it sets an ABSOLUTE priority, which is what a
 *   "set my class" call means. sys_setpriority(which, who, prio):
 *     which = 0  (PRIO_PROCESS -- the only supported selector)
 *     who   = 0  (the current process)
 *     prio  = cls (the target nice; clamped to [-20,+19] by the kernel)
 *   It returns ESUCCESS(0) on success. We then read back the effective nice via
 *   SYS_NICE(self, 0): with pid==0 sys_nice ADDS the increment to the current
 *   priority, so an increment of 0 is a side-effect-free "get my nice" that
 *   returns the (now updated) value -- letting the caller confirm the class took.
 *
 *   (SYS_NICE alone could also set the class, but only RELATIVELY for self:
 *    sys_nice(0, delta) does priority += delta. Absolute-set via SYS_SETPRIORITY
 *    is cleaner for a "set class" semantic, so that's what we use to apply.)
 * ------------------------------------------------------------------------- */
static inline long sched__syscall3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static inline int sched_setclass(int cls) {
    /* Apply: absolute set of THIS process's nice to `cls`. */
    long rc = sched__syscall3(SYS_SETPRIORITY, 0 /*PRIO_PROCESS*/, 0 /*self*/, (long)cls);
    if (rc < 0) {
        return (int)rc;                 /* propagate errno (e.g. -EPERM)        */
    }
    /* Confirm: read back effective nice via a zero-increment self-nice. */
    long eff = sched__syscall3(SYS_NICE, 0 /*self*/, 0 /*increment*/, 0);
    return (int)eff;                    /* effective nice in [-20,+19]          */
}

#endif /* AUTOMATIONOS_SCHED_CLASS_H */
