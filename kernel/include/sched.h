#ifndef SCHED_H
#define SCHED_H

#include "types.h"

// Process states
typedef enum {
    PROCESS_CREATED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} process_state_t;

// CPU context saved during context switch
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr3;  // Page directory base
} cpu_context_t;

// Process Control Block
typedef struct process {
    uint32_t pid;                    // Process ID
    uint32_t parent_pid;             // Parent process ID
    process_state_t state;           // Current state
    cpu_context_t context;           // Saved CPU context
    void* kernel_stack;              // Kernel stack pointer
    void* user_stack;                // User stack pointer
    uint64_t time_slice;             // Remaining time slice (ticks)
    uint64_t total_time;             // Total CPU time used
    struct process* next;            // Next process in queue
    char name[64];                   // Process name
} process_t;

// Process table management
void process_init(void);
process_t* process_create(const char* name, void* entry_point);
void process_destroy(process_t* proc);
process_t* process_get_by_pid(uint32_t pid);
process_t* process_get_current(void);
void process_set_current(process_t* proc);

// Scheduler
void scheduler_init(void);
void scheduler_add_process(process_t* proc);
void scheduler_remove_process(process_t* proc);
void schedule(void);  // Scheduler tick - called from timer interrupt
process_t* scheduler_pick_next(void);

// Context switching
void context_switch(process_t* from, process_t* to);

#endif
