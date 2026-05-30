#ifndef SCHED_H
#define SCHED_H

#include "types.h"
#include "kernel.h"
#include "spinlock.h"

#define KERNEL_STACK_SIZE 8192

// O(1) Scheduler constants
#define SCHED_PRIORITY_LEVELS 140  // 140 priority queues (0-139, Linux O(1) pattern)
#define SCHED_BITMAP_WORDS 3       // ceil(140/64) = 3 uint64_t words for bitmap

// Forward declarations
struct namespace_container;
struct rlimit_container;
struct seccomp_filter;
struct capability_set;

// Process states
typedef enum {
    PROCESS_CREATED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} process_state_t;

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
    uint64_t total_time;             // Total CPU time used
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

    // Blocking waitpid(): a parent with no terminated child sleeps on this queue;
    // a child's sys_exit() wakes it. Pointer (lazily kmalloc'd on first wait) so
    // process_t need not see the full wait_queue_t definition (which appears
    // later in this header). exit_status carries the child's exit code to the
    // reaping waitpid. Appended at the END to keep the asm context offset valid.
    struct wait_queue* child_wait;
    int exit_status;
} process_t;

// Global pointer to current process (for PE loader and other subsystems)
extern process_t* current_process;

// Process table management
void process_init(void);
process_t* process_create(const char* name, void* entry_point);
void process_destroy(process_t* proc);
void process_ref(process_t* proc);      // Increment reference count
void process_unref(process_t* proc);    // Decrement reference count (frees if 0)
void process_on_terminate(process_t* child);  // wake parent's waitpid + drain own wait queue
process_t* process_get_by_pid(uint32_t pid);
process_t* process_get_current(void);
void process_set_current(process_t* proc);

// Userspace-facing process snapshot record (SYS_PROCLIST ABI). MUST stay 48 bytes.
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;        // PROCESS_* enum value
    uint32_t flags;        // reserved (0)
    char     name[32];
} proc_info_t;

// Snapshot the live process table into out[0..max-1]; returns the number filled.
int process_list(proc_info_t* out, int max);

// Re-entrancy guard: set during context_switch, checked by timer handler
// Prevents schedule() from being called re-entrantly if timer fires
// during the critical section of a context switch (GPF-001 fix)
extern volatile int scheduler_in_switch;

// Scheduler
void scheduler_init(void);
void scheduler_add_process(process_t* proc);
void scheduler_remove_process(process_t* proc);
void schedule(void);  // Scheduler tick - called from timer interrupt
process_t* scheduler_pick_next(void);
void scheduler_start(void) NORETURN;  // Start scheduler (does not return)

// SMP Load Balancing (scheduler_smp.c)
void scheduler_tick(void);                                    // Called on timer tick for load balancing
void scheduler_get_load_stats(uint32_t* loads, uint32_t max_cpus);  // Get per-CPU load
void scheduler_print_stats(void);                            // Print load balancing statistics

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
// Wait queues (waitqueue.c) — blocking primitive for I/O and synchronization
// ===========================================================================
typedef struct wait_entry {
    process_t* proc;
    struct wait_entry* next;
} wait_entry_t;

typedef struct wait_queue {
    wait_entry_t* head;
    wait_entry_t* tail;
    spinlock_t lock;
    int initialized;
} wait_queue_t;

// Statically initialize a wait_queue_t at file scope.
#define WAIT_QUEUE_INITIALIZER  { NULL, NULL, { 0, 0xFFFFFFFF, NULL }, 1 }

void wq_init(wait_queue_t* wq);

// Block the current process on wq: marks it PROCESS_BLOCKED, records it on the
// queue, and schedules another process. Returns when the process is later
// woken AND rescheduled. Must be called from a context where it is safe to
// sleep (not inside an IRQ handler, interrupts enabled).
void wq_block_current(wait_queue_t* wq);

// Wake exactly one waiter (FIFO). Returns the woken process (ref NOT taken) or
// NULL if the queue was empty.
process_t* wq_wake_one(wait_queue_t* wq);

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
