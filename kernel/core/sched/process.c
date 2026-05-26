#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

#define MAX_PROCESSES 256
#define KERNEL_STACK_SIZE 8192  // 8KB kernel stack per process

// Process table
static process_t* process_table[MAX_PROCESSES];
static uint32_t next_pid = 1;  // PID 0 reserved for kernel/idle
static process_t* current_process = NULL;

// Simple PID allocation
static uint32_t allocate_pid(void) {
    if (next_pid >= MAX_PROCESSES) {
        kernel_panic("Process table full");
    }
    return next_pid++;
}

void process_init(void) {
    kprintf("[PROCESS] Initializing process management...\n");

    // Clear process table
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i] = NULL;
    }

    kprintf("[PROCESS] Process table initialized (max %d processes)\n", MAX_PROCESSES);
}

process_t* process_create(const char* name, void* entry_point) {
    // Allocate process structure
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) {
        kprintf("[PROCESS] Failed to allocate process structure\n");
        return NULL;
    }

    // Allocate PID
    uint32_t pid = allocate_pid();

    // Initialize process structure
    proc->pid = pid;
    proc->parent_pid = current_process ? current_process->pid : 0;
    proc->state = PROCESS_CREATED;
    proc->time_slice = 10;  // 10 ticks default
    proc->total_time = 0;
    proc->next = NULL;

    // Copy name
    size_t name_len = 0;
    while (name[name_len] && name_len < 63) {
        proc->name[name_len] = name[name_len];
        name_len++;
    }
    proc->name[name_len] = '\0';

    // Allocate kernel stack
    proc->kernel_stack = pmm_alloc_page();
    if (!proc->kernel_stack) {
        kprintf("[PROCESS] Failed to allocate kernel stack for PID %d\n", pid);
        kfree(proc);
        return NULL;
    }

    // Initialize kernel stack pointer (stack grows down)
    uint64_t kstack_top = (uint64_t)proc->kernel_stack + PAGE_SIZE;

    // Setup initial CPU context
    memset(&proc->context, 0, sizeof(cpu_context_t));
    proc->context.rip = (uint64_t)entry_point;  // Instruction pointer
    proc->context.rsp = kstack_top - 16;         // Stack pointer (aligned)
    proc->context.rflags = 0x202;                // IF (interrupts enabled)

    // For now, use kernel page directory (TODO: create per-process page tables)
    proc->context.cr3 = read_cr3();

    // TODO: Setup user stack once we have userspace
    proc->user_stack = NULL;

    // Add to process table
    process_table[pid] = proc;

    kprintf("[PROCESS] Created process '%s' (PID %d) at entry %p\n",
            proc->name, proc->pid, entry_point);

    return proc;
}

void process_destroy(process_t* proc) {
    if (!proc) return;

    kprintf("[PROCESS] Destroying process '%s' (PID %d)\n", proc->name, proc->pid);

    // Remove from process table
    if (proc->pid < MAX_PROCESSES) {
        process_table[proc->pid] = NULL;
    }

    // Free kernel stack
    if (proc->kernel_stack) {
        pmm_free_page(proc->kernel_stack);
    }

    // TODO: Free user stack and page directory

    // Free process structure
    kfree(proc);
}

process_t* process_get_by_pid(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return NULL;
    }
    return process_table[pid];
}

process_t* process_get_current(void) {
    return current_process;
}

void process_set_current(process_t* proc) {
    current_process = proc;
    if (proc) {
        proc->state = PROCESS_RUNNING;
    }
}
