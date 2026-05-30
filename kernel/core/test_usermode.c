/*
 * test_usermode.c - Simple user mode test program
 * ===============================================
 *
 * This is a minimal test program that runs in user mode (ring 3).
 * It will be loaded into memory and executed by the kernel.
 *
 * Test plan:
 * 1. Verify we're in ring 3 (CPL=3)
 * 2. Try to execute a syscall
 * 3. Print "Hello from userspace!"
 */

#include "../include/kernel.h"
#include "../include/x86_64.h"

/*
 * This function will be called from user mode
 * It's the entry point for our test userspace program
 */
void usermode_test_entry(void) {
    // Test 1: Print message via sys_write (syscall 3)
    // Syscall number in RAX, arguments in RDI, RSI, RDX
    // This should transition to kernel mode and back

    const char* msg = "Hello from userspace! (Ring 3)\n";

    // Calculate message length
    uint64_t len = 0;
    while (msg[len] != '\0') {
        len++;
    }

    // Execute sys_write: write(1, msg, len)
    asm volatile(
        "mov $3, %%rax\n"     // Syscall 3 (sys_write)
        "mov $1, %%rdi\n"     // Arg 1: fd = 1 (stdout)
        "mov %0, %%rsi\n"     // Arg 2: buffer pointer
        "mov %1, %%rdx\n"     // Arg 3: count
        "syscall\n"
        :
        : "r"(msg), "r"(len)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );

    // Test 2: Try to get PID (syscall 8)
    uint64_t pid = 0;
    asm volatile(
        "mov $8, %%rax\n"     // Syscall 8 (sys_getpid)
        "syscall\n"
        "mov %%rax, %0\n"     // Save return value (PID)
        : "=r"(pid)
        :
        : "rax", "rcx", "r11", "memory"
    );

    // Loop forever (user mode idle)
    while (1) {
        // Busy loop instead of HLT (HLT requires privileges)
        // In a real system, we'd use a proper yield syscall
        for (volatile int i = 0; i < 1000000; i++) {
            // Spin
        }
    }
}

/*
 * Get the entry point address
 * This is used by the kernel to know where to jump
 */
uint64_t get_usermode_test_entry(void) {
    return (uint64_t)usermode_test_entry;
}
