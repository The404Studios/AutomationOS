/*
 * Enhanced Kernel Panic Handler
 * ==============================
 *
 * Comprehensive panic handler with register dumps, stack traces,
 * memory dumps, and system state diagnostics for effective debugging.
 */

#include "../include/kernel.h"
#include "../include/x86_64.h"

/* Global errno variable for kernel and compat code */
int errno = 0;

/* Maximum stack frames to trace */
#define MAX_STACK_FRAMES 16

/* Maximum memory dump lines around fault address */
#define MEMORY_DUMP_LINES 4

/* Page fault error code flags */
#define PF_PRESENT   (1 << 0)  /* Page was present (protection violation) */
#define PF_WRITE     (1 << 1)  /* Write access */
#define PF_USER      (1 << 2)  /* User-mode access */
#define PF_RESERVED  (1 << 3)  /* Reserved bit violation */
#define PF_INSTR     (1 << 4)  /* Instruction fetch */

/*
 * Decode and display exception details
 * Analyzes CR2 (fault address) and error code to provide human-readable
 * diagnostic information about the fault type and cause
 *
 * This function provides enhanced diagnostics for page faults by decoding
 * the error code bits and providing actionable diagnostic information.
 */
void decode_exception(uint64_t cr2, uint64_t error_code) {
    kprintf("Exception decoder analysis:\n");

    /* Analyze fault address */
    if (cr2 == 0 || cr2 < 0x1000) {
        kprintf("  \033[1;31m[CRITICAL]\033[0m Null pointer dereference at 0x%016llx\n", cr2);
    } else if (cr2 >= 0xFFFF800000000000ULL) {
        kprintf("  Fault address: 0x%016llx (kernel space)\n", cr2);
    } else {
        kprintf("  Fault address: 0x%016llx (user space)\n", cr2);
    }

    /* Decode error code bits */
    kprintf("  Error code: 0x%llx [", error_code);

    if (error_code & PF_PRESENT) {
        kprintf("PROTECTION-VIOLATION ");
    } else {
        kprintf("NOT-PRESENT ");
    }

    if (error_code & PF_WRITE) {
        kprintf("WRITE ");
    } else {
        kprintf("READ ");
    }

    if (error_code & PF_USER) {
        kprintf("USER-MODE ");
    } else {
        kprintf("SUPERVISOR ");
    }

    if (error_code & PF_RESERVED) {
        kprintf("RESERVED-BIT ");
    }

    if (error_code & PF_INSTR) {
        kprintf("INSTRUCTION-FETCH ");
    }

    kprintf("]\n");

    /* Provide specific diagnostic messages */
    if (!(error_code & PF_PRESENT)) {
        kprintf("  \033[1;33m[DIAGNOSIS]\033[0m Page not present in page table\n");
        kprintf("              Possible causes: unmapped memory, demand paging needed, or use-after-free\n");
    } else if (error_code & PF_WRITE) {
        kprintf("  \033[1;33m[DIAGNOSIS]\033[0m Write to read-only page at 0x%016llx\n", cr2);
        kprintf("              Possible causes: const/rodata modification, COW fault, or protection error\n");
    } else if (error_code & PF_INSTR) {
        kprintf("  \033[1;33m[DIAGNOSIS]\033[0m Instruction fetch from NX (no-execute) page at 0x%016llx\n", cr2);
        kprintf("              Possible causes: stack/heap execution, DEP violation, or JIT issue\n");
    } else if (error_code & PF_USER) {
        kprintf("  \033[1;33m[DIAGNOSIS]\033[0m User-mode access to supervisor page at 0x%016llx\n", cr2);
        kprintf("              Possible causes: improper page table flags or privilege escalation attempt\n");
    } else if (error_code & PF_RESERVED) {
        kprintf("  \033[1;31m[CRITICAL]\033[0m Reserved bit set in page table entry\n");
        kprintf("              This indicates page table corruption\n");
    }

    kprintf("\n");
}

/*
 * Print stack trace by walking frame pointers
 * Attempts to unwind the call stack for debugging
 */
static void print_stack_trace(uint64_t rbp) {
    kprintf("Stack trace:\n");

    int frame = 0;
    uint64_t prev_rbp = 0;

    while (rbp && frame < MAX_STACK_FRAMES) {
        // Validate frame pointer (must be in kernel space and aligned)
        if (rbp < 0xFFFF800000000000ULL || (rbp & 0x7) != 0) {
            kprintf("  [%d] <invalid frame pointer: 0x%016llx>\n", frame, rbp);
            break;
        }

        // Prevent infinite loops
        if (rbp <= prev_rbp) {
            kprintf("  [%d] <frame pointer not increasing>\n", frame);
            break;
        }

        uint64_t rip = *(uint64_t*)(rbp + 8);
        uint64_t next_rbp = *(uint64_t*)rbp;

        kprintf("  [%d] RIP: 0x%016llx  RBP: 0x%016llx\n", frame, rip, rbp);

        prev_rbp = rbp;
        rbp = next_rbp;
        frame++;
    }

    if (frame == 0) {
        kprintf("  <no valid frames found>\n");
    } else if (frame == MAX_STACK_FRAMES) {
        kprintf("  <trace truncated at %d frames>\n", MAX_STACK_FRAMES);
    }
}

/*
 * Dump CPU registers for debugging
 * Captures all general-purpose and control registers at panic time
 */
static void print_registers(void) {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags, cr0, cr2, cr3, cr4;

    // Read general-purpose registers
    asm volatile("mov %%rax, %0" : "=r"(rax));
    asm volatile("mov %%rbx, %0" : "=r"(rbx));
    asm volatile("mov %%rcx, %0" : "=r"(rcx));
    asm volatile("mov %%rdx, %0" : "=r"(rdx));
    asm volatile("mov %%rsi, %0" : "=r"(rsi));
    asm volatile("mov %%rdi, %0" : "=r"(rdi));
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    asm volatile("mov %%r8, %0" : "=r"(r8));
    asm volatile("mov %%r9, %0" : "=r"(r9));
    asm volatile("mov %%r10, %0" : "=r"(r10));
    asm volatile("mov %%r11, %0" : "=r"(r11));
    asm volatile("mov %%r12, %0" : "=r"(r12));
    asm volatile("mov %%r13, %0" : "=r"(r13));
    asm volatile("mov %%r14, %0" : "=r"(r14));
    asm volatile("mov %%r15, %0" : "=r"(r15));

    // Read control registers
    asm volatile("mov %%cr0, %%rax; mov %%rax, %0" : "=r"(cr0) :: "rax");
    asm volatile("mov %%cr2, %%rax; mov %%rax, %0" : "=r"(cr2) :: "rax");
    asm volatile("mov %%cr3, %%rax; mov %%rax, %0" : "=r"(cr3) :: "rax");
    asm volatile("mov %%cr4, %%rax; mov %%rax, %0" : "=r"(cr4) :: "rax");
    asm volatile("pushfq; pop %0" : "=r"(rflags));

    kprintf("Register dump:\n");
    kprintf("  RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx  RDX: 0x%016llx\n",
            rax, rbx, rcx, rdx);
    kprintf("  RSI: 0x%016llx  RDI: 0x%016llx  RBP: 0x%016llx  RSP: 0x%016llx\n",
            rsi, rdi, rbp, rsp);
    kprintf("  R8:  0x%016llx  R9:  0x%016llx  R10: 0x%016llx  R11: 0x%016llx\n",
            r8, r9, r10, r11);
    kprintf("  R12: 0x%016llx  R13: 0x%016llx  R14: 0x%016llx  R15: 0x%016llx\n",
            r12, r13, r14, r15);
    kprintf("\n");
    kprintf("  RFLAGS: 0x%016llx [", rflags);
    if (rflags & (1 << 0)) kprintf("CF ");
    if (rflags & (1 << 2)) kprintf("PF ");
    if (rflags & (1 << 4)) kprintf("AF ");
    if (rflags & (1 << 6)) kprintf("ZF ");
    if (rflags & (1 << 7)) kprintf("SF ");
    if (rflags & (1 << 8)) kprintf("TF ");
    if (rflags & (1 << 9)) kprintf("IF ");
    if (rflags & (1 << 10)) kprintf("DF ");
    if (rflags & (1 << 11)) kprintf("OF ");
    kprintf("]\n");
    kprintf("\n");
    kprintf("  CR0: 0x%016llx [", cr0);
    if (cr0 & (1 << 0)) kprintf("PE ");
    if (cr0 & (1 << 1)) kprintf("MP ");
    if (cr0 & (1 << 2)) kprintf("EM ");
    if (cr0 & (1 << 3)) kprintf("TS ");
    if (cr0 & (1 << 4)) kprintf("ET ");
    if (cr0 & (1 << 5)) kprintf("NE ");
    if (cr0 & (1 << 16)) kprintf("WP ");
    if (cr0 & (1 << 31)) kprintf("PG ");
    kprintf("]\n");
    kprintf("  CR2: 0x%016llx (page fault address)\n", cr2);
    kprintf("  CR3: 0x%016llx (page table base, PCID=%llu)\n", cr3, cr3 & 0xFFF);
    kprintf("  CR4: 0x%016llx [", cr4);
    if (cr4 & (1 << 5)) kprintf("PAE ");
    if (cr4 & (1 << 7)) kprintf("PGE ");
    if (cr4 & (1 << 17)) kprintf("PCIDE ");
    kprintf("]\n");
}

/*
 * Dump memory around a fault address
 * Shows hex dump of memory for debugging pointer/access violations
 */
static void print_memory_dump(uint64_t addr) {
    kprintf("Memory dump around 0x%016llx:\n", addr);

    // Align to 16-byte boundary
    uint64_t aligned_addr = addr & ~0xFULL;

    // Dump a few lines before and after
    for (int i = -MEMORY_DUMP_LINES; i <= MEMORY_DUMP_LINES; i++) {
        uint64_t line_addr = aligned_addr + (i * 16);

        kprintf("  0x%016llx: ", line_addr);

        // Check if address is in kernel space (simple check)
        if (line_addr >= 0xFFFF800000000000ULL) {
            // Attempt to read (may still fault on unmapped pages)
            for (int j = 0; j < 16; j++) {
                uint8_t byte = *(uint8_t*)(line_addr + j);
                kprintf("%02x ", byte);
            }
            kprintf(" |");
            // Print ASCII if printable
            for (int j = 0; j < 16; j++) {
                uint8_t byte = *(uint8_t*)(line_addr + j);
                if (byte >= 32 && byte < 127) {
                    kprintf("%c", byte);
                } else {
                    kprintf(".");
                }
            }
            kprintf("|");
        } else {
            kprintf("?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??  |user space|");
        }

        // Mark the actual fault address line
        if (addr >= line_addr && addr < line_addr + 16) {
            kprintf(" <--");
        }

        kprintf("\n");
    }
}

/*
 * Main kernel panic handler
 * Displays comprehensive diagnostic information and halts the system
 */
void kernel_panic(const char* message) {
    // Disable interrupts - we're in critical failure mode
    cli();

    kprintf("\n\n");
    kprintf("\033[1;31m"); /* Red bold */
    kprintf("╔════════════════════════════════════════╗\n");
    kprintf("║  KERNEL PANIC - SYSTEM HALTED          ║\n");
    kprintf("╚════════════════════════════════════════╝\n");
    kprintf("\033[0m"); /* Reset */
    kprintf("\n");

    kprintf("\033[1mError:\033[0m %s\n", message);
    kprintf("\n");

    // Get current RBP for stack trace
    uint64_t rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));

    print_stack_trace(rbp);
    kprintf("\n");

    print_registers();
    kprintf("\n");

    // If CR2 is non-zero, this may be a page fault - dump memory
    uint64_t cr2;
    asm volatile("mov %%cr2, %%rax; mov %%rax, %0" : "=r"(cr2) :: "rax");
    if (cr2 != 0 && cr2 >= 0xFFFF800000000000ULL) {
        print_memory_dump(cr2);
        kprintf("\n");
    }

    kprintf("System information:\n");
    kprintf("  Kernel version: %d.%d.%d\n",
            KERNEL_VERSION_MAJOR, KERNEL_VERSION_MINOR, KERNEL_VERSION_PATCH);
    kprintf("\n");

    /* Try to sync filesystems before halt (best effort) */
    kprintf("Attempting emergency filesystem sync...\n");
    extern int vfs_sync_all(void);
    if (vfs_sync_all() == 0) {
        kprintf("  \033[32m✓\033[0m Filesystems synced successfully\n");
    } else {
        kprintf("  \033[33m!\033[0m Filesystem sync failed or not available\n");
    }
    kprintf("\n");

    kprintf("\033[1;31m");
    kprintf("════════════════════════════════════════════════════════════\n");
    kprintf("  System halted. Please reboot.\n");
    kprintf("════════════════════════════════════════════════════════════\n");
    kprintf("\033[0m");

    // Halt all CPUs (TODO: send IPI to halt other CPUs in SMP)
    while (1) {
        cli();
        hlt();
    }
}

/*
 * Assertion failure handler
 * Called when ASSERT() macro fails, provides file/line context
 */
void assert_failed(const char* expr, const char* file, int line) {
    char buf[256];
    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("                           ASSERTION FAILED                                    \n");
    kprintf("================================================================================\n");
    kprintf("  Expression: %s\n", expr);
    kprintf("  File: %s\n", file);
    kprintf("  Line: %d\n", line);
    kprintf("================================================================================\n");
    kprintf("\n");

    // Format message for main panic handler
    kprintf("ASSERTION FAILED: %s at %s:%d", expr, file, line);

    // Call main panic handler for full diagnostics
    kernel_panic("Assertion failure (see above for details)");
}
