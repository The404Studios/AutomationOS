/**
 * Network Performance Benchmark
 *
 * Measures network stack throughput on loopback interface.
 * Tests socket performance and TCP/IP stack efficiency.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Syscall numbers
#define SYS_SOCKET   51
#define SYS_CONNECT  52
#define SYS_SEND     53
#define SYS_RECV     54
#define SYS_CLOSE_SK 55
#define SYS_BIND     76
#define SYS_LISTEN   77
#define SYS_ACCEPT   78
#define SYS_GET_TICKS_MS 40

// Socket types (kernel ABI: SYS_SOCKET arg1 is type, NOT address family)
#define SOCK_STREAM 1
#define SOCK_DGRAM 2

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

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
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

/* sockaddr_in_t and htons removed -- this kernel's SYS_BIND takes
 * (fd, port_host_order), not a sockaddr struct. */

/**
 * Benchmark TCP loopback throughput
 */
void bench_tcp_loopback(void) {
    printf("\n[BENCH] TCP Loopback Throughput\n");
    printf("================================\n");

    // Create server socket
    // Kernel ABI: SYS_SOCKET(type) -- arg1 = SOCK_STREAM(1) or SOCK_DGRAM(2)
    int server_sock = syscall1(SYS_SOCKET, SOCK_STREAM);
    if (server_sock < 0) {
        printf("[ERROR] Failed to create server socket: %d\n", server_sock);
        printf("[INFO] Network stack may not be fully implemented\n");
        return;
    }

    // Bind to port 8080
    // Kernel ABI: SYS_BIND(fd, port) -- port in host byte order
    int ret = syscall2(SYS_BIND, server_sock, 8080);
    if (ret < 0) {
        printf("[ERROR] Failed to bind: %d\n", ret);
        syscall1(SYS_CLOSE_SK, server_sock);
        return;
    }

    // Listen
    // Kernel ABI: SYS_LISTEN(fd, backlog)
    ret = syscall2(SYS_LISTEN, server_sock, 1);
    if (ret < 0) {
        printf("[ERROR] Failed to listen: %d\n", ret);
        syscall1(SYS_CLOSE_SK, server_sock);
        return;
    }

    printf("[INFO] Server listening on 127.0.0.1:8080\n");

    // For this benchmark, we would need a forked process or threading
    // to act as both client and server. For now, just measure socket creation.

    printf("[INFO] Full loopback test requires process forking\n");
    printf("[INFO] Measuring socket operations only...\n");

    const int iterations = 1000;
    uint64_t start = rdtsc_fence();

    for (int i = 0; i < iterations; i++) {
        int sock = syscall1(SYS_SOCKET, SOCK_STREAM);
        if (sock >= 0) {
            syscall1(SYS_CLOSE_SK, sock);
        }
    }

    uint64_t end = rdtscp();
    uint64_t cycles = end - start;
    uint64_t cycles_per_op = cycles / iterations;

    printf("[BENCH] Socket create/close: %llu cycles/op\n",
           (unsigned long long)cycles_per_op);

    syscall1(SYS_CLOSE_SK, server_sock);
    printf("\n");
}

/**
 * Benchmark socket creation overhead
 */
void bench_socket_creation(void) {
    printf("\n[BENCH] Socket Creation Overhead\n");
    printf("=================================\n");

    const int iterations = 10000;
    uint64_t tcp_total = 0;
    uint64_t udp_total = 0;

    // TCP sockets
    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        int sock = syscall1(SYS_SOCKET, SOCK_STREAM);
        uint64_t end = rdtscp();

        if (sock >= 0) {
            syscall1(SYS_CLOSE_SK, sock);
            tcp_total += (end - start);
        }
    }

    // UDP sockets
    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        int sock = syscall1(SYS_SOCKET, SOCK_DGRAM);
        uint64_t end = rdtscp();

        if (sock >= 0) {
            syscall1(SYS_CLOSE_SK, sock);
            udp_total += (end - start);
        }
    }

    printf("[BENCH] TCP socket creation: %llu cycles (avg)\n",
           (unsigned long long)(tcp_total / iterations));
    printf("[BENCH] UDP socket creation: %llu cycles (avg)\n",
           (unsigned long long)(udp_total / iterations));
    printf("\n");
}

/**
 * Benchmark send/recv buffer copies
 */
void bench_buffer_operations(void) {
    printf("\n[BENCH] Send/Recv Buffer Operations\n");
    printf("====================================\n");

    int sock = syscall1(SYS_SOCKET, SOCK_DGRAM);
    if (sock < 0) {
        printf("[ERROR] Failed to create socket\n");
        return;
    }

    const size_t buf_size = 1024;
    char* send_buf = malloc(buf_size);
    char* recv_buf = malloc(buf_size);

    if (!send_buf || !recv_buf) {
        printf("[ERROR] Failed to allocate buffers\n");
        if (send_buf) free(send_buf);
        if (recv_buf) free(recv_buf);
        syscall1(SYS_CLOSE_SK, sock);
        return;
    }

    memset(send_buf, 0xAA, buf_size);

    const int iterations = 1000;
    uint64_t send_total = 0;

    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        int64_t ret = syscall3(SYS_SEND, sock, (uint64_t)send_buf, buf_size);
        uint64_t end = rdtscp();

        if (ret >= 0) {
            send_total += (end - start);
        }
    }

    printf("[BENCH] Send operation (%zu bytes): %llu cycles (avg)\n",
           buf_size, (unsigned long long)(send_total / iterations));

    free(send_buf);
    free(recv_buf);
    syscall1(SYS_CLOSE_SK, sock);
    printf("\n");
}

/**
 * Benchmark network throughput simulation
 */
void bench_throughput_estimate(void) {
    printf("\n[BENCH] Network Throughput Estimate\n");
    printf("====================================\n");

    const size_t packet_size = 1500;  // MTU
    const int packets = 1000;
    const size_t total_bytes = packet_size * packets;

    int sock = syscall1(SYS_SOCKET, SOCK_DGRAM);
    if (sock < 0) {
        printf("[ERROR] Failed to create socket\n");
        return;
    }

    char* buf = malloc(packet_size);
    if (!buf) {
        syscall1(SYS_CLOSE_SK, sock);
        return;
    }
    memset(buf, 0xAA, packet_size);

    uint64_t start_ms = get_ticks_ms();
    uint64_t sent = 0;

    for (int i = 0; i < packets; i++) {
        int64_t ret = syscall3(SYS_SEND, sock, (uint64_t)buf, packet_size);
        if (ret > 0) {
            sent += ret;
        }
    }

    uint64_t end_ms = get_ticks_ms();
    uint64_t elapsed_ms = end_ms - start_ms;

    free(buf);
    syscall1(SYS_CLOSE_SK, sock);

    if (elapsed_ms > 0) {
        uint64_t mb_per_sec = (sent / 1024) / elapsed_ms;  // KB/ms = MB/s
        printf("[BENCH] Sent %llu bytes in %llu ms\n",
               (unsigned long long)sent, (unsigned long long)elapsed_ms);
        printf("[BENCH] Estimated throughput: %llu MB/s\n",
               (unsigned long long)mb_per_sec);

        if (mb_per_sec >= 80) {
            printf("[PASS] Good network performance\n");
        } else {
            printf("[INFO] Moderate performance (loopback should be >80 MB/s)\n");
        }
    } else {
        printf("[INFO] Operations completed too quickly to measure\n");
    }

    printf("\n");
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  NETWORK BENCHMARK SUITE\n");
    printf("=============================================\n");

    bench_socket_creation();
    bench_buffer_operations();
    bench_throughput_estimate();
    bench_tcp_loopback();

    printf("=============================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("=============================================\n");
    printf("\nExpected Results:\n");
    printf("  Socket creation:   <5000 cycles\n");
    printf("  Send operation:    <3000 cycles\n");
    printf("  Loopback throughput: >100 MB/s\n");
    printf("\n");

    return 0;
}
