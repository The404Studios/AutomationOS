#ifndef SCHED_H
#define SCHED_H

#include "types.h"
#include "kernel.h"
#include "spinlock.h"

#define KERNEL_STACK_SIZE 8192

// ---------------------------------------------------------------------------
// Kernel-stack canary -- detect stack overflow / corruption before it causes
// a silent GPF or data corruption during context switch or timer IRQ.
//
// STACK_CANARY_PLANT(proc) writes the sentinel at the BOTTOM of the process's
// kernel stack (the first 8 bytes, which are the last to be overwritten on
// overflow). STACK_CANARY_CHECK(proc) verifies it; on mismatch it prints a
// diagnostic and kernel_panic()s before the corrupted stack can cause a
// harder-to-debug crash. Called from process_create/thread_create (plant) and
// context_switch / timer_handler (check).
// ---------------------------------------------------------------------------
#define STACK_CANARY_VALUE  0xDEAD57AC0CA4A47FULL  /* "DEAD STACK CANARY" */

#define STACK_CANARY_PLANT(proc) do {                                      \
    if ((proc) && (proc)->kernel_stack) {                                   \
        *(volatile uint64_t*)((proc)->kernel_stack) = STACK_CANARY_VALUE;   \
    }                                                                       \
} while (0)

#define STACK_CANARY_CHECK(proc) do {                                      \
    if ((proc) && (proc)->kernel_stack &&                                   \
        *(volatile uint64_t*)((proc)->kernel_stack) != STACK_CANARY_VALUE) {\
        kprintf("[CANARY] STACK OVERFLOW detected in '%s' (PID %u) "       \
                "canary=0x%016llx expected=0x%016llx\n",                    \
                (proc)->name, (proc)->pid,                                  \
                (unsigned long long)*(volatile uint64_t*)(proc)->kernel_stack,\
                (unsigned long long)STACK_CANARY_VALUE);                     \
        kernel_panic("kernel stack canary corrupted");                      \
    }                                                                       \
} while (0)

// O(1) Scheduler constants
#define SCHED_PRIORITY_LEVELS 140  // 140 priority queues (0-139, Linux O(1) pattern)
#define SCHED_BITMAP_WORDS 3       // ceil(140/64) = 3 uint64_t words for bitmap

// Forward declarations
struct namespace_container;
struct rlimit_container;
struct seccomp_filter;
struct capability_set;
struct process;

// Process states
typedef enum {
    PROCESS_CREATED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} process_state_t;

// F3-2: sentinel for process_t.pinned_cpu meaning "not pinned to any CPU". Defined
// here (NOT smp.h) because sched.h declares pinned_cpu and is included by every PCB
// construction site, whereas scheduler.c/process.c do NOT include smp.h. UINT32_MAX
// comes from types.h. Distinct NAME from ownership.h's OWN_CPU_NONE (no collision;
// the numeric coincidence at 0xFFFFFFFF is harmless). #ifndef-guarded defensively.
#ifndef CPU_NONE
#define CPU_NONE  UINT32_MAX   /* pinned_cpu sentinel: task is NOT pinned */
#endif

// ---------------------------------------------------------------------------
// wait_object_t — the single blocking primitive (engine in waitqueue.c).
// The full STRUCT layout is defined HERE (early) so process_t can embed a
// join wait_object BY VALUE (for threads). The detailed documentation, the
// WAIT_OBJECT_INITIALIZER macro, and the operations API live further below
// (right above the wait_queue_t wrapper) — only the struct definition is
// hoisted. It references only spinlock_t (spinlock.h, included above) and
// struct process (forward-declared above), both available at this point.
// ---------------------------------------------------------------------------
typedef struct wait_object {
    struct process* waiters;  // intrusive FIFO head (via process_t.wait_next)
    struct process* tail;     // FIFO tail for O(1) append (preserves wake order)
    spinlock_t lock;          // protects the waiter list
    int initialized;          // 0 => zero-initialized; lazily inited on first use
} wait_object_t;

// CPU context saved during context switch (must be 16-byte aligned)
//
// NOTE ON LAYOUT: the assembler in context_switch.asm hardcodes the byte
// offsets of these fields (CONTEXT_RAX..CONTEXT_FPU). Do NOT reorder or
// resize the fields before fpu_state without updating context_switch.asm.
// fpu_state lands at offset 160 (the 16-byte alignment attribute pads the
// preceding 19 * 8 = 152 bytes up to 160).
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr3;  // Page directory base
    uint8_t fpu_state[512] __attribute__((aligned(16)));  // FXSAVE/FXRSTOR area
} cpu_context_t;

// ===========================================================================
// Interrupt frame (preemptive scheduling support)
// ===========================================================================
// Mirrors EXACTLY what is on the kernel stack when irq0_preempt (in
// interrupt.asm) calls schedule_from_irq(). The order is the reverse of the
// push sequence in that stub: the C handler receives a pointer to the lowest
// address, which is r15 (last pushed). Below the GP regs sit int_no and the
// CPU-pushed hardware iretq frame.
//
// This struct is the heart of the IRQ-driven switch: schedule_from_irq()
// saves these values into the outgoing process's cpu_context_t and overwrites
// them in place with the incoming process's saved values, so the single iretq
// at the tail of irq0_preempt resumes the *new* process.
typedef struct interrupt_frame {
    // General-purpose registers, in push order of irq0_preempt
    // (pushed last = lowest address = first field).
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;        // Vector number pushed by the stub
    // Hardware frame pushed by the CPU on interrupt entry:
    uint64_t rip;           // Interrupted instruction pointer
    uint64_t cs;            // Interrupted code segment
    uint64_t rflags;        // Interrupted flags
    uint64_t rsp;           // Interrupted stack pointer
    uint64_t ss;            // Interrupted stack segment
} interrupt_frame_t;

// How a suspended process must be resumed.
//  - RESUME_CRETURN: the process was switched out cooperatively (from a
//    syscall/yield via context_switch()/context_switch_asm). It is resumed by
//    the C-return path (a normal `ret` inside context_switch_asm).
//  - RESUME_IRETQ:   the process was switched out from a timer IRQ. It is
//    resumed by rebuilding an iretq frame from its cpu_context_t.
// New processes start as RESUME_CRETURN because their first run goes through
// scheduler_start()/enter_usermode (unchanged).
typedef enum {
    RESUME_CRETURN = 0,
    RESUME_IRETQ = 1
} resume_mode_t;

// Process Control Block
typedef struct process {
    uint32_t pid;                    // Process ID (namespace-local)
    uint32_t parent_pid;             // Parent process ID
    process_state_t state;           // Current state
    cpu_context_t context;           // Saved CPU context
    void* kernel_stack;              // Kernel stack pointer
    void* user_stack;                // User stack pointer
    uint64_t user_entry;             // User-mode entry point (for first run)
    uint64_t user_rsp;               // User-mode stack pointer (for first run)
    uint64_t time_slice;             // Remaining time slice (ticks)
    uint64_t total_time;             // Total CPU time used (scheduler_smp.c reads this; do not repurpose)
    // Per-process scheduler statistics (additive; exposed to userspace via
    // proc_info_t / Task Manager). process_create() memsets the whole PCB, so
    // both start at 0. Present in BOTH builds.
    //   ctx_switches: bumped once per dispatch in process_set_current() (the
    //                 single chokepoint for cooperative + preemptive switches).
    //   cpu_ticks:    real per-process CPU time -- bumped once per 1000 Hz timer
    //                 tick for whichever process is RUNNING (PIT IRQ0 handler in
    //                 the cooperative build; schedule_from_irq() in PREEMPTIVE).
    uint64_t ctx_switches;           // Number of times this process was dispatched
    uint64_t cpu_ticks;              // Timer ticks observed while this process was running
    int32_t priority;                // Process priority (nice value: -20 to +19)
    struct process* next;            // Next process in queue
    char name[64];                   // Process name
    uint32_t ref_count;              // Reference count (prevents use-after-free)

    // Security & Isolation (Phase 2)
    struct capability_set* capabilities;     // Process capabilities (Task 1)
    struct namespace_container* namespaces;  // Process namespaces
    uint32_t sandbox_flags;          // Sandbox flags (for future use)
    struct rlimit_container* rlimits; // Resource limits and usage tracking

    // Seccomp syscall filtering (Phase 2 Task 7)
    struct seccomp_filter* seccomp_filter;  // Seccomp BPF filter chain
    uint32_t seccomp_mode;           // SECCOMP_MODE_* (0=disabled, 1=strict, 2=filter)

    // User/Group IDs (for future UID/GID support)
    uint32_t uid;                    // User ID
    uint32_t gid;                    // Group ID

    // PE loader / Win32 compatibility fields
    void* vmm;                       // Per-process virtual memory context
    char* exe_path;                  // Path to executable (for DLL search)

    // ----------------------------------------------------------------------
    // Preemptive scheduling support.
    // Appended at the END of the struct so the assembler's hardcoded
    // PROCESS_CONTEXT_OFFSET (16) and cpu_context_t offsets stay valid.
    // These fields are present in BOTH builds (so process.c et al. don't need
    // an #ifdef); they are simply ignored on the cooperative path.
    // ----------------------------------------------------------------------
    int resume_mode;                 // resume_mode_t: how to resume this process
    volatile int need_resched;       // Set by timer IRQ when quantum expires

    // Per-process virtual-memory-area list (see kernel/include/vma.h). Source of
    // truth for the page-fault handler. Appended at the END of the struct so the
    // assembler's hardcoded context offset stays valid.
    struct vma* vma_list;

    // Set while this PCB is linked in the scheduler ready queue. Lets
    // scheduler_add_process() be idempotent (a process has a single `next`
    // link, so double-enqueue would truncate the list / leak a ref / form a
    // cycle). Appended at the END to keep the assembler's context offset valid.
    int on_queue;

    // Per-process open-file descriptor table (fd >= 3). stdin/stdout/stderr
    // (0/1/2) are handled out-of-band in sys_read/write/close and never enter
    // this table. Routed through vfs_fd_alloc/get/free so fds no longer collide
    // across processes; closed by vfs_close_all_fds() at teardown so they don't
    // leak. Appended at the END to keep the assembler's context offset valid.
    // PROC_MAX_FDS MUST equal VFS_MAX_FDS (vfs.h); kept as a literal here to
    // avoid a circular include of vfs.h from sched.h.
    struct vfs_file* fd_table[1024 /* == VFS_MAX_FDS */];

    // CHANNEL-0: capability handle table (shared-ring channels). Handle is a
    // process-local index (1..CH_MAX_HANDLES-1; 0 = "none") -> {channel, end
    // (master/slave), rights}. Zeroed by the memset in process_create /
    // thread_create. stdio_chan[fd] = the handle bound to fd0/1/2 (0 = unbound
    // -> serial/ps2 as before). Cleaned up by channel_cleanup_process() at
    // teardown. After the CPU context, so appending here is layout-safe.
    struct { void* ch; uint8_t end; uint32_t rights; } ch_handles[32 /* == CH_MAX_HANDLES */];
    uint8_t stdio_chan[3];

    // Blocking waitpid(): a parent with no terminated child sleeps on this queue;
    // a child's sys_exit() wakes it. Pointer (lazily kmalloc'd on first wait) so
    // process_t need not see the full wait_queue_t definition (which appears
    // later in this header). exit_status carries the child's exit code to the
    // reaping waitpid. Appended at the END to keep the asm context offset valid.
    struct wait_queue* child_wait;
    int exit_status;

    // Real blocking sleep (SYS_SLEEP). A process that calls sleep(ms) goes
    // PROCESS_BLOCKED (zero CPU) and is linked onto the global sleep list via
    // sleep_next; the timer wakeup scan re-readies it once timer_get_ticks()
    // reaches wake_deadline. A sleeper is on the sleep list XOR a ready queue,
    // never both. Both fields are memset-zeroed by process_create(). Appended at
    // the END to keep the assembler's hardcoded context offset valid.
    uint64_t wake_deadline;          // absolute tick (timer_get_ticks units) to wake at
    struct process* sleep_next;      // intrusive singly-linked sleep list link

    // Cooperative priority weighting (scheduler.c: scheduler_yield_requeue).
    // The O(1) active/expired runqueue is a strict round-robin, so on the
    // cooperative path (where time slices are never decremented) two equally
    // greedy yielders get EQUAL CPU regardless of nice -- nice only changes pick
    // ORDER, not share. To make a higher-priority (more-negative nice) process
    // actually accrue MORE CPU when it yields, we let it re-enter the ACTIVE
    // queue a bounded number of extra times before finally rotating to expired;
    // yield_boost counts those remaining bonus re-queues. It is derived from nice
    // (PRIORITY_YIELD_BOOST), so nice 0/positive get ZERO bonus -- i.e. NORMAL
    // and below behave byte-for-byte as before (desktop fairness unchanged); only
    // above-normal classes get extra turns. Bounded => no starvation (the booster
    // still rotates to expired once its boost is spent, letting lower prio run).
    // memset-zeroed by process_create(). Appended at the END to keep the
    // assembler's hardcoded context offset valid.
    int yield_boost;                 // remaining bonus ACTIVE re-queues on yield

    // Unified wait-object support (wait.h / waitqueue.c). A process blocked via
    // wait_object_block() is linked onto ITS wait_object's intrusive waiter list
    // through wait_next, with wait_on pointing back at that object. This is the
    // single block primitive underlying BOTH event waits (wait queues: waitpid,
    // futex, epoll) and timer waits (sys_sleep, which passes a deadline). The
    // object holds one process_ref per linked waiter (mirroring the old wait-
    // entry ref), so a process killed while BLOCKED stays alive until unlinked.
    // wait_on lets the timer wakeup scan (sleep_list_wake_due) unlink a sleeper
    // from its wait_object when its deadline fires first. memset-zeroed by
    // process_create(). Appended at the END to keep the asm context offset valid.
    struct wait_object* wait_on;     // wait_object this process is parked on (or NULL)
    struct process* wait_next;       // intrusive waiter-list link within wait_on

    // ----------------------------------------------------------------------
    // THREADS (real threads sharing the parent's address space).
    // A thread is a process_t that SHARES its parent's CR3 (context.cr3) but has
    // its own kernel_stack, user stack, registers, and FPU state. The three
    // fields below are what make CR3 lifetime + arg-passing + join correct.
    // All appended at the END so the assembler's hardcoded PROCESS_CONTEXT_OFFSET
    // (16) and cpu_context_t offsets stay valid. memset-zeroed by process_create.
    // ----------------------------------------------------------------------
    //
    // as_refcount: ADDRESS-SPACE refcount. A small heap-allocated int* SHARED by
    // every process_t that runs on the same CR3 (the main process + all its
    // threads). process_create() allocates a FRESH one initialized to 1 (a normal
    // process / a fork child each owns its address space). thread_create() does
    // NOT call paging_create_address_space(): it copies the parent's context.cr3
    // AND its as_refcount pointer, then atomically increments *as_refcount. On
    // teardown (process_unref) we atomically decrement *as_refcount and only call
    // paging_destroy_address_space() when it reaches 0 — i.e. the LAST user of the
    // CR3 tears it down. This is the single invariant that prevents freeing page
    // tables out from under a still-running sibling thread (instant triple-fault).
    int* as_refcount;

    // tgid: THREAD-GROUP ID. For a normally-created process tgid == pid (it is its
    // own group leader). For a thread, tgid == the creating process's tgid, so all
    // threads of one process share a tgid. Used as the join permission check
    // (a thread may only join another thread in the SAME group).
    uint32_t tgid;

    // is_thread: 1 for a thread created via SYS_THREAD_CREATE, 0 otherwise. Lets
    // teardown / accounting distinguish a thread from a process without inferring
    // it from tgid.
    int is_thread;

    // Thread entry argument: thread_create stashes the user `arg` here; the
    // thread's first-run trampoline loads it into RDI so the thread begins
    // executing entry(arg) per the SysV calling convention.
    uint64_t thread_arg;

    // Thread return value (set by SYS_THREAD_EXIT) and the wait_object a joiner
    // blocks on (signalled when this thread exits). thread_join copies out
    // thread_retval after the join wait_object wakes it. The wait_object_t struct
    // is hoisted to the top of this header so it can be embedded BY VALUE here.
    // Explicitly initialized by thread_create (wait_object_init); zero-init is
    // also fine (wo_ensure_init lazily inits a zeroed object).
    int thread_retval;
    wait_object_t thread_join_wo;

    // Per-process shared memory attachment list. Each successful shmat() adds a
    // shm_attachment_t node to this singly-linked list; shmdt() removes it. On
    // process teardown shm_cleanup_process() walks this list instead of scanning
    // the entire global shm_attaches[] array, making cleanup O(n) where n is the
    // number of attachments THIS process holds (typically 1-5) instead of O(128).
    // Appended at the END to keep the assembler's hardcoded context offset valid.
    struct shm_attachment* shm_attachments;

    // Reap-claim flag (#9 zombie-leak fix). 0 = the zombie's CREATION ref has not
    // yet been claimed; a single reaper transitions it 0->1 via __atomic_exchange_n
    // and ONLY that winner releases the creation ref (reap_claim_release). Lets the
    // waitpid table-scan, a targeted thread_join, and init all observe the same
    // zombie while exactly one releases the creation ref (no double-free). memset-
    // zeroed by process_create()/thread_create(). Appended at the END to keep the
    // assembler's hardcoded PROCESS_CONTEXT_OFFSET (16) valid.
    volatile int reaped;

    // Stable process identity (#10 wrong-wake-on-PID-reuse fix). PIDs are recycled
    // from the bitmap on death, so pid ALONE is not a stable identity: a recycled
    // PID can make an unrelated new process impersonate a dead parent/child. INVARIANT:
    // identity == (pid, create_seq). create_seq is a monotonic stamp assigned under
    // process_table_lock at create; parent_seq snapshots the CREATOR's create_seq so a
    // child carries its real parent's identity. A reaper/wake validates
    // parent->create_seq == child->parent_seq before acting, so a recycled PID can
    // never be mistaken for the original. Reparent-to-init rewrites BOTH parent_pid=1
    // and parent_seq=<init's create_seq> so the check holds uniformly for orphans.
    // memset-zeroed by process_create()/thread_create(); appended at the END to keep
    // the assembler's hardcoded PROCESS_CONTEXT_OFFSET (16) valid.
    uint64_t create_seq;
    uint64_t parent_seq;

    // Futex wait key (FUTEX-B wrong-waiter-wake fix). When this process is parked in
    // futex_wait, this holds the PHYSICAL address of the futex word it is waiting on;
    // otherwise 0. futex_wake walks the hash bucket's waiter list and re-readies only
    // waiters whose futex_key matches the woken address, so two distinct futex words
    // that hash-collide into one bucket no longer wake each other. Set in futex_wait
    // before the waiter is linked, cleared on resume. memset-zeroed by
    // process_create()/thread_create(); appended at the END to keep the assembler's
    // hardcoded PROCESS_CONTEXT_OFFSET (16) valid. Non-futex waiters keep it 0, so a
    // (nonzero) futex key never matches them.
    uint64_t futex_key;

    // Join target (#48 killed-while-blocked join-ref leak fix). While this thread is
    // blocked in sys_thread_join, it holds a process_get_by_pid(+1) reference on the
    // thread it is joining; join_target points at that target (else NULL). If the
    // joiner is KILLED while blocked, its sys_thread_join cleanup never runs (its stack
    // never resumes), so that reference would leak and the target would become an
    // unreapable zombie. process_destroy() releases join_target at reap to plug that.
    // A normally-completed join clears join_target before reaping its target, so it is
    // NULL for every non-joining process. memset-zeroed by process_create()/
    // thread_create(); appended at the END to preserve the assembler's hardcoded
    // PROCESS_CONTEXT_OFFSET (16).
    struct process* join_target;

    // Per-CPU runqueue membership (F3-1 per-CPU rq_lock). Records WHICH cpu's
    // runqueue this task is currently linked on. VALID iff on_queue==1; left STALE
    // when on_queue==0 (exactly like wait_on/futex_key -- readers MUST gate on
    // on_queue first). Set inside the SAME rq_lock critical section that sets
    // on_queue=1 and calls runqueue_enqueue, so (on_queue, queued_cpu, physical
    // link) are always mutually consistent under that cpu's rq_lock. Lets the
    // F3-0 validators turn "task X is on cpu C's runqueue" into a checkable
    // invariant (membership must land on cpus[queued_cpu] and on NO OTHER cpu --
    // the cross-cpu double-enqueue race signature). At N=1 it is always 0 (cpu0).
    // memset-zeroed by process_create()/thread_create(); appended at the END to
    // preserve the assembler's hardcoded PROCESS_CONTEXT_OFFSET (16).
    uint32_t queued_cpu;

    // CPU affinity model (F3-2). allowed_cpus is a bitmask: bit N set => this task MAY
    // be enqueued on CPU N. pinned_cpu, when != CPU_NONE, REQUIRES the task run only on
    // that CPU (the invariant allowed_cpus == (1ULL<<pinned_cpu) is expected). F3-2
    // policy: normal tasks get allowed_cpus = (1ULL<<0) (CPU0-only) + pinned_cpu =
    // CPU_NONE; the lone CPU1 test kthread gets allowed_cpus = (1ULL<<1), pinned_cpu = 1.
    // Stored as a plain uint64_t (NOT cpumask_t) so sched.h need not include smp.h, and
    // scheduler.c tests it with raw bit ops (it deliberately avoids including smp.h).
    // THE TRAP: memset(proc,0) leaves allowed_cpus=0 (== allowed on NO cpu -> the
    // validator trips for EVERY task) and pinned_cpu=0 (== pinned to CPU0, NOT the
    // unpinned sentinel). So EVERY PCB construction site MUST set BOTH explicitly after
    // its memset (process_create covers every ctor that funnels through it; thread_create
    // sets its own). Appended at the END to preserve PROCESS_CONTEXT_OFFSET (16). NOTE:
    // a uint64 mask addresses cpu0..63 while MAX_CPUS=256 -- irrelevant for F3-2 (cpu0/1),
    // documented for a future >cpu63 brick.
    uint64_t allowed_cpus;
    uint32_t pinned_cpu;   // CPU_NONE if not pinned

    // SIGSTOP/SIGCONT discipline: SIGCONT must only resume a process that was
    // stopped by SIGSTOP, NOT an arbitrary BLOCKED process (e.g. one sleeping
    // in sys_sleep or waiting on a futex). SIGSTOP sets stopped_by_signal = 1;
    // SIGCONT checks it and only resumes if set. Cleared on resume. Without
    // this, sending SIGCONT to any BLOCKED process would incorrectly wake it.
    // memset-zeroed by process_create()/thread_create().
    int stopped_by_signal;
} process_t;

// Global pointer to current process (for PE loader and other subsystems)
extern process_t* current_process;

// Process table management
void process_init(void);
void process_cleanup(void);  // Teardown counterpart: terminate all live processes
process_t* process_create(const char* name, void* entry_point);
void process_adopt_pid0(process_t* proc);   // re-home a process onto reserved PID 0 (idle thread)
void process_destroy(process_t* proc);

// Release the CREATION reference of a terminated process EXACTLY ONCE, even when
// several reapers (a waitpid table-scan, a targeted thread_join, init) race on the
// same zombie. Uses a CAS (__atomic_exchange_n) on proc->reaped: only the reaper
// that transitions reaped 0->1 drops the creation ref; losers no-op. Does NOT
// touch any get_by_pid reference -- the caller still drops its own ref afterwards
// (via process_destroy() or process_unref()). Call this IMMEDIATELY BEFORE
// process_destroy(zombie) at a reaper site, or before the final process_unref() in
// process_cleanup(). [#9 zombie/PID leak]
void reap_claim_release(process_t* proc);

// ----------------------------------------------------------------------------
// Threads (kernel/core/sched/process.c)
// ----------------------------------------------------------------------------
// Create a THREAD of `parent`: a new schedulable process_t that SHARES parent's
// address space (context.cr3 + as_refcount, refcount incremented) but has its
// OWN kernel stack, user stack (caller-supplied), registers, and FPU state. The
// thread begins executing entry(arg) in ring 3 (RDI = arg, SysV). tgid is
// inherited from the parent so all threads of a process share it. Returns the
// new thread's PCB (state PROCESS_READY, NOT yet added to the scheduler — caller
// adds it), or NULL on failure. Does NOT call paging_create_address_space().
process_t* thread_create(process_t* parent, uint64_t entry, uint64_t arg,
                         uint64_t user_stack);

// First-run trampoline for a THREAD (kernel/core/usermode.c). Mirrors
// process_enter_usermode_trampoline but ALSO passes thread_arg in RDI so the
// thread enters entry(arg). context_switch_asm `ret`s here on the thread's first
// dispatch (RESUME_CRETURN), exactly like a normal new process.
void thread_enter_usermode_trampoline(void);
void process_ref(process_t* proc);      // Increment reference count
void process_unref(process_t* proc);    // Decrement reference count (frees if 0)
void process_on_terminate(process_t* child);  // wake parent's waitpid + drain own wait queue
process_t* process_get_by_pid(uint32_t pid);
process_t* process_get_by_name(const char* needle);  // Find first live process by name substring (ref'd)

// SMP-F3-6: THE placement seam (docs/SCHEDULER_POLICY_LAYER.md). Answers
// "which CPU should this task run on?" -- hard legality, then pin/role, then
// the home-CPU0 stub (no balancing until F3-7). ADVISORY: the mandatory F3-2
// enqueue gate in scheduler_add_process_to_cpu stays the backstop. Defined
// only under SMP_SCHED && SMP_SCHED_DISPATCH (the only builds where placement
// has more than one answer).
uint32_t scheduler_choose_cpu(process_t* p);
void scheduler_choosecpu_selftest(void);
// Inlined: process_get_current is on the syscall dispatch hot path (called for
// every SYS_GETPID, SYS_YIELD, and the table-lookup fallback). A cross-TU
// function call costs ~5 cycles; inlining a global-load is a single MOV.
//
// F3-4 (adapted to this inline): THE LAW -- "current" is CPU-LOCAL. Under
// SMP_SCHED_DISPATCH (the only build where CPU1 can RUN a task) the resolver
// routes via cpus[cpu_id()].current_thread so a syscall/exit on the AP
// operates on CPU1's actual task, NOT the global current_process (which still
// names CPU0's). Default/SMP_FOUNDATION builds keep the single-MOV global
// load: cpu_id()==0 always there and process_set_current() holds the per-cpu
// slot in lockstep -- same value, cheaper path, default build byte-identical.
#ifdef SMP_SCHED_DISPATCH
process_t* cpu_get_current_thread(void);   /* fwd (also declared below) */
static inline process_t* process_get_current(void) { return cpu_get_current_thread(); }
#else
static inline process_t* process_get_current(void) { return current_process; }
#endif
void process_set_current(process_t* proc);
int process_set_ready(process_t* proc); // Validated CREATED/BLOCKED->READY transition (ret 0=OK, -1=rejected)

// Userspace-facing process snapshot record (SYS_PROCLIST ABI). MUST stay 64 bytes.
// The first 48 bytes are the original layout (pid/parent_pid/state/flags/name) so
// older 48-byte consumers still read the prefix correctly; the two scheduler-stat
// fields are appended AFTER name[32] (offsets 48/56, both 8-aligned). Every
// userspace mirror of this struct (aictl.h procinfo_t, ps/taskman/sysmon/dashboard/
// terminal/meminfo/aibroker/aiconsole) MUST size its receive buffer to this 64-byte
// stride, or the kernel's copy_to_user of n*64 bytes overflows it.
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;        // PROCESS_* enum value
    uint32_t flags;        // reserved (0)
    char     name[32];
    uint64_t cpu_ticks;    // timer ticks observed while this process was running
    uint64_t ctx_switches; // number of times this process was dispatched
} proc_info_t;

// Snapshot the live process table into out[0..max-1]; returns the number filled.
int process_list(proc_info_t* out, int max);

// Re-entrancy guard: set during context_switch, checked by timer handler
// Prevents schedule() from being called re-entrantly if timer fires
// during the critical section of a context switch (GPF-001 fix)
extern volatile int scheduler_in_switch;

// SMP foundation brick 4: keep the per-CPU cpu_t.current_thread field (in
// scheduler.c's cpus[cpu_id()]) in lockstep with the shared current_process
// global. Called from process_set_current() -- the single dispatch chokepoint.
// At CPU count == 1 (cpu_id()==0) this is a non-observable extra store into
// cpus[0]; it exists so bricks 5+ can read this_cpu()->current_thread directly.
void cpu_set_current_thread(process_t* proc);
// F3-4: the inverse resolver -- this_cpu()->current_thread (per-CPU "current").
process_t* cpu_get_current_thread(void);
// F3-5: HOME-ROUTED wake enqueue -- the woken task goes to ITS cpu (pin, else
// CPU0), never the WAKER's. The wake-side replacement for scheduler_add_process
// (which is this_cpu()-based and cross-CPU-unsafe from the AP).
void scheduler_add_process_home(process_t* proc);
#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
// F3-5: the AP-safe cooperative scheduler (yield + the dying path) -- exposed
// for the CPU1 syscall routing in schedule()/sys_yield and the F2 kthread.
void ap_cooperative_schedule(void);
#endif

// Scheduler
void scheduler_init(void);
void scheduler_shutdown(void);  // Teardown: drain runqueues, release locks, clear sleep list
void scheduler_add_process(process_t* proc);
// Cooperative-yield re-queue with priority weighting. Use INSTEAD of
// scheduler_add_process() from sys_yield(): a process with above-normal priority
// re-enters the ACTIVE queue (so pick_next runs it again ahead of lower-priority
// peers) up to a nice-derived number of bonus turns, then rotates to EXPIRED like
// normal. For nice >= 0 this is exactly equivalent to scheduler_add_process()
// (straight to expired), so NORMAL/background tasks are unaffected. Takes the
// scheduler's reference exactly like scheduler_add_process().
void scheduler_yield_requeue(process_t* proc);
void scheduler_remove_process(process_t* proc);
void schedule(void);  // Scheduler tick - called from timer interrupt
process_t* scheduler_pick_next(void);
process_t* scheduler_idle_thread(void);   // the per-CPU idle fallback (NOT a queued process)
void scheduler_start(void) NORETURN;  // Start scheduler (does not return)

#ifdef SMP_SCHED
// SMP scheduler Brick D: populate a secondary CPU's per-CPU slot (runqueues +
// idle thread + online). Call on the BSP AFTER /sbin/init has PID 1. Returns 1 ok.
int scheduler_init_secondary_cpu(uint32_t cpu, uint32_t apic_id);
// Brick F: enqueue `proc` onto a SPECIFIC CPU's runqueue (cross-CPU affinity pin).
void scheduler_add_process_to_cpu(process_t* proc, uint32_t cpu);
#endif

#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
// SMP scheduler Brick F: the AP-safe dispatcher for CPU1 (scheduler mode). NEVER
// touches the global current_process; NEVER does a PIC EOI. See scheduler.c.
void ap_scheduler_loop(void) NORETURN;            // CPU1's top-level scheduler loop
void ap_cooperative_schedule(void);               // CPU1 voluntary yield/idle check
void ap_schedule_from_irq(interrupt_frame_t* frame); // CPU1 LAPIC-timer preemption
void ap_spawn_test_kthread(void);                 // F2: pin one kernel thread to CPU1
void context_prime_fpu(process_t* to);            // F3 fix D6: AP-safe FPU prime (context.c)
#endif

// Cooperative dispatch helper (scheduler.c). A cooperative resume site
// (sys_yield / schedule / wq_block_current, all in ring 0 inside a syscall)
// hands the CPU to `next`. If `next` is RESUME_CRETURN it resumes via the
// normal context_switch (`ret` to a kernel continuation); if `next` is
// RESUME_IRETQ (preempted in ring 3) it MUST resume via iretq, NOT `ret` — this
// helper routes to the correct mechanism and updates TSS.RSP0 + the SYSCALL
// kernel stack for `next`. Use this instead of calling context_switch() directly
// at any site that may pick a timer-preempted process.
void cooperative_switch_to(process_t* from, process_t* next);

#ifdef PREEMPTIVE
// asm (context_switch.asm): resume a RESUME_IRETQ process from a cooperative
// (syscall) context by restoring its ring-3 register state and IRETQ-ing to
// ring 3. Saves `from`'s kernel C-return point first (so `from` can later be
// resumed by context_switch). Does not return to the caller.
void context_switch_to_iretq(process_t* from, process_t* to);
#endif

// ===========================================================================
// Real blocking sleep support (scheduler.c owns the global sleep list)
// ===========================================================================
// sys_sleep() marks the caller PROCESS_BLOCKED, sets wake_deadline, and pushes
// it onto the global sleep list with sleep_list_push() BEFORE switching away.
// Once per timer tick the wakeup scan (sleep_list_wake_due) is called from BOTH
// the cooperative PIT handler (pit.c timer_handler) and the preemptive
// schedule_from_irq(); it unlinks every sleeper whose wake_deadline <= now,
// marks it PROCESS_READY, and re-adds it via scheduler_add_process(). Present in
// BOTH builds (not gated). Single-core; the implementation brackets the list
// mutation with cli/restore so a timer IRQ cannot observe a half-linked node.
void sleep_list_push(process_t* proc);
void sleep_list_remove(process_t* proc);
void sleep_list_wake_due(uint64_t now);

// SMP Load Balancing (scheduler_smp.c)
void scheduler_tick(void);                                    // Called on timer tick for load balancing
void scheduler_get_load_stats(uint32_t* loads, uint32_t max_cpus);  // Get per-CPU load
void scheduler_print_stats(void);                            // Print load balancing statistics

// Per-CPU Statistics and Diagnostics (scheduler.c)
// Forward declaration of cpu_stats_t (defined in scheduler.c)
typedef struct cpu_stats cpu_stats_t;

uint32_t scheduler_get_ready_count(uint32_t cpu_id);          // Get ready_count for specific CPU
uint32_t scheduler_get_current_ready_count(void);             // Get ready_count for current CPU
void scheduler_reset_ready_count(uint32_t cpu_id);            // Reset ready_count for specific CPU
int scheduler_get_cpu_stats(uint32_t cpu_id, cpu_stats_t* out_stats);  // Get per-CPU stats
void scheduler_reset_cpu_stats(uint32_t cpu_id);              // Reset stats for specific CPU
void scheduler_reset_all_cpu_stats(void);                     // Reset stats for all CPUs
int scheduler_validate_ready_count(uint32_t cpu_id);          // Validate ready_count integrity

// Context switching
void context_switch(process_t* from, process_t* to);

// ===========================================================================
// Preemptive scheduling API
// ===========================================================================
//
// These symbols exist in ALL builds so callers/integrators can reference them
// unconditionally, but the actual preemptive *behavior* is compiled in only
// under -DPREEMPTIVE. With the flag OFF:
//   - schedule_from_irq() is a no-op shim that just sends EOI (it should not
//     be wired up at all; the cooperative IRQ0 path in pit.c is used instead).
//   - irq0_preempt (asm) is not assembled.
//   - The per-quantum need_resched machinery is dormant.
//
// Default time quantum (timer ticks) before a running process is preempted.
#define SCHED_QUANTUM_TICKS 10

// Called from the dedicated preemptive IRQ0 stub (irq0_preempt, interrupt.asm).
// Receives a pointer to the on-stack interrupt_frame_t. It sends the PIC EOI,
// advances the timer tick, decrements the current quantum and, when due,
// performs an in-place IRQ-driven context switch by rewriting *frame so the
// stub's trailing iretq resumes the next process. MUST be called with
// interrupts disabled (the stub keeps IF=0 until iretq).
void schedule_from_irq(interrupt_frame_t* frame);

// Low-level assembly helpers (context_switch.asm) used by the IRQ switch path.
// Save the interrupted frame's GP regs + iretq frame into ctx, plus FPU state.
void context_save_irq(cpu_context_t* ctx, interrupt_frame_t* frame);
// Load ctx into the on-stack frame so the stub's iretq resumes that context,
// also restoring FPU state. Does NOT switch CR3 (the C caller does that).
void context_load_irq(cpu_context_t* ctx, interrupt_frame_t* frame);

// The preemptive IRQ0 entry stub (interrupt.asm). Integrator points IDT[32]
// at this instead of irq0 to enable preemption.
void irq0_preempt(void);

// ===========================================================================
// Unified WAIT OBJECT (wait.h / waitqueue.c) — the SINGLE blocking primitive
// ===========================================================================
// A wait_object_t is the one place the block/resume discipline lives. Both
// blocking flavors are now `wait_object_block(wo, deadline)`:
//   • event wait  == wait_object_block(wo, 0)         — woken by a signal only
//   • timer wait  == wait_object_block(wo, deadline)  — woken by signal OR timer
// so "sleep == wait(timer)" and "wait_event == wait(event)" are the same code.
//
// Waiters are kept on an INTRUSIVE singly-linked FIFO list via the parked
// process's wait_next link (process_t), with the process's wait_on pointing
// back at the object. The object holds ONE process_ref per linked waiter
// (replacing the old kmalloc'd wait_entry's ref), which keeps a process killed
// while BLOCKED alive until it is unlinked — the load-bearing safety property
// the old wait queue relied on. No per-wait heap allocation is needed.
//
// A waiter with a non-zero deadline is ALSO linked onto the global timer sleep
// list (sleep_list_push) with wake_deadline set, so whichever of {signal,
// timeout} happens first wins; the other linkage is cleaned up on resume (or by
// the timer scan, which unlinks the sleeper from its wait_object too).
//
// NOTE: the wait_object_t STRUCT is defined near the TOP of this header (hoisted
// so process_t can embed a join object by value); only the macro + the API are
// declared here.
#define WAIT_OBJECT_INITIALIZER  { NULL, NULL, { 0, 0xFFFFFFFF, NULL }, 1 }

void wait_object_init(wait_object_t* wo);

// THE single block primitive. Marks the current process PROCESS_BLOCKED, links
// it on wo's waiter list (and, if deadline != 0, on the timer sleep list with
// wake_deadline == deadline), then switches to another runnable process exactly
// like the old wq_block_current/sys_sleep (pick next / sti-hlt idle /
// cooperative_switch_to). Returns when the process is later woken AND
// rescheduled, after unlinking from both lists.
// Returns 1 if woken by wait_object_signal, 0 if woken by timeout (or if it
// fell through without truly sleeping). Must be called from a sleepable context
// (not a hard IRQ; interrupts enabled).
int wait_object_block(wait_object_t* wo, uint64_t deadline_or_0);

// FUTEX-A lost-wakeup fix: enqueue-and-recheck under wo->lock (the SAME lock the wake
// path takes), as a two-step prepare/commit so the value test, the link, and the
// BLOCKED store are atomic w.r.t. a concurrent futex_wake. wait_object_prepare_futex
// returns 1 (current is now linked on wo, tagged with `key`, and BLOCKED -- caller MUST
// then call wait_object_park_committed) or 0 (*uaddr != val: nothing linked, caller
// returns EAGAIN). wait_object_park_committed deschedules an already-linked,
// already-BLOCKED waiter and returns 1 if signal-woken. Futex-only; generic blockers
// keep using wait_object_block.
int wait_object_prepare_futex(wait_object_t* wo, int* uaddr, int val, uint64_t key);
int wait_object_park_committed(wait_object_t* wo);

// Wake the wait_object's waiters: wake_all==0 wakes exactly one (FIFO), nonzero
// wakes all. Each woken waiter is unlinked, marked PROCESS_READY and handed to
// scheduler_add_process(). Returns the number of processes woken.
int wait_object_signal(wait_object_t* wo, int wake_all);

// Number of processes currently parked on wo.
int wait_object_count(wait_object_t* wo);

// Force-unlink `proc` from `wo` if it is currently linked there, dropping the
// object's reference on it. Idempotent: a no-op (and NO unref) if proc was already
// removed by a concurrent signal/timer. Used by the kill paths to reclaim the
// object-ref of a process killed while BLOCKED on this wait_object, so the PCB
// collapses to a reapable zombie instead of leaking. Returns 1 if it unlinked-and-
// unref'd, else 0. [#9 kill-while-blocked leak]
int wait_object_abort(wait_object_t* wo, process_t* proc);

// ===========================================================================
// Wait queues — thin compatibility wrapper over wait_object_t
// ===========================================================================
// wait_queue_t keeps its exact historical public API (wq_init/wq_block_current/
// wq_wake_one/wq_wake_all/wq_count) so its many callers (waitpid, futex, epoll)
// are untouched; internally it is now just a wait_object (event-only: deadline
// 0). This is the wait-queue half of the unification.
typedef struct wait_queue {
    wait_object_t wobj;
} wait_queue_t;

// Statically initialize a wait_queue_t at file scope.
#define WAIT_QUEUE_INITIALIZER  { WAIT_OBJECT_INITIALIZER }

void wq_init(wait_queue_t* wq);

// Block the current process on wq: marks it PROCESS_BLOCKED, records it on the
// queue, and schedules another process. Returns when the process is later
// woken AND rescheduled. Must be called from a context where it is safe to
// sleep (not inside an IRQ handler, interrupts enabled).
void wq_block_current(wait_queue_t* wq);

// Wake exactly one waiter (FIFO). Returns the woken process (ref NOT taken) or
// NULL if the queue was empty.
process_t* wq_wake_one(wait_queue_t* wq);

// Wake exactly one waiter whose process_t.futex_key == key (FIFO among matchers),
// skipping (leaving linked) non-matching waiters. Returns the woken process (ref NOT
// taken) or NULL if no live matching waiter. Used by futex_wake so a hash-bucket
// collision between two distinct futex addresses cannot wake the wrong waiter; the
// unfiltered wq_wake_one is unchanged for waitpid/epoll. [FUTEX-B]
process_t* wq_wake_one_key(wait_queue_t* wq, uint64_t key);

// Wake all waiters. Returns the number of processes woken.
int wq_wake_all(wait_queue_t* wq);

// Number of processes currently blocked on wq.
int wq_count(wait_queue_t* wq);

// ELF Execution (exec.c)
void exec_usermode(const char* path, int argc, char** argv) NORETURN;
process_t* exec_create_process(const char* path, const char* name,
                                int argc, char** argv);
int exec_launch_init(void);

#endif
