/*
 * epoll_server.c — Example epoll-based UDP echo server
 * ====================================================
 *
 * Demonstrates scalable event-driven I/O using epoll:
 *   - Creates 10 UDP sockets bound to different ports
 *   - Adds all sockets to a single epoll instance
 *   - Uses epoll_wait() to efficiently wait for incoming data
 *   - Echoes received datagrams back to sender
 *
 * This pattern scales to thousands of sockets with O(1) CPU overhead when idle.
 */

#include <stdio.h>
#include <string.h>
#include "../lib/epoll.h"

/* Socket syscalls (from kernel/include/socket.h) */
#define SYS_SOCKET       51
#define SYS_SENDTO       56
#define SYS_RECVFROM     57
#define SYS_SOCK_POLL    58

#define SOCK_DGRAM       2

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t _pad;
} sock_addr_t;

/* Syscall wrappers */
static inline int64_t _syscall1(uint64_t num, uint64_t a1) {
    int64_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    int64_t ret;
    register uint64_t r10 asm("r10") = a4;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall5(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t ret;
    register uint64_t r10 asm("r10") = a4;
    register uint64_t r8 asm("r8") = a5;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return ret;
}

static inline int sock_socket(int type) {
    return (int)_syscall1(SYS_SOCKET, type);
}

static inline int sock_sendto(int s, const void* buf, uint32_t len, uint32_t ip, uint16_t port) {
    return (int)_syscall5(SYS_SENDTO, s, (uint64_t)buf, len, ip, port);
}

static inline int sock_recvfrom(int s, void* buf, uint32_t len, sock_addr_t* addr) {
    return (int)_syscall4(SYS_RECVFROM, s, (uint64_t)buf, len, (uint64_t)addr);
}

static inline int sock_poll(void) {
    return (int)_syscall1(SYS_SOCK_POLL, 0);
}

/* ------------------------------------------------------------------ */
/* Main server                                                         */
/* ------------------------------------------------------------------ */

#define NUM_SOCKETS  10
#define BASE_PORT    8000
#define MAX_EVENTS   32
#define BUFFER_SIZE  1472

int main(void) {
    int sockets[NUM_SOCKETS];
    int epfd;
    epoll_event_t events[MAX_EVENTS];
    uint8_t buffer[BUFFER_SIZE];

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Epoll-based UDP Echo Server\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Create sockets */
    printf("[INIT] Creating %d UDP sockets (ports %d-%d)...\n",
           NUM_SOCKETS, BASE_PORT, BASE_PORT + NUM_SOCKETS - 1);

    for (int i = 0; i < NUM_SOCKETS; i++) {
        sockets[i] = sock_socket(SOCK_DGRAM);
        if (sockets[i] < 0) {
            printf("  ERROR: failed to create socket %d\n", i);
            return 1;
        }
    }

    printf("  OK: created %d sockets\n", NUM_SOCKETS);

    /* Create epoll instance */
    printf("[INIT] Creating epoll instance...\n");
    epfd = epoll_create(NUM_SOCKETS);
    if (epfd < 0) {
        printf("  ERROR: epoll_create failed (errno=%d)\n", epfd);
        return 1;
    }
    printf("  OK: epoll fd = %d\n", epfd);

    /* Add all sockets to epoll */
    printf("[INIT] Adding sockets to epoll...\n");
    for (int i = 0; i < NUM_SOCKETS; i++) {
        epoll_event_t ev;
        ev.events = EPOLLIN | EPOLLET;  // edge-triggered
        ev.data = i;  // store socket index

        int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sockets[i], &ev);
        if (ret < 0) {
            printf("  ERROR: epoll_ctl(ADD) failed for socket %d\n", i);
            return 1;
        }
    }
    printf("  OK: added %d sockets\n", NUM_SOCKETS);

    printf("\n[SERVER] Listening on ports %d-%d (Ctrl+C to stop)...\n\n",
           BASE_PORT, BASE_PORT + NUM_SOCKETS - 1);

    /* Event loop */
    while (1) {
        /* Poll network stack */
        sock_poll();

        /* Wait for events (1 second timeout) */
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);

        if (nfds < 0) {
            printf("  ERROR: epoll_wait failed (errno=%d)\n", nfds);
            break;
        }

        if (nfds == 0) {
            /* Timeout - print heartbeat */
            printf("  [heartbeat] waiting for packets...\n");
            continue;
        }

        /* Process events */
        printf("  [EVENTS] %d socket(s) ready\n", nfds);

        for (int i = 0; i < nfds; i++) {
            int sock_idx = (int)events[i].data;
            int sockfd = sockets[sock_idx];
            int port = BASE_PORT + sock_idx;

            printf("    Socket %d (port %d): ", sock_idx, port);

            /* Receive datagram */
            sock_addr_t from;
            int len = sock_recvfrom(sockfd, buffer, BUFFER_SIZE, &from);

            if (len < 0) {
                printf("recvfrom failed (errno=%d)\n", len);
                continue;
            }

            if (len == 0) {
                printf("no data available\n");
                continue;
            }

            printf("received %d bytes from %u.%u.%u.%u:%u\n",
                   len,
                   (from.ip >> 24) & 0xFF,
                   (from.ip >> 16) & 0xFF,
                   (from.ip >> 8) & 0xFF,
                   from.ip & 0xFF,
                   from.port);

            /* Echo back */
            int sent = sock_sendto(sockfd, buffer, len, from.ip, from.port);
            if (sent < 0) {
                printf("      sendto failed (errno=%d)\n", sent);
            } else {
                printf("      echoed %d bytes back\n", sent);
            }
        }
    }

    printf("\n[SERVER] Shutting down...\n");
    return 0;
}
