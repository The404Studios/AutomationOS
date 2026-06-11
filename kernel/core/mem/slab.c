/*
 * kernel/core/mem/slab.c — Object-cache slab allocator
 * =============================================================================
 * COMPLEMENTARY to heap.c's kmalloc(): a per-object-size cache that carves
 * PMM-backed 4KB pages into fixed-size slots served O(1) from a free-list.
 *
 * Memory layout of ONE slab (== ONE pmm_alloc_page() page, 4KB, 4KB-aligned):
 *
 *   page base ──► [ slab_t header ]  [ slot 0 ][ slot 1 ] ... [ slot N-1 ]
 *                  ^                  ^
 *                  |                  └─ first_obj (aligned up from header end)
 *                  └─ cache back-pointer + free-list head + bookkeeping
 *
 *   Each FREE slot stores, in its first 8 bytes, a pointer to the next free
 *   slot (an intrusive singly-linked free-list). On alloc we pop the head; on
 *   free we push the slot back. obj_size is rounded up to `align` and is always
 *   >= sizeof(void*) so a freed slot can hold the link pointer.
 *
 * Finding the owning slab/cache from an object pointer (slab_free):
 *   pmm_alloc_page() returns 4KB-aligned pages, and every slab is exactly one
 *   such page. Therefore  slab_base = obj & ~(PAGE_SIZE-1)  yields the slab_t
 *   header for ANY object carved from that page — O(1), no search, no per-object
 *   metadata. The header carries a back-pointer to the owning cache, which is
 *   asserted against the cache passed to slab_free() to catch cross-cache frees.
 *
 * Slab lists (per cache):
 *   - partial: slabs with at least one free slot   (slab_alloc draws from here)
 *   - full:    slabs with every slot handed out
 *   A slab migrates partial->full when its last slot is taken, and
 *   full->partial when a slot is returned to a previously-full slab. An empty
 *   slab (all slots free) is freed back to the PMM immediately to bound memory.
 *
 * Locking: one spinlock per cache (leaf lock — released before any PMM call,
 * mirroring pmm.c's lock-ordering discipline). PMM has its own internal locks.
 */

#include "../../include/slab.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/string.h"
#include "../../include/spinlock.h"
#include "../../include/perf.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Mask an arbitrary pointer down to its containing 4KB page base. */
#define SLAB_PAGE_MASK   (~((uintptr_t)PAGE_SIZE - 1))


/* Sanity canary stored in each slab header; detects a free of a pointer that
 * does not belong to any slab (e.g. a heap or stack pointer). */
#define SLAB_MAGIC       0x51AB0BACE51AB0BULL

/* Default slot alignment when the caller passes align==0. Matches kmalloc. */
#define SLAB_DEFAULT_ALIGN  16
#define SLAB_MIN_ALIGN      8

/* =========================================================================
 * Slab header (lives at the start of each backing page)
 * ========================================================================= */
struct slab {
    uint64_t      magic;       /* SLAB_MAGIC — corruption / bad-free guard */
    slab_cache_t* cache;       /* owning cache (recovered via page mask)   */
    struct slab*  next;        /* next slab in partial/full list           */
    struct slab*  prev;        /* prev slab in partial/full list           */
    void*         free_list;   /* head of this slab's intrusive free-list  */
    uint32_t      free_count;  /* free slots remaining in this slab        */
    uint32_t      total_slots; /* slots carved from this slab              */
};

/* =========================================================================
 * Per-cache control block
 * ========================================================================= */
struct slab_cache {
    const char*  name;          /* label for slab_dump (not copied)        */
    size_t       obj_size;      /* slot size in bytes (rounded to align)   */
    size_t       align;         /* slot alignment (power of two)           */
    uint32_t     slots_per_slab;/* objects carved from one 4KB page        */

    slab_t*      partial;       /* slabs with >=1 free slot                */
    slab_t*      full;          /* slabs with no free slot                 */

    /* Statistics */
    uint64_t     objs_in_use;   /* objects currently handed out            */
    uint64_t     total_slabs;   /* backing pages currently owned           */
    uint64_t     total_allocs;  /* lifetime slab_alloc() successes         */
    uint64_t     total_frees;   /* lifetime slab_free() calls              */

    spinlock_t   lock;          /* per-cache leaf lock                     */
    struct slab_cache* registry_next; /* global cache list (for slab_dump) */
};

/* =========================================================================
 * Cache control blocks: a small static pool (no chicken-and-egg with kmalloc).
 * The slab allocator can come up before the heap, so cache descriptors are
 * drawn from a fixed array rather than kmalloc'd.
 * ========================================================================= */
#define MAX_SLAB_CACHES 64
static struct slab_cache cache_pool[MAX_SLAB_CACHES];
static uint32_t          cache_pool_used = 0;
static struct slab_cache* cache_registry = NULL;   /* head of all live caches */
static spinlock_t        slab_global_lock;         /* protects the pool/registry */
static bool              slab_global_inited = false;

static inline void slab_global_init_once(void) {
    /* BSS-zero means slab_global_inited==false on first entry. The spinlock is
     * also zero-initialized (== unlocked) so this is safe to call lock-free for
     * the very first single-threaded boot-time create. */
    if (!slab_global_inited) {
        spin_lock_init(&slab_global_lock);
        slab_global_inited = true;
    }
}

/* Round v up to the next power-of-two alignment `a` (a must be power of two). */
static inline size_t round_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

/* Coerce an arbitrary value to a power of two >= SLAB_MIN_ALIGN. */
static inline size_t normalize_align(size_t a) {
    if (a == 0) a = SLAB_DEFAULT_ALIGN;
    if (a < SLAB_MIN_ALIGN) a = SLAB_MIN_ALIGN;
    /* Smallest power of two >= a */
    size_t p = SLAB_MIN_ALIGN;
    while (p < a) p <<= 1;
    return p;
}

/* =========================================================================
 * List helpers (doubly-linked via slab->next/prev)
 * ========================================================================= */
static inline void slab_list_push(slab_t** head, slab_t* s) {
    s->prev = NULL;
    s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}

static inline void slab_list_remove(slab_t** head, slab_t* s) {
    if (s->prev) s->prev->next = s->next;
    else         *head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = NULL;
    s->prev = NULL;
}

/* =========================================================================
 * slab_grow — allocate one fresh backing page and carve it into slots.
 * Caller must hold c->lock; PMM is allocated WITHOUT the lock held (leaf-lock
 * discipline), so we drop and re-take it around the PMM call.
 * Returns the new partial slab (already linked into c->partial), or NULL OOM.
 * ========================================================================= */
static slab_t* slab_grow(slab_cache_t* c) {
    /* Drop the cache lock across the PMM call (PMM has its own locks; never
     * nest cache->lock inside a PMM allocation). */
    spin_unlock(&c->lock);
    void* page = pmm_alloc_page();
    spin_lock(&c->lock);

    if (!page) return NULL;   /* out of physical memory */

    /* SHADOW-IMMUNITY: address the slab (header, free-list, and every object we
     * hand to kmalloc callers) through the DIRECT MAP, never the low identity
     * (phys==virt) address. The kernel runs identity-mapped low, so a raw page
     * pointer (~tens of MB) collides with user ELF segments that exec maps at the
     * SAME low VAs (apps link at 0x200000; a big BSS reaches the slab region).
     * Under that process's CR3 the identity VA is shadowed by the user's frame,
     * so reading s->magic via identity sees the user page (magic=0) — the
     * slab-churn "corruption". PML4[256] (direct map) is shared higher-half and
     * NEVER shadowed by user low VAs, so PHYS_TO_DIRECT(page) is correct on ANY
     * CR3. (DMA buffers still come from pmm_alloc_page directly — see ahci.c.) */
    slab_t* s = (slab_t*)PHYS_TO_DIRECT(page);
    s->magic       = SLAB_MAGIC;
    s->cache       = c;
    s->next        = NULL;
    s->prev        = NULL;
    s->free_list   = NULL;
    s->total_slots = c->slots_per_slab;
    s->free_count  = c->slots_per_slab;

    /* First slot begins after the header, aligned up to the cache's alignment.
     * Carve from the DIRECT-MAP base `s` so every slot pointer is a direct-map
     * (shadow-immune) address. */
    uintptr_t first = round_up((uintptr_t)s + sizeof(slab_t), c->align);

    /* Thread every slot onto the intrusive free-list. Pushing in ascending
     * index order leaves the highest-address slot at the head; the exact order
     * is irrelevant to correctness (the self-test checks set membership, not
     * order) — every carved slot ends up on the list exactly once. */
    for (uint32_t i = 0; i < c->slots_per_slab; i++) {
        void* slot = (void*)(first + (uintptr_t)i * c->obj_size);
        *(void**)slot = s->free_list;
        s->free_list = slot;
    }

    slab_list_push(&c->partial, s);
    c->total_slabs++;
    return s;
}

/* =========================================================================
 * slab_cache_create
 * ========================================================================= */
slab_cache_t* slab_cache_create(const char* name, size_t obj_size, size_t align) {
    if (obj_size == 0) return NULL;

    slab_global_init_once();

    align = normalize_align(align);

    /* Slot must be large enough to hold the free-list link, and a multiple of
     * the alignment so consecutive slots stay aligned. */
    size_t slot = obj_size;
    if (slot < sizeof(void*)) slot = sizeof(void*);
    slot = round_up(slot, align);

    /* How many slots fit after the (aligned) header in one 4KB page? */
    uintptr_t first  = round_up(sizeof(slab_t), align);
    if (first >= PAGE_SIZE) return NULL;          /* alignment too large */
    size_t usable    = PAGE_SIZE - first;
    uint32_t per     = (uint32_t)(usable / slot);
    if (per == 0) {
        /* Object too big for a single-page slab. (Multi-page slabs are out of
         * scope: single-page slabs keep slab_free's page-mask lookup O(1).) */
        kprintf("[SLAB] create '%s' failed: obj_size %lu too large for one page\n",
                name ? name : "?", (unsigned long)obj_size);
        return NULL;
    }

    spin_lock(&slab_global_lock);
    if (cache_pool_used >= MAX_SLAB_CACHES) {
        spin_unlock(&slab_global_lock);
        kprintf("[SLAB] create '%s' failed: cache pool exhausted (%d)\n",
                name ? name : "?", MAX_SLAB_CACHES);
        return NULL;
    }
    slab_cache_t* c = &cache_pool[cache_pool_used++];
    spin_unlock(&slab_global_lock);

    c->name           = name;
    c->obj_size       = slot;
    c->align          = align;
    c->slots_per_slab = per;
    c->partial        = NULL;
    c->full           = NULL;
    c->objs_in_use    = 0;
    c->total_slabs    = 0;
    c->total_allocs   = 0;
    c->total_frees    = 0;
    spin_lock_init(&c->lock);

    /* Link into the global registry for slab_dump(). */
    spin_lock(&slab_global_lock);
    c->registry_next = cache_registry;
    cache_registry = c;
    spin_unlock(&slab_global_lock);

    kprintf("[SLAB] cache '%s': obj=%lu align=%lu slots/slab=%u\n",
            name ? name : "?", (unsigned long)slot, (unsigned long)align, per);
    return c;
}

/* =========================================================================
 * slab_alloc — O(1) pop from a partial slab, growing on demand.
 * ========================================================================= */
void* slab_alloc(slab_cache_t* c) {
    if (!c) return NULL;

    PERF_START(PERF_OP_SLAB_ALLOC);

    spin_lock(&c->lock);

    slab_t* s = c->partial;
    if (!s) {
        s = slab_grow(c);          /* may drop/re-take c->lock internally */
        if (!s) {
            spin_unlock(&c->lock);
            return NULL;           /* OOM */
        }
    }

    /* SLABDIAG + RECOVERY: a valid free slot lives inside THIS slab's page, and a
     * live slab keeps its magic + sane counts. A free-list head pointing outside
     * the page, a bad magic, or total_slots==0 means an external writer (a stale
     * pointer to this physical page from a prior life, e.g. a freed file buffer)
     * clobbered the slab header/slot. Rather than #GP on the deref below, ORPHAN
     * the poisoned partial list (leak it — its next/prev are garbage so we cannot
     * safely traverse it) and grow a fresh slab so kmalloc keeps working. */
    void* obj = s->free_list;
    /* A partial slab ALWAYS has free_count>0 and thus a non-NULL free_list, so
     * obj==NULL here is itself corruption (an external writer zeroed the free_list
     * field while leaving magic/total_slots intact -- exactly the partial-clobber
     * this recovery exists to survive). The old guard's `obj &&` short-circuit
     * tolerated obj==NULL and then dereferenced it at *(void**)obj below. */
    if (s->magic != SLAB_MAGIC || s->total_slots == 0 || obj == NULL ||
        ((uintptr_t)obj & SLAB_PAGE_MASK) != (uintptr_t)s) {
        kprintf("[SLAB] partial slab %p of cache '%s' has a corrupt header "
                "(magic=%lx total=%u) — orphaning corrupt head\n",
                s, c->name ? c->name : "?", (unsigned long)s->magic, s->total_slots);

        /* Salvage the rest of the partial chain if the next slab is valid.
         * The corrupt slab's `next` pointer MAY be garbage, so validate it:
         * it must be page-aligned and carry a valid magic canary. If not,
         * we drop the entire chain (the old behaviour). */
        slab_t* salvage = NULL;
        {
            slab_t* raw_next = s->next;
            if (raw_next &&
                ((uintptr_t)raw_next & (PAGE_SIZE - 1)) == 0 &&
                raw_next->magic == SLAB_MAGIC) {
                salvage = raw_next;
                salvage->prev = NULL; /* unlink from the corrupt head */
            }
        }
        c->partial = salvage;       /* keep valid tail, or NULL */

        /* Account for the ONE orphaned slab page (leaked to avoid
         * dereferencing a corrupt next/prev during list removal).
         * objs_in_use is left inflated by whatever the corrupt slab had
         * outstanding — those objects are unrecoverable. */
        if (c->total_slabs > 0)
            c->total_slabs--;

        s = slab_grow(c);           /* fresh backing page (re-links c->partial) */
        if (!s) { spin_unlock(&c->lock); return NULL; }
        obj = s->free_list;
    }
    if (!obj) { spin_unlock(&c->lock); return NULL; }  /* defensive: never deref NULL */
    s->free_list = *(void**)obj;
    s->free_count--;

    /* If the slab is now full, migrate partial -> full. */
    if (s->free_count == 0) {
        slab_list_remove(&c->partial, s);
        slab_list_push(&c->full, s);
    }

    c->objs_in_use++;
    c->total_allocs++;
    spin_unlock(&c->lock);

    PERF_END(PERF_OP_SLAB_ALLOC);
    return obj;
}

/* =========================================================================
 * slab_free — O(1) push back to the owning slab's free-list.
 * Owner slab found by masking obj to its page base; the header back-pointer
 * identifies the cache.
 * ========================================================================= */
void slab_free(slab_cache_t* c, void* obj) {
    if (!obj) return;

    PERF_START(PERF_OP_SLAB_FREE);

    slab_t* s = (slab_t*)((uintptr_t)obj & SLAB_PAGE_MASK);

    if (s->magic != SLAB_MAGIC) {
        kprintf("[SLAB] BLOCKED free of non-slab pointer %p (bad magic)\n", obj);
        return;
    }
    if (c && s->cache != c) {
        kprintf("[SLAB] BLOCKED free of %p: belongs to '%s', freed via '%s'\n",
                obj, s->cache->name ? s->cache->name : "?",
                c->name ? c->name : "?");
        return;
    }
    c = s->cache;   /* authoritative owner */

    spin_lock(&c->lock);

    /* Guard against freeing more than this slab handed out. */
    if (s->free_count >= s->total_slots) {
        spin_unlock(&c->lock);
        kprintf("[SLAB] BLOCKED double/over free of %p in cache '%s'\n",
                obj, c->name ? c->name : "?");
        return;
    }

    /* Per-object double-free detection: walk the free list to check whether this
     * exact slot is already there. The free_count >= total_slots guard above only
     * catches when ALL slots are free; without this scan a slot freed twice while
     * other slots are still allocated silently creates a duplicate entry in the
     * free list, causing two subsequent allocs to return the same pointer and
     * leading to silent data corruption. The walk is O(free_count) per slab which
     * is bounded by slots_per_slab (typically 20-60 for common size classes). */
    {
        void* node = s->free_list;
        while (node) {
            if (node == obj) {
                spin_unlock(&c->lock);
                kprintf("[SLAB] BLOCKED double-free of %p in cache '%s' "
                        "(already on free list)\n",
                        obj, c->name ? c->name : "?");
                return;
            }
            node = *(void**)node;
        }
    }

    bool was_full = (s->free_count == 0);

    /* Push the slot back onto the free-list. */
    *(void**)obj = s->free_list;
    s->free_list = obj;
    s->free_count++;

    c->objs_in_use--;
    c->total_frees++;

    /* A previously-full slab becomes partial again. */
    if (was_full) {
        slab_list_remove(&c->full, s);
        slab_list_push(&c->partial, s);
    }

    /* Fully-empty slab: return its page to the PMM to bound memory use. */
    if (s->free_count == s->total_slots) {
        slab_list_remove(&c->partial, s);
        c->total_slabs--;
        s->magic = 0;                       /* invalidate before release */
        spin_unlock(&c->lock);
        PERF_END(PERF_OP_SLAB_FREE);
        pmm_free_page((void*)DIRECT_TO_PHYS(s));  /* s is direct-map → PMM wants phys */
        return;
    }

    spin_unlock(&c->lock);
    PERF_END(PERF_OP_SLAB_FREE);
}

/* =========================================================================
 * slab_cache_destroy — return every backing page to the PMM.
 * ========================================================================= */
void slab_cache_destroy(slab_cache_t* c) {
    if (!c) return;

    spin_lock(&c->lock);
    /* Detach both slab lists, then free outside the lock. */
    slab_t* lists[2] = { c->partial, c->full };
    c->partial = NULL;
    c->full = NULL;
    c->total_slabs = 0;
    c->objs_in_use = 0;
    spin_unlock(&c->lock);

    for (int li = 0; li < 2; li++) {
        slab_t* s = lists[li];
        while (s) {
            slab_t* next = s->next;
            s->magic = 0;
            pmm_free_page((void*)DIRECT_TO_PHYS(s));   /* direct-map → phys */
            s = next;
        }
    }

    /* Unlink from the global registry. The cache_pool slot is not reclaimed
     * (simple bump pool); a destroyed cache is just emptied and delisted. */
    spin_lock(&slab_global_lock);
    struct slab_cache** pp = &cache_registry;
    while (*pp) {
        if (*pp == c) { *pp = c->registry_next; break; }
        pp = &(*pp)->registry_next;
    }
    spin_unlock(&slab_global_lock);

    kprintf("[SLAB] cache '%s' destroyed\n", c->name ? c->name : "?");
}

/* =========================================================================
 * slab_dump — observability
 * ========================================================================= */
void slab_dump(void) {
    spin_lock(&slab_global_lock);
    struct slab_cache* c = cache_registry;
    kprintf("[SLAB] ==== cache report ====\n");
    if (!c) kprintf("[SLAB]   (no caches)\n");
    while (c) {
        /* Read stats under the cache lock for a consistent snapshot. */
        spin_lock(&c->lock);
        const char* nm  = c->name ? c->name : "?";
        size_t obj      = c->obj_size;
        uint32_t spp    = c->slots_per_slab;
        uint64_t inuse  = c->objs_in_use;
        uint64_t slabs  = c->total_slabs;
        uint64_t allocs = c->total_allocs;
        uint64_t frees  = c->total_frees;
        spin_unlock(&c->lock);

        uint64_t capacity = slabs * (uint64_t)spp;
        kprintf("[SLAB]   '%s' obj=%lu inuse=%llu/%llu slabs=%llu "
                "(allocs=%llu frees=%llu)\n",
                nm, (unsigned long)obj, inuse, capacity, slabs, allocs, frees);
        c = c->registry_next;
    }
    spin_unlock(&slab_global_lock);
}

/* =========================================================================
 * slab_selftest — boot-time correctness gate
 * =========================================================================
 * 1. Create a 64-byte cache and allocate N objects, forcing multiple slab
 *    pages (N chosen so it exceeds slots_per_slab).
 * 2. Write a unique sentinel into each object.
 * 3. Verify NO two pointers are equal AND every sentinel survived (no overlap
 *    / no clobbering between objects).
 * 4. Free all objects, re-allocate N, and verify the pool is reused (every
 *    returned pointer is one we saw on the first pass).
 * 5. Destroy the cache.
 * Logs "[SLAB] SELFTEST: PASS" or "[SLAB] SELFTEST: FAIL <reason>".
 * Returns 0 on PASS, 1 on FAIL.
 * ========================================================================= */
int slab_selftest(void) {
    enum { N = 200, OBJSZ = 64 };
    static void* ptrs[N];
    static void* ptrs2[N];

    slab_cache_t* c = slab_cache_create("selftest-64", OBJSZ, 16);
    if (!c) {
        kprintf("[SLAB] SELFTEST: FAIL cache_create returned NULL\n");
        return 1;
    }

    /* --- Phase 1: allocate N, sentinel each --- */
    for (int i = 0; i < N; i++) {
        void* p = slab_alloc(c);
        if (!p) {
            kprintf("[SLAB] SELFTEST: FAIL alloc %d returned NULL\n", i);
            slab_cache_destroy(c);
            return 1;
        }
        /* Alignment check (cache was created with align=16). */
        if ((uintptr_t)p & 0xF) {
            kprintf("[SLAB] SELFTEST: FAIL alloc %d misaligned: %p\n", i, p);
            slab_cache_destroy(c);
            return 1;
        }
        ptrs[i] = p;
        /* Fill the WHOLE object with a per-index sentinel so any overlap or
         * header-clobber between adjacent objects would corrupt a neighbor. */
        memset(p, (int)(0x40 + (i & 0x3F)), OBJSZ);
        /* Stamp the index in the first 4 bytes for an exact-identity check. */
        *(uint32_t*)p = (uint32_t)(0xA5A50000u | (uint32_t)i);
    }

    /* Forced multiple slabs? 200 * 64B objects cannot fit in one 4KB page. */
    if (c->total_slabs < 2) {
        kprintf("[SLAB] SELFTEST: FAIL expected >=2 slabs, got %llu\n",
                c->total_slabs);
        slab_cache_destroy(c);
        return 1;
    }

    /* --- Phase 2: verify distinctness + sentinel integrity --- */
    for (int i = 0; i < N; i++) {
        /* Distinct pointers: no two allocations alias. O(N^2) but N is small. */
        for (int j = i + 1; j < N; j++) {
            if (ptrs[i] == ptrs[j]) {
                kprintf("[SLAB] SELFTEST: FAIL duplicate ptr %p (%d==%d)\n",
                        ptrs[i], i, j);
                slab_cache_destroy(c);
                return 1;
            }
        }
        /* Sentinel intact: index stamp + fill byte both survive. */
        uint32_t stamp = *(uint32_t*)ptrs[i];
        if (stamp != (uint32_t)(0xA5A50000u | (uint32_t)i)) {
            kprintf("[SLAB] SELFTEST: FAIL stamp clobbered at %d: 0x%x\n",
                    i, stamp);
            slab_cache_destroy(c);
            return 1;
        }
        uint8_t expect = (uint8_t)(0x40 + (i & 0x3F));
        uint8_t* body = (uint8_t*)ptrs[i];
        for (int k = 4; k < OBJSZ; k++) {
            if (body[k] != expect) {
                kprintf("[SLAB] SELFTEST: FAIL body byte %d of obj %d: "
                        "0x%x != 0x%x\n", k, i, body[k], expect);
                slab_cache_destroy(c);
                return 1;
            }
        }
    }

    /* --- Phase 3: free ALTERNATING slots, re-alloc, verify slot reuse ---
     * We must NOT free every object first: an all-free slab is returned to the
     * PMM by design, after which a re-alloc legitimately comes from a fresh
     * page. Freeing every OTHER object instead keeps every slab PARTIALLY
     * occupied, so the freed slots stay in the cache and a re-alloc must hand
     * exactly those back. */
    int nfreed = 0;
    for (int i = 0; i < N; i += 2) { slab_free(c, ptrs[i]); nfreed++; }

    if (c->objs_in_use != (uint64_t)(N - nfreed)) {
        kprintf("[SLAB] SELFTEST: FAIL objs_in_use=%llu after freeing %d (want %d)\n",
                c->objs_in_use, nfreed, N - nfreed);
        slab_cache_destroy(c);
        return 1;
    }

    for (int i = 0; i < nfreed; i++) {
        void* p = slab_alloc(c);
        if (!p) {
            kprintf("[SLAB] SELFTEST: FAIL re-alloc %d returned NULL\n", i);
            slab_cache_destroy(c);
            return 1;
        }
        ptrs2[i] = p;
        /* Must be one of the (even-index) slots we just freed -> true reuse. */
        bool seen = false;
        for (int j = 0; j < N; j += 2) {
            if (p == ptrs[j]) { seen = true; break; }
        }
        if (!seen) {
            kprintf("[SLAB] SELFTEST: FAIL re-alloc %p not a reused slot\n", p);
            slab_cache_destroy(c);
            return 1;
        }
    }

    /* --- Phase 4: free EVERYTHING, verify the eager empty-slab reclaim --- */
    for (int i = 1; i < N; i += 2) slab_free(c, ptrs[i]);   /* live odd originals */
    for (int i = 0; i < nfreed; i++) slab_free(c, ptrs2[i]); /* the re-allocs     */
    if (c->objs_in_use != 0) {
        kprintf("[SLAB] SELFTEST: FAIL objs_in_use=%llu after freeing all\n",
                c->objs_in_use);
        slab_cache_destroy(c);
        return 1;
    }
    /* Informational: with the eager empty-slab-free policy this should be 0;
     * don't hard-fail on it (an impl that caches one empty slab is also valid). */
    if (c->total_slabs != 0)
        kprintf("[SLAB] note: %llu slab(s) still cached after free-all\n",
                c->total_slabs);
    slab_cache_destroy(c);

    kprintf("[SLAB] SELFTEST: PASS\n");
    return 0;
}
