/**
 * AHCI Block Device Adapter
 *
 * Integrates AHCI driver with block device layer
 */

#include "../../include/ahci.h"
#include "../../include/block.h"
#include "../../include/kernel.h"
#include "../../include/string.h"

// AHCI block device operations
static bool ahci_block_read(void* device, uint64_t lba, uint32_t count, void* buffer) {
    ahci_port_t* port = (ahci_port_t*)device;

    // Use NCQ if supported and beneficial
    if (port->supports_ncq && count > 8) {
        return ahci_read_sectors_ncq(port, lba, count, buffer);
    } else {
        return ahci_read_sectors(port, lba, count, buffer);
    }
}

static bool ahci_block_write(void* device, uint64_t lba, uint32_t count, const void* buffer) {
    ahci_port_t* port = (ahci_port_t*)device;

    // Use NCQ if supported and beneficial
    if (port->supports_ncq && count > 8) {
        return ahci_write_sectors_ncq(port, lba, count, buffer);
    } else {
        return ahci_write_sectors(port, lba, count, buffer);
    }
}

static bool ahci_block_flush(void* device) {
    ahci_port_t* port = (ahci_port_t*)device;
    return ahci_flush_cache(port);
}

static block_device_ops_t ahci_block_ops = {
    .read = ahci_block_read,
    .write = ahci_block_write,
    .flush = ahci_block_flush,
};

/**
 * Register AHCI port as block device
 */
bool ahci_register_block_device(ahci_port_t* port, uint8_t port_num) {
    if (!port || !port->device_present) {
        return false;
    }

    char name[32];
    ksnprintf(name, sizeof(name), "sata%u", port_num);

    block_device_t* block_dev = block_register_device(
        BLOCK_DEVICE_AHCI_SATA,
        port,
        &ahci_block_ops,
        port->sectors,
        port->sector_size,
        name
    );

    return block_dev != NULL;
}
