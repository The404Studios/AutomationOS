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
 * Locking Protocol (RACE-001 fix; F3-1 per-CPU split):
 *  - cpus[c].rq_lock protects cpu c's runqueues, bitmaps, ready_count and the
 *    on_queue/queued_cpu membership of its tasks. Held during all per-cpu queue
 *    manipulations (add/remove/pick/yield/reset). At N=1 only cpus[0].rq_lock exists.
 *  - scheduler_lock is RETAINED as the global OUTER lock for scheduler_shutdown
 *    (drain serializer) only.
 *  - Lock ordering: scheduler_lock (outer) -> per-cpu rq_locks (inner, STRICT
 *    ASCENDING cpu-id order). No site holds two rq_locks; the cross-cpu enqueue
 *    (scheduler_add_process_to_cpu) takes exactly one foreign rq_lock, never nests.
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
// Wave-7 perf: cache-line aligned (64 bytes) to prevent false sharing between
// CPUs on SMP. Also uses pointer indirection for rq_active/rq_expired so the
// active/expired swap is a 2-pointer exchange (16 bytes) not a 2-struct copy
// (~4.5 KB).
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
    process_t*  pending_unref;  // F3-5: a dead task's RUNNING ref, handed off by
                                // the AP dying path and dropped by the SUCCESSOR
                                // (schedule_tail-style). The ref outliving the
                                // switch is what keeps the dead task's kernel
                                // stack + CR3 alive until this CPU is off them --
                                // CPU0's reaper can run CONCURRENTLY on SMP, so
                                // the BSP's pre-switch drop (KILL-FIX-002, whose
                                // safety argument is uniprocessor-only) is NOT
                                // safe here.
    // The O(1) MLFQ runqueue state, relocated AS-IS from the old file-scope
    // globals (active_rq / expired_rq / ready_count). ONE runqueue (cpu[0]'s) is
    // used exactly as before -- genuinely separate per-CPU runqueues with their
    // own picks are brick 6, NOT now.
    // Wave-7 perf: rq_active/rq_expired are now POINTERS into the two backing
    // rq_storage[] arrays. runqueue_swap() exchanges the two pointers (O(1), 16
    // bytes) instead of copying the full structs (~4.5 KB).
    runqueue_t* rq_active;      // points at rq_storage[0 or 1]
    runqueue_t* rq_expired;     // points at rq_storage[1 or 0]
    runqueue_t  rq_storage[2];  // backing storage for the two runqueues
    uint32_t    ready_count;    // was the global ready_count
    cpu_stats_t stats;          // per-CPU scheduler stats (unused at N=1)
    // F3-1: per-CPU runqueue mutation lock. Guards rq_active/rq_expired/ready_count
    // and the on_queue/queued_cpu membership of THIS cpu's tasks ONLY. Replaces the
    // global scheduler_lock at every per-cpu runqueue site (add/remove/pick/yield/
    // reset). scheduler_lock is RETAINED as the OUTER lock for scheduler_shutdown
    // (global drain serializer) and as the home for any future global-only state.
    // At N=1 cpu_id()==0 so this_cpu()->rq_lock is ALWAYS cpus[0].rq_lock -- a pure
    // rename of which spinlock instance is taken (same granularity, same critical
    // sections, same enqueue targets) => byte-for-byte the old single-lock behavior.
    spinlock_t  rq_lock;
} __attribute__((aligned(64))) cpu_t;

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

// F3-4: per-CPU "current" resolver -- the inverse of cpu_set_current_thread(). THE LAW:
// "current" is CPU-LOCAL. process_get_current() (process.c) routes through THIS so a
// syscall/exit on the AP resolves cpus[cpu_id()].current_thread, NOT the global
// current_process (which still names CPU0's task). Defined HERE (not process.c) because
// cpus[]/this_cpu() are file-local to the scheduler, and smp.h's this_cpu() is a
// DIFFERENT struct (percpu_data_t) whose current_thread is NOT this lockstep slot.
// BYTE-IDENTICAL on the BSP: cpu_id()==0 always in default/SMP_FOUNDATION builds, so
// this returns cpus[0].current_thread, which process_set_current() holds in lockstep
// with the global current_process at the single dispatch chokepoint. The per-cpu
// divergence appears ONLY under SMP_SCHED_DISPATCH when CPU1 runs a task.
process_t* cpu_get_current_thread(void) {
    return this_cpu()->current_thread;
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

// Check if runqueue is empty — O(1) direct bitmap OR test (no bitmap_ffs loop)
static inline int runqueue_is_empty(const runqueue_t* rq) {
    uint64_t any = 0;
    for (int i = 0; i < SCHED_BITMAP_WORDS; i++)
        any |= rq->bitmap[i];
    return any == 0;
}

// Swap active and expired runqueues — O(1) pointer exchange (wave-7 perf)
static void runqueue_swap(void) {
    cpu_t* c = this_cpu();
    runqueue_t* tmp = c->rq_active;
    c->rq_active  = c->rq_expired;
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

// F3-1: at-rest per-CPU runqueue topology self-test (defined with the F3-0
// validators below; emits a single greppable "RQLOCK: PASS"/"FAIL" line). Forward
// declared because scheduler_init() calls it BEFORE the validators block. A no-op
// stub in a non-SCHED_DEBUG (perf) build.
static void scheduler_rqlock_selftest(void);
static void scheduler_affinity_selftest(void);   // F3-2 (defined with the validators; perf-build stub)

void scheduler_init(void) {
#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] Initializing O(1) multi-level feedback queue scheduler...\n");
#endif

    // RACE-001 fix: Initialize scheduler lock (retained as the OUTER/global lock for
    // scheduler_shutdown; per-CPU runqueue mutation now uses cpus[].rq_lock, F3-1).
    spin_lock_init(&scheduler_lock);

    // F3-1: initialize the BSP's per-CPU runqueue lock. At N=1 this is the ONLY
    // rq_lock ever taken (cpu_id()==0 => this_cpu()->rq_lock == cpus[0].rq_lock).
    spin_lock_init(&cpus[0].rq_lock);

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

    // Initialize active and expired runqueues (pointer indirection into rq_storage[]).
    this_cpu()->rq_active  = &this_cpu()->rq_storage[0];
    this_cpu()->rq_expired = &this_cpu()->rq_storage[1];
    runqueue_init(this_cpu()->rq_active);
    runqueue_init(this_cpu()->rq_expired);
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

#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] O(1) scheduler initialized:\n");
    kprintf("[SCHEDULER]   - Priority levels: %d (0-139)\n", SCHED_PRIORITY_LEVELS);
    kprintf("[SCHEDULER]   - Time slice: %d ticks\n", DEFAULT_TIME_SLICE);
    kprintf("[SCHEDULER]   - Algorithm: Active/Expired double-buffering\n");
    kprintf("[SCHEDULER]   - Complexity: O(1) enqueue, O(1) dequeue, O(1) pick_next\n");
    kprintf("[SCHEDULER]   - SMP-safe: Yes\n");
    kprintf("[SCHEDULER]   - Idle thread: PID %d\n", this_cpu()->idle_thread->pid);
#endif

    // F3-1: prove the per-CPU runqueue lock topology is sound AT REST (rq_lock
    // init'd, ready_count==walk, no cross-cpu duplicate, secondary cpus empty)
    // before init spawns any service. Emits "RQLOCK: PASS" on every boot.
    scheduler_rqlock_selftest();

    // F3-2: prove the CPU-affinity model is sound at rest (predicate correct + ctor
    // defaults fired so no task carries a zero mask). Emits "AFFINITY: PASS".
    scheduler_affinity_selftest();
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

    cpus[cpu].rq_active  = &cpus[cpu].rq_storage[0];
    cpus[cpu].rq_expired = &cpus[cpu].rq_storage[1];
    runqueue_init(cpus[cpu].rq_active);
    runqueue_init(cpus[cpu].rq_expired);
    cpus[cpu].ready_count = 0;
    // F3-1 (F-2 hazard): init this AP's rq_lock BEFORE any code acquires it. The BSP
    // takes cpus[cpu].rq_lock cross-CPU in scheduler_add_process_to_cpu (called by
    // ap_spawn_test_kthread), so an uninitialized lock word here would spin forever.
    spin_lock_init(&cpus[cpu].rq_lock);

    process_t* idle = create_idle_thread(cpu, 0 /* do NOT adopt PID 0 */);
    if (!idle) {
        cpus[cpu].online = 0;
        return 0;
    }
    cpus[cpu].current_thread = idle;   // start CPU as "running idle" until it dispatches
    cpus[cpu].idle_thread    = idle;
    // F3-2: the secondary cpu's idle conceptually belongs to THAT cpu. Idle is NEVER
    // enqueued (empty-runqueue fallback only), so this does not affect the affinity
    // gate today; it keeps the at-rest affinity model coherent for F3-3. Overrides the
    // CPU0-only default process_create installed.
    idle->allowed_cpus = (uint64_t)1 << cpu;
    idle->pinned_cpu   = cpu;

    /* NOTE: CPU1's idle thread runs the AP scheduler loop on the AP BOOT stack (see
     * ap_main). Its pre-seeded context (idle_thread_func / idle->kernel_stack from
     * create_idle_thread) is the SAVE TARGET only: the first cooperative switch
     * (idle->thread) overwrites idle->context with the live boot-stack loop state via
     * fxsave64/context_switch_asm, and later switches back restore exactly that. So
     * idle is self-consistent without an explicit rip/cr3 override here. */

#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] CPU%u secondary slot online: idle PID %d stack=%p "
            "(CPU0 idle PID %d stack=%p) rq_active=%p\n",
            cpu, idle->pid, (void*)idle->kernel_stack,
            cpus[0].idle_thread->pid, (void*)cpus[0].idle_thread->kernel_stack,
            (void*)&cpus[cpu].rq_storage[0]);
#endif
    return 1;
}

// Enqueue `proc` onto a SPECIFIC CPU's EXPIRED runqueue (cross-CPU pinning) — the
// affinity primitive for Brick F. Mirrors scheduler_add_process()'s exact ref /
// lock / idempotency discipline, but targets cpus[cpu].rq instead of this_cpu()'s
// (scheduler_add_process always uses this_cpu(), so the BSP can't use it to pin to
// CPU1). F3-1: it takes the TARGET cpu's own rq_lock (cpus[cpu].rq_lock, a FOREIGN
// lock when called from the BSP for CPU1) -- exactly one lock, never nested, so it
// can't ABBA with shutdown. Takes a ref that pick_next() later transfers.
void scheduler_add_process_to_cpu(process_t* proc, uint32_t cpu) {
    if (!proc || cpu >= MAX_CPUS) return;
    if (proc->state == PROCESS_TERMINATED) return;
    // Idle is the empty-runqueue FALLBACK and must NEVER be enqueued (enqueuing it
    // breaks the active/expired invariant and leaks a ref). Guard explicitly.
    if (proc == cpus[cpu].idle_thread) return;

    // F3-2 ENQUEUE LAW: refuse to enqueue a task on a CPU outside its affinity mask.
    // This is the ONLY cross-cpu enqueue primitive, so it is the sole gatekeeper for
    // CPU1 enqueues -- and it runs BEFORE the lock/ref/link, so a refusal takes no ref
    // and links nothing: it CANNOT freeze a live task (the task was never on this
    // queue). The this_cpu() paths (scheduler_add_process/yield_requeue) are
    // deliberately NOT gated -- they target the running cpu and gating a requeue there
    // would freeze a live task. Raw bit test (smp.h/cpumask_test is not included here);
    // cpu < MAX_CPUS is already guaranteed above, the cpu>=64 guard is future-defense.
    if (cpu >= 64 || ((proc->allowed_cpus >> cpu) & 1ULL) == 0) {
        kprintf("[SCHEDULER] AFFINITY: refuse enqueue pid=%d on cpu=%u (allowed_cpus=0x%016llx)\n",
                proc->pid, cpu, (unsigned long long)proc->allowed_cpus);
        return;
    }

    // Single critical section: (check on_queue, take the queue's ref, set the flag
    // + queued_cpu, enqueue) are done atomically under the TARGET cpu's rq_lock so a
    // concurrent scheduler_remove_process can't orphan the ref (process_ref is a
    // lock-free atomic add, safe to call here). Tighter than the legacy
    // scheduler_add_process (which refs outside the lock) -- the AP path must be
    // race-clean. F3-1: this is a cross-CPU enqueue (BSP -> cpus[cpu]); it acquires
    // the FOREIGN target cpu's rq_lock (NOT this_cpu()'s). Exactly ONE lock is held
    // and it never nests, so it cannot form an ABBA cycle with scheduler_shutdown.
#ifdef SMP_IPI
    int enqueued = 0;     /* SMP_IPI-gated so non-IPI SMP builds stay byte-identical */
#endif
    spin_lock(&cpus[cpu].rq_lock);
    if (!proc->on_queue) {
        process_ref(proc);                   // queue holds a ref (UAF guard)
        process_set_ready(proc);
        proc->on_queue = 1;
        proc->queued_cpu = cpu;              // membership lands on the TARGET cpu
        int priority = process_get_priority(proc);
        runqueue_enqueue(cpus[cpu].rq_expired, proc, priority);
        cpus[cpu].ready_count++;
#ifdef SMP_IPI
        enqueued = 1;
#endif
    }
    spin_unlock(&cpus[cpu].rq_lock);

#ifdef SMP_IPI
    // SMP-G1: kick the target CPU so a hlt-parked idle loop dispatches NOW
    // instead of on its next 10 ms LAPIC tick. Sent AFTER the rq_lock drops
    // (never signal while holding a lock) and only for a REAL foreign enqueue
    // (idempotent re-adds don't spam the ICR). A busy target just consumes the
    // flag and re-loops -- a spurious kick is harmless by design.
    // ipi_reschedule itself no-ops for self/offline/not-armed targets, so
    // every other path through this function is unchanged.
    if (enqueued && cpu != cpu_id()) {
        extern void ipi_reschedule(uint32_t target_cpu);
        ipi_reschedule(cpu);
    }
#endif
}
#endif /* SMP_SCHED */

#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
// ===========================================================================
// SMP-F3-6: scheduler_choose_cpu() -- THE placement seam.
// ===========================================================================
// docs/SCHEDULER_POLICY_LAYER.md made real: a task asks "which CPU should I
// run on?" HERE, and nowhere else. Layers, strict order:
//   1. HARD LEGALITY (never skipped): declared allowed_cpus ∩ online reality.
//      An empty intersection or an off-mask pin is an ILLEGAL request --
//      CLAMPED loudly to a legal CPU, never returned illegal, never wedged.
//   2. ROLE/PIN (law 2, pinning beats balancing): a legal pin wins; a task
//      whose legal mask is CPU1-only goes to CPU1 even unpinned (the doc's
//      cpu1-only role branch). Generic field reads -- NEVER name-based.
//   3. PRESSURE/BALANCING -- DELIBERATE STUB: home CPU0, no balancing, no
//      migration. F3-7 inserts one branch here; no caller changes. Default
//      tasks (CPU0 mask, no pin) land here == exactly today's behavior.
//   4. The untouched MLFQ on the chosen CPU picks the TASK (wrap, never
//      replace).
// ADVISORY by design: the mandatory F3-2 enqueue gate in
// scheduler_add_process_to_cpu remains the backstop, so even a buggy future
// policy cannot enqueue off-mask (legality is structural, not polite).
uint32_t scheduler_choose_cpu(process_t* p) {
    if (!p) return 0;

    /* ---- layer 1: hard legality ---------------------------------------- */
    /* CPU0 is always online; CPU1 joins the legal set only while
     * cpu1_is_online() (law 7: a dead/quarantined CPU drops out -- liveness
     * is today's only quarantine input). */
    extern int cpu1_is_online(void);
    uint64_t online = 1ULL | (cpu1_is_online() ? (1ULL << 1) : 0ULL);
    uint64_t legal  = p->allowed_cpus & online;
    if (legal == 0) {
        /* The F3-2 memset trap (mask=0) or an all-offline mask. */
        kprintf("[SCHEDULER] CHOOSECPU: pid=%d '%s' mask=0x%llx has no legal "
                "online cpu -- CLAMPED to CPU0\n",
                p->pid, p->name, (unsigned long long)p->allowed_cpus);
        return 0;
    }

    /* ---- layer 2: pin/role (pin wins when legal) ------------------------ */
    /* SMP-PROFILE-0: the seam now READS the typed class. Observation only:
     * NORMAL and BATCH route identically (home CPU0 below) until F3-7 lights
     * layer 3; PINNED_RT's placement input was always the pin itself. The one
     * real check the read buys today: a task DECLARED PINNED_RT without an
     * actual pin is a declaration bug -- say so loudly (no behavior change;
     * it still routes like its mask says). */
    if (p->sched.sched_class == SCHED_CLASS_PINNED_RT && p->pinned_cpu == CPU_NONE) {
        kprintf("[SCHEDULER] CHOOSECPU: pid=%d '%s' declared PINNED_RT but has "
                "no pin -- declaration bug (routing by mask)\n", p->pid, p->name);
    }
    if (p->pinned_cpu != CPU_NONE) {
        if (p->pinned_cpu < 64 && ((legal >> p->pinned_cpu) & 1ULL)) {
            return p->pinned_cpu;
        }
        kprintf("[SCHEDULER] CHOOSECPU: pid=%d '%s' pinned_cpu=%u not in legal "
                "mask 0x%llx -- pin CLAMPED to first legal cpu\n",
                p->pid, p->name, p->pinned_cpu, (unsigned long long)legal);
        /* fall through: the role/home steps below return a legal cpu */
    }
    if (((legal >> 1) & 1ULL) && !((legal >> 0) & 1ULL)) {
        return 1;                  /* CPU1-only mask -> CPU1 (role branch) */
    }

    /* ---- layer 3: pressure/balancing ------------------------------------- */
#ifdef SMP_BATCH
    /* SMP-F3-7: THE one batch branch (the policy doc's "F3-7 inserts one
     * branch here; no caller changes" promise, now due). A BATCH-class task
     * whose LEGAL mask includes CPU1 routes to the PINNED_WORKER core --
     * law 5: batch fills idle capacity without touching GENERAL latency.
     * PLACEMENT only, never migration: this decides where a task is
     * enqueued; nothing already queued or running moves. No load measurement
     * yet (pressure counters are F3-9) -- BATCH simply prefers the worker
     * core whenever that is legal. NORMAL falls through to home; the
     * legality walls (layer 1 above + the funnel re-assert + the F3-2
     * enqueue gate) bound this branch on every side. */
    if (p->sched.sched_class == SCHED_CLASS_BATCH && ((legal >> 1) & 1ULL)) {
        return 1;
    }
#endif
    return 0;                      /* home CPU0; NO balancing, NO migration */
}

// SMP-PROFILE-0: the CPU-role table (layer 2's other half). Static intent,
// not discovered state: CPU0 is the general-purpose core (desktop, syscalls),
// CPU1 takes explicitly placed work only. F3-7's balancer consults this so
// BATCH spillover can target PINNED_WORKER cores without touching GENERAL.
cpu_role_t scheduler_cpu_role(uint32_t cpu) {
    return (cpu == 1) ? CPU_ROLE_PINNED_WORKER : CPU_ROLE_GENERAL;
}

// SMP-PROFILE-0: THE named placement funnel (the policy doc's pipeline,
// previously run "by hand" at each call site):
//   1. target = scheduler_choose_cpu(p)        // layers 1-3 (legality inside)
//   2. re-assert legality                      // policy cannot escape layer 1
//   3. scheduler_add_process_to_cpu(p, target) // the proven, gated enqueue sink
// The re-assert can never fire while choose_cpu honors its own legality
// contract -- it exists so a FUTURE buggy layer-3 balancer is caught here,
// loudly, before the (also mandatory) enqueue gate refuses it. One-shot
// narration for the first few submissions; the wake path calls this at
// frequency and must stay serial-quiet.
uint32_t scheduler_submit_task(process_t* p) {
    if (!p) return 0;

    uint32_t target = scheduler_choose_cpu(p);

    extern int cpu1_is_online(void);
    uint64_t online = 1ULL | (cpu1_is_online() ? (1ULL << 1) : 0ULL);
    uint64_t legal  = p->allowed_cpus & online;
    if (legal != 0 && target < 64 && ((legal >> target) & 1ULL) == 0) {
        kprintf("[SCHEDULER] SUBMIT: policy escaped legality (pid=%d target=%u "
                "legal=0x%llx) -- CLAMPED to CPU0\n",
                p->pid, target, (unsigned long long)legal);
        target = 0;
    }

    {
        static volatile int submit_logged = 0;
        if (submit_logged < 4) {
            submit_logged++;
            kprintf("[SCHED] submit: pid=%d '%s' class=%d -> cpu%u\n",
                    p->pid, p->name, (int)p->sched.sched_class, target);
        }
    }

    scheduler_add_process_to_cpu(p, target);
    return target;
}

// SMP-F3-6 selftest: synthetic shells exercise every seam branch (the
// affinity-selftest pattern -- the boot queues hold too few shapes to be a
// non-vacuous proof, so the predicate is tested directly). One static shell
// (process_t is too big for the stack); only the fields the seam reads are
// set per case.
void scheduler_choosecpu_selftest(void) {
    static process_t t;            /* zeroed .bss shell */
    t.pid = 9999;
    t.name[0] = 'c'; t.name[1] = 'c'; t.name[2] = 't'; t.name[3] = 0;

    /* 1. pinned CPU1 task chooses CPU1 */
    t.allowed_cpus = (1ULL << 1); t.pinned_cpu = 1;
    int pinned_cpu1 = (scheduler_choose_cpu(&t) == 1);

    /* 2. normal/default task chooses home CPU0 */
    t.allowed_cpus = (1ULL << 0); t.pinned_cpu = CPU_NONE;
    int default_cpu0 = (scheduler_choose_cpu(&t) == 0);

    /* 3. illegal pin (off-mask) is clamped to a legal cpu, loudly */
    t.allowed_cpus = (1ULL << 0); t.pinned_cpu = 1;
    int illegal_clamped = (scheduler_choose_cpu(&t) == 0);

    /* 4. the F3-2 zero-mask trap is clamped to CPU0, loudly */
    t.allowed_cpus = 0; t.pinned_cpu = CPU_NONE;
    int nomask_clamped = (scheduler_choose_cpu(&t) == 0);

    /* 5. multi-CPU mask, unpinned: HOME, no balancing/migration (layer-3
     * stub) -- the "no migration unless explicitly allowed" proof */
    t.allowed_cpus = (1ULL << 0) | (1ULL << 1); t.pinned_cpu = CPU_NONE;
    int multimask_home = (scheduler_choose_cpu(&t) == 0);

    /* 6. CPU1-only mask, unpinned: the role branch routes to CPU1 (when
     * CPU1 is online; on a single-core boot legality clamps it to CPU0 --
     * accept either as the legal answer for that reality) */
    extern int cpu1_is_online(void);
    t.allowed_cpus = (1ULL << 1); t.pinned_cpu = CPU_NONE;
    uint32_t r6 = scheduler_choose_cpu(&t);
    int cpu1only_role = cpu1_is_online() ? (r6 == 1) : (r6 == 0);

    int pass = pinned_cpu1 && default_cpu0 && illegal_clamped &&
               nomask_clamped && multimask_home && cpu1only_role;
    kprintf("CHOOSECPU: %s pinned_cpu1=%d default_cpu0=%d illegal_clamped=%d "
            "nomask_clamped=%d multimask_home=%d cpu1only_role=%d\n",
            pass ? "PASS" : "FAIL",
            pinned_cpu1, default_cpu0, illegal_clamped,
            nomask_clamped, multimask_home, cpu1only_role);
}

// SMP-PROFILE-0 selftest: the typed profile exists, the seam reads it, and
// NOTHING behaves differently because of it. Synthetic shells (the
// choosecpu-selftest pattern). The submit_funnel flag is NOT asserted here:
// a synthetic shell must never enter a real runqueue, so the funnel is
// proven by the LIVE placements (kthread + cpu1hello route through
// scheduler_submit_task; the smoke greps their '[SCHED] submit:' lines).
void scheduler_profile_selftest(void) {
    static process_t t;
    t.pid = 9998;
    t.name[0] = 'p'; t.name[1] = 'f'; t.name[2] = 't'; t.name[3] = 0;

    /* 1. NORMAL (the memset default, class 0) routes home CPU0 */
    t.sched.sched_class = SCHED_CLASS_NORMAL;
    t.allowed_cpus = (1ULL << 0); t.pinned_cpu = CPU_NONE;
    int normal_home = (scheduler_choose_cpu(&t) == 0);

    /* 2. BATCH declares + reads back. The routing expectation flips WITH
     * the F3-7 gate: under SMP_BATCH a multi-CPU-mask BATCH task routes to
     * the worker core (when online); without it, BATCH is data-only and
     * routes home. The printed CORE line stays identical either way so
     * frozen smokes keep grepping true. */
    t.sched.sched_class = SCHED_CLASS_BATCH;
    t.allowed_cpus = (1ULL << 0) | (1ULL << 1); t.pinned_cpu = CPU_NONE;
    int batch_declared;
#ifdef SMP_BATCH
    {
        extern int cpu1_is_online(void);
        uint32_t rb = scheduler_choose_cpu(&t);
        batch_declared = (t.sched.sched_class == SCHED_CLASS_BATCH) &&
                         (cpu1_is_online() ? (rb == 1) : (rb == 0));
    }
#else
    batch_declared = (t.sched.sched_class == SCHED_CLASS_BATCH) &&
                     (scheduler_choose_cpu(&t) == 0);
#endif

    /* 3. PINNED_RT with a legal pin is honored (the class names what the
     * pin already enforced) */
    t.sched.sched_class = SCHED_CLASS_PINNED_RT;
    t.allowed_cpus = (1ULL << 1); t.pinned_cpu = 1;
    extern int cpu1_is_online(void);
    uint32_t r3 = scheduler_choose_cpu(&t);
    int pinned_rt_legal = cpu1_is_online() ? (r3 == 1) : (r3 == 0);

    /* 4. no behavior change: every class answers exactly what the classless
     * F3-6 seam answered for the same mask/pin shape (NORMAL multimask ->
     * home; BATCH single-CPU0 -> home; PINNED_RT off-mask pin -> clamped) */
    t.sched.sched_class = SCHED_CLASS_NORMAL;
    t.allowed_cpus = (1ULL << 0) | (1ULL << 1); t.pinned_cpu = CPU_NONE;
    int nb1 = (scheduler_choose_cpu(&t) == 0);
    t.sched.sched_class = SCHED_CLASS_BATCH;
    t.allowed_cpus = (1ULL << 0); t.pinned_cpu = CPU_NONE;
    int nb2 = (scheduler_choose_cpu(&t) == 0);
    t.sched.sched_class = SCHED_CLASS_PINNED_RT;
    t.allowed_cpus = (1ULL << 0); t.pinned_cpu = 1;     /* off-mask pin */
    int nb3 = (scheduler_choose_cpu(&t) == 0);          /* clamped, loudly */
    int no_behavior_change = nb1 && nb2 && nb3;

    int pass = normal_home && batch_declared && pinned_rt_legal && no_behavior_change;
    kprintf("SMPPROFILE-CORE: %s normal_home=%d batch_declared=%d "
            "pinned_rt_legal=%d no_behavior_change=%d\n",
            pass ? "PASS" : "FAIL",
            normal_home, batch_declared, pinned_rt_legal, no_behavior_change);
}

#ifdef SMP_BATCH
// SMP-F3-7 selftest: the batch branch routes EXACTLY as specified and the
// legality walls still bound it. Synthetic shells (the house pattern).
void scheduler_batchclass_selftest(void) {
    extern int cpu1_is_online(void);
    static process_t t;
    t.pid = 9997;
    t.name[0] = 'b'; t.name[1] = 'c'; t.name[2] = 't'; t.name[3] = 0;
    int up = cpu1_is_online();

    /* 1. BATCH + legal multi-CPU mask -> CPU1 (the new branch; on a
     * single-core boot legality keeps it home -- accept that reality) */
    t.sched.sched_class = SCHED_CLASS_BATCH;
    t.allowed_cpus = (1ULL << 0) | (1ULL << 1); t.pinned_cpu = CPU_NONE;
    uint32_t r1 = scheduler_choose_cpu(&t);
    int batch_cpu1 = up ? (r1 == 1) : (r1 == 0);

    /* 2. BATCH may NOT escape its mask: CPU0-only BATCH stays home (the
     * layer-1 wall bounds the branch) */
    t.allowed_cpus = (1ULL << 0); t.pinned_cpu = CPU_NONE;
    int batch_mask_respected = (scheduler_choose_cpu(&t) == 0);

    /* 3. NORMAL multimask remains home CPU0 (no accidental balancing) */
    t.sched.sched_class = SCHED_CLASS_NORMAL;
    t.allowed_cpus = (1ULL << 0) | (1ULL << 1); t.pinned_cpu = CPU_NONE;
    int normal_cpu0 = (scheduler_choose_cpu(&t) == 0);

    /* 4. PINNED_RT still obeys its explicit pin (layer 2 outranks 3) */
    t.sched.sched_class = SCHED_CLASS_PINNED_RT;
    t.allowed_cpus = (1ULL << 1); t.pinned_cpu = 1;
    uint32_t r4 = scheduler_choose_cpu(&t);
    int pinned_rt_cpu1 = up ? (r4 == 1) : (r4 == 0);

    /* 5. illegal stays clamped: BATCH with a zero mask -> CPU0, loudly */
    t.sched.sched_class = SCHED_CLASS_BATCH;
    t.allowed_cpus = 0; t.pinned_cpu = CPU_NONE;
    int illegal_clamped = (scheduler_choose_cpu(&t) == 0);

    int pass = batch_cpu1 && batch_mask_respected && normal_cpu0 &&
               pinned_rt_cpu1 && illegal_clamped;
    kprintf("BATCHCLASS-CORE: %s batch_cpu1=%d batch_mask_respected=%d "
            "normal_cpu0=%d pinned_rt_cpu1=%d illegal_clamped=%d\n",
            pass ? "PASS" : "FAIL",
            batch_cpu1, batch_mask_respected, normal_cpu0,
            pinned_rt_cpu1, illegal_clamped);
}
#endif /* SMP_BATCH */
#endif /* SMP_SCHED && SMP_SCHED_DISPATCH */

// F3-5: HOME-ROUTED wake enqueue. Wake-side code (waitqueue signal/timer) used
// the this_cpu()-based scheduler_add_process(), which is correct only when the
// WAKER and the WOKEN share a CPU. On the AP that breaks catastrophically: a
// CPU1 sys_exit waking its CPU0 parent (init, blocked in waitpid) would enqueue
// the parent on cpus[1] -- CPU1 would then run init while CPU0 still owns it.
// F3-6: the woken task's home is now answered by THE seam (was: an inline
// pin-or-CPU0 ternary -- identical result for every task shape that exists
// today, now through the audited legality/role path). On non-DISPATCH builds
// cpu_id()==0 and tasks are CPU0-affine, so this compiles to the old behavior.
void scheduler_add_process_home(process_t* proc) {
    if (!proc) return;
#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
    /* PROFILE-0: through the NAMED funnel (choose + legality re-assert +
     * the gated sink), not the bare choose+add pair. */
    scheduler_submit_task(proc);
#else
    scheduler_add_process(proc);
#endif
}

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
// proof). Doing the guard before ANY rq_lock acquisition ALSO prevents a same-CPU
// self-deadlock: CPU1 must never try to take this_cpu()->rq_lock (cpus[1].rq_lock)
// from this ISR while the ring-0 code it interrupted already holds it. F3-1 makes
// this MORE important, not less: the per-cpu lock is now the cpu's OWN lock, so a
// pre-guard acquire would self-wedge that cpu. Keep the ring-3 guard OUTERMOST.
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
#ifdef SMP_IPI
// ===========================================================================
// SMP-G1 IPI-WAKE instrumentation (proof plumbing, SMP_IPI builds only)
// ===========================================================================
// Ping pair: the BSP's ipiwake_ping_selftest writes req (its send TSC) and the
// AP idle loop's cli'd check writes ack (its consume TSC) -- proving each IPI
// woke the hlt-parked loop, with latency = ack - req. Enqueue pair: kernel.c
// stamps enq_tsc/enq_pid at the REAL cpu1hello enqueue; the first CPU1 switch
// into that pid stamps dispatch_tsc -- the end-to-end enqueue->first-dispatch
// latency through the live scheduler path. Single writer per field per phase;
// plain volatiles (x86-TSO + the consumers poll). DEFINED in ipi.c (low packed
// .bss -- law 15: read from CPU1 contexts that can hold a user CR3; the smoke's
// nm gate caught the original scheduler.c placement at 0x23c000 > 0x200000).
extern volatile uint64_t g_g1_ping_req;       // BSP: rdtsc at IPI send (0 = idle)
extern volatile uint64_t g_g1_ping_ack;       // CPU1: rdtsc at flag consume
extern volatile uint64_t g_g1_enq_tsc;        // BSP: rdtsc at cpu1hello enqueue
extern volatile int      g_g1_enq_pid;        // BSP: the enqueued pid to match
extern volatile uint64_t g_g1_dispatch_tsc;   // CPU1: rdtsc at first dispatch of that pid
extern uint32_t ipi_consume_need_resched(void);   // ipi.c (call with IF=0)
#endif

void ap_cooperative_schedule(void) {
    cpu_t* cpu = this_cpu();                  // CPU1 (cpu_id()==1)

    // F3-5: schedule_tail-style deferred drop. We are executing on a LIVE
    // thread's stack (idle or a requeued thread), so the previously-dead
    // thread's kernel stack + CR3 are no longer in use on THIS cpu -- its
    // running ref can finally go. Dropping it any earlier (the BSP's
    // KILL-FIX-002 pre-switch drop) is uniprocessor-only reasoning: on SMP,
    // CPU0's reaper can run concurrently and would free the PCB/stack/CR3
    // while CPU1 still executes on them.
    if (cpu->pending_unref) {
        process_t* dead = cpu->pending_unref;
        cpu->pending_unref = NULL;
        process_unref(dead);
        kprintf("[SMP] F3-5: dead task's running ref dropped by successor "
                "(cpu1_idle=%d)\n",
                (cpu->current_thread == cpu->idle_thread) ? 1 : 0);
    }

    process_t* current = cpu->current_thread; // idle (initially)
    // F3-5: a TERMINATED current (sys_exit on CPU1, or the F2 kthread
    // retiring) must NEVER be resumed -- the old early-return below would
    // walk straight back into the dead task's context.
    int dying = (current != cpu->idle_thread &&
                 current->state == PROCESS_TERMINATED);

    process_t* next = scheduler_pick_next();  // this_cpu()-based -> cpus[1].rq
    if (!dying && (next == cpu->idle_thread || next == current)) {
        return;  // nothing else runnable (pick_next gives idle on an empty rq; it
                 // transfers NO ref for the idle fallback, so nothing to release).
    }
    if (dying && next == current) {
        next = cpu->idle_thread;              // paranoia: never re-pick the dead
    }

    // Requeue the outgoing thread ONLY if it is a runnable non-idle process that
    // voluntarily yielded (state still RUNNING). The idle fallback is never queued
    // and a TERMINATED current is skipped here by its state.
    if (current != cpu->idle_thread && current->state == PROCESS_RUNNING) {
        scheduler_add_process(current);       // this_cpu()==cpus[1] -> cpus[1].rq
        // LEAK-FIX: drop the outgoing process's old "running" ref. The queue now
        // holds a fresh ref from scheduler_add_process, so the process will not be
        // freed. Mirrors the BSP cooperative schedule() LEAK-FIX.
        process_unref(current);
    }

    if (dying) {
        // F3-5 EXIT PATH (policy law 8). The dead task keeps its RUNNING ref --
        // handed to the successor via pending_unref (dropped at the top of this
        // function on the next pass, when this CPU is provably off the dead
        // stack/CR3). context_switch_asm restores the successor's CR3 from its
        // saved context, so CPU1 leaves the dying address space AT the switch;
        // teardown (reap on CPU0) can only free that CR3 after the ref drops.
        current->resume_mode = RESUME_CRETURN;   // saved ctx is never used again
        cpu->pending_unref   = current;

        // F3-5g: first-CPU1-kfree checkpoint -- prove heap_lock is safe from a
        // real CPU1 teardown context. One-shot.
        static int cpu1_kfree_probed = 0;
        if (!cpu1_kfree_probed) {
            cpu1_kfree_probed = 1;
            void* p = kmalloc(64);
            if (p) {
                kfree(p);
                kprintf("[SMP] F3-5: first CPU1 kmalloc/kfree OK\n");
            } else {
                kprintf("[SMP] F3-5: first CPU1 kmalloc FAILED\n");
            }
        }
    }

    cpu_set_current_thread(next);             // cpus[1].current_thread = next (per-CPU)
#ifdef SMP_RUNMASK
    // SMP-RUNMASK-0: record REALITY -- this is CPU1's only dispatch path, so
    // the stamp here + the one in process_set_current (the BSP chokepoint)
    // covers every dispatch. NOT in cpu_set_current_thread itself: an insert
    // there sits ABOVE the file's ASSERT_ALWAYS lines and the __LINE__ shift
    // breaks default-build byte-identity (the law-2 discipline).
    next->ran_on_cpus |= (1u << cpu_id());
#endif
    if (next != cpu->idle_thread) next->state = PROCESS_RUNNING;

    // Point CPU1's TSS.RSP0 + SYSCALL kernel stack at `next`'s kernel stack (no-op
    // for a pure ring-0 kernel thread, required once `next` is a ring-3 process).
    if (next->kernel_stack) {
        uint64_t kstack = ((uint64_t)next->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
        tss_set_kernel_stack(kstack);         // cpu_id()==1 -> tss_array[1]/kernel_rsp_save_arr[1]
    }

#ifdef SMP_IPI
    // SMP-G1: one-shot enqueue->first-dispatch latency for the instrumented
    // enqueue (kernel.c stamps enq_tsc/enq_pid at the real cpu1hello
    // scheduler_add_process_to_cpu). This line runs ON CPU1 at the moment it
    // commits to switching into that task -- the task's literal first
    // instruction follows within the context_switch_asm tail.
    if (g_g1_enq_tsc && !g_g1_dispatch_tsc && next->pid == g_g1_enq_pid) {
        g_g1_dispatch_tsc = rdtsc();
        kprintf("[SMP] G1: enqueue->dispatch latency=%lu us (pid=%d)\n",
                (unsigned long)((g_g1_dispatch_tsc - g_g1_enq_tsc) / 3000ULL),
                next->pid);
    }
#ifdef SMP_BATCH
    // SMP-F3-7: the same one-shot for the batchdemo placement -- sub-ms
    // proves the BATCH foreign enqueue's G1 IPI kick woke CPU1 (ipi_wake=1).
    {
        extern volatile uint64_t g_f37_enq_tsc;
        extern volatile int      g_f37_enq_pid;
        extern volatile uint64_t g_f37_dispatch_tsc;
        if (g_f37_enq_tsc && !g_f37_dispatch_tsc && next->pid == g_f37_enq_pid) {
            g_f37_dispatch_tsc = rdtsc();
            kprintf("[SMP] F3-7: batchdemo enqueue->dispatch latency=%lu us "
                    "(pid=%d)\n",
                    (unsigned long)((g_f37_dispatch_tsc - g_f37_enq_tsc) / 3000ULL),
                    next->pid);
        }
    }
#endif
#endif

    // Fix D6: prime `next`'s FPU state (context_switch_asm fxrstor's it directly,
    // bypassing context_switch()'s template priming) so a fresh process's MXCSR is
    // clean, not the all-zero #XM trap. Writes only next's own fpu_state (AP-safe).
    context_prime_fpu(next);

    // Cooperative ring-0 context switch. Saves `current`'s kernel resume point into
    // its context and `ret`s into `next`. Control returns here only when `current`
    // is later switched back in (e.g. `next` yields/blocks/exits).
    context_switch_asm(current, next);

    // F3-5 transition marker: this line executes in the RESUMED thread's
    // context. The first few resumes narrate the CPU1 lifecycle on serial
    // (idle -> kthread -> cpu1hello -> kthread -> idle) -- the evidence trail
    // for the exit-path audit. One-shot bounded; DISPATCH builds only.
    {
        static volatile int resume_logged = 0;
        if (resume_logged < 6) {
            resume_logged++;
            cpu_t* c = this_cpu();
            kprintf("[SMP] F3-5: cpu1 resume #%d current='%s'\n",
                    resume_logged,
                    (c->current_thread && c->current_thread->name[0])
                        ? c->current_thread->name : "?");
        }
    }
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

// F3-4 APCURRENT result, read+printed by the BSP (kernel.c) after the F2 verify:
//   0 = pending (kthread hasn't run the one-shot yet), 1 = PASS, 2 = FAIL.
volatile int ap_current_probe_result = 0;

static void ap_test_kthread_fn(void) {
    // F3-4 APCURRENT one-shot (runs ON CPU1): call the ACTUAL deliverable
    // process_get_current() and prove it resolves to THIS (CPU1-local) task, not CPU0's.
    // This is the meaningful boundary proof -- it exercises the per-cpu routing from a
    // real CPU1 context (NOT a BSP slot compare), and embodies the strong invariant
    // "cpu_id()!=0 => process_get_current() != cpus[0].current_thread". If the F3-4
    // one-liner were absent, process_get_current() would return the global (== CPU0's
    // current) and `me == c0` -> FAIL, catching the regression.
    {
        process_t* me = process_get_current();      // per-cpu after F3-4; here == cpus[1].current_thread
        process_t* c0 = cpus[0].current_thread;     // CPU0's current (file-local access)
        if (me && me != c0 && me->pinned_cpu == 1 && me->state == PROCESS_RUNNING)
            ap_current_probe_result = 1;            // PASS: AP current is CPU1-local + distinct from CPU0
        else
            ap_current_probe_result = 2;            // FAIL
    }
    // F3-5: the kthread now YIELDS periodically (so the pinned ring-3 cpu1hello
    // can share CPU1 cooperatively) and RETIRES after the F2 verify window is
    // long past -- through the SAME dying path a user exit takes, so CPU1
    // genuinely returns to idle (the acceptance's cpu1_idle=1). The retire
    // threshold (60M) is ~8x the BSP's F2 verify window, so the delta proof
    // is untouched.
    for (;;) {
        ap_kthread_counter++;
        __asm__ volatile("pause");
        if ((ap_kthread_counter & 0x3FF) == 0) {
            ap_cooperative_schedule();              // cooperative yield on CPU1
        }
        if (ap_kthread_counter >= 60000000ULL) break;
    }
    {
        // Retire exactly like sys_exit does (minus the ring-3 entry): mark
        // TERMINATED, wake/reparent via process_on_terminate (home-routed
        // wakes), drop off any queue, then take the dying path -- which hands
        // the running ref to the successor and switches away forever.
        process_t* me = process_get_current();
        me->exit_status = 0;
        me->state = PROCESS_TERMINATED;
        process_on_terminate(me);
        scheduler_remove_process(me);               // no-op for off-queue current
        kprintf("[SMP] F3-5: F2 kthread retiring through the AP dying path\n");
        ap_cooperative_schedule();                  // never returns
    }
    for (;;) { __asm__ volatile("hlt"); }           // unreachable
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
    // F3-2: this kthread runs ONLY on CPU1. Override the CPU0-only default that
    // process_create installed, so the affinity gate in scheduler_add_process_to_cpu
    // PERMITS the CPU1 enqueue (a CPU0-only mask would be REFUSED). Must precede the
    // enqueue. Raw bit op -- smp.h/cpumask_test is intentionally not included here.
    t->allowed_cpus   = (uint64_t)1 << 1;      // CPU1 only
    t->pinned_cpu     = 1;                      // pinned to CPU1
    // F3-5: the kthread now RETIRES (see ap_test_kthread_fn) -- give it init as
    // the reaping parent so its zombie is harvested like any other child, and
    // a priority BELOW user default (100) so the pinned ring-3 cpu1hello wins
    // the strict-priority pick while both are alive.
    t->priority = 120;
    {
        process_t* ini = process_get_by_pid(1);
        if (ini) {
            t->parent_pid = 1;
            t->parent_seq = ini->create_seq;
            process_unref(ini);
        }
    }
    // PROFILE-0: declare what this task IS (the class names what the pin
    // already enforced), then place through the NAMED funnel (F3-6's "one
    // decider", now with the legality re-assert).
    t->sched.sched_class = SCHED_CLASS_PINNED_RT;
    scheduler_submit_task(t);
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
        // F3-5 FIX: `sti; hlt` (the canonical idle idiom, same as the BSP's
        // idle thread), NOT a bare hlt. A bare hlt inherits IF from whatever
        // path resumed idle -- context_switch_asm restores idle's SAVED
        // rflags, and if any lock section had IF clear at idle's original
        // switch-out, the restored IF=0 turns this hlt into a PERMANENT park
        // (the LAPIC tick can never wake it). Observed as a flaky never-runs-
        // again idle after the F3-5 dying handoff: the pending running-ref
        // was never dropped and the dead task's PCB never freed. sti;hlt is
        // atomic wrt interrupts (an interrupt after sti is taken at the hlt
        // boundary and wakes it), so the next tick ALWAYS resumes the loop.
#ifdef SMP_IPI
        // SMP-G1 LOST-WAKEUP CLOSE. Without this, an IPI_RESCHEDULE landing
        // between ap_cooperative_schedule's empty pick and the hlt below is
        // ABSORBED (handler runs, sets the flag, iretq resumes... straight
        // into hlt) and the runnable task waits for the next 10 ms tick --
        // exactly the latency G1 exists to kill. The canonical close:
        //   cli                      -- any in-flight IPI is now held PENDING
        //   check rq / need_resched  -- the handler cannot interleave here
        //   sti; hlt                 -- a pending IPI is delivered at the hlt
        //                               boundary (law 12's sti shadow) and
        //                               WAKES it; nothing can slip the gap
        // ready_count is read without the rq_lock: a stale miss is covered by
        // the enqueuer's IPI (sent after its unlock), which is pending here
        // and wakes the hlt. A stale hit just re-loops once. Never blocks.
        __asm__ volatile("cli");
        uint32_t kicked = ipi_consume_need_resched();
        if (kicked && g_g1_ping_req && !g_g1_ping_ack) {
            g_g1_ping_ack = rdtsc();   // G1 ping ack (proof plumbing; one-shot
                                       // per ping -- the BSP re-arms req)
        }
        if (kicked || this_cpu()->ready_count > 0) {
            __asm__ volatile("sti");   // work exists: dispatch on the next pass
            continue;
        }
        __asm__ volatile("sti; hlt");
#else
        __asm__ volatile("sti; hlt");
#endif
    }
}

#ifdef SMP_IPI
// ===========================================================================
// SMP-G1 acceptance driver: ipiwake_ping_selftest() -- BSP context.
// ===========================================================================
// Runs in the window where CPU1's scheduler loop is up (ap_dbg_stage==3) but
// its runqueue is still EMPTY (before ap_spawn_test_kthread), so CPU1 is
// genuinely hlt-parked between 100 Hz ticks -- the exact state the lost-wakeup
// close protects. 32 pings: write req=rdtsc, IPI_RESCHEDULE to CPU1, bounded-
// poll (100 ms) for the loop's ack. EVERY ack proves an IPI woke the hlt (a
// tick-mediated wake cannot ack: ticks don't set need_resched); an ack under
// 1 ms is ~10x faster than the best-case tick rescue. All 32 answered =
// no_lost_wake. Serial-safe (pre-desktop BSP window, CPU1 prints nothing).
void ipiwake_ping_selftest(void) {
    extern volatile uint64_t ap_dbg_stage;
    extern void ipi_reschedule(uint32_t target_cpu);
    extern int  cpu1_is_online(void);

    if (!cpu1_is_online()) {
        kprintf("[SMP] G1: ping SKIP (cpu1 offline)\n");
        return;
    }
    // Bounded wait (~200 ms TSC) for CPU1 to enter the scheduler loop.
    uint64_t t0 = rdtsc();
    while (ap_dbg_stage != 3 && (rdtsc() - t0) < 200000ULL * 3000ULL) {
        __asm__ volatile("pause");
    }
    if (ap_dbg_stage != 3) {
        kprintf("[SMP] G1: ping SKIP (cpu1 scheduler loop not up)\n");
        return;
    }

    uint32_t acks = 0;
    uint64_t max_us = 0;
    for (int i = 0; i < 32; i++) {
        g_g1_ping_ack = 0;
        g_g1_ping_req = rdtsc();
        ipi_reschedule(1);
        // Bounded poll: 100 ms TSC deadline per ping (a lost wake would only
        // be rescued by the NEXT enqueue's IPI in real use; here it = FAIL).
        uint64_t s = rdtsc();
        while (!g_g1_ping_ack && (rdtsc() - s) < 100000ULL * 3000ULL) {
            __asm__ volatile("pause");
        }
        if (g_g1_ping_ack) {
            acks++;
            uint64_t us = (g_g1_ping_ack - g_g1_ping_req) / 3000ULL;
            if (us > max_us) max_us = us;
        }
        g_g1_ping_req = 0;
    }
    kprintf("[SMP] G1: ping summary acks=%u/32 max_latency_us=%lu\n",
            acks, (unsigned long)max_us);
}
#endif /* SMP_IPI */
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

    // 1) Acquire the GLOBAL scheduler lock (F3-1: retained as the OUTER lock) to
    //    serialize shutdown vs any concurrent add/remove globally and to guard the
    //    global g_sleep_list clear below. This blocks any timer IRQ that tries to
    //    re-queue a process mid-shutdown.
    spin_lock(&scheduler_lock);

    uint32_t drained_count = 0;

    // F3-1: drain THIS cpu's runqueues under its per-cpu rq_lock (the INNER lock).
    // Lock order is scheduler_lock (outer) -> per-cpu rq_lock (inner); the inner
    // per-cpu locks must be taken in STRICT ASCENDING cpu-id order when a future
    // shutdown drains all online CPUs. At F3-1 shutdown only drains this_cpu()
    // (cpu0), so it takes exactly cpus[0].rq_lock here.
    spin_lock(&this_cpu()->rq_lock);

    // 2) Drain the ACTIVE runqueue: dequeue every priority level, releasing
    //    the scheduler's reference for each process.
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* proc;
        while ((proc = runqueue_dequeue(this_cpu()->rq_active, prio)) != NULL) {
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
        while ((proc = runqueue_dequeue(this_cpu()->rq_expired, prio)) != NULL) {
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

    // Done mutating this cpu's runqueues -- release the inner per-cpu rq_lock. The
    // sleep-list clear and online=0 below are GLOBAL/non-runqueue state and stay
    // under scheduler_lock (outer) only.
    spin_unlock(&this_cpu()->rq_lock);

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


// ===========================================================================
// Brick F3-0: ADDITIVE scheduler/runqueue invariant guards (proof-first).
// Assertions/counters ONLY -- no behavior change. They LOG + COUNT, never panic,
// so a false positive cannot brick boot; each violation prints a greppable
// "[SCHED_INVARIANT] VIOLATION ..." line and bumps g_sched_invariant_violations.
//
// LOCK CONTRACT (F3-1 per-CPU rq_lock): every per-task call site already holds the
// RELEVANT lock -- the caller's this_cpu()->rq_lock (add_process/remove_process/
// pick_next all hold it) -- or runs IF=0 in the IRQ path. These functions therefore
// MUST NOT acquire any lock -- they only READ runqueue/list state + kprintf. All
// list walks are BOUNDED so a corrupt (cyclic) list cannot hang the validator --
// which is exactly the corruption class the SMP harness hunts.
//
// CROSS-CPU READ DEFERRAL (F3-1 -> F3-3): the validators read EVERY online cpu's
// runqueue (validate_runqueues' MAX_CPUS loop + sched_dbg_cross_cpu_duplicates).
// At F3-1 this is SAFE: only cpu0 ever mutates a runqueue (CPU1 is offload-only and
// never enqueues), so reading another cpu's queue is single-writer-safe. The MOMENT
// Brick F3-3 makes CPU1 mutate its own runqueue, these lock-free cross-cpu reads
// become RACY and MUST be gated under the target cpu's rq_lock (or run only on the
// owning cpu). Documented here; NOT yet enforced (no concurrent CPU1 writer exists).
//
// Gated behind -DSCHED_DEBUG (default ON during SMP bring-up; see quick_build.sh);
// compiles to no-ops in a perf build. Mirrors the existing
// scheduler_validate_ready_count() "must hold the relevant lock" contract.
// ===========================================================================
#ifdef SCHED_DEBUG
static volatile uint64_t g_sched_invariant_violations = 0;
// Wave-7 perf: sample validation every 64th call (not every call). The
// validators are O(n) in the runqueue depth and dominate the add/remove/pick
// hot path when SCHED_DEBUG is on. Sampling reduces that overhead by ~98%
// while still catching invariant regressions within ~64 ticks (~64 ms).
static volatile uint64_t g_sched_dbg_call_counter = 0;
#define SCHED_DBG_SAMPLE_INTERVAL 64
#define SCHED_DBG_SHOULD_SAMPLE() \
    ((__atomic_add_fetch(&g_sched_dbg_call_counter, 1, __ATOMIC_RELAXED) & (SCHED_DBG_SAMPLE_INTERVAL - 1)) == 0)

#define SCHED_VIOLATION(where, pid, fmt, ...)                                   \
    do {                                                                        \
        g_sched_invariant_violations++;                                         \
        kprintf("[SCHED_INVARIANT] VIOLATION at %s pid=%d : " fmt "\n",         \
                (where), (int)(pid), ##__VA_ARGS__);                            \
    } while (0)

// F3-2: raw CPU-affinity bit test. smp.h/cpumask_test is intentionally NOT included
// in this file (it re-defines MAX_CPUS; see the local-MAX_CPUS note above), so the
// affinity mask is a plain uint64_t tested by hand. bit N set => cpu N is allowed.
// Guards cpu<64 (the mask is 64-wide) so a corrupt/out-of-range cpu can't shift UB.
static inline int sched_dbg_cpu_allowed(uint64_t mask, uint32_t cpu) {
    return cpu < 64 ? (int)((mask >> cpu) & 1ULL) : 0;
}

// Is `p` linked anywhere in runqueue `rq`? Bounded scan of all priorities.
static int sched_dbg_in_rq(const runqueue_t* rq, const process_t* p) {
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        const process_t* it = rq->queues[prio];
        uint32_t guard = 1024;                 // > MAX_PROCESSES; defends vs a cycle
        while (it && guard--) {
            if (it == p) return 1;
            it = it->next;
        }
    }
    return 0;
}

// Is `p` currently linked on the global timer sleep list? Bounded walk.
static int sched_dbg_on_sleep_list(const process_t* p) {
    const process_t* it = g_sleep_list;
    uint32_t guard = 1024;
    while (it && guard--) {
        if (it == p) return 1;
        it = it->sleep_next;
    }
    return 0;
}

// F3-1: count how many online CPUs have `p` linked in their runqueues. >1 is the
// cross-cpu DOUBLE-ENQUEUE race signature -- the exact corruption the RACE-001
// single-critical-section enqueue prevents, and the failure mode per-CPU rq_locks
// must never produce. Bounded, lock-free reads. SAFE at F3-1 (single runqueue
// writer == cpu0); becomes racy once F3-3 makes CPU1 mutate a runqueue (see the
// cross-cpu deferral note above). Returns the membership count.
static int sched_dbg_cross_cpu_duplicates(const process_t* p, const char* where) {
    if (!p) return 0;
    int seen = 0;
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
        if (!cpus[cid].online) continue;
        if (sched_dbg_in_rq(cpus[cid].rq_active,  p) ||
            sched_dbg_in_rq(cpus[cid].rq_expired, p)) {
            seen++;
        }
    }
    if (seen > 1)
        SCHED_VIOLATION(where, p->pid, "task on %d CPU runqueues (cross-cpu duplicate)", seen);
    return seen;
}

// Per-task invariant guard. `where` names the call site. Returns violations seen.
static int sched_validate_task(process_t* p, const char* where) {
    if (!p) return 0;
    cpu_t* c = this_cpu();
    uint64_t before = g_sched_invariant_violations;

    // F3-1: a queued task is linked on the runqueue of the cpu it CLAIMS
    // (queued_cpu, VALID iff on_queue==1). Evaluate membership against the CLAIMED
    // cpu so a mis-routed enqueue is detectable as a hard invariant violation. For
    // a non-queued task queued_cpu is stale, so fall back to this_cpu() (at N=1 both
    // resolve to cpus[0], making this byte-identical to the F3-0 behavior today).
    uint32_t qcpu = (p->on_queue && p->queued_cpu < MAX_CPUS) ? p->queued_cpu : cpu_id();
    cpu_t* qc = &cpus[qcpu];

    int in_active  = sched_dbg_in_rq(qc->rq_active,  p);
    int in_expired = sched_dbg_in_rq(qc->rq_expired, p);
    int on_sleep   = sched_dbg_on_sleep_list(p);
    int on_wait    = (p->wait_on != NULL);

    if (p == c->idle_thread) {
        // Idle is the fallback -- never enqueued; exempt from state<->queue.
        if (p->on_queue || in_active || in_expired)
            SCHED_VIOLATION(where, p->pid, "idle thread is enqueued");
        return (int)(g_sched_invariant_violations - before);
    }

    if (in_active && in_expired)
        SCHED_VIOLATION(where, p->pid, "on BOTH active and expired runqueues");
    if (!p->on_queue && p->next != NULL)
        SCHED_VIOLATION(where, p->pid, "on_queue=0 but next!=NULL (stale link)");
    if (p->on_queue && !(in_active || in_expired))
        SCHED_VIOLATION(where, p->pid, "on_queue=1 but not linked on cpus[%u] (queued_cpu) runqueue", qcpu);
    if (!p->on_queue && (in_active || in_expired))
        SCHED_VIOLATION(where, p->pid, "on_queue=0 but linked on a runqueue");
    // F3-1: a queued task must be on EXACTLY ONE cpu's runqueue. Cross-cpu
    // duplication is the double-enqueue race signature (no-op/benign at N=1).
    if (p->on_queue)
        sched_dbg_cross_cpu_duplicates(p, where);
    // F3-2 affinity invariants (gated on on_queue, so idle -- never queued -- is
    // auto-exempt; qcpu is the bounds-safe queued_cpu computed above). LOG-only.
    if (p->on_queue) {
        if (p->allowed_cpus == 0)
            SCHED_VIOLATION(where, p->pid, "allowed_cpus=0 (allowed on NO cpu) -- missed ctor default?");
        else if (!sched_dbg_cpu_allowed(p->allowed_cpus, qcpu))
            SCHED_VIOLATION(where, p->pid, "queued_cpu=%u not in allowed_cpus=0x%016llx",
                            qcpu, (unsigned long long)p->allowed_cpus);
        if (p->pinned_cpu != CPU_NONE && qcpu != p->pinned_cpu)
            SCHED_VIOLATION(where, p->pid, "queued_cpu=%u but pinned_cpu=%u (pinned task on wrong cpu)",
                            qcpu, p->pinned_cpu);
    }
    // A task is NEVER on a runqueue AND the SLEEP list at once. NOTE: wait_on is
    // deliberately NOT used as a membership signal here -- it is set on block and
    // left STALE after wake (harmless: every reader guards it with state==BLOCKED),
    // so a just-woken READY task being enqueued legitimately still has a non-NULL
    // wait_on. Only the runqueue walk and the g_sleep_list walk are reliable here;
    // wait_on is consulted ONLY in the BLOCKED case below where it is current.
    if ((in_active || in_expired) && on_sleep)
        SCHED_VIOLATION(where, p->pid, "linked on a runqueue AND the sleep list");

    switch (p->state) {
    case PROCESS_READY:
        if (!p->on_queue)
            SCHED_VIOLATION(where, p->pid, "state=READY but on_queue=0");
        if (!(in_active || in_expired))
            SCHED_VIOLATION(where, p->pid, "state=READY but not on any runqueue");
        if (on_sleep)   // reliable; wait_on is stale-after-wake (see note above)
            SCHED_VIOLATION(where, p->pid, "state=READY but on the sleep list");
        break;
    case PROCESS_RUNNING:
        if (p->on_queue || in_active || in_expired)
            SCHED_VIOLATION(where, p->pid, "state=RUNNING but still on a runqueue");
        break;
    case PROCESS_BLOCKED:
        if (p->on_queue || in_active || in_expired)
            SCHED_VIOLATION(where, p->pid, "state=BLOCKED but still on a runqueue");
        if (!on_wait && !on_sleep)
            SCHED_VIOLATION(where, p->pid, "state=BLOCKED but on neither wait nor sleep list");
        break;
    case PROCESS_TERMINATED:
        if (p->on_queue || in_active || in_expired)
            SCHED_VIOLATION(where, p->pid, "state=TERMINATED but still on a runqueue");
        if (on_sleep)   // reliable list walk; wait_on/wait_next/sleep_next can be stale
            SCHED_VIOLATION(where, p->pid, "state=TERMINATED but still on the sleep list");
        break;
    default:
        SCHED_VIOLATION(where, p->pid, "invalid state value %d", (int)p->state);
        break;
    }
    return (int)(g_sched_invariant_violations - before);
}

// Walk ONE runqueue: per-node + structural checks, accumulate the live node count.
static void sched_dbg_walk_rq(runqueue_t* rq, uint32_t cid, const char* tag,
                              const char* where, uint32_t* out_count) {
    uint32_t bound = cpus[cid].ready_count + 16;          // termination bound
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* it = rq->queues[prio];
        int bit = bitmap_test(rq->bitmap, prio);
        if (bit && it == NULL) {
            kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u %s prio=%d : bitmap set but queue empty\n", where, cid, tag, prio);
            g_sched_invariant_violations++;
        }
        if (!bit && it != NULL) {
            kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u %s prio=%d : queue non-empty but bitmap clear\n", where, cid, tag, prio);
            g_sched_invariant_violations++;
        }
        process_t* last = NULL;
        while (it) {
            (*out_count)++;
            if (*out_count > bound) {            // cycle defense
                kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u %s : walk exceeded bound %u (CYCLE?) pid=%d\n", where, cid, tag, bound, (int)it->pid);
                g_sched_invariant_violations++;
                return;
            }
            // A queued node must be READY, OR a TERMINATED task awaiting the
            // KILL-FIX-001 drain in pick_next (a legitimate, self-healing transient:
            // sys_kill sets TERMINATED on an already-enqueued task; pick_next discards
            // it on the next pick). Any OTHER state (RUNNING/BLOCKED/garbage) in a
            // runqueue is a real violation.
            if (it->state != PROCESS_READY && it->state != PROCESS_TERMINATED) {
                kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u %s : queued pid=%d state=%d (not READY/TERMINATED)\n", where, cid, tag, (int)it->pid, (int)it->state);
                g_sched_invariant_violations++;
            }
            if (!it->on_queue) {
                kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u %s : queued pid=%d on_queue=0\n", where, cid, tag, (int)it->pid);
                g_sched_invariant_violations++;
            }
            last = it;
            it = it->next;
        }
        if (rq->queues[prio] != NULL) {
            if (rq->tails[prio] != last) {
                kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u %s prio=%d : tails[] != actual last node\n", where, cid, tag, prio);
                g_sched_invariant_violations++;
            }
            if (last && last->next != NULL) {
                kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u %s prio=%d : last node next!=NULL\n", where, cid, tag, prio);
                g_sched_invariant_violations++;
            }
        }
    }
}

// Whole-runqueue structural sweep across all online CPUs. Bounded, lock-free reads.
static int sched_validate_runqueues(const char* where) {
    uint64_t before = g_sched_invariant_violations;
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
        if (!cpus[cid].online) continue;
        uint32_t counted = 0;
        sched_dbg_walk_rq(cpus[cid].rq_active,  cid, "active",  where, &counted);
        sched_dbg_walk_rq(cpus[cid].rq_expired, cid, "expired", where, &counted);
        if (counted != cpus[cid].ready_count) {
            kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u : ready_count=%u but counted %u nodes\n", where, cid, cpus[cid].ready_count, counted);
            g_sched_invariant_violations++;
        }
        if (cpus[cid].idle_thread &&
            (sched_dbg_in_rq(cpus[cid].rq_active,  cpus[cid].idle_thread) ||
             sched_dbg_in_rq(cpus[cid].rq_expired, cpus[cid].idle_thread))) {
            kprintf("[SCHED_INVARIANT] VIOLATION at %s cpu=%u : idle_thread is enqueued\n", where, cid);
            g_sched_invariant_violations++;
        }
    }

    // F3-1: cross-cpu duplicate sweep -- a task linked on >1 cpu's runqueue is the
    // double-enqueue race signature. Only MEANINGFUL once >=2 cpus own runqueues;
    // at F3-1 only cpu0 ever mutates a runqueue (CPU1 is offload-only), so counting
    // online cpus first keeps the common single-runqueue path free (zero work) and
    // this becomes active hardening at F3-3. Bounded by each cpu's ready_count.
    uint32_t online_cpus = 0;
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) if (cpus[cid].online) online_cpus++;
    if (online_cpus > 1) {
        for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
            if (!cpus[cid].online) continue;
            uint32_t bound = cpus[cid].ready_count + 16, visited = 0;
            runqueue_t* rqs[2] = { cpus[cid].rq_active, cpus[cid].rq_expired };
            for (int q = 0; q < 2; q++) {
                for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
                    process_t* it = rqs[q]->queues[prio];
                    while (it && visited < bound) {
                        visited++;
                        sched_dbg_cross_cpu_duplicates(it, where);
                        it = it->next;
                    }
                }
            }
        }
    }

    // F3-2 affinity sweep: every queued task must have a non-zero mask, must be
    // allowed on the cpu it is ACTUALLY linked on (cid, the definitely-valid walk
    // index -- more robust than a possibly-stale queued_cpu), and if pinned must be
    // pinned to that cpu. Bounded by each cpu's ready_count. At N=1 every task is on
    // cpu0 with bit0 set -> zero violations. LOG-only.
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
        if (!cpus[cid].online) continue;
        uint32_t bound = cpus[cid].ready_count + 16, visited = 0;
        runqueue_t* rqs[2] = { cpus[cid].rq_active, cpus[cid].rq_expired };
        for (int q = 0; q < 2; q++)
            for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
                process_t* it = rqs[q]->queues[prio];
                while (it && visited < bound) {
                    visited++;
                    if (it->allowed_cpus == 0)
                        SCHED_VIOLATION(where, it->pid, "queued on cpu=%u but allowed_cpus=0", cid);
                    else if (!sched_dbg_cpu_allowed(it->allowed_cpus, cid))
                        SCHED_VIOLATION(where, it->pid, "on cpu=%u not in allowed_cpus=0x%016llx",
                                        cid, (unsigned long long)it->allowed_cpus);
                    if (it->pinned_cpu != CPU_NONE && it->pinned_cpu != cid)
                        SCHED_VIOLATION(where, it->pid, "on cpu=%u but pinned_cpu=%u", cid, it->pinned_cpu);
                    it = it->next;
                }
            }
    }
    return (int)(g_sched_invariant_violations - before);
}

// F3-3a: acquire EVERY online cpu's rq_lock in ASCENDING cpu-id order -- the lock
// order for any AUTHORITATIVE all-cpu runqueue scan. Pairs with the release helper
// (REVERSE order). ONLY called from the at-rest selftests (scheduler_init), where the
// caller holds NO rq_lock, so there is no self-deadlock and no re-acquire of a held
// lock. Mirrors scheduler_shutdown's outer-then-ascending discipline -> no ABBA vs
// shutdown; scheduler_add_process_to_cpu takes exactly ONE foreign lock and never
// nests, so no ABBA there either. WHY THIS IS NEEDED NOW (F3-3 frontier): once CPU1
// mutates cpus[1].rq (F3-3b dispatch), the previously-lock-free cross-cpu walks in the
// selftests become TOCTOU-racy (torn next-pointers, false cross-cpu-duplicate,
// ready_count skew). Arming the lock discipline BEFORE the second writer exists is the
// prerequisite for a trustworthy validator (scheduler law 7: detect an untrustworthy
// cpu race-free). At N=1 / SMP_FOUNDATION only cpus[0] is online, so this takes exactly
// cpus[0].rq_lock -- byte-identical to the single-lock it replaces.
static void sched_acquire_all_rq_locks(void) {
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++)
        if (cpus[cid].online) spin_lock(&cpus[cid].rq_lock);
}
static void sched_release_all_rq_locks(void) {
    for (int cid = (int)MAX_CPUS - 1; cid >= 0; cid--)
        if (cpus[cid].online) spin_unlock(&cpus[cid].rq_lock);
}

// F3-1: at-rest per-CPU runqueue TOPOLOGY proof (modeled on paging_alias_selftest).
// Runs at the END of scheduler_init -- AFTER rq init + idle creation, BEFORE init
// spawns any service, so the queues are provably empty. Holds cpus[0].rq_lock for
// the structural walks (honors the "walk under the relevant rq_lock" contract; at
// this boot point no AP is online yet, so it is single-threaded). Checks 4 things
// and emits exactly one greppable line: "RQLOCK: PASS" (all-pass) or "RQLOCK: FAIL".
static void scheduler_rqlock_selftest(void) {
    int fails = 0;

    // Inv 1: every online cpu's rq_lock is INITIALIZED (unlocked). Checked BEFORE we
    // acquire any rq_lock, so cpu0's own lock still reads the init signature.
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
        if (!cpus[cid].online) continue;
        if (cpus[cid].rq_lock.lock != 0
#ifdef SPINLOCK_DEBUG
            || cpus[cid].rq_lock.owner_cpu != 0xFFFFFFFF
#endif
            ) {
            kprintf("[SCHEDULER] RQLOCK: inv1 cpu=%u rq_lock not initialized (lock=%u"
#ifdef SPINLOCK_DEBUG
                    " owner=%x"
#endif
                    ")\n",
                    cid, cpus[cid].rq_lock.lock
#ifdef SPINLOCK_DEBUG
                    , cpus[cid].rq_lock.owner_cpu
#endif
                    );
            fails++;
        }
    }

    // F3-3a: hold ALL online cpus' rq_locks (ascending) for the structural walks, so
    // the cross-cpu reads below are AUTHORITATIVE once CPU1 becomes a live runqueue
    // writer (F3-3b). At N=1 / SMP_FOUNDATION this is exactly cpus[0].rq_lock.
    sched_acquire_all_rq_locks();
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
        if (!cpus[cid].online) continue;
        // Inv 2: ready_count == bounded node walk of active+expired.
        uint32_t counted = 0, bound = cpus[cid].ready_count + 16;
        runqueue_t* rqs[2] = { cpus[cid].rq_active, cpus[cid].rq_expired };
        for (int q = 0; q < 2; q++)
            for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
                process_t* it = rqs[q]->queues[prio];
                while (it && counted <= bound) { counted++; it = it->next; }
            }
        if (counted != cpus[cid].ready_count) {
            kprintf("[SCHEDULER] RQLOCK: inv2 cpu=%u counted=%u != ready_count=%u\n",
                    cid, counted, cpus[cid].ready_count);
            fails++;
        }
        // Inv 4 (F3-3a RELAXED): a SECONDARY cpu (cid>0) may now LEGALLY host a pinned
        // task (F3-3b puts cpu1hello on CPU1). The invariant is no longer "empty" but
        // "every secondary-queued task is LEGALLY pinned here": pinned_cpu==cid AND
        // allowed_cpus has bit cid. Vacuous at scheduler_init in default/SMP_FOUNDATION
        // (no secondary online); ACTIVE in the SMP_SCHED_DISPATCH cpu1_smoke vehicle.
        if (cid != 0) {
            uint32_t v2 = 0, b2 = cpus[cid].ready_count + 16;
            for (int q = 0; q < 2; q++)
                for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
                    process_t* it = rqs[q]->queues[prio];
                    while (it && v2 < b2) {
                        v2++;
                        if (it->pinned_cpu != cid || !((it->allowed_cpus >> cid) & 1ULL)) {
                            kprintf("[SCHEDULER] RQLOCK: inv4 cpu=%u pid=%u queued but NOT legally pinned here (pinned_cpu=%u allowed=0x%016llx)\n",
                                    cid, it->pid, it->pinned_cpu, (unsigned long long)it->allowed_cpus);
                            fails++;
                        }
                        it = it->next;
                    }
                }
        }
    }

    // Inv 3: no task on >1 cpu's runqueue. Meaningful only with >=2 runqueue-owning
    // cpus (else trivially true); reuses the cross-cpu duplicate scan.
    uint32_t online_cpus = 0;
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) if (cpus[cid].online) online_cpus++;
    if (online_cpus > 1) {
        for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
            if (!cpus[cid].online) continue;
            uint32_t bound = cpus[cid].ready_count + 16, visited = 0;
            runqueue_t* rqs[2] = { cpus[cid].rq_active, cpus[cid].rq_expired };
            for (int q = 0; q < 2; q++)
                for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
                    process_t* it = rqs[q]->queues[prio];
                    while (it && visited < bound) {
                        visited++;
                        if (sched_dbg_cross_cpu_duplicates(it, "rqlock_selftest") > 1) fails++;
                        it = it->next;
                    }
                }
        }
    }
    sched_release_all_rq_locks();

    if (fails == 0)
        kprintf("[SCHEDULER] RQLOCK: PASS (all-rq_lock authoritative: init + ready_count==walk + no cross-cpu dup + secondary tasks legally pinned)\n");
    else
        kprintf("[SCHEDULER] RQLOCK: FAIL (%d invariant(s) violated)\n", fails);
}

// F3-2: at-rest CPU-affinity proof. Runs at END of scheduler_init (after the rqlock
// selftest). NON-VACUOUS by design: the runqueues are EMPTY at this boot point, so a
// pure queue walk would prove nothing -- instead it (1) self-tests the affinity
// PREDICATE on synthetic masks, and (2) asserts every online cpu's idle thread carries
// a NON-ZERO mask, which directly proves the ctor default fired (idle funnels through
// process_create; a zero mask here == the memset-trap default was dropped). It also
// (3) walks any queued task (none at boot; real if ever re-invoked later) for the full
// invariant. Holds cpus[0].rq_lock (single-threaded here; no AP online yet in default).
// Emits exactly one greppable line: "AFFINITY: PASS"/"FAIL".
static void scheduler_affinity_selftest(void) {
    int fails = 0;

    // (1) Predicate self-test -- the affinity machinery must be correct independent of
    // any enqueued task (boot queues are empty). A CPU0-only mask allows cpu0, forbids
    // cpu1; the CPU_NONE sentinel must not collide with a real cpu id.
    if (!sched_dbg_cpu_allowed((uint64_t)1 << 0, 0)) { kprintf("[SCHEDULER] AFFINITY: predicate FAIL (cpu0 not in {cpu0})\n"); fails++; }
    if ( sched_dbg_cpu_allowed((uint64_t)1 << 0, 1)) { kprintf("[SCHEDULER] AFFINITY: predicate FAIL (cpu1 in {cpu0})\n"); fails++; }
    if (CPU_NONE == 0 || CPU_NONE == 1)              { kprintf("[SCHEDULER] AFFINITY: predicate FAIL (CPU_NONE collides with a real cpu)\n"); fails++; }

    // F3-3a: all online cpus' rq_locks (ascending) so the cross-cpu queue walk below is
    // authoritative once CPU1 is a live writer. cpus[0]-only at N=1 / SMP_FOUNDATION.
    sched_acquire_all_rq_locks();
    for (uint32_t cid = 0; cid < MAX_CPUS; cid++) {
        if (!cpus[cid].online) continue;
        // (2) The ctor-default-fired proof: idle funnels through process_create, so a
        // zero idle mask means the F3-2 default was dropped. Idle is never enqueued,
        // so this is its only check.
        process_t* idle = cpus[cid].idle_thread;
        if (idle && idle->allowed_cpus == 0) {
            kprintf("[SCHEDULER] AFFINITY: idle pid=%u cpu=%u allowed_cpus=0 (ctor default missed)\n", idle->pid, cid);
            fails++;
        }
        // (3) Any queued task must have a non-zero mask, be allowed on the cpu it is
        // actually on, and (if pinned) be pinned to that cpu. Empty at boot; bounded.
        uint32_t bound = cpus[cid].ready_count + 16, visited = 0;
        runqueue_t* rqs[2] = { cpus[cid].rq_active, cpus[cid].rq_expired };
        for (int q = 0; q < 2; q++)
            for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
                process_t* it = rqs[q]->queues[prio];
                while (it && visited < bound) {
                    visited++;
                    if (it->allowed_cpus == 0) {
                        kprintf("[SCHEDULER] AFFINITY: pid=%u queued on cpu=%u allowed_cpus=0\n", it->pid, cid); fails++;
                    } else if (!sched_dbg_cpu_allowed(it->allowed_cpus, cid)) {
                        kprintf("[SCHEDULER] AFFINITY: pid=%u on cpu=%u not in allowed_cpus=0x%016llx\n",
                                it->pid, cid, (unsigned long long)it->allowed_cpus); fails++;
                    }
                    if (it->pinned_cpu != CPU_NONE && it->pinned_cpu != cid) {
                        kprintf("[SCHEDULER] AFFINITY: pid=%u on cpu=%u but pinned_cpu=%u\n", it->pid, cid, it->pinned_cpu); fails++;
                    }
                    it = it->next;
                }
            }
    }
    sched_release_all_rq_locks();

    if (fails == 0)
        kprintf("[SCHEDULER] AFFINITY: PASS (predicate sound + ctor defaults fired + no queued task off-affinity)\n");
    else
        kprintf("[SCHEDULER] AFFINITY: FAIL (%d invariant(s) violated)\n", fails);
}
#else  /* !SCHED_DEBUG: compile to nothing for a perf build */
static inline int sched_validate_task(process_t* p, const char* where) { (void)p; (void)where; return 0; }
static inline int sched_validate_runqueues(const char* where) { (void)where; return 0; }
static void scheduler_rqlock_selftest(void) { }  /* perf build: no topology proof */
static void scheduler_affinity_selftest(void) { }  /* perf build: no affinity proof */
#define SCHED_DBG_SHOULD_SAMPLE() (0)  /* perf build: validation compiled out anyway */
#endif /* SCHED_DEBUG */

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
    // the ready_count bump must all be ATOMIC under this_cpu()->rq_lock (F3-1: was the
    // global scheduler_lock). The old code dropped the lock between the on_queue test
    // and the ref/enqueue, so two CPUs (on SMP_SCHED, once Brick-F dispatch makes a
    // second CPU call this) could BOTH observe on_queue==0, both process_ref, and both
    // runqueue_enqueue the same PCB -- overwriting its single `next` link (list
    // truncation / cycle) and leaking the extra ref. Latent on the uniprocessor-
    // cooperative default; this mirrors the already-correct single-critical-section
    // pattern in scheduler_add_process_to_cpu. process_ref is a lock-free atomic add,
    // and process_set_ready / process_get_priority take no locks, so all are safe to
    // call while holding the per-cpu rq_lock. The enqueue targets this_cpu() (cpu_id()
    // == 0 on the BSP), so queued_cpu is set to cpu_id() inside this same section.
    spin_lock(&this_cpu()->rq_lock);
    if (proc->on_queue) {
        spin_unlock(&this_cpu()->rq_lock);
        PERF_END(PERF_OP_SCHEDULER_ADD);
        return;
    }

    process_ref(proc);          // the queue's reference (prevents use-after-free)
    process_set_ready(proc);
    proc->on_queue = 1;
    proc->queued_cpu = cpu_id();  // F3-1: membership lands on this_cpu()

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
    runqueue_enqueue(this_cpu()->rq_expired, proc, priority);

    this_cpu()->ready_count++;

#ifdef SCHED_DEBUG
    // DIAGNOSTIC: the first time a NON-init process (pid>1) is successfully
    // enqueued, paint a marker. This proves a spawned child was created, made
    // READY, and added to the runqueue — i.e. the scheduler now HAS something to
    // run. If this appears but "non-init proc RAN" does not, the child is queued
    // but never switched to (a dispatch bug). If it never appears, no child was
    // ever created (a load/spawn failure).
    if (proc->pid > 1) {
        static volatile int _child_enq_seen = 0;
        if (!_child_enq_seen) {
            _child_enq_seen = 1;
            extern void framebuffer_puts_scaled(const char*, uint32_t, uint32_t,
                                                uint32_t, uint32_t);
            framebuffer_puts_scaled("child ENQUEUED (ready)", 40, 216, 0x0000FF00u, 2);
        }
    }
#endif

    // F3-0 proof guard: settled state here (on_queue=1, state=READY, linked, counted).
    // Wave-7 perf: sampled every 64th call to keep the hot path fast.
    if (SCHED_DBG_SHOULD_SAMPLE()) {
        sched_validate_task(proc, "add_process:exit");
        sched_validate_runqueues("add_process:exit");
    }

    spin_unlock(&this_cpu()->rq_lock);

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

    // F3-1: per-cpu rq_lock (was scheduler_lock). 4a (early on_queue check) and 4b
    // (the main re-queue) take the SAME this_cpu() lock; the unlock-relock gap is
    // preserved EXACTLY as before -- a pre-existing, behavior-identical window, not
    // changed in F3-1.
    spin_lock(&this_cpu()->rq_lock);
    if (proc->on_queue) { spin_unlock(&this_cpu()->rq_lock); return; }
    spin_unlock(&this_cpu()->rq_lock);

    process_ref(proc);

    spin_lock(&this_cpu()->rq_lock);
    process_set_ready(proc);
    proc->on_queue = 1;
    proc->queued_cpu = cpu_id();  // F3-1: membership lands on this_cpu() (both branches)
    int priority = process_get_priority(proc);

    if (proc->yield_boost > 0) {
        // Spend one bonus turn: re-enter ACTIVE so this higher-priority process
        // is eligible to be picked again immediately (ahead of lower priorities).
        proc->yield_boost--;
        runqueue_enqueue(this_cpu()->rq_active, proc, priority);
    } else {
        // Out of bonus turns (or never had any): refill from nice and rotate to
        // EXPIRED, guaranteeing lower-priority ready processes get their turn
        // after the next active/expired swap (anti-starvation preserved).
        proc->yield_boost = priority_yield_boost(proc->priority);
        runqueue_enqueue(this_cpu()->rq_expired, proc, priority);
    }
    this_cpu()->ready_count++;
    spin_unlock(&this_cpu()->rq_lock);
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

    // RACE-001 fix: Acquire this cpu's runqueue lock before queue manipulation
    // (F3-1: was the global scheduler_lock).
    spin_lock(&this_cpu()->rq_lock);

    if (!proc->on_queue) {
        spin_unlock(&this_cpu()->rq_lock);
        PERF_END(PERF_OP_SCHEDULER_REMOVE);
        return;
    }

    // Calculate priority to narrow search
    int priority = process_get_priority(proc);

    // Try to remove from active runqueue first
    int found = runqueue_remove(this_cpu()->rq_active, proc, priority);

    // If not in active, try expired
    if (!found) {
        found = runqueue_remove(this_cpu()->rq_expired, proc, priority);
    }

    if (found) {
        proc->on_queue = 0;
        this_cpu()->ready_count--;

        // F3-0 proof guard: the runqueue must be consistent after removal. (The
        // removed task's state may be mid-transition at this site, so validate the
        // queue STRUCTURE only, not the removed task's per-state membership.)
        // Wave-7 perf: sampled every 64th call.
        if (SCHED_DBG_SHOULD_SAMPLE()) {
            sched_validate_runqueues("remove_process:exit");
        }

        spin_unlock(&this_cpu()->rq_lock);

#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Removed process '%s' (PID %d) from ready queue (priority: %d)\n",
                proc->name, proc->pid, priority);
#endif

        // Release reference that scheduler_add_process took
        process_unref(proc);
    } else {
        // Not found in either queue - already removed
        spin_unlock(&this_cpu()->rq_lock);
    }

    PERF_END(PERF_OP_SCHEDULER_REMOVE);
}

// DIAGNOSTIC markers (SCHED_DEBUG): split "scheduler picked the child" from
// "scheduler found nothing runnable and returned idle". Fire once each.
#ifdef SCHED_DEBUG
#define SCHED_PICK_IDLE_MARK() do { \
    static volatile int _pim = 0; \
    if (!_pim) { _pim = 1; \
        extern void framebuffer_puts_scaled(const char*,uint32_t,uint32_t,uint32_t,uint32_t); \
        framebuffer_puts_scaled("pick=IDLE (no runnable child!)", 40, 260, 0x00FF0000u, 2); } \
} while (0)
#define SCHED_PICK_CHILD_MARK(p) do { \
    static volatile int _pcm = 0; \
    if (!_pcm && (p) && (p)->pid > 1) { _pcm = 1; \
        extern void framebuffer_puts_scaled(const char*,uint32_t,uint32_t,uint32_t,uint32_t); \
        framebuffer_puts_scaled("pick=CHILD ok (dispatching)", 40, 282, 0x0000FF00u, 2); } \
} while (0)
#else
#define SCHED_PICK_IDLE_MARK() do {} while (0)
#define SCHED_PICK_CHILD_MARK(p) do {} while (0)
#endif

process_t* scheduler_pick_next(void) {
    // RACE-001 fix: Acquire this cpu's runqueue lock before queue access (F3-1: was
    // the global scheduler_lock). ALL six lock sites in this function -- the initial
    // acquire, the two idle-fallback releases, the terminated-drain release + its
    // re-lock in the pick_again loop, and the final release -- use this_cpu()->rq_lock
    // so the acquire/release are matched on the SAME lock (a mismatch would wedge).
    spin_lock(&this_cpu()->rq_lock);

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
    next = runqueue_pick_next(this_cpu()->rq_active);

    if (next == NULL) {
        // Active runqueue is empty - check if expired has processes
        if (runqueue_is_empty(this_cpu()->rq_expired)) {
            // Both runqueues are empty: nothing runnable. Return the idle thread,
            // which is NOT enqueued (see scheduler_init) — it is the dedicated
            // fallback that runs only in this all-empty state.
            spin_unlock(&this_cpu()->rq_lock);
            SCHED_PICK_IDLE_MARK();
            return this_cpu()->idle_thread;
        }

        // Swap active and expired runqueues
        runqueue_swap();

        // Try again from newly active runqueue
        next = runqueue_pick_next(this_cpu()->rq_active);

        if (next == NULL) {
            // Expired was non-empty but yielded nothing pickable (all entries
            // drained as terminated): fall back to the idle thread.
            spin_unlock(&this_cpu()->rq_lock);
            SCHED_PICK_IDLE_MARK();
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
        spin_unlock(&this_cpu()->rq_lock);
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] pick_next: discarding terminated '%s' (PID %d), "
                "picking again\n", next->name, next->pid);
#endif
        // Release the reference that scheduler_add_process took
        process_unref(next);
        spin_lock(&this_cpu()->rq_lock);
        goto pick_again;  // Try to pick another process
    }

    this_cpu()->ready_count--;
    next->on_queue = 0;

    // F3-0 proof guard: validate the runqueue STRUCTURE only here. The just-picked
    // task is dequeued (on_queue=0) but still PROCESS_READY (READY->RUNNING happens
    // later in process_set_current), so a per-task check would false-positive.
    // Wave-7 perf: sampled every 64th call.
    if (SCHED_DBG_SHOULD_SAMPLE()) {
        sched_validate_runqueues("pick_next:exit");
    }

    spin_unlock(&this_cpu()->rq_lock);

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
    SCHED_PICK_CHILD_MARK(next);
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
#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
    // F3-5: a CPU1 syscall that reschedules (sys_exit -> schedule()) must take
    // the AP-safe path -- the BSP body below writes the GLOBAL current via
    // process_set_current and ends with PIC-era assumptions. ap_cooperative_
    // schedule handles yield (requeue+switch) and exit (the dying path) on
    // cpus[1] without ever touching CPU0's state.
    if (cpu_id() != 0) {
        ap_cooperative_schedule();
        return;
    }
#endif
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

            // LEAK-FIX: drop the outgoing process's "running" ref BEFORE the
            // switch. scheduler_add_process(current) above already took a NEW
            // queue ref, so current has at least creation(1) + queue(1) = 2 refs
            // and will not be freed here. Without this, the running ref
            // accumulates on every yield/preemption cycle: ref grows by +1 each
            // time the process is re-enqueued and re-picked, and a long-running
            // process that yields N times exits with ref = 2+N. The TERMINATED
            // path only drops 1 (schedule's unref), the reaper drops 1 (creation),
            // and process_destroy drops 1 -- total 3, leaving N-1 orphan refs that
            // pin the PCB/8KB-stack/CR3/PID forever. Dropping it here restores the
            // invariant: a queued process has ref = creation(1) + queue(1) = 2, and
            // when re-picked the queue ref transfers to become the new running ref.
            // Safe: this runs BEFORE cooperative_switch_to, so we are still on
            // current's stack and current is still valid. Cannot reach ref 0:
            // creation ref + queue ref guarantee >= 2. BUG-006 constraint (no code
            // after context_switch) is respected: this is BEFORE the switch.
            process_unref(current);

            cooperative_switch_to(current, next);

            // BUG-006 fix: Do NOT put ANY code here that references 'current' or local variables!
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
            // LEAK-FIX: drop the outgoing process's old "running" ref. The queue
            // now holds a fresh ref from scheduler_add_process, so ref >= 2
            // (creation + queue); this will not free the process. Mirrors the
            // cooperative schedule() LEAK-FIX.
            process_unref(current);
        } else {
            // TERMINATED outgoing on the first-dispatch path (same as step 5):
            // drop the running ref without re-enqueuing. See step 5 comment.
            process_unref(current);
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
        // LEAK-FIX: drop the outgoing process's old "running" ref. The queue
        // now holds a fresh ref from scheduler_add_process, so ref >= 2
        // (creation + queue); this will not free the process. Mirrors the
        // cooperative schedule() LEAK-FIX.
        process_unref(current);
    } else {
        // TERMINATED current (self-SIGKILL that returned to ring 3, then
        // preempted by the timer): do NOT re-enqueue (a dead process must
        // never re-enter the ready queue), but DO drop the running ref so
        // the zombie can reach ref 0 when the reaper drops the creation ref.
        // This mirrors the cooperative schedule()'s KILL-FIX-002 unref of
        // `dead`. Safe: the creation ref (reaped==0) keeps ref_count >= 1,
        // so this cannot free the PCB here.
        process_unref(current);
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
    // T410 diagnostics: paint fine-grained progress markers to the framebuffer
    // so a photograph of a frozen T410 screen reveals the exact sub-step.
    // These are ALWAYS compiled (the freeze is invisible without them) and are
    // harmlessly overwritten by the compositor once the desktop comes up.
    // The function is declared in drivers.h; we use a local extern to avoid
    // pulling the full header into the scheduler.
    extern void framebuffer_puts_scaled(const char*, uint32_t, uint32_t,
                                        uint32_t, uint32_t);
    // DIAGNOSTIC markers for the ring-3 hand-off. Each successive call paints on
    // its OWN line (y advances 22px) so they DON'T overlap into an unreadable
    // smear — the LAST legible yellow line on a frozen screen pinpoints exactly
    // which sub-step died. Gated behind SCHED_DEBUG so SCHED_DEBUG=0 ships a
    // clean boot screen. (Previously all markers wrote y=16 and OR'd on top of
    // each other, which looked like "glitched" yellow text.)
#ifdef SCHED_DEBUG
    static int _sd_line = 0;
#define SCHED_DIAG(msg) \
    framebuffer_puts_scaled((msg), 40, 16 + (_sd_line++) * 22, 0x00FFFF00u, 2)
#else
#define SCHED_DIAG(msg) ((void)0)
#endif

    SCHED_DIAG("sched: pick_next");
#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] ========================================\n");
    kprintf("[SCHEDULER] Starting scheduler...\n");
    kprintf("[SCHEDULER] ========================================\n");
#endif

    // Pick the first process to run (may be idle thread if no user processes)
#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] Picking first process from ready queue...\n");
#endif
    process_t* first = scheduler_pick_next();

    // scheduler_pick_next() now always returns a valid process (either a real
    // runnable process or the idle thread), so we no longer panic on empty queue.
    // The idle thread is a valid boot target - it will halt until an interrupt
    // (e.g., a device driver) readies a real process.

    SCHED_DIAG("sched: picked, set TSS");
#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] First process selected: '%s' (PID %d)\n", first->name, first->pid);
    kprintf("[SCHEDULER]   Entry point: 0x%016lx\n", first->context.rip);
    kprintf("[SCHEDULER]   Stack pointer: 0x%016lx\n", first->context.rsp);
    kprintf("[SCHEDULER]   RFLAGS: 0x%lx\n", first->context.rflags);
    kprintf("[SCHEDULER]   CR3: 0x%016lx\n", first->context.cr3);
    kprintf("[SCHEDULER]   Time slice: %d ticks\n", first->time_slice);
    kprintf("[SCHEDULER]   State: %d -> RUNNING\n", first->state);
#endif

    // Set as current process
    first->state = PROCESS_RUNNING;
    process_set_current(first);
#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] Current process set to PID %d\n", first->pid);
#endif

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

#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] TSS.RSP0 set to 0x%016lx (kernel stack top)\n", kstack_top);
    kprintf("[SCHEDULER]   Kernel stack base: 0x%016lx\n", (uint64_t)first->kernel_stack);
#endif

    // Check if the first process is the idle thread (kernel thread) or a user
    // process. The idle thread has user_entry == 0 (it's kernel-only), so we
    // jump directly to its kernel function instead of entering ring 3.
    if (first->user_entry == 0) {
        // Idle thread or other kernel thread: jump directly to its kernel function.
        // context.rip points at idle_thread_func (or whatever kernel function).
        SCHED_DIAG("sched: IDLE (no user!)");
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] ========================================\n");
        kprintf("[SCHEDULER] Starting kernel thread '%s'...\n", first->name);
        kprintf("[SCHEDULER]   RIP=0x%016lx RSP=0x%016lx CR3=0x%016lx\n",
                first->context.rip, first->context.rsp, first->context.cr3);
        kprintf("[SCHEDULER] ========================================\n");
#endif

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

        // ---------------------------------------------------------------
        // FPU/SSE INIT for first dispatch (T410 cold-start fix).
        //
        // scheduler_start() calls enter_usermode() DIRECTLY, bypassing
        // context_switch() which normally primes the FPU template and
        // FXRSTOR's a clean state.  The hardware FPU registers retain
        // whatever the BIOS / kernel boot left: on real hardware (T410)
        // MXCSR may have unmasked SSE exceptions.  GCC-compiled userspace
        // uses XMM registers for memcpy / integer vectorisation, so the
        // very first SSE instruction would #XM with no handler -> #DF ->
        // triple fault (appears as a freeze at "starting services").
        //
        // Fix: eagerly init the FPU template (idempotent if already done)
        // and FXRSTOR the clean state so the hardware registers are safe
        // BEFORE the one-shot enter_usermode IRETQ.
        // ---------------------------------------------------------------
        SCHED_DIAG("sched: fpu init");
        {
            extern void context_fpu_template_init(void);
            context_fpu_template_init();

            // Prime the process's fpu_state if still zeroed (same logic as
            // context_switch / context_prime_fpu).
            extern int  fpu_state_needs_init(const uint8_t* fpu_state);
            extern void fpu_state_prime(uint8_t* fpu_state);
            if (fpu_state_needs_init(first->context.fpu_state)) {
                fpu_state_prime(first->context.fpu_state);
            }

            // Load the primed FPU state into the HARDWARE registers.
            // enter_usermode does NOT fxrstor (it only builds an IRETQ
            // frame), so without this the hardware retains stale MXCSR.
            __asm__ volatile("fxrstor64 %0" :: "m"(first->context.fpu_state));
        }

#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] ========================================\n");
        kprintf("[SCHEDULER] Transitioning to ring 3 (user mode)...\n");
        kprintf("[SCHEDULER]   RIP=0x%016lx RSP=0x%016lx CR3=0x%016lx\n",
                first->user_entry, first->user_rsp, first->context.cr3);
        kprintf("[SCHEDULER] ========================================\n");
#endif
        SCHED_DIAG("sched: enter_usermode");
        enter_usermode(first->user_entry, first->user_rsp, first->context.cr3);
    }

#undef SCHED_DIAG
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
    // F3-1: guard the SPECIFIC cpu's ready_count under ITS own rq_lock (was the
    // global scheduler_lock). Caller passes the target cpu id explicitly.
    spin_lock(&cpus[cpu_id].rq_lock);
    cpus[cpu_id].ready_count = 0;
    spin_unlock(&cpus[cpu_id].rq_lock);
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
// WARNING: Must be called with the target cpu's rq_lock (cpus[cpu_id].rq_lock) held
// or with interrupts disabled (F3-1: was the global scheduler_lock).
int scheduler_validate_ready_count(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return -1;
    }

    uint32_t actual_count = 0;
    cpu_t* cpu = &cpus[cpu_id];

    // Count processes in active runqueue
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* proc = cpu->rq_active->queues[prio];
        while (proc) {
            actual_count++;
            proc = proc->next;
        }
    }

    // Count processes in expired runqueue
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
        process_t* proc = cpu->rq_expired->queues[prio];
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
