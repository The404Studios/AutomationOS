/**
 * AHCI / SATA driver
 * ==================
 *
 * Brings up an AHCI HBA discovered over PCI (class 0x01 / subclass 0x06 /
 * prog_if 0x01), maps its register block (ABAR == BAR5), resets the HBA,
 * finds the first implemented port with a connected SATA device, runs ATA
 * IDENTIFY DEVICE, and implements 512-byte-sector READ/WRITE via ATA
 * READ/WRITE DMA EXT using the AHCI command list + received-FIS area +
 * command table + PRDT.
 *
 * --- DMA addressing model (important) ---------------------------------------
 * Physical RAM 0..N GiB is identity-mapped (phys == virt) by paging_init using
 * 2MB huge pages, so a page returned by pmm_alloc_page() can be dereferenced
 * directly AND handed to the DMA engine as a physical address. The kernel heap
 * (kmalloc) lives in a HIGH-HALF virtual window (0xFFFFFFFF9...) that the HBA
 * cannot address, so ALL structures the controller reads/writes (command list,
 * received FIS, command tables, PRDT data buffers) are allocated from the PMM.
 *
 * ABAR is a low (< 4 GiB) physical MMIO address from PCI BAR5; it falls inside
 * the identity map, so we use it as a pointer directly (same trick the
 * framebuffer driver uses).
 *
 * Reference: Serial ATA AHCI Specification Rev 1.3.1
 */

#include "../../include/ahci.h"
#include "../../include/block.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/drivers.h"
#include "../../include/string.h"

// Memory barriers — make sure descriptor writes land before we kick the HBA.
#define mb()  asm volatile("mfence" ::: "memory")
#define wmb() asm volatile("sfence" ::: "memory")
#define rmb() asm volatile("lfence" ::: "memory")

// Global single-controller / single-disk model.
static ahci_controller_t* g_ahci_controller = NULL;
static ahci_port_t*       g_blk_port = NULL;       // device 0 backing port

bool ahci_register_block_device(ahci_port_t* port, uint8_t port_num); // ahci_block.c

#define AHCI_PORT_BASE(abar, port_num) \
    ((ahci_port_regs_t*)((uint8_t*)(abar) + 0x100 + ((port_num) * 0x80)))

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Allocate `bytes` of contiguous, identity-mapped, zeroed DMA memory.
 * We only ever need <= one 4KB page per allocation (cmd list 1KB, rx FIS 256B,
 * cmd table 256B, one sector 512B), so a single PMM page is sufficient and is
 * naturally 4KB aligned (satisfies the 1KB/256/128-byte AHCI alignment rules).
 * Returns the identity-mapped pointer (== physical address) or NULL.
 */
static void* dma_alloc_page(void) {
    void* p = pmm_alloc_page();
    if (!p) return NULL;
    memset(p, 0, PAGE_SIZE);
    return p;
}

/* Physical address of an identity-mapped pointer (phys == virt here). */
static inline uint64_t dma_phys(const volatile void* p) {
    return (uint64_t)(uintptr_t)p;
}

static void strncpy_trim(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    while (i > 0 && dest[i - 1] == ' ') i--;
    dest[i] = '\0';
}

/*
 * Per-millisecond busy-spin budget used as a HARD upper bound on every wait
 * loop, applied IN ADDITION TO the wall-clock timeout. This is the safety net
 * that makes the disk probe un-hangable on real hardware.
 *
 * Why this is not optional: the AHCI bring-up (ahci_init) runs EARLY in boot,
 * before interrupts are enabled (kernel.c calls sti() only at the end). At that
 * point the PIT divisor is already programmed, so timer_get_frequency() returns
 * a NON-ZERO value, but IRQ0 is masked so timer_get_ticks() never advances. A
 * pure wall-clock timeout therefore NEVER fires during boot -- the tick delta
 * stays 0 forever and any spin on a register bit a dead/unresponsive device
 * never sets would loop forever. (This is the exact symptom seen on the T410:
 * boot reaches the AHCI probe and hangs.) The iteration cap below guarantees
 * forward progress and termination regardless of whether the timer is ticking.
 *
 * Sizing: each iteration issues an uncached MMIO register read (slow on real
 * PCIe/DMI -- empirically hundreds of ns to low microseconds) plus a `pause`.
 * Even in the pathological case where a read were as cheap as a few ns, this
 * budget bounds a 1000 ms wait to well under a couple of seconds of wall time;
 * with realistic MMIO latency a 1000 ms wait gives up in far less. The whole
 * disk probe issues only a handful of such waits, so the total worst-case probe
 * time is a second or two before boot continues from the RAM root.
 */
#define AHCI_SPIN_ITERS_PER_MS  200000ULL

/* Absolute ceiling on the iteration cap so an over-large timeout_ms argument
 * (e.g. the 10 s spin-up timeout, or FLUSH's 5 s) can never translate into a
 * multi-minute busy-spin when the timer is frozen. ~2 s-equivalent of spinning
 * at the per-ms rate above is plenty for any real controller to respond. */
#define AHCI_SPIN_ITERS_MAX     (2000ULL * AHCI_SPIN_ITERS_PER_MS)

/*
 * Spin until (*reg & mask) == value, or timeout. Takes a volatile uint32_t*
 * so callers can pass packed register members without -Waddress-of-packed
 * grief (the HBA register struct fields are all naturally 4-byte aligned).
 *
 * DUAL-BOUNDED: returns false (gives up) as soon as EITHER
 *   (a) the wall-clock timeout elapses -- only effective once IRQ0 is live and
 *       ticks actually advance (i.e. post-boot reads/writes), OR
 *   (b) the busy-iteration cap is hit -- ALWAYS effective, including during the
 *       pre-sti() boot probe when ticks are frozen.
 * It can never spin unbounded.
 */
static bool ahci_wait_until(volatile uint32_t* reg, uint32_t mask,
                            uint32_t value, uint32_t timeout_ms) {
    uint64_t start = timer_get_ticks();
    uint32_t freq  = timer_get_frequency();
    uint64_t timeout_ticks = freq ? ((uint64_t)timeout_ms * freq / 1000) : 0;

    /* Hard iteration ceiling, ALWAYS applied (independent of the timer). */
    uint64_t iter_cap = (uint64_t)timeout_ms * AHCI_SPIN_ITERS_PER_MS;
    if (iter_cap > AHCI_SPIN_ITERS_MAX) iter_cap = AHCI_SPIN_ITERS_MAX;
    if (iter_cap == 0) iter_cap = AHCI_SPIN_ITERS_PER_MS;   /* timeout_ms==0 -> tiny but nonzero */

    uint64_t iters = 0;
    while ((*reg & mask) != value) {
        /* (a) wall-clock bound -- fires only when ticks are advancing. */
        if (freq && (timer_get_ticks() - start) > timeout_ticks) {
            return false;
        }
        /* (b) iteration bound -- fires even if ticks are frozen (boot path). */
        if (++iters >= iter_cap) {
            return (*reg & mask) == value;   /* final check, then give up */
        }
        asm volatile("pause");   /* ease bus contention while polling MMIO */
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Controller bring-up                                                       */
/* ------------------------------------------------------------------------- */

ahci_controller_t* ahci_probe_controller(pci_device_t* pci_dev) {
    ahci_controller_t* controller =
        (ahci_controller_t*)kmalloc(sizeof(ahci_controller_t));
    if (!controller) return NULL;
    memset(controller, 0, sizeof(ahci_controller_t));
    controller->pci_dev = pci_dev;

    // Enable MMIO + bus mastering so the HBA can DMA.
    pci_enable_memory_space(pci_dev);
    pci_enable_bus_master(pci_dev);

    uint64_t abar_phys = pci_get_bar(pci_dev, 5);
    if (!abar_phys) {
        kprintf("[AHCI] BAR5 (ABAR) not present - controller initialization failed\n");
        kfree(controller);
        return NULL;
    }
    // ABAR is a low physical MMIO addr; identity-mapped -> use directly.
    controller->abar = (ahci_hba_mem_t*)(uintptr_t)abar_phys;

    uint32_t cap = controller->abar->cap;
    controller->num_ports        = (cap & AHCI_CAP_NP_MASK) + 1;
    controller->num_cmd_slots    = ((cap & AHCI_CAP_NCS_MASK) >> 8) + 1;
    controller->supports_64bit   = (cap & AHCI_CAP_S64A) != 0;
    controller->supports_ncq     = (cap & AHCI_CAP_SNCQ) != 0;
    controller->supports_pm      = (cap & AHCI_CAP_SPM) != 0;
    controller->interface_speed  = (cap & AHCI_CAP_ISS_MASK) >> 20;
    controller->ports_implemented = controller->abar->pi;

    kprintf("[AHCI] ABAR=%p ver=0x%08x ports=%u slots=%u 64bit=%d ncq=%d pi=0x%08x\n",
            (void*)(uintptr_t)abar_phys, controller->abar->vs,
            controller->num_ports, controller->num_cmd_slots,
            controller->supports_64bit, controller->supports_ncq,
            controller->ports_implemented);

    return controller;
}

/*
 * Take ownership from BIOS if BIOS/OS handoff (BOHC) is supported, enable AHCI
 * mode, and (best-effort) reset the HBA.
 */
bool ahci_init_controller(ahci_controller_t* controller) {
    volatile ahci_hba_mem_t* hba = controller->abar;

    // BIOS/OS handoff (CAP2.BOH). Request ownership if BIOS owns the HBA.
    if (hba->cap2 & (1u << 0)) {            // BOH supported
        hba->bohc |= (1u << 1);            // OOS = OS Ownership requested
        // Wait for BOS (BIOS owned) to clear.
        ahci_wait_until((volatile uint32_t*)&hba->bohc, (1u << 0), 0, AHCI_TIMEOUT_MS);
    }

    // Enable AHCI mode before touching anything else.
    hba->ghc |= AHCI_GHC_AE;

    // Reset HBA (GHC.HR self-clears when done).
    hba->ghc |= AHCI_GHC_HR;
    if (!ahci_wait_until((volatile uint32_t*)&hba->ghc, AHCI_GHC_HR, 0, AHCI_TIMEOUT_MS)) {
        kprintf("[AHCI] HBA reset timeout - controller may be unresponsive or disabled in BIOS\n");
        return false;
    }

    // AE is cleared by reset; re-enable.
    hba->ghc |= AHCI_GHC_AE;

    // Bring up every implemented port; stop at the first usable SATA disk for
    // our single-device block model.
    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(controller->ports_implemented & (1u << i))) {
            // Port not implemented in hardware - this is normal, skip silently
            continue;
        }
        if (ahci_init_port(controller, i)) {
            if (!g_blk_port && !controller->ports[i].is_atapi) {
                g_blk_port = &controller->ports[i];
            }
        }
    }

    return true;
}

bool ahci_init_port(ahci_controller_t* controller, uint8_t port_num) {
    ahci_port_t* port = &controller->ports[port_num];
    memset(port, 0, sizeof(*port));
    port->port_num = port_num;
    port->regs = AHCI_PORT_BASE(controller->abar, port_num);

    // Device present and PHY communication established?
    uint32_t ssts = port->regs->ssts;
    uint32_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
    uint32_t ipm = (ssts & AHCI_PORT_SSTS_IPM_MASK) >> 8;
    if (det != AHCI_PORT_SSTS_DET_PRESENT || ipm != 1) {
        return false;  // nothing attached / not active — silent, common case
    }

    kprintf("[AHCI] Port %u: device detected (det=%u ipm=%u)\n", port_num, det, ipm);

    // Stop the command engine before reprogramming pointers.
    if (!ahci_port_stop_cmd(port)) {
        kprintf("[AHCI] Port %u: Command engine stop timeout - controller may need reset\n", port_num);
        return false;
    }

    // Allocate DMA structures from the PMM (identity-mapped, DMA-addressable).
    port->cmd_list = (ahci_cmd_header_t*)dma_alloc_page();   // 1KB, page-aligned
    port->rx_fis   = (ahci_rx_fis_t*)dma_alloc_page();       // 256B, page-aligned
    port->dma_bounce = dma_alloc_page();                     // 1 sector scratch
    if (!port->cmd_list || !port->rx_fis || !port->dma_bounce) {
        kprintf("[AHCI] Port %u: DMA memory allocation failed - insufficient physical RAM\n", port_num);
        return false;
    }

    // One command table page per slot (256B each, page-aligned).
    for (int i = 0; i < AHCI_MAX_CMD_SLOTS; i++) {
        port->cmd_tables[i] = (ahci_cmd_table_t*)dma_alloc_page();
        if (!port->cmd_tables[i]) {
            kprintf("[AHCI] Port %u: Command table %d allocation failed - insufficient physical RAM\n", port_num, i);
            return false;
        }
    }

    // Program command-list and received-FIS base addresses (physical).
    uint64_t clb = dma_phys(port->cmd_list);
    port->regs->clb  = (uint32_t)clb;
    port->regs->clbu = (uint32_t)(clb >> 32);

    uint64_t fb = dma_phys(port->rx_fis);
    port->regs->fb  = (uint32_t)fb;
    port->regs->fbu = (uint32_t)(fb >> 32);

    // Point each command header at its command table.
    for (int i = 0; i < AHCI_MAX_CMD_SLOTS; i++) {
        port->cmd_list[i].ctba  = dma_phys(port->cmd_tables[i]);
        port->cmd_list[i].prdtl = 0;
    }

    // Clear errors and interrupt status.
    port->regs->serr = 0xFFFFFFFF;
    port->regs->is   = 0xFFFFFFFF;

    // Spin up + power on the device, set ICC active, enable FIS receive.
    port->regs->cmd |= AHCI_PORT_CMD_SUD | AHCI_PORT_CMD_POD;
    port->regs->cmd = (port->regs->cmd & ~(0xFu << 28)) | AHCI_PORT_CMD_ICC_ACTIVE;
    port->regs->cmd |= AHCI_PORT_CMD_FRE;

    // Wait for the link/task file to come ready (BSY|DRQ clear).
    if (!ahci_wait_until((volatile uint32_t*)&port->regs->tfd,
                         AHCI_PORT_TFD_STS_BSY | AHCI_PORT_TFD_STS_DRQ, 0,
                         AHCI_SPINUP_TIMEOUT_MS)) {
        kprintf("[AHCI] Port %u: AHCI device timeout (tfd=0x%08x) - check SATA cable or BIOS settings\n",
                port_num, port->regs->tfd);
        return false;
    }

    // Start the command engine.
    if (!ahci_port_start_cmd(port)) {
        kprintf("[AHCI] Port %u: Command engine start timeout - port initialization failed\n", port_num);
        return false;
    }

    if (!ahci_port_detect_device(port)) return false;
    if (!ahci_port_identify(port)) {
        kprintf("[AHCI] Port %u: Drive IDENTIFY command failed - verify drive is functional\n", port_num);
        return false;
    }

    port->device_present = true;

    kprintf("[AHCI] Port %u ready: model=\"%s\" serial=\"%s\" fw=\"%s\"\n",
            port_num, port->model, port->serial, port->firmware);
    kprintf("[AHCI] Port %u: %llu sectors x %u B = %llu MB\n", port_num,
            port->sectors, port->sector_size,
            (port->sectors * port->sector_size) / (1024 * 1024));
    return true;
}

bool ahci_port_start_cmd(ahci_port_t* port) {
    // CR must be clear before setting ST.
    if (!ahci_wait_until((volatile uint32_t*)&port->regs->cmd,
                         AHCI_PORT_CMD_CR, 0, AHCI_TIMEOUT_MS)) {
        return false;
    }
    port->regs->cmd |= AHCI_PORT_CMD_FRE;
    port->regs->cmd |= AHCI_PORT_CMD_ST;
    return true;
}

bool ahci_port_stop_cmd(ahci_port_t* port) {
    port->regs->cmd &= ~AHCI_PORT_CMD_ST;
    if (!ahci_wait_until((volatile uint32_t*)&port->regs->cmd,
                         AHCI_PORT_CMD_CR, 0, AHCI_TIMEOUT_MS)) {
        return false;
    }
    port->regs->cmd &= ~AHCI_PORT_CMD_FRE;
    if (!ahci_wait_until((volatile uint32_t*)&port->regs->cmd,
                         AHCI_PORT_CMD_FR, 0, AHCI_TIMEOUT_MS)) {
        return false;
    }
    return true;
}

bool ahci_port_detect_device(ahci_port_t* port) {
    uint32_t sig = port->regs->sig;
    port->device_signature = sig;
    switch (sig) {
        case ATA_SIG_ATA:
            port->is_atapi = false;
            return true;
        case ATA_SIG_ATAPI:
            kprintf("[AHCI] Port %u: ATAPI device (read/write unsupported)\n",
                    port->port_num);
            port->is_atapi = true;
            return true;
        default:
            kprintf("[AHCI] Port %u: Unsupported device signature 0x%08x - not a standard SATA/ATAPI device\n",
                    port->port_num, sig);
            return false;
    }
}

/* ------------------------------------------------------------------------- */
/* Command slot management                                                   */
/* ------------------------------------------------------------------------- */

int ahci_port_alloc_slot(ahci_port_t* port) {
    uint32_t busy = port->regs->ci | port->regs->sact;
    for (int i = 0; i < AHCI_MAX_CMD_SLOTS; i++) {
        if (!(busy & (1u << i)) && !(port->slot_bitmap & (1u << i))) {
            port->slot_bitmap |= (1u << i);
            return i;
        }
    }
    return -1;
}

void ahci_port_free_slot(ahci_port_t* port, int slot) {
    port->slot_bitmap &= ~(1u << slot);
}

bool ahci_port_issue_cmd(ahci_port_t* port, int slot) {
    // Wait for the port to be idle (BSY|DRQ clear) before issuing.
    ahci_wait_until((volatile uint32_t*)&port->regs->tfd,
                    AHCI_PORT_TFD_STS_BSY | AHCI_PORT_TFD_STS_DRQ, 0,
                    AHCI_TIMEOUT_MS);
    port->regs->is = 0xFFFFFFFF;
    wmb();
    port->regs->ci = (1u << slot);
    return true;
}

bool ahci_port_wait_cmd(ahci_port_t* port, int slot, uint32_t timeout_ms) {
    uint32_t slot_mask = (1u << slot);
    if (!ahci_wait_until((volatile uint32_t*)&port->regs->ci,
                         slot_mask, 0, timeout_ms)) {
        kprintf("[AHCI] Port %u: Command timeout (slot %d, ci=0x%08x is=0x%08x tfd=0x%08x) - drive may be hung or cable faulty\n",
                port->port_num, slot, port->regs->ci, port->regs->is, port->regs->tfd);
        return false;
    }
    if (port->regs->is & AHCI_PORT_INT_TFES) {
        uint32_t tfd = port->regs->tfd;
        kprintf("[AHCI] Port %u: Task file error (tfd=0x%08x) - ATA command failed, check drive health\n", port->port_num, tfd);
        port->regs->is = AHCI_PORT_INT_TFES;
        port->error_count++;
        port->last_error = tfd;
        return false;
    }
    rmb();
    return true;
}

/* ------------------------------------------------------------------------- */
/* IDENTIFY                                                                   */
/* ------------------------------------------------------------------------- */

bool ahci_port_identify(ahci_port_t* port) {
    uint16_t* id = (uint16_t*)port->dma_bounce;  // identity-mapped scratch page
    memset(id, 0, 512);

    int slot = ahci_port_alloc_slot(port);
    if (slot < 0) return false;

    ahci_cmd_header_t* hdr = &port->cmd_list[slot];
    hdr->cfl   = sizeof(fis_reg_h2d_t) / 4;
    hdr->w     = 0;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    ahci_cmd_table_t* tbl = port->cmd_tables[slot];
    memset((void*)tbl, 0, 256);

    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)tbl->cfis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_IDENTIFY_DEVICE;
    fis->device = 0;

    tbl->prdt[0].dba = dma_phys(id);
    tbl->prdt[0].dbc = 512 - 1;
    tbl->prdt[0].i   = 0;

    ahci_port_issue_cmd(port, slot);
    bool ok = ahci_port_wait_cmd(port, slot, AHCI_TIMEOUT_MS);
    ahci_port_free_slot(port, slot);
    if (!ok) return false;

    char raw[41];
    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        raw[i * 2] = (w >> 8) & 0xFF; raw[i * 2 + 1] = w & 0xFF;
    }
    raw[40] = '\0'; strncpy_trim(port->model, raw, 40);

    for (int i = 0; i < 10; i++) {
        uint16_t w = id[10 + i];
        raw[i * 2] = (w >> 8) & 0xFF; raw[i * 2 + 1] = w & 0xFF;
    }
    raw[20] = '\0'; strncpy_trim(port->serial, raw, 20);

    for (int i = 0; i < 4; i++) {
        uint16_t w = id[23 + i];
        raw[i * 2] = (w >> 8) & 0xFF; raw[i * 2 + 1] = w & 0xFF;
    }
    raw[8] = '\0'; strncpy_trim(port->firmware, raw, 8);

    if (id[83] & (1 << 10)) {  // LBA48
        port->sectors = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                        ((uint64_t)id[101] << 16) | (uint64_t)id[100];
    } else {
        port->sectors = ((uint32_t)id[61] << 16) | id[60];
    }
    port->sector_size = 512;

    port->supports_ncq = (id[76] & (1 << 8)) &&
                         g_ahci_controller && g_ahci_controller->supports_ncq;
    port->queue_depth = port->supports_ncq ? ((id[75] & 0x1F) + 1) : 1;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Sector read / write (ATA READ/WRITE DMA EXT)                              */
/*                                                                           */
/* These take a *physical* (identity-mapped) DMA buffer. The public blk_*    */
/* wrappers bounce through port->dma_bounce one sector at a time so callers  */
/* may pass any kernel pointer.                                              */
/* ------------------------------------------------------------------------- */

static bool ahci_rw_one(ahci_port_t* port, uint64_t lba, void* dma_buf, bool write) {
    int slot = ahci_port_alloc_slot(port);
    if (slot < 0) return false;

    ahci_cmd_header_t* hdr = &port->cmd_list[slot];
    hdr->cfl   = sizeof(fis_reg_h2d_t) / 4;
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

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
    fis->countl = 1;          // exactly one sector
    fis->counth = 0;

    tbl->prdt[0].dba = dma_phys(dma_buf);
    tbl->prdt[0].dbc = 512 - 1;
    tbl->prdt[0].i   = 0;

    wmb();
    ahci_port_issue_cmd(port, slot);
    bool ok = ahci_port_wait_cmd(port, slot, AHCI_TIMEOUT_MS);
    ahci_port_free_slot(port, slot);
    return ok;
}

bool ahci_read_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer) {
    if (!port || !port->device_present || count == 0) return false;
    // Overflow-safe range check: `lba + count` (uint64) wraps when lba is within
    // `count` of UINT64_MAX, making a wrapped-small sum pass the old `> sectors` test
    // and slip an out-of-range LBA past the guard. count>=1 here (count==0 rejected
    // above) and the first clause short-circuits before the subtraction, so
    // `sectors - count` never underflows.
    if (count > port->sectors || lba > port->sectors - count) return false;

    uint8_t* dst = (uint8_t*)buffer;
    for (uint32_t s = 0; s < count; s++) {
        if (!ahci_rw_one(port, lba + s, port->dma_bounce, /*write=*/false)) {
            if (g_ahci_controller) g_ahci_controller->total_errors++;
            return false;
        }
        memcpy(dst + s * 512, port->dma_bounce, 512);
        if (g_ahci_controller) g_ahci_controller->total_reads++;
    }
    return true;
}

bool ahci_write_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer) {
    if (!port || !port->device_present || count == 0) return false;
    // Overflow-safe range check: `lba + count` (uint64) wraps when lba is within
    // `count` of UINT64_MAX, making a wrapped-small sum pass the old `> sectors` test
    // and slip an out-of-range LBA past the guard. count>=1 here (count==0 rejected
    // above) and the first clause short-circuits before the subtraction, so
    // `sectors - count` never underflows.
    if (count > port->sectors || lba > port->sectors - count) return false;

    const uint8_t* src = (const uint8_t*)buffer;
    for (uint32_t s = 0; s < count; s++) {
        memcpy(port->dma_bounce, src + s * 512, 512);
        if (!ahci_rw_one(port, lba + s, port->dma_bounce, /*write=*/true)) {
            if (g_ahci_controller) g_ahci_controller->total_errors++;
            return false;
        }
        if (g_ahci_controller) g_ahci_controller->total_writes++;
    }
    return true;
}

/*
 * NCQ paths are not used by the public API; keep simple non-NCQ fallbacks so
 * the existing ahci_block.c adapter still links.
 */
bool ahci_read_sectors_ncq(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer) {
    return ahci_read_sectors(port, lba, count, buffer);
}
bool ahci_write_sectors_ncq(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer) {
    return ahci_write_sectors(port, lba, count, buffer);
}

bool ahci_flush_cache(ahci_port_t* port) {
    if (!port || !port->device_present) return false;
    int slot = ahci_port_alloc_slot(port);
    if (slot < 0) return false;

    ahci_cmd_header_t* hdr = &port->cmd_list[slot];
    hdr->cfl = sizeof(fis_reg_h2d_t) / 4;
    hdr->w = 0;
    hdr->prdtl = 0;
    hdr->prdbc = 0;

    ahci_cmd_table_t* tbl = port->cmd_tables[slot];
    memset((void*)tbl, 0, 256);
    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)tbl->cfis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_FLUSH_CACHE_EXT;
    fis->device = (1 << 6);

    wmb();
    ahci_port_issue_cmd(port, slot);
    bool ok = ahci_port_wait_cmd(port, slot, AHCI_TIMEOUT_MS * 5);
    ahci_port_free_slot(port, slot);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Interrupt helpers (polling driver — these are informational only)         */
/* ------------------------------------------------------------------------- */

void ahci_port_handle_interrupt(ahci_port_t* port) {
    uint32_t is = port->regs->is;
    port->regs->is = is;
    if (is & AHCI_PORT_INT_TFES) port->error_count++;
}

void ahci_handle_interrupt(ahci_controller_t* controller) {
    uint32_t is = controller->abar->is;
    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if ((is & (1u << i)) && controller->ports[i].device_present) {
            ahci_port_handle_interrupt(&controller->ports[i]);
        }
    }
    controller->abar->is = is;
}

/* ------------------------------------------------------------------------- */
/* Public init + block API                                                   */
/* ------------------------------------------------------------------------- */

int ahci_init(void) {
    kprintf("[AHCI] Initializing AHCI/SATA driver...\n");

    pci_device_t* dev = pci_find_class(PCI_CLASS_STORAGE_AHCI,
                                       PCI_SUBCLASS_AHCI,
                                       PCI_PROG_IF_AHCI);
    if (!dev) {
        kprintf("[AHCI] No AHCI controller found - check BIOS SATA mode settings (must be AHCI, not IDE)\n");
        return -1;
    }
    kprintf("[AHCI] Found controller %04x:%04x (bus %u dev %u fn %u)\n",
            dev->vendor_id, dev->device_id, dev->bus, dev->device, dev->function);

    g_ahci_controller = ahci_probe_controller(dev);
    if (!g_ahci_controller) {
        kprintf("[AHCI] Controller probe failed - memory allocation or BAR access error\n");
        return -2;
    }
    if (!ahci_init_controller(g_ahci_controller)) {
        kprintf("[AHCI] Controller initialization failed - reset or handoff error\n");
        return -3;
    }
    if (!g_blk_port || !g_blk_port->device_present) {
        kprintf("[AHCI] No usable SATA disk found - verify drive is connected and powered\n");
        return -4;
    }

    // Register the disk with the generic block layer as device 0.
    if (ahci_register_block_device(g_blk_port, g_blk_port->port_num)) {
        kprintf("[AHCI] Registered SATA disk as block device 0\n");
    }
    kprintf("[AHCI] AHCI driver initialized successfully\n");
    return 0;
}

ahci_port_t* blk_get_port(uint32_t dev) {
    return (dev == 0) ? g_blk_port : NULL;
}

int blk_read(uint32_t dev, uint64_t lba, uint32_t count, void* buf) {
    ahci_port_t* port = blk_get_port(dev);
    if (!port) return -1;
    return ahci_read_sectors(port, lba, count, buf) ? 0 : -2;
}

int blk_write(uint32_t dev, uint64_t lba, uint32_t count, const void* buf) {
    ahci_port_t* port = blk_get_port(dev);
    if (!port) return -1;
    return ahci_write_sectors(port, lba, count, buf) ? 0 : -2;
}

/* ------------------------------------------------------------------------- */
/* Flat syscall-facing API (used by SYS_BLK_READ=49 / SYS_BLK_WRITE=50)     */
/* ------------------------------------------------------------------------- */

/*
 * ahci_present() - returns 1 if a usable SATA drive was found, 0 otherwise.
 * Safe to call before ahci_init(); returns 0 in that case.
 */
int ahci_present(void) {
    return (g_blk_port && g_blk_port->device_present) ? 1 : 0;
}

/*
 * ahci_read() - read `count` 512-byte sectors starting at `lba` into `buf`.
 * `buf` may be any kernel virtual pointer (bounced through DMA scratch page).
 * Returns 0 on success, -1 if no drive is available, -2 on I/O error.
 *
 * Syscall handler body for SYS_BLK_READ (syscall number 49):
 *
 *   uint64_t lba   = (uint64_t)arg1;
 *   uint32_t count = (uint32_t)arg2;
 *   void*    ubuf  = (void*)arg3;
 *   if (!ahci_present()) return -ENODEV;
 *   size_t len = (size_t)count * 512;
 *   if (!validate_user_buffer(ubuf, len)) return -EFAULT;
 *   void* kbuf = kmalloc(len);
 *   if (!kbuf) return -ENOMEM;
 *   int r = ahci_read(lba, count, kbuf);
 *   if (r == 0 && copy_to_user(ubuf, kbuf, len) != COPY_SUCCESS) r = -EFAULT;
 *   kfree(kbuf);
 *   return r;
 */
int ahci_read(uint64_t lba, uint32_t count, void* buf) {
    return blk_read(0, lba, count, buf);
}

/*
 * ahci_write() - write `count` 512-byte sectors starting at `lba` from `buf`.
 * `buf` may be any kernel virtual pointer (bounced through DMA scratch page).
 * Returns 0 on success, -1 if no drive is available, -2 on I/O error.
 *
 * Syscall handler body for SYS_BLK_WRITE (syscall number 50):
 *
 *   uint64_t lba   = (uint64_t)arg1;
 *   uint32_t count = (uint32_t)arg2;
 *   const void* ubuf = (const void*)arg3;
 *   if (!ahci_present()) return -ENODEV;
 *   size_t len = (size_t)count * 512;
 *   if (!validate_user_buffer(ubuf, len)) return -EFAULT;
 *   void* kbuf = kmalloc(len);
 *   if (!kbuf) return -ENOMEM;
 *   int r = COPY_SUCCESS;
 *   if (copy_from_user(kbuf, ubuf, len) != COPY_SUCCESS) { kfree(kbuf); return -EFAULT; }
 *   r = ahci_write(lba, count, kbuf);
 *   kfree(kbuf);
 *   return r;
 */
int ahci_write(uint64_t lba, uint32_t count, const void* buf) {
    return blk_write(0, lba, count, buf);
}
