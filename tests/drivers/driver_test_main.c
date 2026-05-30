#include "driver_test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

// External test registration functions
extern void register_serial_tests(void);
extern void register_ps2_tests(void);
extern void register_pit_tests(void);
extern void register_framebuffer_tests(void);

// Future driver tests (to be implemented)
extern void register_nvme_tests(void);
extern void register_ahci_tests(void);
extern void register_e1000_tests(void);
extern void register_i915_tests(void);
extern void register_hda_tests(void);
extern void register_xhci_tests(void);
extern void register_hid_tests(void);
extern void register_driver_integration_tests(void);

// Test configuration
typedef struct {
    bool run_all;
    bool run_serial;
    bool run_ps2;
    bool run_pit;
    bool run_framebuffer;
    bool run_storage;
    bool run_network;
    bool run_graphics;
    bool run_audio;
    bool run_usb;
    bool run_stress;
    bool verbose;
    bool quick;
    const char* specific_test;
} test_config_t;

// Print usage
static void print_usage(const char* program) {
    printf("AutomationOS Driver Test Suite\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program);
    printf("Options:\n");
    printf("  -a, --all              Run all tests\n");
    printf("  -s, --serial           Run serial driver tests\n");
    printf("  -p, --ps2              Run PS/2 keyboard tests\n");
    printf("  -t, --pit              Run PIT timer tests\n");
    printf("  -f, --framebuffer      Run framebuffer tests\n");
    printf("  --storage              Run storage driver tests (NVMe, AHCI)\n");
    printf("  --network              Run network driver tests (e1000)\n");
    printf("  --graphics             Run graphics driver tests (i915)\n");
    printf("  --audio                Run audio driver tests (HDA)\n");
    printf("  --usb                  Run USB driver tests (xHCI, HID)\n");
    printf("  --stress               Run stress tests only\n");
    printf("  --test <name>          Run specific test case\n");
    printf("  -q, --quick            Run quick tests only (skip stress tests)\n");
    printf("  -v, --verbose          Verbose output\n");
    printf("  -h, --help             Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --all                    # Run all tests\n", program);
    printf("  %s --serial --ps2           # Run serial and PS/2 tests\n", program);
    printf("  %s --test serial::init      # Run specific test\n", program);
    printf("  %s --storage --quick        # Quick storage tests\n", program);
    printf("  %s --stress                 # Run stress tests\n", program);
    printf("\n");
}

// Parse command line arguments
static void parse_args(int argc, char** argv, test_config_t* config) {
    static struct option long_options[] = {
        {"all",          no_argument,       0, 'a'},
        {"serial",       no_argument,       0, 's'},
        {"ps2",          no_argument,       0, 'p'},
        {"pit",          no_argument,       0, 't'},
        {"framebuffer",  no_argument,       0, 'f'},
        {"storage",      no_argument,       0, 'S'},
        {"network",      no_argument,       0, 'N'},
        {"graphics",     no_argument,       0, 'G'},
        {"audio",        no_argument,       0, 'A'},
        {"usb",          no_argument,       0, 'U'},
        {"stress",       no_argument,       0, 'X'},
        {"test",         required_argument, 0, 'T'},
        {"quick",        no_argument,       0, 'q'},
        {"verbose",      no_argument,       0, 'v'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "asptfqvh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'a':
                config->run_all = true;
                break;
            case 's':
                config->run_serial = true;
                break;
            case 'p':
                config->run_ps2 = true;
                break;
            case 't':
                config->run_pit = true;
                break;
            case 'f':
                config->run_framebuffer = true;
                break;
            case 'S':
                config->run_storage = true;
                break;
            case 'N':
                config->run_network = true;
                break;
            case 'G':
                config->run_graphics = true;
                break;
            case 'A':
                config->run_audio = true;
                break;
            case 'U':
                config->run_usb = true;
                break;
            case 'X':
                config->run_stress = true;
                break;
            case 'T':
                config->specific_test = optarg;
                break;
            case 'q':
                config->quick = true;
                break;
            case 'v':
                config->verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    // If no specific tests selected, run all
    if (!config->run_serial && !config->run_ps2 && !config->run_pit &&
        !config->run_framebuffer && !config->run_storage && !config->run_network &&
        !config->run_graphics && !config->run_audio && !config->run_usb &&
        !config->run_stress && !config->specific_test) {
        config->run_all = true;
    }
}

int main(int argc, char** argv) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║     AutomationOS Driver Test Suite v1.0              ║\n");
    printf("║     Comprehensive Hardware Driver Testing            ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Parse arguments
    test_config_t config = {0};
    parse_args(argc, argv, &config);

    // Initialize test framework
    test_framework_init();

    // Register phase 1 driver tests (existing drivers)
    if (config.run_all || config.run_serial) {
        printf("[*] Registering serial driver tests...\n");
        register_serial_tests();
    }

    if (config.run_all || config.run_ps2) {
        printf("[*] Registering PS/2 keyboard tests...\n");
        register_ps2_tests();
    }

    if (config.run_all || config.run_pit) {
        printf("[*] Registering PIT timer tests...\n");
        // register_pit_tests(); // TODO: Implement
        printf("    [SKIP] PIT tests not yet implemented\n");
    }

    if (config.run_all || config.run_framebuffer) {
        printf("[*] Registering framebuffer tests...\n");
        // register_framebuffer_tests(); // TODO: Implement
        printf("    [SKIP] Framebuffer tests not yet implemented\n");
    }

    // Register phase 2/3 driver tests (comprehensive driver tests)
    if (config.run_all || config.run_storage) {
        printf("[*] Registering storage driver tests...\n");
        register_nvme_tests();
        register_ahci_tests();
        printf("    [OK] NVMe and AHCI tests registered\n");
    }

    if (config.run_all || config.run_network) {
        printf("[*] Registering network driver tests...\n");
        register_e1000_tests();
        printf("    [OK] e1000 network tests registered\n");
    }

    if (config.run_all || config.run_graphics) {
        printf("[*] Registering graphics driver tests...\n");
        register_i915_tests();
        printf("    [OK] i915 graphics tests registered\n");
    }

    if (config.run_all || config.run_audio) {
        printf("[*] Registering audio driver tests...\n");
        register_hda_tests();
        printf("    [OK] HDA audio tests registered\n");
    }

    if (config.run_all || config.run_usb) {
        printf("[*] Registering USB driver tests...\n");
        register_xhci_tests();
        register_hid_tests();
        printf("    [OK] xHCI and HID tests registered\n");
    }

    // Register driver integration tests
    printf("[*] Registering driver integration tests...\n");
    register_driver_integration_tests();
    printf("    [OK] Driver integration tests registered\n");

    printf("\n");

    // Run tests
    test_result_t result;

    if (config.specific_test) {
        // Run specific test
        printf("[*] Running specific test: %s\n\n", config.specific_test);

        // Parse suite::test format
        char* suite_name = strdup(config.specific_test);
        char* test_name = strchr(suite_name, ':');

        if (test_name && test_name[1] == ':') {
            *test_name = '\0';
            test_name += 2;

            result = test_run_case(suite_name, test_name);
        } else {
            // Run entire suite
            result = test_run_suite(config.specific_test);
        }

        free(suite_name);
    } else {
        // Run all registered tests
        printf("[*] Starting test execution...\n\n");
        result = test_run_all();
    }

    // Check for memory leaks
    printf("\n[*] Checking for memory leaks...\n");
    if (test_memory_check_leaks()) {
        printf("    [OK] No memory leaks detected\n");
    } else {
        printf("    [WARN] Memory leaks detected!\n");
        result = TEST_FAIL;
    }

    // Print final result
    printf("\n");
    if (result == TEST_PASS) {
        printf("╔═══════════════════════════════════════════════════════╗\n");
        printf("║                  ✓ ALL TESTS PASSED                   ║\n");
        printf("╚═══════════════════════════════════════════════════════╝\n");
        return 0;
    } else {
        printf("╔═══════════════════════════════════════════════════════╗\n");
        printf("║                  ✗ SOME TESTS FAILED                  ║\n");
        printf("╚═══════════════════════════════════════════════════════╝\n");
        return 1;
    }
}
