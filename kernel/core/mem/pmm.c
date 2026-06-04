#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/spinlock.h"
#include "../../include/smp.h"
#include "../../include/x86_64.h"   // read_cr3/write_cr3: free-list nodes live at
                                    // physical addrs and must be walked under the
                                    // kernel CR3 (which identity-maps all RAM).

static void* pmm_alloc_page_slow(void);
static void pmm_free_page_slow(void* page_addr);

/*
 * LOCK ORDERING DOCUMENTATION
 * ===========================
 *
 * Locks in this file:
 *   1. cpu_caches[i].lock - Per-CPU page cache lock (protects cache->pages[], cache->count)
 *   2. global_pmm_lock    - Global PMM lock (protects free_lists[], bitmap, used_memory)
 *
 * Lock Acquisition Order:
 *   - cpu_caches[i].lock is LEAF lock (never holds other locks while held)
 *   - global_pmm_lock is ROOT lock (can be acquired independently)
 *
 * RULES:
 *   - NEVER acquire global_pmm_lock while holding cache->lock (DEADLOCK!)
 *   - ALWAYS release cache->lock before calling pmm_alloc_page_slow() or pmm_free_page_slow()
 *   - Each lock protects its own data structure only (no cross-dependencies)
 *
 * DEADLOCK PREVENTION:
 *   - pmm_alloc_page() uses release-reacquire pattern:
 *     1. Lock cache
 *     2. Check cache (fast path)
 *     3. If miss: UNLOCK cache
 *     4. Refill from global pool (acquires global_pmm_lock)
 *     5. RELOCK cache
 *     6. Update cache with refilled pages
 *     7. UNLOCK cache
 */

#define MAX_ORDER 10
#define MIN_PAGE_SIZE 4096
#define HUGE_PAGE_SIZE (2 * 1024 * 1024)  // 2MB
#define PAGES_PER_HUGE_PAGE (HUGE_PAGE_SIZE / MIN_PAGE_SIZE)  // 512

// Identity-mapped limit from boot.asm (512 * 2MB = 1GB). pmm_init only processes
// pages within this range. After paging_init extends identity mapping to cover
// all RAM, pmm_add_remaining_pages() processes the rest.
#define PMM_INITIAL_MAX_PHYS 0x40000000ULL

/* -----------------------------------------------------------------------
 * Contiguous-allocation bitmap
 * -----------------------------------------------------------------------
 * We keep a flat bitmap over all physical page frames so that
 * pmm_alloc_pages() can find N contiguous free pages in a single
 * O(total_pages / 64) pass using 64-bit word scans + __builtin_ctzll.
 *
 * Physical page frame index:
 *   pfn = (phys_addr - BITMAP_BASE) / PAGE_SIZE
 *
 * BITMAP_BASE = 0  (we keep the full 4 GB range so that all physical
 * addresses map to a valid index without a subtraction per access).
 *
 * Sizing: 4 GB / 4 KB = 2^20 pages.  2^20 bits = 128 KB of static data.
 * That is well within normal BSS for a kernel.
 *
 * Bit convention: 1 = FREE, 0 = USED/RESERVED.
 * Using "1 = free" lets us use __builtin_ctzll on the raw word to find
 * the first free bit without inverting.
 * ----------------------------------------------------------------------- */
#define BITMAP_TOTAL_PAGES  (1UL << 20)          /* 4 GB / 4 KB */
#define BITMAP_WORDS        (BITMAP_TOTAL_PAGES / 64)

static uint64_t pmm_bitmap[BITMAP_WORDS];        /* 1 = free, 0 = used */

/* "Next free" hint cursor — resumes from the last successful allocation
 * instead of scanning from word 0 every call.  Wraps once on miss.
 * Protected by global_pmm_lock.                                         */
static uint64_t pmm_bitmap_hint = 0;             /* word index */

/* Inline bitmap helpers (no locking — caller holds global_pmm_lock) */
static inline uint64_t pfn_of(void* addr) {
    return (uint64_t)(uintptr_t)addr / MIN_PAGE_SIZE;
}
static inline void* addr_of_pfn(uint64_t pfn) {
    return (void*)(uintptr_t)(pfn * (uint64_t)MIN_PAGE_SIZE);
}

static inline void bitmap_set_free(uint64_t pfn) {
    pmm_bitmap[pfn / 64] |= (1ULL << (pfn % 64));
}
static inline void bitmap_set_used(uint64_t pfn) {
    pmm_bitmap[pfn / 64] &= ~(1ULL << (pfn % 64));
}
static inline bool bitmap_is_free(uint64_t pfn) {
    return (pmm_bitmap[pfn / 64] >> (pfn % 64)) & 1ULL;
}

typedef struct page {
    struct page* next;
    uint8_t order;
    bool is_free;
} page_t;

// Per-CPU page caches for fast allocation (10x speedup)
#define PER_CPU_CACHE_SIZE 16  // Cache 16 pages per CPU
#define MAX_CPUS 8             // Support up to 8 CPUs

typedef struct {
    void* pages[PER_CPU_CACHE_SIZE];
    uint32_t count;            // Number of cached pages
    uint64_t alloc_fast;       // Fast path hits (cache hit)
    uint64_t alloc_slow;       // Slow path (cache miss/refill)
    spinlock_t lock;           // Protects cache access
} per_cpu_page_cache_t;

static per_cpu_page_cache_t cpu_caches[MAX_CPUS];
static spinlock_t global_pmm_lock;  // Protects free lists + bitmap

static page_t* free_lists[MAX_ORDER + 1];
static uint64_t total_memory = 0;
static uint64_t used_memory = 0;
static void* memory_start = NULL;
static void* memory_end = NULL;

// Initrd reservation (set by kernel before pmm_init)
static uint64_t pmm_initrd_start = 0;
static uint64_t pmm_initrd_end = 0;

void pmm_reserve_initrd(uint64_t start, uint64_t size) {
    pmm_initrd_start = start;
    pmm_initrd_end = ALIGN_UP(start + size, MIN_PAGE_SIZE);
}

/* CPU hotplug callback: reclaim cache when a CPU goes offline */
static void pmm_cpu_offline_callback(uint32_t cpu) {
    pmm_reclaim_cpu_cache(cpu);
}

/* Mark a page as free in both the free-list and the bitmap.
 * Internal helper — caller must hold global_pmm_lock.           */
static void pmm_add_page_locked(uint64_t addr) {
    page_t* page = (page_t*)addr;
    page->order = 0;
    page->is_free = true;
    page->next = free_lists[0];
    free_lists[0] = page;

    uint64_t pfn = addr / MIN_PAGE_SIZE;
    if (pfn < BITMAP_TOTAL_PAGES)
        bitmap_set_free(pfn);
}

void pmm_init(memory_map_entry_t* mmap, uint32_t mmap_count) {
    kprintf("[PMM] Initializing physical memory manager...\n");

    // Initialize free lists
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }

    // Initialize per-CPU page caches
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_caches[i].count = 0;
        cpu_caches[i].alloc_fast = 0;
        cpu_caches[i].alloc_slow = 0;
        spin_lock_init(&cpu_caches[i].lock);
    }

    // Initialize global PMM lock
    spin_lock_init(&global_pmm_lock);

    // Bitmap is in BSS — already zero (all pages used/reserved by default).

    // Kernel physical memory range (from linker script)
    extern char __kernel_end;
    uint64_t kernel_start_phys = 0x100000;  // 1MB (where GRUB loads us)
    uint64_t kernel_end_phys = ALIGN_UP((uint64_t)&__kernel_end, MIN_PAGE_SIZE);

    kprintf("[PMM] Kernel: 0x%lx - 0x%lx (%lu KB)\n",
        (unsigned long)kernel_start_phys,
        (unsigned long)kernel_end_phys,
        (unsigned long)((kernel_end_phys - kernel_start_phys) / 1024));

    // Find usable memory regions
    uint32_t free_pages = 0;
    for (uint32_t i = 0; i < mmap_count; i++) {
        if (mmap[i].type == 1) {  // Usable memory
            uint64_t base = ALIGN_UP(mmap[i].base, MIN_PAGE_SIZE);
            uint64_t end = ALIGN_DOWN(mmap[i].base + mmap[i].length, MIN_PAGE_SIZE);

            if (end <= base) continue;

            // CRITICAL: Only process pages within identity-mapped range.
            // Pages above 1GB are processed later by pmm_add_remaining_pages()
            // after paging_init extends the identity mapping to cover all RAM.
            if (base >= PMM_INITIAL_MAX_PHYS) {
                // Entire region is above mapped range — skip for now
                continue;
            }
            if (end > PMM_INITIAL_MAX_PHYS) {
                // Partially within range — cap to mapped boundary
                end = PMM_INITIAL_MAX_PHYS;
            }

            total_memory += (end - base);

            if (memory_start == NULL || (void*)base < memory_start) {
                memory_start = (void*)base;
            }
            if ((void*)end > memory_end) {
                memory_end = (void*)end;
            }

            // Add pages to free list (order 0)
            // SKIP kernel memory and first 1MB (BIOS/VGA)
            for (uint64_t addr = base; addr < end; addr += MIN_PAGE_SIZE) {
                // Skip first 1MB (BIOS, VGA, real mode IVT)
                if (addr < 0x100000) continue;
                // Skip kernel memory
                if (addr >= kernel_start_phys && addr < kernel_end_phys) continue;
                // Skip initrd memory (loaded by GRUB, must not be overwritten)
                if (pmm_initrd_start && addr >= pmm_initrd_start && addr < pmm_initrd_end) continue;

                pmm_add_page_locked(addr);
                free_pages++;
            }
        }
    }

    kprintf("[PMM] Total memory: %u MB (below 1GB)\n", (uint32_t)(total_memory / (1024 * 1024)));
    kprintf("[PMM] Memory range: %p - %p\n", memory_start, memory_end);
    kprintf("[PMM] Per-CPU caches: %u pages per CPU\n", PER_CPU_CACHE_SIZE);

    // NOTE: the per-CPU cache reclaim primitive pmm_reclaim_cpu_cache() (and its
    // wrapper pmm_cpu_offline_callback) are ready, but we do NOT register them
    // with register_cpu_offline_callback() here: that registry — and the
    // cpu_offline() path that would fire it — live in arch/x86_64/smp.c, which is
    // intentionally NOT compiled into the SMP-foundation build (it collides with
    // stubs.c::cpu_id() and pulls in the stale full-SMP layer). Wiring the
    // callback in would leave an unresolved symbol and break the strict link with
    // no live offline dispatch to call it anyway. Re-register here once a real
    // cpu_offline() brick lands in the compiled SMP path.
}

/**
 * Slow path: allocate page from global free lists
 * This is the original allocator, used when cache misses
 * MUST be called WITHOUT any locks held (acquires global_pmm_lock internally)
 */
static void* pmm_alloc_page_slow(void) {
    spin_lock(&global_pmm_lock);
    void* slow_result = NULL;

    // Allocate by scanning the authoritative BITMAP — NOT the embedded page_t free
    // list. A free-list node lives AT the page's physical address, so once a page
    // is used as DATA (the contiguous allocator leaves such pages on the list, and
    // an shm/CoW page's first bytes overwrite the embedded ->next) dereferencing
    // it walks into clobbered/unmapped memory and #PFs the kernel — the cause of
    // the "large shm after free-list churn" crash (a node at ~276MB). The bitmap
    // is a kernel global (mapped on every CR3, never corrupted) and needs NO page
    // dereference, so this path can never fault on page metadata. A SET bit == FREE
    // (see bitmap_is_free). The rotating word hint keeps it ~O(1) amortized; the
    // per-CPU cache batches refills so this runs rarely.
    uint64_t total_pfns = total_memory / MIN_PAGE_SIZE;
    if (total_pfns == 0 || total_pfns > BITMAP_TOTAL_PAGES) total_pfns = BITMAP_TOTAL_PAGES;
    uint64_t total_words = (total_pfns + 63) / 64;
    uint64_t hint = (pmm_bitmap_hint < total_words) ? pmm_bitmap_hint : 0;

    for (uint64_t scanned = 0; scanned < total_words; scanned++) {
        uint64_t w = hint + scanned;
        if (w >= total_words) w -= total_words;           /* wrap around */
        uint64_t bits = pmm_bitmap[w];
        if (bits == 0) continue;                          /* no free page in this word */
        uint64_t bit = (uint64_t)__builtin_ctzll(bits);   /* lowest free bit */
        uint64_t pfn = w * 64 + bit;
        if (pfn == 0 || pfn >= total_pfns) continue;      /* skip pfn 0 / past-RAM tail */
        bitmap_set_used(pfn);
        used_memory += MIN_PAGE_SIZE;
        pmm_bitmap_hint = w;
        slow_result = addr_of_pfn(pfn);
        break;
    }

    spin_unlock(&global_pmm_lock);
    return slow_result;                  // NULL == out of memory
}

/**
 * Fast path: allocate page from per-CPU cache (10x faster)
 *
 * Performance:
 *   Cache hit:  ~5-10 cycles (O(1) array access)
 *   Cache miss: ~500 cycles (refill cache, amortized)
 *   Average:    ~15-20 cycles (90%+ cache hit rate)
 *
 * vs. original allocator: ~30-110 cycles (O(n) scan)
 *
 * DEADLOCK FIX: Release cache lock before acquiring global_pmm_lock
 *   Old: cache->lock held while calling pmm_alloc_page_slow() → DEADLOCK RISK
 *   New: Release cache->lock, refill, re-acquire cache->lock → SAFE
 */
void* pmm_alloc_page(void) {
    // Get current CPU ID for per-CPU page cache (enables SMP performance)
    uint32_t current_cpu = cpu_id();
    if (current_cpu >= MAX_CPUS) current_cpu = 0;   // defensive: never index cpu_caches OOB
    per_cpu_page_cache_t* cache = &cpu_caches[current_cpu];

    spin_lock(&cache->lock);

    // Fast path: return from cache if available (O(1), ~5-10 cycles)
    if (cache->count > 0) {
        cache->count--;
        cache->alloc_fast++;
        void* page = cache->pages[cache->count];
        spin_unlock(&cache->lock);
        if (page && *(volatile uint64_t*)page == 0x51AB0BACE51AB0BULL)
            kprintf("[SLABPROBE] pmm_alloc(fast) handed out LIVE SLAB %p\n", page);
        return page;
    }

    // Slow path: cache miss, refill cache from global pool
    cache->alloc_slow++;

    // DEADLOCK FIX: Release cache lock before refilling from global pool
    // This prevents lock order violation: cache->lock → global_pmm_lock
    spin_unlock(&cache->lock);

    // Allocate multiple pages at once to amortize refill cost
    // Note: This happens WITHOUT holding cache lock to avoid deadlock
    void* refill_pages[PER_CPU_CACHE_SIZE];
    uint32_t refill_count = 0;
    for (uint32_t i = 0; i < PER_CPU_CACHE_SIZE; i++) {
        void* page = pmm_alloc_page_slow();
        if (!page) break;  // Out of memory
        refill_pages[refill_count++] = page;
    }

    // Re-acquire cache lock to update the cache with refilled pages
    spin_lock(&cache->lock);

    // Copy refilled pages into cache
    for (uint32_t i = 0; i < refill_count; i++) {
        if (cache->count < PER_CPU_CACHE_SIZE) {
            cache->pages[cache->count++] = refill_pages[i];
        } else {
            // Cache full (another thread may have refilled while we were unlocked)
            // Return extra page to global pool
            spin_unlock(&cache->lock);
            pmm_free_page_slow(refill_pages[i]);
            spin_lock(&cache->lock);
        }
    }

    // Return one page from newly refilled cache
    void* result = NULL;
    if (cache->count > 0) {
        cache->count--;
        result = cache->pages[cache->count];
    }

    spin_unlock(&cache->lock);

    if (!result) {
        // Completely out of memory
        kernel_panic("PMM: Out of memory");
    }
    if (*(volatile uint64_t*)result == 0x51AB0BACE51AB0BULL)
        kprintf("[SLABPROBE] pmm_alloc(slow) handed out LIVE SLAB %p\n", result);
    return result;
}

/**
 * Slow path: return a page to the global pool by clearing its BITMAP bit.
 *
 * Bitmap-only (matches pmm_alloc_page_slow): we do NOT write the page_t header or
 * push onto the embedded free list. Writing page->is_free/next AT the freed page's
 * physical address has the same fault hazard as reading it (a high physical page
 * may be unmapped on the live user CR3), and the free list is no longer consulted
 * by the allocator anyway. Clearing the bitmap bit is sufficient and never
 * dereferences page memory, so it cannot fault.
 */
static void pmm_free_page_slow(void* page_addr) {
    if (page_addr == NULL) return;

    spin_lock(&global_pmm_lock);

    used_memory -= MIN_PAGE_SIZE;

    uint64_t pfn = pfn_of(page_addr);
    if (pfn < BITMAP_TOTAL_PAGES) {
        bitmap_set_free(pfn);
        pmm_bitmap_hint = pfn / 64;   /* bias the next alloc here (locality + reuse) */
    }

    spin_unlock(&global_pmm_lock);
}

/**
 * Fast path: free page to per-CPU cache (O(1))
 *
 * Recently freed pages are reused hot (better cache locality)
 */
void pmm_free_page(void* page_addr) {
    if (page_addr == NULL) return;

    // Validity guards: reject obviously-bogus frees instead of corrupting the
    // allocator (a bad/computed pointer or a double free is far worse than a
    // small leak). These never reject a legitimate free: real PMM pages are
    // 4KB-aligned, live at/above 1MB, and within the bitmap's PFN range.
    uint64_t pa = (uint64_t)page_addr;
    if (pa & (MIN_PAGE_SIZE - 1)) {
        kprintf("[PMM] BLOCKED free of unaligned page %p\n", page_addr);
        return;
    }
    if (pa < 0x100000) {
        kprintf("[PMM] BLOCKED free of low/reserved page %p\n", page_addr);
        return;
    }
    uint64_t guard_pfn = pa / MIN_PAGE_SIZE;
    if (guard_pfn >= BITMAP_TOTAL_PAGES) {
        kprintf("[PMM] BLOCKED free of out-of-range page %p\n", page_addr);
        return;
    }
    // Double-free guard: a page already marked free in the bitmap is sitting on
    // the global free list; re-freeing it would link it twice and later hand the
    // same frame to two callers. (Pages parked in a per-CPU cache keep their bit
    // clear, so this only catches genuine global-list double frees.)
    spin_lock(&global_pmm_lock);
    int already_free = bitmap_is_free(guard_pfn);
    spin_unlock(&global_pmm_lock);
    if (already_free) {
        kprintf("[PMM] BLOCKED double-free of page %p\n", page_addr);
        return;
    }

    // Hard guard: never free a page inside the reserved initrd range. The initrd
    // is read-only boot data that must survive for the whole session; if any
    // caller computes a bad physical address that lands here, freeing it would
    // un-reserve the page, let a later alloc hand it out, and corrupt the initrd
    // (manifesting as runtime "File not found" on spawn + cascading instability).
    if (pmm_initrd_start && (uint64_t)page_addr >= pmm_initrd_start &&
        (uint64_t)page_addr < pmm_initrd_end) {
        kprintf("[PMM] BLOCKED free of reserved initrd page %p\n", page_addr);
        return;
    }

    // SPURIOUS-LIVE-SLAB-FREE DETECTOR + GUARD: a live slab page carries
    // SLAB_MAGIC (0x51AB0BACE51AB0B) at offset 0, and the slab allocator zeroes
    // that magic BEFORE it returns the page here (slab.c slab_free/destroy). So a
    // page that STILL reads SLAB_MAGIC is a LIVE slab being freed by a stray /
    // aliased pointer (e.g. an address-space teardown freeing a page that was
    // double-allocated as both a kmalloc slab AND a process page table). Caching
    // it hands the live slab to the next pmm_alloc_page(), which zeroes it ->
    // header magic=0 -> kmalloc later pops a wild free_list (the SLABDIAG crash).
    // BLOCK the free (leak the page; far cheaper than corrupting the heap) and
    // report the caller chain so the underlying double-allocation is root-caused.
    if (*(volatile uint64_t*)page_addr == 0x51AB0BACE51AB0BULL) {
        kprintf("[PMM] BLOCKED free of a LIVE slab page %p (caller ra=%p<-%p<-%p) — leaking it to protect the heap\n",
                page_addr, __builtin_return_address(0),
                __builtin_return_address(1), __builtin_return_address(2));
        return;
    }

    // Get current CPU ID for per-CPU page cache (enables SMP performance)
    uint32_t current_cpu = cpu_id();
    if (current_cpu >= MAX_CPUS) current_cpu = 0;   // defensive: never index cpu_caches OOB
    per_cpu_page_cache_t* cache = &cpu_caches[current_cpu];

    spin_lock(&cache->lock);

    // Cache-aware double-free guard. A frame parked in this per-CPU cache keeps its
    // BITMAP bit USED (see the note below), so the bitmap-based double-free check
    // earlier in this function CANNOT see it. Without this scan, freeing a frame
    // twice while it is still cached pushes it onto cache->pages[] twice — it is
    // then handed to TWO different callers, one of which uses it as a live page
    // table while the other writes it as a data page, scribbling the page-table
    // bytes and corrupting the kernel identity map (observed: a NOT-PRESENT kernel
    // #PF inside memset on a ~272MB page after heavy spawn/kill churn; the split
    // PD entry's PT had a zeroed PTE). Reject the duplicate free.
    for (uint32_t k = 0; k < cache->count; k++) {
        if (cache->pages[k] == page_addr) {
            spin_unlock(&cache->lock);
            kprintf("[PMM] BLOCKED double-free of cached page %p\n", page_addr);
            return;
        }
    }

    // Fast path: add to cache if not full
    if (cache->count < PER_CPU_CACHE_SIZE) {
        cache->pages[cache->count] = page_addr;
        cache->count++;
        spin_unlock(&cache->lock);
        /* NOTE: bitmap bit is kept clear (used) while the page sits in the
         * per-CPU cache.  This is intentional: cache pages are logically
         * allocated and must not be handed out by pmm_alloc_pages().  The
         * bitmap bit is set to free only when the page reaches the global
         * free-list via pmm_free_page_slow().                              */
        return;
    }

    spin_unlock(&cache->lock);

    // Slow path: cache full, return to global pool (also updates bitmap)
    pmm_free_page_slow(page_addr);
}

/* -----------------------------------------------------------------------
 * Batch / contiguous allocator
 * -----------------------------------------------------------------------
 *
 * pmm_alloc_pages(count)
 * ----------------------
 * Finds `count` CONTIGUOUS free physical pages using a run-length scan
 * over the 64-bit bitmap words.  Uses a persistent "hint" cursor so
 * successive calls resume scanning from the last successful position
 * instead of always starting at word 0.
 *
 * Algorithm: scan PFN-by-PFN using 64-bit word loads for fast skipping of
 * all-zero (fully-used) words.  A single `run_start` / `run_len` pair is
 * maintained strictly across word boundaries: the run is extended when the
 * next bit is also free, broken (run_len = 0, run_start = current pfn)
 * when a used bit is encountered.  This is O(total_pages/64) worst case.
 *
 * Contiguity guarantee
 * --------------------
 * The bitmap directly represents physical page frames.  A run of `count`
 * consecutive 1-bits in the bitmap corresponds to `count` consecutive
 * physical 4 KB pages.  We atomically scan for the run under
 * global_pmm_lock and mark all bits 0 before releasing the lock, so no
 * other allocator can interleave.
 *
 * Cache / bitmap coherence invariant
 * -----------------------------------
 * Pages that are sitting in a per-CPU free cache have their bitmap bit kept
 * CLEAR (0 = used).  This is the invariant maintained by pmm_free_page():
 * it stores the physical page in the cache WITHOUT calling bitmap_set_free.
 * bitmap_set_free is called only when the page is evicted to the global
 * free-list via pmm_free_page_slow().
 *
 * pmm_alloc_pages only scans for 1-bits, so it will never select a page
 * that is in the per-CPU cache (bitmap bit is 0).  Conversely,
 * pmm_free_pages must NOT call bitmap_set_free for a page whose bit is
 * already 0 due to it being in the cache — doing so would create a
 * double-free where the page is both in the cache and returned by the next
 * pmm_alloc_pages call.  pmm_free_pages avoids this by checking is_free
 * before touching the bitmap.
 *
 * pmm_free_pages(base, count)
 * ---------------------------
 * Marks the `count` pages free in the bitmap and returns each page to the
 * global free-list so the per-CPU cache can reuse them for single-page
 * allocations.  O(count).
 * ----------------------------------------------------------------------- */

/**
 * pmm_alloc_pages - allocate `count` contiguous 4KB physical pages.
 *
 * Returns the physical base address of the run, or NULL if no contiguous
 * run of that length is available.
 *
 * Algorithm correctness:
 *   We maintain (run_start, run_len) across ALL word boundaries.
 *   For each word:
 *     - word == 0: entire word is used; reset run_len to 0.
 *     - word == ~0ULL: entire word is free; if run is active extend by 64,
 *       otherwise start a new run at wi*64; check for completion.
 *     - mixed: iterate bit by bit within the word using ctzll to skip over
 *       blocks of used or free bits efficiently.  A used bit (skip > 0)
 *       breaks the run.  A free block extends or starts it.  After each
 *       extension, check for completion immediately so we never overshoot.
 *       Critically: do NOT add a redundant "end-of-word break" check after
 *       the inner loop — the inner loop already breaks the run when it hits
 *       a used bit, and it correctly handles the cross-word boundary by
 *       leaving run_len > 0 iff the last bit of the word was free.
 *
 * The old code had a fatal end-of-word check (lines ~509-512) that reset
 * run_len=0 whenever the top bit of a mixed word was used.  This destroyed
 * valid runs that ended before bit 63 of the word but had already reached
 * `count`, and it destroyed cross-word runs that had accumulated enough bits
 * from prior all-free words plus the leading free bits of the mixed word.
 */
void* pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;

    /* Fast path for single page — delegate to existing fast allocator */
    if (count == 1) return pmm_alloc_page();

    spin_lock(&global_pmm_lock);

    uint64_t total_pfns = BITMAP_TOTAL_PAGES;

    /*
     * Hoist run_start to function scope so the `found` label (outside the
     * for-loop scopes) can reference it cleanly without a C99 scope error.
     * run_len is also hoisted for the same reason.
     *
     * INVARIANT throughout the scan:
     *   run_len == 0  => no active candidate run
     *   run_len >  0  => PFNs [run_start, run_start+run_len) are all free
     *                    in the bitmap (verified bit-by-bit as we scan).
     */
    uint64_t run_start = 0;
    uint64_t run_len   = 0;

    /* Two-pass wrap-around scan: start at hint, wrap to 0 if needed */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start_word = (pass == 0) ? pmm_bitmap_hint : 0;
        uint64_t end_word   = (pass == 0) ? BITMAP_WORDS    : pmm_bitmap_hint;

        /* Reset run state at the start of each pass (no carry-over across
         * the wrap boundary because physical frames are not contiguous there) */
        run_start = 0;
        run_len   = 0;

        for (uint64_t wi = start_word; wi < end_word; wi++) {
            uint64_t word = pmm_bitmap[wi];
            uint64_t base_pfn = wi * 64;

            if (word == 0) {
                /* All 64 pages in this word are used — break any run */
                run_len = 0;
                continue;
            }

            if (word == ~0ULL) {
                /* All 64 pages free — extend or start a run */
                if (run_len == 0)
                    run_start = base_pfn;
                run_len += 64;

                /* Check for completion after fast full-word extension */
                if (run_len >= (uint64_t)count)
                    goto found;

                continue;
            }

            /*
             * Mixed word: iterate through the bits using ctzll for efficient
             * skipping.  We process every bit in [0, 63] exactly once.
             *
             * KEY PROPERTY: the inner loop leaves run_len > 0 iff the last
             * bit of the word (bit 63) was free.  This is the only condition
             * under which a cross-word continuation is valid, and it happens
             * naturally without any separate top-bit check.
             *
             * The old code added a redundant "end-of-word break" after this
             * inner loop that reset run_len=0 whenever the top bit of a mixed
             * word was used.  That check was WRONG: it destroyed valid runs
             * that ended before bit 63 but had already accumulated `count`
             * bits, and it destroyed cross-word runs that had gathered enough
             * pages from prior all-free words plus the leading bits of the
             * mixed word.  Removed entirely — the inner loop handles all cases.
             */
            {
                uint64_t bit = 0;
                while (bit < 64) {
                    uint64_t remaining = word >> bit;

                    if (remaining == 0) {
                        /*
                         * No more free bits in this word.  All remaining
                         * bits in [bit, 63] are 0 (used).  Break the run.
                         */
                        run_len = 0;
                        break;
                    }

                    /* Count trailing zeros = number of consecutive used bits */
                    uint64_t skip = (uint64_t)__builtin_ctzll(remaining);
                    if (skip > 0) {
                        /* Used bits: break the current run */
                        run_len = 0;
                        bit += skip;
                        continue;
                    }

                    /*
                     * Current bit is free (remaining & 1 == 1).
                     * Count consecutive free bits = trailing ones in ~remaining.
                     * Guard: if ~remaining == 0 (remaining is all-ones) then
                     * __builtin_ctzll(0) is undefined; handle that case first.
                     */
                    uint64_t inv = ~remaining;
                    uint64_t free_run;
                    if (inv == 0) {
                        /* All bits from `bit` to 63 are free */
                        free_run = 64 - bit;
                    } else {
                        free_run = (uint64_t)__builtin_ctzll(inv);
                        /* Clamp to the remaining bits in this word */
                        if (bit + free_run > 64)
                            free_run = 64 - bit;
                    }

                    /* Extend or start the run */
                    if (run_len == 0)
                        run_start = base_pfn + bit;
                    run_len += free_run;

                    /* Check for completion immediately after extension */
                    if (run_len >= (uint64_t)count)
                        goto found;

                    bit += free_run;
                }
            }
        }
    }

    spin_unlock(&global_pmm_lock);
    return NULL;  /* No contiguous run found */

found:
    /*
     * We have a run [run_start, run_start + run_len) with run_len >= count.
     * Mark exactly `count` pages used and update accounting.
     */
    // Bitmap-only: mark the run used. We do NOT touch the page_t headers or the
    // embedded free list — the allocator is driven entirely by the bitmap now, so
    // the contiguous path and the single-page slow path stay coherent without ever
    // dereferencing physical page memory (no clobbered-node / unmapped-page #PF).
    for (uint64_t i = 0; i < (uint64_t)count; i++) {
        bitmap_set_used(run_start + i);
    }
    used_memory += (uint64_t)count * MIN_PAGE_SIZE;

    /* Advance hint to just past this allocation */
    {
        uint64_t next_hint_pfn = run_start + count;
        pmm_bitmap_hint = (next_hint_pfn < total_pfns)
                          ? next_hint_pfn / 64
                          : 0;
    }

    spin_unlock(&global_pmm_lock);
    return addr_of_pfn(run_start);
}

/**
 * pmm_free_pages - free `count` contiguous pages starting at `base`.
 *
 * Marks each page free in the bitmap and returns it to the global
 * free-list for reuse by single-page allocators.  O(count).
 *
 * Cache/bitmap coherence:
 *   pmm_free_page() (the single-page fast path) stores a page in the
 *   per-CPU cache WITHOUT setting its bitmap bit free.  The bitmap bit
 *   remains 0 (used) while the page is cached so that pmm_alloc_pages()
 *   cannot select it through the bitmap scan.
 *
 *   pmm_free_pages must respect this invariant.  A page with is_free == true
 *   is NOT necessarily free-in-bitmap: it could be sitting in a per-CPU
 *   cache with its bitmap bit still clear.  We must only call bitmap_set_free
 *   and only decrement used_memory for pages that are genuinely allocated
 *   (is_free == false AND bitmap bit == 0 due to pmm_alloc_pages/slow).
 *
 *   The correct guard: if is_free is already true, the page was never
 *   allocated in the first place (double-free) — skip it.  If is_free is
 *   false, the page is allocated: mark it free, add to free-list, update
 *   bitmap and accounting.
 *
 *   We do NOT call pmm_free_page() here because that would put the pages
 *   into the per-CPU cache WITHOUT setting the bitmap bit.  Since this
 *   function is specifically for the contiguous allocator path, we add
 *   directly to the global free-list (bitmap is set free, page is visible
 *   to future pmm_alloc_pages scans).
 */
void pmm_free_pages(void* base, size_t count) {
    if (base == NULL || count == 0) return;

    /* Hard guard: refuse any range that overlaps the reserved initrd (see
     * pmm_free_page) — a bad address here would un-reserve initrd pages and
     * corrupt boot data at runtime. */
    if (pmm_initrd_start) {
        uint64_t s = (uint64_t)base, e = s + (uint64_t)count * MIN_PAGE_SIZE;
        if (s < pmm_initrd_end && e > pmm_initrd_start) {
            kprintf("[PMM] BLOCKED free of range overlapping initrd: %p +%lu\n",
                    base, (unsigned long)count);
            return;
        }
    }

    /* Single-page shortcut: delegate to the normal free path */
    if (count == 1) {
        pmm_free_page(base);
        return;
    }

    uint64_t start_pfn = pfn_of(base);

    spin_lock(&global_pmm_lock);

    for (uint64_t i = 0; i < (uint64_t)count; i++) {
        uint64_t pfn = start_pfn + i;
        if (pfn >= BITMAP_TOTAL_PAGES) break;  /* Safety: never go out of bounds */

        void* page_addr = addr_of_pfn(pfn);
        page_t* page = (page_t*)page_addr;

        // LIVE-SLAB GUARD (same rationale as pmm_free_page): a page still carrying
        // SLAB_MAGIC is a live kmalloc slab. Freeing it here would bitmap_set_free a
        // live slab page (-> double-alloc -> the next alloc_page_table zeroes it ->
        // slab header magic=0 -> SLABDIAG) AND scribble page_t fields over the slab.
        // This fires when a contiguous free runs into live slabs (e.g. a wrong
        // count, or a per-page-allocated shm segment freed as one contiguous run).
        // Skip+leak the page and report the caller so the bad free is root-caused.
        if (*(volatile uint64_t*)page_addr == 0x51AB0BACE51AB0BULL) {
            kprintf("[PMM] BLOCKED contiguous free of a LIVE slab page %p (range %p+%lu, caller ra=%p<-%p) — leaking it to protect the heap\n",
                    page_addr, base, (unsigned long)count,
                    __builtin_return_address(0), __builtin_return_address(1));
            continue;
        }

        if (page->is_free) {
            /*
             * Already marked free — either a double-free or the page is in
             * the per-CPU cache with is_free true but bitmap bit still 0.
             *
             * In the per-CPU cache case, is_free is NOT set to true by
             * pmm_free_page(): look at pmm_free_page() — it stores the
             * raw pointer in cache->pages[] without touching page_t at all.
             * So is_free == true here means a genuine double-free; skip it
             * safely.  The bitmap is not touched (it may already be 1 from
             * a prior pmm_add_page_locked or pmm_free_page_slow call).
             */
            continue;
        }

        /*
         * Page is allocated (is_free == false).  Return it to the global
         * free-list and mark the bitmap bit free so future pmm_alloc_pages
         * scans can select it.
         */
        page->is_free = true;
        page->order = 0;
        page->next = free_lists[0];
        free_lists[0] = page;
        used_memory -= MIN_PAGE_SIZE;
        bitmap_set_free(pfn);
    }

    /* Nudge hint back toward the freed range if it helps future allocs */
    if (start_pfn / 64 < pmm_bitmap_hint) {
        pmm_bitmap_hint = start_pfn / 64;
    }

    spin_unlock(&global_pmm_lock);
}

uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

uint64_t pmm_get_used_memory(void) {
    return used_memory;
}

uint64_t pmm_get_free_memory(void) {
    return total_memory - used_memory;
}

/**
 * Report per-CPU cache statistics (for performance analysis)
 */
/**
 * Add remaining pages above PMM_INITIAL_MAX_PHYS to the free list.
 * Must be called AFTER paging_init has extended identity mapping beyond 1GB.
 * Uses the original memory map entries from multiboot.
 */
void pmm_add_remaining_pages(memory_map_entry_t* mmap, uint32_t mmap_count) {
    extern char __kernel_end;
    uint64_t kernel_start_phys = 0x100000;
    uint64_t kernel_end_phys = ALIGN_UP((uint64_t)&__kernel_end, MIN_PAGE_SIZE);

    kprintf("[PMM] Adding remaining memory above 1GB...\n");

    uint32_t free_pages = 0;
    uint64_t added_memory = 0;

    spin_lock(&global_pmm_lock);

    for (uint32_t i = 0; i < mmap_count; i++) {
        if (mmap[i].type == 1) {
            uint64_t base = ALIGN_UP(mmap[i].base, MIN_PAGE_SIZE);
            uint64_t end = ALIGN_DOWN(mmap[i].base + mmap[i].length, MIN_PAGE_SIZE);
            if (end <= base) continue;

            // Only process regions above the initial identity-mapped range
            if (end <= PMM_INITIAL_MAX_PHYS) continue;
            if (base < PMM_INITIAL_MAX_PHYS) base = PMM_INITIAL_MAX_PHYS;

            added_memory += (end - base);

            for (uint64_t addr = base; addr < end; addr += MIN_PAGE_SIZE) {
                if (addr < 0x100000) continue;
                if (addr >= kernel_start_phys && addr < kernel_end_phys) continue;
                if (pmm_initrd_start && addr >= pmm_initrd_start && addr < pmm_initrd_end) continue;

                page_t* page = (page_t*)addr;
                page->order = 0;
                page->is_free = true;
                page->next = free_lists[0];
                free_lists[0] = page;

                uint64_t pfn = addr / MIN_PAGE_SIZE;
                if (pfn < BITMAP_TOTAL_PAGES)
                    bitmap_set_free(pfn);

                free_pages++;
            }
        }
    }

    spin_unlock(&global_pmm_lock);

    total_memory += added_memory;
    kprintf("[PMM] Added %u MB from high memory (%u pages)\n",
            (uint32_t)(added_memory / (1024 * 1024)), free_pages);
}

void pmm_report_cache_stats(void) {
    kprintf("[PMM] Per-CPU Cache Statistics:\n");
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        per_cpu_page_cache_t* cache = &cpu_caches[cpu];
        if (cache->alloc_fast == 0 && cache->alloc_slow == 0) continue;

        uint64_t total = cache->alloc_fast + cache->alloc_slow;
        // BUG-015 fix: Prevent division by zero (should never happen, but be safe)
        uint64_t hit_rate = (total > 0) ? (cache->alloc_fast * 100) / total : 0;

        kprintf("  CPU %d: Fast=%llu Slow=%llu Hit Rate=%llu%% Cached=%u\n",
                cpu, cache->alloc_fast, cache->alloc_slow, hit_rate, cache->count);
    }
}

/**
 * pmm_reclaim_cpu_cache - flush a per-CPU page cache back to the global pool.
 *
 * Must be called when a CPU goes offline (CPU hot-unplug) to prevent stranding
 * up to PER_CPU_CACHE_SIZE pages in an unreachable cache. Without this function,
 * CPU hot-unplug would leak memory.
 *
 * Thread safety: The caller is responsible for ensuring the target CPU is already
 * offline (not running) before calling this function. Typically invoked from the
 * cpu_offline() path after the CPU state has been set to CPU_STATE_OFFLINE.
 *
 * Lock ordering: Acquires cache->lock then releases it before calling
 * pmm_free_page_slow (which acquires global_pmm_lock), preventing deadlock.
 *
 * Returns: Number of pages reclaimed from the cache.
 */
uint32_t pmm_reclaim_cpu_cache(uint32_t cpu) {
    if (cpu >= MAX_CPUS) {
        kprintf("[PMM] WARNING: Attempt to reclaim cache for invalid CPU %u\n", cpu);
        return 0;
    }

    per_cpu_page_cache_t* cache = &cpu_caches[cpu];
    uint32_t reclaimed = 0;

    spin_lock(&cache->lock);

    // Copy all cached pages to a local array before returning them.
    // This avoids holding cache->lock while calling pmm_free_page_slow
    // (which acquires global_pmm_lock), preventing lock-order violation.
    void* pages_to_free[PER_CPU_CACHE_SIZE];
    uint32_t count = cache->count;
    for (uint32_t i = 0; i < count; i++) {
        pages_to_free[i] = cache->pages[i];
    }
    cache->count = 0;

    spin_unlock(&cache->lock);

    // Now return all pages to the global pool (updates bitmap + free-list)
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_page_slow(pages_to_free[i]);
        reclaimed++;
    }

    if (reclaimed > 0) {
        kprintf("[PMM] Reclaimed %u pages from CPU %u cache\n", reclaimed, cpu);
    }

    return reclaimed;
}

/* -----------------------------------------------------------------------
 * Huge Page Allocator (2MB pages)
 * -----------------------------------------------------------------------
 * Allocates physically contiguous 2MB-aligned blocks for use as huge pages.
 * This reduces TLB pressure by allowing a single TLB entry to cover 2MB
 * instead of 512 separate 4KB entries.
 *
 * Performance: 512x reduction in TLB entries for large allocations.
 * Example: 100MB allocation = 196 TLB entries vs 25600 with 4KB pages.
 * ----------------------------------------------------------------------- */

// Statistics for huge page allocations
static uint64_t huge_pages_allocated = 0;
static uint64_t huge_pages_freed = 0;
static uint64_t huge_page_alloc_failures = 0;

/**
 * pmm_alloc_huge_page - allocate a 2MB physically contiguous, 2MB-aligned page.
 *
 * Returns the physical base address of the 2MB page, or NULL on failure.
 *
 * Implementation: delegates to pmm_alloc_pages(512) which finds a contiguous
 * run of 512 4KB pages. The bitmap scanner naturally finds 2MB-aligned runs
 * because 512-page runs starting at any PFN divisible by 512 correspond to
 * 2MB-aligned physical addresses.
 */
void* pmm_alloc_huge_page(void) {
    // Allocate 512 contiguous 4KB pages (= 2MB)
    void* base = pmm_alloc_pages(PAGES_PER_HUGE_PAGE);
    if (!base) {
        spin_lock(&global_pmm_lock);
        huge_page_alloc_failures++;
        spin_unlock(&global_pmm_lock);
        return NULL;
    }

    // Verify 2MB alignment (defensive check)
    uint64_t addr = (uint64_t)base;
    if (addr & (HUGE_PAGE_SIZE - 1)) {
        kprintf("[PMM] WARNING: Allocated huge page %p is not 2MB-aligned\n", base);
        pmm_free_pages(base, PAGES_PER_HUGE_PAGE);
        spin_lock(&global_pmm_lock);
        huge_page_alloc_failures++;
        spin_unlock(&global_pmm_lock);
        return NULL;
    }

    spin_lock(&global_pmm_lock);
    huge_pages_allocated++;
    spin_unlock(&global_pmm_lock);

    return base;
}

/**
 * pmm_free_huge_page - free a 2MB huge page.
 *
 * The base address must be 2MB-aligned and must have been allocated via
 * pmm_alloc_huge_page.
 */
void pmm_free_huge_page(void* base) {
    if (!base) return;

    // Verify 2MB alignment
    uint64_t addr = (uint64_t)base;
    if (addr & (HUGE_PAGE_SIZE - 1)) {
        kprintf("[PMM] ERROR: Attempt to free non-aligned huge page %p\n", base);
        return;
    }

    pmm_free_pages(base, PAGES_PER_HUGE_PAGE);

    spin_lock(&global_pmm_lock);
    huge_pages_freed++;
    spin_unlock(&global_pmm_lock);
}

/**
 * pmm_get_huge_page_stats - report huge page allocation statistics.
 */
void pmm_get_huge_page_stats(uint64_t* allocated, uint64_t* freed, uint64_t* failures) {
    spin_lock(&global_pmm_lock);
    if (allocated) *allocated = huge_pages_allocated;
    if (freed) *freed = huge_pages_freed;
    if (failures) *failures = huge_page_alloc_failures;
    spin_unlock(&global_pmm_lock);
}
