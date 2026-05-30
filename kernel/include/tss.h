#ifndef TSS_H
#define TSS_H

#include "types.h"

/*
 * Task State Segment (TSS) for x86_64
 * ====================================
 *
 * In x86_64, the TSS is primarily used for privilege level transitions.
 * When a syscall or interrupt occurs and the CPU transitions from ring 3 (user)
 * to ring 0 (kernel), the CPU loads RSP from TSS.RSP0 as the kernel stack.
 *
 * This is critical for security: user code cannot control the kernel stack pointer.
 */

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;          // Kernel stack pointer for ring 0
    uint64_t rsp1;          // Stack pointer for ring 1 (unused)
    uint64_t rsp2;          // Stack pointer for ring 2 (unused)
    uint64_t reserved1;
    uint64_t ist1;          // Interrupt Stack Table 1 (optional)
    uint64_t ist2;          // Interrupt Stack Table 2 (optional)
    uint64_t ist3;          // Interrupt Stack Table 3 (optional)
    uint64_t ist4;          // Interrupt Stack Table 4 (optional)
    uint64_t ist5;          // Interrupt Stack Table 5 (optional)
    uint64_t ist6;          // Interrupt Stack Table 6 (optional)
    uint64_t ist7;          // Interrupt Stack Table 7 (optional)
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    // I/O permission bitmap base (unused)
} PACKED tss_t;

// TSS management functions
void tss_init(void);
void tss_set_kernel_stack(uint64_t stack);
tss_t* tss_get(void);

#endif
