/*
 * userspace/lib/aictl/aictl.c -- AI Control Library implementation.
 * ==================================================================
 *
 * Implements the aictl.h API: typed wrappers over the OS syscalls that let
 * an AI agent (or any userspace caller) observe and control every process.
 *
 * Graceful degradation contract:
 *   - SYS_PROC_QUERY (60) and SYS_SYSINFO (62) are newer; if they return a
 *     negative value the corresponding function returns -1 so callers can
 *     fall back to SYS_PROCLIST (44) data and display "n/a".
 *   - SYS_PROC_CTL (61) verb KILL falls back to SYS_KILL (26) on -ENOSYS.
 *   - No function aborts or loops on error; all return to the caller.
 *
 * Build (ALL flags DIRECTLY on the cmdline -- no shell vars):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/lib/aictl/aictl.c -o /tmp/aictl.o
 *
 *   # Verify no stack-canary:
 *   objdump -d /tmp/aictl.o | grep fs:0x28   # must be empty
 */

#include "aictl.h"

/* -------------------------------------------------------------------------
 * Syscall numbers.
 * ---------------------------------------------------------------------- */
#define SYS_WRITE      3
#define SYS_GETPID     8
#define SYS_YIELD      15
#define SYS_KILL       26
#define SYS_PROCLIST   44
#define SYS_PROC_QUERY 60
#define SYS_PROC_CTL   61
#define SYS_SYSINFO    62

/* Linux/custom OS -ENOSYS value (errno 38). */
#define ENOSYS_NEG  (-38)

/* -------------------------------------------------------------------------
 * Inline 3-argument syscall.
 * NOTE: All four arguments must be literal or local values; never pass a
 *       shell-expanded variable here or the compiler may emit a canary.
 * ---------------------------------------------------------------------- */
static inline long _sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -------------------------------------------------------------------------
 * Helper: zero a block of memory (no libc).
 * ---------------------------------------------------------------------- */
static void _zero(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    for (unsigned long i = 0; i < n; i++) b[i] = 0;
}

/* =========================================================================
 * Public API implementations.
 * ====================================================================== */

/*
 * aictl_list -- SYS_PROCLIST(44): buf <- up to max procinfo_t entries.
 * Returns count (>= 0) or negative errno.
 */
int aictl_list(procinfo_t *buf, int max)
{
    if (!buf || max <= 0) return -1;
    long r = _sc(SYS_PROCLIST, (long)buf, (long)max, 0);
    /* If positive or zero, it is the count.  Negative = error. */
    return (int)r;
}

/*
 * aictl_query -- SYS_PROC_QUERY(60): fills *detail for a single pid.
 * Degrades gracefully: returns -1 if the syscall is not wired.
 */
int aictl_query(u32 pid, proc_detail_t *detail)
{
    if (!detail) return -1;
    _zero(detail, sizeof(*detail));
    long r = _sc(SYS_PROC_QUERY, (long)pid, (long)detail, 0);
    if (r < 0) return -1;
    return 0;
}

/*
 * aictl_suspend -- SYS_PROC_CTL(61) verb 0 (SUSPEND).
 */
int aictl_suspend(u32 pid)
{
    long r = _sc(SYS_PROC_CTL, (long)pid, (long)AICTL_VERB_SUSPEND, 0);
    return (int)r;
}

/*
 * aictl_resume -- SYS_PROC_CTL(61) verb 1 (RESUME).
 */
int aictl_resume(u32 pid)
{
    long r = _sc(SYS_PROC_CTL, (long)pid, (long)AICTL_VERB_RESUME, 0);
    return (int)r;
}

/*
 * aictl_kill -- SYS_PROC_CTL(61) verb 2 (KILL), fallback to SYS_KILL(26).
 */
int aictl_kill(u32 pid)
{
    long r = _sc(SYS_PROC_CTL, (long)pid, (long)AICTL_VERB_KILL, 0);
    if (r == ENOSYS_NEG || r == -1) {
        /* Syscall not wired; fall back to generic kill (SIGKILL=9). */
        r = _sc(SYS_KILL, (long)pid, 9, 0);
    }
    return (int)r;
}

/*
 * aictl_setprio -- SYS_PROC_CTL(61) verb 3 (SETPRIO), arg = new priority.
 */
int aictl_setprio(u32 pid, int prio)
{
    long r = _sc(SYS_PROC_CTL, (long)pid, (long)AICTL_VERB_SETPRIO, (long)prio);
    return (int)r;
}

/*
 * aictl_sysinfo -- SYS_SYSINFO(62): fills *info with system statistics.
 * On error zeroes *info and returns -1 so callers can display "--".
 */
int aictl_sysinfo(sysinfo_t *info)
{
    if (!info) return -1;
    _zero(info, sizeof(*info));
    long r = _sc(SYS_SYSINFO, (long)info, 0, 0);
    if (r < 0) {
        _zero(info, sizeof(*info));
        return -1;
    }
    return 0;
}
