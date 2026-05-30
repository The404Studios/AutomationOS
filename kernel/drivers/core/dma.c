/*
 * DMA (Direct Memory Access) Framework
 * Handles DMA memory allocation, mapping, and scatter-gather operations
 */

#include "../../include/dma.h"
#include "../../include/device.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// DMA debug flag
static bool dma_debug_enabled = false;

// DMA statistics
static struct {
    uint64_t total_allocations;
    uint64_t total_mappings;
    uint64_t active_allocations;
    uint64_t active_mappings;
    uint64_t bytes_allocated;
} dma_stats;

/**
 * Initialize DMA subsystem
 */
int dma_init(void) {
    memset(&dma_stats, 0, sizeof(dma_stats));
    kprintf("[DMA] DMA subsystem initialized\n");
    return 0;
}

/**
 * Allocate coherent (consistent) DMA memory
 * Returns CPU-visible pointer and sets device-visible address
 */
void* dma_alloc_coherent(device_t* dev, size_t size, uint64_t* dma_handle, uint32_t flags) {
    if (!dev || !dma_handle || size == 0) {
        return NULL;
    }

    // Align size to page boundary
    size = ALIGN_UP(size, 4096);

    // Allocate physically contiguous memory
    void* cpu_addr = pmm_alloc_pages(size / 4096);
    if (!cpu_addr) {
        kprintf("[DMA] Failed to allocate %zu bytes for device %s\n", size, dev->name);
        return NULL;
    }

    // For now, assume identity mapping (physical == DMA address)
    // In future, this will go through IOMMU
    *dma_handle = (uint64_t)cpu_addr;

    // Zero memory if requested
    if (flags & DMA_ATTR_WRITE_COMBINE || !(flags & DMA_ATTR_NO_KERNEL_MAPPING)) {
        memset(cpu_addr, 0, size);
    }

    dma_stats.total_allocations++;
    dma_stats.active_allocations++;
    dma_stats.bytes_allocated += size;

    if (dma_debug_enabled) {
        kprintf("[DMA] Allocated coherent: dev=%s size=%zu cpu=%p dma=%llx\n",
                dev->name, size, cpu_addr, *dma_handle);
    }

    return cpu_addr;
}

/**
 * Free coherent DMA memory
 */
void dma_free_coherent(device_t* dev, size_t size, void* cpu_addr, uint64_t dma_handle) {
    if (!dev || !cpu_addr || size == 0) {
        return;
    }

    size = ALIGN_UP(size, 4096);

    // Free pages
    pmm_free_pages(cpu_addr, size / 4096);

    dma_stats.active_allocations--;
    dma_stats.bytes_allocated -= size;

    if (dma_debug_enabled) {
        kprintf("[DMA] Freed coherent: dev=%s size=%zu cpu=%p dma=%llx\n",
                dev->name, size, cpu_addr, dma_handle);
    }
}

/**
 * Map single buffer for DMA streaming
 */
uint64_t dma_map_single(device_t* dev, void* ptr, size_t size, dma_direction_t dir) {
    if (!dev || !ptr || size == 0) {
        return 0;
    }

    // For now, assume identity mapping
    uint64_t dma_addr = (uint64_t)ptr;

    dma_stats.total_mappings++;
    dma_stats.active_mappings++;

    if (dma_debug_enabled) {
        kprintf("[DMA] Mapped single: dev=%s ptr=%p size=%zu dir=%d dma=%llx\n",
                dev->name, ptr, size, dir, dma_addr);
    }

    return dma_addr;
}

/**
 * Unmap single DMA buffer
 */
void dma_unmap_single(device_t* dev, uint64_t dma_addr, size_t size, dma_direction_t dir) {
    if (!dev || dma_addr == 0 || size == 0) {
        return;
    }

    dma_stats.active_mappings--;

    if (dma_debug_enabled) {
        kprintf("[DMA] Unmapped single: dev=%s dma=%llx size=%zu dir=%d\n",
                dev->name, dma_addr, size, dir);
    }
}

/**
 * Sync DMA buffer for CPU access
 */
void dma_sync_single_for_cpu(device_t* dev, uint64_t dma_addr, size_t size, dma_direction_t dir) {
    // TODO: Implement cache operations if needed
    // For x86-64, cache coherency is usually handled by hardware
}

/**
 * Sync DMA buffer for device access
 */
void dma_sync_single_for_device(device_t* dev, uint64_t dma_addr, size_t size, dma_direction_t dir) {
    // TODO: Implement cache operations if needed
}

/**
 * Map scatter-gather list for DMA
 */
int dma_map_sg(device_t* dev, dma_sg_table_t* sgt, dma_direction_t dir) {
    if (!dev || !sgt || !sgt->entries || sgt->num_entries == 0) {
        return -1;
    }

    // Map each entry
    for (uint32_t i = 0; i < sgt->num_entries; i++) {
        dma_sg_entry_t* entry = &sgt->entries[i];

        // For now, assume identity mapping
        // entry->dma_addr should already be set by caller
        if (entry->dma_addr == 0) {
            kprintf("[DMA] Warning: SG entry %u has zero DMA address\n", i);
        }
    }

    sgt->num_mapped = sgt->num_entries;
    sgt->direction = dir;

    dma_stats.total_mappings++;
    dma_stats.active_mappings++;

    if (dma_debug_enabled) {
        kprintf("[DMA] Mapped SG: dev=%s entries=%u dir=%d\n",
                dev->name, sgt->num_entries, dir);
    }

    return sgt->num_mapped;
}

/**
 * Unmap scatter-gather list
 */
void dma_unmap_sg(device_t* dev, dma_sg_table_t* sgt, dma_direction_t dir) {
    if (!dev || !sgt) {
        return;
    }

    sgt->num_mapped = 0;

    dma_stats.active_mappings--;

    if (dma_debug_enabled) {
        kprintf("[DMA] Unmapped SG: dev=%s entries=%u dir=%d\n",
                dev->name, sgt->num_entries, dir);
    }
}

/**
 * Sync scatter-gather list for CPU
 */
void dma_sync_sg_for_cpu(device_t* dev, dma_sg_table_t* sgt, dma_direction_t dir) {
    // TODO: Implement cache operations
}

/**
 * Sync scatter-gather list for device
 */
void dma_sync_sg_for_device(device_t* dev, dma_sg_table_t* sgt, dma_direction_t dir) {
    // TODO: Implement cache operations
}

/**
 * Allocate scatter-gather table
 */
int dma_sg_alloc(dma_sg_table_t* sgt, uint32_t num_entries) {
    if (!sgt || num_entries == 0) {
        return -1;
    }

    sgt->entries = (dma_sg_entry_t*)kmalloc(num_entries * sizeof(dma_sg_entry_t));
    if (!sgt->entries) {
        return -1;
    }

    memset(sgt->entries, 0, num_entries * sizeof(dma_sg_entry_t));
    sgt->num_entries = num_entries;
    sgt->num_mapped = 0;

    return 0;
}

/**
 * Free scatter-gather table
 */
void dma_sg_free(dma_sg_table_t* sgt) {
    if (!sgt || !sgt->entries) {
        return;
    }

    kfree(sgt->entries);
    sgt->entries = NULL;
    sgt->num_entries = 0;
    sgt->num_mapped = 0;
}

/**
 * Initialize scatter-gather entry
 */
void dma_sg_init_entry(dma_sg_table_t* sgt, uint32_t index, void* buf, uint32_t len) {
    if (!sgt || !sgt->entries || index >= sgt->num_entries) {
        return;
    }

    dma_sg_entry_t* entry = &sgt->entries[index];
    entry->dma_addr = (uint64_t)buf;  // Identity mapping for now
    entry->length = len;
    entry->offset = 0;
}

/**
 * Create DMA pool
 */
dma_pool_t* dma_pool_create(const char* name, device_t* dev, size_t size,
                           size_t align, size_t boundary) {
    if (!name || !dev || size == 0) {
        return NULL;
    }

    dma_pool_t* pool = (dma_pool_t*)kmalloc(sizeof(dma_pool_t));
    if (!pool) {
        return NULL;
    }

    memset(pool, 0, sizeof(dma_pool_t));
    pool->name = name;
    pool->dev = dev;
    pool->size = size;
    pool->align = align ? align : 4;
    pool->boundary = boundary;

    // Allocate initial pool (e.g., 64 blocks)
    pool->total_blocks = 64;
    size_t pool_size = pool->total_blocks * ALIGN_UP(size, align);

    uint64_t dma_handle;
    pool->pool_start = dma_alloc_coherent(dev, pool_size, &dma_handle, 0);
    if (!pool->pool_start) {
        kfree(pool);
        return NULL;
    }

    // Build free list
    pool->free_list = pool->pool_start;
    pool->free_blocks = pool->total_blocks;

    void** current = (void**)pool->pool_start;
    for (uint32_t i = 0; i < pool->total_blocks - 1; i++) {
        void* next = (char*)pool->pool_start + (i + 1) * ALIGN_UP(size, align);
        *current = next;
        current = (void**)next;
    }
    *current = NULL;  // Last block

    kprintf("[DMA] Created pool '%s': blocks=%u size=%zu align=%zu\n",
            name, pool->total_blocks, size, align);

    return pool;
}

/**
 * Destroy DMA pool
 */
void dma_pool_destroy(dma_pool_t* pool) {
    if (!pool) {
        return;
    }

    if (pool->free_blocks != pool->total_blocks) {
        kprintf("[DMA] Warning: Destroying pool '%s' with %u/%u blocks still allocated\n",
                pool->name, pool->total_blocks - pool->free_blocks, pool->total_blocks);
    }

    // Free pool memory
    size_t pool_size = pool->total_blocks * ALIGN_UP(pool->size, pool->align);
    dma_free_coherent(pool->dev, pool_size, pool->pool_start, (uint64_t)pool->pool_start);

    kfree(pool);
}

/**
 * Allocate from DMA pool
 */
void* dma_pool_alloc(dma_pool_t* pool, uint64_t* dma_handle) {
    if (!pool || !dma_handle) {
        return NULL;
    }

    if (pool->free_blocks == 0) {
        kprintf("[DMA] Pool '%s' exhausted\n", pool->name);
        return NULL;
    }

    // Pop from free list
    void* block = pool->free_list;
    pool->free_list = *(void**)block;
    pool->free_blocks--;

    // Return DMA address (identity mapping)
    *dma_handle = (uint64_t)block;

    return block;
}

/**
 * Free to DMA pool
 */
void dma_pool_free(dma_pool_t* pool, void* cpu_addr, uint64_t dma_handle) {
    if (!pool || !cpu_addr) {
        return;
    }

    // Push onto free list
    *(void**)cpu_addr = pool->free_list;
    pool->free_list = cpu_addr;
    pool->free_blocks++;
}

/**
 * Set DMA mask for device
 */
void dma_set_mask(device_t* dev, uint64_t mask) {
    if (dev) {
        // TODO: Store in device structure
        kprintf("[DMA] Set DMA mask for %s: 0x%llx\n", dev->name, mask);
    }
}

/**
 * Get DMA mask for device
 */
uint64_t dma_get_mask(device_t* dev) {
    // Default to 64-bit
    return DMA_MASK_64BIT;
}

/**
 * Set coherent DMA mask
 */
void dma_set_coherent_mask(device_t* dev, uint64_t mask) {
    if (dev) {
        kprintf("[DMA] Set coherent DMA mask for %s: 0x%llx\n", dev->name, mask);
    }
}

/**
 * Check if device can access address
 */
bool dma_capable(device_t* dev, uint64_t addr, size_t size) {
    uint64_t mask = dma_get_mask(dev);
    return (addr + size - 1) <= mask;
}

/**
 * Enable DMA debugging
 */
void dma_debug_enable(void) {
    dma_debug_enabled = true;
    kprintf("[DMA] Debug enabled\n");
}

/**
 * Disable DMA debugging
 */
void dma_debug_disable(void) {
    dma_debug_enabled = false;
    kprintf("[DMA] Debug disabled\n");
}

/**
 * Check for DMA leaks
 */
void dma_check_leaks(void) {
    if (dma_stats.active_allocations > 0 || dma_stats.active_mappings > 0) {
        kprintf("[DMA] LEAK DETECTED: %llu active allocations, %llu active mappings\n",
                dma_stats.active_allocations, dma_stats.active_mappings);
    } else {
        kprintf("[DMA] No leaks detected\n");
    }
}

/**
 * Print DMA statistics
 */
void dma_print_stats(void) {
    kprintf("[DMA] Statistics:\n");
    kprintf("  Total allocations: %llu\n", dma_stats.total_allocations);
    kprintf("  Total mappings: %llu\n", dma_stats.total_mappings);
    kprintf("  Active allocations: %llu\n", dma_stats.active_allocations);
    kprintf("  Active mappings: %llu\n", dma_stats.active_mappings);
    kprintf("  Bytes allocated: %llu\n", dma_stats.bytes_allocated);
}

/**
 * Check if IOMMU is present
 */
bool dma_iommu_present(void) {
    // TODO: Implement IOMMU detection
    return false;
}

/**
 * Map through IOMMU
 */
int dma_iommu_map(device_t* dev, uint64_t iova, uint64_t paddr, size_t size) {
    // TODO: Implement IOMMU mapping
    return -1;
}

/**
 * Unmap through IOMMU
 */
void dma_iommu_unmap(device_t* dev, uint64_t iova, size_t size) {
    // TODO: Implement IOMMU unmapping
}
