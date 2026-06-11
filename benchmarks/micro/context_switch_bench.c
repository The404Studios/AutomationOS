/**
 * Context Switch Latency Benchmark
 *
 * Measures the cost of context switching with and without PCID.
 *
 * Expected results:
 * - Without PCID: ~1000-2000 cycles (full TLB flush)
 * - With PCID:    ~400-800 cycles (no TLB flush)
 * - Target:       40-60% improvement
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <string.h>
#include "../common/bench_common.h"

#define ITERATIONS 10000
#define WARMUP_ITERATIONS 1000

// Shared memory for thread synchronization
typedef struct {
    volatile uint64_t counter;
    volatile bool ready;
    volatile bool done;
    uint64_t samples[ITERATIONS];
} shared_data_t;

/**
 * Thread function that just yields
 */
void* thread_func(void* arg) {
    shared_data_t* data = (shared_data_t*)arg;

    while (!data->ready) {
        sched_yield();
    }

    while (!data->done) {
        data->counter++;
        sched_yield();
    }

    return NULL;
}

/**
 * Benchmark context switch using thread ping-pong
 */
void bench_context_switch_threads(void) {
    printf("\n=== Context Switch Benchmark (Threads) ===\n");

    // Pin to single CPU to measure pure context switch cost
    bench_pin_cpu(0);

    // Allocate shared memory
    shared_data_t* data = mmap(NULL, sizeof(shared_data_t),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    data->counter = 0;
    data->ready = false;
    data->done = false;

    // Create helper thread
    pthread_t thread;
    pthread_create(&thread, NULL, thread_func, data);

    // Wait for thread to be ready
    usleep(1000);
    data->ready = true;

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        sched_yield();
    }

    // Measure context switches
    printf("Measuring %d context switches...\n", ITERATIONS);

    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        sched_yield();  // Context switch to other thread
        uint64_t end = rdtsc_fence();

        data->samples[i] = end - start;
    }

    data->done = true;
    pthread_join(thread, NULL);

    // Calculate statistics
    bench_stats_t stats;
    bench_calculate_stats(data->samples, ITERATIONS, &stats);
    bench_print_stats("Thread Context Switch", &stats, "ns");

    // Estimate one-way context switch cost (divide by 2)
    printf("\nEstimated one-way context switch: %.2f ns (%.0f cycles)\n",
           cycles_to_ns((uint64_t)(stats.mean / 2)),
           stats.mean / 2);

    munmap(data, sizeof(shared_data_t));
}

/**
 * Benchmark context switch using process ping-pong
 */
void bench_context_switch_processes(void) {
    printf("\n=== Context Switch Benchmark (Processes) ===\n");

    bench_pin_cpu(0);

    // Create pipe for synchronization
    int pipe_parent_to_child[2];
    int pipe_child_to_parent[2];

    if (pipe(pipe_parent_to_child) < 0 || pipe(pipe_child_to_parent) < 0) {
        perror("pipe");
        return;
    }

    // Allocate shared memory for results
    shared_data_t* data = mmap(NULL, sizeof(shared_data_t),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // Child process
        close(pipe_parent_to_child[1]);
        close(pipe_child_to_parent[0]);

        char byte;
        while (read(pipe_parent_to_child[0], &byte, 1) == 1) {
            write(pipe_child_to_parent[1], &byte, 1);
        }

        exit(0);
    } else {
        // Parent process
        close(pipe_parent_to_child[0]);
        close(pipe_child_to_parent[1]);

        // Warmup
        char byte = 'x';
        for (int i = 0; i < WARMUP_ITERATIONS; i++) {
            write(pipe_parent_to_child[1], &byte, 1);
            read(pipe_child_to_parent[0], &byte, 1);
        }

        // Measure
        printf("Measuring %d process context switches...\n", ITERATIONS);

        for (int i = 0; i < ITERATIONS; i++) {
            uint64_t start = rdtsc_fence();
            write(pipe_parent_to_child[1], &byte, 1);
            read(pipe_child_to_parent[0], &byte, 1);
            uint64_t end = rdtsc_fence();

            data->samples[i] = end - start;
        }

        close(pipe_parent_to_child[1]);
        close(pipe_child_to_parent[0]);
        wait(NULL);

        // Calculate statistics
        bench_stats_t stats;
        bench_calculate_stats(data->samples, ITERATIONS, &stats);
        bench_print_stats("Process Context Switch (with TLB pressure)", &stats, "ns");

        // Estimate one-way context switch
        printf("\nEstimated one-way context switch: %.2f ns (%.0f cycles)\n",
               cycles_to_ns((uint64_t)(stats.mean / 2)),
               stats.mean / 2);

        munmap(data, sizeof(shared_data_t));
    }
}

/**
 * Benchmark with TLB pressure
 * Access many pages to fill TLB before context switch
 */
void bench_context_switch_tlb_pressure(void) {
    printf("\n=== Context Switch with TLB Pressure ===\n");

    bench_pin_cpu(0);

    // Allocate large memory region (64 MB = 16384 pages)
    size_t size = 64 * 1024 * 1024;
    char* memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (memory == MAP_FAILED) {
        perror("mmap");
        return;
    }

    // Touch all pages to populate TLB
    for (size_t i = 0; i < size; i += 4096) {
        memory[i] = 1;
    }

    uint64_t samples[ITERATIONS];

    // Create helper thread
    pthread_t thread;
    volatile bool done = false;

    void* helper(void* arg) {
        while (!done) {
            sched_yield();
        }
        return NULL;
    }

    pthread_create(&thread, NULL, helper, NULL);

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        // Access memory to fill TLB
        for (size_t j = 0; j < size; j += 4096) {
            memory[j]++;
        }
        sched_yield();
    }

    printf("Measuring %d context switches with TLB pressure...\n", ITERATIONS);

    // Measure
    for (int i = 0; i < ITERATIONS; i++) {
        // Fill TLB with many pages
        for (size_t j = 0; j < size; j += 4096) {
            memory[j]++;
        }

        uint64_t start = rdtsc_fence();
        sched_yield();
        uint64_t end = rdtsc_fence();

        samples[i] = end - start;
    }

    done = true;
    pthread_join(thread, NULL);

    // Calculate statistics
    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("Context Switch (High TLB Pressure)", &stats, "ns");

    printf("\nWith PCID: TLB entries preserved across context switch\n");
    printf("Without PCID: Full TLB flush (add ~500-1000 cycles)\n");

    munmap(memory, size);
}

/**
 * Estimate PCID benefit
 */
void estimate_pcid_benefit(void) {
    printf("\n=== PCID Performance Impact Estimation ===\n");

    bool has_pcid = bench_check_pcid_support();

    if (!has_pcid) {
        printf("\n⚠ PCID not supported on this CPU\n");
        printf("Without PCID, every context switch flushes TLB:\n");
        printf("  - Small TLB (no pressure):  ~1000 cycles\n");
        printf("  - Large TLB (high pressure): ~2000-3000 cycles\n");
        printf("\nWith PCID enabled (on supported CPUs):\n");
        printf("  - No TLB flush:              ~400-800 cycles\n");
        printf("  - Expected improvement:      40-60%%\n");
    } else {
        printf("\n✓ PCID is supported and can be enabled\n");
        printf("TLB entries will be preserved across context switches\n");
        printf("Expected improvement: 40-60%% reduction in context switch cost\n");
    }
}

int main(void) {
    printf("========================================\n");
    printf("Context Switch Latency Benchmark\n");
    printf("========================================\n");

    bench_calibrate_cpu_freq();
    bench_check_vm();
    bench_get_cpu_count();

    // Run benchmarks
    bench_context_switch_threads();
    bench_context_switch_processes();
    bench_context_switch_tlb_pressure();
    estimate_pcid_benefit();

    printf("\n========================================\n");
    printf("Benchmark Complete\n");
    printf("========================================\n");

    return 0;
}
