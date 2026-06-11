/*
 * wl_client.h -- M3 "Wayland-lite" client library (freestanding, ring 3).
 * =======================================================================
 *
 * A tiny client-side library over the kernel SysV IPC (shm + message queues)
 * implementing the protocol declared in userspace/include/wl_proto.h.
 *
 * Usage sketch:
 *   if (wl_connect() != 0) { ... fatal ... }
 *   wl_window *w = wl_create_window(400, 260, "title");
 *   for (;;) {
 *       // draw into w->pixels (ARGB32, w->stride bytes per row)
 *       wl_commit(w);
 *       int kind, a, b, c;
 *       while (wl_poll_event(w, &kind, &a, &b, &c)) { ... handle input ... }
 *   }
 *
 * No libc: pure inline syscalls. Build freestanding alongside the app TU,
 * e.g. wltest.c + wl_client.c.
 */

#ifndef WL_CLIENT_H
#define WL_CLIENT_H

/* Minimal fixed-width type the freestanding build relies on. */
typedef unsigned int wl_u32;

/*
 * A connected, created window. `pixels` is the shmat'd ARGB32 buffer the
 * client draws into directly; `stride` is bytes-per-row (== w * 4).
 */
typedef struct {
    int      win_id;     /* compositor-assigned window id (from WL_EVT_CREATED) */
    int      shm_id;     /* SysV shm segment id backing `pixels`                */
    wl_u32   w, h;       /* size in pixels                                      */
    wl_u32   stride;     /* bytes per row (w * 4)                               */
    wl_u32  *pixels;     /* mapped ARGB32 framebuffer (zero-copy with server)   */
} wl_window;

/* Event kinds reported by wl_poll_event() via *kind (mirror WL_EVT_*). */
#define WL_EVENT_POINTER 2   /* a=x, b=y, c=buttons   */
#define WL_EVENT_KEY     3   /* a=keycode, b=pressed  */
#define WL_EVENT_RESIZE  4   /* a=new_w, b=new_h; win->{w,h,stride,pixels} ALREADY
                              * updated by the library (it reallocated the buffer).
                              * Apps that re-read win->w/h each frame need no action;
                              * apps caching geometry should invalidate it here. */

/*
 * Connect to the compositor.
 *   - opens (creating if needed) the shared compositor inbox queue,
 *   - creates this process's own reply/event queue (WL_REPLY_KEY(getpid())).
 * Returns 0 on success, negative on failure.
 */
int wl_connect(void);

/*
 * Create a window: allocate+map a shm pixel buffer, send WL_REQ_CREATE and
 * block for WL_EVT_CREATED to learn the assigned win_id.
 * Returns a pointer to a static wl_window on success, or 0 (NULL) on failure.
 */
wl_window *wl_create_window(wl_u32 w, wl_u32 h, const char *title);

/*
 * Destroy the window: send WL_REQ_DESTROY to the compositor, detach the SHM
 * pixel buffer. After this call the win pointer is invalid for drawing.
 * If the client crashes without calling this, the compositor's
 * reap_dead_windows() will clean up on the next liveness sweep.
 */
void wl_destroy(wl_window *win);

/* Commit the current buffer contents (full-surface damage) as a new frame. */
void wl_commit(wl_window *win);

/* Commit a partial damage rect: only the region (x,y,w,h) is sent to the
 * compositor. Coordinates are clamped to the surface bounds; a zero-area or
 * fully-out-of-bounds rect is silently ignored. Use this instead of wl_commit()
 * when only a small portion of the surface changed (e.g. a cursor blink) to
 * reduce compositor recomposite cost. */
void wl_commit_damage(wl_window *win, unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h);

/*
 * Non-blocking event poll.
 *   *kind <- WL_EVENT_POINTER or WL_EVENT_KEY
 *   pointer: *a=x, *b=y, *c=buttons
 *   key:     *a=keycode, *b=pressed, *c=0
 * Returns 1 if an event was delivered, 0 if none pending.
 */
int wl_poll_event(wl_window *win, int *kind, int *a, int *b, int *c);

#endif /* WL_CLIENT_H */
