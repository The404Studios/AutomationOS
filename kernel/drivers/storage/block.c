/**
 * Block Device Layer for AutomationOS
 *
 * Provides a generic interface for block devices (AHCI, NVMe, etc.)
 */

#include "../../include/block.h"
#include "../../include/kernel.h"
#include "../../include/string.h"

#define MAX_BLOCK_DEVICES 16

static block_device_t* block_devices[MAX_BLOCK_DEVICES];
static uint32_t num_block_devices = 0;

/**
 * Initialize block layer
 */
void block_init(void) {
    kprintf("[BLOCK] Initializing block device layer...\n");
    memset(block_devices, 0, sizeof(block_devices));
    num_block_devices = 0;
}

/**
 * Register a block device
 */
block_device_t* block_register_device(block_device_type_t type, void* driver_data,
                                      block_device_ops_t* ops, uint64_t sector_count,
                                      uint32_t sector_size, const char* name) {
    if (num_block_devices >= MAX_BLOCK_DEVICES) {
        kprintf("[BLOCK] Maximum number of devices reached\n");
        return NULL;
    }

    block_device_t* dev = (block_device_t*)kmalloc(sizeof(block_device_t));
    if (!dev) {
        return NULL;
    }

    memset(dev, 0, sizeof(block_device_t));
    dev->type = type;
    dev->driver_data = driver_data;
    dev->ops = ops;
    dev->sector_count = sector_count;
    dev->sector_size = sector_size;
    strncpy(dev->name, name, sizeof(dev->name) - 1);

    block_devices[num_block_devices++] = dev;

    kprintf("[BLOCK] Registered device: %s (%llu sectors, %u bytes/sector)\n",
            dev->name, dev->sector_count, dev->sector_size);

    return dev;
}

/**
 * Read from block device
 */
bool block_read(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    if (!dev || !dev->ops || !dev->ops->read) {
        return false;
    }

    /* overflow-safe: `lba + count` (uint64) wraps when lba is within count of
     * UINT64_MAX, making a wrapped-small sum pass the old `> sector_count` test
     * and slip an out-of-range LBA through to the driver callback. count>=0 and
     * the first clause short-circuits so `sector_count - count` never underflows. */
    if (count > dev->sector_count || lba > dev->sector_count - count) {
        kprintf("[BLOCK] Read out of bounds: LBA %llu, count %u, max %llu\n",
                lba, count, dev->sector_count);
        return false;
    }

    bool success = dev->ops->read(dev->driver_data, lba, count, buffer);
    if (success) {
        dev->reads++;
    } else {
        dev->errors++;
    }

    return success;
}

/**
 * Write to block device
 */
bool block_write(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer) {
    if (!dev || !dev->ops || !dev->ops->write) {
        return false;
    }

    /* overflow-safe: `lba + count` (uint64) wraps when lba is within count of
     * UINT64_MAX, making a wrapped-small sum pass the old `> sector_count` test
     * and slip an out-of-range LBA through to the driver callback. count>=0 and
     * the first clause short-circuits so `sector_count - count` never underflows. */
    if (count > dev->sector_count || lba > dev->sector_count - count) {
        kprintf("[BLOCK] Write out of bounds: LBA %llu, count %u, max %llu\n",
                lba, count, dev->sector_count);
        return false;
    }

    bool success = dev->ops->write(dev->driver_data, lba, count, buffer);
    if (success) {
        dev->writes++;
    } else {
        dev->errors++;
    }

    return success;
}

/**
 * Flush block device cache
 */
bool block_flush(block_device_t* dev) {
    if (!dev || !dev->ops || !dev->ops->flush) {
        return false;
    }

    return dev->ops->flush(dev->driver_data);
}

/**
 * Get a block device by name
 */
block_device_t* block_get_device(const char* name) {
    if (!name) {
        return NULL;
    }

    for (uint32_t i = 0; i < num_block_devices; i++) {
        if (strcmp(block_devices[i]->name, name) == 0) {
            return block_devices[i];
        }
    }

    return NULL;
}

/**
 * List all registered block devices
 */
void block_list_devices(void) {
    kprintf("[BLOCK] Registered block devices:\n");
    for (uint32_t i = 0; i < num_block_devices; i++) {
        block_device_t* dev = block_devices[i];
        uint64_t size_mb = (dev->sector_count * dev->sector_size) / (1024 * 1024);
        kprintf("  %s: %llu MB (%llu sectors x %u bytes)\n",
                dev->name, size_mb, dev->sector_count, dev->sector_size);
        kprintf("    Reads: %llu, Writes: %llu, Errors: %llu\n",
                dev->reads, dev->writes, dev->errors);
    }
}
