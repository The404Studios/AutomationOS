/**
 * Syscall Latency Benchmark
 *
 * Measures the overhead of entering and exiting the kernel via syscalls.
 * Tests both null syscalls (minimal work) and real syscalls.
 */

#include "../../kernel/include/syscall.h"
#include "../../kernel/include/perf.h"
#include "../../kernel/include/kernel.h"

#define BENCH_ITERATIONS 100000

/**
 * Benchmark null syscall (getpid)
 *
 * getpid is the fastest syscall - it just returns current process ID
 * with no other work. This measures the base syscall overhead.
 */
void bench_syscall_null(void) {
    kprintf("\n[BENCH] Null Syscall Benchmark (getpid)\n");
    kprintf("========================================\n");

    perf_stat_t stats;
    perf_stat_init(&stats, "Syscall (getpid)");

    // Warmup
    for (int i = 0; i < 1000; i++) {
        syscall(SYS_GETPID);
    }

    // Benchmark
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        syscall(SYS_GETPID);
        uint64_t end = rdtscp();
        perf_stat_record(&stats, end - start);
    }

    perf_stat_report(&stats);
    kprintf("\n");
}

/**
 * Benchmark read syscall
 *
 * Measures syscall overhead for a more complex operation (read from /dev/null)
 */
void bench_syscall_read(void) {
    kprintf("\n[BENCH] Read Syscall Benchmark\n");
    kprintf("===============================\n");

    char buffer[64];
    int fd = 0;  // stdin (or /dev/null when implemented)

    perf_stat_t stats;
    perf_stat_init(&stats, "Syscall (read)");

    // Warmup
    for (int i = 0; i < 100; i++) {
        syscall(SYS_READ, fd, buffer, 1);
    }

    // Benchmark
    for (int i = 0; i < 10000; i++) {
        uint64_t start = rdtsc_fence();
        syscall(SYS_READ, fd, buffer, 1);
        uint64_t end = rdtscp();
        perf_stat_record(&stats, end - start);
    }

    perf_stat_report(&stats);
    kprintf("\n");
}

/**
 * Benchmark write syscall
 *
 * Measures syscall overhead for write (to /dev/null when implemented)
 */
void bench_syscall_write(void) {
    kprintf("\n[BENCH] Write Syscall Benchmark\n");
    kprintf("================================\n");

    const char* msg = "x";
    int fd = 1;  // stdout (or /dev/null when implemented)

    perf_stat_t stats;
    perf_stat_init(&stats, "Syscall (write)");

    // Warmup
    for (int i = 0; i < 100; i++) {
        syscall(SYS_WRITE, fd, msg, 1);
    }

    // Benchmark (fewer iterations to avoid spamming output)
    for (int i = 0; i < 1000; i++) {
        uint64_t start = rdtsc_fence();
        syscall(SYS_WRITE, fd, msg, 1);
        uint64_t end = rdtscp();
        perf_stat_record(&stats, end - start);
    }

    perf_stat_report(&stats);
    kprintf("\n");
}

/**
 * Benchmark syscall entry/exit only (no handler execution)
 *
 * Measures the pure overhead of syscall_entry/syscall_exit assembly code
 */
void bench_syscall_entry_exit(void) {
    kprintf("\n[BENCH] Syscall Entry/Exit Benchmark\n");
    kprintf("=====================================\n");

    // This requires a special syscall number that does nothing
    // For now, use getpid as it's very fast

    perf_stat_t stats;
    perf_stat_init(&stats, "Syscall Entry/Exit");

    // Measure SYSCALL instruction overhead
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        uint64_t start = rdtsc_fence();
        __asm__ volatile(
            "mov $%c0, %%rax\n"
            "syscall\n"
            : : "i"(SYS_GETPID) : "rax", "rcx", "r11", "memory"
        );
        uint64_t end = rdtscp();
        perf_stat_record(&stats, end - start);
    }

    perf_stat_report(&stats);

    // Calculate breakdown
    uint64_t avg = stats.total_cycles / stats.iterations;
    kprintf("\n[BENCH] Analysis:\n");
    kprintf("  Base overhead: ~%llu cycles\n", avg);
    kprintf("  Entry/exit: ~%llu cycles (estimated)\n", avg - 50);  // Subtract handler cost
    kprintf("  SYSCALL instr: ~60-100 cycles (hardware)\n");
    kprintf("  Register save/restore: ~40-60 cycles\n");
    kprintf("  Dispatch: ~10-20 cycles\n");

    kprintf("\n");
}

/**
 * Benchmark syscall throughput
 *
 * How many syscalls per second can the system handle?
 */
void bench_syscall_throughput(void) {
    kprintf("\n[BENCH] Syscall Throughput\n");
    kprintf("==========================\n");

    const int iterations = BENCH_ITERATIONS;

    uint64_t start = rdtsc_fence();
    for (int i = 0; i < iterations; i++) {
        syscall(SYS_GETPID);
    }
    uint64_t end = rdtscp();

    uint64_t total_cycles = end - start;
    uint64_t cycles_per_call = total_cycles / iterations;
    uint64_t cpu_freq_hz = perf_get_cpu_freq_mhz() * 1000000;
    uint64_t calls_per_sec = cpu_freq_hz / cycles_per_call;

    kprintf("[BENCH] Total cycles: %llu\n", total_cycles);
    kprintf("[BENCH] Cycles/syscall: %llu\n", cycles_per_call);
    kprintf("[BENCH] Syscalls/second: %llu (%.2f M/s)\n",
            calls_per_sec, (double)calls_per_sec / 1000000.0);
    kprintf("[BENCH] Time/syscall: %.2f ns\n",
            cycles_to_us(cycles_per_call) * 1000.0);

    kprintf("\n");
}

/**
 * Compare different syscall methods
 *
 * SYSCALL vs INT 0x80 (if implemented)
 */
void bench_syscall_methods(void) {
    kprintf("\n[BENCH] Syscall Method Comparison\n");
    kprintf("==================================\n");

    // Test SYSCALL instruction
    perf_stat_t syscall_stats;
    perf_stat_init(&syscall_stats, "SYSCALL instruction");

    for (int i = 0; i < 10000; i++) {
        uint64_t start = rdtsc_fence();
        syscall(SYS_GETPID);
        uint64_t end = rdtscp();
        perf_stat_record(&syscall_stats, end - start);
    }

    perf_stat_report(&syscall_stats);

    // TODO: Test INT 0x80 if implemented
    kprintf("[BENCH] INT 0x80 not implemented\n");

    // TODO: Test SYSENTER if implemented
    kprintf("[BENCH] SYSENTER not implemented\n");

    kprintf("\n");
}

/**
 * Run all syscall benchmarks
 */
void bench_syscall_all(void) {
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  SYSCALL BENCHMARK SUITE\n");
    kprintf("=============================================\n");

    bench_syscall_null();
    bench_syscall_entry_exit();
    bench_syscall_throughput();
    bench_syscall_methods();

    // These are commented out to avoid spam during development
    // Uncomment when read/write are fully implemented
    // bench_syscall_read();
    // bench_syscall_write();

    kprintf("=============================================\n");
    kprintf("  BENCHMARK COMPLETE\n");
    kprintf("=============================================\n");
    kprintf("\n");
}
