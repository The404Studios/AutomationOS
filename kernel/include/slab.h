#ifndef SLAB_H
#define SLAB_H

#include "mem.h"   /* pulls types.h in the kernel's established include order */

/*
 * Slab Allocator — object cache for fixed-size kernel objects
 * ===========================================================
 *
 * COMPLEMENTARY to the segregated-bin kmalloc() in heap.c. Where kmalloc is a
 * general-purpose variable-size allocator, the slab allocator carves PMM-backed
 * 4KB pages ("slabs") into fixed-size object slots and serves them O(1) from a
 * per-slab free-list. Each cache is dedicated to ONE object size, which gives:
 *   - zero internal fragmentation between differently-sized requests,
 *   - excellent locality (objects of one type packed tightly),
 *   - O(1) alloc/free with no search.
 *
 * Backing memory comes from pmm_alloc_page() (identity-mapped, so phys==virt is
 * directly usable). One 4KB page == one slab. The first bytes of each slab page
 * hold a slab_t header; the remainder is divided into obj_size slots. Because
 * PMM pages are 4KB-aligned, slab_free() recovers the owning slab (and through
 * it the owning cache) by masking the object pointer down to the page base —
 * no search, no per-object metadata.
 */

/* Opaque slab handle (defined in slab.c) */
typedef struct slab slab_t;

/*
 * Per-cache control block. All fields are private to slab.c; the struct is
 * exposed only so callers can hold a slab_cache_t* handle.
 */
typedef struct slab_cache slab_cache_t;

/*
 * Create a cache that hands out fixed-size objects.
 *   name     — short label for slab_dump() (not copied; must outlive the cache).
 *   obj_size — requested object size in bytes (rounded up to `align`).
 *   align    — slot alignment; coerced to a power of two, min 8. Pass 0 for the
 *              default (16, matching kmalloc's guarantee).
 * Returns the cache handle, or NULL on OOM / invalid arguments.
 */
slab_cache_t* slab_cache_create(const char* name, size_t obj_size, size_t align);

/*
 * Allocate one object from the cache. O(1): pops a free slot from a partial
 * slab, growing by one PMM page when none is free. Returns NULL on OOM.
 */
void* slab_alloc(slab_cache_t* c);

/*
 * Return an object previously obtained from `c` to its cache. O(1): the owning
 * slab is found by masking `obj` to its page base. A NULL obj is ignored.
 */
void slab_free(slab_cache_t* c, void* obj);

/*
 * Destroy a cache, returning every backing slab page to the PMM. Objects still
 * in use are abandoned (the caller is responsible for having freed them).
 */
void slab_cache_destroy(slab_cache_t* c);

/* Print per-cache statistics (objects in use, total slabs, …) via kprintf. */
void slab_dump(void);

/*
 * Boot-time self-test. Creates a cache, allocates enough objects to span
 * multiple slab pages, verifies all pointers are distinct and sentinels are
 * intact, frees and re-allocates to confirm reuse, then destroys the cache.
 * Logs "[SLAB] SELFTEST: PASS" or "[SLAB] SELFTEST: FAIL <reason>".
 * Returns 0 on PASS, non-zero on FAIL.
 */
int slab_selftest(void);

#endif /* SLAB_H */
