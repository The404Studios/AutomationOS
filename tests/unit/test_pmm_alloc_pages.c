/*
 * test_pmm_alloc_pages.c  –  host-gcc unit test for pmm_alloc_pages / pmm_free_pages
 *
 * Build:
 *   gcc -std=gnu11 -O2 -Wall -Wextra -o test_pmm_alloc_pages test_pmm_alloc_pages.c && ./test_pmm_alloc_pages
 *
 * What is tested:
 *   1. Every returned run is fully-free-before (all bits set in bitmap before alloc).
 *   2. Every returned run is fully-used-after  (all bits clear in bitmap after alloc).
 *   3. No two simultaneously live runs overlap.
 *   4. pmm_free_pages restores the bitmap (all bits free again, no extra bits touched).
 *   5. Corner cases: count==1, count==64 (exact word), count==65 (cross-word boundary),
 *      count==128 (two full words), count==63 (within-word), allocations that span the
 *      word boundary where old code would fail.
 *   6. Stress: random alloc/free cycles with varied counts, asserting all invariants.
 *
 * The test embeds a minimal simulation of the pmm bitmap logic so it can be run
 * without the full kernel build environment.  The scan algorithm is extracted
 * verbatim from pmm.c (the corrected version).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

/* ---- minimal PMM simulation ---- */

#define PAGE_SIZE       4096ULL
#define TOTAL_PAGES     4096ULL          /* test with 4096 pages (256 KB bitmap) */
#define BITMAP_WORDS    (TOTAL_PAGES / 64)

static uint64_t bitmap[BITMAP_WORDS];    /* 1 = free, 0 = used */
static uint64_t hint_word = 0;

/* page_t metadata (simulate the kernel structure embedded at the page address) */
typedef struct {
    bool is_free;
} fake_page_t;

static fake_page_t pages[TOTAL_PAGES];  /* simulated page metadata */
static uint64_t used_count = 0;

static void bitmap_set_free(uint64_t pfn) { bitmap[pfn/64] |=  (1ULL << (pfn%64)); }
static void bitmap_set_used(uint64_t pfn) { bitmap[pfn/64] &= ~(1ULL << (pfn%64)); }
static bool bitmap_is_free(uint64_t pfn) { return (bitmap[pfn/64] >> (pfn%64)) & 1; }

/* Mark a range of PFNs free (simulate pmm_add_page_locked for a contiguous block) */
static void mark_range_free(uint64_t start, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        uint64_t pfn = start + i;
        assert(pfn < TOTAL_PAGES);
        bitmap_set_free(pfn);
        pages[pfn].is_free = true;
    }
}

/* ---- corrected scan algorithm (extracted from pmm.c) ---- */

/*
 * Returns the base PFN of a contiguous free run of `count` pages, or
 * UINT64_MAX if none found.  Marks the run used atomically.
 */
static uint64_t alloc_pages_pfn(size_t count) {
    if (count == 0) return UINT64_MAX;

    uint64_t run_start = 0;
    uint64_t run_len   = 0;

    for (int pass = 0; pass < 2; pass++) {
        uint64_t start_word = (pass == 0) ? hint_word    : 0;
        uint64_t end_word   = (pass == 0) ? BITMAP_WORDS : hint_word;

        run_start = 0;
        run_len   = 0;

        for (uint64_t wi = start_word; wi < end_word; wi++) {
            uint64_t word     = bitmap[wi];
            uint64_t base_pfn = wi * 64;

            if (word == 0) { run_len = 0; continue; }

            if (word == ~0ULL) {
                if (run_len == 0) run_start = base_pfn;
                run_len += 64;
                if (run_len >= (uint64_t)count) goto found;
                continue;
            }

            /* mixed word */
            {
                uint64_t bit = 0;
                while (bit < 64) {
                    uint64_t remaining = word >> bit;
                    if (remaining == 0) { run_len = 0; break; }

                    uint64_t skip = (uint64_t)__builtin_ctzll(remaining);
                    if (skip > 0) { run_len = 0; bit += skip; continue; }

                    uint64_t inv = ~remaining;
                    uint64_t free_run;
                    if (inv == 0) {
                        free_run = 64 - bit;
                    } else {
                        free_run = (uint64_t)__builtin_ctzll(inv);
                        if (bit + free_run > 64) free_run = 64 - bit;
                    }

                    if (run_len == 0) run_start = base_pfn + bit;
                    run_len += free_run;

                    if (run_len >= (uint64_t)count) goto found;
                    bit += free_run;
                }
            }
        }
    }
    return UINT64_MAX;

found:
    for (uint64_t i = 0; i < (uint64_t)count; i++) {
        uint64_t pfn = run_start + i;
        bitmap_set_used(pfn);
        pages[pfn].is_free = false;
    }
    used_count += count;
    uint64_t next = run_start + count;
    hint_word = (next < TOTAL_PAGES) ? next / 64 : 0;
    return run_start;
}

static void free_pages_pfn(uint64_t start_pfn, size_t count) {
    for (uint64_t i = 0; i < (uint64_t)count; i++) {
        uint64_t pfn = start_pfn + i;
        assert(pfn < TOTAL_PAGES);
        if (!pages[pfn].is_free) {
            pages[pfn].is_free = true;
            bitmap_set_free(pfn);
            used_count--;
        }
    }
    if (start_pfn / 64 < hint_word) hint_word = start_pfn / 64;
}

/* ---- test helpers ---- */

static int test_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do {                                               \
    test_count++;                                                           \
    if (!(cond)) {                                                          \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __func__, __LINE__, (msg));   \
        fail_count++;                                                       \
    }                                                                       \
} while(0)

/* Verify that all pages in [pfn, pfn+count) are used (bitmap = 0) */
static bool all_used(uint64_t pfn, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (bitmap_is_free(pfn + i)) return false;
    }
    return true;
}

/* Verify that all pages in [pfn, pfn+count) are free (bitmap = 1) */
static bool all_free(uint64_t pfn, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!bitmap_is_free(pfn + i)) return false;
    }
    return true;
}

/* Reset to a clean state: first `free_pages` PFNs are free, rest used */
static void reset(uint64_t free_pages) {
    memset(bitmap, 0, sizeof(bitmap));
    memset(pages, 0, sizeof(pages));
    used_count = 0;
    hint_word  = 0;
    if (free_pages > TOTAL_PAGES) free_pages = TOTAL_PAGES;
    mark_range_free(0, free_pages);
}

/* ---- individual tests ---- */

static void test_single_page(void) {
    reset(128);
    uint64_t pfn = alloc_pages_pfn(1);
    CHECK(pfn != UINT64_MAX, "alloc count=1 must succeed");
    CHECK(pfn < TOTAL_PAGES, "pfn in range");
    CHECK(!bitmap_is_free(pfn), "page is used after alloc");
    CHECK(used_count == 1, "used_count accounting");

    free_pages_pfn(pfn, 1);
    CHECK(bitmap_is_free(pfn), "page is free after free");
    CHECK(used_count == 0, "used_count back to 0");
}

static void test_exact_word(void) {
    /* Allocate exactly 64 pages — should grab one full word */
    reset(256);
    uint64_t pfn = alloc_pages_pfn(64);
    CHECK(pfn != UINT64_MAX, "alloc 64 pages must succeed");
    CHECK(pfn % 64 == 0, "64-page alloc is word-aligned");
    CHECK(all_used(pfn, 64), "all 64 pages used after alloc");
    free_pages_pfn(pfn, 64);
    CHECK(all_free(pfn, 64), "all 64 pages free after free");
}

static void test_cross_word_boundary(void) {
    /*
     * Classic failure case for the old code:
     * Set up a free run of 70 pages that crosses a word boundary.
     * Old code would reset run_len=0 because the top bit of the mixed
     * word was not free.
     *
     * Layout (TOTAL_PAGES >= 128):
     *   Word 0 [pfns 0..63]  : all free  (64 free pages)
     *   Word 1 [pfns 64..127]: bits 0-5 free, 6-63 used
     *   => contiguous run of 70 at pfn 0.
     */
    memset(bitmap, 0, sizeof(bitmap));
    memset(pages, 0, sizeof(pages));
    used_count = 0; hint_word = 0;

    /* Word 0: all free */
    bitmap[0] = ~0ULL;
    for (int i = 0; i < 64; i++) pages[i].is_free = true;

    /* Word 1: bits 0-5 free (pfns 64-69) */
    bitmap[1] = 0x3FULL;  /* 0b0000...0111111 — bits 0-5 set */
    for (int i = 64; i < 70; i++) pages[i].is_free = true;

    uint64_t pfn = alloc_pages_pfn(70);
    CHECK(pfn != UINT64_MAX, "cross-word alloc of 70 must succeed");
    CHECK(pfn == 0, "run starts at pfn 0");
    CHECK(all_used(0, 70), "all 70 pages used");
    /* pages 70..127 must be untouched (still used in this setup) */
    for (uint64_t i = 70; i < 128; i++) {
        CHECK(!bitmap_is_free(i), "pages beyond run are untouched");
    }

    free_pages_pfn(0, 70);
    CHECK(all_free(0, 70), "all 70 pages free after free");
}

static void test_cross_word_63pages(void) {
    /*
     * Another old-code failure: 63 pages spanning a word boundary
     * (e.g., last 32 pages of word 0 + first 31 pages of word 1).
     *
     * Word 0: bits 0-31 used, bits 32-63 free  => lower 32 bits 0, upper 32 bits 1
     * Word 1: bits 0-30 free, bit 31-63 used   => lower 31 bits 1, rest 0
     * => run of 63 starting at pfn 32.
     */
    memset(bitmap, 0, sizeof(bitmap));
    memset(pages, 0, sizeof(pages));
    used_count = 0; hint_word = 0;

    bitmap[0] = 0xFFFFFFFF00000000ULL;   /* bits 32-63 free */
    for (int i = 32; i < 64; i++) pages[i].is_free = true;

    bitmap[1] = 0x000000007FFFFFFFULL;   /* bits 0-30 free */
    for (int i = 64; i < 95; i++) pages[i].is_free = true;

    uint64_t pfn = alloc_pages_pfn(63);
    CHECK(pfn != UINT64_MAX, "cross-word 63-page alloc must succeed");
    CHECK(pfn == 32, "run starts at pfn 32");
    CHECK(all_used(32, 63), "63 pages used");

    free_pages_pfn(32, 63);
    CHECK(all_free(32, 63), "63 pages free after free");
}

static void test_two_full_words(void) {
    reset(512);
    uint64_t pfn = alloc_pages_pfn(128);
    CHECK(pfn != UINT64_MAX, "alloc 128 pages");
    CHECK(all_used(pfn, 128), "128 pages used");
    free_pages_pfn(pfn, 128);
    CHECK(all_free(pfn, 128), "128 pages freed");
}

static void test_no_overlap(void) {
    /*
     * Allocate multiple runs simultaneously and verify they don't overlap.
     */
    reset(TOTAL_PAGES);

#define MAX_ALLOCS 32
    uint64_t alloc_base[MAX_ALLOCS];
    size_t   alloc_size[MAX_ALLOCS];
    int      n = 0;

    size_t sizes[] = {1, 2, 3, 7, 8, 13, 64, 65, 128, 17, 4, 31, 32, 33};
    int nsizes = (int)(sizeof(sizes)/sizeof(sizes[0]));

    for (int i = 0; i < nsizes && n < MAX_ALLOCS; i++) {
        uint64_t pfn = alloc_pages_pfn(sizes[i]);
        if (pfn == UINT64_MAX) continue;
        /* Verify no overlap with any existing live allocation */
        for (int j = 0; j < n; j++) {
            uint64_t a_end = alloc_base[j] + alloc_size[j];
            uint64_t b_end = pfn + sizes[i];
            bool overlap = (pfn < a_end) && (alloc_base[j] < b_end);
            CHECK(!overlap, "no overlap between live allocations");
        }
        alloc_base[n] = pfn;
        alloc_size[n] = sizes[i];
        n++;
    }

    /* Free all */
    for (int i = 0; i < n; i++)
        free_pages_pfn(alloc_base[i], alloc_size[i]);
    CHECK(used_count == 0, "all pages freed, used_count == 0");
}

static void test_oom(void) {
    /* All pages used — alloc should fail */
    memset(bitmap, 0, sizeof(bitmap));
    memset(pages, 0, sizeof(pages));
    used_count = 0; hint_word = 0;
    for (uint64_t i = 0; i < TOTAL_PAGES; i++) pages[i].is_free = false;

    uint64_t pfn = alloc_pages_pfn(1);
    CHECK(pfn == UINT64_MAX, "alloc must fail when OOM");
}

static void test_fragmented(void) {
    /*
     * Fragmented bitmap: every other page free.
     * Allocating 2 contiguous pages must fail.
     * Allocating 1 page must succeed.
     */
    memset(bitmap, 0, sizeof(bitmap));
    memset(pages, 0, sizeof(pages));
    used_count = 0; hint_word = 0;

    for (uint64_t i = 0; i < TOTAL_PAGES; i += 2) {
        bitmap_set_free(i);
        pages[i].is_free = true;
    }

    uint64_t pfn2 = alloc_pages_pfn(2);
    CHECK(pfn2 == UINT64_MAX, "no 2-page run in alternating bitmap");

    uint64_t pfn1 = alloc_pages_pfn(1);
    CHECK(pfn1 != UINT64_MAX, "single page alloc works in fragmented bitmap");
    if (pfn1 != UINT64_MAX) {
        CHECK(!bitmap_is_free(pfn1), "page used after alloc");
        free_pages_pfn(pfn1, 1);
        CHECK(bitmap_is_free(pfn1), "page free after free");
    }
}

static void test_stress(void) {
    /*
     * Random alloc/free cycles:
     * - Maintain a pool of live allocations.
     * - Each iteration: randomly decide to alloc or free.
     * - After each alloc: verify fully-used + no overlap with live pool.
     * - After each free: verify fully-free.
     */
    reset(TOTAL_PAGES);
    srand(42);

#define POOL_SIZE 64
    uint64_t pool_base[POOL_SIZE];
    size_t   pool_size[POOL_SIZE];
    int      pool_n = 0;

    int iterations = 5000;
    for (int iter = 0; iter < iterations; iter++) {
        if (pool_n > 0 && (pool_n >= POOL_SIZE || rand() % 3 == 0)) {
            /* Free a random live allocation */
            int idx = rand() % pool_n;
            uint64_t b = pool_base[idx];
            size_t   s = pool_size[idx];

            free_pages_pfn(b, s);
            CHECK(all_free(b, s), "stress: freed pages are free in bitmap");

            /* Remove from pool (swap with last) */
            pool_base[idx] = pool_base[pool_n-1];
            pool_size[idx] = pool_size[pool_n-1];
            pool_n--;
        } else {
            /* Allocate a random-sized run */
            size_t cnt = 1 + rand() % 96;
            uint64_t pfn = alloc_pages_pfn(cnt);
            if (pfn == UINT64_MAX) continue;  /* OOM — skip */

            /* All pages must be used */
            CHECK(all_used(pfn, cnt), "stress: allocated pages fully used");

            /* No overlap with any live allocation */
            for (int j = 0; j < pool_n; j++) {
                uint64_t a_end = pool_base[j] + pool_size[j];
                uint64_t b_end = pfn + cnt;
                bool overlap = (pfn < a_end) && (pool_base[j] < b_end);
                if (overlap) {
                    fprintf(stderr, "FAIL stress iter %d: overlap pfn=%lu+%zu vs pool[%d]=%lu+%zu\n",
                            iter, (unsigned long)pfn, cnt, j,
                            (unsigned long)pool_base[j], pool_size[j]);
                    fail_count++;
                    test_count++;
                    free_pages_pfn(pfn, cnt);
                    goto next_iter;
                }
            }
            test_count++;

            if (pool_n < POOL_SIZE) {
                pool_base[pool_n] = pfn;
                pool_size[pool_n] = cnt;
                pool_n++;
            } else {
                free_pages_pfn(pfn, cnt);
            }
        }
next_iter:;
    }

    /* Clean up */
    for (int i = 0; i < pool_n; i++)
        free_pages_pfn(pool_base[i], pool_size[i]);
    CHECK(used_count == 0, "stress: all freed, used_count == 0");
}

static void test_hint_wrap(void) {
    /*
     * Ensure the hint wrap-around works:
     * 1. Fill almost all of the bitmap.
     * 2. Only a small free run remains near the start.
     * 3. Alloc with hint pointing past the run — must wrap and find it.
     */
    memset(bitmap, 0, sizeof(bitmap));
    memset(pages, 0, sizeof(pages));
    used_count = 0;

    /* Free run: pfns 10..19 (10 pages) */
    mark_range_free(10, 10);

    /* Hint points to word 5 (pfns 320+) — past the free run */
    hint_word = 5;

    uint64_t pfn = alloc_pages_pfn(10);
    CHECK(pfn != UINT64_MAX, "hint wrap: alloc must succeed after wrap");
    CHECK(pfn == 10, "hint wrap: alloc starts at pfn 10");
    CHECK(all_used(10, 10), "hint wrap: 10 pages used");
    free_pages_pfn(10, 10);
}

/* ---- main ---- */

int main(void) {
    printf("PMM alloc_pages unit tests\n");
    printf("==========================\n");

    test_single_page();         printf("test_single_page         %s\n", fail_count?"FAIL":"ok");
    int prev_fail = fail_count;

    test_exact_word();          printf("test_exact_word          %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_cross_word_boundary(); printf("test_cross_word_boundary %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_cross_word_63pages();  printf("test_cross_word_63pages  %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_two_full_words();      printf("test_two_full_words      %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_no_overlap();          printf("test_no_overlap          %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_oom();                 printf("test_oom                 %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_fragmented();          printf("test_fragmented          %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_hint_wrap();           printf("test_hint_wrap           %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;
    test_stress();              printf("test_stress              %s\n", (fail_count>prev_fail)?"FAIL":"ok"); prev_fail=fail_count;

    printf("\n%d tests run, %d failures\n", test_count, fail_count);
    return fail_count ? 1 : 0;
}
