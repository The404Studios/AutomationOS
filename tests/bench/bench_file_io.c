/**
 * File I/O Performance Benchmark
 *
 * Measures sequential file read performance with and without read-ahead.
 * Tests the effectiveness of the read-ahead and sendfile optimizations.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// Syscall numbers
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_READ 2
#define SYS_WRITE 3
#define SYS_GET_TICKS_MS 40
#define SYS_SENDFILE 71

// Open flags
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_fence(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtscp(void) {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

static inline int64_t syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t num, uint64_t arg1) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(arg1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg3;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall4(uint64_t num, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline uint64_t get_ticks_ms(void) {
    return syscall0(SYS_GET_TICKS_MS);
}

/**
 * Benchmark sequential file read with 4KB chunks
 *
 * Expected without read-ahead: ~50 MB/s
 * Expected with read-ahead: ~200 MB/s (4x improvement)
 */
void bench_sequential_read(void) {
    printf("\n[BENCH] Sequential File Read (4KB chunks)\n");
    printf("==========================================\n");

    const char* test_file = "/tmp/benchmark_file.bin";
    const size_t chunk_size = 4096;
    const size_t total_size = 1024 * 1024;  // 1 MB
    const int iterations = total_size / chunk_size;

    // Create test file
    int fd = syscall3(SYS_OPEN, (uint64_t)test_file, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        printf("[ERROR] Failed to create test file\n");
        return;
    }

    // Fill with test data
    char* write_buf = malloc(chunk_size);
    memset(write_buf, 0xAA, chunk_size);
    for (int i = 0; i < iterations; i++) {
        syscall3(SYS_WRITE, fd, (uint64_t)write_buf, chunk_size);
    }
    syscall1(SYS_CLOSE, fd);
    free(write_buf);

    // Benchmark read
    char* read_buf = malloc(chunk_size);

    printf("[BENCH] Reading %zu bytes in %d chunks of %zu bytes...\n",
           total_size, iterations, chunk_size);

    uint64_t start_cycles = rdtsc_fence();
    uint64_t start_ms = get_ticks_ms();

    fd = syscall3(SYS_OPEN, (uint64_t)test_file, O_RDONLY, 0);
    if (fd < 0) {
        printf("[ERROR] Failed to open test file\n");
        free(read_buf);
        return;
    }

    for (int i = 0; i < iterations; i++) {
        ssize_t n = syscall3(SYS_READ, fd, (uint64_t)read_buf, chunk_size);
        if (n != chunk_size) {
            printf("[ERROR] Short read at iteration %d: %ld\n", i, n);
            break;
        }
    }

    uint64_t end_cycles = rdtscp();
    uint64_t end_ms = get_ticks_ms();

    syscall1(SYS_CLOSE, fd);
    free(read_buf);

    // Calculate performance
    uint64_t elapsed_cycles = end_cycles - start_cycles;
    uint64_t elapsed_ms = end_ms - start_ms;

    // Assume 3 GHz CPU
    uint64_t cpu_freq_mhz = 3000;
    uint64_t mb_per_sec = 0;
    if (elapsed_ms > 0) {
        mb_per_sec = (total_size / 1024) / elapsed_ms;  // KB/ms = MB/s
    }

    printf("[BENCH] Total cycles: %llu\n", (unsigned long long)elapsed_cycles);
    printf("[BENCH] Time: %llu ms\n", (unsigned long long)elapsed_ms);
    printf("[BENCH] Throughput: %llu MB/s\n", (unsigned long long)mb_per_sec);
    printf("[BENCH] Cycles per byte: %llu\n",
           (unsigned long long)(elapsed_cycles / total_size));

    if (mb_per_sec >= 150) {
        printf("[PASS] Read-ahead is working well (>150 MB/s)\n");
    } else if (mb_per_sec >= 80) {
        printf("[INFO] Moderate performance (80-150 MB/s)\n");
    } else {
        printf("[WARN] Low throughput (%llu MB/s, expected >80 MB/s)\n",
               (unsigned long long)mb_per_sec);
    }

    printf("\n");
}

/**
 * Benchmark different block sizes
 */
void bench_block_sizes(void) {
    printf("\n[BENCH] Read Performance vs Block Size\n");
    printf("=======================================\n");

    const char* test_file = "/tmp/benchmark_file.bin";
    const size_t total_size = 1024 * 1024;  // 1 MB
    const size_t block_sizes[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    const int num_sizes = sizeof(block_sizes) / sizeof(block_sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        size_t block_size = block_sizes[s];
        int iterations = total_size / block_size;

        char* buf = malloc(block_size);
        if (!buf) continue;

        uint64_t start_ms = get_ticks_ms();

        int fd = syscall3(SYS_OPEN, (uint64_t)test_file, O_RDONLY, 0);
        if (fd < 0) {
            free(buf);
            continue;
        }

        for (int i = 0; i < iterations; i++) {
            syscall3(SYS_READ, fd, (uint64_t)buf, block_size);
        }

        uint64_t end_ms = get_ticks_ms();
        syscall1(SYS_CLOSE, fd);
        free(buf);

        uint64_t elapsed_ms = end_ms - start_ms;
        uint64_t mb_per_sec = 0;
        if (elapsed_ms > 0) {
            mb_per_sec = (total_size / 1024) / elapsed_ms;
        }

        printf("  Block size %6zu bytes: %4llu MB/s (%llu ms)\n",
               block_size, (unsigned long long)mb_per_sec,
               (unsigned long long)elapsed_ms);
    }

    printf("\n");
}

/**
 * Benchmark sendfile zero-copy transfer
 */
void bench_sendfile(void) {
    printf("\n[BENCH] Sendfile Zero-Copy Transfer\n");
    printf("====================================\n");

    const char* src_file = "/tmp/benchmark_file.bin";
    const size_t file_size = 1024 * 1024;  // 1 MB

    int src_fd = syscall3(SYS_OPEN, (uint64_t)src_file, O_RDONLY, 0);
    if (src_fd < 0) {
        printf("[ERROR] Failed to open source file\n");
        return;
    }

    // For this benchmark, we'll measure the sendfile syscall overhead
    // In a real scenario, this would send to a socket
    uint64_t start_cycles = rdtsc_fence();

    // Note: This would normally send to a socket fd
    // For benchmarking, we're just measuring the syscall overhead
    int64_t sent = syscall4(SYS_SENDFILE, 1, src_fd, 0, file_size);

    uint64_t end_cycles = rdtscp();

    syscall1(SYS_CLOSE, src_fd);

    if (sent > 0) {
        uint64_t cycles = end_cycles - start_cycles;
        uint64_t cpu_freq_mhz = 3000;
        uint64_t us = cycles / cpu_freq_mhz;

        printf("[BENCH] Sent %lld bytes\n", (long long)sent);
        printf("[BENCH] Cycles: %llu\n", (unsigned long long)cycles);
        printf("[BENCH] Time: ~%llu us\n", (unsigned long long)us);

        if (us > 0) {
            uint64_t mb_per_sec = (sent / us);  // bytes/us = MB/s
            printf("[BENCH] Throughput: ~%llu MB/s\n", (unsigned long long)mb_per_sec);
        }
    } else {
        printf("[INFO] Sendfile not fully implemented or returned %lld\n", (long long)sent);
    }

    printf("\n");
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  FILE I/O BENCHMARK SUITE\n");
    printf("=============================================\n");

    bench_sequential_read();
    bench_block_sizes();
    bench_sendfile();

    printf("=============================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("=============================================\n");
    printf("\nExpected Results:\n");
    printf("  Sequential read (4KB):  >150 MB/s (with read-ahead)\n");
    printf("  Larger blocks:          Higher throughput\n");
    printf("  Sendfile:               >200 MB/s (zero-copy)\n");
    printf("\n");

    return 0;
}
