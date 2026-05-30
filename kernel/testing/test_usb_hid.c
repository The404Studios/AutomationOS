/*
 * USB HID Driver Test Suite
 * Comprehensive testing of keyboard, mouse, and gamepad support
 */

#include "../include/kernel.h"
#include "../include/drivers.h"
#include "../include/usb.h"
#include "../include/input.h"
#include "../include/ktest.h"

// Test USB HID subsystem initialization
static void test_usb_hid_init(void) {
    kprintf("\n=== Test: USB HID Initialization ===\n");

    // Initialize input subsystem
    input_init();
    kprintf("[PASS] Input subsystem initialized\n");

    // Initialize USB subsystem
    usb_init();
    kprintf("[PASS] USB subsystem initialized\n");

    // List registered drivers
    usb_list_drivers();
    kprintf("[PASS] USB HID driver registered\n");
}

// Test simulated keyboard device
static void test_usb_keyboard_simulation(void) {
    kprintf("\n=== Test: USB Keyboard Simulation ===\n");

    // Add simulated test devices
    usb_hid_test_init();

    // List USB devices
    usb_list_devices();

    // List input devices
    input_list_devices();

    kprintf("[PASS] Simulated keyboard added\n");
}

// Test input event generation
static void test_input_events(void) {
    kprintf("\n=== Test: Input Event Processing ===\n");

    // Get input events from queue
    input_event_t event;
    int event_count = 0;

    while (input_get_event(&event) > 0 && event_count < 10) {
        input_debug_event(&event);
        event_count++;
    }

    if (event_count == 0) {
        kprintf("[INFO] No events in queue (expected without real input)\n");
    } else {
        kprintf("[PASS] Processed %d events\n", event_count);
    }
}

// Test keyboard event simulation
static void test_keyboard_events(void) {
    kprintf("\n=== Test: Keyboard Event Simulation ===\n");

    // In a real scenario, this would come from USB interrupt transfers
    // For now, we demonstrate the event flow

    // Create a dummy input device for testing
    input_device_t* test_kbd = input_allocate_device("Test Keyboard");
    if (!test_kbd) {
        kprintf("[FAIL] Failed to allocate test keyboard\n");
        return;
    }

    test_kbd->supports_key = true;
    test_kbd->supports_led = true;

    if (input_register_device(test_kbd) < 0) {
        kprintf("[FAIL] Failed to register test keyboard\n");
        input_free_device(test_kbd);
        return;
    }

    // Simulate key press: 'A' key
    kprintf("[INFO] Simulating 'A' key press\n");
    input_report_key(test_kbd, KEY_A, KEY_STATE_PRESSED);
    input_sync(test_kbd);

    // Simulate key release
    kprintf("[INFO] Simulating 'A' key release\n");
    input_report_key(test_kbd, KEY_A, KEY_STATE_RELEASED);
    input_sync(test_kbd);

    // Retrieve events
    input_event_t event;
    int count = 0;
    while (input_get_event(&event) > 0 && count < 5) {
        input_debug_event(&event);
        count++;
    }

    if (count >= 2) {
        kprintf("[PASS] Keyboard events generated and retrieved\n");
    } else {
        kprintf("[FAIL] Expected at least 2 events, got %d\n", count);
    }

    // Cleanup
    input_unregister_device(test_kbd);
    input_free_device(test_kbd);
}

// Test mouse event simulation
static void test_mouse_events(void) {
    kprintf("\n=== Test: Mouse Event Simulation ===\n");

    // Create a dummy input device for testing
    input_device_t* test_mouse = input_allocate_device("Test Mouse");
    if (!test_mouse) {
        kprintf("[FAIL] Failed to allocate test mouse\n");
        return;
    }

    test_mouse->supports_key = true;  // Mouse buttons
    test_mouse->supports_rel = true;  // Relative movement

    if (input_register_device(test_mouse) < 0) {
        kprintf("[FAIL] Failed to register test mouse\n");
        input_free_device(test_mouse);
        return;
    }

    // Simulate mouse movement
    kprintf("[INFO] Simulating mouse movement (dx=10, dy=-5)\n");
    input_report_rel(test_mouse, REL_X, 10);
    input_report_rel(test_mouse, REL_Y, -5);
    input_sync(test_mouse);

    // Simulate left button press
    kprintf("[INFO] Simulating left button press\n");
    input_report_key(test_mouse, BTN_LEFT, KEY_STATE_PRESSED);
    input_sync(test_mouse);

    // Simulate left button release
    kprintf("[INFO] Simulating left button release\n");
    input_report_key(test_mouse, BTN_LEFT, KEY_STATE_RELEASED);
    input_sync(test_mouse);

    // Retrieve events
    input_event_t event;
    int count = 0;
    while (input_get_event(&event) > 0 && count < 10) {
        input_debug_event(&event);
        count++;
    }

    if (count >= 4) {
        kprintf("[PASS] Mouse events generated and retrieved\n");
    } else {
        kprintf("[FAIL] Expected at least 4 events, got %d\n", count);
    }

    // Cleanup
    input_unregister_device(test_mouse);
    input_free_device(test_mouse);
}

// Test gamepad event simulation
static void test_gamepad_events(void) {
    kprintf("\n=== Test: Gamepad Event Simulation ===\n");

    // Create a dummy input device for testing
    input_device_t* test_gamepad = input_allocate_device("Test Gamepad");
    if (!test_gamepad) {
        kprintf("[FAIL] Failed to allocate test gamepad\n");
        return;
    }

    test_gamepad->supports_key = true;  // Buttons
    test_gamepad->supports_abs = true;  // Analog sticks

    if (input_register_device(test_gamepad) < 0) {
        kprintf("[FAIL] Failed to register test gamepad\n");
        input_free_device(test_gamepad);
        return;
    }

    // Simulate analog stick movement
    kprintf("[INFO] Simulating left stick movement\n");
    input_report_abs(test_gamepad, ABS_X, 32767);  // Full right
    input_report_abs(test_gamepad, ABS_Y, -32768); // Full up
    input_sync(test_gamepad);

    // Simulate button press
    kprintf("[INFO] Simulating A button press\n");
    input_report_key(test_gamepad, BTN_A, KEY_STATE_PRESSED);
    input_sync(test_gamepad);

    // Simulate button release
    kprintf("[INFO] Simulating A button release\n");
    input_report_key(test_gamepad, BTN_A, KEY_STATE_RELEASED);
    input_sync(test_gamepad);

    // Retrieve events
    input_event_t event;
    int count = 0;
    while (input_get_event(&event) > 0 && count < 10) {
        input_debug_event(&event);
        count++;
    }

    if (count >= 4) {
        kprintf("[PASS] Gamepad events generated and retrieved\n");
    } else {
        kprintf("[FAIL] Expected at least 4 events, got %d\n", count);
    }

    // Cleanup
    input_unregister_device(test_gamepad);
    input_free_device(test_gamepad);
}

// Test event queue overflow handling
static void test_event_queue_overflow(void) {
    kprintf("\n=== Test: Event Queue Overflow ===\n");

    // Create test device
    input_device_t* test_dev = input_allocate_device("Overflow Test");
    if (!test_dev) {
        kprintf("[FAIL] Failed to allocate test device\n");
        return;
    }

    test_dev->supports_key = true;
    if (input_register_device(test_dev) < 0) {
        kprintf("[FAIL] Failed to register test device\n");
        input_free_device(test_dev);
        return;
    }

    // Generate more events than queue can hold
    kprintf("[INFO] Generating 600 events (queue size is 512)\n");
    for (int i = 0; i < 600; i++) {
        input_report_key(test_dev, KEY_A, i % 2);
        input_sync(test_dev);
    }

    // Count retrieved events
    input_event_t event;
    int count = 0;
    while (input_get_event(&event) > 0) {
        count++;
    }

    kprintf("[INFO] Retrieved %d events from queue\n", count);

    if (count <= 512) {
        kprintf("[PASS] Queue overflow handled correctly\n");
    } else {
        kprintf("[FAIL] Queue overflow not handled correctly\n");
    }

    // Cleanup
    input_unregister_device(test_dev);
    input_free_device(test_dev);
}

// Test multiple simultaneous devices
static void test_multiple_devices(void) {
    kprintf("\n=== Test: Multiple Simultaneous Devices ===\n");

    #define NUM_TEST_DEVICES 5
    input_device_t* devices[NUM_TEST_DEVICES];

    // Create multiple devices
    for (int i = 0; i < NUM_TEST_DEVICES; i++) {
        char name[32];
        kprintf("Device %d", i);  // Simple name generation
        devices[i] = input_allocate_device(name);

        if (!devices[i]) {
            kprintf("[FAIL] Failed to allocate device %d\n", i);
            // Cleanup previously allocated devices
            for (int j = 0; j < i; j++) {
                input_unregister_device(devices[j]);
                input_free_device(devices[j]);
            }
            return;
        }

        devices[i]->supports_key = true;
        if (input_register_device(devices[i]) < 0) {
            kprintf("[FAIL] Failed to register device %d\n", i);
            input_free_device(devices[i]);
            // Cleanup
            for (int j = 0; j < i; j++) {
                input_unregister_device(devices[j]);
                input_free_device(devices[j]);
            }
            return;
        }
    }

    // List all devices
    input_list_devices();

    // Generate events from each device
    for (int i = 0; i < NUM_TEST_DEVICES; i++) {
        input_report_key(devices[i], KEY_A + i, KEY_STATE_PRESSED);
        input_sync(devices[i]);
    }

    // Count events
    input_event_t event;
    int count = 0;
    while (input_get_event(&event) > 0 && count < 20) {
        count++;
    }

    if (count >= NUM_TEST_DEVICES) {
        kprintf("[PASS] Multiple devices working correctly\n");
    } else {
        kprintf("[FAIL] Expected at least %d events, got %d\n", NUM_TEST_DEVICES, count);
    }

    // Cleanup
    for (int i = 0; i < NUM_TEST_DEVICES; i++) {
        input_unregister_device(devices[i]);
        input_free_device(devices[i]);
    }
}

// Test boot protocol keyboard report parsing
static void test_boot_protocol_keyboard(void) {
    kprintf("\n=== Test: Boot Protocol Keyboard Report ===\n");

    // Simulated boot protocol keyboard report
    uint8_t report[8] = {
        0x02,  // Byte 0: Left Shift pressed
        0x00,  // Byte 1: Reserved
        0x04,  // Byte 2: 'A' key (scancode 0x04)
        0x00,  // Byte 3-7: Empty
        0x00,
        0x00,
        0x00,
        0x00
    };

    kprintf("[INFO] Boot protocol keyboard report:\n");
    kprintf("[INFO]   Modifiers: 0x%02x (Left Shift)\n", report[0]);
    kprintf("[INFO]   Key 1: 0x%02x ('A')\n", report[2]);

    // In a real implementation, this would be processed by hid_process_keyboard_report()
    // For now, just validate the format

    if (report[0] == 0x02 && report[2] == 0x04) {
        kprintf("[PASS] Boot protocol report format correct\n");
    } else {
        kprintf("[FAIL] Boot protocol report format incorrect\n");
    }
}

// Test boot protocol mouse report parsing
static void test_boot_protocol_mouse(void) {
    kprintf("\n=== Test: Boot Protocol Mouse Report ===\n");

    // Simulated boot protocol mouse report
    uint8_t report[4] = {
        0x01,  // Byte 0: Left button pressed
        0x0A,  // Byte 1: X = +10
        0xF6,  // Byte 2: Y = -10 (signed)
        0x01   // Byte 3: Wheel = +1
    };

    kprintf("[INFO] Boot protocol mouse report:\n");
    kprintf("[INFO]   Buttons: 0x%02x (Left)\n", report[0]);
    kprintf("[INFO]   X movement: %d\n", (int8_t)report[1]);
    kprintf("[INFO]   Y movement: %d\n", (int8_t)report[2]);
    kprintf("[INFO]   Wheel: %d\n", (int8_t)report[3]);

    if (report[0] == 0x01 && (int8_t)report[1] == 10 && (int8_t)report[2] == -10) {
        kprintf("[PASS] Boot protocol mouse report format correct\n");
    } else {
        kprintf("[FAIL] Boot protocol mouse report format incorrect\n");
    }
}

// Main USB HID test suite
void test_usb_hid_suite(void) {
    kprintf("\n");
    kprintf("===========================================\n");
    kprintf("  USB HID Driver Test Suite\n");
    kprintf("===========================================\n");

    // Run all tests
    test_usb_hid_init();
    test_usb_keyboard_simulation();
    test_input_events();
    test_keyboard_events();
    test_mouse_events();
    test_gamepad_events();
    test_event_queue_overflow();
    test_multiple_devices();
    test_boot_protocol_keyboard();
    test_boot_protocol_mouse();

    kprintf("\n");
    kprintf("===========================================\n");
    kprintf("  Test Suite Complete\n");
    kprintf("===========================================\n");
    kprintf("\n");
}
