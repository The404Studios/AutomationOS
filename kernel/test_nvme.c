/**
 * NVMe Driver Test Suite
 *
 * Comprehensive tests for NVMe storage driver including:
 * - Controller detection and initialization
 * - Namespace discovery
 * - Read/Write operations
 * - Performance benchmarks
 * - Error handling
 */

#include "kernel.h"
#include "nvme.h"
#include "mem.h"

// Test buffer size (4KB - one page)
#define TEST_BUFFER_SIZE 4096

// Test patterns
#define PATTERN_0x55 0x55
#define PATTERN_0xAA 0xAA
#define PATTERN_0xFF 0xFF
#define PATTERN_0x00 0x00

// Forward declarations
static void test_nvme_detection(void);
static void test_nvme_read_write(void);
static void test_nvme_sequential_io(void);
static void test_nvme_random_io(void);
static void test_nvme_flush(void);
static void test_nvme_error_handling(void);
static void nvme_benchmark(void);
static bool verify_buffer(uint8_t* buffer, size_t size, uint8_t pattern);
static void fill_buffer(uint8_t* buffer, size_t size, uint8_t pattern);

// External controller access (from nvme.c)
extern nvme_controller_t* g_nvme_controllers[8];
extern uint8_t g_num_controllers;

/**
 * Main NVMe test entry point
 */
void test_nvme(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("   NVMe Driver Test Suite v1.0\n");
    kprintf("========================================\n");
    kprintf("\n");

    // Initialize NVMe subsystem
    kprintf("[TEST] Initializing NVMe subsystem...\n");
    nvme_init();
    kprintf("\n");

    // Run tests
    test_nvme_detection();
    test_nvme_read_write();
    test_nvme_sequential_io();
    test_nvme_random_io();
    test_nvme_flush();
    test_nvme_error_handling();
    nvme_benchmark();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("   NVMe Test Suite Complete\n");
    kprintf("========================================\n");
    kprintf("\n");
}

/**
 * Test 1: NVMe Controller Detection
 */
static void test_nvme_detection(void) {
    kprintf("[TEST 1] NVMe Controller Detection\n");
    kprintf("-----------------------------------\n");

    if (g_num_controllers == 0) {
        kprintf("[FAIL] No NVMe controllers detected\n");
        kprintf("       Expected: At least 1 controller\n");
        kprintf("       Note: Run with QEMU: -drive file=nvme.img,if=none,id=nvm\n");
        kprintf("             -device nvme,serial=deadbeef,drive=nvm\n");
        return;
    }

    kprintf("[PASS] Detected %d NVMe controller(s)\n", g_num_controllers);

    // Print controller details
    for (uint8_t i = 0; i < g_num_controllers; i++) {
        nvme_controller_t* ctrl = g_nvme_controllers[i];
        kprintf("       Controller %d:\n", i);
        kprintf("         BAR0: %p\n", ctrl->bar);
        kprintf("         Version: 0x%08x\n", ctrl->vs);
        kprintf("         I/O Queues: %d\n", ctrl->num_io_queues);
        kprintf("         Namespaces: %d\n", ctrl->num_namespaces);

        // Print namespace info
        for (uint32_t ns = 0; ns < ctrl->num_namespaces; ns++) {
            if (ctrl->namespaces[ns].active) {
                nvme_namespace_t* namespace = &ctrl->namespaces[ns];
                uint64_t size_mb = namespace->capacity / (1024 * 1024);
                kprintf("         NS %d: %llu MB, Block Size: %u bytes\n",
                        namespace->nsid, size_mb, namespace->block_size);
            }
        }
    }

    kprintf("\n");
}

/**
 * Test 2: Basic Read/Write Operations
 */
static void test_nvme_read_write(void) {
    kprintf("[TEST 2] Basic Read/Write Operations\n");
    kprintf("-------------------------------------\n");

    if (g_num_controllers == 0) {
        kprintf("[SKIP] No NVMe controllers available\n\n");
        return;
    }

    nvme_controller_t* ctrl = g_nvme_controllers[0];

    // Find first active namespace
    nvme_namespace_t* ns = NULL;
    for (uint32_t i = 0; i < ctrl->num_namespaces; i++) {
        if (ctrl->namespaces[i].active) {
            ns = &ctrl->namespaces[i];
            break;
        }
    }

    if (!ns) {
        kprintf("[FAIL] No active namespaces found\n\n");
        return;
    }

    kprintf("       Using Namespace %d\n", ns->nsid);

    // Allocate test buffers
    uint8_t* write_buffer = (uint8_t*)kmalloc(TEST_BUFFER_SIZE);
    uint8_t* read_buffer = (uint8_t*)kmalloc(TEST_BUFFER_SIZE);

    if (!write_buffer || !read_buffer) {
        kprintf("[FAIL] Failed to allocate test buffers\n\n");
        if (write_buffer) kfree(write_buffer);
        if (read_buffer) kfree(read_buffer);
        return;
    }

    // Test pattern 0x55
    fill_buffer(write_buffer, TEST_BUFFER_SIZE, PATTERN_0x55);
    fill_buffer(read_buffer, TEST_BUFFER_SIZE, 0x00);

    // Calculate number of blocks (assuming 512-byte sectors)
    uint16_t block_count = (TEST_BUFFER_SIZE / 512) - 1; // 0-based

    // Write to LBA 0
    kprintf("       Writing pattern 0x55 to LBA 0...\n");
    int ret = nvme_write(ctrl, ns->nsid, 0, block_count, write_buffer);
    if (ret != 0) {
        kprintf("[FAIL] Write failed with error %d\n\n", ret);
        kfree(write_buffer);
        kfree(read_buffer);
        return;
    }

    // Read back from LBA 0
    kprintf("       Reading from LBA 0...\n");
    ret = nvme_read(ctrl, ns->nsid, 0, block_count, read_buffer);
    if (ret != 0) {
        kprintf("[FAIL] Read failed with error %d\n\n", ret);
        kfree(write_buffer);
        kfree(read_buffer);
        return;
    }

    // Verify data
    if (verify_buffer(read_buffer, TEST_BUFFER_SIZE, PATTERN_0x55)) {
        kprintf("[PASS] Read/Write pattern 0x55 successful\n");
    } else {
        kprintf("[FAIL] Data mismatch after read\n");
        kprintf("       Expected: 0x55, Got: 0x%02x\n", read_buffer[0]);
    }

    // Test pattern 0xAA
    fill_buffer(write_buffer, TEST_BUFFER_SIZE, PATTERN_0xAA);
    fill_buffer(read_buffer, TEST_BUFFER_SIZE, 0x00);

    kprintf("       Writing pattern 0xAA to LBA 100...\n");
    ret = nvme_write(ctrl, ns->nsid, 100, block_count, write_buffer);
    if (ret != 0) {
        kprintf("[FAIL] Write failed with error %d\n\n", ret);
        kfree(write_buffer);
        kfree(read_buffer);
        return;
    }

    kprintf("       Reading from LBA 100...\n");
    ret = nvme_read(ctrl, ns->nsid, 100, block_count, read_buffer);
    if (ret != 0) {
        kprintf("[FAIL] Read failed with error %d\n\n", ret);
        kfree(write_buffer);
        kfree(read_buffer);
        return;
    }

    if (verify_buffer(read_buffer, TEST_BUFFER_SIZE, PATTERN_0xAA)) {
        kprintf("[PASS] Read/Write pattern 0xAA successful\n");
    } else {
        kprintf("[FAIL] Data mismatch after read\n");
    }

    kfree(write_buffer);
    kfree(read_buffer);
    kprintf("\n");
}

/**
 * Test 3: Sequential I/O
 */
static void test_nvme_sequential_io(void) {
    kprintf("[TEST 3] Sequential I/O Operations\n");
    kprintf("----------------------------------\n");

    if (g_num_controllers == 0) {
        kprintf("[SKIP] No NVMe controllers available\n\n");
        return;
    }

    nvme_controller_t* ctrl = g_nvme_controllers[0];
    nvme_namespace_t* ns = NULL;

    for (uint32_t i = 0; i < ctrl->num_namespaces; i++) {
        if (ctrl->namespaces[i].active) {
            ns = &ctrl->namespaces[i];
            break;
        }
    }

    if (!ns) {
        kprintf("[SKIP] No active namespaces\n\n");
        return;
    }

    // Allocate buffers
    uint8_t* buffer = (uint8_t*)kmalloc(TEST_BUFFER_SIZE);
    if (!buffer) {
        kprintf("[FAIL] Failed to allocate buffer\n\n");
        return;
    }

    // Write 10 sequential blocks
    kprintf("       Writing 10 sequential 4KB blocks...\n");
    uint16_t block_count = (TEST_BUFFER_SIZE / 512) - 1;

    for (uint64_t lba = 1000; lba < 1010; lba++) {
        uint8_t pattern = (uint8_t)(lba & 0xFF);
        fill_buffer(buffer, TEST_BUFFER_SIZE, pattern);

        int ret = nvme_write(ctrl, ns->nsid, lba * 8, block_count, buffer);
        if (ret != 0) {
            kprintf("[FAIL] Sequential write failed at LBA %llu\n\n", lba * 8);
            kfree(buffer);
            return;
        }
    }

    // Read back and verify
    kprintf("       Reading and verifying...\n");
    bool pass = true;

    for (uint64_t lba = 1000; lba < 1010; lba++) {
        uint8_t pattern = (uint8_t)(lba & 0xFF);
        fill_buffer(buffer, TEST_BUFFER_SIZE, 0x00);

        int ret = nvme_read(ctrl, ns->nsid, lba * 8, block_count, buffer);
        if (ret != 0) {
            kprintf("[FAIL] Sequential read failed at LBA %llu\n", lba * 8);
            pass = false;
            break;
        }

        if (!verify_buffer(buffer, TEST_BUFFER_SIZE, pattern)) {
            kprintf("[FAIL] Data mismatch at LBA %llu\n", lba * 8);
            pass = false;
            break;
        }
    }

    if (pass) {
        kprintf("[PASS] Sequential I/O successful\n");
    }

    kfree(buffer);
    kprintf("\n");
}

/**
 * Test 4: Random I/O
 */
static void test_nvme_random_io(void) {
    kprintf("[TEST 4] Random I/O Operations\n");
    kprintf("------------------------------\n");

    if (g_num_controllers == 0) {
        kprintf("[SKIP] No NVMe controllers available\n\n");
        return;
    }

    nvme_controller_t* ctrl = g_nvme_controllers[0];
    nvme_namespace_t* ns = NULL;

    for (uint32_t i = 0; i < ctrl->num_namespaces; i++) {
        if (ctrl->namespaces[i].active) {
            ns = &ctrl->namespaces[i];
            break;
        }
    }

    if (!ns) {
        kprintf("[SKIP] No active namespaces\n\n");
        return;
    }

    uint8_t* buffer = (uint8_t*)kmalloc(TEST_BUFFER_SIZE);
    if (!buffer) {
        kprintf("[FAIL] Failed to allocate buffer\n\n");
        return;
    }

    // Random LBAs to test
    uint64_t test_lbas[] = {2000, 5000, 10000, 50000, 100000};
    uint16_t block_count = (TEST_BUFFER_SIZE / 512) - 1;

    kprintf("       Testing random I/O at 5 different LBAs...\n");
    bool pass = true;

    for (size_t i = 0; i < sizeof(test_lbas) / sizeof(test_lbas[0]); i++) {
        uint64_t lba = test_lbas[i];

        // Skip if LBA exceeds namespace size
        if (lba * 512 >= ns->capacity) {
            continue;
        }

        uint8_t pattern = (uint8_t)(i * 0x11);
        fill_buffer(buffer, TEST_BUFFER_SIZE, pattern);

        // Write
        int ret = nvme_write(ctrl, ns->nsid, lba, block_count, buffer);
        if (ret != 0) {
            kprintf("[FAIL] Random write failed at LBA %llu\n", lba);
            pass = false;
            break;
        }

        // Clear buffer
        fill_buffer(buffer, TEST_BUFFER_SIZE, 0x00);

        // Read
        ret = nvme_read(ctrl, ns->nsid, lba, block_count, buffer);
        if (ret != 0) {
            kprintf("[FAIL] Random read failed at LBA %llu\n", lba);
            pass = false;
            break;
        }

        // Verify
        if (!verify_buffer(buffer, TEST_BUFFER_SIZE, pattern)) {
            kprintf("[FAIL] Data mismatch at LBA %llu\n", lba);
            pass = false;
            break;
        }
    }

    if (pass) {
        kprintf("[PASS] Random I/O successful\n");
    }

    kfree(buffer);
    kprintf("\n");
}

/**
 * Test 5: Flush Command
 */
static void test_nvme_flush(void) {
    kprintf("[TEST 5] Flush Command\n");
    kprintf("----------------------\n");

    if (g_num_controllers == 0) {
        kprintf("[SKIP] No NVMe controllers available\n\n");
        return;
    }

    nvme_controller_t* ctrl = g_nvme_controllers[0];
    nvme_namespace_t* ns = NULL;

    for (uint32_t i = 0; i < ctrl->num_namespaces; i++) {
        if (ctrl->namespaces[i].active) {
            ns = &ctrl->namespaces[i];
            break;
        }
    }

    if (!ns) {
        kprintf("[SKIP] No active namespaces\n\n");
        return;
    }

    kprintf("       Issuing flush command to namespace %d...\n", ns->nsid);
    int ret = nvme_flush(ctrl, ns->nsid);

    if (ret == 0) {
        kprintf("[PASS] Flush command successful\n");
    } else {
        kprintf("[FAIL] Flush command failed with error %d\n", ret);
    }

    kprintf("\n");
}

/**
 * Test 6: Error Handling
 */
static void test_nvme_error_handling(void) {
    kprintf("[TEST 6] Error Handling\n");
    kprintf("-----------------------\n");

    if (g_num_controllers == 0) {
        kprintf("[SKIP] No NVMe controllers available\n\n");
        return;
    }

    nvme_controller_t* ctrl = g_nvme_controllers[0];

    // Test 1: Invalid namespace ID
    kprintf("       Testing invalid namespace ID (9999)...\n");
    uint8_t* buffer = (uint8_t*)kmalloc(TEST_BUFFER_SIZE);
    if (buffer) {
        int ret = nvme_read(ctrl, 9999, 0, 7, buffer);
        if (ret != 0) {
            kprintf("[PASS] Invalid namespace correctly rejected\n");
        } else {
            kprintf("[FAIL] Invalid namespace accepted\n");
        }
        kfree(buffer);
    }

    // Test 2: Out of bounds LBA (would require knowing namespace size)
    kprintf("       Testing out-of-bounds LBA...\n");
    kprintf("       (Skipped - requires namespace size check)\n");

    kprintf("\n");
}

/**
 * Performance Benchmark
 */
static void nvme_benchmark(void) {
    kprintf("[BENCHMARK] NVMe Performance Test\n");
    kprintf("----------------------------------\n");

    if (g_num_controllers == 0) {
        kprintf("[SKIP] No NVMe controllers available\n\n");
        return;
    }

    nvme_controller_t* ctrl = g_nvme_controllers[0];
    nvme_namespace_t* ns = NULL;

    for (uint32_t i = 0; i < ctrl->num_namespaces; i++) {
        if (ctrl->namespaces[i].active) {
            ns = &ctrl->namespaces[i];
            break;
        }
    }

    if (!ns) {
        kprintf("[SKIP] No active namespaces\n\n");
        return;
    }

    uint8_t* buffer = (uint8_t*)kmalloc(TEST_BUFFER_SIZE);
    if (!buffer) {
        kprintf("[FAIL] Failed to allocate buffer\n\n");
        return;
    }

    fill_buffer(buffer, TEST_BUFFER_SIZE, PATTERN_0x55);
    uint16_t block_count = (TEST_BUFFER_SIZE / 512) - 1;

    // Sequential Write Benchmark
    kprintf("       Sequential Write (1000 x 4KB blocks)...\n");
    uint64_t start_ticks = timer_get_ticks();

    for (int i = 0; i < 1000; i++) {
        nvme_write(ctrl, ns->nsid, 200000 + (i * 8), block_count, buffer);
    }

    uint64_t end_ticks = timer_get_ticks();
    uint64_t elapsed_ms = (end_ticks - start_ticks) * 1000 / timer_get_frequency();

    if (elapsed_ms > 0) {
        uint64_t throughput_kb = (1000 * 4) * 1000 / elapsed_ms;
        kprintf("       Time: %llu ms\n", elapsed_ms);
        kprintf("       Throughput: %llu KB/s (%llu MB/s)\n",
                throughput_kb, throughput_kb / 1024);
    } else {
        kprintf("       Time: < 1 ms (too fast to measure)\n");
    }

    // Sequential Read Benchmark
    kprintf("       Sequential Read (1000 x 4KB blocks)...\n");
    start_ticks = timer_get_ticks();

    for (int i = 0; i < 1000; i++) {
        nvme_read(ctrl, ns->nsid, 200000 + (i * 8), block_count, buffer);
    }

    end_ticks = timer_get_ticks();
    elapsed_ms = (end_ticks - start_ticks) * 1000 / timer_get_frequency();

    if (elapsed_ms > 0) {
        uint64_t throughput_kb = (1000 * 4) * 1000 / elapsed_ms;
        kprintf("       Time: %llu ms\n", elapsed_ms);
        kprintf("       Throughput: %llu KB/s (%llu MB/s)\n",
                throughput_kb, throughput_kb / 1024);
    } else {
        kprintf("       Time: < 1 ms (too fast to measure)\n");
    }

    kfree(buffer);
    kprintf("\n");
}

/**
 * Utility: Fill buffer with pattern
 */
static void fill_buffer(uint8_t* buffer, size_t size, uint8_t pattern) {
    for (size_t i = 0; i < size; i++) {
        buffer[i] = pattern;
    }
}

/**
 * Utility: Verify buffer contains pattern
 */
static bool verify_buffer(uint8_t* buffer, size_t size, uint8_t pattern) {
    for (size_t i = 0; i < size; i++) {
        if (buffer[i] != pattern) {
            return false;
        }
    }
    return true;
}
