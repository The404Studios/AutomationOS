/*
 * wl_proto.h -- M3 "Wayland-lite" client <-> compositor protocol.
 * ===============================================================
 *
 * Transport: kernel System V IPC.
 *   - One shared compositor command queue (WL_COMP_INBOX_KEY).
 *   - One per-client event queue (WL_REPLY_KEY(pid)).
 *   - Window pixel buffers are SysV shared memory segments (ARGB32).
 *
 * Message convention (matches kernel/ipc/msgqueue.c):
 *   - The first field of every message struct is `long mtype` (8 bytes on
 *     x86_64). The kernel copies that header separately; `msgsz` passed to
 *     msgsnd/msgrcv is the payload size AFTER mtype, i.e.
 *     sizeof(struct) - sizeof(long).
 *   - mtype carries the message id (WL_REQ_* / WL_EVT_*), which doubles as
 *     the msgrcv type filter.
 *
 * Buffer model:
 *   client: shm_id = shmget(IPC_PRIVATE, w*h*4, IPC_CREAT|0666);
 *           pixels = shmat(shm_id, 0, 0);   // ARGB32, stride = w*4
 *   client draws into pixels, then sends WL_REQ_CREATE{pid,shm_id,w,h,stride,
 *   title}; the compositor shmat's the SAME shm_id for zero-copy access.
 *   WL_REQ_COMMIT signals a new frame plus a damage rectangle.
 *
 * This header is the single source of truth shared with the sibling
 * compositor server agent -- both sides MUST compile against it verbatim.
 */

#ifndef WL_PROTO_H
#define WL_PROTO_H

#define WL_COMP_INBOX_KEY 0x434F4D50            /* compositor command queue */
#define WL_REPLY_KEY(pid) (0x52000000 + (pid))  /* per-client event queue   */

/* client->compositor (to WL_COMP_INBOX_KEY), mtype = id */
#define WL_REQ_CREATE  1
#define WL_REQ_COMMIT  2
#define WL_REQ_DESTROY 3
#define WL_REQ_RESIZE  4   /* client reallocated its buffer; carries new shm_id  */

/* compositor->client (to WL_REPLY_KEY(pid)), mtype = id */
#define WL_EVT_CREATED 1
#define WL_EVT_POINTER 2
#define WL_EVT_KEY     3
#define WL_EVT_CONFIGURE 4 /* compositor asks the client to resize to w x h       */

typedef struct { long mtype; int pid; int shm_id; unsigned int w,h,stride; char title[48]; } wl_create_req;
typedef struct { long mtype; int win_id; unsigned int x,y,w,h; } wl_commit_req;   /* damage */
typedef struct { long mtype; int win_id; } wl_destroy_req;
/* Client -> compositor after it reallocs a bigger/smaller pixel buffer in
 * response to WL_EVT_CONFIGURE. win_id stays stable; the compositor re-attaches
 * the new shm_id and updates the window's immutable buffer extent. */
typedef struct { long mtype; int win_id; int shm_id; unsigned int w,h,stride; } wl_resize_req;
typedef struct { long mtype; int win_id; } wl_created_evt;
typedef struct { long mtype; int x,y,buttons,wheel; } wl_pointer_evt;
typedef struct { long mtype; int keycode,pressed; } wl_key_evt;
/* Compositor -> client: "resize your surface to w x h" (e.g. on maximize). */
typedef struct { long mtype; int win_id; unsigned int w,h; } wl_configure_evt;

#endif /* WL_PROTO_H */
