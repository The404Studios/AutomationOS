// userspace/libc/selftest.c - In-process libc smoke test.
//
// libc_selftest() exercises the core formatting, allocation, sorting and
// string/conversion paths that ported tools depend on. It performs no I/O and
// allocates only from the libc heap, so an integrator can call it from any test
// app and check the return value: 0 == PASS, negative == first failing check.

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include "errno.h"

static int cmp_int(const void* a, const void* b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

// Compare two doubles within a relative/absolute tolerance.
static int dbl_near(double a, double b, double tol) {
    double diff = a - b;
    if (diff < 0.0) diff = -diff;
    double ref  = b < 0.0 ? -b : b;
    if (ref < 1.0) ref = 1.0;
    return diff / ref < tol;
}

int libc_selftest(void) {
    // --- snprintf: width, precision, flags, hex, string truncation. -------
    char buf[64];

    int n = snprintf(buf, sizeof(buf), "%d %05d %x %-4d|", 42, 7, 255, 3);
    if (n != (int)strlen(buf)) return -1;
    if (strcmp(buf, "42 00007 ff 3   |") != 0) return -2;

    snprintf(buf, sizeof(buf), "%s/%.3s/%8s", "ab", "abcdef", "x");
    if (strcmp(buf, "ab/abc/       x") != 0) return -3;

    // Bounded write must always NUL-terminate and report full length.
    n = snprintf(buf, 4, "%d", 123456);
    if (n != 6 || strcmp(buf, "123") != 0) return -4;

    snprintf(buf, sizeof(buf), "%+d %ld %#x", 5, (long)-9, 16u);
    if (strcmp(buf, "+5 -9 0x10") != 0) return -5;

    // --- malloc / free / realloc ------------------------------------------

    // Basic alloc + write + realloc preserving contents.
    char* p = (char*)malloc(32);
    if (!p) return -6;
    for (int i = 0; i < 32; i++) p[i] = (char)i;
    char* q = (char*)realloc(p, 64);
    if (!q) return -7;
    for (int i = 0; i < 32; i++) {
        if (q[i] != (char)i) return -8;  // realloc must preserve contents
    }
    free(q);

    // A freed block should be reusable (coalescing/first-fit sanity).
    void* a = malloc(1000);
    if (!a) return -9;
    free(a);
    void* b = malloc(1000);
    if (!b) return -10;
    // The second alloc should reuse the same freed space (first-fit).
    // We don't require the exact same pointer (split bookkeeping may differ),
    // but we DO require the allocation succeeds and is writable.
    ((char*)b)[0]   = 0xAB;
    ((char*)b)[999] = 0xCD;
    if ((unsigned char)((char*)b)[0]   != 0xAB) return -100;
    if ((unsigned char)((char*)b)[999] != 0xCD) return -101;
    free(b);

    // --- calloc: verify zero initialisation --------------------------------
    unsigned long* ca = (unsigned long*)calloc(16, sizeof(unsigned long));
    if (!ca) return -102;
    for (int i = 0; i < 16; i++) {
        if (ca[i] != 0UL) return -103;
    }
    // Write, then free, then re-calloc — new block must still be zeroed.
    for (int i = 0; i < 16; i++) ca[i] = (unsigned long)i * 0xDEADBEEFUL;
    free(ca);
    unsigned long* ca2 = (unsigned long*)calloc(16, sizeof(unsigned long));
    if (!ca2) return -104;
    for (int i = 0; i < 16; i++) {
        if (ca2[i] != 0UL) return -105;
    }
    free(ca2);

    // --- Multiple simultaneous live allocations ----------------------------
    // Allocate N blocks, stamp each with its index, free every other one,
    // re-malloc the same size, re-stamp, verify all are correct.
#define NBLOCKS 12
    char* ptrs[NBLOCKS];
    for (int i = 0; i < NBLOCKS; i++) {
        ptrs[i] = (char*)malloc(64);
        if (!ptrs[i]) return -106;
        for (int j = 0; j < 64; j++) ptrs[i][j] = (char)(i + 1);
    }
    // Free even-indexed blocks.
    for (int i = 0; i < NBLOCKS; i += 2) {
        free(ptrs[i]);
        ptrs[i] = (void*)0;
    }
    // Odd-indexed blocks must still hold their stamps.
    for (int i = 1; i < NBLOCKS; i += 2) {
        for (int j = 0; j < 64; j++) {
            if (ptrs[i][j] != (char)(i + 1)) return -107;
        }
    }
    // Reallocate even slots.
    for (int i = 0; i < NBLOCKS; i += 2) {
        ptrs[i] = (char*)malloc(64);
        if (!ptrs[i]) return -108;
        for (int j = 0; j < 64; j++) ptrs[i][j] = (char)(i + 100);
    }
    // Verify all blocks.
    for (int i = 0; i < NBLOCKS; i++) {
        char expected = (i % 2 == 0) ? (char)(i + 100) : (char)(i + 1);
        for (int j = 0; j < 64; j++) {
            if (ptrs[i][j] != expected) return -109;
        }
    }
    for (int i = 0; i < NBLOCKS; i++) free(ptrs[i]);
#undef NBLOCKS

    // --- realloc grow / shrink --------------------------------------------
    char* rp = (char*)malloc(128);
    if (!rp) return -110;
    for (int i = 0; i < 128; i++) rp[i] = (char)(i & 0xFF);
    rp = (char*)realloc(rp, 256);
    if (!rp) return -111;
    for (int i = 0; i < 128; i++) {
        if ((unsigned char)rp[i] != (unsigned char)(i & 0xFF)) return -112;
    }
    // Shrink: realloc to smaller size — old content in the surviving range
    // must be unchanged (implementation may return same pointer).
    rp = (char*)realloc(rp, 64);
    if (!rp) return -113;
    for (int i = 0; i < 64; i++) {
        if ((unsigned char)rp[i] != (unsigned char)(i & 0xFF)) return -114;
    }
    free(rp);

    // --- large allocation (stress mmap overflow tier) ---------------------
    // 10 MB total exceeds the 8 MB static arena; if SYS_MMAP works the
    // overflow tier serves it.  If mmap is unavailable the allocation will
    // return NULL — we accept that gracefully (don't fail the selftest).
#define BIG_SZ (10UL * 1024 * 1024)
    char* big = (char*)malloc(BIG_SZ);
    if (big) {
        big[0]          = 0x55;
        big[BIG_SZ - 1] = 0xAA;
        if ((unsigned char)big[0]          != 0x55) return -115;
        if ((unsigned char)big[BIG_SZ - 1] != 0xAA) return -116;
        free(big);
    }
#undef BIG_SZ

    // --- qsort ------------------------------------------------------------
    int arr[8] = { 5, 3, 8, 1, 9, 2, 7, 4 };
    qsort(arr, 8, sizeof(int), cmp_int);
    for (int i = 0; i < 7; i++) {
        if (arr[i] > arr[i + 1]) return -11;
    }
    if (arr[0] != 1 || arr[7] != 9) return -12;

    // bsearch over the now-sorted array.
    int key = 7;
    int* found = (int*)bsearch(&key, arr, 8, sizeof(int), cmp_int);
    if (!found || *found != 7) return -13;

    // qsort with a single-element and empty array (edge cases).
    int one = 42;
    qsort(&one, 1, sizeof(int), cmp_int);
    if (one != 42) return -120;
    qsort((void*)0, 0, sizeof(int), cmp_int);  // must not crash

    // --- strtol / strtoul -------------------------------------------------
    char* end;
    long v = strtol("  -1234xyz", &end, 10);
    if (v != -1234 || *end != 'x') return -14;

    long h2 = strtol("0x1F", (void*)0, 16);
    if (h2 != 31) return -15;

    unsigned long u = strtoul("4294967295", (void*)0, 10);
    if (u != 4294967295UL) return -16;

    // atoi / atol
    if (atoi("  -99abc") != -99)  return -121;
    if (atoi("+0")        !=   0) return -122;
    if (atol("2147483648") != 2147483648L) return -123;

    // --- strtod -----------------------------------------------------------
    // Basic positive and negative.
    double d1 = strtod("3.14", (void*)0);
    if (!dbl_near(d1, 3.14, 1e-6)) return -130;

    double d2 = strtod("-2.718", (void*)0);
    if (!dbl_near(d2, -2.718, 1e-6)) return -131;

    // Scientific notation.
    double d3 = strtod("1.5e3", (void*)0);
    if (!dbl_near(d3, 1500.0, 1e-9)) return -132;

    double d4 = strtod("6.022e-1", (void*)0);
    if (!dbl_near(d4, 0.6022, 1e-6)) return -133;

    // endptr stops at non-numeric character.
    char* ep;
    double d5 = strtod("42.0abc", &ep);
    if (!dbl_near(d5, 42.0, 1e-9) || *ep != 'a') return -134;

    // Zero.
    double d6 = strtod("0.0", (void*)0);
    if (!dbl_near(d6, 0.0, 1e-15)) return -135;

    // --- strstr / string utilities ----------------------------------------
    const char* hay = "the quick brown fox";
    char* hit = strstr(hay, "brown");
    if (!hit || hit != hay + 10) return -17;
    if (strstr(hay, "zzz") != NULL) return -18;

    // strtok tokenisation.
    char tok[] = "a,bb,,ccc";
    char* save;
    char* t1 = strtok_r(tok, ",", &save);
    char* t2 = strtok_r(NULL, ",", &save);
    char* t3 = strtok_r(NULL, ",", &save);
    if (!t1 || strcmp(t1, "a") != 0) return -19;
    if (!t2 || strcmp(t2, "bb") != 0) return -20;
    if (!t3 || strcmp(t3, "ccc") != 0) return -21;

    // ctype + memchr quick checks.
    if (!isdigit('7') || isdigit('x') || toupper('a') != 'A') return -22;
    if (memchr("hello", 'l', 5) == NULL) return -23;

    // --- errno / strerror -------------------------------------------------
    errno = ENOENT;
    if (errno != ENOENT) return -24;
    if (strcmp(strerror(ENOENT), "No such file or directory") != 0) return -25;

    // --- abs / labs -------------------------------------------------------
    if (abs(-7)     !=  7)  return -140;
    if (abs(7)      !=  7)  return -141;
    if (labs(-1000L) != 1000L) return -142;

    // --- rand / srand -----------------------------------------------------
    // srand then two successive rand() values must differ (LCG property).
    srand(12345);
    int r1 = rand();
    int r2 = rand();
    if (r1 == r2) return -150;  // astronomically unlikely for any decent PRNG
    if (r1 < 0 || r1 > 32767) return -151;
    if (r2 < 0 || r2 > 32767) return -152;
    // Seeding again with the same seed must reproduce the same sequence.
    srand(12345);
    if (rand() != r1) return -153;

    return 0;  // all checks passed
}
