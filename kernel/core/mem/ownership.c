/*
 * kernel/core/mem/ownership.c — Explicit object-ownership state model
 * ====================================================================
 *
 * Implements the ownership state machine defined in ownership.h. This file
 * provides the single chokepoint (own_transition) through which all lifecycle
 * changes flow, enforces the transition table and dynamic invariants, and
 * provides the convenience wrappers (own_share, own_transfer, own_borrow, etc.)
 * that subsystems actually call.
 *
 * DESIGN PRINCIPLES (extract, don't invent)
 *   - Reuses kref_t for liveness (kref_init/get/put verbatim).
 *   - Reuses spinlock_t for transition critical sections.
 *   - Reuses cpu_id() for owner attribution.
 *   - Reuses ASSERT/ASSERT_ALWAYS/kernel_panic for violation detection.
 *   - No new atomics or locking convention introduced.
 *
 * FILE PLACEMENT
 *   kernel/core/mem/ownership.c sits next to kref.c (the allocation-level
 *   refcount primitives) and cow.c (the page-level refcount table). Same
 *   subsystem, same SEQ_CST style. kref.c is the precedent for "refcount ->
 *   free-on-zero" living under core/mem.
 *
 * SMP BRING-UP DISCIPLINE
 *   This file is brick 1 of the 8-brick SMP roadmap: the ownership MODEL,
 *   implemented and compile-verified, but NOT yet live in any allocator or
 *   subsystem. It is branch-isolated (smp-foundation) and gated: AP-online
 *   != ownership-model-live. Later bricks will integrate it into kmalloc_ref,
 *   network buffers, graphics compositor, and tensor jobs — one subsystem at
 *   a time, each checkpoint-verified before the next brick.
 */

#include "../../include/ownership.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/smp.h"
#include "../../include/types.h"

/* =========================================================================
 * Core lifecycle: init, get, put
 * ========================================================================= */

void own_init(ownership_t *o) {
    if (!o) {
        kernel_panic("own_init: NULL ownership_t pointer");
    }

    /* Initialize the embedded kref (refcount=1, object alive with one owner) */
    kref_init(&o->ref);

    /* Set the ownership state */
    o->magic = OWN_MAGIC;
    o->state = OWN_OWNED;
    o->flags = OWN_FLAG_NONE;
    o->owner_cpu = cpu_id();
    o->borrow_depth = 0;

    /* Initialize the lock */
    spin_lock_init(&o->lock);

    /* Trace if requested (but OWN_FLAG_TRACED is off by default at init) */
    if (o->flags & OWN_FLAG_TRACED) {
        kprintf("[OWN] init %p: state=OWNED owner_cpu=%u refs=1\n",
                (void *)o, o->owner_cpu);
    }
}

void own_get(ownership_t *o) {
    own_validate(o);

    /* own_get is only legal on SHARED (taking another reader). Calling it on
     * any other state is a violation: you can't refcount an exclusively-owned
     * object without first transitioning it to SHARED. This catches the classic
     * "I refcounted an exclusively-owned object and now two CPUs think they
     * own it" bug. */
    if (o->state != OWN_SHARED) {
        own_dump(o);
        kprintf("own_get: illegal on non-SHARED object (state=%s)\n",
                own_state_name((own_state_t)o->state));
        kernel_panic("own_get: illegal state");
    }

    /* Increment the embedded kref (no lock needed: kref ops are atomic) */
    kref_get(&o->ref);

    /* Trace if requested */
    if (o->flags & OWN_FLAG_TRACED) {
        kprintf("[OWN] get %p: refs=%u\n", (void *)o, kref_read(&o->ref));
    }
}

int own_put(ownership_t *o) {
    own_validate(o);

    /* Decrement the embedded kref. If this did NOT drive count to 0, we're done. */
    if (!kref_put(&o->ref)) {
        if (o->flags & OWN_FLAG_TRACED) {
            kprintf("[OWN] put %p: refs=%u (still alive)\n",
                    (void *)o, kref_read(&o->ref));
        }
        return 0;
    }

    /* Count hit 0. Check if we can actually reclaim (I3: no-free-while-borrowed,
     * no-free-while-pinned). If not reclaimable, transition to ORPHANED + set
     * DEFER_FREE, and return 0 — the last own_return/own_unpin will complete
     * the free. */
    if (!own_is_reclaimable(o)) {
        spin_lock(&o->lock);
        o->state = OWN_ORPHANED;
        o->flags |= OWN_FLAG_DEFER_FREE;
        if (o->flags & OWN_FLAG_TRACED) {
            kprintf("[OWN] put %p: 0 refs but not reclaimable (borrows=%u pinned=%d) -> ORPHANED+DEFER\n",
                    (void *)o, o->borrow_depth,
                    (o->flags & OWN_FLAG_PINNED) ? 1 : 0);
        }
        spin_unlock(&o->lock);
        return 0;
    }

    /* Reclaimable: poison the magic and return 1 (caller should free).
     * Trace before poisoning so the trace shows the final state. */
    if (o->flags & OWN_FLAG_TRACED) {
        kprintf("[OWN] put %p: 0 refs, reclaimable -> FREEING\n", (void *)o);
    }

    spin_lock(&o->lock);
    o->magic = 0xDEAD0000u | (o->state & 0xFFu);  /* poison with final state */
    spin_unlock(&o->lock);

    return 1;
}

/* =========================================================================
 * State transition chokepoint
 * ========================================================================= */

int own_transition(ownership_t *o, own_state_t to, uint32_t new_owner_cpu) {
    own_validate(o);

    own_state_t from = (own_state_t)o->state;

    /* Check the static transition table (I1/I5: legal edge) */
    if (!own_transition_is_legal(from, to)) {
        own_dump(o);
        kprintf("own_transition: illegal %s -> %s\n",
                own_state_name(from), own_state_name(to));
        kernel_panic("own_transition: illegal transition");
    }

    /* Critical section: state and owner_cpu are coupled, must update atomically */
    spin_lock(&o->lock);

    /* Re-validate inside the lock (catch concurrent corruption) */
    if (o->state != from) {
        spin_unlock(&o->lock);
        own_dump(o);
        kprintf("own_transition: state changed under us (was %s, now %s)\n",
                own_state_name(from), own_state_name((own_state_t)o->state));
        kernel_panic("own_transition: concurrent state corruption");
    }

    /* Dynamic guards (enforced after the static table passes):
     *
     * I2: SHARED -> OWNED requires refcount==1 (last reader re-acquires
     *     exclusivity). This is the only "Y*" cell in the table.
     */
    if (from == OWN_SHARED && to == OWN_OWNED) {
        uint32_t refs = kref_read(&o->ref);
        if (refs != 1) {
            spin_unlock(&o->lock);
            own_dump(o);
            kprintf("own_transition: SHARED -> OWNED requires refs==1 (have %u)\n", refs);
            kernel_panic("own_transition: SHARED->OWNED with multiple refs");
        }
    }

    /* I6: borrow-nesting: OWNED -> TRANSFERRED requires borrow_depth==0
     *     (borrower must return before transfer). Applies to any state leaving
     *     to TRANSFERRED (SHARED, BORROWED, or OWNED -> TRANSFERRED). */
    if (to == OWN_TRANSFERRED && o->borrow_depth > 0) {
        spin_unlock(&o->lock);
        own_dump(o);
        kprintf("own_transition: -> TRANSFERRED requires borrow_depth==0 (have %u)\n",
                o->borrow_depth);
        kernel_panic("own_transition: transfer while borrowed");
    }

    /* Apply the transition */
    o->state = to;

    /* Update owner_cpu for TRANSFERRED / OWNED (new exclusive owner) */
    if (to == OWN_TRANSFERRED || to == OWN_OWNED) {
        o->owner_cpu = new_owner_cpu;
    }

    /* Trace if requested */
    if (o->flags & OWN_FLAG_TRACED) {
        kprintf("[OWN] transition %p: %s -> %s (owner_cpu=%u refs=%u borrows=%u)\n",
                (void *)o,
                own_state_name(from),
                own_state_name(to),
                o->owner_cpu,
                kref_read(&o->ref),
                o->borrow_depth);
    }

    spin_unlock(&o->lock);
    return 0;
}

/* =========================================================================
 * Convenience wrappers (the vocabulary subsystems actually call)
 * ========================================================================= */

void own_share(ownership_t *o) {
    /* OWNED -> SHARED: exclusive owner releases exclusivity, multiple readers
     * may now hold refs. The caller (the original owner) implicitly holds the
     * first ref (count is already 1 from own_init); other holders call own_get
     * to take additional readers. */
    own_transition(o, OWN_SHARED, OWN_CPU_NONE);
}

void own_transfer(ownership_t *o, uint32_t to_cpu) {
    /* * -> TRANSFERRED: ownership MOVES to a new owner. The original owner's
     * handle is poisoned (any access by the original owner after this is a
     * violation, caught by own_assert_can_read/write via the owner_cpu check).
     * This is the move semantic (CPU0 -> CPU1 tensor job, userspace -> AI
     * worker inference job). */
    own_transition(o, OWN_TRANSFERRED, to_cpu);
}

void own_orphan(ownership_t *o) {
    /* * -> ORPHANED: the owner is gone (exited/crashed/revoked) but the object
     * is not yet reclaimed (e.g. an in-flight DMA must drain, or a borrower
     * still holds a lease). The object is scheduled for cleanup; no NEW owner
     * may be established. Terminal except for the final put that frees it. */
    own_transition(o, OWN_ORPHANED, OWN_CPU_NONE);
}

void own_borrow(ownership_t *o, int mutable, int take_ref) {
    own_validate(o);

    /* A borrow is a temporary, non-owning lease. The lender remains the real
     * owner (recorded in owner_cpu); the borrower may read (and write iff the
     * lease was taken mutably) but MUST NOT free, transfer, or re-lend. */

    spin_lock(&o->lock);

    /* Bump borrow depth (nested borrows are allowed) */
    o->borrow_depth++;

    /* Set mutable flag if requested */
    if (mutable) {
        o->flags |= OWN_FLAG_MUTABLE;
    } else {
        o->flags &= ~OWN_FLAG_MUTABLE;
    }

    /* Transition to BORROWED state (no-op if already BORROWED — this handles
     * nested borrows correctly). We do NOT pass through own_transition here
     * because we need to update borrow_depth inside the same critical section
     * as the state change. Instead, we inline the transition logic. */
    own_state_t from = (own_state_t)o->state;
    if (from != OWN_BORROWED) {
        /* First borrow: transition from OWNED/SHARED/TRANSFERRED to BORROWED.
         * Check the static table. */
        if (!own_transition_is_legal(from, OWN_BORROWED)) {
            spin_unlock(&o->lock);
            own_dump(o);
            kprintf("own_borrow: illegal %s -> BORROWED\n", own_state_name(from));
            kernel_panic("own_borrow: illegal transition to BORROWED");
        }
        o->state = OWN_BORROWED;

        /* Trace if requested */
        if (o->flags & OWN_FLAG_TRACED) {
            kprintf("[OWN] borrow %p: %s -> BORROWED (mutable=%d depth=%u)\n",
                    (void *)o, own_state_name(from), mutable, o->borrow_depth);
        }
    } else {
        /* Nested borrow: already BORROWED, just increment depth */
        if (o->flags & OWN_FLAG_TRACED) {
            kprintf("[OWN] borrow %p: BORROWED (nested, mutable=%d depth=%u)\n",
                    (void *)o, mutable, o->borrow_depth);
        }
    }

    spin_unlock(&o->lock);

    /* A borrow does NOT take a refcount by default (the lender's lifetime
     * dominates the lease, I6). Pass take_ref=1 only for async borrows that
     * may outlive the lender's stack frame. */
    if (take_ref) {
        kref_get(&o->ref);
    }
}

void own_return(ownership_t *o) {
    own_validate(o);

    spin_lock(&o->lock);

    /* Sanity check: borrow_depth must be > 0 (balancing invariant) */
    if (o->borrow_depth == 0) {
        spin_unlock(&o->lock);
        own_dump(o);
        kernel_panic("own_return: borrow_depth already 0 (unbalanced return)");
    }

    /* Decrement borrow depth */
    o->borrow_depth--;

    /* If depth hits 0, restore the prior state (or complete a deferred free) */
    if (o->borrow_depth == 0) {
        if (o->flags & OWN_FLAG_DEFER_FREE) {
            /* Deferred free: the object was ORPHANED while borrowed. Now that
             * the lease has ended, we can complete the reclaim. Poison the
             * magic and unlock before returning (the caller will free). */
            if (o->flags & OWN_FLAG_TRACED) {
                kprintf("[OWN] return %p: depth=0, completing deferred free (ORPHANED)\n",
                        (void *)o);
            }
            o->magic = 0xDEAD0000u | (o->state & 0xFFu);
            spin_unlock(&o->lock);
            /* The caller must now free the object (not done here because the
             * caller holds the pointer, and we can't kfree an ownership_t* —
             * the ownership_t is embedded in a larger allocation). This mirrors
             * own_put's "return 1 to signal caller should free" convention. */
            return;
        }

        /* Normal case: restore the prior state. If the object was OWNED before
         * the borrow, it returns to OWNED. If it was SHARED, it was never
         * supposed to be borrowed (borrows are for exclusive owners), so this
         * is a violation. The transition table forbids BORROWED -> SHARED. */
        if (o->state == OWN_BORROWED) {
            /* Restore to OWNED (the lender was the exclusive owner) */
            o->state = OWN_OWNED;
            if (o->flags & OWN_FLAG_TRACED) {
                kprintf("[OWN] return %p: BORROWED -> OWNED (depth=0)\n",
                        (void *)o);
            }
        }
    } else {
        /* Nested borrow: still have outstanding leases */
        if (o->flags & OWN_FLAG_TRACED) {
            kprintf("[OWN] return %p: BORROWED (depth=%u)\n",
                    (void *)o, o->borrow_depth);
        }
    }

    spin_unlock(&o->lock);
}

/* =========================================================================
 * HW pin/unpin (for in-flight DMA: "a bus stall is not a software spin")
 * ========================================================================= */

void own_pin(ownership_t *o) {
    own_validate(o);

    spin_lock(&o->lock);
    o->flags |= OWN_FLAG_PINNED;

    if (o->flags & OWN_FLAG_TRACED) {
        kprintf("[OWN] pin %p: PINNED set\n", (void *)o);
    }

    spin_unlock(&o->lock);
}

void own_unpin(ownership_t *o) {
    own_validate(o);

    spin_lock(&o->lock);

    /* Sanity check: PINNED must be set (balancing invariant) */
    if (!(o->flags & OWN_FLAG_PINNED)) {
        spin_unlock(&o->lock);
        own_dump(o);
        kernel_panic("own_unpin: PINNED flag not set (unbalanced unpin)");
    }

    o->flags &= ~OWN_FLAG_PINNED;

    /* If a free was deferred while pinned, and we're now reclaimable, complete
     * the free. This mirrors own_return's deferred-free completion. */
    if ((o->flags & OWN_FLAG_DEFER_FREE) && own_is_reclaimable(o)) {
        if (o->flags & OWN_FLAG_TRACED) {
            kprintf("[OWN] unpin %p: completing deferred free (ORPHANED, now reclaimable)\n",
                    (void *)o);
        }
        o->magic = 0xDEAD0000u | (o->state & 0xFFu);
        spin_unlock(&o->lock);
        /* Caller must free (same as own_return's deferred-free path) */
        return;
    }

    if (o->flags & OWN_FLAG_TRACED) {
        kprintf("[OWN] unpin %p: PINNED cleared\n", (void *)o);
    }

    spin_unlock(&o->lock);
}
