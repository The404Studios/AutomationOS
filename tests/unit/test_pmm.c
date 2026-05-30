#include "../../kernel/include/mem.h"
#include "../../kernel/include/kernel.h"

void test_pmm_init(void) {
    memory_map_entry_t mmap[2];
    mmap[0].base = 0x100000;      // 1 MB
    mmap[0].length = 0x1000000;   // 16 MB
    mmap[0].type = 1;             // Usable

    mmap[1].base = 0x2000000;     // 32 MB
    mmap[1].length = 0x1000000;   // 16 MB
    mmap[1].type = 2;             // Reserved

    pmm_init(mmap, 2);

    uint64_t total = pmm_get_total_memory();
    ASSERT(total == 0x1000000);  // Should be 16 MB

    kprintf("[TEST] PMM init: PASS\n");
}

void test_pmm_alloc_free(void) {
    void* page1 = pmm_alloc_page();
    ASSERT(page1 != NULL);

    void* page2 = pmm_alloc_page();
    ASSERT(page2 != NULL);
    ASSERT(page1 != page2);

    uint64_t used_before = pmm_get_used_memory();

    pmm_free_page(page1);
    pmm_free_page(page2);

    uint64_t used_after = pmm_get_used_memory();
    ASSERT(used_after == used_before - (2 * PAGE_SIZE));

    kprintf("[TEST] PMM alloc/free: PASS\n");
}

void run_pmm_tests(void) {
    kprintf("[TEST] Running PMM tests...\n");
    test_pmm_init();
    test_pmm_alloc_free();
    kprintf("[TEST] All PMM tests passed\n");
}
