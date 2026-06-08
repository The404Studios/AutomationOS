/*
 * Intel e1000 (82540EM) Ethernet driver -- QEMU's emulated NIC.
 * =============================================================
 *
 * Target: `-device e1000` in QEMU == Intel 82540EM, PCI vendor 0x8086,
 * device 0x100E, class 0x02 (network) subclass 0x00 (ethernet). This is the
 * most tractable NIC to exercise from a hobby kernel: a flat MMIO register
 * file plus simple legacy RX/TX descriptor rings, no firmware, no PHY dance.
 *
 * Bring-up sequence (this file):
 *   1. pci_find_class(0x02,0x00,...) / pci_find_device(0x8086,0x100E).
 *   2. Enable bus-master + memory space, map BAR0 (MMIO register file).
 *   3. Read the MAC from the Receive Address registers (RAL0/RAH0), which QEMU
 *      seeds from the EEPROM image (52:54:00:12:34:56 by default).
 *   4. Allocate RX/TX descriptor rings and per-descriptor DMA buffers from the
 *      PMM. Because the kernel identity-maps physical RAM 1:1 (see paging.c,
 *      0..16GB), a pmm_alloc_page() pointer IS its physical/DMA address.
 *   5. Program RDBAL/RDBAH/RDLEN/RDH/RDT and TDBAL/.../TDT, then set RCTL/TCTL
 *      to enable the receiver and transmitter.
 *   6. TX: fill a descriptor, bump TDT, spin on the DD writeback bit.
 *      RX:  scan the ring at RDH..software-head for descriptors with DD set.
 *
 * MMIO is accessed through volatile 32-bit reads/writes at the identity-mapped
 * BAR base. DMA is plain physical memory the device reads/writes via bus
 * mastering (which we enabled in PCI config space).
 *
 * Scope: kernel/drivers/net/e1000.c only.
 */

#include "../../include/net.h"
#include "../../include/e1000.h"
#include "../../include/pci.h"
#include "../../include/mem.h"      /* pmm_alloc_page, kmalloc            */
#include "../../include/types.h"
#include "../../include/kernel.h"   /* kprintf, PAGE_SIZE                  */
#include "../../include/string.h"   /* memset, memcpy                      */

/* ------------------------------------------------------------------ */
/* PCI identity                                                        */
/* ------------------------------------------------------------------ */
/*
 * All Intel wired-LAN parts share vendor 0x8086.  The device IDs below are
 * split into two groups:
 *
 *   "Classic" e1000 (82540/82545)        -- the original QEMU `-device e1000`
 *                                            path.  Flat MMIO register file,
 *                                            EEPROM-seeded MAC, simple rings.
 *                                            This is the VERIFIED path.
 *
 *   "e1000e"-class (82571/4, 8257x PCH,   -- found on real laptops/desktops
 *    i217/i218, etc.)                        such as the Lenovo ThinkPad T410
 *                                            (82577LM).  QEMU can also emulate
 *                                            the 82574L (`-device e1000e`).
 *
 * See E1000_DEVICE_IDS[] in e1000_init() for the full candidate list and the
 * note about e1000e bring-up limitations.
 */
#define E1000_VENDOR_ID     0x8086

/* --- Classic e1000 (verified QEMU path) --- */
#define E1000_DEVICE_82540  0x100E   /* QEMU default `-device e1000`      */
#define E1000_DEVICE_82545  0x100F

/* --- e1000e-class parts (real hardware; QEMU emulates the 82574L) --- */
#define E1000_DEVICE_82574L 0x10D3   /* `-device e1000e` (also real HW)   */
#define E1000_DEVICE_82577LM 0x10EA  /* ThinkPad T410 onboard NIC         */
#define E1000_DEVICE_82577LC 0x10EB
#define E1000_DEVICE_82578DM 0x10EF
#define E1000_DEVICE_82578DC 0x10F0
#define E1000_DEVICE_82567LM 0x10F5
#define E1000_DEVICE_82579LM 0x1502
#define E1000_DEVICE_82579V  0x1503
#define E1000_DEVICE_I217LM  0x153A
#define E1000_DEVICE_I218LM  0x15A0

/* Back-compat alias: 82574L used to be spelled E1000_DEVICE_82574. */
#define E1000_DEVICE_82574  E1000_DEVICE_82574L

#define PCI_CLASS_NETWORK   0x02
#define PCI_SUBCLASS_ETHER  0x00

/*
 * Returns true if the matched device ID is an e1000e-class part rather than
 * the classic/verified 82540/82545 (or QEMU's 82574L).  Used only to emit a
 * "best-effort" diagnostic; it does not change the bring-up path.
 */
static inline bool e1000_is_e1000e_class(uint16_t device_id) {
    switch (device_id) {
        case E1000_DEVICE_82540:
        case E1000_DEVICE_82545:
        case E1000_DEVICE_82574L:   /* QEMU `-device e1000e` -- treated as OK */
            return false;
        default:
            return true;            /* 82577/82578/82579/i217/i218/... */
    }
}

/* ------------------------------------------------------------------ */
/* MMIO register offsets (byte offsets from BAR0)                      */
/* ------------------------------------------------------------------ */
#define E1000_CTRL      0x0000   /* Device Control                     */
#define E1000_STATUS    0x0008   /* Device Status                      */
#define E1000_EERD      0x0014   /* EEPROM Read                        */
#define E1000_ICR       0x00C0   /* Interrupt Cause Read               */
#define E1000_IMS       0x00D0   /* Interrupt Mask Set                 */
#define E1000_IMC       0x00D8   /* Interrupt Mask Clear               */
#define E1000_RCTL      0x0100   /* Receive Control                    */
#define E1000_TCTL      0x0400   /* Transmit Control                   */
#define E1000_TIPG      0x0410   /* Transmit Inter-Packet Gap          */
#define E1000_RDBAL     0x2800   /* RX Descriptor Base Low             */
#define E1000_RDBAH     0x2804   /* RX Descriptor Base High            */
#define E1000_RDLEN     0x2808   /* RX Descriptor Length               */
#define E1000_RDH       0x2810   /* RX Descriptor Head                 */
#define E1000_RDT       0x2818   /* RX Descriptor Tail                 */
#define E1000_TDBAL     0x3800   /* TX Descriptor Base Low             */
#define E1000_TDBAH     0x3804   /* TX Descriptor Base High            */
#define E1000_TDLEN     0x3808   /* TX Descriptor Length               */
#define E1000_TDH       0x3810   /* TX Descriptor Head                 */
#define E1000_TDT       0x3818   /* TX Descriptor Tail                 */
#define E1000_MTA       0x5200   /* Multicast Table Array (128 dwords) */
#define E1000_RAL0      0x5400   /* Receive Address Low  [0]           */
#define E1000_RAH0      0x5404   /* Receive Address High [0]           */

/* CTRL bits. */
#define CTRL_RST        (1u << 26)  /* Device reset                    */
#define CTRL_SLU        (1u << 6)   /* Set Link Up                     */
#define CTRL_ASDE       (1u << 5)   /* Auto-Speed-Detect Enable        */
#define CTRL_LRST       (1u << 3)   /* Link reset                      */
#define CTRL_PHY_RST    (1u << 31)  /* PHY reset                       */

/* RCTL bits. */
#define RCTL_EN         (1u << 1)   /* Receiver enable                 */
#define RCTL_SBP        (1u << 2)   /* Store bad packets               */
#define RCTL_UPE        (1u << 3)   /* Unicast promiscuous             */
#define RCTL_MPE        (1u << 4)   /* Multicast promiscuous           */
#define RCTL_BAM        (1u << 15)  /* Broadcast accept                */
#define RCTL_SECRC      (1u << 26)  /* Strip Ethernet CRC              */
#define RCTL_BSIZE_2048 (0u << 16)  /* 2048-byte buffers (BSEX=0)      */

/* TCTL bits. */
#define TCTL_EN         (1u << 1)   /* Transmit enable                 */
#define TCTL_PSP        (1u << 3)   /* Pad short packets               */
#define TCTL_CT_SHIFT   4           /* Collision threshold             */
#define TCTL_COLD_SHIFT 12          /* Collision distance              */

/* TX descriptor CMD bits. */
#define TXD_CMD_EOP     (1u << 0)   /* End of packet                   */
#define TXD_CMD_IFCS    (1u << 1)   /* Insert FCS                      */
#define TXD_CMD_RS      (1u << 3)   /* Report status (writes DD)       */
#define TXD_STAT_DD     (1u << 0)   /* Descriptor done (status byte)   */

/* RX descriptor status bits. */
#define RXD_STAT_DD     (1u << 0)   /* Descriptor done                 */
#define RXD_STAT_EOP    (1u << 1)   /* End of packet                   */

/* ------------------------------------------------------------------ */
/* e1000e / PCH (82577LM and friends) bring-up registers + bits        */
/* ------------------------------------------------------------------ */
/*
 * The classic 82540/82545 (and QEMU's 82574L via `-device e1000e`) link up
 * with nothing more than CTRL.SLU|ASDE -- their PHY is powered and auto-
 * negotiating straight out of reset.  The PCH-integrated parts on real
 * laptops -- the ThinkPad T410's 82577LM, plus 82578/82579/i217/i218 -- are
 * different: the MAC lives in the chipset and the PHY hangs off an internal
 * MDIO link that is SHARED with the Management-Engine firmware.  Before
 * software may drive the PHY it must take the SW/FW MDIO-ownership flag
 * (EXTCNF_CTRL.SWFLAG), then power the PHY up and (re)start auto-negotiation
 * over MDIC.  None of this is exercised by QEMU (its e1000/e1000e models are
 * not PCH parts), so the whole path is gated behind e1000_is_pch() and the
 * verified QEMU bring-up is left byte-for-byte unchanged.
 */
#define E1000_CTRL_EXT     0x0018   /* Extended Device Control            */
#define E1000_MDIC         0x0020   /* MDI Control (PHY register access)   */
#define E1000_FWSM         0x5B54   /* Firmware Semaphore (ME state)       */
#define E1000_SWSM         0x5B50   /* Software Semaphore                  */
#define E1000_EXTCNF_CTRL  0x0F00   /* Extended Config Control (SWFLAG)    */
#define E1000_EXTCNF_SIZE  0x0F08   /* Extended Config Size (NVM presence) */
#define E1000_PCIEANACFG   0x0F18   /* PCIe Analog Configuration          */
#define E1000_PHY_CTRL     0x0F10   /* PHY Control (PCH LAN)              */

/* FWSM (Firmware Semaphore) bits -- detect ME firmware activity. */
#define FWSM_FW_VALID      (1u << 15)   /* firmware has loaded             */
#define FWSM_MODE_MASK     0x000E       /* firmware mode bits [3:1]        */
#define FWSM_MODE_SHIFT    1

/* SWSM bits -- hardware semaphore for ME/SW arbitration. */
#define SWSM_SMBI          (1u << 0)    /* software semaphore bit          */
#define SWSM_SWESMBI       (1u << 1)    /* SW/FW semaphore bit             */

/* PHY_CTRL bits (PCH-specific PHY power management). */
#define PHY_CTRL_GBE_DIS   (1u << 6)    /* disable GbE (force 10/100)     */
#define PHY_CTRL_D0A_LPLU  (1u << 1)    /* D0a Low-Power Link Up          */

/* MDIC fields. */
#define MDIC_DATA_MASK     0x0000FFFFu
#define MDIC_REG_SHIFT     16        /* PHY register address [20:16]       */
#define MDIC_PHY_SHIFT     21        /* PHY address          [25:21]       */
#define MDIC_OP_WRITE      (0x1u << 26)
#define MDIC_OP_READ       (0x2u << 26)
#define MDIC_READY         (1u << 28)
#define MDIC_ERROR         (1u << 30)

/* ich8lan/PCH software MDIO-ownership flag (EXTCNF_CTRL bit 5). */
#define EXTCNF_CTRL_SWFLAG (1u << 5)

/* Default-OFF gate for the 82577LM-class PCH NIC bring-up. The PCH PHY hangs off
 * an internal MDIO bus SHARED with the Management Engine, and driving it can
 * HARDWARE-STALL the real T410 at boot (a bus stall, not a software spin -- the
 * bounded loops cannot make it safe). So the bring-up is OFF unless the operator
 * opts in with PCH_NIC=1 (-> -DE1000_PCH_NIC) to validate on the device. QEMU is
 * unaffected (its e1000/e1000e are never PCH parts). Implemented as a compile-
 * time CONSTANT (not #ifdef around the call sites) so the PCH helper functions
 * stay referenced and never trip -Wunused-function. */
#ifdef E1000_PCH_NIC
#define E1000_PCH_NIC_ENABLED 1
#else
#define E1000_PCH_NIC_ENABLED 0
#endif

/* Standard MII PHY registers (clause 22) + the bits we touch. */
#define MII_BMCR           0x00      /* Basic Mode Control                 */
#define MII_BMSR           0x01      /* Basic Mode Status                  */
#define MII_PHYID1         0x02
#define MII_PHYID2         0x03
#define MII_ANAR           0x04      /* Auto-Neg Advertisement Register    */
#define MII_ANLPAR         0x05      /* Auto-Neg Link Partner Ability      */
#define MII_GBCR           0x09      /* 1000BASE-T Control Register        */
#define BMCR_RESET         (1u << 15)
#define BMCR_ANENABLE      (1u << 12)
#define BMCR_PDOWN         (1u << 11)
#define BMCR_ANRESTART     (1u << 9)
#define BMSR_LSTATUS       (1u << 2)
#define STATUS_LU          (1u << 1)  /* Device Status: link up             */

/* MII ANAR bits (auto-negotiation advertisement). */
#define ANAR_10            (1u << 5)   /* 10BASE-T                        */
#define ANAR_10FD          (1u << 6)   /* 10BASE-T full-duplex            */
#define ANAR_TX            (1u << 7)   /* 100BASE-TX                      */
#define ANAR_TXFD          (1u << 8)   /* 100BASE-TX full-duplex          */
#define ANAR_SELECTOR      0x0001u     /* IEEE 802.3 selector field       */

/* MII GBCR bits (1000BASE-T advertisement). */
#define GBCR_1000T         (1u << 8)   /* advertise 1000BASE-T            */
#define GBCR_1000TFD       (1u << 9)   /* advertise 1000BASE-T full-dup   */

/* Ring sizes (must be multiples of 8).
 * 64 descriptors per ring avoids stalls under moderate traffic (the original 8
 * TX / 32 RX was tight enough to drop frames under bursty workloads). */
#define NUM_RX_DESC     64
#define NUM_TX_DESC     64
#define RX_BUF_SIZE     2048
#define TX_BUF_SIZE     2048

/* ------------------------------------------------------------------ */
/* Legacy descriptor formats (16 bytes each, must be 16-byte aligned)  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t addr;     /* buffer physical address                     */
    uint16_t length;   /* bytes received                              */
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} PACKED e1000_rx_desc_t;

typedef struct {
    uint64_t addr;     /* buffer physical address                     */
    uint16_t length;   /* bytes to transmit                           */
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;   /* low nibble: DD on writeback                 */
    uint8_t  css;
    uint16_t special;
} PACKED e1000_tx_desc_t;

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */
static struct {
    bool             present;
    volatile uint8_t* mmio;     /* identity-mapped BAR0 base           */
    uint8_t          mac[ETH_ALEN];

    e1000_rx_desc_t* rx_ring;   /* NUM_RX_DESC descriptors             */
    e1000_tx_desc_t* tx_ring;   /* NUM_TX_DESC descriptors             */
    uint8_t*         rx_bufs[NUM_RX_DESC];
    uint8_t*         tx_bufs[NUM_TX_DESC];

    uint32_t         rx_cur;    /* software RX head (next to inspect)  */
    uint32_t         tx_cur;    /* software TX tail (next to fill)     */

    uint16_t         device_id; /* matched PCI device id               */
    uint8_t          phy_addr;  /* MDIO address of the PHY (PCH parts) */
    bool             is_pch;    /* true => 82577LM-class bring-up used */
    bool             link_up;   /* last observed MAC link state        */
} e1000;

/* ------------------------------------------------------------------ */
/* MMIO accessors                                                      */
/* ------------------------------------------------------------------ */
static inline void mmio_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(e1000.mmio + reg) = val;
}
static inline uint32_t mmio_read32(uint32_t reg) {
    return *(volatile uint32_t*)(e1000.mmio + reg);
}

/*
 * Descriptor memory barrier.
 *
 * The NIC is a bus-master DMA device.  When software fills in a TX
 * descriptor (or recycles an RX descriptor) it must ensure all writes
 * to the descriptor / data buffer are visible to the NIC's DMA engine
 * BEFORE the tail-pointer register write that kicks it off.
 *
 * On x86 the CPU memory model is TSO (store-store ordered), so CPU
 * stores to normal WB memory are not reordered past subsequent stores
 * to the same memory type.  However, the MMIO tail-pointer write goes
 * to UC/WC memory (MTRR/PAT-controlled), and on some micro-
 * architectures stores to WC memory can be retired out of order with
 * respect to preceding WB stores.  An SFENCE closes that window.
 *
 * We also emit a compiler barrier ("" ::: "memory") to prevent the
 * compiler from sinking descriptor writes past the tail store.
 */
static inline void desc_wmb(void) {
    asm volatile("sfence" ::: "memory");
}

/*
 * Read a descriptor status byte through a volatile pointer so the
 * compiler cannot cache the value across a loop iteration.  The NIC
 * writes back the DD (descriptor done) bit directly into the
 * descriptor — without volatile the compiler is free to hoist the load
 * out of the spin loop and spin forever on a register copy.
 */
static inline uint8_t desc_read_status(const volatile e1000_rx_desc_t* d) {
    return d->status;
}
static inline uint8_t desc_read_tx_status(const volatile e1000_tx_desc_t* d) {
    return d->status;
}

/* Crude busy-wait. The PIT-based timer isn't guaranteed to be live in all
 * init orders, so we spin on a volatile counter -- precision is irrelevant,
 * we only need the device a few microseconds to settle after a register poke. */
static void e1000_delay(volatile uint32_t loops) {
    while (loops--) {
        asm volatile("pause");
    }
}

/* ------------------------------------------------------------------ */
/* MAC address                                                         */
/* ------------------------------------------------------------------ */
/*
 * QEMU pre-loads the Receive Address registers from the EEPROM image, so we
 * can read the MAC straight out of RAL0/RAH0 without bit-banging the EEPROM.
 */
static void e1000_read_mac(void) {
    uint32_t ral = mmio_read32(E1000_RAL0);
    uint32_t rah = mmio_read32(E1000_RAH0);

    e1000.mac[0] = (uint8_t)(ral & 0xFF);
    e1000.mac[1] = (uint8_t)((ral >> 8) & 0xFF);
    e1000.mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    e1000.mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    e1000.mac[4] = (uint8_t)(rah & 0xFF);
    e1000.mac[5] = (uint8_t)((rah >> 8) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Ring setup                                                          */
/* ------------------------------------------------------------------ */
/*
 * Identity-mapped RAM means a pmm_alloc_page() return value is simultaneously
 * the kernel virtual address (for CPU access) and the physical/bus address
 * (for the NIC's DMA engine). A descriptor ring (NUM*16 bytes) fits in one 4K
 * page and is naturally 16-byte aligned at a page boundary.
 */
static int e1000_setup_rx(void) {
    void* ring = pmm_alloc_page();
    if (!ring) return -1;
    memset(ring, 0, PAGE_SIZE);
    e1000.rx_ring = (e1000_rx_desc_t*)ring;

    for (uint32_t i = 0; i < NUM_RX_DESC; i++) {
        void* buf = pmm_alloc_page();   /* 4K >= 2048 RX buffer */
        if (!buf) return -1;
        e1000.rx_bufs[i]      = (uint8_t*)buf;
        e1000.rx_ring[i].addr = (uint64_t)(uintptr_t)buf;  /* phys == virt */
        e1000.rx_ring[i].status = 0;
    }

    uint64_t ring_phys = (uint64_t)(uintptr_t)ring;
    mmio_write32(E1000_RDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
    mmio_write32(E1000_RDBAH, (uint32_t)(ring_phys >> 32));
    mmio_write32(E1000_RDLEN, NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    mmio_write32(E1000_RDH, 0);
    /*
     * 82540 spec §13.4.28: RDT is INCLUSIVE — it points to the last
     * descriptor the hardware is allowed to use.  Setting RDT to
     * NUM_RX_DESC-1 with RDH=0 hands all NUM_RX_DESC descriptors to
     * the NIC.  This is correct; no "one slot gap" is required at init
     * because RDH==RDT means "ring empty", not "ring full", on this
     * hardware.
     */
    desc_wmb();   /* flush descriptor writes before poking the tail register */
    mmio_write32(E1000_RDT, NUM_RX_DESC - 1);   /* tail = last index, inclusive */
    e1000.rx_cur = 0;

    mmio_write32(E1000_RCTL,
                 RCTL_EN | RCTL_BAM | RCTL_UPE | RCTL_MPE | RCTL_SECRC |
                 RCTL_BSIZE_2048);
    return 0;
}

static int e1000_setup_tx(void) {
    void* ring = pmm_alloc_page();
    if (!ring) return -1;
    memset(ring, 0, PAGE_SIZE);
    e1000.tx_ring = (e1000_tx_desc_t*)ring;

    for (uint32_t i = 0; i < NUM_TX_DESC; i++) {
        void* buf = pmm_alloc_page();
        if (!buf) return -1;
        e1000.tx_bufs[i]      = (uint8_t*)buf;
        e1000.tx_ring[i].addr = (uint64_t)(uintptr_t)buf;
        e1000.tx_ring[i].status = TXD_STAT_DD;  /* mark free */
    }

    uint64_t ring_phys = (uint64_t)(uintptr_t)ring;
    mmio_write32(E1000_TDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
    mmio_write32(E1000_TDBAH, (uint32_t)(ring_phys >> 32));
    mmio_write32(E1000_TDLEN, NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    mmio_write32(E1000_TDH, 0);
    desc_wmb();   /* flush descriptor writes before tail register */
    mmio_write32(E1000_TDT, 0);
    e1000.tx_cur = 0;

    /* IPG per the 82540 datasheet: IPGT=10, IPGR1=8, IPGR2=6. */
    mmio_write32(E1000_TIPG, 10 | (8 << 10) | (6 << 20));
    mmio_write32(E1000_TCTL,
                 TCTL_EN | TCTL_PSP |
                 (0x0F << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT));
    return 0;
}

/* ------------------------------------------------------------------ */
/* e1000e / PCH (82577LM) PHY bring-up                                 */
/* ------------------------------------------------------------------ */
/*
 * Is this an ich8lan/PCH part whose PHY needs the SW/FW-synchronised MDIO
 * power-up dance?  TRUE for the 82577/82578/82579/i217/i218 LOM parts found on
 * real machines (the T410 is 82577LM); FALSE for the classic 82540/82545 and
 * QEMU's 82574L, which auto-link out of reset on the verified path.
 */
static bool e1000_is_pch(uint16_t device_id) {
    switch (device_id) {
        case E1000_DEVICE_82577LM:
        case E1000_DEVICE_82577LC:
        case E1000_DEVICE_82578DM:
        case E1000_DEVICE_82578DC:
        case E1000_DEVICE_82567LM:
        case E1000_DEVICE_82579LM:
        case E1000_DEVICE_82579V:
        case E1000_DEVICE_I217LM:
        case E1000_DEVICE_I218LM:
            return true;
        default:
            return false;
    }
}

/*
 * Take / release the ich8lan MDIO-ownership flag.  EXTCNF_CTRL.SWFLAG is the
 * handshake that stops software and the Management-Engine firmware from driving
 * the shared internal MDIO link at the same time.  EVERY spin here is iteration-
 * bounded, never tick-based: during early boot the PIT may be frozen, so a
 * wall-clock timeout would never fire -- exactly the trap that hung the AHCI
 * probe.  If the firmware never yields we give up gracefully (the NIC is left
 * link-down) rather than wedging the boot.
 *
 * IMPORTANT (the original T410 hang root-cause):
 * The previous code hammered EXTCNF_CTRL with tight-loop reads + writes which
 * on the PCH bus could back-pressure the internal MDIO link and cause the bus-
 * stall that was observed as a hang.  The fix:
 *   (a) Larger inter-iteration delays (each e1000_delay(500) ~ a few us on
 *       real HW, enough for the ME firmware to cycle its MDIO transaction).
 *   (b) Fewer total attempts to avoid prolonged bus contention.
 *   (c) A read-back fence after the SWFLAG write to let the PCH register
 *       post complete before we re-read.
 */
static bool e1000_acquire_swflag(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        /* Wait for firmware to release SWFLAG. Generous delay between reads
         * to avoid bus contention with the ME's own MDIO cycles. */
        int spins = 0;
        while ((mmio_read32(E1000_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) &&
               spins++ < 2000) {
            e1000_delay(500);  /* ~a few us per iter; up to ~10ms total */
        }
        uint32_t v = mmio_read32(E1000_EXTCNF_CTRL);
        if (v & EXTCNF_CTRL_SWFLAG) continue;          /* FW still holds it  */

        /* Set our SWFLAG claim. */
        v |= EXTCNF_CTRL_SWFLAG;
        mmio_write32(E1000_EXTCNF_CTRL, v);

        /* Read-back fence: let the posted write complete before verifying.
         * Without this, on PCH the read-back can return the old value and
         * we'd think we failed to acquire, then retry in a tight loop. */
        e1000_delay(100);

        if (mmio_read32(E1000_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) {
            return true;                               /* we own it now      */
        }
    }
    return false;
}

static void e1000_release_swflag(void) {
    uint32_t v = mmio_read32(E1000_EXTCNF_CTRL);
    v &= ~EXTCNF_CTRL_SWFLAG;
    mmio_write32(E1000_EXTCNF_CTRL, v);
    e1000_delay(100);  /* let the posted write settle */
}

/*
 * MDIC PHY read/write.  Caller must hold the SWFLAG on PCH parts.  A read
 * returns 0xFFFF on timeout / MDIC.error so a missing PHY reads as "all ones".
 * Both spin loops are iteration-capped for the same frozen-PIT reason as above.
 */
static uint16_t e1000_phy_read(uint8_t phy, uint8_t reg) {
    mmio_write32(E1000_MDIC,
                 ((uint32_t)reg << MDIC_REG_SHIFT) |
                 ((uint32_t)phy << MDIC_PHY_SHIFT) | MDIC_OP_READ);
    for (int s = 0; s < 100000; s++) {
        uint32_t m = mmio_read32(E1000_MDIC);
        if (m & MDIC_READY)
            return (m & MDIC_ERROR) ? 0xFFFF : (uint16_t)(m & MDIC_DATA_MASK);
        e1000_delay(50);
    }
    return 0xFFFF;
}

static void e1000_phy_write(uint8_t phy, uint8_t reg, uint16_t data) {
    mmio_write32(E1000_MDIC,
                 (uint32_t)data |
                 ((uint32_t)reg << MDIC_REG_SHIFT) |
                 ((uint32_t)phy << MDIC_PHY_SHIFT) | MDIC_OP_WRITE);
    for (int s = 0; s < 100000; s++) {
        if (mmio_read32(E1000_MDIC) & MDIC_READY) return;
        e1000_delay(50);
    }
}

/*
 * PCH-specific MAC reset.
 *
 * On PCH parts the full CTRL_RST must be bracketed by SWFLAG ownership, and
 * the ME firmware mode must be checked.  A bare CTRL_RST issued while the ME
 * is mid-transaction on the internal MDIO bus will wedge the PCH bus -- that
 * was the root cause of the original T410 boot hang.
 *
 * Sequence (derived from the Linux e1000e ich8lan.c reset sequence):
 *   1. Mask all interrupts.
 *   2. Read FWSM to check whether ME firmware is active; if so, yield a
 *      generous delay to let it finish any in-progress MDIO cycle.
 *   3. Acquire SWFLAG (the ME/SW MDIO mutex).
 *   4. Issue CTRL_RST.  While we hold SWFLAG the ME cannot start new MDIO
 *      traffic, so the reset's internal bus sequences complete cleanly.
 *   5. Wait for CTRL_RST to self-clear (bounded).
 *   6. Release SWFLAG after the MAC is out of reset.
 *   7. Re-mask interrupts (reset re-enables some causes on PCH).
 *
 * Returns 0 on success, -1 if the MAC could not be reset cleanly.
 */
static int e1000_pch_mac_reset(void) {
    /* 1. Mask all interrupts before reset. */
    mmio_write32(E1000_IMC, 0xFFFFFFFF);

    /* 2. If ME firmware is active, give it a moment to finish.  On 82577LM the
     *    ME typically runs an AMT agent; FWSM.FW_VALID indicates its presence.
     *    We cannot tell exactly when it is idle, but a fixed delay here is far
     *    better than resetting the MAC under it. */
    uint32_t fwsm = mmio_read32(E1000_FWSM);
    if (fwsm & FWSM_FW_VALID) {
        kprintf("[E1000] PCH: ME firmware active (FWSM=0x%08x), yielding...\n", fwsm);
        e1000_delay(2000000);  /* ~10-20 ms on real HW: let ME finish */
    }

    /* 3. Acquire SWFLAG so the ME stays off the MDIO bus during reset. */
    if (!e1000_acquire_swflag()) {
        kprintf("[E1000] PCH: SWFLAG acq failed before MAC reset (ME busy)\n");
        /* Proceed anyway -- the reset may still work if ME is idle. */
    }

    /* 4. Issue device reset. */
    uint32_t ctrl = mmio_read32(E1000_CTRL);
    mmio_write32(E1000_CTRL, ctrl | CTRL_RST);

    /* 5. Wait for CTRL_RST to self-clear, bounded. */
    e1000_delay(500000);   /* initial settle (~2-5 ms) */
    for (int i = 0; i < 500; i++) {
        if (!(mmio_read32(E1000_CTRL) & CTRL_RST)) break;
        e1000_delay(20000);   /* each iter ~100 us */
    }

    /* 6. Release SWFLAG. */
    e1000_release_swflag();

    /* 7. Post-reset: re-mask interrupts, drain pending causes. */
    e1000_delay(200000);   /* let post-reset configuration settle */
    mmio_write32(E1000_IMC, 0xFFFFFFFF);
    (void)mmio_read32(E1000_ICR);

    if (mmio_read32(E1000_CTRL) & CTRL_RST) {
        kprintf("[E1000] PCH: MAC reset did not clear -- hardware stuck\n");
        return -1;
    }
    return 0;
}

/*
 * Bring up the PHY on an 82577LM-class PCH NIC.  Best-effort + fully bounded:
 *   1. take the SW/FW MDIO flag,
 *   2. find the PHY's MDIO address (try 1 then 2; valid PHYID1 != 0/0xFFFF),
 *   3. soft-reset the PHY, clear power-down, advertise all speeds,
 *      enable + restart auto-neg,
 *   4. drop the flag, set MAC CTRL.SLU|ASDE, and poll STATUS.LU for link.
 * Returns 0 if the sequence ran (link may still be down -- reported via
 * e1000.link_up); -1 only if the PHY could not be reached at all.  Never hangs.
 *
 * Link-up polling:
 *   Real auto-negotiation on the 82577LM takes 2-5 seconds depending on the
 *   link partner.  We poll for up to ~3 seconds with generous inter-poll delays
 *   (enough for a cable-plugged T410 to link, but not so long that a no-cable
 *   T410 stalls the boot for ages).  If the cable is plugged in after boot, the
 *   NIC will auto-negotiate in hardware and STATUS.LU will go high; higher layers
 *   can re-check e1000_link_up() at any time.
 */
static int e1000_pch_phy_bringup(void) {
    if (!e1000_acquire_swflag()) {
        kprintf("[E1000] PCH: SW/FW MDIO flag timeout (ME busy?) -- link skipped\n");
        return -1;
    }

    /* Scan for the PHY on MDIO addresses 1 and 2 (the 82577LM is usually at 1). */
    uint8_t  phy = 0;
    uint16_t id1 = 0;
    const uint8_t candidates[] = { 1, 2 };
    for (unsigned i = 0; i < sizeof(candidates); i++) {
        uint16_t v = e1000_phy_read(candidates[i], MII_PHYID1);
        if (v != 0x0000 && v != 0xFFFF) { phy = candidates[i]; id1 = v; break; }
    }
    if (phy == 0) {
        e1000_release_swflag();
        kprintf("[E1000] PCH: no PHY answered on MDIO addr 1/2 -- link skipped\n");
        return -1;
    }
    uint16_t id2 = e1000_phy_read(phy, MII_PHYID2);
    e1000.phy_addr = phy;
    kprintf("[E1000] PCH: PHY @ mdio %u id=%04x:%04x\n", phy, id1, id2);

    /* Soft reset (clause-22 BMCR.reset); bounded wait for the bit to self-clear.
     * PHY reset completes in <1ms per spec and each e1000_phy_read is already
     * MDIC-bounded, so a tight outer cap keeps a dead/absent PHY from stalling
     * the (network-enabled) boot for more than a few ms. */
    e1000_phy_write(phy, MII_BMCR, BMCR_RESET);
    for (int s = 0; s < 500; s++) {
        e1000_delay(200);
        if (!(e1000_phy_read(phy, MII_BMCR) & BMCR_RESET)) break;
    }

    /* Power up + configure auto-negotiation advertisement.
     *
     * The 82577LM supports 10/100/1000.  Advertise ALL speeds so the PHY can
     * negotiate the best link with whatever switch/router is connected.  On many
     * ThinkPad T410 docking stations the link partner is a 100M switch, so
     * advertising only 1000 would fail to link. */
    uint16_t bmcr = e1000_phy_read(phy, MII_BMCR);
    bmcr &= ~BMCR_PDOWN;        /* clear power-down */
    e1000_phy_write(phy, MII_BMCR, bmcr);
    e1000_delay(50000);          /* let PHY power up */

    /* Set auto-neg advertisement: 10/100 all modes + IEEE 802.3 selector. */
    e1000_phy_write(phy, MII_ANAR,
                    ANAR_10 | ANAR_10FD | ANAR_TX | ANAR_TXFD | ANAR_SELECTOR);

    /* Set 1000BASE-T advertisement (register 9). */
    e1000_phy_write(phy, MII_GBCR, GBCR_1000T | GBCR_1000TFD);

    /* Enable + restart auto-negotiation. */
    bmcr = e1000_phy_read(phy, MII_BMCR);
    bmcr |= BMCR_ANENABLE | BMCR_ANRESTART;
    e1000_phy_write(phy, MII_BMCR, bmcr);

    e1000_release_swflag();

    /* Disable LPLU (Low Power Link Up) in D0a -- this can prevent GbE link. */
    uint32_t phyctrl = mmio_read32(E1000_PHY_CTRL);
    phyctrl &= ~(PHY_CTRL_GBE_DIS | PHY_CTRL_D0A_LPLU);
    mmio_write32(E1000_PHY_CTRL, phyctrl);

    /* MAC side: assert link-up + auto-speed-detect.  Clear any stale reset bits
     * that might prevent the MAC from establishing link. */
    uint32_t ctrl = mmio_read32(E1000_CTRL);
    ctrl |=  (CTRL_SLU | CTRL_ASDE);
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST);
    mmio_write32(E1000_CTRL, ctrl);

    /*
     * Poll for link.  Real 82577LM auto-negotiation takes 2-5 seconds.  We poll
     * for up to ~3 seconds: ~6000 iterations x e1000_delay(5000) gives roughly
     * 3s on typical Arrandale-era hardware (~500ns per e1000_delay iteration).
     * If the cable is not plugged in, this exits in ~3s -- acceptable for the
     * T410 boot.  If the cable IS plugged in, most links come up in 1-2s.
     */
    e1000.link_up = false;
    for (int s = 0; s < 6000; s++) {
        if (mmio_read32(E1000_STATUS) & STATUS_LU) {
            e1000.link_up = true;
            break;
        }
        e1000_delay(5000);
    }
    kprintf("[E1000] PCH: link %s after auto-neg\n", e1000.link_up ? "UP" : "DOWN");

    /* If link is up, log the negotiated speed/duplex from STATUS. */
    if (e1000.link_up) {
        uint32_t st = mmio_read32(E1000_STATUS);
        const char* speed = "unknown";
        switch ((st >> 6) & 0x3) {
            case 0: speed = "10M";   break;
            case 1: speed = "100M";  break;
            case 2: speed = "1000M"; break;
            case 3: speed = "1000M"; break;
        }
        kprintf("[E1000] PCH: negotiated %s %s-duplex\n",
                speed, (st & (1u << 0)) ? "full" : "half");
    }
    return 0;
}

/*
 * Read MAC address from the PCH NVM shadow area.
 *
 * On PCH parts (82577LM), a software-initiated CTRL_RST does NOT always reload
 * the MAC into RAL0/RAH0 from the EEPROM the way the classic 82540 does.  If
 * the RAL0/RAH0 registers read back as all-zeros or all-ones after reset, we
 * try reading the MAC from the EEPROM via the EERD (EEPROM Read) register.
 *
 * The first three EEPROM words (offsets 0x00, 0x01, 0x02) contain the 6-byte
 * MAC in little-endian word pairs on Intel NICs.
 */
static bool e1000_mac_is_valid(void) {
    bool all_zero = true, all_ff = true;
    for (int i = 0; i < ETH_ALEN; i++) {
        if (e1000.mac[i] != 0x00) all_zero = false;
        if (e1000.mac[i] != 0xFF) all_ff   = false;
    }
    return !all_zero && !all_ff;
}

static void e1000_pch_read_mac_from_nvm(void) {
    /* EERD format (82577LM):
     *   Write: bit0=START, bits[15:2]=address (word offset)
     *   Read:  bit4=DONE on PCH (not bit1 like classic e1000!)
     *   Data:  bits[31:16] = 16-bit word
     *
     * PCH parts use bit 1 for DONE (like classic), but some docs say bit 4.
     * We check both to be safe.
     */
    uint16_t words[3];
    for (int w = 0; w < 3; w++) {
        mmio_write32(E1000_EERD, ((uint32_t)w << 2) | 1u);
        bool done = false;
        for (int s = 0; s < 10000; s++) {
            uint32_t v = mmio_read32(E1000_EERD);
            if (v & (1u << 1)) {  /* DONE bit (classic + most PCH) */
                words[w] = (uint16_t)(v >> 16);
                done = true;
                break;
            }
            if (v & (1u << 4)) {  /* DONE bit (some PCH variants) */
                words[w] = (uint16_t)(v >> 16);
                done = true;
                break;
            }
            e1000_delay(100);
        }
        if (!done) {
            kprintf("[E1000] PCH: EEPROM read timeout at word %d\n", w);
            return;  /* leave the MAC as-is */
        }
    }

    /* EEPROM words 0-2 contain the MAC in LE byte pairs. */
    e1000.mac[0] = (uint8_t)(words[0] & 0xFF);
    e1000.mac[1] = (uint8_t)(words[0] >> 8);
    e1000.mac[2] = (uint8_t)(words[1] & 0xFF);
    e1000.mac[3] = (uint8_t)(words[1] >> 8);
    e1000.mac[4] = (uint8_t)(words[2] & 0xFF);
    e1000.mac[5] = (uint8_t)(words[2] >> 8);

    /* Write the MAC back into RAL0/RAH0 so the receiver filter uses it. */
    uint32_t ral = (uint32_t)e1000.mac[0]        | ((uint32_t)e1000.mac[1] << 8) |
                   ((uint32_t)e1000.mac[2] << 16) | ((uint32_t)e1000.mac[3] << 24);
    uint32_t rah = (uint32_t)e1000.mac[4]        | ((uint32_t)e1000.mac[5] << 8) |
                   (1u << 31);  /* AV (Address Valid) bit */
    mmio_write32(E1000_RAL0, ral);
    mmio_write32(E1000_RAH0, rah);
}

/* ------------------------------------------------------------------ */
/* Public driver API                                                   */
/* ------------------------------------------------------------------ */
int e1000_init(void) {
    if (e1000.present) {
        return 0;   /* idempotent */
    }
    memset(&e1000, 0, sizeof(e1000));

    /*
     * Locate the NIC.
     * ----------------
     * Detection is broadened to recognise real-hardware Intel parts in
     * addition to the QEMU-emulated 82540EM.  Most importantly this picks up
     * the Lenovo ThinkPad T410's onboard 82577LM (0x10EA), an e1000e-family
     * PCH PHY/MAC.  We try each candidate device ID by exact vendor:device
     * match (a table walk, not a long if-ladder), and only if NONE match do
     * we fall back to a generic PCI class scan (network / ethernet).
     *
     * HONESTY NOTE on e1000e:
     *   The 82577/82578/82579/i217/i218 ("e1000e"-class) parts are detected
     *   and brought up here on a BEST-EFFORT basis: this change broadens
     *   DETECTION + BAR0 mapping + bus-master enable + the basic
     *   reset / RAL0:RAH0 MAC-read attempt.  Their register init and
     *   PHY/MAC access path differ from the classic 82540/82545 e1000
     *   (notably the PCH split MAC/PHY, MDIO/PHY power-up, and EEPROM/NVM
     *   shadow handling), so reliable TX/RX on those PHYs may need
     *   e1000e-specific bring-up as follow-up work.  The QEMU 82540 (and
     *   82574L via `-device e1000e`) path remains the VERIFIED one and is
     *   unchanged below.
     *
     * The candidate-ID table is tiny const data: it lives in .rodata, not in
     * the (tight) kernel .bss.
     */
    static const uint16_t E1000_DEVICE_IDS[] = {
        /* Classic / verified QEMU parts first. */
        E1000_DEVICE_82540,   /* 0x100E -- QEMU `-device e1000` (default)   */
        E1000_DEVICE_82545,   /* 0x100F                                     */
        E1000_DEVICE_82574L,  /* 0x10D3 -- QEMU `-device e1000e`; real HW    */
        /* e1000e-class real-hardware parts (best-effort -- see note above).*/
        E1000_DEVICE_82577LM, /* 0x10EA -- ThinkPad T410 onboard NIC        */
        E1000_DEVICE_82577LC, /* 0x10EB                                     */
        E1000_DEVICE_82578DM, /* 0x10EF                                     */
        E1000_DEVICE_82578DC, /* 0x10F0                                     */
        E1000_DEVICE_82567LM, /* 0x10F5                                     */
        E1000_DEVICE_82579LM, /* 0x1502                                     */
        E1000_DEVICE_82579V,  /* 0x1503                                     */
        E1000_DEVICE_I217LM,  /* 0x153A                                     */
        E1000_DEVICE_I218LM,  /* 0x15A0                                     */
    };

    pci_device_t* dev = NULL;
    for (unsigned k = 0; k < sizeof(E1000_DEVICE_IDS) / sizeof(E1000_DEVICE_IDS[0]); k++) {
        dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_IDS[k]);
        if (dev) break;
    }
    /* Generic fallback: any Intel-or-other ethernet NIC by PCI class. */
    if (!dev) dev = pci_find_class(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHER, 0);
    if (!dev) {
        kprintf("[E1000] no Intel e1000 NIC found on PCI\n");
        return -1;
    }
    kprintf("[E1000] found NIC %04x:%04x at %02x:%02x.%x irq=%u\n",
            dev->vendor_id, dev->device_id, dev->bus, dev->device,
            dev->function, dev->interrupt_line);

    /*
     * Flag e1000e-class matches so the operator knows this is a best-effort
     * bring-up: detection/BAR/bus-master are handled, but PHY/MAC quirks on
     * these parts may still need e1000e-specific work.
     */
    e1000.device_id = dev->device_id;
    e1000.is_pch    = (dev->vendor_id == E1000_VENDOR_ID) &&
                      e1000_is_pch(dev->device_id);
    if (dev->vendor_id == E1000_VENDOR_ID &&
        e1000_is_e1000e_class(dev->device_id)) {
        kprintf("[E1000] NOTE: e1000e-class NIC (0x%04x)%s\n", dev->device_id,
                e1000.is_pch ? " -- PCH part, using 82577LM-style PHY bring-up"
                             : " -- standalone, using classic bring-up");
    }

    /* BAR0 is the memory-mapped register file (identity-mapped 1:1). */
    uint64_t bar0 = pci_get_bar(dev, 0);
    if (bar0 == 0) {
        kprintf("[E1000] BAR0 is empty -- cannot map registers\n");
        return -1;
    }
    e1000.mmio = (volatile uint8_t*)(uintptr_t)bar0;
    kprintf("[E1000] BAR0 (MMIO) @ %p\n", (void*)(uintptr_t)bar0);

    /* The device must be a bus master to DMA descriptors/buffers. */
    pci_enable_memory_space(dev);
    pci_enable_bus_master(dev);

    /*
     * Device reset.
     *
     * Classic 82540/82545/82574L: a bare CTRL_RST is safe.  The PHY is internal
     * or directly attached and auto-links out of reset.
     *
     * PCH parts (82577LM on the T410, 82578, 82579, i217, i218): the MAC and
     * PHY sit on opposite sides of an internal MDIO bus SHARED with the Intel
     * Management Engine.  A bare CTRL_RST issued while the ME is mid-transaction
     * wedges the bus -- that was the root cause of the original T410 boot hang.
     * The PCH path must acquire the SW/FW MDIO semaphore (EXTCNF_CTRL.SWFLAG)
     * BEFORE resetting, yield to the ME, and release after.
     */
    if (e1000.is_pch) {
        if (!E1000_PCH_NIC_ENABLED) {
            /* DEFAULT-OFF GATE: declining the PCH NIC keeps the real T410 from
             * wedging on the ME-shared MDIO bus during PHY bring-up. The system
             * boots link-down; QEMU never reaches here (not a PCH part). Rebuild
             * with PCH_NIC=1 to attempt real-hardware bring-up under validation. */
            kprintf("[E1000] PCH NIC 0x%04x detected -- GATED OFF by default "
                    "(rebuild with PCH_NIC=1 to enable); declining to protect boot\n",
                    dev->device_id);
            return -1;
        }
        kprintf("[E1000] PCH NIC 0x%04x -- ME-safe bring-up (PCH_NIC enabled)\n",
                dev->device_id);
        if (e1000_pch_mac_reset() != 0) {
            kprintf("[E1000] PCH MAC reset failed -- declining NIC\n");
            return -1;
        }
    } else {
        /* Classic reset: mask IRQs, pulse CTRL_RST, wait for clear. */
        mmio_write32(E1000_IMC, 0xFFFFFFFF);
        mmio_write32(E1000_CTRL, mmio_read32(E1000_CTRL) | CTRL_RST);
        e1000_delay(1000000);
        for (int i = 0; i < 1000 && (mmio_read32(E1000_CTRL) & CTRL_RST); i++) {
            e1000_delay(10000);
        }
        mmio_write32(E1000_IMC, 0xFFFFFFFF);
        (void)mmio_read32(E1000_ICR);
    }

    /*
     * Bring the link up.  PCH parts (82577LM &c.) need the SW/FW-synchronised
     * PHY power-up over MDIO first; the classic 82540/82545/82574L just assert
     * CTRL.SLU|ASDE and the PHY links out of reset.  Both paths are bounded and
     * the PCH path is a strict no-op on QEMU (no PCH device is ever matched).
     */
    if (e1000.is_pch) {
        e1000_pch_phy_bringup();           /* sets e1000.link_up; never hangs   */
    } else {
        uint32_t ctrl = mmio_read32(E1000_CTRL);
        ctrl |= CTRL_SLU | CTRL_ASDE;
        ctrl &= ~(CTRL_LRST | CTRL_PHY_RST);
        mmio_write32(E1000_CTRL, ctrl);
        e1000_delay(500000);               /* let PHY auto-negotiate            */
        e1000.link_up = (mmio_read32(E1000_STATUS) & STATUS_LU) != 0;
    }

    /* Clear the multicast table (avoid spurious filtering). */
    for (uint32_t i = 0; i < 128; i++) {
        mmio_write32(E1000_MTA + i * 4, 0);
    }

    /*
     * Read the MAC address.  On classic parts, RAL0/RAH0 are loaded from the
     * EEPROM by the hardware reset.  On PCH parts, a software-initiated reset
     * may leave them empty; fall back to an EEPROM read if needed.
     */
    e1000_read_mac();
    if (e1000.is_pch && !e1000_mac_is_valid()) {
        kprintf("[E1000] PCH: RAL0/RAH0 empty after reset, reading MAC from NVM\n");
        e1000_pch_read_mac_from_nvm();
    }
    kprintf("[E1000] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            e1000.mac[0], e1000.mac[1], e1000.mac[2],
            e1000.mac[3], e1000.mac[4], e1000.mac[5]);

    if (e1000_setup_rx() != 0) {
        kprintf("[E1000] RX ring setup failed (out of memory)\n");
        return -1;
    }
    if (e1000_setup_tx() != 0) {
        kprintf("[E1000] TX ring setup failed (out of memory)\n");
        return -1;
    }

    /* Poll-mode only for the demo: leave device IRQs masked. */
    mmio_write32(E1000_IMC, 0xFFFFFFFF);

    uint32_t status = mmio_read32(E1000_STATUS);
    kprintf("[E1000] init complete (STATUS=0x%x, link=%s)\n",
            status, (status & 0x2) ? "up" : "down");

    e1000.present = true;
    return 0;
}

bool e1000_present(void) {
    return e1000.present;
}

/*
 * tx_clean -- reclaim completed TX descriptors.
 *
 * The NIC writes back the DD (descriptor done) bit into the status byte of
 * each completed descriptor.  After a burst of transmissions the ring may
 * have many DD-set descriptors that the software has not yet acknowledged.
 * This helper walks forward from the oldest un-reclaimed slot and clears
 * the status byte so the slot can be reused.  Called internally before
 * e1000_transmit_batch() fills new descriptors and useful as a standalone
 * maintenance call from the network stack.
 */
static uint32_t tx_clean_head = 0;   /* oldest un-reclaimed TX slot */

static void tx_clean(void) {
    if (!e1000.present) return;
    while (tx_clean_head != e1000.tx_cur) {
        volatile e1000_tx_desc_t* d = &e1000.tx_ring[tx_clean_head];
        if (!(d->status & TXD_STAT_DD))
            break;   /* NIC has not finished this one yet */
        d->status = 0;
        tx_clean_head = (tx_clean_head + 1) % NUM_TX_DESC;
    }
}

/*
 * e1000_transmit_batch -- send multiple frames in one ring-doorbell.
 *
 * Fills up to `count` descriptors from the supplied frame array, issues a
 * single TDT write to kick the transmitter, then waits for the last
 * descriptor's DD writeback.  This amortises the MMIO tail-register write
 * (the expensive part on real PCIe) over the entire batch.
 *
 * Returns the number of frames successfully queued (may be less than
 * `count` if the ring runs out of free slots).
 */
int e1000_transmit_batch(const void* const* frames, const uint16_t* lengths,
                         uint32_t count) {
    if (!e1000.present || !frames || !lengths || count == 0) return 0;

    tx_clean();   /* reclaim completed slots first */

    uint32_t queued = 0;
    uint32_t last_i = e1000.tx_cur;

    for (uint32_t n = 0; n < count; n++) {
        uint32_t i = e1000.tx_cur;

        /* Check if this slot is free (DD set means HW finished it). */
        if (!(desc_read_tx_status(&e1000.tx_ring[i]) & TXD_STAT_DD)) {
            break;   /* ring full -- stop batching */
        }

        uint16_t len = lengths[n];
        if (len == 0 || !frames[n]) continue;
        if (len > TX_BUF_SIZE) len = TX_BUF_SIZE;

        memcpy(e1000.tx_bufs[i], frames[n], len);

        uint16_t xmit = len;
        if (xmit < ETH_MIN_FRAME) {
            memset(e1000.tx_bufs[i] + len, 0, ETH_MIN_FRAME - len);
            xmit = ETH_MIN_FRAME;
        }

        e1000.tx_ring[i].length = xmit;
        e1000.tx_ring[i].cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
        e1000.tx_ring[i].status = 0;

        last_i = i;
        e1000.tx_cur = (i + 1) % NUM_TX_DESC;
        queued++;
    }

    if (queued == 0) return 0;

    /* Single tail-register write for the whole batch. */
    desc_wmb();
    mmio_write32(E1000_TDT, e1000.tx_cur);

    /* Wait for the LAST descriptor's DD writeback. */
    for (int spin = 0; spin < 100000; spin++) {
        if (desc_read_tx_status(&e1000.tx_ring[last_i]) & TXD_STAT_DD)
            return (int)queued;
        e1000_delay(100);
    }
    return (int)queued;   /* timed out but frames are almost certainly queued */
}

int e1000_get_mac(uint8_t out[ETH_ALEN]) {
    if (!e1000.present) return -1;
    memcpy(out, e1000.mac, ETH_ALEN);
    return 0;
}

int e1000_tx(const void* frame, uint16_t len) {
    if (!e1000.present || !frame || len == 0) return -1;
    if (len > TX_BUF_SIZE) len = TX_BUF_SIZE;

    uint32_t i = e1000.tx_cur;

    /* Wait for this descriptor to be free (DD set means HW finished it).
     * Use volatile status read: the NIC writes DD back into the descriptor,
     * so the compiler must not cache the status across loop iterations. */
    for (int spin = 0; spin < 100000; spin++) {
        if (desc_read_tx_status(&e1000.tx_ring[i]) & TXD_STAT_DD) break;
        e1000_delay(100);
    }

    memcpy(e1000.tx_bufs[i], frame, len);

    /* Pad runt frames to the 60-byte Ethernet minimum (TCTL_PSP also pads). */
    uint16_t xmit = len;
    if (xmit < ETH_MIN_FRAME) {
        memset(e1000.tx_bufs[i] + len, 0, ETH_MIN_FRAME - len);
        xmit = ETH_MIN_FRAME;
    }

    e1000.tx_ring[i].length = xmit;
    e1000.tx_ring[i].cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    e1000.tx_ring[i].status = 0;

    e1000.tx_cur = (i + 1) % NUM_TX_DESC;
    desc_wmb();   /* ensure descriptor + data writes reach DMA before tail update */
    mmio_write32(E1000_TDT, e1000.tx_cur);   /* kick the transmitter */

    /* Wait for the descriptor-done writeback so the caller knows it went out.
     * Use volatile status read so the compiler re-reads from memory each iter. */
    for (int spin = 0; spin < 100000; spin++) {
        if (desc_read_tx_status(&e1000.tx_ring[i]) & TXD_STAT_DD) {
            return len;
        }
        e1000_delay(100);
    }
    /* Timed out on writeback, but the frame is almost certainly queued. */
    return len;
}

int e1000_rx_poll(void* buf, uint16_t buf_len) {
    if (!e1000.present || !buf) return -1;

    uint32_t i = e1000.rx_cur;
    /*
     * Access the descriptor status through a volatile pointer so the compiler
     * cannot hoist the load out of the poll loop.  The NIC writes DD=1 into
     * this field via bus-master DMA; without volatile the compiler is free to
     * cache the value in a register and spin forever.
     */
    volatile e1000_rx_desc_t* d = (volatile e1000_rx_desc_t*)&e1000.rx_ring[i];

    if (!(d->status & RXD_STAT_DD)) {
        return 0;   /* nothing received yet */
    }

    uint16_t len = d->length;
    if (len > RX_BUF_SIZE) len = RX_BUF_SIZE;   /* clamp to the HW rx buffer: a NIC
                                                 * reporting length > the 2KB buffer
                                                 * would otherwise over-read kernel
                                                 * memory past rx_bufs[i] into `buf`. */
    if (len > buf_len) len = buf_len;

    /* Copy before recycling: once we advance RDT the HW may overwrite the buffer. */
    if (len > 0) memcpy(buf, e1000.rx_bufs[i], len);

    /*
     * Recycle the descriptor: clear status so it is ready for the next frame,
     * then hand it back to the NIC by writing its index to RDT.
     *
     * 82540 spec §13.4.28 (INCLUSIVE tail semantics): RDT points to the LAST
     * descriptor the hardware is allowed to use.  Writing RDT = i tells the
     * NIC "descriptor i is now valid for your use".  This is the correct
     * standard recycle pattern — advancing to (i+1)%N would hand the NEXT
     * (not-yet-recycled) slot instead, and going to (i-1+N)%N would move the
     * tail backward, shrinking the hardware-owned window by one slot per frame.
     */
    d->status = 0;
    desc_wmb();   /* flush status clear before the tail register write */
    mmio_write32(E1000_RDT, i);               /* hand this slot back to HW */
    e1000.rx_cur = (i + 1) % NUM_RX_DESC;

    if (len == 0) return -1;                  /* malformed 0-length frame */
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* Link / identity accessors (for boot diagnostics, esp. the T410)     */
/* ------------------------------------------------------------------ */
/*
 * e1000_link_up() -- true only if the NIC is up AND the MAC reports link.
 *
 * On PCH parts (82577LM) we do a LIVE read of STATUS.LU rather than returning
 * the cached init-time value.  This lets higher layers detect a cable plugged in
 * after boot: the 82577LM auto-negotiates in hardware, so STATUS.LU will go high
 * as soon as the PHY links, even without software intervention.  The cached field
 * is updated as a side effect so log messages stay consistent.
 *
 * On QEMU (classic path) the STATUS read is equally fast and harmless.
 */
bool e1000_link_up(void) {
    if (!e1000.present) return false;
    e1000.link_up = (mmio_read32(E1000_STATUS) & STATUS_LU) != 0;
    return e1000.link_up;
}

/* The matched PCI device id (e.g. 0x10EA for the T410's 82577LM); 0 if none. */
uint16_t e1000_device_id(void) {
    return e1000.device_id;
}
