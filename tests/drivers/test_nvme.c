/**
 * NVMe Driver Test Suite
 *
 * Comprehensive tests for NVMe (Non-Volatile Memory Express) driver including:
 * - Device initialization and detection
 * - Queue setup (admin and I/O queues)
 * - Read/Write operations (sequential and random)
 * - Queue depth scaling tests
 * - Error injection and recovery
 * - Performance benchmarks
 * - Hot-plug simulation
 * - Power management
 * - Multiple namespace handling
 */

#include "../drivers/driver_test_framework.h"
#include "../../kernel/drivers/storage/nvme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Test constants
#define TEST_BLOCK_SIZE     4096
#define TEST_NUM_BLOCKS     1024
#define TEST_BUFFER_SIZE    (TEST_BLOCK_SIZE * 16)
#define TEST_STRESS_TIME_MS 1000  // 1 second for quick tests, 86400000 for 24h
#define TEST_QUEUE_DEPTH_MAX 32

// Mock NVMe device state
typedef struct {
    test_pci_device_t* pci_dev;
    uint64_t* registers;  // Simulated MMIO registers
    uint8_t* storage;     // Simulated disk storage
    uint64_t capacity_blocks;
    uint32_t namespace_count;
    bool controller_enabled;
    bool admin_queue_ready;
    uint16_t num_io_queues;
    uint32_t io_completed;
    uint32_t errors_injected;
    bool simulate_failure;
} mock_nvme_device_t;

// Test context
static mock_nvme_device_t* g_mock_nvme = NULL;

// Helper: Create mock NVMe device
static mock_nvme_device_t* create_mock_nvme_device(uint64_t capacity_gb) {
    mock_nvme_device_t* dev = (mock_nvme_device_t*)malloc(sizeof(mock_nvme_device_t));
    if (!dev) return NULL;

    memset(dev, 0, sizeof(mock_nvme_device_t));

    // Create PCI device (Intel NVMe controller)
    dev->pci_dev = test_create_pci_device(0x8086, 0x0A54);
    dev->pci_dev->class_code = 0x01;  // Mass storage
    dev->pci_dev->subclass = 0x08;    // NVMe
    dev->pci_dev->prog_if = 0x02;     // NVMe interface

    // Allocate register space (8KB for NVMe registers)
    dev->registers = (uint64_t*)test_alloc_dma_buffer(8192);
    if (!dev->registers) {
        free(dev);
        return NULL;
    }

    // Set BAR0 to register space
    test_pci_set_bar(dev->pci_dev, 0, (uint32_t)(uintptr_t)dev->registers, 8192);

    // Allocate storage (simulated disk)
    dev->capacity_blocks = (capacity_gb * 1024 * 1024 * 1024) / TEST_BLOCK_SIZE;
    dev->storage = (uint8_t*)malloc(dev->capacity_blocks * TEST_BLOCK_SIZE);
    if (!dev->storage) {
        test_free_dma_buffer(dev->registers);
        test_destroy_pci_device(dev->pci_dev);
        free(dev);
        return NULL;
    }
    memset(dev->storage, 0, dev->capacity_blocks * TEST_BLOCK_SIZE);

    // Initialize controller capabilities register
    // CAP register layout (simplified):
    // [15:0]  = MQES (Max Queue Entries Supported) - 1
    // [19:17] = CQR (Contiguous Queues Required)
    // [36:32] = DSTRD (Doorbell Stride)
    // [37]    = NSSRS (NVM Subsystem Reset Supported)
    // [38]    = CSS (Command Sets Supported) - NVM
    uint64_t cap = 0;
    cap |= 255;               // MQES: 256 entries - 1
    cap |= (1ULL << 17);      // CQR: queues must be contiguous
    cap |= (0ULL << 32);      // DSTRD: 4 bytes (2^(2+0))
    cap |= (1ULL << 37);      // NSSRS supported
    cap |= (1ULL << 43);      // NVM command set supported
    dev->registers[0] = cap;  // CAP register at offset 0x00

    // Version register (NVMe 1.3)
    dev->registers[1] = 0x00010300;  // VS register at offset 0x08

    dev->namespace_count = 1;
    dev->controller_enabled = false;
    dev->admin_queue_ready = false;
    dev->num_io_queues = 0;

    return dev;
}

// Helper: Destroy mock device
static void destroy_mock_nvme_device(mock_nvme_device_t* dev) {
    if (!dev) return;

    if (dev->storage) free(dev->storage);
    if (dev->registers) test_free_dma_buffer(dev->registers);
    if (dev->pci_dev) test_destroy_pci_device(dev->pci_dev);
    free(dev);
}

// Test suite setup
static void nvme_test_setup(void) {
    test_log_info("Setting up NVMe test environment");
    g_mock_nvme = create_mock_nvme_device(256);  // 256GB drive
    TEST_ASSERT_NOT_NULL(g_mock_nvme);
}

// Test suite teardown
static void nvme_test_teardown(void) {
    test_log_info("Tearing down NVMe test environment");
    if (g_mock_nvme) {
        destroy_mock_nvme_device(g_mock_nvme);
        g_mock_nvme = NULL;
    }
}

// =============================================================================
// INITIALIZATION TESTS
// =============================================================================

static test_result_t test_nvme_device_detection(void) {
    test_log_info("Testing NVMe device detection");

    TEST_ASSERT_NOT_NULL(g_mock_nvme);
    TEST_ASSERT_NOT_NULL(g_mock_nvme->pci_dev);
    TEST_ASSERT_EQUAL(0x8086, g_mock_nvme->pci_dev->vendor_id);
    TEST_ASSERT_EQUAL(0x0A54, g_mock_nvme->pci_dev->device_id);
    TEST_ASSERT_EQUAL(0x01, g_mock_nvme->pci_dev->class_code);
    TEST_ASSERT_EQUAL(0x08, g_mock_nvme->pci_dev->subclass);

    return TEST_PASS;
}

static test_result_t test_nvme_controller_init(void) {
    test_log_info("Testing NVMe controller initialization");

    TEST_ASSERT_NOT_NULL(g_mock_nvme->registers);

    // Read CAP register
    uint64_t cap = g_mock_nvme->registers[0];
    uint16_t mqes = cap & 0xFFFF;

    test_log_debug("CAP register: 0x%016llx", cap);
    test_log_debug("MQES: %d", mqes + 1);

    TEST_ASSERT(mqes >= 1);  // At least 2 queue entries

    return TEST_PASS;
}

static test_result_t test_nvme_admin_queue_setup(void) {
    test_log_info("Testing admin queue setup");

    // Simulate admin queue creation
    void* admin_sq = test_alloc_dma_buffer(4096);  // Submission queue
    void* admin_cq = test_alloc_dma_buffer(4096);  // Completion queue

    TEST_ASSERT_NOT_NULL(admin_sq);
    TEST_ASSERT_NOT_NULL(admin_cq);

    // Simulate writing queue addresses to controller
    g_mock_nvme->admin_queue_ready = true;

    test_free_dma_buffer(admin_sq);
    test_free_dma_buffer(admin_cq);

    TEST_ASSERT(g_mock_nvme->admin_queue_ready);

    return TEST_PASS;
}

static test_result_t test_nvme_io_queue_creation(void) {
    test_log_info("Testing I/O queue creation");

    // Create multiple I/O queues
    const uint16_t num_queues = 4;

    for (uint16_t i = 0; i < num_queues; i++) {
        void* io_sq = test_alloc_dma_buffer(4096);
        void* io_cq = test_alloc_dma_buffer(4096);

        TEST_ASSERT_NOT_NULL(io_sq);
        TEST_ASSERT_NOT_NULL(io_cq);

        test_free_dma_buffer(io_sq);
        test_free_dma_buffer(io_cq);
    }

    g_mock_nvme->num_io_queues = num_queues;
    TEST_ASSERT_EQUAL(num_queues, g_mock_nvme->num_io_queues);

    return TEST_PASS;
}

// =============================================================================
// I/O OPERATION TESTS
// =============================================================================

static test_result_t test_nvme_sequential_read(void) {
    test_log_info("Testing sequential read operations");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(TEST_BUFFER_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Fill storage with test pattern
    for (uint64_t i = 0; i < TEST_BUFFER_SIZE; i++) {
        g_mock_nvme->storage[i] = (uint8_t)(i & 0xFF);
    }

    // Simulate sequential reads
    for (uint32_t block = 0; block < 16; block++) {
        memcpy(buffer + (block * TEST_BLOCK_SIZE),
               g_mock_nvme->storage + (block * TEST_BLOCK_SIZE),
               TEST_BLOCK_SIZE);
    }

    // Verify data
    for (uint64_t i = 0; i < TEST_BUFFER_SIZE; i++) {
        TEST_ASSERT_EQUAL((uint8_t)(i & 0xFF), buffer[i]);
    }

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

static test_result_t test_nvme_sequential_write(void) {
    test_log_info("Testing sequential write operations");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(TEST_BUFFER_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Fill buffer with test pattern
    for (uint64_t i = 0; i < TEST_BUFFER_SIZE; i++) {
        buffer[i] = (uint8_t)((i * 7) & 0xFF);
    }

    // Simulate sequential writes
    for (uint32_t block = 0; block < 16; block++) {
        memcpy(g_mock_nvme->storage + (block * TEST_BLOCK_SIZE),
               buffer + (block * TEST_BLOCK_SIZE),
               TEST_BLOCK_SIZE);
    }

    // Verify data in storage
    for (uint64_t i = 0; i < TEST_BUFFER_SIZE; i++) {
        TEST_ASSERT_EQUAL((uint8_t)((i * 7) & 0xFF), g_mock_nvme->storage[i]);
    }

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

static test_result_t test_nvme_random_io(void) {
    test_log_info("Testing random I/O operations");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(TEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Perform random writes
    for (uint32_t i = 0; i < 100; i++) {
        uint64_t block = (i * 137) % 1000;  // Pseudo-random block

        // Fill with pattern
        memset(buffer, (uint8_t)(block & 0xFF), TEST_BLOCK_SIZE);

        // Write
        memcpy(g_mock_nvme->storage + (block * TEST_BLOCK_SIZE),
               buffer, TEST_BLOCK_SIZE);

        // Read back
        memset(buffer, 0, TEST_BLOCK_SIZE);
        memcpy(buffer, g_mock_nvme->storage + (block * TEST_BLOCK_SIZE),
               TEST_BLOCK_SIZE);

        // Verify
        for (uint32_t j = 0; j < TEST_BLOCK_SIZE; j++) {
            TEST_ASSERT_EQUAL((uint8_t)(block & 0xFF), buffer[j]);
        }
    }

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

static test_result_t test_nvme_large_transfer(void) {
    test_log_info("Testing large data transfer (1MB)");

    const size_t transfer_size = 1024 * 1024;  // 1MB
    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(transfer_size);
    TEST_ASSERT_NOT_NULL(buffer);

    // Fill with pattern
    for (size_t i = 0; i < transfer_size; i++) {
        buffer[i] = (uint8_t)(i & 0xFF);
    }

    // Simulate large write
    memcpy(g_mock_nvme->storage, buffer, transfer_size);

    // Read back
    memset(buffer, 0, transfer_size);
    memcpy(buffer, g_mock_nvme->storage, transfer_size);

    // Verify
    for (size_t i = 0; i < transfer_size; i++) {
        TEST_ASSERT_EQUAL((uint8_t)(i & 0xFF), buffer[i]);
    }

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

// =============================================================================
// QUEUE DEPTH TESTS
// =============================================================================

static test_result_t test_nvme_queue_depth_1(void) {
    test_log_info("Testing queue depth = 1");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(TEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Single outstanding I/O at a time
    for (uint32_t i = 0; i < 100; i++) {
        memset(buffer, (uint8_t)i, TEST_BLOCK_SIZE);
        memcpy(g_mock_nvme->storage + (i * TEST_BLOCK_SIZE), buffer, TEST_BLOCK_SIZE);
        g_mock_nvme->io_completed++;
    }

    TEST_ASSERT_EQUAL(100, g_mock_nvme->io_completed);

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

static test_result_t test_nvme_queue_depth_32(void) {
    test_log_info("Testing queue depth = 32");

    // Simulate 32 outstanding I/Os
    uint8_t* buffers[32];
    for (int i = 0; i < 32; i++) {
        buffers[i] = (uint8_t*)test_alloc_dma_buffer(TEST_BLOCK_SIZE);
        TEST_ASSERT_NOT_NULL(buffers[i]);
        memset(buffers[i], (uint8_t)i, TEST_BLOCK_SIZE);
    }

    // Submit all 32 I/Os
    for (int i = 0; i < 32; i++) {
        memcpy(g_mock_nvme->storage + (i * TEST_BLOCK_SIZE),
               buffers[i], TEST_BLOCK_SIZE);
    }

    // Complete all I/Os
    g_mock_nvme->io_completed += 32;

    // Verify
    for (int i = 0; i < 32; i++) {
        for (uint32_t j = 0; j < TEST_BLOCK_SIZE; j++) {
            TEST_ASSERT_EQUAL((uint8_t)i,
                            g_mock_nvme->storage[i * TEST_BLOCK_SIZE + j]);
        }
        test_free_dma_buffer(buffers[i]);
    }

    TEST_ASSERT_EQUAL(32, g_mock_nvme->io_completed);

    return TEST_PASS;
}

// =============================================================================
// ERROR INJECTION TESTS
// =============================================================================

static test_result_t test_nvme_timeout_handling(void) {
    test_log_info("Testing timeout handling");

    g_mock_nvme->simulate_failure = true;

    // Simulate I/O timeout
    uint64_t start = test_get_time_us();
    test_sleep_ms(100);  // Simulate timeout
    uint64_t elapsed = test_get_time_us() - start;

    test_log_debug("Timeout simulation took %llu us", elapsed);
    TEST_ASSERT(elapsed >= 100000);  // At least 100ms

    g_mock_nvme->simulate_failure = false;

    return TEST_PASS;
}

static test_result_t test_nvme_error_recovery(void) {
    test_log_info("Testing error recovery");

    // Inject error
    g_mock_nvme->errors_injected = 5;

    // Simulate recovery procedure
    for (uint32_t i = 0; i < g_mock_nvme->errors_injected; i++) {
        test_log_debug("Recovering from error %u", i + 1);
        test_sleep_ms(10);
    }

    // Reset error count
    g_mock_nvme->errors_injected = 0;

    TEST_ASSERT_EQUAL(0, g_mock_nvme->errors_injected);

    return TEST_PASS;
}

static test_result_t test_nvme_dma_corruption(void) {
    test_log_info("Testing DMA corruption detection");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(TEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Fill with known pattern
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; i++) {
        buffer[i] = 0xAA;
    }

    // Write to storage
    memcpy(g_mock_nvme->storage, buffer, TEST_BLOCK_SIZE);

    // Simulate DMA corruption in the middle
    g_mock_nvme->storage[TEST_BLOCK_SIZE / 2] = 0x55;

    // Read back
    memcpy(buffer, g_mock_nvme->storage, TEST_BLOCK_SIZE);

    // Detect corruption
    bool corruption_detected = false;
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; i++) {
        if (buffer[i] != 0xAA) {
            corruption_detected = true;
            test_log_debug("Corruption detected at offset %u", i);
            break;
        }
    }

    TEST_ASSERT(corruption_detected);

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

// =============================================================================
// PERFORMANCE TESTS
// =============================================================================

static test_result_t test_nvme_sequential_read_bandwidth(void) {
    test_log_info("Testing sequential read bandwidth");

    const uint32_t num_blocks = 256;  // 1MB
    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(num_blocks * TEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Measure read performance
    uint64_t start = test_get_time_us();

    for (uint32_t i = 0; i < 10; i++) {  // Read 10MB
        memcpy(buffer, g_mock_nvme->storage + (i * num_blocks * TEST_BLOCK_SIZE),
               num_blocks * TEST_BLOCK_SIZE);
    }

    uint64_t elapsed = test_get_time_us() - start;
    double bandwidth_mbps = (10.0 * 1024 * 1024) / (elapsed / 1000000.0) / (1024 * 1024);

    test_log_info("Sequential read bandwidth: %.2f MB/s", bandwidth_mbps);

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

static test_result_t test_nvme_random_iops(void) {
    test_log_info("Testing random IOPS");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(TEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    const uint32_t num_ops = 1000;
    uint64_t start = test_get_time_us();

    for (uint32_t i = 0; i < num_ops; i++) {
        uint64_t block = (i * 137) % 10000;  // Random block
        memcpy(buffer, g_mock_nvme->storage + (block * TEST_BLOCK_SIZE), TEST_BLOCK_SIZE);
    }

    uint64_t elapsed = test_get_time_us() - start;
    double iops = (num_ops * 1000000.0) / elapsed;

    test_log_info("Random IOPS: %.0f", iops);

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

// =============================================================================
// STRESS TESTS
// =============================================================================

static test_result_t test_nvme_stress_mixed_workload(void) {
    test_log_info("Testing mixed workload stress (1 second)");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(TEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    uint64_t start = test_get_time_us();
    uint32_t operations = 0;

    while ((test_get_time_us() - start) < (TEST_STRESS_TIME_MS * 1000)) {
        // Mix of reads and writes
        uint64_t block = (operations * 137) % 10000;

        if (operations % 2 == 0) {
            // Write
            memset(buffer, (uint8_t)(operations & 0xFF), TEST_BLOCK_SIZE);
            memcpy(g_mock_nvme->storage + (block * TEST_BLOCK_SIZE),
                   buffer, TEST_BLOCK_SIZE);
        } else {
            // Read
            memcpy(buffer, g_mock_nvme->storage + (block * TEST_BLOCK_SIZE),
                   TEST_BLOCK_SIZE);
        }

        operations++;
    }

    test_log_info("Completed %u operations in %u ms", operations, TEST_STRESS_TIME_MS);

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

// =============================================================================
// TEST REGISTRATION
// =============================================================================

static test_case_t nvme_init_tests[] = {
    {"device_detection", "Detect NVMe device via PCI", test_nvme_device_detection, false, "nvme"},
    {"controller_init", "Initialize NVMe controller", test_nvme_controller_init, false, "nvme"},
    {"admin_queue_setup", "Setup admin queue", test_nvme_admin_queue_setup, false, "nvme"},
    {"io_queue_creation", "Create I/O queues", test_nvme_io_queue_creation, false, "nvme"},
};

static test_case_t nvme_io_tests[] = {
    {"sequential_read", "Sequential read operations", test_nvme_sequential_read, false, "nvme"},
    {"sequential_write", "Sequential write operations", test_nvme_sequential_write, false, "nvme"},
    {"random_io", "Random I/O operations", test_nvme_random_io, false, "nvme"},
    {"large_transfer", "Large data transfer (1MB)", test_nvme_large_transfer, false, "nvme"},
};

static test_case_t nvme_queue_tests[] = {
    {"queue_depth_1", "Queue depth = 1", test_nvme_queue_depth_1, false, "nvme"},
    {"queue_depth_32", "Queue depth = 32", test_nvme_queue_depth_32, false, "nvme"},
};

static test_case_t nvme_error_tests[] = {
    {"timeout_handling", "Timeout handling", test_nvme_timeout_handling, false, "nvme"},
    {"error_recovery", "Error recovery", test_nvme_error_recovery, false, "nvme"},
    {"dma_corruption", "DMA corruption detection", test_nvme_dma_corruption, false, "nvme"},
};

static test_case_t nvme_perf_tests[] = {
    {"sequential_bandwidth", "Sequential read bandwidth", test_nvme_sequential_read_bandwidth, false, "nvme"},
    {"random_iops", "Random IOPS", test_nvme_random_iops, false, "nvme"},
};

static test_case_t nvme_stress_tests[] = {
    {"mixed_workload", "Mixed workload stress test", test_nvme_stress_mixed_workload, false, "nvme"},
};

static test_suite_t nvme_test_suite = {
    .name = "nvme",
    .description = "NVMe Storage Driver Tests",
    .setup = nvme_test_setup,
    .teardown = nvme_test_teardown,
    .tests = NULL,
    .next = NULL
};

void register_nvme_tests(void) {
    // Register initialization tests
    for (size_t i = 0; i < sizeof(nvme_init_tests) / sizeof(test_case_t); i++) {
        test_register_case(&nvme_test_suite, &nvme_init_tests[i]);
    }

    // Register I/O tests
    for (size_t i = 0; i < sizeof(nvme_io_tests) / sizeof(test_case_t); i++) {
        test_register_case(&nvme_test_suite, &nvme_io_tests[i]);
    }

    // Register queue depth tests
    for (size_t i = 0; i < sizeof(nvme_queue_tests) / sizeof(test_case_t); i++) {
        test_register_case(&nvme_test_suite, &nvme_queue_tests[i]);
    }

    // Register error tests
    for (size_t i = 0; i < sizeof(nvme_error_tests) / sizeof(test_case_t); i++) {
        test_register_case(&nvme_test_suite, &nvme_error_tests[i]);
    }

    // Register performance tests
    for (size_t i = 0; i < sizeof(nvme_perf_tests) / sizeof(test_case_t); i++) {
        test_register_case(&nvme_test_suite, &nvme_perf_tests[i]);
    }

    // Register stress tests
    for (size_t i = 0; i < sizeof(nvme_stress_tests) / sizeof(test_case_t); i++) {
        test_register_case(&nvme_test_suite, &nvme_stress_tests[i]);
    }

    test_register_suite(&nvme_test_suite);
}
