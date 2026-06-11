/**
 * Unified Page Cache for AutomationOS VFS
 *
 * Provides 10-100x faster file I/O via caching (Linux page cache pattern)
 */

#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include "types.h"
#include "vfs.h"

/* Page cache configuration */
#define PAGE_CACHE_SIZE 4096           /* Page size (4KB) */
#define PAGE_CACHE_HASH_SIZE 1024      /* Hash table size (power of 2) */
#define PAGE_CACHE_MAX_PAGES 2048      /* Max cached pages (8MB total) */

/* Page cache entry flags */
#define PCE_DIRTY       0x01           /* Page has been modified */
#define PCE_LOCKED      0x02           /* Page is being read/written */
#define PCE_VALID       0x04           /* Page contains valid data */
#define PCE_READAHEAD   0x08           /* Page was prefetched (lower eviction priority) */
#define PCE_ACCESSED    0x10           /* Page was accessed after prefetch */

/**
 * Page cache entry - maps (inode, offset) to cached page
 */
typedef struct page_cache_entry {
    vfs_inode_t* inode;                /* File inode */
    uint64_t offset;                   /* Page-aligned offset in file */
    void* page_data;                   /* Cached page data (4KB) */
    uint32_t flags;                    /* PCE_* flags */

    /* LRU tracking */
    struct page_cache_entry* lru_prev; /* Previous in LRU list */
    struct page_cache_entry* lru_next; /* Next in LRU list */

    /* Hash chain */
    struct page_cache_entry* hash_next; /* Next in hash bucket */

    uint64_t access_time;              /* Last access timestamp (for LRU) */
    uint32_t ref_count;                /* Reference count */
} page_cache_entry_t;

/**
 * Page cache statistics
 */
typedef struct {
    uint64_t hits;                     /* Cache hits */
    uint64_t misses;                   /* Cache misses */
    uint64_t evictions;                /* Pages evicted */
    uint64_t dirty_pages;              /* Currently dirty pages */
    uint64_t total_pages;              /* Total cached pages */
    uint64_t max_pages;                /* Maximum pages allowed */
    uint64_t readahead_pages;          /* Pages prefetched by read-ahead */
    uint64_t readahead_hits;           /* Prefetched pages that were accessed */
    uint64_t readahead_misses;         /* Prefetched pages evicted before use */
} page_cache_stats_t;

/* Initialize page cache subsystem */
void page_cache_init(void);

/* Lookup page in cache - returns cached entry or NULL */
page_cache_entry_t* page_cache_lookup(vfs_inode_t* inode, uint64_t offset);

/* Read page from cache or disk */
ssize_t page_cache_read(vfs_file_t* file, void* buf, size_t count);

/* Write page to cache (mark dirty, flush later) */
ssize_t page_cache_write(vfs_file_t* file, const void* buf, size_t count);

/* Flush dirty pages for an inode */
int page_cache_flush_inode(vfs_inode_t* inode);

/* Flush all dirty pages */
int page_cache_flush_all(void);

/* Evict pages for an inode (on file close/delete) */
void page_cache_evict_inode(vfs_inode_t* inode);

/* Get cache statistics */
void page_cache_get_stats(page_cache_stats_t* stats);

/* Print cache statistics */
void page_cache_print_stats(void);

/* Prefetch pages for read-ahead - internal use */
int page_cache_prefetch(vfs_inode_t* inode, uint64_t offset, size_t count);

/**
 * page_cache_readahead - Public API for sequential read-ahead.
 *
 * Prefetches `window` pages starting at `offset` into the cache for the given
 * inode.  Designed to be called by VFS/FS read paths when sequential access is
 * detected.  Skips pages that are already cached.  Stops early if the cache is
 * >75% full or the prefetch would extend past EOF.
 */
void page_cache_readahead(vfs_inode_t* inode, uint64_t offset, uint64_t window);

#endif // PAGE_CACHE_H
