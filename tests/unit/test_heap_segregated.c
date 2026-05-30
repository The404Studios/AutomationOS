/*
 * tests/unit/test_heap_segregated.c
 *
 * Self-contained host test for the segregated-free-list heap.
 * Does NOT include kernel headers — reimplements only what heap.c needs.
 *
 * Compile:
 *   gcc -std=c11 -O2 -g -fsanitize=address,undefined \
 *       tests/unit/test_heap_segregated.c -o /tmp/test_heap && /tmp/test_heap
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* =========================================================================
 * Host shim — replicate only what heap.c pulls from kernel headers
 * ========================================================================= */

#define PAGE_SIZE   4096

/*
 * Host heap buffer.  heap_test_base is set to its address before heap_init().
 * HEAP_TEST_HOST tells heap.c to treat HEAP_START as this runtime variable.
 */
#define HOST_HEAP_SIZE  (16 * 1024 * 1024)
static uint8_t _heap_buf[HOST_HEAP_SIZE] __attribute__((aligned(64)));
uint64_t heap_test_base;   /* referenced by heap.c when HEAP_TEST_HOST is set */

/* --- types.h shims (use host types, avoid redefining stdint types) --- */
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
/* size_t, bool, uint* etc. come from standard C headers above */

/* --- kernel.h shims --- */
static int panic_count = 0;
static const char* last_panic = NULL;
#define NORETURN   __attribute__((noreturn))
#define PACKED     __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))
#define UNUSED     __attribute__((unused))

/*
 * kernel_panic: in the host test we need different behaviour depending on
 * the return type of the calling function.  We implement it as a real
 * function that calls exit() so the NORETURN attribute is satisfied for
 * every call site (kmalloc, kfree, heap_init all call it).
 */
__attribute__((noreturn))
static void kernel_panic(const char* msg) {
    fprintf(stderr, "  [PANIC] %s\n", msg);
    panic_count++;
    last_panic = msg;
    exit(1);
}

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

/* --- spinlock.h shims --- */
typedef struct { volatile uint32_t lock; uint32_t owner_cpu; const char* name; } spinlock_t;
static void spin_lock_init(spinlock_t* l) { l->lock=0; l->owner_cpu=0xFFFFFFFF; l->name=NULL; }
static void spin_lock(spinlock_t* l)   { l->lock=1; }
static void spin_unlock(spinlock_t* l) { l->lock=0; }

/* =========================================================================
 * Pull in the heap implementation source directly
 * ========================================================================= */

/*
 * We replicate only the types/macros that heap.c uses from the kernel headers.
 * heap.c includes mem.h / kernel.h / spinlock.h — we guard against that with
 * sentinel defines.
 */
#define MEM_H
#define KERNEL_H
#define SPINLOCK_H
#define TYPES_H
/* types.h defines NULL, size_t etc — gate them */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Provide size_t alias that heap.c uses (it comes from types.h normally) */
/* Already provided by <stdint.h>/<stddef.h> via stdio.h */

#include "../../kernel/core/mem/heap.c"

/* =========================================================================
 * Test framework
 * ========================================================================= */

static int failures = 0;

#define TEST(name) do { printf("  %-60s", (name)); fflush(stdout); } while(0)
#define PASS()     printf("PASS\n")
#define FAIL(r)    do { printf("FAIL (%s)\n", (r)); failures++; } while(0)

static bool is_16aligned(void* p) { return ((uintptr_t)p & 15) == 0; }
static bool no_overlap(void* a, size_t sa, void* b, size_t sb) {
    return ((uint8_t*)a + sa <= (uint8_t*)b) ||
           ((uint8_t*)b + sb <= (uint8_t*)a);
}

/* =========================================================================
 * Tests
 * ========================================================================= */

static void t_basic(void) {
    TEST("basic alloc/free");
    void* p = kmalloc(64);
    if (!p)              { FAIL("null"); return; }
    if (!is_16aligned(p)){ FAIL("unaligned"); return; }
    memset(p, 0xAB, 64);
    kfree(p);
    PASS();
}

static void t_null_size(void) {
    TEST("kmalloc(0) == NULL");
    void* p = kmalloc(0);
    if (p) { kfree(p); FAIL("non-null"); return; }
    PASS();
}

static void t_alignment_sizes(void) {
    TEST("16-byte alignment for many sizes");
    size_t sizes[] = {1,7,15,16,17,31,32,33,63,64,127,128,255,256,
                      511,512,1023,1024,2047,4095,4096,8191,16383,32767};
    int n = (int)(sizeof sizes/sizeof sizes[0]);
    void* ptrs[32];
    for (int i = 0; i < n; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i] || !is_16aligned(ptrs[i])) {
            for (int j=0;j<i;j++) kfree(ptrs[j]);
            FAIL("null/unaligned"); return;
        }
    }
    for (int i = 0; i < n; i++) kfree(ptrs[i]);
    PASS();
}

static void t_no_overlap(void) {
    TEST("no overlap: 512 live allocations");
#define NB 512
    void*  ptrs[NB];
    size_t szs[NB];
    srand(42);
    for (int i = 0; i < NB; i++) {
        szs[i] = (size_t)(rand() % 256 + 1);
        ptrs[i] = kmalloc(szs[i]);
        if (!ptrs[i]) { FAIL("OOM"); return; }
        memset(ptrs[i], (uint8_t)i, szs[i]);
    }
    bool ok = true;
    for (int i = 0; i < NB && ok; i++)
        for (int j = i+1; j < NB && ok; j++)
            if (!no_overlap(ptrs[i],szs[i],ptrs[j],szs[j])) ok = false;
    if (!ok) { for(int i=0;i<NB;i++) kfree(ptrs[i]); FAIL("overlap"); return; }
    /* fill integrity */
    for (int i = 0; i < NB; i++) {
        uint8_t* p = ptrs[i];
        for (size_t k = 0; k < szs[i]; k++)
            if (p[k] != (uint8_t)i) {
                for(int j=0;j<NB;j++) kfree(ptrs[j]);
                FAIL("fill corrupted"); return;
            }
    }
    for (int i = 0; i < NB; i++) kfree(ptrs[i]);
    PASS();
#undef NB
}

static void t_coalesce(void) {
    TEST("coalescing: free all then alloc 64KiB");
    void* small[128];
    for (int i = 0; i < 128; i++) small[i] = kmalloc(64);
    for (int i = 0; i < 128; i++) kfree(small[i]);
    void* big = kmalloc(64 * 1024);
    if (!big) { FAIL("OOM after coalesce"); return; }
    kfree(big);
    PASS();
}

static void t_interleaved(void) {
    TEST("interleaved alloc/free stress (2000 ops)");
#define LV 64
    void*  ptrs[LV]; memset(ptrs,0,sizeof ptrs);
    size_t szs[LV];
    srand(1337);
    bool ok = true;
    for (int op = 0; op < 2000 && ok; op++) {
        int slot = rand() % LV;
        if (ptrs[slot]) {
            kfree(ptrs[slot]); ptrs[slot] = NULL;
        } else {
            szs[slot] = (size_t)(rand() % 512 + 1);
            ptrs[slot] = kmalloc(szs[slot]);
            if (ptrs[slot] && !is_16aligned(ptrs[slot])) { ok = false; break; }
            if (ptrs[slot]) memset(ptrs[slot], (uint8_t)slot, szs[slot]);
        }
    }
    for (int i = 0; i < LV; i++) if (ptrs[i]) kfree(ptrs[i]);
    if (!ok) FAIL("misaligned"); else PASS();
#undef LV
}

static void t_fill_check(void) {
    TEST("fill check: neighbours not stomped");
    void* a = kmalloc(32);
    void* b = kmalloc(32);
    void* c = kmalloc(32);
    if (!a||!b||!c) { FAIL("OOM"); return; }
    memset(a,0xAA,32); memset(b,0xBB,32); memset(c,0xCC,32);
    bool ok = true;
    for (int i = 0; i < 32; i++) {
        if (((uint8_t*)a)[i]!=0xAA||((uint8_t*)b)[i]!=0xBB||((uint8_t*)c)[i]!=0xCC)
            { ok = false; break; }
    }
    kfree(a); kfree(b); kfree(c);
    if (!ok) FAIL("fill stomped"); else PASS();
}

static void t_realloc_pattern(void) {
    TEST("free then re-alloc same size");
    void* p1 = kmalloc(128);
    if (!p1) { FAIL("first"); return; }
    memset(p1,0x55,128);
    kfree(p1);
    void* p2 = kmalloc(128);
    if (!p2) { FAIL("second"); return; }
    if (!is_16aligned(p2)) { kfree(p2); FAIL("unaligned"); return; }
    memset(p2,0x66,128);
    uint8_t* q = p2;
    bool ok = true;
    for (int i = 0; i < 128; i++) if (q[i]!=0x66) { ok=false; break; }
    kfree(p2);
    if (!ok) FAIL("fill bad"); else PASS();
}

static void t_large(void) {
    TEST("1 MiB allocation");
    void* p = kmalloc(1024*1024);
    if (!p) { FAIL("OOM"); return; }
    if (!is_16aligned(p)) { kfree(p); FAIL("unaligned"); return; }
    memset(p, 0x7F, 1024*1024);
    kfree(p);
    PASS();
}

static void t_many_tiny(void) {
    TEST("1024 tiny (16-byte) allocs");
#define NT 1024
    void* ptrs[NT];
    for (int i = 0; i < NT; i++) {
        ptrs[i] = kmalloc(16);
        if (!ptrs[i]||!is_16aligned(ptrs[i])) {
            for(int j=0;j<i;j++) kfree(ptrs[j]);
            FAIL("null/unaligned"); return;
        }
        *(uint64_t*)ptrs[i] = (uint64_t)i;
    }
    bool ok = true;
    for (int i = 0; i < NT; i++)
        if (*(uint64_t*)ptrs[i] != (uint64_t)i) { ok=false; break; }
    for (int i = 0; i < NT; i++) kfree(ptrs[i]);
    if (!ok) FAIL("value corrupted"); else PASS();
#undef NT
}

static void t_random_mixed(void) {
    TEST("random mixed sizes 5000 ops (alignment + overlap)");
#define M 128
    void*  ptrs[M]; memset(ptrs,0,sizeof ptrs);
    size_t szs[M];
    srand(999);
    bool ok = true;
    for (int op = 0; op < 5000 && ok; op++) {
        int slot = rand() % M;
        if (ptrs[slot]) {
            kfree(ptrs[slot]); ptrs[slot] = NULL;
        } else {
            int r = rand() % 10;
            if (r < 6)      szs[slot] = (size_t)(rand()%128+1);
            else if (r < 9) szs[slot] = (size_t)(rand()%4096+129);
            else            szs[slot] = (size_t)(rand()%65536+4097);
            ptrs[slot] = kmalloc(szs[slot]);
            if (ptrs[slot]) {
                if (!is_16aligned(ptrs[slot])) { ok=false; break; }
                for (int j = 0; j < M; j++) {
                    if (j==slot||!ptrs[j]) continue;
                    if (!no_overlap(ptrs[slot],szs[slot],ptrs[j],szs[j]))
                        { ok=false; break; }
                }
            }
        }
    }
    for (int i = 0; i < M; i++) if (ptrs[i]) kfree(ptrs[i]);
    if (!ok) FAIL("misaligned/overlap"); else PASS();
#undef M
}

static void t_coalesce_alternating(void) {
    TEST("coalesce: alternating free (forward+backward merge)");
    /* Allocate 6 equal blocks, free evens, then odds — heap must merge */
    void* p[6];
    for (int i = 0; i < 6; i++) p[i] = kmalloc(256);
    for (int i = 0; i < 6; i++) if (!p[i]) { FAIL("OOM"); return; }
    kfree(p[1]); kfree(p[3]); kfree(p[5]);
    kfree(p[0]); kfree(p[2]); kfree(p[4]);
    /* All freed — large alloc should succeed */
    void* big = kmalloc(256 * 4);
    if (!big) { FAIL("failed after alternating free"); return; }
    kfree(big);
    PASS();
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    /* Set heap_test_base so heap.c's HEAP_TEST_HOST paths use the right base */
    heap_test_base  = (uint64_t)(uintptr_t)_heap_buf;
    _next_page_ptr  = _heap_buf;

    printf("=== Kernel segregated-free-list heap unit tests ===\n");
    printf("    block_t size = %u bytes (must be 64)\n", (unsigned)sizeof(block_t));
    printf("    heap buffer  = %p\n\n", (void*)_heap_buf);

    if (sizeof(block_t) != 64) {
        fprintf(stderr, "FATAL: block_t size is %u, expected 64\n",
                (unsigned)sizeof(block_t));
        return 2;
    }

    heap_init();

    t_basic();
    t_null_size();
    t_alignment_sizes();
    t_no_overlap();
    t_coalesce();
    t_interleaved();
    t_fill_check();
    t_realloc_pattern();
    t_large();
    t_many_tiny();
    t_random_mixed();
    t_coalesce_alternating();

    printf("\n");
    if (failures == 0)
        printf("All %d tests PASSED.\n", 12);
    else
        printf("%d test(s) FAILED.\n", failures);
    return failures ? 1 : 0;
}
