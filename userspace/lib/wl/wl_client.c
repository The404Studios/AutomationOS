/*
 * wl_client.c -- M3 "Wayland-lite" client library implementation.
 * ===============================================================
 *
 * Freestanding (ring 3, no libc). Implements wl_client.h over the kernel
 * SysV IPC syscalls. See userspace/include/wl_proto.h for the wire protocol.
 *
 * Build (freestanding, flags passed DIRECTLY -- see wltest.c / project rule):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -c wl_client.c -o wl_client.o
 */

#include "wl_client.h"
#include "../../include/wl_proto.h"

/* ---- syscall numbers (per task spec) ---- */
#define SYS_WRITE         3
#define SYS_GETPID        8
#define SYS_YIELD         15
#define SYS_SHMGET        18    /* (key, size, flg)            -> id        */
#define SYS_SHMAT         19    /* (id, addr, flg)             -> mapped va */
#define SYS_SHMDT         20    /* (addr)                                   */
#define SYS_MSGGET        22    /* (key, flg)                  -> qid       */
#define SYS_MSGSND        23    /* (qid, ptr, sz, flg)                      */
#define SYS_MSGRCV        24    /* (qid, ptr, sz, typ, flg)    -> nbytes    */
#define SYS_GET_TICKS_MS  40

/* ---- IPC flags / perms (mirror kernel/include/ipc.h) ---- */
#define IPC_PRIVATE   0
#define IPC_CREAT     0x0200
#define IPC_NOWAIT    0x0800

/* 6-argument inline syscall (args rdi/rsi/rdx/r10/r8/r9). */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny serial diagnostics (fd 1) ---- */
static unsigned long wl_strlen(const char *s) {
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}
static void wl_print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)wl_strlen(m), 0, 0, 0); }

/* ---- library state ---- */
static int      g_pid       = -1;   /* cached getpid()                    */
static int      g_inbox_qid = -1;   /* compositor command queue id        */
static int      g_reply_qid = -1;   /* this process's event queue id      */
static int      g_connected = 0;

/* A single window record handed back by reference (one window per client). */
static wl_window g_window;

int wl_connect(void) {
    g_pid = (int)sc(SYS_GETPID, 0, 0, 0, 0, 0, 0);

    /* Shared compositor inbox: create if it does not exist yet. */
    long inbox = sc(SYS_MSGGET, WL_COMP_INBOX_KEY, IPC_CREAT | 0666, 0, 0, 0, 0);
    if (inbox < 0) {
        wl_print("[WL] msgget(inbox) failed\n");
        return (int)inbox;
    }
    g_inbox_qid = (int)inbox;

    /* Per-client reply/event queue keyed by our pid. */
    long reply = sc(SYS_MSGGET, WL_REPLY_KEY(g_pid), IPC_CREAT | 0666, 0, 0, 0, 0);
    if (reply < 0) {
        wl_print("[WL] msgget(reply) failed\n");
        return (int)reply;
    }
    g_reply_qid = (int)reply;

    g_connected = 1;
    return 0;
}

wl_window *wl_create_window(wl_u32 w, wl_u32 h, const char *title) {
    if (!g_connected) {
        if (wl_connect() != 0) return 0;
    }
    if (w == 0 || h == 0) return 0;
    /* Bound dimensions: keeps w*4 (the ARGB32 stride) from overflowing 32 bits
     * and matches the compositor's screen-size geometry check. */
    if (w > 4096u || h > 4096u) return 0;

    wl_u32 stride = w * 4u;                 /* ARGB32; w<=4096 so no overflow */
    unsigned long bytes = (unsigned long)stride * (unsigned long)h;

    /* Allocate a private shared-memory segment for the pixel buffer. */
    long shm_id = sc(SYS_SHMGET, IPC_PRIVATE, (long)bytes, IPC_CREAT | 0666, 0, 0, 0);
    if (shm_id < 0) {
        wl_print("[WL] shmget failed\n");
        return 0;
    }

    /* Map it into our address space (kernel chooses the VA when addr==0). */
    long va = sc(SYS_SHMAT, shm_id, 0, 0, 0, 0, 0);
    if (va < 0) {
        wl_print("[WL] shmat failed\n");
        return 0;
    }

    g_window.win_id = -1;
    g_window.shm_id = (int)shm_id;
    g_window.w      = w;
    g_window.h      = h;
    g_window.stride = stride;
    g_window.pixels = (wl_u32 *)va;

    /* Compose and send WL_REQ_CREATE. */
    wl_create_req req;
    req.mtype  = WL_REQ_CREATE;
    req.pid    = g_pid;
    req.shm_id = (int)shm_id;
    req.w      = w;
    req.h      = h;
    req.stride = stride;
    /* Copy the title (truncate to the fixed 48-byte field, NUL terminated). */
    {
        unsigned i = 0;
        if (title) {
            for (; i < sizeof(req.title) - 1 && title[i]; i++) req.title[i] = title[i];
        }
        for (; i < sizeof(req.title); i++) req.title[i] = 0;
    }

    long msgsz = (long)(sizeof(req) - sizeof(long));   /* payload after mtype */
    long sr = sc(SYS_MSGSND, g_inbox_qid, (long)&req, msgsz, 0, 0, 0);
    if (sr < 0) {
        wl_print("[WL] msgsnd(CREATE) failed\n");
        return 0;
    }

    /* Block for the WL_EVT_CREATED reply carrying our window id. */
    wl_created_evt evt;
    long rsz = (long)(sizeof(evt) - sizeof(long));
    /* Bounded spin: blocking msgrcv is not implemented kernel-side yet (returns
       ENOMSG), so we spin-yield until the compositor posts the reply. Cap the
       wait so a missing/dead compositor causes a clean failure (return NULL,
       which every caller checks) instead of hanging the app forever. */
    long spins = 0;
    const long WL_CREATE_MAX_SPINS = 2000000;
    for (;;) {
        long rr = sc(SYS_MSGRCV, g_reply_qid, (long)&evt, rsz, WL_EVT_CREATED, 0, 0);
        if (rr >= 0) break;
        if (++spins > WL_CREATE_MAX_SPINS) {
            wl_print("[WL] wl_create_window: timed out waiting for compositor\n");
            return 0;
        }
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    g_window.win_id = evt.win_id;
    return &g_window;
}

/* Reallocate the window's pixel buffer to w x h in response to a compositor
 * WL_EVT_CONFIGURE (e.g. maximize). Allocates a NEW shm segment, maps it, points
 * g_window at it, tells the compositor the new shm_id (WL_REQ_RESIZE), then
 * detaches the OLD mapping. The old SHM segment stays alive until the compositor
 * also detaches it (it re-attaches on WL_REQ_RESIZE), so there is no torn read. */
static void wl_resize_buffer(wl_u32 w, wl_u32 h) {
    if (w == 0 || h == 0 || w > 4096u || h > 4096u) return;
    if (w == g_window.w && h == g_window.h) return;          /* no-op */

    wl_u32 stride = w * 4u;
    unsigned long bytes = (unsigned long)stride * (unsigned long)h;

    long shm_id = sc(SYS_SHMGET, IPC_PRIVATE, (long)bytes, IPC_CREAT | 0666, 0, 0, 0);
    if (shm_id < 0) { wl_print("[WL] resize shmget failed\n"); return; }
    long va = sc(SYS_SHMAT, shm_id, 0, 0, 0, 0, 0);
    if (va < 0) { wl_print("[WL] resize shmat failed\n"); return; }

    /* Point the window at the new buffer BEFORE notifying so the app's next draw
     * lands in the buffer the compositor is about to map. */
    g_window.shm_id = (int)shm_id;
    g_window.w      = w;
    g_window.h      = h;
    g_window.stride = stride;
    g_window.pixels = (wl_u32 *)va;

    wl_resize_req req;
    req.mtype  = WL_REQ_RESIZE;
    req.win_id = g_window.win_id;
    req.shm_id = (int)shm_id;
    req.w      = w;
    req.h      = h;
    req.stride = stride;
    long msgsz = (long)(sizeof(req) - sizeof(long));
    sc(SYS_MSGSND, g_inbox_qid, (long)&req, msgsz, 0, 0, 0);

    /* NOTE: we deliberately do NOT shmdt the OLD mapping. An app that re-reads
     * win->pixels each frame (e.g. the IDE) picks up the new buffer immediately;
     * an app that CACHED the old pointer keeps drawing into the (now-orphaned but
     * still-mapped) old buffer, which the compositor simply ignores -- it shows
     * the new buffer. Detaching here would page-fault such a caching app. The old
     * segment is reclaimed when the process exits; the per-resize leak is bounded
     * and a far better tradeoff than crashing a client.) */
}

void wl_commit(wl_window *win) {
    if (!win || win->win_id < 0) return;

    wl_commit_req req;
    req.mtype  = WL_REQ_COMMIT;
    req.win_id = win->win_id;
    req.x = 0;
    req.y = 0;
    req.w = win->w;            /* full-surface damage */
    req.h = win->h;

    long msgsz = (long)(sizeof(req) - sizeof(long));
    sc(SYS_MSGSND, g_inbox_qid, (long)&req, msgsz, 0, 0, 0);
}

int wl_poll_event(wl_window *win, int *kind, int *a, int *b, int *c) {
    (void)win;
    if (g_reply_qid < 0) return 0;

    /* Receive the first pending event of ANY type (type 0), non-blocking.
       The largest event payload determines the receive buffer size. */
    union {
        long             mtype;
        wl_pointer_evt   ptr;
        wl_key_evt       key;
        wl_configure_evt cfg;
    } ev;

    long want = (long)(sizeof(ev) - sizeof(long));
    long rr = sc(SYS_MSGRCV, g_reply_qid, (long)&ev, want, 0, IPC_NOWAIT, 0);
    if (rr < 0) return 0;       /* ENOMSG / nothing pending */

    if (ev.mtype == WL_EVT_CONFIGURE) {
        /* Compositor wants us at a new size: reallocate the buffer, then report
         * WL_EVENT_RESIZE so the app can invalidate any cached geometry. */
        wl_resize_buffer(ev.cfg.w, ev.cfg.h);
        if (kind) *kind = WL_EVENT_RESIZE;
        if (a) *a = (int)g_window.w;
        if (b) *b = (int)g_window.h;
        if (c) *c = 0;
        return 1;
    }

    if (ev.mtype == WL_EVT_POINTER) {
        if (kind) *kind = WL_EVENT_POINTER;
        if (a) *a = ev.ptr.x;
        if (b) *b = ev.ptr.y;
        if (c) *c = ev.ptr.buttons;
        /* Encode wheel in high bits of 'c' for backward compatibility:
         * bits 0-15: buttons, bits 16-31: signed wheel delta */
        if (c && ev.ptr.wheel != 0) {
            int wheel_packed = (ev.ptr.wheel & 0xFFFF) << 16;
            *c = (*c & 0xFFFF) | wheel_packed;
        }
        return 1;
    }
    if (ev.mtype == WL_EVT_KEY) {
        if (kind) *kind = WL_EVENT_KEY;
        if (a) *a = ev.key.keycode;
        if (b) *b = ev.key.pressed;
        if (c) *c = 0;
        return 1;
    }

    /* Unknown event type (e.g. a stray WL_EVT_CREATED): consumed, report none. */
    return 0;
}
