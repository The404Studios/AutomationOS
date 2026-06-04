/*
 * kernel/core/procapi/procapi.c
 * Process Introspection + Control API
 *
 * Provides three functions the integrator wires to syscalls 60/61/62:
 *   procapi_query()   — rich per-process snapshot (SYS_PROC_QUERY = 60)
 *   procapi_ctl()     — suspend / resume / kill / reprioritise (SYS_PROC_CTL = 61)
 *   procapi_sysinfo() — system-wide memory + uptime + proc count (SYS_SYSINFO = 62)
 *
 * DESIGN NOTES
 * ============
 * process_table is static inside process.c.  We reach the process table
 * exclusively through the stable exported API:
 *   process_get_by_pid(pid)          — returns a ref-bumped pointer or NULL
 *   process_list(proc_info_t*, max)  — snapshot up to `max` entries
 *   process_unref(proc)              — release a reference obtained above
 *
 * For vma counting we walk the vma_list linked list without holding any lock
 * (vma_list is modified only on process creation/destruction, both of which
 * happen under the process table lock; a racing reader sees a consistent
 * prefix of the list at worst).
 *
 * Signal delivery reuses the existing sys_kill() mechanism directly by
 * manipulating process state the same way kill.c does, so no new kernel
 * surface is needed.  Priority changes use process_t->priority directly,
 * mirroring nice.c.
 */

#include "../../include/procapi.h"
#include "../../include/string.h"
#include "../../include/sched.h"
#include "../../include/mem.h"
#include "../../include/drivers.h"
#include "../../include/kernel.h"
#include "../../include/vma.h"
#include "../../include/errno.h"   /* canonical negative errno (EPERM/ESRCH/EINVAL) */

/* Priority bounds (mirrors nice.c) */
#define PA_PRIO_MIN  (-20)
#define PA_PRIO_MAX    19

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/*
 * vma_count_list - count entries in proc->vma_list without a lock.
 * Safe because vma_list entries are only appended/removed under the process
 * table lock, and we hold a ref to proc so the struct is not freed.
 */
static uint32_t vma_count_list(const process_t* proc)
{
    uint32_t n = 0;
    const vma_t* v = proc->vma_list;
    while (v) {
        n++;
        v = v->next;
    }
    return n;
}

/*
 * vma_page_count - sum page counts across all VMAs.
 * Each vma_t carries a byte-length; we convert to 4 KB pages.
 */
static uint32_t vma_page_count(const process_t* proc)
{
    uint64_t total_bytes = 0;
    const vma_t* v = proc->vma_list;
    while (v) {
        total_bytes += v->length;
        v = v->next;
    }
    return (uint32_t)(total_bytes / 4096);
}

/* =========================================================================
 * procapi_query
 * ========================================================================= */

int procapi_query(uint32_t pid, proc_detail_t* out)
{
    if (!out) {
        return EINVAL;
    }

    /* Zero the whole struct first so padding holes don't leak kernel stack
     * bytes to userspace when this is later copy_to_user'd. */
    memset(out, 0, sizeof(*out));

    /* process_get_by_pid() increments ref_count; we must call process_unref()
     * before returning.                                                       */
    process_t* proc = process_get_by_pid(pid);
    if (!proc) {
        return ESRCH;
    }

    /* --- populate proc_detail_t ------------------------------------------ */
    out->pid       = proc->pid;
    out->ppid      = proc->parent_pid;
    out->state     = (uint32_t)proc->state;
    out->prio      = (uint32_t)(uint32_t)(int32_t)proc->priority;
    out->cpu_ticks = proc->total_time;
    out->mem_pages = vma_page_count(proc);
    out->vma_count = vma_count_list(proc);

    /* Copy process name (proc->name is 64 chars; out->name is 32 chars).     */
    int k = 0;
    for (; k < 31 && proc->name[k]; k++) {
        out->name[k] = proc->name[k];
    }
    out->name[k] = '\0';

    process_unref(proc);
    return 0;
}

/* =========================================================================
 * procapi_ctl
 * ========================================================================= */

int procapi_ctl(uint32_t pid, uint32_t verb, uint64_t arg)
{
    /* Protect init and idle from any control operation. */
    if (pid == 0 || pid == 1) {
        return EPERM;
    }

    if (verb > PROCAPI_CTL_SET_PRIORITY) {
        return EINVAL;
    }

    process_t* proc = process_get_by_pid(pid);
    if (!proc) {
        return ESRCH;
    }

    /* Authorization: a process may control another only with the same UID; root
     * (uid 0) may control anyone. Mirrors the check in sys_kill (kill.c) -- this
     * path applies the same suspend/resume/kill/reprioritize operations and must
     * gate them the same way. Today everything runs as uid 0 so this allows all
     * callers, but it closes the cross-owner control gap under a future MU model. */
    {
        process_t* caller = process_get_current();
        if (caller && caller->uid != 0 && caller->uid != proc->uid) {
            return EPERM;
        }
    }

    switch (verb) {

    case PROCAPI_CTL_SUSPEND:
        /*
         * Mirror SIGSTOP: if the process is running or runnable, move it to
         * PROCESS_BLOCKED.  The scheduler naturally skips blocked processes.
         * Identical logic to kill.c case SIGSTOP.
         */
        kprintf("[PROCAPI] Suspending PID %u (%s)\n", proc->pid, proc->name);
        if (proc->state == PROCESS_RUNNING || proc->state == PROCESS_READY) {
            proc->state = PROCESS_BLOCKED;
        }
        break;

    case PROCAPI_CTL_RESUME:
        /*
         * Mirror SIGCONT: lift BLOCKED -> READY so the scheduler re-admits it.
         * Identical logic to kill.c case SIGCONT.
         */
        kprintf("[PROCAPI] Resuming PID %u (%s)\n", proc->pid, proc->name);
        if (proc->state == PROCESS_BLOCKED) {
            process_set_ready(proc);
        }
        break;

    case PROCAPI_CTL_KILL:
        /*
         * Mirror SIGKILL: mark terminated and remove from the run queue.
         * Identical logic to kill.c case SIGKILL.
         */
        kprintf("[PROCAPI] Killing PID %u (%s)\n", proc->pid, proc->name);
        proc->state = PROCESS_TERMINATED;
        proc->exit_status = 128 + 9;   // SIGKILL-equivalent
        process_on_terminate(proc);    // wake a waitpid'ing parent
        scheduler_remove_process(proc);
        break;

    case PROCAPI_CTL_SET_PRIORITY: {
        /*
         * Clamp and apply a new nice value.  Mirrors nice.c / sys_setpriority.
         * `arg` carries the desired priority as a signed value in the low 32 bits.
         */
        int32_t new_prio = (int32_t)(uint32_t)(arg & 0xFFFFFFFFU);
        if (new_prio < PA_PRIO_MIN) new_prio = PA_PRIO_MIN;
        if (new_prio > PA_PRIO_MAX) new_prio = PA_PRIO_MAX;
        kprintf("[PROCAPI] Set priority PID %u (%s): %d -> %d\n",
                proc->pid, proc->name, proc->priority, new_prio);
        proc->priority = new_prio;
        break;
    }

    default:
        process_unref(proc);
        return EINVAL;
    }

    process_unref(proc);
    return 0;
}

/* =========================================================================
 * procapi_sysinfo
 * ========================================================================= */

int procapi_sysinfo(sysinfo_t* out)
{
    if (!out) {
        return EINVAL;
    }

    /*
     * Count live processes by scanning through proc_info snapshots.
     * process_list() acquires the process table lock internally, so it is
     * safe to call without additional locking here.
     *
     * We use a stack-local buffer of 256 entries (matching MAX_PROCESSES in
     * process.c).  Each proc_info_t is 64 bytes -> 64 * 256 = 16 KB.
     * The kernel stack is 8 KB, which would overflow.  We therefore declare
     * the buffer static (BSS) and guard the implicit single-threaded use with
     * the fact that this path is only ever called from the syscall handler
     * (which runs with interrupts disabled on the kernel stack).
     *
     * If true concurrency is ever needed, kmalloc / a global scratch buffer
     * protected by a dedicated spinlock should replace the static array.
     */
    static proc_info_t _pa_snap[256];
    int n = process_list(_pa_snap, 256);
    if (n < 0) {
        n = 0;
    }

    out->total_mem  = pmm_get_total_memory();
    out->free_mem   = pmm_get_free_memory();
    out->uptime_ms  = timer_get_ticks_ms();
    out->proc_count = (uint32_t)n;
    out->_pad       = 0;

    return 0;
}
