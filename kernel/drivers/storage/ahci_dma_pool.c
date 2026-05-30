/**
 * AHCI DMA Pool Integration
 * ==========================
 *
 * Demonstrates how to use the DMA buffer pool with AHCI driver.
 * This file shows the modified I/O path that uses dma_buffer_alloc()
 * instead of per-port static dma_bounce buffers.
 *
 * Performance Benefits:
 * - Eliminates PMM allocation overhead on every I/O
 * - Enables multiple concurrent I/O operations per port
 * - Reduces memory waste (buffers shared across all ports)
 * - Better cache behavior (buffers reused while hot)
 *
 * Integration Steps:
 * 1. Remove dma_bounce from ahci_port_t
 * 2. Allocate DMA buffer on-demand for each I/O
 * 3. Free buffer immediately after transfer completes
 */

#include "../../include/ahci.h"
#include "../../include/dma_pool.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/string.h"

// Memory barriers
#define wmb() asm volatile("sfence" ::: "memory")
#define rmb() asm volatile("lfence" ::: "memory")

// Forward declarations from ahci.c
extern ahci_controller_t* g_ahci_controller;
extern ahci_port_t* g_blk_port;

int ahci_port_alloc_slot(ahci_port_t* port);
void ahci_port_free_slot(ahci_port_t* port, int slot);
bool ahci_port_issue_cmd(ahci_port_t* port, int slot);
bool ahci_port_wait_cmd(ahci_port_t* port, int slot, uint32_t timeout_ms);

// ATA commands
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define AHCI_FIS_TYPE_REG_H2D  0x27
#define AHCI_TIMEOUT_MS        5000

/* ------------------------------------------------------------------------- */
/* DMA Pool-based I/O (replaces original dma_bounce approach)               */
/* ------------------------------------------------------------------------- */

/**
 * Read/Write one sector using DMA pool buffer
 *
 * Instead of using port->dma_bounce (permanently allocated), we allocate
 * a buffer from the pool for this I/O and return it immediately after.
 */
static bool ahci_rw_one_pooled(ahci_port_t* port, uint64_t lba,
                               void* user_buf, bool write) {
    // Allocate DMA buffer from pool (fast O(1))
    dma_buffer_t* dma_buf = dma_buffer_alloc();
    if (!dma_buf) {
        kprintf("[AHCI] DMA pool exhausted\n");
        return false;
    }

    void* dma_virt = dma_buffer_virt(dma_buf);
    uint64_t dma_phys = dma_buffer_phys(dma_buf);

    // For writes, copy user data into DMA buffer
    if (write) {
        memcpy(dma_virt, user_buf, 512);
    }

    // Allocate command slot
    int slot = ahci_port_alloc_slot(port);
    if (slot < 0) {
        dma_buffer_free(dma_buf);
        return false;
    }

    // Build command header
    ahci_cmd_header_t* hdr = &port->cmd_list[slot];
    hdr->cfl   = sizeof(fis_reg_h2d_t) / 4;
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    // Build command table + FIS
    ahci_cmd_table_t* tbl = port->cmd_tables[slot];
    memset((void*)tbl, 0, 256);

    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)tbl->cfis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis->lba0 = lba & 0xFF;
    fis->lba1 = (lba >> 8) & 0xFF;
    fis->lba2 = (lba >> 16) & 0xFF;
    fis->device = (1 << 6);  // LBA mode
    fis->lba3 = (lba >> 24) & 0xFF;
    fis->lba4 = (lba >> 32) & 0xFF;
    fis->lba5 = (lba >> 40) & 0xFF;
    fis->countl = 1;  // One sector
    fis->counth = 0;

    // Setup PRDT with pool buffer's physical address
    tbl->prdt[0].dba = dma_phys;
    tbl->prdt[0].dbc = 512 - 1;
    tbl->prdt[0].i   = 0;

    wmb();

    // Issue command and wait
    ahci_port_issue_cmd(port, slot);
    bool ok = ahci_port_wait_cmd(port, slot, AHCI_TIMEOUT_MS);

    // For reads, copy DMA buffer back to user
    if (ok && !write) {
        memcpy(user_buf, dma_virt, 512);
    }

    // Cleanup
    ahci_port_free_slot(port, slot);
    dma_buffer_free(dma_buf);  // Return to pool (fast O(1))

    return ok;
}

/**
 * Read sectors using DMA pool (NEW implementation)
 */
bool ahci_read_sectors_pooled(ahci_port_t* port, uint64_t lba,
                               uint32_t count, void* buffer) {
    if (!port || !port->device_present || count == 0) return false;
    if (lba + count > port->sectors) return false;

    uint8_t* dst = (uint8_t*)buffer;
    for (uint32_t s = 0; s < count; s++) {
        if (!ahci_rw_one_pooled(port, lba + s, dst + s * 512, false)) {
            if (g_ahci_controller) g_ahci_controller->total_errors++;
            return false;
        }
        if (g_ahci_controller) g_ahci_controller->total_reads++;
    }
    return true;
}

/**
 * Write sectors using DMA pool (NEW implementation)
 */
bool ahci_write_sectors_pooled(ahci_port_t* port, uint64_t lba,
                                uint32_t count, const void* buffer) {
    if (!port || !port->device_present || count == 0) return false;
    if (lba + count > port->sectors) return false;

    const uint8_t* src = (const uint8_t*)buffer;
    for (uint32_t s = 0; s < count; s++) {
        if (!ahci_rw_one_pooled(port, lba + s, (void*)(src + s * 512), true)) {
            if (g_ahci_controller) g_ahci_controller->total_errors++;
            return false;
        }
        if (g_ahci_controller) g_ahci_controller->total_writes++;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Optimized Multi-Sector I/O (uses 64KB pool buffers)                      */
/* ------------------------------------------------------------------------- */

/**
 * Read multiple sectors at once using large DMA buffer
 *
 * This is MORE efficient than reading one sector at a time:
 * - Single AHCI command instead of N commands
 * - Single DMA transfer instead of N transfers
 * - Amortizes command setup/teardown overhead
 * - Better disk head scheduling
 */
bool ahci_read_sectors_multi(ahci_port_t* port, uint64_t lba,
                              uint32_t count, void* buffer) {
    if (!port || !port->device_present || count == 0) return false;
    if (lba + count > port->sectors) return false;

    // Use 64KB buffer for up to 128 sectors (64KB / 512B)
    const uint32_t MAX_SECTORS_PER_CMD = 128;

    uint8_t* dst = (uint8_t*)buffer;
    uint32_t sectors_done = 0;

    while (sectors_done < count) {
        uint32_t sectors_this_batch = count - sectors_done;
        if (sectors_this_batch > MAX_SECTORS_PER_CMD) {
            sectors_this_batch = MAX_SECTORS_PER_CMD;
        }

        // Allocate large DMA buffer from pool
        dma_buffer_t* dma_buf = dma_buffer_alloc_64k();
        if (!dma_buf) {
            kprintf("[AHCI] Failed to allocate 64KB DMA buffer\n");
            return false;
        }

        void* dma_virt = dma_buffer_virt(dma_buf);
        uint64_t dma_phys = dma_buffer_phys(dma_buf);

        // Allocate command slot
        int slot = ahci_port_alloc_slot(port);
        if (slot < 0) {
            dma_buffer_free(dma_buf);
            return false;
        }

        // Build command
        ahci_cmd_header_t* hdr = &port->cmd_list[slot];
        hdr->cfl   = sizeof(fis_reg_h2d_t) / 4;
        hdr->w     = 0;  // Read
        hdr->prdtl = 1;
        hdr->prdbc = 0;

        ahci_cmd_table_t* tbl = port->cmd_tables[slot];
        memset((void*)tbl, 0, 256);

        fis_reg_h2d_t* fis = (fis_reg_h2d_t*)tbl->cfis;
        fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
        fis->c = 1;
        fis->command = ATA_CMD_READ_DMA_EXT;

        uint64_t current_lba = lba + sectors_done;
        fis->lba0 = current_lba & 0xFF;
        fis->lba1 = (current_lba >> 8) & 0xFF;
        fis->lba2 = (current_lba >> 16) & 0xFF;
        fis->device = (1 << 6);
        fis->lba3 = (current_lba >> 24) & 0xFF;
        fis->lba4 = (current_lba >> 32) & 0xFF;
        fis->lba5 = (current_lba >> 40) & 0xFF;
        fis->countl = sectors_this_batch & 0xFF;
        fis->counth = (sectors_this_batch >> 8) & 0xFF;

        // PRDT: one large entry
        uint32_t transfer_bytes = sectors_this_batch * 512;
        tbl->prdt[0].dba = dma_phys;
        tbl->prdt[0].dbc = transfer_bytes - 1;
        tbl->prdt[0].i   = 0;

        wmb();

        // Execute
        ahci_port_issue_cmd(port, slot);
        bool ok = ahci_port_wait_cmd(port, slot, AHCI_TIMEOUT_MS);

        if (ok) {
            // Copy data from DMA buffer to user buffer
            memcpy(dst + sectors_done * 512, dma_virt, transfer_bytes);
            if (g_ahci_controller) {
                g_ahci_controller->total_reads += sectors_this_batch;
            }
        }

        // Cleanup
        ahci_port_free_slot(port, slot);
        dma_buffer_free(dma_buf);

        if (!ok) {
            if (g_ahci_controller) g_ahci_controller->total_errors++;
            return false;
        }

        sectors_done += sectors_this_batch;
    }

    return true;
}

/**
 * Write multiple sectors at once using large DMA buffer
 */
bool ahci_write_sectors_multi(ahci_port_t* port, uint64_t lba,
                               uint32_t count, const void* buffer) {
    if (!port || !port->device_present || count == 0) return false;
    if (lba + count > port->sectors) return false;

    const uint32_t MAX_SECTORS_PER_CMD = 128;
    const uint8_t* src = (const uint8_t*)buffer;
    uint32_t sectors_done = 0;

    while (sectors_done < count) {
        uint32_t sectors_this_batch = count - sectors_done;
        if (sectors_this_batch > MAX_SECTORS_PER_CMD) {
            sectors_this_batch = MAX_SECTORS_PER_CMD;
        }

        dma_buffer_t* dma_buf = dma_buffer_alloc_64k();
        if (!dma_buf) {
            kprintf("[AHCI] Failed to allocate 64KB DMA buffer\n");
            return false;
        }

        void* dma_virt = dma_buffer_virt(dma_buf);
        uint64_t dma_phys = dma_buffer_phys(dma_buf);

        // Copy user data to DMA buffer
        uint32_t transfer_bytes = sectors_this_batch * 512;
        memcpy(dma_virt, src + sectors_done * 512, transfer_bytes);

        int slot = ahci_port_alloc_slot(port);
        if (slot < 0) {
            dma_buffer_free(dma_buf);
            return false;
        }

        ahci_cmd_header_t* hdr = &port->cmd_list[slot];
        hdr->cfl   = sizeof(fis_reg_h2d_t) / 4;
        hdr->w     = 1;  // Write
        hdr->prdtl = 1;
        hdr->prdbc = 0;

        ahci_cmd_table_t* tbl = port->cmd_tables[slot];
        memset((void*)tbl, 0, 256);

        fis_reg_h2d_t* fis = (fis_reg_h2d_t*)tbl->cfis;
        fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
        fis->c = 1;
        fis->command = ATA_CMD_WRITE_DMA_EXT;

        uint64_t current_lba = lba + sectors_done;
        fis->lba0 = current_lba & 0xFF;
        fis->lba1 = (current_lba >> 8) & 0xFF;
        fis->lba2 = (current_lba >> 16) & 0xFF;
        fis->device = (1 << 6);
        fis->lba3 = (current_lba >> 24) & 0xFF;
        fis->lba4 = (current_lba >> 32) & 0xFF;
        fis->lba5 = (current_lba >> 40) & 0xFF;
        fis->countl = sectors_this_batch & 0xFF;
        fis->counth = (sectors_this_batch >> 8) & 0xFF;

        tbl->prdt[0].dba = dma_phys;
        tbl->prdt[0].dbc = transfer_bytes - 1;
        tbl->prdt[0].i   = 0;

        wmb();

        ahci_port_issue_cmd(port, slot);
        bool ok = ahci_port_wait_cmd(port, slot, AHCI_TIMEOUT_MS);

        ahci_port_free_slot(port, slot);
        dma_buffer_free(dma_buf);

        if (!ok) {
            if (g_ahci_controller) g_ahci_controller->total_errors++;
            return false;
        }

        if (g_ahci_controller) {
            g_ahci_controller->total_writes += sectors_this_batch;
        }

        sectors_done += sectors_this_batch;
    }

    return true;
}
