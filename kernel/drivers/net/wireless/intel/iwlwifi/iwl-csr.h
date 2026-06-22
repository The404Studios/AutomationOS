/*
 * iwl-csr.h -- Intel iwlwifi CSR / PRPH / FH register map used by IWL-TRANS.
 * =========================================================================
 * Brick 3 of the real Intel WiFi driver (the transport layer). This header is
 * the register dictionary IWL-TRANS (iwl-trans.c) uses to power up the radio
 * (APM init), grab NIC access, reach the device-internal periphery (PRPH), and
 * program the command/RX DMA rings.
 *
 * EVERY value below is copied verbatim from the Linux iwlwifi sources (kernel
 * v5.10 unless noted); the citing comment names the exact file + symbol so a
 * reviewer can diff this against upstream without guessing. NOTHING here is an
 * invented value -- the whole brick is "correct-by-review against Linux" since
 * no QEMU emulates these cards and it must never be tested on hardware blind.
 *
 *   Sources (drivers/net/wireless/intel/iwlwifi/):
 *     iwl-csr.h   -- CSR_* offsets/bits, HBUS_TARG_* indirection, CSR_GIO/RESET
 *     iwl-prph.h  -- APMG_* periphery registers + bits
 *     iwl-fh.h    -- FH_* flow-handler RX-ring + command-queue (CBBC/TCSR) regs
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-csr.h
 */
#ifndef IWL_CSR_H
#define IWL_CSR_H

/* ====================================================================== *
 *  CSR registers -- the directly-mapped BAR0 register file.
 *  iwl-csr.h: #define CSR_BASE (0x000); each reg = CSR_BASE + offset.
 * ====================================================================== */
#define CSR_BASE                  0x000

#define CSR_HW_IF_CONFIG_REG      (CSR_BASE + 0x000)  /* iwl-csr.h: hardware interface config */
#define CSR_INT_COALESCING        (CSR_BASE + 0x004)  /* iwl-csr.h: accumulated ints, 32-usec units */
#define CSR_INT                   (CSR_BASE + 0x008)  /* iwl-csr.h: host interrupt status/ack */
#define CSR_INT_MASK              (CSR_BASE + 0x00c)  /* iwl-csr.h: host interrupt enable */
#define CSR_FH_INT_STATUS         (CSR_BASE + 0x010)  /* iwl-csr.h: busmaster int status/ack */
#define CSR_RESET                 (CSR_BASE + 0x020)  /* iwl-csr.h: busmaster enable, NMI, etc */
#define CSR_GP_CNTRL              (CSR_BASE + 0x024)  /* iwl-csr.h: general purpose control */
#define CSR_HW_REV                (CSR_BASE + 0x028)  /* iwl-csr.h: hardware revision (read-only) */
#define CSR_EEPROM_REG            (CSR_BASE + 0x02c)  /* iwl-csr.h: EEPROM access */
#define CSR_GIO_REG               (CSR_BASE + 0x03c)  /* iwl-csr.h: GIO chicken bits (L0S disable) */
#define CSR_ANA_PLL_CFG           (CSR_BASE + 0x20c)  /* iwl-csr.h: analog PLL config */
#define CSR_HW_REV_WA_REG         (CSR_BASE + 0x22c)  /* iwl-csr.h: HW rev work-around */
#define CSR_DBG_HPET_MEM_REG      (CSR_BASE + 0x240)  /* iwl-csr.h: FH wait-threshold W/A */
#define CSR_MBOX_SET_REG          (CSR_BASE + 0x088)  /* iwl-csr.h: mailbox set (OS_ALIVE) */
#define CSR_GIO_CHICKEN_BITS      (CSR_BASE + 0x100)  /* iwl-csr.h: GIO chicken bits register */

/* --- CSR_RESET bits (iwl-csr.h: CSR_RESET_REG_FLAG_*) --- */
#define CSR_RESET_REG_FLAG_NEVO_RESET        0x00000001
#define CSR_RESET_REG_FLAG_FORCE_NMI         0x00000002
#define CSR_RESET_REG_FLAG_SW_RESET          0x00000080
#define CSR_RESET_REG_FLAG_MASTER_DISABLED   0x00000100
#define CSR_RESET_REG_FLAG_STOP_MASTER       0x00000200

/* --- CSR_GP_CNTRL bits (iwl-csr.h: CSR_GP_CNTRL_REG_*) --- */
#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY   0x00000001
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE         0x00000004
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ    0x00000008
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP    0x00000010
#define CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN      0x00000001
#define CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW     0x08000000

/* --- CSR_INT bits (iwl-csr.h) --- */
#define CSR_INT_BIT_HW_ERR       (1 << 29)   /* iwl-csr.h: DMA hardware error FH_INT[31] */

/* --- CSR_GIO_REG bits (iwl-csr.h: CSR_GIO_REG_VAL_L0S_DISABLED) --- */
#define CSR_GIO_REG_VAL_L0S_DISABLED   0x00000002

/* --- CSR_GIO_CHICKEN_BITS (iwl-csr.h: CSR_GIO_CHICKEN_BITS_REG_BIT_*) --- */
#define CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX       0x00800000
#define CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER  0x20000000

/* --- CSR_DBG_HPET_MEM_REG value (iwl-csr.h: CSR_DBG_HPET_MEM_REG_VAL) --- */
#define CSR_DBG_HPET_MEM_REG_VAL   0xFFFF0000

/* --- CSR_ANA_PLL_CFG value (iwl-csr.h: CSR50_ANA_PLL_CFG_VAL) --- *
 * Linux iwl_pcie_apm_init sets this on families whose base_params->pll_cfg is
 * true (the 1000 + 5000 series: iwl1000/iwl5000_base_params.pll_cfg_val). The
 * 6000 series does NOT need it (pll_cfg_val == 0), so this is applied only when
 * trans->pll_cfg is set (see iwl-ops.c family detection). */
#define CSR50_ANA_PLL_CFG_VAL      0x00880300

/* --- CSR_HW_IF_CONFIG_REG bits (iwl-csr.h) --- */
#define CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A   0x00080000   /* iwl-csr.h */
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_READY      0x00400000   /* iwl-csr.h: PCI_OWN_SEM */
#define CSR_HW_IF_CONFIG_REG_PREPARE            0x08000000   /* iwl-csr.h: WAKE_ME */

/* --- CSR_MBOX_SET_REG bit (iwl-csr.h: CSR_MBOX_SET_REG_OS_ALIVE = BIT(5)) --- */
#define CSR_MBOX_SET_REG_OS_ALIVE   (1 << 5)

/* ====================================================================== *
 *  HBUS_TARG_* -- the indirect window into device-internal MEM + PRPH.
 *  iwl-csr.h: #define HBUS_BASE (0x400). The PRPH accessors below program an
 *  address into *_WADDR/*_RADDR, then read/write *_WDAT/*_RDAT.
 * ====================================================================== */
#define HBUS_BASE                 0x400

#define HBUS_TARG_MEM_RADDR       (HBUS_BASE + 0x00c)  /* iwl-csr.h: 0x40c */
#define HBUS_TARG_MEM_WADDR       (HBUS_BASE + 0x010)  /* iwl-csr.h: 0x410 */
#define HBUS_TARG_MEM_WDAT        (HBUS_BASE + 0x018)  /* iwl-csr.h: 0x418 */
#define HBUS_TARG_MEM_RDAT        (HBUS_BASE + 0x01c)  /* iwl-csr.h: 0x41c */
#define HBUS_TARG_PRPH_WADDR      (HBUS_BASE + 0x044)  /* iwl-csr.h: 0x444 */
#define HBUS_TARG_PRPH_RADDR      (HBUS_BASE + 0x048)  /* iwl-csr.h: 0x448 */
#define HBUS_TARG_PRPH_WDAT       (HBUS_BASE + 0x04c)  /* iwl-csr.h: 0x44c */
#define HBUS_TARG_PRPH_RDAT       (HBUS_BASE + 0x050)  /* iwl-csr.h: 0x450 */
#define HBUS_TARG_WRPTR           (HBUS_BASE + 0x060)  /* iwl-csr.h: 0x460 (TX queue doorbell) */

/*
 * PRPH address mask for the older (gen1) parts the T410 carries.
 * trans.c: iwl_trans_pcie_prph_msk() returns 0x000FFFFF for pre-9000 devices.
 * The accessor ORs (3 << 24) into the address word (a 4-byte burst selector).
 */
#define IWL_PRPH_ADDR_MASK        0x000FFFFF
#define IWL_PRPH_BURST_4B         (3 << 24)

/* ====================================================================== *
 *  APMG -- the periphery (PRPH-space) clock/power registers programmed via
 *  the HBUS_TARG_PRPH_* indirection during APM init.
 *  iwl-prph.h: APMG_BASE = PRPH_BASE(0x00000) + 0x3000.
 * ====================================================================== */
#define PRPH_BASE                 0x00000
#define APMG_BASE                 (PRPH_BASE + 0x3000)

#define APMG_CLK_CTRL_REG         (APMG_BASE + 0x0000)  /* iwl-prph.h */
#define APMG_CLK_EN_REG           (APMG_BASE + 0x0004)  /* iwl-prph.h */
#define APMG_CLK_DIS_REG          (APMG_BASE + 0x0008)  /* iwl-prph.h */
#define APMG_PS_CTRL_REG          (APMG_BASE + 0x000c)  /* iwl-prph.h */
#define APMG_PCIDEV_STT_REG       (APMG_BASE + 0x0010)  /* iwl-prph.h */
#define APMG_RTC_INT_STT_REG      (APMG_BASE + 0x001c)  /* iwl-prph.h */
#define APMG_DIGITAL_SVR_REG      (APMG_BASE + 0x0058)  /* iwl-prph.h */

/* --- APMG bit/field values (iwl-prph.h) --- */
#define APMG_CLK_VAL_DMA_CLK_RQT          0x00000200
#define APMG_CLK_VAL_BSM_CLK_RQT          0x00000800
#define APMG_PS_CTRL_VAL_RESET_REQ        0x04000000
#define APMG_PS_CTRL_MSK_PWR_SRC          0x03000000
#define APMG_PS_CTRL_VAL_PWR_SRC_VMAIN    0x00000000
#define APMG_PS_CTRL_VAL_PWR_SRC_VAUX     0x02000000
#define APMG_PCIDEV_STT_VAL_L1_ACT_DIS    0x00000800
#define APMG_RTC_INT_STT_RFKILL           0x10000000

/* ====================================================================== *
 *  FH (flow handler) -- the RX BD ring + command-queue (TFD/CBBC) registers.
 *  iwl-fh.h: #define FH_MEM_LOWER_BOUND (0x1000); offsets derive from it.
 * ====================================================================== */
#define FH_MEM_LOWER_BOUND        0x1000

/* RX serial-config-status-register channel 0 (RBD ring) -- FH_MEM_RSCSR_CHNL0
 * = FH_MEM_LOWER_BOUND + 0xBC0 = 0x1BC0. */
#define FH_MEM_RSCSR_CHNL0            (FH_MEM_LOWER_BOUND + 0xBC0)        /* 0x1BC0 */
#define FH_RSCSR_CHNL0_STTS_WPTR_REG  (FH_MEM_RSCSR_CHNL0)               /* 0x1BC0: RB status DMA base */
#define FH_RSCSR_CHNL0_RBDCB_BASE_REG (FH_MEM_RSCSR_CHNL0 + 0x004)       /* 0x1BC4: RBD ring DMA base */
#define FH_RSCSR_CHNL0_RBDCB_WPTR_REG (FH_MEM_RSCSR_CHNL0 + 0x008)       /* 0x1BC8: RBD write ptr (doorbell) */
#define FH_RSCSR_CHNL0_WPTR           (FH_RSCSR_CHNL0_RBDCB_WPTR_REG)    /* alias used by rx.c */
#define FH_RSCSR_CHNL0_RDPTR          (FH_MEM_RSCSR_CHNL0 + 0x00c)       /* 0x1BCC: RBD read ptr */

/* RX config register channel 0 -- FH_MEM_RCSR_CHNL0 = FH_MEM_LOWER_BOUND + 0xC00
 * = 0x1C00. */
#define FH_MEM_RCSR_LOWER_BOUND       (FH_MEM_LOWER_BOUND + 0xC00)       /* 0x1C00 */
#define FH_MEM_RCSR_CHNL0             (FH_MEM_RCSR_LOWER_BOUND)          /* 0x1C00 */
#define FH_MEM_RCSR_CHNL0_CONFIG_REG  (FH_MEM_RCSR_CHNL0)               /* 0x1C00: RX DMA enable/config */

/* RX config field values + shifts (iwl-fh.h) */
#define FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL        0x80000000
#define FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K        0x00000000
#define FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY           0x00000004
#define FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL 0x00001000
#define FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS          4
#define FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS            20
#define RX_RB_TIMEOUT                               0x11   /* iwl-fh.h */
#define RX_QUEUE_SIZE                               256    /* iwl-fh.h */
#define RX_QUEUE_SIZE_LOG                           8      /* iwl-fh.h: log2(256) */

/* Command/TX TFD ring: circular-buffer base registers (per channel).
 * iwl-fh.h: FH_MEM_CBBC_0_15_LOWER_BOUND = FH_MEM_LOWER_BOUND + 0x9D0 = 0x19D0,
 * and FH_MEM_CBBC_QUEUE(chnl) = base + 4*chnl for chnl < 16. */
#define FH_MEM_CBBC_0_15_LOWER_BOUND  (FH_MEM_LOWER_BOUND + 0x9D0)        /* 0x19D0 */
#define FH_MEM_CBBC_QUEUE(chnl)       (FH_MEM_CBBC_0_15_LOWER_BOUND + 4 * (chnl))

/* TX DMA config registers (per channel).
 * iwl-fh.h: FH_TCSR_LOWER_BOUND = FH_MEM_LOWER_BOUND + 0xD00 = 0x1D00, and
 * FH_TCSR_CHNL_TX_CONFIG_REG(chnl) = base + 0x20*chnl. */
#define FH_TCSR_LOWER_BOUND           (FH_MEM_LOWER_BOUND + 0xD00)        /* 0x1D00 */
#define FH_TCSR_CHNL_TX_CONFIG_REG(chnl)  (FH_TCSR_LOWER_BOUND + 0x20 * (chnl))

#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE   0x00000008  /* iwl-fh.h */
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_IFTFD     0x00200000  /* iwl-fh.h */
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE     0x80000000  /* iwl-fh.h */

/* FH TX chicken bits -- iwl_pcie_tx_start sets SCD_AUTO_RETRY_EN after enabling
 * the TX DMA channels (iwl-fh.h FH_TX_CHICKEN_BITS_REG = FH_MEM_LOWER_BOUND +
 * 0xE98 = 0x1E98). */
#define FH_TX_CHICKEN_BITS_REG        (FH_MEM_LOWER_BOUND + 0xE98)  /* 0x1E98 */
#define FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN  0x00000002            /* iwl-fh.h BIT(1) */

/* TFD command-queue depth (iwl-fh.h: TFD_QUEUE_SIZE_MAX). One TFD per slot. */
#define TFD_QUEUE_SIZE_MAX            256

/* ====================================================================== *
 *  Poll discipline (mirrors Linux but BOUNDED here -- see iwl-trans.c).
 *  iwl-io.c: IWL_POLL_INTERVAL = 10 us; iwl_finish_nic_init polls
 *  MAC_CLOCK_READY with a 25000 us timeout; grab_nic_access uses 15000 us.
 *  We translate "microsecond timeout" into a bounded iteration count because
 *  the PIT can be frozen at the time the future trigger runs -- NEVER wait on
 *  ticks. The counts below are deliberately generous.
 * ====================================================================== */
#define IWL_TRANS_POLL_MAX        100000   /* hard iteration cap per poll loop */

#endif /* IWL_CSR_H */
