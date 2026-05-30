#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/include/mem.h"
#include "../../kernel/include/kernel.h"

// Mock functions for testing (would be replaced by actual implementations)
static uint64_t mock_pmm_alloc_page_count = 0;
static uint64_t mock_vmm_map_page_count = 0;

void* pmm_alloc_page(void) {
    mock_pmm_alloc_page_count++;
    // Return fake physical address
    return (void*)(0x100000 + (mock_pmm_alloc_page_count - 1) * PAGE_SIZE);
}

void* vmm_map_page(void* virt, void* phys, uint32_t flags) {
    (void)virt;
    (void)phys;
    (void)flags;
    mock_vmm_map_page_count++;
    return virt;
}

void kernel_panic(const char* message) {
    fprintf(stderr, "PANIC: %s\n", message);
    exit(1);
}

int kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vprintf(format, args);
    va_end(args);
    return ret;
}

// Test functions
void test_heap_init(void) {
    printf("Test: heap_init()\n");
    heap_init();

    // Verify pages were allocated and mapped
    assert(mock_pmm_alloc_page_count == (16 * 1024 * 1024) / PAGE_SIZE);
    assert(mock_vmm_map_page_count == (16 * 1024 * 1024) / PAGE_SIZE);

    printf("  PASS: heap_init() allocated and mapped pages\n");
}

void test_kmalloc_basic(void) {
    printf("Test: kmalloc() basic allocation\n");

    void* ptr1 = kmalloc(128);
    assert(ptr1 != NULL);
    printf("  PASS: kmalloc(128) returned non-NULL\n");

    void* ptr2 = kmalloc(256);
    assert(ptr2 != NULL);
    assert(ptr2 != ptr1);
    printf("  PASS: kmalloc(256) returned different pointer\n");

    // Test zero allocation
    void* ptr3 = kmalloc(0);
    assert(ptr3 == NULL);
    printf("  PASS: kmalloc(0) returned NULL\n");
}

void test_kfree_basic(void) {
    printf("Test: kfree() basic deallocation\n");

    void* ptr = kmalloc(512);
    assert(ptr != NULL);

    kfree(ptr);
    printf("  PASS: kfree() did not crash\n");

    // Test NULL pointer
    kfree(NULL);
    printf("  PASS: kfree(NULL) handled gracefully\n");
}

void test_allocation_patterns(void) {
    printf("Test: Allocation patterns\n");

    // Allocate several blocks
    void* ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = kmalloc(64);
        assert(ptrs[i] != NULL);
    }
    printf("  PASS: Allocated 10 blocks of 64 bytes\n");

    // Free every other block
    for (int i = 0; i < 10; i += 2) {
        kfree(ptrs[i]);
    }
    printf("  PASS: Freed 5 blocks\n");

    // Allocate again (should reuse freed blocks)
    void* new_ptr = kmalloc(64);
    assert(new_ptr != NULL);
    printf("  PASS: Reallocation successful\n");

    // Free remaining blocks
    for (int i = 1; i < 10; i += 2) {
        kfree(ptrs[i]);
    }
    kfree(new_ptr);
    printf("  PASS: Freed all remaining blocks\n");
}

void test_alignment(void) {
    printf("Test: Alignment\n");

    void* ptr1 = kmalloc(1);
    void* ptr2 = kmalloc(15);
    void* ptr3 = kmalloc(17);

    // All pointers should be 16-byte aligned
    assert(((uint64_t)ptr1 & 0xF) == 0);
    assert(((uint64_t)ptr2 & 0xF) == 0);
    assert(((uint64_t)ptr3 & 0xF) == 0);

    printf("  PASS: All allocations are 16-byte aligned\n");

    kfree(ptr1);
    kfree(ptr2);
    kfree(ptr3);
}

int main(void) {
    printf("========================================\n");
    printf("Kernel Heap Allocator Unit Tests\n");
    printf("========================================\n\n");

    test_heap_init();
    test_kmalloc_basic();
    test_kfree_basic();
    test_allocation_patterns();
    test_alignment();

    printf("\n========================================\n");
    printf("All tests passed!\n");
    printf("========================================\n");

    return 0;
}
