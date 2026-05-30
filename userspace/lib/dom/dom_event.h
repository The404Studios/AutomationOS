/*
 * dom_event.h -- AutomationOS DOM Event / EventTarget infrastructure.
 * ===================================================================
 *
 * Implements a subset of the W3C DOM Events specification:
 *   - Event object (type, target, currentTarget, phase, prevention,
 *     stop-propagation, opaque payload).
 *   - addEventListener / removeEventListener per node, capture or bubble
 *     phase, with opaque user pointer forwarded to each C callback.
 *   - Synchronous three-phase dispatch: capture (root → target),
 *     target, bubble (target → root).
 *
 * Freestanding ring-3 (NO libc/stdio).  Uses only:
 *   - userspace/libc/malloc.h  (malloc/free/calloc)
 *   - userspace/libc/string.h  (strlen/strcmp/strncpy/memset)
 *
 * Listener storage:
 *   dom_node->user is reserved by the layout/render layer.  We therefore
 *   use a side global hash table (DOM_EVENT_HASH_SIZE = 256 buckets) keyed
 *   by node pointer.  Each bucket is a singly-linked list of
 *   node_listener_head_t, each of which owns a singly-linked list of
 *   listener_record_t.  All records are heap-allocated (malloc/free).
 *
 * No malloc per dispatch:  dom_dispatch_event() stack-allocates its
 *   dom_event_t; the ancestor-chain walk uses a fixed-depth stack array
 *   (DOM_EVENT_MAX_DEPTH = 64) also on the call stack.
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2 -c dom_event.c -o dom_event.o
 *
 * Threading: single-threaded.  No locks.
 */
#ifndef DOM_EVENT_H
#define DOM_EVENT_H

#include "dom.h"      /* struct dom_node, dom_node_type */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Event phases (matches W3C spec)                                   */
/* ------------------------------------------------------------------ */
#define DOM_EVENT_PHASE_CAPTURE  1
#define DOM_EVENT_PHASE_TARGET   2
#define DOM_EVENT_PHASE_BUBBLE   3

/* ------------------------------------------------------------------ */
/*  Event object                                                       */
/* ------------------------------------------------------------------ */
typedef struct dom_event {
    char            type[32];   /* e.g. "click", "load", "DOMContentLoaded" */
    struct dom_node *target;    /* the node the event was dispatched to      */
    struct dom_node *current;   /* current node in capture/bubble walk       */
    int              phase;     /* 1=capture  2=target  3=bubble             */
    int              default_prevented;
    int              stop_propagation;
    void            *data;      /* event-specific payload; opaque            */
} dom_event_t;

/* Listener callback signature. */
typedef void (*dom_event_listener)(dom_event_t *ev, void *user);

/* ------------------------------------------------------------------ */
/*  Subscribe / unsubscribe                                            */
/* ------------------------------------------------------------------ */

/*
 * dom_add_event_listener -- attach a C callback for `type` on `target`.
 *
 * `capture` != 0  → listener fires during capture phase (root → target).
 * `capture` == 0  → listener fires during bubble phase  (target → root).
 * At the target node itself both capture and non-capture listeners run
 * during the TARGET phase (in registration order, capture first).
 *
 * `user` is forwarded verbatim to fn.
 *
 * Returns  0   on success.
 * Returns -1   on bad argument.
 * Returns -2   on allocation failure.
 */
int dom_add_event_listener(struct dom_node *target,
                           const char *type,
                           dom_event_listener fn,
                           void *user,
                           int capture);

/*
 * dom_remove_event_listener -- detach a previously-registered callback.
 *
 * The tuple (target, type, fn, user, capture) must match exactly.
 * Returns  0   if a matching record was found and removed.
 * Returns -1   on bad argument or record not found.
 */
int dom_remove_event_listener(struct dom_node *target,
                              const char *type,
                              dom_event_listener fn,
                              void *user,
                              int capture);

/* ------------------------------------------------------------------ */
/*  Dispatch                                                           */
/* ------------------------------------------------------------------ */

/*
 * dom_dispatch_event -- synchronous three-phase dispatch.
 *
 * 1. Capture phase:  walk from the tree root down to `target`,
 *    calling capture listeners at each ancestor (not at target).
 * 2. Target phase:   call both capture and bubble listeners at `target`.
 * 3. Bubble phase:   walk from `target`'s parent back up to root,
 *    calling bubble listeners.
 *
 * `stop_propagation` set by any listener terminates further propagation.
 *
 * Returns 1 if at least one listener was invoked; 0 otherwise.
 *
 * Stack-allocation only: a dom_event_t and a pointer array of up to
 * DOM_EVENT_MAX_DEPTH ancestors are allocated on the call stack.
 * No heap allocation during dispatch.
 */
int dom_dispatch_event(struct dom_node *target,
                       const char *type,
                       void *event_data);

/*
 * dom_dispatch_event_full -- dispatch using a caller-supplied dom_event_t.
 *
 * The caller fills in at minimum ev->type and ev->data; target/current/phase
 * are overwritten during dispatch.  Returns 1 if any listener ran.
 * Also checks ev->default_prevented after all listeners have run.
 */
int dom_dispatch_event_full(struct dom_node *target,
                            dom_event_t *ev);

/* ------------------------------------------------------------------ */
/*  Housekeeping                                                       */
/* ------------------------------------------------------------------ */

/*
 * dom_event_purge_node -- remove ALL listener records for `node`.
 *
 * Call before dom_node_free() if listeners may have been attached.
 * Harmless if no listeners exist.
 */
void dom_event_purge_node(struct dom_node *node);

/* ------------------------------------------------------------------ */
/*  Self-test                                                          */
/* ------------------------------------------------------------------ */

/*
 * dom_event_selftest -- exercise the full event system.
 *
 * Builds a tiny DOM:  root → mid → leaf
 * Attaches:
 *   root  capture listener  (counter A)
 *   leaf  target  listener  (counter B, non-capture)
 *   mid   bubble  listener  (counter C, non-capture)
 *
 * Dispatches "click" on leaf.  Asserts:
 *   A fired with phase == DOM_EVENT_PHASE_CAPTURE
 *   B fired with phase == DOM_EVENT_PHASE_TARGET
 *   C fired with phase == DOM_EVENT_PHASE_BUBBLE
 *
 * Then removes leaf's listener; dispatches again; asserts B no longer fires.
 *
 * Also asserts (mirroring the JS-side Event API in dom_bindings.c):
 *   - a CAPTURE-phase stopPropagation halts the walk before the target;
 *   - a target/bubble-phase stopPropagation halts the bubble chain so an
 *     ancestor's bubble listener does NOT run;
 *   - default_prevented set inside a handler survives to the caller (the
 *     bindings layer returns !default_prevented from dispatchEvent()).
 *
 * Returns 0 on pass, negative on failure.
 */
int dom_event_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* DOM_EVENT_H */
