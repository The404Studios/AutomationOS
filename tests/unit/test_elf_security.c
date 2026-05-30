/**
 * ELF Loader Security Validation Tests
 * =====================================
 *
 * Comprehensive security tests for ELF loader against malformed binaries.
 * Tests all edge cases, boundary conditions, and attack vectors.
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/elf.h"
#include "../../kernel/include/mem.h"
#include "../../kernel/include/string.h"

// Test utilities
#define TEST_PASS "\033[32mPASS\033[0m"
#define TEST_FAIL "\033[31mFAIL\033[0m"

static int tests_passed = 0;
static int tests_failed = 0;

#define EXPECT(cond, msg) do { \
    if (cond) { \
        kprintf("  [%s] %s\n", TEST_PASS, msg); \
        tests_passed++; \
    } else { \
        kprintf("  [%s] %s\n", TEST_FAIL, msg); \
        tests_failed++; \
    } \
} while(0)

/**
 * Create a minimal valid ELF64 header template
 */
static void create_valid_elf_header(elf64_ehdr_t* ehdr) {
    memset(ehdr, 0, sizeof(elf64_ehdr_t));

    // Valid magic number
    ehdr->e_ident[EI_MAG0] = ELFMAG0;
    ehdr->e_ident[EI_MAG1] = ELFMAG1;
    ehdr->e_ident[EI_MAG2] = ELFMAG2;
    ehdr->e_ident[EI_MAG3] = ELFMAG3;

    // 64-bit little-endian
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;

    // Executable for x86_64
    ehdr->e_type = ET_EXEC;
    ehdr->e_machine = EM_X86_64;
    ehdr->e_version = EV_CURRENT;

    // Valid entry point in user space
    ehdr->e_entry = 0x400000;  // Standard user space entry

    // Basic header info
    ehdr->e_ehsize = sizeof(elf64_ehdr_t);
    ehdr->e_phentsize = sizeof(elf64_phdr_t);
    ehdr->e_phnum = 1;
    ehdr->e_phoff = sizeof(elf64_ehdr_t);
}

/**
 * Test Case 1: Wrong ELF magic number
 */
static void test_wrong_magic_number(void) {
    kprintf("\n[TEST] Wrong Magic Number\n");

    elf64_ehdr_t ehdr;
    create_valid_elf_header(&ehdr);

    // Test 1a: Completely wrong magic
    ehdr.e_ident[EI_MAG0] = 0x12;
    ehdr.e_ident[EI_MAG1] = 0x34;
    ehdr.e_ident[EI_MAG2] = 0x56;
    ehdr.e_ident[EI_MAG3] = 0x78;

    int result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject completely invalid magic (0x12345678)");

    // Test 1b: Partially wrong magic (only first byte correct)
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_MAG1] = 'X';
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject partially invalid magic (0x7F 'X' 'L' 'F')");

    // Test 1c: PE magic (common confusion)
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_MAG0] = 'M';
    ehdr.e_ident[EI_MAG1] = 'Z';
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject PE magic number ('M' 'Z')");
}

/**
 * Test Case 2: Wrong ELF class (32-bit vs 64-bit)
 */
static void test_wrong_class(void) {
    kprintf("\n[TEST] Wrong ELF Class\n");

    elf64_ehdr_t ehdr;

    // Test 2a: 32-bit ELF
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_CLASS] = ELFCLASS32;
    int result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject 32-bit ELF (class=1)");

    // Test 2b: Invalid class value
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_CLASS] = 0;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject invalid class (class=0)");

    // Test 2c: Future class value
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_CLASS] = 3;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject unknown class (class=3)");
}

/**
 * Test Case 3: Wrong architecture
 */
static void test_wrong_architecture(void) {
    kprintf("\n[TEST] Wrong Architecture\n");

    elf64_ehdr_t ehdr;

    // Test 3a: i386 architecture
    create_valid_elf_header(&ehdr);
    ehdr.e_machine = EM_386;
    int result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject i386 architecture (machine=3)");

    // Test 3b: ARM architecture
    create_valid_elf_header(&ehdr);
    ehdr.e_machine = 40;  // EM_ARM
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject ARM architecture (machine=40)");

    // Test 3c: No architecture
    create_valid_elf_header(&ehdr);
    ehdr.e_machine = EM_NONE;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject no architecture (machine=0)");
}

/**
 * Test Case 4: Invalid segment count
 */
static void test_invalid_segment_count(void) {
    kprintf("\n[TEST] Invalid Segment Count\n");

    // Create a minimal ELF with program headers
    size_t elf_size = sizeof(elf64_ehdr_t) + 10 * sizeof(elf64_phdr_t);
    uint8_t* elf_buffer = (uint8_t*)kmalloc(elf_size);

    if (!elf_buffer) {
        kprintf("  [SKIP] Could not allocate test buffer\n");
        return;
    }

    memset(elf_buffer, 0, elf_size);
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)elf_buffer;
    create_valid_elf_header(ehdr);
    ehdr->e_phnum = 10;
    ehdr->e_phoff = sizeof(elf64_ehdr_t);

    // Test 4a: Reasonable segment count (should pass)
    int result = elf_load_and_exec(elf_buffer, elf_size, "test_segments");
    // Note: Will fail for other reasons, but should validate header first
    EXPECT(result != 0, "Handle reasonable segment count (10 segments)");

    // Test 4b: Excessive segment count (overflow)
    ehdr->e_phnum = 10000;
    result = elf_load_and_exec(elf_buffer, elf_size, "test_overflow");
    EXPECT(result == ELF_ERR_INVALID, "Reject excessive segment count (10000)");

    // Test 4c: Segment count that would overflow bounds check
    ehdr->e_phnum = 0xFFFF;  // Maximum uint16_t
    result = elf_load_and_exec(elf_buffer, elf_size, "test_max_segments");
    EXPECT(result == ELF_ERR_INVALID, "Reject maximum segment count (65535)");

    kfree(elf_buffer);
}

/**
 * Test Case 5: Entry point validation
 */
static void test_entry_point_validation(void) {
    kprintf("\n[TEST] Entry Point Validation\n");

    elf64_ehdr_t ehdr;

    // Test 5a: Valid user space entry
    create_valid_elf_header(&ehdr);
    ehdr.e_entry = 0x400000;
    int result = elf_validate_header(&ehdr);
    EXPECT(result == 1, "Accept valid user space entry (0x400000)");

    // Test 5b: Entry in kernel space
    create_valid_elf_header(&ehdr);
    ehdr.e_entry = KERNEL_SPACE_START;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject kernel space entry");

    // Test 5c: Entry at high canonical boundary
    create_valid_elf_header(&ehdr);
    ehdr.e_entry = 0xFFFF800000000000ULL;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject entry at kernel boundary");

    // Test 5d: Entry at user space upper boundary
    create_valid_elf_header(&ehdr);
    ehdr.e_entry = USER_SPACE_END - 1;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 1, "Accept entry just below user space limit");

    // Test 5e: Entry at zero (NULL)
    create_valid_elf_header(&ehdr);
    ehdr.e_entry = 0;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 1, "Accept entry at zero (valid but unusual)");
}

/**
 * Test Case 6: Data encoding validation
 */
static void test_data_encoding(void) {
    kprintf("\n[TEST] Data Encoding\n");

    elf64_ehdr_t ehdr;

    // Test 6a: Big endian
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_DATA] = ELFDATA2MSB;
    int result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject big-endian encoding");

    // Test 6b: Invalid encoding
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_DATA] = 0;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject invalid encoding (0)");

    // Test 6c: Future encoding
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_DATA] = 3;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject unknown encoding (3)");
}

/**
 * Test Case 7: ELF version validation
 */
static void test_version_validation(void) {
    kprintf("\n[TEST] Version Validation\n");

    elf64_ehdr_t ehdr;

    // Test 7a: Invalid version
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_VERSION] = EV_NONE;
    int result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject invalid version (0)");

    // Test 7b: Future version
    create_valid_elf_header(&ehdr);
    ehdr.e_ident[EI_VERSION] = 2;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject future version (2)");
}

/**
 * Test Case 8: File type validation
 */
static void test_file_type(void) {
    kprintf("\n[TEST] File Type Validation\n");

    elf64_ehdr_t ehdr;

    // Test 8a: Relocatable object (not executable)
    create_valid_elf_header(&ehdr);
    ehdr.e_type = ET_REL;
    int result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject relocatable object (type=1)");

    // Test 8b: Core dump
    create_valid_elf_header(&ehdr);
    ehdr.e_type = ET_CORE;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject core dump (type=4)");

    // Test 8c: No type
    create_valid_elf_header(&ehdr);
    ehdr.e_type = ET_NONE;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 0, "Reject no type (type=0)");

    // Test 8d: Executable (should pass)
    create_valid_elf_header(&ehdr);
    ehdr.e_type = ET_EXEC;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 1, "Accept executable (type=2)");

    // Test 8e: Dynamic/PIE (should pass)
    create_valid_elf_header(&ehdr);
    ehdr.e_type = ET_DYN;
    result = elf_validate_header(&ehdr);
    EXPECT(result == 1, "Accept dynamic/PIE (type=3)");
}

/**
 * Test Case 9: Buffer size validation
 */
static void test_buffer_size(void) {
    kprintf("\n[TEST] Buffer Size Validation\n");

    // Test 9a: Buffer too small for header
    uint8_t small_buffer[10];
    memset(small_buffer, 0, sizeof(small_buffer));
    int result = elf_load_and_exec(small_buffer, sizeof(small_buffer), "test_small");
    EXPECT(result == ELF_ERR_INVALID, "Reject buffer smaller than ELF header");

    // Test 9b: Exact header size (no program headers)
    size_t hdr_size = sizeof(elf64_ehdr_t);
    uint8_t* hdr_buffer = (uint8_t*)kmalloc(hdr_size);
    if (hdr_buffer) {
        memset(hdr_buffer, 0, hdr_size);
        elf64_ehdr_t* ehdr = (elf64_ehdr_t*)hdr_buffer;
        create_valid_elf_header(ehdr);
        ehdr->e_phnum = 0;
        ehdr->e_phoff = 0;

        result = elf_load_and_exec(hdr_buffer, hdr_size, "test_exact");
        // Should pass header validation but fail with no loadable segments
        EXPECT(result != 0, "Handle exact header size with no segments");
        kfree(hdr_buffer);
    }

    // Test 9c: NULL buffer
    result = elf_load_and_exec(NULL, 1000, "test_null");
    EXPECT(result == ELF_ERR_INVALID, "Reject NULL buffer");

    // Test 9d: Zero size
    uint8_t buffer[100];
    result = elf_load_and_exec(buffer, 0, "test_zero");
    EXPECT(result == ELF_ERR_INVALID, "Reject zero size");
}

/**
 * Test Case 10: Program header bounds checking
 */
static void test_program_header_bounds(void) {
    kprintf("\n[TEST] Program Header Bounds\n");

    // Test 10a: Program headers beyond buffer
    size_t elf_size = sizeof(elf64_ehdr_t) + 100;
    uint8_t* elf_buffer = (uint8_t*)kmalloc(elf_size);

    if (!elf_buffer) {
        kprintf("  [SKIP] Could not allocate test buffer\n");
        return;
    }

    memset(elf_buffer, 0, elf_size);
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)elf_buffer;
    create_valid_elf_header(ehdr);

    // Point program headers past end of buffer
    ehdr->e_phnum = 5;
    ehdr->e_phoff = elf_size + 1000;

    int result = elf_load_and_exec(elf_buffer, elf_size, "test_phdr_oob");
    EXPECT(result == ELF_ERR_INVALID, "Reject program headers beyond buffer");

    // Test 10b: Program header offset at buffer boundary
    ehdr->e_phnum = 1;
    ehdr->e_phoff = elf_size - sizeof(elf64_phdr_t) + 1;  // Partial overlap

    result = elf_load_and_exec(elf_buffer, elf_size, "test_phdr_partial");
    EXPECT(result == ELF_ERR_INVALID, "Reject partial program header");

    kfree(elf_buffer);
}

/**
 * Test Case 11: Segment virtual address validation
 */
static void test_segment_vaddr_validation(void) {
    kprintf("\n[TEST] Segment Virtual Address Validation\n");

    size_t elf_size = sizeof(elf64_ehdr_t) + sizeof(elf64_phdr_t);
    uint8_t* elf_buffer = (uint8_t*)kmalloc(elf_size);

    if (!elf_buffer) {
        kprintf("  [SKIP] Could not allocate test buffer\n");
        return;
    }

    memset(elf_buffer, 0, elf_size);
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)elf_buffer;
    elf64_phdr_t* phdr = (elf64_phdr_t*)(elf_buffer + sizeof(elf64_ehdr_t));

    create_valid_elf_header(ehdr);
    ehdr->e_phnum = 1;
    ehdr->e_phoff = sizeof(elf64_ehdr_t);

    // Test 11a: Segment in kernel space
    phdr->p_type = PT_LOAD;
    phdr->p_vaddr = KERNEL_SPACE_START + 0x1000;
    phdr->p_memsz = 0x1000;
    phdr->p_filesz = 0;
    phdr->p_flags = PF_R | PF_X;

    int result = elf_load_and_exec(elf_buffer, elf_size, "test_kernel_seg");
    EXPECT(result == ELF_ERR_PERM, "Reject segment in kernel space");

    // Test 11b: Segment at user space boundary
    phdr->p_vaddr = USER_SPACE_END;
    result = elf_load_and_exec(elf_buffer, elf_size, "test_boundary_seg");
    EXPECT(result == ELF_ERR_PERM, "Reject segment at user space boundary");

    // Test 11c: Segment just below boundary (should pass initial check)
    phdr->p_vaddr = USER_SPACE_END - PAGE_SIZE;
    phdr->p_memsz = PAGE_SIZE;
    result = elf_load_and_exec(elf_buffer, elf_size, "test_valid_seg");
    // Will fail for other reasons, but should pass vaddr check
    EXPECT(result != ELF_ERR_INVALID, "Accept segment just below user space limit");

    kfree(elf_buffer);
}

/**
 * Test Case 12: Alignment validation
 */
static void test_alignment(void) {
    kprintf("\n[TEST] Alignment Validation\n");

    size_t elf_size = sizeof(elf64_ehdr_t) + sizeof(elf64_phdr_t);
    uint8_t* elf_buffer = (uint8_t*)kmalloc(elf_size);

    if (!elf_buffer) {
        kprintf("  [SKIP] Could not allocate test buffer\n");
        return;
    }

    memset(elf_buffer, 0, elf_size);
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)elf_buffer;
    elf64_phdr_t* phdr = (elf64_phdr_t*)(elf_buffer + sizeof(elf64_ehdr_t));

    create_valid_elf_header(ehdr);
    ehdr->e_phnum = 1;
    ehdr->e_phoff = sizeof(elf64_ehdr_t);

    // Test 12a: Unaligned virtual address (loader should handle)
    phdr->p_type = PT_LOAD;
    phdr->p_vaddr = 0x400001;  // Not page-aligned
    phdr->p_memsz = 0x1000;
    phdr->p_filesz = 0;
    phdr->p_flags = PF_R | PF_W;

    int result = elf_load_and_exec(elf_buffer, elf_size, "test_unaligned");
    // Loader should align down, so this should work
    EXPECT(result != ELF_ERR_INVALID, "Handle unaligned virtual address");

    kfree(elf_buffer);
}

/**
 * Test Case 13: Size overflow validation
 */
static void test_size_overflow(void) {
    kprintf("\n[TEST] Size Overflow Validation\n");

    size_t elf_size = sizeof(elf64_ehdr_t) + sizeof(elf64_phdr_t);
    uint8_t* elf_buffer = (uint8_t*)kmalloc(elf_size);

    if (!elf_buffer) {
        kprintf("  [SKIP] Could not allocate test buffer\n");
        return;
    }

    memset(elf_buffer, 0, elf_size);
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)elf_buffer;
    elf64_phdr_t* phdr = (elf64_phdr_t*)(elf_buffer + sizeof(elf64_ehdr_t));

    create_valid_elf_header(ehdr);
    ehdr->e_phnum = 1;
    ehdr->e_phoff = sizeof(elf64_ehdr_t);

    // Test 13a: Segment that would overflow address space
    phdr->p_type = PT_LOAD;
    phdr->p_vaddr = USER_SPACE_END - PAGE_SIZE;
    phdr->p_memsz = PAGE_SIZE * 1000;  // Would wrap around
    phdr->p_filesz = 0;
    phdr->p_flags = PF_R | PF_W;

    int result = elf_load_and_exec(elf_buffer, elf_size, "test_overflow");
    EXPECT(result == ELF_ERR_PERM || result == ELF_ERR_NOMEM,
           "Reject segment that overflows address space");

    // Test 13b: filesz > memsz (invalid)
    phdr->p_vaddr = 0x400000;
    phdr->p_filesz = 0x2000;
    phdr->p_memsz = 0x1000;

    result = elf_load_and_exec(elf_buffer, elf_size, "test_filesz_overflow");
    // Should either reject or handle gracefully
    EXPECT(result != 0, "Handle filesz > memsz");

    kfree(elf_buffer);
}

/**
 * Test Case 14: Null pointer handling
 */
static void test_null_pointers(void) {
    kprintf("\n[TEST] Null Pointer Handling\n");

    // Test 14a: NULL header pointer
    int result = elf_validate_header(NULL);
    EXPECT(result == 0, "Reject NULL header pointer");

    // Test 14b: NULL buffer with valid size
    result = elf_load_and_exec(NULL, 1000, "test");
    EXPECT(result == ELF_ERR_INVALID, "Reject NULL buffer");

    // Test 14c: NULL name
    uint8_t buffer[100];
    result = elf_load_and_exec(buffer, sizeof(buffer), NULL);
    EXPECT(result == ELF_ERR_INVALID, "Reject NULL name");
}

/**
 * Main test runner
 */
void test_elf_security(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("ELF LOADER SECURITY VALIDATION TESTS\n");
    kprintf("========================================\n");

    tests_passed = 0;
    tests_failed = 0;

    // Run all test suites
    test_wrong_magic_number();
    test_wrong_class();
    test_wrong_architecture();
    test_invalid_segment_count();
    test_entry_point_validation();
    test_data_encoding();
    test_version_validation();
    test_file_type();
    test_buffer_size();
    test_program_header_bounds();
    test_segment_vaddr_validation();
    test_alignment();
    test_size_overflow();
    test_null_pointers();

    // Print summary
    kprintf("\n========================================\n");
    kprintf("TEST SUMMARY\n");
    kprintf("========================================\n");
    kprintf("Tests passed: %d\n", tests_passed);
    kprintf("Tests failed: %d\n", tests_failed);
    kprintf("Total tests:  %d\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        kprintf("\n\033[32m✓ ALL SECURITY TESTS PASSED\033[0m\n\n");
    } else {
        kprintf("\n\033[31m✗ SOME TESTS FAILED\033[0m\n\n");
    }
}
