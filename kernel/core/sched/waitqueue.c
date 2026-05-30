/*
 * Wait Queues — blocking primitive for I/O and synchronization
 * ============================================================
 *
 * A wait_queue_t is a FIFO list of processes parked in PROCESS_BLOCKED. A
 * blocking subsystem (keyboard read, pipe, semaphore, ...) calls
 * wq_block_current() to sleep the running process until another context calls
 * wq_wake_one()/wq_wake_all().
 *
 * Relationship to the scheduler:
 *  - wq_block_current() removes the caller from the run path by marking it
 *    PROCESS_BLOCKED and switching away via the cooperative context_switch()
 *    (the SAME mechanism SYS_YIELD uses). The blocked process is therefore
 *    saved with resume_mode = RESUME_CRETURN and is resumed by the cooperative
 *    path — it simply "returns" from wq_block_current() when re-readied and
 *    next picked. This works identically with or without -DPREEMPTIVE.
 *  - wq_wake_one()/all() move processes back to PROCESS_READY and re-insert
 *    them into the scheduler ready queue via scheduler_add_process().
 *
 * Concurrency: each queue has its own spinlock. Wait-entry nodes are allocated
 * from the kernel heap. wq_wake_* may be called from process context; calling
 * them from a hard IRQ handler is safe ONLY if kmalloc/kfree are not used on
 * that path — these routines only kfree on the wake side and only kmalloc on
 * the (process-context) block side, so IRQ wakeups are safe.
 */

#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/tss.h"
#include "../../include/x86_64.h"

void wq_init(wait_queue_t* wq) {
    if (!wq) return;
    wq->head = NULL;
    wq->tail = NULL;
    spin_lock_init(&wq->lock);
    wq->initialized = 1;
}

static void wq_ensure_init(wait_queue_t* wq) {
    // Allow zero-initialized (BSS) queues to be used without an explicit
    // wq_init() call. A statically WAIT_QUEUE_INITIALIZER'd queue already has
    // initialized == 1 and is skipped.
    if (!wq->initialized) {
        wq_init(wq);
    }
}

// Enqueue a wait entry for `proc` on `wq`. Caller must hold no locks; this
// takes the queue lock internally. Returns 0 on success, -1 on OOM.
static int wq_enqueue(wait_queue_t* wq, process_t* proc) {
    wait_entry_t* e = (wait_entry_t*)kmalloc(sizeof(wait_entry_t));
    if (!e) {
        return -1;
    }
    e->proc = proc;
    e->next = NULL;
    // The entry owns a reference to the parked process. Without this, killing a
    // process while it is BLOCKED (sys_kill / procapi KILL drops the last ref
    // and frees the PCB) leaves this entry pointing at freed memory; a later
    // wq_wake_* would dereference and re-queue freed memory. The ref keeps the
    // PCB alive until the entry is dequeued (and unref'd) below.
    process_ref(proc);

    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    if (wq->tail) {
        wq->tail->next = e;
        wq->tail = e;
    } else {
        wq->head = e;
        wq->tail = e;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    return 0;
}

// Pop the head wait entry. Returns the parked process (and frees the node), or
// NULL if the queue is empty.
static process_t* wq_dequeue(wait_queue_t* wq) {
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    wait_entry_t* e = wq->head;
    if (!e) {
        spin_unlock_irqrestore(&wq->lock, flags);
        return NULL;
    }
    wq->head = e->next;
    if (!wq->head) {
        wq->tail = NULL;
    }
    spin_unlock_irqrestore(&wq->lock, flags);

    process_t* proc = e->proc;
    kfree(e);
    return proc;
}

void wq_block_current(wait_queue_t* wq) {
    if (!wq) return;
    wq_ensure_init(wq);

    process_t* current = process_get_current();
    if (!current) {
        kprintf("[WAITQ] wq_block_current: no current process\n");
        return;
    }

    // Record the waiter BEFORE marking blocked / switching away, so a wakeup
    // racing in right after we set state cannot miss us. (Wakeups also take
    // the queue lock, and re-readying a not-yet-switched-out process is safe:
    // it just goes back on the ready queue and we never actually sleep.)
    if (wq_enqueue(wq, current) != 0) {
        kprintf("[WAITQ] wq_block_current: OOM enqueuing PID %d\n", current->pid);
        return;  // Fail open: do not block (caller should re-check condition).
    }

    current->state = PROCESS_BLOCKED;

    // Switch to another runnable process. Mirror sys_yield()/cooperative
    // context_switch exactly: pick a successor, hand it the CPU. The current
    // (blocked) process is NOT re-added to the ready queue, so it will not run
    // again until wq_wake_* re-readies it.
    process_t* next = scheduler_pick_next();

    // If nothing else is runnable, idle until something becomes ready (an IRQ
    // wakeup will re-ready a process and the next pick will succeed). We must
    // re-enable interrupts to make progress.
    while (next == NULL) {
        __asm__ volatile("sti; hlt; cli" ::: "memory");
        // If we ourselves were woken in the meantime, stop idling and resume.
        if (current->state != PROCESS_BLOCKED) {
            return;
        }
        next = scheduler_pick_next();
    }

    if (next == current) {
        // Degenerate: we got picked despite being blocked (e.g. woken between
        // enqueue and pick). Just resume.
        next->state = PROCESS_RUNNING;
        process_set_current(next);
        return;
    }

    next->state = PROCESS_RUNNING;
    process_set_current(next);

    if (next->kernel_stack) {
        uint64_t kstack_top =
            ((uint64_t)next->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
        tss_set_kernel_stack(kstack_top);
    }

    // Cooperative (C-return) save: when this process is later re-readied and
    // picked, context_switch() "returns" right here and we fall through back to
    // the caller of wq_block_current().
    current->resume_mode = RESUME_CRETURN;
    context_switch(current, next);

    // Resumed here after a wakeup. (Do not touch stale locals; just return.)
}

process_t* wq_wake_one(wait_queue_t* wq) {
    if (!wq) return NULL;
    wq_ensure_init(wq);

    process_t* proc = wq_dequeue(wq);  // owns the entry's reference
    if (!proc) {
        return NULL;
    }

    // Only re-ready genuinely-blocked processes. If the process already moved
    // on (e.g. was killed), release its entry-ref and try the next one.
    while (proc && proc->state != PROCESS_BLOCKED) {
        process_unref(proc);
        proc = wq_dequeue(wq);
    }
    if (!proc) {
        return NULL;
    }

    proc->state = PROCESS_READY;
    scheduler_add_process(proc);  // takes its own reference
    process_unref(proc);          // release the entry-ref; scheduler holds one
    return proc;
}

int wq_wake_all(wait_queue_t* wq) {
    if (!wq) return 0;
    wq_ensure_init(wq);

    int woken = 0;
    process_t* proc;
    while ((proc = wq_dequeue(wq)) != NULL) {  // owns the entry's reference
        if (proc->state != PROCESS_BLOCKED) {
            process_unref(proc);  // release entry-ref for skipped process
            continue;
        }
        proc->state = PROCESS_READY;
        scheduler_add_process(proc);  // takes its own reference
        process_unref(proc);          // release the entry-ref
        woken++;
    }
    return woken;
}

int wq_count(wait_queue_t* wq) {
    if (!wq) return 0;

    int n = 0;
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    for (wait_entry_t* e = wq->head; e; e = e->next) {
        n++;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    return n;
}
