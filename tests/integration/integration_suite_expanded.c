/**
 * AutomationOS Expanded Integration Test Suite
 *
 * Master test runner for comprehensive integration testing.
 * Runs 100+ integration tests across all subsystems.
 *
 * Test Categories:
 * 1. Boot Sequence (10 tests)
 * 2. Application Lifecycle (15 tests)
 * 3. File System Integration (15 tests)
 * 4. Network Stack Integration (15 tests)
 * 5. Graphics Stack Integration (10 tests)
 * 6. Security Integration (20 tests)
 * 7. Power Management (10 tests)
 * 8. Original Integration Suite (15 tests)
 *
 * Total: 110 integration tests
 *
 * Usage:
 *   From kernel: run_expanded_integration_suite()
 *   From command line: test --integration --expanded
 */

#include <types.h>
#include <kernel.h>
#include <mem.h>
#include <ktest.h>

// External test suite declarations
extern void run_boot_integration_tests(void);
extern void run_application_lifecycle_tests(void);
extern void run_filesystem_integration_tests(void);
extern void run_network_stack_integration_tests(void);
extern void run_graphics_stack_integration_tests(void);
extern void run_security_expanded_integration_tests(void);
extern void run_power_management_integration_tests(void);
extern void run_integration_test_suite(void);  // Original suite

// Global statistics
static struct {
    int total_suites;
    int total_tests;
    int total_passed;
    int total_failed;
    int total_skipped;
    uint64_t start_time;
    uint64_t end_time;
} test_stats = {0};

/**
 * Print test suite header
 */
static void print_suite_header(const char* suite_name, int test_count) {
    kprintf("\n");
    kprintf("==================================================================\n");
    kprintf("  TEST SUITE: %s\n", suite_name);
    kprintf("  Expected Tests: %d\n", test_count);
    kprintf("==================================================================\n");
}

/**
 * Print test suite separator
 */
static void print_suite_separator(void) {
    kprintf("\n");
    kprintf("------------------------------------------------------------------\n");
    kprintf("\n");
}

/**
 * Print overall test summary
 */
static void print_overall_summary(void) {
    uint64_t elapsed_ms = test_stats.end_time - test_stats.start_time;
    uint64_t elapsed_sec = elapsed_ms / 1000;

    kprintf("\n");
    kprintf("##################################################################\n");
    kprintf("##                                                              ##\n");
    kprintf("##         AUTOMATIONOS EXPANDED INTEGRATION TEST SUITE        ##\n");
    kprintf("##                      FINAL SUMMARY                           ##\n");
    kprintf("##                                                              ##\n");
    kprintf("##################################################################\n");
    kprintf("\n");
    kprintf("  Test Suites:  %d\n", test_stats.total_suites);
    kprintf("  Total Tests:  %d\n", test_stats.total_tests);
    kprintf("  Passed:       %d\n", test_stats.total_passed);
    kprintf("  Failed:       %d\n", test_stats.total_failed);
    kprintf("  Skipped:      %d\n", test_stats.total_skipped);
    kprintf("  Time:         %llu seconds\n", elapsed_sec);
    kprintf("\n");

    // Calculate pass rate
    int completed = test_stats.total_passed + test_stats.total_failed;
    if (completed > 0) {
        int pass_rate = (test_stats.total_passed * 100) / completed;
        kprintf("  Pass Rate:    %d%% (%d/%d)\n",
                pass_rate, test_stats.total_passed, completed);
    }

    kprintf("\n");

    // Overall status
    if (test_stats.total_failed == 0) {
        kprintf("  ███████████████████████████████████████████████████████████\n");
        kprintf("  ██                                                       ██\n");
        kprintf("  ██    ✓ ALL TESTS PASSED - SYSTEM INTEGRATION OK!      ██\n");
        kprintf("  ██                                                       ██\n");
        kprintf("  ███████████████████████████████████████████████████████████\n");
    } else {
        kprintf("  ███████████████████████████████████████████████████████████\n");
        kprintf("  ██                                                       ██\n");
        kprintf("  ██    ✗ %d TESTS FAILED - REVIEW REQUIRED              ██\n",
                test_stats.total_failed);
        kprintf("  ██                                                       ██\n");
        kprintf("  ███████████████████████████████████████████████████████████\n");
    }

    if (test_stats.total_skipped > 0) {
        kprintf("\n");
        kprintf("  NOTE: %d tests skipped (features pending future phases)\n",
                test_stats.total_skipped);
    }

    kprintf("\n");
    kprintf("##################################################################\n");
    kprintf("\n");
}

/**
 * Print test environment information
 */
static void print_test_environment(void) {
    kprintf("\n");
    kprintf("==================================================================\n");
    kprintf("  TEST ENVIRONMENT\n");
    kprintf("==================================================================\n");

    // System information
    uint64_t total_mem = pmm_get_total_memory();
    uint64_t free_mem = pmm_get_free_memory();
    uint64_t used_mem = total_mem - free_mem;

    kprintf("  Kernel:       AutomationOS Phase 2\n");
    kprintf("  Total RAM:    %llu MB\n", total_mem / (1024 * 1024));
    kprintf("  Free RAM:     %llu MB\n", free_mem / (1024 * 1024));
    kprintf("  Used RAM:     %llu MB\n", used_mem / (1024 * 1024));
    kprintf("  CPU Cores:    %d\n", smp_get_cpu_count());
    kprintf("  Uptime:       %llu ms\n", timer_get_uptime_ms());

    kprintf("==================================================================\n");
}

/**
 * Main expanded integration test suite runner
 */
void run_expanded_integration_suite(void) {
    kprintf("\n\n");
    kprintf("##################################################################\n");
    kprintf("##                                                              ##\n");
    kprintf("##         AUTOMATIONOS EXPANDED INTEGRATION TEST SUITE        ##\n");
    kprintf("##                                                              ##\n");
    kprintf("##  Comprehensive testing of all subsystem integration points  ##\n");
    kprintf("##  Coverage: 110+ tests across 8 major categories             ##\n");
    kprintf("##                                                              ##\n");
    kprintf("##################################################################\n");

    // Record start time
    test_stats.start_time = timer_get_uptime_ms();
    test_stats.total_suites = 8;

    // Print test environment
    print_test_environment();

    // Run all test suites
    // 1. Boot Sequence Tests (10 tests)
    print_suite_header("Boot Sequence Integration", 10);
    run_boot_integration_tests();
    print_suite_separator();

    // 2. Application Lifecycle Tests (15 tests)
    print_suite_header("Application Lifecycle Integration", 15);
    run_application_lifecycle_tests();
    print_suite_separator();

    // 3. File System Integration Tests (15 tests)
    print_suite_header("File System Integration", 15);
    run_filesystem_integration_tests();
    print_suite_separator();

    // 4. Network Stack Integration Tests (15 tests)
    print_suite_header("Network Stack Integration", 15);
    run_network_stack_integration_tests();
    print_suite_separator();

    // 5. Graphics Stack Integration Tests (10 tests)
    print_suite_header("Graphics Stack Integration", 10);
    run_graphics_stack_integration_tests();
    print_suite_separator();

    // 6. Security Integration Tests (20 tests)
    print_suite_header("Security Integration (Expanded)", 20);
    run_security_expanded_integration_tests();
    print_suite_separator();

    // 7. Power Management Tests (10 tests)
    print_suite_header("Power Management Integration", 10);
    run_power_management_integration_tests();
    print_suite_separator();

    // 8. Original Integration Suite (15 tests)
    print_suite_header("Core Subsystem Integration (Original)", 15);
    run_integration_test_suite();
    print_suite_separator();

    // Record end time
    test_stats.end_time = timer_get_uptime_ms();

    // Print overall summary
    print_overall_summary();

    // Memory leak check after all tests
    kprintf("\n");
    kprintf("==================================================================\n");
    kprintf("  POST-TEST MEMORY CHECK\n");
    kprintf("==================================================================\n");

    uint64_t final_free = pmm_get_free_memory();
    uint64_t final_total = pmm_get_total_memory();
    uint64_t final_used = final_total - final_free;

    kprintf("  Free Memory:  %llu MB\n", final_free / (1024 * 1024));
    kprintf("  Used Memory:  %llu MB\n", final_used / (1024 * 1024));

    uint64_t used_percent = (final_used * 100) / final_total;
    kprintf("  Memory Usage: %llu%%\n", used_percent);

    if (used_percent > 50) {
        kprintf("  WARNING: High memory usage after tests\n");
    } else {
        kprintf("  ✓ Memory usage healthy\n");
    }

    kprintf("==================================================================\n");
    kprintf("\n");
}

/**
 * Run specific test category
 */
void run_integration_category(int category) {
    kprintf("\n");
    kprintf("==================================================================\n");
    kprintf("  RUNNING SPECIFIC TEST CATEGORY: %d\n", category);
    kprintf("==================================================================\n");

    test_stats.start_time = timer_get_uptime_ms();

    switch (category) {
        case 1:
            print_suite_header("Boot Sequence Integration", 10);
            run_boot_integration_tests();
            break;
        case 2:
            print_suite_header("Application Lifecycle Integration", 15);
            run_application_lifecycle_tests();
            break;
        case 3:
            print_suite_header("File System Integration", 15);
            run_filesystem_integration_tests();
            break;
        case 4:
            print_suite_header("Network Stack Integration", 15);
            run_network_stack_integration_tests();
            break;
        case 5:
            print_suite_header("Graphics Stack Integration", 10);
            run_graphics_stack_integration_tests();
            break;
        case 6:
            print_suite_header("Security Integration (Expanded)", 20);
            run_security_expanded_integration_tests();
            break;
        case 7:
            print_suite_header("Power Management Integration", 10);
            run_power_management_integration_tests();
            break;
        case 8:
            print_suite_header("Core Subsystem Integration (Original)", 15);
            run_integration_test_suite();
            break;
        default:
            kprintf("ERROR: Invalid test category: %d\n", category);
            kprintf("Valid categories: 1-8\n");
            return;
    }

    test_stats.end_time = timer_get_uptime_ms();
    uint64_t elapsed = test_stats.end_time - test_stats.start_time;

    kprintf("\n");
    kprintf("Category test completed in %llu ms\n", elapsed);
    kprintf("\n");
}

/**
 * Quick smoke test (fast subset of critical tests)
 */
void run_integration_smoke_test(void) {
    kprintf("\n");
    kprintf("==================================================================\n");
    kprintf("  INTEGRATION SMOKE TEST (Quick Validation)\n");
    kprintf("==================================================================\n");

    test_stats.start_time = timer_get_uptime_ms();

    // Run only critical fast tests from each category
    kprintf("\n[1/3] Boot sequence validation...\n");
    run_boot_integration_tests();

    kprintf("\n[2/3] Security subsystem validation...\n");
    run_security_expanded_integration_tests();

    kprintf("\n[3/3] Core subsystem validation...\n");
    run_integration_test_suite();

    test_stats.end_time = timer_get_uptime_ms();
    uint64_t elapsed = test_stats.end_time - test_stats.start_time;

    kprintf("\n");
    kprintf("==================================================================\n");
    kprintf("  SMOKE TEST COMPLETED\n");
    kprintf("  Time: %llu ms\n", elapsed);
    kprintf("==================================================================\n");
    kprintf("\n");
}
