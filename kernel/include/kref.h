#ifndef KREF_H
#define KREF_H

#include "types.h"

/*
 * kref.h — Reference-counted kernel object primitives
 * ====================================================
 *
 * Provides atomic reference counting for kernel memory allocations to safely
 * share objects across multiple owners (e.g., file descriptors, shared memory,
 * network buffers). The reference count starts at 1 on allocation; kget()
 * increments it atomically; kput() decrements and frees the object when the
 * last owner releases it.
 *
 * Design:
 *   - SEQ_CST atomics throughout (matches process.c / cow.c style)
 *   - Saturating counter at KREF_SATURATED to prevent overflow UAF
 *   - Magic canary detection for use-after-free / double-free
 *   - Optional destructor callback for cleanup before kfree
 *
 * Usage:
 *   void *obj = kmalloc_ref(size);       // allocate with refcount=1
 *   void *shared = kget(obj);            // +1 ref
 *   kput(obj);                           // -1 ref, no-op here
 *   kput(shared);                        // -1 ref, frees when 0
 */

/* =========================================================================
 * Core reference counter
 * ========================================================================= */

typedef struct {
    uint32_t count;
} kref_t;

/* Saturate at this value to prevent overflow UAF. Saturated objects are
 * intentionally leaked (never freed) to avoid use-after-free if a buggy
 * caller keeps calling kget() without bounds. */
#define KREF_SATURATED 0xFFFFFFFFU

/* Initialize a kref to 1 reference (object alive with one owner) */
void kref_init(kref_t* k);

/* Increment reference count atomically. Saturates at KREF_SATURATED instead
 * of wrapping. Returns the object pointer for convenience (allows chaining). */
void kref_get(kref_t* k);

/* Decrement reference count atomically. Returns 1 if this was the final
 * reference (caller should destroy the object); 0 otherwise. */
int kref_put(kref_t* k);

/* Read the current reference count (debug/diagnostics only; never branch on
 * this for correctness — the value may be stale by the time you see it). */
uint32_t kref_read(const kref_t* k);

/* =========================================================================
 * Allocation wrappers (kmalloc + hidden header with embedded refcount)
 * ========================================================================= */

/* Magic canary to detect use-after-free / double-free / heap corruption */
#define KREF_MAGIC 0xCAFEBEEFU

/* Hidden header prepended to every kmalloc_ref allocation. The user pointer
 * returned by kmalloc_ref points to the payload immediately after this header. */
typedef struct kref_hdr {
    kref_t   ref;    // Reference counter
    uint32_t magic;  // Canary (KREF_MAGIC when alive, 0 when freed)
    uint32_t size;   // Payload size in bytes (for bounds checking / debug)
    void (*dtor)(void* payload);  // Optional destructor (NULL if none)
} kref_hdr_t;

/* Allocate size bytes of refcounted memory with optional destructor.
 * Returns pointer to payload (NOT the header). Reference count starts at 1.
 * Returns NULL on allocation failure. */
void* kmalloc_ref_dtor(size_t size, void (*dtor)(void* payload));

/* Allocate size bytes of refcounted memory (no destructor).
 * Returns pointer to payload. Reference count starts at 1. */
void* kmalloc_ref(size_t size);

/* Increment reference count for a payload pointer returned by kmalloc_ref.
 * Validates the magic canary before incrementing. Returns the pointer on
 * success; NULL if the pointer is invalid or corrupted. */
void* kget(void* ptr);

/* Decrement reference count for a payload pointer. If this is the last
 * reference, calls the destructor (if any), poisons the magic canary, and
 * frees the allocation. Returns 1 if freed; 0 otherwise. */
int kput(void* ptr);

#endif
