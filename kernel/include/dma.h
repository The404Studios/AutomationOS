#ifndef DMA_H
#define DMA_H

#include "types.h"

// Forward declaration
struct device;

// DMA direction
typedef enum {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE,        // Data goes to device (write)
    DMA_FROM_DEVICE,      // Data comes from device (read)
    DMA_NONE
} dma_direction_t;

// DMA attributes
#define DMA_ATTR_WRITE_BARRIER      0x0001
#define DMA_ATTR_WEAK_ORDERING      0x0002
#define DMA_ATTR_WRITE_COMBINE      0x0004
#define DMA_ATTR_NON_CONSISTENT     0x0008
#define DMA_ATTR_NO_KERNEL_MAPPING  0x0010
#define DMA_ATTR_SKIP_CPU_SYNC      0x0020
#define DMA_ATTR_FORCE_CONTIGUOUS   0x0040

// DMA addressing constraints
typedef struct {
    uint64_t dma_mask;        // Maximum addressable range
    bool need_64bit;          // Requires 64-bit addressing
    bool need_32bit;          // Limited to 32-bit addressing
    uint32_t alignment;       // Required alignment
    uint32_t boundary;        // Cannot cross this boundary
    uint32_t max_segment_size; // Maximum size per segment
    uint32_t min_align_mask;  // Minimum alignment mask
} dma_constraints_t;

// DMA mapping
typedef struct {
    uint64_t dma_addr;        // Device-visible address
    void* cpu_addr;           // CPU-visible address
    size_t size;              // Size of mapping
    dma_direction_t direction;
    uint32_t flags;
    struct device* dev;
} dma_mapping_t;

// Scatter-gather list entry
typedef struct {
    uint64_t dma_addr;        // Device-visible address
    uint32_t length;          // Length of this segment
    uint32_t offset;          // Offset into page
} dma_sg_entry_t;

// Scatter-gather table
typedef struct {
    dma_sg_entry_t* entries;
    uint32_t num_entries;
    uint32_t num_mapped;      // Number of entries actually mapped
    dma_direction_t direction;
} dma_sg_table_t;

// DMA pool (for frequently allocated small buffers)
typedef struct dma_pool {
    const char* name;
    struct device* dev;
    size_t size;              // Size of each allocation
    size_t align;             // Required alignment
    size_t boundary;          // Boundary constraint
    void* pool_start;         // Start of pool memory
    void* free_list;          // Free list head
    uint32_t total_blocks;
    uint32_t free_blocks;
    void* lock;               // spinlock_t
} dma_pool_t;

// DMA initialization
int dma_init(void);

// DMA coherent (consistent) memory allocation
void* dma_alloc_coherent(struct device* dev, size_t size, uint64_t* dma_handle, uint32_t flags);
void dma_free_coherent(struct device* dev, size_t size, void* cpu_addr, uint64_t dma_handle);

// DMA streaming mappings
uint64_t dma_map_single(struct device* dev, void* ptr, size_t size, dma_direction_t dir);
void dma_unmap_single(struct device* dev, uint64_t dma_addr, size_t size, dma_direction_t dir);

// DMA sync operations (for non-coherent mappings)
void dma_sync_single_for_cpu(struct device* dev, uint64_t dma_addr, size_t size, dma_direction_t dir);
void dma_sync_single_for_device(struct device* dev, uint64_t dma_addr, size_t size, dma_direction_t dir);

// Scatter-gather DMA
int dma_map_sg(struct device* dev, dma_sg_table_t* sgt, dma_direction_t dir);
void dma_unmap_sg(struct device* dev, dma_sg_table_t* sgt, dma_direction_t dir);
void dma_sync_sg_for_cpu(struct device* dev, dma_sg_table_t* sgt, dma_direction_t dir);
void dma_sync_sg_for_device(struct device* dev, dma_sg_table_t* sgt, dma_direction_t dir);

// Scatter-gather table management
int dma_sg_alloc(dma_sg_table_t* sgt, uint32_t num_entries);
void dma_sg_free(dma_sg_table_t* sgt);
void dma_sg_init_entry(dma_sg_table_t* sgt, uint32_t index, void* buf, uint32_t len);

// DMA pool management
dma_pool_t* dma_pool_create(const char* name, struct device* dev, size_t size,
                           size_t align, size_t boundary);
void dma_pool_destroy(dma_pool_t* pool);
void* dma_pool_alloc(dma_pool_t* pool, uint64_t* dma_handle);
void dma_pool_free(dma_pool_t* pool, void* cpu_addr, uint64_t dma_handle);

// DMA constraints
void dma_set_mask(struct device* dev, uint64_t mask);
uint64_t dma_get_mask(struct device* dev);
void dma_set_coherent_mask(struct device* dev, uint64_t mask);
bool dma_capable(struct device* dev, uint64_t addr, size_t size);

// DMA addressing helpers
#define DMA_BIT_MASK(n)  (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

// Common DMA masks
#define DMA_MASK_24BIT   DMA_BIT_MASK(24)  // 16MB
#define DMA_MASK_32BIT   DMA_BIT_MASK(32)  // 4GB
#define DMA_MASK_64BIT   DMA_BIT_MASK(64)  // Full 64-bit

// DMA debugging and validation
void dma_debug_enable(void);
void dma_debug_disable(void);
void dma_check_leaks(void);
void dma_print_stats(void);

// DMA attributes for allocation
typedef struct {
    uint32_t flags;
    uint64_t max_addr;        // Maximum physical address
    uint32_t alignment;
    bool zero_memory;
} dma_alloc_attrs_t;

// Extended DMA allocation with attributes
void* dma_alloc_attrs(struct device* dev, size_t size, uint64_t* dma_handle,
                     dma_alloc_attrs_t* attrs);
void dma_free_attrs(struct device* dev, size_t size, void* cpu_addr,
                   uint64_t dma_handle, dma_alloc_attrs_t* attrs);

// DMA contiguous allocations (for large buffers)
void* dma_alloc_contiguous(struct device* dev, size_t size, uint64_t* dma_handle);
void dma_free_contiguous(struct device* dev, size_t size, void* cpu_addr, uint64_t dma_handle);

// IOMMU support (placeholder for Phase 4)
bool dma_iommu_present(void);
int dma_iommu_map(struct device* dev, uint64_t iova, uint64_t paddr, size_t size);
void dma_iommu_unmap(struct device* dev, uint64_t iova, size_t size);

#endif
