/*
 * EHCI Host Controller Driver (USB 2.0, Enhanced Host Controller Interface)
 * =========================================================================
 *
 * USB-EHCI-0 brick: drive ONE wired boot-protocol HID mouse on the T410's
 * Intel QM57 / Ibex Peak PCH, which is EHCI-ONLY -- it has no UHCI controller
 * for uhci.c to bind, so a real USB mouse there needs this driver.
 *
 * GATED behind -DEHCI_USB (EHCI_USB=1 build). The default build never compiles
 * or calls this, so it is byte-for-byte unchanged and never touches USB at boot.
 * Poll-driven (no IRQ): ehci_poll() runs from the pit timer tick.
 *
 * Reuses the shared input_report_* and input_sync injection layer (input.c) --
 * the SAME path ps2mouse.c and uhci.c use, so there is no second mouse system.
 *
 * CHECKPOINT STATUS: E2 (PCI discovery + MMIO map + BIOS handoff + bounded
 * reset).  ehci_init() finds every EHCI controller, takes ownership from the
 * BIOS, resets it, and emits the routing-ledger lines THROUGH configflag.  It
 * stops there: it does NOT read the port routing decision (E3) and does NOT
 * touch CONFIGFLAG=1 or any transfer (E4+).  Per the routing law, no split
 * transactions until E3 proves the mouse routes through an EHCI rate-matching-
 * hub TT (vs a UHCI companion).
 *
 * MMIO note: this kernel identity-maps physical RAM 1:1 over 0..16GB (paging.c),
 * so the EHCI BAR0 physical base (in the low-4GB PCI hole) is usable directly as
 * a pointer -- the same approach e1000.c uses for its MMIO register file.
 *
 * Scope: kernel/drivers/usb/* and kernel/include/usb.h only.
 */

#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/pci.h"
#include "../../include/x86_64.h"

/* ---- EHCI capability registers (offsets from BAR0 base, read-only) ---- */
#define EHCI_CAP_CAPLENGTH    0x00  /* u8  : length of cap regs = op-reg offset */
#define EHCI_CAP_HCIVERSION   0x02  /* u16 */
#define EHCI_CAP_HCSPARAMS    0x04  /* u32 : structural params (N_PORTS, N_CC..) */
#define EHCI_CAP_HCCPARAMS    0x08  /* u32 : capability params (EECP, 64-bit..)  */

/* ---- EHCI operational registers (offsets from op_base = BAR0 + CAPLENGTH) ---- */
#define EHCI_OP_USBCMD        0x00
#define EHCI_OP_USBSTS        0x04
#define EHCI_OP_CONFIGFLAG    0x40
#define EHCI_OP_PORTSC        0x44  /* + 4*port */

/* USBCMD / USBSTS bits */
#define EHCI_CMD_RUN          (1u << 0)
#define EHCI_CMD_HCRESET      (1u << 1)
#define EHCI_STS_HCHALTED     (1u << 12)

/* HCSPARAMS / HCCPARAMS decode */
#define EHCI_HCS_NPORTS(x)    ((x) & 0xFu)
#define EHCI_HCS_NCC(x)       (((x) >> 12) & 0xFu)   /* # companion controllers */
#define EHCI_HCC_EECP(x)      (((x) >> 8) & 0xFFu)   /* PCI-config offset, 0=none */

/* USBLEGSUP (at PCI-config offset EECP) ownership semaphores */
#define EHCI_LEGSUP_BIOS      (1u << 16)
#define EHCI_LEGSUP_OS        (1u << 24)

/* Bounded, tick-independent delay (~us). Same discipline as uhci_udelay: a
 * port-0x80 access is ~1us and does not depend on the timer tick (syscalls/IRQs
 * run IF=0), so no wait here can hang the single core. */
static void ehci_udelay(uint32_t us) {
    while (us--) { (void)inb(0x80); }
}

static inline uint32_t mmrd32(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static inline void     mmwr32(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
static inline uint8_t  mmrd8 (volatile uint8_t* b, uint32_t off) { return *(volatile uint8_t*)(b + off); }
static inline uint16_t mmrd16(volatile uint8_t* b, uint32_t off) { return *(volatile uint16_t*)(b + off); }

/*
 * E2 per-controller bring-up: enable, MMIO-map, read cap regs, take ownership
 * from the BIOS, and reset.  Emits the routing ledger through configflag.
 * Leaves the controller owned + halted + reset.  Does NOT set CONFIGFLAG=1 and
 * does NOT read the port routing decision -- that is E3.
 */
static void ehci_bringup(pci_device_t* dev) {
    uint8_t bus = dev->bus, d = dev->device, fn = dev->function;
    kprintf("[EHCI] controller %x:%x.%x vendor/device %x:%x\n",
            bus, d, fn, dev->vendor_id, dev->device_id);

    /* BAR0 + size probe.  Disable memory decode during the all-ones write, then
     * restore the original BAR and command word (bus-master/mem-space re-enabled
     * explicitly below).  This never leaves the BAR misconfigured. */
    uint16_t cmd0 = pci_config_read_word(bus, d, fn, PCI_CONFIG_COMMAND);
    pci_config_write_word(bus, d, fn, PCI_CONFIG_COMMAND, cmd0 & ~PCI_COMMAND_MEMORY_SPACE);
    uint32_t bar0  = pci_config_read_dword(bus, d, fn, PCI_CONFIG_BAR0);
    pci_config_write_dword(bus, d, fn, PCI_CONFIG_BAR0, 0xFFFFFFFFu);
    uint32_t barsz = pci_config_read_dword(bus, d, fn, PCI_CONFIG_BAR0);
    pci_config_write_dword(bus, d, fn, PCI_CONFIG_BAR0, bar0);
    pci_config_write_word(bus, d, fn, PCI_CONFIG_COMMAND, cmd0);

    int      is_mmio = (bar0 & 0x1u) == 0;
    uint32_t base    = bar0 & ~0xFu;
    uint32_t size    = is_mmio ? (~(barsz & ~0xFu)) + 1u : 0u;
    int      bar_ok  = is_mmio && base != 0;
    kprintf("[EHCI] BAR0 mmio base=%x size=%x valid=%d\n", base, size, bar_ok);
    if (!bar_ok) { kprintf("[EHCI] decision: no device (BAR0 invalid)\n"); return; }

    pci_enable_memory_space(dev);
    pci_enable_bus_master(dev);

    volatile uint8_t* cap = (volatile uint8_t*)(uintptr_t)base;
    uint8_t  caplength = mmrd8 (cap, EHCI_CAP_CAPLENGTH);
    uint16_t hciver    = mmrd16(cap, EHCI_CAP_HCIVERSION);
    uint32_t hcsp      = mmrd32(cap, EHCI_CAP_HCSPARAMS);
    uint32_t hccp      = mmrd32(cap, EHCI_CAP_HCCPARAMS);
    uint8_t  eecp      = EHCI_HCC_EECP(hccp);
    volatile uint8_t* op = cap + caplength;
    kprintf("[EHCI] caplength=%x hciversion=%x hcsparams=%x hccparams=%x eecp=%x\n",
            caplength, hciver, hcsp, hccp, eecp);
    kprintf("[EHCI]   n_ports=%d n_cc=%d (companions; 0=>EHCI owns full/low-speed)\n",
            EHCI_HCS_NPORTS(hcsp), EHCI_HCS_NCC(hcsp));

    /* BIOS handoff via the USBLEGSUP extended capability at PCI-config eecp. */
    if (eecp >= 0x40) {
        uint32_t legsup = pci_config_read_dword(bus, d, fn, eecp);
        kprintf("[EHCI] BIOS owned before=%d OS owned before=%d\n",
                !!(legsup & EHCI_LEGSUP_BIOS), !!(legsup & EHCI_LEGSUP_OS));
        pci_config_write_dword(bus, d, fn, eecp, legsup | EHCI_LEGSUP_OS);
        int spins = 1000;                 /* ~1000 * 100us = 100ms cap */
        while (spins-- > 0) {
            legsup = pci_config_read_dword(bus, d, fn, eecp);
            if (!(legsup & EHCI_LEGSUP_BIOS) && (legsup & EHCI_LEGSUP_OS)) break;
            ehci_udelay(100);
        }
        legsup = pci_config_read_dword(bus, d, fn, eecp);
        kprintf("[EHCI] BIOS owned after=%d OS owned after=%d\n",
                !!(legsup & EHCI_LEGSUP_BIOS), !!(legsup & EHCI_LEGSUP_OS));
        pci_config_write_dword(bus, d, fn, eecp + 4, 0);   /* clear BIOS SMIs */
    } else {
        kprintf("[EHCI] BIOS owned after=n/a OS owned after=n/a (no USBLEGSUP eecp)\n");
    }

    /* Stop, then HCRESET -- both bounded. */
    uint32_t usbcmd = mmrd32(op, EHCI_OP_USBCMD);
    if (usbcmd & EHCI_CMD_RUN) {
        mmwr32(op, EHCI_OP_USBCMD, usbcmd & ~EHCI_CMD_RUN);
        int spins = 1000;
        while (spins-- > 0 && !(mmrd32(op, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED)) ehci_udelay(100);
    }
    mmwr32(op, EHCI_OP_USBCMD, EHCI_CMD_HCRESET);
    int spins = 1000;
    while (spins-- > 0 && (mmrd32(op, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) ehci_udelay(100);
    int reset_ok = !(mmrd32(op, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET);

    uint32_t cfgflag = mmrd32(op, EHCI_OP_CONFIGFLAG);
    kprintf("[EHCI] configflag before=%d after=%d (after=before: set only post-routing-decision, E3)\n",
            cfgflag & 1u, cfgflag & 1u);
    kprintf("[EHCI] reset_ok=%d -- controller owned+halted+reset; routing ledger + decision is E3\n",
            reset_ok);
}

/*
 * ehci_init -- E2 entry: find EVERY EHCI controller (class 0x0C/0x03/0x20; the
 * T410 has two) and bring each up.  Returns 0 if at least one was found, -1 if
 * none (graceful: the default/QEMU-UHCI case where no EHCI exists).
 */
int ehci_init(void) {
    uint32_t count = pci_get_device_count();
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        pci_device_t* dev = pci_get_device_by_index(i);
        if (!dev) continue;
        if (dev->class_code == 0x0C && dev->subclass == 0x03 && dev->prog_if == 0x20) {
            found++;
            ehci_bringup(dev);
        }
    }
    if (!found) {
        kprintf("[EHCI] No EHCI controller found on PCI bus (E2)\n");
        return -1;
    }
    return 0;
}

/*
 * ehci_poll -- still nothing to poll until E4+ arms a mouse endpoint. Bounded
 * no-op; runs in IRQ context from timer_handler().
 */
void ehci_poll(void) {
    /* intentionally empty until a mouse path is selected (E3) + armed (E4+) */
}
