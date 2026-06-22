/*
 * iwl-trans.c -- Intel iwlwifi TRANSPORT layer (IWL-TRANS).
 * ========================================================================
 * Brick 3 of the real Intel WiFi driver. The transport is the bottom half that
 * brings the radio's silicon to life *before* any firmware runs: it powers up
 * the Advanced Power Management (APM) block, takes "NIC access" (the clock/sleep
 * handshake that lets the host reach device-internal memory), reaches the
 * periphery (PRPH) clock/power registers, and allocates + programs the command
 * (TFD) DMA ring and the RX buffer-descriptor (RBD) DMA ring. Once this brick
 * succeeds the radio is ready for IWL-LOAD to copy the parsed .ucode (IWL-FW)
 * into its SRAM and start the embedded processor.
 *
 * ===================  HELD FOR HARDWARE -- READ THIS  ===================
 * Nothing in this file runs at boot. There is exactly ONE entry point,
 * iwl_trans_bringup(), and it is NOT called from iwl_init() (iwl-pci.c) nor any
 * boot path. No QEMU emulates an iwlwifi card, so this code has ZERO automated
 * coverage; running un-validated radio power-up on the real T410 can wedge the
 * PCH bus and hang the machine. The future post-desktop trigger (wired by the
 * parent brick, on the physical T410, across many flash->boot cycles) is what
 * calls iwl_trans_bringup() -- we only WRITE it here, correct-by-review against
 * Linux iwlwifi. The gate here is "compiles under -DIWLWIFI + matches upstream",
 * not "passes a test".
 *
 * Safety laws this file obeys (mirroring the e1000 PCH deferred-bringup model):
 *   1. Never called at boot (only iwl_trans_bringup, never invoked here).
 *   2. Every hardware poll is ITERATION-CAPPED (IWL_TRANS_POLL_MAX), never a
 *      bare while() and never tick-based -- the PIT may be frozen, so a wall-
 *      clock timeout would never fire. A bus stall becomes a bounded fail.
 *   3. A kprintf SERIAL MARKER is printed immediately BEFORE every risky MMIO
 *      write/poll, so on a real T410 the last marker before a stall pinpoints
 *      exactly where the silicon died.
 *   4. On any timeout we kprintf a clean failure and return -1 (abort-clean).
 *      Never panic.
 *
 * Register values + the apm_init / grab_nic_access sequence are cited line-by-
 * line against the Linux iwlwifi sources in iwl-csr.h and in the comments below
 * (drivers/net/wireless/intel/iwlwifi/, kernel v5.10):
 *     pcie/trans.c   -- iwl_pcie_prepare_card_hw / iwl_pcie_set_hw_ready (the
 *                       NIC_READY/PREPARE ownership handshake, before APM),
 *                       iwl_pcie_apm_init, iwl_pcie_apm_config,
 *                       iwl_trans_pcie_grab_nic_access, iwl_pcie_set_pwr,
 *                       iwl_trans_pcie_{read,write}_prph
 *     iwl-io.c       -- iwl_finish_nic_init (the MAC_CLOCK_READY poll), iwl_poll_bit
 *     pcie/rx.c      -- iwl_pcie_rx_hw_init (the RBD-base/WPTR/config programming)
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-trans.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf, PAGE_SIZE */
#include "mem.h"             /* pmm_alloc_page / pmm_alloc_pages (phys==virt) */
#include "iwl-csr.h"
#include "iwl-trans.h"       /* struct iwl_trans + the (now external) accessor protos --
                             * shared with the DVM LOAD/NVM/SCAN/OPS bricks */
#include "iwl-dvm-commands.h" /* IWL_CMD_QUEUE_ID -- the cmd queue the FW services */

/*
 * sizeof(struct iwl_tfd) on the gen1 parts the T410 carries (1000/5000/6000).
 * Linux iwl-fh.h: __reserved1[3] + num_tbs + tbs[IWL_NUM_OF_TBS=20] + __pad[4]
 * = 128 bytes; upstream comment "For pre-22000 HW it is 256 x 128 = 32 KBytes".
 * The cmd-ring DMA window is therefore TFD_QUEUE_SIZE_MAX * 128 = 32 KB = 8
 * pages -- NOT one. (Review fix: the ring was allocated 8x too small.)
 */
#define IWL_TFD_SIZE  128

/* struct iwl_trans now lives in iwl-trans.h (lifted out so the DVM bring-up
 * bricks can reach trans->mmio + the rings). The original five fields keep their
 * order/types; the header adds hw_rev/cmd_wr_ptr/rx_read/mac[6]/is_otp for those
 * bricks. iwl-trans.c is unaffected -- it only touches the original fields. */

/* ====================================================================== *
 *  MMIO accessors over BAR0. Volatile 32-bit reads/writes at the identity-
 *  mapped base -- same model as e1000.c's mmio_{read,write}32.
 * ====================================================================== */
void iwl_write32(struct iwl_trans* trans, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(trans->mmio + off) = val;
}
uint32_t iwl_read32(struct iwl_trans* trans, uint32_t off) {
    return *(volatile uint32_t*)(trans->mmio + off);
}

/* Read-modify-write helpers (Linux iwl_set_bit / iwl_clear_bit). */
void iwl_set_bit(struct iwl_trans* trans, uint32_t off, uint32_t mask) {
    iwl_write32(trans, off, iwl_read32(trans, off) | mask);
}
void iwl_clear_bit(struct iwl_trans* trans, uint32_t off, uint32_t mask) {
    iwl_write32(trans, off, iwl_read32(trans, off) & ~mask);
}

/*
 * Descriptor write barrier: flush descriptor/ring writes to memory before the
 * doorbell register write that hands the ring to the DMA engine. Mirrors
 * e1000.c desc_wmb() -- sfence + compiler barrier (see that file for the WC/UC
 * ordering rationale).
 */
void iwl_desc_wmb(void) {
    asm volatile("sfence" ::: "memory");
}

/*
 * Crude bounded delay. The PIT is not guaranteed live when the future trigger
 * runs, so we spin on `pause` for a few microseconds of settle time rather than
 * waiting on ticks -- same reasoning as e1000_delay().
 */
void iwl_udelay_approx(volatile uint32_t loops) {
    while (loops--)
        asm volatile("pause");
}

/* ====================================================================== *
 *  Bounded poll. Linux iwl_poll_bit() loops until (read & mask) == (bits & mask)
 *  with a microsecond timeout; we translate that into a HARD iteration cap so a
 *  stalled bus can never hang. Returns 0 on match, -1 on timeout.
 *  (Linux iwl-io.c iwl_poll_bit + IWL_POLL_INTERVAL = 10us.)
 * ====================================================================== */
int iwl_poll_bit(struct iwl_trans* trans, uint32_t off,
                 uint32_t bits, uint32_t mask) {
    for (int i = 0; i < IWL_TRANS_POLL_MAX; i++) {
        if ((iwl_read32(trans, off) & mask) == (bits & mask))
            return 0;
        iwl_udelay_approx(64);   /* ~ a few us per iter; bounded total */
    }
    return -1;
}

/* ====================================================================== *
 *  PRPH (periphery) indirect access via HBUS_TARG_PRPH_*.
 *  Linux trans.c iwl_trans_pcie_{write,read}_prph: program
 *  ((addr & mask) | (3 << 24)) into *_WADDR/*_RADDR, then write/read *_WDAT/
 *  *_RDAT. `mask` = 0x000FFFFF on the gen1 parts the T410 carries.
 *  These touch device-internal silicon, so they print a marker and must run
 *  only while NIC access is held (the caller grabs it).
 * ====================================================================== */
void iwl_write_prph(struct iwl_trans* trans, uint32_t addr, uint32_t val) {
    iwl_write32(trans, HBUS_TARG_PRPH_WADDR,
                (addr & IWL_PRPH_ADDR_MASK) | IWL_PRPH_BURST_4B);
    iwl_write32(trans, HBUS_TARG_PRPH_WDAT, val);
}

uint32_t iwl_read_prph(struct iwl_trans* trans, uint32_t addr) {
    iwl_write32(trans, HBUS_TARG_PRPH_RADDR,
                (addr & IWL_PRPH_ADDR_MASK) | IWL_PRPH_BURST_4B);
    return iwl_read32(trans, HBUS_TARG_PRPH_RDAT);
}

/* PRPH read-modify-write (Linux iwl_set_bits_prph). Caller holds NIC access. */
void iwl_set_bits_prph(struct iwl_trans* trans, uint32_t addr, uint32_t mask) {
    iwl_write_prph(trans, addr, iwl_read_prph(trans, addr) | mask);
}

/* ====================================================================== *
 *  iwl_grab_nic_access -- the clock/sleep handshake that lets the host reach
 *  device-internal resources (PRPH + SRAM).
 *  Linux trans.c iwl_trans_pcie_grab_nic_access:
 *     set CSR_GP_CNTRL.MAC_ACCESS_REQ, then poll CSR_GP_CNTRL for
 *     MAC_ACCESS_EN under the mask (MAC_CLOCK_READY | GOING_TO_SLEEP), 15000us.
 *  Returns 0 on grant, -1 on timeout (abort-clean).
 * ====================================================================== */
int iwl_grab_nic_access(struct iwl_trans* trans) {
    kprintf("IWLTRANS: grab nic access (set MAC_ACCESS_REQ)...\n");
    iwl_set_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);

    /* Poll for the device to report it is awake (MAC_ACCESS_EN) while not
     * mid-sleep transition. Bounded -- a stalled bus fails, never hangs. */
    int rc = iwl_poll_bit(trans, CSR_GP_CNTRL,
                          CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN,
                          (CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY |
                           CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP));
    if (rc != 0) {
        uint32_t v = iwl_read32(trans, CSR_GP_CNTRL);
        kprintf("IWLTRANS: grab nic access TIMEOUT (CSR_GP_CNTRL=0x%08x) -- abort\n", v);
        return -1;
    }
    return 0;
}

/* Linux trans.c: clear CSR_GP_CNTRL.MAC_ACCESS_REQ to release. */
void iwl_release_nic_access(struct iwl_trans* trans) {
    iwl_clear_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

/* ====================================================================== *
 *  iwl_is_rfkill -- read the live hardware RF-kill switch state.
 *  Linux iwl_is_rfkill_set: CSR_GP_CNTRL bit 27 (HW_RF_KILL_SW) reads SET when
 *  the radio is DISABLED -- the physical wireless slider on the T410 front edge,
 *  or a BIOS "WLAN disabled" setting. An asserted RF-kill makes the firmware
 *  drop everything silently: scan returns zero SSIDs with no other error. So we
 *  read + report it at bring-up and before every scan. Caches into trans->rf_kill.
 * ====================================================================== */
int iwl_is_rfkill(struct iwl_trans* trans) {
    if (!trans || !trans->mmio) return 0;
    uint32_t gp = iwl_read32(trans, CSR_GP_CNTRL);
    int killed = (gp & CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) ? 1 : 0;
    trans->rf_kill = killed;
    return killed;
}

/* ====================================================================== *
 *  iwl_prepare_card_hw -- take ownership of the device from the Management
 *  Engine / BIOS, which MUST happen before APM power-up.
 *  Linux pcie/trans.c iwl_pcie_prepare_card_hw -> iwl_pcie_set_hw_ready:
 *     set CSR_HW_IF_CONFIG_REG_BIT_NIC_READY (PCI_OWN_SEM) and poll it; if it
 *     does not come back, pulse CSR_HW_IF_CONFIG_REG_PREPARE (WAKE_ME) and retry,
 *     up to ~10 times. On a real T410 the ME can still own the card at trigger
 *     time -- without this handshake the later INIT_DONE/MAC_CLOCK_READY poll in
 *     APM can never complete (or the APMG PRPH writes go nowhere).
 *  (Review fix: this mandatory ownership step was missing; the NIC_READY/PREPARE
 *  bits were defined in iwl-csr.h but never used -- the tell.)
 *  Bounded throughout; marker before each MMIO; returns 0 on ready, -1 on fail.
 * ====================================================================== */
#define IWL_PREPARE_HW_TRIES 10
static int iwl_set_hw_ready(struct iwl_trans* trans) {
    iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
    /* Bounded poll for the device to echo NIC_READY back (it took the semaphore). */
    return iwl_poll_bit(trans, CSR_HW_IF_CONFIG_REG,
                        CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
                        CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
}
static int iwl_prepare_card_hw(struct iwl_trans* trans) {
    kprintf("IWLTRANS: prepare card HW (take ownership from ME/BIOS)...\n");
    if (iwl_set_hw_ready(trans) == 0) {
        kprintf("IWLTRANS: NIC_READY (semaphore already granted)\n");
        return 0;
    }
    /* Not ready: pulse PREPARE/WAKE_ME and retry a BOUNDED number of times. */
    for (int t = 0; t < IWL_PREPARE_HW_TRIES; t++) {
        kprintf("IWLTRANS: pulse PREPARE/WAKE_ME (try %d/%d)...\n", t + 1, IWL_PREPARE_HW_TRIES);
        iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PREPARE);
        iwl_udelay_approx(20000);   /* ~1ms settle between tries (Linux: 200us poll) */
        if (iwl_set_hw_ready(trans) == 0) {
            kprintf("IWLTRANS: NIC_READY after %d prepare pulse(s)\n", t + 1);
            return 0;
        }
    }
    uint32_t v = iwl_read32(trans, CSR_HW_IF_CONFIG_REG);
    kprintf("IWLTRANS: prepare card HW TIMEOUT (CSR_HW_IF_CONFIG_REG=0x%08x) -- "
            "ME still owns the card; abort (safe to re-run)\n", v);
    return -1;
}

/* ====================================================================== *
 *  iwl_apm_init -- power up the radio's basic functions (no uCode yet).
 *  Faithful to Linux trans.c iwl_pcie_apm_init (+ iwl_pcie_apm_config and
 *  iwl_finish_nic_init for the gen1 parts the T410 carries):
 *     1. CSR_GIO_CHICKEN_BITS: DIS_L0S_EXIT_TIMER + L1A_NO_L0S_RX.
 *     2. CSR_DBG_HPET_MEM_REG = HPET_MEM_REG_VAL (FH wait-threshold W/A).
 *     3. CSR_HW_IF_CONFIG_REG: BIT_HAP_WAKE_L1A (HAP INTA wake).
 *     4. CSR_GIO_REG: L0S_DISABLED (apm_config).
 *     5. CSR_GP_CNTRL: INIT_DONE, then POLL MAC_CLOCK_READY (25000us upstream).
 *     6. grab NIC access, then via PRPH: APMG_CLK_EN_REG = DMA_CLK_RQT, settle,
 *        APMG_PCIDEV_STT_REG |= L1_ACT_DIS, APMG_RTC_INT_STT_REG = RFKILL clear,
 *        release access.
 *  Every risky step has a marker; the one poll is bounded. Returns 0 / -1.
 * ====================================================================== */
static int iwl_apm_init(struct iwl_trans* trans) {
    kprintf("IWLTRANS: APM init...\n");

    /* (1) Disable L0S exit timer + don't wait for ICH L0s (chicken bits). */
    kprintf("IWLTRANS: APM set GIO chicken bits (L0S work-arounds)...\n");
    iwl_set_bit(trans, CSR_GIO_CHICKEN_BITS,
                CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER);
    iwl_set_bit(trans, CSR_GIO_CHICKEN_BITS,
                CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);

    /* (2) Set FH wait threshold to maximum (HW error during stress W/A). */
    kprintf("IWLTRANS: APM set FH wait threshold (CSR_DBG_HPET_MEM_REG)...\n");
    iwl_set_bit(trans, CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);

    /* (3) Enable HAP INTA to wake the PCIe link (CSR_HW_IF_CONFIG_REG). */
    kprintf("IWLTRANS: APM enable HAP INTA wake (CSR_HW_IF_CONFIG_REG)...\n");
    iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
                CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);

    /* (4) Always disable L0S (apm_config). */
    kprintf("IWLTRANS: APM disable L0S (CSR_GIO_REG)...\n");
    iwl_set_bit(trans, CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);

    /* (4b) Analog PLL config -- ONLY the 1000 + 5000 families need it (Linux
     * iwl_pcie_apm_init: if base_params->pll_cfg). The 6000 series must NOT get
     * this write. trans->pll_cfg is set per-family in iwl-ops.c. (Audit #7, the
     * one genuine APM item -- gated so it never harms a 6000 card.) */
    if (trans->pll_cfg) {
        kprintf("IWLTRANS: APM set ANA_PLL_CFG (1000/5000 family)...\n");
        iwl_set_bit(trans, CSR_ANA_PLL_CFG, CSR50_ANA_PLL_CFG_VAL);
    }

    /* (5) Move adapter D0U* -> D0A* (INIT_DONE) then wait for clock to stabilize.
     * Linux iwl_finish_nic_init polls MAC_CLOCK_READY with a 25000us timeout;
     * here that is the bounded iwl_poll_bit. */
    kprintf("IWLTRANS: APM set INIT_DONE (CSR_GP_CNTRL)...\n");
    iwl_set_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);

    kprintf("IWLTRANS: APM poll MAC_CLOCK_READY (bounded)...\n");
    if (iwl_poll_bit(trans, CSR_GP_CNTRL,
                     CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                     CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY) != 0) {
        uint32_t v = iwl_read32(trans, CSR_GP_CNTRL);
        kprintf("IWLTRANS: APM MAC_CLOCK_READY TIMEOUT (CSR_GP_CNTRL=0x%08x) -- abort\n", v);
        return -1;
    }

    /* (6) Program the APMG clock/power registers (PRPH). These reach device-
     * internal silicon, so we must hold NIC access for the whole sequence. */
    if (iwl_grab_nic_access(trans) != 0) {
        kprintf("IWLTRANS: APM could not grab NIC access for APMG -- abort\n");
        return -1;
    }

    kprintf("IWLTRANS: APM enable DMA clock (PRPH APMG_CLK_EN_REG)...\n");
    iwl_write_prph(trans, APMG_CLK_EN_REG, APMG_CLK_VAL_DMA_CLK_RQT);
    iwl_udelay_approx(2000);   /* Linux udelay(20) -- let the clock stabilize */

    kprintf("IWLTRANS: APM disable L1-Active (PRPH APMG_PCIDEV_STT_REG)...\n");
    iwl_set_bits_prph(trans, APMG_PCIDEV_STT_REG, APMG_PCIDEV_STT_VAL_L1_ACT_DIS);

    kprintf("IWLTRANS: APM clear RFKILL int (PRPH APMG_RTC_INT_STT_REG)...\n");
    iwl_write_prph(trans, APMG_RTC_INT_STT_REG, APMG_RTC_INT_STT_RFKILL);

    iwl_release_nic_access(trans);

    kprintf("IWLTRANS: APM init OK\n");
    return 0;
}

/* ====================================================================== *
 *  iwl_alloc_cmd_ring -- allocate + register the command-queue TFD ring.
 *  The host writes host-commands as TFDs into this DRAM ring and rings the
 *  doorbell (HBUS_TARG_WRPTR) to hand them to the device. We allocate one
 *  identity-mapped page (phys==virt, like e1000), zero it, and program its
 *  base into the command queue's circular-buffer-base register.
 *  Linux: FH_MEM_CBBC_QUEUE(txq_id) holds (dma_addr >> 8); the cmd queue is
 *  the last txq. We program channel 0's slot here as the brick's single ring.
 * ====================================================================== */
static int iwl_alloc_cmd_ring(struct iwl_trans* trans) {
    kprintf("IWLTRANS: alloc cmd ring (TFD)...\n");

    /* The ring is TFD_QUEUE_SIZE_MAX descriptors x IWL_TFD_SIZE(128) = 32 KB = 8
     * contiguous pages. Allocating one page (the old bug) would point the DMA
     * engine (FH_MEM_CBBC_QUEUE[0] = base >> 8) at a 32 KB window backed by only
     * 4 KB; the device walking slots > 31 would read/write adjacent physical RAM
     * the allocator handed elsewhere -- silent corruption on real hardware. */
    uint32_t ring_bytes = (uint32_t)TFD_QUEUE_SIZE_MAX * IWL_TFD_SIZE;   /* 32768 */
    uint32_t ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;      /* 8 */
    void* ring = pmm_alloc_pages(ring_pages);   /* contiguous; phys==virt */
    if (!ring) {
        kprintf("IWLTRANS: cmd ring alloc FAILED (%u pages) -- abort\n", ring_pages);
        return -1;
    }
    for (uint32_t i = 0; i < ring_bytes; i++)
        ((volatile uint8_t*)ring)[i] = 0;

    trans->cmd_ring     = ring;
    trans->cmd_ring_dma = (uint64_t)(uintptr_t)ring;   /* phys == virt */

    /* Program the circular-buffer base (device wants the address >> 8). The
     * register write reaches the FH, so it needs NIC access held. */
    if (iwl_grab_nic_access(trans) != 0) {
        kprintf("IWLTRANS: cmd ring could not grab NIC access -- abort\n");
        return -1;
    }
    kprintf("IWLTRANS: program cmd ring base (FH_MEM_CBBC_QUEUE[%d])...\n",
            IWL_CMD_QUEUE_ID);
    iwl_desc_wmb();   /* flush the zeroed ring before the device can see the base */
    iwl_write32(trans, FH_MEM_CBBC_QUEUE(IWL_CMD_QUEUE_ID),
                (uint32_t)(trans->cmd_ring_dma >> 8));

    /* Reset the queue write pointer doorbell to the empty-ring value. Linux
     * encodes HBUS_TARG_WRPTR as (ssn & 0xff) | (txq_id << 8); DVM firmware
     * services host commands on queue IWL_CMD_QUEUE_ID (4), NOT queue 0 -- the
     * CBBC base slot and this doorbell must target that queue or the device never
     * sees the ring. (Review fix H-C1c.) */
    kprintf("IWLTRANS: reset cmd ring write ptr (HBUS_TARG_WRPTR, q=%d)...\n",
            IWL_CMD_QUEUE_ID);
    iwl_desc_wmb();
    iwl_write32(trans, HBUS_TARG_WRPTR, (0u & 0xff) | (IWL_CMD_QUEUE_ID << 8));

    iwl_release_nic_access(trans);
    /* NOTE: the ring is allocated + based, but the Scheduler byte-count table
     * (SCD_DRAM_BASE_ADDR) and the TX channel TCSR config are NOT programmed here
     * -- those are deferred to IWL-OPS. uCode LOAD uses a direct SRAM copy (no
     * SCD), so this is sufficient for IWL-LOAD; carrying a host command to a
     * *running* uCode additionally needs the SCD setup OPS will add. */
    kprintf("IWLTRANS: cmd ring ready (%d TFD slots, 8 pages; SCD bc-table deferred to OPS)\n",
            TFD_QUEUE_SIZE_MAX);
    return 0;
}

/* ====================================================================== *
 *  iwl_alloc_rx_ring -- allocate + program the RX buffer-descriptor ring.
 *  Layout (Linux pcie/rx.c iwl_pcie_rx_hw_init, gen1 path):
 *    - one page for the RBD ring (RX_QUEUE_SIZE 32-bit descriptors),
 *    - one DMA page per RBD (the device DMAs received frames into these),
 *    - one page for the shared RB-status writeback block,
 *  then program:
 *    FH_RSCSR_CHNL0_RBDCB_BASE_REG = rbd_dma >> 8
 *    FH_RSCSR_CHNL0_STTS_WPTR_REG  = rb_status_dma >> 4
 *    FH_RSCSR_CHNL0_WPTR           = the filled write index (doorbell)
 *    FH_MEM_RCSR_CHNL0_CONFIG_REG  = EN | IGNORE_RXF_EMPTY | IRQ_DEST_HOST |
 *                                    RB_SIZE_4K | (RB_TIMEOUT<<RBTH_POS) |
 *                                    (RX_QUEUE_SIZE_LOG<<RBDCB_SIZE_POS)
 *  All FH writes reach the device, so NIC access is held throughout.
 * ====================================================================== */
static int iwl_alloc_rx_ring(struct iwl_trans* trans) {
    kprintf("IWLTRANS: alloc rx ring (RBD + buffers + RB-status)...\n");

    /* RBD ring: RX_QUEUE_SIZE 32-bit entries (1024 bytes) fit in one page. */
    void* bd = pmm_alloc_page();
    if (!bd) {
        kprintf("IWLTRANS: rx RBD ring alloc FAILED -- abort\n");
        return -1;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        ((volatile uint8_t*)bd)[i] = 0;
    trans->rx_bd     = bd;
    trans->rx_bd_dma = (uint64_t)(uintptr_t)bd;

    /* Shared RB-status writeback block (device reports completed RBDs here). */
    void* rbs = pmm_alloc_page();
    if (!rbs) {
        kprintf("IWLTRANS: rx RB-status alloc FAILED -- abort\n");
        return -1;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        ((volatile uint8_t*)rbs)[i] = 0;
    trans->rb_status     = rbs;
    trans->rb_status_dma = (uint64_t)(uintptr_t)rbs;

    /* Per-RBD DMA pages. The RBD entry carries the page address >> 8 (the
     * device's 256-byte-granular DMA pointer encoding). */
    volatile uint32_t* rbd = (volatile uint32_t*)bd;
    for (uint32_t i = 0; i < RX_QUEUE_SIZE; i++) {
        void* buf = pmm_alloc_page();
        if (!buf) {
            kprintf("IWLTRANS: rx buffer %u alloc FAILED -- abort\n", i);
            return -1;
        }
        trans->rx_bufs[i] = buf;
        rbd[i] = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 8);   /* phys == virt */
    }

    /* Program the FH RX-ring registers (all need NIC access held). */
    if (iwl_grab_nic_access(trans) != 0) {
        kprintf("IWLTRANS: rx ring could not grab NIC access -- abort\n");
        return -1;
    }

    kprintf("IWLTRANS: stop rx DMA (FH_MEM_RCSR_CHNL0_CONFIG_REG=0)...\n");
    iwl_write32(trans, FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);

    kprintf("IWLTRANS: program RBD base (FH_RSCSR_CHNL0_RBDCB_BASE_REG)...\n");
    iwl_desc_wmb();   /* flush RBD ring + buffers before handing them to DMA */
    iwl_write32(trans, FH_RSCSR_CHNL0_RBDCB_BASE_REG,
                (uint32_t)(trans->rx_bd_dma >> 8));

    kprintf("IWLTRANS: program RB-status base (FH_RSCSR_CHNL0_STTS_WPTR_REG)...\n");
    iwl_write32(trans, FH_RSCSR_CHNL0_STTS_WPTR_REG,
                (uint32_t)(trans->rb_status_dma >> 4));

    kprintf("IWLTRANS: reset RBD read ptr (FH_RSCSR_CHNL0_RDPTR)...\n");
    iwl_write32(trans, FH_RSCSR_CHNL0_RDPTR, 0);

    kprintf("IWLTRANS: enable rx DMA (FH_MEM_RCSR_CHNL0_CONFIG_REG)...\n");
    iwl_write32(trans, FH_MEM_RCSR_CHNL0_CONFIG_REG,
                FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL |
                FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY |
                FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL |
                FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K |
                (RX_RB_TIMEOUT << FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS) |
                (RX_QUEUE_SIZE_LOG << FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS));

    /* Ring the RX write-pointer doorbell: hand the RBDs to the device. The HW
     * requires the write index be a multiple of 8 (Linux iwl_pcie_rxq_inc_wr_ptr
     * writes round_down(write, 8)); 255 is not 8-aligned, so mask the low 3 bits
     * -> 248. (Review fix: was RX_QUEUE_SIZE-1 = 255, mis-aligned.) */
    kprintf("IWLTRANS: ring RX write ptr doorbell (FH_RSCSR_CHNL0_WPTR)...\n");
    iwl_desc_wmb();
    iwl_write32(trans, FH_RSCSR_CHNL0_WPTR, (RX_QUEUE_SIZE - 1) & ~7u);

    iwl_release_nic_access(trans);
    kprintf("IWLTRANS: rx ring ready (%d RBDs)\n", RX_QUEUE_SIZE);
    return 0;
}

/* ====================================================================== *
 *  iwl_trans_bringup -- THE HELD ENTRY POINT.
 *  Called ONLY by the future post-desktop trigger on the real T410 (wired by a
 *  separate parent brick). NOT invoked from boot, iwl_init(), or anywhere in
 *  this tree. Requires trans->mmio to already point at the BAR0 mapped by
 *  IWL-IDENT. Returns 0 on success (rings allocated, radio ready for IWL-LOAD),
 *  -1 on any clean failure.
 * ====================================================================== */
int iwl_trans_bringup(struct iwl_trans* trans) {
    kprintf("IWLTRANS: bring-up START (real T410 only)\n");

    if (!trans || !trans->mmio) {
        kprintf("IWLTRANS: no BAR0 mapping -- abort (IWL-IDENT must map BAR0 first)\n");
        return -1;
    }

    /* Take ownership from the ME/BIOS FIRST -- APM power-up assumes the host owns
     * the device (Linux _iwl_trans_pcie_start_hw: prepare_card_hw before apm_init). */
    if (iwl_prepare_card_hw(trans) != 0) {
        kprintf("IWLTRANS: bring-up FAILED at prepare-card-hw (NIC ownership)\n");
        return -1;
    }

    if (iwl_apm_init(trans) != 0) {
        kprintf("IWLTRANS: bring-up FAILED at APM init\n");
        return -1;
    }

    /* Report HW RF-kill now that the APM is up + CSR_GP_CNTRL is meaningful. We
     * do NOT abort on RF-kill (the rest of bring-up is harmless and the user may
     * flip the switch), but a prominent marker means a later empty scan is
     * immediately explained. */
    if (iwl_is_rfkill(trans)) {
        kprintf("IWLTRANS: *** HW RF-KILL ASSERTED *** radio is DISABLED -- "
                "flip the wireless switch (T410 front-left slider) or enable WLAN "
                "in BIOS; scan will find NOTHING until then\n");
    } else {
        kprintf("IWLTRANS: RF-kill clear (radio enabled)\n");
    }

    if (iwl_alloc_cmd_ring(trans) != 0) {
        kprintf("IWLTRANS: bring-up FAILED at cmd ring alloc\n");
        return -1;
    }

    if (iwl_alloc_rx_ring(trans) != 0) {
        kprintf("IWLTRANS: bring-up FAILED at rx ring alloc\n");
        return -1;
    }

    kprintf("IWLTRANS: transport ready (rings allocated; uCode load is IWL-LOAD)\n");
    return 0;
}
