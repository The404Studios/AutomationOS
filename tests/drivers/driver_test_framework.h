#ifndef DRIVER_TEST_FRAMEWORK_H
#define DRIVER_TEST_FRAMEWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Test result codes
typedef enum {
    TEST_PASS = 0,
    TEST_FAIL = 1,
    TEST_SKIP = 2,
    TEST_ERROR = 3
} test_result_t;

// Test statistics
typedef struct {
    uint32_t tests_run;
    uint32_t tests_passed;
    uint32_t tests_failed;
    uint32_t tests_skipped;
    uint32_t tests_errored;
    uint64_t total_time_us;
} test_stats_t;

// Test case structure
typedef struct test_case {
    const char* name;
    const char* description;
    test_result_t (*test_func)(void);
    bool requires_hardware;
    const char* required_driver;
    struct test_case* next;
} test_case_t;

// Test suite structure
typedef struct test_suite {
    const char* name;
    const char* description;
    void (*setup)(void);
    void (*teardown)(void);
    test_case_t* tests;
    struct test_suite* next;
} test_suite_t;

// Mock device interface
typedef struct {
    const char* name;
    void* device_data;
    int (*init)(void* device_data);
    int (*cleanup)(void* device_data);
    int (*read)(void* device_data, void* buffer, size_t size);
    int (*write)(void* device_data, const void* buffer, size_t size);
    int (*ioctl)(void* device_data, uint32_t cmd, void* arg);
} mock_device_t;

// Test framework API
void test_framework_init(void);
void test_register_suite(test_suite_t* suite);
void test_register_case(test_suite_t* suite, test_case_t* test);
test_result_t test_run_all(void);
test_result_t test_run_suite(const char* suite_name);
test_result_t test_run_case(const char* suite_name, const char* case_name);
void test_print_stats(test_stats_t* stats);
test_stats_t* test_get_stats(void);

// Mock device API
mock_device_t* mock_device_create(const char* name);
void mock_device_destroy(mock_device_t* device);
int mock_device_register(mock_device_t* device);
int mock_device_unregister(mock_device_t* device);

// Assertion macros
#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            test_log_error(__FILE__, __LINE__, "Assertion failed: " #condition); \
            return TEST_FAIL; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            test_log_error(__FILE__, __LINE__, "Expected %d, got %d", (int)(expected), (int)(actual)); \
            return TEST_FAIL; \
        } \
    } while(0)

#define TEST_ASSERT_NOT_EQUAL(expected, actual) \
    do { \
        if ((expected) == (actual)) { \
            test_log_error(__FILE__, __LINE__, "Expected not %d, but got %d", (int)(expected), (int)(actual)); \
            return TEST_FAIL; \
        } \
    } while(0)

#define TEST_ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != NULL) { \
            test_log_error(__FILE__, __LINE__, "Expected NULL, got %p", (ptr)); \
            return TEST_FAIL; \
        } \
    } while(0)

#define TEST_ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            test_log_error(__FILE__, __LINE__, "Expected non-NULL pointer"); \
            return TEST_FAIL; \
        } \
    } while(0)

#define TEST_ASSERT_STRING_EQUAL(expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            test_log_error(__FILE__, __LINE__, "Expected '%s', got '%s'", (expected), (actual)); \
            return TEST_FAIL; \
        } \
    } while(0)

#define TEST_ASSERT_MEMORY_EQUAL(expected, actual, size) \
    do { \
        if (memcmp((expected), (actual), (size)) != 0) { \
            test_log_error(__FILE__, __LINE__, "Memory comparison failed (%zu bytes)", (size_t)(size)); \
            return TEST_FAIL; \
        } \
    } while(0)

// Logging utilities
void test_log_info(const char* fmt, ...);
void test_log_error(const char* file, int line, const char* fmt, ...);
void test_log_debug(const char* fmt, ...);

// Timing utilities
uint64_t test_get_time_us(void);
void test_sleep_ms(uint32_t ms);

// Memory leak detection
void test_memory_mark(void);
bool test_memory_check_leaks(void);

// DMA buffer simulation
void* test_alloc_dma_buffer(size_t size);
void test_free_dma_buffer(void* buffer);
uint64_t test_virt_to_phys(void* virt);

// Interrupt simulation
typedef void (*test_irq_handler_t)(void* context);
int test_register_irq(uint32_t irq, test_irq_handler_t handler, void* context);
void test_unregister_irq(uint32_t irq);
void test_trigger_irq(uint32_t irq);

// PCI device simulation
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint32_t bar[6];
    uint32_t bar_size[6];
    uint8_t irq_line;
    uint8_t irq_pin;
} test_pci_device_t;

test_pci_device_t* test_create_pci_device(uint16_t vendor, uint16_t device);
void test_destroy_pci_device(test_pci_device_t* pci_dev);
void test_pci_set_bar(test_pci_device_t* pci_dev, uint8_t bar_num, uint32_t addr, uint32_t size);

#endif // DRIVER_TEST_FRAMEWORK_H
