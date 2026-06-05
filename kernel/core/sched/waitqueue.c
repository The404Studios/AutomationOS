/*
 * Unified Wait Object — the SINGLE blocking primitive
 * ===================================================
 *
 * Historically this kernel had TWO separate block/resume implementations that
 * were byte-for-byte the same discipline:
 *   (a) blocking sleep  — sys_sleep parked on a global timer sleep-list with a
 *       wake_deadline, woken by the per-tick timer scan (sleep_list_wake_due).
 *   (b) wait queues      — wq_block_current()/wq_wake_*() parked on a per-queue
 *       FIFO, woken by another context (waitpid, futex, epoll).
 *
 * They are now ONE primitive, wait_object_block(wo, deadline):
 *   • event wait  == wait_object_block(wo, 0)         — woken by a signal only
 *   • timer wait  == wait_object_block(wo, deadline)  — woken by signal OR timer
 * so `sleep == wait(timer)` and `wait_event == wait(event)` share one code path.
 *
 * THE BLOCK/RESUME DISCIPLINE (identical in both old primitives, captured once
 * here): record the waiter BEFORE marking it BLOCKED / switching away (so a
 * wakeup racing in right after the state store cannot miss it); mark it
 * PROCESS_BLOCKED; pick a successor; if none is runnable, sti/hlt idle until an
 * IRQ re-readies a process; cooperative_switch_to() the successor with
 * resume_mode = RESUME_CRETURN; and on resume touch NO stale locals — just
 * unlink from both lists and return. This works identically with or without
 * -DPREEMPTIVE (a preempted ring-3 successor is routed through iretq by
 * cooperative_switch_to).
 *
 * Intrusive list + ref discipline: waiters link through process_t.wait_next
 * (FIFO, with wait_on pointing back at the object). The object holds ONE
 * process_ref per linked waiter — exactly replacing the ref the old kmalloc'd
 * wait_entry held. That ref is what keeps a process killed while BLOCKED
 * (sys_kill sets it TERMINATED but leaves it linked here) alive until a later
 * signal/timer unlinks it and drops the ref. No per-wait heap allocation.
 *
 * Concurrency (uniprocessor): the only async mutator is the timer IRQ wakeup
 * scan. The object's spinlock + spin_lock_irqsave bracket every list mutation,
 * so an IRQ can never observe a half-linked node (and the IRQ scan itself runs
 * with IF=0). wait_object_signal may be called from a hard IRQ: it only
 * scheduler_add_process()'s and process_unref()'s (no kmalloc/kfree), so IRQ
 * wakeups are safe.
 */

#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/tss.h"
#include "../../include/x86_64.h"

// ===========================================================================
// wait_object_t — engine
// ===========================================================================

void wait_object_init(wait_object_t* wo) {
    if (!wo) return;
    wo->waiters = NULL;
    wo->tail = NULL;
    spin_lock_init(&wo->lock);
    wo->initialized = 1;
}

static void wo_ensure_init(wait_object_t* wo) {
    // Allow zero-initialized (BSS / kmalloc'd-and-memset) objects to be used
    // without an explicit wait_object_init(). A statically
    // WAIT_OBJECT_INITIALIZER'd object already has initialized == 1.
    if (!wo->initialized) {
        wait_object_init(wo);
    }
}

// Link `proc` at the tail of wo's intrusive FIFO waiter list and take the
// object's reference on it. Caller holds no locks; we take wo->lock internally.
static void wo_link_tail(wait_object_t* wo, process_t* proc) {
    // The object owns a reference to the parked process. Without this, killing a
    // process while it is BLOCKED (sys_kill sets state=TERMINATED but leaves it
    // linked here, since scheduler_remove_process is a no-op for a non-queued
    // process) would drop the last ref and free the PCB — leaving this list
    // pointing at freed memory. The ref keeps the PCB alive until it is unlinked
    // (and unref'd) by a later signal or the timer scan.
    process_ref(proc);

    uint64_t flags;
    spin_lock_irqsave(&wo->lock, &flags);
    proc->wait_on = wo;
    proc->wait_next = NULL;
    if (wo->tail) {
        wo->tail->wait_next = proc;
        wo->tail = proc;
    } else {
        wo->waiters = proc;
        wo->tail = proc;
    }
    spin_unlock_irqrestore(&wo->lock, flags);
}

// Unlink `proc` from wo if it is currently linked there. Returns 1 if it was
// found-and-removed (caller then owns the dropped object-ref and must unref),
// 0 if it was not linked (already unlinked by someone else — do NOT unref).
// Caller holds no locks.
static int wo_unlink(wait_object_t* wo, process_t* proc) {
    uint64_t flags;
    spin_lock_irqsave(&wo->lock, &flags);
    process_t* prev = NULL;
    process_t* it = wo->waiters;
    while (it) {
        if (it == proc) {
            if (prev) {
                prev->wait_next = it->wait_next;
            } else {
                wo->waiters = it->wait_next;
            }
            if (wo->tail == it) {
                wo->tail = prev;
            }
            it->wait_next = NULL;
            it->wait_on = NULL;
            spin_unlock_irqrestore(&wo->lock, flags);
            return 1;
        }
        prev = it;
        it = it->wait_next;
    }
    spin_unlock_irqrestore(&wo->lock, flags);
    return 0;
}

// Force-unlink `proc` from `wo` if (and only if) it is currently linked there,
// dropping the object-ref it holds. Idempotent: a no-op (and NO unref) if proc was
// already removed by a concurrent signal/timer. Used by the kill paths to reclaim
// the object-ref of a process killed while BLOCKED on this wait_object, so the PCB
// collapses to a reapable zombie instead of leaking. wo_unlink takes wo->lock
// IRQ-safe internally; callers hold no locks. IRQ-safety: a hard-IRQ
// wait_object_signal racing us either pops proc first (then we find nothing and do
// nothing) or runs after our unlink (then it finds nothing) -- wo->lock serializes
// the two, and exactly one of them drops the single object-ref. [#9]
int wait_object_abort(wait_object_t* wo, process_t* proc) {
    if (!wo || !proc) return 0;
    wo_ensure_init(wo);
    if (wo_unlink(wo, proc)) {
        process_unref(proc);   // release the stranded object-ref exactly once
        return 1;
    }
    return 0;
}

// Pop the head waiter (FIFO). Returns the parked process still holding the
// object's reference (caller must unref), or NULL if empty.
static process_t* wo_pop_head(wait_object_t* wo) {
    uint64_t flags;
    spin_lock_irqsave(&wo->lock, &flags);
    process_t* p = wo->waiters;
    if (!p) {
        spin_unlock_irqrestore(&wo->lock, flags);
        return NULL;
    }
    wo->waiters = p->wait_next;
    if (!wo->waiters) {
        wo->tail = NULL;
    }
    p->wait_next = NULL;
    p->wait_on = NULL;
    spin_unlock_irqrestore(&wo->lock, flags);
    return p;
}

int wait_object_block(wait_object_t* wo, uint64_t deadline_or_0) {
    if (!wo) return 0;
    wo_ensure_init(wo);

    process_t* current = process_get_current();
    if (!current) {
        kprintf("[WAIT] wait_object_block: no current process\n");
        return 0;
    }

    // Record the waiter BEFORE marking blocked / switching away, so a wakeup
    // (signal or timer) racing in right after we set state cannot miss us.
    // (Wakeups also take the object/sleep locks, and re-readying a not-yet-
    // switched-out process is safe: it goes back on the ready queue and we never
    // actually sleep.)
    wo_link_tail(wo, current);

    // Timer-armed wait: also park on the global sleep list with the deadline, so
    // the per-tick scan (sleep_list_wake_due) can wake us if the timeout fires
    // before any signal. sleep_list_push brackets the list mutation with
    // cli/restore so the timer IRQ can't observe a half-linked node.
    if (deadline_or_0 != 0) {
        current->wake_deadline = deadline_or_0;
        sleep_list_push(current);
    }

    current->state = PROCESS_BLOCKED;

    // Switch to another runnable process. Mirror sys_yield()/cooperative
    // context_switch exactly: pick a successor, hand it the CPU. The current
    // (blocked) process is NOT re-added to the ready queue, so it will not run
    // again until a signal or the timer re-readies it.
    process_t* idle = scheduler_idle_thread();
    process_t* next = scheduler_pick_next();

    // If nothing is runnable, idle until something becomes ready (an IRQ wakeup —
    // a signal-side scheduler_add_process or the timer scan — re-readies a
    // process). We must re-enable interrupts to make progress.
    //
    // scheduler_pick_next() returns the IDLE THREAD (never NULL) when both
    // runqueues are empty. In this cooperative path we must NOT switch into it:
    // the idle thread runs `sti;hlt` forever in ring 0 and never calls schedule(),
    // so a later IRQ that re-readies us would never switch back — a permanent hang,
    // and cooperative_switch_to() into idle's synthetic never-run context can fault
    // outright. So treat `idle` exactly like "nothing runnable": halt HERE on our
    // own kernel stack and re-check pick_next() after each IRQ wakeup.
    while (next == NULL || next == idle) {
        __asm__ volatile("sti; hlt; cli" ::: "memory");
        // If we ourselves were woken in the meantime, stop idling and resume.
        if (current->state != PROCESS_BLOCKED) {
            break;
        }
        next = scheduler_pick_next();
    }

    if (next != NULL && next != idle && next != current) {
        next->state = PROCESS_RUNNING;
        process_set_current(next);

        // Cooperative (C-return) save: when this process is later re-readied and
        // picked, context_switch() "returns" right here and we fall through to
        // the cleanup below. cooperative_switch_to() sets next's TSS/kernel stack
        // and routes RESUME_IRETQ successors (timer-preempted ring-3 tasks, e.g.
        // CPU burners) through iretq so a blocking task can hand off to them.
        current->resume_mode = RESUME_CRETURN;
        cooperative_switch_to(current, next);
        // Resumed here after a wakeup. (Stale locals above are NOT touched.)
    } else if (next == current) {
        // Degenerate: we got picked despite being blocked (woken between link and
        // pick). Just resume as RUNNING.
        next->state = PROCESS_RUNNING;
        process_set_current(next);
    }
    // else: next == NULL && we broke out of the idle loop already woken — resume.

    // ── Resume cleanup (runs in our own context, no stale locals) ──────────
    // We were woken by EITHER a signal (wait_object_signal already unlinked us
    // from the waiter list) OR the timer (sleep_list_wake_due unlinked us from
    // BOTH lists). Whatever linkage remains, tear it down so we leave on neither
    // list. wo_unlink/sleep_list_remove each drop the corresponding ref iff they
    // actually found us linked, so this is idempotent and ref-balanced.
    int woke_by_signal;
    if (wo_unlink(wo, current)) {
        // Still linked on the wait object => the waker did NOT take us off it
        // (i.e. the timer woke us first). Drop the object-ref we still hold.
        process_unref(current);
        woke_by_signal = 0;
    } else {
        // Already unlinked from the wait object => a signal popped us (it dropped
        // the object-ref as part of the wake). This is the signal wakeup.
        woke_by_signal = 1;
    }

    // Ensure we are off the timer sleep list too (we may have been signal-woken
    // while still armed with a deadline). sleep_list_remove is a no-op if absent.
    sleep_list_remove(current);
    current->wake_deadline = 0;

    return woke_by_signal;
}

// Wake exactly one waiter (FIFO), skipping any that already moved on (killed).
// Returns the re-readied process (the object's ref on it has been released — the
// scheduler now holds one; the returned pointer is "ref NOT taken" per the old
// wq_wake_one contract), or NULL if no live waiter was found.
static process_t* wo_wake_one_proc(wait_object_t* wo) {
    process_t* proc;
    while ((proc = wo_pop_head(wo)) != NULL) {  // holds the object's ref
        // Only re-ready genuinely-blocked processes. If the process already moved
        // on (e.g. it was killed — sys_kill set it TERMINATED but left it linked
        // here), release the object-ref and keep draining.
        if (proc->state != PROCESS_BLOCKED) {
            process_unref(proc);
            continue;
        }

        // A timer-armed waiter may also be on the sleep list; take it off so the
        // timer can't also "wake" it after we've re-readied it.
        sleep_list_remove(proc);
        proc->wake_deadline = 0;

        process_set_ready(proc);
        scheduler_add_process(proc);  // takes its own reference
        process_unref(proc);          // release the object-ref; scheduler holds one
        return proc;
    }
    return NULL;
}

int wait_object_signal(wait_object_t* wo, int wake_all) {
    if (!wo) return 0;
    wo_ensure_init(wo);

    int woken = 0;
    if (wake_all) {
        while (wo_wake_one_proc(wo) != NULL) {
            woken++;
        }
    } else {
        if (wo_wake_one_proc(wo) != NULL) {
            woken = 1;
        }
    }
    return woken;
}

int wait_object_count(wait_object_t* wo) {
    if (!wo) return 0;

    int n = 0;
    uint64_t flags;
    spin_lock_irqsave(&wo->lock, &flags);
    for (process_t* p = wo->waiters; p; p = p->wait_next) {
        n++;
    }
    spin_unlock_irqrestore(&wo->lock, flags);
    return n;
}

// ===========================================================================
// wait_queue_t — thin compatibility wrappers over the wait_object engine
// ===========================================================================
// Keep the exact historical public API so waitpid/futex/epoll are untouched;
// internally a wait_queue is just an event-only (deadline 0) wait_object.

void wq_init(wait_queue_t* wq) {
    if (!wq) return;
    wait_object_init(&wq->wobj);
}

void wq_block_current(wait_queue_t* wq) {
    if (!wq) return;
    // Event-only wait: no deadline. The discipline + ref-safety + resume are all
    // in wait_object_block (return value — signal vs timeout — is irrelevant for
    // an event-only wait, since it can only be woken by a signal).
    (void)wait_object_block(&wq->wobj, 0);
}

process_t* wq_wake_one(wait_queue_t* wq) {
    if (!wq) return NULL;
    wo_ensure_init(&wq->wobj);
    // Historical contract preserved exactly: return the woken process (ref NOT
    // taken — the scheduler holds the ref now) or NULL if the queue was empty /
    // held only already-dead waiters. futex_wake tests non-NULL == "woke one";
    // child_wait/epoll ignore the return.
    return wo_wake_one_proc(&wq->wobj);
}

int wq_wake_all(wait_queue_t* wq) {
    if (!wq) return 0;
    return wait_object_signal(&wq->wobj, 1);
}

int wq_count(wait_queue_t* wq) {
    if (!wq) return 0;
    return wait_object_count(&wq->wobj);
}
