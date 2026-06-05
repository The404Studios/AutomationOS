/*
 * O(1) Multi-Level Feedback Queue Scheduler
 * ==========================================
 *
 * Linux O(1) scheduler pattern with constant-time operations.
 * Scales to 100+ processes without performance degradation.
 *
 * Architecture:
 *  - 140 priority queues (0-139): higher priority = lower number
 *  - Active/Expired arrays: processes rotate between them
 *  - 140-bit bitmap: tracks non-empty queues for O(1) lookup
 *  - Priority calculation: base priority + dynamic adjustments
 *
 * Key Features:
 *  - O(1) enqueue: add to priority queue, set bitmap bit
 *  - O(1) dequeue: ffs(bitmap) finds highest priority queue
 *  - O(1) pick_next: constant time regardless of process count
 *  - Active/expired swap: when active empty, swap arrays
 *  - Each process gets DEFAULT_TIME_SLICE (10) timer ticks per quantum
 *  - Preempted processes maintain their remaining time slice (fairness fix)
 *  - SMP-safe with global scheduler lock (RACE-001 fix)
 *
 * Priority Mapping:
 *  - Nice value -20 to +19 (40 levels)
 *  - Maps to priority 0-139 (lower = higher priority)
 *  - Default nice = 0 → priority 100
 *  - Formula: priority = 100 + nice
 *
 * Important: Do NOT reset time_slice in scheduler_add_process()!
 *            This is critical for maintaining scheduling fairness.
 *            See bug fix documentation: docs/BUG_FIX_SCHEDULER_TIME_SLICE.md
 *
 * Locking Protocol (RACE-001 fix):
 *  - scheduler_lock protects runqueues, bitmaps, and ready_count
 *  - Must be held during all queue manipulations
 *  - Lock ordering: scheduler_lock is Level 1 (highest)
 */

#ifndef CONFIG_SMP

#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/perf.h"
#include "../../include/spinlock.h"
#include "../../include/usermode.h"
#include "../../include/tss.h"
#include "../../include/mem.h"

#ifdef PREEMPTIVE
// Optional hook the integrator can provide in pit.c so the timer tick counter
// keeps advancing while IRQ0 is routed through the preemptive stub (which
// bypasses the cooperative timer_handler). Declared weak so the kernel links
// even if it is not defined; if absent, ticks simply don't advance and
// timer_sleep()/uptime are inaccurate (preemption itself still works).
__attribute__((weak)) void pit_tick(void) {}

// PIC EOI ports/command (the preemptive IRQ0 stub bypasses idt.c's irq_handler,
// so we must acknowledge the master PIC ourselves before iretq).
#define PIC1_COMMAND 0x20
#define PIC_EOI      0x20
#endif

// ===========================================================================
// O(1) Scheduler Data Structures
// ===========================================================================

// Runqueue: 140 priority queues + bitmap for non-empty queues
typedef struct {
    process_t* queues[SCHED_PRIORITY_LEVELS];  // Priority queues (linked lists)
    process_t* tails[SCHED_PRIORITY_LEVELS];   // Tail pointers for O(1) enqueue
    uint64_t bitmap[SCHED_BITMAP_WORDS];       // 140 bits: 1 = non-empty queue
} runqueue_t;

// ===========================================================================
// SMP foundation brick 4: per-CPU structure (cpu_t), at CPU count == 1.
// ===========================================================================
// This is a PURE-RENAME BEHAVIORAL NO-OP. The scheduler used to refer to *the*
// active/expired runqueue and *the* ready count via file-scope globals, which
// implicitly assumes a single CPU. We relocate that per-CPU state into
// cpus[cpu_id()] and access it through this_cpu(). Because stubs.c::cpu_id()
// still returns 0, this_cpu() is ALWAYS &cpus[0], so every relocated field is
// just reached through cpus[0] instead of directly -- byte-for-byte identical
// scheduling decisions. The whole value is structural: bricks 5+ (per-CPU idle,
// per-CPU runqueues, migration, work-stealing) become a rename of cpu_id()'s
// return instead of a concurrent rewrite of the scheduler.
//
// See docs/PERCPU_DESIGN.md. cpu_id() is the hinge (stubs.c, returns 0 today;
// later reads the LAPIC id). MAX_CPUS is declared locally to avoid pulling in
// smp.h (which is part of the deferred, uncompiled SMP subsystem).
#ifndef MAX_CPUS
#define MAX_CPUS 8
#endif

// cpu_id(): single-CPU stub in stubs.c (returns 0). Declared here rather than
// including smp.h so we do not drag in the uncompiled SMP machinery; the symbol
// resolves to stubs.c at link time (smp.c, which also defines it, is NOT built).
extern uint32_t cpu_id(void);

// Per-CPU scheduler statistics. The two scheduler stats this kernel tracks
// (ctx_switches, cpu_ticks) are actually PER-PROCESS fields on process_t (see
// sched.h), not scheduler globals, so there is nothing to relocate here today.
// The struct is defined for the brick-5+ shape (a later brick that adds genuine
// per-CPU counters -- e.g. ticks observed on this CPU -- fills it in); at N=1 it
// stays zero and unused, which keeps this a no-op.
typedef struct cpu_stats {
    uint64_t reserved;  // placeholder so the field exists; unused at N=1
} cpu_stats_t;

// The per-CPU structure. At N=1 only cpus[0] is ever touched (cpu_id()==0).
typedef struct cpu {
    uint32_t    apic_id;        // LAPIC id; 0 for the BSP today
    int         online;         // 1 for the BSP (this CPU is up)
    process_t*  current_thread; // currently-running task on THIS cpu (mirror of
                                // the shared current_process, kept in lockstep at
                                // the process_set_current() chokepoint -- see the
                                // note below on why the global is not privatized
                                // yet). Live and correct; brick 5+ may make it the
                                // sole authority.
    process_t*  idle_thread;    // this CPU's idle task. A kernel thread that
                                // runs when no other processes are ready. It
                                // halts with interrupts enabled (sti; hlt) to
                                // consume zero CPU. Created by scheduler_init().
    // The O(1) MLFQ runqueue state, relocated AS-IS from the old file-scope
    // globals (active_rq / expired_rq / ready_count). ONE runqueue (cpu[0]'s) is
    // used exactly as before -- genuinely separate per-CPU runqueues with their
    // own picks are brick 6, NOT now.
    runqueue_t  rq_active;      // was the global active_rq
    runqueue_t  rq_expired;     // was the global expired_rq
    uint32_t    ready_count;    // was the global ready_count
    cpu_stats_t stats;          // per-CPU scheduler stats (unused at N=1)
} cpu_t;

static cpu_t cpus[MAX_CPUS];

// THE hinge. cpu_id()==0 today, so this is always &cpus[0]. Scheduler code
// written against this_cpu()->field resolves correctly when an AP later makes
// cpu_id() return a real LAPIC id -- with zero edits to this file.
#define this_cpu()  (&cpus[cpu_id()])

// Keep cpus[].current_thread in lockstep with the shared current_process global.
// process_set_current() (process.c -- the SINGLE dispatch chokepoint for both the
// cooperative and preemptive switch paths) calls this so this_cpu()->current_thread
// always names the task running on THIS cpu. At N=1 it is just an extra store into
// cpus[0] (cpu_id()==0); it changes NO observable behavior. The shared global
// remains the authority for the many subsystems that read current_process directly
// (PE loader, net, kill, ...); a later brick may make current_thread the sole
// source of truth once those readers are migrated. Defined here (not process.c)
// because cpus[] is file-local to the scheduler.
void cpu_set_current_thread(process_t* proc) {
    this_cpu()->current_thread = proc;
}

// RACE-001 fix: Global scheduler lock protects runqueues
// This prevents race conditions when multiple CPUs add/remove processes.
// Kept GLOBAL (shared) on purpose: a per-CPU runqueue lock is brick 6, when the
// runqueues actually become independent. At N=1 one lock guarding cpus[0]'s rq
// is byte-for-byte the old behavior.
static spinlock_t scheduler_lock;

// GPF-001 fix: Re-entrancy guard for context switch
// Set by context_switch() before calling context_switch_asm, cleared after return.
// Checked by timer_handler() to defer schedule() if a timer fires during switch.
volatile int scheduler_in_switch = 0;

// Scheduler constants
#define DEFAULT_TIME_SLICE 10  // 10 timer ticks (quantum) for nice 0 (NORMAL)
#define DEFAULT_NICE 0         // Default nice value (maps to priority 100)
#define NICE_TO_PRIORITY(nice) (100 + (nice))  // Convert nice (-20 to +19) to priority (0-139)

// ---------------------------------------------------------------------------
// PRIORITY-PROPORTIONAL TIME SLICE (the piece that makes nice actually shift
// CPU *share*, not just pick order).
//
// The O(1) active/expired runqueue is a strict-priority round-robin: within one
// active->expired cycle every ready process runs exactly once, so picking the
// lowest-nice process FIRST changes ORDER but not the NUMBER of turns. To make a
// lower nice translate into MORE CPU, we scale the per-turn quantum by nice:
// higher-priority (more-negative nice) processes get a LONGER time slice, so
// across equal numbers of turns they accrue proportionally more CPU ticks.
//
// This is the classic Linux-O(1) idea (task_timeslice scaled by static_prio).
// It takes effect wherever a process runs to quantum exhaustion -- i.e. the
// PREEMPTIVE build, where the timer slices non-yielding ring-3 burners by
// quantum. (In the cooperative build a process that voluntarily yields before
// its quantum expires is unaffected; cooperative share is governed by yield
// behavior, not the quantum.) Default nice 0 still yields exactly
// DEFAULT_TIME_SLICE / SCHED_QUANTUM_TICKS, so existing behavior is unchanged
// for normal-priority tasks.
//
// Mapping (monotonic in nice; clamped to a sane [min,max] tick band):
//   nice -20  -> ~3.0x default   (REALTIME, big slices)
//   nice -10  -> ~2.0x default   (HIGH)
//   nice   0  -> 1.0x default    (NORMAL, == DEFAULT_TIME_SLICE)
//   nice +10  -> ~0.5x default   (BACKGROUND, small slices)
//   nice +19  -> floor (1 tick)  (IDLE)
// Computed as DEFAULT_TIME_SLICE * (40 - (nice+20)) / 20, clamped to [1, 32].
#define SCHED_MIN_SLICE 1
#define SCHED_MAX_SLICE 32
static inline uint64_t priority_time_slice(int32_t nice) {
    if (nice < -20) nice = -20;
    if (nice >  19) nice =  19;
    // weight in [1..40], higher for more-negative nice (40 at -20, 1 at +19).
    int weight = 40 - (nice + 20);              // nice -20 -> 40 ; nice +19 -> 1
    long slice = (long)DEFAULT_TIME_SLICE * weight / 20;  // nice 0 (w=20) -> DEFAULT
    if (slice < SCHED_MIN_SLICE) slice = SCHED_MIN_SLICE;
    if (slice > SCHED_MAX_SLICE) slice = SCHED_MAX_SLICE;
    return (uint64_t)slice;
}

// ===========================================================================
// O(1) Bitmap Operations
// ===========================================================================

// Find first set bit in bitmap (returns priority 0-139, or -1 if empty)
static inline int bitmap_ffs(const uint64_t* bitmap) {
    for (int word = 0; word < SCHED_BITMAP_WORDS; word++) {
        if (bitmap[word] != 0) {
            // Find first set bit in this word using __builtin_ffsll
            // __builtin_ffsll returns 1-based index, we need 0-based
            int bit = __builtin_ffsll(bitmap[word]) - 1;
            return word * 64 + bit;
        }
    }
    return -1;  // No bits set
}

// Set bit in bitmap (mark queue as non-empty)
static inline void bitmap_set(uint64_t* bitmap, int priority) {
    int word = priority / 64;
    int bit = priority % 64;
    bitmap[word] |= (1ULL << bit);
}

// Clear bit in bitmap (mark queue as empty)
static inline void bitmap_clear(uint64_t* bitmap, int priority) {
    int word = priority / 64;
    int bit = priority % 64;
    bitmap[word] &= ~(1ULL << bit);
}

// Test bit in bitmap
static inline int bitmap_test(const uint64_t* bitmap, int priority) {
    int word = priority / 64;
    int bit = priority % 64;
    return (bitmap[word] & (1ULL << bit)) != 0;
}

// ===========================================================================
// O(1) Runqueue Operations
// ===========================================================================

// Initialize a runqueue
static void runqueue_init(runqueue_t* rq) {
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        rq->queues[i] = NULL;
        rq->tails[i] = NULL;
    }
    for (int i = 0; i < SCHED_BITMAP_WORDS; i++) {
        rq->bitmap[i] = 0;
    }
}

// Enqueue process at given priority (O(1))
static void runqueue_enqueue(runqueue_t* rq, process_t* proc, int priority) {
    // Invariant checks
    ASSERT_ALWAYS(rq != NULL);
    ASSERT_ALWAYS(proc != NULL);
    ASSERT_ALWAYS(proc->state == PROCESS_READY);

    // Clamp priority to valid range
    if (priority < 0) priority = 0;
    if (priority >= SCHED_PRIORITY_LEVELS) priority = SCHED_PRIORITY_LEVELS - 1;

    proc->next = NULL;

    if (rq->tails[priority] == NULL) {
        // Empty queue - add as head
        rq->queues[priority] = proc;
        rq->tails[priority] = proc;
        bitmap_set(rq->bitmap, priority);
    } else {
        // Non-empty queue - add to tail
        rq->tails[priority]->next = proc;
        rq->tails[priority] = proc;
    }
}

// Dequeue process from given priority (O(1))
static process_t* runqueue_dequeue(runqueue_t* rq, int priority) {
    ASSERT_ALWAYS(rq != NULL);

    if (priority < 0 || priority >= SCHED_PRIORITY_LEVELS) {
        return NULL;
    }

    process_t* proc = rq->queues[priority];
    if (proc == NULL) {
        return NULL;
    }

    rq->queues[priority] = proc->next;
    if (rq->queues[priority] == NULL) {
        // Queue is now empty
        rq->tails[priority] = NULL;
        bitmap_clear(rq->bitmap, priority);
    }

    proc->next = NULL;
    return proc;
}

// Pick highest priority process (O(1))
static process_t* runqueue_pick_next(runqueue_t* rq) {
    int priority = bitmap_ffs(rq->bitmap);
    if (priority < 0) {
        return NULL;  // No processes
    }
    return runqueue_dequeue(rq, priority);
}

// Remove specific process from runqueue (O(n) per priority level, but only one level)
static int runqueue_remove(runqueue_t* rq, process_t* proc, int priority) {
    if (priority < 0 || priority >= SCHED_PRIORITY_LEVELS) {
        return 0;
    }

    process_t* current = rq->queues[priority];
    process_t* prev = NULL;

    while (current != NULL) {
        if (current == proc) {
            // Found it - remove
            if (prev == NULL) {
                // Remove from head
                rq->queues[priority] = current->next;
                if (rq->queues[priority] == NULL) {
                    rq->tails[priority] = NULL;
                    bitmap_clear(rq->bitmap, priority);
                }
            } else {
                prev->next = current->next;
                if (current == rq->tails[priority]) {
                    rq->tails[priority] = prev;
                }
            }
            current->next = NULL;
            return 1;  // Found and removed
        }
        prev = current;
        current = current->next;
    }

    return 0;  // Not found
}

// Check if runqueue is empty
static inline int runqueue_is_empty(const runqueue_t* rq) {
    return bitmap_ffs(rq->bitmap) < 0;
}

// Swap active and expired runqueues
static void runqueue_swap(void) {
    cpu_t* c = this_cpu();
    runqueue_t tmp = c->rq_active;
    c->rq_active = c->rq_expired;
    c->rq_expired = tmp;
#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] Swapped active/expired runqueues\n");
#endif
}

// Get process priority
static inline int process_get_priority(process_t* proc) {
    // Map nice value (-20 to +19) to priority (0-139)
    // Lower priority number = higher priority
    int priority = NICE_TO_PRIORITY(proc->priority);

    // Clamp to valid range
    if (priority < 0) priority = 0;
    if (priority >= SCHED_PRIORITY_LEVELS) priority = SCHED_PRIORITY_LEVELS - 1;

    return priority;
}

// ===========================================================================
// Global sleep list — the TIMER half of the unified wait object
// ===========================================================================
// Intrusive singly-linked list of processes that armed a wakeup DEADLINE when
// they blocked. Each node links via proc->sleep_next and carries an absolute
// wake_deadline in timer_get_ticks() units (the PIT runs at 1000 Hz, so 1 tick
// == 1 ms). It is populated by wait_object_block(wo, deadline) — the single
// block primitive — for any timer-armed wait (sys_sleep is the canonical one):
//   block:      state=BLOCKED, sleep_list_push(), switch away (not re-queued).
//   timer scan: unlink when wake_deadline<=now, state=READY, scheduler_add_process.
//
// A timer-armed waiter is on BOTH this list AND its wait_object's waiter list.
// Whichever fires first (timeout here, or a wait_object_signal) re-readies it;
// the OTHER linkage is cleaned up afterward:
//   • signal first: wait_object_signal calls sleep_list_remove() before
//     re-readying, taking the waiter off this list.
//   • timer first (here): we just re-ready via the sleep list; the waiter is
//     still linked on its wait_object, and the resuming wait_object_block()
//     unlinks itself from the wait_object (dropping the object-ref) on return.
// So this scan does NOT need to touch the wait_object — keeping the IRQ path
// free of cross-lock ordering. A pure-timer wait (no signal possible) is just
// the degenerate case where only this list ever fires.
//
// Single-core: the only concurrent mutator of this list is the timer IRQ's
// wakeup scan. The block path may run with IF=1, so we bracket every mutation
// with save_flags_cli()/restore_flags() — that makes the IRQ unable to observe
// a half-linked node (and the IRQ scan itself already runs with IF=0). No
// spinlock is needed beyond this on a uniprocessor.
static process_t* g_sleep_list = NULL;

void sleep_list_push(process_t* proc) {
    if (!proc) return;
    uint64_t flags = save_flags_cli();
    proc->sleep_next = g_sleep_list;
    g_sleep_list = proc;
    restore_flags(flags);
}

// Remove `proc` from the sleep list if present (idempotent; no-op if absent).
// Called when a timer-armed waiter is woken by a SIGNAL instead of its timeout
// (wait_object_signal), and again as belt-and-braces on the wait_object_block
// resume path. Bracketed with cli/restore so it is race-free vs the IRQ scan.
void sleep_list_remove(process_t* proc) {
    if (!proc) return;
    uint64_t flags = save_flags_cli();
    process_t** link = &g_sleep_list;
    while (*link) {
        if (*link == proc) {
            *link = proc->sleep_next;
            proc->sleep_next = NULL;
            break;
        }
        link = &(*link)->sleep_next;
    }
    restore_flags(flags);
}

// Walk the sleep list and wake every process whose deadline has arrived. Called
// once per timer tick from BOTH the cooperative pit.c handler and the preemptive
// schedule_from_irq() (after the tick counter advances). Unlinks due sleepers,
// marks them READY, and hands them to scheduler_add_process() (which takes its
// own reference). Runs with interrupts disabled in the IRQ context; we also save
// flags so it is safe if ever invoked from an IF=1 path.
//
// A woken sleeper that was a timer-armed wait_object waiter is left linked on
// its wait_object here (with its object-ref intact); the resuming
// wait_object_block() unlinks it and drops that ref. We only re-ready genuinely
// BLOCKED sleepers: a process a signal already re-readied (and removed via
// sleep_list_remove) is gone from this list, but the BLOCKED guard is kept as
// defense in depth so a stray non-blocked node can never be doubly re-added.
void sleep_list_wake_due(uint64_t now) {
    uint64_t flags = save_flags_cli();
    process_t** link = &g_sleep_list;
    while (*link) {
        process_t* it = *link;
        if (it->wake_deadline <= now) {
            // Unlink first so the list is consistent before we touch the
            // scheduler (scheduler_add_process clobbers it->next, a different
            // field than sleep_next, but we keep the invariant tight anyway).
            *link = it->sleep_next;
            it->sleep_next = NULL;
            if (it->state == PROCESS_BLOCKED) {
                process_set_ready(it);
                scheduler_add_process(it);
            }
            // *link now points at the successor; do NOT advance link.
        } else {
            link = &it->sleep_next;
        }
    }
    restore_flags(flags);
}

// ===========================================================================
// Idle thread function - runs when no other processes are ready
// ===========================================================================
// This is a minimal kernel thread that executes when all runqueues are empty.
// It consumes zero CPU by halting until the next interrupt (HLT instruction).
// The timer interrupt will wake it, re-run the scheduler, and either pick a
// newly-ready process or return here to halt again.
//
// CRITICAL: This must run with interrupts ENABLED (sti before hlt), otherwise
// the CPU would halt forever (no interrupt to wake it). The sti/hlt sequence
// is atomic on x86-64: interrupts are checked AFTER hlt completes, so there
// is no race between enabling interrupts and halting.
//
// This runs in kernel mode (ring 0) with the idle thread's kernel stack, NOT
// in user mode. It never returns - the scheduler switches away from it when
// a real process becomes ready.
static void idle_thread_func(void) {
    kprintf("[SCHEDULER] Idle thread started (CPU %d)\n", cpu_id());

    while (1) {
        // Halt with interrupts enabled - wake on next interrupt
        // This is the x86-64 idle loop pattern: atomically enable IRQs and halt
        __asm__ volatile("sti; hlt" ::: "memory");

        // When we wake here (timer interrupt), loop back to hlt unless the
        // scheduler switches us out. In practice the timer's schedule() call
        // picks a newly-ready process and we never return here - but if both
        // runqueues stay empty we re-halt immediately.
    }
}

// Create the per-CPU idle thread. Called once during scheduler_init() for the
// BSP (cpu 0). In an SMP build this would be called for each AP too.
// Returns the created idle thread PCB, or NULL on failure.
static process_t* create_idle_thread(uint32_t cpu_id_val, int adopt_pid0) {
    (void)cpu_id_val;
    // Allocate the idle thread PCB (memset-zeroed by process_create)
    process_t* idle = process_create("idle", (void*)idle_thread_func);
    if (!idle) {
        return NULL;
    }

    // The BSP's idle thread (adopt_pid0=1) is re-homed onto the reserved PID 0.
    // process_create() hands out PID 1 to this (the first-ever process); leaving it
    // there bumps /sbin/init to PID 2, and init.c exits with "Not PID 1!". Moving
    // idle to slot 0 makes the first real process (init) PID 1. See process_adopt_pid0().
    // A secondary CPU's idle thread (adopt_pid0=0) is created AFTER init already has
    // PID 1, so it just takes the next free PID -- it is never enqueued (a fallback
    // only), so its PID is irrelevant to scheduling.
    if (adopt_pid0) {
        process_adopt_pid0(idle);
    }

    // The idle thread is a KERNEL thread: it runs in ring 0, not ring 3.
    // Set its context.rip to the idle function and rsp to its kernel stack.
    // It has NO user stack or user entry (user_rsp/user_entry stay 0).
    idle->context.rip = (uint64_t)idle_thread_func;
    idle->context.rsp = (uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE - 16;
    idle->context.rflags = 0x202;  // IF=1 (interrupts enabled), bit 1 reserved
    // context.cr3 is already set by process_create (kernel page table)

    // Idle runs at the LOWEST priority (nice +19 -> priority 139)
    idle->priority = 19;  // IDLE priority

    // Idle never exhausts its quantum (it yields via hlt), but set a nominal
    // slice so priority_time_slice doesn't return 0.
    idle->time_slice = 1;

    // Idle is always RESUME_CRETURN (it's a kernel thread, never preempted in
    // ring 3). When the scheduler switches to it, context_switch `ret`s into
    // idle_thread_func above.
    idle->resume_mode = RESUME_CRETURN;

    // Mark it ready but do NOT add to runqueue - it's the fallback when the
    // runqueue is empty, not a normal schedulable process.
    process_set_ready(idle);

    return idle;
}

void scheduler_init(void) {
    kprintf("[SCHEDULER] Initializing O(1) multi-level feedback queue scheduler...\n");

    // RACE-001 fix: Initialize scheduler lock
    spin_lock_init(&scheduler_lock);

#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
    // Audit fix: eagerly build the FPU template HERE (BSP, before CPU1 is brought
    // online) so CPU1's context_prime_fpu() never races CPU0's first context_switch
    // to lazily initialize it. Idempotent.
    {
        extern void context_fpu_template_init(void);
        context_fpu_template_init();
    }
#endif

    // SMP brick 4: bring the BSP's per-CPU slot online. At N=1 this is the only
    // CPU; cpu_id()==0 so this_cpu()==&cpus[0]. apic_id 0 == the BSP. The rest of
    // cpus[] stays zeroed (.bss) and untouched until APs exist (brick 5+).
    cpus[0].apic_id = 0;
    cpus[0].online  = 1;

    // Initialize active and expired runqueues (now living in cpus[0].rq_*).
    runqueue_init(&this_cpu()->rq_active);
    runqueue_init(&this_cpu()->rq_expired);
    this_cpu()->ready_count = 0;

    // Create the idle thread for this CPU (BSP). This is the fallback when no
    // other processes are runnable. It is added to the runqueue at the lowest
    // priority (nice +19 -> priority 139), so it only runs when nothing else is
    // ready. Also stored in cpu_t.idle_thread for reference.
    this_cpu()->idle_thread = create_idle_thread(cpu_id(), 1 /* adopt PID 0 */);
    if (!this_cpu()->idle_thread) {
        kernel_panic("scheduler_init: Failed to create idle thread");
    }

    // The idle thread is NOT enqueued. It is a FALLBACK that scheduler_pick_next()
    // returns only when both runqueues are empty — never a regular queued process.
    // (Enqueuing it breaks the active/expired model: as a live entry in rq_active
    // it keeps active non-empty, so pick_next never swaps in rq_expired, and any
    // process queued to expired — i.e. EVERY freshly spawned process — can never be
    // picked. That froze the whole system right after init spawned its services.)

    kprintf("[SCHEDULER] O(1) scheduler initialized:\n");
    kprintf("[SCHEDULER]   - Priority levels: %d (0-139)\n", SCHED_PRIORITY_LEVELS);
    kprintf("[SCHEDULER]   - Time slice: %d ticks\n", DEFAULT_TIME_SLICE);
    kprintf("[SCHEDULER]   - Algorithm: Active/Expired double-buffering\n");
    kprintf("[SCHEDULER]   - Complexity: O(1) enqueue, O(1) dequeue, O(1) pick_next\n");
    kprintf("[SCHEDULER]   - SMP-safe: Yes\n");
    kprintf("[SCHEDULER]   - Idle thread: PID %d\n", this_cpu()->idle_thread->pid);
}

#ifdef SMP_SCHED
// ===========================================================================
// scheduler_init_secondary_cpu() — SMP scheduler Brick D
// ===========================================================================
// Populate a secondary CPU's per-CPU slot (cpus[cpu]) so that, once that CPU
// starts taking timer interrupts and calling schedule_from_irq()/scheduler_pick_next()
// (Bricks E/F), this_cpu() resolves to a fully-formed cpu_t: its own empty
// active/expired runqueues, its own ready_count, and its OWN idle thread (with its
// OWN kernel stack) as the empty-runqueue fallback.
//
// MUST be called by the BSP AFTER /sbin/init has claimed PID 1 (the idle thread
// here does NOT adopt PID 0; it takes a normal free PID, which is fine because it
// is never enqueued). Runs on the BSP -- it only writes cpus[cpu] data; the target
// CPU is still in the coprocessor loop (no dispatch yet, Brick D takes no IRQ on it).
// Returns 1 on success, 0 on failure (caller logs + continues; AP just stays a
// coprocessor).
int scheduler_init_secondary_cpu(uint32_t cpu, uint32_t apic_id) {
    if (cpu == 0 || cpu >= MAX_CPUS) return 0;

    cpus[cpu].apic_id = apic_id;
    cpus[cpu].online  = 1;

    runqueue_init(&cpus[cpu].rq_active);
    runqueue_init(&cpus[cpu].rq_expired);
    cpus[cpu].ready_count = 0;

    process_t* idle = create_idle_thread(cpu, 0 /* do NOT adopt PID 0 */);
    if (!idle) {
        cpus[cpu].online = 0;
        return 0;
    }
    cpus[cpu].current_thread = idle;   // start CPU as "running idle" until it dispatches
    cpus[cpu].idle_thread    = idle;

    /* NOTE: CPU1's idle thread runs the AP scheduler loop on the AP BOOT stack (see
     * ap_main). Its pre-seeded context (idle_thread_func / idle->kernel_stack from
     * create_idle_thread) is the SAVE TARGET only: the first cooperative switch
     * (idle->thread) overwrites idle->context with the live boot-stack loop state via
     * fxsave64/context_switch_asm, and later switches back restore exactly that. So
     * idle is self-consistent without an explicit rip/cr3 override here. */

    kprintf("[SCHEDULER] CPU%u secondary slot online: idle PID %d stack=%p "
            "(CPU0 idle PID %d stack=%p) rq_active=%p\n",
            cpu, idle->pid, (void*)idle->kernel_stack,
            cpus[0].idle_thread->pid, (void*)cpus[0].idle_thread->kernel_stack,
            (void*)&cpus[cpu].rq_active);
    return 1;
}

// Enqueue `proc` onto a SPECIFIC CPU's EXPIRED runqueue (cross-CPU pinning) — the
// affinity primitive for Brick F. Mirrors scheduler_add_process()'s exact ref /
// lock / idempotency discipline, but targets cpus[cpu].rq instead of this_cpu()'s
// (scheduler_add_process always uses this_cpu(), so the BSP can't use it to pin to
// CPU1). The global scheduler_lock covers ALL CPUs' runqueues, so this is safe to
// call from the BSP for a CPU1 queue. Takes a ref that pick_next() later transfers.
void scheduler_add_process_to_cpu(process_t* proc, uint32_t cpu) {
    if (!proc || cpu >= MAX_CPUS) return;
    if (proc->state == PROCESS_TERMINATED) return;
    // Idle is the empty-runqueue FALLBACK and must NEVER be enqueued (enqueuing it
    // breaks the active/expired invariant and leaks a ref). Guard explicitly.
    if (proc == cpus[cpu].idle_thread) return;

    // Single critical section: (check on_queue, take the queue's ref, set the flag,
    // enqueue) are done atomically under scheduler_lock so a concurrent
    // scheduler_remove_process can't orphan the ref (process_ref is a lock-free
    // atomic add, safe to call here). Tighter than the legacy scheduler_add_process
    // (which refs outside the lock) -- the AP path must be race-clean.
    spin_lock(&scheduler_lock);
    if (!proc->on_queue) {
        process_ref(proc);                   // queue holds a ref (UAF guard)
        process_set_ready(proc);
        proc->on_queue = 1;
        int priority = process_get_priority(proc);
        runqueue_enqueue(&cpus[cpu].rq_expired, proc, priority);
        cpus[cpu].ready_count++;
    }
    spin_unlock(&scheduler_lock);
}
#endif /* SMP_SCHED */

#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
// ===========================================================================
// AP-SAFE SCHEDULER (Brick F) — CPU1 SCHEDULER-MODE dispatch
// ===========================================================================
// A DEDICATED dispatcher for CPU1, the AP-safe twin of the BSP's
// schedule()/schedule_from_irq(). Two non-negotiable rules that make it AP-safe:
//   1. NEVER write the global current_process. It uses cpu_set_current_thread()
//      (which writes ONLY this_cpu()->current_thread == cpus[1].current_thread).
//      Writing the global from CPU1 would corrupt the BSP's notion of "current".
//   2. NEVER do a PIC EOI. The LAPIC timer ISR (lapic_tick) already EOIs the LAPIC;
//      a PIC EOI from CPU1 would wedge the BSP's IRQ0.
// scheduler_pick_next(), scheduler_add_process(), runqueue_* are all this_cpu()-
// based, so on CPU1 they naturally operate on cpus[1]'s runqueue.
//
// Built sub-brick by sub-brick (see docs/SMP_SCHEDULER_PLAN.md, user's F1..F5 split):
//   F1 (this commit): SKELETON. Only ever "schedules" CPU1 idle. No context switch,
//      no user process. Proves the loop + ISR run on CPU1 with 0 panics.
//   F2: cooperative switch idle <-> ONE pinned KERNEL thread (context_switch_asm).
//   F3: preempt/dispatch ONE pinned RING-3 process (context_save_irq/load_irq).
//   F4/F5: real apps on CPU1, compositor pinned to CPU0.

// ap_schedule_from_irq() — CPU1 LAPIC-timer preemption entry. Called from
// lapic_tick() AFTER the LAPIC EOI. The HARD ring-3 guard comes FIRST: only ever
// preempt code the timer caught running in ring 3 (CPL==3). CPU1's scheduler loop
// runs in ring 0, so in F1 this returns immediately every tick (the skeleton
// proof). Doing the guard before ANY scheduler_lock acquisition ALSO prevents a
// same-CPU self-deadlock: CPU1 must never try to take scheduler_lock from this ISR
// while the ring-0 code it interrupted already holds it.
void ap_schedule_from_irq(interrupt_frame_t* frame) {
    if ((frame->cs & 3) != 3) {
        return;  // kernel/idle interrupted: never preempt. (F1: ALWAYS taken.)
    }
    // F3+ adds: per-CPU quantum decrement, scheduler_pick_next() on cpus[1],
    //           context_save_irq(current)/context_load_irq(next), CR3 switch,
    //           cpu_set_current_thread(next). All without touching the global.
    (void)frame;
}

// ap_cooperative_schedule() — the cooperative half (the AP's voluntary yield/idle
// check). Pick CPU1's next runnable; if it's the idle fallback (empty runqueue) or
// the current thread, return so the caller hlts. Otherwise context_switch_asm() to
// it (cooperative ring-0 switch). F2 proves idle -> ONE pinned kernel thread.
//
// AP-safety: cpu_set_current_thread() writes ONLY cpus[1].current_thread (NEVER the
// global current_process — that would clobber the BSP). scheduler_pick_next()/
// scheduler_add_process() are this_cpu()-based so they act on cpus[1]'s runqueue.
void ap_cooperative_schedule(void) {
    cpu_t* cpu = this_cpu();                  // CPU1 (cpu_id()==1)
    process_t* current = cpu->current_thread; // idle (initially)
    process_t* next = scheduler_pick_next();  // this_cpu()-based -> cpus[1].rq
    if (next == cpu->idle_thread || next == current) {
        return;  // nothing else runnable (pick_next gives idle on an empty rq; it
                 // transfers NO ref for the idle fallback, so nothing to release).
    }

    // Requeue the outgoing thread ONLY if it is a runnable non-idle process that
    // voluntarily yielded (state still RUNNING). The idle fallback is never queued.
    if (current != cpu->idle_thread && current->state == PROCESS_RUNNING) {
        scheduler_add_process(current);       // this_cpu()==cpus[1] -> cpus[1].rq
    }

    cpu_set_current_thread(next);             // cpus[1].current_thread = next (per-CPU)
    next->state = PROCESS_RUNNING;

    // Point CPU1's TSS.RSP0 + SYSCALL kernel stack at `next`'s kernel stack (no-op
    // for a pure ring-0 kernel thread, required once `next` is a ring-3 process).
    if (next->kernel_stack) {
        uint64_t kstack = ((uint64_t)next->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
        tss_set_kernel_stack(kstack);         // cpu_id()==1 -> tss_array[1]/kernel_rsp_save_arr[1]
    }

    // Fix D6: prime `next`'s FPU state (context_switch_asm fxrstor's it directly,
    // bypassing context_switch()'s template priming) so a fresh process's MXCSR is
    // clean, not the all-zero #XM trap. Writes only next's own fpu_state (AP-safe).
    context_prime_fpu(next);

    // Cooperative ring-0 context switch. Saves `current`'s kernel resume point into
    // its context and `ret`s into `next`. Control returns here only when `current`
    // is later switched back in (e.g. `next` yields/blocks/exits).
    context_switch_asm(current, next);
}

// ap_enter_scheduler() — ap_main's one-way entry into SCHEDULER mode (fix D1). Jumps
// CPU1 into its idle thread, which RUNS ap_scheduler_loop on idle->kernel_stack.
// context_switch_asm(NULL, idle): from==NULL skips the save, restores idle's context
// (CR3=idle's PML4, FPU, RSP=idle->kernel_stack, RIP=ap_scheduler_loop, IF=1) and
// 'ret's into it. The AP boot stack is abandoned (ap_main never returns). NEVER
// returns. After this, idle is a proper running process so all later switches into
// and out of idle are self-consistent (no boot-stack overwrite of idle->context).
// Diagnostic: how far CPU1 got through scheduler-mode entry (BSP reads it; the AP
// must not touch the serial port). 3 = ap_scheduler_loop running.
// (The earlier ap_enter_scheduler / context_switch_asm(NULL, idle) approach wedged
// CPU1 at the from=NULL fxrstor-before-stack-switch path -- REMOVED. ap_main runs
// ap_scheduler_loop() directly on the boot stack instead; idle is the save target.)
volatile uint64_t ap_dbg_stage = 0;

// ---------------------------------------------------------------------------
// Brick F2 test workload: ONE pinned ring-0 kernel thread.
// ---------------------------------------------------------------------------
// A trivial kernel thread that increments a shared counter forever. Pinned to CPU1
// (enqueued onto cpus[1].rq). The AP scheduler loop's first ap_cooperative_schedule()
// switches CPU1 into it; the BSP then reads ap_kthread_counter climbing — PROVING
// CPU1 context-switched into a scheduled thread and runs it in parallel with CPU0's
// desktop. It runs in ring 0, so the LAPIC timer's ap_schedule_from_irq ring-3 guard
// leaves it un-preempted (F3 exercises ring-3 preemption).
volatile uint64_t ap_kthread_counter = 0;

static void ap_test_kthread_fn(void) {
    for (;;) {
        ap_kthread_counter++;
        __asm__ volatile("pause");
    }
}

// Called ONCE by the BSP (after scheduler_init_secondary_cpu, so cpus[1] is ready
// and init owns PID 1). Creates the kernel thread and pins it to CPU1.
void ap_spawn_test_kthread(void) {
    process_t* t = process_create("ap_ktest", (void*)ap_test_kthread_fn);
    if (!t) {
        kprintf("[SMP] Brick F2: failed to create AP test kthread\n");
        return;
    }
    t->context.rip    = (uint64_t)ap_test_kthread_fn;
    t->context.rsp    = (uint64_t)t->kernel_stack + KERNEL_STACK_SIZE - 16;
    t->context.rflags = 0x202;                 // IF=1 (runs with interrupts enabled)
    t->priority       = 0;
    t->time_slice     = 1;
    t->resume_mode    = RESUME_CRETURN;        // kernel thread: resumes via ctx_switch ret
    scheduler_add_process_to_cpu(t, 1);        // pin to CPU1
    kprintf("[SMP] Brick F2: pinned AP test kthread PID %d to CPU1 (kernel stack=%p)\n",
            t->pid, (void*)t->kernel_stack);
}

// ap_scheduler_loop() — CPU1's top-level SCHEDULER-mode loop (replaces the
// coprocessor worker loop under SMP_SCHED_DISPATCH). Runs forever on the AP's boot
// stack with cpus[1].current_thread == cpus[1].idle_thread (set by Brick D's
// scheduler_init_secondary_cpu). Each pass cooperatively checks CPU1's runqueue,
// then hlts until the next LAPIC tick wakes it. NEVER returns.
void ap_scheduler_loop(void) {
    // D7 guard: this loop uses this_cpu()->rq (== cpus[cpu_id()].rq). If cpu_id()
    // ever returned 0 on the AP (LAPIC not enabled / apic_id unmapped), CPU1 would
    // operate on the BSP's runqueue and corrupt CPU0. Convert that silent
    // cross-CPU corruption into a loud halt -- the ONE latent issue whose blast
    // radius is the BSP. (cpu_id() is verified ==1 by the Brick A checkpoint;
    // ap_main enables the LAPIC + populates cpus[1].apic_id before we get here.)
    if (cpu_id() != 1) {
        kernel_panic("ap_scheduler_loop: cpu_id() != 1 -- AP would corrupt CPU0's runqueue");
    }
    extern volatile uint64_t ap_dbg_stage;
    ap_dbg_stage = 3;
    for (;;) {
        ap_cooperative_schedule();
        __asm__ volatile("hlt");
    }
}
#endif // SMP_SCHED && SMP_SCHED_DISPATCH

// ===========================================================================
// scheduler_shutdown() — quiesce scheduler for reboot/poweroff or SMP CPU-offline
// ===========================================================================
// Teardown counterpart to scheduler_init(). Drains all runqueues (active and
// expired), clears the global sleep list, and releases references to all queued
// processes. After this call the scheduler is STOPPED — no further scheduling
// can occur. Intended for:
//   • System reboot/poweroff (power.c power_shutdown path): stops all CPUs,
//     cleanly drains all tasks before halting.
//   • SMP CPU-offline (brick teardown): when taking an AP offline, drains its
//     per-CPU runqueue and migrates or terminates its tasks before marking the
//     CPU unavailable.
// The scheduler_lock is KEPT initialized (not destroyed): it is a simple spinlock
// with no heap allocation, so there is nothing to free. Re-initializing it after
// shutdown (e.g. for a kexec-style reload) is safe.
//
// Process ref discipline:
//   • Each runqueue slot holds ONE process_ref per enqueued process (taken by
//     scheduler_add_process). Dequeuing here releases that ref via process_unref.
//   • The sleep list does NOT hold a ref directly (the ref is on the process's
//     wait_object); we only clear the list structure, not unref sleepers.
//   • The current_process global ref is NOT touched here (callers like
//     power_shutdown own the decision to terminate/unref the running task).
// Idempotent: calling twice is safe (a drained scheduler is left drained).
void scheduler_shutdown(void) {
    kprintf("[SCHEDULER] Shutting down scheduler (draining runqueues and sleep list)...\n");

    // 1) Acquire the scheduler lock to prevent concurrent add/remove operations
    //    while we drain. This blocks any timer IRQ that tries to re-queue a
    //    process mid-shutdown.
    spin_lock(&scheduler_lock);

    uint32_t drained_count = 0;

    // 2) Drain the ACTIVE runqueue: dequeue every priority level, releasing
    //    the scheduler's reference for each process.
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* proc;
        while ((proc = runqueue_dequeue(&this_cpu()->rq_active, prio)) != NULL) {
            proc->on_queue = 0;
            this_cpu()->ready_count--;
            drained_count++;
#ifndef SCHEDULER_QUIET
            kprintf("[SCHEDULER] Drained from active[%d]: '%s' (PID %d)\n",
                    prio, proc->name, proc->pid);
#endif
            // Release the reference that scheduler_add_process took.
            process_unref(proc);
        }
    }

    // 3) Drain the EXPIRED runqueue.
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* proc;
        while ((proc = runqueue_dequeue(&this_cpu()->rq_expired, prio)) != NULL) {
            proc->on_queue = 0;
            this_cpu()->ready_count--;
            drained_count++;
#ifndef SCHEDULER_QUIET
            kprintf("[SCHEDULER] Drained from expired[%d]: '%s' (PID %d)\n",
                    prio, proc->name, proc->pid);
#endif
            process_unref(proc);
        }
    }

    // 4) Clear the global sleep list. We do NOT unref the sleepers here (their
    //    refs are on the wait_objects they are parked on; the wait-object teardown
    //    or process termination owns those refs). We only clear the intrusive
    //    sleep_next linkage so no timer wakeup scan can touch a stale list.
    uint64_t flags = save_flags_cli();
    process_t* sleeper = g_sleep_list;
    uint32_t sleep_count = 0;
    while (sleeper) {
        process_t* next = sleeper->sleep_next;
        sleeper->sleep_next = NULL;
        sleeper = next;
        sleep_count++;
    }
    g_sleep_list = NULL;
    restore_flags(flags);

    // 5) Mark the BSP offline (mirrors the scheduler_init online=1 step). At
    //    N=1 this is cosmetic; at N>1 (SMP brick 5+) it prevents a CPU-offline
    //    flow from re-using a drained CPU's runqueue. The scheduler is STOPPED.
    this_cpu()->online = 0;

    spin_unlock(&scheduler_lock);

    kprintf("[SCHEDULER] Scheduler shutdown complete\n");
    kprintf("[SCHEDULER]   - Runqueues drained: %u processes\n", drained_count);
    kprintf("[SCHEDULER]   - Sleep list cleared: %u sleepers\n", sleep_count);
    kprintf("[SCHEDULER]   - Status: STOPPED\n");
}


void scheduler_add_process(process_t* proc) {
    PERF_START(PERF_OP_SCHEDULER_ADD);

    if (!proc) {
        kprintf("[SCHEDULER] Warning: Attempted to add NULL process\n");
        PERF_END(PERF_OP_SCHEDULER_ADD);
        return;
    }

    if (proc->state == PROCESS_TERMINATED) {
        kprintf("[SCHEDULER] Warning: Attempted to add terminated process %d\n", proc->pid);
        PERF_END(PERF_OP_SCHEDULER_ADD);
        return;
    }

    // Idempotency guard: a PCB has a single `next` link, so it can be in the
    // ready queue at most once. Re-adding an already-queued process would
    // overwrite its `next` (truncating the list / forming a cycle) and leak the
    // extra reference. Bail out if it is already enqueued.
    // SINGLE critical section (RACE-001): the on_queue test, the ref, the enqueue, and
    // the ready_count bump must all be ATOMIC under scheduler_lock. The old code dropped
    // the lock between the on_queue test and the ref/enqueue, so two CPUs (on SMP_SCHED,
    // once Brick-F dispatch makes a second CPU call this) could BOTH observe on_queue==0,
    // both process_ref, and both runqueue_enqueue the same PCB -- overwriting its single
    // `next` link (list truncation / cycle) and leaking the extra ref. Latent on the
    // uniprocessor-cooperative default; this mirrors the already-correct single-critical-
    // section pattern in scheduler_add_process_to_cpu. process_ref is a lock-free atomic
    // add, and process_set_ready / process_get_priority take no locks, so all are safe
    // to call while holding scheduler_lock.
    spin_lock(&scheduler_lock);
    if (proc->on_queue) {
        spin_unlock(&scheduler_lock);
        PERF_END(PERF_OP_SCHEDULER_ADD);
        return;
    }

    process_ref(proc);          // the queue's reference (prevents use-after-free)
    process_set_ready(proc);
    proc->on_queue = 1;

    // CRITICAL: Do NOT reset time_slice here!
    // This function is called when:
    //   1. Process is preempted by timer (time_slice = 0)
    //   2. Process voluntarily yields (time_slice > 0)
    // We must preserve the time_slice value to maintain scheduling fairness.
    // Time slice is only reset in scheduler_pick_next() when it's 0.

    // Calculate priority from nice value
    int priority = process_get_priority(proc);

    // Add to EXPIRED runqueue (processes go to expired after using quantum)
    // This implements round-robin within each priority level
    // When time_slice == 0, process exhausted quantum → expired queue
    // When time_slice > 0, process yielded early → expired queue (maintains fairness)
    runqueue_enqueue(&this_cpu()->rq_expired, proc, priority);

    this_cpu()->ready_count++;

    spin_unlock(&scheduler_lock);

#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] Added process '%s' (PID %d) to expired queue (priority: %d, time_slice: %d, ref_count: %d)\n",
            proc->name, proc->pid, priority, proc->time_slice, proc->ref_count);
#endif

    PERF_END(PERF_OP_SCHEDULER_ADD);
}

// Bonus ACTIVE re-queues granted to an above-normal-priority process when it
// yields, derived from nice. nice 0 and positive => 0 (NORMAL/background behave
// exactly as before: straight to expired). More-negative nice => more bonus
// turns, so a HIGH/REALTIME yielder runs several times per swap cycle while a
// NORMAL/BACKGROUND peer runs once -> higher-priority gets proportionally more
// CPU. Clamped so even REALTIME (nice -20) gets a bounded boost (no starvation).
//   nice 0   -> 0     (unchanged)
//   nice -10 -> 5     (HIGH:  ~6 turns per cycle vs 1)
//   nice -20 -> 10    (REALTIME)
//   nice +k  -> 0     (BACKGROUND/IDLE: unchanged)
#define PRIORITY_YIELD_BOOST_MAX 10
static inline int priority_yield_boost(int32_t nice) {
    if (nice >= 0) return 0;
    int b = (-nice) / 2;                 // nice -10 -> 5, nice -20 -> 10
    if (b > PRIORITY_YIELD_BOOST_MAX) b = PRIORITY_YIELD_BOOST_MAX;
    return b;
}

void scheduler_yield_requeue(process_t* proc) {
    // Priority-weighted twin of scheduler_add_process() for the cooperative
    // yield path. Identical reference/idempotency/locking discipline; the ONLY
    // difference is the target runqueue: while a high-priority process has
    // yield_boost remaining we re-queue it to ACTIVE (so pick_next picks it again
    // ahead of lower-priority peers this same cycle), decrementing the boost;
    // otherwise we refill its boost from nice and rotate it to EXPIRED exactly
    // like the normal path. For nice >= 0 the boost is always 0, so this routes
    // straight to EXPIRED -- byte-for-byte the old behavior.
    if (!proc) return;
    if (proc->state == PROCESS_TERMINATED) return;

    spin_lock(&scheduler_lock);
    if (proc->on_queue) { spin_unlock(&scheduler_lock); return; }
    spin_unlock(&scheduler_lock);

    process_ref(proc);

    spin_lock(&scheduler_lock);
    process_set_ready(proc);
    proc->on_queue = 1;
    int priority = process_get_priority(proc);

    if (proc->yield_boost > 0) {
        // Spend one bonus turn: re-enter ACTIVE so this higher-priority process
        // is eligible to be picked again immediately (ahead of lower priorities).
        proc->yield_boost--;
        runqueue_enqueue(&this_cpu()->rq_active, proc, priority);
    } else {
        // Out of bonus turns (or never had any): refill from nice and rotate to
        // EXPIRED, guaranteeing lower-priority ready processes get their turn
        // after the next active/expired swap (anti-starvation preserved).
        proc->yield_boost = priority_yield_boost(proc->priority);
        runqueue_enqueue(&this_cpu()->rq_expired, proc, priority);
    }
    this_cpu()->ready_count++;
    spin_unlock(&scheduler_lock);
}

void scheduler_remove_process(process_t* proc) {
    if (!proc) return;

    PERF_START(PERF_OP_SCHEDULER_REMOVE);

    // KILL-FIX-003: Idempotent removal guard.
    //
    // A process that is not in the ready queue has proc->on_queue == 0.
    // We check this first for a fast exit, then search both active and
    // expired runqueues. If the process is not found we simply return —
    // no double-unref, no double-print. This makes calling
    // scheduler_remove_process() twice on the same process completely safe.

    // RACE-001 fix: Acquire scheduler lock before queue manipulation
    spin_lock(&scheduler_lock);

    if (!proc->on_queue) {
        spin_unlock(&scheduler_lock);
        PERF_END(PERF_OP_SCHEDULER_REMOVE);
        return;
    }

    // Calculate priority to narrow search
    int priority = process_get_priority(proc);

    // Try to remove from active runqueue first
    int found = runqueue_remove(&this_cpu()->rq_active, proc, priority);

    // If not in active, try expired
    if (!found) {
        found = runqueue_remove(&this_cpu()->rq_expired, proc, priority);
    }

    if (found) {
        proc->on_queue = 0;
        this_cpu()->ready_count--;

        spin_unlock(&scheduler_lock);

#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Removed process '%s' (PID %d) from ready queue (priority: %d)\n",
                proc->name, proc->pid, priority);
#endif

        // Release reference that scheduler_add_process took
        process_unref(proc);
    } else {
        // Not found in either queue - already removed
        spin_unlock(&scheduler_lock);
    }

    PERF_END(PERF_OP_SCHEDULER_REMOVE);
}

process_t* scheduler_pick_next(void) {
    // RACE-001 fix: Acquire scheduler lock before queue access
    spin_lock(&scheduler_lock);

    process_t* next = NULL;

    // -------------------------------------------------------------------------
    // O(1) SCHEDULER CORE ALGORITHM
    // -------------------------------------------------------------------------
    // 1. Try to pick from active runqueue (O(1) via bitmap_ffs)
    // 2. If active is empty, swap active ↔ expired
    // 3. Pick from newly active runqueue
    // -------------------------------------------------------------------------

pick_again:
    // Try to pick highest priority process from active runqueue
    next = runqueue_pick_next(&this_cpu()->rq_active);

    if (next == NULL) {
        // Active runqueue is empty - check if expired has processes
        if (runqueue_is_empty(&this_cpu()->rq_expired)) {
            // Both runqueues are empty: nothing runnable. Return the idle thread,
            // which is NOT enqueued (see scheduler_init) — it is the dedicated
            // fallback that runs only in this all-empty state.
            spin_unlock(&scheduler_lock);
            return this_cpu()->idle_thread;
        }

        // Swap active and expired runqueues
        runqueue_swap();

        // Try again from newly active runqueue
        next = runqueue_pick_next(&this_cpu()->rq_active);

        if (next == NULL) {
            // Expired was non-empty but yielded nothing pickable (all entries
            // drained as terminated): fall back to the idle thread.
            spin_unlock(&scheduler_lock);
            return this_cpu()->idle_thread;
        }
    }

    // -------------------------------------------------------------------------
    // KILL-FIX-001: Drain any TERMINATED processes
    // -------------------------------------------------------------------------
    // A TERMINATED process can appear in the queue when sys_kill sets the
    // state AFTER the process was enqueued but BEFORE scheduler_remove_process
    // had a chance to run. We discard it here and pick again.
    // -------------------------------------------------------------------------
    if (next->state == PROCESS_TERMINATED) {
        this_cpu()->ready_count--;
        next->on_queue = 0;
        spin_unlock(&scheduler_lock);
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] pick_next: discarding terminated '%s' (PID %d), "
                "picking again\n", next->name, next->pid);
#endif
        // Release the reference that scheduler_add_process took
        process_unref(next);
        spin_lock(&scheduler_lock);
        goto pick_again;  // Try to pick another process
    }

    this_cpu()->ready_count--;
    next->on_queue = 0;

    spin_unlock(&scheduler_lock);

    // ============================================================================
    // TIME SLICE FAIRNESS FIX (Bug fix from Agent 29 investigation)
    // ============================================================================
    // Only reset time slice when it's COMPLETELY exhausted (time_slice == 0).
    // This prevents "free refill" bug where preempted processes get extra CPU time.
    //
    // Fairness guarantee:
    //   - Process that runs for FULL quantum (10 ticks) → Gets fresh 10 ticks next time
    //   - Process that yields EARLY (e.g., 5 ticks used) → Resumes with 5 ticks left
    //
    // Without this check, a process preempted at any point would get a free refill,
    // violating round-robin fairness and allowing unfair CPU monopolization.
    //
    // Example without fix (WRONG):
    //   Process A: uses 10 ticks → preempted → gets 10 ticks (FAIR)
    //   Process B: uses 3 ticks, yields → gets 10 ticks (UNFAIR! Should only get 7)
    //
    // Example with fix (CORRECT):
    //   Process A: uses 10 ticks → preempted → gets 10 ticks (FAIR)
    //   Process B: uses 3 ticks, yields → resumes with 7 ticks left (FAIR)
    // ============================================================================
    if (next->time_slice == 0) {
        // Process exhausted its quantum - give a fresh, PRIORITY-SCALED allocation
        // (lower nice => longer slice => more CPU share where slices matter).
        next->time_slice = priority_time_slice(next->priority);
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Fresh quantum for '%s' (PID %d): %llu ticks (priority: %d)\n",
                next->name, next->pid, (unsigned long long)next->time_slice, process_get_priority(next));
#endif
    } else {
        // Process yielded early - keep remaining allocation
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Resuming '%s' (PID %d) with %d ticks remaining (priority: %d)\n",
                next->name, next->pid, next->time_slice, process_get_priority(next));
#endif
    }

    // Reference is transferred to caller - they own it now
    // (scheduler_add_process took a reference, we're just transferring ownership)
    return next;
}

// Expose this CPU's idle thread. The idle thread is NOT a queued process; it is
// the fallback scheduler_pick_next() returns when both runqueues are empty.
// Cooperative blocking paths (wait_object_block) must recognise it so they idle
// in place rather than context_switch into idle's synthetic kernel context.
process_t* scheduler_idle_thread(void) {
    return this_cpu()->idle_thread;
}

// ===========================================================================
// cooperative_switch_to() — hand the CPU to `next` from a cooperative site
// ===========================================================================
// A cooperative resume site (sys_yield / schedule / wq_block_current) runs in
// ring 0 inside a syscall and switches to `next`. The mechanism DEPENDS on how
// `next` was suspended:
//
//  • RESUME_CRETURN — `next` was suspended cooperatively (SYS_YIELD / a blocking
//    syscall), or is brand-new (context.rip == usermode trampoline). Its
//    context.rip is a KERNEL continuation, so the normal context_switch()
//    (which ends in `ret`) resumes it correctly.
//
//  • RESUME_IRETQ — `next` was preempted by the timer IRQ while in ring 3, so
//    context_save_irq stored its *ring-3* RIP/RSP/regs into its context. A `ret`
//    into that would execute user code at CPL=0 (runaway → #DF). It must be
//    resumed by restoring its ring-3 state and IRETQ-ing — context_switch_to_iretq.
//
// This is the piece the preemptive first-dispatch fix made necessary: once
// never-run processes start under the timer IRQ, many become RESUME_IRETQ and a
// cooperative caller can legitimately need to run one (e.g. a UI app that yields
// while only timer-preempted burners are ready). Routing both kinds here keeps
// every cooperative site correct without each having to special-case resume_mode.
//
// In a non-preemptive build no process is ever RESUME_IRETQ, so this is always
// the plain context_switch() path — identical behaviour to before.
void cooperative_switch_to(process_t* from, process_t* next) {
    // TSS.RSP0 + SYSCALL kernel stack must point at `next`'s kernel stack before
    // it runs in ring 3 (so its next syscall/interrupt lands on the right stack).
    // 16-byte align: the CPU pushes the iretq frame here and aligned SSE stores
    // in the entry path fault on a misaligned stack.
    if (next->kernel_stack) {
        uint64_t kstack_top =
            ((uint64_t)next->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
        tss_set_kernel_stack(kstack_top);
        extern uint64_t kernel_rsp_save;
        kernel_rsp_save = kstack_top;
    }

#ifdef PREEMPTIVE
    if (next->resume_mode == RESUME_IRETQ) {
        // Resume a timer-preempted (ring-3) process via iretq. Does not return;
        // `from` is saved inside so the scheduler can resume it later.
        context_switch_to_iretq(from, next);
        return;  // unreachable (iretq transferred control to ring 3)
    }
#endif
    // RESUME_CRETURN (or any non-preemptive build): kernel-continuation resume.
    context_switch(from, next);
}

void schedule(void) {
    // Called from timer interrupt (scheduler tick)
#ifdef PERF_CONTEXT_SWITCH
    uint64_t schedule_start = rdtsc();
#endif

    process_t* current = process_get_current();

    // If no current process, pick one (may be idle thread if queues are empty)
    if (current == NULL) {
        process_t* next = scheduler_pick_next();
        // scheduler_pick_next() always returns a valid process now (real process
        // or idle thread), so no NULL check needed.
        next->state = PROCESS_RUNNING;
        process_set_current(next);
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Started process '%s' (PID %d) with time slice %d\n",
                next->name, next->pid, next->time_slice);
#endif
        return;
    }

    // CRITICAL: Check if current process has terminated (via sys_exit or SIGKILL)
    // Must switch away immediately, even if it has remaining time slice.
    //
    // KILL-FIX-002: Handle the terminated-current path exactly once.
    //
    // The dead process must NEVER be re-added to the ready queue (scheduler_add_process
    // already guards on PROCESS_TERMINATED), so after we switch away it can never
    // become current again.  We release the scheduler's "current" reference here so
    // that the process can be freed by whoever holds the last reference (typically
    // sys_waitpid / process_destroy in the parent, or process_unref at ref_count==1).
    //
    // We stash the dead pointer in a local, clear current_process FIRST, then
    // release our reference.  This ordering ensures that if an interrupt fires
    // between the unref and the context_switch call it will not see a dangling
    // current_process pointer.
    if (current->state == PROCESS_TERMINATED) {
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Current process '%s' (PID %d) terminated, "
                "switching away (exactly once)\n",
                current->name, current->pid);
#endif
        process_t* next = scheduler_pick_next();
        // scheduler_pick_next() now always returns a valid process (either a
        // real runnable process or the idle thread), so we no longer need to
        // check for NULL. The idle thread case is a valid switch target.
        next->state = PROCESS_RUNNING;
        process_set_current(next);

        // Stash the dead process pointer before the context switch so we
        // can release its reference after we return in the new process's
        // context (or, in the cooperative case, so it never gets back on
        // the queue).
        process_t* dead = current;

        // Cooperative (C-return) save path.  The context of the dead
        // process is saved here so the assembly stub can return — but
        // because dead->state == PROCESS_TERMINATED and scheduler_add_process
        // rejects TERMINATED processes, this saved context is never used.
        // cooperative_switch_to routes RESUME_IRETQ successors through iretq
        // (a dead process can hand off to a timer-preempted one safely).
        dead->resume_mode = RESUME_CRETURN;

        // -------------------------------------------------------------------
        // KILL-FIX-002 (CORRECTED): release the dead process's scheduler
        // ("current"/running) reference HERE, BEFORE the switch.
        //
        // The original code placed this process_unref(dead) AFTER
        // cooperative_switch_to(dead,next), on the theory it would run "when
        // this kernel-stack frame is resumed." But a TERMINATED process is
        // NEVER re-queued (scheduler_add_process rejects it), so dead's stack
        // frame is never resumed -- the post-switch unref NEVER executed and
        // the scheduler ref leaked, pinning ref_count at 1 forever. That hid
        // under the (separate) #9 creation-ref leak; once the reaper releases
        // the creation ref, this orphaned scheduler ref is exactly what stops
        // the PCB / 8KB stack / CR3 / PID from EVER being freed -> PIDs never
        // recycle and the 256-PID pool exhausts. (Verified live: a reaped
        // zombie stuck at ref_count==1 with no unref ever dropping it.)
        //
        // Dropping it here does NOT free the stack we are still executing on:
        // the process is a zombie awaiting reap, so its CREATION ref keeps
        // ref_count >= 1 (reaped==0 until a reaper claims it, and on the
        // cooperative uniprocessor no reaper can run while this process is
        // `current`). This unref therefore takes 2 -> 1 (never to 0); the
        // reaper later drops the creation ref to reach 0 and frees the PCB OFF
        // this stack. (SMP note: when AP scheduling lands, revisit to a
        // schedule_tail-style drop in the successor's context.)
        // -------------------------------------------------------------------
        process_unref(dead);
        cooperative_switch_to(dead, next);
        return;
    }

    // Decrement time slice for running process
    // This is called on EVERY timer tick (e.g., 100 Hz = every 10ms)
    // When time_slice reaches 0, the process is preempted
    if (current->time_slice > 0) {
        current->time_slice--;
    }

    // Check if time slice expired (quantum exhausted)
    // NOTE: Timer-driven preemption disabled until kernel stack frames are
    // properly managed for context switches inside interrupt handlers.
    // For now, cooperative multitasking via SYS_YIELD only.
    if (current->time_slice == 0) {
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Time slice expired for process '%s' (PID %d) - preempting\n",
                current->name, current->pid);
#endif

        scheduler_add_process(current);

        process_t* next = scheduler_pick_next();

        // scheduler_pick_next() now always returns a valid process (either a
        // real runnable process or the idle thread). If next == current, it
        // means pick_next returned the only process in the queue (which was
        // current itself, just re-added above). In that case, just refill its
        // quantum and continue - no context switch needed.
        if (next == current) {
            // Only process in the system - continue with a fresh quantum
            current->time_slice = priority_time_slice(current->priority);
            current->state = PROCESS_RUNNING;
            // Release the EXTRA reference scheduler_add_process() took above and
            // scheduler_pick_next() then "transferred" back to us: we are NOT
            // switching away (current keeps running), so the transferred ref is
            // surplus. Without this, ref_count climbs by one for every expired
            // quantum a sole process runs (~100x/s under PREEMPT) -> the PCB,
            // 8KB kernel stack and address space never reach ref 0 / get freed,
            // and the PID is never reclaimed. Mirrors the process_unref(next) on
            // schedule_from_irq's no-switch path. Safe re BUG-006: NO context
            // switch happens here, so `current` and locals are still valid.
            process_unref(current);
#ifndef SCHEDULER_QUIET
            kprintf("[SCHEDULER] Only process in system, continuing '%s' (PID %d) with fresh time slice\n",
                    current->name, current->pid);
#endif
        } else {
            // Context switch to next process (may be idle thread)
            next->state = PROCESS_RUNNING;
#ifndef SCHEDULER_QUIET
            kprintf("[SCHEDULER] Context switching: '%s' (PID %d) -> '%s' (PID %d), time slice: %d\n",
                    current->name, current->pid, next->name, next->pid, next->time_slice);
#endif

            process_set_current(next);
            // Cooperative (C-return) save path. cooperative_switch_to routes to
            // context_switch (RESUME_CRETURN) or iretq resume (RESUME_IRETQ) and
            // sets next's TSS/kernel stack.
            current->resume_mode = RESUME_CRETURN;
            cooperative_switch_to(current, next);

            // BUG-006 fix: Do NOT put ANY code here that references 'current' or local variables!
            //
            // When context_switch() "returns", we're actually resuming THIS process after it
            // was previously switched out. At that point, local variables like 'current',
            // 'next', and 'old' contain STALE values from the previous context switch.
            //
            // For example:
            //   1. Process A switches to Process B (current=A, next=B)
            //   2. Later, Process B switches to Process A
            //   3. Process A resumes HERE with stale values (current=A, next=B from step 1)
            //   4. Calling process_unref(current) would incorrectly unref Process A!
            //
            // Solution: Never access local variables after context_switch().
            // The process reference counting is handled correctly because:
            //   - scheduler_add_process() takes a reference when adding to ready queue
            //   - scheduler_pick_next() transfers that reference to the caller
            //   - When process terminates, scheduler_remove_process() releases it
        }
    }

#ifdef PERF_CONTEXT_SWITCH
    uint64_t schedule_end = rdtsc();
    uint64_t schedule_cycles = schedule_end - schedule_start;
    // Only log slow context switches (> 1000 cycles)
    if (schedule_cycles > 1000) {
        kprintf("[PERF] Context switch: %llu cycles (%.2f us)\n",
                schedule_cycles, cycles_to_us(schedule_cycles));
    }
#endif
}

#ifdef PREEMPTIVE
// ===========================================================================
// schedule_from_irq() — IRQ-driven preemptive context switch
// ===========================================================================
// Called from irq0_preempt (interrupt.asm) with interrupts disabled and a
// pointer to the on-stack interrupt_frame_t. This is the preemptive twin of
// schedule(): instead of saving the kernel C-return point (fatal from an IRQ),
// it saves/restores the *interrupted* register frame so a switch can occur
// from the timer IRQ and resume via the stub's trailing iretq.
//
// Switch eligibility: we only perform an in-place iretq swap when the incoming
// process is RESUME_IRETQ (i.e. it was itself last suspended by a timer IRQ in
// ring 3, so its saved context is iretq-resumable to user mode). A process
// that was suspended cooperatively (RESUME_CRETURN, via SYS_YIELD / blocking)
// is NOT iretq-resumable here; it is left on the ready queue and resumed by the
// cooperative path (context_switch) the next time the running process yields or
// syscalls. This keeps the two resume paths cleanly separated and never tries
// to iretq into a kernel C-return continuation. In the steady state of a
// CPU-bound ring-3 desktop, every process is RESUME_IRETQ after its first
// preemption, so round-robin preemption proceeds normally.
void schedule_from_irq(interrupt_frame_t* frame) {
    // 1) Acknowledge the timer IRQ at the PIC and advance the tick counter.
    //    (We bypass idt.c's irq_handler, so we own the EOI here.)
    pit_tick();
    outb(PIC1_COMMAND, PIC_EOI);

    // Real-sleep wakeups: once per tick, re-ready any process whose sleep
    // deadline has arrived. Mirrors the cooperative pit.c timer_handler so
    // blocking sleep works identically in BOTH builds. Done before the quantum
    // logic so a just-woken sleeper can be picked as the successor below.
    extern uint64_t timer_get_ticks(void);
    sleep_list_wake_due(timer_get_ticks());

    process_t* current = process_get_current();
    if (current == NULL) {
        return;  // Nothing running yet; leave frame untouched.
    }

    // Per-process CPU accounting (mirror of the cooperative pit.c handler): charge
    // this timer tick to the currently RUNNING process. Counter only -- it does NOT
    // alter total_time, the quantum, or any switch decision below. Done here once
    // per tick (before the ring-3 guard / quantum logic) so cpu_ticks reflects real
    // wall-clock CPU time consistently with the cooperative build.
    current->cpu_ticks++;

    // HARD RING-3 GUARD — non-preemptible kernel (CRITICAL safety).
    // Only ever switch tasks that the timer interrupted while running in
    // userspace (CPL==3). If the interrupted code segment's RPL is not 3, the
    // KERNEL itself was running (a syscall, IRQ handler, or any kernel critical
    // section), so we must NOT preempt: doing so could switch away mid-update of
    // a kernel data structure that holds a lock or a half-built frame, deadlock
    // the system, or resume into a kernel continuation via the iretq path (which
    // only knows how to resume ring-3 RESUME_IRETQ contexts). Leaving the frame
    // untouched makes iretq simply resume the interrupted kernel code. This is
    // the single most important invariant of this scheduler: the timer preempts
    // user code only, never the kernel.
    if ((frame->cs & 3) != 3) {
        return;  // kernel was running: leave frame untouched, resume it.
    }

    // Guard: only preempt a genuinely RUNNING process. If current is BLOCKED
    // (e.g. the timer fired while wq_block_current() was idling in sti/hlt
    // before it switched away) or otherwise not running, do nothing — switching
    // away here would wrongly re-ready a blocked process. The cooperative path
    // (wq_block_current/schedule) owns transitions out of RUNNING in those
    // cases. We still leave the frame untouched so iretq resumes the idle loop.
    if (current->state != PROCESS_RUNNING &&
        current->state != PROCESS_TERMINATED) {
        return;
    }

    // 2) A terminated current must switch away immediately.
    if (current->state != PROCESS_TERMINATED) {
        // Decrement quantum; not expired yet -> keep running (frame untouched).
        if (current->time_slice > 0) {
            current->time_slice--;
        }
        if (current->time_slice > 0) {
            return;
        }
        // Quantum hit 0: flag a reschedule.
        current->need_resched = 1;
    }

    // 3) Pick a successor. scheduler_pick_next() always returns a valid process
    //    (real process or idle thread). If next == current, it means current was
    //    the only process in the queue (re-added earlier); refill and continue.
    process_t* next = scheduler_pick_next();
    process_t* idle = scheduler_idle_thread();
    // If the only runnable task is current itself OR the idle fallback (no OTHER
    // ring-3 process is ready), just keep current running with a fresh quantum.
    // Critically, do NOT let next==idle fall through to the RESUME_CRETURN reject
    // branch below: that branch would scheduler_add_process(idle) — idle must NEVER
    // be enqueued (it is the empty-runqueue fallback, not a queued task; enqueuing
    // it breaks the active/expired invariant and starves the expired queue) — and
    // process_unref(idle), which underflows idle's refcount (pick_next transfers no
    // ref for the idle fallback) toward a premature free. That was the ROOT CAUSE
    // of the preempt hang: a CPU-bound burner that is the only runnable process has
    // its quantum expire ~100x/sec, each time enqueuing+unref'ing idle. (A
    // TERMINATED current with next==idle is the genuine going-idle case and still
    // falls through to the conservative panic below.)
    if (next == current ||
        (next == idle && current->state != PROCESS_TERMINATED)) {
        if (current->state != PROCESS_TERMINATED) {
            current->time_slice = priority_time_slice(current->priority);
            current->need_resched = 0;
            current->state = PROCESS_RUNNING;
        }
        return;  // frame untouched
    }

    // 4) The incoming process must be iretq-resumable (interrupted in ring 3).
    //    If not, we cannot safely resume it from here: put it back and continue
    //    the current process; the cooperative path will resume it later.
    //
    //    Ref accounting note: this mirrors the cooperative schedule() exactly.
    //    pick_next() transferred the queue's reference to us. We hand it right
    //    back to the queue via scheduler_add_process()... but add_process takes
    //    its OWN ref, so we must release the one pick_next gave us to avoid a
    //    leak on this reject-and-requeue path (the cooperative path never
    //    rejects, so it has no equivalent release — this branch is unique to
    //    the IRQ path).
    //
    //    FIRST-DISPATCH EXCEPTION (fairness fix): a brand-new userspace process
    //    that has never run is NOT yet RESUME_IRETQ — exec.c set its
    //    context.rip to process_enter_usermode_trampoline and its real ring-3
    //    entry/stack into user_entry/user_rsp, expecting the cooperative path to
    //    `ret` into the trampoline. But a non-yielding CPU hog never enters the
    //    cooperative path, so without this case new processes STARVE (only the
    //    first-launched burner ever runs). Detect the never-run state by its
    //    one reliable signature — context.rip == trampoline — and give it its
    //    first ring-3 entry directly from the IRQ by SYNTHESIZING a fresh iretq
    //    frame (we cannot context_load_irq it: next->context holds the trampoline
    //    rip + a kernel rsp, not the userspace entry). After this the process is
    //    RESUME_IRETQ like any preempted ring-3 task. Only never-run userspace
    //    procs match; a yielded/blocked process has rip != trampoline and stays
    //    on the reject/defer path below, untouched.
    // Detect a never-run task by its trampoline RIP. TWO trampolines qualify:
    //   * process_enter_usermode_trampoline — a brand-new process/fork child.
    //   * thread_enter_usermode_trampoline   — a brand-new THREAD (SYS_THREAD_CREATE),
    //     which additionally needs RDI = thread_arg at first ring-3 entry (SysV
    //     entry(arg)). Both expect the cooperative path to `ret` into the
    //     trampoline; a non-yielding CPU hog never enters it, so the same first-
    //     dispatch synth that rescues starved new processes must also rescue
    //     starved new threads. We branch on which trampoline it is to decide RDI.
    extern void process_enter_usermode_trampoline(void);
    extern void thread_enter_usermode_trampoline(void);
    int is_thread_trampoline =
        (next->context.rip == (uint64_t)thread_enter_usermode_trampoline);
    if (next->resume_mode != RESUME_IRETQ &&
        (next->context.rip == (uint64_t)process_enter_usermode_trampoline ||
         is_thread_trampoline)) {
        // Causal-proof trace (gated: runs only in the PREEMPTIVE build, since
        // schedule_from_irq is the IRQ path). One line per never-run process the
        // first time the timer bootstraps it into ring 3 -- so the stress log
        // shows a first-dispatch for each previously-starved burner, then its
        // heartbeats. Naturally once-per-process; rate-limit/remove once stable.
        kprintf("[SCHED] first-dispatch pid=%d via irq iretq%s\n", next->pid,
                is_thread_trampoline ? " (thread)" : "");

        // Save the OUTGOING current exactly as the RESUME_IRETQ path does:
        // it was interrupted in ring 3, so an iretq frame save is correct.
        // (Skip if it terminated — nothing to preserve.)
        if (current->state != PROCESS_TERMINATED) {
            context_save_irq(&current->context, frame);
            current->resume_mode = RESUME_IRETQ;
            current->need_resched = 0;
            current->total_time++;
            scheduler_add_process(current);
        }

        // SYNTHESIZE a fresh ring-3 entry frame directly into `frame`. Zero all
        // GP registers (clean process start — no inherited register state), then
        // set the hardware iretq fields for the real userspace entry. CS=0x23 /
        // SS=0x1B are the ring-3 (RPL=3) user code/data selectors; RFLAGS=0x202
        // sets IF=1 so the new process runs with interrupts enabled (and the
        // reserved bit 1). This mirrors what the trampoline -> enter_usermode ->
        // iretq path would have built, but does it from the IRQ frame.
        frame->rax = 0; frame->rbx = 0; frame->rcx = 0; frame->rdx = 0;
        frame->rsi = 0; frame->rdi = 0; frame->rbp = 0;
        frame->r8  = 0; frame->r9  = 0; frame->r10 = 0; frame->r11 = 0;
        frame->r12 = 0; frame->r13 = 0; frame->r14 = 0; frame->r15 = 0;
        frame->rip    = next->user_entry;  // real ELF entry, not the trampoline
        frame->cs     = 0x23;              // ring-3 user code selector (RPL=3)
        frame->rflags = 0x202;             // IF=1, reserved bit 1
        frame->rsp    = next->user_rsp;    // user stack
        frame->ss     = 0x1B;              // ring-3 user data selector (RPL=3)
        // A thread enters entry(arg) per SysV: deliver thread_arg in RDI. (For a
        // process first-dispatch this stays 0, matching the cooperative path.)
        if (is_thread_trampoline) {
            frame->rdi = next->thread_arg;
        }

        // Make the incoming process current; it is now ring-3 RESUME_IRETQ.
        next->resume_mode = RESUME_IRETQ;
        next->state = PROCESS_RUNNING;
        process_set_current(next);
        next->total_time++;

        // TSS.RSP0 + SYSCALL kernel stack (mirror the RESUME_IRETQ path).
        if (next->kernel_stack) {
            uint64_t kstack_top =
                ((uint64_t)next->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
            tss_set_kernel_stack(kstack_top);
            extern uint64_t kernel_rsp_save;
            kernel_rsp_save = kstack_top;
        }

        // Switch address space last. PCID bit-63 preserve mirrors the
        // RESUME_IRETQ path / context_switch_asm. The synthesized frame and
        // next->context live in identity-mapped kernel memory shared across all
        // address spaces, so they stay valid after CR3.
        uint64_t cr3 = next->context.cr3;
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        if (cr4 & (1ULL << 17)) {
            cr3 |= (1ULL << 63);  // CR4.PCIDE set: don't flush TLB
        }
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");

        // Ref accounting: pick_next() transferred the queue's ref for `next` to
        // us; it is now held by `next` being current_process (identical to the
        // RESUME_IRETQ path). Do NOT unref here.

        // Return to irq0_preempt, whose iretq enters ring 3 at user_entry.
        return;
    }

    if (next->resume_mode != RESUME_IRETQ) {
        // The incoming process is RESUME_CRETURN (kernel thread or yielded user
        // process). We cannot resume it from the IRQ path (iretq can't jump to
        // kernel code). Re-queue it and continue current... but if current is
        // TERMINATED, we have a problem: we can't continue a dead process, and we
        // can't switch to the kernel thread from here. This should only happen if
        // the idle thread was picked (all user processes terminated), which means
        // the system is shutting down. Panic rather than resuming a dead process.
        if (current->state == PROCESS_TERMINATED) {
            kernel_panic("schedule_from_irq: Cannot switch to kernel thread from "
                         "IRQ when current is terminated (system shutting down?)");
        }
        scheduler_add_process(next);   // re-queue (takes a fresh ref)
        process_unref(next);           // release pick_next's transferred ref
        current->time_slice = priority_time_slice(current->priority);
        current->need_resched = 0;
        current->state = PROCESS_RUNNING;
        return;  // frame untouched
    }

    // 5) Save the OUTGOING process (unless it terminated — nothing to preserve).
    //    It was just interrupted in ring 3, so an iretq frame save is correct.
    if (current->state != PROCESS_TERMINATED) {
        context_save_irq(&current->context, frame);
        current->resume_mode = RESUME_IRETQ;
        current->need_resched = 0;
        current->total_time++;
        // Re-queue it (preserve remaining quantum semantics: a preempted
        // process that exhausted its quantum gets a fresh one in pick_next).
        scheduler_add_process(current);
    }

    // 6) Make the incoming process current and load it into the on-stack frame
    //    so the stub's iretq resumes it. Update TSS.RSP0 + SYSCALL stack.
    next->state = PROCESS_RUNNING;
    process_set_current(next);

    if (next->kernel_stack) {
        uint64_t kstack_top =
            ((uint64_t)next->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
        tss_set_kernel_stack(kstack_top);
        extern uint64_t kernel_rsp_save;
        kernel_rsp_save = kstack_top;
    }

    // Rewrite the frame (GP regs, RIP/RSP/RFLAGS, CS/SS) BEFORE switching CR3,
    // while context_load_irq's reads of next->context are on the current map.
    // next->context and the on-stack frame are both in identity-mapped kernel
    // memory shared across all address spaces, so they stay valid after CR3.
    context_load_irq(&next->context, frame);

    // Switch address space last. PCID bit-63 preserve mirrors context_switch_asm.
    uint64_t cr3 = next->context.cr3;
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 17)) {
        cr3 |= (1ULL << 63);  // CR4.PCIDE set: don't flush TLB
    }
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");

    // Ref accounting: pick_next() transferred the queue's ref for `next` to us;
    // that ref is now held by `next` being current_process. This is identical
    // to the cooperative schedule() path (which also does not unref next after
    // making it current). Do NOT unref here.

    // Return to irq0_preempt, which pops the rewritten GP regs and iretq's into
    // the new process.
}
#endif // PREEMPTIVE

void scheduler_start(void) {
    kprintf("[SCHEDULER] ========================================\n");
    kprintf("[SCHEDULER] Starting scheduler...\n");
    kprintf("[SCHEDULER] ========================================\n");

    // Pick the first process to run (may be idle thread if no user processes)
    kprintf("[SCHEDULER] Picking first process from ready queue...\n");
    process_t* first = scheduler_pick_next();

    // scheduler_pick_next() now always returns a valid process (either a real
    // runnable process or the idle thread), so we no longer panic on empty queue.
    // The idle thread is a valid boot target - it will halt until an interrupt
    // (e.g., a device driver) readies a real process.

    kprintf("[SCHEDULER] First process selected: '%s' (PID %d)\n", first->name, first->pid);
    kprintf("[SCHEDULER]   Entry point: 0x%016lx\n", first->context.rip);
    kprintf("[SCHEDULER]   Stack pointer: 0x%016lx\n", first->context.rsp);
    kprintf("[SCHEDULER]   RFLAGS: 0x%lx\n", first->context.rflags);
    kprintf("[SCHEDULER]   CR3: 0x%016lx\n", first->context.cr3);
    kprintf("[SCHEDULER]   Time slice: %d ticks\n", first->time_slice);
    kprintf("[SCHEDULER]   State: %d -> RUNNING\n", first->state);

    // Set as current process
    first->state = PROCESS_RUNNING;
    process_set_current(first);
    kprintf("[SCHEDULER] Current process set to PID %d\n", first->pid);

    // CRITICAL: Set TSS.RSP0 to this process's kernel stack
    // When userspace is interrupted, CPU loads kernel stack from TSS.RSP0
    if (!first->kernel_stack) {
        kprintf("[TSS] Init process (PID %d) has NULL kernel_stack!\n", first->pid);
        kernel_panic("[TSS] Init process has NULL kernel_stack");
    }

    // kmalloc only guarantees 8-byte alignment, so round the stack top down to
    // a 16-byte boundary (x86-64 ABI requirement for the interrupt entry frame).
    uint64_t kstack_top = ((uint64_t)first->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;

    tss_set_kernel_stack(kstack_top);

    // Also set kernel RSP for SYSCALL entry (SYSCALL doesn't auto-switch RSP)
    extern uint64_t kernel_rsp_save;
    kernel_rsp_save = kstack_top;

    kprintf("[SCHEDULER] TSS.RSP0 set to 0x%016lx (kernel stack top)\n", kstack_top);
    kprintf("[SCHEDULER]   Kernel stack base: 0x%016lx\n", (uint64_t)first->kernel_stack);

    // Check if the first process is the idle thread (kernel thread) or a user
    // process. The idle thread has user_entry == 0 (it's kernel-only), so we
    // jump directly to its kernel function instead of entering ring 3.
    if (first->user_entry == 0) {
        // Idle thread or other kernel thread: jump directly to its kernel function.
        // context.rip points at idle_thread_func (or whatever kernel function).
        kprintf("[SCHEDULER] ========================================\n");
        kprintf("[SCHEDULER] Starting kernel thread '%s'...\n", first->name);
        kprintf("[SCHEDULER]   RIP=0x%016lx RSP=0x%016lx CR3=0x%016lx\n",
                first->context.rip, first->context.rsp, first->context.cr3);
        kprintf("[SCHEDULER] ========================================\n");

        // Switch to the idle thread's kernel stack and jump to its entry.
        // This is a one-way jump (no return) - the idle thread runs forever.
        __asm__ volatile(
            "mov %0, %%rsp\n"       // Load idle thread's kernel stack
            "mov %1, %%cr3\n"       // Load idle thread's page table (kernel CR3)
            "push $0\n"             // Dummy return address (never used)
            "jmp *%2\n"             // Jump to idle_thread_func
            :: "r"(first->context.rsp), "r"(first->context.cr3), "r"(first->context.rip)
            : "memory"
        );
    } else {
        // User process: transition to ring 3 as normal.
        // CRITICAL: Enable interrupts AFTER TSS.RSP0 is set
        // This prevents race where timer IRQ fires before kernel stack is configured
        // NOTE: Do NOT sti() here - IRETQ in enter_usermode sets IF=1 in RFLAGS
        // This avoids timer interrupt firing before we transition to ring 3
        kprintf("[SCHEDULER] ========================================\n");
        kprintf("[SCHEDULER] Transitioning to ring 3 (user mode)...\n");
        kprintf("[SCHEDULER]   RIP=0x%016lx RSP=0x%016lx CR3=0x%016lx\n",
                first->user_entry, first->user_rsp, first->context.cr3);
        kprintf("[SCHEDULER] ========================================\n");
        enter_usermode(first->user_entry, first->user_rsp, first->context.cr3);
    }

    // Should never reach here
    kernel_panic("scheduler_start: Returned from initial dispatch");
}

// ===========================================================================
// Per-CPU Statistics Accessors and Reset Functions
// ===========================================================================
// These functions provide access to and manipulation of per-CPU scheduler
// statistics. At N=1 (single CPU), they operate on cpus[0]. When SMP is
// enabled (brick 5+), they will work across all online CPUs.

// Get the ready_count for a specific CPU
// Returns the number of processes in the ready queue for the given CPU.
uint32_t scheduler_get_ready_count(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return 0;
    }
    return cpus[cpu_id].ready_count;
}

// Get the ready_count for the current CPU
// Convenience wrapper that operates on this_cpu().
uint32_t scheduler_get_current_ready_count(void) {
    return this_cpu()->ready_count;
}

// Reset the ready_count for a specific CPU
// Used during CPU bring-up or runqueue reinitialization.
void scheduler_reset_ready_count(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return;
    }
    spin_lock(&scheduler_lock);
    cpus[cpu_id].ready_count = 0;
    spin_unlock(&scheduler_lock);
}

// Get per-CPU statistics for a specific CPU
// Copies the cpu_stats_t structure for the given CPU into the provided buffer.
// Returns 0 on success, -1 if cpu_id is invalid.
int scheduler_get_cpu_stats(uint32_t cpu_id, cpu_stats_t* out_stats) {
    if (cpu_id >= MAX_CPUS || !out_stats) {
        return -1;
    }
    // No lock needed for reading a single uint64_t (atomic on x86-64)
    out_stats->reserved = cpus[cpu_id].stats.reserved;
    return 0;
}

// Reset per-CPU statistics for a specific CPU
// Used during CPU bring-up or statistics collection resets.
void scheduler_reset_cpu_stats(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return;
    }
    // No lock needed for writing a single uint64_t (atomic on x86-64)
    cpus[cpu_id].stats.reserved = 0;
}

// Reset per-CPU statistics for all CPUs
// Iterates through all online CPUs and resets their statistics.
void scheduler_reset_all_cpu_stats(void) {
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].online) {
            scheduler_reset_cpu_stats(i);
        }
    }
}

// Validate ready_count against actual runqueue contents
// Debugging function that counts processes in both runqueues and compares
// against ready_count. Returns 0 if counts match, -1 otherwise.
// WARNING: Must be called with scheduler_lock held or with interrupts disabled.
int scheduler_validate_ready_count(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return -1;
    }

    uint32_t actual_count = 0;
    cpu_t* cpu = &cpus[cpu_id];

    // Count processes in active runqueue
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* proc = cpu->rq_active.queues[prio];
        while (proc) {
            actual_count++;
            proc = proc->next;
        }
    }

    // Count processes in expired runqueue
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* proc = cpu->rq_expired.queues[prio];
        while (proc) {
            actual_count++;
            proc = proc->next;
        }
    }

    if (actual_count != cpu->ready_count) {
        kprintf("[SCHEDULER] ERROR: CPU %d ready_count mismatch: expected %d, actual %d\n",
                cpu_id, cpu->ready_count, actual_count);
        return -1;
    }

    return 0;
}

#endif // CONFIG_SMP
