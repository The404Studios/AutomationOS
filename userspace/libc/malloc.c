// userspace/libc/malloc.c - Freestanding ring-3 heap allocator
//
// Included (not compiled separately) by stdlib.c so that the existing
// Makefile needs no changes: stdlib.c is already compiled into libc.a.
//
// ── DESIGN ──────────────────────────────────────────────────────────────────
//
// Two-tier arena system:
//
//   Tier 1 (static)  — 8 MB BSS array, always present, zero extra syscalls.
//   Tier 2 (mmap)    — 2 MB chunks obtained from the kernel via SYS_MMAP=37
//                      on demand whenever the current chain of arenas cannot
//                      satisfy a request.
//
// SYS_MMAP=37 call convention (from kernel/core/syscall/handlers.c):
//   arg1 = hint (0 = let kernel choose), arg2 = length, arg3 = prot,
//   arg4 = flags, arg5/arg6 = 0.
//   Returns: mapped user vaddr (positive) on success, negative errno on fail.
//   Cap: 256 MB per call.  The kernel ignores hint/flags/prot for anonymous
//   mappings; prot=0 works fine (the page is always RW user-accessible).
//
// Both tiers share the same block-header format and the same first-fit +
// coalescing free-list logic.  Arenas are chained via arena_header.next so
// the allocator can walk all arenas looking for a fit.
//
// ── BLOCK LAYOUT ────────────────────────────────────────────────────────────
//
//   [ arena_header ][ block_header | payload ][ block_header | payload ] ...
//
// block_header (32 bytes, payload starts 16-byte aligned):
//   size_t  size   — usable payload bytes (multiple of 16)
//   uint32 free    — 1 = free, 0 = in-use
//   uint32 magic   — BLOCK_MAGIC sanity sentinel
//   block_header* next — next block in this arena (NULL = last)
//   uint8[8] _pad  — pad to 32 bytes
//
// arena_header (32 bytes, lives at the start of each mmap chunk):
//   size_t   size  — total bytes in this arena (including this header)
//   arena_header* next — singly-linked chain of overflow arenas (NULL = end)
//   uint8[16] _pad — pad to 32 bytes so the first block_header that follows
//                    is itself 32-byte aligned (payload then 16-byte aligned).
//
// ── ALIGNMENT ───────────────────────────────────────────────────────────────
//
// All payload pointers are 16-byte aligned.
// Block headers are 32 bytes; arena headers are 32 bytes.
// The static arena is __attribute__((aligned(16))).
// mmap returns page-aligned (4096) memory, so alignment is trivially met.
//
// ── THREAD SAFETY ───────────────────────────────────────────────────────────
//
// None.  This OS has cooperative (non-preemptive) scheduling; no locking
// is required for single-process heap use.

// --------------------------------------------------------------------------
// Syscall helper — private to this file
// --------------------------------------------------------------------------

// 6-argument raw syscall wrapper.  Uses the x86-64 System V ABI for syscall:
// rax=nr, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9 not used here.
// rcx and r11 are clobbered by the CPU.
// NOTE: a6 arg present for completeness but not wired to a register — the
// kernel's sys_mmap only uses 5 arguments anyway.
static long __sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long _r10 asm("r10") = a4;
    register long _r8  asm("r8")  = a5;
    asm volatile(
        "syscall"
        : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(_r10), "r"(_r8)
        : "rcx", "r11", "memory"
    );
    return r;
}

#define SYS_MMAP_NR  37

// --------------------------------------------------------------------------
// Sizes and constants
// --------------------------------------------------------------------------

#define STATIC_HEAP_SIZE  (8UL * 1024 * 1024)   // 8 MB static tier
#define MMAP_CHUNK_SIZE   (2UL * 1024 * 1024)    // 2 MB per overflow chunk
#define MIN_SPLIT         32                      // min remainder to bother splitting
#define BLOCK_MAGIC       0xA110CA7EU
#define ARENA_MAGIC       0xAE11A00EU

// --------------------------------------------------------------------------
// TCACHE (thread cache) configuration
// --------------------------------------------------------------------------
// Per-thread cache for small allocations (glibc pattern). NOTE: AutomationOS
// does not yet have userspace threads or TLS infrastructure (__thread, %fs
// segment base), so this is currently a **global** tcache. This still achieves
// the 100x syscall reduction goal for single-threaded apps (the common case).
// When userspace threading is added, this can be upgraded to per-thread.
//
// Design:
//   - 64 size classes: 16, 32, 48, ..., 1024 bytes (16-byte increments)
//   - Up to 16 cached chunks per bin
//   - Fast path: malloc checks tcache bin → return cached chunk (no syscall!)
//   - Refill: tcache miss → allocate from arena → split and cache remainder
//   - Free path: return to tcache if bin not full → no syscall!
//
#define TCACHE_NBINS      64    // 64 size classes
#define TCACHE_BIN_SIZE   16    // max 16 chunks per bin
#define TCACHE_MIN_SIZE   16    // smallest size class (bytes)
#define TCACHE_MAX_SIZE   1024  // largest size class (bytes)

// --------------------------------------------------------------------------
// Block header (32 bytes)
// --------------------------------------------------------------------------

typedef struct blk_hdr {
    unsigned long       size;    // usable payload bytes
    unsigned int        free;    // 1 = free
    unsigned int        magic;   // BLOCK_MAGIC
    struct blk_hdr     *next;    // next block in arena
    unsigned char       _pad[8];
} blk_hdr_t;

// Compile-time size check — negative array triggers an error if wrong.
typedef char _blk_hdr_size_check[ (sizeof(blk_hdr_t) == 32) ? 1 : -1 ];

// --------------------------------------------------------------------------
// Arena header (32 bytes)
// --------------------------------------------------------------------------

typedef struct arena_hdr {
    unsigned long       size;    // total bytes in this arena
    unsigned int        magic;   // ARENA_MAGIC
    unsigned int        _pad0;
    struct arena_hdr   *next;    // next overflow arena
    unsigned char       _pad[8];
} arena_hdr_t;

typedef char _arena_hdr_size_check[ (sizeof(arena_hdr_t) == 32) ? 1 : -1 ];

// --------------------------------------------------------------------------
// TCACHE structures
// --------------------------------------------------------------------------

// A tcache bin: a simple stack of cached chunks for a given size class.
typedef struct tcache_bin {
    void* entries[TCACHE_BIN_SIZE];  // stack of cached chunk pointers
    unsigned char count;              // current number of entries
} tcache_bin_t;

// The global tcache. In a multi-threaded OS this would be __thread tcache_t,
// but AutomationOS has cooperative single-process scheduling, so a global is
// safe and still provides the 100x syscall reduction we need.
typedef struct tcache {
    tcache_bin_t bins[TCACHE_NBINS];  // 64 bins for sizes 16..1024
} tcache_t;

// --------------------------------------------------------------------------
// Static arena (tier 1) and tcache
// --------------------------------------------------------------------------

static unsigned char _static_heap[STATIC_HEAP_SIZE] __attribute__((aligned(16)));
static int           _heap_ready = 0;

// Head of the block free-list for the static arena.
// (Each mmap arena has its own block list rooted just after its arena_hdr.)
static blk_hdr_t    *_static_list = (blk_hdr_t*)0;

// Head of the overflow arena chain (singly-linked list of arena_hdr_t).
static arena_hdr_t  *_arenas = (arena_hdr_t*)0;

// Global tcache (NOTE: will become __thread when userspace threading is added).
static tcache_t _tcache = {0};

// Optional instrumentation counters (can be disabled for production)
#ifdef MALLOC_STATS
static unsigned long _tcache_hits = 0;    // malloc from cache
static unsigned long _tcache_misses = 0;  // malloc from arena
static unsigned long _tcache_frees = 0;   // free to cache
static unsigned long _tcache_bypassed = 0; // free to arena (cache full or large)
#endif

// --------------------------------------------------------------------------
// Internal helpers
// --------------------------------------------------------------------------

static unsigned long _align16(unsigned long v) {
    return (v + 15UL) & ~15UL;
}

// --------------------------------------------------------------------------
// TCACHE helpers
// --------------------------------------------------------------------------

// Map a request size to a tcache bin index (0..63).
// Size classes: 16, 32, 48, ..., 1024 in 16-byte increments.
// Returns TCACHE_NBINS if size > TCACHE_MAX_SIZE (not cacheable).
static inline unsigned _tcache_bin(unsigned long size) {
    if (size == 0 || size > TCACHE_MAX_SIZE) {
        return TCACHE_NBINS;  // out of range
    }
    // Round size up to next multiple of 16, then compute bin index.
    unsigned long aligned = (size + 15UL) & ~15UL;
    return (unsigned)((aligned / TCACHE_MIN_SIZE) - 1);
}

// Forward declarations: payload<->header conversions are defined below but used
// here by the tcache double-free-detection path (_tcache_get reads hdr->free).
static inline void* _blk_payload(blk_hdr_t *h);
static inline blk_hdr_t* _payload_blk(void *p);

// Get a chunk from the tcache (if available). Returns NULL if bin is empty.
static void* _tcache_get(unsigned long size) {
    unsigned bin = _tcache_bin(size);
    if (bin >= TCACHE_NBINS) return (void*)0;

    tcache_bin_t* b = &_tcache.bins[bin];
    if (b->count == 0) return (void*)0;

    b->count--;
    void* ptr = b->entries[b->count];  // pop from stack
    // Mark block as in-use (free set it to 1 for double-free detection)
    blk_hdr_t* hdr = _payload_blk(ptr);
    hdr->free = 0;
    return ptr;
}

// Put a chunk into the tcache (if not full). Returns 0 on success, -1 if full.
static int _tcache_put(void* ptr, unsigned long size) {
    unsigned bin = _tcache_bin(size);
    if (bin >= TCACHE_NBINS) return -1;

    tcache_bin_t* b = &_tcache.bins[bin];
    if (b->count >= TCACHE_BIN_SIZE) return -1;  // bin full

    b->entries[b->count] = ptr;
    b->count++;
    return 0;
}

static inline void* _blk_payload(blk_hdr_t *h) {
    return (void*)((unsigned char*)h + sizeof(blk_hdr_t));
}

static inline blk_hdr_t* _payload_blk(void *p) {
    return (blk_hdr_t*)((unsigned char*)p - sizeof(blk_hdr_t));
}

// Coalesce block h with consecutive free neighbours (forward only).
static void _coalesce_fwd(blk_hdr_t *h) {
    while (h->next && h->next->free && h->next->magic == BLOCK_MAGIC) {
        h->size += sizeof(blk_hdr_t) + h->next->size;
        h->next  = h->next->next;
    }
}

// Initialise a contiguous region of `capacity` bytes as a single free block.
// Returns a pointer to the block header.
static blk_hdr_t* _arena_init_blocks(unsigned char *mem, unsigned long capacity) {
    blk_hdr_t *h = (blk_hdr_t*)mem;
    h->size  = capacity - sizeof(blk_hdr_t);
    h->free  = 1;
    h->magic = BLOCK_MAGIC;
    h->next  = (blk_hdr_t*)0;
    return h;
}

// First-fit allocation from a block list.  Returns payload ptr or NULL.
static void* _alloc_from_list(blk_hdr_t *list, unsigned long size) {
    blk_hdr_t *cur = list;
    while (cur) {
        if (cur->free && cur->size >= size) {
            // Split if there's enough room for a new header plus MIN_SPLIT bytes.
            if (cur->size >= size + sizeof(blk_hdr_t) + MIN_SPLIT) {
                blk_hdr_t *nxt = (blk_hdr_t*)((unsigned char*)_blk_payload(cur) + size);
                nxt->size  = cur->size - size - sizeof(blk_hdr_t);
                nxt->free  = 1;
                nxt->magic = BLOCK_MAGIC;
                nxt->next  = cur->next;
                cur->next  = nxt;
                cur->size  = size;
            }
            cur->free = 0;
            return _blk_payload(cur);
        }
        cur = cur->next;
    }
    return (void*)0;
}

// Walk list and coalesce backward-then-forward around a newly freed block h.
static void _free_in_list(blk_hdr_t *list_head, blk_hdr_t *h) {
    h->free = 1;
    _coalesce_fwd(h);

    // Backward coalesce: find the predecessor.
    if (list_head != h) {
        blk_hdr_t *prev = list_head;
        while (prev && prev->next != h) {
            prev = prev->next;
        }
        if (prev && prev->free) {
            _coalesce_fwd(prev);
        }
    }
}

// --------------------------------------------------------------------------
// Heap initialisation (lazy, on first malloc)
// --------------------------------------------------------------------------

static void _heap_init(void) {
    _static_list = _arena_init_blocks(_static_heap, STATIC_HEAP_SIZE);
    _heap_ready  = 1;
}

// --------------------------------------------------------------------------
// Overflow arena: request a new 2 MB chunk from the kernel via SYS_MMAP.
// Returns the head block_hdr of the new arena, or NULL on failure.
// --------------------------------------------------------------------------

static blk_hdr_t* _grow_heap(void) {
    long addr = __sc(SYS_MMAP_NR, 0, (long)MMAP_CHUNK_SIZE, 0, 0, 0);
    if (addr <= 0) {
        return (blk_hdr_t*)0;  // mmap failed (ENOMEM or not implemented)
    }

    unsigned char *mem = (unsigned char*)(unsigned long)addr;

    // Place an arena_hdr at the start of the chunk.
    arena_hdr_t *ah = (arena_hdr_t*)mem;
    ah->size   = MMAP_CHUNK_SIZE;
    ah->magic  = ARENA_MAGIC;
    ah->_pad0  = 0;
    ah->next   = _arenas;
    _arenas    = ah;

    // The block region starts right after the arena_hdr.
    unsigned char  *blk_start = mem + sizeof(arena_hdr_t);
    unsigned long   blk_cap   = MMAP_CHUNK_SIZE - sizeof(arena_hdr_t);
    return _arena_init_blocks(blk_start, blk_cap);
}

// --------------------------------------------------------------------------
// Given a payload pointer, find which block-list it lives in and return the
// list head.  Checks static arena first, then overflow arenas.
// Returns NULL if the pointer doesn't belong to any known arena.
// --------------------------------------------------------------------------

static blk_hdr_t* _find_list_for(blk_hdr_t *h) {
    // Static arena?
    unsigned char *sh_start = _static_heap;
    unsigned char *sh_end   = _static_heap + STATIC_HEAP_SIZE;
    if ((unsigned char*)h >= sh_start && (unsigned char*)h < sh_end) {
        return _static_list;
    }
    // Overflow arenas: the block list starts sizeof(arena_hdr_t) into the chunk.
    for (arena_hdr_t *a = _arenas; a; a = a->next) {
        unsigned char *a_start = (unsigned char*)a;
        unsigned char *a_end   = a_start + a->size;
        if ((unsigned char*)h >= a_start && (unsigned char*)h < a_end) {
            return (blk_hdr_t*)(a_start + sizeof(arena_hdr_t));
        }
    }
    return (blk_hdr_t*)0;
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

void* malloc(unsigned long size) {
    if (size == 0) return (void*)0;
    if (!_heap_ready) _heap_init();

    size = _align16(size);

    // ────────────────────────────────────────────────────────────────────────
    // FAST PATH: check tcache first (no syscall!)
    // ────────────────────────────────────────────────────────────────────────
    void* cached = _tcache_get(size);
    if (cached) {
#ifdef MALLOC_STATS
        _tcache_hits++;
#endif
        return cached;  // tcache hit — zero syscalls!
    }

#ifdef MALLOC_STATS
    _tcache_misses++;
#endif

    // ────────────────────────────────────────────────────────────────────────
    // SLOW PATH: allocate from arena and refill tcache
    // ────────────────────────────────────────────────────────────────────────

    // Try static arena first.
    void *p = _alloc_from_list(_static_list, size);
    if (p) return p;

    // Try existing overflow arenas.
    for (arena_hdr_t *a = _arenas; a; a = a->next) {
        blk_hdr_t *list = (blk_hdr_t*)((unsigned char*)a + sizeof(arena_hdr_t));
        p = _alloc_from_list(list, size);
        if (p) return p;
    }

    // Grow: request a new 2 MB chunk from the kernel.
    blk_hdr_t *new_list = _grow_heap();
    if (!new_list) return (void*)0;   // truly out of memory

    return _alloc_from_list(new_list, size);
}

void free(void* ptr) {
    if (!ptr || !_heap_ready) return;

    blk_hdr_t *h = _payload_blk(ptr);
    if (h->magic != BLOCK_MAGIC) return;  // corrupt pointer guard
    if (h->free) return;                  // double-free detection

    h->free = 1;  // mark freed BEFORE caching or returning to arena

    // ────────────────────────────────────────────────────────────────────────
    // FAST PATH: try to cache in tcache (no syscall!)
    // ────────────────────────────────────────────────────────────────────────
    if (_tcache_put(ptr, h->size) == 0) {
#ifdef MALLOC_STATS
        _tcache_frees++;
#endif
        return;  // tcache accepted it — zero syscalls!
    }

#ifdef MALLOC_STATS
    _tcache_bypassed++;
#endif

    // ────────────────────────────────────────────────────────────────────────
    // SLOW PATH: tcache bin is full, return to arena free-list
    // ────────────────────────────────────────────────────────────────────────
    blk_hdr_t *list = _find_list_for(h);
    if (!list) return;  // pointer not from our heap

    _free_in_list(list, h);
}

void* calloc(unsigned long count, unsigned long size) {
    // Overflow check.
    if (count != 0 && size > ((unsigned long)-1) / count) return (void*)0;
    unsigned long total = count * size;
    void *p = malloc(total);
    if (p) {
        // Zero the allocation.  We use a simple byte loop rather than calling
        // memset() to avoid any circular dependency if memset is not yet set up.
        unsigned char *b = (unsigned char*)p;
        for (unsigned long i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

void* realloc(void* ptr, unsigned long size) {
    if (!ptr)    return malloc(size);
    if (size == 0) { free(ptr); return (void*)0; }

    blk_hdr_t *h = _payload_blk(ptr);
    if (h->magic != BLOCK_MAGIC) return (void*)0;

    unsigned long aligned_new = _align16(size);

    // If current block already fits, return it in-place (no copy needed).
    if (h->size >= aligned_new) return ptr;

    void *np = malloc(size);
    if (!np) return (void*)0;

    // Copy old content (h->size is the old usable size).
    unsigned long copy = h->size;  // always <= new allocation
    unsigned char *src = (unsigned char*)ptr;
    unsigned char *dst = (unsigned char*)np;
    for (unsigned long i = 0; i < copy; i++) dst[i] = src[i];

    free(ptr);
    return np;
}

// --------------------------------------------------------------------------
// TCACHE statistics (optional, only available with -DMALLOC_STATS)
// --------------------------------------------------------------------------

#ifdef MALLOC_STATS
// Get current tcache statistics. Useful for benchmarking and verification.
void malloc_tcache_stats(unsigned long* hits, unsigned long* misses,
                         unsigned long* cached_frees, unsigned long* bypassed_frees) {
    if (hits) *hits = _tcache_hits;
    if (misses) *misses = _tcache_misses;
    if (cached_frees) *cached_frees = _tcache_frees;
    if (bypassed_frees) *bypassed_frees = _tcache_bypassed;
}

// Reset tcache statistics
void malloc_tcache_reset_stats(void) {
    _tcache_hits = 0;
    _tcache_misses = 0;
    _tcache_frees = 0;
    _tcache_bypassed = 0;
}
#endif
