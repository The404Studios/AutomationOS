#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/string.h"
#include "../../include/x86_64.h"
#include "../../include/namespace.h"
#include "../../include/spinlock.h"
#include "../../include/vma.h"
#include "../../include/ipc.h"

/* Verbose per-process creation logging dominates boot serial I/O; gate it. */
#ifdef PROCESS_QUIET
#define PROC_LOG(...) ((void)0)
#else
#define PROC_LOG(...) kprintf(__VA_ARGS__)
#endif

#define MAX_PROCESSES 256

// Process table
static process_t* process_table[MAX_PROCESSES];
// PID allocation bitmap (0 = free, 1 = in use). Index 0 reserved for kernel/idle
// and never handed out. Replaces the old monotonic next_pid, which only ever
// incremented and panicked after MAX_PROCESSES creations — a guaranteed crash
// for any long-running session that spawns/closes apps. PIDs are reclaimed on
// process death (process_unref), so the pool truly cycles.
static uint8_t pid_used[MAX_PROCESSES];
process_t* current_process = NULL;

// RACE-002 fix: Global process table lock protects process_table[] and pid_used[]
static spinlock_t process_table_lock;

// Allocate a free PID by scanning the bitmap under the table lock. Returns a PID
// in [1, MAX_PROCESSES) on success, or 0 if the table is full (caller returns
// NULL rather than panicking).
static uint32_t allocate_pid(void) {
    spin_lock(&process_table_lock);
    for (uint32_t i = 1; i < MAX_PROCESSES; i++) {
        if (!pid_used[i]) {
            pid_used[i] = 1;
            spin_unlock(&process_table_lock);
            return i;
        }
    }
    spin_unlock(&process_table_lock);
    return 0;  // no free PID
}

// Return a PID to the pool so it can be reused.
static void free_pid(uint32_t pid) {
    if (pid == 0 || pid >= MAX_PROCESSES) return;
    spin_lock(&process_table_lock);
    pid_used[pid] = 0;
    spin_unlock(&process_table_lock);
}

void process_init(void) {
    PROC_LOG("[PROCESS] Initializing process management...\n");

    // RACE-002 fix: Initialize process table lock
    spin_lock_init(&process_table_lock);

    // Clear process table
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i] = NULL;
    }

    PROC_LOG("[PROCESS] Process table initialized (max %d processes, SMP-safe)\n", MAX_PROCESSES);
}

process_t* process_create(const char* name, void* entry_point) {
    // Allocate PID first (before any allocations)
    uint32_t pid = allocate_pid();
    if (pid == 0) {
        PROC_LOG("[PROCESS] No free PID — process table full\n");
        return NULL;
    }

    // Allocate process structure
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) {
        PROC_LOG("[PROCESS] Failed to allocate process structure\n");
        free_pid(pid);  // LEAK-004 fix: Return PID to pool
        return NULL;
    }

    // Zero the ENTIRE PCB first. kmalloc does not zero, so without this the
    // security/scheduling fields that are not explicitly assigned below
    // (uid, gid, capabilities, rlimits, seccomp_*, vmm, exe_path, resume_mode,
    // need_resched, on_queue) would hold heap garbage. A garbage resume_mode
    // under the preemptive path can iretq into an arbitrary RIP; garbage
    // uid/gid silently corrupts IPC permission checks. memset is the single
    // root-cause fix for that whole class of uninitialized-field bugs.
    memset(proc, 0, sizeof(process_t));

    // Initialize process structure
    proc->pid = pid;
    proc->parent_pid = current_process ? current_process->pid : 0;
    proc->state = PROCESS_CREATED;
    proc->resume_mode = RESUME_CRETURN;  // new processes resume via the C-return path
    // Inherit credentials from the creating process (root/0 for early procs).
    proc->uid = current_process ? current_process->uid : 0;
    proc->gid = current_process ? current_process->gid : 0;

    // Initialize time_slice to a full quantum (SCHED_QUANTUM_TICKS, == the
    // cooperative DEFAULT_TIME_SLICE of 10). Two reasons:
    //   (1) Cooperative path: unchanged behavior. scheduler_pick_next() only
    //       refills when time_slice==0, so a fresh process keeps its 10 here and
    //       still runs exactly one 10-tick quantum on first pick -- identical to
    //       starting at 0 and being refilled to 10.
    //   (2) Preemptive path (-DPREEMPTIVE): schedule_from_irq decrements the
    //       quantum each tick. Starting at a sane non-zero value avoids any
    //       first-quantum underflow if a process ever became current without
    //       passing through pick_next's refill. Defensive, costs nothing.
    proc->time_slice = SCHED_QUANTUM_TICKS;

    proc->total_time = 0;
    proc->priority = 0;  // Default priority (nice value 0)
    proc->next = NULL;
    proc->ref_count = 1;  // Start with 1 reference
    proc->vma_list = NULL;  // No memory regions tracked yet (loader installs them)

    // Copy name (BUG-010 fix: validate name pointer)
    if (!name) {
        proc->name[0] = '?';
        proc->name[1] = '\0';
    } else {
        size_t name_len = 0;
        while (name[name_len] && name_len < 63) {
            proc->name[name_len] = name[name_len];
            name_len++;
        }
        proc->name[name_len] = '\0';
    }

    // Allocate kernel stack (8KB, contiguous via kmalloc)
    proc->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!proc->kernel_stack) {
        PROC_LOG("[PROCESS] Failed to allocate kernel stack for PID %d\n", pid);
        kfree(proc);
        free_pid(pid);
        return NULL;
    }

    uint64_t kstack_top = (uint64_t)proc->kernel_stack + KERNEL_STACK_SIZE;

    // Setup initial CPU context
    memset(&proc->context, 0, sizeof(cpu_context_t));
    proc->context.rip = (uint64_t)entry_point;  // Instruction pointer
    proc->context.rsp = kstack_top - 16;         // Stack pointer (aligned)
    proc->context.rflags = 0x202;                // IF (interrupts enabled)

    // Per-process address space
    proc->context.cr3 = paging_create_address_space();
    if (!proc->context.cr3) {
        PROC_LOG("[PROCESS] Failed to create address space for PID %d\n", pid);
        kfree(proc->kernel_stack);  // kernel_stack is kmalloc'd, not a PMM page
        kfree(proc);
        free_pid(pid);
        return NULL;
    }
    PROC_LOG("[PROCESS] Created address space CR3=0x%016lx for PID %d\n",
            proc->context.cr3, pid);

    // ADDRESS-SPACE REFCOUNT (threads): a normally-created process (this path is
    // also taken by fork, which gets a FRESH address space) is the sole user of
    // its CR3, so start a fresh as_refcount at 1. thread_create() shares this
    // CR3 + this same as_refcount pointer and bumps it; process_unref() only
    // tears the CR3 down when it returns to 0. If the small allocation fails we
    // must abort cleanly (a NULL as_refcount would make teardown undecidable).
    proc->as_refcount = (int*)kmalloc(sizeof(int));
    if (!proc->as_refcount) {
        PROC_LOG("[PROCESS] Failed to allocate as_refcount for PID %d\n", pid);
        vmm_as_release(proc->context.cr3);
        paging_destroy_address_space(proc->context.cr3);
        kfree(proc->kernel_stack);
        kfree(proc);
        free_pid(pid);
        return NULL;
    }
    *proc->as_refcount = 1;

    // A normally-created process is its OWN thread-group leader: tgid == pid.
    // thread_create() overrides tgid to the creator's tgid for threads.
    proc->tgid = pid;
    proc->is_thread = 0;

    // TODO: Setup user stack once we have userspace
    proc->user_stack = NULL;

    // Initialize namespaces (optional - skip if namespace system not ready)
    proc->namespaces = NULL;
    if (current_process && current_process->namespaces) {
        proc->namespaces = namespace_clone_container(current_process->namespaces, 0);
    }
    // Note: namespaces are optional for early processes. Init runs without them.

    if (0 /* namespaces are optional */) {
        PROC_LOG("[PROCESS] Failed to create namespaces for PID %d\n", pid);
        kfree(proc->kernel_stack);  // kernel_stack is kmalloc'd, not a PMM page
        kfree(proc);
        free_pid(pid);
        return NULL;
    }

    // Initialize sandbox flags
    proc->sandbox_flags = 0;

    // RACE-002 fix: Add to process table with lock protection
    spin_lock(&process_table_lock);
    process_table[pid] = proc;
    spin_unlock(&process_table_lock);

    PROC_LOG("[PROCESS] Created process '%s' (PID %d)\n", proc->name, proc->pid);
    PROC_LOG("[PROCESS]   Entry point: %p\n", entry_point);
    PROC_LOG("[PROCESS]   State: %d (CREATED=%d, READY=%d)\n",
            proc->state, PROCESS_CREATED, PROCESS_READY);
    PROC_LOG("[PROCESS]   Kernel stack: %p - %p\n",
            proc->kernel_stack, (void*)((uint64_t)proc->kernel_stack + PAGE_SIZE));
    PROC_LOG("[PROCESS]   Context: RIP=%p RSP=%p RFLAGS=0x%lx CR3=0x%lx\n",
            (void*)proc->context.rip, (void*)proc->context.rsp,
            proc->context.rflags, proc->context.cr3);
    PROC_LOG("[PROCESS]   Time slice: %d, Ref count: %d\n",
            proc->time_slice, proc->ref_count);
    PROC_LOG("[PROCESS]   Namespaces: %p\n", proc->namespaces);

    return proc;
}

// ===========================================================================
// thread_create — a schedulable task that SHARES the parent's address space
// ===========================================================================
// A thread is a process_t that is identical to a forked process EXCEPT:
//   (1) it does NOT call paging_create_address_space(): it copies the parent's
//       context.cr3 and SHARES the parent's as_refcount (atomically bumped), so
//       the page tables stay alive until the LAST thread/process on them exits.
//   (2) it has its OWN kernel stack, user stack (caller-supplied), GP regs and
//       FPU state — only the address space is shared.
//   (3) it begins executing entry(arg) in ring 3: its first dispatch goes
//       through thread_enter_usermode_trampoline (RESUME_CRETURN, exactly like a
//       new process), which loads thread_arg into RDI before enter_usermode_thread.
// Returns the new thread PCB (READY, NOT yet scheduled — caller schedules it), or
// NULL on failure. The caller (sys_thread_create) holds the parent ref.
process_t* thread_create(process_t* parent, uint64_t entry, uint64_t arg,
                         uint64_t user_stack) {
    if (!parent || !parent->context.cr3 || !parent->as_refcount) {
        return NULL;
    }

    uint32_t pid = allocate_pid();
    if (pid == 0) {
        PROC_LOG("[THREAD] No free PID — process table full\n");
        return NULL;
    }

    process_t* t = (process_t*)kmalloc(sizeof(process_t));
    if (!t) {
        free_pid(pid);
        return NULL;
    }
    memset(t, 0, sizeof(process_t));

    t->pid = pid;
    t->parent_pid = parent->pid;
    t->state = PROCESS_CREATED;
    t->resume_mode = RESUME_CRETURN;     // first run via the trampoline `ret` path
    t->uid = parent->uid;
    t->gid = parent->gid;
    t->time_slice = SCHED_QUANTUM_TICKS;
    t->total_time = 0;
    t->priority = parent->priority;      // inherit scheduling class
    t->next = NULL;
    t->ref_count = 1;
    t->vma_list = NULL;                  // thread's own (empty) PCB VMA list; the
                                         // real mappings live in the shared CR3.

    // Name: "<parent>-thr" (truncated). Purely cosmetic.
    {
        size_t k = 0;
        while (parent->name[k] && k < 58) { t->name[k] = parent->name[k]; k++; }
        const char* suf = "-thr";
        for (int s = 0; suf[s] && k < 63; s++) t->name[k++] = suf[s];
        t->name[k] = '\0';
    }

    // Own kernel stack (8KB, kmalloc'd) — independent from the parent's.
    t->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!t->kernel_stack) {
        PROC_LOG("[THREAD] Failed to allocate kernel stack for TID %d\n", pid);
        kfree(t);
        free_pid(pid);
        return NULL;
    }

    // SHARE the parent's address space: same CR3, same as_refcount, +1 owner.
    // This is the crux of "a thread shares its parent's address space" and of the
    // teardown-safety contract: the CR3 now has (at least) two users, and
    // process_unref() will only destroy it when this count returns to 0.
    memset(&t->context, 0, sizeof(cpu_context_t));
    t->context.cr3 = parent->context.cr3;
    t->as_refcount = parent->as_refcount;
    __atomic_add_fetch(t->as_refcount, 1, __ATOMIC_SEQ_CST);

    // Thread group: threads of a process share the parent's tgid.
    t->tgid = parent->tgid;
    t->is_thread = 1;

    // Stash the entry argument; the trampoline loads it into RDI (SysV: entry(arg)).
    t->thread_arg = arg;
    // 16-align the user stack pointer (ABI: RSP%16==0 at the iretq into entry).
    t->user_entry = entry;
    t->user_rsp = user_stack & ~0xFULL;

    // First-run kernel context: context_switch_asm `ret`s into the thread
    // trampoline (RESUME_CRETURN), which reads user_entry/user_rsp/thread_arg/cr3
    // from this PCB and enter_usermode_thread()s to ring 3 with RDI = arg.
    extern void thread_enter_usermode_trampoline(void);
    t->context.rip = (uint64_t)thread_enter_usermode_trampoline;
    uint64_t kstack_top = (uint64_t)t->kernel_stack + KERNEL_STACK_SIZE;
    t->context.rsp = kstack_top - 8;     // room for the trampoline's return addr
    t->context.rflags = 0x202;           // IF=1

    // Initialize the join wait_object so a joiner can block on it.
    wait_object_init(&t->thread_join_wo);

    t->state = PROCESS_READY;

    // Publish into the process table so process_get_by_pid()/join can find it.
    spin_lock(&process_table_lock);
    process_table[pid] = t;
    spin_unlock(&process_table_lock);

    PROC_LOG("[THREAD] Created thread TID %d (tgid %d) sharing CR3=0x%016lx "
             "entry=0x%lx arg=0x%lx ustack=0x%lx (AS refs now %d)\n",
             t->pid, t->tgid, t->context.cr3, entry, arg, t->user_rsp,
             *t->as_refcount);

    return t;
}

// Increment process reference count
void process_ref(process_t* proc) {
    if (proc) {
        // BUG-008 fix: Use atomic increment (SMP-safe, no cli/sti needed)
        __atomic_add_fetch(&proc->ref_count, 1, __ATOMIC_SEQ_CST);
    }
}

// A process just became TERMINATED (via exit, signal, fault, or rlimit kill).
// Centralizes the two things every terminate path must do for blocking
// waitpid() to work:
//   1. Wake its parent if the parent is blocked in waitpid(), so the zombie is
//      reaped (and, for init, the service restarted). Without this, a process
//      killed by anything other than its own sys_exit() (e.g. force-quit via
//      SYS_KILL, task-manager kill, a fault, or an rlimit kill) would never wake
//      a parent sleeping in waitpid().
//   2. Drain the process's OWN child_wait queue: if it was itself blocked in
//      waitpid() when killed, the self-referencing wait entry holds a ref that
//      would otherwise keep its PCB alive forever (leak). wq_wake_all dequeues
//      the entry, sees the process is no longer BLOCKED, and releases that ref.
void process_on_terminate(process_t* child) {
    if (!child) return;
    if (child->parent_pid) {
        process_t* parent = process_get_by_pid(child->parent_pid);
        if (parent) {
            if (parent->child_wait) {
                wq_wake_one((wait_queue_t*)parent->child_wait);
            }
            process_unref(parent);
        }
    }
    if (child->child_wait) {
        wq_wake_all((wait_queue_t*)child->child_wait);
    }
}

// Decrement process reference count and free if zero
void process_unref(process_t* proc) {
    if (!proc) return;

    // BUG-008 fix: Use atomic decrement (SMP-safe)
    // Only ONE CPU will see the transition to 0
    uint32_t old_count = __atomic_sub_fetch(&proc->ref_count, 1, __ATOMIC_SEQ_CST);

    if (old_count == 0) {
        // Only the CPU that decremented from 1 to 0 will enter here
        // This prevents double-free race condition
        PROC_LOG("[PROCESS] Freeing process '%s' (PID %d) - ref_count reached 0\n",
                proc->name, proc->pid);

        // RACE-002 fix: Remove from process table with lock protection, and
        // return the PID to the pool so it can be reused (prevents the eventual
        // "table full" panic on long-running, app-churning sessions).
        if (proc->pid < MAX_PROCESSES) {
            spin_lock(&process_table_lock);
            process_table[proc->pid] = NULL;
            pid_used[proc->pid] = 0;
            spin_unlock(&process_table_lock);
        }

        // Release IPC resources owned by this process (SysV shm/msg). Must run
        // while proc->pid is still valid and BEFORE the address space is torn
        // down, so deferred-destroy segments are accounted correctly.
        shm_cleanup_process(proc->pid);
        msg_cleanup_process(proc->pid);

        // Close any file descriptors the process still had open (per-process fd
        // table) so their inodes/file structs don't leak. fds 0/1/2 are stdio
        // and not in the table. Safe here: still single-threaded teardown.
        extern void vfs_close_all_fds(struct process* proc);
        vfs_close_all_fds(proc);

        // Free the lazily-allocated waitpid wait queue, if any.
        if (proc->child_wait) {
            kfree(proc->child_wait);
            proc->child_wait = NULL;
        }

        // Free kernel stack
        if (proc->kernel_stack) {
            kfree(proc->kernel_stack);  // kernel_stack is kmalloc'd, not a PMM page
        }

        // Free page directory (CR3) - destroy user-space page tables.
        //
        // THREADS / ADDRESS-SPACE LIFETIME (critical): the CR3 may be SHARED by
        // several process_t (the main process + its threads), all pointing at the
        // SAME as_refcount. We must tear the address space down EXACTLY ONCE, when
        // the LAST user exits — freeing the page tables while a sibling thread is
        // still running on them is an instant triple-fault. So we atomically
        // decrement *as_refcount here and only destroy the CR3 (and free the
        // refcount cell) when it hits 0. A NULL as_refcount can only happen for a
        // PCB that never finished process_create(); treat it as "last owner".
        int last_user = 1;
        if (proc->as_refcount) {
            last_user = (__atomic_sub_fetch(proc->as_refcount, 1, __ATOMIC_SEQ_CST) == 0);
        }

        if (proc->context.cr3 && last_user) {
            uint64_t kernel_cr3 = read_cr3() & ~0xFFF;  // Mask off PCID bits
            uint64_t proc_cr3 = proc->context.cr3 & ~0xFFF;

            // Only destroy if it's not the kernel's CR3
            if (proc_cr3 != kernel_cr3) {
                PROC_LOG("[PROCESS] Destroying address space CR3=0x%016lx for PID %d\n",
                        proc->context.cr3, proc->pid);
                vmm_as_release(proc->context.cr3);  // free the mmap cursor slot before CR3 dies
                paging_destroy_address_space(proc->context.cr3);
            }
            // Free the shared as_refcount cell now that the last user is gone.
            if (proc->as_refcount) {
                kfree(proc->as_refcount);
            }
        }
        // A non-last thread leaves the CR3 and as_refcount intact for the
        // survivors; it must NOT touch either (no vmm_as_release, no free).
        proc->as_refcount = NULL;  // this PCB no longer references the cell

        // TODO: Free user stack

        // Free the per-process VMA list
        vma_clear(proc);

        // Destroy namespace container
        if (proc->namespaces) {
            namespace_destroy_container(proc->namespaces);
        }

        // Free process structure
        kfree(proc);
    }
}

void process_destroy(process_t* proc) {
    if (!proc) return;

    PROC_LOG("[PROCESS] Destroying process '%s' (PID %d)\n", proc->name, proc->pid);

    // Remove from scheduler first (releases its reference)
    scheduler_remove_process(proc);

    // Release our reference
    process_unref(proc);
}

process_t* process_get_by_pid(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return NULL;
    }

    // RACE-002 fix: Read process_table[] with lock AND take reference
    // This prevents the process from being freed while caller uses it
    spin_lock(&process_table_lock);
    process_t* proc = process_table[pid];
    if (proc) {
        process_ref(proc);  // Increment ref count before returning
    }
    spin_unlock(&process_table_lock);

    return proc;

    // IMPORTANT: Callers MUST call process_unref() when done with the returned process!
}

process_t* process_get_current(void) {
    return current_process;
}

void process_set_current(process_t* proc) {
    current_process = proc;
    if (proc) {
        proc->state = PROCESS_RUNNING;
        // Single chokepoint hit by EVERY dispatch (cooperative schedule/yield/wq
        // AND preemptive resume/first-dispatch): count one context switch per
        // dispatch. Counter only; does not alter switch logic.
        proc->ctx_switches++;
    }
}

// Snapshot the live process table for SYS_PROCLIST. Returns the number filled.
int process_list(proc_info_t* out, int max) {
    if (!out || max <= 0) {
        return 0;
    }
    int n = 0;
    spin_lock(&process_table_lock);
    for (int i = 0; i < MAX_PROCESSES && n < max; i++) {
        process_t* p = process_table[i];
        if (!p) {
            continue;
        }
        out[n].pid = p->pid;
        out[n].parent_pid = p->parent_pid;
        out[n].state = (uint32_t)p->state;
        out[n].flags = 0;
        int k = 0;
        for (; k < 31 && p->name[k]; k++) {
            out[n].name[k] = p->name[k];
        }
        out[n].name[k] = '\0';
        // Scheduler stats (appended fields, SYS_PROCLIST 64-byte ABI).
        out[n].cpu_ticks = p->cpu_ticks;
        out[n].ctx_switches = p->ctx_switches;
        n++;
    }
    spin_unlock(&process_table_lock);
    return n;
}
