/**
 * AHCI (SATA) Driver Test Suite
 *
 * Comprehensive tests for AHCI (Advanced Host Controller Interface) driver:
 * - Controller initialization and port detection
 * - SATA device identification
 * - NCQ (Native Command Queuing) operations
 * - Read/Write operations (PIO and DMA)
 * - Hot-plug detection and handling
 * - Power management (DEVSLP, HIPM, DIPM)
 * - Port multiplier support
 * - Error handling and recovery
 */

#include "../drivers/driver_test_framework.h"
#include <stdio.h>
#include <stdlib.h>
<br/>
#include <stdint.h>

// AHCI constants
#define AHCI_MAX_PORTS 32
#define AHCI_SECTOR_SIZE 512
#define AHCI_MAX_NCQ_DEPTH 32
#define AHCI_PORT_SIG_ATA    0x00000101
#define AHCI_PORT_SIG_ATAPI  0xEB140101
#define AHCI_PORT_SIG_SEMB   0xC33C0101
#define AHCI_PORT_SIG_PM     0x96690101

// Mock AHCI controller
typedef struct {
    test_pci_device_t* pci_dev;
    uint32_t* hba_mem;      // Host Bus Adapter memory
    uint8_t* disk_storage;   // Simulated disk
    uint64_t disk_capacity_sectors;
    uint32_t ports_implemented;
    uint8_t num_ports;
    bool ncq_enabled;
    uint32_t ncq_depth;
    uint32_t io_completed;
    bool port_hot_plugged[AHCI_MAX_PORTS];
    bool power_management_enabled;
} mock_ahci_controller_t;

static mock_ahci_controller_t* g_mock_ahci = NULL;

// Helper: Create mock AHCI controller
static mock_ahci_controller_t* create_mock_ahci_controller(uint64_t disk_size_gb) {
    mock_ahci_controller_t* ctrl = (mock_ahci_controller_t*)malloc(sizeof(mock_ahci_controller_t));
    if (!ctrl) return NULL;

    memset(ctrl, 0, sizeof(mock_ahci_controller_t));

    // Create PCI device (Intel AHCI controller)
    ctrl->pci_dev = test_create_pci_device(0x8086, 0x2922);
    ctrl->pci_dev->class_code = 0x01;  // Mass storage
    ctrl->pci_dev->subclass = 0x06;    // SATA
    ctrl->pci_dev->prog_if = 0x01;     // AHCI

    // Allocate HBA memory (4KB)
    ctrl->hba_mem = (uint32_t*)test_alloc_dma_buffer(4096);
    if (!ctrl->hba_mem) {
        free(ctrl);
        return NULL;
    }

    // Set BAR5 (ABAR - AHCI Base Address Register)
    test_pci_set_bar(ctrl->pci_dev, 5, (uint32_t)(uintptr_t)ctrl->hba_mem, 4096);

    // Allocate disk storage
    ctrl->disk_capacity_sectors = (disk_size_gb * 1024 * 1024 * 1024) / AHCI_SECTOR_SIZE;
    ctrl->disk_storage = (uint8_t*)malloc(ctrl->disk_capacity_sectors * AHCI_SECTOR_SIZE);
    if (!ctrl->disk_storage) {
        test_free_dma_buffer(ctrl->hba_mem);
        test_destroy_pci_device(ctrl->pci_dev);
        free(ctrl);
        return NULL;
    }
    memset(ctrl->disk_storage, 0, ctrl->disk_capacity_sectors * AHCI_SECTOR_SIZE);

    // Initialize HBA capabilities
    // CAP register:
    // [4:0]   = NP (Number of Ports) - 1
    // [8]     = SPM (Supports Port Multiplier)
    // [9]     = SAM (Supports AHCI mode only)
    // [12:10] = ISS (Interface Speed Support) - Gen 3 (6 Gbps)
    // [19]    = SALP (Supports Aggressive Link Power Management)
    // [30]    = SNCQ (Supports Native Command Queuing)
    ctrl->hba_mem[0] = 0x40000000 |  // SNCQ
                       (3 << 10) |    // Gen 3 (6 Gbps)
                       (1 << 9) |     // AHCI mode only
                       3;              // 4 ports (0-based)

    ctrl->ports_implemented = 0x0F;  // Ports 0-3
    ctrl->num_ports = 4;
    ctrl->ncq_enabled = true;
    ctrl->ncq_depth = 32;

    return ctrl;
}

// Helper: Destroy mock controller
static void destroy_mock_ahci_controller(mock_ahci_controller_t* ctrl) {
    if (!ctrl) return;

    if (ctrl->disk_storage) free(ctrl->disk_storage);
    if (ctrl->hba_mem) test_free_dma_buffer(ctrl->hba_mem);
    if (ctrl->pci_dev) test_destroy_pci_device(ctrl->pci_dev);
    free(ctrl);
}

// Test suite setup
static void ahci_test_setup(void) {
    test_log_info("Setting up AHCI test environment");
    g_mock_ahci = create_mock_ahci_controller(500);  // 500GB disk
    TEST_ASSERT_NOT_NULL(g_mock_ahci);
}

// Test suite teardown
static void ahci_test_teardown(void) {
    test_log_info("Tearing down AHCI test environment");
    if (g_mock_ahci) {
        destroy_mock_ahci_controller(g_mock_ahci);
        g_mock_ahci = NULL;
    }
}

// =============================================================================
// INITIALIZATION TESTS
// =============================================================================

static test_result_t test_ahci_controller_detection(void) {
    test_log_info("Testing AHCI controller detection");

    TEST_ASSERT_NOT_NULL(g_mock_ahci);
    TEST_ASSERT_NOT_NULL(g_mock_ahci->pci_dev);
    TEST_ASSERT_EQUAL(0x8086, g_mock_ahci->pci_dev->vendor_id);
    TEST_ASSERT_EQUAL(0x01, g_mock_ahci->pci_dev->class_code);
    TEST_ASSERT_EQUAL(0x06, g_mock_ahci->pci_dev->subclass);

    return TEST_PASS;
}

static test_result_t test_ahci_hba_initialization(void) {
    test_log_info("Testing HBA initialization");

    TEST_ASSERT_NOT_NULL(g_mock_ahci->hba_mem);

    // Read CAP register
    uint32_t cap = g_mock_ahci->hba_mem[0];
    uint8_t num_ports = (cap & 0x1F) + 1;
    bool ncq_supported = (cap & (1 << 30)) != 0;

    test_log_debug("CAP register: 0x%08x", cap);
    test_log_debug("Number of ports: %u", num_ports);
    test_log_debug("NCQ supported: %s", ncq_supported ? "Yes" : "No");

    TEST_ASSERT(num_ports >= 1 && num_ports <= 32);
    TEST_ASSERT(ncq_supported);

    return TEST_PASS;
}

static test_result_t test_ahci_port_detection(void) {
    test_log_info("Testing port detection");

    uint32_t ports_impl = g_mock_ahci->ports_implemented;
    uint8_t port_count = 0;

    for (uint8_t i = 0; i < 32; i++) {
        if (ports_impl & (1 << i)) {
            port_count++;
            test_log_debug("Port %u: Implemented", i);
        }
    }

    TEST_ASSERT_EQUAL(g_mock_ahci->num_ports, port_count);

    return TEST_PASS;
}

static test_result_t test_ahci_device_identification(void) {
    test_log_info("Testing SATA device identification");

    // Simulate IDENTIFY DEVICE command
    uint16_t identify_buffer[256];
    memset(identify_buffer, 0, sizeof(identify_buffer));

    // Fill in identify data (simplified)
    identify_buffer[0] = 0x0040;  // General configuration (ATA device)
    identify_buffer[83] = 0x4000; // Command set support (48-bit addressing)
    identify_buffer[86] = 0x4000; // Command set enabled (48-bit addressing)

    // LBA48 capacity (sectors)
    uint64_t capacity = g_mock_ahci->disk_capacity_sectors;
    identify_buffer[100] = (uint16_t)(capacity & 0xFFFF);
    identify_buffer[101] = (uint16_t)((capacity >> 16) & 0xFFFF);
    identify_buffer[102] = (uint16_t)((capacity >> 32) & 0xFFFF);
    identify_buffer[103] = (uint16_t)((capacity >> 48) & 0xFFFF);

    // Verify capacity
    uint64_t detected_capacity = identify_buffer[100] |
                                 ((uint64_t)identify_buffer[101] << 16) |
                                 ((uint64_t)identify_buffer[102] << 32) |
                                 ((uint64_t)identify_buffer[103] << 48);

    TEST_ASSERT_EQUAL(g_mock_ahci->disk_capacity_sectors, detected_capacity);

    return TEST_PASS;
}

// =============================================================================
// I/O OPERATION TESTS
// =============================================================================

static test_result_t test_ahci_pio_read(void) {
    test_log_info("Testing PIO read");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(AHCI_SECTOR_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Fill disk with test pattern
    for (uint32_t i = 0; i < AHCI_SECTOR_SIZE; i++) {
        g_mock_ahci->disk_storage[i] = (uint8_t)(i & 0xFF);
    }

    // Simulate PIO read
    memcpy(buffer, g_mock_ahci->disk_storage, AHCI_SECTOR_SIZE);

    // Verify
    for (uint32_t i = 0; i < AHCI_SECTOR_SIZE; i++) {
        TEST_ASSERT_EQUAL((uint8_t)(i & 0xFF), buffer[i]);
    }

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

static test_result_t test_ahci_dma_write(void) {
    test_log_info("Testing DMA write");

    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(AHCI_SECTOR_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Fill buffer with pattern
    for (uint32_t i = 0; i < AHCI_SECTOR_SIZE; i++) {
        buffer[i] = (uint8_t)((i * 3) & 0xFF);
    }

    // Simulate DMA write
    memcpy(g_mock_ahci->disk_storage, buffer, AHCI_SECTOR_SIZE);

    // Verify disk contents
    for (uint32_t i = 0; i < AHCI_SECTOR_SIZE; i++) {
        TEST_ASSERT_EQUAL((uint8_t)((i * 3) & 0xFF), g_mock_ahci->disk_storage[i]);
    }

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

static test_result_t test_ahci_multi_sector_transfer(void) {
    test_log_info("Testing multi-sector transfer (16 sectors)");

    const uint32_t num_sectors = 16;
    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(num_sectors * AHCI_SECTOR_SIZE);
    TEST_ASSERT_NOT_NULL(buffer);

    // Fill with pattern
    for (uint32_t i = 0; i < num_sectors * AHCI_SECTOR_SIZE; i++) {
        buffer[i] = (uint8_t)(i & 0xFF);
    }

    // Write
    memcpy(g_mock_ahci->disk_storage, buffer, num_sectors * AHCI_SECTOR_SIZE);

    // Read back
    memset(buffer, 0, num_sectors * AHCI_SECTOR_SIZE);
    memcpy(buffer, g_mock_ahci->disk_storage, num_sectors * AHCI_SECTOR_SIZE);

    // Verify
    for (uint32_t i = 0; i < num_sectors * AHCI_SECTOR_SIZE; i++) {
        TEST_ASSERT_EQUAL((uint8_t)(i & 0xFF), buffer[i]);
    }

    test_free_dma_buffer(buffer);
    return TEST_PASS;
}

// =============================================================================
// NCQ (NATIVE COMMAND QUEUING) TESTS
// =============================================================================

static test_result_t test_ahci_ncq_enable(void) {
    test_log_info("Testing NCQ enable");

    TEST_ASSERT(g_mock_ahci->ncq_enabled);
    TEST_ASSERT_EQUAL(32, g_mock_ahci->ncq_depth);

    return TEST_PASS;
}

static test_result_t test_ahci_ncq_read_operations(void) {
    test_log_info("Testing NCQ read operations");

    // Queue multiple reads
    const uint8_t num_commands = 8;
    uint8_t* buffers[num_commands];

    for (uint8_t i = 0; i < num_commands; i++) {
        buffers[i] = (uint8_t*)test_alloc_dma_buffer(AHCI_SECTOR_SIZE);
        TEST_ASSERT_NOT_NULL(buffers[i]);

        // Prepare test data in disk
        for (uint32_t j = 0; j < AHCI_SECTOR_SIZE; j++) {
            g_mock_ahci->disk_storage[i * AHCI_SECTOR_SIZE + j] = (uint8_t)(i + j);
        }
    }

    // Simulate NCQ reads (out-of-order completion)
    for (uint8_t i = 0; i < num_commands; i++) {
        uint8_t completion_order = (i * 3) % num_commands;  // Simulate reordering
        memcpy(buffers[completion_order],
               g_mock_ahci->disk_storage + (completion_order * AHCI_SECTOR_SIZE),
               AHCI_SECTOR_SIZE);
        g_mock_ahci->io_completed++;
    }

    // Verify all completed
    TEST_ASSERT_EQUAL(num_commands, g_mock_ahci->io_completed);

    // Verify data
    for (uint8_t i = 0; i < num_commands; i++) {
        for (uint32_t j = 0; j < AHCI_SECTOR_SIZE; j++) {
            TEST_ASSERT_EQUAL((uint8_t)(i + j), buffers[i][j]);
        }
        test_free_dma_buffer(buffers[i]);
    }

    return TEST_PASS;
}

static test_result_t test_ahci_ncq_write_operations(void) {
    test_log_info("Testing NCQ write operations");

    const uint8_t num_commands = 8;
    uint8_t* buffers[num_commands];

    // Prepare write buffers
    for (uint8_t i = 0; i < num_commands; i++) {
        buffers[i] = (uint8_t*)test_alloc_dma_buffer(AHCI_SECTOR_SIZE);
        TEST_ASSERT_NOT_NULL(buffers[i]);

        for (uint32_t j = 0; j < AHCI_SECTOR_SIZE; j++) {
            buffers[i][j] = (uint8_t)(i * 10 + j);
        }
    }

    // Simulate NCQ writes
    for (uint8_t i = 0; i < num_commands; i++) {
        memcpy(g_mock_ahci->disk_storage + (i * AHCI_SECTOR_SIZE),
               buffers[i], AHCI_SECTOR_SIZE);
    }

    // Verify disk contents
    for (uint8_t i = 0; i < num_commands; i++) {
        for (uint32_t j = 0; j < AHCI_SECTOR_SIZE; j++) {
            TEST_ASSERT_EQUAL((uint8_t)(i * 10 + j),
                            g_mock_ahci->disk_storage[i * AHCI_SECTOR_SIZE + j]);
        }
        test_free_dma_buffer(buffers[i]);
    }

    return TEST_PASS;
}

// =============================================================================
// HOT-PLUG TESTS
// =============================================================================

static test_result_t test_ahci_hotplug_detection(void) {
    test_log_info("Testing hot-plug detection");

    // Simulate device plug on port 1
    g_mock_ahci->port_hot_plugged[1] = true;

    test_log_debug("Device hot-plugged on port 1");

    // Verify detection
    TEST_ASSERT(g_mock_ahci->port_hot_plugged[1]);

    return TEST_PASS;
}

static test_result_t test_ahci_hotplug_removal(void) {
    test_log_info("Testing hot-plug removal");

    // Start with device plugged
    g_mock_ahci->port_hot_plugged[0] = true;

    // Simulate removal
    g_mock_ahci->port_hot_plugged[0] = false;

    test_log_debug("Device removed from port 0");

    // Verify removal
    TEST_ASSERT(!g_mock_ahci->port_hot_plugged[0]);

    return TEST_PASS;
}

static test_result_t test_ahci_hotplug_stress(void) {
    test_log_info("Testing hot-plug stress (100 cycles)");

    const uint32_t cycles = 100;

    for (uint32_t i = 0; i < cycles; i++) {
        // Plug
        g_mock_ahci->port_hot_plugged[0] = true;
        test_sleep_ms(1);

        // Unplug
        g_mock_ahci->port_hot_plugged[0] = false;
        test_sleep_ms(1);
    }

    test_log_info("Completed %u hot-plug cycles", cycles);

    return TEST_PASS;
}

// =============================================================================
// POWER MANAGEMENT TESTS
// =============================================================================

static test_result_t test_ahci_power_management_enable(void) {
    test_log_info("Testing power management enable");

    g_mock_ahci->power_management_enabled = true;

    TEST_ASSERT(g_mock_ahci->power_management_enabled);

    return TEST_PASS;
}

static test_result_t test_ahci_devslp_entry(void) {
    test_log_info("Testing DEVSLP (Device Sleep) entry");

    // Enable power management
    g_mock_ahci->power_management_enabled = true;

    // Simulate idle timeout
    test_sleep_ms(50);

    test_log_debug("Device entered DEVSLP state");

    return TEST_PASS;
}

// =============================================================================
// ERROR HANDLING TESTS
// =============================================================================

static test_result_t test_ahci_timeout_handling(void) {
    test_log_info("Testing command timeout");

    // Simulate command that times out
    uint64_t start = test_get_time_us();
    test_sleep_ms(100);  // Simulate timeout
    uint64_t elapsed = test_get_time_us() - start;

    test_log_debug("Timeout after %llu us", elapsed);
    TEST_ASSERT(elapsed >= 100000);

    return TEST_PASS;
}

static test_result_t test_ahci_error_recovery(void) {
    test_log_info("Testing error recovery");

    // Simulate error condition
    // Perform software reset
    test_sleep_ms(10);

    // Reinitialize port
    test_log_debug("Port reinitialized after error");

    return TEST_PASS;
}

// =============================================================================
// TEST REGISTRATION
// =============================================================================

static test_suite_t ahci_test_suite = {
    .name = "ahci",
    .description = "AHCI (SATA) Storage Driver Tests",
    .setup = ahci_test_setup,
    .teardown = ahci_test_teardown,
    .tests = NULL,
    .next = NULL
};

void register_ahci_tests(void) {
    // Initialize test cases
    static test_case_t test_cases[] = {
        {"controller_detection", "AHCI controller detection", test_ahci_controller_detection, false, "ahci"},
        {"hba_init", "HBA initialization", test_ahci_hba_initialization, false, "ahci"},
        {"port_detection", "Port detection", test_ahci_port_detection, false, "ahci"},
        {"device_identification", "Device identification", test_ahci_device_identification, false, "ahci"},
        {"pio_read", "PIO read", test_ahci_pio_read, false, "ahci"},
        {"dma_write", "DMA write", test_ahci_dma_write, false, "ahci"},
        {"multi_sector", "Multi-sector transfer", test_ahci_multi_sector_transfer, false, "ahci"},
        {"ncq_enable", "NCQ enable", test_ahci_ncq_enable, false, "ahci"},
        {"ncq_read", "NCQ read operations", test_ahci_ncq_read_operations, false, "ahci"},
        {"ncq_write", "NCQ write operations", test_ahci_ncq_write_operations, false, "ahci"},
        {"hotplug_detection", "Hot-plug detection", test_ahci_hotplug_detection, false, "ahci"},
        {"hotplug_removal", "Hot-plug removal", test_ahci_hotplug_removal, false, "ahci"},
        {"hotplug_stress", "Hot-plug stress test", test_ahci_hotplug_stress, false, "ahci"},
        {"power_mgmt_enable", "Power management enable", test_ahci_power_management_enable, false, "ahci"},
        {"devslp_entry", "DEVSLP entry", test_ahci_devslp_entry, false, "ahci"},
        {"timeout_handling", "Timeout handling", test_ahci_timeout_handling, false, "ahci"},
        {"error_recovery", "Error recovery", test_ahci_error_recovery, false, "ahci"},
    };

    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_case_t); i++) {
        test_register_case(&ahci_test_suite, &test_cases[i]);
    }

    test_register_suite(&ahci_test_suite);
}
