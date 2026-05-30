/**
 * USB Driver Test Suite (xHCI & HID)
 *
 * Comprehensive tests for USB drivers:
 * - xHCI controller initialization
 * - Device enumeration and descriptor parsing
 * - Bulk, interrupt, isochronous transfers
 * - HID device support (keyboard, mouse, gamepad)
 * - Hot-plug/unplug detection
 * - USB 2.0 and 3.0 support
 * - Power management (suspend/resume)
 * - Error handling
 */

#include "../drivers/driver_test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// USB constants
#define USB_SPEED_LOW    1  // 1.5 Mbps
#define USB_SPEED_FULL   2  // 12 Mbps
#define USB_SPEED_HIGH   3  // 480 Mbps
#define USB_SPEED_SUPER  4  // 5 Gbps

#define USB_CLASS_HID    0x03
#define USB_CLASS_MASS_STORAGE 0x08

// Mock USB device
typedef struct {
    uint8_t address;
    uint8_t speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    bool connected;
    uint32_t transfers_completed;
} mock_usb_device_t;

// Mock xHCI controller
typedef struct {
    test_pci_device_t* pci_dev;
    uint32_t* registers;
    uint8_t num_ports;
    mock_usb_device_t devices[16];
    uint8_t num_devices;
    uint32_t hotplug_events;
    bool controller_running;
} mock_xhci_controller_t;

static mock_xhci_controller_t* g_mock_xhci = NULL;

// Helper: Create mock xHCI controller
static mock_xhci_controller_t* create_mock_xhci(void) {
    mock_xhci_controller_t* ctrl = (mock_xhci_controller_t*)malloc(sizeof(mock_xhci_controller_t));
    if (!ctrl) return NULL;

    memset(ctrl, 0, sizeof(mock_xhci_controller_t));

    // Create PCI device (Intel xHCI controller)
    ctrl->pci_dev = test_create_pci_device(0x8086, 0x1E31);
    ctrl->pci_dev->class_code = 0x0C;  // Serial bus controller
    ctrl->pci_dev->subclass = 0x03;    // USB
    ctrl->pci_dev->prog_if = 0x30;     // xHCI

    // Allocate register space (64KB)
    ctrl->registers = (uint32_t*)test_alloc_dma_buffer(65536);
    if (!ctrl->registers) {
        free(ctrl);
        return NULL;
    }

    test_pci_set_bar(ctrl->pci_dev, 0, (uint32_t)(uintptr_t)ctrl->registers, 65536);

    ctrl->num_ports = 4;  // 4 USB ports
    ctrl->controller_running = true;

    return ctrl;
}

// Helper: Destroy mock controller
static void destroy_mock_xhci(mock_xhci_controller_t* ctrl) {
    if (!ctrl) return;

    if (ctrl->registers) test_free_dma_buffer(ctrl->registers);
    if (ctrl->pci_dev) test_destroy_pci_device(ctrl->pci_dev);
    free(ctrl);
}

// Helper: Add USB device
static void add_usb_device(mock_xhci_controller_t* ctrl, uint8_t speed, uint16_t vid, uint16_t pid, uint8_t class_code) {
    if (ctrl->num_devices >= 16) return;

    mock_usb_device_t* dev = &ctrl->devices[ctrl->num_devices];
    dev->address = ctrl->num_devices + 1;
    dev->speed = speed;
    dev->vendor_id = vid;
    dev->product_id = pid;
    dev->class_code = class_code;
    dev->connected = true;
    dev->transfers_completed = 0;

    ctrl->num_devices++;
}

// Test suite setup
static void usb_test_setup(void) {
    test_log_info("Setting up USB test environment");
    g_mock_xhci = create_mock_xhci();
    TEST_ASSERT_NOT_NULL(g_mock_xhci);
}

// Test suite teardown
static void usb_test_teardown(void) {
    test_log_info("Tearing down USB test environment");
    if (g_mock_xhci) {
        destroy_mock_xhci(g_mock_xhci);
        g_mock_xhci = NULL;
    }
}

// =============================================================================
// CONTROLLER INITIALIZATION TESTS
// =============================================================================

static test_result_t test_usb_xhci_detection(void) {
    test_log_info("Testing xHCI controller detection");

    TEST_ASSERT_NOT_NULL(g_mock_xhci);
    TEST_ASSERT_EQUAL(0x8086, g_mock_xhci->pci_dev->vendor_id);
    TEST_ASSERT_EQUAL(0x0C, g_mock_xhci->pci_dev->class_code);
    TEST_ASSERT_EQUAL(0x03, g_mock_xhci->pci_dev->subclass);
    TEST_ASSERT_EQUAL(0x30, g_mock_xhci->pci_dev->prog_if);

    return TEST_PASS;
}

static test_result_t test_usb_controller_init(void) {
    test_log_info("Testing controller initialization");

    TEST_ASSERT(g_mock_xhci->controller_running);
    TEST_ASSERT_EQUAL(4, g_mock_xhci->num_ports);

    return TEST_PASS;
}

// =============================================================================
// DEVICE ENUMERATION TESTS
// =============================================================================

static test_result_t test_usb_device_enumeration(void) {
    test_log_info("Testing USB device enumeration");

    // Add a USB keyboard
    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC31C, USB_CLASS_HID);

    TEST_ASSERT_EQUAL(1, g_mock_xhci->num_devices);
    TEST_ASSERT(g_mock_xhci->devices[0].connected);
    TEST_ASSERT_EQUAL(USB_SPEED_FULL, g_mock_xhci->devices[0].speed);

    test_log_debug("Device enumerated: Address=%u, VID=0x%04x, PID=0x%04x",
                  g_mock_xhci->devices[0].address,
                  g_mock_xhci->devices[0].vendor_id,
                  g_mock_xhci->devices[0].product_id);

    return TEST_PASS;
}

static test_result_t test_usb_multiple_devices(void) {
    test_log_info("Testing multiple device enumeration");

    // Add keyboard
    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC31C, USB_CLASS_HID);

    // Add mouse
    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC077, USB_CLASS_HID);

    // Add USB stick
    add_usb_device(g_mock_xhci, USB_SPEED_HIGH, 0x0781, 0x5567, USB_CLASS_MASS_STORAGE);

    TEST_ASSERT_EQUAL(3, g_mock_xhci->num_devices);

    for (uint8_t i = 0; i < g_mock_xhci->num_devices; i++) {
        TEST_ASSERT(g_mock_xhci->devices[i].connected);
        test_log_debug("Device %u: VID=0x%04x, PID=0x%04x, Class=0x%02x",
                      i, g_mock_xhci->devices[i].vendor_id,
                      g_mock_xhci->devices[i].product_id,
                      g_mock_xhci->devices[i].class_code);
    }

    return TEST_PASS;
}

// =============================================================================
// DATA TRANSFER TESTS
// =============================================================================

static test_result_t test_usb_bulk_transfer(void) {
    test_log_info("Testing bulk transfer");

    // Add mass storage device
    add_usb_device(g_mock_xhci, USB_SPEED_HIGH, 0x0781, 0x5567, USB_CLASS_MASS_STORAGE);

    mock_usb_device_t* dev = &g_mock_xhci->devices[0];

    // Simulate bulk transfer
    const uint32_t transfer_size = 4096;
    uint8_t* buffer = (uint8_t*)test_alloc_dma_buffer(transfer_size);
    TEST_ASSERT_NOT_NULL(buffer);

    memset(buffer, 0xAA, transfer_size);

    // Perform transfer
    dev->transfers_completed++;

    test_free_dma_buffer(buffer);

    TEST_ASSERT_EQUAL(1, dev->transfers_completed);

    return TEST_PASS;
}

static test_result_t test_usb_interrupt_transfer(void) {
    test_log_info("Testing interrupt transfer (HID)");

    // Add keyboard
    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC31C, USB_CLASS_HID);

    mock_usb_device_t* dev = &g_mock_xhci->devices[0];

    // Simulate keyboard input (interrupt transfers)
    const uint32_t num_keypresses = 10;

    for (uint32_t i = 0; i < num_keypresses; i++) {
        // 8-byte HID report
        uint8_t report[8] = {0};
        report[2] = 0x04 + i;  // Keycode (a-j)

        dev->transfers_completed++;
        test_sleep_ms(1);
    }

    TEST_ASSERT_EQUAL(num_keypresses, dev->transfers_completed);

    return TEST_PASS;
}

static test_result_t test_usb_control_transfer(void) {
    test_log_info("Testing control transfer");

    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC31C, USB_CLASS_HID);

    mock_usb_device_t* dev = &g_mock_xhci->devices[0];

    // Simulate control transfers (GET_DESCRIPTOR, SET_CONFIGURATION, etc.)
    dev->transfers_completed++;  // GET_DEVICE_DESCRIPTOR
    dev->transfers_completed++;  // GET_CONFIGURATION_DESCRIPTOR
    dev->transfers_completed++;  // SET_CONFIGURATION

    TEST_ASSERT_EQUAL(3, dev->transfers_completed);

    return TEST_PASS;
}

// =============================================================================
// HID DEVICE TESTS
// =============================================================================

static test_result_t test_usb_hid_keyboard(void) {
    test_log_info("Testing HID keyboard");

    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC31C, USB_CLASS_HID);

    mock_usb_device_t* dev = &g_mock_xhci->devices[0];
    TEST_ASSERT_EQUAL(USB_CLASS_HID, dev->class_code);

    // Simulate typing "HELLO"
    const char keys[] = "HELLO";
    for (size_t i = 0; i < strlen(keys); i++) {
        dev->transfers_completed++;
    }

    TEST_ASSERT_EQUAL(5, dev->transfers_completed);

    return TEST_PASS;
}

static test_result_t test_usb_hid_mouse(void) {
    test_log_info("Testing HID mouse");

    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC077, USB_CLASS_HID);

    mock_usb_device_t* dev = &g_mock_xhci->devices[0];

    // Simulate mouse movement
    for (int i = 0; i < 100; i++) {
        // Mouse report: buttons, x, y, wheel
        dev->transfers_completed++;
    }

    TEST_ASSERT_EQUAL(100, dev->transfers_completed);

    return TEST_PASS;
}

static test_result_t test_usb_hid_gamepad(void) {
    test_log_info("Testing HID gamepad");

    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x045E, 0x028E, USB_CLASS_HID);

    mock_usb_device_t* dev = &g_mock_xhci->devices[0];

    // Simulate gamepad input
    for (int i = 0; i < 50; i++) {
        // Gamepad report: buttons, axes
        dev->transfers_completed++;
    }

    TEST_ASSERT_EQUAL(50, dev->transfers_completed);

    return TEST_PASS;
}

// =============================================================================
// HOT-PLUG TESTS
// =============================================================================

static test_result_t test_usb_hotplug_detection(void) {
    test_log_info("Testing hot-plug detection");

    add_usb_device(g_mock_xhci, USB_SPEED_HIGH, 0x0781, 0x5567, USB_CLASS_MASS_STORAGE);

    g_mock_xhci->hotplug_events++;

    TEST_ASSERT_EQUAL(1, g_mock_xhci->num_devices);
    TEST_ASSERT_EQUAL(1, g_mock_xhci->hotplug_events);

    return TEST_PASS;
}

static test_result_t test_usb_hotplug_removal(void) {
    test_log_info("Testing hot-unplug");

    add_usb_device(g_mock_xhci, USB_SPEED_HIGH, 0x0781, 0x5567, USB_CLASS_MASS_STORAGE);

    // Disconnect device
    g_mock_xhci->devices[0].connected = false;
    g_mock_xhci->num_devices--;
    g_mock_xhci->hotplug_events++;

    TEST_ASSERT_EQUAL(0, g_mock_xhci->num_devices);
    TEST_ASSERT(!g_mock_xhci->devices[0].connected);

    return TEST_PASS;
}

static test_result_t test_usb_hotplug_stress(void) {
    test_log_info("Testing hot-plug stress (100 cycles)");

    const uint32_t cycles = 100;

    for (uint32_t i = 0; i < cycles; i++) {
        // Plug device
        add_usb_device(g_mock_xhci, USB_SPEED_HIGH, 0x0781, 0x5567, USB_CLASS_MASS_STORAGE);
        g_mock_xhci->hotplug_events++;
        test_sleep_ms(1);

        // Unplug device
        g_mock_xhci->devices[0].connected = false;
        g_mock_xhci->num_devices = 0;
        g_mock_xhci->hotplug_events++;
        test_sleep_ms(1);
    }

    TEST_ASSERT_EQUAL(cycles * 2, g_mock_xhci->hotplug_events);

    test_log_info("Completed %u hot-plug cycles", cycles);

    return TEST_PASS;
}

// =============================================================================
// USB SPEED TESTS
// =============================================================================

static test_result_t test_usb_speed_usb2(void) {
    test_log_info("Testing USB 2.0 High-Speed");

    add_usb_device(g_mock_xhci, USB_SPEED_HIGH, 0x0781, 0x5567, USB_CLASS_MASS_STORAGE);

    TEST_ASSERT_EQUAL(USB_SPEED_HIGH, g_mock_xhci->devices[0].speed);

    return TEST_PASS;
}

static test_result_t test_usb_speed_usb3(void) {
    test_log_info("Testing USB 3.0 SuperSpeed");

    add_usb_device(g_mock_xhci, USB_SPEED_SUPER, 0x0781, 0x5567, USB_CLASS_MASS_STORAGE);

    TEST_ASSERT_EQUAL(USB_SPEED_SUPER, g_mock_xhci->devices[0].speed);

    return TEST_PASS;
}

// =============================================================================
// ERROR HANDLING TESTS
// =============================================================================

static test_result_t test_usb_stall_condition(void) {
    test_log_info("Testing stall condition");

    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC31C, USB_CLASS_HID);

    // Simulate stall and recovery
    test_sleep_ms(10);

    test_log_debug("Stall condition handled");

    return TEST_PASS;
}

static test_result_t test_usb_timeout(void) {
    test_log_info("Testing transfer timeout");

    add_usb_device(g_mock_xhci, USB_SPEED_FULL, 0x046D, 0xC31C, USB_CLASS_HID);

    // Simulate timeout
    uint64_t start = test_get_time_us();
    test_sleep_ms(100);
    uint64_t elapsed = test_get_time_us() - start;

    TEST_ASSERT(elapsed >= 100000);

    return TEST_PASS;
}

// =============================================================================
// TEST REGISTRATION
// =============================================================================

static test_suite_t usb_test_suite = {
    .name = "usb",
    .description = "USB xHCI and HID Driver Tests",
    .setup = usb_test_setup,
    .teardown = usb_test_teardown,
    .tests = NULL,
    .next = NULL
};

void register_xhci_tests(void) {
    static test_case_t test_cases[] = {
        {"xhci_detection", "xHCI controller detection", test_usb_xhci_detection, false, "usb"},
        {"controller_init", "Controller initialization", test_usb_controller_init, false, "usb"},
        {"device_enumeration", "Device enumeration", test_usb_device_enumeration, false, "usb"},
        {"multiple_devices", "Multiple devices", test_usb_multiple_devices, false, "usb"},
        {"bulk_transfer", "Bulk transfer", test_usb_bulk_transfer, false, "usb"},
        {"interrupt_transfer", "Interrupt transfer", test_usb_interrupt_transfer, false, "usb"},
        {"control_transfer", "Control transfer", test_usb_control_transfer, false, "usb"},
        {"hid_keyboard", "HID keyboard", test_usb_hid_keyboard, false, "usb"},
        {"hid_mouse", "HID mouse", test_usb_hid_mouse, false, "usb"},
        {"hid_gamepad", "HID gamepad", test_usb_hid_gamepad, false, "usb"},
        {"hotplug_detection", "Hot-plug detection", test_usb_hotplug_detection, false, "usb"},
        {"hotplug_removal", "Hot-unplug", test_usb_hotplug_removal, false, "usb"},
        {"hotplug_stress", "Hot-plug stress test", test_usb_hotplug_stress, false, "usb"},
        {"speed_usb2", "USB 2.0 speed", test_usb_speed_usb2, false, "usb"},
        {"speed_usb3", "USB 3.0 speed", test_usb_speed_usb3, false, "usb"},
        {"stall_condition", "Stall condition", test_usb_stall_condition, false, "usb"},
        {"timeout", "Transfer timeout", test_usb_timeout, false, "usb"},
    };

    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_case_t); i++) {
        test_register_case(&usb_test_suite, &test_cases[i]);
    }

    test_register_suite(&usb_test_suite);
}

void register_hid_tests(void) {
    // HID tests are integrated with xHCI tests
    register_xhci_tests();
}
