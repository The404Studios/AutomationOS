/*
 * dom_event.c -- AutomationOS DOM Event / EventTarget infrastructure.
 * ===================================================================
 *
 * See dom_event.h for the full design contract.
 *
 * Listener storage strategy
 * -------------------------
 * dom_node->user is owned by the layout/render layer and MUST NOT be
 * hijacked.  We therefore maintain a side data structure:
 *
 *   g_listener_table[DOM_EVENT_HASH_SIZE]
 *
 * Each entry is the head of a singly-linked list of node_listener_head_t.
 * Each node_listener_head_t contains:
 *   - the dom_node pointer it belongs to
 *   - a singly-linked list of listener_record_t (the actual callbacks)
 *
 * Bucket index = ((uintptr_t)node_ptr >> 4) % DOM_EVENT_HASH_SIZE
 * (>> 4 because nodes are typically 16-byte aligned; this spreads bits.)
 *
 * Dispatch ancestor stack
 * -----------------------
 * dom_dispatch_event() builds an ancestor chain (target … root) in a
 * fixed-size stack-local array of up to DOM_EVENT_MAX_DEPTH pointers.
 * No heap allocation occurs during dispatch.
 *
 * Build flags (no fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2
 */

#include "dom_event.h"
#include "../../libc/malloc.h"
#include "../../libc/string.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ================================================================== */
/*  Compile-time parameters                                           */
/* ================================================================== */

/* Hash table size (must be a power of two for the mask trick).       */
#define DOM_EVENT_HASH_SIZE   256u
#define DOM_EVENT_HASH_MASK   (DOM_EVENT_HASH_SIZE - 1u)

/* Maximum ancestor depth considered during dispatch.                  */
#define DOM_EVENT_MAX_DEPTH   64u

/* ================================================================== */
/*  Internal types                                                     */
/* ================================================================== */

/* One registered listener. */
typedef struct listener_record {
    char                    type[32]; /* event type string                 */
    dom_event_listener      fn;       /* callback                          */
    void                   *user;     /* forwarded to fn                   */
    int                     capture;  /* 1=capture phase, 0=bubble phase   */
    struct listener_record *next;
} listener_record_t;

/* Per-node bucket entry: groups all listener_record_t for one node.   */
typedef struct node_listener_head {
    struct dom_node            *node;     /* key                           */
    listener_record_t          *records;  /* linked list of listeners       */
    struct node_listener_head  *next;     /* next entry in hash bucket      */
} node_listener_head_t;

/* ================================================================== */
/*  Global hash table (zero-initialised by BSS)                       */
/* ================================================================== */
static node_listener_head_t *g_listener_table[DOM_EVENT_HASH_SIZE];

/* ================================================================== */
/*  Hash / lookup helpers                                             */
/* ================================================================== */

/* Compute bucket index from node pointer. */
static unsigned int node_hash(const struct dom_node *node)
{
    unsigned long v = (unsigned long)(unsigned long long)(void *)node;
    return (unsigned int)((v >> 4u) & DOM_EVENT_HASH_MASK);
}

/* Find the node_listener_head_t for `node`, or NULL if absent. */
static node_listener_head_t *find_head(const struct dom_node *node)
{
    if (!node) return NULL;
    unsigned int bucket = node_hash(node);
    node_listener_head_t *h = g_listener_table[bucket];
    while (h) {
        if (h->node == node) return h;
        h = h->next;
    }
    return NULL;
}

/*
 * find_or_create_head -- return (creating if necessary) the
 * node_listener_head_t for `node`.  Returns NULL on OOM.
 */
static node_listener_head_t *find_or_create_head(struct dom_node *node)
{
    node_listener_head_t *h = find_head(node);
    if (h) return h;

    h = (node_listener_head_t *)calloc(1, sizeof(node_listener_head_t));
    if (!h) return NULL;
    h->node = node;
    h->records = NULL;

    unsigned int bucket = node_hash(node);
    h->next = g_listener_table[bucket];
    g_listener_table[bucket] = h;
    return h;
}

/*
 * remove_head -- unlink and free a node_listener_head_t (and all its
 * records) from the hash table.  Assumes records list is already empty
 * or we want to free everything.
 */
static void remove_head(struct dom_node *node)
{
    if (!node) return;
    unsigned int bucket = node_hash(node);
    node_listener_head_t *prev = NULL;
    node_listener_head_t *h    = g_listener_table[bucket];
    while (h) {
        if (h->node == node) {
            /* Free all remaining records. */
            listener_record_t *r = h->records;
            while (r) {
                listener_record_t *rn = r->next;
                free(r);
                r = rn;
            }
            /* Unlink from bucket chain. */
            if (prev) prev->next = h->next;
            else       g_listener_table[bucket] = h->next;
            free(h);
            return;
        }
        prev = h;
        h    = h->next;
    }
}

/* ================================================================== */
/*  Safe string helpers (no external dependencies beyond string.h)    */
/* ================================================================== */

/* strncpy-based copy into a fixed-size buffer, always NUL-terminates. */
static void safe_copy(char *dst, const char *src, unsigned long dsz)
{
    if (!dst || dsz == 0) return;
    if (!src) { dst[0] = 0; return; }
    unsigned long i;
    for (i = 0; i + 1 < dsz && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ================================================================== */
/*  Public API: add / remove                                          */
/* ================================================================== */

int dom_add_event_listener(struct dom_node *target,
                           const char      *type,
                           dom_event_listener fn,
                           void            *user,
                           int              capture)
{
    if (!target || !type || !fn) return -1;

    node_listener_head_t *head = find_or_create_head(target);
    if (!head) return -2;

    listener_record_t *r = (listener_record_t *)calloc(1, sizeof(listener_record_t));
    if (!r) return -2;

    safe_copy(r->type, type, sizeof(r->type));
    r->fn      = fn;
    r->user    = user;
    r->capture = capture ? 1 : 0;
    r->next    = NULL;

    /* Append to keep registration order. */
    if (!head->records) {
        head->records = r;
    } else {
        listener_record_t *tail = head->records;
        while (tail->next) tail = tail->next;
        tail->next = r;
    }
    return 0;
}

int dom_remove_event_listener(struct dom_node *target,
                              const char      *type,
                              dom_event_listener fn,
                              void            *user,
                              int              capture)
{
    if (!target || !type || !fn) return -1;

    node_listener_head_t *head = find_head(target);
    if (!head) return -1;

    int cap = capture ? 1 : 0;
    listener_record_t *prev = NULL;
    listener_record_t *r    = head->records;
    while (r) {
        if (r->fn == fn &&
            r->user == user &&
            r->capture == cap &&
            strcmp(r->type, type) == 0)
        {
            if (prev) prev->next = r->next;
            else      head->records = r->next;
            free(r);
            /* If no records left, clean up the head entry. */
            if (!head->records) remove_head(target);
            return 0;
        }
        prev = r;
        r    = r->next;
    }
    return -1; /* not found */
}

/* ================================================================== */
/*  Dispatch helpers                                                  */
/* ================================================================== */

/*
 * call_listeners_on_node -- invoke all matching listeners for one node
 * during a specific phase.
 *
 * At the TARGET node (phase == DOM_EVENT_PHASE_TARGET) both capture and
 * non-capture listeners run (W3C spec §3.1).  For all other phases only
 * the matching capture flag fires.
 *
 * Returns the number of listeners called.
 * Respects ev->stop_propagation (stops after the current node's list
 * only if stopImmediatePropagation were implemented; here we honour
 * stop_propagation between nodes, not within).
 */
static int call_listeners_on_node(struct dom_node *node,
                                  dom_event_t     *ev,
                                  int              phase)
{
    node_listener_head_t *head = find_head(node);
    if (!head) return 0;

    ev->current = node;
    ev->phase   = phase;

    int called = 0;
    listener_record_t *r = head->records;
    while (r) {
        /* Capture the next pointer before calling in case the listener
           calls remove_event_listener on itself.                      */
        listener_record_t *rnext = r->next;

        int type_match = (strcmp(r->type, ev->type) == 0);
        int phase_match;

        if (phase == DOM_EVENT_PHASE_TARGET) {
            /* Both capture and bubble listeners fire at target. */
            phase_match = 1;
        } else if (phase == DOM_EVENT_PHASE_CAPTURE) {
            phase_match = (r->capture != 0);
        } else { /* BUBBLE */
            phase_match = (r->capture == 0);
        }

        if (type_match && phase_match) {
            r->fn(ev, r->user);
            called++;
        }
        r = rnext;
    }
    return called;
}

/* ================================================================== */
/*  Public API: dispatch                                              */
/* ================================================================== */

int dom_dispatch_event_full(struct dom_node *target, dom_event_t *ev)
{
    if (!target || !ev) return 0;

    ev->target              = target;
    ev->current             = NULL;
    ev->phase               = 0;
    ev->stop_propagation    = 0;
    /* NOTE: default_prevented is NOT reset here; the caller controls it. */

    /* --------------------------------------------------------------- */
    /* Build ancestor chain: ancestors[0] = root, ...,
       ancestors[depth-1] = target->parent.
       (target itself is handled separately as the TARGET phase.)      */
    /* --------------------------------------------------------------- */
    struct dom_node *ancestors[DOM_EVENT_MAX_DEPTH];
    unsigned int depth = 0;

    {
        struct dom_node *n = target->parent;
        /* Walk up to root, collect in reverse. */
        unsigned int tmp_depth = 0;
        struct dom_node *tmp_buf[DOM_EVENT_MAX_DEPTH];
        while (n && tmp_depth < DOM_EVENT_MAX_DEPTH) {
            tmp_buf[tmp_depth++] = n;
            n = n->parent;
        }
        /* Reverse: tmp_buf[0]=parent, tmp_buf[tmp_depth-1]=root.
           ancestors[0] should be root (top of capture walk). */
        for (unsigned int i = 0; i < tmp_depth; i++) {
            ancestors[i] = tmp_buf[tmp_depth - 1 - i];
        }
        depth = tmp_depth;
    }

    int any = 0;

    /* --------------------------------------------------------------- */
    /* Phase 1: CAPTURE  (root → parent of target)                    */
    /* --------------------------------------------------------------- */
    for (unsigned int i = 0; i < depth && !ev->stop_propagation; i++) {
        any += call_listeners_on_node(ancestors[i], ev, DOM_EVENT_PHASE_CAPTURE);
    }

    /* --------------------------------------------------------------- */
    /* Phase 2: TARGET                                                 */
    /* --------------------------------------------------------------- */
    if (!ev->stop_propagation) {
        any += call_listeners_on_node(target, ev, DOM_EVENT_PHASE_TARGET);
    }

    /* --------------------------------------------------------------- */
    /* Phase 3: BUBBLE  (parent of target → root)                     */
    /* --------------------------------------------------------------- */
    if (!ev->stop_propagation) {
        /* Walk ancestors in reverse (parent-first up to root). */
        unsigned int i = depth;
        while (i > 0 && !ev->stop_propagation) {
            i--;
            any += call_listeners_on_node(ancestors[i], ev, DOM_EVENT_PHASE_BUBBLE);
        }
    }

    ev->current = NULL;
    return (any > 0) ? 1 : 0;
}

int dom_dispatch_event(struct dom_node *target,
                       const char      *type,
                       void            *event_data)
{
    if (!target || !type) return 0;

    /* Stack-allocate the event object — no heap allocation during dispatch. */
    dom_event_t ev;
    memset(&ev, 0, sizeof(ev));
    safe_copy(ev.type, type, sizeof(ev.type));
    ev.data              = event_data;
    ev.default_prevented = 0;
    ev.stop_propagation  = 0;

    return dom_dispatch_event_full(target, &ev);
}

/* ================================================================== */
/*  Housekeeping                                                      */
/* ================================================================== */

void dom_event_purge_node(struct dom_node *node)
{
    if (!node) return;
    remove_head(node);
}

/* ================================================================== */
/*  Self-test helpers (file scope so callbacks can reference them)   */
/* ================================================================== */

/* Minimal write syscall shim so the selftest can report without stdio. */
static void ev_write(const char *s)
{
    if (!s) return;
    unsigned long n = strlen(s);
    if (n == 0) return;
    /* AutomationOS SYS_WRITE = 3 -- NOT Linux's 1. On AOS syscall 1 is SYS_FORK,
     * so the old "a"(1UL) FORKED instead of writing: dom_selftest reaches this
     * shim once the heisenbug fix lets it run to completion, spawning the
     * "domtest-forked" child that then faults. rax must be 3. */
    __asm__ volatile (
        "syscall"
        :
        : "D"(1UL),   /* rdi = fd 1 (stdout)   */
          "S"(s),     /* rsi = buf             */
          "d"(n),     /* rdx = len             */
          "a"(3UL)    /* rax = SYS_WRITE (AOS) */
        : "rcx", "r11", "memory"
    );
}

/* Listener callback: increments an int counter. */
static void count_cb(dom_event_t *ev, void *user)
{
    (void)ev;
    int *ctr = (int *)user;
    if (ctr) (*ctr)++;
}

/* Listener callback: records count + last phase seen. */
typedef struct { int count; int last_phase; } phase_rec_t;

static void phase_cb(dom_event_t *ev, void *user)
{
    phase_rec_t *r = (phase_rec_t *)user;
    if (r) { r->count++; r->last_phase = ev->phase; }
}

/* Counter for the stop-propagation sub-test (file-scope static). */
static int s_stop_called = 0;

/* Listener that sets stop_propagation and increments s_stop_called. */
static void stop_prop_cb(dom_event_t *ev, void *user)
{
    (void)user;
    ev->stop_propagation = 1;
    s_stop_called++;
}

/* Listener that, while bubbling, records its order tag then calls
 * stopPropagation -- modelling exactly what the JS-side e.stopPropagation()
 * does (see dom_bindings.c m_stopPropagation). `user` is an int* order
 * counter shared with bubble_tag_cb. */
typedef struct { int *order; int seen_at; } bubble_rec_t;

static void bubble_tag_cb(dom_event_t *ev, void *user)
{
    (void)ev;
    bubble_rec_t *r = (bubble_rec_t *)user;
    if (r && r->order) r->seen_at = ++(*r->order);
}

static void bubble_stop_cb(dom_event_t *ev, void *user)
{
    bubble_rec_t *r = (bubble_rec_t *)user;
    if (r && r->order) r->seen_at = ++(*r->order);
    ev->stop_propagation  = 1;   /* halt the bubble chain here            */
    ev->default_prevented = 1;   /* and cancel the default action         */
}

/* ================================================================== */
/*  Self-test                                                         */
/* ================================================================== */

int dom_event_selftest(void)
{
    int failures = 0;

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            ev_write("SELFTEST FAIL: " #cond "\n"); \
            failures++; \
        } \
    } while (0)

    /* ---------------------------------------------------------------- */
    /* Build tiny DOM:   root → mid → leaf                             */
    /* ---------------------------------------------------------------- */
    dom_node *root = dom_create_element("root");
    dom_node *mid  = dom_create_element("mid");
    dom_node *leaf = dom_create_element("leaf");
    ASSERT(root != NULL);
    ASSERT(mid  != NULL);
    ASSERT(leaf != NULL);
    if (!root || !mid || !leaf) { failures++; goto done; }

    dom_append_child(root, mid);
    dom_append_child(mid,  leaf);

    /* ---------------------------------------------------------------- */
    /* Attach listeners:                                                 */
    /*   A  root  capture  (phase==CAPTURE expected)                    */
    /*   B  leaf  bubble   (phase==TARGET  expected — at-target node)   */
    /*   C  mid   bubble   (phase==BUBBLE  expected)                    */
    /* ---------------------------------------------------------------- */
    phase_rec_t A = {0, 0};
    phase_rec_t B = {0, 0};
    phase_rec_t C = {0, 0};

    ASSERT(dom_add_event_listener(root, "click", phase_cb, &A, 1) == 0);
    ASSERT(dom_add_event_listener(leaf, "click", phase_cb, &B, 0) == 0);
    ASSERT(dom_add_event_listener(mid,  "click", phase_cb, &C, 0) == 0);

    /* ---------------------------------------------------------------- */
    /* First dispatch: "click" on leaf                                  */
    /* ---------------------------------------------------------------- */
    int dispatched = dom_dispatch_event(leaf, "click", NULL);
    ASSERT(dispatched == 1);

    ASSERT(A.count      == 1);
    ASSERT(A.last_phase == DOM_EVENT_PHASE_CAPTURE);

    ASSERT(B.count      == 1);
    ASSERT(B.last_phase == DOM_EVENT_PHASE_TARGET);

    ASSERT(C.count      == 1);
    ASSERT(C.last_phase == DOM_EVENT_PHASE_BUBBLE);

    /* ---------------------------------------------------------------- */
    /* Remove leaf's listener; re-dispatch; B must NOT increment        */
    /* ---------------------------------------------------------------- */
    ASSERT(dom_remove_event_listener(leaf, "click", phase_cb, &B, 0) == 0);

    dispatched = dom_dispatch_event(leaf, "click", NULL);
    ASSERT(dispatched == 1);   /* A and C still fire */

    ASSERT(A.count == 2);      /* root capture fired again */
    ASSERT(B.count == 1);      /* leaf listener removed — no change */
    ASSERT(C.count == 2);      /* mid bubble fired again */

    /* ---------------------------------------------------------------- */
    /* stop_propagation: mid capture listener halts walk                 */
    /* root capture (A) fires; then mid capture fires + stops;          */
    /* leaf target (B) and mid bubble (C) must NOT fire.                */
    /* ---------------------------------------------------------------- */

    /* Re-attach leaf listener so we can confirm it does NOT fire. */
    ASSERT(dom_add_event_listener(leaf, "click", phase_cb, &B, 0) == 0);

    s_stop_called = 0;
    ASSERT(dom_add_event_listener(mid, "click", stop_prop_cb, NULL, 1) == 0);

    int A_before = A.count;
    int B_before = B.count;
    int C_before = C.count;

    dom_dispatch_event(leaf, "click", NULL);

    ASSERT(A.count == A_before + 1);  /* root capture fires before mid */
    ASSERT(s_stop_called == 1);       /* mid capture fired + stopped   */
    ASSERT(B.count == B_before);      /* leaf target did NOT fire      */
    ASSERT(C.count == C_before);      /* mid bubble did NOT fire       */

    ASSERT(dom_remove_event_listener(mid, "click", stop_prop_cb, NULL, 1) == 0);

    /* ---------------------------------------------------------------- */
    /* Wrong event type: no listeners should fire                        */
    /* ---------------------------------------------------------------- */
    int A2 = A.count, B2 = B.count, C2 = C.count;
    dom_dispatch_event(leaf, "mousemove", NULL);
    ASSERT(A.count == A2 && B.count == B2 && C.count == C2);

    /* ---------------------------------------------------------------- */
    /* dom_dispatch_event_full: confirm caller-supplied ev is used       */
    /* ---------------------------------------------------------------- */
    {
        dom_event_t fullev;
        memset(&fullev, 0, sizeof(fullev));
        safe_copy(fullev.type, "submit", sizeof(fullev.type));
        fullev.data = NULL;
        int pc = 0;
        ASSERT(dom_add_event_listener(leaf, "submit", count_cb, &pc, 0) == 0);
        int ret = dom_dispatch_event_full(leaf, &fullev);
        ASSERT(ret == 1);
        ASSERT(pc == 1);
        ASSERT(dom_remove_event_listener(leaf, "submit", count_cb, &pc, 0) == 0);
    }

    /* ---------------------------------------------------------------- */
    /* Bubble-phase stopPropagation halts the chain (mirrors the JS      */
    /* e.stopPropagation() path). leaf (TARGET) fires + stops; mid's     */
    /* bubble listener must NOT run. Also confirms default_prevented set */
    /* by a handler survives to the caller (dispatchEvent returns        */
    /* !default_prevented on the bindings side).                         */
    /* ---------------------------------------------------------------- */
    {
        int order = 0;
        bubble_rec_t leaf_rec = { &order, 0 };
        bubble_rec_t mid_rec  = { &order, 0 };
        /* leaf is the target: a non-capture listener fires in TARGET phase
         * and stops propagation before the bubble walk reaches mid. */
        ASSERT(dom_add_event_listener(leaf, "bubblestop",
                                      bubble_stop_cb, &leaf_rec, 0) == 0);
        ASSERT(dom_add_event_listener(mid, "bubblestop",
                                      bubble_tag_cb, &mid_rec, 0) == 0);

        dom_event_t bev;
        memset(&bev, 0, sizeof(bev));
        safe_copy(bev.type, "bubblestop", sizeof(bev.type));
        int bret = dom_dispatch_event_full(leaf, &bev);

        ASSERT(bret == 1);                  /* at least leaf fired         */
        ASSERT(leaf_rec.seen_at == 1);      /* leaf ran first              */
        ASSERT(mid_rec.seen_at  == 0);      /* mid bubble suppressed       */
        ASSERT(bev.stop_propagation == 1);  /* flag latched                */
        ASSERT(bev.default_prevented == 1); /* preventDefault honoured     */

        ASSERT(dom_remove_event_listener(leaf, "bubblestop",
                                         bubble_stop_cb, &leaf_rec, 0) == 0);
        ASSERT(dom_remove_event_listener(mid, "bubblestop",
                                         bubble_tag_cb, &mid_rec, 0) == 0);
    }

    /* ---------------------------------------------------------------- */
    /* Purge; after purge dispatch returns 0                             */
    /* ---------------------------------------------------------------- */
    dom_event_purge_node(root);
    dom_event_purge_node(mid);
    dom_event_purge_node(leaf);

    ASSERT(dom_dispatch_event(leaf, "click", NULL) == 0);

done:
    if (leaf && leaf->parent) dom_remove_child(mid, leaf);
    if (mid  && mid->parent)  dom_remove_child(root, mid);
    if (leaf) dom_node_free(leaf);
    if (mid)  dom_node_free(mid);
    if (root) dom_node_free(root);

#undef ASSERT

    if (failures == 0) {
        ev_write("dom_event_selftest: PASS\n");
        return 0;
    }
    ev_write("dom_event_selftest: FAIL\n");
    return -failures;
}
