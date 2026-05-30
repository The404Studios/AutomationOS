#include "driver_test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

// Global test state
static test_suite_t* test_suites_head = NULL;
static test_stats_t global_stats = {0};
static bool framework_initialized = false;

// Memory tracking
static size_t allocated_bytes = 0;
static size_t freed_bytes = 0;
static size_t allocation_mark = 0;

// IRQ simulation
#define MAX_IRQS 256
static test_irq_handler_t irq_handlers[MAX_IRQS];
static void* irq_contexts[MAX_IRQS];

// Framework initialization
void test_framework_init(void) {
    if (framework_initialized) {
        return;
    }

    memset(&global_stats, 0, sizeof(test_stats_t));
    memset(irq_handlers, 0, sizeof(irq_handlers));
    memset(irq_contexts, 0, sizeof(irq_contexts));

    test_suites_head = NULL;
    allocated_bytes = 0;
    freed_bytes = 0;
    allocation_mark = 0;

    framework_initialized = true;
    test_log_info("Driver test framework initialized");
}

// Suite registration
void test_register_suite(test_suite_t* suite) {
    if (!framework_initialized) {
        test_framework_init();
    }

    suite->next = test_suites_head;
    test_suites_head = suite;

    test_log_info("Registered test suite: %s", suite->name);
}

// Test case registration
void test_register_case(test_suite_t* suite, test_case_t* test) {
    test->next = suite->tests;
    suite->tests = test;
}

// Run all tests
test_result_t test_run_all(void) {
    if (!framework_initialized) {
        test_framework_init();
    }

    test_log_info("=== Running all driver tests ===");

    memset(&global_stats, 0, sizeof(test_stats_t));
    uint64_t start_time = test_get_time_us();

    test_suite_t* suite = test_suites_head;
    while (suite != NULL) {
        test_log_info("Running suite: %s - %s", suite->name, suite->description);

        if (suite->setup) {
            suite->setup();
        }

        test_case_t* test = suite->tests;
        while (test != NULL) {
            test_log_info("  [TEST] %s", test->name);

            global_stats.tests_run++;
            test_result_t result = test->test_func();

            switch (result) {
                case TEST_PASS:
                    global_stats.tests_passed++;
                    test_log_info("    PASS");
                    break;
                case TEST_FAIL:
                    global_stats.tests_failed++;
                    test_log_error(__FILE__, __LINE__, "    FAIL");
                    break;
                case TEST_SKIP:
                    global_stats.tests_skipped++;
                    test_log_info("    SKIP");
                    break;
                case TEST_ERROR:
                    global_stats.tests_errored++;
                    test_log_error(__FILE__, __LINE__, "    ERROR");
                    break;
            }

            test = test->next;
        }

        if (suite->teardown) {
            suite->teardown();
        }

        suite = suite->next;
    }

    global_stats.total_time_us = test_get_time_us() - start_time;

    test_log_info("=== Test run complete ===");
    test_print_stats(&global_stats);

    return (global_stats.tests_failed == 0 && global_stats.tests_errored == 0)
           ? TEST_PASS : TEST_FAIL;
}

// Run specific suite
test_result_t test_run_suite(const char* suite_name) {
    test_suite_t* suite = test_suites_head;
    while (suite != NULL) {
        if (strcmp(suite->name, suite_name) == 0) {
            test_log_info("Running suite: %s", suite->name);

            if (suite->setup) {
                suite->setup();
            }

            test_case_t* test = suite->tests;
            while (test != NULL) {
                test_log_info("  [TEST] %s", test->name);
                test_result_t result = test->test_func();

                if (result == TEST_PASS) {
                    test_log_info("    PASS");
                } else {
                    test_log_error(__FILE__, __LINE__, "    FAIL");
                }

                test = test->next;
            }

            if (suite->teardown) {
                suite->teardown();
            }

            return TEST_PASS;
        }
        suite = suite->next;
    }

    test_log_error(__FILE__, __LINE__, "Suite not found: %s", suite_name);
    return TEST_ERROR;
}

// Run specific test case
test_result_t test_run_case(const char* suite_name, const char* case_name) {
    test_suite_t* suite = test_suites_head;
    while (suite != NULL) {
        if (strcmp(suite->name, suite_name) == 0) {
            test_case_t* test = suite->tests;
            while (test != NULL) {
                if (strcmp(test->name, case_name) == 0) {
                    test_log_info("Running test: %s::%s", suite_name, case_name);

                    if (suite->setup) {
                        suite->setup();
                    }

                    test_result_t result = test->test_func();

                    if (suite->teardown) {
                        suite->teardown();
                    }

                    return result;
                }
                test = test->next;
            }

            test_log_error(__FILE__, __LINE__, "Test case not found: %s", case_name);
            return TEST_ERROR;
        }
        suite = suite->next;
    }

    test_log_error(__FILE__, __LINE__, "Suite not found: %s", suite_name);
    return TEST_ERROR;
}

// Print statistics
void test_print_stats(test_stats_t* stats) {
    test_log_info("");
    test_log_info("========================================");
    test_log_info("Test Results:");
    test_log_info("  Total:   %u", stats->tests_run);
    test_log_info("  Passed:  %u (%.1f%%)", stats->tests_passed,
                  stats->tests_run > 0 ? (100.0 * stats->tests_passed / stats->tests_run) : 0.0);
    test_log_info("  Failed:  %u (%.1f%%)", stats->tests_failed,
                  stats->tests_run > 0 ? (100.0 * stats->tests_failed / stats->tests_run) : 0.0);
    test_log_info("  Skipped: %u", stats->tests_skipped);
    test_log_info("  Errors:  %u", stats->tests_errored);
    test_log_info("  Time:    %.2f seconds", stats->total_time_us / 1000000.0);
    test_log_info("========================================");
    test_log_info("");
}

// Get statistics
test_stats_t* test_get_stats(void) {
    return &global_stats;
}

// Mock device implementation
mock_device_t* mock_device_create(const char* name) {
    mock_device_t* device = (mock_device_t*)malloc(sizeof(mock_device_t));
    if (!device) {
        return NULL;
    }

    device->name = strdup(name);
    device->device_data = NULL;
    device->init = NULL;
    device->cleanup = NULL;
    device->read = NULL;
    device->write = NULL;
    device->ioctl = NULL;

    allocated_bytes += sizeof(mock_device_t) + strlen(name) + 1;

    return device;
}

void mock_device_destroy(mock_device_t* device) {
    if (device) {
        if (device->name) {
            freed_bytes += strlen(device->name) + 1;
            free((void*)device->name);
        }
        freed_bytes += sizeof(mock_device_t);
        free(device);
    }
}

// Logging
void test_log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[INFO] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void test_log_error(const char* file, int line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[ERROR] %s:%d: ", file, line);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void test_log_debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[DEBUG] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

// Timing
uint64_t test_get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

void test_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// Memory leak detection
void test_memory_mark(void) {
    allocation_mark = allocated_bytes - freed_bytes;
}

bool test_memory_check_leaks(void) {
    size_t current = allocated_bytes - freed_bytes;
    if (current != allocation_mark) {
        test_log_error(__FILE__, __LINE__,
                      "Memory leak detected: %zu bytes leaked",
                      current - allocation_mark);
        return false;
    }
    return true;
}

// DMA buffer simulation
void* test_alloc_dma_buffer(size_t size) {
    void* buffer = malloc(size);
    if (buffer) {
        allocated_bytes += size;
        memset(buffer, 0, size);
    }
    return buffer;
}

void test_free_dma_buffer(void* buffer) {
    if (buffer) {
        freed_bytes += 1; // Track that something was freed
        free(buffer);
    }
}

uint64_t test_virt_to_phys(void* virt) {
    // Simulate virtual to physical translation
    return (uint64_t)virt;
}

// IRQ simulation
int test_register_irq(uint32_t irq, test_irq_handler_t handler, void* context) {
    if (irq >= MAX_IRQS) {
        return -1;
    }

    irq_handlers[irq] = handler;
    irq_contexts[irq] = context;

    return 0;
}

void test_unregister_irq(uint32_t irq) {
    if (irq < MAX_IRQS) {
        irq_handlers[irq] = NULL;
        irq_contexts[irq] = NULL;
    }
}

void test_trigger_irq(uint32_t irq) {
    if (irq < MAX_IRQS && irq_handlers[irq]) {
        irq_handlers[irq](irq_contexts[irq]);
    }
}

// PCI device simulation
test_pci_device_t* test_create_pci_device(uint16_t vendor, uint16_t device) {
    test_pci_device_t* pci_dev = (test_pci_device_t*)malloc(sizeof(test_pci_device_t));
    if (!pci_dev) {
        return NULL;
    }

    memset(pci_dev, 0, sizeof(test_pci_device_t));
    pci_dev->vendor_id = vendor;
    pci_dev->device_id = device;

    allocated_bytes += sizeof(test_pci_device_t);

    return pci_dev;
}

void test_destroy_pci_device(test_pci_device_t* pci_dev) {
    if (pci_dev) {
        freed_bytes += sizeof(test_pci_device_t);
        free(pci_dev);
    }
}

void test_pci_set_bar(test_pci_device_t* pci_dev, uint8_t bar_num, uint32_t addr, uint32_t size) {
    if (pci_dev && bar_num < 6) {
        pci_dev->bar[bar_num] = addr;
        pci_dev->bar_size[bar_num] = size;
    }
}
