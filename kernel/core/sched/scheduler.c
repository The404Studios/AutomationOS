#include "../../include/sched.h"
#include "../../include/kernel.h"

// Ready queue (circular linked list)
static process_t* ready_queue_head = NULL;
static process_t* ready_queue_tail = NULL;
static uint32_t ready_count = 0;

// Scheduler constants
#define DEFAULT_TIME_SLICE 10  // 10 timer ticks

void scheduler_init(void) {
    kprintf("[SCHEDULER] Initializing round-robin scheduler...\n");

    ready_queue_head = NULL;
    ready_queue_tail = NULL;
    ready_count = 0;

    kprintf("[SCHEDULER] Scheduler initialized (time slice: %d ticks)\n", DEFAULT_TIME_SLICE);
}

void scheduler_add_process(process_t* proc) {
    if (!proc) return;

    proc->state = PROCESS_READY;
    proc->time_slice = DEFAULT_TIME_SLICE;
    proc->next = NULL;

    if (ready_queue_tail == NULL) {
        // Empty queue
        ready_queue_head = proc;
        ready_queue_tail = proc;
    } else {
        // Add to tail
        ready_queue_tail->next = proc;
        ready_queue_tail = proc;
    }

    ready_count++;

    kprintf("[SCHEDULER] Added process '%s' (PID %d) to ready queue\n",
            proc->name, proc->pid);
}

void scheduler_remove_process(process_t* proc) {
    if (!proc || ready_queue_head == NULL) return;

    process_t* current = ready_queue_head;
    process_t* prev = NULL;

    // Search for process in ready queue
    while (current != NULL) {
        if (current == proc) {
            // Found it - remove from queue
            if (prev == NULL) {
                // Remove from head
                ready_queue_head = current->next;
                if (ready_queue_head == NULL) {
                    ready_queue_tail = NULL;
                }
            } else {
                prev->next = current->next;
                if (current == ready_queue_tail) {
                    ready_queue_tail = prev;
                }
            }

            current->next = NULL;
            ready_count--;

            kprintf("[SCHEDULER] Removed process '%s' (PID %d) from ready queue\n",
                    proc->name, proc->pid);
            return;
        }

        prev = current;
        current = current->next;
    }
}

process_t* scheduler_pick_next(void) {
    if (ready_queue_head == NULL) {
        return NULL;  // No processes ready
    }

    // Round-robin: pick from head
    process_t* next = ready_queue_head;

    // Remove from head
    ready_queue_head = next->next;
    if (ready_queue_head == NULL) {
        ready_queue_tail = NULL;
    }

    ready_count--;
    next->next = NULL;

    // Reset time slice
    next->time_slice = DEFAULT_TIME_SLICE;

    return next;
}

void schedule(void) {
    // Called from timer interrupt (scheduler tick)

    process_t* current = process_get_current();

    // If no current process, pick one
    if (current == NULL) {
        process_t* next = scheduler_pick_next();
        if (next) {
            process_set_current(next);
            kprintf("[SCHEDULER] Started process '%s' (PID %d)\n",
                    next->name, next->pid);
        }
        return;
    }

    // Decrement time slice
    if (current->time_slice > 0) {
        current->time_slice--;
    }

    // Check if time slice expired
    if (current->time_slice == 0) {
        // Time slice expired - preempt
        kprintf("[SCHEDULER] Preempting process '%s' (PID %d)\n",
                current->name, current->pid);

        // Add current process back to ready queue
        scheduler_add_process(current);

        // Pick next process
        process_t* next = scheduler_pick_next();

        if (next == NULL) {
            // No other processes - continue current
            current->time_slice = DEFAULT_TIME_SLICE;
            kprintf("[SCHEDULER] No other processes, continuing '%s' (PID %d)\n",
                    current->name, current->pid);
        } else {
            // Context switch to next process
            kprintf("[SCHEDULER] Switching to process '%s' (PID %d)\n",
                    next->name, next->pid);

            process_t* old = current;
            process_set_current(next);
            context_switch(old, next);
        }
    }
}
