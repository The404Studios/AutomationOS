/*
 * tests/unit/test_heap_neghmagic.c
 *
 * NEGHMAGIC -- negative regression test for the forged-heap-magic type-confusion
 * HIGH fixed in kernel/core/mem/heap.c (KERNEL-ROBUST-0).
 *
 * Pre-fix bug (parent commit 55926d4):
 *   kfree()/krealloc() read an 8-byte "SLAB_MAGIC" sentinel straight out of the
 *   caller-freeable data at the block's page-aligned base BEFORE ever consulting
 *   heap_owns(). A heap-owned allocation whose data pointer happens to be
 *   page-aligned (page_base == ptr) and whose first 8 bytes an attacker sets to
 *   0x51AB0BACE51AB0BULL is therefore misrouted into slab_free(NULL, ptr) --
 *   type confusion: slab_free() interprets attacker-controlled heap bytes as a
 *   slab_t header (cache pointer, lock, free-list) and performs an atomic RMW
 *   through them.
 *
 * Fix (HEAD, kfree/krealloc): classify by VA range via heap_owns() FIRST. The
 * heap window and the slab/direct-map region are disjoint, so heap_owns() is an
 * authoritative, unforgeable classifier -- a heap block is always freed via the
 * heap path regardless of its bytes.
 *
 * This test pulls the REAL kernel/core/mem/heap.c source directly into this
 * translation unit (the same "#include the .c" host-harness idiom used by
 * tests/unit/test_heap_segregated.c), built with -DHEAP_TEST_HOST so the heap
 * lives in a host-owned, page-aligned buffer we control. slab_cache_create()
 * is stubbed to return NULL for every size class (heap_init() still forces
 * slab_enabled = true unconditionally, exactly as it does on real hardware --
 * see heap.c: "slab_enabled = true;" runs after the cache-creation loop with
 * no gate on whether any cache actually got created). That means:
 *   - kmalloc() never dispatches to slab_alloc() (every slab_caches[i] is NULL),
 *     so every allocation we hand out in this test is guaranteed heap-owned.
 *   - kfree()/krealloc()'s slab-magic branch is still LIVE (slab_enabled==true),
 *     which is exactly the condition needed for the pre-fix bug to fire.
 * slab_free() is instrumented (not a real slab implementation) to record
 * whether it was invoked and with what pointer, without touching the forged
 * memory unsafely -- this observes the misroute directly instead of gambling
 * on whatever undefined behaviour a real slab_free() would produce when handed
 * a bogus cache pointer (which could crash, hang, or silently corrupt memory
 * unpredictably; instrumentation gives a deterministic, safe signal).
 *
 * Forcing page alignment:
 *   The host heap buffer is allocated 4096-byte aligned, so HEAP_START (via
 *   heap_test_base) is itself page-aligned (call it X). kmalloc() carves exact,
 *   16-byte-aligned byte counts with no extra padding beyond the 64-byte
 *   block_t header (verified by reading heap.c's kmalloc()/split logic). A
 *   first allocation of exactly 3968 bytes (4096 - 2*64) consumes
 *   [X, X+64+3968) = [X, X+4032); the very next allocation is therefore carved
 *   starting at header offset X+4032, giving a data pointer of
 *   X+4032+64 == X+4096 -- exactly page-aligned, with page_base == ptr, which
 *   is precisely the pre-fix bug's misroute condition.
 *
 * Discrimination:
 *   FIXED  (HEAD):    kfree(forged) -> heap_owns() true -> heap free path.
 *                      slab_free_called stays false; the block's real header
 *                      (untouched -- we only forged the DATA region) validates
 *                      and the memory is returned to the heap free-list, so a
 *                      subsequent kmalloc() of the same size reuses the exact
 *                      same address.
 *   BUGGY  (55926d4):  kfree(forged) -> slab-magic branch fires first ->
 *                      slab_free(NULL, forged) called and returns immediately,
 *                      NEVER reaching heap_owns()/the heap free-list.
 *                      slab_free_called becomes true (misroute observed) and
 *                      the block is never returned to the heap bins, so a
 *                      subsequent kmalloc() of the same size does NOT reuse the
 *                      forged address (heap_used is also left stale).
 *   Both symptoms are checked; either alone already discriminates fixed vs
 *   buggy, and printing NEGHMAGIC: FAIL <why> makes the failure mode explicit
 *   either way.
 *
 * Build & run (WSL / Linux host, gcc):
 *   gcc -std=c11 -O2 -g -DHEAP_TEST_HOST tests/unit/test_heap_neghmagic.c \
 *       -o /tmp/test_heap_neghmagic && /tmp/test_heap_neghmagic
 *   (also wired into tests/unit/Makefile as the `test_heap_neghmagic` target)
 *
 * Exit code: 0 on NEGHMAGIC: PASS, 1 on NEGHMAGIC: FAIL <reason>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* =========================================================================
 * Host shim -- replicate only what heap.c pulls from kernel headers
 * ========================================================================= */

#define PAGE_SIZE   4096

/* Host heap buffer. Must be page-aligned so HEAP_START (== heap_test_base) is
 * page-aligned, which is what lets us engineer a page-aligned data pointer
 * below. heap_test_base is the runtime HEAP_START used when heap.c is built
 * with HEAP_TEST_HOST. */
#define HOST_HEAP_SIZE  (16 * 1024 * 1024)
static uint8_t _heap_buf[HOST_HEAP_SIZE] __attribute__((aligned(4096)));
uint64_t heap_test_base;   /* referenced by heap.c when HEAP_TEST_HOST is set */

/* --- types.h shims --- */
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

/* --- kernel.h shims --- */
static int panic_count = 0;
static const char* last_panic = NULL;
#define NORETURN   __attribute__((noreturn))
#define PACKED     __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))
#define UNUSED     __attribute__((unused))

__attribute__((noreturn))
static void kernel_panic(const char* msg) {
    fprintf(stderr, "  [PANIC] %s\n", msg);
    panic_count++;
    last_panic = msg;
    /* A pre-fix run must never legitimately reach here for this test (the
     * forged block's real header is untouched, so block_magic_ok() holds and
     * no corruption/double-free panic should fire on either fixed or buggy
     * code). If it does, treat it as a hard FAIL via a distinct exit code so
     * it is never confused with a clean NEGHMAGIC: PASS/FAIL line. */
    printf("NEGHMAGIC: FAIL kernel_panic(\"%s\")\n", msg);
    exit(3);
}

/* assert_failed() backs ASSERT_ALWAYS() in kernel.h's real macro body. */
__attribute__((noreturn))
static void assert_failed(const char* expr, const char* file, int line) {
    fprintf(stderr, "  [ASSERT] %s at %s:%d\n", expr, file, line);
    printf("NEGHMAGIC: FAIL assert_failed(\"%s\")\n", expr);
    exit(3);
}

#define ASSERT_ALWAYS(expr) \
    do { if (!(expr)) { assert_failed(#expr, __FILE__, __LINE__); } } while (0)

static int kprintf(const char* fmt, ...) { (void)fmt; return 0; }

/* --- mem.h shims --- */
#define PAGE_PRESENT  0x01
#define PAGE_WRITE    0x02
#define PAGE_USER     0x04

static uint8_t* _next_page_ptr;
static void* pmm_alloc_page(void) {
    void* p = _next_page_ptr;
    _next_page_ptr += PAGE_SIZE;
    if (_next_page_ptr > _heap_buf + HOST_HEAP_SIZE)
        return NULL;
    return p;
}
static void* vmm_map_page(void* virt, void* phys, uint32_t flags) {
    (void)virt; (void)phys; (void)flags;
    return virt;
}
/* Referenced (but never actually called in this test) by heap_slab_benchmark(),
 * which is compiled unconditionally in heap.c. */
static uint64_t pmm_get_used_memory(void) {
    return (uint64_t)(_next_page_ptr - _heap_buf);
}

/* --- spinlock.h shims --- */
typedef struct { volatile uint32_t lock; uint32_t owner_cpu; const char* name; } spinlock_t;
static void spin_lock_init(spinlock_t* l) { l->lock=0; l->owner_cpu=0xFFFFFFFF; l->name=NULL; }
static void spin_lock(spinlock_t* l)   { l->lock=1; }
static void spin_unlock(spinlock_t* l) { l->lock=0; }

/* --- slab.h shims ---
 * Real slab_cache_create() always fails (returns NULL) here, so heap.c's
 * heap_init() logs a WARNING per size class but still unconditionally sets
 * slab_enabled = true afterwards (see heap.c) -- exactly like real boot,
 * where a slab-cache OOM at boot still leaves slab_enabled live. Because
 * every slab_caches[i] stays NULL, kmalloc() NEVER takes the slab_alloc()
 * fast path (it gates on slab_caches[i] != NULL), so every pointer this test
 * hands out is guaranteed heap-owned -- while kfree()/krealloc()'s
 * slab-magic branch (gated only on slab_enabled, not on any cache actually
 * existing) stays exercisable, which is what the pre-fix bug needs.
 *
 * slab_free() is instrumented rather than a real implementation: it records
 * that it was called and with which pointer instead of dereferencing the
 * (attacker-forged, in the buggy path) bytes at the page base as a slab_t
 * header. This observes the misroute deterministically instead of exercising
 * genuinely undefined behaviour on bogus memory. */
typedef struct slab_cache slab_cache_t;

static int    g_slab_free_called = 0;
static void*  g_slab_free_arg    = NULL;

static slab_cache_t* slab_cache_create(const char* name, size_t obj_size, size_t align) {
    (void)name; (void)obj_size; (void)align;
    return NULL;
}
static void* slab_alloc(slab_cache_t* c) {
    (void)c;
    return NULL; /* never reached: no slab_caches[i] is ever non-NULL */
}
static void slab_free(slab_cache_t* c, void* obj) {
    (void)c;
    g_slab_free_called++;
    g_slab_free_arg = obj;
}

/* =========================================================================
 * Pull in the heap implementation source directly
 * ========================================================================= */

#define MEM_H
#define KERNEL_H
#define SPINLOCK_H
#define SLAB_H
#define TYPES_H
#ifndef NULL
#define NULL ((void*)0)
#endif

/* krealloc() (defined earlier in heap.c than kfree()) calls kfree() in its
 * new_size==0 and GROW paths; forward-declare it so heap.c's single
 * translation unit sees a prototype before that first call site. */
void kfree(void* ptr);

#include "../../kernel/core/mem/heap.c"

/* =========================================================================
 * Test
 * ========================================================================= */

/* Must match heap.c's private SLAB_MAGIC constant exactly (kfree()/krealloc()
 * compare against this literal; slab.c defines the same value independently). */
#define SLAB_MAGIC_VALUE 0x51AB0BACE51AB0BULL

static void fail(const char* why) {
    printf("NEGHMAGIC: FAIL %s\n", why);
    exit(1);
}

int main(void) {
    if (sizeof(block_t) != 64) fail("block_t size != 64 (layout assumption broken)");

    heap_test_base = (uint64_t)(uintptr_t)_heap_buf;
    _next_page_ptr = _heap_buf;

    heap_init();

    if (!slab_enabled) fail("slab_enabled is false (test precondition not met -- "
                             "heap_init() must set it true even with no live caches)");
    for (int i = 0; i < NUM_SLAB_CACHES; i++)
        if (slab_caches[i] != NULL) fail("a slab cache unexpectedly exists (test setup bug)");

    if (((uintptr_t)heap_test_base & (PAGE_SIZE - 1)) != 0)
        fail("host heap buffer is not page-aligned (test setup bug)");

    /* Step 1: sacrificial allocation sized so the NEXT block's data pointer
     * lands exactly on a page boundary (see file header comment for the
     * arithmetic: 4096 - 2*sizeof(block_t) == 3968). */
    size_t sac_size = PAGE_SIZE - 2 * sizeof(block_t);
    void* sacrificial = kmalloc(sac_size);
    if (!sacrificial) fail("sacrificial kmalloc failed");
    if ((uintptr_t)sacrificial != heap_test_base + sizeof(block_t))
        fail("sacrificial allocation landed at an unexpected address (heap layout assumption broken)");

    /* Step 2: the block we will forge and free. Must be page-aligned. */
    void* victim = kmalloc(64);
    if (!victim) fail("victim kmalloc failed");
    if (((uintptr_t)victim & (PAGE_SIZE - 1)) != 0)
        fail("victim allocation is not page-aligned (heap layout assumption broken)");
    if ((uintptr_t)victim != heap_test_base + PAGE_SIZE)
        fail("victim allocation landed at an unexpected address (heap layout assumption broken)");

    /* Sanity: victim is genuinely heap-owned before we forge anything. */
    if (!heap_owns(victim)) fail("victim is not heap_owns() before forging (test setup bug)");

    /* Step 3: forge the SLAB_MAGIC sentinel into the victim's DATA region
     * only (never touch the real block_t header, which lives 64 bytes
     * *before* victim at heap_test_base+PAGE_SIZE-64). This is exactly the
     * attacker-controlled write a caller-freeable buffer allows. */
    memset(victim, 0, 64);
    *(uint64_t*)victim = SLAB_MAGIC_VALUE;

    uintptr_t page_base = (uintptr_t)victim & ~((uintptr_t)PAGE_SIZE - 1);
    if (page_base != (uintptr_t)victim) fail("page_base != victim (alignment assumption broken)");
    if (*(uint64_t*)page_base != SLAB_MAGIC_VALUE) fail("forged magic not readable at page_base (test setup bug)");

    /* Step 4: the actual discriminator. Free the forged, page-aligned,
     * heap-owned block. */
    g_slab_free_called = 0;
    g_slab_free_arg = NULL;
    kfree(victim);

    if (g_slab_free_called) {
        fail("kfree() misrouted a heap-owned block into slab_free() because its "
             "first 8 bytes matched SLAB_MAGIC -- pre-fix type-confusion bug is present");
    }

    /* Confirm the heap path actually ran: the freed block must be reusable
     * by a same-size kmalloc() at the SAME address (heap free-list reuse).
     * On the buggy kernel the block is never returned to any heap bin (it
     * was "freed" via the bogus slab_free() instead), so this would either
     * return a different, freshly-carved address or fail outright. */
    void* reused = kmalloc(64);
    if (!reused) fail("post-free kmalloc(64) failed (block was not returned to the heap free-list)");
    if (reused != victim) {
        fail("post-free kmalloc(64) did not reuse the freed block's address "
             "(block was not returned to the heap free-list -- consistent with the misroute bug)");
    }

    /* --- Secondary check: krealloc() has the identical pre-fix ordering bug
     * (see git show 55926d4:kernel/core/mem/heap.c). Re-forge a fresh
     * page-aligned victim and confirm krealloc() also takes the heap path. */
    kfree(reused);
    void* victim2 = kmalloc(64);
    if (!victim2 || victim2 != victim)
        fail("could not re-obtain the page-aligned block for the krealloc sub-check");

    memset(victim2, 0, 64);
    *(uint64_t*)victim2 = SLAB_MAGIC_VALUE;

    g_slab_free_called = 0;
    g_slab_free_arg = NULL;
    void* grown = krealloc(victim2, 256);

    if (g_slab_free_called) {
        fail("krealloc() misrouted a heap-owned block into slab_free() because its "
             "first 8 bytes matched SLAB_MAGIC -- pre-fix type-confusion bug is present (krealloc)");
    }
    if (!grown) fail("krealloc() on the forged heap block returned NULL on the fixed heap path");

    kfree(grown);

    printf("NEGHMAGIC: PASS\n");
    return 0;
}
