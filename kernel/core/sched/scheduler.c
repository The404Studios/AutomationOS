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

// Active and expired runqueues (double-buffering)
static runqueue_t active_rq;
static runqueue_t expired_rq;
static uint32_t ready_count = 0;

// RACE-001 fix: Global scheduler lock protects runqueues
// This prevents race conditions when multiple CPUs add/remove processes
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
    runqueue_t tmp = active_rq;
    active_rq = expired_rq;
    expired_rq = tmp;
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
                it->state = PROCESS_READY;
                scheduler_add_process(it);
            }
            // *link now points at the successor; do NOT advance link.
        } else {
            link = &it->sleep_next;
        }
    }
    restore_flags(flags);
}

void scheduler_init(void) {
    kprintf("[SCHEDULER] Initializing O(1) multi-level feedback queue scheduler...\n");

    // RACE-001 fix: Initialize scheduler lock
    spin_lock_init(&scheduler_lock);

    // Initialize active and expired runqueues
    runqueue_init(&active_rq);
    runqueue_init(&expired_rq);
    ready_count = 0;

    kprintf("[SCHEDULER] O(1) scheduler initialized:\n");
    kprintf("[SCHEDULER]   - Priority levels: %d (0-139)\n", SCHED_PRIORITY_LEVELS);
    kprintf("[SCHEDULER]   - Time slice: %d ticks\n", DEFAULT_TIME_SLICE);
    kprintf("[SCHEDULER]   - Algorithm: Active/Expired double-buffering\n");
    kprintf("[SCHEDULER]   - Complexity: O(1) enqueue, O(1) dequeue, O(1) pick_next\n");
    kprintf("[SCHEDULER]   - SMP-safe: Yes\n");
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
    spin_lock(&scheduler_lock);
    if (proc->on_queue) {
        spin_unlock(&scheduler_lock);
        PERF_END(PERF_OP_SCHEDULER_ADD);
        return;
    }
    spin_unlock(&scheduler_lock);

    // Take reference when adding to queue (prevents use-after-free)
    process_ref(proc);

    // RACE-001 fix: Acquire scheduler lock before queue manipulation
    spin_lock(&scheduler_lock);

    proc->state = PROCESS_READY;
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
    runqueue_enqueue(&expired_rq, proc, priority);

    ready_count++;

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
    proc->state = PROCESS_READY;
    proc->on_queue = 1;
    int priority = process_get_priority(proc);

    if (proc->yield_boost > 0) {
        // Spend one bonus turn: re-enter ACTIVE so this higher-priority process
        // is eligible to be picked again immediately (ahead of lower priorities).
        proc->yield_boost--;
        runqueue_enqueue(&active_rq, proc, priority);
    } else {
        // Out of bonus turns (or never had any): refill from nice and rotate to
        // EXPIRED, guaranteeing lower-priority ready processes get their turn
        // after the next active/expired swap (anti-starvation preserved).
        proc->yield_boost = priority_yield_boost(proc->priority);
        runqueue_enqueue(&expired_rq, proc, priority);
    }
    ready_count++;
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
    int found = runqueue_remove(&active_rq, proc, priority);

    // If not in active, try expired
    if (!found) {
        found = runqueue_remove(&expired_rq, proc, priority);
    }

    if (found) {
        proc->on_queue = 0;
        ready_count--;

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
    next = runqueue_pick_next(&active_rq);

    if (next == NULL) {
        // Active runqueue is empty - check if expired has processes
        if (runqueue_is_empty(&expired_rq)) {
            // Both runqueues are empty - no processes to run
            spin_unlock(&scheduler_lock);
            return NULL;
        }

        // Swap active and expired runqueues
        runqueue_swap();

        // Try again from newly active runqueue
        next = runqueue_pick_next(&active_rq);

        if (next == NULL) {
            // Should never happen after swap, but defensive check
            spin_unlock(&scheduler_lock);
            return NULL;
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
        ready_count--;
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

    ready_count--;
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

    // If no current process, pick one
    if (current == NULL) {
        process_t* next = scheduler_pick_next();
        if (next) {
            next->state = PROCESS_RUNNING;
            process_set_current(next);
#ifndef SCHEDULER_QUIET
            kprintf("[SCHEDULER] Started process '%s' (PID %d) with time slice %d\n",
                    next->name, next->pid, next->time_slice);
#endif
        }
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
        if (next) {
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
            cooperative_switch_to(dead, next);

            // -------------------------------------------------------------------
            // KILL-FIX-002 (continued): Release the dead process's "current"
            // reference NOW.  context_switch() returns here when the *next*
            // process is eventually switched away from and this kernel stack
            // frame is resumed.  At that point `dead` holds the process that
            // was terminated; we release its scheduler reference exactly once.
            // scheduler_add_process() ensures TERMINATED processes are never
            // re-queued, so `dead` can never become current again — making this
            // unref safe and non-repeating.
            // -------------------------------------------------------------------
            process_unref(dead);
        } else {
            kernel_panic("[SCHEDULER] No processes to run after exit");
        }
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

        if (next == NULL) {
            // No other processes - continue current with a fresh priority-scaled slice
            current->time_slice = priority_time_slice(current->priority);
            current->state = PROCESS_RUNNING;
#ifndef SCHEDULER_QUIET
            kprintf("[SCHEDULER] No other processes, continuing '%s' (PID %d) with fresh time slice\n",
                    current->name, current->pid);
#endif
        } else {
            // Context switch to next process
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

    // 3) Pick a successor. If none, refill and continue current.
    process_t* next = scheduler_pick_next();
    if (next == NULL || next == current) {
        if (next == current) {
            // pick_next returned current (it was the only one re-queued earlier
            // in some races); just refill and continue.
        }
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
    extern void process_enter_usermode_trampoline(void);
    if (next->resume_mode != RESUME_IRETQ &&
        next->context.rip == (uint64_t)process_enter_usermode_trampoline) {
        // Causal-proof trace (gated: runs only in the PREEMPTIVE build, since
        // schedule_from_irq is the IRQ path). One line per never-run process the
        // first time the timer bootstraps it into ring 3 -- so the stress log
        // shows a first-dispatch for each previously-starved burner, then its
        // heartbeats. Naturally once-per-process; rate-limit/remove once stable.
        kprintf("[SCHED] first-dispatch pid=%d via irq iretq\n", next->pid);

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
        scheduler_add_process(next);   // re-queue (takes a fresh ref)
        process_unref(next);           // release pick_next's transferred ref
        if (current->state != PROCESS_TERMINATED) {
            current->time_slice = priority_time_slice(current->priority);
            current->need_resched = 0;
            current->state = PROCESS_RUNNING;
        }
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

    // Pick the first process to run
    kprintf("[SCHEDULER] Picking first process from ready queue...\n");
    process_t* first = scheduler_pick_next();

    if (!first) {
        kprintf("[SCHEDULER] ERROR: No processes in ready queue!\n");
        kernel_panic("scheduler_start: No processes to run");
    }

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

    // Should never reach here - enter_usermode does not return
    kernel_panic("scheduler_start: Returned from enter_usermode");
}

#endif // CONFIG_SMP
