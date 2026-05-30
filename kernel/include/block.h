#ifndef BLOCK_H
#define BLOCK_H

#include "types.h"

// Block device types
typedef enum {
    BLOCK_DEVICE_AHCI_SATA,
    BLOCK_DEVICE_NVME,
    BLOCK_DEVICE_IDE,
    BLOCK_DEVICE_RAMDISK,
} block_device_type_t;

// Block device operations
typedef struct block_device_ops {
    bool (*read)(void* device, uint64_t lba, uint32_t count, void* buffer);
    bool (*write)(void* device, uint64_t lba, uint32_t count, const void* buffer);
    bool (*flush)(void* device);
} block_device_ops_t;

// Block device structure
typedef struct {
    block_device_type_t type;
    void* driver_data;
    block_device_ops_t* ops;

    uint64_t sector_count;
    uint32_t sector_size;
    char name[32];

    // Statistics
    uint64_t reads;
    uint64_t writes;
    uint64_t errors;
} block_device_t;

// Block layer functions
void block_init(void);
block_device_t* block_register_device(block_device_type_t type, void* driver_data,
                                      block_device_ops_t* ops, uint64_t sector_count,
                                      uint32_t sector_size, const char* name);
block_device_t* block_get_device(const char* name);
bool block_read(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer);
bool block_write(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer);
bool block_flush(block_device_t* dev);
void block_list_devices(void);

#endif
