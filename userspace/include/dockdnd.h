/*
 * dockdnd.h -- Start-menu -> dock drag handoff (DOCK-DND-1).
 * =========================================================
 *
 * A tiny shared-memory page that lets a separate app (the Start menu) hand a
 * drag-in-progress to the compositor, which owns the dock + the global cursor.
 * The wl protocol has no app->compositor drag message, so this well-known SysV
 * SHM page is the seam (same pattern as the SELFHEAL heartbeat).
 *
 * Ownership: the COMPOSITOR creates + zeroes the page at boot (it is the
 * consumer and always running). The Start menu attaches LOOKUP-ONLY
 * (shmget(KEY, SIZE, 0) -- NO IPC_CREAT) and writes a pending drag when the
 * user drags a tile out toward the dock; it then exits. The compositor renders
 * the drag ghost following the cursor and, on release over the dock strip,
 * pins the app into the dock's "Pinned" box.
 *
 * Single producer (the active Start menu) + single consumer (the compositor),
 * one in-flight drag at a time -> no locking needed beyond the `active`/
 * `claimed` flags.
 */
#ifndef DOCKDND_H
#define DOCKDND_H

#define DOCKDND_SHM_KEY   0x444E4401u   /* 'DND' + 1                            */
#define DOCKDND_SHM_SIZE  4096u         /* one page                             */
#define DOCKDND_MAGIC     0xD0CCD0CCu   /* set once the compositor inits it     */

typedef struct {
    unsigned int magic;       /* DOCKDND_MAGIC once the compositor initialised   */
    int          active;      /* 1 = a Start-menu drag is in flight              */
    int          claimed;     /* compositor sets 1 when it consumes the drop     */
    unsigned int color;       /* tile color of the dragged app (ARGB)            */
    char         path[64];    /* "sbin/xxx" spawn path being dragged             */
    char         label[8];    /* 2-char icon label                              */
} dockdnd_shm_t;

#endif /* DOCKDND_H */
