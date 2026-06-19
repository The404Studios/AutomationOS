/*
 * iwl-trans.h -- shared transport interface for the iwlwifi DVM bring-up bricks.
 * ============================================================================
 * The transport state (`struct iwl_trans`) and the low-level MMIO/PRPH/ring
 * primitives originally lived PRIVATE inside iwl-trans.c. The DVM firmware-load,
 * host-command, NVM and scan bricks (iwl-fw-load.c / iwl-hostcmd.c / iwl-nvm.c /
 * iwl-scan.c / iwl-ops.c) all need to reach `trans->mmio`, the rings, and the
 * register accessors -- so this header lifts that contract out of iwl-trans.c.
 *
 *   ===================  HELD FOR HARDWARE -- READ THIS  ===================
 *   None of this runs at boot. The ONLY entry is iwl_wifi_bringup() (iwl-ops.c),
 *   invoked by the future post-desktop trigger on the physical T410 -- never from
 *   any boot path. No QEMU emulates an iwlwifi card, so the whole tail is
 *   correct-by-review against Linux iwlwifi (kernel v5.10,
 *   drivers/net/wireless/intel/iwlwifi/), NOT test-covered. Every register/cmd
 *   value is cited line-by-line against upstream.
 *
 * INTEGRATION NOTE (reported to the parent, NOT done here):
 *   iwl-trans.c currently defines `struct iwl_trans` privately and marks its
 *   accessors `static`. To let the new bricks link against them, the parent must
 *   (1) move the `struct iwl_trans { ... }` definition out of iwl-trans.c into
 *   THIS header (it is reproduced below verbatim), (2) `#include "iwl-trans.h"`
 *   from iwl-trans.c, and (3) drop `static` from the handful of helpers the new
 *   bricks call (iwl_read32/iwl_write32, iwl_read_prph/iwl_write_prph,
 *   iwl_grab_nic_access/iwl_release_nic_access, iwl_poll_bit, iwl_desc_wmb,
 *   iwl_udelay_approx) so they match the `extern` prototypes here. The exact
 *   diff lines are in the brick report. Until then this header stands alone and
 *   the new .c files host-compile against the stubs in /tmp.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-trans.h
 */
#ifndef IWL_TRANS_H
#define IWL_TRANS_H

#include "types.h"
#include "iwl-csr.h"   /* RX_QUEUE_SIZE, register map */

/* ====================================================================== *
 *  struct iwl_trans -- the transport state. This is the EXACT struct from
 *  iwl-trans.c, extended with the few fields the firmware-load / host-command /
 *  RX-consumer bricks need (hw_rev for the EEPROM/OTP decision, the RX read
 *  cursor, and the MAC recovered from the NVM). The original five fields keep
 *  their order + types so iwl-trans.c is unaffected once it includes this.
 * ====================================================================== */
struct iwl_trans {
    volatile uint8_t* mmio;     /* BAR0 base (CSR/PRPH/FH register file) */

    void*    cmd_ring;          /* TFD command-queue ring (one page) */
    uint64_t cmd_ring_dma;      /* its physical/DMA address (== virt) */

    void*    rx_bd;             /* RX buffer-descriptor (RBD) ring (one page) */
    uint64_t rx_bd_dma;
    void*    rx_bufs[RX_QUEUE_SIZE];   /* the RX DMA pages the RBDs point at */
    void*    rb_status;         /* shared RB-status writeback block (one page) */
    uint64_t rb_status_dma;

    /* ---- extensions used by the DVM bring-up bricks (zeroed by the caller) ---- */
    uint32_t hw_rev;            /* CSR_HW_REV, read by IWL-IDENT (EEPROM/OTP pick) */
    uint32_t cmd_wr_ptr;        /* next free cmd-queue TFD slot (0..TFD_QUEUE_SIZE_MAX-1) */
    uint32_t rx_read;           /* host RX read cursor into the RBD ring */
    uint8_t  mac[6];            /* MAC recovered by iwl_read_nvm() */
    int      is_otp;            /* 1 if NVM is OTP, 0 if EEPROM (set by iwl_read_nvm) */

    /* ---- TX-scheduler (SCD) state for the command queue (iwl_scd_cmd_queue_init).
     * The scd byte-count table the device DMAs from, the firmware-reported SCD
     * base address, and a one-shot "already brought up" guard. ---- */
    void*    scd_bc_tbl;        /* DMA page holding the cmd-queue byte-count table */
    uint64_t scd_bc_tbl_dma;    /* its physical/DMA address (== virt) */
    uint32_t scd_base_addr;     /* device-internal SCD base (read from SCD_SRAM_BASE_ADDR) */
    int      scd_ready;         /* 1 once iwl_scd_cmd_queue_init has run */
};

/* ====================================================================== *
 *  Transport primitives. Defined in iwl-trans.c (de-static'd at integration).
 *  Bounded, marker-printing, abort-clean -- see iwl-trans.c for the contract.
 * ====================================================================== */
void     iwl_write32(struct iwl_trans* trans, uint32_t off, uint32_t val);
uint32_t iwl_read32(struct iwl_trans* trans, uint32_t off);
void     iwl_set_bit(struct iwl_trans* trans, uint32_t off, uint32_t mask);
void     iwl_clear_bit(struct iwl_trans* trans, uint32_t off, uint32_t mask);
void     iwl_write_prph(struct iwl_trans* trans, uint32_t addr, uint32_t val);
uint32_t iwl_read_prph(struct iwl_trans* trans, uint32_t addr);
void     iwl_set_bits_prph(struct iwl_trans* trans, uint32_t addr, uint32_t mask);
int      iwl_grab_nic_access(struct iwl_trans* trans);
void     iwl_release_nic_access(struct iwl_trans* trans);
int      iwl_poll_bit(struct iwl_trans* trans, uint32_t off,
                      uint32_t bits, uint32_t mask);
void     iwl_desc_wmb(void);
void     iwl_udelay_approx(volatile uint32_t loops);

/* The held transport entry (iwl-trans.c). Allocates rings + powers the APM. */
int      iwl_trans_bringup(struct iwl_trans* trans);

#endif /* IWL_TRANS_H */
