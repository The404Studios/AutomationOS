/**
 * ELF Loader Test Suite
 * ======================
 *
 * Tests for ELF64 loader functionality.
 */

#include "../include/kernel.h"
#include "../include/elf.h"
#include "../include/sched.h"
#include "../include/initrd.h"
#include "../include/mem.h"

/**
 * Test 1: Validate ELF header detection
 */
void test_elf_validate_header(void) {
    kprintf("[TEST] ELF header validation\n");

    // Test valid ELF header
    elf64_ehdr_t valid = {
        .e_ident = {
            0x7F, 'E', 'L', 'F',  // Magic
            ELFCLASS64,            // 64-bit
            ELFDATA2LSB,           // Little-endian
            EV_CURRENT,            // Current version
            0, 0, 0, 0, 0, 0, 0, 0, 0
        },
        .e_type = ET_EXEC,
        .e_machine = EM_X86_64,
        .e_version = EV_CURRENT,
        .e_entry = 0x401000,  // User space
    };

    if (elf_validate_header(&valid)) {
        kprintf("[TEST]   ✓ Valid ELF header accepted\n");
    } else {
        kprintf("[TEST]   ✗ Valid ELF header rejected\n");
    }

    // Test invalid magic
    elf64_ehdr_t invalid_magic = valid;
    invalid_magic.e_ident[0] = 0x42;

    if (!elf_validate_header(&invalid_magic)) {
        kprintf("[TEST]   ✓ Invalid magic rejected\n");
    } else {
        kprintf("[TEST]   ✗ Invalid magic accepted\n");
    }

    // Test wrong architecture
    elf64_ehdr_t wrong_arch = valid;
    wrong_arch.e_machine = EM_386;  // 32-bit x86

    if (!elf_validate_header(&wrong_arch)) {
        kprintf("[TEST]   ✓ Wrong architecture rejected\n");
    } else {
        kprintf("[TEST]   ✗ Wrong architecture accepted\n");
    }

    // Test kernel space entry point
    elf64_ehdr_t kernel_entry = valid;
    kernel_entry.e_entry = 0xFFFF800000000000ULL;

    if (!elf_validate_header(&kernel_entry)) {
        kprintf("[TEST]   ✓ Kernel space entry rejected\n");
    } else {
        kprintf("[TEST]   ✗ Kernel space entry accepted\n");
    }
}

/**
 * Test 2: Print ELF file info (if /init exists in initrd)
 */
void test_elf_print_info(void) {
    kprintf("[TEST] ELF file info\n");

    uint64_t size;
    void* data = initrd_get_file("init", &size);

    if (!data) {
        kprintf("[TEST]   ⚠ /init not found in initrd (skipping)\n");
        return;
    }

    elf_print_info("init");
}

/**
 * Test 3: Load ELF (dry run - doesn't execute)
 */
void test_elf_load(void) {
    kprintf("[TEST] ELF loading\n");

    uint64_t size;
    void* data = initrd_get_file("init", &size);

    if (!data) {
        kprintf("[TEST]   ⚠ /init not found in initrd (skipping)\n");
        return;
    }

    uint64_t entry = 0;
    uint64_t stack = 0;

    int ret = elf_load("init", 0, NULL, &entry, &stack);

    if (ret == ELF_SUCCESS) {
        kprintf("[TEST]   ✓ ELF loaded successfully\n");
        kprintf("[TEST]     Entry: 0x%016lx\n", entry);
        kprintf("[TEST]     Stack: 0x%016lx\n", stack);

        // Validate entry point is in user space
        if (entry < USER_SPACE_END) {
            kprintf("[TEST]   ✓ Entry point in user space\n");
        } else {
            kprintf("[TEST]   ✗ Entry point in kernel space\n");
        }

        // Validate stack is in user space and properly aligned
        if (stack < USER_SPACE_END && (stack & 0xF) == 0) {
            kprintf("[TEST]   ✓ Stack in user space and aligned\n");
        } else {
            kprintf("[TEST]   ✗ Stack invalid\n");
        }
    } else {
        kprintf("[TEST]   ✗ ELF load failed: error %d\n", ret);
    }
}

/**
 * Test 4: Create process from ELF
 */
void test_exec_create_process(void) {
    kprintf("[TEST] Process creation from ELF\n");

    uint64_t size;
    void* data = initrd_get_file("init", &size);

    if (!data) {
        kprintf("[TEST]   ⚠ /init not found in initrd (skipping)\n");
        return;
    }

    char* argv[] = { "init", NULL };
    process_t* proc = exec_create_process("init", "test_init", 1, argv);

    if (proc) {
        kprintf("[TEST]   ✓ Process created: PID=%d\n", proc->pid);
        kprintf("[TEST]     Name: %s\n", proc->name);
        kprintf("[TEST]     Entry: 0x%016lx\n", proc->context.rip);
        kprintf("[TEST]     Stack: 0x%016lx\n", proc->context.rsp);

        // Don't add to scheduler - this is just a test
        // Clean up (simplified - in real code would need to free pages)
        process_destroy(proc);

        kprintf("[TEST]   ✓ Process destroyed\n");
    } else {
        kprintf("[TEST]   ✗ Process creation failed\n");
    }
}

/**
 * Run all ELF loader tests
 */
void elf_run_tests(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  ELF Loader Test Suite\n");
    kprintf("========================================\n");
    kprintf("\n");

    test_elf_validate_header();
    kprintf("\n");

    test_elf_print_info();
    kprintf("\n");

    test_elf_load();
    kprintf("\n");

    test_exec_create_process();
    kprintf("\n");

    kprintf("========================================\n");
    kprintf("  ELF Loader Tests Complete\n");
    kprintf("========================================\n");
    kprintf("\n");
}
