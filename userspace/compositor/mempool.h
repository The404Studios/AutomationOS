/**
 * Memory Pool for Window Buffers - Header
 *
 * Pre-allocated window surface memory to reduce malloc overhead
 */

#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize memory pool
 */
bool mempool_init(void);

/**
 * Cleanup memory pool
 */
void mempool_cleanup(void);

/**
 * Allocate window surface from pool
 */
uint32_t *mempool_alloc_surface(uint32_t width, uint32_t height);

/**
 * Free window surface back to pool
 */
void mempool_free_surface(uint32_t *pixels);

/**
 * Print memory pool statistics
 */
void mempool_print_stats(void);

/**
 * Get pool hit rate
 */
float mempool_get_hit_rate(void);

/**
 * Get pool usage percentage
 */
float mempool_get_usage_percent(void);

/**
 * Reset statistics (for benchmarking)
 */
void mempool_reset_stats(void);

#endif // MEMPOOL_H
