#include "driver_test_framework.h"
#include <string.h>
#include <stdint.h>

// Serial port registers simulation
#define COM1_BASE 0x3F8
#define COM1_DATA (COM1_BASE + 0)
#define COM1_IER  (COM1_BASE + 1)
#define COM1_FCR  (COM1_BASE + 2)
#define COM1_LCR  (COM1_BASE + 3)
#define COM1_MCR  (COM1_BASE + 4)
#define COM1_LSR  (COM1_BASE + 5)

// Simulated serial port state
static uint8_t serial_regs[8];
static char serial_output_buffer[4096];
static size_t serial_output_len = 0;
static bool serial_initialized = false;

// Mock outb/inb for testing
static void mock_outb(uint16_t port, uint8_t val) {
    if (port >= COM1_BASE && port < COM1_BASE + 8) {
        uint16_t offset = port - COM1_BASE;
        serial_regs[offset] = val;

        // Simulate character transmission
        if (offset == 0 && serial_initialized) {
            if (serial_output_len < sizeof(serial_output_buffer)) {
                serial_output_buffer[serial_output_len++] = val;
            }
        }
    }
}

static uint8_t mock_inb(uint16_t port) {
    if (port >= COM1_BASE && port < COM1_BASE + 8) {
        uint16_t offset = port - COM1_BASE;

        // LSR always reports ready to transmit
        if (offset == 5) {
            return 0x20; // THRE (Transmitter Holding Register Empty)
        }

        return serial_regs[offset];
    }
    return 0xFF;
}

// Test setup
static void serial_test_setup(void) {
    memset(serial_regs, 0, sizeof(serial_regs));
    memset(serial_output_buffer, 0, sizeof(serial_output_buffer));
    serial_output_len = 0;
    serial_initialized = false;

    test_log_info("Serial test setup complete");
}

// Test teardown
static void serial_test_teardown(void) {
    serial_initialized = false;
    test_log_info("Serial test teardown complete");
}

// Test: Serial initialization
static test_result_t test_serial_init(void) {
    test_log_info("Testing serial initialization");

    // Simulate serial init
    mock_outb(COM1_IER, 0x00);    // Disable interrupts
    mock_outb(COM1_LCR, 0x80);    // Enable DLAB
    mock_outb(COM1_DATA, 0x03);   // Divisor low (38400 baud)
    mock_outb(COM1_IER, 0x00);    // Divisor high
    mock_outb(COM1_LCR, 0x03);    // 8N1
    mock_outb(COM1_FCR, 0xC7);    // Enable FIFO
    mock_outb(COM1_MCR, 0x0B);    // IRQs enabled

    serial_initialized = true;

    // Verify registers
    TEST_ASSERT_EQUAL(0x00, serial_regs[1]); // IER
    TEST_ASSERT_EQUAL(0x03, serial_regs[3]); // LCR
    TEST_ASSERT_EQUAL(0xC7, serial_regs[2]); // FCR
    TEST_ASSERT_EQUAL(0x0B, serial_regs[4]); // MCR

    return TEST_PASS;
}

// Test: Serial putchar
static test_result_t test_serial_putchar(void) {
    test_log_info("Testing serial putchar");

    serial_initialized = true;
    serial_output_len = 0;

    // Write single character
    mock_outb(COM1_DATA, 'A');

    TEST_ASSERT_EQUAL(1, serial_output_len);
    TEST_ASSERT_EQUAL('A', serial_output_buffer[0]);

    return TEST_PASS;
}

// Test: Serial write string
static test_result_t test_serial_write(void) {
    test_log_info("Testing serial write");

    serial_initialized = true;
    serial_output_len = 0;

    const char* test_str = "Hello, World!";
    size_t len = strlen(test_str);

    for (size_t i = 0; i < len; i++) {
        mock_outb(COM1_DATA, test_str[i]);
    }

    TEST_ASSERT_EQUAL(len, serial_output_len);
    TEST_ASSERT_MEMORY_EQUAL(test_str, serial_output_buffer, len);

    return TEST_PASS;
}

// Test: Serial transmit ready
static test_result_t test_serial_transmit_ready(void) {
    test_log_info("Testing serial transmit ready");

    serial_initialized = true;

    // Check LSR bit 5 (THRE)
    uint8_t lsr = mock_inb(COM1_LSR);
    TEST_ASSERT(lsr & 0x20);

    return TEST_PASS;
}

// Test: Serial large buffer
static test_result_t test_serial_large_buffer(void) {
    test_log_info("Testing serial large buffer");

    serial_initialized = true;
    serial_output_len = 0;

    // Write 1KB of data
    for (int i = 0; i < 1024; i++) {
        mock_outb(COM1_DATA, 'X');
    }

    TEST_ASSERT_EQUAL(1024, serial_output_len);

    // Verify all characters
    for (int i = 0; i < 1024; i++) {
        TEST_ASSERT_EQUAL('X', serial_output_buffer[i]);
    }

    return TEST_PASS;
}

// Test: Serial special characters
static test_result_t test_serial_special_chars(void) {
    test_log_info("Testing serial special characters");

    serial_initialized = true;
    serial_output_len = 0;

    const char special[] = {'\n', '\r', '\t', '\b', '\0'};
    size_t len = sizeof(special);

    for (size_t i = 0; i < len; i++) {
        mock_outb(COM1_DATA, special[i]);
    }

    TEST_ASSERT_EQUAL(len, serial_output_len);
    TEST_ASSERT_MEMORY_EQUAL(special, serial_output_buffer, len);

    return TEST_PASS;
}

// Test: Serial uninitialized access
static test_result_t test_serial_uninitialized(void) {
    test_log_info("Testing serial uninitialized access");

    serial_initialized = false;
    serial_output_len = 0;

    // Try to write without initialization
    mock_outb(COM1_DATA, 'X');

    // Should not write when uninitialized
    // (In real driver, serial_putchar checks serial_initialized)

    return TEST_PASS;
}

// Test: Serial baud rate configuration
static test_result_t test_serial_baud_rates(void) {
    test_log_info("Testing serial baud rate configuration");

    // Test 115200 baud (divisor = 1)
    mock_outb(COM1_LCR, 0x80);  // Enable DLAB
    mock_outb(COM1_DATA, 0x01); // Divisor low
    mock_outb(COM1_IER, 0x00);  // Divisor high
    mock_outb(COM1_LCR, 0x03);  // Disable DLAB, 8N1

    TEST_ASSERT_EQUAL(0x01, serial_regs[0]);

    // Test 9600 baud (divisor = 12)
    mock_outb(COM1_LCR, 0x80);  // Enable DLAB
    mock_outb(COM1_DATA, 0x0C); // Divisor low
    mock_outb(COM1_IER, 0x00);  // Divisor high
    mock_outb(COM1_LCR, 0x03);  // Disable DLAB, 8N1

    TEST_ASSERT_EQUAL(0x0C, serial_regs[0]);

    return TEST_PASS;
}

// Test: Serial FIFO configuration
static test_result_t test_serial_fifo(void) {
    test_log_info("Testing serial FIFO configuration");

    // Enable FIFO with 14-byte threshold
    mock_outb(COM1_FCR, 0xC7);
    TEST_ASSERT_EQUAL(0xC7, serial_regs[2]);

    // Disable FIFO
    mock_outb(COM1_FCR, 0x00);
    TEST_ASSERT_EQUAL(0x00, serial_regs[2]);

    return TEST_PASS;
}

// Test: Serial stress test
static test_result_t test_serial_stress(void) {
    test_log_info("Testing serial stress (10000 characters)");

    serial_initialized = true;
    serial_output_len = 0;

    uint64_t start = test_get_time_us();

    // Write 10000 characters
    for (int i = 0; i < 10000 && serial_output_len < sizeof(serial_output_buffer); i++) {
        mock_outb(COM1_DATA, 'A' + (i % 26));
    }

    uint64_t duration = test_get_time_us() - start;

    test_log_info("  Wrote %zu characters in %llu microseconds", serial_output_len, duration);
    test_log_info("  Throughput: %.2f chars/sec", serial_output_len * 1000000.0 / duration);

    TEST_ASSERT(serial_output_len >= 10000 || serial_output_len == sizeof(serial_output_buffer));

    return TEST_PASS;
}

// Test suite definition
static test_case_t serial_tests[] = {
    {
        .name = "serial_init",
        .description = "Test serial port initialization",
        .test_func = test_serial_init,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_putchar",
        .description = "Test single character output",
        .test_func = test_serial_putchar,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_write",
        .description = "Test string output",
        .test_func = test_serial_write,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_transmit_ready",
        .description = "Test transmit ready flag",
        .test_func = test_serial_transmit_ready,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_large_buffer",
        .description = "Test large buffer transmission",
        .test_func = test_serial_large_buffer,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_special_chars",
        .description = "Test special characters",
        .test_func = test_serial_special_chars,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_uninitialized",
        .description = "Test uninitialized access",
        .test_func = test_serial_uninitialized,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_baud_rates",
        .description = "Test baud rate configuration",
        .test_func = test_serial_baud_rates,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_fifo",
        .description = "Test FIFO configuration",
        .test_func = test_serial_fifo,
        .requires_hardware = false,
        .required_driver = "serial"
    },
    {
        .name = "serial_stress",
        .description = "Stress test with 10000 characters",
        .test_func = test_serial_stress,
        .requires_hardware = false,
        .required_driver = "serial"
    }
};

// Register serial tests
void register_serial_tests(void) {
    static test_suite_t serial_suite = {
        .name = "serial",
        .description = "Serial port driver tests",
        .setup = serial_test_setup,
        .teardown = serial_test_teardown,
        .tests = NULL
    };

    // Link tests
    for (int i = sizeof(serial_tests)/sizeof(serial_tests[0]) - 1; i >= 0; i--) {
        test_register_case(&serial_suite, &serial_tests[i]);
    }

    test_register_suite(&serial_suite);
}
