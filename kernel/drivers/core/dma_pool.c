/**
 * DMA Buffer Pool for Storage Drivers
 * ====================================
 *
 * Provides reusable DMA buffers to avoid allocation overhead on every I/O.
 * Pre-allocates physically contiguous, 4KB-aligned buffers suitable for
 * AHCI, NVMe, and other DMA-capable storage controllers.
 *
 * Key Features:
 * - Fast O(1) allocation/free via free-list
 * - 4KB alignment for DMA controllers
 * - Identity-mapped (phys == virt) for direct DMA addressing
 * - Zero-copy where possible
 * - Thread-safe (with spinlocks when SMP is enabled)
 *
 * Usage:
 *   1. Initialize pool: dma_pool_init(num_buffers, buffer_size)
 *   2. Allocate: dma_buffer_alloc() returns { virt_addr, phys_addr }
 *   3. Use for DMA I/O
 *   4. Free: dma_buffer_free(buffer)
 */

#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/string.h"
#include "../../include/spinlock.h"

#define DMA_POOL_MAX_BUFFERS  256
#define DMA_BUFFER_SIZE_4K    4096
#define DMA_BUFFER_SIZE_64K   (64 * 1024)

typedef struct dma_buffer {
    void* virt_addr;           // CPU-visible address (identity-mapped)
    uint64_t phys_addr;        // Device-visible physical address
    size_t size;               // Buffer size in bytes
    bool free;                 // 1 = available, 0 = in use
    uint32_t alloc_count;      // Number of times allocated (for stats)
    struct dma_buffer* next;   // Next in free list
} dma_buffer_t;

typedef struct {
    dma_buffer_t buffers[DMA_POOL_MAX_BUFFERS];
    dma_buffer_t* free_list;   // Head of free list
    uint32_t total_buffers;
    uint32_t free_count;
    uint32_t buffer_size;
    spinlock_t lock;           // Protects pool access (SMP-safe)

    // Statistics
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t peak_usage;
    uint64_t failed_allocs;
} dma_pool_state_t;

static dma_pool_state_t g_dma_pool_4k;
static dma_pool_state_t g_dma_pool_64k;
static bool g_dma_pool_initialized = false;

/* ------------------------------------------------------------------------- */
/* Internal Helpers                                                          */
/* ------------------------------------------------------------------------- */

/**
 * Initialize a single pool with specified buffer size
 */
static int dma_pool_init_internal(dma_pool_state_t* pool, uint32_t num_buffers,
                                  size_t buffer_size) {
    if (num_buffers > DMA_POOL_MAX_BUFFERS) {
        num_buffers = DMA_POOL_MAX_BUFFERS;
    }

    memset(pool, 0, sizeof(dma_pool_state_t));
    pool->total_buffers = num_buffers;
    pool->buffer_size = buffer_size;
    pool->free_count = 0;
    pool->free_list = NULL;
    spin_lock_init(&pool->lock);

    // Allocate buffers from PMM (identity-mapped, DMA-addressable)
    uint32_t pages_per_buffer = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < num_buffers; i++) {
        void* addr;

        if (pages_per_buffer == 1) {
            addr = pmm_alloc_page();
        } else {
            addr = pmm_alloc_pages(pages_per_buffer);
        }

        if (!addr) {
            kprintf("[DMA Pool] Warning: Only allocated %u/%u buffers of size %zu\n",
                    i, num_buffers, buffer_size);
            pool->total_buffers = i;
            break;
        }

        // Zero the buffer
        memset(addr, 0, buffer_size);

        // Initialize buffer descriptor
        dma_buffer_t* buf = &pool->buffers[i];
        buf->virt_addr = addr;
        buf->phys_addr = (uint64_t)addr;  // Identity-mapped
        buf->size = buffer_size;
        buf->free = true;
        buf->alloc_count = 0;

        // Add to free list
        buf->next = pool->free_list;
        pool->free_list = buf;
        pool->free_count++;
    }

    kprintf("[DMA Pool] Initialized: %u buffers of %zu bytes each (%zu KB total)\n",
            pool->total_buffers, buffer_size,
            (pool->total_buffers * buffer_size) / 1024);

    return pool->total_buffers > 0 ? 0 : -1;
}

/**
 * Allocate from a specific pool
 */
static dma_buffer_t* dma_pool_alloc_internal(dma_pool_state_t* pool) {
    spin_lock(&pool->lock);

    if (!pool->free_list) {
        pool->failed_allocs++;
        spin_unlock(&pool->lock);
        return NULL;
    }

    // Pop from free list
    dma_buffer_t* buf = pool->free_list;
    pool->free_list = buf->next;
    buf->next = NULL;
    buf->free = false;
    buf->alloc_count++;

    pool->free_count--;
    pool->total_allocs++;

    uint32_t current_usage = pool->total_buffers - pool->free_count;
    if (current_usage > pool->peak_usage) {
        pool->peak_usage = current_usage;
    }

    spin_unlock(&pool->lock);
    return buf;
}

/**
 * Free back to a specific pool
 */
static void dma_pool_free_internal(dma_pool_state_t* pool, dma_buffer_t* buf) {
    if (!buf || buf->free) {
        return;  // Double free protection
    }

    spin_lock(&pool->lock);

    // Optional: zero buffer on free for security
    // memset(buf->virt_addr, 0, buf->size);

    // Push onto free list
    buf->next = pool->free_list;
    pool->free_list = buf;
    buf->free = true;

    pool->free_count++;
    pool->total_frees++;

    spin_unlock(&pool->lock);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/**
 * Initialize DMA buffer pool subsystem
 *
 * @param num_4k_buffers  Number of 4KB buffers (typical: 128)
 * @param num_64k_buffers Number of 64KB buffers (typical: 32)
 * @return 0 on success, -1 on failure
 */
int dma_pool_init(uint32_t num_4k_buffers, uint32_t num_64k_buffers) {
    if (g_dma_pool_initialized) {
        kprintf("[DMA Pool] Already initialized\n");
        return 0;
    }

    kprintf("[DMA Pool] Initializing buffer pools...\n");

    // Initialize 4KB pool (for single-sector I/O, command tables, etc.)
    if (dma_pool_init_internal(&g_dma_pool_4k, num_4k_buffers,
                                DMA_BUFFER_SIZE_4K) < 0) {
        kprintf("[DMA Pool] Failed to initialize 4KB pool\n");
        return -1;
    }

    // Initialize 64KB pool (for larger I/O transfers)
    if (num_64k_buffers > 0) {
        if (dma_pool_init_internal(&g_dma_pool_64k, num_64k_buffers,
                                    DMA_BUFFER_SIZE_64K) < 0) {
            kprintf("[DMA Pool] Warning: Failed to initialize 64KB pool\n");
            // Don't fail completely if only 64KB pool failed
        }
    }

    g_dma_pool_initialized = true;
    kprintf("[DMA Pool] Initialization complete\n");
    return 0;
}

/**
 * Allocate a DMA buffer from the pool (4KB)
 * Fast O(1) allocation from free list
 *
 * @return Buffer descriptor or NULL if pool exhausted
 */
dma_buffer_t* dma_buffer_alloc(void) {
    if (!g_dma_pool_initialized) {
        kprintf("[DMA Pool] ERROR: Pool not initialized\n");
        return NULL;
    }

    return dma_pool_alloc_internal(&g_dma_pool_4k);
}

/**
 * Allocate a large (64KB) DMA buffer from the pool
 *
 * @return Buffer descriptor or NULL if pool exhausted
 */
dma_buffer_t* dma_buffer_alloc_64k(void) {
    if (!g_dma_pool_initialized) {
        kprintf("[DMA Pool] ERROR: Pool not initialized\n");
        return NULL;
    }

    if (g_dma_pool_64k.total_buffers == 0) {
        // Fall back to allocating from PMM if 64K pool not initialized
        void* addr = pmm_alloc_pages(16);  // 64KB = 16 pages
        if (!addr) return NULL;

        // Create temporary buffer descriptor (not from pool)
        static dma_buffer_t temp_bufs[8];
        static uint32_t temp_idx = 0;

        dma_buffer_t* buf = &temp_bufs[temp_idx++ % 8];
        buf->virt_addr = addr;
        buf->phys_addr = (uint64_t)addr;
        buf->size = DMA_BUFFER_SIZE_64K;
        buf->free = false;
        buf->alloc_count = 1;
        buf->next = NULL;

        return buf;
    }

    return dma_pool_alloc_internal(&g_dma_pool_64k);
}

/**
 * Free a DMA buffer back to the pool
 * Fast O(1) return to free list
 *
 * @param buf Buffer to free (from dma_buffer_alloc)
 */
void dma_buffer_free(dma_buffer_t* buf) {
    if (!buf) return;

    if (!g_dma_pool_initialized) {
        kprintf("[DMA Pool] ERROR: Pool not initialized\n");
        return;
    }

    // Determine which pool this buffer belongs to
    if (buf->size == DMA_BUFFER_SIZE_4K) {
        dma_pool_free_internal(&g_dma_pool_4k, buf);
    } else if (buf->size == DMA_BUFFER_SIZE_64K) {
        if (g_dma_pool_64k.total_buffers > 0) {
            dma_pool_free_internal(&g_dma_pool_64k, buf);
        } else {
            // Was allocated via PMM fallback, free it
            pmm_free_pages(buf->virt_addr, 16);
        }
    }
}

/**
 * Get virtual address from buffer
 */
void* dma_buffer_virt(dma_buffer_t* buf) {
    return buf ? buf->virt_addr : NULL;
}

/**
 * Get physical address from buffer
 */
uint64_t dma_buffer_phys(dma_buffer_t* buf) {
    return buf ? buf->phys_addr : 0;
}

/**
 * Get buffer size
 */
size_t dma_buffer_size(dma_buffer_t* buf) {
    return buf ? buf->size : 0;
}

/**
 * Print pool statistics
 */
void dma_pool_print_stats(void) {
    if (!g_dma_pool_initialized) {
        kprintf("[DMA Pool] Not initialized\n");
        return;
    }

    kprintf("\n[DMA Pool] Statistics:\n");
    kprintf("  4KB Pool:\n");
    kprintf("    Total buffers: %u\n", g_dma_pool_4k.total_buffers);
    kprintf("    Free buffers:  %u\n", g_dma_pool_4k.free_count);
    kprintf("    In use:        %u\n",
            g_dma_pool_4k.total_buffers - g_dma_pool_4k.free_count);
    kprintf("    Peak usage:    %llu\n", g_dma_pool_4k.peak_usage);
    kprintf("    Total allocs:  %llu\n", g_dma_pool_4k.total_allocs);
    kprintf("    Total frees:   %llu\n", g_dma_pool_4k.total_frees);
    kprintf("    Failed allocs: %llu\n", g_dma_pool_4k.failed_allocs);

    if (g_dma_pool_64k.total_buffers > 0) {
        kprintf("  64KB Pool:\n");
        kprintf("    Total buffers: %u\n", g_dma_pool_64k.total_buffers);
        kprintf("    Free buffers:  %u\n", g_dma_pool_64k.free_count);
        kprintf("    In use:        %u\n",
                g_dma_pool_64k.total_buffers - g_dma_pool_64k.free_count);
        kprintf("    Peak usage:    %llu\n", g_dma_pool_64k.peak_usage);
        kprintf("    Total allocs:  %llu\n", g_dma_pool_64k.total_allocs);
        kprintf("    Total frees:   %llu\n", g_dma_pool_64k.total_frees);
        kprintf("    Failed allocs: %llu\n", g_dma_pool_64k.failed_allocs);
    }

    kprintf("\n");
}

/**
 * Check for buffer leaks
 */
void dma_pool_check_leaks(void) {
    if (!g_dma_pool_initialized) return;

    uint32_t leaks_4k = g_dma_pool_4k.total_buffers - g_dma_pool_4k.free_count;
    uint32_t leaks_64k = g_dma_pool_64k.total_buffers - g_dma_pool_64k.free_count;

    if (leaks_4k > 0 || leaks_64k > 0) {
        kprintf("[DMA Pool] LEAK DETECTED: %u x 4KB, %u x 64KB buffers not freed\n",
                leaks_4k, leaks_64k);
    } else {
        kprintf("[DMA Pool] No leaks detected\n");
    }
}

/**
 * Get pool utilization percentage (0-100)
 */
uint32_t dma_pool_utilization(void) {
    if (!g_dma_pool_initialized || g_dma_pool_4k.total_buffers == 0) {
        return 0;
    }

    uint32_t in_use = g_dma_pool_4k.total_buffers - g_dma_pool_4k.free_count;
    return (in_use * 100) / g_dma_pool_4k.total_buffers;
}
