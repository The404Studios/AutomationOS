/* fsmem.c -- weak freestanding memcpy/memset/memmove/memcmp for userspace ELFs.
 *
 * GCC's freestanding contract: the environment must provide these four. Newer
 * GCC (16.x) lowers aggregate copies/initialisers -- and loop idioms -- into
 * calls to them even under -ffreestanding -fno-builtin, and many apps here link
 * with crt0 only (no libc), so the link failed with "undefined reference to
 * memcpy/memset".
 *
 * Every definition is WEAK: an app or library that ships its own strong copy
 * (clib.c, libc/string.c, per-app helpers) wins silently, so linking this into
 * every ELF can never produce a duplicate-symbol error.
 *
 * rep movsb/stosb (not C loops) so GCC cannot lower the body back into a call
 * to the very function being defined. The one C loop (memmove backward) and
 * memcmp are protected by the pragma below. Deliberately no `std` for the
 * overlapping case: an interrupt taken with DF=1 would corrupt any string op
 * the kernel path executes before it re-establishes DF=0.
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")

typedef unsigned long size_t;

__attribute__((weak)) void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}

__attribute__((weak)) void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

__attribute__((weak)) void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) {          /* no destructive overlap: forward */
        void *r = d;
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
        return r;
    }
    while (n--) dp[n] = sp[n];              /* overlapping, dst above src: backward */
    return d;
}

__attribute__((weak)) int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
