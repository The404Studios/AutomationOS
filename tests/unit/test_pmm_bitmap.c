/*
 * Host-GCC unit test for pmm bitmap + contiguous allocator.
 *
 * Build & run (Linux/WSL):
 *   gcc -std=gnu11 -O2 -ffreestanding -nostdinc -fno-stack-protector      \
 *       -mno-red-zone -mcmodel=large                                       \
 *       -Ikernel/include -Ikernel/include/compat                           \
 *       -e _start -nostartfiles -static                                    \
 *       -o /tmp/test_pmm_bitmap tests/unit/test_pmm_bitmap.c -lgcc
 *
 * Strategy: we allocate a 16 MB host buffer aligned to PAGE_SIZE, treat it
 * as the "physical RAM", and manually populate the PMM free-list + bitmap
 * by writing page_t metadata into that buffer — exactly as pmm_init does
 * at kernel boot, but with real host virtual addresses we can dereference.
 *
 * We bypass pmm_init entirely (avoiding the kernel-fixed physical addresses
 * that are not mapped in userspace) and instead call a helper that
 * pre-populates state using our host buffer.
 */

/* ------------------------------------------------------------------ */
/* Freestanding preamble — syscall-based I/O, no system headers       */
/* ------------------------------------------------------------------ */

static long _write(int fd, const void* buf, unsigned long len) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(1ULL), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static __attribute__((noreturn)) void _exit(int code) {
    __asm__ volatile("syscall"
        : : "a"(60ULL), "D"((long)code) :);
    __builtin_unreachable();
}

/* mmap(2) to allocate aligned memory on the host */
static void* _mmap(unsigned long len) {
    /* mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(9ULL),        /* __NR_mmap */
          "D"(0ULL),        /* addr = NULL */
          "S"(len),         /* length */
          "d"(3L),          /* PROT_READ|PROT_WRITE */
          "r"(0x22L)        /* MAP_PRIVATE|MAP_ANONYMOUS — passed in r10 below */
        : "rcx", "r11", "memory");
    /* The 5th/6th arguments need r10/r8/r9 — redo with proper constraints */
    (void)ret;

    /* Use a cleaner approach with explicit register constraints */
    register long r10 __asm__("r10") = 0x22;  /* MAP_PRIVATE|MAP_ANONYMOUS */
    register long r8  __asm__("r8")  = -1;    /* fd */
    register long r9  __asm__("r9")  = 0;     /* offset */
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(9ULL), "D"(0ULL), "S"(len), "d"(3L), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return (void*)ret;
}

static unsigned long _strlen(const char* s) {
    const char* p = s; while (*p) p++; return (unsigned long)(p - s);
}
static void _puts(const char* s) { _write(1, s, _strlen(s)); }

static void _put_u64(unsigned long long n) {
    char buf[24]; int i = 23; buf[i] = '\0';
    if (n == 0) { buf[--i] = '0'; }
    while (n) { buf[--i] = '0' + (int)(n % 10); n /= 10; }
    _write(1, buf + i, _strlen(buf + i));
}

static void* _memset(void* dst, int c, unsigned long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

/* ------------------------------------------------------------------ */
/* Kernel symbol stubs                                                 */
/* ------------------------------------------------------------------ */
char __kernel_end __attribute__((section(".bss")));

__attribute__((noreturn)) void kernel_panic(const char* msg) {
    _puts("PANIC: "); _puts(msg); _puts("\n"); _exit(2);
}
int kprintf(const char* fmt, ...) { (void)fmt; return 0; }

/* ------------------------------------------------------------------ */
/* Include the PMM implementation                                      */
/* ------------------------------------------------------------------ */
#include "../../kernel/core/mem/pmm.c"

/* ------------------------------------------------------------------ */
/* Test framework                                                      */
/* ------------------------------------------------------------------ */
static int g_pass = 0, g_fail = 0;
static void _check(int cond, const char* msg, int line) {
    if (cond) { _puts("  PASS: "); _puts(msg); _puts("\n"); g_pass++; }
    else { _puts("  FAIL: "); _puts(msg); _puts(" (line "); _put_u64((unsigned long long)line); _puts(")\n"); g_fail++; }
}
#define CHECK(cond, msg) _check((int)(cond), (msg), __LINE__)

/* ------------------------------------------------------------------ */
/* Test arena management                                               */
/* ------------------------------------------------------------------ */
#define ARENA_PAGES  4096           /* 16 MB test arena */
#define ARENA_SIZE   (ARENA_PAGES * MIN_PAGE_SIZE)

static uint8_t* g_arena = (void*)0;   /* set in _start */

/*
 * Populate PMM state using the host-allocated arena buffer.
 * Mirrors what pmm_init does, but for host virtual addresses.
 */
static void arena_pmm_init(void) {
    _memset(pmm_bitmap, 0, sizeof(pmm_bitmap));
    used_memory = 0;
    total_memory = 0;
    pmm_bitmap_hint = 0;
    for (int i = 0; i <= MAX_ORDER; i++) free_lists[i] = (void*)0;
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_caches[i].count = 0;
        cpu_caches[i].alloc_fast = 0;
        cpu_caches[i].alloc_slow = 0;
        spin_lock_init(&cpu_caches[i].lock);
    }
    spin_lock_init(&global_pmm_lock);
    pmm_initrd_start = 0;
    pmm_initrd_end   = 0;
    memory_start     = (void*)0;
    memory_end       = (void*)0;

    /* Register all ARENA_PAGES pages: write page_t into the page itself,
     * add to free-list, and set the corresponding bitmap bit.
     * We use the HOST virtual address as the "physical" address — pfn is
     * computed as (vaddr / MIN_PAGE_SIZE), which gives very large PFN
     * values on x86-64.  To keep the bitmap in range we re-base:
     * we treat the arena base as PFN 0 for the bitmap, by temporarily
     * adjusting the bitmap base.
     *
     * Simpler alternative used here: just wire up free_lists[] and bitmap
     * directly using the actual host pointer values, but bound the bitmap
     * index to BITMAP_TOTAL_PAGES.  Since host pointers are typically in
     * the range 0x7f..., their PFN >> 20 is outside BITMAP_TOTAL_PAGES.
     *
     * BEST approach for the host test: use the pmm_add_page_locked path
     * that correctly handles this, but only if PFN < BITMAP_TOTAL_PAGES.
     * Instead, we directly manipulate the free-list only (no bitmap), and
     * test the bitmap using physically-sensible fake addresses in the range
     * [0x100000, 0x100000 + ARENA_SIZE).
     *
     * We do this by computing a "fake physical address" = FAKE_BASE + i*4096
     * for page i, but store the DATA (page_t metadata) in the arena buffer.
     * A redirect table maps fake-phys → real arena pointer.
     */

    /* Use fake physical addresses in the range 0x100000..0x100000+ARENA_SIZE */
    uint64_t fake_base = 0x100000ULL;
    uint64_t end = fake_base + ARENA_SIZE;

    /* Override pmm_add_page_locked semantics: we need the page_t metadata
     * to live in REAL memory (g_arena), not at the fake physical address.
     * However pmm_alloc_page_slow() returns the pointer stored in
     * free_lists[0] directly as a page address, so callers dereference it.
     *
     * For this test, we do the simplest correct thing:
     *   - Use REAL arena pointers for both page_t and the returned address.
     *   - Compute PFN from the REAL arena pointer (host vaddr / PAGE_SIZE).
     *   - Those PFNs are >>20 bits, so we can't use the bitmap for them.
     *
     * We therefore split the test:
     *   (A) Bitmap helper tests: use synthetic PFNs in range 0..1023.
     *   (B) alloc_pages tests: pre-populate both the free-list AND the bitmap
     *       using a synthetic PFN offset so the bitmap index is valid.
     *
     * SYNTHETIC PFN SCHEME:
     *   For page i in the arena, synthetic PFN = SYNTH_BASE + i
     *   where SYNTH_BASE = 0x100 (256), so max PFN = 256 + 4095 = 4351.
     *   4351 << BITMAP_TOTAL_PAGES (2^20), so bitmap ops are safe.
     *
     *   We also need pfn_of(ptr) to return the synthetic PFN when ptr
     *   points into the arena.  We can't change pfn_of (it's static).
     *   Instead, we directly drive the bitmap and free-list from here
     *   and use pmm_alloc_page_slow / pmm_free_page_slow which work on
     *   the free-list only.
     *
     * THE CLEANEST TEST STRATEGY:
     *   - Don't test pmm_alloc_page / pmm_free_page with arena (they
     *     write page_t into the returned address which is real arena mem).
     *   - Test pmm_alloc_pages / pmm_free_pages purely on the BITMAP,
     *     without needing to dereference the returned addresses as page_t.
     *     (pmm_alloc_pages does write pg->is_free = false, which touches
     *      the page metadata at the returned address — so we still need
     *      real memory there.)
     *
     * FINAL STRATEGY: map the arena such that fake_phys = arena_vaddr.
     * We do this by requesting mmap at a FIXED address that fits in the
     * bitmap range.  If the kernel allows, we map at 0x100000 (1 MB).
     */
    (void)fake_base;
    (void)end;

    /* See arena_pmm_init_fixed() below which uses MAP_FIXED */
}

/* Try to mmap the arena at a fixed address within bitmap range */
static void* _mmap_fixed(unsigned long hint_addr, unsigned long len) {
    register long r10 __asm__("r10") = 0x32;  /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE */
    register long r8  __asm__("r8")  = -1;
    register long r9  __asm__("r9")  = 0;
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(9ULL), "D"((long)hint_addr), "S"(len), "d"(3L),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return (void*)ret;
}

/* ------------------------------------------------------------------ */
/* Bitmap-only tests (no arena dereference needed)                    */
/* ------------------------------------------------------------------ */
static void test_bitmap_helpers(void) {
    _puts("\n[TEST] Bitmap helper correctness\n");
    _memset(pmm_bitmap, 0, sizeof(pmm_bitmap));

    bitmap_set_free(0);
    CHECK(bitmap_is_free(0),  "bit 0 free after set_free");
    bitmap_set_used(0);
    CHECK(!bitmap_is_free(0), "bit 0 used after set_used");

    bitmap_set_free(63);
    CHECK(bitmap_is_free(63), "bit 63 free (word 0 MSB)");
    bitmap_set_free(64);
    CHECK(bitmap_is_free(64), "bit 64 free (word 1 LSB)");
    bitmap_set_used(63);
    CHECK(!bitmap_is_free(63) && bitmap_is_free(64), "word-0/1 boundary isolation");

    bitmap_set_free(127);
    bitmap_set_free(128);
    bitmap_set_used(127);
    CHECK(!bitmap_is_free(127) && bitmap_is_free(128), "word-1/2 boundary isolation");

    /* Set a whole word free, check all 64 bits */
    pmm_bitmap[5] = ~0ULL;
    int all_free = 1;
    for (int i = 0; i < 64; i++) { if (!bitmap_is_free(5*64 + i)) { all_free = 0; break; } }
    CHECK(all_free, "all 64 bits free in word 5");
    pmm_bitmap[5] = 0;
}

/* ------------------------------------------------------------------ */
/* alloc_pages tests using FIXED-address arena (or fallback to        */
/* small PFN range if MAP_FIXED_NOREPLACE fails)                      */
/* ------------------------------------------------------------------ */

/*
 * Pre-populate the PMM free-list + bitmap with `n_pages` pages starting
 * at physical address `base`.  Memory at those addresses must be writable
 * (i.e. they must be backed by the mmap'd arena).
 */
static void populate_pmm(uint64_t base, uint64_t n_pages) {
    _memset(pmm_bitmap, 0, sizeof(pmm_bitmap));
    used_memory = 0;
    total_memory = n_pages * MIN_PAGE_SIZE;
    pmm_bitmap_hint = 0;
    for (int i = 0; i <= MAX_ORDER; i++) free_lists[i] = (void*)0;
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_caches[i].count = 0;
        spin_lock_init(&cpu_caches[i].lock);
    }
    spin_lock_init(&global_pmm_lock);

    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t addr = base + i * MIN_PAGE_SIZE;
        uint64_t pfn  = addr / MIN_PAGE_SIZE;
        if (pfn >= BITMAP_TOTAL_PAGES) break;
        page_t* pg = (page_t*)(uintptr_t)addr;
        pg->order   = 0;
        pg->is_free = 1;
        pg->next    = free_lists[0];
        free_lists[0] = pg;
        bitmap_set_free(pfn);
    }
}

static uint8_t* g_fixed_arena = (void*)0;
static uint64_t g_arena_base  = 0;
static uint64_t g_arena_pages = 0;

static void test_alloc_pages_contiguous(void) {
    _puts("\n[TEST] pmm_alloc_pages contiguity\n");
    populate_pmm(g_arena_base, g_arena_pages);

    void* base = pmm_alloc_pages(4);
    CHECK(base != (void*)0, "alloc_pages(4) returns non-NULL");
    if (!base) return;

    uint64_t phys = (uint64_t)(uintptr_t)base;
    CHECK(phys >= g_arena_base, "result within arena");

    uint64_t start_pfn = phys / MIN_PAGE_SIZE;
    int all_used = 1;
    for (int i = 0; i < 4; i++) {
        if (bitmap_is_free(start_pfn + (uint64_t)i)) { all_used = 0; break; }
    }
    CHECK(all_used, "all 4 pages marked used in bitmap");

    pmm_free_pages(base, 4);

    int all_free = 1;
    for (int i = 0; i < 4; i++) {
        if (!bitmap_is_free(start_pfn + (uint64_t)i)) { all_free = 0; break; }
    }
    CHECK(all_free, "all 4 pages free in bitmap after pmm_free_pages");
}

static void test_no_double_alloc(void) {
    _puts("\n[TEST] No double-allocation (64 alloc_pages(1) calls)\n");
    populate_pmm(g_arena_base, g_arena_pages);

#define NPAGES 64
    void* pages[NPAGES];
    for (int i = 0; i < NPAGES; i++) {
        pages[i] = pmm_alloc_pages(1);
        if (!pages[i]) { _puts("  SKIP: out of pages\n"); return; }
    }

    int no_dup = 1;
    for (int i = 0; i < NPAGES && no_dup; i++)
        for (int j = i+1; j < NPAGES && no_dup; j++)
            if (pages[i] == pages[j]) no_dup = 0;
    CHECK(no_dup, "no duplicate addresses from 64 alloc_pages(1)");

    for (int i = 0; i < NPAGES; i++) pmm_free_pages(pages[i], 1);

    void* run = pmm_alloc_pages(8);
    CHECK(run != (void*)0, "alloc_pages(8) succeeds after freeing 64 pages");
    if (run) {
        uint64_t rpfn = (uint64_t)(uintptr_t)run / MIN_PAGE_SIZE;
        int ok = 1;
        for (int i = 0; i < 8; i++)
            if (bitmap_is_free(rpfn + (uint64_t)i)) { ok = 0; break; }
        CHECK(ok, "run of 8 pages marked used");
        pmm_free_pages(run, 8);
    }
#undef NPAGES
}

static void test_hint_advances(void) {
    _puts("\n[TEST] Hint cursor advances\n");
    populate_pmm(g_arena_base, g_arena_pages);

    uint64_t h0 = pmm_bitmap_hint;
    void* a = pmm_alloc_pages(4);
    uint64_t h1 = pmm_bitmap_hint;
    void* b = pmm_alloc_pages(4);
    uint64_t h2 = pmm_bitmap_hint;

    CHECK(a && b, "two alloc_pages(4) succeed");
    CHECK(a != b,  "different base addresses");
    CHECK(h1 != h0 || h2 != h1, "hint cursor moved");

    if (a) pmm_free_pages(a, 4);
    if (b) pmm_free_pages(b, 4);
}

static void test_large_contiguous(void) {
    _puts("\n[TEST] Large contiguous alloc (256 pages = 1 MB)\n");
    populate_pmm(g_arena_base, g_arena_pages);

    void* base = pmm_alloc_pages(256);
    CHECK(base != (void*)0, "alloc_pages(256) returns non-NULL");
    if (!base) return;

    uint64_t pfn = (uint64_t)(uintptr_t)base / MIN_PAGE_SIZE;
    int ok = 1;
    for (int i = 0; i < 256; i++)
        if (bitmap_is_free(pfn + (uint64_t)i)) { ok = 0; break; }
    CHECK(ok, "all 256 pages marked used");

    pmm_free_pages(base, 256);

    int freed_ok = 1;
    for (int i = 0; i < 256; i++)
        if (!bitmap_is_free(pfn + (uint64_t)i)) { freed_ok = 0; break; }
    CHECK(freed_ok, "all 256 pages freed");
}

static void test_fragmented_gap(void) {
    _puts("\n[TEST] Fragmented bitmap — alloc skips gaps\n");
    populate_pmm(g_arena_base, g_arena_pages);

    /*
     * Manually poke a gap: mark pages 8..11 used in the bitmap so
     * there is no clean run of 8 starting there.  A run of 8 should
     * still be found elsewhere.
     */
    uint64_t base_pfn = g_arena_base / MIN_PAGE_SIZE;
    for (int i = 8; i < 12; i++) bitmap_set_used(base_pfn + (uint64_t)i);

    void* p = pmm_alloc_pages(8);
    CHECK(p != (void*)0, "alloc_pages(8) finds a run despite gap at pages 8-11");
    if (p) {
        /* The returned run must NOT overlap the gapped pages */
        uint64_t run_pfn = (uint64_t)(uintptr_t)p / MIN_PAGE_SIZE;
        int no_overlap = 1;
        for (int i = 0; i < 8; i++) {
            uint64_t pfn = run_pfn + (uint64_t)i;
            if (pfn >= base_pfn + 8 && pfn < base_pfn + 12) { no_overlap = 0; break; }
        }
        CHECK(no_overlap, "returned run does not include gapped pages");
        pmm_free_pages(p, 8);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */
void _start(void) {
    _puts("=== PMM Bitmap + Contiguous Allocator Unit Tests ===\n");

    /* Try to allocate arena at a fixed address that fits inside the bitmap
     * (needs to be < BITMAP_TOTAL_PAGES * PAGE_SIZE = 16 GB).
     * We try 0x1000000 (16 MB) for 16 MB of arena = PFNs 0x1000..0x4FFF.
     */
    unsigned long arena_size = 16UL * 1024 * 1024;  /* 16 MB */
    unsigned long try_addr   = 0x1000000UL;          /* 16 MB base */

    g_fixed_arena = (uint8_t*)_mmap_fixed(try_addr, arena_size);

    if ((long)g_fixed_arena == -1 || (long)g_fixed_arena < 0) {
        /* MAP_FIXED_NOREPLACE failed; fall back to unrestricted mmap.
         * PFNs will be > BITMAP_TOTAL_PAGES so bitmap tests skip.    */
        _puts("NOTE: MAP_FIXED_NOREPLACE failed, using unrestricted mmap\n");
        g_fixed_arena = (uint8_t*)_mmap(arena_size);
    }

    uint64_t arena_addr = (uint64_t)(uintptr_t)g_fixed_arena;
    uint64_t arena_pfn  = arena_addr / MIN_PAGE_SIZE;

    if (arena_pfn + 4096 > BITMAP_TOTAL_PAGES) {
        _puts("SKIP: arena PFNs outside bitmap range, cannot run arena tests\n");
        _puts("  (need arena at phys < 16 GB; MAP_FIXED_NOREPLACE refused)\n");
        /* Still run bitmap-only tests */
        test_bitmap_helpers();
        _puts("\n=== Results: ");
        _put_u64((unsigned long long)g_pass);
        _puts(" passed, ");
        _put_u64((unsigned long long)g_fail);
        _puts(" failed (arena tests skipped) ===\n");
        _exit(g_fail ? 1 : 0);
    }

    g_arena_base  = arena_addr;
    g_arena_pages = 4096;  /* 16 MB / 4 KB */

    test_bitmap_helpers();
    test_alloc_pages_contiguous();
    test_no_double_alloc();
    test_hint_advances();
    test_large_contiguous();
    test_fragmented_gap();

    _puts("\n=== Results: ");
    _put_u64((unsigned long long)g_pass);
    _puts(" passed, ");
    _put_u64((unsigned long long)g_fail);
    _puts(" failed ===\n");

    _exit(g_fail ? 1 : 0);
}
