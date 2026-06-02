#ifndef OWNERSHIP_H
#define OWNERSHIP_H
/* ===========================================================================
 * ownership.h — Explicit object-ownership state model for AutomationOS
 *
 * WHY THIS EXISTS
 *   Refcounting (kref.h) answers exactly ONE question: "is this object still
 *   alive?" It does NOT answer "who is allowed to touch it, and how?" As the
 *   kernel grows CPU jobs, AI inference jobs, network buffers, FS caches, and
 *   graphics buffers, each subsystem would otherwise invent its own ad-hoc
 *   ownership convention ("the NIC owns it until it sets a flag", "the
 *   compositor borrows it but mustn't free it", ...). That divergence is
 *   technical debt that surfaces as use-after-free, double-free, and
 *   cross-CPU data races — the exact bugs SMP bring-up makes lethal.
 *
 *   This header defines ONE shared vocabulary of ownership *states* layered on
 *   top of kref. A refcount tells you the object is alive; the ownership state
 *   tells you whether YOU, right now, on THIS cpu, are allowed to read it,
 *   write it, transfer it, or free it.
 *
 * RELATIONSHIP TO kref.h
 *   - kref answers liveness.       ownership answers access discipline.
 *   - An ownership_t EMBEDS a kref_t. The two are not redundant:
 *       refcount == "how many handles keep it alive"
 *       state    == "what the holder of a handle may legally do"
 *   - SHARED is the ONLY state that is allowed to have refcount > 1. Every
 *     other state is a single-handle state (count meaning is state-specific,
 *     see the per-state notes). This is the central invariant.
 *
 * DESIGN PRINCIPLE (extract, don't invent)
 *   We reuse the kernel's existing primitives verbatim: kref_t for liveness,
 *   spinlock_t for the transition critical section, cpu_id() for owner
 *   attribution, ASSERT/ASSERT_ALWAYS/kernel_panic for violation detection.
 *   Nothing here introduces a new locking or atomics convention.
 * ===========================================================================
 */

#include "types.h"
#include "kref.h"
#include "spinlock.h"
#include "kernel.h"   /* ASSERT, ASSERT_ALWAYS, kernel_panic, kprintf */

/* ---------------------------------------------------------------------------
 * 1. THE STATE TYPE
 *
 * Decision: a small enum stored in a uint8_t field, NOT bit flags.
 *   - The five ownership states are mutually exclusive: an object is in
 *     EXACTLY one state at a time. Bit flags model orthogonal/composable
 *     attributes; an exclusive lifecycle is a state MACHINE, so an enum is the
 *     honest representation. Using flags would invite illegal combinations
 *     (OWNED|SHARED) that the type system should make unrepresentable.
 *   - We DO use a separate bitmask (own_flags below) for the genuinely
 *     orthogonal attributes (e.g. "cleanup deferred", "debug-traced"), which
 *     is what bit flags are actually good at.
 *   - uint8_t keeps ownership_t cache-friendly when embedded in hot objects
 *     (net buffers, tensor job descriptors) of which there may be millions.
 * ------------------------------------------------------------------------- */
typedef enum own_state {
    /* OWNED — exactly one owner cpu/context has exclusive read+write. This is
     * the birth state (own_init). refcount is meaningless beyond "alive == 1";
     * exclusivity is enforced by owner_cpu, not by the count. */
    OWN_OWNED       = 0,

    /* SHARED — multiple READERS, lifetime governed by the refcount. The ONLY
     * state where kref count may exceed 1. No holder may write (writers must
     * first transition the object out of SHARED, or use a copy-on-write path
     * the subsystem layers on top). */
    OWN_SHARED      = 1,

    /* BORROWED — a temporary, non-owning lease. The lender remains the real
     * owner (recorded in owner_cpu); the borrower may read (and write iff the
     * lease was taken mutably) but MUST NOT free, transfer, or re-lend. A
     * borrow is strictly nested within the lender's ownership lifetime. */
    OWN_BORROWED    = 2,

    /* TRANSFERRED — ownership has MOVED to a new owner (new owner_cpu). The
     * old owner's handle is poisoned: any access by the original owner is a
     * violation. This is the move semantic (CPU0 -> CPU1 tensor job,
     * userspace -> AI worker inference job). */
    OWN_TRANSFERRED = 3,

    /* ORPHANED — the owner is gone (exited/crashed/revoked) but the object is
     * not yet reclaimed (e.g. an in-flight DMA must drain, or a borrower still
     * holds a lease). The object is scheduled for cleanup; no NEW owner may be
     * established. Terminal except for the final put that frees it. */
    OWN_ORPHANED    = 4,

    OWN_STATE_COUNT
} own_state_t;

/* Orthogonal attribute flags (the legitimate bit-flag use). Stored separately
 * from the exclusive state so the two concerns never alias. */
#define OWN_FLAG_NONE        0x00u
#define OWN_FLAG_MUTABLE     0x01u  /* a BORROW that permits writes            */
#define OWN_FLAG_DEFER_FREE  0x02u  /* ORPHAN cleanup is queued, not immediate */
#define OWN_FLAG_TRACED      0x04u  /* emit a kprintf trace on each transition */
#define OWN_FLAG_PINNED      0x08u  /* in-flight DMA/HW: may not free even at 0 */

/* ---------------------------------------------------------------------------
 * 2. THE OWNERSHIP DESCRIPTOR
 *
 * Embed BY VALUE in any ownable object, exactly like kref_t is embedded today
 * (mirrors process_t.thread_join_wo / kref_hdr_t). 24 bytes, one cache line
 * share with the object header.
 * ------------------------------------------------------------------------- */
#define OWN_MAGIC      0x4F574E45u   /* "OWNE" — detects ops on a non-owned ptr */
#define OWN_CPU_NONE   0xFFFFFFFFu   /* owner_cpu sentinel: no current owner     */

typedef struct ownership {
    kref_t    ref;          /* liveness — reused verbatim from kref.h          */
    uint32_t  magic;        /* OWN_MAGIC; poisoned to 0xDEAD0WN on final free   */
    uint8_t   state;        /* own_state_t — the exclusive lifecycle state      */
    uint8_t   flags;        /* OWN_FLAG_* orthogonal attributes                 */
    uint8_t   _pad[2];
    uint32_t  owner_cpu;    /* cpu that currently OWNS (or last owned); or NONE */
    uint32_t  borrow_depth; /* nested mutable/shared borrows outstanding        */
    spinlock_t lock;        /* serializes transitions (state+owner are coupled) */
} ownership_t;

/* ---------------------------------------------------------------------------
 * 3. THE LEGAL STATE-TRANSITION GRAPH
 *
 * Encoded as a 2D table so the validator is data, not a tangle of if-trees,
 * and so the graph is auditable at a glance. own_transition() consults this
 * table on every move; an entry of 0 means "illegal — panic/assert".
 *
 *   from \ to     OWNED  SHARED  BORROWED  TRANSFERRED  ORPHANED
 *   OWNED           .      Y        Y           Y           Y
 *   SHARED          Y*     Y        Y           .           Y
 *   BORROWED        Y      .        Y           .           Y**
 *   TRANSFERRED     Y      Y        Y           Y           Y
 *   ORPHANED        .      .        .           .           Y(self)
 *
 *   Y   = always legal.
 *   Y*  = SHARED -> OWNED only when refcount has fallen to 1 (last reader
 *         re-acquires exclusivity). Enforced dynamically in own_transition().
 *   Y** = BORROWED -> ORPHANED only records intent; the lease must END
 *         (own_return) before the object is actually reclaimable. The DEFER
 *         flag is set so the final put doesn't free under a live borrower.
 *   .   = ILLEGAL.
 *
 *   The diagonal self-edges (X->X) are legal no-ops EXCEPT they are how a
 *   SHARED object takes another reader and how TRANSFERRED re-transfers
 *   (chained hand-off CPU0->CPU1->CPU2). OWNED->OWNED is forbidden: you can't
 *   re-own what you already exclusively own (catches double-acquire bugs).
 *
 * KEY INVARIANTS (each is asserted, see section 5):
 *   I1. Exactly-one-state: state is always a valid own_state_t.
 *   I2. SHARED-iff-multi: refcount > 1  =>  state == OWN_SHARED.
 *   I3. No-free-while-borrowed: a 1->0 put with borrow_depth>0 PANICS.
 *   I4. No-access-after-transfer: original owner_cpu touching a TRANSFERRED
 *       object is a violation (own_assert_can_write/read catch it).
 *   I5. Orphan-is-terminal: ORPHANED only transitions to itself; it is never
 *       re-owned, re-shared, or re-borrowed. It only gets freed.
 *   I6. Borrow-nesting: own_return must balance own_borrow; borrow_depth==0
 *       is required before OWNED->TRANSFERRED or before the final free.
 * ------------------------------------------------------------------------- */
static const uint8_t OWN_TRANSITION_TABLE[OWN_STATE_COUNT][OWN_STATE_COUNT] = {
    /*               ->OWNED ->SHARED ->BORROWED ->TRANSFERRED ->ORPHANED */
    /* OWNED       */ {  0,     1,      1,          1,            1 },
    /* SHARED      */ {  1,     1,      1,          0,            1 },
    /* BORROWED    */ {  1,     0,      1,          0,            1 },
    /* TRANSFERRED */ {  1,     1,      1,          1,            1 },
    /* ORPHANED    */ {  0,     0,      0,          0,            1 },
};

static inline int own_transition_is_legal(own_state_t from, own_state_t to) {
    if ((unsigned)from >= OWN_STATE_COUNT || (unsigned)to >= OWN_STATE_COUNT)
        return 0;
    return OWN_TRANSITION_TABLE[from][to] != 0;
}

/* Human-readable state name for panics/traces. */
static inline const char *own_state_name(own_state_t s) {
    switch (s) {
        case OWN_OWNED:       return "OWNED";
        case OWN_SHARED:      return "SHARED";
        case OWN_BORROWED:    return "BORROWED";
        case OWN_TRANSFERRED: return "TRANSFERRED";
        case OWN_ORPHANED:    return "ORPHANED";
        default:              return "<corrupt>";
    }
}

/* ---------------------------------------------------------------------------
 * 4. PUBLIC API  (implemented in kernel/core/mem/ownership.c)
 *
 * Naming mirrors kref's verb style (own_init/own_get/own_put) so the two read
 * as one family. Every mutating call takes the embedded ownership_t*.
 * ------------------------------------------------------------------------- */

/* Birth: state=OWNED, refcount=1, owner_cpu=cpu_id(), borrow_depth=0. */
void own_init(ownership_t *o);

/* Liveness passthrough to kref. own_get is only legal on SHARED (taking
 * another reader) — calling it on any other state is a violation (catches the
 * classic "I refcounted an exclusively-owned object and now two CPUs think
 * they own it" bug). Returns the same pointer for chaining, like kget. */
void own_get(ownership_t *o);

/* Drop one reference. Returns 1 IFF this drove refcount 1->0 AND the object is
 * legally reclaimable (borrow_depth==0, not PINNED). On 1->0 with an
 * outstanding borrow or pin, it transitions the object to ORPHANED+DEFER_FREE
 * and returns 0 — the LAST own_return/own_unpin then completes the free.
 * Mirrors kref_put's "only the caller that hit a reclaimable 0 frees". */
int  own_put(ownership_t *o);

/* The single chokepoint every lifecycle change flows through. Validates the
 * edge against OWN_TRANSITION_TABLE and the dynamic invariants, updates
 * owner_cpu, and (if OWN_FLAG_TRACED) emits a trace. PANICS on an illegal
 * edge in any build (these are memory-safety invariants, not debug niceties).
 * Returns 0 on success. new_owner_cpu is used only for ->TRANSFERRED/->OWNED
 * (pass OWN_CPU_NONE otherwise). */
int  own_transition(ownership_t *o, own_state_t to, uint32_t new_owner_cpu);

/* Convenience wrappers — the vocabulary subsystems actually call. Each is a
 * thin, intent-revealing shim over own_transition + bookkeeping. */
void own_share    (ownership_t *o);                       /* OWNED  -> SHARED   */
void own_transfer (ownership_t *o, uint32_t to_cpu);      /* *      -> TRANSFERRED, new owner = to_cpu */
void own_orphan   (ownership_t *o);                       /* *      -> ORPHANED (owner gone) */

/* Borrow lease: bumps borrow_depth, sets state=BORROWED (recording the lender
 * in owner_cpu so the return can restore it). mutable!=0 sets OWN_FLAG_MUTABLE.
 * A borrow does NOT take a refcount by default — the lender's lifetime already
 * dominates the lease (I6); pass take_ref=1 only for async borrows that may
 * outlive the lender's stack frame. */
void own_borrow(ownership_t *o, int mutable, int take_ref);

/* End a lease: decrements borrow_depth. When depth hits 0, restores the prior
 * state (OWNED, or completes a deferred ORPHAN free). Balances own_borrow. */
void own_return(ownership_t *o);

/* HW pin/unpin for in-flight DMA: while PINNED, own_put never frees even at
 * refcount 0 (it ORPHAN+DEFERs). own_unpin completes a deferred free. This is
 * the "a bus stall is not a software spin" discipline applied to memory: HW
 * may still be reading the buffer after the last software ref drops. */
void own_pin(ownership_t *o);
void own_unpin(ownership_t *o);

/* ---------------------------------------------------------------------------
 * 5. DEBUG / VIOLATION DETECTION
 *
 * Three layers, cheapest first:
 *   (a) Always-on, zero-cost-in-release structural guards (magic + state range)
 *       that protect memory safety — these PANIC in every build.
 *   (b) DEBUG-only access assertions that catch logic violations (read after
 *       transfer, write while shared) — compiled out in release via ASSERT.
 *   (c) Optional per-object tracing (OWN_FLAG_TRACED) for live debugging of a
 *       suspect object without recompiling.
 * ------------------------------------------------------------------------- */

/* Structural validity — call at the top of every op. Always-on: a bad magic or
 * out-of-range state means memory corruption, which is never safe to ignore. */
static inline void own_validate(const ownership_t *o) {
    ASSERT_ALWAYS(o != NULL);
    ASSERT_ALWAYS(o->magic == OWN_MAGIC);          /* not an ownership obj / UAF */
    ASSERT_ALWAYS(o->state < OWN_STATE_COUNT);     /* corrupted state byte       */
    /* I2: the SHARED-iff-multi invariant, checked structurally. */
    ASSERT_ALWAYS(!(kref_read(&o->ref) > 1) || o->state == OWN_SHARED);
}

/* Access-discipline guards (DEBUG builds). Subsystems sprinkle these at the
 * point of actual access so a violation is caught at the offending CPU, not
 * three handoffs later. cur_cpu is the caller's cpu_id(). */
static inline void own_assert_can_read(const ownership_t *o, uint32_t cur_cpu) {
    own_validate(o);
    /* TRANSFERRED: only the NEW owner may read; ORPHANED: nobody establishes
     * fresh access. SHARED/BORROWED: any holder may read. OWNED: owner only. */
    ASSERT((o->state != OWN_TRANSFERRED) || (o->owner_cpu == cur_cpu));
    ASSERT(o->state != OWN_ORPHANED);
    ASSERT((o->state != OWN_OWNED) || (o->owner_cpu == cur_cpu));
    (void)cur_cpu;
}

static inline void own_assert_can_write(const ownership_t *o, uint32_t cur_cpu) {
    own_validate(o);
    /* SHARED is read-only (writers must leave SHARED first). A BORROW writes
     * only if it was taken mutable. TRANSFERRED writes only by the new owner. */
    ASSERT(o->state != OWN_SHARED);
    ASSERT(o->state != OWN_ORPHANED);
    ASSERT((o->state != OWN_BORROWED) || (o->flags & OWN_FLAG_MUTABLE));
    ASSERT((o->state != OWN_OWNED)    || (o->owner_cpu == cur_cpu));
    ASSERT((o->state != OWN_TRANSFERRED) || (o->owner_cpu == cur_cpu));
    (void)cur_cpu;
}

/* I3: the no-free-while-borrowed / no-free-while-pinned guard. own_put calls
 * this before it would actually reclaim. Always-on — freeing memory a borrower
 * or DMA engine still references is a UAF, lethal under SMP. */
static inline int own_is_reclaimable(const ownership_t *o) {
    return o->borrow_depth == 0 && !(o->flags & OWN_FLAG_PINNED);
}

/* One-line dump for panic context / `own` debug command. */
static inline void own_dump(const ownership_t *o) {
    kprintf("ownership %p: state=%s owner_cpu=%u refs=%u borrows=%u flags=0x%02x\n",
            (void *)o,
            o ? own_state_name((own_state_t)o->state) : "<null>",
            o ? o->owner_cpu : OWN_CPU_NONE,
            o ? kref_read(&o->ref) : 0u,
            o ? o->borrow_depth : 0u,
            o ? o->flags : 0u);
}

#endif /* OWNERSHIP_H */
