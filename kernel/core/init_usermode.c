/*
 * init_usermode.c - Initialize and test user mode
 * ================================================
 *
 * This file provides initialization and testing for user mode support.
 * It should be called from kernel_main after all subsystems are initialized.
 */

#include "../include/kernel.h"
#include "../include/usermode.h"
#include "../include/x86_64.h"

// External test function from test_usermode.c
extern uint64_t get_usermode_test_entry(void);

/*
 * Initialize user mode support
 * Should be called after GDT is initialized
 */
void init_usermode_support(void) {
    kprintf("\n");
    kprintf("[INIT] ========================================\n");
    kprintf("[INIT] Initializing User Mode Support\n");
    kprintf("[INIT] ========================================\n");

    // Initialize TSS (Task State Segment)
    // This must be done before any transition to user mode
    tss_init();

    kprintf("[INIT] User mode support initialized\n");
    kprintf("[INIT] Ready to switch to ring 3\n");
}

/*
 * Test user mode by executing a simple test program
 * This function will:
 * 1. Get the test program entry point
 * 2. Verify we're in ring 0
 * 3. Switch to ring 3 and execute the test program
 * 4. Never return (execution continues in user mode)
 */
void test_usermode_switch(void) {
    kprintf("\n");
    kprintf("[TEST] ========================================\n");
    kprintf("[TEST] User Mode Switch Test\n");
    kprintf("[TEST] ========================================\n");

    // Verify we're in kernel mode
    uint8_t cpl = get_cpl();
    kprintf("[TEST] Current privilege level: %d (expected 0)\n", cpl);

    if (cpl != 0) {
        kprintf("[TEST] ERROR: Not in kernel mode!\n");
        return;
    }

    // Get test program entry point
    uint64_t entry = get_usermode_test_entry();
    kprintf("[TEST] Test program entry point: 0x%016llX\n", entry);

    if (!entry) {
        kprintf("[TEST] ERROR: Invalid entry point!\n");
        return;
    }

    kprintf("[TEST] Starting user mode test program...\n");
    kprintf("[TEST] This will switch from ring 0 to ring 3\n");
    kprintf("[TEST] Test program will execute syscalls to verify transition\n");
    kprintf("\n");

    // This never returns - execution continues in user mode
    // Pass 0 for stack to let start_usermode allocate one
    start_usermode(entry, 0);

    // Should never reach here
    kprintf("[TEST] ERROR: Returned from start_usermode!\n");
}
