/*
 * kernel/include/procapi.h
 * Process Introspection + Control API — kernel-internal interface
 *
 * Three functions are wired by the integrator to syscalls:
 *   SYS_PROC_QUERY = 60  ->  procapi_query()
 *   SYS_PROC_CTL   = 61  ->  procapi_ctl()
 *   SYS_SYSINFO    = 62  ->  procapi_sysinfo()
 *
 * ABI STABILITY: proc_detail_t and sysinfo_t must not be reordered once
 * userspace libraries are built against them; add new fields at the end only.
 */

#ifndef PROCAPI_H
#define PROCAPI_H

#include "types.h"

/* -------------------------------------------------------------------------
 * proc_detail_t  —  per-process rich snapshot
 *
 * Byte layout (all fields little-endian, no implicit padding):
 *   offset  0  uint32_t  pid          4 bytes
 *   offset  4  uint32_t  ppid         4 bytes
 *   offset  8  uint32_t  state        4 bytes   (process_state_t value)
 *   offset 12  uint32_t  prio         4 bytes   (signed nice value cast to u32)
 *   offset 16  uint64_t  cpu_ticks    8 bytes   (total_time from PCB)
 *   offset 24  uint32_t  mem_pages    4 bytes   (cr3 address space pages, 0=unknown)
 *   offset 28  uint32_t  vma_count    4 bytes   (number of VMAs in vma_list)
 *   offset 32  char[32]  name        32 bytes   (NUL-terminated process name)
 *   -------
 *   total: 64 bytes
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t pid;           /*  0 — process ID                              */
    uint32_t ppid;          /*  4 — parent process ID                       */
    uint32_t state;         /*  8 — process_state_t cast to uint32_t        */
    uint32_t prio;          /* 12 — priority (int32_t nice reinterpreted)   */
    uint64_t cpu_ticks;     /* 16 — total CPU ticks consumed (total_time)   */
    uint32_t mem_pages;     /* 24 — VMA-backed page count (0 = unavailable) */
    uint32_t vma_count;     /* 28 — number of vma_t entries in vma_list     */
    char     name[32];      /* 32 — process name, NUL-terminated            */
    /* Total: 64 bytes */
} proc_detail_t;

/* -------------------------------------------------------------------------
 * sysinfo_t  —  system-wide resource snapshot
 *
 * Byte layout:
 *   offset  0  uint64_t  total_mem      8 bytes   (bytes, from pmm_get_total_memory)
 *   offset  8  uint64_t  free_mem       8 bytes   (bytes, from pmm_get_free_memory)
 *   offset 16  uint64_t  uptime_ms      8 bytes   (milliseconds, timer_get_ticks_ms)
 *   offset 24  uint32_t  proc_count     4 bytes   (live processes in table)
 *   offset 28  uint32_t  heap_used_kb   4 bytes   (kernel heap usage in KiB)
 *   -------
 *   total: 32 bytes
 *
 * ABI note: heap_used_kb replaces the former _pad field (which was always 0).
 * Old userspace ignoring this field is unaffected; new userspace can read it
 * to detect kernel heap leaks (monitor growth over time).
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t total_mem;     /*  0 — total physical memory in bytes          */
    uint64_t free_mem;      /*  8 — free physical memory in bytes           */
    uint64_t uptime_ms;     /* 16 — milliseconds since boot                 */
    uint32_t proc_count;    /* 24 — number of live processes                */
    uint32_t heap_used_kb;  /* 28 — kernel heap bytes in use / 1024         */
    /* Total: 32 bytes */
} sysinfo_t;

/* -------------------------------------------------------------------------
 * procapi_ctl() verb codes
 * ------------------------------------------------------------------------- */
#define PROCAPI_CTL_SUSPEND      0   /* SIGSTOP — freeze scheduling          */
#define PROCAPI_CTL_RESUME       1   /* SIGCONT — unfreeze                   */
#define PROCAPI_CTL_KILL         2   /* SIGKILL — terminate immediately       */
#define PROCAPI_CTL_SET_PRIORITY 3   /* arg = new nice value (int32_t)        */

/* -------------------------------------------------------------------------
 * Public kernel API (implemented in kernel/core/procapi/procapi.c)
 * ------------------------------------------------------------------------- */

/*
 * procapi_query - fill *out with a rich snapshot of process `pid`.
 *
 * Returns  0           on success (out is valid).
 *         -ESRCH (−3)  if no process with that PID exists.
 *         -EINVAL(−22) if out is NULL or pid is out of range.
 */
int procapi_query(uint32_t pid, proc_detail_t* out);

/*
 * procapi_ctl - control a running process.
 *
 * verb = PROCAPI_CTL_SUSPEND      suspend the process (SIGSTOP)
 *      = PROCAPI_CTL_RESUME       resume  the process (SIGCONT)
 *      = PROCAPI_CTL_KILL         kill    the process (SIGKILL)
 *      = PROCAPI_CTL_SET_PRIORITY set priority; arg = desired nice value
 *
 * PIDs 0 and 1 are protected: returns -EPERM (−1) for them.
 *
 * Returns  0            on success.
 *         -ESRCH  (−3)  process not found.
 *         -EINVAL (−22) unknown verb.
 *         -EPERM  (−1)  operation not permitted (pid 0/1).
 */
int procapi_ctl(uint32_t pid, uint32_t verb, uint64_t arg);

/*
 * procapi_sysinfo - fill *out with a system-wide resource snapshot.
 *
 * Returns  0           on success.
 *         -EINVAL(−22) if out is NULL.
 */
int procapi_sysinfo(sysinfo_t* out);

#endif /* PROCAPI_H */
