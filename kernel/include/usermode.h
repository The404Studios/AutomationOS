#ifndef USERMODE_H
#define USERMODE_H

#include "types.h"

/*
 * User Mode Support
 * =================
 *
 * Functions for transitioning from kernel mode (ring 0) to user mode (ring 3).
 *
 * Usage:
 *   1. Call tss_init() to setup Task State Segment
 *   2. Call start_usermode(entry_point, stack_ptr) to switch to user mode
 *   3. User code executes at entry_point with reduced privileges
 *   4. Syscalls return to kernel mode automatically (via TSS)
 */

// Initialize TSS (must be called before start_usermode)
void tss_init(void);

// Start user mode at given entry point with given stack (never returns)
// This is a C wrapper that calls enter_usermode (assembly)
void start_usermode(uint64_t entry, uint64_t stack);

// Assembly function: enter user mode via IRETQ (defined in usermode.asm)
// RDI = entry point, RSI = user stack pointer, RDX = process CR3
// EXECVE-INPLACE-0 (decision 9.5): noreturn -- enter_usermode zeroes the GP regs,
// loads CR3, and IRETQs to ring 3; it never returns. Marking it lets GCC treat
// sys_execve's commit tail-call as terminal (no fall-through codegen / warning).
extern void enter_usermode(uint64_t entry, uint64_t stack, uint64_t cr3)
    __attribute__((noreturn));

// Get current privilege level (0 = kernel, 3 = user)
uint8_t get_cpl(void);

// Set kernel stack in TSS (used for syscall returns)
void tss_set_kernel_stack(uint64_t stack);

#endif
