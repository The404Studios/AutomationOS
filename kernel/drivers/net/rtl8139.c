/*
 * Realtek RTL8139 (RTL8139C/C+) Ethernet driver -- QEMU `-device rtl8139`.
 * ========================================================================
 *
 * Target: PCI vendor 0x10EC, device 0x8139, class 0x02 (network) subclass
 * 0x00 (ethernet).  This is one of the most widely emulated and historically
 * common NICs: QEMU's `-device rtl8139`, VirtualBox, VMware's legacy adapter,
 * and a huge installed base of cheap real PCI / CardBus Fast-Ethernet cards.
 * Adding it BROADENS this kernel's hardware support beyond the Intel e1000.
 *
 * This file mirrors kernel/drivers/net/e1000.c's structure and contract so the
 * net layer can drive either NIC through an identical 5-call API
 * (rtl8139_init/_present/_get_mac/_tx/_rx_poll).
 *
 * ---------------------------------------------------------------------------
 * BAR / I-O approach
 * ---------------------------------------------------------------------------
 * The RTL8139 exposes its register file twice:
 *   BAR0 = IOAR  -- a PORT-I/O window.
 *   BAR1 = MEMAR -- an MMIO window (a mirror of the same registers).
 * The classic, best-documented RTL8139 bring-up uses PORT I/O, and this kernel
 * already ships inb/outb/inw/outw/inl/outl (kernel/include/x86_64.h) which the
 * UHCI driver uses the same way for its I/O-BAR controller.  So we use BAR0
 * (port I/O) by default and only fall back to BAR1 (MMIO, accessed through
 * volatile reads/writes on the identity-mapped base) if BAR0 is not an
 * I/O-space BAR.  All register accessors (reg_r8/r16/r32, reg_w8/w16/w32)
 * dispatch on that choice at run time.
 *
 * ---------------------------------------------------------------------------
 * DMA / .bss-safety  (same pattern as e1000.c)
 * ---------------------------------------------------------------------------
 * The chip is a 32-bit bus master.  Every DMA buffer comes from the PMM
 * (pmm_alloc_page / pmm_alloc_pages); because the kernel identity-maps all
 * physical RAM 1:1, a PMM pointer IS its physical/DMA address.  There are NO
 * large static arrays in this file -- the RX ring buffer and the 4 TX buffers
 * are all PMM allocations -- so the driver stays .bss-safe just like e1000.c.
 * Because RBSTART / TSAD are 32-bit, we verify each allocation sits below the
 * 4 GiB line and bail (cleanly, returning <0) if it does not.
 *
 * ---------------------------------------------------------------------------
 * Bring-up sequence (this file)
 * ---------------------------------------------------------------------------
 *   1. pci_find_device(0x10EC,0x8139) (or generic ethernet-class fallback).
 *   2. Enable I/O (or memory) space + bus-master in PCI COMMAND.
 *   3. Map BAR0 (port I/O) -- or BAR1 (MMIO) if BAR0 is not I/O.
 *   4. Power on: CONFIG1 = 0x00 (LWAKE + LWPTN clear -> not in low power).
 *   5. Soft reset: CMD = RST(0x10); spin until the RST bit self-clears.
 *   6. Allocate the RX ring buffer (8K + 16 + 1500 slack) from the PMM and
 *      write its 32-bit physical address to RBSTART.
 *   7. IMR = 0, ISR = 0xFFFF (poll mode: mask & ack all interrupt causes).
 *   8. RCR = accept BCAST+PHYS-match+MCAST, WRAP, 8K(+16) ring, max RX burst.
 *   9. TCR = max TX DMA burst, normal IFG.
 *  10. CMD = RE | TE (enable receiver + transmitter).
 *  11. Read the MAC from IDR0..IDR5.
 *  TX: round-robin over the 4 TSAD/TSD descriptors, spin on TOK.
 *  RX: read the 4-byte RX header at the CAPR cursor, copy the frame, advance
 *      CAPR (4-byte aligned, wrapping); BUFE in CMD reports "ring empty".
 *
 * Scope: kernel/drivers/net/rtl8139.c only.  Does NOT edit net.c, e1000.c, or
 * any build script.  Wiring this as a fallback NIC is the integrator's job
 * (see the INTEGRATOR NOTE in kernel/include/rtl8139.h).
 */

#include "../../include/rtl8139.h"
#include "../../include/pci.h"
#include "../../include/mem.h"       /* pmm_alloc_page, pmm_alloc_pages       */
#include "../../include/types.h"
#include "../../include/kernel.h"    /* kprintf, PAGE_SIZE                     */
#include "../../include/string.h"    /* memset, memcpy                         */
#include "../../include/x86_64.h"    /* inb/outb/inw/outw/inl/outl            */

/* ------------------------------------------------------------------ */
/* PCI identity                                                        */
/* ------------------------------------------------------------------ */
#define RTL8139_VENDOR_ID   0x10EC
#define RTL8139_DEVICE_ID   0x8139

#define PCI_CLASS_NETWORK   0x02
#define PCI_SUBCLASS_ETHER  0x00

/* COMMAND-register bit for enabling I/O-space decoding (pci.h has the
 * MEMORY/BUS_MASTER helpers but no I/O-space enable, so we set it directly). */
#define RTL_PCI_CMD_IO_SPACE  0x0001

/* ------------------------------------------------------------------ */
/* RTL8139 register offsets (from the BAR base)                        */
/* ------------------------------------------------------------------ */
#define RTL_IDR0        0x00   /* ID (MAC) register 0..5 (6 bytes)         */
#define RTL_MAR0        0x08   /* Multicast register 0..7 (8 bytes)        */
#define RTL_TSD0        0x10   /* Transmit Status of Descriptor 0 (dword)  */
#define RTL_TSAD0       0x20   /* Transmit Start Address Desc 0 (dword)    */
#define RTL_RBSTART     0x30   /* Receive (Rx) Buffer Start Address (dword)*/
#define RTL_CMD         0x37   /* Command register (byte)                  */
#define RTL_CAPR        0x38   /* Current Address of Packet Read (word)    */
#define RTL_CBR         0x3A   /* Current Buffer Address (word)            */
#define RTL_IMR         0x3C   /* Interrupt Mask Register (word)           */
#define RTL_ISR         0x3E   /* Interrupt Status Register (word)         */
#define RTL_TCR         0x40   /* Transmit Configuration Register (dword)  */
#define RTL_RCR         0x44   /* Receive Configuration Register (dword)   */
#define RTL_CONFIG1     0x52   /* Configuration register 1 (byte)          */

/* TSDn (per-descriptor) -- a stride of 4 between TSD0..TSD3 / TSAD0..TSAD3. */
#define RTL_TSD(n)      (RTL_TSD0  + (n) * 4)
#define RTL_TSAD(n)     (RTL_TSAD0 + (n) * 4)

/* Command register (CMD, 0x37) bits. */
#define CMD_BUFE        (1u << 0)   /* Rx buffer empty (1 = nothing to read)*/
#define CMD_TE          (1u << 2)   /* Transmitter enable                   */
#define CMD_RE          (1u << 3)   /* Receiver enable                      */
#define CMD_RST         (1u << 4)   /* Software reset (self-clears)         */

/* Interrupt Status/Mask (ISR/IMR, 0x3E/0x3C) bits. */
#define INT_ROK         (1u << 0)   /* Receive OK                           */
#define INT_RER         (1u << 1)   /* Receive error                        */
#define INT_TOK         (1u << 2)   /* Transmit OK                          */
#define INT_TER         (1u << 3)   /* Transmit error                       */
#define INT_RXOVW       (1u << 4)   /* Rx buffer overflow                   */

/* Transmit Status of Descriptor (TSDn, 0x10..0x1C). */
#define TSD_SIZE_MASK   0x1FFF      /* bits[12:0] = frame size to transmit  */
#define TSD_OWN         (1u << 13)  /* OWN: 0 while DMA in progress         */
#define TSD_TUN         (1u << 14)  /* Transmit FIFO underrun               */
#define TSD_TOK         (1u << 15)  /* Transmit OK (frame fully sent)       */
#define TSD_ERTXTH_SHIFT 16         /* Early-Tx threshold (bits[21:16])     */

/* Receive Configuration Register (RCR, 0x44). */
#define RCR_AAP         (1u << 0)   /* Accept All Packets (promiscuous)     */
#define RCR_APM         (1u << 1)   /* Accept Physical Match (our MAC)      */
#define RCR_AM          (1u << 2)   /* Accept Multicast                     */
#define RCR_AB          (1u << 3)   /* Accept Broadcast                     */
#define RCR_WRAP        (1u << 7)   /* Wrap: do NOT wrap mid-packet         */
/* RBLEN (bits[12:11]) selects the Rx ring size:
 *   00 = 8K+16, 01 = 16K+16, 10 = 32K+16, 11 = 64K+16. We use 8K+16. */
#define RCR_RBLEN_8K    (0u << 11)
/* MXDMA (bits[10:8]) = max Rx DMA burst; 0b111 = unlimited (1024 bytes). */
#define RCR_MXDMA_UNLIM (7u << 8)
/* RXFTH (bits[15:13]) = Rx FIFO threshold; 0b111 = no threshold (whole pkt).*/
#define RCR_RXFTH_NONE  (7u << 13)

/* Transmit Configuration Register (TCR, 0x40). */
/* MXDMA (bits[10:8]) = max Tx DMA burst; 0b110 = 1024 bytes. */
#define TCR_MXDMA_1024  (6u << 8)
/* IFG (bits[25:24]) = interframe gap; 0b11 = the IEEE 802.3 standard 9.6us. */
#define TCR_IFG_STD     (3u << 24)

/* ------------------------------------------------------------------ */
/* RX-ring sizing                                                      */
/* ------------------------------------------------------------------ */
/*
 * RBLEN=00 selects an 8 KiB ring.  The chip can DMA up to ~1500 bytes PAST the
 * nominal end of the ring (because WRAP=1 means it never splits a packet), so
 * the OSDev-canonical allocation is 8192 + 16 + 1500.  We round that up to a
 * whole number of 4 KiB pages and allocate a contiguous run from the PMM so it
 * is a single identity-mapped (== physical) DMA region.
 */
#define RTL_RX_RING_SIZE   8192                       /* RBLEN=00 -> 8K       */
#define RTL_RX_PAD         (16 + 1500)                /* header + wrap slack  */
#define RTL_RX_ALLOC       (RTL_RX_RING_SIZE + RTL_RX_PAD)
#define RTL_RX_PAGES       ((RTL_RX_ALLOC + PAGE_SIZE - 1) / PAGE_SIZE)

/* The RTL8139 has exactly 4 hardware TX descriptors. */
#define RTL_NUM_TX_DESC    4
#define RTL_TX_BUF_SIZE    PAGE_SIZE                  /* one page per TX slot */

/* Per-packet RX header is 4 bytes (status:16, length:16). */
#define RTL_RX_HDR_LEN     4
/* The 16-bit length in the RX header INCLUDES the trailing 4-byte CRC. */
#define RTL_RX_CRC_LEN     4

/* RX-header status bit: ROK (this packet received OK). */
#define RXSTAT_ROK         (1u << 0)

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */
static struct {
    bool             present;
    bool             use_mmio;        /* true: BAR1 MMIO; false: BAR0 port-IO */
    uint16_t         io_base;         /* port-I/O base (when !use_mmio)       */
    volatile uint8_t* mmio;           /* identity-mapped MMIO base (use_mmio) */

    uint8_t          mac[ETH_ALEN];

    uint8_t*         rx_buf;          /* PMM RX ring buffer (RBSTART)         */
    uint32_t         rx_offset;       /* software read cursor into rx_buf     */

    uint8_t*         tx_buf[RTL_NUM_TX_DESC];
    uint32_t         tx_cur;          /* next TX descriptor (round-robin 0..3)*/
} rtl;

/* ------------------------------------------------------------------ */
/* Register accessors -- dispatch port-I/O vs MMIO at run time         */
/* ------------------------------------------------------------------ */
static inline void reg_w8(uint16_t reg, uint8_t v) {
    if (rtl.use_mmio) *(volatile uint8_t*)(rtl.mmio + reg) = v;
    else              outb((uint16_t)(rtl.io_base + reg), v);
}
static inline void reg_w16(uint16_t reg, uint16_t v) {
    if (rtl.use_mmio) *(volatile uint16_t*)(rtl.mmio + reg) = v;
    else              outw((uint16_t)(rtl.io_base + reg), v);
}
static inline void reg_w32(uint16_t reg, uint32_t v) {
    if (rtl.use_mmio) *(volatile uint32_t*)(rtl.mmio + reg) = v;
    else              outl((uint16_t)(rtl.io_base + reg), v);
}
static inline uint8_t reg_r8(uint16_t reg) {
    if (rtl.use_mmio) return *(volatile uint8_t*)(rtl.mmio + reg);
    return inb((uint16_t)(rtl.io_base + reg));
}
static inline uint16_t reg_r16(uint16_t reg) {
    if (rtl.use_mmio) return *(volatile uint16_t*)(rtl.mmio + reg);
    return inw((uint16_t)(rtl.io_base + reg));
}
static inline uint32_t reg_r32(uint16_t reg) {
    if (rtl.use_mmio) return *(volatile uint32_t*)(rtl.mmio + reg);
    return inl((uint16_t)(rtl.io_base + reg));
}

/*
 * Ensure prior CPU writes to a DMA buffer are globally visible before the
 * register poke that hands the buffer to the chip's DMA engine.  On x86 (TSO)
 * a compiler barrier suffices for WB memory + port I/O ordering; sfence also
 * closes the WC-store reorder window for the MMIO path.  Mirrors e1000.c's
 * desc_wmb().
 */
static inline void dma_wmb(void) {
    asm volatile("sfence" ::: "memory");
}

/*
 * Crude busy-wait -- the PIT timer is not assumed live during NIC bring-up,
 * so we spin on the `pause` hint (e1000.c does the same).  Precision is
 * irrelevant; we only need the chip a few microseconds to settle.
 */
static void rtl_delay(volatile uint32_t loops) {
    while (loops--) {
        asm volatile("pause");
    }
}

/* The chip's RBSTART / TSAD are 32-bit, so every DMA buffer must be < 4 GiB. */
static inline bool dma_addr_ok(void* p) {
    return ((uint64_t)(uintptr_t)p) <= 0xFFFFFFFFull;
}

/* ------------------------------------------------------------------ */
/* MAC address                                                         */
/* ------------------------------------------------------------------ */
/*
 * The MAC is loaded from the chip's serial EEPROM into IDR0..IDR5 at power-on;
 * QEMU seeds it (52:54:00:12:34:56 by default).  We read it as two dwords.
 */
static void rtl_read_mac(void) {
    uint32_t lo = reg_r32(RTL_IDR0);
    uint32_t hi = reg_r32(RTL_IDR0 + 4);
    rtl.mac[0] = (uint8_t)(lo & 0xFF);
    rtl.mac[1] = (uint8_t)((lo >> 8) & 0xFF);
    rtl.mac[2] = (uint8_t)((lo >> 16) & 0xFF);
    rtl.mac[3] = (uint8_t)((lo >> 24) & 0xFF);
    rtl.mac[4] = (uint8_t)(hi & 0xFF);
    rtl.mac[5] = (uint8_t)((hi >> 8) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* RX / TX buffer setup                                                */
/* ------------------------------------------------------------------ */
static int rtl_setup_rx(void) {
    /* One contiguous identity-mapped DMA region for the whole ring + slack. */
    void* ring = pmm_alloc_pages(RTL_RX_PAGES);
    if (!ring) {
        kprintf("[RTL8139] RX ring alloc failed (%u pages)\n", RTL_RX_PAGES);
        return -1;
    }
    if (!dma_addr_ok(ring)) {
        kprintf("[RTL8139] RX ring above 4GiB (%p) -- chip can't DMA there\n",
                ring);
        pmm_free_pages(ring, RTL_RX_PAGES);
        return -1;
    }
    memset(ring, 0, RTL_RX_PAGES * PAGE_SIZE);
    rtl.rx_buf    = (uint8_t*)ring;
    rtl.rx_offset = 0;

    uint32_t phys = (uint32_t)(uintptr_t)ring;   /* identity-mapped */
    dma_wmb();
    reg_w32(RTL_RBSTART, phys);
    return 0;
}

static int rtl_setup_tx(void) {
    for (uint32_t i = 0; i < RTL_NUM_TX_DESC; i++) {
        void* buf = pmm_alloc_page();
        if (!buf || !dma_addr_ok(buf)) {
            if (buf) pmm_free_page(buf);
            kprintf("[RTL8139] TX buffer %u alloc failed / above 4GiB\n", i);
            return -1;
        }
        memset(buf, 0, PAGE_SIZE);
        rtl.tx_buf[i] = (uint8_t*)buf;
        /* Pre-program TSAD with the (static) physical buffer address. */
        reg_w32(RTL_TSAD(i), (uint32_t)(uintptr_t)buf);
    }
    rtl.tx_cur = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public driver API                                                   */
/* ------------------------------------------------------------------ */
int rtl8139_init(void) {
    if (rtl.present) {
        return 0;   /* idempotent */
    }
    memset(&rtl, 0, sizeof(rtl));

    /* Locate the NIC: exact RTL8139 ID first, generic ethernet class second. */
    pci_device_t* dev = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!dev) {
        dev = pci_find_class(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHER, 0);
        /* Only accept the class fallback if it is actually a Realtek 8139. */
        if (dev && !(dev->vendor_id == RTL8139_VENDOR_ID &&
                     dev->device_id == RTL8139_DEVICE_ID)) {
            dev = NULL;
        }
    }
    if (!dev) {
        kprintf("[RTL8139] no RTL8139 NIC found on PCI (10EC:8139)\n");
        return -1;
    }
    kprintf("[RTL8139] found NIC %04x:%04x at %02x:%02x.%x irq=%u\n",
            dev->vendor_id, dev->device_id, dev->bus, dev->device,
            dev->function, dev->interrupt_line);

    /*
     * Choose the register window.  BAR0 is the I/O-port window (preferred,
     * matches classic RTL8139 bring-up + this kernel's UHCI idiom); BAR1 is
     * the MMIO mirror.  dev->bar[i] holds the RAW BAR with its low type bits,
     * so we can test bit0 (1 = I/O space) before deciding.
     */
    uint64_t bar0 = pci_get_bar(dev, 0);
    uint64_t bar1 = pci_get_bar(dev, 1);
    if (bar0 != 0 && (dev->bar[0] & PCI_BAR_TYPE_IO)) {
        rtl.use_mmio = false;
        rtl.io_base  = (uint16_t)bar0;
        kprintf("[RTL8139] using BAR0 port-I/O @ 0x%x\n", rtl.io_base);
    } else if (bar1 != 0 && !(dev->bar[1] & PCI_BAR_TYPE_IO)) {
        rtl.use_mmio = true;
        rtl.mmio     = (volatile uint8_t*)(uintptr_t)bar1;
        kprintf("[RTL8139] using BAR1 MMIO @ %p\n", (void*)(uintptr_t)bar1);
    } else if (bar0 != 0) {
        /* Last resort: treat BAR0 as port I/O even without the type bit. */
        rtl.use_mmio = false;
        rtl.io_base  = (uint16_t)bar0;
        kprintf("[RTL8139] BAR type ambiguous; assuming BAR0 port-I/O @ 0x%x\n",
                rtl.io_base);
    } else {
        kprintf("[RTL8139] no usable BAR (BAR0/BAR1 empty)\n");
        return -1;
    }

    /*
     * Enable the proper address space + bus-master in the PCI COMMAND register.
     * pci.h gives us bus-master and memory-space helpers; the I/O-space enable
     * bit has no helper, so we set it via the exported config-space writers.
     */
    if (rtl.use_mmio) {
        pci_enable_memory_space(dev);
    } else {
        uint16_t cmd = pci_config_read_word(dev->bus, dev->device, dev->function,
                                            PCI_CONFIG_COMMAND);
        cmd |= RTL_PCI_CMD_IO_SPACE;
        pci_config_write_word(dev->bus, dev->device, dev->function,
                              PCI_CONFIG_COMMAND, cmd);
    }
    pci_enable_bus_master(dev);

    /* Power on: clear CONFIG1 (de-assert LWAKE/LWPTN low-power signals). */
    reg_w8(RTL_CONFIG1, 0x00);

    /* Software reset: set RST and spin until the chip clears it. */
    reg_w8(RTL_CMD, CMD_RST);
    bool reset_done = false;
    for (int i = 0; i < 100000; i++) {
        if (!(reg_r8(RTL_CMD) & CMD_RST)) {
            reset_done = true;
            break;
        }
        rtl_delay(100);
    }
    if (!reset_done) {
        kprintf("[RTL8139] soft reset did not complete (CMD.RST stuck)\n");
        return -1;
    }

    /* Mask all interrupts and ack any pending causes (poll-mode only). */
    reg_w16(RTL_IMR, 0x0000);
    reg_w16(RTL_ISR, 0xFFFF);

    /* Allocate + program the RX ring buffer (RBSTART). */
    if (rtl_setup_rx() != 0) {
        return -1;
    }

    /* Allocate the 4 TX buffers and pre-load TSAD0..3. */
    if (rtl_setup_tx() != 0) {
        return -1;
    }

    /*
     * RCR: accept broadcast + our-physical-match + multicast, WRAP set (never
     * split a packet across the ring end -> our +1500 slack covers the spill),
     * 8K+16 ring (RBLEN=00), unlimited RX DMA burst, no FIFO threshold.
     * (We deliberately do NOT set AAP/promiscuous.)
     */
    reg_w32(RTL_RCR,
            RCR_AB | RCR_APM | RCR_AM | RCR_WRAP |
            RCR_RBLEN_8K | RCR_MXDMA_UNLIM | RCR_RXFTH_NONE);

    /* TCR: max DMA burst + standard interframe gap. */
    reg_w32(RTL_TCR, TCR_MXDMA_1024 | TCR_IFG_STD);

    /* Enable receiver + transmitter. */
    reg_w8(RTL_CMD, CMD_RE | CMD_TE);

    /* Keep interrupts masked after enabling RX/TX (poll-mode). */
    reg_w16(RTL_IMR, 0x0000);

    /* Read the MAC now that the chip is up. */
    rtl_read_mac();
    kprintf("[RTL8139] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            rtl.mac[0], rtl.mac[1], rtl.mac[2],
            rtl.mac[3], rtl.mac[4], rtl.mac[5]);

    kprintf("[RTL8139] init complete (RX ring=%u bytes, %u TX bufs, %s)\n",
            RTL_RX_RING_SIZE, RTL_NUM_TX_DESC,
            rtl.use_mmio ? "MMIO" : "port-I/O");

    rtl.present = true;
    return 0;
}

int rtl8139_present(void) {
    return rtl.present ? 1 : 0;
}

int rtl8139_get_mac(unsigned char out[6]) {
    if (!rtl.present || !out) return -1;
    memcpy(out, rtl.mac, ETH_ALEN);
    return 0;
}

int rtl8139_tx(const void* frame, unsigned short len) {
    if (!rtl.present || !frame || len == 0) return -1;
    if (len > RTL_TX_BUF_SIZE) len = RTL_TX_BUF_SIZE;

    uint32_t i = rtl.tx_cur;

    /*
     * Wait for this descriptor to be free.  TSD.OWN is 0 while the DMA is in
     * progress and the chip sets it back to 1 when the buffer is available
     * again; after a completed send TOK is also set.  We treat "OWN set or
     * TOK set" as free.  Re-read through the volatile accessor each spin.
     */
    for (int spin = 0; spin < 100000; spin++) {
        uint32_t tsd = reg_r32(RTL_TSD(i));
        if ((tsd & TSD_OWN) || (tsd & TSD_TOK)) break;
        rtl_delay(100);
    }

    /* Copy the frame into this slot's DMA buffer. */
    memcpy(rtl.tx_buf[i], frame, len);

    /* Pad runt frames to the 60-byte Ethernet minimum. */
    uint16_t xmit = len;
    if (xmit < 60) {
        memset(rtl.tx_buf[i] + len, 0, (uint32_t)(60 - len));
        xmit = 60;
    }

    /*
     * TSAD was pre-loaded in rtl_setup_tx() and the buffer address is static,
     * but re-write it defensively in case a prior path clobbered it, then
     * write TSD with the size -- writing TSD (clearing OWN to 0 via the size
     * field) kicks the transmit DMA.  An early-Tx threshold of 0 means
     * "wait for the whole packet in FIFO before sending".
     */
    reg_w32(RTL_TSAD(i), (uint32_t)(uintptr_t)rtl.tx_buf[i]);
    dma_wmb();   /* frame bytes visible before we hand the buffer to the DMA */
    reg_w32(RTL_TSD(i), (uint32_t)xmit & TSD_SIZE_MASK);

    /* Advance the round-robin cursor. */
    rtl.tx_cur = (i + 1) % RTL_NUM_TX_DESC;

    /* Spin briefly for the TOK completion so the caller knows it went out. */
    for (int spin = 0; spin < 100000; spin++) {
        uint32_t tsd = reg_r32(RTL_TSD(i));
        if (tsd & TSD_TOK) {
            return len;
        }
        if (tsd & TSD_TUN) {
            /* Transmit FIFO underrun -- frame likely dropped, but report
             * the queued length; the integrator can retry at a higher layer.*/
            return len;
        }
        rtl_delay(100);
    }
    /* Timed out on writeback, but the frame is almost certainly queued. */
    return len;
}

int rtl8139_rx_poll(void* buf, unsigned short buf_len) {
    if (!rtl.present || !buf) return -1;

    /* CMD.BUFE == 1 means the RX ring is empty (nothing to read). */
    if (reg_r8(RTL_CMD) & CMD_BUFE) {
        return 0;
    }

    uint32_t off = rtl.rx_offset;

    /*
     * Each packet in the ring is prefixed by a 4-byte header:
     *   bytes[0..1] = receive status (little-endian; bit0 = ROK)
     *   bytes[2..3] = length (little-endian; INCLUDES the 4-byte CRC)
     * Read it from the ring with explicit byte loads so we are wrap-safe and
     * do not assume the 4 header bytes are contiguous in memory (they are,
     * within RTL_RX_RING_SIZE, but the read stays defensive and aligned).
     */
    uint8_t* ring = rtl.rx_buf;
    uint16_t rx_status = (uint16_t)(ring[off] | (ring[(off + 1)] << 8));
    uint16_t rx_len    = (uint16_t)(ring[(off + 2)] | (ring[(off + 3)] << 8));

    /*
     * Sanity-check the header.  An rx_len of 0 / 0xFFFF, or one larger than the
     * ring, means the DMA is mid-write or the header is corrupt.  Bail without
     * advancing so the caller can retry once the chip finishes.
     */
    if (rx_len == 0 || rx_len == 0xFFFF ||
        rx_len > (RTL_RX_RING_SIZE + RTL_RX_PAD)) {
        return -1;
    }
    if (!(rx_status & RXSTAT_ROK)) {
        /*
         * Bad packet (CRC/runt/etc).  Still advance CAPR past it so the ring
         * does not stall, but report "nothing usable" to the caller.
         */
        uint32_t adv_bad = (uint32_t)rx_len + RTL_RX_HDR_LEN;
        adv_bad = (adv_bad + 3) & ~3u;                  /* dword-align */
        rtl.rx_offset = (off + adv_bad) % RTL_RX_RING_SIZE;
        reg_w16(RTL_CAPR, (uint16_t)(rtl.rx_offset - 16));
        return -1;
    }

    /* Frame length = header length minus the trailing 4-byte CRC. */
    uint16_t frame_len = (rx_len > RTL_RX_CRC_LEN)
                       ? (uint16_t)(rx_len - RTL_RX_CRC_LEN) : 0;

    uint16_t copy_len = frame_len;
    if (copy_len > buf_len) copy_len = buf_len;

    /*
     * Copy the frame body, which starts right after the 4-byte header.
     * Because RCR_WRAP is set the chip never splits a packet across the ring
     * end (it spills into our +1500 slack instead), so the body is contiguous
     * starting at off + 4.
     */
    if (copy_len > 0) {
        memcpy(buf, ring + off + RTL_RX_HDR_LEN, copy_len);
    }

    /*
     * Advance the read cursor past header + packet + CRC, dword-aligned and
     * wrapped to the ring size, then publish it to CAPR.  CAPR is biased by
     * -16 (the chip adds 16 internally), which is the canonical RTL8139 quirk.
     */
    uint32_t adv = (uint32_t)rx_len + RTL_RX_HDR_LEN;
    adv = (adv + 3) & ~3u;                              /* 4-byte align */
    rtl.rx_offset = (off + adv) % RTL_RX_RING_SIZE;
    dma_wmb();
    reg_w16(RTL_CAPR, (uint16_t)(rtl.rx_offset - 16));

    /* Ack the ROK/RER causes we may have latched (poll-mode housekeeping). */
    reg_w16(RTL_ISR, INT_ROK | INT_RER);

    if (frame_len == 0) return -1;     /* CRC-only / malformed */
    return (int)copy_len;
}
