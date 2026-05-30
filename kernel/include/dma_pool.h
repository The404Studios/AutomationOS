/**
 * DMA Buffer Pool - Fast reusable DMA buffers for storage drivers
 */

#ifndef DMA_POOL_H
#define DMA_POOL_H

#include "types.h"

// Forward declaration
typedef struct dma_buffer dma_buffer_t;

/**
 * Initialize DMA buffer pool subsystem
 *
 * Call early during kernel initialization, after PMM is ready.
 * Typical configuration: 128 x 4KB buffers, 32 x 64KB buffers
 *
 * @param num_4k_buffers  Number of 4KB buffers (for single-sector I/O)
 * @param num_64k_buffers Number of 64KB buffers (for large transfers)
 * @return 0 on success, -1 on failure
 */
int dma_pool_init(uint32_t num_4k_buffers, uint32_t num_64k_buffers);

/**
 * Allocate a 4KB DMA buffer from the pool
 *
 * Fast O(1) allocation. Buffer is pre-allocated, physically contiguous,
 * 4KB-aligned, and identity-mapped (phys == virt).
 *
 * @return Buffer descriptor or NULL if pool exhausted
 */
dma_buffer_t* dma_buffer_alloc(void);

/**
 * Allocate a 64KB DMA buffer from the pool
 *
 * For large I/O transfers (e.g., multi-sector reads/writes).
 *
 * @return Buffer descriptor or NULL if pool exhausted
 */
dma_buffer_t* dma_buffer_alloc_64k(void);

/**
 * Free a DMA buffer back to the pool
 *
 * Fast O(1) return to free list. Buffer becomes available for reuse.
 *
 * @param buf Buffer to free (from dma_buffer_alloc)
 */
void dma_buffer_free(dma_buffer_t* buf);

/**
 * Get CPU-visible (virtual) address from buffer
 *
 * Use this address to read/write buffer contents from CPU.
 *
 * @param buf Buffer descriptor
 * @return Virtual address or NULL
 */
void* dma_buffer_virt(dma_buffer_t* buf);

/**
 * Get device-visible (physical) address from buffer
 *
 * Use this address to program DMA controller (AHCI PRDT, NVMe PRP, etc.)
 *
 * @param buf Buffer descriptor
 * @return Physical address or 0
 */
uint64_t dma_buffer_phys(dma_buffer_t* buf);

/**
 * Get buffer size in bytes
 *
 * @param buf Buffer descriptor
 * @return Size in bytes (4096 or 65536)
 */
size_t dma_buffer_size(dma_buffer_t* buf);

/**
 * Print pool statistics (debug)
 *
 * Shows allocation counts, peak usage, failed allocations, etc.
 */
void dma_pool_print_stats(void);

/**
 * Check for buffer leaks (debug)
 *
 * Warns if buffers are still allocated at shutdown.
 */
void dma_pool_check_leaks(void);

/**
 * Get pool utilization percentage
 *
 * @return 0-100 (percentage of 4KB pool in use)
 */
uint32_t dma_pool_utilization(void);

/**
 * Run smoke test to validate pool functionality
 *
 * Call after dma_pool_init() to verify everything works.
 *
 * @return 0 on success, -1 on failure
 */
int dma_pool_smoke_test(void);

#endif
