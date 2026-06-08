/*
 * usermode.c - User mode transition support
 * ==========================================
 *
 * Handles switching from kernel mode (ring 0) to user mode (ring 3).
 * This is required to run userspace programs with reduced privileges.
 *
 * Key concepts:
 * - CPL (Current Privilege Level): 0 = kernel, 3 = user
 * - Segments: User code (0x1B), User data (0x23)
 * - TSS: Stores kernel stack for privilege transitions
 * - IRETQ: Instruction to return to user mode
 */

#include "../include/kernel.h"
#include "../include/x86_64.h"
#include "../include/tss.h"
#include "../include/mem.h"
#include "../include/sched.h"

// External assembly function
extern void enter_usermode(uint64_t entry, uint64_t stack, uint64_t cr3);

// User stack size (16 KB)
#define USER_STACK_SIZE (16 * 1024)

// KERNEL_STACK_SIZE is in sched.h (8192 = 8 KB)
// Do NOT redefine here - must match sched.h's canonical value

/*
 * Allocate a user mode stack
 * Returns the top of the stack (stack grows downward)
 */
static uint64_t allocate_user_stack(void) {
    // Allocate user stack
    void* stack_base = kmalloc(USER_STACK_SIZE);
    if (!stack_base) {
        kprintf("[USERMODE] ERROR: Failed to allocate user stack\n");
        return 0;
    }

    // Stack grows downward, so return top address
    uint64_t stack_top = (uint64_t)stack_base + USER_STACK_SIZE;

    // Align to 16 bytes (required by System V ABI)
    stack_top &= ~0xF;

    kprintf("[USERMODE] User stack allocated: 0x%016llX - 0x%016llX (size: %d KB)\n",
            (uint64_t)stack_base, stack_top, USER_STACK_SIZE / 1024);

    return stack_top;
}

/*
 * Allocate a kernel mode stack for TSS
 * This stack is used when transitioning from user to kernel mode
 */
static uint64_t allocate_kernel_stack(void) {
    // Allocate kernel stack
    void* stack_base = kmalloc(KERNEL_STACK_SIZE);
    if (!stack_base) {
        kprintf("[USERMODE] ERROR: Failed to allocate kernel stack\n");
        return 0;
    }

    // Stack grows downward, so return top address
    uint64_t stack_top = (uint64_t)stack_base + KERNEL_STACK_SIZE;

    // Align to 16 bytes
    stack_top &= ~0xF;

    kprintf("[USERMODE] Kernel stack allocated: 0x%016llX - 0x%016llX (size: %d KB)\n",
            (uint64_t)stack_base, stack_top, KERNEL_STACK_SIZE / 1024);

    return stack_top;
}

/*
 * Start user mode execution
 *
 * This function:
 * 1. Sets up TSS with kernel stack (for syscalls/interrupts)
 * 2. Switches to user mode using IRETQ
 *
 * After this call, we're in ring 3 with reduced privileges!
 *
 * WARNING: This function never returns in the normal sense.
 * Execution continues at 'entry' in user mode.
 *
 * @param entry  Entry point address (RIP)
 * @param stack  User stack pointer (RSP), or 0 to allocate one
 */
void start_usermode(uint64_t entry, uint64_t stack) {
    kprintf("\n");
    kprintf("[USERMODE] ==========================================\n");
    kprintf("[USERMODE] Transitioning to User Mode (Ring 3)\n");
    kprintf("[USERMODE] ==========================================\n");
    kprintf("[USERMODE] Entry point: 0x%016llX\n", entry);

    // If no stack provided, allocate one
    uint64_t user_stack = stack;
    if (!user_stack) {
        user_stack = allocate_user_stack();
        if (!user_stack) {
            kprintf("[USERMODE] FATAL: Failed to allocate user stack\n");
            return;
        }
    }

    // Allocate kernel stack for TSS
    uint64_t kernel_stack = allocate_kernel_stack();
    if (!kernel_stack) {
        kprintf("[USERMODE] FATAL: Failed to allocate kernel stack\n");
        return;
    }

    // Setup TSS with kernel stack
    // This stack will be used when syscalls or interrupts occur
    tss_set_kernel_stack(kernel_stack);
    kprintf("[USERMODE] TSS.RSP0 set to 0x%016llX\n", kernel_stack);

    kprintf("[USERMODE] Switching to user mode...\n");
    kprintf("[USERMODE] CPL will change: 0 (kernel) -> 3 (user)\n");
    kprintf("[USERMODE] Segments: CS=0x1B, DS/SS=0x23\n");
    kprintf("\n");

    // Point of no return! We're going to user mode.
    // This will execute IRETQ and never return here.
    enter_usermode(entry, user_stack, read_cr3());

    // Should never reach here
    kprintf("[USERMODE] ERROR: Returned from enter_usermode!\n");
}

/*
 * Verify CPL (Current Privilege Level)
 * For debugging: check if we're in ring 0 or ring 3
 */
uint8_t get_cpl(void) {
    uint16_t cs;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    return cs & 0x3;
}

// Trampoline for newly created processes.
// context_switch_asm does `ret` to here on first run.
// Reads user entry/stack/cr3 from the current process struct.
void process_enter_usermode_trampoline(void) {
    process_t* proc = process_get_current();
    if (!proc) {
        kernel_panic("Trampoline: no current process");
    }

    uint64_t entry = proc->user_entry;
    uint64_t stack = proc->user_rsp;
    uint64_t cr3 = proc->context.cr3;

    kprintf("[TRAMPOLINE] PID %d entering usermode: entry=0x%016lx stack=0x%016lx\n",
            proc->pid, entry, stack);

    uint64_t kstack_top = (uint64_t)proc->kernel_stack + KERNEL_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);

    extern uint64_t kernel_rsp_save;
    kernel_rsp_save = kstack_top;

#ifdef SCHED_DEBUG
    // DIAGNOSTIC: the first time the spawned-process trampoline runs, paint a
    // marker. This is the ring-0 step a fresh process executes right before its
    // IRETQ to ring 3. If this appears but "non-init proc RAN" does not, the
    // switch worked but enter_usermode (the IRETQ to ring 3) fails for spawned
    // processes. If it never appears, the scheduler never switched to the child.
    {
        static volatile int _tramp_seen = 0;
        if (!_tramp_seen) {
            _tramp_seen = 1;
            extern void framebuffer_puts_scaled(const char*, uint32_t, uint32_t,
                                                uint32_t, uint32_t);
            framebuffer_puts_scaled("trampoline RAN (ring0->ring3)", 40, 304, 0x0000FF00u, 2);
        }
    }
#endif

    enter_usermode(entry, stack, cr3);
}

// First-run trampoline for a THREAD (created via SYS_THREAD_CREATE).
// Mirrors process_enter_usermode_trampoline, but enters via enter_usermode_thread
// so the thread starts executing entry(arg) with RDI = arg (SysV first argument).
// context_switch_asm `ret`s into here on the thread's first dispatch, exactly as
// for a brand-new process (RESUME_CRETURN).
void thread_enter_usermode_trampoline(void) {
    process_t* t = process_get_current();
    if (!t) {
        kernel_panic("Thread trampoline: no current process");
    }

    uint64_t entry = t->user_entry;
    uint64_t stack = t->user_rsp;
    uint64_t cr3   = t->context.cr3;
    uint64_t arg   = t->thread_arg;

    kprintf("[THREAD] TID %d entering usermode: entry=0x%016lx stack=0x%016lx arg=0x%lx\n",
            t->pid, entry, stack, arg);

    uint64_t kstack_top = (uint64_t)t->kernel_stack + KERNEL_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);

    extern uint64_t kernel_rsp_save;
    kernel_rsp_save = kstack_top;

    extern void enter_usermode_thread(uint64_t entry, uint64_t stack,
                                      uint64_t cr3, uint64_t arg);
    enter_usermode_thread(entry, stack, cr3, arg);
}
