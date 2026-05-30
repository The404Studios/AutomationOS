/**
 * User Mode Switcher - SYSRET variant (DISABLED)
 *
 * DISABLED: The canonical start_usermode() implementation is in
 * kernel/core/usermode.c, which delegates to enter_usermode() in
 * kernel/arch/x86_64/usermode.asm (IRETQ-based transition).
 *
 * This SYSRET-based alternative is kept for reference but guarded
 * to avoid duplicate symbol errors with kernel/core/usermode.c.
 *
 * To use the SYSRET version instead, define USE_SYSRET_USERMODE
 * and exclude kernel/core/usermode.c from the build.
 */

#ifdef USE_SYSRET_USERMODE

#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/sched.h"

// GDT segment selectors (defined in gdt.c)
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x18
#define USER_DS   0x20

/**
 * Enter user mode and jump to entry point
 *
 * This function sets up the user mode context and uses SYSRET
 * to transition to ring 3.
 *
 * @param entry Entry point address (RIP)
 * @param stack User stack pointer (RSP)
 */
void start_usermode(uint64_t entry, uint64_t stack) {
    kprintf("[USERMODE] Transitioning to user mode\n");
    kprintf("[USERMODE] Entry point: 0x%016lx\n", entry);
    kprintf("[USERMODE] Stack: 0x%016lx\n", stack);

    // Prepare for SYSRET:
    // - RCX will hold user RIP (entry point)
    // - R11 will hold user RFLAGS
    // - RSP will hold user stack pointer

    // RFLAGS for user mode:
    // - IF (bit 9) = 1: Enable interrupts
    // - IOPL (bits 12-13) = 0: No direct I/O access
    // - Bit 1 = 1: Reserved (must be 1)
    uint64_t rflags = (1 << 9) | (1 << 1);  // IF | Reserved

    // Switch to user mode using inline assembly
    asm volatile(
        // Set up segment registers for user mode
        "mov %0, %%ax\n"          // User data segment
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"

        // Prepare registers for SYSRET
        "mov %1, %%rcx\n"         // RCX = user RIP
        "mov %2, %%r11\n"         // R11 = user RFLAGS
        "mov %3, %%rsp\n"         // RSP = user stack

        // Clear all other registers for security
        "xor %%rax, %%rax\n"
        "xor %%rbx, %%rbx\n"
        "xor %%rdx, %%rdx\n"
        "xor %%rsi, %%rsi\n"
        "xor %%rdi, %%rdi\n"
        "xor %%rbp, %%rbp\n"
        "xor %%r8, %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "xor %%r12, %%r12\n"
        "xor %%r13, %%r13\n"
        "xor %%r14, %%r14\n"
        "xor %%r15, %%r15\n"

        // Jump to user mode using SYSRET
        // Note: We use a trampoline because SYSRET is complex
        "sysretq\n"
        :
        : "i"(USER_DS | 3),       // User data segment with RPL=3
          "r"(entry),              // Entry point
          "r"(rflags),             // RFLAGS
          "r"(stack)               // Stack pointer
        : "memory", "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
    );

    // Should never reach here
    kernel_panic("SYSRET returned to kernel!");
}

/**
 * Alternative: Enter user mode using IRETQ
 *
 * This is a more compatible method that works even if SYSCALL/SYSRET
 * is not properly configured.
 *
 * @param entry Entry point address
 * @param stack User stack pointer
 */
void start_usermode_iretq(uint64_t entry, uint64_t stack) {
    kprintf("[USERMODE] Using IRETQ to enter user mode\n");
    kprintf("[USERMODE] Entry: 0x%016lx Stack: 0x%016lx\n", entry, stack);

    // IRETQ expects stack to contain:
    // [RSP+0]  = RIP (entry point)
    // [RSP+8]  = CS (user code segment)
    // [RSP+16] = RFLAGS
    // [RSP+24] = RSP (user stack)
    // [RSP+32] = SS (user data segment)

    uint64_t rflags = (1 << 9) | (1 << 1);  // IF | Reserved

    asm volatile(
        // Push IRETQ frame onto kernel stack
        "push %0\n"               // SS (user data segment)
        "push %1\n"               // RSP (user stack)
        "push %2\n"               // RFLAGS
        "push %3\n"               // CS (user code segment)
        "push %4\n"               // RIP (entry point)

        // Set up user data segments
        "mov %5, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"

        // Clear registers
        "xor %%rax, %%rax\n"
        "xor %%rbx, %%rbx\n"
        "xor %%rcx, %%rcx\n"
        "xor %%rdx, %%rdx\n"
        "xor %%rsi, %%rsi\n"
        "xor %%rdi, %%rdi\n"
        "xor %%rbp, %%rbp\n"
        "xor %%r8, %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "xor %%r11, %%r11\n"
        "xor %%r12, %%r12\n"
        "xor %%r13, %%r13\n"
        "xor %%r14, %%r14\n"
        "xor %%r15, %%r15\n"

        // Return to user mode
        "iretq\n"
        :
        : "r"((uint64_t)(USER_DS | 3)),   // SS
          "r"(stack),                      // RSP
          "r"(rflags),                     // RFLAGS
          "r"((uint64_t)(USER_CS | 3)),   // CS
          "r"(entry),                      // RIP
          "r"((uint64_t)(USER_DS | 3))    // DS/ES/FS/GS
        : "memory"
    );

    // Should never reach here
    kernel_panic("IRETQ returned to kernel!");
}

#endif /* USE_SYSRET_USERMODE */
