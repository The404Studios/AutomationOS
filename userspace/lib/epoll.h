/*
 * userspace/lib/epoll.h — Userspace epoll API wrapper
 * ===================================================
 *
 * Provides a convenient interface to the kernel epoll syscalls.
 */

#ifndef USERSPACE_EPOLL_H
#define USERSPACE_EPOLL_H

#include <stdint.h>

/* Epoll syscall numbers */
#define SYS_EPOLL_CREATE 73
#define SYS_EPOLL_CTL    74
#define SYS_EPOLL_WAIT   75

/* epoll_ctl operations */
#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_MOD  2
#define EPOLL_CTL_DEL  3

/* Event flags */
#define EPOLLIN      0x001         // readable
#define EPOLLOUT     0x004         // writable
#define EPOLLERR     0x008         // error condition
#define EPOLLHUP     0x010         // hang up
#define EPOLLET      0x80000000    // edge-triggered mode

/* epoll_event structure */
typedef struct {
    uint32_t events;   // event mask
    uint64_t data;     // user data
} epoll_event_t;

/* Syscall wrappers */
static inline int64_t _syscall1(uint64_t num, uint64_t a1) {
    int64_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    int64_t ret;
    register uint64_t r10 asm("r10") = a4;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

/**
 * Create an epoll instance.
 *
 * @param size_hint  Hint for the number of fds to watch (ignored, kept for Linux compat)
 * @return           epoll file descriptor on success, negative error code on failure
 */
static inline int epoll_create(int size_hint) {
    return (int)_syscall1(SYS_EPOLL_CREATE, size_hint);
}

/**
 * Control interface for an epoll file descriptor.
 *
 * @param epfd   epoll file descriptor
 * @param op     EPOLL_CTL_ADD | EPOLL_CTL_MOD | EPOLL_CTL_DEL
 * @param fd     file descriptor to add/modify/remove
 * @param event  event specification (NULL for EPOLL_CTL_DEL)
 * @return       0 on success, negative error code on failure
 */
static inline int epoll_ctl(int epfd, int op, int fd, epoll_event_t* event) {
    return (int)_syscall4(SYS_EPOLL_CTL, epfd, op, fd, (uint64_t)event);
}

/**
 * Wait for events on an epoll file descriptor.
 *
 * @param epfd       epoll file descriptor
 * @param events     buffer to receive events
 * @param maxevents  maximum number of events to return
 * @param timeout_ms timeout in milliseconds (-1 = infinite, 0 = non-blocking)
 * @return           number of events ready, 0 on timeout, negative on error
 */
static inline int epoll_wait(int epfd, epoll_event_t* events, int maxevents, int timeout_ms) {
    return (int)_syscall4(SYS_EPOLL_WAIT, epfd, (uint64_t)events, maxevents, timeout_ms);
}

#endif /* USERSPACE_EPOLL_H */
