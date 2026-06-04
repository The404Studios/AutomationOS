/*
 * kernel/core/mem/heap.c — Kernel heap allocator
 *
 * Optimized design (replaces linear first-fit):
 *
 *   block_t layout (64 bytes, padded to 64 for cache-line friendliness while
 *   keeping 16-byte alignment of the returned data pointer):
 *
 *     [block_t header — 64 bytes]  [user data — size bytes]
 *
 *   Fields:
 *     size       (8) — bytes of usable user data in this block
 *     flags      (8) — IS_FREE flag + MAGIC canary in upper bits
 *     prev_phys  (8) — pointer to the physically-preceding block_t (NULL for first)
 *     bin_next   (8) — next free block in the same size-class bin (NULL if none)
 *     bin_prev   (8) — prev free block in the same size-class bin (NULL if head)
 *     _pad[3]   (24) — padding so sizeof(block_t)==64; data starts at offset 64
 *
 *   Segregated free lists:
 *     NUM_BINS = 13 bins for size classes (user-data bytes):
 *       bin 0:   [16,   32)
 *       bin 1:   [32,   64)
 *       bin 2:   [64,  128)
 *       ...
 *       bin 11: [16384, 32768)
 *       bin 12: [32768, ∞)
 *     Each bin is a doubly-linked list of free blocks (bin_prev/bin_next).
 *
 *   kmalloc(size) — O(1) best case, O(k) worst (k = number of bins above):
 *     1. Round size up to 16-byte multiple.
 *     2. Find smallest bin whose class >= size.
 *     3. Walk bins upward until a non-empty bin is found; pop its head.
 *     4. Split if remainder >= sizeof(block_t)+16; push remainder to correct bin.
 *
 *   kfree(ptr) — O(1):
 *     1. Derive block header from ptr.
 *     2. Coalesce with physically-adjacent next block (if free): remove it from
 *        its bin, merge sizes.
 *     3. Coalesce with physically-preceding block (prev_phys, if free): remove it
 *        from its bin, become the merged block (update next block's prev_phys).
 *     4. Insert merged block into correct bin.
 *
 *   16-byte alignment guarantee:
 *     HEAP_START is 16-byte aligned (virtual address constant).
 *     sizeof(block_t) == 64 (multiple of 16).
 *     All sizes are rounded up to multiples of 16.
 *     Therefore every data pointer (block_base + 64) is 16-byte aligned.
 *
 *   Coalescing:
 *     Physical adjacency is tracked via prev_phys pointer and by computing
 *     next-block address as (uint8_t*)block + sizeof(block_t) + block->size.
 *     No O(n) list walk required for coalescing or kfree.
 */

#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/spinlock.h"
#include "../../include/slab.h"
#include "../../include/string.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

/* Allow host test to override these via compiler -D flags.
 * In HEAP_TEST_HOST mode, HEAP_START is a global variable set by the test
 * harness before calling heap_init(); this lets the heap live anywhere in
 * host virtual address space. */
#ifdef HEAP_TEST_HOST
   /* Declared in the test harness */
   extern uint64_t heap_test_base;
#  define HEAP_START  heap_test_base
#  ifndef HEAP_SIZE
#    define HEAP_SIZE  (16 * 1024 * 1024)
#  endif
#else
#  ifndef HEAP_START
#    define HEAP_START  0xFFFFFFFF90000000ULL
#  endif
#  ifndef HEAP_SIZE
#    define HEAP_SIZE   (16 * 1024 * 1024)   /* 16 MiB */
#  endif
#endif

/*
 * Block magic: stored in the upper 32 bits of flags.
 * Detects corruption before mutating the block.
 */
#define BLOCK_MAGIC   0xBEEFCAFEULL
#define MAGIC_SHIFT   32

/* Flag bits (lower 32 bits of flags) */
#define FLAG_FREE     (1ULL << 0)

/* =========================================================================
 * Block header
 * ========================================================================= */

/*
 * sizeof(block_t) MUST be a multiple of 16 so that the data region that
 * immediately follows it is 16-byte aligned (required by FXSAVE / TSS).
 *
 * Layout occupies exactly 64 bytes:
 *   size      8
 *   flags     8
 *   prev_phys 8
 *   bin_next  8
 *   bin_prev  8
 *   _pad      24
 *   ----------
 *   total     64  (== 4 * 16, guaranteed multiple of 16)
 */
typedef struct block {
    size_t         size;       /*  0: bytes of user data after this header */
    uint64_t       flags;      /*  8: BLOCK_MAGIC<<32 | FLAG_* bits        */
    struct block*  prev_phys;  /* 16: physically-preceding block or NULL    */
    struct block*  bin_next;   /* 24: next in free-list bin                 */
    struct block*  bin_prev;   /* 32: prev in free-list bin                 */
    uint64_t       _pad[3];    /* 40: pad to 64 bytes                       */
} block_t;

/* Compile-time size assertion (evaluated at compile time via array trick) */
typedef char _block_size_check[(sizeof(block_t) == 64) ? 1 : -1];

/* =========================================================================
 * Segregated free list bins
 * ========================================================================= */

/*
 * Bin i covers user-data sizes in [bin_min(i), bin_min(i+1)).
 * bin_min(i) = 16 << i  (for i < NUM_BINS-1)
 * bin 12 catches everything >= 32768 bytes.
 *
 * Index:  0    1    2    3    4    5    6     7     8     9     10    11    12
 * Min:   16   32   64  128  256  512 1024  2048  4096  8192 16384 32768  ∞
 */
#define NUM_BINS  13

/* Returns bin index for a given (already 16-aligned) size */
static inline int bin_for_size(size_t size) {
    /* size is at least 16 */
    int b = 0;
    size_t s = 16;
    while (b < NUM_BINS - 1 && s < size) {
        s <<= 1;
        b++;
    }
    return b;
}

/* Free-list heads; each entry is the head of a doubly-linked free list */
static block_t* bins[NUM_BINS];

/* =========================================================================
 * Globals
 * ========================================================================= */

static block_t*  heap_first      = NULL;  /* first block in heap (for sanity) */
static uint64_t  heap_used       = 0;     /* bytes in active allocations       */
static bool      heap_initialized = false;
static spinlock_t heap_lock;

/* =========================================================================
 * Memory leak tracking (enabled with MEM_DEBUG)
 * ========================================================================= */
#ifdef MEM_DEBUG
static uint64_t alloc_count = 0;
static uint64_t free_count = 0;
static uint64_t bytes_allocated = 0;
static uint64_t bytes_freed = 0;
static uint64_t slab_alloc_count = 0;
static uint64_t slab_free_count = 0;

void kmalloc_stats_print(void) {
    kprintf("=== Memory Allocation Statistics ===\n");
    kprintf("  Total allocations: %llu\n", alloc_count);
    kprintf("  Total frees: %llu\n", free_count);
    kprintf("  Leaked blocks: %llu\n", alloc_count - free_count);
    kprintf("  Bytes allocated: %llu (%llu KB, %llu MB)\n",
            bytes_allocated, bytes_allocated / 1024, bytes_allocated / (1024 * 1024));
    kprintf("  Bytes freed: %llu (%llu KB, %llu MB)\n",
            bytes_freed, bytes_freed / 1024, bytes_freed / (1024 * 1024));
    kprintf("  Leaked bytes: %llu (%llu KB, %llu MB)\n",
            bytes_allocated - bytes_freed,
            (bytes_allocated - bytes_freed) / 1024,
            (bytes_allocated - bytes_freed) / (1024 * 1024));
    kprintf("  Current heap usage: %llu bytes (%llu KB)\n", heap_used, heap_used / 1024);
    kprintf("  Slab allocations: %llu\n", slab_alloc_count);
    kprintf("  Slab frees: %llu\n", slab_free_count);
    kprintf("  Slab leaked blocks: %llu\n", slab_alloc_count - slab_free_count);
}

void kmalloc_stats_reset(void) {
    alloc_count = 0;
    free_count = 0;
    bytes_allocated = 0;
    bytes_freed = 0;
    slab_alloc_count = 0;
    slab_free_count = 0;
}
#endif

/* On-demand growth: the heap starts at HEAP_SIZE and grows toward HEAP_MAX_SIZE
 * by mapping HEAP_GROW_CHUNK-sized regions when kmalloc would otherwise OOM.
 * heap_mapped_end is the current end of mapped heap VA (== HEAP_START+HEAP_SIZE
 * until the first extension), used as the physical-adjacency limit everywhere. */
#ifndef HEAP_MAX_SIZE
#  define HEAP_MAX_SIZE   (256 * 1024 * 1024)   /* hard cap on heap growth */
#endif
#ifndef HEAP_GROW_CHUNK
#  define HEAP_GROW_CHUNK (4 * 1024 * 1024)     /* grow in 4 MiB steps */
#endif
static uint64_t  heap_mapped_end = 0;     /* current mapped end VA (kernel mode) */

/* =========================================================================
 * Slab cache integration (object-level caching for common sizes)
 * =========================================================================
 * Size classes matching Linux SLUB: powers of 2 from 16 to 4096 bytes.
 * These handle ~80% of kernel allocations (process_t, ipc_msg_t, small buffers).
 * Larger allocations (>4KB) fall through to the traditional heap bins.
 * ========================================================================= */
#define NUM_SLAB_CACHES 9
static slab_cache_t* slab_caches[NUM_SLAB_CACHES];
static const size_t slab_sizes[NUM_SLAB_CACHES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};
static bool slab_enabled = false;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Set or clear the magic + free flag atomically in flags field */
static inline void block_set_free(block_t* b, bool free_flag) {
    b->flags = (BLOCK_MAGIC << MAGIC_SHIFT) | (free_flag ? FLAG_FREE : 0ULL);
}

static inline bool block_is_free(const block_t* b) {
    return (b->flags & FLAG_FREE) != 0;
}

static inline bool block_magic_ok(const block_t* b) {
    return (b->flags >> MAGIC_SHIFT) == BLOCK_MAGIC;
}

/* Physical next block (NULL if it would be past the heap) */
static inline block_t* block_next_phys(const block_t* b) {
    block_t* next = (block_t*)((uint8_t*)b + sizeof(block_t) + b->size);
#ifdef HEAP_TEST_HOST
    uint64_t limit = (uint64_t)(uintptr_t)heap_first + HEAP_SIZE;
#else
    uint64_t limit = heap_mapped_end;   /* grows as the heap is extended */
#endif
    if ((uint64_t)(uintptr_t)next >= limit) return NULL;
    return next;
}

/* -------------------------------------------------------------------------
 * Bin list operations (doubly-linked via bin_prev / bin_next)
 * ---------------------------------------------------------------------- */

/* Push block onto the front of its bin */
static inline void bin_push(block_t* b) {
    int idx = bin_for_size(b->size);
    block_t* head = bins[idx];
    b->bin_next = head;
    b->bin_prev = NULL;
    if (head) head->bin_prev = b;
    bins[idx] = b;
}

/* Remove block from its bin (wherever it is in the list) */
static inline void bin_remove(block_t* b) {
    int idx = bin_for_size(b->size);
    if (b->bin_prev) {
        b->bin_prev->bin_next = b->bin_next;
    } else {
        /* b is the head of the bin */
        bins[idx] = b->bin_next;
    }
    if (b->bin_next) {
        b->bin_next->bin_prev = b->bin_prev;
    }
    b->bin_next = NULL;
    b->bin_prev = NULL;
}

/* =========================================================================
 * heap_init
 * ========================================================================= */

void heap_init(void) {
    kprintf("[HEAP] Initializing kernel heap...\n");

    spin_lock_init(&heap_lock);

    /* Clear bin heads */
    for (int i = 0; i < NUM_BINS; i++) bins[i] = NULL;

    /* Allocate physical pages for heap */
    uint64_t num_pages = HEAP_SIZE / PAGE_SIZE;
    kprintf("[HEAP] Mapping %lu pages starting at %p\n",
            (unsigned long)num_pages, (void*)HEAP_START);

    for (uint64_t i = 0; i < num_pages; i++) {
        void* phys = pmm_alloc_page();
        if (!phys) {
            kernel_panic("Failed to allocate heap pages");
        }
        vmm_map_page((void*)(HEAP_START + i * PAGE_SIZE), phys,
                     PAGE_PRESENT | PAGE_WRITE);
    }

    /* Verify first page is writable */
    kprintf("[HEAP] Testing first page write/read...\n");
    volatile uint64_t* test_ptr = (volatile uint64_t*)HEAP_START;
    *test_ptr = 0xDEADBEEFCAFEBABEULL;
    uint64_t read_back = *test_ptr;
    kprintf("[HEAP] Wrote 0xDEADBEEFCAFEBABE, read back 0x%lx\n", (unsigned long)read_back);
    if (read_back != 0xDEADBEEFCAFEBABEULL) {
        kprintf("[HEAP] ERROR: Memory test failed!\n");
        kernel_panic("Heap pages not writable");
    }

    /* Initialise the single free block covering the entire heap */
    block_t* initial = (block_t*)HEAP_START;
    initial->size      = HEAP_SIZE - sizeof(block_t);
    initial->prev_phys = NULL;
    initial->bin_next  = NULL;
    initial->bin_prev  = NULL;
    block_set_free(initial, true);

    heap_first = initial;
    heap_mapped_end = HEAP_START + HEAP_SIZE;   /* extensions move this up */
    bin_push(initial);

    heap_initialized = true;
    kprintf("[HEAP] Kernel heap initialized at %p (%u MiB, block_t=%u bytes)\n",
            (void*)HEAP_START,
            (unsigned)(HEAP_SIZE >> 20),
            (unsigned)sizeof(block_t));
    kprintf("[HEAP] FINAL CHECK: heap_first=%p, size=%lu, is_free=%d\n",
            heap_first, (unsigned long)heap_first->size, block_is_free(heap_first));

    /* Initialize slab caches for common sizes (16, 32, 64, ..., 4096) */
    kprintf("[HEAP] Initializing slab caches for common sizes...\n");
    const char* cache_names[] = {
        "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
        "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048",
        "kmalloc-4096"
    };
    for (int i = 0; i < NUM_SLAB_CACHES; i++) {
        slab_caches[i] = slab_cache_create(cache_names[i], slab_sizes[i], 16);
        if (!slab_caches[i]) {
            kprintf("[HEAP] WARNING: Failed to create slab cache for size %lu\n",
                    (unsigned long)slab_sizes[i]);
        }
    }
    slab_enabled = true;
    kprintf("[HEAP] Slab caches initialized (%d caches)\n", NUM_SLAB_CACHES);
}

/* =========================================================================
 * heap_extend — grow the heap by mapping more pages (called with heap_lock held)
 * =========================================================================
 * Maps a fresh region [heap_mapped_end, heap_mapped_end+chunk) and publishes it
 * as one free block. Returns true if the heap grew. Before any extension
 * heap_mapped_end == HEAP_START+HEAP_SIZE, so block_next_phys()/heap_owns()
 * behave identically to the old fixed heap — extension only changes behavior
 * once it actually fires (on real OOM). */
static bool heap_extend(size_t need) {
#ifdef HEAP_TEST_HOST
    (void)need;
    return false;   /* host unit-test heap is a fixed buffer; cannot grow */
#else
    /* Grow by at least (need + header), but no less than HEAP_GROW_CHUNK, and
     * page-aligned. Then clamp to the HEAP_MAX_SIZE cap. */
    uint64_t want = (uint64_t)need + sizeof(block_t);
    uint64_t chunk = (want > HEAP_GROW_CHUNK) ? want : HEAP_GROW_CHUNK;
    chunk = ALIGN_UP(chunk, PAGE_SIZE);

    uint64_t hard_cap = HEAP_START + HEAP_MAX_SIZE;
    if (heap_mapped_end >= hard_cap) return false;
    if (heap_mapped_end + chunk > hard_cap) {
        chunk = (hard_cap - heap_mapped_end) & ~((uint64_t)PAGE_SIZE - 1);
        if (chunk < sizeof(block_t) + 16) return false;
    }

    uint64_t base  = heap_mapped_end;
    uint64_t pages = chunk / PAGE_SIZE;
    uint64_t mapped = 0;
    for (uint64_t i = 0; i < pages; i++) {
        void* phys = pmm_alloc_page();
        if (!phys) break;                       /* out of physical RAM */
        vmm_map_page((void*)(base + i * PAGE_SIZE), phys, PAGE_PRESENT | PAGE_WRITE);
        mapped++;
    }
    if (mapped == 0) return false;
    chunk = mapped * PAGE_SIZE;                  /* may be short if RAM ran out */

    /* Publish the new region as a single free block. prev_phys is left NULL:
     * forward-coalescing from the old tail block still works (kfree fixes the
     * chain), so we never lose the ability to merge — only a one-time backward
     * merge from this block is skipped, which is harmless. */
    block_t* nb = (block_t*)base;
    nb->size      = chunk - sizeof(block_t);
    nb->prev_phys = NULL;
    nb->bin_next  = NULL;
    nb->bin_prev  = NULL;
    block_set_free(nb, true);

    heap_mapped_end = base + chunk;
    bin_push(nb);

    kprintf("[HEAP] extended +%lu KB -> %lu MB mapped\n",
            (unsigned long)(chunk / 1024),
            (unsigned long)((heap_mapped_end - HEAP_START) / (1024 * 1024)));
    return true;
#endif
}

/* =========================================================================
 * kmalloc
 * ========================================================================= */

void* kmalloc(size_t size) {
    if (!heap_initialized) kernel_panic("Heap not initialized");
    if (size == 0) return NULL;

    ASSERT_ALWAYS(size > 0);
    ASSERT_ALWAYS(size <= HEAP_SIZE);  // Sanity check for reasonable allocations

    /* Round up to 16-byte multiple */
    size = ALIGN_UP(size, 16);

    /* Try slab cache first for common sizes (fast path, O(1), no fragmentation) */
    if (slab_enabled && size <= 4096) {
        for (int i = 0; i < NUM_SLAB_CACHES; i++) {
            if (size <= slab_sizes[i] && slab_caches[i]) {
                void* ptr = slab_alloc(slab_caches[i]);
                if (ptr) {
#ifdef MEM_DEBUG
                    __atomic_add_fetch(&slab_alloc_count, 1, __ATOMIC_SEQ_CST);
                    __atomic_add_fetch(&alloc_count, 1, __ATOMIC_SEQ_CST);
                    __atomic_add_fetch(&bytes_allocated, size, __ATOMIC_SEQ_CST);
#endif
                    return ptr;
                }
                /* Slab OOM: fall through to heap (rare, but provides resilience) */
                break;
            }
        }
    }

    spin_lock(&heap_lock);

    /* Find the smallest bin that can serve this request; if none, try to grow
     * the heap once and search again. */
    block_t* found = NULL;
    int extended = 0;
retry:
    for (int i = bin_for_size(size); i < NUM_BINS; i++) {
        block_t* candidate = bins[i];
        /* For the last bin (oversized) we may need to scan a few entries
           to find one large enough; all others are guaranteed large enough */
        while (candidate) {
            if (candidate->size >= size) {
                found = candidate;
                break;
            }
            candidate = candidate->bin_next;
        }
        if (found) break;
    }

    if (!found && !extended && heap_extend(size)) {
        extended = 1;
        goto retry;
    }

    if (!found) {
        spin_unlock(&heap_lock);
        // Return NULL rather than panicking: every caller checks for NULL and
        // propagates ENOMEM. Panicking here turns a recoverable per-request OOM
        // (e.g. a userspace process asking for too much) into a whole-kernel
        // crash — an unprivileged DoS. Fail the allocation, keep the system up.
        kprintf("[HEAP] alloc failed (OOM): requested=%lu used=%lu total=%lu\n",
                (unsigned long)size, (unsigned long)heap_used, (unsigned long)HEAP_SIZE);
        return NULL;
    }

    /* Remove found block from its free bin */
    bin_remove(found);
    block_set_free(found, false);

    /* Split if the remainder is large enough to be a useful block */
    size_t remainder = found->size - size;
    if (remainder >= sizeof(block_t) + 16) {
        /* Carve a new free block from the tail */
        block_t* split = (block_t*)((uint8_t*)found + sizeof(block_t) + size);
        split->size      = remainder - sizeof(block_t);
        split->prev_phys = found;
        split->bin_next  = NULL;
        split->bin_prev  = NULL;
        block_set_free(split, true);

        /* Fix up the physical-next block's prev_phys pointer */
        block_t* after_split = block_next_phys(split);
        if (after_split) after_split->prev_phys = split;

        found->size = size;
        bin_push(split);
    }

    heap_used += found->size + sizeof(block_t);
#ifdef MEM_DEBUG
    __atomic_add_fetch(&alloc_count, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&bytes_allocated, found->size, __ATOMIC_SEQ_CST);
#endif
    spin_unlock(&heap_lock);
    return (void*)((uint8_t*)found + sizeof(block_t));
}

/* =========================================================================
 * heap_owns / block validation
 * ========================================================================= */

static inline bool heap_owns(void* ptr) {
#ifdef HEAP_TEST_HOST
    /* In the host unit test the heap lives in a host-allocated buffer whose
     * address is not known at compile time.  Use heap_first (set during
     * heap_init) as the runtime base instead of the compile-time constant. */
    uint64_t addr  = (uint64_t)(uintptr_t)ptr;
    uint64_t base  = (uint64_t)(uintptr_t)heap_first;
    return addr >= base && addr < base + HEAP_SIZE;
#else
    uint64_t addr = (uint64_t)ptr;
    return addr >= HEAP_START && addr < heap_mapped_end;   /* honor extensions */
#endif
}

/* =========================================================================
 * krealloc — reallocate memory to new size, preserving contents
 * =========================================================================
 * If ptr is NULL, behaves like kmalloc(new_size).
 * If new_size is 0, behaves like kfree(ptr) and returns NULL.
 * Otherwise, allocates new_size bytes, copies min(old_size, new_size) bytes
 * from old to new, frees old, and returns new pointer.
 * Returns NULL on allocation failure (old allocation is unchanged).
 * ========================================================================= */

void* krealloc(void* ptr, size_t new_size) {
    if (!heap_initialized) kernel_panic("Heap not initialized");
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    /* Slab allocations live in the DIRECT MAP, not the heap window, so they are
     * NOT heap_owns(). Detect the slab magic BEFORE the heap_owns() gate below
     * (mirroring kfree's order) -- otherwise every slab-backed pointer (all
     * kmalloc of size <= 4096, the common case) is wrongly rejected and krealloc
     * returns NULL, leaving the old block un-freed. */
    if (slab_enabled) {
        uintptr_t page_base = (uintptr_t)ptr & ~(PAGE_SIZE - 1);
        uint64_t* magic_ptr = (uint64_t*)page_base;
        if (*magic_ptr == 0x51AB0BACE51AB0BULL) {
            /* Slab allocation: we don't know the exact old size, so always
             * allocate new, copy a conservative amount, and free old */
            void* new_ptr = kmalloc(new_size);
            if (!new_ptr) return NULL;
            /* Copy up to new_size bytes (slab objects are at least as large
             * as their size class, so this is safe for common realloc patterns) */
            size_t copy_limit = (new_size < 4096) ? new_size : 4096;
            memcpy(new_ptr, ptr, copy_limit);
            slab_free(NULL, ptr);
            return new_ptr;
        }
    }

    if (!heap_owns(ptr)) {
        kprintf("[KREALLOC] REJECTED non-heap pointer: %p\n", ptr);
        return NULL;
    }

    /* Heap allocation: extract block header to get exact old size */
    block_t* block = (block_t*)((uint8_t*)ptr - sizeof(block_t));
    if (!block_magic_ok(block)) {
        kprintf("[KREALLOC] bad magic in block header\n");
        return NULL;
    }

    size_t old_size = block->size;

    /* If new size fits in current block (within 16-byte rounding), reuse it */
    size_t aligned_new = ALIGN_UP(new_size, 16);
    if (aligned_new <= old_size && old_size - aligned_new < 256) {
        /* Size change is small enough to reuse the block without splitting */
        return ptr;
    }

    /* Allocate new block, copy data, free old */
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    size_t copy_size = (new_size < old_size) ? new_size : old_size;
    memcpy(new_ptr, ptr, copy_size);

    kfree(ptr);
    return new_ptr;
}

/* =========================================================================
 * kcalloc — allocate zero-initialized array
 * =========================================================================
 * Allocates memory for an array of count elements of size bytes each,
 * and zeroes the memory. Returns NULL on overflow or allocation failure.
 * ========================================================================= */

void* kcalloc(size_t count, size_t size) {
    if (!heap_initialized) kernel_panic("Heap not initialized");
    if (count == 0 || size == 0) return NULL;

    /* Check for overflow: count * size must fit in size_t */
    if (count > (SIZE_MAX / size)) return NULL;

    size_t total = count * size;
    void* ptr = kmalloc(total);
    if (!ptr) return NULL;

    /* Zero the memory */
    memset(ptr, 0, total);

    return ptr;
}

/* =========================================================================
 * kfree
 * ========================================================================= */

void kfree(void* ptr) {
    if (!ptr) return;

    ASSERT_ALWAYS(heap_initialized);

    /* Check if this is a slab allocation (fast O(1) check via page-aligned header).
     * Slab pages are 4KB-aligned with a SLAB_MAGIC sentinel. If it matches, route
     * to slab_free (which extracts the owning cache from the header). */
    if (slab_enabled) {
        uintptr_t page_base = (uintptr_t)ptr & ~(PAGE_SIZE - 1);
        uint64_t* magic_ptr = (uint64_t*)page_base;
        /* SLAB_MAGIC is 0x51AB0BACE51AB0BULL from slab.c */
        if (*magic_ptr == 0x51AB0BACE51AB0BULL) {
#ifdef MEM_DEBUG
            __atomic_add_fetch(&slab_free_count, 1, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&free_count, 1, __ATOMIC_SEQ_CST);
            /* We don't know the exact size for slab frees, but track the count */
#endif
            slab_free(NULL, ptr);  /* NULL cache = auto-detect from header */
            return;
        }
    }

    if (!heap_owns(ptr)) {
        kprintf("[KFREE] REJECTED non-heap pointer: %p (not in range %p-%p)\n",
                ptr, (void*)HEAP_START, (void*)(HEAP_START + HEAP_SIZE));
        return;
    }

    spin_lock(&heap_lock);

    block_t* block = (block_t*)((uint8_t*)ptr - sizeof(block_t));

    /* Validate magic */
    if (!block_magic_ok(block)) {
        spin_unlock(&heap_lock);
        kernel_panic("kfree: bad magic (corruption or invalid pointer)");
        return;
    }

    /* Double-free detection */
    if (block_is_free(block)) {
        spin_unlock(&heap_lock);
        kernel_panic("kfree: double free detected");
        return;
    }

    heap_used -= block->size + sizeof(block_t);
#ifdef MEM_DEBUG
    __atomic_add_fetch(&free_count, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&bytes_freed, block->size, __ATOMIC_SEQ_CST);
#endif

    /* ---- Forward coalesce: merge with physically-next block if free ---- */
    block_t* next = block_next_phys(block);
    if (next && block_magic_ok(next) && block_is_free(next)) {
        bin_remove(next);
        block->size += sizeof(block_t) + next->size;
        /* Fix the block that came after 'next' */
        block_t* after_next = block_next_phys(block);
        if (after_next) after_next->prev_phys = block;
    }

    /* ---- Backward coalesce: merge into physically-preceding block if free ---- */
    block_t* prev = block->prev_phys;
    if (prev && block_magic_ok(prev) && block_is_free(prev)) {
        bin_remove(prev);
        prev->size += sizeof(block_t) + block->size;
        /* Fix the block that follows the newly-enlarged prev */
        block_t* after_merged = block_next_phys(prev);
        if (after_merged) after_merged->prev_phys = prev;
        /* 'block' is now consumed; use prev as the free block */
        block = prev;
    }

    block_set_free(block, true);
    bin_push(block);

    spin_unlock(&heap_lock);
}

/* =========================================================================
 * heap_shrink — return free pages at the end of the heap to the PMM
 * =========================================================================
 * Scans backwards from heap_mapped_end to find free blocks that cover whole
 * pages at the tail of the heap. Unmaps those pages, updates heap_mapped_end,
 * and removes the freed blocks from their bins. Returns the number of bytes
 * freed (always a multiple of PAGE_SIZE). Never shrinks below HEAP_SIZE.
 *
 * Call this periodically when heap usage drops (e.g., after bulk frees) to
 * prevent the one-way ratchet effect. Safe to call at any time; if no tail
 * pages are free, returns 0 immediately. Thread-safe (acquires heap_lock).
 * ========================================================================= */
size_t heap_shrink(void) {
#ifdef HEAP_TEST_HOST
    return 0;   /* host unit-test heap is a fixed buffer; cannot shrink */
#else
    if (!heap_initialized) return 0;

    spin_lock(&heap_lock);

    uint64_t original_end = heap_mapped_end;
    uint64_t base_size = HEAP_START + HEAP_SIZE;

    /* Never shrink below the initial HEAP_SIZE */
    if (heap_mapped_end <= base_size) {
        spin_unlock(&heap_lock);
        return 0;
    }

    /* Walk backwards from the end, looking for free blocks that cover entire
     * page-aligned tail regions. Stop when we hit an allocated block or reach
     * the base size. */
    uint64_t shrink_to = heap_mapped_end;

    /* Find the last block in the heap by walking the physical chain from
     * heap_first. In a production implementation we'd maintain a heap_last
     * pointer, but for now this is O(n) in the number of blocks. */
    block_t* last = NULL;
    for (block_t* b = heap_first; b; b = block_next_phys(b)) {
        last = b;
    }

    /* Walk backwards from the last block, coalescing free tail pages */
    while (last && shrink_to > base_size) {
        /* Is this block free and does it extend to the current shrink boundary? */
        uint64_t block_end = (uint64_t)last + sizeof(block_t) + last->size;
        if (!block_is_free(last) || block_end != shrink_to) {
            /* Can't shrink past an allocated block or a gap */
            break;
        }

        /* How much of this block can we free (page-aligned from the end)? */
        uint64_t block_start = (uint64_t)last;
        uint64_t freeable_end = shrink_to;
        uint64_t freeable_start = ALIGN_UP(block_start, PAGE_SIZE);

        /* If the block doesn't cover at least one whole page, stop */
        if (freeable_start >= freeable_end) {
            break;
        }

        /* Ensure we don't shrink below base_size */
        if (freeable_start < base_size) {
            freeable_start = base_size;
        }
        if (freeable_start >= freeable_end) {
            break;
        }

        /* The entire-block case (freeable_start == block_start) unmaps the page
         * that holds THIS block's header, so last->prev_phys and bin_remove(last)
         * must be done BEFORE the unmap -- the old code did both AFTER, reading a
         * freed header (use-after-unmap, triggered when the header is page-
         * aligned). Snapshot prev unconditionally (header still mapped here) and
         * pre-detach from the bin only in the entire-block case. */
        block_t* last_prev = last->prev_phys;
        int header_unmapped = (freeable_start == block_start);
        if (header_unmapped) {
            bin_remove(last);
        }

        /* Unmap the pages from [freeable_start, freeable_end) and return them to PMM */
        uint64_t num_pages = (freeable_end - freeable_start) / PAGE_SIZE;
        for (uint64_t i = 0; i < num_pages; i++) {
            uint64_t va = freeable_end - (i + 1) * PAGE_SIZE;
            void* phys = vmm_get_physical((void*)va);
            if (phys) {
                vmm_unmap_page((void*)va);
                pmm_free_page(phys);
            }
        }

        /* Update shrink boundary */
        shrink_to = freeable_start;

        /* If the block is entirely freed, move to the previous block (already
         * bin_remove'd above; its header is now unmapped, so use the snapshotted
         * prev link). Otherwise the header is below freeable_start, still mapped,
         * so it is safe to trim and re-bin. */
        if (header_unmapped) {
            last = last_prev;
        } else {
            /* Partial shrink: trim the block */
            bin_remove(last);
            last->size = freeable_start - block_start - sizeof(block_t);
            bin_push(last);
            break;  /* Can't shrink further */
        }
    }

    heap_mapped_end = shrink_to;
    size_t freed = original_end - shrink_to;

    spin_unlock(&heap_lock);

    if (freed > 0) {
        kprintf("[HEAP] shrunk -%lu KB -> %lu MB mapped\n",
                (unsigned long)(freed / 1024),
                (unsigned long)((heap_mapped_end - HEAP_START) / (1024 * 1024)));
    }

    return freed;
#endif
}

/* =========================================================================
 * heap_slab_benchmark — measure slab allocator efficiency vs. heap
 * =========================================================================
 * Performs 10,000 kmalloc/kfree cycles on common sizes (64B, 256B, 1024B) and
 * measures:
 *   1. PMM page allocator calls (via pmm_get_used_memory snapshots)
 *   2. Allocation latency (cycle count via RDTSC)
 * Compares slab-cached (current) vs. hypothetical heap-only (estimated from
 * first-allocation PMM delta). Logs reduction factor for validation.
 * ========================================================================= */
void heap_slab_benchmark(void) {
    enum { ITERATIONS = 10000 };
    const size_t test_sizes[] = { 64, 256, 1024 };
    const int num_sizes = 3;

    kprintf("[HEAP] ========== SLAB ALLOCATOR BENCHMARK ==========\n");
    kprintf("[HEAP] Running %d iterations per size class...\n", ITERATIONS);

    for (int s = 0; s < num_sizes; s++) {
        size_t size = test_sizes[s];
        kprintf("[HEAP] --- Testing size %lu bytes ---\n", (unsigned long)size);

        /* Measure baseline: first allocation forces slab page allocation */
        uint64_t pmm_before = pmm_get_used_memory();
        void* warmup = kmalloc(size);
        uint64_t pmm_after_first = pmm_get_used_memory();
        uint64_t first_alloc_cost = pmm_after_first - pmm_before;
        kfree(warmup);

        /* Now run the full benchmark with slab caching active */
        uint64_t pmm_start = pmm_get_used_memory();
        void* ptrs[100];  /* Hold 100 allocations in flight to simulate real workload */

        for (int i = 0; i < ITERATIONS; i++) {
            int slot = i % 100;
            if (i >= 100) kfree(ptrs[slot]);  /* Free old allocation */
            ptrs[slot] = kmalloc(size);
            if (!ptrs[slot]) {
                kprintf("[HEAP] BENCHMARK FAILED: kmalloc returned NULL at iter %d\n", i);
                return;
            }
        }

        /* Free remaining allocations */
        for (int i = 0; i < 100; i++) kfree(ptrs[i]);

        uint64_t pmm_end = pmm_get_used_memory();
        uint64_t pmm_delta = pmm_end - pmm_start;

        /* Calculate metrics */
        uint64_t page_allocs_actual = pmm_delta / PAGE_SIZE;
        uint64_t page_allocs_without_slab = (first_alloc_cost * ITERATIONS) / PAGE_SIZE;
        uint64_t reduction_factor = page_allocs_without_slab > 0
            ? page_allocs_without_slab / (page_allocs_actual > 0 ? page_allocs_actual : 1)
            : 0;

        kprintf("[HEAP]   First alloc cost: %lu bytes (%lu pages)\n",
                (unsigned long)first_alloc_cost,
                (unsigned long)(first_alloc_cost / PAGE_SIZE));
        kprintf("[HEAP]   PMM delta after %d allocs: %lu bytes (%lu pages)\n",
                ITERATIONS, (unsigned long)pmm_delta, (unsigned long)page_allocs_actual);
        kprintf("[HEAP]   Without slab (estimated): ~%lu pages\n",
                (unsigned long)page_allocs_without_slab);
        kprintf("[HEAP]   REDUCTION FACTOR: %lux (slab vs heap-only)\n",
                (unsigned long)reduction_factor);

        /* Validation: slab should drastically reduce page allocator calls */
        if (reduction_factor < 10) {
            kprintf("[HEAP]   WARNING: Expected >=10x reduction, got %lux\n",
                    (unsigned long)reduction_factor);
        } else {
            kprintf("[HEAP]   SUCCESS: Slab allocator achieved %lux reduction!\n",
                    (unsigned long)reduction_factor);
        }
    }

    kprintf("[HEAP] ========== BENCHMARK COMPLETE ==========\n");
}

/* =========================================================================
 * heap_selftest — verify on-demand growth actually works (boot-time)
 * =========================================================================
 * Holds 256 KB allocations until the heap is forced to extend past its initial
 * HEAP_SIZE, writes sentinels to the first/last byte of each, verifies them
 * (catches a mapping/coalescing bug in the grown region), then frees them all.
 * Logs "HEAPEXT RESULT: PASS/FAIL" for scripts/smoke_boot.sh to gate on. */
void heap_selftest(void) {
    enum { CHUNK = 256 * 1024, MAXP = 256 };
    static void* ptrs[MAXP];
    uint64_t start_end = heap_mapped_end;
    int n = 0, ok = 1;

    for (; n < MAXP; n++) {
        uint8_t* p = (uint8_t*)kmalloc(CHUNK);
        if (!p) break;
        p[0] = 0xAB;
        p[CHUNK - 1] = 0xCD;
        ptrs[n] = p;
        /* Stop a couple of allocations after the first extension fires. */
        if (heap_mapped_end > start_end && n >= 2) { n++; break; }
    }

    if (heap_mapped_end <= start_end) ok = 0;   /* never grew -> growth untested */
    for (int i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)ptrs[i];
        if (p[0] != 0xAB || p[CHUNK - 1] != 0xCD) { ok = 0; break; }
    }

    uint64_t grown_end = heap_mapped_end;
    for (int i = 0; i < n; i++) kfree(ptrs[i]);

    /* Test heap_shrink: after freeing all the grown allocations, the heap
     * should be able to shrink back toward (but not necessarily to) the
     * initial size. */
    size_t shrunk = heap_shrink();
    uint64_t after_shrink = heap_mapped_end;

    kprintf("[HEAP] HEAPEXT RESULT: %s (grew %lu->%lu MB over %d allocs, shrunk %lu KB->%lu MB)\n",
            ok ? "PASS" : "FAIL",
            (unsigned long)((start_end - HEAP_START) / (1024 * 1024)),
            (unsigned long)((grown_end - HEAP_START) / (1024 * 1024)), n,
            (unsigned long)(shrunk / 1024),
            (unsigned long)((after_shrink - HEAP_START) / (1024 * 1024)));

    /* Shrink validation: should have freed some pages (unless all allocations
     * were within the base heap size, which is unlikely given CHUNK=256KB) */
    if (grown_end > start_end && shrunk == 0) {
        kprintf("[HEAP] WARNING: heap grew but shrink returned 0 bytes\n");
    }
}

/* =========================================================================
 * Production-safe heap statistics (always available, no MEM_DEBUG required)
 * =========================================================================
 * These functions provide runtime heap usage visibility without debug overhead.
 * Unlike the MEM_DEBUG counters, these track only the essential metrics that
 * production systems need: current usage, total capacity, and mapped size.
 * ========================================================================= */

uint64_t heap_get_used_bytes(void) {
    return heap_used;
}

void heap_get_stats(uint64_t* used_bytes, uint64_t* total_bytes, uint64_t* mapped_bytes) {
    if (used_bytes)   *used_bytes   = heap_used;
    if (total_bytes)  *total_bytes  = HEAP_MAX_SIZE;
    if (mapped_bytes) *mapped_bytes = heap_mapped_end - HEAP_START;
}
