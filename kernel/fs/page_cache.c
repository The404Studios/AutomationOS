/**
 * Unified Page Cache Implementation
 *
 * Implements a Linux-style page cache with:
 * - Hash table mapping (inode, offset) → page
 * - LRU eviction policy
 * - Write-back caching (dirty pages flushed lazily)
 * - 10-100x speedup for repeated reads
 */

#include "../include/page_cache.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/string.h"
#include "../include/vfs.h"

/* Global page cache state */
static struct {
    page_cache_entry_t* hash_table[PAGE_CACHE_HASH_SIZE];
    page_cache_entry_t* lru_head;      /* Most recently used */
    page_cache_entry_t* lru_tail;      /* Least recently used */
    page_cache_entry_t* clock_hand;    /* CLOCK eviction hand position */
    page_cache_stats_t stats;
    uint32_t initialized;
    uint64_t timestamp;                /* Monotonic counter for LRU */
} cache_state;

/* Hash function for (inode, offset) */
static inline uint32_t cache_hash(vfs_inode_t* inode, uint64_t offset) {
    uint64_t hash = ((uint64_t)inode ^ offset) >> 12; /* offset is page-aligned */
    return (uint32_t)(hash % PAGE_CACHE_HASH_SIZE);
}

/* Round offset down to page boundary */
static inline uint64_t page_align(uint64_t offset) {
    return offset & ~(PAGE_CACHE_SIZE - 1);
}

/* Allocate a new cache entry */
static page_cache_entry_t* cache_entry_alloc(void) {
    page_cache_entry_t* entry = (page_cache_entry_t*)kmalloc(sizeof(page_cache_entry_t));
    if (!entry) {
        return NULL;
    }

    /* Zero the struct FIRST, then allocate page_data. The previous order
     * (alloc page_data, then memset the whole struct) clobbered page_data back
     * to NULL and leaked the 4KB buffer, so every entry came back with
     * page_data == NULL -> NULL kernel write in cache_read_page/page_cache_write. */
    memset(entry, 0, sizeof(page_cache_entry_t));
    entry->page_data = kmalloc(PAGE_CACHE_SIZE);
    if (!entry->page_data) {
        kfree(entry);
        return NULL;
    }
    entry->ref_count = 1;
    return entry;
}

/* Free a cache entry */
static void cache_entry_free(page_cache_entry_t* entry) {
    if (!entry) return;
    if (entry->page_data) {
        kfree(entry->page_data);
    }
    kfree(entry);
}

/* Remove entry from LRU list */
static void lru_remove(page_cache_entry_t* entry) {
    if (entry->lru_prev) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else {
        cache_state.lru_head = entry->lru_next;
    }

    if (entry->lru_next) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else {
        cache_state.lru_tail = entry->lru_prev;
    }

    entry->lru_prev = NULL;
    entry->lru_next = NULL;
}

/* Add entry to head of LRU list (most recently used) */
static void lru_add_head(page_cache_entry_t* entry) {
    entry->lru_next = cache_state.lru_head;
    entry->lru_prev = NULL;

    if (cache_state.lru_head) {
        cache_state.lru_head->lru_prev = entry;
    } else {
        cache_state.lru_tail = entry;
    }

    cache_state.lru_head = entry;
}

/* Move entry to head of LRU list (mark as recently used) */
static void lru_touch(page_cache_entry_t* entry) {
    if (entry == cache_state.lru_head) {
        return; /* Already at head */
    }
    lru_remove(entry);
    lru_add_head(entry);
    entry->access_time = cache_state.timestamp++;
}

/* Flush a dirty page to disk (write through to inode->data) */
static int cache_flush_entry(page_cache_entry_t* entry) {
    if (!entry || !(entry->flags & PCE_DIRTY)) {
        return 0; /* Nothing to flush */
    }

    vfs_inode_t* inode = entry->inode;
    if (!inode || !inode->data) {
        return -1;
    }

    /* Calculate write position and size */
    uint64_t file_offset = entry->offset;
    size_t write_size = PAGE_CACHE_SIZE;

    /* Don't write beyond end of file */
    if (file_offset >= inode->size) {
        return 0; /* Page is beyond EOF */
    }
    if (file_offset + write_size > inode->size) {
        write_size = inode->size - file_offset;
    }

    /* Write to inode's data buffer */
    if (inode->data_capacity < file_offset + write_size) {
        /* Need to grow inode data buffer */
        size_t new_capacity = file_offset + write_size;
        if (new_capacity < inode->data_capacity * 2) {
            new_capacity = inode->data_capacity * 2;
        }

        void* new_data = kmalloc(new_capacity);
        if (!new_data) {
            return -1;
        }

        if (inode->data) {
            memcpy(new_data, inode->data, inode->data_capacity);
            if (inode->flags & VFS_DATA_OWNED) {
                kfree(inode->data);
            }
        }

        inode->data = new_data;
        inode->data_capacity = new_capacity;
        inode->flags |= VFS_DATA_OWNED;
        inode->flags &= ~VFS_DATA_INITRD_BACKED;
    }

    /* Write cached page to inode data */
    memcpy((uint8_t*)inode->data + file_offset, entry->page_data, write_size);

    /* Clear dirty flag */
    entry->flags &= ~PCE_DIRTY;
    if (cache_state.stats.dirty_pages > 0) {
        cache_state.stats.dirty_pages--;
    }

    return 0;
}

/**
 * CLOCK eviction algorithm (replaces linear LRU scan).
 *
 * The CLOCK hand sweeps through the circular LRU list.  Each page has a
 * "referenced" bit (PCE_ACCESSED).  When the hand visits a page:
 *   - If ACCESSED is clear: evict this page (it was not touched recently).
 *   - If ACCESSED is set:   clear it and advance (give it a second chance).
 *
 * Unused read-ahead pages (PCE_READAHEAD && !PCE_ACCESSED) are still preferred
 * as victims on a fast first pass, since they represent wasted prefetch.
 *
 * This avoids the O(N) linear scan of pure LRU eviction.
 */
static int cache_evict_lru(void) {
    page_cache_entry_t* victim = NULL;

    /* Fast pass: find an unused read-ahead page anywhere in the list. These are
     * wasted prefetches and should be reclaimed before real pages. */
    page_cache_entry_t* curr = cache_state.lru_tail;
    while (curr) {
        if ((curr->flags & PCE_READAHEAD) && !(curr->flags & PCE_ACCESSED)) {
            victim = curr;
            cache_state.stats.readahead_misses++;
            break;
        }
        curr = curr->lru_prev;
    }

    /* CLOCK sweep: if no dead read-ahead page, use the clock hand. */
    if (!victim) {
        if (!cache_state.clock_hand) {
            cache_state.clock_hand = cache_state.lru_tail;
        }

        /* Sweep at most 2*total_pages entries (two full rotations max) to
         * guarantee termination even when every page is accessed. */
        uint64_t budget = cache_state.stats.total_pages * 2;
        while (cache_state.clock_hand && budget > 0) {
            page_cache_entry_t* hand = cache_state.clock_hand;
            budget--;

            if (hand->flags & PCE_ACCESSED) {
                /* Second chance: clear the bit and advance. */
                hand->flags &= ~(uint32_t)PCE_ACCESSED;
                cache_state.clock_hand = hand->lru_prev ? hand->lru_prev : cache_state.lru_head;
            } else {
                /* Victim found. */
                victim = hand;
                /* Advance the hand past the victim before removing it. */
                cache_state.clock_hand = hand->lru_prev ? hand->lru_prev : cache_state.lru_head;
                if (cache_state.clock_hand == victim) {
                    cache_state.clock_hand = NULL; /* last entry */
                }
                break;
            }
        }
    }

    /* Ultimate fallback: if CLOCK couldn't find a victim (all pages accessed
     * and budget exhausted), just take the LRU tail. */
    if (!victim) {
        victim = cache_state.lru_tail;
    }

    if (!victim) {
        return -1; /* Cache is empty */
    }

    /* Flush if dirty */
    if (victim->flags & PCE_DIRTY) {
        cache_flush_entry(victim);
    }

    /* Remove from hash table */
    uint32_t hash = cache_hash(victim->inode, victim->offset);
    page_cache_entry_t** slot = &cache_state.hash_table[hash];
    while (*slot && *slot != victim) {
        slot = &(*slot)->hash_next;
    }
    if (*slot) {
        *slot = victim->hash_next;
    }

    /* Remove from LRU */
    lru_remove(victim);

    /* Free entry */
    cache_entry_free(victim);

    cache_state.stats.evictions++;
    cache_state.stats.total_pages--;

    return 0;
}

/* Initialize page cache */
void page_cache_init(void) {
    kprintf("[PageCache] Initializing unified page cache...\n");

    memset(&cache_state, 0, sizeof(cache_state));

    for (int i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        cache_state.hash_table[i] = NULL;
    }

    cache_state.lru_head = NULL;
    cache_state.lru_tail = NULL;
    cache_state.clock_hand = NULL;
    cache_state.stats.max_pages = PAGE_CACHE_MAX_PAGES;
    cache_state.timestamp = 0;
    cache_state.initialized = 1;

    kprintf("[PageCache] Page cache initialized (max %d pages = %d KB)\n",
            PAGE_CACHE_MAX_PAGES, (PAGE_CACHE_MAX_PAGES * PAGE_CACHE_SIZE) / 1024);
}

/* Lookup page in cache */
page_cache_entry_t* page_cache_lookup(vfs_inode_t* inode, uint64_t offset) {
    if (!cache_state.initialized || !inode) {
        return NULL;
    }

    offset = page_align(offset);
    uint32_t hash = cache_hash(inode, offset);

    /* Search hash chain */
    page_cache_entry_t* entry = cache_state.hash_table[hash];
    while (entry) {
        if (entry->inode == inode && entry->offset == offset) {
            /* Cache hit! */
            cache_state.stats.hits++;

            /* Track read-ahead effectiveness */
            if ((entry->flags & PCE_READAHEAD) && !(entry->flags & PCE_ACCESSED)) {
                /* First access to a prefetched page - good prediction! */
                cache_state.stats.readahead_hits++;
                entry->flags |= PCE_ACCESSED;
            }

            lru_touch(entry);
            return entry;
        }
        entry = entry->hash_next;
    }

    /* Cache miss */
    cache_state.stats.misses++;
    return NULL;
}

/* Read page from disk into cache */
static page_cache_entry_t* cache_read_page(vfs_inode_t* inode, uint64_t offset) {
    offset = page_align(offset);

    /* Check if we need to evict */
    if (cache_state.stats.total_pages >= PAGE_CACHE_MAX_PAGES) {
        cache_evict_lru();
    }

    /* Allocate new entry */
    page_cache_entry_t* entry = cache_entry_alloc();
    if (!entry) {
        return NULL;
    }

    entry->inode = inode;
    entry->offset = offset;
    entry->flags = PCE_VALID;
    entry->access_time = cache_state.timestamp++;

    /* Read data from inode */
    if (inode->data && offset < inode->size) {
        size_t read_size = PAGE_CACHE_SIZE;
        if (offset + read_size > inode->size) {
            read_size = inode->size - offset;
        }
        memcpy(entry->page_data, (uint8_t*)inode->data + offset, read_size);

        /* Zero-fill remainder */
        if (read_size < PAGE_CACHE_SIZE) {
            memset((uint8_t*)entry->page_data + read_size, 0,
                   PAGE_CACHE_SIZE - read_size);
        }
    } else {
        /* Beyond EOF or no data - zero fill */
        memset(entry->page_data, 0, PAGE_CACHE_SIZE);
    }

    /* Insert into hash table */
    uint32_t hash = cache_hash(inode, offset);
    entry->hash_next = cache_state.hash_table[hash];
    cache_state.hash_table[hash] = entry;

    /* Add to LRU */
    lru_add_head(entry);

    cache_state.stats.total_pages++;

    return entry;
}

/**
 * Read-ahead: Prefetch upcoming pages for sequential I/O
 *
 * When sequential access is detected, prefetch the next N pages into the cache
 * to hide I/O latency. This is done synchronously for simplicity (async would
 * require worker threads).
 */
static void cache_readahead(vfs_inode_t* inode, uint64_t offset, uint64_t window) {
    if (!cache_state.initialized || !inode || window == 0) {
        return;
    }

    /* Prefetch up to 'window' pages ahead */
    for (uint64_t i = 1; i <= window; i++) {
        uint64_t prefetch_offset = page_align(offset) + (i * PAGE_CACHE_SIZE);

        /* Don't prefetch beyond EOF */
        if (prefetch_offset >= inode->size) {
            break;
        }

        /* Skip if already cached (use direct lookup without stats update) */
        uint32_t hash = cache_hash(inode, prefetch_offset);
        page_cache_entry_t* existing = cache_state.hash_table[hash];
        int already_cached = 0;
        while (existing) {
            if (existing->inode == inode && existing->offset == prefetch_offset) {
                already_cached = 1;
                break;
            }
            existing = existing->hash_next;
        }
        if (already_cached) {
            continue;
        }

        /* Prefetch page and mark as read-ahead */
        page_cache_entry_t* entry = cache_read_page(inode, prefetch_offset);
        if (entry) {
            entry->flags |= PCE_READAHEAD; /* Mark as prefetched */
            cache_state.stats.readahead_pages++;
        }

        /* Throttle: stop if cache is getting full (>75% capacity) */
        if (cache_state.stats.total_pages > (PAGE_CACHE_MAX_PAGES * 3) / 4) {
            break;
        }
    }
}

/* Read from cache or disk */
ssize_t page_cache_read(vfs_file_t* file, void* buf, size_t count) {
    if (!cache_state.initialized || !file || !file->inode || !buf) {
        return -1;
    }

    vfs_inode_t* inode = file->inode;
    uint64_t offset = file->offset;
    size_t total_read = 0;

    /* Check bounds */
    if (offset >= inode->size) {
        return 0; /* EOF */
    }

    uint64_t available = inode->size - offset;
    if (count > available) {
        count = available;
    }

    /* Detect sequential access pattern and trigger read-ahead */
    int is_sequential = 0;
    if (file->ra_last_offset > 0 && offset == file->ra_last_offset) {
        /* Sequential read detected */
        file->ra_sequential++;
        is_sequential = 1;
    } else {
        /* Non-sequential, reset counter and shrink window */
        file->ra_sequential = 0;
        if (file->ra_window > VFS_READAHEAD_PAGES) {
            file->ra_window = VFS_READAHEAD_PAGES; /* Reset to default */
        }
    }

    /* Trigger read-ahead if we've seen enough sequential reads */
    if (is_sequential && file->ra_sequential >= VFS_READAHEAD_THRESHOLD) {
        /* Adaptive window: grow on sustained sequential access */
        /* Pattern: 4 → 8 → 16 → 32 pages (max 128KB) */
        if (file->ra_sequential > 10 && file->ra_window < 32) {
            file->ra_window *= 2; /* Grow aggressively for long sequences */
        }

        cache_readahead(inode, offset + count, file->ra_window);
    }

    /* Read page by page */
    while (count > 0) {
        uint64_t page_offset = page_align(offset);
        uint64_t offset_in_page = offset - page_offset;
        size_t bytes_in_page = PAGE_CACHE_SIZE - offset_in_page;
        if (bytes_in_page > count) {
            bytes_in_page = count;
        }

        /* Lookup or read page */
        page_cache_entry_t* entry = page_cache_lookup(inode, page_offset);
        if (!entry) {
            entry = cache_read_page(inode, page_offset);
            if (!entry) {
                return total_read > 0 ? (ssize_t)total_read : -1;
            }
        }

        /* Copy data from cached page */
        memcpy((uint8_t*)buf + total_read,
               (uint8_t*)entry->page_data + offset_in_page,
               bytes_in_page);

        total_read += bytes_in_page;
        offset += bytes_in_page;
        count -= bytes_in_page;
    }

    file->offset = offset;
    /* Track last read position for sequential detection */
    file->ra_last_offset = offset;

    return (ssize_t)total_read;
}

/* Write to cache (mark dirty, flush later) */
ssize_t page_cache_write(vfs_file_t* file, const void* buf, size_t count) {
    if (!cache_state.initialized || !file || !file->inode || !buf) {
        return -1;
    }

    vfs_inode_t* inode = file->inode;
    uint64_t offset = file->offset;
    size_t total_written = 0;

    /* Handle append mode */
    if (file->flags & O_APPEND) {
        offset = inode->size;
        file->offset = offset;
    }

    /* Check for overflow */
    if (offset + count > (256ULL * 1024 * 1024)) {
        return -1; /* Would exceed max file size */
    }

    /* Write page by page */
    while (count > 0) {
        uint64_t page_offset = page_align(offset);
        uint64_t offset_in_page = offset - page_offset;
        size_t bytes_in_page = PAGE_CACHE_SIZE - offset_in_page;
        if (bytes_in_page > count) {
            bytes_in_page = count;
        }

        /* Lookup or allocate page */
        page_cache_entry_t* entry = page_cache_lookup(inode, page_offset);
        if (!entry) {
            /* For writes, we need to read the page first (partial page write) */
            if (offset_in_page != 0 || bytes_in_page < PAGE_CACHE_SIZE) {
                entry = cache_read_page(inode, page_offset);
            } else {
                /* Full page write - can skip read */
                if (cache_state.stats.total_pages >= PAGE_CACHE_MAX_PAGES) {
                    cache_evict_lru();
                }
                entry = cache_entry_alloc();
                if (entry) {
                    entry->inode = inode;
                    entry->offset = page_offset;
                    entry->flags = PCE_VALID;
                    entry->access_time = cache_state.timestamp++;

                    uint32_t hash = cache_hash(inode, page_offset);
                    entry->hash_next = cache_state.hash_table[hash];
                    cache_state.hash_table[hash] = entry;
                    lru_add_head(entry);
                    cache_state.stats.total_pages++;
                }
            }

            if (!entry) {
                return total_written > 0 ? (ssize_t)total_written : -1;
            }
        }

        /* Copy data to cached page */
        memcpy((uint8_t*)entry->page_data + offset_in_page,
               (uint8_t*)buf + total_written,
               bytes_in_page);

        /* Mark dirty */
        if (!(entry->flags & PCE_DIRTY)) {
            entry->flags |= PCE_DIRTY;
            cache_state.stats.dirty_pages++;
        }

        total_written += bytes_in_page;
        offset += bytes_in_page;
        count -= bytes_in_page;
    }

    /* Update file size if we extended it */
    if (offset > inode->size) {
        inode->size = offset;
    }

    file->offset = offset;
    return (ssize_t)total_written;
}

/* Flush dirty pages for an inode */
int page_cache_flush_inode(vfs_inode_t* inode) {
    if (!cache_state.initialized || !inode) {
        return -1;
    }

    int flushed = 0;

    /* Scan all hash buckets */
    for (int i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        page_cache_entry_t* entry = cache_state.hash_table[i];
        while (entry) {
            if (entry->inode == inode && (entry->flags & PCE_DIRTY)) {
                cache_flush_entry(entry);
                flushed++;
            }
            entry = entry->hash_next;
        }
    }

    return flushed;
}

/* Flush all dirty pages */
int page_cache_flush_all(void) {
    if (!cache_state.initialized) {
        return -1;
    }

    int flushed = 0;

    for (int i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        page_cache_entry_t* entry = cache_state.hash_table[i];
        while (entry) {
            if (entry->flags & PCE_DIRTY) {
                cache_flush_entry(entry);
                flushed++;
            }
            entry = entry->hash_next;
        }
    }

    return flushed;
}

/* Evict pages for an inode */
void page_cache_evict_inode(vfs_inode_t* inode) {
    if (!cache_state.initialized || !inode) {
        return;
    }

    /* Scan all hash buckets */
    for (int i = 0; i < PAGE_CACHE_HASH_SIZE; i++) {
        page_cache_entry_t** slot = &cache_state.hash_table[i];
        while (*slot) {
            page_cache_entry_t* entry = *slot;
            if (entry->inode == inode) {
                /* Flush if dirty */
                if (entry->flags & PCE_DIRTY) {
                    cache_flush_entry(entry);
                }

                /* Remove from hash chain */
                *slot = entry->hash_next;

                /* Advance clock hand if it points at this entry */
                if (cache_state.clock_hand == entry) {
                    cache_state.clock_hand = entry->lru_prev ? entry->lru_prev : entry->lru_next;
                }

                /* Remove from LRU */
                lru_remove(entry);

                /* Free entry */
                cache_entry_free(entry);
                cache_state.stats.total_pages--;
            } else {
                slot = &entry->hash_next;
            }
        }
    }
}

/* Get cache statistics */
void page_cache_get_stats(page_cache_stats_t* stats) {
    if (stats) {
        *stats = cache_state.stats;
    }
}

/* Print cache statistics */
void page_cache_print_stats(void) {
    if (!cache_state.initialized) {
        kprintf("[PageCache] Not initialized\n");
        return;
    }

    uint64_t total_requests = cache_state.stats.hits + cache_state.stats.misses;
    uint64_t hit_rate = 0;

    if (total_requests > 0) {
        hit_rate = (cache_state.stats.hits * 100) / total_requests;
    }

    kprintf("[PageCache] Statistics:\n");
    kprintf("  Hits:        %llu\n", cache_state.stats.hits);
    kprintf("  Misses:      %llu\n", cache_state.stats.misses);
    kprintf("  Hit Rate:    %llu%%\n", hit_rate);
    kprintf("  Evictions:   %llu\n", cache_state.stats.evictions);
    kprintf("  Total Pages: %llu / %llu\n",
            cache_state.stats.total_pages,
            cache_state.stats.max_pages);
    kprintf("  Dirty Pages: %llu\n", cache_state.stats.dirty_pages);
    kprintf("  Memory Used: %llu KB\n",
            (cache_state.stats.total_pages * PAGE_CACHE_SIZE) / 1024);

    /* Read-ahead effectiveness */
    kprintf("  Read-ahead Prefetched: %llu pages\n", cache_state.stats.readahead_pages);
    kprintf("  Read-ahead Hits:   %llu\n", cache_state.stats.readahead_hits);
    kprintf("  Read-ahead Misses: %llu\n", cache_state.stats.readahead_misses);

    if (cache_state.stats.readahead_pages > 0) {
        uint64_t ra_hit_rate = (cache_state.stats.readahead_hits * 100) /
                               cache_state.stats.readahead_pages;
        kprintf("  Read-ahead Accuracy: %llu%%\n", ra_hit_rate);
    }
}

/**
 * page_cache_readahead - Public API for sequential read-ahead.
 *
 * Wraps the internal cache_readahead so that filesystem drivers and the VFS
 * layer can trigger read-ahead without accessing the static helper directly.
 */
void page_cache_readahead(vfs_inode_t* inode, uint64_t offset, uint64_t window) {
    cache_readahead(inode, offset, window);
}

/**
 * page_cache_prefetch - Public API for sendfile to prefetch file ranges
 *
 * Prefetches a range of pages for upcoming sendfile operation.
 * This allows sendfile to warm the cache before the actual transfer.
 */
int page_cache_prefetch(vfs_inode_t* inode, uint64_t offset, size_t count) {
    if (!cache_state.initialized || !inode || count == 0) {
        return -1;
    }

    /* Calculate number of pages needed */
    uint64_t start_page = page_align(offset);
    uint64_t end_offset = offset + count;
    uint64_t pages_needed = ((end_offset - start_page) + PAGE_CACHE_SIZE - 1) / PAGE_CACHE_SIZE;

    /* Limit prefetch to prevent cache thrashing */
    if (pages_needed > 64) {
        pages_needed = 64; /* Cap at 256KB prefetch */
    }

    /* Prefetch pages */
    uint64_t prefetched = 0;
    for (uint64_t i = 0; i < pages_needed; i++) {
        uint64_t page_offset = start_page + (i * PAGE_CACHE_SIZE);

        /* Don't prefetch beyond EOF */
        if (page_offset >= inode->size) {
            break;
        }

        /* Skip if already cached */
        if (page_cache_lookup(inode, page_offset)) {
            prefetched++;
            continue;
        }

        /* Prefetch page */
        page_cache_entry_t* entry = cache_read_page(inode, page_offset);
        if (entry) {
            entry->flags |= PCE_READAHEAD;
            cache_state.stats.readahead_pages++;
            prefetched++;
        }

        /* Stop if cache is getting too full */
        if (cache_state.stats.total_pages > (PAGE_CACHE_MAX_PAGES * 3) / 4) {
            break;
        }
    }

    return (int)prefetched;
}
