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

// Monotonic creation stamp for stable process identity (#10). Incremented under
// process_table_lock each time a PCB is published, so (pid, create_seq) uniquely
// identifies an incarnation even after the PID is recycled. Never reused/reset.
static uint64_t g_create_seq = 0;

// Allocate a free PID by scanning the bitmap under the table lock. Returns a PID
// in [1, MAX_PROCESSES) on success, or 0 if the table is full (caller returns
// NULL rather than panicking).
static uint32_t allocate_pid(void) {
    // IRQ-safe: process_unref() may run in hard-IRQ context and now ALWAYS takes
    // process_table_lock (TOCTOU fix). A plain spin_lock here would let an IRQ that
    // unrefs self-deadlock against this same-CPU holder. spin_lock_irqsave masks
    // interrupts for the (short) duration we hold the lock.
    uint64_t flags;
    spin_lock_irqsave(&process_table_lock, &flags);
    for (uint32_t i = 1; i < MAX_PROCESSES; i++) {
        if (!pid_used[i]) {
            pid_used[i] = 1;
            spin_unlock_irqrestore(&process_table_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&process_table_lock, flags);
    return 0;  // no free PID
}

// Return a PID to the pool so it can be reused.
static void free_pid(uint32_t pid) {
    if (pid == 0 || pid >= MAX_PROCESSES) return;
    // IRQ-safe (see allocate_pid): IRQ-context process_unref() also takes this lock.
    uint64_t flags;
    spin_lock_irqsave(&process_table_lock, &flags);
    pid_used[pid] = 0;
    spin_unlock_irqrestore(&process_table_lock, flags);
}

// Relocate an already-created process onto the reserved PID 0 (the idle thread).
// allocate_pid() never hands out 0 (it scans from index 1), so the FIRST
// process_create() — the per-CPU idle thread in scheduler_init() — is given PID 1
// and bumps /sbin/init to PID 2. init.c hard-requires getpid()==1 and exits(1)
// otherwise ("Not PID 1!"), so we re-home idle into slot 0: free its allocated
// PID back to the pool, reserve slot 0, and move its table entry. After this the
// first real allocate_pid() returns 1, which init receives. No-op for a NULL arg
// or a process already at PID 0.
void process_adopt_pid0(process_t* proc) {
    if (!proc || proc->pid == 0) return;
    // IRQ-safe (see allocate_pid): IRQ-context process_unref() also takes this lock.
    uint64_t flags;
    spin_lock_irqsave(&process_table_lock, &flags);
    uint32_t old = proc->pid;
    if (old < MAX_PROCESSES && process_table[old] == proc) {
        process_table[old] = NULL;
        pid_used[old] = 0;
    }
    proc->pid = 0;
    pid_used[0] = 1;            // keep slot 0 reserved (allocate_pid never returns 0)
    process_table[0] = proc;
    spin_unlock_irqrestore(&process_table_lock, flags);
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

// ===========================================================================
// process_cleanup — teardown counterpart to process_init()
// ===========================================================================
// Walks the entire process table and forcibly terminates/frees all live
// processes, preventing PCB + address-space leaks on kernel shutdown. This
// is the single-point cleanup that process_init() was missing.
//
// Safe teardown sequence per process:
//   1. Mark TERMINATED (cooperative paths stop scheduling it)
//   2. Remove from scheduler (releases scheduler's reference)
//   3. Unref our table reference (triggers full PCB teardown if ref==0)
//
// Two-phase approach: Phase 1 collects all live processes under the lock (with
// extra refs so they don't vanish mid-cleanup). Phase 2 releases the lock and
// performs the complex teardown (process_on_terminate, scheduler_remove_process,
// process_unref) without holding locks.
//
// Called from kernel shutdown/halt path. Runs with interrupts disabled (no
// concurrent process_create/destroy). The table lock is still acquired out
// of discipline (safe even at shutdown), but no real contention is expected.
void process_cleanup(void) {
    PROC_LOG("[PROCESS] Cleaning up process table...\n");

    // Phase 1: collect all live processes under the lock
    process_t* to_cleanup[MAX_PROCESSES];
    int cleanup_count = 0;

    // IRQ-safe (see allocate_pid): IRQ-context process_unref() also takes this lock.
    uint64_t flags;
    spin_lock_irqsave(&process_table_lock, &flags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* proc = process_table[i];
        if (proc) {
            // Take an extra ref so the process doesn't vanish while we work on it
            process_ref(proc);
            to_cleanup[cleanup_count++] = proc;

            // Mark TERMINATED
            proc->state = PROCESS_TERMINATED;

            // Clear table slot and free PID
            process_table[i] = NULL;
            pid_used[i] = 0;
        }
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    // Phase 2: outside the lock, terminate and unref each process
    for (int i = 0; i < cleanup_count; i++) {
        process_t* proc = to_cleanup[i];

        PROC_LOG("[PROCESS] Cleaning up PID %d (%s) refcount=%d\n",
                 proc->pid, proc->name, proc->ref_count);

        // Wake waiters (parent blocked in waitpid, or own child_wait queue)
        process_on_terminate(proc);

        // Try to remove from scheduler (if it was enqueued). This releases
        // the scheduler's reference. Safe even if already removed.
        extern void scheduler_remove_process(process_t* proc);
        scheduler_remove_process(proc);

        // #9 fix: release the CREATION ref too. A never-reaped process at shutdown
        // still holds it, so without this the final leak scan below reports it as
        // "still in table". CAS-guarded so a concurrent parent reaper can't double-
        // release, and it MUST run BEFORE the extra-ref unref below so the PCB
        // survives to be freed by that final unref.
        reap_claim_release(proc);

        // Drop the table's reference (our +ref above will be the last one after
        // scheduler_remove_process released its ref, so this should trigger the
        // final teardown)
        process_unref(proc);
    }

    // Final leak check: walk the table one more time to see if anything survived
    int leaked_count = 0;
    spin_lock_irqsave(&process_table_lock, &flags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] != NULL) {
            leaked_count++;
            PROC_LOG("[PROCESS] LEAK: PID %d still in table after cleanup\n", i);
        }
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    if (leaked_count > 0) {
        PROC_LOG("[PROCESS] WARNING: %d processes leaked after cleanup\n", leaked_count);
    }

    PROC_LOG("[PROCESS] Process cleanup complete: %d processes terminated\n",
             cleanup_count);
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

    // F3-2 affinity defaults — MUST follow the memset, which zeroed BOTH fields into
    // their trap states: allowed_cpus=0 means "allowed on NO cpu" (the validator would
    // trip for every task) and pinned_cpu=0 means "pinned to CPU0", NOT the unpinned
    // sentinel. Normal tasks are CPU0-only and unpinned. process_create is the funnel
    // for almost every PCB ctor (idle, exec, fork, health_monitor, ...), so this one
    // assignment closes the trap for all of them. The lone CPU1 test kthread OVERRIDES
    // these after process_create returns (see ap_spawn_test_kthread in scheduler.c).
    proc->allowed_cpus = (uint64_t)1 << 0;   // CPU0 only
    proc->pinned_cpu   = CPU_NONE;           // not pinned

    // Initialize process structure
    proc->pid = pid;
    proc->parent_pid = current_process ? current_process->pid : 0;
    // #10: snapshot the creator's stable identity so this child can later validate
    // its parent is the SAME incarnation (guards against a recycled parent PID).
    proc->parent_seq = current_process ? current_process->create_seq : 0;
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

    // RACE-002 fix: Add to process table with lock protection.
    // IRQ-safe (see allocate_pid): IRQ-context process_unref() also takes this lock.
    {
        uint64_t flags;
        spin_lock_irqsave(&process_table_lock, &flags);
        proc->create_seq = ++g_create_seq;   // stable-identity stamp (#10), under lock
        process_table[pid] = proc;
        spin_unlock_irqrestore(&process_table_lock, flags);
    }

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

    // F3-2 affinity defaults (post-memset, same trap as process_create). thread_create
    // has its OWN kmalloc+memset (does NOT funnel through process_create), so it must
    // set both here. A child thread takes the CPU0-only default (NOT the parent's mask)
    // -- conservative: no cross-cpu spread at F3-2.
    t->allowed_cpus = (uint64_t)1 << 0;   // CPU0 only
    t->pinned_cpu   = CPU_NONE;           // not pinned

    t->pid = pid;
    t->parent_pid = parent->pid;
    t->parent_seq = parent->create_seq;   // stable parent identity (#10)
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

    // Mark READY (CREATED->READY transition)
    process_set_ready(t);

    // Publish into the process table so process_get_by_pid()/join can find it.
    // IRQ-safe (see allocate_pid): IRQ-context process_unref() also takes this lock.
    {
        uint64_t flags;
        spin_lock_irqsave(&process_table_lock, &flags);
        t->create_seq = ++g_create_seq;   // stable-identity stamp (#10), under lock
        process_table[pid] = t;
        spin_unlock_irqrestore(&process_table_lock, flags);
    }

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

    // ---- Phase A: reparent this dying process's own children to init (PID 1).
    //
    // LOCKED DESIGN (#9): orphans are reparented-to-init; NO process self-reaps.
    // When a process dies, every child that still names it as parent must be handed
    // to init so init's waitpid(-1) loop (userspace/init/main.c) eventually harvests
    // the child's zombie and releases its creation ref via reap_claim_release.
    // Without this, a child that outlives its parent -- or is already a zombie when
    // the parent dies -- is NEVER reaped (init's scan matches only parent_pid==1, so
    // a grandchild keeps leaking). This is the load-bearing half of the leak fix.
    //
    // RECURSIVE-DEADLOCK AVOIDANCE: process_table_lock is a PLAIN, NON-RECURSIVE
    // spinlock. We must walk process_table[] under it to find our children, so we
    // MUST NOT call process_get_by_pid() (or anything that takes the same lock)
    // while holding it. We therefore touch process_table[i] DIRECTLY here, exactly
    // like process_cleanup() does, and defer every lock-taking call (the parent wake
    // and the init wake, both of which use process_get_by_pid) to AFTER we drop the
    // lock. We only read/set scalar PCB fields (parent_pid, state) under the lock --
    // no allocation, no wq calls, no unref -- so the section is tiny and IRQ-safe
    // (acquired irqsave, matching every other holder).
    //
    // dying==init guard: if the dying process IS init (pid 1) we are on the shutdown
    // path; reparenting children to "1" would point them at the corpse and no
    // surviving reaper exists, so skip Phase A and the init-wake entirely.
    int reparented_zombie = 0;   // did we hand init an already-TERMINATED child?
    if (child->pid != 1) {
        // Fetch init's stable identity ONCE (outside the table lock) so reparented
        // orphans adopt BOTH parent_pid=1 AND parent_seq=<init's create_seq> -- then
        // the (pid, create_seq) identity check in Phase B and sys_waitpid holds for
        // them uniformly (no init special-case needed). (#10)
        uint64_t init_seq = 0;
        process_t* init_p = process_get_by_pid(1);
        if (init_p) { init_seq = init_p->create_seq; process_unref(init_p); }

        uint64_t flags;
        spin_lock_irqsave(&process_table_lock, &flags);
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t* p = process_table[i];
            // Skip empty slots and the dying process itself. Touch fields directly
            // -- NO process_get_by_pid here (it would re-take this same lock).
            if (!p || p == child) continue;
            if (p->parent_pid == child->pid) {
                p->parent_pid = 1;                       // reparent to init
                p->parent_seq = init_seq;                // adopt init's identity (#10)
                if (p->state == PROCESS_TERMINATED) {     // already a zombie
                    reparented_zombie = 1;                // init must be woken
                }
            }
        }
        spin_unlock_irqrestore(&process_table_lock, flags);
    }

    // ---- Phase B: wake this dying process's OWN parent (unchanged behavior), now
    // safely outside process_table_lock. process_get_by_pid()/process_unref() take
    // the lock internally -- fine, we hold nothing here.
    if (child->parent_pid) {
        process_t* parent = process_get_by_pid(child->parent_pid);
        if (parent) {
            // #10: only wake the parent if it is the SAME incarnation the child was
            // created under (pid + create_seq). A recycled PID now belonging to an
            // unrelated process has a different create_seq -- we must NOT poke its
            // child_wait (spurious wake / wrong-child reap). Reparented orphans carry
            // init's create_seq (set in Phase A), so init is matched correctly.
            // init (PID 1) is never recycled and is the universal reaper, so always
            // match it; otherwise require the stable-identity match. The pid-1
            // wildcard also covers the (unreachable in normal boot) init-absent
            // reparent edge where an orphan's parent_seq stayed 0.
            if ((child->parent_pid == 1 || parent->create_seq == child->parent_seq)
                && parent->child_wait) {
                wq_wake_one((wait_queue_t*)parent->child_wait);
            }
            process_unref(parent);
        }
    }

    // ---- Phase C: if we handed init an already-TERMINATED orphan, wake init's
    // child_wait so its waitpid(-1) loop re-scans and harvests the zombie. Done
    // outside the lock (process_get_by_pid takes it). Tolerate a missing init (NULL
    // pre-init / at shutdown) and a not-yet-allocated init->child_wait (init has
    // never blocked in waitpid yet -- its next scan finds the zombie anyway).
    if (reparented_zombie) {
        process_t* init_proc = process_get_by_pid(1);
        if (init_proc) {
            if (init_proc->child_wait) {
                wq_wake_one((wait_queue_t*)init_proc->child_wait);
            }
            process_unref(init_proc);
        }
    }

    // ---- Self-drain (unchanged): if this dying process was itself blocked in
    // waitpid(), its self-referencing wait entry holds a ref; wq_wake_all releases
    // it so the PCB can actually be freed.
    if (child->child_wait) {
        wq_wake_all((wait_queue_t*)child->child_wait);
    }
}

// Decrement process reference count and free if zero
void process_unref(process_t* proc) {
    if (!proc) return;

    // TOCTOU fix: do the FINAL decrement and the table-slot NULL in the SAME
    // critical section under process_table_lock.
    //
    // The old code decremented ref_count OUTSIDE the lock and then took the lock
    // only to NULL the slot. That left a window where the PCB was still published
    // in process_table[pid] with ref_count already at 0: a concurrent
    // process_get_by_pid() could take the lock, observe the still-published slot,
    // and process_ref() it back to 1 — resurrecting an object this CPU has already
    // committed to freeing. The unref'ing CPU then NULLs the slot and frees the
    // PCB, leaving the resurrecter with a dangling pointer (UAF / double-free).
    // Benign on the current uniprocessor build (only CPU0 schedules, so no other
    // context runs between the decrement and the lock) but a guaranteed UAF once
    // AP scheduling goes live.
    //
    // By decrementing UNDER the lock and NULLing the slot in the same section,
    // every process_get_by_pid() either (a) refs before our decrement — then it
    // observes ref_count > 0 and we do NOT free — or (b) runs after we NULL the
    // slot — then it sees NULL and returns NULL. There is no observe-and-ref gap.
    //
    // IRQ-SAFETY / DEADLOCK: process_unref() can be reached from a hard IRQ
    // (wait_object_signal() -> wo_wake_one_proc() -> process_unref(), documented
    // IRQ-safe in waitqueue.c). process_table_lock is a plain (non-IRQ) spinlock,
    // so if a non-IRQ holder is interrupted by an IRQ-context unref that takes the
    // same lock, the CPU self-deadlocks. We therefore acquire it IRQ-safe here
    // (spin_lock_irqsave): interrupts are masked for the (tiny, allocation-free)
    // duration of the decrement + slot NULL, so no IRQ-context unref can land
    // while we hold it. The heavy teardown below runs AFTER the lock is dropped
    // and interrupts are restored, exactly as before.
    uint64_t ptl_flags;
    spin_lock_irqsave(&process_table_lock, &ptl_flags);

    uint32_t new_count = __atomic_sub_fetch(&proc->ref_count, 1, __ATOMIC_SEQ_CST);

    if (new_count != 0) {
        // Not the last reference (or a getter re-reffed in the gap we just closed).
        // Nothing to free; just drop the lock and return.
        spin_unlock_irqrestore(&process_table_lock, ptl_flags);
        return;
    }

    // We are the single CPU that drove ref_count 1 -> 0 while holding the lock.
    // No concurrent process_get_by_pid() can observe-and-ref this PCB anymore:
    // unpublish it from the table NOW, in the same critical section, before we
    // drop the lock. The PID is returned to the pool here too.
    if (proc->pid < MAX_PROCESSES) {
        process_table[proc->pid] = NULL;
        pid_used[proc->pid] = 0;
    }

    spin_unlock_irqrestore(&process_table_lock, ptl_flags);

    {
        // Only the CPU that decremented from 1 to 0 reaches here.
        // This prevents double-free race condition.
        PROC_LOG("[PROCESS] Freeing process '%s' (PID %d) - ref_count reached 0\n",
                proc->name, proc->pid);

#ifdef SMP_FOUNDATION
        // CPU1 job orphan cleanup: if this process owns a pending CPU1 job we must
        // NOT block indefinitely (violates the async goal) and must NOT free any
        // buffers CPU1 is still touching. cpu1_orphan_jobs() is two-phase: it first
        // GRACE-WAITS up to ~100 ms for the in-flight job to drain cleanly (the
        // common case -- no orphan needed), and only if it is STILL pending does it
        // orphan it (clear owner_pid so the result is dropped, and transition the
        // arg/result ownership descriptors TRANSFERRED -> ORPHANED). The offload
        // BUFFERS are kref'd (kmalloc_ref in sys_cpu1_offload); the handler's own
        // kput frees them only once CPU1 is also done -- so no UAF and no leak. The
        // exiting process returns promptly (<= ~100 ms, usually instantly), bounded
        // on a finite TSC deadline so a wedged AP can never hang teardown. job_seq
        // guards against the slot being recycled by a newer job mid-wait. Called
        // AFTER process_table_lock is dropped, so the grace-wait holds no locks.
        extern void cpu1_orphan_jobs(uint32_t exiting_pid);
        cpu1_orphan_jobs(proc->pid);
#endif

        // Release IPC resources owned by this process (SysV shm/msg). shm cleanup
        // takes the process_t* directly (NOT a pid lookup): process_table[pid] has
        // already been nulled above, so a by-pid lookup would return NULL and skip
        // all cleanup (segment/attachment leak). msg cleanup scans by owner_pid so
        // it still uses the pid.
        shm_cleanup_process(proc);
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

// See sched.h. CAS-guarded single release of the CREATION ref: only the reaper
// that wins reaped:0->1 drops the creation ref; losers no-op. Never touches the
// caller's get_by_pid ref (the caller drops that itself, via process_destroy or
// process_unref). SEQ_CST matches the rest of the refcount atomics in this file.
// This is the headline #9 fix: today the creation ref (process_create sets
// ref_count=1) is NEVER released for a reaped zombie, so the PCB/8KB stack/CR3/PID
// leak and the 256-PID pool exhausts under spawn/exit churn.
void reap_claim_release(process_t* proc) {
    if (!proc) return;
    if (__atomic_exchange_n(&proc->reaped, 1, __ATOMIC_SEQ_CST) == 0) {
        process_unref(proc);   // release the creation ref, exactly once
    }
}

void process_destroy(process_t* proc) {
    if (!proc) return;

    // #48: if this process died while blocked in sys_thread_join (killed mid-join), it
    // still holds a get_by_pid reference on its join target that its own join cleanup
    // never released (its stack never resumed). Drop it here at reap so the target does
    // not leak as an unreapable zombie. A normally-completed joiner cleared join_target
    // before reaping its target, so this is NULL for it and for every non-joining
    // process. Released OUTSIDE process_table_lock (process_destroy holds none), so the
    // nested process_unref cannot self-deadlock.
    if (proc->join_target) {
        process_t* jt = proc->join_target;
        proc->join_target = NULL;
        process_unref(jt);
    }

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
    // This prevents the process from being freed while caller uses it.
    //
    // IRQ-SAFE (TOCTOU fix): this acquisition MUST mask interrupts. process_unref()
    // now does its final decrement + slot-NULL under this same lock, and may run in
    // hard-IRQ context (wait_object_signal path). A plain spin_lock here would let
    // such an IRQ fire while we hold the lock and self-deadlock. With interrupts
    // masked, the decrement-vs-ref race is fully serialized: we either ref a PCB
    // whose ref_count is still > 0 (it cannot reach 0 and free until our ref is
    // dropped), or we read a NULL slot that a completed unref already cleared.
    uint64_t flags;
    spin_lock_irqsave(&process_table_lock, &flags);
    process_t* proc = process_table[pid];
    if (proc) {
        process_ref(proc);  // Increment ref count before returning
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    return proc;

    // IMPORTANT: Callers MUST call process_unref() when done with the returned process!
}

process_t* process_get_current(void) {
    return current_process;
}

// ===========================================================================
// process_set_ready — validated CREATED/BLOCKED -> READY transition
// ===========================================================================
// THE single chokepoint for marking a process READY (schedulable). Validates
// the transition is legal (CREATED->READY or BLOCKED->READY) and logs illegal
// attempts (TERMINATED->READY would silently resurrect a zombie; READY->READY
// is a no-op but may indicate caller confusion). Returns 0 on success, -1 if
// the transition was rejected.
//
// Callers: health_monitor.c, procapi.c, kill.c, waitqueue.c, scheduler.c
// (sleep wakeup), exec.c, thread_create, and any code that needs to activate
// a newly-created or blocked process. Use this INSTEAD of direct
// `proc->state = PROCESS_READY` assignments to catch resurrection bugs.
int process_set_ready(process_t* proc) {
    if (!proc) {
        return -1;
    }

    process_state_t old = proc->state;

    // Valid transitions: CREATED->READY (initial activation), BLOCKED->READY (wake)
    if (old == PROCESS_CREATED || old == PROCESS_BLOCKED) {
        proc->state = PROCESS_READY;
        return 0;
    }

    // RUNNING->READY is the cooperative yield/requeue transition. A process that
    // voluntarily yields (sys_yield -> scheduler_yield_requeue) or is rotated by
    // the scheduler is still marked RUNNING at the moment it is put back on the
    // ready queue; the subsequent context_switch() saves its context so it can be
    // resumed later. This is legal and happens on EVERY yield, so allow it. (The
    // dangerous transition this chokepoint guards against is TERMINATED->READY
    // below — resurrecting a zombie — which stays rejected.)
    if (old == PROCESS_RUNNING) {
        proc->state = PROCESS_READY;
        return 0;
    }

    // READY->READY is a harmless idempotent no-op: the cooperative requeue paths
    // (scheduler_add_process / scheduler_yield_requeue) call process_set_ready and
    // are guarded against double-enqueue by proc->on_queue, so a process can be
    // marked READY while already READY without harm. This fires constantly for
    // CPU-bound, frequently-requeued tasks (e.g. the priority-test burners under
    // preemption — 70k lines/boot), so it is NOT logged. Still tolerated.
    if (old == PROCESS_READY) {
        return 0;  // tolerate (idempotent)
    }

    // TERMINATED->READY is illegal (resurrection)
    if (old == PROCESS_TERMINATED) {
        kprintf("[PROCESS] ERROR: rejected TERMINATED->READY transition for "
                "process %u (%s) — cannot resurrect a zombie\n",
                proc->pid, proc->name);
        return -1;
    }

    return -1;  // unknown state
}

void process_set_current(process_t* proc) {
    current_process = proc;
    // SMP brick 4: mirror the current task into this CPU's cpu_t slot. At N=1
    // (cpu_id()==0) this is just an extra store into cpus[0].current_thread and
    // changes no observable behavior; it keeps this_cpu()->current_thread live
    // for the later SMP bricks. The shared global above stays authoritative.
    cpu_set_current_thread(proc);
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
    // IRQ-safe (see allocate_pid): IRQ-context process_unref() also takes this lock.
    uint64_t flags;
    spin_lock_irqsave(&process_table_lock, &flags);
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
    spin_unlock_irqrestore(&process_table_lock, flags);
    return n;
}
