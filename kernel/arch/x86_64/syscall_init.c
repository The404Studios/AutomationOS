// syscall_init.c - Initialize SYSCALL/SYSRET MSRs for x86_64
// This configures the CPU to handle the SYSCALL instruction correctly

#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/syscall.h"

// External syscall entry point from syscall.asm
extern void syscall_entry(void);

void syscall_msr_init(void) {
    kprintf("[SYSCALL] Initializing SYSCALL/SYSRET MSRs...\n");

    // Enable SCE (System Call Enable) in EFER MSR
    uint64_t efer = rdmsr(0xC0000080);  // IA32_EFER
    efer |= 1;  // Bit 0 = SCE
    wrmsr(0xC0000080, efer);
    kprintf("[SYSCALL]   EFER.SCE enabled\n");

    // IA32_STAR: Configure code segment selectors for SYSCALL/SYSRET
    // Bits 32-47: Kernel CS (0x08)
    // Bits 48-63: User CS base (0x18)
    //   SYSRET loads CS from STAR[48:63]
    //   SYSRET loads SS from STAR[48:63] + 8
    // Layout:
    //   Kernel CS = 0x08 (GDT entry 1)
    //   User CS   = 0x18 (GDT entry 3, with RPL=3 it becomes 0x1B)
    //   User SS   = 0x20 (GDT entry 4, with RPL=3 it becomes 0x23)
    // SYSRET: CS = STAR[63:48]+16 = 0x10+16 = 0x20 (user code), SS = 0x10+8 = 0x18 (user data)
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);
    kprintf("[SYSCALL]   IA32_STAR  = 0x%016llX (Kernel CS=0x08, SYSRET base=0x10)\n", star);

    // IA32_LSTAR: Set syscall entry point address
    uint64_t lstar = (uint64_t)syscall_entry;
    wrmsr(MSR_LSTAR, lstar);
    kprintf("[SYSCALL]   IA32_LSTAR = 0x%016llX (syscall_entry)\n", lstar);

    // IA32_FMASK: Mask RFLAGS bits during syscall
    // Bit 9 (IF) = Interrupt Enable Flag - clear to disable interrupts during syscall
    // This ensures interrupts are disabled when entering the kernel via syscall
    uint64_t fmask = 0x200;  // Clear IF (bit 9)
    wrmsr(MSR_FMASK, fmask);
    kprintf("[SYSCALL]   IA32_FMASK = 0x%016llX (clear IF)\n", fmask);

    kprintf("[SYSCALL] SYSCALL/SYSRET MSRs initialized successfully\n");
}
