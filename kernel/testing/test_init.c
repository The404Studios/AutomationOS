#include "../include/ktest.h"
#include "../include/kernel.h"

/*
 * Test Initialization and Boot Integration
 * This file handles initializing tests during kernel boot
 */

// Boot flag to control testing
static bool g_tests_enabled = true;
static bool g_verbose_mode = true;
static const char* g_test_filter = NULL;

// Parse boot parameters for test configuration
void parse_test_boot_params(const char* cmdline) {
    if (!cmdline) return;

    // Look for ktest parameters
    const char* ktest_param = kstrstr(cmdline, "ktest=");
    if (ktest_param) {
        ktest_param += 6;  // Skip "ktest="

        if (kstrncmp(ktest_param, "off", 3) == 0) {
            g_tests_enabled = false;
            kprintf("[KTEST] Testing disabled via boot parameter\n");
        } else if (kstrncmp(ktest_param, "verbose", 7) == 0) {
            g_verbose_mode = true;
            kprintf("[KTEST] Verbose mode enabled\n");
        } else if (kstrncmp(ktest_param, "quiet", 5) == 0) {
            g_verbose_mode = false;
            kprintf("[KTEST] Quiet mode enabled\n");
        } else {
            // Treat as filter string
            g_test_filter = ktest_param;
            kprintf("[KTEST] Filter: %s\n", g_test_filter);
        }
    }
}

// Initialize testing framework during kernel boot
void kernel_tests_init(void) {
    if (!g_tests_enabled) {
        kprintf("[KTEST] Testing is disabled\n");
        return;
    }

    kprintf("\n");
    kprintf("==========================================\n");
    kprintf("  Initializing Kernel Test Framework\n");
    kprintf("==========================================\n");

    // Initialize test framework
    ktest_init();
    ktest_set_enabled(g_tests_enabled);
    ktest_set_verbose(g_verbose_mode);

    if (g_test_filter) {
        ktest_set_filter(g_test_filter);
    }

    kprintf("[KTEST] Framework initialized\n");
    kprintf("\n");
}

// Run all kernel tests
void kernel_tests_run(void) {
    if (!g_tests_enabled) {
        return;
    }

    kprintf("\n");
    kprintf("==========================================\n");
    kprintf("  Running Kernel Tests\n");
    kprintf("==========================================\n");
    kprintf("\n");

    uint64_t start_time = rdtsc();

    // Run all registered tests
    ktest_run_all();

    uint64_t end_time = rdtsc();
    uint64_t elapsed = end_time - start_time;

    kprintf("\n");
    kprintf("==========================================\n");
    kprintf("  Test Execution Complete\n");
    kprintf("  Total Time: %llu cycles\n", elapsed);
    kprintf("==========================================\n");
    kprintf("\n");

    // Get final statistics
    ktest_stats_t stats = ktest_get_stats();

    if (stats.failed > 0) {
        kprintf("[KTEST] WARNING: %u test(s) failed!\n", stats.failed);
        kprintf("[KTEST] System may be unstable\n");
    } else {
        kprintf("[KTEST] All tests passed successfully\n");
    }
}

// Run tests for specific subsystem (called from subsystem init)
void kernel_test_subsystem(const char* subsystem_name) {
    if (!g_tests_enabled) {
        return;
    }

    kprintf("[KTEST] Running tests for: %s\n", subsystem_name);
    ktest_run_suite(subsystem_name);
}

// Check if testing is enabled
bool kernel_tests_enabled(void) {
    return g_tests_enabled;
}

// Enable/disable testing at runtime
void kernel_tests_set_enabled(bool enabled) {
    g_tests_enabled = enabled;
    ktest_set_enabled(enabled);
}

// Query test statistics
ktest_stats_t kernel_tests_get_stats(void) {
    return ktest_get_stats();
}

// Run tests in phases (early boot, late boot, etc.)
typedef enum {
    TEST_PHASE_EARLY,    // Basic subsystems (PMM, VMM)
    TEST_PHASE_MIDDLE,   // Core subsystems (heap, scheduler)
    TEST_PHASE_LATE,     // High-level subsystems (syscall, fs)
    TEST_PHASE_ALL
} test_phase_t;

void kernel_tests_run_phase(test_phase_t phase) {
    if (!g_tests_enabled) {
        return;
    }

    const char* phase_name;
    const char* suites[10];
    int suite_count = 0;

    switch (phase) {
        case TEST_PHASE_EARLY:
            phase_name = "Early Boot Tests";
            suites[suite_count++] = "pmm";
            suites[suite_count++] = "vmm";
            suites[suite_count++] = "string";
            break;

        case TEST_PHASE_MIDDLE:
            phase_name = "Middle Boot Tests";
            suites[suite_count++] = "heap";
            suites[suite_count++] = "sched";
            break;

        case TEST_PHASE_LATE:
            phase_name = "Late Boot Tests";
            suites[suite_count++] = "syscall";
            break;

        case TEST_PHASE_ALL:
            phase_name = "All Tests";
            ktest_run_all();
            return;

        default:
            kprintf("[KTEST] Unknown test phase: %d\n", phase);
            return;
    }

    kprintf("\n");
    kprintf("[KTEST] ========================================\n");
    kprintf("[KTEST] %s\n", phase_name);
    kprintf("[KTEST] ========================================\n");

    for (int i = 0; i < suite_count; i++) {
        ktest_run_suite(suites[i]);
    }

    kprintf("[KTEST] %s complete\n", phase_name);
    kprintf("\n");
}

// Export test results for logging/debugging
void kernel_tests_export_results(void) {
    ktest_stats_t stats = ktest_get_stats();

    kprintf("\n");
    kprintf("========== Test Results ==========\n");
    kprintf("Total Tests:   %u\n", stats.total);
    kprintf("Passed Tests:  %u\n", stats.passed);
    kprintf("Failed Tests:  %u\n", stats.failed);
    kprintf("Skipped Tests: %u\n", stats.skipped);
    kprintf("Start Time:    %llu\n", stats.start_time);
    kprintf("End Time:      %llu\n", stats.end_time);
    kprintf("Duration:      %llu cycles\n", stats.end_time - stats.start_time);

    if (stats.total > 0) {
        uint32_t pass_rate = (stats.passed * 100) / stats.total;
        kprintf("Pass Rate:     %u%%\n", pass_rate);
    }

    kprintf("==================================\n");
    kprintf("\n");
}

// Hook for kernel panic - report test that was running
void kernel_tests_on_panic(void) {
    kprintf("\n");
    kprintf("[KTEST] !!!!! KERNEL PANIC DURING TESTING !!!!!\n");

    ktest_stats_t stats = ktest_get_stats();
    kprintf("[KTEST] Tests completed before panic: %u/%u\n",
           stats.passed + stats.failed, stats.total);

    // TODO: Report which test was running when panic occurred
    kprintf("[KTEST] Check test output above for failure details\n");
    kprintf("\n");
}
