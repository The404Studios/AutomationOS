// userspace/libc/malloc.h - Heap allocator API
//
// Freestanding ring-3 heap allocator.  No standard headers.
// The implementation lives in malloc.c (included by stdlib.c).
// Primary arena: static 8 MB BSS.  Overflow arenas: grown on demand
// via SYS_MMAP=37 in 2 MB chunks (falls back to OOM if SYS_MMAP fails).

#ifndef MALLOC_H
#define MALLOC_H

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef unsigned long size_t;

void* malloc(size_t size);
void  free(void* ptr);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);

// --------------------------------------------------------------------------
// TCACHE statistics (optional, only available with -DMALLOC_STATS)
// --------------------------------------------------------------------------
#ifdef MALLOC_STATS
// Get current tcache statistics. Useful for benchmarking and verification.
void malloc_tcache_stats(unsigned long* hits, unsigned long* misses,
                         unsigned long* cached_frees, unsigned long* bypassed_frees);

// Reset tcache statistics
void malloc_tcache_reset_stats(void);
#endif

// libc integration test: returns 0 on pass, negative on first failure.
// Exercises malloc/free/calloc/realloc, atoi, strtod, qsort.
int libc_selftest(void);

#endif /* MALLOC_H */
