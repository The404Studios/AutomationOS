/**
 * @file cache.c
 * @brief AutoFS Caching Layer
 *
 * Implements multiple caching strategies:
 * - Inode cache: Cache recently used inodes
 * - Dentry cache: Cache directory entries
 * - Page cache: Cache file data blocks
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CACHE_SIZE 1024  /* Number of cache entries */

/* Simple cache entry */
typedef struct cache_entry {
    uint64_t key;
    void *data;
    size_t size;
    time_t access_time;
    bool dirty;
} cache_entry_t;

/**
 * @brief Initialize caches
 *
 * @param fs Filesystem
 * @return 0 on success, -1 on error
 */
int autofs_cache_init(autofs_fs_t *fs) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    /* Allocate inode cache */
    fs->inode_cache = calloc(CACHE_SIZE, sizeof(cache_entry_t));
    if (!fs->inode_cache) {
        return -1;
    }

    /* Allocate dentry cache */
    fs->dentry_cache = calloc(CACHE_SIZE, sizeof(cache_entry_t));
    if (!fs->dentry_cache) {
        free(fs->inode_cache);
        return -1;
    }

    /* Allocate page cache */
    fs->page_cache = calloc(CACHE_SIZE, sizeof(cache_entry_t));
    if (!fs->page_cache) {
        free(fs->inode_cache);
        free(fs->dentry_cache);
        return -1;
    }

    printf("AutoFS Cache: Initialized (%d entries each)\n", CACHE_SIZE);

    return 0;
}

/**
 * @brief Destroy caches
 *
 * @param fs Filesystem
 */
void autofs_cache_destroy(autofs_fs_t *fs) {
    if (!fs) {
        return;
    }

    /* Free inode cache */
    if (fs->inode_cache) {
        cache_entry_t *entries = fs->inode_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data) {
                free(entries[i].data);
            }
        }
        free(fs->inode_cache);
    }

    /* Free dentry cache */
    if (fs->dentry_cache) {
        cache_entry_t *entries = fs->dentry_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data) {
                free(entries[i].data);
            }
        }
        free(fs->dentry_cache);
    }

    /* Free page cache */
    if (fs->page_cache) {
        cache_entry_t *entries = fs->page_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data) {
                free(entries[i].data);
            }
        }
        free(fs->page_cache);
    }
}

/**
 * @brief Lookup entry in cache
 *
 * @param cache Cache
 * @param key Key to lookup
 * @return Data pointer or NULL if not found
 */
static void* cache_lookup(void *cache, uint64_t key) {
    cache_entry_t *entries = cache;

    for (int i = 0; i < CACHE_SIZE; i++) {
        if (entries[i].data && entries[i].key == key) {
            entries[i].access_time = time(NULL);
            return entries[i].data;
        }
    }

    return NULL;
}

/**
 * @brief Insert entry into cache
 *
 * @param cache Cache
 * @param key Key
 * @param data Data to cache
 * @param size Data size
 * @return 0 on success, -1 on error
 */
static int cache_insert(void *cache, uint64_t key, const void *data, size_t size) {
    cache_entry_t *entries = cache;

    /* Find empty slot or oldest entry */
    int victim = 0;
    time_t oldest_time = time(NULL);

    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!entries[i].data) {
            victim = i;
            break;
        }

        if (entries[i].access_time < oldest_time) {
            oldest_time = entries[i].access_time;
            victim = i;
        }
    }

    /* Free old data */
    if (entries[victim].data) {
        free(entries[victim].data);
    }

    /* Insert new data */
    entries[victim].key = key;
    entries[victim].data = malloc(size);
    if (!entries[victim].data) {
        return -1;
    }

    memcpy(entries[victim].data, data, size);
    entries[victim].size = size;
    entries[victim].access_time = time(NULL);
    entries[victim].dirty = false;

    return 0;
}

/**
 * @brief Invalidate cache entry
 *
 * @param fs Filesystem
 * @param ino Inode number to invalidate
 */
void autofs_cache_invalidate(autofs_fs_t *fs, uint64_t ino) {
    if (!fs) {
        return;
    }

    /* Invalidate inode cache */
    if (fs->inode_cache) {
        cache_entry_t *entries = fs->inode_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data && entries[i].key == ino) {
                free(entries[i].data);
                entries[i].data = NULL;
                break;
            }
        }
    }

    /* Invalidate page cache for this inode */
    if (fs->page_cache) {
        cache_entry_t *entries = fs->page_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data && (entries[i].key >> 32) == ino) {
                free(entries[i].data);
                entries[i].data = NULL;
            }
        }
    }
}

/**
 * @brief Flush all dirty cache entries
 *
 * @param fs Filesystem
 */
void autofs_cache_flush(autofs_fs_t *fs) {
    if (!fs || fs->read_only) {
        return;
    }

    int flushed = 0;

    /* Flush inode cache */
    if (fs->inode_cache) {
        cache_entry_t *entries = fs->inode_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data && entries[i].dirty) {
                /* Write back to disk */
                autofs_inode_t *inode = entries[i].data;
                autofs_put_inode(fs, inode);
                entries[i].dirty = false;
                flushed++;
            }
        }
    }

    /* Flush page cache */
    if (fs->page_cache) {
        cache_entry_t *entries = fs->page_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data && entries[i].dirty) {
                /* Write back to disk */
                uint64_t block = entries[i].key & 0xFFFFFFFF;
                autofs_write_block(fs, block, entries[i].data);
                entries[i].dirty = false;
                flushed++;
            }
        }
    }

    if (flushed > 0) {
        printf("AutoFS Cache: Flushed %d dirty entries\n", flushed);
    }
}

/**
 * @brief Get inode from cache or disk
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @return Cached inode or NULL
 */
autofs_inode_t* autofs_cache_get_inode(autofs_fs_t *fs, uint64_t ino) {
    if (!fs || !fs->inode_cache) {
        return NULL;
    }

    /* Check cache first */
    autofs_inode_t *cached = cache_lookup(fs->inode_cache, ino);
    if (cached) {
        fs->stats.cache_hits++;
        return cached;
    }

    fs->stats.cache_misses++;

    /* Not in cache - load from disk */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (inode) {
        /* Insert into cache */
        cache_insert(fs->inode_cache, ino, inode, sizeof(autofs_inode_t));
    }

    return inode;
}

/**
 * @brief Get block from cache or disk
 *
 * @param fs Filesystem
 * @param block Block number
 * @param buf Buffer to read into
 * @return 0 on success, -1 on error
 */
int autofs_cache_read_block(autofs_fs_t *fs, uint64_t block, void *buf) {
    if (!fs || !fs->page_cache || !buf) {
        errno = EINVAL;
        return -1;
    }

    /* Check cache first */
    void *cached = cache_lookup(fs->page_cache, block);
    if (cached) {
        fs->stats.cache_hits++;
        memcpy(buf, cached, AUTOFS_BLOCK_SIZE);
        return 0;
    }

    fs->stats.cache_misses++;

    /* Not in cache - read from disk */
    if (autofs_read_block(fs, block, buf) < 0) {
        return -1;
    }

    /* Insert into cache */
    cache_insert(fs->page_cache, block, buf, AUTOFS_BLOCK_SIZE);

    return 0;
}

/**
 * @brief Write block through cache
 *
 * @param fs Filesystem
 * @param block Block number
 * @param buf Data to write
 * @return 0 on success, -1 on error
 */
int autofs_cache_write_block(autofs_fs_t *fs, uint64_t block, const void *buf) {
    if (!fs || !fs->page_cache || !buf) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Write to disk */
    if (autofs_write_block(fs, block, buf) < 0) {
        return -1;
    }

    /* Update cache */
    cache_insert(fs->page_cache, block, buf, AUTOFS_BLOCK_SIZE);

    return 0;
}

/**
 * @brief Print cache statistics
 *
 * @param fs Filesystem
 */
void autofs_cache_stats(autofs_fs_t *fs) {
    if (!fs) {
        return;
    }

    printf("\n=== AutoFS Cache Statistics ===\n");
    printf("Hits: %lu\n", fs->stats.cache_hits);
    printf("Misses: %lu\n", fs->stats.cache_misses);

    if (fs->stats.cache_hits + fs->stats.cache_misses > 0) {
        uint64_t total_ops = fs->stats.cache_hits + fs->stats.cache_misses;
        uint64_t hit_pct = 100 * fs->stats.cache_hits / total_ops;
        uint64_t hit_frac = (1000 * fs->stats.cache_hits / total_ops) % 10;
        printf("Hit rate: %lu.%lu%%\n", hit_pct, hit_frac);
    }

    /* Count active entries */
    int inode_entries = 0;
    int dentry_entries = 0;
    int page_entries = 0;

    if (fs->inode_cache) {
        cache_entry_t *entries = fs->inode_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data) inode_entries++;
        }
    }

    if (fs->dentry_cache) {
        cache_entry_t *entries = fs->dentry_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data) dentry_entries++;
        }
    }

    if (fs->page_cache) {
        cache_entry_t *entries = fs->page_cache;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (entries[i].data) page_entries++;
        }
    }

    printf("Inode cache: %d/%d entries\n", inode_entries, CACHE_SIZE);
    printf("Dentry cache: %d/%d entries\n", dentry_entries, CACHE_SIZE);
    printf("Page cache: %d/%d entries\n", page_entries, CACHE_SIZE);
    printf("===============================\n\n");
}
