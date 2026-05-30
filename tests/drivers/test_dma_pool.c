/**
 * DMA Pool Benchmark
 * ==================
 *
 * Compares I/O performance:
 * 1. Original AHCI (static dma_bounce buffer)
 * 2. DMA Pool single-sector (pooled buffers)
 * 3. DMA Pool multi-sector (batched + pooled)
 *
 * Metrics:
 * - Throughput (MB/s)
 * - IOPS (I/O operations per second)
 * - Latency (microseconds per operation)
 * - Pool utilization
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/ahci.h"
#include "../../kernel/include/dma_pool.h"
#include "../../kernel/include/time.h"
#include "../../kernel/include/mem.h"

// Forward declarations for AHCI functions
extern ahci_port_t* g_blk_port;
bool ahci_read_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);
bool ahci_write_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);
bool ahci_read_sectors_pooled(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);
bool ahci_write_sectors_pooled(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);
bool ahci_read_sectors_multi(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);
bool ahci_write_sectors_multi(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);

#define TEST_SECTOR_COUNT  1024   // 512KB
#define TEST_ITERATIONS    10
#define TEST_START_LBA     0x1000 // Safe test area

typedef struct {
    const char* name;
    uint64_t total_time_us;
    uint64_t total_bytes;
    uint32_t operations;
    double throughput_mbs;
    double iops;
    double latency_us;
} benchmark_result_t;

/**
 * Get current timestamp in microseconds
 */
static uint64_t get_time_us(void) {
    uint64_t ticks = timer_get_ticks();
    uint32_t freq = timer_get_frequency();
    if (freq == 0) freq = 1000;  // Default 1ms tick
    return (ticks * 1000000ULL) / freq;
}

/**
 * Run benchmark for a specific I/O function
 */
static void run_benchmark(benchmark_result_t* result, const char* name,
                         bool (*read_fn)(ahci_port_t*, uint64_t, uint32_t, void*),
                         bool (*write_fn)(ahci_port_t*, uint64_t, uint32_t, const void*)) {
    result->name = name;
    result->operations = 0;
    result->total_bytes = 0;
    result->total_time_us = 0;

    if (!g_blk_port || !g_blk_port->device_present) {
        kprintf("  [SKIP] No AHCI device available\n");
        return;
    }

    // Allocate test buffer
    void* buffer = pmm_alloc_pages(TEST_SECTOR_COUNT * 512 / PAGE_SIZE);
    if (!buffer) {
        kprintf("  [FAIL] Buffer allocation failed\n");
        return;
    }

    // Fill with test pattern
    for (uint32_t i = 0; i < TEST_SECTOR_COUNT * 512 / sizeof(uint32_t); i++) {
        ((uint32_t*)buffer)[i] = 0xDEADBEEF ^ i;
    }

    kprintf("  Running %s...\n", name);

    uint64_t start = get_time_us();

    for (int iter = 0; iter < TEST_ITERATIONS; iter++) {
        // Write test
        if (!write_fn(g_blk_port, TEST_START_LBA, TEST_SECTOR_COUNT, buffer)) {
            kprintf("    [WARN] Write failed at iteration %d\n", iter);
            continue;
        }

        // Read test
        memset(buffer, 0, TEST_SECTOR_COUNT * 512);
        if (!read_fn(g_blk_port, TEST_START_LBA, TEST_SECTOR_COUNT, buffer)) {
            kprintf("    [WARN] Read failed at iteration %d\n", iter);
            continue;
        }

        result->operations += 2;  // Read + Write
        result->total_bytes += TEST_SECTOR_COUNT * 512 * 2;
    }

    uint64_t end = get_time_us();
    result->total_time_us = end - start;

    // Calculate metrics
    if (result->total_time_us > 0) {
        double seconds = (double)result->total_time_us / 1000000.0;
        result->throughput_mbs = (double)result->total_bytes / (1024.0 * 1024.0) / seconds;
        result->iops = (double)(result->operations * TEST_SECTOR_COUNT) / seconds;
        result->latency_us = (double)result->total_time_us / (double)result->operations;
    }

    pmm_free_pages(buffer, TEST_SECTOR_COUNT * 512 / PAGE_SIZE);

    kprintf("    Time:       %llu us\n", result->total_time_us);
    kprintf("    Throughput: %.2f MB/s\n", result->throughput_mbs);
    kprintf("    IOPS:       %.0f\n", result->iops);
    kprintf("    Latency:    %.2f us/op\n", result->latency_us);
}

/**
 * Main benchmark entry point
 */
void test_dma_pool_benchmark(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  DMA Pool Benchmark\n");
    kprintf("========================================\n");
    kprintf("Test size: %u sectors (%u KB)\n", TEST_SECTOR_COUNT,
            TEST_SECTOR_COUNT / 2);
    kprintf("Iterations: %d\n", TEST_ITERATIONS);
    kprintf("\n");

    benchmark_result_t results[3];

    // Benchmark 1: Original AHCI (static dma_bounce)
    kprintf("[1] Original AHCI (static bounce buffer)\n");
    run_benchmark(&results[0], "Original",
                  ahci_read_sectors, ahci_write_sectors);

    // Benchmark 2: DMA Pool single-sector
    kprintf("\n[2] DMA Pool (single-sector)\n");
    run_benchmark(&results[1], "Pool-Single",
                  ahci_read_sectors_pooled, ahci_write_sectors_pooled);

    // Benchmark 3: DMA Pool multi-sector
    kprintf("\n[3] DMA Pool (multi-sector batching)\n");
    run_benchmark(&results[2], "Pool-Multi",
                  ahci_read_sectors_multi, ahci_write_sectors_multi);

    // Print comparison
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Results Summary\n");
    kprintf("========================================\n");
    kprintf("%-20s %10s %10s %10s\n", "Method", "MB/s", "IOPS", "us/op");
    kprintf("----------------------------------------\n");

    for (int i = 0; i < 3; i++) {
        if (results[i].operations > 0) {
            kprintf("%-20s %10.2f %10.0f %10.2f\n",
                    results[i].name,
                    results[i].throughput_mbs,
                    results[i].iops,
                    results[i].latency_us);
        }
    }

    // Calculate improvements
    if (results[0].throughput_mbs > 0) {
        double improvement_single = (results[1].throughput_mbs / results[0].throughput_mbs - 1.0) * 100.0;
        double improvement_multi = (results[2].throughput_mbs / results[0].throughput_mbs - 1.0) * 100.0;

        kprintf("\n");
        kprintf("Performance vs Original:\n");
        kprintf("  Pool-Single: %+.1f%%\n", improvement_single);
        kprintf("  Pool-Multi:  %+.1f%%\n", improvement_multi);
    }

    // Print DMA pool stats
    kprintf("\n");
    dma_pool_print_stats();
    kprintf("========================================\n");
}

/**
 * Test DMA pool basic functionality
 */
void test_dma_pool_basic(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  DMA Pool Basic Tests\n");
    kprintf("========================================\n");

    // Test 1: Allocate and free
    kprintf("[1] Allocate/Free Test\n");
    dma_buffer_t* buf1 = dma_buffer_alloc();
    if (!buf1) {
        kprintf("  [FAIL] Allocation failed\n");
        return;
    }
    kprintf("  Allocated: virt=%p phys=0x%llx size=%zu\n",
            dma_buffer_virt(buf1), dma_buffer_phys(buf1), dma_buffer_size(buf1));

    // Verify alignment
    if (dma_buffer_phys(buf1) % PAGE_SIZE != 0) {
        kprintf("  [FAIL] Buffer not 4KB aligned!\n");
    } else {
        kprintf("  [PASS] Buffer is 4KB aligned\n");
    }

    dma_buffer_free(buf1);
    kprintf("  [PASS] Free successful\n");

    // Test 2: Pool exhaustion
    kprintf("\n[2] Pool Exhaustion Test\n");
    dma_buffer_t* buffers[300];
    uint32_t allocated = 0;

    for (int i = 0; i < 300; i++) {
        buffers[i] = dma_buffer_alloc();
        if (buffers[i]) {
            allocated++;
        } else {
            break;
        }
    }

    kprintf("  Allocated %u buffers before exhaustion\n", allocated);
    kprintf("  Pool utilization: %u%%\n", dma_pool_utilization());

    // Free all
    for (uint32_t i = 0; i < allocated; i++) {
        dma_buffer_free(buffers[i]);
    }
    kprintf("  [PASS] Freed all buffers\n");

    // Test 3: 64KB buffers
    kprintf("\n[3] 64KB Buffer Test\n");
    dma_buffer_t* big_buf = dma_buffer_alloc_64k();
    if (big_buf) {
        kprintf("  Allocated: size=%zu\n", dma_buffer_size(big_buf));
        if (dma_buffer_size(big_buf) == 65536) {
            kprintf("  [PASS] 64KB buffer correct size\n");
        }
        dma_buffer_free(big_buf);
    } else {
        kprintf("  [INFO] 64KB pool not available\n");
    }

    dma_pool_print_stats();
    kprintf("========================================\n");
}
