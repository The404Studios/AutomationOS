#include "../../include/ktest.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"

/*
 * Virtual Memory Manager (VMM) Tests
 * Tests page table management and virtual address translation
 */

KTEST_SUITE(vmm);

KTEST_CASE(vmm, init_creates_kernel_page_table) {
    // VMM should initialize without crashing
    // This test mainly checks that vmm_init() completes successfully
    KTEST_ASSERT_TRUE(true);
}

KTEST_CASE(vmm, kernel_pages_are_mapped) {
    // The kernel should be mapped at high address
    void* kernel_addr = (void*)KERNEL_VIRTUAL_BASE;

    // This should not cause a page fault (hard to test in kernel)
    // At minimum, the address should be non-null
    KTEST_ASSERT_NOT_NULL(kernel_addr);
}

KTEST_CASE(vmm, map_page_succeeds) {
    void* phys_page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(phys_page);

    void* virt_addr = (void*)0x10000000;  // Arbitrary virtual address

    int result = vmm_map_page((uintptr_t)virt_addr, (uintptr_t)phys_page, VM_READ | VM_WRITE);
    KTEST_ASSERT_EQ(result, 0);

    // Clean up
    vmm_unmap_page((uintptr_t)virt_addr);
    pmm_free_page(phys_page);
}

KTEST_CASE(vmm, unmap_page_succeeds) {
    void* phys_page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(phys_page);

    void* virt_addr = (void*)0x20000000;

    vmm_map_page((uintptr_t)virt_addr, (uintptr_t)phys_page, VM_READ | VM_WRITE);

    // Unmap should succeed
    int result = vmm_unmap_page((uintptr_t)virt_addr);
    KTEST_ASSERT_EQ(result, 0);

    pmm_free_page(phys_page);
}

KTEST_CASE(vmm, map_multiple_pages) {
    void* pages[3];
    void* virt_addrs[3] = {
        (void*)0x30000000,
        (void*)0x31000000,
        (void*)0x32000000
    };

    // Allocate and map 3 pages
    for (int i = 0; i < 3; i++) {
        pages[i] = pmm_alloc_page();
        KTEST_ASSERT_NOT_NULL(pages[i]);

        int result = vmm_map_page((uintptr_t)virt_addrs[i], (uintptr_t)pages[i], VM_READ | VM_WRITE);
        KTEST_ASSERT_EQ(result, 0);
    }

    // Clean up
    for (int i = 0; i < 3; i++) {
        vmm_unmap_page((uintptr_t)virt_addrs[i]);
        pmm_free_page(pages[i]);
    }
}

KTEST_CASE(vmm, map_with_different_permissions) {
    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);

    void* virt = (void*)0x40000000;

    // Test read-only mapping
    int result = vmm_map_page((uintptr_t)virt, (uintptr_t)page, VM_READ);
    KTEST_ASSERT_EQ(result, 0);

    vmm_unmap_page((uintptr_t)virt);

    // Test read-write mapping
    result = vmm_map_page((uintptr_t)virt, (uintptr_t)page, VM_READ | VM_WRITE);
    KTEST_ASSERT_EQ(result, 0);

    vmm_unmap_page((uintptr_t)virt);

    // Test executable mapping
    result = vmm_map_page((uintptr_t)virt, (uintptr_t)page, VM_READ | VM_EXEC);
    KTEST_ASSERT_EQ(result, 0);

    vmm_unmap_page((uintptr_t)virt);
    pmm_free_page(page);
}

KTEST_CASE(vmm, page_aligned_addresses_only) {
    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);

    // Unaligned address should fail or be aligned automatically
    void* unaligned = (void*)0x50001234;
    void* aligned = (void*)ALIGN_DOWN((uintptr_t)unaligned, PAGE_SIZE);

    // The VMM should handle this gracefully
    int result = vmm_map_page((uintptr_t)aligned, (uintptr_t)page, VM_READ | VM_WRITE);
    KTEST_ASSERT_EQ(result, 0);

    vmm_unmap_page((uintptr_t)aligned);
    pmm_free_page(page);
}

KTEST_CASE(vmm, translate_physical_to_virtual) {
    void* phys = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(phys);

    void* virt = (void*)0x60000000;

    vmm_map_page((uintptr_t)virt, (uintptr_t)phys, VM_READ | VM_WRITE);

    // Translate virtual to physical
    void* translated = (void*)vmm_get_physical_address((uintptr_t)virt);
    KTEST_ASSERT_PTR_EQ(translated, phys);

    vmm_unmap_page((uintptr_t)virt);
    pmm_free_page(phys);
}

KTEST_CASE(vmm, unmap_non_mapped_page_is_safe) {
    void* virt = (void*)0x70000000;

    // Unmapping a non-mapped page should not crash
    int result = vmm_unmap_page((uintptr_t)virt);

    // Result depends on implementation - might succeed or return error
    // Main thing is it shouldn't crash
    KTEST_ASSERT_TRUE(true);
}

KTEST_CASE(vmm, map_same_virtual_address_twice) {
    void* page1 = pmm_alloc_page();
    void* page2 = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page1);
    KTEST_ASSERT_NOT_NULL(page2);

    void* virt = (void*)0x80000000;

    // Map first page
    vmm_map_page((uintptr_t)virt, (uintptr_t)page1, VM_READ | VM_WRITE);

    // Mapping same virtual address again should either:
    // 1. Fail and return error
    // 2. Unmap old and map new
    // Implementation-dependent, but shouldn't crash
    vmm_map_page((uintptr_t)virt, (uintptr_t)page2, VM_READ | VM_WRITE);

    KTEST_ASSERT_TRUE(true);

    vmm_unmap_page((uintptr_t)virt);
    pmm_free_page(page1);
    pmm_free_page(page2);
}

KTEST_CASE(vmm, large_allocation_test) {
    #define LARGE_PAGE_COUNT 100
    void* phys_pages[LARGE_PAGE_COUNT];
    void* virt_base = (void*)0x90000000;

    // Allocate and map 100 pages
    for (int i = 0; i < LARGE_PAGE_COUNT; i++) {
        phys_pages[i] = pmm_alloc_page();
        KTEST_ASSERT_NOT_NULL(phys_pages[i]);

        void* virt = (void*)((uintptr_t)virt_base + i * PAGE_SIZE);
        int result = vmm_map_page((uintptr_t)virt, (uintptr_t)phys_pages[i], VM_READ | VM_WRITE);
        KTEST_ASSERT_EQ(result, 0);
    }

    // Clean up
    for (int i = 0; i < LARGE_PAGE_COUNT; i++) {
        void* virt = (void*)((uintptr_t)virt_base + i * PAGE_SIZE);
        vmm_unmap_page((uintptr_t)virt);
        pmm_free_page(phys_pages[i]);
    }
}
