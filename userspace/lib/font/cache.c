/**
 * Font Glyph Cache - LRU Cache Implementation
 *
 * Caches rendered glyphs to avoid re-rasterization.
 * Uses LRU eviction policy with hash table for fast lookup.
 */

#include "font.h"
#include "font_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Cache entry
typedef struct cache_entry {
    font_t* font;              // Font this glyph belongs to
    uint32_t codepoint;        // Unicode codepoint
    font_glyph_t* glyph;       // Rendered glyph
    struct cache_entry* next;  // Hash table chain
    struct cache_entry* lru_prev;  // LRU doubly-linked list
    struct cache_entry* lru_next;
} cache_entry_t;

// Cache state
static struct {
    cache_entry_t** hash_table;  // Hash table (array of entry pointers)
    size_t hash_size;            // Hash table size
    cache_entry_t* lru_head;     // Most recently used
    cache_entry_t* lru_tail;     // Least recently used
    size_t max_entries;          // Maximum cache entries
    size_t num_entries;          // Current number of entries
    size_t hits;                 // Cache hits
    size_t misses;               // Cache misses
    bool initialized;
} cache = {0};

/**
 * Hash function for (font, codepoint) pair
 */
static inline size_t hash_key(font_t* font, uint32_t codepoint) {
    // FNV-1a hash
    size_t hash = 2166136261u;
    hash ^= (size_t)font;
    hash *= 16777619u;
    hash ^= codepoint;
    hash *= 16777619u;
    return hash % cache.hash_size;
}

/**
 * Move entry to front of LRU list (mark as most recently used)
 */
static void lru_touch(cache_entry_t* entry) {
    if (!entry || entry == cache.lru_head) return;

    // Remove from current position
    if (entry->lru_prev) entry->lru_prev->lru_next = entry->lru_next;
    if (entry->lru_next) entry->lru_next->lru_prev = entry->lru_prev;
    if (entry == cache.lru_tail) cache.lru_tail = entry->lru_prev;

    // Insert at head
    entry->lru_prev = NULL;
    entry->lru_next = cache.lru_head;
    if (cache.lru_head) cache.lru_head->lru_prev = entry;
    cache.lru_head = entry;
    if (!cache.lru_tail) cache.lru_tail = entry;
}

/**
 * Evict least recently used entry
 */
static void lru_evict(void) {
    if (!cache.lru_tail) return;

    cache_entry_t* victim = cache.lru_tail;

    // Remove from LRU list
    cache.lru_tail = victim->lru_prev;
    if (cache.lru_tail) {
        cache.lru_tail->lru_next = NULL;
    } else {
        cache.lru_head = NULL;
    }

    // Remove from hash table
    size_t idx = hash_key(victim->font, victim->codepoint);
    cache_entry_t** ptr = &cache.hash_table[idx];
    while (*ptr) {
        if (*ptr == victim) {
            *ptr = victim->next;
            break;
        }
        ptr = &(*ptr)->next;
    }

    // Free glyph data
    if (victim->glyph) {
        if (victim->glyph->bitmap) free(victim->glyph->bitmap);
        free(victim->glyph);
    }

    free(victim);
    cache.num_entries--;
}

/**
 * Initialize font cache
 */
bool font_init(size_t cache_size) {
    if (cache.initialized) return true;

    if (cache_size == 0) cache_size = 1000;  // Default

    cache.max_entries = cache_size;
    cache.hash_size = cache_size * 2;  // 50% load factor target

    cache.hash_table = calloc(cache.hash_size, sizeof(cache_entry_t*));
    if (!cache.hash_table) return false;

    cache.initialized = true;
    return true;
}

/**
 * Shutdown font cache
 */
void font_shutdown(void) {
    if (!cache.initialized) return;

    // Free all entries
    font_cache_clear(NULL);

    free(cache.hash_table);
    memset(&cache, 0, sizeof(cache));
}

/**
 * Get glyph from cache (or rasterize if not found)
 */
const font_glyph_t* font_get_glyph(font_t* font, uint32_t codepoint) {
    if (!font || !cache.initialized) return NULL;

    // Look up in hash table
    size_t idx = hash_key(font, codepoint);
    cache_entry_t* entry = cache.hash_table[idx];

    while (entry) {
        if (entry->font == font && entry->codepoint == codepoint) {
            // Cache hit!
            cache.hits++;
            lru_touch(entry);
            return entry->glyph;
        }
        entry = entry->next;
    }

    // Cache miss - rasterize glyph
    cache.misses++;

    font_glyph_t* glyph = font_rasterize_glyph_internal(font, codepoint);
    if (!glyph) return NULL;

    // Evict if cache is full
    if (cache.num_entries >= cache.max_entries) {
        lru_evict();
    }

    // Create cache entry
    entry = calloc(1, sizeof(cache_entry_t));
    if (!entry) {
        if (glyph->bitmap) free(glyph->bitmap);
        free(glyph);
        return NULL;
    }

    entry->font = font;
    entry->codepoint = codepoint;
    entry->glyph = glyph;

    // Insert into hash table
    entry->next = cache.hash_table[idx];
    cache.hash_table[idx] = entry;

    // Insert into LRU list at head
    lru_touch(entry);

    cache.num_entries++;
    return glyph;
}

/**
 * Clear cache for specific font (or all fonts if NULL)
 */
void font_cache_clear(font_t* font) {
    if (!cache.initialized) return;

    // Iterate through all hash buckets
    for (size_t i = 0; i < cache.hash_size; i++) {
        cache_entry_t** ptr = &cache.hash_table[i];
        while (*ptr) {
            cache_entry_t* entry = *ptr;

            // Check if we should remove this entry
            if (!font || entry->font == font) {
                // Remove from hash chain
                *ptr = entry->next;

                // Remove from LRU list
                if (entry->lru_prev) entry->lru_prev->lru_next = entry->lru_next;
                if (entry->lru_next) entry->lru_next->lru_prev = entry->lru_prev;
                if (entry == cache.lru_head) cache.lru_head = entry->lru_next;
                if (entry == cache.lru_tail) cache.lru_tail = entry->lru_prev;

                // Free glyph data
                if (entry->glyph) {
                    if (entry->glyph->bitmap) free(entry->glyph->bitmap);
                    free(entry->glyph);
                }

                free(entry);
                cache.num_entries--;
            } else {
                ptr = &entry->next;
            }
        }
    }
}

/**
 * Get cache statistics
 */
void font_cache_stats(size_t* hits, size_t* misses, size_t* size) {
    if (hits) *hits = cache.hits;
    if (misses) *misses = cache.misses;
    if (size) *size = cache.num_entries;
}
