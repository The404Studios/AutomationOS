/*
 * PCI Bus Driver (Legacy Configuration Mechanism #1)
 * ==================================================
 *
 * Enumerates the PCI bus using the legacy I/O-port configuration mechanism:
 *   - 0xCF8 (CONFIG_ADDRESS): selects bus/device/function/register
 *   - 0xCFC (CONFIG_DATA):    32-bit data window into config space
 *
 * On pci_init() we brute-force scan all 256 buses x 32 devices x 8 functions,
 * recording every present device (vendor_id != 0xFFFF) in a static table.
 * Drivers then locate their controller via pci_find_class()/pci_find_device(),
 * decode BARs with pci_get_bar(), and enable the controller's COMMAND-register
 * bits with pci_enable_bus_master()/pci_enable_memory_space().
 *
 * Scope: kernel/drivers/pci.c only. No edits to kernel.c, syscall.*, or the
 * build script -- those are the integrator's wiring (see the report).
 */

#include "../include/pci.h"
#include "../include/x86_64.h"   /* inl/outl */
#include "../include/types.h"
#include "../include/kernel.h"   /* kprintf */

/* Legacy configuration access ports (Mechanism #1). */
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* PCI header-type fields. */
#define PCI_HEADER_TYPE_MASK        0x7F  /* low 7 bits = layout            */
#define PCI_HEADER_MULTIFUNCTION    0x80  /* bit 7 set => multifunction dev */
#define PCI_HEADER_TYPE_GENERAL     0x00  /* type-0 (endpoint) has 6 BARs   */
#define PCI_HEADER_TYPE_BRIDGE      0x01  /* PCI-to-PCI bridge has 2 BARs   */

/* BAR low-bit decode masks. */
#define PCI_BAR_SPACE_IO            0x01  /* bit0: 1 = I/O space            */
#define PCI_BAR_MEM_TYPE_MASK       0x06  /* bits[2:1]: memory BAR type     */
#define PCI_BAR_MEM_TYPE_64         0x04  /* type 0x2 => 64-bit BAR pair    */
#define PCI_BAR_MEM_ADDR_MASK       (~0xFULL) /* clear low 4 type bits      */
#define PCI_BAR_IO_ADDR_MASK        (~0x3ULL) /* clear low 2 type bits      */

/* Maximum devices we record. Plenty for any QEMU machine; real machines too. */
#define PCI_MAX_DEVICES             64

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static uint32_t     g_pci_device_count = 0;

/* ------------------------------------------------------------------ */
/* Configuration-space accessors (Mechanism #1 via 0xCF8 / 0xCFC).    */
/* ------------------------------------------------------------------ */

/*
 * Build the CONFIG_ADDRESS value. Layout:
 *   bit31      = enable
 *   bits30:24  = reserved (0)
 *   bits23:16  = bus
 *   bits15:11  = device
 *   bits10:8   = function
 *   bits7:0    = register offset (dword-aligned; low 2 bits forced 0)
 */
static inline uint32_t pci_config_address(uint8_t bus, uint8_t device,
                                          uint8_t function, uint8_t offset) {
    return (uint32_t)((1u << 31) |
                      ((uint32_t)bus << 16) |
                      (((uint32_t)device & 0x1F) << 11) |
                      (((uint32_t)function & 0x07) << 8) |
                      ((uint32_t)offset & 0xFC));
}

uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function,
                               uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function,
                              uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    /* Select the 16-bit lane within the dword (offset bit1 picks high half). */
    return (uint16_t)((dword >> ((offset & 0x2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    /* Select the 8-bit lane within the dword (offset bits[1:0] pick the byte). */
    return (uint8_t)((dword >> ((offset & 0x3) * 8)) & 0xFF);
}

void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset, uint16_t value) {
    /* Read-modify-write the containing dword (no 16-bit data-port access). */
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    uint32_t shift = (offset & 0x2) * 8;
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_config_write_dword(bus, device, function, offset & 0xFC, dword);
}

void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset, uint8_t value) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    uint32_t shift = (offset & 0x3) * 8;
    dword = (dword & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    pci_config_write_dword(bus, device, function, offset & 0xFC, dword);
}

/* ------------------------------------------------------------------ */
/* Enumeration.                                                        */
/* ------------------------------------------------------------------ */

/*
 * Read one present function into a pci_device_t. Caller has already verified
 * vendor_id != 0xFFFF. We record identity, class triplet, IRQ wiring, and the
 * RAW BAR register values (low type-bits intact) in dev->bar[]. Decoding into
 * a usable base address is deferred to pci_get_bar(); drivers that look at
 * dev->bar[i] directly (e.g. NVMe) inspect the raw value and mask themselves.
 */
static void pci_read_function(uint8_t bus, uint8_t device, uint8_t function,
                              pci_device_t* dev) {
    uint32_t id = pci_config_read_dword(bus, device, function,
                                        PCI_CONFIG_VENDOR_ID);

    dev->vendor_id      = (uint16_t)(id & 0xFFFF);
    dev->device_id      = (uint16_t)(id >> 16);
    dev->bus            = bus;
    dev->device         = device;
    dev->function       = function;

    dev->revision_id    = pci_config_read_byte(bus, device, function,
                                               PCI_CONFIG_REVISION_ID);
    dev->prog_if        = pci_config_read_byte(bus, device, function,
                                               PCI_CONFIG_PROG_IF);
    dev->subclass       = pci_config_read_byte(bus, device, function,
                                               PCI_CONFIG_SUBCLASS);
    dev->class_code     = pci_config_read_byte(bus, device, function,
                                               PCI_CONFIG_CLASS_CODE);
    dev->interrupt_line = pci_config_read_byte(bus, device, function,
                                               PCI_CONFIG_INTERRUPT_LINE);
    dev->interrupt_pin  = pci_config_read_byte(bus, device, function,
                                               PCI_CONFIG_INTERRUPT_PIN);

    uint8_t header_type = pci_config_read_byte(bus, device, function,
                                               PCI_CONFIG_HEADER_TYPE);
    uint8_t layout      = header_type & PCI_HEADER_TYPE_MASK;

    /* Number of BARs depends on header layout: endpoint=6, bridge=2, other=0. */
    uint8_t num_bars = 0;
    if (layout == PCI_HEADER_TYPE_GENERAL) {
        num_bars = 6;
    } else if (layout == PCI_HEADER_TYPE_BRIDGE) {
        num_bars = 2;
    }

    for (uint8_t i = 0; i < 6; i++) {
        dev->bar[i] = 0;
    }

    for (uint8_t i = 0; i < num_bars; i++) {
        uint8_t  off = (uint8_t)(PCI_CONFIG_BAR0 + i * 4);
        uint32_t lo  = pci_config_read_dword(bus, device, function, off);

        if (lo == 0) {
            dev->bar[i] = 0;
            continue;
        }

        if (lo & PCI_BAR_SPACE_IO) {
            /* I/O-space BAR: store raw 32-bit value. */
            dev->bar[i] = (uint64_t)lo;
        } else if ((lo & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64) {
            /* 64-bit memory BAR: combine this dword with the next one. */
            uint32_t hi = pci_config_read_dword(bus, device, function,
                                                (uint8_t)(off + 4));
            dev->bar[i] = ((uint64_t)hi << 32) | (uint64_t)lo;
            if (i + 1 < 6) {
                dev->bar[i + 1] = 0;  /* high dword consumed by the pair */
            }
            i++;  /* skip the BAR slot we just absorbed */
        } else {
            /* 32-bit memory BAR: store raw 32-bit value. */
            dev->bar[i] = (uint64_t)lo;
        }
    }
}

static void pci_log_device(const pci_device_t* dev) {
    kprintf("[PCI] %02x:%02x.%x  %04x:%04x  class=%02x:%02x:%02x  irq=%u\n",
            dev->bus, dev->device, dev->function,
            dev->vendor_id, dev->device_id,
            dev->class_code, dev->subclass, dev->prog_if,
            dev->interrupt_line);
}

/* Probe a single function; record it if present. */
static void pci_check_function(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor = pci_config_read_word(bus, device, function,
                                           PCI_CONFIG_VENDOR_ID);
    if (vendor == 0xFFFF) {
        return;  /* no function here */
    }

    if (g_pci_device_count >= PCI_MAX_DEVICES) {
        return;  /* table full -- silently stop recording */
    }

    pci_device_t* dev = &g_pci_devices[g_pci_device_count];
    pci_read_function(bus, device, function, dev);
    g_pci_device_count++;

    pci_log_device(dev);
}

/* Probe a device slot: function 0, then 1..7 if it is multifunction. */
static void pci_check_device(uint8_t bus, uint8_t device) {
    uint16_t vendor = pci_config_read_word(bus, device, 0, PCI_CONFIG_VENDOR_ID);
    if (vendor == 0xFFFF) {
        return;  /* slot empty */
    }

    pci_check_function(bus, device, 0);

    uint8_t header_type = pci_config_read_byte(bus, device, 0,
                                               PCI_CONFIG_HEADER_TYPE);
    if (header_type & PCI_HEADER_MULTIFUNCTION) {
        for (uint8_t function = 1; function < 8; function++) {
            uint16_t fv = pci_config_read_word(bus, device, function,
                                               PCI_CONFIG_VENDOR_ID);
            if (fv != 0xFFFF) {
                pci_check_function(bus, device, function);
            }
        }
    }
}

void pci_init(void) {
    g_pci_device_count = 0;

    kprintf("[PCI] Scanning all buses (config mechanism #1, 0xCF8/0xCFC)...\n");

    /* Brute-force scan: every bus x device slot. */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            pci_check_device((uint8_t)bus, device);
        }
    }

    kprintf("[PCI] Enumeration complete: %u device(s) found\n",
            g_pci_device_count);
}

/* ------------------------------------------------------------------ */
/* Lookup APIs.                                                        */
/* ------------------------------------------------------------------ */

pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (uint32_t i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].vendor_id == vendor_id &&
            g_pci_devices[i].device_id == device_id) {
            return &g_pci_devices[i];
        }
    }
    return NULL;
}

pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass,
                             uint8_t prog_if) {
    for (uint32_t i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].class_code == class_code &&
            g_pci_devices[i].subclass == subclass &&
            g_pci_devices[i].prog_if == prog_if) {
            return &g_pci_devices[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* BAR decode and COMMAND-register enables.                            */
/* ------------------------------------------------------------------ */

/*
 * Return the usable base address held in dev->bar[bar_num], with the low
 * type-encoding bits masked off. Memory BARs return a physical/MMIO base;
 * I/O BARs return a port base. 64-bit BARs were already coalesced into the
 * single bar[] slot at enumeration time, so the full 64-bit value is masked
 * here. Returns 0 for empty/unused slots.
 */
uint64_t pci_get_bar(pci_device_t* dev, uint8_t bar_num) {
    if (dev == NULL || bar_num >= 6) {
        return 0;
    }

    uint64_t raw = dev->bar[bar_num];
    if (raw == 0) {
        return 0;
    }

    if (raw & PCI_BAR_SPACE_IO) {
        return raw & PCI_BAR_IO_ADDR_MASK;
    }
    return raw & PCI_BAR_MEM_ADDR_MASK;
}

void pci_enable_bus_master(pci_device_t* dev) {
    if (dev == NULL) {
        return;
    }
    uint16_t command = pci_config_read_word(dev->bus, dev->device, dev->function,
                                            PCI_CONFIG_COMMAND);
    command |= PCI_COMMAND_BUS_MASTER;
    pci_config_write_word(dev->bus, dev->device, dev->function,
                          PCI_CONFIG_COMMAND, command);
}

void pci_enable_memory_space(pci_device_t* dev) {
    if (dev == NULL) {
        return;
    }
    uint16_t command = pci_config_read_word(dev->bus, dev->device, dev->function,
                                            PCI_CONFIG_COMMAND);
    command |= PCI_COMMAND_MEMORY_SPACE;
    pci_config_write_word(dev->bus, dev->device, dev->function,
                          PCI_CONFIG_COMMAND, command);
}
