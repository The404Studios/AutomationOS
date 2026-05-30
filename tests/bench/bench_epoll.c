/**
 * Epoll Scalability Benchmark
 *
 * Measures epoll performance and scalability:
 * - epoll_wait should be O(1) regardless of monitored fd count
 * - Only returns fds with actual events
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Syscall numbers
#define SYS_EPOLL_CREATE 73
#define SYS_EPOLL_CTL    74
#define SYS_EPOLL_WAIT   75
#define SYS_SOCKET       51
#define SYS_CLOSE_SK     55
#define SYS_WRITE        3

// Epoll operations
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

// Epoll events
#define EPOLLIN  0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010

// Socket constants
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2

typedef union {
    void* ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

typedef struct {
    uint32_t events;
    epoll_data_t data;
} epoll_event_t;

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

// Statistics
typedef struct {
    const char* name;
    uint64_t min_cycles;
    uint64_t max_cycles;
    uint64_t total_cycles;
    uint32_t iterations;
} perf_stat_t;

static void perf_stat_init(perf_stat_t* stat, const char* name) {
    stat->name = name;
    stat->min_cycles = UINT64_MAX;
    stat->max_cycles = 0;
    stat->total_cycles = 0;
    stat->iterations = 0;
}

static void perf_stat_record(perf_stat_t* stat, uint64_t cycles) {
    if (cycles < stat->min_cycles) stat->min_cycles = cycles;
    if (cycles > stat->max_cycles) stat->max_cycles = cycles;
    stat->total_cycles += cycles;
    stat->iterations++;
}

static void perf_stat_report(perf_stat_t* stat) {
    if (stat->iterations == 0) {
        printf("[PERF] %s: No data\n", stat->name);
        return;
    }

    uint64_t avg = stat->total_cycles / stat->iterations;
    printf("[PERF] %s (n=%u):\n", stat->name, stat->iterations);
    printf("  Min: %llu cycles\n", (unsigned long long)stat->min_cycles);
    printf("  Avg: %llu cycles\n", (unsigned long long)avg);
    printf("  Max: %llu cycles\n", (unsigned long long)stat->max_cycles);
}

/**
 * Benchmark epoll_create
 */
void bench_epoll_create(void) {
    printf("\n[BENCH] Epoll Create\n");
    printf("====================\n");

    const int iterations = 1000;
    perf_stat_t stats;
    perf_stat_init(&stats, "epoll_create");

    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        int epfd = syscall1(SYS_EPOLL_CREATE, 100);
        uint64_t end = rdtscp();

        if (epfd >= 0) {
            perf_stat_record(&stats, end - start);
            // Note: We should close epfd, but for benchmark we'll let it leak
            // In production code: close(epfd);
        }
    }

    perf_stat_report(&stats);
    printf("\n");
}

/**
 * Benchmark epoll_ctl (add/modify/delete)
 */
void bench_epoll_ctl(void) {
    printf("\n[BENCH] Epoll Control Operations\n");
    printf("=================================\n");

    int epfd = syscall1(SYS_EPOLL_CREATE, 100);
    if (epfd < 0) {
        printf("[ERROR] Failed to create epoll instance\n");
        return;
    }

    // Create some file descriptors to monitor
    int fds[100];
    for (int i = 0; i < 100; i++) {
        fds[i] = syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    }

    // Benchmark EPOLL_CTL_ADD
    perf_stat_t add_stats;
    perf_stat_init(&add_stats, "epoll_ctl ADD");

    for (int i = 0; i < 100; i++) {
        epoll_event_t ev;
        ev.events = EPOLLIN;
        ev.data.fd = fds[i];

        uint64_t start = rdtsc_fence();
        int ret = syscall4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, fds[i], (uint64_t)&ev);
        uint64_t end = rdtscp();

        if (ret >= 0) {
            perf_stat_record(&add_stats, end - start);
        }
    }

    perf_stat_report(&add_stats);

    // Benchmark EPOLL_CTL_MOD
    perf_stat_t mod_stats;
    perf_stat_init(&mod_stats, "epoll_ctl MOD");

    for (int i = 0; i < 100; i++) {
        epoll_event_t ev;
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fds[i];

        uint64_t start = rdtsc_fence();
        int ret = syscall4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_MOD, fds[i], (uint64_t)&ev);
        uint64_t end = rdtscp();

        if (ret >= 0) {
            perf_stat_record(&mod_stats, end - start);
        }
    }

    perf_stat_report(&mod_stats);

    // Cleanup
    for (int i = 0; i < 100; i++) {
        if (fds[i] >= 0) {
            syscall1(SYS_CLOSE_SK, fds[i]);
        }
    }

    printf("\n");
}

/**
 * Benchmark epoll_wait scalability
 *
 * Key test: O(1) performance regardless of monitored fd count
 */
void bench_epoll_wait_scalability(void) {
    printf("\n[BENCH] Epoll Wait Scalability (O(1) test)\n");
    printf("===========================================\n");

    const int fd_counts[] = {10, 50, 100, 500, 1000};
    const int num_counts = sizeof(fd_counts) / sizeof(fd_counts[0]);

    for (int c = 0; c < num_counts; c++) {
        int fd_count = fd_counts[c];

        int epfd = syscall1(SYS_EPOLL_CREATE, fd_count);
        if (epfd < 0) {
            printf("[ERROR] Failed to create epoll instance\n");
            continue;
        }

        // Create and register file descriptors
        int* fds = malloc(fd_count * sizeof(int));
        for (int i = 0; i < fd_count; i++) {
            fds[i] = syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);

            epoll_event_t ev;
            ev.events = EPOLLIN;
            ev.data.fd = fds[i];
            syscall4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, fds[i], (uint64_t)&ev);
        }

        // Make only ONE fd ready (write to fds[0])
        // In a real scenario, this would make it readable
        // For benchmark, we'll just measure epoll_wait with no events

        epoll_event_t events[10];
        perf_stat_t stats;
        char name[64];
        snprintf(name, sizeof(name), "epoll_wait (%d fds)", fd_count);
        perf_stat_init(&stats, name);

        // Benchmark epoll_wait (with timeout 0 = non-blocking)
        for (int i = 0; i < 1000; i++) {
            uint64_t start = rdtsc_fence();
            int n = syscall4(SYS_EPOLL_WAIT, epfd, (uint64_t)events, 10, 0);
            uint64_t end = rdtscp();

            perf_stat_record(&stats, end - start);
        }

        perf_stat_report(&stats);

        // Cleanup
        for (int i = 0; i < fd_count; i++) {
            if (fds[i] >= 0) {
                syscall1(SYS_CLOSE_SK, fds[i]);
            }
        }
        free(fds);

        printf("\n");
    }

    printf("[INFO] epoll_wait should show O(1) behavior:\n");
    printf("[INFO] Performance should be similar regardless of fd count\n");
    printf("\n");
}

/**
 * Benchmark epoll_wait with events ready
 */
void bench_epoll_wait_with_events(void) {
    printf("\n[BENCH] Epoll Wait with Ready Events\n");
    printf("=====================================\n");

    int epfd = syscall1(SYS_EPOLL_CREATE, 100);
    if (epfd < 0) {
        printf("[ERROR] Failed to create epoll instance\n");
        return;
    }

    // Create 100 sockets, register all
    int fds[100];
    for (int i = 0; i < 100; i++) {
        fds[i] = syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);

        epoll_event_t ev;
        ev.events = EPOLLIN | EPOLLOUT;  // EPOLLOUT usually ready immediately
        ev.data.fd = fds[i];
        syscall4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, fds[i], (uint64_t)&ev);
    }

    // Benchmark epoll_wait (should return immediately with ready events)
    epoll_event_t events[100];
    perf_stat_t stats;
    perf_stat_init(&stats, "epoll_wait (events ready)");

    for (int i = 0; i < 1000; i++) {
        uint64_t start = rdtsc_fence();
        int n = syscall4(SYS_EPOLL_WAIT, epfd, (uint64_t)events, 100, 0);
        uint64_t end = rdtscp();

        if (n >= 0) {
            perf_stat_record(&stats, end - start);
        }
    }

    perf_stat_report(&stats);

    // Cleanup
    for (int i = 0; i < 100; i++) {
        if (fds[i] >= 0) {
            syscall1(SYS_CLOSE_SK, fds[i]);
        }
    }

    printf("\n");
}

/**
 * Benchmark epoll throughput (events/second)
 */
void bench_epoll_throughput(void) {
    printf("\n[BENCH] Epoll Throughput\n");
    printf("========================\n");

    int epfd = syscall1(SYS_EPOLL_CREATE, 10);
    if (epfd < 0) {
        printf("[ERROR] Failed to create epoll instance\n");
        return;
    }

    // Create one socket
    int sock = syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("[ERROR] Failed to create socket\n");
        return;
    }

    epoll_event_t ev;
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = sock;
    syscall4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, sock, (uint64_t)&ev);

    const int iterations = 100000;
    epoll_event_t events[1];

    uint64_t start = rdtsc_fence();
    for (int i = 0; i < iterations; i++) {
        syscall4(SYS_EPOLL_WAIT, epfd, (uint64_t)events, 1, 0);
    }
    uint64_t end = rdtscp();

    uint64_t total_cycles = end - start;
    uint64_t cycles_per_call = total_cycles / iterations;

    // Assume 3 GHz CPU
    uint64_t cpu_freq_hz = 3000000000ULL;
    uint64_t calls_per_sec = cpu_freq_hz / cycles_per_call;

    printf("[BENCH] Total cycles: %llu\n", (unsigned long long)total_cycles);
    printf("[BENCH] Cycles/call:  %llu\n", (unsigned long long)cycles_per_call);
    printf("[BENCH] Calls/second: %llu (%.2f M/s)\n",
           (unsigned long long)calls_per_sec,
           (double)calls_per_sec / 1000000.0);

    syscall1(SYS_CLOSE_SK, sock);

    printf("\n");
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  EPOLL BENCHMARK SUITE\n");
    printf("=============================================\n");

    bench_epoll_create();
    bench_epoll_ctl();
    bench_epoll_wait_scalability();
    bench_epoll_wait_with_events();
    bench_epoll_throughput();

    printf("=============================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("=============================================\n");
    printf("\nExpected Results:\n");
    printf("  epoll_create:      <1000 cycles\n");
    printf("  epoll_ctl:         <500 cycles\n");
    printf("  epoll_wait:        <500 cycles (O(1) regardless of fd count)\n");
    printf("  Throughput:        >1M calls/sec\n");
    printf("\n");

    return 0;
}
