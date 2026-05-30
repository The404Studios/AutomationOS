/*
 * String Function Performance Benchmark
 * =====================================
 *
 * Benchmarks optimized string functions to verify 8x speedup target.
 *
 * Measures:
 *  - memcpy performance for small (16B), medium (256B), large (4KB)
 *  - memset performance for small, medium, large
 *  - memmove performance (non-overlapping)
 *
 * Expected results:
 *  - Small (<64B): ~1-2x speedup (byte-by-byte fast path)
 *  - Medium (256B): ~4-6x speedup (partial alignment overhead)
 *  - Large (4KB): ~8x speedup (fully aligned 64-bit operations)
 */

#include "../include/types.h"
#include "../include/kernel.h"

// Forward declarations
extern void* memcpy(void* dest, const void* src, size_t count);
extern void* memset(void* dest, int val, size_t count);
extern void* memmove(void* dest, const void* src, size_t count);

// RDTSC timing functions
static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"  // Serialize
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    );
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    __asm__ volatile (
        "rdtscp\n\t"  // Serialize + read
        "mov %%eax, %0\n\t"
        "mov %%edx, %1\n\t"
        "lfence"  // Prevent reordering
        : "=r"(lo), "=r"(hi)
        :: "%rax", "%rdx", "%rcx"
    );
    return ((uint64_t)hi << 32) | lo;
}

// Benchmark helpers
#define BENCHMARK_ITERATIONS 1000

static uint64_t benchmark_memcpy(size_t size) {
    // Allocate buffers (aligned)
    static uint8_t src_buf[4096] __attribute__((aligned(64)));
    static uint8_t dst_buf[4096] __attribute__((aligned(64)));

    // Initialize source buffer
    for (size_t i = 0; i < size; i++) {
        src_buf[i] = (uint8_t)i;
    }

    // Warm up cache
    for (int i = 0; i < 10; i++) {
        memcpy(dst_buf, src_buf, size);
    }

    // Measure performance
    uint64_t start = rdtsc_start();
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        memcpy(dst_buf, src_buf, size);
    }
    uint64_t end = rdtsc_end();

    return (end - start) / BENCHMARK_ITERATIONS;
}

static uint64_t benchmark_memset(size_t size) {
    static uint8_t buf[4096] __attribute__((aligned(64)));

    // Warm up cache
    for (int i = 0; i < 10; i++) {
        memset(buf, 0x42, size);
    }

    // Measure performance
    uint64_t start = rdtsc_start();
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        memset(buf, 0x42, size);
    }
    uint64_t end = rdtsc_end();

    return (end - start) / BENCHMARK_ITERATIONS;
}

static uint64_t benchmark_memmove(size_t size) {
    static uint8_t src_buf[4096] __attribute__((aligned(64)));
    static uint8_t dst_buf[4096] __attribute__((aligned(64)));

    // Initialize source buffer
    for (size_t i = 0; i < size; i++) {
        src_buf[i] = (uint8_t)i;
    }

    // Warm up cache
    for (int i = 0; i < 10; i++) {
        memmove(dst_buf, src_buf, size);
    }

    // Measure performance
    uint64_t start = rdtsc_start();
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        memmove(dst_buf, src_buf, size);
    }
    uint64_t end = rdtsc_end();

    return (end - start) / BENCHMARK_ITERATIONS;
}

/**
 * Run string function benchmarks
 *
 * Call this from kernel_main() to measure string function performance.
 */
void string_benchmark_run(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("String Function Performance Benchmark\n");
    kprintf("========================================\n");
    kprintf("\n");
    kprintf("Iterations: %d per benchmark\n", BENCHMARK_ITERATIONS);
    kprintf("Measuring: Average cycles per operation\n");
    kprintf("\n");

    // memcpy benchmarks
    kprintf("memcpy Performance:\n");
    kprintf("------------------\n");

    uint64_t memcpy_16 = benchmark_memcpy(16);
    kprintf("  16 bytes:   %llu cycles (%llu.%02llu cycles/byte)\n",
            memcpy_16, memcpy_16 / 16, (memcpy_16 * 100 / 16) % 100);

    uint64_t memcpy_64 = benchmark_memcpy(64);
    kprintf("  64 bytes:   %llu cycles (%llu.%02llu cycles/byte)\n",
            memcpy_64, memcpy_64 / 64, (memcpy_64 * 100 / 64) % 100);

    uint64_t memcpy_256 = benchmark_memcpy(256);
    kprintf("  256 bytes:  %llu cycles (%llu.%02llu cycles/byte)\n",
            memcpy_256, memcpy_256 / 256, (memcpy_256 * 100 / 256) % 100);

    uint64_t memcpy_4k = benchmark_memcpy(4096);
    kprintf("  4KB:        %llu cycles (%llu.%02llu cycles/byte)\n",
            memcpy_4k, memcpy_4k / 4096, (memcpy_4k * 100 / 4096) % 100);

    kprintf("\n");

    // memset benchmarks
    kprintf("memset Performance:\n");
    kprintf("------------------\n");

    uint64_t memset_16 = benchmark_memset(16);
    kprintf("  16 bytes:   %llu cycles (%llu.%02llu cycles/byte)\n",
            memset_16, memset_16 / 16, (memset_16 * 100 / 16) % 100);

    uint64_t memset_64 = benchmark_memset(64);
    kprintf("  64 bytes:   %llu cycles (%llu.%02llu cycles/byte)\n",
            memset_64, memset_64 / 64, (memset_64 * 100 / 64) % 100);

    uint64_t memset_256 = benchmark_memset(256);
    kprintf("  256 bytes:  %llu cycles (%llu.%02llu cycles/byte)\n",
            memset_256, memset_256 / 256, (memset_256 * 100 / 256) % 100);

    uint64_t memset_4k = benchmark_memset(4096);
    kprintf("  4KB:        %llu cycles (%llu.%02llu cycles/byte)\n",
            memset_4k, memset_4k / 4096, (memset_4k * 100 / 4096) % 100);

    kprintf("\n");

    // memmove benchmarks
    kprintf("memmove Performance (non-overlapping):\n");
    kprintf("--------------------------------------\n");

    uint64_t memmove_16 = benchmark_memmove(16);
    kprintf("  16 bytes:   %llu cycles (%llu.%02llu cycles/byte)\n",
            memmove_16, memmove_16 / 16, (memmove_16 * 100 / 16) % 100);

    uint64_t memmove_64 = benchmark_memmove(64);
    kprintf("  64 bytes:   %llu cycles (%llu.%02llu cycles/byte)\n",
            memmove_64, memmove_64 / 64, (memmove_64 * 100 / 64) % 100);

    uint64_t memmove_256 = benchmark_memmove(256);
    kprintf("  256 bytes:  %llu cycles (%llu.%02llu cycles/byte)\n",
            memmove_256, memmove_256 / 256, (memmove_256 * 100 / 256) % 100);

    uint64_t memmove_4k = benchmark_memmove(4096);
    kprintf("  4KB:        %llu cycles (%llu.%02llu cycles/byte)\n",
            memmove_4k, memmove_4k / 4096, (memmove_4k * 100 / 4096) % 100);

    kprintf("\n");

    // Performance summary
    kprintf("Performance Summary:\n");
    kprintf("-------------------\n");
    kprintf("Expected performance (optimized vs byte-by-byte):\n");
    kprintf("  - Small (<64B):  ~1-2x speedup\n");
    kprintf("  - Medium (256B): ~4-6x speedup\n");
    kprintf("  - Large (4KB):   ~8x speedup\n");
    kprintf("\n");
    kprintf("Target cycles/byte (large operations):\n");
    kprintf("  - Baseline (byte-by-byte): ~1-2 cycles/byte\n");
    kprintf("  - Optimized (64-bit words): ~0.125-0.25 cycles/byte\n");
    kprintf("\n");

    // Verify correctness
    kprintf("Correctness Verification:\n");
    kprintf("------------------------\n");

    static uint8_t test_src[256];
    static uint8_t test_dst[256];

    // Initialize test data
    for (int i = 0; i < 256; i++) {
        test_src[i] = (uint8_t)i;
    }

    // Test memcpy
    memset(test_dst, 0, 256);
    memcpy(test_dst, test_src, 256);
    int memcpy_ok = 1;
    for (int i = 0; i < 256; i++) {
        if (test_dst[i] != (uint8_t)i) {
            memcpy_ok = 0;
            break;
        }
    }
    kprintf("  memcpy:  %s\n", memcpy_ok ? "PASS" : "FAIL");

    // Test memset
    memset(test_dst, 0xAA, 256);
    int memset_ok = 1;
    for (int i = 0; i < 256; i++) {
        if (test_dst[i] != 0xAA) {
            memset_ok = 0;
            break;
        }
    }
    kprintf("  memset:  %s\n", memset_ok ? "PASS" : "FAIL");

    // Test memmove (non-overlapping)
    memset(test_dst, 0, 256);
    memmove(test_dst, test_src, 256);
    int memmove_ok = 1;
    for (int i = 0; i < 256; i++) {
        if (test_dst[i] != (uint8_t)i) {
            memmove_ok = 0;
            break;
        }
    }
    kprintf("  memmove: %s\n", memmove_ok ? "PASS" : "FAIL");

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("Benchmark Complete\n");
    kprintf("========================================\n");
    kprintf("\n");
}
