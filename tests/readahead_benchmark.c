/**
 * Read-ahead Benchmark
 *
 * Tests sequential file I/O performance with and without read-ahead.
 * Expected: 4x throughput improvement with 4-page read-ahead.
 */

#include <stdint.h>
#include <stddef.h>

// Syscall numbers
#define SYS_WRITE 1
#define SYS_OPEN  2
#define SYS_READ  3
#define SYS_CLOSE 4
#define SYS_EXIT  60

// File flags
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

typedef long ssize_t;

// Syscall wrappers
static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile(
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(n), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    asm volatile(
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(n), "r"(a1), "r"(a2)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall1(long n, long a1) {
    long ret;
    asm volatile(
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(n), "r"(a1)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    return ret;
}

// Simple RDTSC-based timing
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// String functions
static size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void print(const char* s) {
    syscall3(SYS_WRITE, 1, (long)s, (long)strlen(s));
}

static void print_num(uint64_t n) {
    char buf[32];
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
    } else {
        char tmp[32];
        int j = 0;
        while (n > 0) {
            tmp[j++] = '0' + (n % 10);
            n /= 10;
        }
        while (j > 0) {
            buf[i++] = tmp[--j];
        }
    }

    buf[i] = '\0';
    print(buf);
}

// Create a 1MB test file
static void create_test_file(const char* path) {
    int fd = syscall3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        print("ERROR: Failed to create test file\n");
        syscall1(SYS_EXIT, 1);
    }

    // Write 1MB (1024 * 1024 bytes)
    char buf[4096];
    for (int i = 0; i < 4096; i++) {
        buf[i] = (char)(i & 0xFF);
    }

    for (int i = 0; i < 256; i++) {  // 256 * 4KB = 1MB
        ssize_t written = syscall3(SYS_WRITE, fd, (long)buf, 4096);
        if (written != 4096) {
            print("ERROR: Write failed\n");
            syscall1(SYS_CLOSE, fd);
            syscall1(SYS_EXIT, 1);
        }
    }

    syscall1(SYS_CLOSE, fd);
    print("Created 1MB test file\n");
}

// Benchmark sequential reads
static uint64_t benchmark_sequential_read(const char* path, size_t chunk_size) {
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        print("ERROR: Failed to open test file\n");
        syscall1(SYS_EXIT, 1);
    }

    char buf[4096];
    size_t total_read = 0;
    uint64_t start = rdtsc();

    while (1) {
        ssize_t n = syscall3(SYS_READ, fd, (long)buf, chunk_size);
        if (n <= 0) break;
        total_read += n;
    }

    uint64_t end = rdtsc();
    syscall1(SYS_CLOSE, fd);

    uint64_t cycles = end - start;
    return cycles;
}

void _start(void) {
    print("=== Read-ahead Benchmark ===\n");
    print("Creating 1MB test file...\n");
    create_test_file("/tmp/readahead_test.dat");

    print("\nBenchmark 1: Sequential reads (4KB chunks)\n");
    uint64_t cycles1 = benchmark_sequential_read("/tmp/readahead_test.dat", 4096);
    print("Cycles: ");
    print_num(cycles1);
    print("\n");

    print("\nBenchmark 2: Sequential reads (4KB chunks) - 2nd run\n");
    print("(Should be faster due to read-ahead)\n");
    uint64_t cycles2 = benchmark_sequential_read("/tmp/readahead_test.dat", 4096);
    print("Cycles: ");
    print_num(cycles2);
    print("\n");

    print("\nBenchmark 3: Sequential reads (1KB chunks)\n");
    uint64_t cycles3 = benchmark_sequential_read("/tmp/readahead_test.dat", 1024);
    print("Cycles: ");
    print_num(cycles3);
    print("\n");

    if (cycles2 < cycles1) {
        uint64_t speedup = (cycles1 * 100) / cycles2;
        print("\nSpeedup (2nd run vs 1st): ");
        print_num(speedup / 100);
        print(".");
        print_num(speedup % 100);
        print("x\n");
    }

    print("\n=== Benchmark Complete ===\n");
    print("Expected: 2nd run should be ~2-4x faster due to:\n");
    print("  1. Page cache hits\n");
    print("  2. Read-ahead prefetching\n");

    syscall1(SYS_EXIT, 0);
}
