#ifndef KTEST_H
#define KTEST_H

#include "types.h"

/*
 * KTest - In-Kernel Testing Framework for AutomationOS
 * Inspired by Linux KUnit
 *
 * This framework allows writing and running tests in kernel context
 * during boot time with minimal overhead.
 */

// Test result types
typedef enum {
    KTEST_SUCCESS = 0,
    KTEST_FAILURE = 1,
    KTEST_SKIPPED = 2,
    KTEST_EXPECTED_DEATH = 3
} ktest_result_t;

// Test statistics
typedef struct {
    uint32_t total;
    uint32_t passed;
    uint32_t failed;
    uint32_t skipped;
    uint64_t start_time;
    uint64_t end_time;
} ktest_stats_t;

// Forward declarations
struct ktest_case;
struct ktest_suite;

// Test fixture function types
typedef void (*ktest_setup_fn)(void* fixture);
typedef void (*ktest_teardown_fn)(void* fixture);
typedef void (*ktest_case_fn)(void* fixture);

// Test case structure
typedef struct ktest_case {
    const char* name;
    ktest_case_fn test_fn;
    ktest_result_t result;
    const char* failure_msg;
    const char* file;
    int line;
    struct ktest_case* next;
} ktest_case_t;

// Test suite structure
typedef struct ktest_suite {
    const char* name;
    ktest_setup_fn setup;
    ktest_teardown_fn teardown;
    void* fixture;
    size_t fixture_size;
    ktest_case_t* test_cases;
    struct ktest_suite* next;
    bool enabled;
} ktest_suite_t;

// Global test context
typedef struct {
    ktest_suite_t* suites;
    ktest_stats_t stats;
    bool enabled;
    bool verbose;
    const char* filter;  // Run only tests matching this pattern
    ktest_case_t* current_test;
    bool expect_panic;
} ktest_context_t;

/*
 * Core API
 */

// Initialize test framework
void ktest_init(void);

// Register a test suite
void ktest_register_suite(ktest_suite_t* suite);

// Run all registered tests
void ktest_run_all(void);

// Run specific suite
void ktest_run_suite(const char* suite_name);

// Get test statistics
ktest_stats_t ktest_get_stats(void);

// Enable/disable testing
void ktest_set_enabled(bool enabled);

// Set verbose mode
void ktest_set_verbose(bool verbose);

// Set test filter (run only matching tests)
void ktest_set_filter(const char* filter);

/*
 * Assertion Macros
 */

// Internal assertion implementation
void __ktest_assert_failed(const char* expr, const char* file, int line, const char* msg);

// Basic assertions
#define KTEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            __ktest_assert_failed(#cond, __FILE__, __LINE__, NULL); \
            return; \
        } \
    } while(0)

#define KTEST_ASSERT_MSG(cond, msg) \
    do { \
        if (!(cond)) { \
            __ktest_assert_failed(#cond, __FILE__, __LINE__, msg); \
            return; \
        } \
    } while(0)

#define KTEST_ASSERT_TRUE(cond) KTEST_ASSERT(cond)
#define KTEST_ASSERT_FALSE(cond) KTEST_ASSERT(!(cond))

// Equality assertions
#define KTEST_ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            char __msg[256]; \
            ksnprintf(__msg, sizeof(__msg), "Expected %lld, got %lld", (int64_t)(b), (int64_t)(a)); \
            __ktest_assert_failed(#a " == " #b, __FILE__, __LINE__, __msg); \
            return; \
        } \
    } while(0)

#define KTEST_ASSERT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            char __msg[256]; \
            ksnprintf(__msg, sizeof(__msg), "Expected not equal to %lld", (int64_t)(a)); \
            __ktest_assert_failed(#a " != " #b, __FILE__, __LINE__, __msg); \
            return; \
        } \
    } while(0)

// Comparison assertions
#define KTEST_ASSERT_LT(a, b) KTEST_ASSERT((a) < (b))
#define KTEST_ASSERT_LE(a, b) KTEST_ASSERT((a) <= (b))
#define KTEST_ASSERT_GT(a, b) KTEST_ASSERT((a) > (b))
#define KTEST_ASSERT_GE(a, b) KTEST_ASSERT((a) >= (b))

// Pointer assertions
#define KTEST_ASSERT_NULL(ptr) KTEST_ASSERT((ptr) == NULL)
#define KTEST_ASSERT_NOT_NULL(ptr) KTEST_ASSERT((ptr) != NULL)
#define KTEST_ASSERT_PTR_EQ(a, b) KTEST_ASSERT((void*)(a) == (void*)(b))

// String assertions (requires kstrlen, kstrcmp)
#define KTEST_ASSERT_STR_EQ(a, b) \
    do { \
        if (kstrcmp((a), (b)) != 0) { \
            __ktest_assert_failed(#a " == " #b, __FILE__, __LINE__, "Strings not equal"); \
            return; \
        } \
    } while(0)

// Memory assertions
#define KTEST_ASSERT_MEM_EQ(a, b, size) \
    do { \
        if (kmemcmp((a), (b), (size)) != 0) { \
            __ktest_assert_failed(#a " == " #b, __FILE__, __LINE__, "Memory not equal"); \
            return; \
        } \
    } while(0)

// Expect assertions (don't stop test on failure, just record)
#define KTEST_EXPECT(cond) \
    do { \
        if (!(cond)) { \
            __ktest_expect_failed(#cond, __FILE__, __LINE__, NULL); \
        } \
    } while(0)

#define KTEST_EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            char __msg[256]; \
            ksnprintf(__msg, sizeof(__msg), "Expected %lld, got %lld", (int64_t)(b), (int64_t)(a)); \
            __ktest_expect_failed(#a " == " #b, __FILE__, __LINE__, __msg); \
        } \
    } while(0)

void __ktest_expect_failed(const char* expr, const char* file, int line, const char* msg);

// Death test (expect kernel panic)
#define KTEST_EXPECT_DEATH(code) \
    do { \
        ktest_mark_expect_panic(); \
        code; \
        __ktest_assert_failed("Expected panic", __FILE__, __LINE__, "Code did not panic"); \
        return; \
    } while(0)

void ktest_mark_expect_panic(void);

/*
 * Test Registration Macros
 */

// Define a test case
#define KTEST_CASE(suite_name, test_name) \
    static void suite_name##_##test_name##_impl(void* fixture); \
    static ktest_case_t suite_name##_##test_name##_case = { \
        .name = #test_name, \
        .test_fn = suite_name##_##test_name##_impl, \
        .result = KTEST_SUCCESS, \
        .failure_msg = NULL, \
        .file = __FILE__, \
        .line = __LINE__, \
        .next = NULL \
    }; \
    static void __attribute__((constructor)) suite_name##_##test_name##_register(void) { \
        ktest_register_case(&suite_name##_suite, &suite_name##_##test_name##_case); \
    } \
    static void suite_name##_##test_name##_impl(void* fixture)

// Define a test suite
#define KTEST_SUITE(suite_name) \
    ktest_suite_t suite_name##_suite = { \
        .name = #suite_name, \
        .setup = NULL, \
        .teardown = NULL, \
        .fixture = NULL, \
        .fixture_size = 0, \
        .test_cases = NULL, \
        .next = NULL, \
        .enabled = true \
    }; \
    static void __attribute__((constructor)) suite_name##_suite_register(void) { \
        ktest_register_suite(&suite_name##_suite); \
    }

// Define suite with fixtures
#define KTEST_SUITE_WITH_FIXTURE(suite_name, fixture_type, setup_fn, teardown_fn) \
    ktest_suite_t suite_name##_suite = { \
        .name = #suite_name, \
        .setup = (ktest_setup_fn)setup_fn, \
        .teardown = (ktest_teardown_fn)teardown_fn, \
        .fixture = NULL, \
        .fixture_size = sizeof(fixture_type), \
        .test_cases = NULL, \
        .next = NULL, \
        .enabled = true \
    }; \
    static void __attribute__((constructor)) suite_name##_suite_register(void) { \
        ktest_register_suite(&suite_name##_suite); \
    }

// Register a test case to a suite
void ktest_register_case(ktest_suite_t* suite, ktest_case_t* test_case);

/*
 * Utility functions
 */

// String utilities for tests
int kstrcmp(const char* s1, const char* s2);
size_t kstrlen(const char* s);
int kmemcmp(const void* s1, const void* s2, size_t n);
int ksnprintf(char* buf, size_t size, const char* format, ...);

// Benchmarking utilities
uint64_t ktest_rdtsc(void);
void ktest_benchmark_start(void);
uint64_t ktest_benchmark_end(const char* name);

/*
 * Mock framework
 */

typedef void* (*ktest_mock_fn)(void* args);

typedef struct ktest_mock {
    const char* name;
    ktest_mock_fn original;
    ktest_mock_fn mock;
    uint32_t call_count;
    void* last_args;
    void* return_value;
} ktest_mock_t;

void ktest_mock_function(ktest_mock_t* mock);
void ktest_unmock_function(ktest_mock_t* mock);
uint32_t ktest_get_call_count(ktest_mock_t* mock);
void ktest_reset_mock(ktest_mock_t* mock);

#endif // KTEST_H
