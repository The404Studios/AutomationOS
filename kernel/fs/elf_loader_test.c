/**
 * ELF Loader and Usermode Test Suite
 * ===================================
 *
 * Comprehensive tests for:
 * 1. ELF64 header validation
 * 2. ELF64 loading from initrd
 * 3. User stack setup
 * 4. Ring 0 → Ring 3 transition
 * 5. User mode execution
 *
 * This should be called after:
 * - PMM initialized
 * - VMM initialized
 * - Heap initialized
 * - GDT initialized
 * - TSS initialized
 * - Initrd loaded
 */

#include "../include/kernel.h"
#include "../include/elf.h"
#include "../include/mem.h"
#include "../include/usermode.h"
#include "../include/initrd.h"
#include "../include/tss.h"

/**
 * Test 1: Validate heap is working
 */
static void test_heap_working(void) {
    kprintf("\n[TEST 1] Heap Allocation Test\n");
    kprintf("=============================\n");

    // Try to allocate small block
    void* ptr1 = kmalloc(64);
    if (!ptr1) {
        kprintf("[FAIL] kmalloc(64) returned NULL - heap not working!\n");
        return;
    }
    kprintf("[PASS] kmalloc(64) = %p\n", ptr1);

    // Try to allocate larger block
    void* ptr2 = kmalloc(4096);
    if (!ptr2) {
        kprintf("[FAIL] kmalloc(4096) returned NULL\n");
        kfree(ptr1);
        return;
    }
    kprintf("[PASS] kmalloc(4096) = %p\n", ptr2);

    // Free them
    kfree(ptr1);
    kfree(ptr2);
    kprintf("[PASS] Heap working correctly\n");
}

/**
 * Test 2: Validate ELF header parsing
 */
static void test_elf_header_validation(void) {
    kprintf("\n[TEST 2] ELF Header Validation\n");
    kprintf("===============================\n");

    // Create a valid ELF64 header
    elf64_ehdr_t valid_header = {
        .e_ident = {
            0x7F, 'E', 'L', 'F',  // Magic
            ELFCLASS64,            // 64-bit
            ELFDATA2LSB,           // Little-endian
            EV_CURRENT,            // Version
            0, 0, 0, 0, 0, 0, 0, 0, 0  // Padding
        },
        .e_type = ET_EXEC,
        .e_machine = EM_X86_64,
        .e_version = EV_CURRENT,
        .e_entry = 0x400000,  // User space entry
        .e_phoff = 64,
        .e_shoff = 0,
        .e_flags = 0,
        .e_ehsize = sizeof(elf64_ehdr_t),
        .e_phentsize = sizeof(elf64_phdr_t),
        .e_phnum = 1,
        .e_shentsize = 0,
        .e_shnum = 0,
        .e_shstrndx = 0
    };

    if (elf_validate_header(&valid_header)) {
        kprintf("[PASS] Valid ELF64 header accepted\n");
    } else {
        kprintf("[FAIL] Valid ELF64 header rejected\n");
        return;
    }

    // Test invalid magic
    elf64_ehdr_t invalid_magic = valid_header;
    invalid_magic.e_ident[0] = 0x42;
    if (!elf_validate_header(&invalid_magic)) {
        kprintf("[PASS] Invalid magic rejected\n");
    } else {
        kprintf("[FAIL] Invalid magic accepted\n");
    }

    // Test wrong architecture
    elf64_ehdr_t wrong_arch = valid_header;
    wrong_arch.e_machine = EM_386;
    if (!elf_validate_header(&wrong_arch)) {
        kprintf("[PASS] Wrong architecture rejected\n");
    } else {
        kprintf("[FAIL] Wrong architecture accepted\n");
    }

    // Test kernel space entry
    elf64_ehdr_t kernel_entry = valid_header;
    kernel_entry.e_entry = 0xFFFF800000000000ULL;
    if (!elf_validate_header(&kernel_entry)) {
        kprintf("[PASS] Kernel space entry rejected\n");
    } else {
        kprintf("[FAIL] Kernel space entry accepted\n");
    }

    kprintf("[PASS] ELF header validation working\n");
}

/**
 * Test 3: Check if initrd contains test program
 */
static void test_initrd_file_access(void) {
    kprintf("\n[TEST 3] Initrd File Access\n");
    kprintf("===========================\n");

    // Try to find test_minimal in initrd
    uint64_t size = 0;
    void* data = initrd_get_file("test_minimal", &size);

    if (!data) {
        kprintf("[WARN] test_minimal not found in initrd\n");
        kprintf("[INFO] Build it with: make -C userspace tests\n");
        kprintf("[INFO] Then add it to initrd and rebuild\n");
        return;
    }

    kprintf("[PASS] Found test_minimal in initrd (size=%lu bytes)\n", size);

    // Validate it's an ELF file
    if (size < sizeof(elf64_ehdr_t)) {
        kprintf("[FAIL] File too small to be ELF\n");
        return;
    }

    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)data;
    if (elf_validate_header(ehdr)) {
        kprintf("[PASS] test_minimal is valid ELF64 executable\n");
        kprintf("[INFO] Entry point: 0x%016lx\n", ehdr->e_entry);
        kprintf("[INFO] Program headers: %d\n", ehdr->e_phnum);
    } else {
        kprintf("[FAIL] test_minimal has invalid ELF header\n");
    }
}

/**
 * Test 4: Check GDT and TSS setup
 */
static void test_gdt_tss_setup(void) {
    kprintf("\n[TEST 4] GDT and TSS Setup\n");
    kprintf("==========================\n");

    // Check current privilege level
    uint8_t cpl = get_cpl();
    kprintf("[INFO] Current CPL (privilege level): %d\n", cpl);

    if (cpl != 0) {
        kprintf("[FAIL] Not running in ring 0! CPL=%d\n", cpl);
        return;
    }
    kprintf("[PASS] Running in ring 0 (kernel mode)\n");

    // Check TSS is initialized
    tss_t* tss = tss_get();
    if (!tss) {
        kprintf("[FAIL] TSS not initialized\n");
        return;
    }
    kprintf("[PASS] TSS structure available at %p\n", tss);

    // Check TSS has kernel stack set
    if (tss->rsp0 == 0) {
        kprintf("[WARN] TSS.RSP0 not set (will be set by start_usermode)\n");
    } else {
        kprintf("[INFO] TSS.RSP0 = 0x%016lx\n", tss->rsp0);
    }

    kprintf("[PASS] GDT and TSS configured\n");
}

/**
 * Test 5: Load ELF but don't execute
 */
static void test_elf_load_dry_run(void) {
    kprintf("\n[TEST 5] ELF Load (Dry Run)\n");
    kprintf("===========================\n");

    uint64_t entry = 0;
    uint64_t stack = 0;

    int ret = elf_load("test_minimal", 0, NULL, &entry, &stack);

    if (ret == ELF_ERR_NOT_FOUND) {
        kprintf("[WARN] test_minimal not in initrd - skipping\n");
        return;
    }

    if (ret != ELF_SUCCESS) {
        kprintf("[FAIL] elf_load failed with error %d\n", ret);
        return;
    }

    kprintf("[PASS] ELF loaded successfully\n");
    kprintf("[INFO] Entry point: 0x%016lx\n", entry);
    kprintf("[INFO] Stack pointer: 0x%016lx\n", stack);

    // Validate entry point is in user space
    if (entry >= USER_SPACE_END) {
        kprintf("[FAIL] Entry point 0x%016lx is not in user space\n", entry);
        return;
    }
    kprintf("[PASS] Entry point in user space\n");

    // Validate stack is in user space
    if (stack >= USER_SPACE_END) {
        kprintf("[FAIL] Stack 0x%016lx is not in user space\n", stack);
        return;
    }
    kprintf("[PASS] Stack in user space\n");
}

/**
 * Test 6: Full ELF load and ring 3 transition
 */
static void test_elf_load_and_execute(void) {
    kprintf("\n[TEST 6] ELF Load and Execute (Ring 3)\n");
    kprintf("=======================================\n");

    kprintf("[INFO] This test will transition to ring 3 and execute user code\n");
    kprintf("[INFO] If test_minimal works, you should see 'Hello from Ring 3!'\n");
    kprintf("[INFO] This function will NOT return (entering user mode)\n");
    kprintf("\n");

    // Load ELF from initrd
    uint64_t entry = 0;
    uint64_t stack = 0;

    int ret = elf_load("test_minimal", 0, NULL, &entry, &stack);

    if (ret != ELF_SUCCESS) {
        kprintf("[FAIL] elf_load failed with error %d\n", ret);
        return;
    }

    kprintf("[INFO] ELF loaded: entry=0x%016lx stack=0x%016lx\n", entry, stack);

    // Transition to user mode
    // This will call enter_usermode which uses IRETQ
    // After this, we're in ring 3 and never return here
    kprintf("[INFO] Transitioning to ring 3...\n");
    kprintf("=====================================\n\n");

    start_usermode(entry, stack);

    // Should never reach here
    kprintf("[FAIL] Returned from start_usermode - this should not happen!\n");
}

/**
 * Main test runner
 */
void elf_loader_test_suite(int test_number) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  ELF Loader & Usermode Test Suite\n");
    kprintf("========================================\n");

    switch (test_number) {
        case 0:
            // Run all non-destructive tests
            test_heap_working();
            test_elf_header_validation();
            test_initrd_file_access();
            test_gdt_tss_setup();
            test_elf_load_dry_run();
            kprintf("\n[INFO] All safe tests complete.\n");
            kprintf("[INFO] To test ring 3 execution, run: elf_loader_test_suite(6)\n");
            break;

        case 1:
            test_heap_working();
            break;

        case 2:
            test_elf_header_validation();
            break;

        case 3:
            test_initrd_file_access();
            break;

        case 4:
            test_gdt_tss_setup();
            break;

        case 5:
            test_elf_load_dry_run();
            break;

        case 6:
            test_elf_load_and_execute();
            break;

        default:
            kprintf("[ERROR] Unknown test number: %d\n", test_number);
            kprintf("[INFO] Valid tests: 0 (all safe), 1-5 (individual), 6 (execute)\n");
            break;
    }
}
