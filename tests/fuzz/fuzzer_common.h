#ifndef FUZZER_COMMON_H
#define FUZZER_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

// ============================================================================
// Fuzzing Configuration
// ============================================================================

#define FUZZ_MAX_INPUT_SIZE     (1024 * 1024)  // 1MB max input
#define FUZZ_TIMEOUT_SECONDS    10             // Timeout per test case
#define FUZZ_DEFAULT_ITERATIONS 100000         // Default iteration count

// ============================================================================
// Syscall Numbers (matching kernel/include/syscall.h)
// ============================================================================

#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_WAITPID 6
#define SYS_EXECVE  7
#define SYS_GETPID  8
#define SYS_SLEEP   9

#define MAX_SYSCALLS 256

// ============================================================================
// Error Codes (matching kernel/include/syscall.h)
// ============================================================================

#define ESUCCESS    0
#define ENOTSUP     -1
#define EINVAL      -22
#define EBADF       -9
#define ENOMEM      -12
#define ESRCH       -3
#define EFAULT      -14

// ============================================================================
// Fuzzing Statistics
// ============================================================================

typedef struct {
    uint64_t iterations;
    uint64_t crashes;
    uint64_t hangs;
    uint64_t unique_crashes;
    uint64_t total_execs;
    double coverage;
    time_t start_time;
    time_t end_time;
} fuzz_stats_t;

static fuzz_stats_t g_stats = {0};

// ============================================================================
// Random Number Generation
// ============================================================================

static inline uint32_t fuzz_rand(void) {
    // xorshift32 PRNG (fast and good enough for fuzzing)
    static uint32_t seed = 0x12345678;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static inline uint64_t fuzz_rand64(void) {
    return ((uint64_t)fuzz_rand() << 32) | fuzz_rand();
}

static inline void fuzz_seed(uint32_t s) {
    srand(s);
}

// Generate random data into buffer
static inline void fuzz_random_data(void* buf, size_t size) {
    uint8_t* p = (uint8_t*)buf;
    for (size_t i = 0; i < size; i++) {
        p[i] = fuzz_rand() & 0xFF;
    }
}

// ============================================================================
// Memory Sanitization (Detect use-after-free, buffer overflow)
// ============================================================================

#define FUZZ_CANARY_VALUE 0xDEADBEEFCAFEBABEULL

typedef struct {
    uint64_t canary_before;
    size_t size;
    void* data;
    uint64_t canary_after;
} fuzz_alloc_t;

static inline void* fuzz_malloc(size_t size) {
    fuzz_alloc_t* alloc = malloc(sizeof(fuzz_alloc_t) + size + 16);
    if (!alloc) return NULL;

    alloc->canary_before = FUZZ_CANARY_VALUE;
    alloc->size = size;
    alloc->data = (void*)((uint8_t*)alloc + sizeof(fuzz_alloc_t));
    alloc->canary_after = FUZZ_CANARY_VALUE;

    // Write canary after data
    uint64_t* canary_ptr = (uint64_t*)((uint8_t*)alloc->data + size);
    *canary_ptr = FUZZ_CANARY_VALUE;

    return alloc->data;
}

static inline void fuzz_free(void* ptr) {
    if (!ptr) return;

    fuzz_alloc_t* alloc = (fuzz_alloc_t*)((uint8_t*)ptr - sizeof(fuzz_alloc_t));

    // Check canaries
    if (alloc->canary_before != FUZZ_CANARY_VALUE) {
        fprintf(stderr, "[FUZZER] HEAP CORRUPTION: Canary before corrupted!\n");
        abort();
    }

    uint64_t* canary_ptr = (uint64_t*)((uint8_t*)alloc->data + alloc->size);
    if (*canary_ptr != FUZZ_CANARY_VALUE) {
        fprintf(stderr, "[FUZZER] HEAP CORRUPTION: Canary after corrupted (buffer overflow)!\n");
        abort();
    }

    // Poison memory to detect use-after-free
    memset(alloc->data, 0xCC, alloc->size);
    free(alloc);
}

// ============================================================================
// Crash Detection
// ============================================================================

static volatile bool g_timeout_occurred = false;
static volatile bool g_crashed = false;

static void fuzz_alarm_handler(int sig) {
    (void)sig;
    g_timeout_occurred = true;
    fprintf(stderr, "[FUZZER] HANG DETECTED: Test case exceeded timeout\n");
    exit(2);  // Exit with distinct code for hangs
}

static void fuzz_crash_handler(int sig) {
    const char* signame = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: signame = "SIGSEGV"; break;
        case SIGILL:  signame = "SIGILL"; break;
        case SIGFPE:  signame = "SIGFPE"; break;
        case SIGABRT: signame = "SIGABRT"; break;
        case SIGBUS:  signame = "SIGBUS"; break;
    }

    fprintf(stderr, "[FUZZER] CRASH DETECTED: Signal %s (%d)\n", signame, sig);
    g_crashed = true;
    g_stats.crashes++;
    exit(1);  // Exit with crash code
}

static inline void fuzz_setup_handlers(void) {
    // Set up timeout handler
    signal(SIGALRM, fuzz_alarm_handler);

    // Set up crash handlers
    signal(SIGSEGV, fuzz_crash_handler);
    signal(SIGILL, fuzz_crash_handler);
    signal(SIGFPE, fuzz_crash_handler);
    signal(SIGABRT, fuzz_crash_handler);
    signal(SIGBUS, fuzz_crash_handler);
}

// ============================================================================
// Timeout Management
// ============================================================================

static inline void fuzz_set_timeout(unsigned int seconds) {
    alarm(seconds);
}

static inline void fuzz_clear_timeout(void) {
    alarm(0);
}

// ============================================================================
// Input Reading (AFL++ compatible)
// ============================================================================

static inline uint8_t* fuzz_read_input(const char* filename, size_t* out_size) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        perror("fopen");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > FUZZ_MAX_INPUT_SIZE) {
        fprintf(stderr, "[FUZZER] Input too large: %zu bytes (max %d)\n",
                size, FUZZ_MAX_INPUT_SIZE);
        fclose(f);
        return NULL;
    }

    uint8_t* buf = malloc(size);
    if (!buf) {
        perror("malloc");
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, size, f) != size) {
        perror("fread");
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = size;
    return buf;
}

// ============================================================================
// Statistics and Reporting
// ============================================================================

static inline void fuzz_init_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = time(NULL);
}

static inline void fuzz_update_stats(void) {
    g_stats.iterations++;
    g_stats.total_execs++;
}

static inline void fuzz_print_stats(void) {
    g_stats.end_time = time(NULL);
    double elapsed = difftime(g_stats.end_time, g_stats.start_time);
    double execs_per_sec = g_stats.total_execs / (elapsed > 0 ? elapsed : 1);

    printf("\n");
    printf("==================== FUZZING STATISTICS ====================\n");
    printf("Total Iterations:   %llu\n", (unsigned long long)g_stats.iterations);
    printf("Total Executions:   %llu\n", (unsigned long long)g_stats.total_execs);
    printf("Crashes Found:      %llu\n", (unsigned long long)g_stats.crashes);
    printf("Hangs Found:        %llu\n", (unsigned long long)g_stats.hangs);
    printf("Unique Crashes:     %llu\n", (unsigned long long)g_stats.unique_crashes);
    printf("Elapsed Time:       %.0f seconds\n", elapsed);
    printf("Exec/sec:           %.2f\n", execs_per_sec);
    printf("Coverage:           %.2f%%\n", g_stats.coverage);
    printf("===========================================================\n");
}

// ============================================================================
// Logging
// ============================================================================

#define FUZZ_LOG(fmt, ...) \
    fprintf(stdout, "[FUZZER] " fmt "\n", ##__VA_ARGS__)

#define FUZZ_ERROR(fmt, ...) \
    fprintf(stderr, "[FUZZER ERROR] " fmt "\n", ##__VA_ARGS__)

#define FUZZ_DEBUG(fmt, ...) \
    do { \
        if (getenv("FUZZ_DEBUG")) { \
            fprintf(stdout, "[FUZZER DEBUG] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while(0)

// ============================================================================
// Assertions
// ============================================================================

#define FUZZ_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FUZZER ASSERTION FAILED] %s at %s:%d\n", \
                    msg, __FILE__, __LINE__); \
            abort(); \
        } \
    } while(0)

// ============================================================================
// Test Case Management
// ============================================================================

typedef struct {
    uint8_t* data;
    size_t size;
    uint32_t checksum;
} fuzz_testcase_t;

static inline uint32_t fuzz_checksum(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

#endif // FUZZER_COMMON_H
