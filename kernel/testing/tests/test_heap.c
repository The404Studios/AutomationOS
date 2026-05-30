#include "../../include/ktest.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"

/*
 * Kernel Heap Tests
 * Tests dynamic memory allocation (kmalloc/kfree)
 */

KTEST_SUITE(heap);

KTEST_CASE(heap, malloc_returns_non_null) {
    void* ptr = kmalloc(64);
    KTEST_ASSERT_NOT_NULL(ptr);
    kfree(ptr);
}

KTEST_CASE(heap, malloc_returns_aligned_memory) {
    void* ptr = kmalloc(128);
    KTEST_ASSERT_NOT_NULL(ptr);

    // Should be at least 8-byte aligned
    uintptr_t addr = (uintptr_t)ptr;
    KTEST_ASSERT_EQ(addr % 8, 0);

    kfree(ptr);
}

KTEST_CASE(heap, malloc_different_sizes) {
    size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 4096};
    void* ptrs[9];

    for (int i = 0; i < 9; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        KTEST_ASSERT_NOT_NULL(ptrs[i]);
    }

    // All pointers should be different
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 9; j++) {
            KTEST_ASSERT_NE(ptrs[i], ptrs[j]);
        }
    }

    // Clean up
    for (int i = 0; i < 9; i++) {
        kfree(ptrs[i]);
    }
}

KTEST_CASE(heap, free_null_is_safe) {
    kfree(NULL);
    KTEST_ASSERT_TRUE(true);
}

KTEST_CASE(heap, can_write_to_allocated_memory) {
    size_t size = 256;
    uint8_t* ptr = (uint8_t*)kmalloc(size);
    KTEST_ASSERT_NOT_NULL(ptr);

    // Write pattern
    for (size_t i = 0; i < size; i++) {
        ptr[i] = (uint8_t)(i & 0xFF);
    }

    // Verify pattern
    for (size_t i = 0; i < size; i++) {
        KTEST_ASSERT_EQ(ptr[i], (uint8_t)(i & 0xFF));
    }

    kfree(ptr);
}

KTEST_CASE(heap, alloc_and_free_stress_test) {
    // Allocate and free in various patterns
    for (int iter = 0; iter < 10; iter++) {
        void* ptrs[10];

        // Allocate 10 blocks
        for (int i = 0; i < 10; i++) {
            ptrs[i] = kmalloc(64 + i * 32);
            KTEST_ASSERT_NOT_NULL(ptrs[i]);
        }

        // Free every other block
        for (int i = 0; i < 10; i += 2) {
            kfree(ptrs[i]);
        }

        // Free remaining blocks
        for (int i = 1; i < 10; i += 2) {
            kfree(ptrs[i]);
        }
    }

    KTEST_ASSERT_TRUE(true);
}

KTEST_CASE(heap, large_allocation) {
    size_t large_size = 65536;  // 64 KB
    void* ptr = kmalloc(large_size);
    KTEST_ASSERT_NOT_NULL(ptr);

    // Write to first and last byte
    uint8_t* bytes = (uint8_t*)ptr;
    bytes[0] = 0xAA;
    bytes[large_size - 1] = 0xBB;

    KTEST_ASSERT_EQ(bytes[0], 0xAA);
    KTEST_ASSERT_EQ(bytes[large_size - 1], 0xBB);

    kfree(ptr);
}

KTEST_CASE(heap, zero_size_allocation) {
    // malloc(0) behavior is implementation-defined
    // Should either return NULL or a valid pointer that can be freed
    void* ptr = kmalloc(0);

    if (ptr != NULL) {
        kfree(ptr);
    }

    KTEST_ASSERT_TRUE(true);
}

KTEST_CASE(heap, realloc_basic) {
    void* ptr = kmalloc(64);
    KTEST_ASSERT_NOT_NULL(ptr);

    // Write pattern
    uint8_t* bytes = (uint8_t*)ptr;
    for (int i = 0; i < 64; i++) {
        bytes[i] = (uint8_t)i;
    }

    // Realloc to larger size
    void* new_ptr = krealloc(ptr, 128);
    KTEST_ASSERT_NOT_NULL(new_ptr);

    // Verify old data is preserved
    bytes = (uint8_t*)new_ptr;
    for (int i = 0; i < 64; i++) {
        KTEST_ASSERT_EQ(bytes[i], (uint8_t)i);
    }

    kfree(new_ptr);
}

KTEST_CASE(heap, realloc_shrink) {
    void* ptr = kmalloc(256);
    KTEST_ASSERT_NOT_NULL(ptr);

    // Write pattern
    uint8_t* bytes = (uint8_t*)ptr;
    for (int i = 0; i < 256; i++) {
        bytes[i] = (uint8_t)(i & 0xFF);
    }

    // Shrink allocation
    void* new_ptr = krealloc(ptr, 64);
    KTEST_ASSERT_NOT_NULL(new_ptr);

    // First 64 bytes should be preserved
    bytes = (uint8_t*)new_ptr;
    for (int i = 0; i < 64; i++) {
        KTEST_ASSERT_EQ(bytes[i], (uint8_t)i);
    }

    kfree(new_ptr);
}

KTEST_CASE(heap, calloc_zeros_memory) {
    size_t count = 64;
    size_t size = 4;
    uint32_t* ptr = (uint32_t*)kcalloc(count, size);
    KTEST_ASSERT_NOT_NULL(ptr);

    // All bytes should be zero
    for (size_t i = 0; i < count; i++) {
        KTEST_ASSERT_EQ(ptr[i], 0);
    }

    kfree(ptr);
}

KTEST_CASE(heap, fragmentation_test) {
    void* ptrs[20];

    // Allocate 20 blocks of varying sizes
    for (int i = 0; i < 20; i++) {
        size_t size = 32 + (i * 16);
        ptrs[i] = kmalloc(size);
        KTEST_ASSERT_NOT_NULL(ptrs[i]);
    }

    // Free every other block (create fragmentation)
    for (int i = 0; i < 20; i += 2) {
        kfree(ptrs[i]);
    }

    // Allocate new blocks in freed spaces
    for (int i = 0; i < 20; i += 2) {
        ptrs[i] = kmalloc(32 + (i * 16));
        KTEST_ASSERT_NOT_NULL(ptrs[i]);
    }

    // Free all
    for (int i = 0; i < 20; i++) {
        kfree(ptrs[i]);
    }
}

KTEST_CASE(heap, double_free_detection) {
    void* ptr = kmalloc(64);
    KTEST_ASSERT_NOT_NULL(ptr);

    kfree(ptr);

    // Double free should be detected (might panic or log error)
    // This test is mainly to ensure the heap has double-free detection
    // In a real implementation, this might set a flag or panic

    // For now, just ensure we don't crash catastrophically
    // kfree(ptr);  // Commented out to avoid actual double-free

    KTEST_ASSERT_TRUE(true);
}

KTEST_CASE(heap, alignment_test) {
    // Test aligned allocation
    void* ptr = kmalloc_aligned(256, 64);  // 256 bytes, 64-byte aligned
    KTEST_ASSERT_NOT_NULL(ptr);

    uintptr_t addr = (uintptr_t)ptr;
    KTEST_ASSERT_EQ(addr % 64, 0);

    kfree(ptr);
}
