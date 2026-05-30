// tests/unit/test_libc.c - Unit tests for libc functions

#include "../../userspace/libc/stdio.h"
#include "../../userspace/libc/stdlib.h"
#include "../../userspace/libc/string.h"
#include "../../userspace/libc/time.h"
#include "../../userspace/libc/dirent.h"
#include "../../userspace/libc/signal.h"

#define TEST_PASSED 0
#define TEST_FAILED 1

static int tests_passed = 0;
static int tests_failed = 0;

// Test result macros
#define ASSERT(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, message); \
        tests_failed++; \
        return TEST_FAILED; \
    } \
} while(0)

#define TEST(name) \
    static int test_##name(void); \
    static void run_test_##name(void) { \
        printf("Running test: %s... ", #name); \
        if (test_##name() == TEST_PASSED) { \
            printf("PASSED\n"); \
            tests_passed++; \
        } else { \
            printf("FAILED\n"); \
        } \
    } \
    static int test_##name(void)

// ============================================================================
// STDLIB TESTS
// ============================================================================

TEST(atoi_basic) {
    ASSERT(atoi("123") == 123, "atoi('123') should be 123");
    ASSERT(atoi("-456") == -456, "atoi('-456') should be -456");
    ASSERT(atoi("0") == 0, "atoi('0') should be 0");
    ASSERT(atoi("  789") == 789, "atoi with whitespace");
    return TEST_PASSED;
}

TEST(atof_basic) {
    double d1 = atof("3.14");
    ASSERT(d1 > 3.13 && d1 < 3.15, "atof('3.14') should be ~3.14");

    double d2 = atof("-2.5");
    ASSERT(d2 > -2.51 && d2 < -2.49, "atof('-2.5') should be ~-2.5");

    double d3 = atof("0.0");
    ASSERT(d3 == 0.0, "atof('0.0') should be 0.0");

    return TEST_PASSED;
}

TEST(strtol_basic) {
    char* endptr;

    long val1 = strtol("123", &endptr, 10);
    ASSERT(val1 == 123, "strtol('123', 10) should be 123");

    long val2 = strtol("0x1A", &endptr, 16);
    ASSERT(val2 == 26, "strtol('0x1A', 16) should be 26");

    long val3 = strtol("1010", &endptr, 2);
    ASSERT(val3 == 10, "strtol('1010', 2) should be 10");

    long val4 = strtol("-42", &endptr, 10);
    ASSERT(val4 == -42, "strtol('-42', 10) should be -42");

    return TEST_PASSED;
}

TEST(strtod_basic) {
    char* endptr;

    double d1 = strtod("3.14159", &endptr);
    ASSERT(d1 > 3.14 && d1 < 3.15, "strtod('3.14159') should be ~3.14159");

    double d2 = strtod("-0.5", &endptr);
    ASSERT(d2 > -0.51 && d2 < -0.49, "strtod('-0.5') should be ~-0.5");

    double d3 = strtod("1.5e2", &endptr);
    ASSERT(d3 > 149.0 && d3 < 151.0, "strtod('1.5e2') should be ~150");

    return TEST_PASSED;
}

TEST(qsort_basic) {
    int arr[] = {5, 2, 8, 1, 9, 3, 7};
    int expected[] = {1, 2, 3, 5, 7, 8, 9};
    int n = 7;

    // Comparison function for integers
    int cmp(const void* a, const void* b) {
        return (*(int*)a - *(int*)b);
    }

    qsort(arr, n, sizeof(int), cmp);

    for (int i = 0; i < n; i++) {
        ASSERT(arr[i] == expected[i], "qsort failed to sort array");
    }

    return TEST_PASSED;
}

TEST(bsearch_basic) {
    int arr[] = {1, 2, 3, 5, 7, 8, 9};
    int n = 7;
    int key;

    int cmp(const void* a, const void* b) {
        return (*(int*)a - *(int*)b);
    }

    // Search for existing element
    key = 5;
    int* result = (int*)bsearch(&key, arr, n, sizeof(int), cmp);
    ASSERT(result != NULL && *result == 5, "bsearch should find 5");

    // Search for non-existing element
    key = 6;
    result = (int*)bsearch(&key, arr, n, sizeof(int), cmp);
    ASSERT(result == NULL, "bsearch should not find 6");

    return TEST_PASSED;
}

TEST(abs_functions) {
    ASSERT(abs(-5) == 5, "abs(-5) should be 5");
    ASSERT(abs(5) == 5, "abs(5) should be 5");
    ASSERT(abs(0) == 0, "abs(0) should be 0");

    ASSERT(labs(-100L) == 100L, "labs(-100) should be 100");
    ASSERT(llabs(-1000LL) == 1000LL, "llabs(-1000) should be 1000");

    return TEST_PASSED;
}

TEST(div_functions) {
    div_t d = div(17, 5);
    ASSERT(d.quot == 3 && d.rem == 2, "div(17, 5) should be quot=3, rem=2");

    ldiv_t ld = ldiv(100L, 7L);
    ASSERT(ld.quot == 14L && ld.rem == 2L, "ldiv(100, 7) should be quot=14, rem=2");

    return TEST_PASSED;
}

TEST(rand_basic) {
    srand(12345);
    int r1 = rand();
    int r2 = rand();

    ASSERT(r1 >= 0 && r1 <= RAND_MAX, "rand() should be in valid range");
    ASSERT(r2 >= 0 && r2 <= RAND_MAX, "rand() should be in valid range");
    ASSERT(r1 != r2, "consecutive rand() calls should differ");

    // Same seed should produce same sequence
    srand(12345);
    int r3 = rand();
    ASSERT(r3 == r1, "same seed should produce same sequence");

    return TEST_PASSED;
}

// ============================================================================
// STDIO TESTS
// ============================================================================

TEST(snprintf_basic) {
    char buf[100];

    int n = snprintf(buf, sizeof(buf), "Hello %s", "World");
    ASSERT(strcmp(buf, "Hello World") == 0, "snprintf string formatting");
    ASSERT(n == 11, "snprintf should return correct length");

    snprintf(buf, sizeof(buf), "Number: %d", 42);
    ASSERT(strcmp(buf, "Number: 42") == 0, "snprintf integer formatting");

    snprintf(buf, sizeof(buf), "Hex: 0x%x", 255);
    ASSERT(strcmp(buf, "Hex: 0xff") == 0, "snprintf hex formatting");

    return TEST_PASSED;
}

TEST(snprintf_truncation) {
    char buf[10];

    snprintf(buf, sizeof(buf), "This is a long string");
    ASSERT(strlen(buf) < sizeof(buf), "snprintf should truncate");
    ASSERT(buf[sizeof(buf) - 1] == '\0', "snprintf should null-terminate");

    return TEST_PASSED;
}

// ============================================================================
// TIME TESTS
// ============================================================================

TEST(gmtime_basic) {
    time_t t = 0;  // Unix epoch: 1970-01-01 00:00:00 UTC
    struct tm* tm = gmtime(&t);

    ASSERT(tm != NULL, "gmtime should not return NULL");
    ASSERT(tm->tm_year == 70, "epoch year should be 1970 (70 + 1900)");
    ASSERT(tm->tm_mon == 0, "epoch month should be January (0)");
    ASSERT(tm->tm_mday == 1, "epoch day should be 1");
    ASSERT(tm->tm_hour == 0, "epoch hour should be 0");
    ASSERT(tm->tm_min == 0, "epoch minute should be 0");
    ASSERT(tm->tm_sec == 0, "epoch second should be 0");

    return TEST_PASSED;
}

TEST(mktime_basic) {
    struct tm tm = {0};
    tm.tm_year = 70;   // 1970
    tm.tm_mon = 0;     // January
    tm.tm_mday = 1;    // 1st
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;

    time_t t = mktime(&tm);
    ASSERT(t == 0, "mktime of epoch should be 0");

    return TEST_PASSED;
}

TEST(strftime_basic) {
    struct tm tm = {0};
    tm.tm_year = 121;  // 2021
    tm.tm_mon = 11;    // December
    tm.tm_mday = 25;   // 25th
    tm.tm_hour = 13;
    tm.tm_min = 30;
    tm.tm_sec = 45;
    tm.tm_wday = 6;    // Saturday

    char buf[100];

    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    ASSERT(strcmp(buf, "2021-12-25") == 0, "strftime date formatting");
    ASSERT(n == 10, "strftime should return correct length");

    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    ASSERT(strcmp(buf, "13:30:45") == 0, "strftime time formatting");

    strftime(buf, sizeof(buf), "%a %b %d", &tm);
    ASSERT(strcmp(buf, "Sat Dec 25") == 0, "strftime named date formatting");

    return TEST_PASSED;
}

TEST(difftime_basic) {
    time_t t1 = 1000;
    time_t t2 = 500;

    double diff = difftime(t1, t2);
    ASSERT(diff == 500.0, "difftime should calculate difference");

    return TEST_PASSED;
}

// ============================================================================
// SIGNAL TESTS
// ============================================================================

static int signal_handled = 0;

void test_signal_handler(int sig) {
    signal_handled = sig;
}

TEST(signal_basic) {
    sighandler_t old = signal(SIGUSR1, test_signal_handler);
    ASSERT(old != SIG_ERR, "signal() should succeed");

    signal_handled = 0;
    raise(SIGUSR1);
    ASSERT(signal_handled == SIGUSR1, "signal handler should be called");

    return TEST_PASSED;
}

TEST(sigset_operations) {
    sigset_t set;

    int result = sigemptyset(&set);
    ASSERT(result == 0, "sigemptyset should succeed");
    ASSERT(set == 0, "empty set should be 0");

    result = sigaddset(&set, SIGINT);
    ASSERT(result == 0, "sigaddset should succeed");
    ASSERT(sigismember(&set, SIGINT) == 1, "SIGINT should be in set");

    result = sigdelset(&set, SIGINT);
    ASSERT(result == 0, "sigdelset should succeed");
    ASSERT(sigismember(&set, SIGINT) == 0, "SIGINT should not be in set");

    result = sigfillset(&set);
    ASSERT(result == 0, "sigfillset should succeed");
    ASSERT(sigismember(&set, SIGTERM) == 1, "SIGTERM should be in full set");

    return TEST_PASSED;
}

TEST(strsignal_basic) {
    const char* msg = strsignal(SIGINT);
    ASSERT(msg != NULL, "strsignal should not return NULL");
    ASSERT(strcmp(msg, "Interrupt") == 0, "strsignal(SIGINT) should be 'Interrupt'");

    msg = strsignal(SIGTERM);
    ASSERT(strcmp(msg, "Terminated") == 0, "strsignal(SIGTERM) should be 'Terminated'");

    return TEST_PASSED;
}

// ============================================================================
// STRING TESTS (verify existing implementation)
// ============================================================================

TEST(strlen_basic) {
    ASSERT(strlen("") == 0, "strlen of empty string");
    ASSERT(strlen("hello") == 5, "strlen of 'hello'");
    ASSERT(strlen("a") == 1, "strlen of single char");

    return TEST_PASSED;
}

TEST(strcmp_basic) {
    ASSERT(strcmp("abc", "abc") == 0, "equal strings");
    ASSERT(strcmp("abc", "abd") < 0, "less than");
    ASSERT(strcmp("abd", "abc") > 0, "greater than");

    return TEST_PASSED;
}

TEST(strcpy_basic) {
    char buf[20];
    strcpy(buf, "test");
    ASSERT(strcmp(buf, "test") == 0, "strcpy should copy string");

    return TEST_PASSED;
}

TEST(memset_basic) {
    char buf[10];
    memset(buf, 'A', 5);
    buf[5] = '\0';
    ASSERT(strcmp(buf, "AAAAA") == 0, "memset should fill memory");

    return TEST_PASSED;
}

TEST(memcpy_basic) {
    char src[] = "source";
    char dest[20];
    memcpy(dest, src, 7);
    ASSERT(strcmp(dest, "source") == 0, "memcpy should copy memory");

    return TEST_PASSED;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
    printf("\n=== LibC Unit Tests ===\n\n");

    // stdlib tests
    printf("--- stdlib tests ---\n");
    run_test_atoi_basic();
    run_test_atof_basic();
    run_test_strtol_basic();
    run_test_strtod_basic();
    run_test_qsort_basic();
    run_test_bsearch_basic();
    run_test_abs_functions();
    run_test_div_functions();
    run_test_rand_basic();

    // stdio tests
    printf("\n--- stdio tests ---\n");
    run_test_snprintf_basic();
    run_test_snprintf_truncation();

    // time tests
    printf("\n--- time tests ---\n");
    run_test_gmtime_basic();
    run_test_mktime_basic();
    run_test_strftime_basic();
    run_test_difftime_basic();

    // signal tests
    printf("\n--- signal tests ---\n");
    run_test_signal_basic();
    run_test_sigset_operations();
    run_test_strsignal_basic();

    // string tests
    printf("\n--- string tests ---\n");
    run_test_strlen_basic();
    run_test_strcmp_basic();
    run_test_strcpy_basic();
    run_test_memset_basic();
    run_test_memcpy_basic();

    // Summary
    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Total:  %d\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("\nAll tests PASSED!\n");
        return 0;
    } else {
        printf("\nSome tests FAILED!\n");
        return 1;
    }
}
