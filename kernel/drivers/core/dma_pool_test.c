/**
 * DMA Pool Smoke Test
 * ===================
 *
 * Quick validation that DMA pool is working correctly.
 * Call from kernel_main() after dma_pool_init().
 */

#include "../../include/dma_pool.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

/**
 * Smoke test - quick validation of basic functionality
 *
 * Returns 0 on success, -1 on failure
 */
int dma_pool_smoke_test(void) {
    kprintf("[DMA Pool] Running smoke test...\n");

    // Test 1: Basic allocation
    dma_buffer_t* buf1 = dma_buffer_alloc();
    if (!buf1) {
        kprintf("[DMA Pool] FAIL: Could not allocate buffer\n");
        return -1;
    }

    // Test 2: Verify alignment
    uint64_t phys = dma_buffer_phys(buf1);
    if (phys % PAGE_SIZE != 0) {
        kprintf("[DMA Pool] FAIL: Buffer not 4KB aligned (phys=0x%llx)\n", phys);
        dma_buffer_free(buf1);
        return -1;
    }

    // Test 3: Verify size
    size_t size = dma_buffer_size(buf1);
    if (size != PAGE_SIZE) {
        kprintf("[DMA Pool] FAIL: Wrong buffer size (expected 4096, got %zu)\n", size);
        dma_buffer_free(buf1);
        return -1;
    }

    // Test 4: Verify we can write to it
    void* virt = dma_buffer_virt(buf1);
    if (!virt) {
        kprintf("[DMA Pool] FAIL: NULL virtual address\n");
        dma_buffer_free(buf1);
        return -1;
    }

    // Write test pattern
    uint32_t* data = (uint32_t*)virt;
    for (int i = 0; i < 1024; i++) {
        data[i] = 0xDEADBEEF ^ i;
    }

    // Verify pattern
    for (int i = 0; i < 1024; i++) {
        if (data[i] != (0xDEADBEEF ^ i)) {
            kprintf("[DMA Pool] FAIL: Data corruption at offset %d\n", i * 4);
            dma_buffer_free(buf1);
            return -1;
        }
    }

    // Test 5: Allocate second buffer
    dma_buffer_t* buf2 = dma_buffer_alloc();
    if (!buf2) {
        kprintf("[DMA Pool] FAIL: Could not allocate second buffer\n");
        dma_buffer_free(buf1);
        return -1;
    }

    // Verify buffers are different
    if (dma_buffer_phys(buf1) == dma_buffer_phys(buf2)) {
        kprintf("[DMA Pool] FAIL: Duplicate buffer allocation\n");
        dma_buffer_free(buf1);
        dma_buffer_free(buf2);
        return -1;
    }

    // Test 6: Free and re-allocate
    dma_buffer_free(buf1);
    dma_buffer_free(buf2);

    dma_buffer_t* buf3 = dma_buffer_alloc();
    if (!buf3) {
        kprintf("[DMA Pool] FAIL: Could not re-allocate after free\n");
        return -1;
    }

    // Should get one of the freed buffers back
    uint64_t phys3 = dma_buffer_phys(buf3);
    if (phys3 != dma_buffer_phys(buf1) && phys3 != dma_buffer_phys(buf2)) {
        // This is OK - just means the free list order is different
        // Not a failure
    }

    dma_buffer_free(buf3);

    // Test 7: 64KB buffer (if available)
    dma_buffer_t* big = dma_buffer_alloc_64k();
    if (big) {
        if (dma_buffer_size(big) != 65536) {
            kprintf("[DMA Pool] FAIL: 64KB buffer wrong size (%zu)\n",
                    dma_buffer_size(big));
            dma_buffer_free(big);
            return -1;
        }
        dma_buffer_free(big);
        kprintf("[DMA Pool] 64KB buffers: OK\n");
    } else {
        kprintf("[DMA Pool] 64KB buffers: not configured (OK)\n");
    }

    // All tests passed
    kprintf("[DMA Pool] Smoke test PASSED\n");
    return 0;
}
