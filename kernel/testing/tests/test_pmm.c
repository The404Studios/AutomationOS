#include "../../include/ktest.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"

/*
 * Physical Memory Manager (PMM) Tests
 * Tests the buddy allocator and physical page management
 */

// Test fixture
typedef struct {
    memory_map_entry_t mmap[4];
    uint32_t mmap_count;
    void* test_pages[10];
    uint32_t page_count;
} pmm_test_fixture_t;

// Setup function
static void pmm_setup(pmm_test_fixture_t* fixture) {
    fixture->mmap_count = 0;
    fixture->page_count = 0;

    // Create a test memory map
    fixture->mmap[0].base = 0x100000;      // 1 MB
    fixture->mmap[0].length = 0x1000000;   // 16 MB
    fixture->mmap[0].type = 1;             // Usable
    fixture->mmap_count++;

    fixture->mmap[1].base = 0x2000000;     // 32 MB
    fixture->mmap[1].length = 0x2000000;   // 32 MB
    fixture->mmap[1].type = 1;             // Usable
    fixture->mmap_count++;

    fixture->mmap[2].base = 0x5000000;     // 80 MB
    fixture->mmap[2].length = 0x1000000;   // 16 MB
    fixture->mmap[2].type = 2;             // Reserved (should be ignored)
    fixture->mmap_count++;
}

// Teardown function
static void pmm_teardown(pmm_test_fixture_t* fixture) {
    // Free any allocated test pages
    for (uint32_t i = 0; i < fixture->page_count; i++) {
        if (fixture->test_pages[i]) {
            pmm_free_page(fixture->test_pages[i]);
        }
    }
}

// Define the test suite with fixture
KTEST_SUITE_WITH_FIXTURE(pmm, pmm_test_fixture_t, pmm_setup, pmm_teardown);

/*
 * Test Cases
 */

KTEST_CASE(pmm, init_calculates_correct_total_memory) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    // PMM should only count usable memory (type == 1)
    // Expected: 16 MB + 32 MB = 48 MB = 0x3000000
    uint64_t total = pmm_get_total_memory();
    KTEST_ASSERT_EQ(total, 0x3000000);
}

KTEST_CASE(pmm, alloc_returns_non_null_page) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);

    f->test_pages[f->page_count++] = page;
}

KTEST_CASE(pmm, alloc_returns_aligned_pages) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);

    // Page should be 4KB aligned
    uintptr_t addr = (uintptr_t)page;
    KTEST_ASSERT_EQ(addr % PAGE_SIZE, 0);

    f->test_pages[f->page_count++] = page;
}

KTEST_CASE(pmm, alloc_returns_different_pages) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    void* page1 = pmm_alloc_page();
    void* page2 = pmm_alloc_page();

    KTEST_ASSERT_NOT_NULL(page1);
    KTEST_ASSERT_NOT_NULL(page2);
    KTEST_ASSERT_NE(page1, page2);

    f->test_pages[f->page_count++] = page1;
    f->test_pages[f->page_count++] = page2;
}

KTEST_CASE(pmm, free_reduces_used_memory) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    uint64_t used_before = pmm_get_used_memory();

    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);

    uint64_t used_after_alloc = pmm_get_used_memory();
    KTEST_ASSERT_EQ(used_after_alloc, used_before + PAGE_SIZE);

    pmm_free_page(page);

    uint64_t used_after_free = pmm_get_used_memory();
    KTEST_ASSERT_EQ(used_after_free, used_before);
}

KTEST_CASE(pmm, can_allocate_multiple_pages) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    // Allocate 10 pages
    for (int i = 0; i < 10; i++) {
        void* page = pmm_alloc_page();
        KTEST_ASSERT_NOT_NULL(page);
        f->test_pages[f->page_count++] = page;
    }

    // All pages should be different
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j < 10; j++) {
            KTEST_ASSERT_NE(f->test_pages[i], f->test_pages[j]);
        }
    }
}

KTEST_CASE(pmm, free_null_page_is_safe) {
    // Should not crash
    pmm_free_page(NULL);
    KTEST_ASSERT_TRUE(true);  // If we get here, test passed
}

KTEST_CASE(pmm, alloc_and_free_stress_test) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    uint64_t initial_used = pmm_get_used_memory();

    // Allocate and free in a loop
    for (int iter = 0; iter < 5; iter++) {
        void* pages[5];

        // Allocate 5 pages
        for (int i = 0; i < 5; i++) {
            pages[i] = pmm_alloc_page();
            KTEST_ASSERT_NOT_NULL(pages[i]);
        }

        // Free them
        for (int i = 0; i < 5; i++) {
            pmm_free_page(pages[i]);
        }
    }

    // Memory usage should return to initial state
    uint64_t final_used = pmm_get_used_memory();
    KTEST_ASSERT_EQ(final_used, initial_used);
}

KTEST_CASE(pmm, used_memory_tracking_is_accurate) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    uint64_t initial = pmm_get_used_memory();

    void* p1 = pmm_alloc_page();
    KTEST_ASSERT_EQ(pmm_get_used_memory(), initial + PAGE_SIZE);

    void* p2 = pmm_alloc_page();
    KTEST_ASSERT_EQ(pmm_get_used_memory(), initial + 2 * PAGE_SIZE);

    void* p3 = pmm_alloc_page();
    KTEST_ASSERT_EQ(pmm_get_used_memory(), initial + 3 * PAGE_SIZE);

    pmm_free_page(p2);
    KTEST_ASSERT_EQ(pmm_get_used_memory(), initial + 2 * PAGE_SIZE);

    pmm_free_page(p1);
    pmm_free_page(p3);
    KTEST_ASSERT_EQ(pmm_get_used_memory(), initial);
}

KTEST_CASE(pmm, memory_boundaries_are_respected) {
    pmm_test_fixture_t* f = (pmm_test_fixture_t*)fixture;

    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);

    uintptr_t addr = (uintptr_t)page;

    // Page should be within valid memory regions
    bool in_region1 = (addr >= 0x100000 && addr < 0x1100000);
    bool in_region2 = (addr >= 0x2000000 && addr < 0x4000000);

    KTEST_ASSERT_TRUE(in_region1 || in_region2);

    f->test_pages[f->page_count++] = page;
}
