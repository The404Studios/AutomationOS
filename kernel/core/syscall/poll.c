/*
 * kernel/core/syscall/poll.c — POLL-SELECT-0 (B10): poll(2) + select(2)
 * ============================================================================
 * Implements fd-readiness multiplexing over the (non-unified) fd kinds in this
 * kernel and the shared readiness probe fd_poll_state() that epoll also uses.
 *
 * Blocking model: socket RX state only advances when the stack is pumped
 * (sock_poll) — there is no interrupt-driven NIC RX — so a blocking poll/select
 * is a pump → check → yielding-sleep loop bounded by the caller's timeout. The
 * sleep uses the same wait_object_block() timer-wait as sys_sleep (zero CPU,
 * yields to other processes, works in cooperative AND preemptive builds).
 */

#include "../../include/poll.h"
#include "../../include/types.h"
#include "../../include/errno.h"
#include "../../include/sched.h"
#include "../../include/socket.h"
#include "../../include/vfs.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/string.h"
#include "../../include/drivers.h"   /* timer_get_ticks */

#define POLL_MAX_FDS    64    /* max pollfd entries per poll() call    */
#define FD_SETSIZE_K    256   /* select() fd ceiling (4 x uint64 mask) */
#define POLL_SLICE_MS   5     /* re-check cadence while blocking        */

/* epoll readiness probe (defined in epoll.c). 1 if the epoll fd has pending
 * events. Declared here to avoid a header cycle. */
extern int epoll_has_ready(int epfd);

/* Timer-only wait object shared by all polling sleeps. Nothing ever signals it,
 * so a waiter is only ever woken by its own deadline (then self-unlinks). */
static wait_object_t g_poll_wobj = WAIT_OBJECT_INITIALIZER;

void poll_pump(void) {
    /* sock_poll() internally no-ops if the net stack is down. This is the only
     * thing that pulls NIC frames into socket RX buffers today. */
    sock_poll();
}

void poll_sleep_slice(uint64_t until_ticks) {
    (void)wait_object_block(&g_poll_wobj, until_ticks);
}

/* ------------------------------------------------------------------ */
/* The unified readiness probe.                                        */
/* Precedence: epoll-encoded fd > live socket > regular vfs file >     */
/* std stdout/stderr > stdin > invalid. (Socket indices 0..31 shadow   */
/* same-numbered std/vfs fds — a known limitation of the non-unified   */
/* fd model; an fd is a socket iff g_socks[fd] is live.)               */
/* ------------------------------------------------------------------ */
uint32_t fd_poll_state(int fd) {
    if (fd < 0) return POLLNVAL;

    /* epoll instance fd (encoded 0x10000 + idx) */
    if (fd >= 0x10000) {
        return epoll_has_ready(fd) ? POLLIN : 0;
    }

    /* live socket? */
    int sb = sock_poll_bits(fd);
    if (sb >= 0) {
        uint32_t r = 0;
        if (sb & SOCKPOLL_READ)  r |= POLLIN;
        if (sb & SOCKPOLL_WRITE) r |= POLLOUT;
        if (sb & SOCKPOLL_HUP)   r |= POLLHUP;
        if (sb & SOCKPOLL_ERR)   r |= POLLERR;
        return r;
    }

    /* regular vfs file: never blocks for poll purposes (always read/writable) */
    if (vfs_fd_get(fd) != NULL) {
        return POLLIN | POLLOUT;
    }

    /* std streams (only when not shadowed by a same-numbered socket above) */
    if (fd == 1 || fd == 2) return POLLOUT;   /* stdout/stderr always writable */
    if (fd == 0)            return 0;          /* stdin: no non-consuming probe wired yet */

    return POLLNVAL;
}

/* ------------------------------------------------------------------ */
/* SYS_POLL: poll(struct pollfd* fds, nfds_t nfds, int timeout_ms)     */
/* timeout: <0 = infinite, 0 = return immediately, >0 = milliseconds.  */
/* Returns the number of fds with a non-zero revents, or -errno.       */
/* ------------------------------------------------------------------ */
int64_t sys_poll(uint64_t fds_ptr, uint64_t nfds_arg, uint64_t timeout_ms,
                 uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;

    int nfds = (int)nfds_arg;
    if (nfds < 0 || nfds > POLL_MAX_FDS) return EINVAL;

    struct pollfd kfds[POLL_MAX_FDS];
    if (nfds > 0) {
        if (copy_from_user(kfds, (void*)fds_ptr,
                           (size_t)nfds * sizeof(struct pollfd)) != 0)
            return EFAULT;
    }

    int64_t timeout = (int64_t)timeout_ms;
    int infinite  = (timeout < 0);
    int immediate = (timeout == 0);
    uint64_t deadline = infinite ? 0 : timer_get_ticks() + (uint64_t)timeout;

    int ready = 0;
    for (;;) {
        poll_pump();

        ready = 0;
        for (int i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            if (kfds[i].fd < 0) continue;     /* negative fd ignored (POSIX) */
            uint32_t want = (uint32_t)(unsigned short)kfds[i].events
                          | POLLERR | POLLHUP | POLLNVAL;   /* always reported */
            uint32_t rev  = fd_poll_state(kfds[i].fd) & want;
            if (rev) { kfds[i].revents = (short)rev; ready++; }
        }

        if (ready > 0 || immediate) break;
        if (!infinite && timer_get_ticks() >= deadline) break;   /* timed out */

        uint64_t until = timer_get_ticks() + POLL_SLICE_MS;
        if (!infinite && until > deadline) until = deadline;
        poll_sleep_slice(until);
    }

    if (nfds > 0) {
        if (copy_to_user((void*)fds_ptr, kfds,
                         (size_t)nfds * sizeof(struct pollfd)) != 0)
            return EFAULT;
    }
    return ready;
}

/* ------------------------------------------------------------------ */
/* SYS_SELECT: select(nfds, readfds, writefds, exceptfds, timeval*)    */
/* fd_set is FD_SETSIZE_K bits packed into uint64 words (kernel/user    */
/* must agree). timeout: NULL ptr = infinite, else struct {long sec,    */
/* long usec}. Returns the count of ready fds across all three sets.    */
/* ------------------------------------------------------------------ */
typedef struct { uint64_t w[FD_SETSIZE_K / 64]; } fdset_k;

static inline int fdset_test(const fdset_k* s, int fd) {
    return (s->w[fd >> 6] >> (fd & 63)) & 1ull;
}
static inline void fdset_set(fdset_k* s, int fd) {
    s->w[fd >> 6] |= (1ull << (fd & 63));
}

int64_t sys_select(uint64_t nfds_arg, uint64_t rfds_ptr, uint64_t wfds_ptr,
                   uint64_t efds_ptr, uint64_t timeout_ptr, uint64_t a6) {
    (void)a6;

    int nfds = (int)nfds_arg;
    if (nfds < 0 || nfds > FD_SETSIZE_K) return EINVAL;

    fdset_k in_r = {{0}}, in_w = {{0}}, in_e = {{0}};
    if (rfds_ptr && copy_from_user(&in_r, (void*)rfds_ptr, sizeof(in_r)) != 0) return EFAULT;
    if (wfds_ptr && copy_from_user(&in_w, (void*)wfds_ptr, sizeof(in_w)) != 0) return EFAULT;
    if (efds_ptr && copy_from_user(&in_e, (void*)efds_ptr, sizeof(in_e)) != 0) return EFAULT;

    int64_t timeout = -1;   /* infinite if no timeval given */
    if (timeout_ptr) {
        struct { int64_t tv_sec; int64_t tv_usec; } tv;
        if (copy_from_user(&tv, (void*)timeout_ptr, sizeof(tv)) != 0) return EFAULT;
        timeout = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (timeout < 0) timeout = 0;
    }
    int infinite  = (timeout < 0);
    int immediate = (timeout == 0);
    uint64_t deadline = infinite ? 0 : timer_get_ticks() + (uint64_t)timeout;

    fdset_k out_r = {{0}}, out_w = {{0}}, out_e = {{0}};
    int total = 0;
    for (;;) {
        poll_pump();

        out_r = (fdset_k){{0}}; out_w = (fdset_k){{0}}; out_e = (fdset_k){{0}};
        total = 0;
        for (int fd = 0; fd < nfds; fd++) {
            int want_r = rfds_ptr && fdset_test(&in_r, fd);
            int want_w = wfds_ptr && fdset_test(&in_w, fd);
            int want_e = efds_ptr && fdset_test(&in_e, fd);
            if (!want_r && !want_w && !want_e) continue;

            uint32_t st = fd_poll_state(fd);
            /* select "readable" includes EOF/HUP/ERR so a drained peer wakes. */
            if (want_r && (st & (POLLIN | POLLHUP | POLLERR))) { fdset_set(&out_r, fd); total++; }
            if (want_w && (st & POLLOUT))                      { fdset_set(&out_w, fd); total++; }
            if (want_e && (st & (POLLPRI | POLLERR)))          { fdset_set(&out_e, fd); total++; }
        }

        if (total > 0 || immediate) break;
        if (!infinite && timer_get_ticks() >= deadline) break;

        uint64_t until = timer_get_ticks() + POLL_SLICE_MS;
        if (!infinite && until > deadline) until = deadline;
        poll_sleep_slice(until);
    }

    /* select() rewrites the fd_sets in place to the ready subset. */
    if (rfds_ptr && copy_to_user((void*)rfds_ptr, &out_r, sizeof(out_r)) != 0) return EFAULT;
    if (wfds_ptr && copy_to_user((void*)wfds_ptr, &out_w, sizeof(out_w)) != 0) return EFAULT;
    if (efds_ptr && copy_to_user((void*)efds_ptr, &out_e, sizeof(out_e)) != 0) return EFAULT;
    return total;
}
