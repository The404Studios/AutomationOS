/**
 * Memory Pool for Window Buffers
 *
 * Pre-allocates window surface memory to avoid malloc/free overhead during
 * window creation/destruction. Reduces memory fragmentation and improves
 * cache performance.
 *
 * Benefits:
 * - Eliminates malloc() latency (typically 10-50 microseconds)
 * - Reduces memory fragmentation
 * - Better cache locality for window surfaces
 * - Supports fast window creation/destruction
 */

#include "fb_compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Standard window sizes (in pixels)
#define POOL_SIZE_SMALL  (320 * 240)    // 76.8 KB
#define POOL_SIZE_MEDIUM (640 * 480)    // 307.2 KB
#define POOL_SIZE_LARGE  (1024 * 768)   // 786.4 KB
#define POOL_SIZE_XLARGE (1920 * 1080)  // 2073.6 KB

// Pool capacities
#define POOL_SMALL_COUNT  16  // 16x small windows
#define POOL_MEDIUM_COUNT 8   // 8x medium windows
#define POOL_LARGE_COUNT  4   // 4x large windows
#define POOL_XLARGE_COUNT 2   // 2x xlarge windows

/**
 * Pool entry structure
 */
typedef struct {
    uint32_t *pixels;      // Pixel buffer
    uint32_t size_pixels;  // Size in pixels (not bytes)
    bool in_use;           // Allocation status
    uint32_t alloc_count;  // Usage counter
} pool_entry_t;

/**
 * Memory pool structure
 */
typedef struct {
    pool_entry_t small[POOL_SMALL_COUNT];
    pool_entry_t medium[POOL_MEDIUM_COUNT];
    pool_entry_t large[POOL_LARGE_COUNT];
    pool_entry_t xlarge[POOL_XLARGE_COUNT];

    // Statistics
    uint32_t alloc_hits;     // Allocations from pool
    uint32_t alloc_misses;   // Allocations via malloc
    uint32_t free_hits;      // Frees returned to pool
    uint32_t free_misses;    // Frees via free()

    uint32_t total_pool_bytes;
    uint32_t used_pool_bytes;
} mempool_t;

static mempool_t g_mempool;
static bool g_mempool_initialized = false;

/**
 * Initialize memory pool
 */
bool mempool_init(void) {
    if (g_mempool_initialized) {
        printf("[MemPool] Already initialized\n");
        return true;
    }

    memset(&g_mempool, 0, sizeof(mempool_t));

    uint32_t total_bytes = 0;

    // Allocate small buffers
    for (uint32_t i = 0; i < POOL_SMALL_COUNT; i++) {
        g_mempool.small[i].pixels = calloc(POOL_SIZE_SMALL, sizeof(uint32_t));
        if (!g_mempool.small[i].pixels) {
            fprintf(stderr, "[MemPool] Failed to allocate small buffer %u\n", i);
            mempool_cleanup();
            return false;
        }
        g_mempool.small[i].size_pixels = POOL_SIZE_SMALL;
        g_mempool.small[i].in_use = false;
        g_mempool.small[i].alloc_count = 0;
        total_bytes += POOL_SIZE_SMALL * sizeof(uint32_t);
    }

    // Allocate medium buffers
    for (uint32_t i = 0; i < POOL_MEDIUM_COUNT; i++) {
        g_mempool.medium[i].pixels = calloc(POOL_SIZE_MEDIUM, sizeof(uint32_t));
        if (!g_mempool.medium[i].pixels) {
            fprintf(stderr, "[MemPool] Failed to allocate medium buffer %u\n", i);
            mempool_cleanup();
            return false;
        }
        g_mempool.medium[i].size_pixels = POOL_SIZE_MEDIUM;
        g_mempool.medium[i].in_use = false;
        g_mempool.medium[i].alloc_count = 0;
        total_bytes += POOL_SIZE_MEDIUM * sizeof(uint32_t);
    }

    // Allocate large buffers
    for (uint32_t i = 0; i < POOL_LARGE_COUNT; i++) {
        g_mempool.large[i].pixels = calloc(POOL_SIZE_LARGE, sizeof(uint32_t));
        if (!g_mempool.large[i].pixels) {
            fprintf(stderr, "[MemPool] Failed to allocate large buffer %u\n", i);
            mempool_cleanup();
            return false;
        }
        g_mempool.large[i].size_pixels = POOL_SIZE_LARGE;
        g_mempool.large[i].in_use = false;
        g_mempool.large[i].alloc_count = 0;
        total_bytes += POOL_SIZE_LARGE * sizeof(uint32_t);
    }

    // Allocate xlarge buffers
    for (uint32_t i = 0; i < POOL_XLARGE_COUNT; i++) {
        g_mempool.xlarge[i].pixels = calloc(POOL_SIZE_XLARGE, sizeof(uint32_t));
        if (!g_mempool.xlarge[i].pixels) {
            fprintf(stderr, "[MemPool] Failed to allocate xlarge buffer %u\n", i);
            mempool_cleanup();
            return false;
        }
        g_mempool.xlarge[i].size_pixels = POOL_SIZE_XLARGE;
        g_mempool.xlarge[i].in_use = false;
        g_mempool.xlarge[i].alloc_count = 0;
        total_bytes += POOL_SIZE_XLARGE * sizeof(uint32_t);
    }

    g_mempool.total_pool_bytes = total_bytes;
    g_mempool.used_pool_bytes = 0;

    g_mempool_initialized = true;

    printf("[MemPool] Initialized: %.2f MB total\n", total_bytes / (1024.0 * 1024.0));
    printf("[MemPool]   Small: %u x %u pixels (%.1f KB each)\n",
           POOL_SMALL_COUNT, POOL_SIZE_SMALL,
           POOL_SIZE_SMALL * 4 / 1024.0);
    printf("[MemPool]   Medium: %u x %u pixels (%.1f KB each)\n",
           POOL_MEDIUM_COUNT, POOL_SIZE_MEDIUM,
           POOL_SIZE_MEDIUM * 4 / 1024.0);
    printf("[MemPool]   Large: %u x %u pixels (%.1f KB each)\n",
           POOL_LARGE_COUNT, POOL_SIZE_LARGE,
           POOL_SIZE_LARGE * 4 / 1024.0);
    printf("[MemPool]   XLarge: %u x %u pixels (%.1f KB each)\n",
           POOL_XLARGE_COUNT, POOL_SIZE_XLARGE,
           POOL_SIZE_XLARGE * 4 / 1024.0);

    return true;
}

/**
 * Cleanup memory pool
 */
void mempool_cleanup(void) {
    if (!g_mempool_initialized) return;

    // Free all buffers
    for (uint32_t i = 0; i < POOL_SMALL_COUNT; i++) {
        free(g_mempool.small[i].pixels);
    }
    for (uint32_t i = 0; i < POOL_MEDIUM_COUNT; i++) {
        free(g_mempool.medium[i].pixels);
    }
    for (uint32_t i = 0; i < POOL_LARGE_COUNT; i++) {
        free(g_mempool.large[i].pixels);
    }
    for (uint32_t i = 0; i < POOL_XLARGE_COUNT; i++) {
        free(g_mempool.xlarge[i].pixels);
    }

    memset(&g_mempool, 0, sizeof(mempool_t));
    g_mempool_initialized = false;

    printf("[MemPool] Cleaned up\n");
}

/**
 * Find best-fit pool for requested size
 */
static pool_entry_t *find_pool_entry(uint32_t width, uint32_t height) {
    uint32_t required_pixels = width * height;

    // Try small pool
    if (required_pixels <= POOL_SIZE_SMALL) {
        for (uint32_t i = 0; i < POOL_SMALL_COUNT; i++) {
            if (!g_mempool.small[i].in_use) {
                return &g_mempool.small[i];
            }
        }
    }

    // Try medium pool
    if (required_pixels <= POOL_SIZE_MEDIUM) {
        for (uint32_t i = 0; i < POOL_MEDIUM_COUNT; i++) {
            if (!g_mempool.medium[i].in_use) {
                return &g_mempool.medium[i];
            }
        }
    }

    // Try large pool
    if (required_pixels <= POOL_SIZE_LARGE) {
        for (uint32_t i = 0; i < POOL_LARGE_COUNT; i++) {
            if (!g_mempool.large[i].in_use) {
                return &g_mempool.large[i];
            }
        }
    }

    // Try xlarge pool
    if (required_pixels <= POOL_SIZE_XLARGE) {
        for (uint32_t i = 0; i < POOL_XLARGE_COUNT; i++) {
            if (!g_mempool.xlarge[i].in_use) {
                return &g_mempool.xlarge[i];
            }
        }
    }

    return NULL;  // No suitable pool entry available
}

/**
 * Allocate window surface from pool
 */
uint32_t *mempool_alloc_surface(uint32_t width, uint32_t height) {
    if (!g_mempool_initialized) {
        fprintf(stderr, "[MemPool] Not initialized\n");
        return NULL;
    }

    pool_entry_t *entry = find_pool_entry(width, height);

    if (entry) {
        // Found a pool entry
        entry->in_use = true;
        entry->alloc_count++;

        g_mempool.alloc_hits++;
        g_mempool.used_pool_bytes += entry->size_pixels * sizeof(uint32_t);

        // Clear buffer (for security and cleanliness)
        memset(entry->pixels, 0, entry->size_pixels * sizeof(uint32_t));

        return entry->pixels;
    }

    // Pool exhausted - fall back to malloc
    g_mempool.alloc_misses++;

    uint32_t *pixels = calloc(width * height, sizeof(uint32_t));
    if (!pixels) {
        fprintf(stderr, "[MemPool] malloc fallback failed for %ux%u\n", width, height);
        return NULL;
    }

    printf("[MemPool] ⚠️  Pool miss: allocated %ux%u via malloc\n", width, height);
    return pixels;
}

/**
 * Free window surface back to pool
 */
void mempool_free_surface(uint32_t *pixels) {
    if (!pixels || !g_mempool_initialized) return;

    // Try to find this pointer in our pools
    for (uint32_t i = 0; i < POOL_SMALL_COUNT; i++) {
        if (g_mempool.small[i].pixels == pixels) {
            g_mempool.small[i].in_use = false;
            g_mempool.free_hits++;
            g_mempool.used_pool_bytes -= g_mempool.small[i].size_pixels * sizeof(uint32_t);
            return;
        }
    }

    for (uint32_t i = 0; i < POOL_MEDIUM_COUNT; i++) {
        if (g_mempool.medium[i].pixels == pixels) {
            g_mempool.medium[i].in_use = false;
            g_mempool.free_hits++;
            g_mempool.used_pool_bytes -= g_mempool.medium[i].size_pixels * sizeof(uint32_t);
            return;
        }
    }

    for (uint32_t i = 0; i < POOL_LARGE_COUNT; i++) {
        if (g_mempool.large[i].pixels == pixels) {
            g_mempool.large[i].in_use = false;
            g_mempool.free_hits++;
            g_mempool.used_pool_bytes -= g_mempool.large[i].size_pixels * sizeof(uint32_t);
            return;
        }
    }

    for (uint32_t i = 0; i < POOL_XLARGE_COUNT; i++) {
        if (g_mempool.xlarge[i].pixels == pixels) {
            g_mempool.xlarge[i].in_use = false;
            g_mempool.free_hits++;
            g_mempool.used_pool_bytes -= g_mempool.xlarge[i].size_pixels * sizeof(uint32_t);
            return;
        }
    }

    // Not in pool - must be a malloc'd buffer
    g_mempool.free_misses++;
    free(pixels);
}

/**
 * Print memory pool statistics
 */
void mempool_print_stats(void) {
    if (!g_mempool_initialized) {
        printf("[MemPool] Not initialized\n");
        return;
    }

    uint32_t small_used = 0, medium_used = 0, large_used = 0, xlarge_used = 0;

    for (uint32_t i = 0; i < POOL_SMALL_COUNT; i++) {
        if (g_mempool.small[i].in_use) small_used++;
    }
    for (uint32_t i = 0; i < POOL_MEDIUM_COUNT; i++) {
        if (g_mempool.medium[i].in_use) medium_used++;
    }
    for (uint32_t i = 0; i < POOL_LARGE_COUNT; i++) {
        if (g_mempool.large[i].in_use) large_used++;
    }
    for (uint32_t i = 0; i < POOL_XLARGE_COUNT; i++) {
        if (g_mempool.xlarge[i].in_use) xlarge_used++;
    }

    float hit_rate = 0.0f;
    uint32_t total_allocs = g_mempool.alloc_hits + g_mempool.alloc_misses;
    if (total_allocs > 0) {
        hit_rate = (float)g_mempool.alloc_hits * 100.0f / total_allocs;
    }

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║           MEMORY POOL STATISTICS                     ║\n");
    printf("╠═══════════════════════════════════════════════════════╣\n");
    printf("║ Pool Usage:                                           ║\n");
    printf("║   Small:  %2u / %2u in use                            ║\n",
           small_used, POOL_SMALL_COUNT);
    printf("║   Medium: %2u / %2u in use                            ║\n",
           medium_used, POOL_MEDIUM_COUNT);
    printf("║   Large:  %2u / %2u in use                            ║\n",
           large_used, POOL_LARGE_COUNT);
    printf("║   XLarge: %2u / %2u in use                            ║\n",
           xlarge_used, POOL_XLARGE_COUNT);
    printf("╠═══════════════════════════════════════════════════════╣\n");
    printf("║ Memory:                                               ║\n");
    printf("║   Total Pool: %.2f MB                                 ║\n",
           g_mempool.total_pool_bytes / (1024.0 * 1024.0));
    printf("║   In Use:     %.2f MB (%.1f%%)                        ║\n",
           g_mempool.used_pool_bytes / (1024.0 * 1024.0),
           (float)g_mempool.used_pool_bytes * 100.0f / g_mempool.total_pool_bytes);
    printf("╠═══════════════════════════════════════════════════════╣\n");
    printf("║ Allocation Stats:                                     ║\n");
    printf("║   Pool Hits:   %u                                     ║\n",
           g_mempool.alloc_hits);
    printf("║   Pool Misses: %u (malloc fallback)                   ║\n",
           g_mempool.alloc_misses);
    printf("║   Hit Rate:    %.1f%%                                 ║\n",
           hit_rate);
    printf("║   Free Hits:   %u                                     ║\n",
           g_mempool.free_hits);
    printf("║   Free Misses: %u                                     ║\n",
           g_mempool.free_misses);
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/**
 * Get pool hit rate
 */
float mempool_get_hit_rate(void) {
    if (!g_mempool_initialized) return 0.0f;

    uint32_t total = g_mempool.alloc_hits + g_mempool.alloc_misses;
    if (total == 0) return 0.0f;

    return (float)g_mempool.alloc_hits * 100.0f / total;
}

/**
 * Get pool usage percentage
 */
float mempool_get_usage_percent(void) {
    if (!g_mempool_initialized || g_mempool.total_pool_bytes == 0) return 0.0f;

    return (float)g_mempool.used_pool_bytes * 100.0f / g_mempool.total_pool_bytes;
}

/**
 * Reset statistics (for benchmarking)
 */
void mempool_reset_stats(void) {
    if (!g_mempool_initialized) return;

    g_mempool.alloc_hits = 0;
    g_mempool.alloc_misses = 0;
    g_mempool.free_hits = 0;
    g_mempool.free_misses = 0;

    printf("[MemPool] Statistics reset\n");
}
