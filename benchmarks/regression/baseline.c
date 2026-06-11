/**
 * Performance Regression Baseline
 *
 * Establishes baseline performance metrics for regression detection.
 * Runs subset of quick benchmarks to track performance over time.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/wait.h>
#include "../common/bench_common.h"

#define QUICK_ITERATIONS 1000

typedef struct {
    const char* name;
    uint64_t baseline_cycles;
    uint64_t current_cycles;
    double change_percent;
} regression_metric_t;

#define MAX_METRICS 20
static regression_metric_t metrics[MAX_METRICS];
static int metric_count = 0;

/**
 * Add metric for regression tracking
 */
void add_metric(const char* name, uint64_t baseline, uint64_t current) {
    if (metric_count >= MAX_METRICS) return;

    regression_metric_t* m = &metrics[metric_count++];
    m->name = name;
    m->baseline_cycles = baseline;
    m->current_cycles = current;

    if (baseline > 0) {
        m->change_percent = ((double)current - (double)baseline) / (double)baseline * 100.0;
    } else {
        m->change_percent = 0.0;
    }
}

/**
 * Quick context switch benchmark
 */
void quick_bench_context_switch(void) {
    printf("\n[BASELINE] Context Switch...\n");

    uint64_t samples[QUICK_ITERATIONS];

    for (int i = 0; i < QUICK_ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        sched_yield();
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, QUICK_ITERATIONS, &stats);

    printf("  Mean: %.0f cycles (%.2f ns)\n",
           stats.mean, cycles_to_ns((uint64_t)stats.mean));

    add_metric("context_switch_cycles", 0, (uint64_t)stats.mean);
}

/**
 * Quick allocation benchmark
 */
void quick_bench_allocation(void) {
    printf("\n[BASELINE] Page Allocation...\n");

    uint64_t samples[QUICK_ITERATIONS];

    for (int i = 0; i < QUICK_ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        void* ptr = malloc(4096);
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
        free(ptr);
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, QUICK_ITERATIONS, &stats);

    printf("  Mean: %.0f cycles (%.2f ns)\n",
           stats.mean, cycles_to_ns((uint64_t)stats.mean));

    add_metric("allocation_cycles", 0, (uint64_t)stats.mean);
}

/**
 * Quick syscall benchmark
 */
void quick_bench_syscall(void) {
    printf("\n[BASELINE] Syscall (getpid)...\n");

    uint64_t samples[QUICK_ITERATIONS];

    for (int i = 0; i < QUICK_ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        bench_syscall0(SYS_getpid);
        uint64_t end = rdtsc_fence();
        samples[i] = end - start;
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, QUICK_ITERATIONS, &stats);

    printf("  Mean: %.0f cycles (%.2f ns)\n",
           stats.mean, cycles_to_ns((uint64_t)stats.mean));

    add_metric("syscall_getpid_cycles", 0, (uint64_t)stats.mean);
}

/**
 * Quick process creation benchmark
 */
void quick_bench_fork(void) {
    printf("\n[BASELINE] Process Creation (fork)...\n");

    uint64_t samples[100];  // Fewer iterations (fork is expensive)

    for (int i = 0; i < 100; i++) {
        uint64_t start = rdtsc_fence();
        pid_t pid = fork();
        uint64_t end = rdtsc_fence();

        if (pid == 0) {
            exit(0);
        } else {
            samples[i] = end - start;
            wait(NULL);
        }
    }

    bench_stats_t stats;
    bench_calculate_stats(samples, 100, &stats);

    printf("  Mean: %.0f cycles (%.2f us)\n",
           stats.mean, cycles_to_us((uint64_t)stats.mean));

    add_metric("fork_cycles", 0, (uint64_t)stats.mean);
}

/**
 * Load baseline from file
 */
int load_baseline(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return -1;

    printf("\n[LOAD] Loading baseline from %s...\n", filename);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char name[128];
        uint64_t cycles;

        if (sscanf(line, "%127[^:]: %lu", name, &cycles) == 2) {
            // Find matching metric
            for (int i = 0; i < metric_count; i++) {
                if (strcmp(metrics[i].name, name) == 0) {
                    metrics[i].baseline_cycles = cycles;
                    metrics[i].change_percent =
                        ((double)metrics[i].current_cycles - (double)cycles) / (double)cycles * 100.0;
                    printf("  %s: %lu cycles (baseline)\n", name, cycles);
                    break;
                }
            }
        }
    }

    fclose(f);
    return 0;
}

/**
 * Save baseline to file
 */
void save_baseline(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) {
        printf("Failed to save baseline to %s\n", filename);
        return;
    }

    printf("\n[SAVE] Saving baseline to %s...\n", filename);

    for (int i = 0; i < metric_count; i++) {
        fprintf(f, "%s: %lu\n", metrics[i].name, metrics[i].current_cycles);
        printf("  %s: %lu cycles\n", metrics[i].name, metrics[i].current_cycles);
    }

    fclose(f);
}

/**
 * Print regression report
 */
void print_regression_report(void) {
    printf("\n========================================\n");
    printf("Performance Regression Report\n");
    printf("========================================\n");

    int regressions = 0;
    int improvements = 0;

    for (int i = 0; i < metric_count; i++) {
        regression_metric_t* m = &metrics[i];

        if (m->baseline_cycles == 0) {
            printf("\n%-30s: %lu cycles (no baseline)\n",
                   m->name, m->current_cycles);
            continue;
        }

        const char* status;
        if (m->change_percent > 5.0) {
            status = "REGRESSION";
            regressions++;
        } else if (m->change_percent < -5.0) {
            status = "IMPROVEMENT";
            improvements++;
        } else {
            status = "OK";
        }

        printf("\n%-30s\n", m->name);
        printf("  Baseline:   %8lu cycles (%.2f ns)\n",
               m->baseline_cycles, cycles_to_ns(m->baseline_cycles));
        printf("  Current:    %8lu cycles (%.2f ns)\n",
               m->current_cycles, cycles_to_ns(m->current_cycles));
        printf("  Change:     %+.2f%%\n", m->change_percent);
        printf("  Status:     %s\n", status);
    }

    printf("\n========================================\n");
    printf("Summary\n");
    printf("========================================\n");
    printf("Total metrics:       %d\n", metric_count);
    printf("Regressions (>5%%):   %d\n", regressions);
    printf("Improvements (<-5%%): %d\n", improvements);
    printf("Stable:              %d\n", metric_count - regressions - improvements);

    if (regressions > 0) {
        printf("\n❌ REGRESSION DETECTED!\n");
        printf("Review changes that may have caused performance degradation.\n");
    } else if (improvements > 0) {
        printf("\n✅ PERFORMANCE IMPROVED!\n");
    } else {
        printf("\n✅ No significant performance changes\n");
    }
}

/**
 * Generate JSON report for CI
 */
void generate_json_report(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"timestamp\": %ld,\n", time(NULL));
    fprintf(f, "  \"cpu_freq_mhz\": %lu,\n", cpu_freq_mhz);
    fprintf(f, "  \"metrics\": [\n");

    for (int i = 0; i < metric_count; i++) {
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", metrics[i].name);
        fprintf(f, "      \"baseline_cycles\": %lu,\n", metrics[i].baseline_cycles);
        fprintf(f, "      \"current_cycles\": %lu,\n", metrics[i].current_cycles);
        fprintf(f, "      \"change_percent\": %.2f,\n", metrics[i].change_percent);
        fprintf(f, "      \"baseline_ns\": %.2f,\n", cycles_to_ns(metrics[i].baseline_cycles));
        fprintf(f, "      \"current_ns\": %.2f\n", cycles_to_ns(metrics[i].current_cycles));
        fprintf(f, "    }%s\n", (i < metric_count - 1) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    printf("\n[JSON] Report saved to %s\n", filename);
}

int main(int argc, char* argv[]) {
    printf("========================================\n");
    printf("Performance Regression Baseline\n");
    printf("========================================\n");

    bool update_baseline = false;
    const char* baseline_file = "baseline.txt";

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0) {
            update_baseline = true;
        } else if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc) {
            baseline_file = argv[++i];
        }
    }

    bench_calibrate_cpu_freq();
    bench_check_pcid_support();
    bench_check_vm();

    // Run quick benchmarks
    printf("\nRunning baseline benchmarks...\n");
    quick_bench_syscall();
    quick_bench_allocation();
    quick_bench_context_switch();
    quick_bench_fork();

    // Load baseline if exists
    if (access(baseline_file, F_OK) == 0 && !update_baseline) {
        load_baseline(baseline_file);
        print_regression_report();
        generate_json_report("regression_report.json");
    } else {
        printf("\nNo baseline found or update requested.\n");
        if (update_baseline) {
            save_baseline(baseline_file);
        }
    }

    printf("\n========================================\n");
    printf("Usage:\n");
    printf("  %s                    - Compare against baseline\n", argv[0]);
    printf("  %s --update           - Update baseline\n", argv[0]);
    printf("  %s --baseline FILE    - Use specific baseline file\n", argv[0]);
    printf("========================================\n");

    return 0;
}
