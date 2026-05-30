/*
 * userspace/lib/aictl/aictl.h -- AI Control Library public API.
 * ==============================================================
 *
 * Provides an AI agent (or any userspace program) with a clean, typed
 * interface to observe and control every process running on the OS.
 *
 * All functions degrade gracefully: if a kernel syscall returns a negative
 * value (not yet wired, or genuine error), the functions fall back to the
 * always-available SYS_PROCLIST=44 or return -1 with a reasonable in-band
 * indication.  Callers must treat any negative return as "unavailable /
 * try again later" rather than a hard fault.
 *
 * Freestanding-safe: no libc, no includes other than what is inlined here.
 * Build flag REQUIRED on cmdline (never a shell var):
 *   -fno-stack-protector  (no fs:0x28 canary emitted)
 *
 * Kernel ABI (syscall numbers):
 *   SYS_PROCLIST  = 44  sc(44,buf,max,0)    -> count or -errno
 *   SYS_PROC_QUERY= 60  sc(60,pid,&detail,0)-> 0 or -errno
 *   SYS_PROC_CTL  = 61  sc(61,pid,verb,arg) -> 0 or -errno
 *   SYS_SYSINFO   = 62  sc(62,&info,0,0)    -> 0 or -errno
 *   SYS_KILL      = 26  fallback kill
 *   SYS_GETPID    = 8
 *   SYS_WRITE     = 3
 *   SYS_YIELD     = 15
 *
 * Include this header in any userspace TU that needs the API.
 * Link against aictl.o (compiled from aictl.c).
 */

#ifndef AICTL_H
#define AICTL_H

/* -------------------------------------------------------------------------
 * Freestanding integer types (no stdint.h).
 * ---------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;

/* -------------------------------------------------------------------------
 * procinfo_t: shallow process entry returned by SYS_PROCLIST.
 * Exactly 64 bytes to match the kernel ABI (kernel/include/sched.h proc_info_t).
 *   offset  0: u32 pid          (4 bytes)
 *   offset  4: u32 parent_pid   (4 bytes)
 *   offset  8: u32 state        (4 bytes) 0=created 1=ready 2=running 3=blocked 4=terminated
 *   offset 12: u32 flags        (4 bytes)
 *   offset 16: char name[32]    (32 bytes)
 *   offset 48: u64 cpu_ticks    (8 bytes) timer ticks observed while running
 *   offset 56: u64 ctx_switches (8 bytes) number of times dispatched
 *   total: 64 bytes
 * The first 48 bytes are the original layout, so older readers keep working;
 * the two scheduler-stat fields are appended after name[32].
 * ---------------------------------------------------------------------- */
typedef struct {
    u32  pid;
    u32  parent_pid;
    u32  state;
    u32  flags;
    char name[32];
    u64  cpu_ticks;
    u64  ctx_switches;
} procinfo_t;

/* Compile-time layout assertion. */
typedef char _procinfo_size_assert[sizeof(procinfo_t) == 64 ? 1 : -1];

/* -------------------------------------------------------------------------
 * proc_detail_t: rich per-process record returned by SYS_PROC_QUERY.
 * ---------------------------------------------------------------------- */
typedef struct {
    u32  pid;
    u32  ppid;
    u32  state;
    u32  prio;       /* scheduling priority (0 = highest in most policies) */
    u64  cpu_ticks;  /* cumulative CPU ticks consumed                       */
    u32  mem_pages;  /* resident physical pages                             */
    u32  vma_count;  /* number of virtual memory areas                     */
    char name[32];
} proc_detail_t;

/* -------------------------------------------------------------------------
 * sysinfo_t: system-wide statistics returned by SYS_SYSINFO.
 * ---------------------------------------------------------------------- */
typedef struct {
    u64 total_mem;   /* total physical memory in bytes */
    u64 free_mem;    /* free physical memory in bytes  */
    u64 uptime_ms;   /* milliseconds since boot        */
    u32 proc_count;  /* total live processes           */
} sysinfo_t;

/* -------------------------------------------------------------------------
 * SYS_PROC_CTL verb codes.
 * ---------------------------------------------------------------------- */
#define AICTL_VERB_SUSPEND  0
#define AICTL_VERB_RESUME   1
#define AICTL_VERB_KILL     2
#define AICTL_VERB_SETPRIO  3

/* -------------------------------------------------------------------------
 * Process state constants.
 * ---------------------------------------------------------------------- */
#define PROC_STATE_CREATED    0
#define PROC_STATE_READY      1
#define PROC_STATE_RUNNING    2
#define PROC_STATE_BLOCKED    3
#define PROC_STATE_TERMINATED 4

/* -------------------------------------------------------------------------
 * API
 *
 * All functions return a count (>= 0) or 0 on success, or < 0 on error.
 * On -ENOSYS / not-wired, the library degrades gracefully (documented per
 * function).
 * ---------------------------------------------------------------------- */

/*
 * aictl_list -- enumerate live processes.
 *
 * Calls SYS_PROCLIST(44).  Fills up to `max` entries into `buf`.
 * Returns the number of entries written (>= 0) or a negative errno.
 *
 * Degradation: this is the base fallback syscall; if it returns < 0 the
 * other calls in this library also degrade.  Callers should show
 * "(process list unavailable)" when the return value is negative.
 */
int aictl_list(procinfo_t *buf, int max);

/*
 * aictl_query -- fetch rich detail for a single process.
 *
 * Calls SYS_PROC_QUERY(60).  On success, fills *detail and returns 0.
 * On -ENOSYS or other error, returns -1.  Callers should show "n/a" for
 * the fields that would have come from this call.
 */
int aictl_query(u32 pid, proc_detail_t *detail);

/*
 * aictl_suspend -- suspend (pause) a process.
 *
 * Calls SYS_PROC_CTL(61) with verb AICTL_VERB_SUSPEND.
 * Returns 0 on success, negative on failure.
 */
int aictl_suspend(u32 pid);

/*
 * aictl_resume -- resume a previously suspended process.
 *
 * Calls SYS_PROC_CTL(61) with verb AICTL_VERB_RESUME.
 * Returns 0 on success, negative on failure.
 */
int aictl_resume(u32 pid);

/*
 * aictl_kill -- terminate a process.
 *
 * Tries SYS_PROC_CTL(61) with verb AICTL_VERB_KILL first.
 * On -ENOSYS falls back to SYS_KILL(26).
 * Returns 0 on success, negative on failure.
 */
int aictl_kill(u32 pid);

/*
 * aictl_setprio -- change the scheduling priority of a process.
 *
 * Calls SYS_PROC_CTL(61) with verb AICTL_VERB_SETPRIO, arg=prio.
 * Returns 0 on success, negative on failure.
 */
int aictl_setprio(u32 pid, int prio);

/*
 * aictl_sysinfo -- query global system statistics.
 *
 * Calls SYS_SYSINFO(62).  On success, fills *info and returns 0.
 * On -ENOSYS, returns -1 and zeroes *info so callers can display "--".
 */
int aictl_sysinfo(sysinfo_t *info);

#endif /* AICTL_H */
