#include "../../include/sched.h"
#include "../../include/kernel.h"

// External assembly function (defined in context_switch.asm)
extern void context_switch_asm(process_t* from, process_t* to);

// C wrapper for context switching
void context_switch(process_t* from, process_t* to) {
    if (!to) {
        kernel_panic("context_switch: 'to' process is NULL");
    }

    // Update process states
    if (from) {
        // Save 'from' state
        from->total_time++;

        if (from->state == PROCESS_RUNNING) {
            from->state = PROCESS_READY;
        }

        kprintf("[CONTEXT] Switching from '%s' (PID %d) to '%s' (PID %d)\n",
                from->name, from->pid, to->name, to->pid);
    } else {
        kprintf("[CONTEXT] Starting first process '%s' (PID %d)\n",
                to->name, to->pid);
    }

    // Update 'to' state
    to->state = PROCESS_RUNNING;

    // Perform the actual context switch
    // This will save 'from' registers and restore 'to' registers
    context_switch_asm(from, to);

    // When we return here, we've been context-switched back
}
