/*
 * kernel/include/poll.h — POLL-SELECT-0 (B10)
 * ============================================================================
 * Unified fd-readiness multiplexing: poll(2), select(2), and the readiness
 * probe shared with epoll. There is no single fd namespace in this kernel
 * (sockets index g_socks[0..31], regular files use vfs fds, epoll fds are
 * encoded 0x10000+idx, and 0/1/2 are the std streams), so fd_poll_state()
 * dispatches by probing each backing object in a fixed precedence.
 *
 * Readiness for sockets only advances when the network RX path is pumped
 * (sock_poll) — there is no interrupt-driven RX here — so the blocking forms
 * are a pump/check/yield loop (poll_pump + a yielding timed wait), NOT a pure
 * event wait. This is intrinsic until interrupt-driven NIC RX exists.
 */
#ifndef _KERNEL_POLL_H
#define _KERNEL_POLL_H

#include "types.h"

/* poll(2) event bits — Linux/POSIX ABI values. NOTE: these are deliberately the
 * SAME values epoll uses (EPOLLIN=0x001, EPOLLOUT=0x004, ...), so fd_poll_state()
 * output feeds both poll() and epoll without translation. */
#define POLLIN    0x001   /* data to read                  */
#define POLLPRI   0x002   /* urgent data                   */
#define POLLOUT   0x004   /* writable                      */
#define POLLERR   0x008   /* error (always reported)       */
#define POLLHUP   0x010   /* hang up (always reported)     */
#define POLLNVAL  0x020   /* invalid fd (always reported)  */

struct pollfd {
    int   fd;        /* fd to poll (<0 => ignored, revents set 0) */
    short events;    /* requested events                          */
    short revents;   /* returned events                           */
};

/* Current POLL* readiness of any fd kind. POLLNVAL for an unknown fd. Does NOT
 * pump the network — call poll_pump() first if socket RX state must be fresh. */
uint32_t fd_poll_state(int fd);

/* Advance state machines whose readiness only changes when polled (today: the
 * network RX path, via sock_poll). No-op when the net stack is down. */
void poll_pump(void);

/* Yielding timed wait used by the poll/select/epoll re-check loops: blocks the
 * caller at zero CPU until `until_ticks` (absolute, 1 tick == 1 ms), letting
 * other processes run. Woken only by the timer (shared timer-only wait object). */
void poll_sleep_slice(uint64_t until_ticks);

int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout_ms,
                 uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                   uint64_t exceptfds, uint64_t timeout_ptr, uint64_t a6);

#endif /* _KERNEL_POLL_H */
