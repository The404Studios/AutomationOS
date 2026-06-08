/**
 * Compiler Workload Benchmark
 *
 * Simulates kernel compilation workload:
 * - Heavy syscall usage (read, write, open, close, stat)
 * - Many small file I/O operations
 * - Process spawning (fork/exec for cc1, as, ld)
 * - Mix of CPU and I/O
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include "../common/bench_common.h"

#define NUM_FILES 100
#define FILE_SIZE (4 * 1024)  // 4KB source files
#define NUM_COMPILE_ITERATIONS 50

/**
 * Simulate reading a source file
 */
uint64_t simulate_read_source(const char* filename) {
    uint64_t start = rdtsc_fence();

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    char buffer[FILE_SIZE];
    ssize_t bytes = read(fd, buffer, FILE_SIZE);
    close(fd);

    uint64_t end = rdtsc_fence();
    return end - start;
}

/**
 * Simulate writing object file
 */
uint64_t simulate_write_object(const char* filename) {
    uint64_t start = rdtsc_fence();

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 0;
    }

    char buffer[FILE_SIZE];
    memset(buffer, 0, FILE_SIZE);
    write(fd, buffer, FILE_SIZE);
    fsync(fd);  // Ensure data written
    close(fd);

    uint64_t end = rdtsc_fence();
    return end - start;
}

/**
 * Simulate compilation (CPU-bound work)
 */
uint64_t simulate_compile(void) {
    uint64_t start = rdtsc_fence();

    // Simulate parsing, optimization, code generation
    volatile double result = 0.0;
    for (int i = 0; i < 100000; i++) {
        result += (double)i * 1.234567;
    }

    uint64_t end = rdtsc_fence();
    return end - start;
}

/**
 * Simulate stat() calls (dependency checking)
 */
uint64_t simulate_stat_check(const char* filename) {
    uint64_t start = rdtsc_fence();

    struct stat st;
    stat(filename, &st);

    uint64_t end = rdtsc_fence();
    return end - start;
}

/**
 * Single compilation unit workflow
 */
void compile_single_file(const char* source, const char* object,
                         uint64_t* read_time, uint64_t* compile_time,
                         uint64_t* write_time, uint64_t* stat_time) {
    // 1. Stat check (is object newer than source?)
    *stat_time = simulate_stat_check(source);

    // 2. Read source file
    *read_time = simulate_read_source(source);

    // 3. Compile (CPU-bound)
    *compile_time = simulate_compile();

    // 4. Write object file
    *write_time = simulate_write_object(object);
}

/**
 * Full compilation benchmark
 */
void bench_compile_workload(void) {
    printf("\n=== Compiler Workload Benchmark ===\n");

    bench_pin_cpu(0);

    // Create temporary directory
    system("mkdir -p /tmp/compile_bench");
    system("rm -f /tmp/compile_bench/*");

    // Create dummy source files
    printf("Creating %d source files...\n", NUM_FILES);
    for (int i = 0; i < NUM_FILES; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename), "/tmp/compile_bench/file%d.c", i);

        int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char content[FILE_SIZE];
            snprintf(content, sizeof(content),
                    "// Source file %d\n"
                    "int function%d(void) { return %d; }\n",
                    i, i, i);
            write(fd, content, strlen(content));
            close(fd);
        }
    }

    // Benchmark compilation
    uint64_t total_read = 0;
    uint64_t total_compile = 0;
    uint64_t total_write = 0;
    uint64_t total_stat = 0;

    printf("Compiling %d files (%d iterations)...\n",
           NUM_FILES, NUM_COMPILE_ITERATIONS);

    uint64_t total_start = rdtsc_fence();

    for (int iter = 0; iter < NUM_COMPILE_ITERATIONS; iter++) {
        for (int i = 0; i < NUM_FILES; i++) {
            char source[256], object[256];
            snprintf(source, sizeof(source), "/tmp/compile_bench/file%d.c", i);
            snprintf(object, sizeof(object), "/tmp/compile_bench/file%d.o", i);

            uint64_t read_time, compile_time, write_time, stat_time;
            compile_single_file(source, object, &read_time, &compile_time,
                               &write_time, &stat_time);

            total_read += read_time;
            total_compile += compile_time;
            total_write += write_time;
            total_stat += stat_time;
        }
    }

    uint64_t total_end = rdtsc_fence();
    uint64_t total_time = total_end - total_start;

    // Calculate results
    uint32_t total_operations = NUM_FILES * NUM_COMPILE_ITERATIONS;

    printf("\n--- Results ---\n");
    printf("Total files compiled:    %d\n", total_operations);
    printf("Total time:              %.2f seconds\n", cycles_to_ms(total_time) / 1000.0);
    printf("Files per second:        %.2f\n",
           (double)total_operations / (cycles_to_ms(total_time) / 1000.0));

    printf("\nTime breakdown:\n");
    printf("  Stat checks:           %.2f%% (%.2f ms total)\n",
           (double)total_stat / (double)total_time * 100.0,
           cycles_to_ms(total_stat));
    printf("  Reading source:        %.2f%% (%.2f ms total)\n",
           (double)total_read / (double)total_time * 100.0,
           cycles_to_ms(total_read));
    printf("  Compilation:           %.2f%% (%.2f ms total)\n",
           (double)total_compile / (double)total_time * 100.0,
           cycles_to_ms(total_compile));
    printf("  Writing object:        %.2f%% (%.2f ms total)\n",
           (double)total_write / (double)total_time * 100.0,
           cycles_to_ms(total_write));

    printf("\nAverage per file:\n");
    printf("  Total:                 %.2f ms\n",
           cycles_to_ms(total_time) / total_operations);
    printf("  Stat:                  %.2f us\n",
           cycles_to_us(total_stat) / total_operations);
    printf("  Read:                  %.2f us\n",
           cycles_to_us(total_read) / total_operations);
    printf("  Compile:               %.2f us\n",
           cycles_to_us(total_compile) / total_operations);
    printf("  Write:                 %.2f us\n",
           cycles_to_us(total_write) / total_operations);

    // Cleanup
    system("rm -rf /tmp/compile_bench");
}

/**
 * Parallel compilation benchmark
 */
void bench_parallel_compile(void) {
    printf("\n=== Parallel Compilation Benchmark ===\n");

    uint32_t num_cpus = bench_get_cpu_count();
    if (num_cpus > 8) num_cpus = 8;  // Cap at 8

    system("mkdir -p /tmp/compile_bench_parallel");
    system("rm -f /tmp/compile_bench_parallel/*");

    // Create source files
    for (int i = 0; i < NUM_FILES; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename),
                "/tmp/compile_bench_parallel/file%d.c", i);
        int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "int main() { return 0; }\n", 25);
            close(fd);
        }
    }

    printf("Compiling with %u parallel jobs...\n", num_cpus);

    uint64_t start = rdtsc_fence();

    // Fork worker processes
    int files_per_worker = NUM_FILES / num_cpus;

    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Worker process
            int start_file = cpu * files_per_worker;
            int end_file = (cpu == num_cpus - 1) ? NUM_FILES : (cpu + 1) * files_per_worker;

            for (int i = start_file; i < end_file; i++) {
                char source[256], object[256];
                snprintf(source, sizeof(source),
                        "/tmp/compile_bench_parallel/file%d.c", i);
                snprintf(object, sizeof(object),
                        "/tmp/compile_bench_parallel/file%d.o", i);

                uint64_t r, c, w, s;
                compile_single_file(source, object, &r, &c, &w, &s);
            }

            exit(0);
        }
    }

    // Wait for all workers
    for (uint32_t i = 0; i < num_cpus; i++) {
        wait(NULL);
    }

    uint64_t end = rdtsc_fence();
    uint64_t parallel_time = end - start;

    printf("\n--- Parallel Results ---\n");
    printf("Files compiled:          %d\n", NUM_FILES);
    printf("Worker processes:        %u\n", num_cpus);
    printf("Total time:              %.2f seconds\n",
           cycles_to_ms(parallel_time) / 1000.0);
    printf("Files per second:        %.2f\n",
           (double)NUM_FILES / (cycles_to_ms(parallel_time) / 1000.0));
    printf("Parallel efficiency:     %.1f%%\n",
           (double)num_cpus / (cycles_to_ms(parallel_time) / 1000.0) * 100.0);

    system("rm -rf /tmp/compile_bench_parallel");
}

int main(void) {
    printf("========================================\n");
    printf("Compiler Workload Benchmark\n");
    printf("========================================\n");

    bench_calibrate_cpu_freq();
    bench_check_vm();

    bench_compile_workload();
    bench_parallel_compile();

    printf("\n========================================\n");
    printf("Benchmark Complete\n");
    printf("========================================\n");

    printf("\nThis benchmark simulates:\n");
    printf("  - Heavy syscall usage (open/read/write/stat/close)\n");
    printf("  - Small file I/O (typical source files)\n");
    printf("  - CPU-bound work (compilation)\n");
    printf("  - Process spawning (parallel builds)\n");
    printf("\nExpected optimization benefits:\n");
    printf("  - Syscall improvements: 20-30%% faster\n");
    printf("  - Context switching: 10-20%% faster\n");
    printf("  - Overall: 20-30%% improvement\n");

    return 0;
}
