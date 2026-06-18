/*
 * synthinput.h -- Synthetic input injection seam (SYNTHINPUT-0).
 * =============================================================
 *
 * A tiny shared-memory page that lets a TRUSTED agent (e.g. the Nemotron
 * OS-automation agent) inject synthetic mouse + keyboard events into the
 * compositor, so it can drive any GUI app exactly as a human hand would. The
 * compositor owns the global cursor / button state / keyboard focus, and there
 * is no wl message for "some other process moved the pointer", so this
 * well-known SysV SHM page is the seam (same pattern as the DOCK-DND drag
 * handoff and the SELFHEAL heartbeat).
 *
 * Ownership: the COMPOSITOR creates + zeroes the page at boot (it is the
 * consumer and always running) at mode 0600 -- it owns the page and is the
 * sole reader. An agent tool attaches LOOKUP-ONLY (shmget(KEY, SIZE, 0) --
 * NO IPC_CREAT) and only ever writes events into the ring; it never creates,
 * resizes, or owns the page.
 *
 * The queue is a classic single-producer / single-consumer ring: the agent
 * tool is the only producer (it advances `head`), the compositor's input pump
 * is the only consumer (it advances `tail`). With exactly one producer and one
 * consumer, the ring needs NO locking -- correctness depends only on the two
 * cursors being read/written atomically and in order, so BOTH `head` and
 * `tail` must be accessed through a volatile pointer (the compositor must not
 * cache `head`, the agent must not cache `tail`). An event is present iff
 * head != tail; the producer must not advance `head` into `tail` (a full ring
 * drops the newest event rather than corrupting an unread one). Both cursors
 * are taken mod SYNTHINPUT_QMAX when indexing `q[]`.
 *
 * The `magic` field is the publish guard: the compositor zeroes the whole page
 * FIRST and writes SYNTHINPUT_MAGIC LAST, so an agent tool that races the
 * compositor's init sees either an all-zero page (magic mismatch -> not ready)
 * or a fully initialised one, never a half-built ring.
 *
 * The `active` field is the visible "an agent is controlling input" flag AND
 * the hard-stop: injection is only authorised while active == 1. The
 * compositor sets it when it grants control and clears it to revoke (a kill
 * switch); an agent tool should treat active == 0 as "stop, I no longer have
 * the input rail" and the compositor should ignore / drain queued events when
 * active == 0.
 */
#ifndef SYNTHINPUT_H
#define SYNTHINPUT_H

#define SYNTHINPUT_SHM_KEY  0x53594E49u   /* 'SYNI'                                */
#define SYNTHINPUT_SHM_SIZE 4096u          /* one page                              */
#define SYNTHINPUT_MAGIC    0x534E4901u   /* 'SNI\x01' -- compositor publishes LAST */
#define SYNTHINPUT_QMAX     64            /* ring depth (power of two)              */

/* clean self-contained encoding (NOT raw /dev/input): the compositor's pump maps these
 * to its cursor/button/focus updates. */
#define SI_EV_REL     1      /* relative pointer move / wheel: code = SI_REL_*, value = delta   */
#define SI_EV_KEY     0      /* button or key: code = SI_BTN_* or a keycode, value = 1 press / 0 release */
#define SI_REL_X      0
#define SI_REL_Y      1
#define SI_REL_WHEEL  8
#define SI_BTN_LEFT   0x110
#define SI_BTN_RIGHT  0x111
#define SI_BTN_MIDDLE 0x112

typedef struct {
    unsigned int magic;   /* SYNTHINPUT_MAGIC once the compositor has initialised the page   */
    unsigned int head;    /* producer (agent tool) write cursor, taken mod SYNTHINPUT_QMAX   */
    unsigned int tail;    /* consumer (compositor) read cursor, taken mod SYNTHINPUT_QMAX    */
    unsigned int active;  /* 1 = injection currently authorised (agent controlling input)    */
    struct { unsigned short type; unsigned short code; int value; } q[SYNTHINPUT_QMAX];
} synthinput_shm_t;

#endif /* SYNTHINPUT_H */
