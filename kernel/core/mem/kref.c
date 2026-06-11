/*
 * kernel/core/mem/kref.c — Atomic reference counting primitives
 * ==============================================================
 *
 * Provides atomic reference counting for kernel memory allocations to enable
 * safe sharing of objects across multiple owners (e.g., file descriptors,
 * shared memory segments, network buffers, SMP-shared data structures).
 *
 * Design rationale:
 *   - SEQ_CST atomics throughout: matches process.c and cow.c house style.
 *     Simpler than acquire/release split, sufficient for correctness.
 *   - Saturating counter: prevents overflow UAF. A buggy caller spinning
 *     kget() in a loop will saturate at KREF_SATURATED rather than wrapping
 *     to 0, which would trigger a spurious free-under-active-use. Saturated
 *     objects are intentionally leaked (never freed), but this is the safe
 *     failure mode vs. use-after-free.
 *   - No spinlock: the atomic counter IS the synchronization. The 1->0
 *     transition is single-winner (exactly like process_unref), so the
 *     destructor runs on exactly one CPU.
 *   - Magic canary: detects use-after-free and double-free. The header magic
 *     is KREF_MAGIC when the object is alive and 0 after kput frees it, so
 *     a later kput/kget on the freed pointer will catch the corruption.
 *
 * File placement:
 *   kernel/core/mem/kref.c sits next to cow.c (the existing phys-page
 *   refcount table). Same subsystem, same SEQ_CST style. cow.c is the
 *   precedent for "refcount -> free-on-zero" living under core/mem.
 *
 * Usage:
 *   void *obj = kmalloc_ref(1024);       // allocate 1KB with refcount=1
 *   void *shared = kget(obj);            // +1 ref (now 2)
 *   kput(obj);                           // -1 ref (now 1)
 *   kput(shared);                        // -1 ref (now 0 -> free)
 */

#include "../../include/kref.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/types.h"

/* SIZE_MAX is defined in types.h as UINT64_MAX for size_t == uint64_t */
#ifndef SIZE_MAX
#define SIZE_MAX UINT64_MAX
#endif

/* =========================================================================
 * Core kref operations (raw counter manipulation)
 * ========================================================================= */

void kref_init(kref_t* k) {
    if (!k) return;
    __atomic_store_n(&k->count, 1, __ATOMIC_SEQ_CST);
}

void kref_get(kref_t* k) {
    if (!k) return;

    /* CAS loop to saturate instead of wrapping. A plain __atomic_add_fetch
     * would be simpler but cannot enforce the saturation requirement. */
    uint32_t old, new_val;
    do {
        old = __atomic_load_n(&k->count, __ATOMIC_SEQ_CST);

        /* If already saturated, leak the reference (never increment past
         * KREF_SATURATED). This prevents overflow UAF. */
        if (old == KREF_SATURATED) {
            return;
        }

        /* Saturate at KREF_SATURATED rather than wrapping to 0 */
        if (old == KREF_SATURATED - 1) {
            new_val = KREF_SATURATED;
        } else {
            new_val = old + 1;
        }

        /* CAS loop: retry if another CPU raced us */
    } while (!__atomic_compare_exchange_n(&k->count, &old, new_val,
                                           0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
}

int kref_put(kref_t* k) {
    if (!k) return 0;

    /* If saturated, never decrement (saturated objects are intentionally
     * leaked to avoid UAF if gets were dropped). */
    uint32_t current = __atomic_load_n(&k->count, __ATOMIC_SEQ_CST);
    if (current == KREF_SATURATED) {
        return 0;
    }

    /* Atomic decrement */
    uint32_t old = __atomic_sub_fetch(&k->count, 1, __ATOMIC_SEQ_CST);

    /* Underflow guard: if old is a huge value (close to UINT32_MAX), it means
     * we decremented from 0 (double-put without a matching get). This is a
     * bug, but we should not free the object (that would cause UAF). Log and
     * return 0. The threshold is arbitrary but chosen to catch wraparound. */
    if (old > KREF_SATURATED - 256) {
        kprintf("[KREF] ERROR: kref_put underflow (double-put?), count=%u\n", old);
        return 0;
    }

    /* Return 1 if this was the last reference (caller should free) */
    return (old == 0) ? 1 : 0;
}

uint32_t kref_read(const kref_t* k) {
    if (!k) return 0;
    return __atomic_load_n(&k->count, __ATOMIC_RELAXED);
}

/* =========================================================================
 * Allocation wrappers (kmalloc + hidden header)
 * ========================================================================= */

/* Recover the header from a payload pointer. Inverse of (hdr+1). */
static inline kref_hdr_t* hdr_of(void* ptr) {
    if (!ptr) return NULL;
    return ((kref_hdr_t*)ptr) - 1;
}

void* kmalloc_ref_dtor(size_t size, void (*dtor)(void* payload)) {
    /* Guard size overflow before computing total */
    if (size > SIZE_MAX - sizeof(kref_hdr_t)) {
        kprintf("[KREF] kmalloc_ref_dtor: size overflow (%llu)\n",
                (unsigned long long)size);
        return NULL;
    }

    size_t total = sizeof(kref_hdr_t) + size;
    kref_hdr_t* hdr = (kref_hdr_t*)kmalloc(total);
    if (!hdr) {
        return NULL;
    }

    /* Initialize header */
    kref_init(&hdr->ref);
    hdr->magic = KREF_MAGIC;
    hdr->size = (uint32_t)size;  // safe cast: size < SIZE_MAX - sizeof(hdr)
    hdr->dtor = dtor;

#ifdef SMP_FOUNDATION
    extern void health_monitor_record_alloc(void);
    health_monitor_record_alloc();
#endif

    /* Return pointer to payload (user never sees the header) */
    return (void*)(hdr + 1);
}

void* kmalloc_ref(size_t size) {
    return kmalloc_ref_dtor(size, NULL);
}

void* kget(void* ptr) {
    if (!ptr) return NULL;

    kref_hdr_t* hdr = hdr_of(ptr);

    /* Validate magic canary (catches use-after-free / heap corruption) */
    if (hdr->magic != KREF_MAGIC) {
        kprintf("[KREF] kget: bad magic (ptr=%p, magic=0x%x, expected=0x%x)\n",
                ptr, hdr->magic, KREF_MAGIC);
        return NULL;
    }

    kref_get(&hdr->ref);
    return ptr;
}

int kput(void* ptr) {
    if (!ptr) return 0;

    kref_hdr_t* hdr = hdr_of(ptr);

    /* Validate magic canary */
    if (hdr->magic != KREF_MAGIC) {
        kprintf("[KREF] kput: bad magic (ptr=%p, magic=0x%x, expected=0x%x)\n",
                ptr, hdr->magic, KREF_MAGIC);
        return 0;
    }

    /* Decrement refcount; if this was the last reference, destroy */
    if (kref_put(&hdr->ref)) {
        /* Call destructor if present (before freeing, so dtor can still
         * access the payload) */
        if (hdr->dtor) {
            hdr->dtor(ptr);
        }

        /* Poison magic so a later double-kput is caught */
        hdr->magic = 0;

#ifdef SMP_FOUNDATION
        extern void health_monitor_record_free(void);
        health_monitor_record_free();
#endif

        /* Free the entire allocation (header + payload) */
        kfree(hdr);
        return 1;
    }

    return 0;
}
