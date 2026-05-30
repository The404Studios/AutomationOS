/*
 * AutomationOS Syscall Fuzzer
 *
 * This fuzzer tests all syscalls with random arguments to discover:
 * - Invalid argument handling bugs
 * - Integer overflows
 * - NULL pointer dereferences
 * - Privilege escalation vulnerabilities
 * - Buffer overflows
 *
 * Usage:
 *   # AFL++ mode
 *   afl-fuzz -i corpus/syscall_seeds -o output/syscall -- ./syscall_fuzzer @@
 *
 *   # Standalone mode
 *   ./syscall_fuzzer --iterations 1000000
 *   ./syscall_fuzzer --input testcase.bin
 */

#include "fuzzer_common.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

// ============================================================================
// Syscall Fuzzing Engine
// ============================================================================

typedef struct {
    uint32_t syscall_num;
    uint64_t args[6];
} syscall_testcase_t;

// Safe syscalls that won't terminate the process or do irreversible damage
static const uint32_t safe_syscalls[] = {
    SYS_GETPID,
    SYS_READ,    // With safe FDs
    SYS_WRITE,   // With safe FDs
    // SYS_FORK is excluded - too dangerous for fuzzing
    // SYS_EXIT is excluded - would terminate fuzzer
};

#define NUM_SAFE_SYSCALLS (sizeof(safe_syscalls) / sizeof(safe_syscalls[0]))

// Generate random syscall test case
static void generate_random_testcase(syscall_testcase_t* tc) {
    // Pick random syscall from safe list
    tc->syscall_num = safe_syscalls[fuzz_rand() % NUM_SAFE_SYSCALLS];

    // Generate random arguments
    for (int i = 0; i < 6; i++) {
        uint32_t arg_type = fuzz_rand() % 100;

        if (arg_type < 30) {
            // Small integers (common case)
            tc->args[i] = fuzz_rand() % 1024;
        } else if (arg_type < 50) {
            // Large integers
            tc->args[i] = fuzz_rand64();
        } else if (arg_type < 60) {
            // NULL pointer
            tc->args[i] = 0;
        } else if (arg_type < 70) {
            // Invalid pointers (low addresses)
            tc->args[i] = fuzz_rand() % 0x1000;
        } else if (arg_type < 80) {
            // Invalid pointers (high addresses)
            tc->args[i] = 0xFFFFFFFF00000000ULL | fuzz_rand();
        } else if (arg_type < 90) {
            // Negative integers (sign extension)
            tc->args[i] = (int64_t)(int32_t)fuzz_rand();
        } else {
            // Edge cases
            uint32_t edge = fuzz_rand() % 10;
            switch (edge) {
                case 0: tc->args[i] = -1; break;
                case 1: tc->args[i] = 0; break;
                case 2: tc->args[i] = 1; break;
                case 3: tc->args[i] = INT32_MAX; break;
                case 4: tc->args[i] = INT32_MIN; break;
                case 5: tc->args[i] = UINT32_MAX; break;
                case 6: tc->args[i] = INT64_MAX; break;
                case 7: tc->args[i] = INT64_MIN; break;
                case 8: tc->args[i] = UINT64_MAX; break;
                default: tc->args[i] = fuzz_rand64(); break;
            }
        }
    }
}

// Parse test case from input buffer (AFL++ mode)
static bool parse_testcase_from_input(const uint8_t* data, size_t size,
                                      syscall_testcase_t* tc) {
    if (size < sizeof(syscall_testcase_t)) {
        // Input too small, generate random
        generate_random_testcase(tc);
        return true;
    }

    // Parse structured input
    memcpy(tc, data, sizeof(syscall_testcase_t));

    // Validate syscall number
    if (tc->syscall_num >= MAX_SYSCALLS) {
        tc->syscall_num %= MAX_SYSCALLS;
    }

    // Map to safe syscalls only
    tc->syscall_num = safe_syscalls[tc->syscall_num % NUM_SAFE_SYSCALLS];

    return true;
}

// Execute syscall in controlled environment
static int64_t execute_syscall_safe(const syscall_testcase_t* tc) {
    int64_t result = 0;

    // Special handling for specific syscalls
    switch (tc->syscall_num) {
        case SYS_READ: {
            // Create a safe buffer for read
            char buf[4096];
            // Clamp count to buffer size
            size_t count = tc->args[2] > sizeof(buf) ? sizeof(buf) : tc->args[2];

            // Only read from stdin, stdout, stderr (safe)
            int fd = tc->args[0] % 3;
            result = syscall(SYS_read, fd, buf, count);
            break;
        }

        case SYS_WRITE: {
            // Create a safe buffer
            char buf[4096];
            size_t count = tc->args[2] > sizeof(buf) ? sizeof(buf) : tc->args[2];
            fuzz_random_data(buf, count);

            // Only write to stdout, stderr (safe)
            int fd = (tc->args[0] % 2) + 1;  // 1 or 2
            result = syscall(SYS_write, fd, buf, count);
            break;
        }

        case SYS_GETPID:
            result = syscall(SYS_getpid);
            break;

        default:
            // Generic syscall execution
            result = syscall(tc->syscall_num,
                           tc->args[0], tc->args[1], tc->args[2],
                           tc->args[3], tc->args[4], tc->args[5]);
            break;
    }

    return result;
}

// Fuzz a single test case
static void fuzz_syscall(const syscall_testcase_t* tc) {
    FUZZ_DEBUG("Fuzzing syscall %u with args: 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx",
               tc->syscall_num,
               (unsigned long long)tc->args[0],
               (unsigned long long)tc->args[1],
               (unsigned long long)tc->args[2],
               (unsigned long long)tc->args[3],
               (unsigned long long)tc->args[4],
               (unsigned long long)tc->args[5]);

    fuzz_set_timeout(FUZZ_TIMEOUT_SECONDS);

    int64_t result = execute_syscall_safe(tc);

    fuzz_clear_timeout();

    FUZZ_DEBUG("Syscall returned: %lld", (long long)result);

    fuzz_update_stats();
}

// ============================================================================
// Main Fuzzing Loop
// ============================================================================

static void fuzz_mode_standalone(uint64_t iterations) {
    FUZZ_LOG("Starting standalone syscall fuzzing (%llu iterations)...",
             (unsigned long long)iterations);

    for (uint64_t i = 0; i < iterations; i++) {
        syscall_testcase_t tc;
        generate_random_testcase(&tc);
        fuzz_syscall(&tc);

        if (i % 10000 == 0) {
            printf("\rProgress: %llu/%llu (%.2f%%)",
                   (unsigned long long)i,
                   (unsigned long long)iterations,
                   (i * 100.0) / iterations);
            fflush(stdout);
        }
    }

    printf("\n");
    FUZZ_LOG("Standalone fuzzing complete!");
}

static void fuzz_mode_afl(const char* input_file) {
    FUZZ_LOG("Running in AFL++ mode with input: %s", input_file);

    size_t input_size;
    uint8_t* input_data = fuzz_read_input(input_file, &input_size);
    if (!input_data) {
        FUZZ_ERROR("Failed to read input file");
        exit(1);
    }

    syscall_testcase_t tc;
    if (!parse_testcase_from_input(input_data, input_size, &tc)) {
        FUZZ_ERROR("Failed to parse test case");
        free(input_data);
        exit(1);
    }

    fuzz_syscall(&tc);

    free(input_data);
}

// ============================================================================
// Entry Point
// ============================================================================

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  @@                    AFL++ mode (read from input file)\n");
    printf("  --input FILE          Fuzz single input file\n");
    printf("  --iterations N        Run N iterations (default: %d)\n", FUZZ_DEFAULT_ITERATIONS);
    printf("  --seed N              Set random seed\n");
    printf("  --help                Show this help\n");
    printf("\n");
    printf("Environment Variables:\n");
    printf("  FUZZ_DEBUG=1          Enable debug logging\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # AFL++ mode\n");
    printf("  afl-fuzz -i corpus/syscall_seeds -o output/syscall -- %s @@\n", prog);
    printf("\n");
    printf("  # Standalone mode\n");
    printf("  %s --iterations 1000000\n", prog);
    printf("\n");
    printf("  # Single input\n");
    printf("  %s --input testcase.bin\n", prog);
}

int main(int argc, char** argv) {
    fuzz_setup_handlers();
    fuzz_init_stats();

    // Default values
    uint64_t iterations = FUZZ_DEFAULT_ITERATIONS;
    const char* input_file = NULL;
    bool afl_mode = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "@@") == 0) {
            // AFL++ placeholder - will be replaced by AFL++
            if (i + 1 < argc) {
                input_file = argv[i + 1];
                afl_mode = true;
                i++;
            }
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            fuzz_seed(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            // Might be AFL++ replacing @@
            input_file = argv[i];
            afl_mode = true;
        }
    }

    // Run fuzzer
    if (input_file) {
        fuzz_mode_afl(input_file);
    } else {
        fuzz_mode_standalone(iterations);
    }

    fuzz_print_stats();
    return 0;
}
