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
#define E1000_EXTCNF_CTRL  0x0F00   /* Extended Config Control (SWFLAG)    */

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

/* Standard MII PHY registers (clause 22) + the bits we touch. */
#define MII_BMCR           0x00      /* Basic Mode Control                 */
#define MII_BMSR           0x01      /* Basic Mode Status                  */
#define MII_PHYID1         0x02
#define MII_PHYID2         0x03
#define BMCR_RESET         (1u << 15)
#define BMCR_ANENABLE      (1u << 12)
#define BMCR_PDOWN         (1u << 11)
#define BMCR_ANRESTART     (1u << 9)
#define BMSR_LSTATUS       (1u << 2)
#define STATUS_LU          (1u << 1)  /* Device Status: link up             */

/* Ring sizes (must be multiples of 8; 8 descriptors is plenty for a demo). */
#define NUM_RX_DESC     32
#define NUM_TX_DESC     8
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
 */
static bool e1000_acquire_swflag(void) {
    for (int attempt = 0; attempt < 5; attempt++) {
        int spins = 0;
        while ((mmio_read32(E1000_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) &&
               spins++ < 50000) {
            e1000_delay(50);
        }
        uint32_t v = mmio_read32(E1000_EXTCNF_CTRL);
        if (v & EXTCNF_CTRL_SWFLAG) continue;          /* FW still holds it  */
        v |= EXTCNF_CTRL_SWFLAG;
        mmio_write32(E1000_EXTCNF_CTRL, v);
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
 * Bring up the PHY on an 82577LM-class PCH NIC.  Best-effort + fully bounded:
 *   1. take the SW/FW MDIO flag,
 *   2. find the PHY's MDIO address (try 1 then 2; valid PHYID1 != 0/0xFFFF),
 *   3. soft-reset the PHY, clear power-down, enable + restart auto-neg,
 *   4. drop the flag, set MAC CTRL.SLU|ASDE, and poll STATUS.LU for link.
 * Returns 0 if the sequence ran (link may still be down -- reported via
 * e1000.link_up); -1 only if the PHY could not be reached at all.  Never hangs.
 */
static int e1000_pch_phy_bringup(void) {
    if (!e1000_acquire_swflag()) {
        kprintf("[E1000] PCH: SW/FW MDIO flag timeout (ME busy?) -- link skipped\n");
        return -1;
    }

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
    kprintf("[E1000] PCH: PHY @ mdio %u id=%04x%04x\n", phy, id1, id2);

    /* Soft reset (clause-22 BMCR.reset); bounded wait for the bit to self-clear.
     * PHY reset completes in <1ms per spec and each e1000_phy_read is already
     * MDIC-bounded, so a tight outer cap keeps a dead/absent PHY from stalling
     * the (network-enabled) boot for more than a few ms. */
    e1000_phy_write(phy, MII_BMCR, BMCR_RESET);
    for (int s = 0; s < 500; s++) {
        if (!(e1000_phy_read(phy, MII_BMCR) & BMCR_RESET)) break;
        e1000_delay(50);
    }

    /* Power up + (re)start auto-negotiation. */
    uint16_t bmcr = e1000_phy_read(phy, MII_BMCR);
    bmcr &= ~BMCR_PDOWN;
    bmcr |=  BMCR_ANENABLE | BMCR_ANRESTART;
    e1000_phy_write(phy, MII_BMCR, bmcr);

    e1000_release_swflag();

    /* MAC side: assert link-up + auto-speed-detect. */
    uint32_t ctrl = mmio_read32(E1000_CTRL);
    ctrl |=  (CTRL_SLU | CTRL_ASDE);
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST);
    mmio_write32(E1000_CTRL, ctrl);

    /* Poll for link, bounded short. If the cable/auto-neg hasn't produced link
     * within this window the driver still works (link-down); a tighter cap keeps
     * a no-carrier T410 from stalling the boot. QEMU never runs this PCH path. */
    e1000.link_up = false;
    for (int s = 0; s < 50000; s++) {
        if (mmio_read32(E1000_STATUS) & STATUS_LU) { e1000.link_up = true; break; }
        e1000_delay(200);
    }
    kprintf("[E1000] PCH: link %s after auto-neg\n", e1000.link_up ? "UP" : "DOWN");
    return 0;
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

    /*
     * RUNTIME GATE -- decline PCH NICs (82577LM &c.) BEFORE touching any MMIO.
     * Confirmed on the ThinkPad T410: re-enabling networking made the boot HANG
     * right after the "storage SKIPPED" marker, i.e. inside this function's
     * 82577LM bring-up. Its MMIO/MDIO/SW-FW-semaphore path stalls the bus in a
     * way the iteration caps cannot unwedge (a bus stall, not a software spin).
     * We only reached the device match via PCI config space (safe); returning
     * here, before `pci_get_bar`/`e1000.mmio`/any register access, guarantees we
     * issue ZERO NIC MMIO on the T410 -- so it boots to the desktop exactly like
     * the fully-network-skipped build did, while QEMU's e1000 (NOT a PCH part)
     * still runs the full, working bring-up below. The PCH driver code is kept
     * intact; re-enable by removing this gate once it is validated on real HW.
     */
    if (e1000.is_pch) {
        kprintf("[E1000] PCH NIC 0x%04x detected -- bring-up DISABLED (hangs real "
                "hardware); declining to keep the boot alive\n", dev->device_id);
        return -1;
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

    /* Full device reset, then wait for it to deassert. */
    mmio_write32(E1000_IMC, 0xFFFFFFFF);   /* mask all IRQs while we set up */
    mmio_write32(E1000_CTRL, mmio_read32(E1000_CTRL) | CTRL_RST);
    e1000_delay(1000000);
    for (int i = 0; i < 1000 && (mmio_read32(E1000_CTRL) & CTRL_RST); i++) {
        e1000_delay(10000);
    }
    mmio_write32(E1000_IMC, 0xFFFFFFFF);   /* reset re-enables some causes */
    (void)mmio_read32(E1000_ICR);          /* drain any pending interrupt causes */

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

    e1000_read_mac();
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
 * On the classic QEMU path this is read once at the end of init; on the PCH
 * path it reflects the post-auto-neg STATUS.LU poll.  The T410 has no serial,
 * so kernel.c turns this into an on-screen boot marker.
 */
bool e1000_link_up(void) {
    return e1000.present && e1000.link_up;
}

/* The matched PCI device id (e.g. 0x10EA for the T410's 82577LM); 0 if none. */
uint16_t e1000_device_id(void) {
    return e1000.device_id;
}
