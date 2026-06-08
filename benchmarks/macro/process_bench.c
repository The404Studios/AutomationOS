/**
 * Process Management Benchmark
 *
 * Measures:
 * - Process creation latency (fork)
 * - Process switching throughput
 * - Zombie reaping performance
 * - Fork bomb handling (stress test)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include "../common/bench_common.h"

#define ITERATIONS 1000
#define FORK_BOMB_SIZE 10000

/**
 * Benchmark process creation (fork)
 */
void bench_process_creation(void) {
    printf("\n=== Process Creation Benchmark ===\n");

    bench_pin_cpu(0);

    uint64_t* samples = malloc(ITERATIONS * sizeof(uint64_t));

    // Warmup
    for (int i = 0; i < 10; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            exit(0);
        }
        wait(NULL);
    }

    printf("Measuring %d fork() operations...\n", ITERATIONS);

    // Measure fork latency
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        pid_t pid = fork();
        uint64_t end = rdtsc_fence();

        if (pid == 0) {
            // Child process - exit immediately
            exit(0);
        } else {
            // Parent - record time and wait for child
            samples[i] = end - start;
            wait(NULL);
        }
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, ITERATIONS, &stats);
    bench_print_stats("Process Creation (fork)", &stats, "ns");

    printf("\nNote: fork() includes:\n");
    printf("  - Copy page tables (~500-1000 cycles with PCID)\n");
    printf("  - Allocate process struct (~50-200 cycles with per-CPU cache)\n");
    printf("  - Setup FD table (~100-300 cycles)\n");
    printf("  - Add to scheduler (~50-100 cycles)\n");

    free(samples);
}

/**
 * Benchmark process switching throughput
 */
void bench_process_switching(void) {
    printf("\n=== Process Switching Throughput ===\n");

    bench_pin_cpu(0);

    // Create multiple child processes
    const int num_procs = 4;
    pid_t pids[num_procs];

    // Create pipe for synchronization
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }

    for (int i = 0; i < num_procs; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            // Child process - just yield in a loop
            close(pipefd[1]);
            char byte;
            read(pipefd[0], &byte, 1);  // Wait for signal
            for (int j = 0; j < 10000; j++) {
                sched_yield();
            }
            exit(0);
        }
    }

    close(pipefd[0]);

    printf("Measuring context switches across %d processes...\n", num_procs);

    // Start all child processes
    uint64_t start = rdtsc_fence();
    close(pipefd[1]);  // Signal children to start

    // Wait for all children
    for (int i = 0; i < num_procs; i++) {
        wait(NULL);
    }

    uint64_t end = rdtsc_fence();
    uint64_t total_cycles = end - start;

    // Each process did 10000 yields, total switches = num_procs * 10000
    uint64_t total_switches = num_procs * 10000ULL;
    double cycles_per_switch = (double)total_cycles / (double)total_switches;

    printf("\nResults:\n");
    printf("Total switches:       %lu\n", total_switches);
    printf("Total time:           %.2f ms\n", cycles_to_ms(total_cycles));
    printf("Cycles per switch:    %.2f cycles (%.2f ns)\n",
           cycles_per_switch, cycles_to_ns((uint64_t)cycles_per_switch));
    printf("Switches per second:  %.2f M/s\n",
           (double)total_switches / cycles_to_ms(total_cycles) / 1000.0);
}

/**
 * Benchmark zombie reaping
 */
void bench_zombie_reaping(void) {
    printf("\n=== Zombie Process Reaping ===\n");

    bench_pin_cpu(0);

    const int num_zombies = 1000;

    printf("Creating %d zombie processes...\n", num_zombies);

    // Create many child processes that exit immediately
    uint64_t start = rdtsc_fence();

    for (int i = 0; i < num_zombies; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            exit(0);  // Become zombie
        }
    }

    // Now reap all zombies
    uint64_t reap_start = rdtsc_fence();

    for (int i = 0; i < num_zombies; i++) {
        wait(NULL);
    }

    uint64_t end = rdtsc_fence();

    uint64_t total_time = end - start;
    uint64_t reap_time = end - reap_start;

    printf("\nResults:\n");
    printf("Total time (creation + reaping): %.2f ms\n", cycles_to_ms(total_time));
    printf("Reaping time:                    %.2f ms\n", cycles_to_ms(reap_time));
    printf("Average reap time per zombie:    %.2f us (%.0f cycles)\n",
           cycles_to_us(reap_time) / num_zombies,
           (double)reap_time / (double)num_zombies);
}

/**
 * Fork bomb stress test
 */
void bench_fork_bomb(void) {
    printf("\n=== Fork Bomb Stress Test ===\n");

    // Set resource limit to prevent actual fork bomb
    struct rlimit rlim;
    rlim.rlim_cur = FORK_BOMB_SIZE + 100;  // Allow test + safety margin
    rlim.rlim_max = FORK_BOMB_SIZE + 100;
    setrlimit(RLIMIT_NPROC, &rlim);

    printf("Creating %d processes (testing process limit)...\n", FORK_BOMB_SIZE);

    pid_t* pids = malloc(FORK_BOMB_SIZE * sizeof(pid_t));
    int created = 0;

    uint64_t start = rdtsc_fence();

    // Create as many processes as possible
    for (int i = 0; i < FORK_BOMB_SIZE; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            // Fork failed (hit limit)
            break;
        } else if (pid == 0) {
            // Child - sleep and exit
            usleep(100000);  // Sleep 100ms
            exit(0);
        } else {
            pids[created++] = pid;
        }
    }

    printf("Created %d processes\n", created);

    // Wait for all children
    for (int i = 0; i < created; i++) {
        waitpid(pids[i], NULL, 0);
    }

    uint64_t end = rdtsc_fence();

    printf("\nResults:\n");
    printf("Processes created:    %d\n", created);
    printf("Total time:           %.2f seconds\n", cycles_to_ms(end - start) / 1000.0);
    printf("Time per process:     %.2f ms\n", cycles_to_ms(end - start) / created);

    if (created >= FORK_BOMB_SIZE * 0.9) {
        printf("✓ Successfully handled %d processes\n", created);
    } else {
        printf("⚠ Only created %d/%d processes\n", created, FORK_BOMB_SIZE);
    }

    free(pids);
}

/**
 * Measure exec overhead
 */
void bench_exec_overhead(void) {
    printf("\n=== Exec Overhead Benchmark ===\n");

    bench_pin_cpu(0);

    uint64_t samples[100];

    printf("Measuring 100 exec() operations...\n");

    for (int i = 0; i < 100; i++) {
        uint64_t start = rdtsc_fence();

        pid_t pid = fork();
        if (pid == 0) {
            // Child - exec /bin/true (does nothing, exits immediately)
            execl("/bin/true", "true", NULL);
            exit(1);  // Should never reach here
        }

        wait(NULL);
        uint64_t end = rdtsc_fence();

        samples[i] = end - start;
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, 100, &stats);
    bench_print_stats("fork + exec + wait", &stats, "ns");
}

/**
 * Benchmark process priority/scheduling
 */
void bench_process_priority(void) {
    printf("\n=== Process Priority Benchmark ===\n");

    // Test that high priority processes get more CPU
    const int iterations = 10000;

    volatile uint64_t counter_high = 0;
    volatile uint64_t counter_low = 0;

    pid_t high_pid = fork();
    if (high_pid == 0) {
        // High priority process
        nice(-10);  // Higher priority
        for (int i = 0; i < iterations; i++) {
            counter_high++;
            sched_yield();
        }
        exit(0);
    }

    pid_t low_pid = fork();
    if (low_pid == 0) {
        // Low priority process
        nice(10);  // Lower priority
        for (int i = 0; i < iterations; i++) {
            counter_low++;
            sched_yield();
        }
        exit(0);
    }

    // Wait for both
    wait(NULL);
    wait(NULL);

    printf("\nScheduler fairness test:\n");
    printf("High priority process: %lu iterations\n", counter_high);
    printf("Low priority process:  %lu iterations\n", counter_low);

    if (counter_high > counter_low) {
        printf("✓ High priority process got more CPU time\n");
    } else {
        printf("⚠ Priority scheduling may not be working correctly\n");
    }
}

int main(void) {
    printf("========================================\n");
    printf("Process Management Benchmark\n");
    printf("========================================\n");

    bench_calibrate_cpu_freq();
    bench_check_vm();

    // Run benchmarks
    bench_process_creation();
    bench_process_switching();
    bench_zombie_reaping();
    bench_exec_overhead();
    bench_process_priority();

    // Fork bomb test (optional, can be destructive)
    printf("\n⚠ Fork bomb test disabled by default (can consume many resources)\n");
    printf("Uncomment to enable fork bomb stress test\n");
    // bench_fork_bomb();

    printf("\n========================================\n");
    printf("Benchmark Complete\n");
    printf("========================================\n");

    return 0;
}
