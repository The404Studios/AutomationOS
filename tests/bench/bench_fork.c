/**
 * Process Creation Benchmark
 *
 * Measures fork, exec, and process lifecycle performance.
 * Tests the efficiency of copy-on-write page tables and process management.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Syscall numbers
#define SYS_FORK     1
#define SYS_EXIT     0
#define SYS_WAITPID  6
#define SYS_GETPID   8
#define SYS_SPAWN    16
#define SYS_GET_TICKS_MS 40

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

static inline uint64_t get_ticks_ms(void) {
    return syscall0(SYS_GET_TICKS_MS);
}

static inline int64_t fork(void) {
    return syscall0(SYS_FORK);
}

static inline void exit(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static inline int64_t waitpid(int pid, int* status, int options) {
    return syscall3(SYS_WAITPID, pid, (uint64_t)status, options);
}

static inline int64_t getpid(void) {
    return syscall0(SYS_GETPID);
}

/**
 * Benchmark fork() overhead
 *
 * Measures the cost of creating a new process via fork.
 * Expected: <5ms per fork (with CoW page tables)
 */
void bench_fork_only(void) {
    printf("\n[BENCH] Fork Overhead\n");
    printf("=====================\n");

    const int iterations = 100;
    uint64_t total_cycles = 0;
    uint64_t total_ms = 0;

    printf("[BENCH] Creating %d child processes...\n", iterations);

    for (int i = 0; i < iterations; i++) {
        uint64_t start_cycles = rdtsc_fence();
        uint64_t start_ms = get_ticks_ms();

        int64_t pid = fork();

        if (pid == 0) {
            // Child process - exit immediately
            exit(0);
        } else if (pid > 0) {
            uint64_t end_cycles = rdtscp();
            uint64_t end_ms = get_ticks_ms();

            // Parent - wait for child
            int status;
            waitpid(pid, &status, 0);

            total_cycles += (end_cycles - start_cycles);
            total_ms += (end_ms - start_ms);
        } else {
            printf("[ERROR] Fork failed at iteration %d\n", i);
            break;
        }
    }

    uint64_t avg_cycles = total_cycles / iterations;
    uint64_t avg_ms = total_ms / iterations;

    // Assume 3 GHz CPU
    uint64_t cpu_freq_mhz = 3000;
    uint64_t avg_us = avg_cycles / cpu_freq_mhz;

    printf("[BENCH] Average fork time:\n");
    printf("  Cycles: %llu\n", (unsigned long long)avg_cycles);
    printf("  Time:   %llu us (~%llu ms)\n",
           (unsigned long long)avg_us, (unsigned long long)avg_ms);

    if (avg_ms < 10) {
        printf("[PASS] Fork performance good (<10ms)\n");
    } else if (avg_ms < 50) {
        printf("[INFO] Fork performance acceptable (10-50ms)\n");
    } else {
        printf("[WARN] Fork performance slow (>50ms)\n");
    }

    printf("\n");
}

/**
 * Benchmark fork + exit + wait cycle
 *
 * Measures complete process lifecycle overhead
 */
void bench_fork_exit_wait(void) {
    printf("\n[BENCH] Fork + Exit + Wait Cycle\n");
    printf("=================================\n");

    const int iterations = 100;
    uint64_t start_ms = get_ticks_ms();

    for (int i = 0; i < iterations; i++) {
        int64_t pid = fork();

        if (pid == 0) {
            // Child - do minimal work then exit
            volatile int x = 0;
            for (int j = 0; j < 100; j++) {
                x += j;
            }
            exit(0);
        } else if (pid > 0) {
            // Parent - wait for child
            int status;
            waitpid(pid, &status, 0);
        } else {
            printf("[ERROR] Fork failed\n");
            break;
        }
    }

    uint64_t end_ms = get_ticks_ms();
    uint64_t elapsed_ms = end_ms - start_ms;
    uint64_t avg_ms = elapsed_ms / iterations;

    printf("[BENCH] Completed %d fork+exit+wait cycles\n", iterations);
    printf("[BENCH] Total time: %llu ms\n", (unsigned long long)elapsed_ms);
    printf("[BENCH] Average:    %llu ms per cycle\n", (unsigned long long)avg_ms);

    if (avg_ms < 15) {
        printf("[PASS] Process lifecycle efficient (<15ms)\n");
    } else {
        printf("[INFO] Process lifecycle: %llu ms\n", (unsigned long long)avg_ms);
    }

    printf("\n");
}

/**
 * Benchmark spawn (if implemented)
 *
 * Spawn is like fork+exec combined
 */
void bench_spawn(void) {
    printf("\n[BENCH] Spawn Performance\n");
    printf("=========================\n");

    const char* test_prog = "/bin/true";  // Minimal test program

    uint64_t start = rdtsc_fence();
    int64_t pid = syscall1(SYS_SPAWN, (uint64_t)test_prog);
    uint64_t end = rdtscp();

    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);

        uint64_t cycles = end - start;
        uint64_t cpu_freq_mhz = 3000;
        uint64_t us = cycles / cpu_freq_mhz;

        printf("[BENCH] Spawn time: %llu cycles (~%llu us)\n",
               (unsigned long long)cycles, (unsigned long long)us);
    } else if (pid == -2) {
        printf("[INFO] Spawn syscall not implemented (returned -ENOENT)\n");
    } else {
        printf("[INFO] Spawn failed or not available: %lld\n", (long long)pid);
    }

    printf("\n");
}

/**
 * Benchmark process creation rate
 */
void bench_process_creation_rate(void) {
    printf("\n[BENCH] Process Creation Rate\n");
    printf("==============================\n");

    const int duration_sec = 2;
    int count = 0;

    uint64_t start_ms = get_ticks_ms();
    uint64_t target_ms = start_ms + (duration_sec * 1000);

    while (get_ticks_ms() < target_ms) {
        int64_t pid = fork();

        if (pid == 0) {
            exit(0);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            count++;
        } else {
            break;
        }
    }

    uint64_t end_ms = get_ticks_ms();
    uint64_t elapsed_ms = end_ms - start_ms;
    double elapsed_sec = (double)elapsed_ms / 1000.0;
    double rate = (double)count / elapsed_sec;

    printf("[BENCH] Created %d processes in %.2f seconds\n", count, elapsed_sec);
    printf("[BENCH] Rate: %.2f processes/second\n", rate);

    if (rate > 100) {
        printf("[PASS] High process creation rate\n");
    } else if (rate > 50) {
        printf("[INFO] Moderate process creation rate\n");
    } else {
        printf("[WARN] Low process creation rate\n");
    }

    printf("\n");
}

/**
 * Benchmark fork with different memory footprints
 */
void bench_fork_memory_sizes(void) {
    printf("\n[BENCH] Fork with Different Memory Footprints\n");
    printf("==============================================\n");

    const size_t sizes[] = {
        4096,           // 4 KB
        65536,          // 64 KB
        1024 * 1024,    // 1 MB
        16 * 1024 * 1024  // 16 MB
    };
    const int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        size_t size = sizes[s];

        // Allocate memory
        char* mem = malloc(size);
        if (!mem) {
            printf("[ERROR] Failed to allocate %zu bytes\n", size);
            continue;
        }

        // Touch all pages to ensure they're mapped
        for (size_t i = 0; i < size; i += 4096) {
            mem[i] = 0xAA;
        }

        // Benchmark fork
        uint64_t start = rdtsc_fence();
        int64_t pid = fork();
        uint64_t end = rdtscp();

        if (pid == 0) {
            // Child
            free(mem);
            exit(0);
        } else if (pid > 0) {
            // Parent
            int status;
            waitpid(pid, &status, 0);

            uint64_t cycles = end - start;
            uint64_t cpu_freq_mhz = 3000;
            uint64_t us = cycles / cpu_freq_mhz;

            printf("  Memory size %8zu bytes: %6llu us (%llu cycles)\n",
                   size, (unsigned long long)us, (unsigned long long)cycles);

            free(mem);
        } else {
            printf("[ERROR] Fork failed\n");
            free(mem);
        }
    }

    printf("[INFO] CoW (Copy-on-Write) should make large forks fast\n");
    printf("\n");
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  PROCESS CREATION BENCHMARK SUITE\n");
    printf("=============================================\n");

    bench_fork_only();
    bench_fork_exit_wait();
    bench_fork_memory_sizes();
    bench_process_creation_rate();
    bench_spawn();

    printf("=============================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("=============================================\n");
    printf("\nExpected Results:\n");
    printf("  Fork time:         <10 ms\n");
    printf("  Lifecycle:         <15 ms\n");
    printf("  Creation rate:     >50 processes/sec\n");
    printf("  Large fork (CoW):  Similar to small fork\n");
    printf("\n");

    return 0;
}
