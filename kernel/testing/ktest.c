#include "../include/ktest.h"
#include "../include/kernel.h"
#include "../include/x86_64.h"

// Global test context
static ktest_context_t g_test_ctx = {
    .suites = NULL,
    .stats = {0},
    .enabled = false,
    .verbose = false,
    .filter = NULL,
    .current_test = NULL,
    .expect_panic = false
};

// Color codes for output
#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"

/*
 * Initialization
 */
void ktest_init(void) {
    g_test_ctx.enabled = true;
    g_test_ctx.verbose = true;
    g_test_ctx.stats.total = 0;
    g_test_ctx.stats.passed = 0;
    g_test_ctx.stats.failed = 0;
    g_test_ctx.stats.skipped = 0;

    kprintf("\n");
    kprintf(COLOR_CYAN "====================================\n" COLOR_RESET);
    kprintf(COLOR_CYAN "   KTest Framework v1.0\n" COLOR_RESET);
    kprintf(COLOR_CYAN "   In-Kernel Testing System\n" COLOR_RESET);
    kprintf(COLOR_CYAN "====================================\n" COLOR_RESET);
    kprintf("\n");
}

/*
 * Suite and case registration
 */
void ktest_register_suite(ktest_suite_t* suite) {
    if (!suite) return;

    // Add to linked list
    suite->next = g_test_ctx.suites;
    g_test_ctx.suites = suite;

    if (g_test_ctx.verbose) {
        kprintf("[KTEST] Registered suite: %s\n", suite->name);
    }
}

void ktest_register_case(ktest_suite_t* suite, ktest_case_t* test_case) {
    if (!suite || !test_case) return;

    // Add to suite's test case list
    test_case->next = suite->test_cases;
    suite->test_cases = test_case;
}

/*
 * String utilities
 */
int kstrcmp(const char* s1, const char* s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

size_t kstrlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int kmemcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

// Simple sprintf implementation for tests
int ksnprintf(char* buf, size_t size, const char* format, ...) {
    // Simple implementation - just copy format string for now
    // A full implementation would handle format specifiers
    size_t i = 0;
    while (format[i] && i < size - 1) {
        buf[i] = format[i];
        i++;
    }
    buf[i] = '\0';
    return i;
}

/*
 * Pattern matching for test filtering
 */
static bool matches_filter(const char* name, const char* filter) {
    if (!filter || filter[0] == '\0') {
        return true;  // No filter means run all
    }

    // Simple substring match
    const char* p = name;
    while (*p) {
        const char* f = filter;
        const char* n = p;
        while (*f && *n && *f == *n) {
            f++;
            n++;
        }
        if (*f == '\0') {
            return true;  // Match found
        }
        p++;
    }
    return false;
}

/*
 * Test execution
 */
static void run_test_case(ktest_suite_t* suite, ktest_case_t* test_case) {
    g_test_ctx.stats.total++;
    g_test_ctx.current_test = test_case;
    g_test_ctx.expect_panic = false;

    if (g_test_ctx.verbose) {
        kprintf("  [RUN ] %s.%s\n", suite->name, test_case->name);
    }

    // Setup fixture if provided
    void* fixture = NULL;
    if (suite->fixture_size > 0) {
        // Allocate fixture memory (simplified - should use heap_alloc)
        static char fixture_buffer[4096];
        fixture = fixture_buffer;

        if (suite->setup) {
            suite->setup(fixture);
        }
    }

    // Run test
    uint64_t start = rdtsc();
    test_case->test_fn(fixture);
    uint64_t end = rdtsc();
    uint64_t cycles = end - start;

    // Teardown fixture
    if (suite->teardown && fixture) {
        suite->teardown(fixture);
    }

    // Report result
    if (test_case->result == KTEST_SUCCESS) {
        g_test_ctx.stats.passed++;
        if (g_test_ctx.verbose) {
            kprintf(COLOR_GREEN "  [ OK ] %s.%s (%llu cycles)\n" COLOR_RESET,
                   suite->name, test_case->name, cycles);
        }
    } else if (test_case->result == KTEST_SKIPPED) {
        g_test_ctx.stats.skipped++;
        kprintf(COLOR_YELLOW "  [SKIP] %s.%s\n" COLOR_RESET,
               suite->name, test_case->name);
    } else {
        g_test_ctx.stats.failed++;
        kprintf(COLOR_RED "  [FAIL] %s.%s\n" COLOR_RESET,
               suite->name, test_case->name);
        if (test_case->failure_msg) {
            kprintf(COLOR_RED "         %s:%d: %s\n" COLOR_RESET,
                   test_case->file, test_case->line, test_case->failure_msg);
        }
    }

    g_test_ctx.current_test = NULL;
}

void ktest_run_suite(const char* suite_name) {
    ktest_suite_t* suite = g_test_ctx.suites;

    while (suite) {
        if (kstrcmp(suite->name, suite_name) == 0) {
            if (!suite->enabled) {
                kprintf(COLOR_YELLOW "[SKIP] Suite %s is disabled\n" COLOR_RESET, suite->name);
                return;
            }

            kprintf(COLOR_BLUE "[=====] Running tests from %s\n" COLOR_RESET, suite->name);

            ktest_case_t* test_case = suite->test_cases;
            uint32_t suite_total = 0;
            uint32_t suite_passed = 0;

            while (test_case) {
                if (matches_filter(test_case->name, g_test_ctx.filter)) {
                    run_test_case(suite, test_case);
                    suite_total++;
                    if (test_case->result == KTEST_SUCCESS) {
                        suite_passed++;
                    }
                }
                test_case = test_case->next;
            }

            kprintf(COLOR_BLUE "[=====] %s: %u/%u tests passed\n" COLOR_RESET,
                   suite->name, suite_passed, suite_total);
            kprintf("\n");
            return;
        }
        suite = suite->next;
    }

    kprintf(COLOR_RED "[ERROR] Suite '%s' not found\n" COLOR_RESET, suite_name);
}

void ktest_run_all(void) {
    if (!g_test_ctx.enabled) {
        kprintf("[KTEST] Testing is disabled\n");
        return;
    }

    g_test_ctx.stats.start_time = rdtsc();

    // Count total suites
    uint32_t suite_count = 0;
    ktest_suite_t* suite = g_test_ctx.suites;
    while (suite) {
        if (suite->enabled) suite_count++;
        suite = suite->next;
    }

    kprintf(COLOR_CYAN "[=====] Running tests from %u suites\n" COLOR_RESET, suite_count);
    kprintf("\n");

    // Run all suites
    suite = g_test_ctx.suites;
    while (suite) {
        if (suite->enabled) {
            kprintf(COLOR_BLUE "[-----] %s\n" COLOR_RESET, suite->name);

            ktest_case_t* test_case = suite->test_cases;
            while (test_case) {
                if (matches_filter(test_case->name, g_test_ctx.filter)) {
                    run_test_case(suite, test_case);
                }
                test_case = test_case->next;
            }

            kprintf("\n");
        }
        suite = suite->next;
    }

    g_test_ctx.stats.end_time = rdtsc();
    uint64_t total_cycles = g_test_ctx.stats.end_time - g_test_ctx.stats.start_time;

    // Print summary
    kprintf(COLOR_CYAN "====================================\n" COLOR_RESET);
    kprintf(COLOR_CYAN "   Test Summary\n" COLOR_RESET);
    kprintf(COLOR_CYAN "====================================\n" COLOR_RESET);
    kprintf("Total:   %u tests\n", g_test_ctx.stats.total);
    kprintf(COLOR_GREEN "Passed:  %u tests\n" COLOR_RESET, g_test_ctx.stats.passed);

    if (g_test_ctx.stats.failed > 0) {
        kprintf(COLOR_RED "Failed:  %u tests\n" COLOR_RESET, g_test_ctx.stats.failed);
    }

    if (g_test_ctx.stats.skipped > 0) {
        kprintf(COLOR_YELLOW "Skipped: %u tests\n" COLOR_RESET, g_test_ctx.stats.skipped);
    }

    kprintf("Time:    %llu cycles\n", total_cycles);
    kprintf(COLOR_CYAN "====================================\n" COLOR_RESET);

    if (g_test_ctx.stats.failed == 0) {
        kprintf(COLOR_GREEN "\nAll tests PASSED!\n" COLOR_RESET);
    } else {
        kprintf(COLOR_RED "\nSome tests FAILED!\n" COLOR_RESET);
    }
    kprintf("\n");
}

/*
 * Assertion implementation
 */
void __ktest_assert_failed(const char* expr, const char* file, int line, const char* msg) {
    if (g_test_ctx.current_test) {
        g_test_ctx.current_test->result = KTEST_FAILURE;

        // Format failure message
        static char failure_buffer[512];
        if (msg) {
            ksnprintf(failure_buffer, sizeof(failure_buffer),
                     "Assertion failed: %s (%s)", expr, msg);
        } else {
            ksnprintf(failure_buffer, sizeof(failure_buffer),
                     "Assertion failed: %s", expr);
        }
        g_test_ctx.current_test->failure_msg = failure_buffer;
    }
}

void __ktest_expect_failed(const char* expr, const char* file, int line, const char* msg) {
    // Expectations don't stop test execution, just log the failure
    kprintf(COLOR_YELLOW "  [WARN] Expectation failed at %s:%d: %s\n" COLOR_RESET,
           file, line, expr);
    if (msg) {
        kprintf(COLOR_YELLOW "         %s\n" COLOR_RESET, msg);
    }
}

void ktest_mark_expect_panic(void) {
    g_test_ctx.expect_panic = true;
}

/*
 * Statistics
 */
ktest_stats_t ktest_get_stats(void) {
    return g_test_ctx.stats;
}

/*
 * Configuration
 */
void ktest_set_enabled(bool enabled) {
    g_test_ctx.enabled = enabled;
}

void ktest_set_verbose(bool verbose) {
    g_test_ctx.verbose = verbose;
}

void ktest_set_filter(const char* filter) {
    g_test_ctx.filter = filter;
}

/*
 * Benchmarking utilities
 */
uint64_t ktest_rdtsc(void) {
    return rdtsc();
}

static uint64_t benchmark_start_time = 0;

void ktest_benchmark_start(void) {
    benchmark_start_time = rdtsc();
}

uint64_t ktest_benchmark_end(const char* name) {
    uint64_t end = rdtsc();
    uint64_t cycles = end - benchmark_start_time;

    if (g_test_ctx.verbose && name) {
        kprintf("  [BENCH] %s: %llu cycles\n", name, cycles);
    }

    return cycles;
}

/*
 * Mock framework
 */
void ktest_mock_function(ktest_mock_t* mock) {
    if (!mock) return;
    mock->call_count = 0;
    mock->last_args = NULL;
    // In a real implementation, we would patch the function pointer
}

void ktest_unmock_function(ktest_mock_t* mock) {
    if (!mock) return;
    // Restore original function
    mock->call_count = 0;
}

uint32_t ktest_get_call_count(ktest_mock_t* mock) {
    return mock ? mock->call_count : 0;
}

void ktest_reset_mock(ktest_mock_t* mock) {
    if (!mock) return;
    mock->call_count = 0;
    mock->last_args = NULL;
    mock->return_value = NULL;
}
