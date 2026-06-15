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
 * Design (POLL-SELECT-0, pull-based):
 *  - Each epoll instance is an fd (encoded 0x10000+idx) tracking a set of
 *    "interest" fds.
 *  - epoll_wait() re-scans the interest set against fd_poll_state() each wait,
 *    sleeping a short slice via poll_sleep_slice() until something is ready or
 *    the timeout expires. Level-triggered by default; EPOLLET reports only the
 *    bits newly asserted since a watch's last_state. Readiness is PULLED, not
 *    pushed — there is no event ring or wait_queue (that scaffolding was dead
 *    and was removed; see the epoll_instance struct note).
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

// epoll instance descriptor. AUDIT FIX: the ready_events ring + wait_queue +
// epoll_add_ready/epoll_notify_socket scaffolding was DEAD — sys_epoll_wait
// re-scans the watch set against fd_poll_state() each wait (level/edge) and
// never read the ring nor blocked on the queue, and epoll_notify_socket had no
// caller. Removed (~2 KB/instance, ~130 KB across the 64-slot table) so the
// struct describes the actual pull-based model.
typedef struct epoll_instance {
    bool used;                           // slot in use?
    uint32_t owner_pid;                  // creating process (AUDIT FIX: exit reclaim)
    spinlock_t lock;                     // protects watches
    epoll_watch_t watches[EPOLL_MAX_WATCHES];
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
    { process_t* cur = process_get_current(); ep->owner_pid = cur ? cur->pid : 0; }

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
/* AUDIT FIX — instance reclamation. Mirrors sock_cleanup_process.     */
/* Before this, close(epfd) returned EBADF (sys_close rejects fd >=    */
/* MAX_FDS and an epoll fd is >= 0x10000) and nothing reclaimed an     */
/* instance on process exit, so the 64 system-wide slots leaked        */
/* permanently -> EMFILE for the whole system after 64 lifetime        */
/* epoll_create()s (an unprivileged exhaustion DoS).                   */
/* ------------------------------------------------------------------ */

static void epoll_release(epoll_instance_t* ep) {
    spin_lock(&ep->lock);
    for (int i = 0; i < EPOLL_MAX_WATCHES; i++) ep->watches[i].fd = -1;
    ep->owner_pid = 0;
    ep->used = false;
    spin_unlock(&ep->lock);
}

// Release an epoll fd. Called from sys_close() for the 0x10000.. fd range.
// Returns 0 on success, EBADF (positive, matching sys_close) for a bad epfd.
int epoll_close(int epfd) {
    epoll_instance_t* ep = epoll_from_fd(epfd);
    if (!ep) return EBADF;
    epoll_release(ep);
    return 0;
}

// Reclaim every epoll instance owned by a dying process (process teardown).
void epoll_cleanup_process(uint32_t pid) {
    if (!g_epoll_initialized) return;
    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (g_epoll_instances[i].used && g_epoll_instances[i].owner_pid == pid)
            epoll_release(&g_epoll_instances[i]);
    }
}
