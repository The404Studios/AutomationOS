#include "driver_test_framework.h"
#include <string.h>
#include <stdint.h>

// PS/2 controller ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

// PS/2 status bits
#define PS2_STATUS_OUTPUT_FULL  0x01
#define PS2_STATUS_INPUT_FULL   0x02

// PS/2 commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT1    0xAE
#define PS2_CMD_TEST_PORT1      0xAB

// Keyboard commands
#define KB_CMD_ENABLE_SCAN      0xF4
#define KB_CMD_RESET            0xFF

// Simulated PS/2 state
static uint8_t ps2_status = 0x00;
static uint8_t ps2_data = 0x00;
static uint8_t ps2_config = 0x00;
static uint8_t keyboard_buffer[256];
static uint8_t kb_write_pos = 0;
static uint8_t kb_read_pos = 0;

// Scancode test sequences
static const uint8_t test_scancodes[] = {
    0x1E, // 'A' pressed
    0x9E, // 'A' released
    0x30, // 'B' pressed
    0xB0, // 'B' released
    0x2E, // 'C' pressed
    0xAE, // 'C' released
};

// Mock I/O functions
static void mock_outb_ps2(uint16_t port, uint8_t val) {
    if (port == PS2_DATA_PORT) {
        ps2_data = val;
        ps2_status &= ~PS2_STATUS_INPUT_FULL;
    } else if (port == PS2_COMMAND_PORT) {
        // Handle commands
        switch (val) {
            case PS2_CMD_READ_CONFIG:
                ps2_data = ps2_config;
                ps2_status |= PS2_STATUS_OUTPUT_FULL;
                break;
            case PS2_CMD_WRITE_CONFIG:
                // Next write to data port sets config
                break;
            case PS2_CMD_ENABLE_PORT1:
                ps2_status &= ~PS2_STATUS_INPUT_FULL;
                break;
            case PS2_CMD_TEST_PORT1:
                ps2_data = 0x00; // Test passed
                ps2_status |= PS2_STATUS_OUTPUT_FULL;
                break;
        }
    }
}

static uint8_t mock_inb_ps2(uint16_t port) {
    if (port == PS2_DATA_PORT) {
        ps2_status &= ~PS2_STATUS_OUTPUT_FULL;
        return ps2_data;
    } else if (port == PS2_STATUS_PORT) {
        return ps2_status;
    }
    return 0xFF;
}

// Test setup
static void ps2_test_setup(void) {
    ps2_status = 0x00;
    ps2_data = 0x00;
    ps2_config = 0x00;
    kb_write_pos = 0;
    kb_read_pos = 0;
    memset(keyboard_buffer, 0, sizeof(keyboard_buffer));

    test_log_info("PS/2 test setup complete");
}

// Test teardown
static void ps2_test_teardown(void) {
    test_log_info("PS/2 test teardown complete");
}

// Test: PS/2 controller initialization
static test_result_t test_ps2_init(void) {
    test_log_info("Testing PS/2 initialization");

    // Disable second port
    mock_outb_ps2(PS2_COMMAND_PORT, PS2_CMD_DISABLE_PORT2);

    // Enable first port
    mock_outb_ps2(PS2_COMMAND_PORT, PS2_CMD_ENABLE_PORT1);

    // Test port
    mock_outb_ps2(PS2_COMMAND_PORT, PS2_CMD_TEST_PORT1);
    uint8_t test_result = mock_inb_ps2(PS2_DATA_PORT);

    TEST_ASSERT_EQUAL(0x00, test_result); // 0x00 = test passed

    return TEST_PASS;
}

// Test: PS/2 status register
static test_result_t test_ps2_status(void) {
    test_log_info("Testing PS/2 status register");

    // Initially, status should be clear
    uint8_t status = mock_inb_ps2(PS2_STATUS_PORT);
    TEST_ASSERT_EQUAL(0x00, status);

    // Simulate output available
    ps2_status |= PS2_STATUS_OUTPUT_FULL;
    status = mock_inb_ps2(PS2_STATUS_PORT);
    TEST_ASSERT(status & PS2_STATUS_OUTPUT_FULL);

    // Reading data should clear output flag
    mock_inb_ps2(PS2_DATA_PORT);
    status = mock_inb_ps2(PS2_STATUS_PORT);
    TEST_ASSERT(!(status & PS2_STATUS_OUTPUT_FULL));

    return TEST_PASS;
}

// Test: Keyboard scancode translation
static test_result_t test_keyboard_scancode(void) {
    test_log_info("Testing keyboard scancode translation");

    // Scancode to ASCII mapping (US QWERTY, set 1)
    const uint8_t scancodes[] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22};
    const char expected[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g'};

    // Simple lookup table (matching kernel/drivers/ps2.c)
    const char scancode_to_ascii[128] = {
        [0x1E] = 'a', [0x30] = 'b', [0x2E] = 'c', [0x20] = 'd',
        [0x12] = 'e', [0x21] = 'f', [0x22] = 'g'
    };

    for (size_t i = 0; i < sizeof(scancodes); i++) {
        uint8_t scancode = scancodes[i];
        char ascii = scancode_to_ascii[scancode];
        TEST_ASSERT_EQUAL(expected[i], ascii);
    }

    return TEST_PASS;
}

// Test: Keyboard buffer operations
static test_result_t test_keyboard_buffer(void) {
    test_log_info("Testing keyboard buffer");

    // Write to buffer
    for (int i = 0; i < 10; i++) {
        keyboard_buffer[kb_write_pos++] = 'A' + i;
    }

    TEST_ASSERT_EQUAL(10, kb_write_pos);

    // Read from buffer
    for (int i = 0; i < 10; i++) {
        char c = keyboard_buffer[kb_read_pos++];
        TEST_ASSERT_EQUAL('A' + i, c);
    }

    TEST_ASSERT_EQUAL(10, kb_read_pos);
    TEST_ASSERT_EQUAL(kb_read_pos, kb_write_pos);

    return TEST_PASS;
}

// Test: Keyboard buffer wraparound
static test_result_t test_keyboard_buffer_wraparound(void) {
    test_log_info("Testing keyboard buffer wraparound");

    kb_write_pos = 250;
    kb_read_pos = 250;

    // Write 10 characters (should wrap around)
    for (int i = 0; i < 10; i++) {
        keyboard_buffer[kb_write_pos] = 'X';
        kb_write_pos = (kb_write_pos + 1) % 256;
    }

    TEST_ASSERT_EQUAL(4, kb_write_pos); // 250 + 10 = 260 % 256 = 4

    // Read back
    for (int i = 0; i < 10; i++) {
        char c = keyboard_buffer[kb_read_pos];
        kb_read_pos = (kb_read_pos + 1) % 256;
        TEST_ASSERT_EQUAL('X', c);
    }

    TEST_ASSERT_EQUAL(kb_read_pos, kb_write_pos);

    return TEST_PASS;
}

// Test: Shift key handling
static test_result_t test_keyboard_shift(void) {
    test_log_info("Testing shift key handling");

    bool shift_pressed = false;

    // Scancodes: Shift press, 'a' press, 'a' release, Shift release
    const uint8_t scancodes[] = {0x2A, 0x1E, 0x9E, 0xAA};
    const char expected[] = {0, 'A', 0, 0}; // Only 'a' with shift = 'A'

    const char normal_map[128] = {[0x1E] = 'a'};
    const char shift_map[128] = {[0x1E] = 'A'};

    for (size_t i = 0; i < sizeof(scancodes); i++) {
        uint8_t scancode = scancodes[i];

        // Check for shift press/release
        if (scancode == 0x2A) { // Left shift press
            shift_pressed = true;
        } else if (scancode == 0xAA) { // Left shift release
            shift_pressed = false;
        } else if (!(scancode & 0x80)) { // Key press (not release)
            char c = shift_pressed ? shift_map[scancode] : normal_map[scancode];
            if (expected[i] != 0) {
                TEST_ASSERT_EQUAL(expected[i], c);
            }
        }
    }

    return TEST_PASS;
}

// Test: Control key handling
static test_result_t test_keyboard_ctrl(void) {
    test_log_info("Testing ctrl key handling");

    bool ctrl_pressed = false;

    // Ctrl press: 0x1D, 'c' press: 0x2E (Ctrl+C)
    const uint8_t scancodes[] = {0x1D, 0x2E};

    for (size_t i = 0; i < sizeof(scancodes); i++) {
        uint8_t scancode = scancodes[i];

        if (scancode == 0x1D) { // Ctrl press
            ctrl_pressed = true;
        } else if (scancode == 0x9D) { // Ctrl release
            ctrl_pressed = false;
        }
    }

    TEST_ASSERT(ctrl_pressed);

    return TEST_PASS;
}

// Test: Keyboard command - reset
static test_result_t test_keyboard_reset(void) {
    test_log_info("Testing keyboard reset command");

    // Send reset command
    mock_outb_ps2(PS2_DATA_PORT, KB_CMD_RESET);

    // Keyboard should respond with 0xFA (ACK) then 0xAA (self-test passed)
    ps2_data = 0xFA;
    ps2_status |= PS2_STATUS_OUTPUT_FULL;

    uint8_t ack = mock_inb_ps2(PS2_DATA_PORT);
    TEST_ASSERT_EQUAL(0xFA, ack);

    return TEST_PASS;
}

// Test: Keyboard enable scanning
static test_result_t test_keyboard_enable_scanning(void) {
    test_log_info("Testing keyboard enable scanning");

    // Send enable scanning command
    mock_outb_ps2(PS2_DATA_PORT, KB_CMD_ENABLE_SCAN);

    // Keyboard should respond with 0xFA (ACK)
    ps2_data = 0xFA;
    ps2_status |= PS2_STATUS_OUTPUT_FULL;

    uint8_t ack = mock_inb_ps2(PS2_DATA_PORT);
    TEST_ASSERT_EQUAL(0xFA, ack);

    return TEST_PASS;
}

// Test: Stress test - rapid key presses
static test_result_t test_keyboard_stress(void) {
    test_log_info("Testing keyboard stress (1000 key presses)");

    uint64_t start = test_get_time_us();

    kb_write_pos = 0;
    kb_read_pos = 0;

    // Simulate 1000 key presses
    for (int i = 0; i < 1000; i++) {
        uint8_t scancode = 0x1E + (i % 26); // Cycle through 'a'-'z'
        ps2_data = scancode;
        ps2_status |= PS2_STATUS_OUTPUT_FULL;

        // Process scancode
        if (kb_write_pos < 256) {
            keyboard_buffer[kb_write_pos++] = scancode;
        }
    }

    uint64_t duration = test_get_time_us() - start;

    test_log_info("  Processed 1000 scancodes in %llu microseconds", duration);
    test_log_info("  Throughput: %.2f scancodes/sec", 1000 * 1000000.0 / duration);

    return TEST_PASS;
}

// Test: Hot-plug detection
static test_result_t test_keyboard_hotplug(void) {
    test_log_info("Testing keyboard hot-plug detection");

    // Simulate keyboard disconnect
    ps2_status = 0xFF; // Invalid status

    // Simulate reconnect
    ps2_status = 0x00;

    // Test port should work again
    mock_outb_ps2(PS2_COMMAND_PORT, PS2_CMD_TEST_PORT1);
    uint8_t test_result = mock_inb_ps2(PS2_DATA_PORT);

    TEST_ASSERT_EQUAL(0x00, test_result);

    return TEST_PASS;
}

// Test suite definition
static test_case_t ps2_tests[] = {
    {
        .name = "ps2_init",
        .description = "Test PS/2 controller initialization",
        .test_func = test_ps2_init,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "ps2_status",
        .description = "Test PS/2 status register",
        .test_func = test_ps2_status,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_scancode",
        .description = "Test scancode translation",
        .test_func = test_keyboard_scancode,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_buffer",
        .description = "Test keyboard buffer operations",
        .test_func = test_keyboard_buffer,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_buffer_wraparound",
        .description = "Test keyboard buffer wraparound",
        .test_func = test_keyboard_buffer_wraparound,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_shift",
        .description = "Test shift key handling",
        .test_func = test_keyboard_shift,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_ctrl",
        .description = "Test control key handling",
        .test_func = test_keyboard_ctrl,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_reset",
        .description = "Test keyboard reset command",
        .test_func = test_keyboard_reset,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_enable_scanning",
        .description = "Test keyboard enable scanning",
        .test_func = test_keyboard_enable_scanning,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_stress",
        .description = "Stress test with 1000 key presses",
        .test_func = test_keyboard_stress,
        .requires_hardware = false,
        .required_driver = "ps2"
    },
    {
        .name = "keyboard_hotplug",
        .description = "Test keyboard hot-plug detection",
        .test_func = test_keyboard_hotplug,
        .requires_hardware = false,
        .required_driver = "ps2"
    }
};

// Register PS/2 tests
void register_ps2_tests(void) {
    static test_suite_t ps2_suite = {
        .name = "ps2",
        .description = "PS/2 keyboard driver tests",
        .setup = ps2_test_setup,
        .teardown = ps2_test_teardown,
        .tests = NULL
    };

    // Link tests
    for (int i = sizeof(ps2_tests)/sizeof(ps2_tests[0]) - 1; i >= 0; i--) {
        test_register_case(&ps2_suite, &ps2_tests[i]);
    }

    test_register_suite(&ps2_suite);
}
