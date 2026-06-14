/*
 * kernel/core/syscall/epoll.c — Edge-triggered epoll for scalable I/O multiplexing
 * ================================================================================
 *
 * Linux-compatible epoll API for event-driven I/O on sockets. Implements:
 *   - epoll_create()  : create an epoll instance
 *   - epoll_ctl()     : add/modify/remove file descriptors
 *   - epoll_wait()    : block until events are ready (with timeout)
 *
 * Edge-triggered semantics: events are reported when the state CHANGES (e.g., a
 * socket becomes readable), not while it remains readable. Applications must drain
 * all available data/events when notified to avoid missing future wakeups.
 *
 * Design:
 *  - Each epoll instance is a file descriptor that tracks a set of "interest" fds.
 *  - When a monitored socket becomes readable/writable, the epoll instance records
 *    the event and wakes any threads blocked in epoll_wait().
 *  - Uses wait_queue infrastructure for blocking/wakeup (same as futex/waitpid).
 *  - O(1) wakeup: only threads waiting on epoll instances with ready events wake.
 *
 * Scope: kernel/core/syscall/epoll.c (new).
 */

#include "../../include/syscall.h"
#include "../../include/types.h"
#include "../../include/errno.h"
#include "../../include/sched.h"
#include "../../include/socket.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/string.h"
#include "../../include/drivers.h"  // timer_get_ticks_ms
#include "../../include/spinlock.h"
#include "../../include/poll.h"     // POLL-SELECT-0: fd_poll_state / poll_pump / poll_sleep_slice

/* ------------------------------------------------------------------ */
/* Constants and structures                                            */
/* ------------------------------------------------------------------ */

#define EPOLL_MAX_INSTANCES   64      // max epoll fds per system
#define EPOLL_MAX_WATCHES     256     // max fds watched per epoll instance
#define EPOLL_MAX_EVENTS      128     // max events returned by epoll_wait

// epoll_ctl operations
#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_MOD  2
#define EPOLL_CTL_DEL  3

// Event flags (match Linux)
#define EPOLLIN      0x001   // readable
#define EPOLLOUT     0x004   // writable
#define EPOLLERR     0x008   // error condition
#define EPOLLHUP     0x010   // hang up
#define EPOLLET      0x80000000  // edge-triggered mode (vs level-triggered)

// epoll_event structure (must match userspace definition)
typedef struct {
    uint32_t events;   // event mask (EPOLLIN | EPOLLOUT | etc)
    uint64_t data;     // user data (typically the fd)
} epoll_event_t;

// Watched file descriptor entry
typedef struct epoll_watch {
    int fd;                  // watched file descriptor (-1 = unused slot)
    uint32_t events;         // event mask (EPOLLIN | EPOLLOUT)
    uint64_t user_data;      // opaque user data (passed back in epoll_event)
    uint32_t last_state;     // last reported state (for edge-trigger detection)
} epoll_watch_t;

// Ready event entry (ring buffer)
typedef struct {
    uint32_t events;    // triggered events
    uint64_t data;      // user data from watch
} ready_event_t;

// epoll instance descriptor
typedef struct epoll_instance {
    bool used;                           // slot in use?
    spinlock_t lock;                     // protects watches/ready_events
    epoll_watch_t watches[EPOLL_MAX_WATCHES];

    // Ready event ring (edge-triggered: an event is added when state changes)
    ready_event_t ready_events[EPOLL_MAX_EVENTS];
    int ready_head;                      // next slot to read
    int ready_tail;                      // next slot to write
    int ready_count;                     // number of ready events

    wait_queue_t wait_queue;             // processes blocked in epoll_wait
} epoll_instance_t;

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static epoll_instance_t* g_epoll_instances = NULL;
static spinlock_t g_epoll_lock = {0, 0xFFFFFFFF, NULL};
static bool g_epoll_initialized = false;

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

void epoll_init(void) {
    if (g_epoll_initialized) return;

    // Allocate epoll instances table
    g_epoll_instances = (epoll_instance_t*)kmalloc(
        sizeof(epoll_instance_t) * EPOLL_MAX_INSTANCES);
    if (!g_epoll_instances) {
        kprintf("[EPOLL] FATAL: failed to allocate instance table\n");
        return;
    }

    memset(g_epoll_instances, 0, sizeof(epoll_instance_t) * EPOLL_MAX_INSTANCES);

    // Initialize all instances
    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        epoll_instance_t* ep = &g_epoll_instances[i];
        ep->used = false;
        spin_lock_init(&ep->lock);
        wq_init(&ep->wait_queue);

        // Mark all watch slots as unused
        for (int j = 0; j < EPOLL_MAX_WATCHES; j++) {
            ep->watches[j].fd = -1;
        }
    }

    g_epoll_initialized = true;
    kprintf("[EPOLL] Initialized (instances=%d, watches/inst=%d)\n",
            EPOLL_MAX_INSTANCES, EPOLL_MAX_WATCHES);
}

/* ------------------------------------------------------------------ */
/* Helper: validate epoll fd and return instance                      */
/* ------------------------------------------------------------------ */

static epoll_instance_t* epoll_from_fd(int epfd) {
    if (!g_epoll_initialized) return NULL;

    // epoll fds are encoded as: 0x10000 + instance_index
    // (to distinguish from regular socket fds which are 0..15)
    if (epfd < 0x10000 || epfd >= 0x10000 + EPOLL_MAX_INSTANCES)
        return NULL;

    int idx = epfd - 0x10000;
    epoll_instance_t* ep = &g_epoll_instances[idx];
    if (!ep->used) return NULL;

    return ep;
}

/* ------------------------------------------------------------------ */
/* Helper: find watch entry for fd                                    */
/* ------------------------------------------------------------------ */

static epoll_watch_t* epoll_find_watch(epoll_instance_t* ep, int fd) {
    for (int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        if (ep->watches[i].fd == fd)
            return &ep->watches[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Helper: add ready event (edge-triggered: only if state changed)    */
/* ------------------------------------------------------------------ */

static void epoll_add_ready(epoll_instance_t* ep, int fd, uint32_t new_state) {
    // Find the watch
    epoll_watch_t* w = epoll_find_watch(ep, fd);
    if (!w) return;

    // Edge-triggered: only add event if state CHANGED
    uint32_t triggered = (new_state & w->events) & ~w->last_state;
    if (triggered == 0) return;  // no new events

    w->last_state = new_state & w->events;

    // Add to ready ring (drop if full)
    if (ep->ready_count >= EPOLL_MAX_EVENTS) return;

    ready_event_t* re = &ep->ready_events[ep->ready_tail];
    re->events = triggered;
    re->data = w->user_data;

    ep->ready_tail = (ep->ready_tail + 1) % EPOLL_MAX_EVENTS;
    ep->ready_count++;

    // Wake one waiter (if any)
    wq_wake_one(&ep->wait_queue);
}

/* ------------------------------------------------------------------ */
/* Helper: is any watched fd currently ready? (side-effect-free probe) */
/* Used by fd_poll_state() so an epoll fd can itself be poll()'d. The   */
/* EPOLL* bits are numerically identical to the POLL* bits, so          */
/* fd_poll_state() output is reused directly with no translation.       */
/* ------------------------------------------------------------------ */
int epoll_has_ready(int epfd) {
    epoll_instance_t* ep = epoll_from_fd(epfd);
    if (!ep) return 0;
    int ready = 0;
    spin_lock(&ep->lock);
    for (int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        if (ep->watches[i].fd == -1) continue;
        uint32_t want = ep->watches[i].events | EPOLLERR | EPOLLHUP;
        if (fd_poll_state(ep->watches[i].fd) & want) { ready = 1; break; }
    }
    spin_unlock(&ep->lock);
    return ready;
}

/* ------------------------------------------------------------------ */
/* SYS_EPOLL_CREATE: create an epoll instance                         */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_create(uint64_t size_hint, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)size_hint; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!g_epoll_initialized) epoll_init();
    if (!g_epoll_initialized) return ENOMEM;

    spin_lock(&g_epoll_lock);

    // Find unused instance
    int idx = -1;
    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (!g_epoll_instances[i].used) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        spin_unlock(&g_epoll_lock);
        return EMFILE;  // too many epoll instances
    }

    epoll_instance_t* ep = &g_epoll_instances[idx];
    ep->used = true;
    ep->ready_head = 0;
    ep->ready_tail = 0;
    ep->ready_count = 0;

    // Clear all watches
    for (int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        ep->watches[i].fd = -1;
        ep->watches[i].last_state = 0;
    }

    spin_unlock(&g_epoll_lock);

    // Return epoll fd (encoded as 0x10000 + idx)
    int epfd = 0x10000 + idx;
    return epfd;
}

/* ------------------------------------------------------------------ */
/* SYS_EPOLL_CTL: add/modify/remove watched fds                       */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_ctl(uint64_t epfd_arg, uint64_t op_arg, uint64_t fd_arg,
                      uint64_t event_ptr, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;

    int epfd = (int)epfd_arg;
    int op = (int)op_arg;
    int fd = (int)fd_arg;

    epoll_instance_t* ep = epoll_from_fd(epfd);
    if (!ep) return EBADF;

    spinlock_acquire(&ep->lock);

    if (op == EPOLL_CTL_ADD) {
        // Read user event structure
        epoll_event_t event;
        if (copy_from_user(&event, (void*)event_ptr, sizeof(event)) != 0) {
            spin_unlock(&ep->lock);
            return EFAULT;
        }

        // Check if already watched
        if (epoll_find_watch(ep, fd) != NULL) {
            spin_unlock(&ep->lock);
            return EEXIST;
        }

        // Find free watch slot
        epoll_watch_t* w = NULL;
        for (int i = 0; i < EPOLL_MAX_WATCHES; i++) {
            if (ep->watches[i].fd == -1) {
                w = &ep->watches[i];
                break;
            }
        }

        if (!w) {
            spin_unlock(&ep->lock);
            return ENOSPC;  // too many watches
        }

        w->fd = fd;
        w->events = event.events;
        w->user_data = event.data;
        w->last_state = 0;  // edge trigger: next poll will report initial state

        spin_unlock(&ep->lock);
        return 0;
    }
    else if (op == EPOLL_CTL_MOD) {
        epoll_event_t event;
        if (copy_from_user(&event, (void*)event_ptr, sizeof(event)) != 0) {
            spin_unlock(&ep->lock);
            return EFAULT;
        }

        epoll_watch_t* w = epoll_find_watch(ep, fd);
        if (!w) {
            spin_unlock(&ep->lock);
            return ENOENT;
        }

        w->events = event.events;
        w->user_data = event.data;
        w->last_state = 0;  // reset edge state

        spin_unlock(&ep->lock);
        return 0;
    }
    else if (op == EPOLL_CTL_DEL) {
        epoll_watch_t* w = epoll_find_watch(ep, fd);
        if (!w) {
            spin_unlock(&ep->lock);
            return ENOENT;
        }

        w->fd = -1;  // mark slot as unused

        spin_unlock(&ep->lock);
        return 0;
    }

    spinlock_release(&ep->lock);
    return EINVAL;  // invalid op
}

/* ------------------------------------------------------------------ */
/* SYS_EPOLL_WAIT: block until events ready (with timeout)            */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_wait(uint64_t epfd_arg, uint64_t events_ptr,
                       uint64_t maxevents_arg, uint64_t timeout_ms,
                       uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;

    int epfd = (int)epfd_arg;
    int maxevents = (int)maxevents_arg;

    if (maxevents <= 0 || maxevents > EPOLL_MAX_EVENTS)
        return EINVAL;

    epoll_instance_t* ep = epoll_from_fd(epfd);
    if (!ep) return EBADF;

    // timeout_ms: (uint64_t)-1 == infinite, 0 == return immediately, else ms.
    int infinite  = (timeout_ms == (uint64_t)-1);
    int immediate = (timeout_ms == 0);
    uint64_t deadline = infinite ? 0 : timer_get_ticks() + timeout_ms;

    // POLL-SELECT-0: scan each watch against the REAL readiness probe
    // (fd_poll_state) every wait — no more "always EPOLLIN" fake, no sock_poll
    // pump-hack inside the readiness check (poll_pump centralizes RX advance).
    // Level-triggered by default; a watch with EPOLLET set reports only the
    // bits that newly asserted since its last_state.
    for (;;) {
        poll_pump();                       // advance network RX once per wait

        spin_lock(&ep->lock);
        int n = 0;
        for (int i = 0; i < EPOLL_MAX_WATCHES && n < maxevents; i++) {
            epoll_watch_t* w = &ep->watches[i];
            if (w->fd == -1) continue;

            uint32_t want = w->events | EPOLLERR | EPOLLHUP;   // err/hup always
            uint32_t st   = fd_poll_state(w->fd) & want;

            uint32_t report = (w->events & EPOLLET)
                            ? (st & ~w->last_state)   // edge: only new bits
                            : st;                     // level: while ready
            w->last_state = st;
            if (!report) continue;

            epoll_event_t ev;
            ev.events = report;
            ev.data   = w->user_data;
            if (copy_to_user((void*)(events_ptr + (uint64_t)n * sizeof(ev)),
                             &ev, sizeof(ev)) != 0) {
                spin_unlock(&ep->lock);
                return EFAULT;
            }
            n++;
        }
        spin_unlock(&ep->lock);

        if (n > 0 || immediate) return n;
        if (!infinite && timer_get_ticks() >= deadline) return 0;  // timed out

        uint64_t until = timer_get_ticks() + 5;   // 5ms re-check slice (yields)
        if (!infinite && until > deadline) until = deadline;
        poll_sleep_slice(until);
    }
}

/* ------------------------------------------------------------------ */
/* Hook: notify epoll instances when socket state changes             */
/* ------------------------------------------------------------------ */

void epoll_notify_socket(int sockfd, uint32_t events) {
    if (!g_epoll_initialized) return;

    // Scan all epoll instances for watches on this socket
    spin_lock(&g_epoll_lock);

    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (!g_epoll_instances[i].used) continue;

        epoll_instance_t* ep = &g_epoll_instances[i];
        spin_lock(&ep->lock);

        epoll_add_ready(ep, sockfd, events);

        spin_unlock(&ep->lock);
    }

    spin_unlock(&g_epoll_lock);
}
