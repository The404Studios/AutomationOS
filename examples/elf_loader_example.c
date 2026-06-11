/**
 * ELF Loader Integration Example
 * ===============================
 *
 * Minimal example showing how to integrate the ELF loader into kernel_main().
 * This demonstrates the three lines of code needed to launch /init.
 */

#include "../kernel/include/kernel.h"
#include "../kernel/include/elf.h"
#include "../kernel/include/sched.h"
#include "../kernel/include/mem.h"
#include "../kernel/include/initrd.h"

/**
 * Example 1: Minimal Integration (3 lines)
 *
 * This is the absolute minimum needed to launch /init.
 */
void example_minimal_integration(void) {
    // Assume all initialization is done (PMM, VMM, GDT, etc.)

    // Launch init process
    if (exec_launch_init() != 0) {
        kernel_panic("Failed to launch /init");
    }

    // Start scheduler (never returns)
    schedule();
}

/**
 * Example 2: Complete Boot Sequence
 *
 * Full kernel_main() showing all initialization steps.
 */
void example_complete_boot(uint64_t multiboot_addr) {
    // 1. Early initialization
    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   AutomationOS Kernel Boot\n");
    kprintf("=====================================\n");
    kprintf("\n");

    // 2. Memory management
    kprintf("[BOOT] Initializing memory management...\n");
    // pmm_init(memory_map, map_count);  // From multiboot
    // vmm_init();
    // heap_init();

    uint64_t free_mem = 128 * 1024 * 1024;  // Example: 128 MB
    kprintf("[BOOT] Free memory: %lu MB\n", free_mem / (1024*1024));

    // 3. CPU features
    kprintf("[BOOT] Initializing CPU features...\n");
    // gdt_init();
    // idt_init();

    // 4. Process management
    kprintf("[BOOT] Initializing process management...\n");
    // process_init();
    // scheduler_init();

    // 5. Load initrd
    kprintf("[BOOT] Loading initrd...\n");
    uint64_t initrd_addr = 0;  // From multiboot
    uint64_t initrd_size = 0;  // From multiboot

    // initrd_init(initrd_addr, initrd_size);
    // initrd_mount();
    // initrd_list_files();

    // 6. Launch init
    kprintf("\n");
    kprintf("=== Launching /init ===\n");
    kprintf("\n");

    if (exec_launch_init() != 0) {
        kernel_panic("Failed to launch /init");
    }

    // 7. Enable interrupts and start scheduling
    kprintf("[BOOT] Starting scheduler...\n");
    // sti();
    schedule();

    // Should never reach here
    kernel_panic("Scheduler returned");
}

/**
 * Example 3: With Error Checking
 *
 * Shows proper error handling for production use.
 */
void example_with_error_checking(void) {
    // Check prerequisites
    uint64_t free_mem = pmm_get_free_memory();
    if (free_mem < 8 * 1024 * 1024) {
        kprintf("[ERROR] Not enough free memory (%lu MB)\n",
                free_mem / (1024*1024));
        kernel_panic("Insufficient memory for user stack");
    }

    // Verify initrd is mounted
    uint64_t num_files, total_size;
    initrd_get_stats(&num_files, &total_size);
    if (num_files == 0) {
        kprintf("[ERROR] Initrd is empty or not mounted\n");
        kernel_panic("No initrd");
    }

    kprintf("[BOOT] Initrd: %lu files, %lu bytes\n", num_files, total_size);

    // Check if /init exists
    uint64_t init_size;
    void* init_data = initrd_get_file("init", &init_size);
    if (!init_data) {
        kprintf("[ERROR] /init not found in initrd\n");
        kprintf("[ERROR] Available files:\n");
        initrd_list_files();
        kernel_panic("Missing /init");
    }

    kprintf("[BOOT] Found /init (%lu bytes)\n", init_size);

    // Launch init
    kprintf("\n=== Launching /init ===\n\n");

    int ret = exec_launch_init();
    if (ret != 0) {
        kprintf("[ERROR] exec_launch_init() failed: %d\n", ret);
        kernel_panic("Failed to launch /init");
    }

    kprintf("[BOOT] Init process created, starting scheduler...\n");

    // Start scheduler
    schedule();

    kernel_panic("Scheduler returned");
}

/**
 * Example 4: Manual Process Creation
 *
 * Shows how to manually create a process instead of using exec_launch_init().
 */
void example_manual_process_creation(void) {
    kprintf("[BOOT] Creating init process manually...\n");

    // Create process with arguments
    char* argv[] = { "init", "--verbose", NULL };
    process_t* init_proc = exec_create_process("init", "init", 2, argv);

    if (!init_proc) {
        kprintf("[ERROR] Failed to create process\n");
        kernel_panic("exec_create_process failed");
    }

    kprintf("[BOOT] Process created: PID=%d\n", init_proc->pid);
    kprintf("[BOOT]   Name: %s\n", init_proc->name);
    kprintf("[BOOT]   Entry: 0x%016lx\n", init_proc->context.rip);
    kprintf("[BOOT]   Stack: 0x%016lx\n", init_proc->context.rsp);

    // Add to scheduler
    scheduler_add_process(init_proc);

    kprintf("[BOOT] Process added to scheduler\n");

    // Start scheduling
    schedule();
}

/**
 * Example 5: Testing Before Launch
 *
 * Shows how to test the ELF loader before actually launching init.
 */
void example_test_before_launch(void) {
    kprintf("\n");
    kprintf("=== ELF Loader Tests ===\n");
    kprintf("\n");

    // Run test suite
    elf_run_tests();

    kprintf("\n");
    kprintf("=== Tests Complete ===\n");
    kprintf("\n");

    // Now launch for real
    kprintf("Launching /init...\n");

    if (exec_launch_init() != 0) {
        kernel_panic("Failed to launch /init");
    }

    schedule();
}

/**
 * Example 6: Direct User Mode Jump (No Scheduler)
 *
 * Shows how to jump directly to user mode without using the scheduler.
 * This is useful for single-process systems or testing.
 */
void example_direct_usermode_jump(void) {
    kprintf("[BOOT] Jumping directly to user mode...\n");

    // This function does NOT return
    exec_usermode("init", 0, NULL);

    // Never reached
}

/**
 * Example 7: Load Multiple Processes
 *
 * Shows how to load multiple user processes before starting the scheduler.
 */
void example_multiple_processes(void) {
    kprintf("[BOOT] Creating multiple processes...\n");

    // Create init
    char* init_argv[] = { "init", NULL };
    process_t* init = exec_create_process("init", "init", 1, init_argv);
    if (init) {
        scheduler_add_process(init);
        kprintf("[BOOT] Created init (PID %d)\n", init->pid);
    }

    // Create shell (if it exists)
    char* shell_argv[] = { "shell", NULL };
    process_t* shell = exec_create_process("shell", "shell", 1, shell_argv);
    if (shell) {
        scheduler_add_process(shell);
        kprintf("[BOOT] Created shell (PID %d)\n", shell->pid);
    }

    // Create background daemon (if it exists)
    char* daemon_argv[] = { "daemon", NULL };
    process_t* daemon = exec_create_process("daemon", "daemon", 1, daemon_argv);
    if (daemon) {
        scheduler_add_process(daemon);
        kprintf("[BOOT] Created daemon (PID %d)\n", daemon->pid);
    }

    kprintf("[BOOT] Starting scheduler with %d processes...\n",
            init ? 1 : 0 + shell ? 1 : 0 + daemon ? 1 : 0);

    schedule();
}

/**
 * Example 8: ELF Information Debugging
 *
 * Shows how to print detailed ELF information for debugging.
 */
void example_elf_debugging(void) {
    kprintf("\n");
    kprintf("=== ELF Information ===\n");
    kprintf("\n");

    // Print info for init
    elf_print_info("init");

    kprintf("\n");

    // Try to load (but don't execute)
    uint64_t entry, stack;
    int ret = elf_load("init", 0, NULL, &entry, &stack);

    if (ret == ELF_SUCCESS) {
        kprintf("✓ Load successful\n");
        kprintf("  Entry point: 0x%016lx\n", entry);
        kprintf("  Stack pointer: 0x%016lx\n", stack);
        kprintf("  Stack alignment: %s\n",
                (stack & 0xF) ? "BAD" : "OK");

        // Check if entry is in user space
        if (entry < USER_SPACE_END) {
            kprintf("  Entry in user space: ✓\n");
        } else {
            kprintf("  Entry in kernel space: ✗\n");
        }
    } else {
        kprintf("✗ Load failed: error %d\n", ret);
    }

    kprintf("\n");
}

/**
 * Recommended Integration (Copy This!)
 *
 * This is the recommended way to integrate the ELF loader.
 * Copy this into your kernel_main().
 */
void recommended_integration(void) {
    /*
     * Add this to your kernel_main() after all initialization:
     */

    // Optional: Run tests first
    #ifdef DEBUG_ELF_LOADER
    kprintf("\n=== ELF Loader Tests ===\n");
    elf_run_tests();
    #endif

    // Launch init
    kprintf("\n=== Launching Init Process ===\n\n");

    if (exec_launch_init() != 0) {
        kernel_panic("Failed to launch /init");
    }

    // Start scheduler
    kprintf("[KERNEL] Starting scheduler...\n");
    schedule();  // Never returns

    // Should never reach here
    kernel_panic("Scheduler returned");
}
