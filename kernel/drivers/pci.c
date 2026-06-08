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
#include "../include/mem.h"      /* copy_to_user, COPY_SUCCESS */
#include "../include/errno.h"    /* EINVAL, EFAULT */

/* Gate verbose per-device logging: each device prints a line to serial, and
 * a real machine like the T410 has ~20-30 PCI devices. Under BOOT_QUIET we
 * suppress the per-device and progress lines to cut serial latency. */
#ifdef BOOT_QUIET
#define PCI_LOG(...) ((void)0)
#else
#define PCI_LOG(...) kprintf(__VA_ARGS__)
#endif

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
            /* 64-bit memory BAR: it occupies THIS slot plus the next (high dword).
             * A well-formed device only marks a BAR 64-bit where a high-half slot
             * exists (i+1 < num_bars). A malformed device that marks the LAST BAR
             * 64-bit would otherwise make us read off+4 -- a NON-BAR config register
             * (e.g. the Cardbus CIS pointer at 0x28 for BAR5) -- and fold that
             * garbage into dev->bar[i]. Guard it: with no high-half slot, treat the
             * low dword as a plain 32-bit BAR. */
            if (i + 1 >= num_bars) {
                dev->bar[i] = (uint64_t)lo;
                continue;
            }
            uint32_t hi = pci_config_read_dword(bus, device, function,
                                                (uint8_t)(off + 4));
            dev->bar[i]     = ((uint64_t)hi << 32) | (uint64_t)lo;
            dev->bar[i + 1] = 0;  /* high dword consumed by the pair (i+1 < num_bars) */
            i++;  /* skip the BAR slot we just absorbed */
        } else {
            /* 32-bit memory BAR: store raw 32-bit value. */
            dev->bar[i] = (uint64_t)lo;
        }
    }
}

static void pci_log_device(const pci_device_t* dev) {
    PCI_LOG("[PCI] %02x:%02x.%x  %04x:%04x  class=%02x:%02x:%02x  irq=%u\n",
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

    PCI_LOG("[PCI] Scanning all buses (config mechanism #1, 0xCF8/0xCFC)...\n");

    /* Brute-force scan: every bus x device slot.
     *
     * T410 SAFETY NOTE: On real hardware, accessing a non-existent PCI
     * device/bus returns 0xFFFF (master abort completion). This is
     * architecturally guaranteed by the PCI spec and handled by the
     * vendor == 0xFFFF checks in pci_check_device(). No hardware hang
     * can result from probing an empty slot -- only a benign 0xFFFF read.
     *
     * Progress markers: on serial, log every 64 buses so the operator can
     * see where the scan is if the boot stalls for another reason. Each
     * bus x 32 devices = 32 config reads of vendor ID (fast I/O port
     * accesses); the full 256-bus scan takes <100ms on real hardware. */
    for (uint32_t bus = 0; bus < 256; bus++) {
        if (bus > 0 && (bus & 63) == 0) {
            PCI_LOG("[PCI]   ...bus %u/%u (%u devices so far)\n",
                    bus, 256, g_pci_device_count);
        }
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

/* ------------------------------------------------------------------ */
/* Device-count / indexed accessors (for procapi / userspace).         */
/* ------------------------------------------------------------------ */

uint32_t pci_get_device_count(void) {
    return g_pci_device_count;
}

pci_device_t* pci_get_device_by_index(uint32_t index) {
    if (index >= g_pci_device_count) {
        return NULL;
    }
    return &g_pci_devices[index];
}

/* ------------------------------------------------------------------ */
/* T410 PCI Device Map + human-readable lister ("pci list").           */
/* ------------------------------------------------------------------ */
/*
 * Comprehensive vendor:device name database covering every PCI device the
 * ThinkPad T410 exposes, plus QEMU's common virtual devices. This lets
 * `pci_list()` produce an lspci-like dump invaluable for debugging T410
 * boot issues (e.g. which device an unknown driver is touching).
 *
 * The table is const .rodata -- zero runtime cost when not called.
 */

typedef struct {
    uint16_t    vendor_id;
    uint16_t    device_id;
    const char* name;
} pci_id_entry_t;

static const pci_id_entry_t pci_known_devices[] = {
    /* ---- Intel Core i5 / Ironlake (T410 northbridge) ---- */
    { 0x8086, 0x0044, "Intel Core i5 Host Bridge (Ironlake)" },
    { 0x8086, 0x0046, "Intel HD Graphics (Ironlake)" },
    { 0x8086, 0x0040, "Intel Core i5 DRAM Controller" },
    { 0x8086, 0x0042, "Intel HD Graphics (Ironlake Desktop)" },

    /* ---- Intel 5 Series / ICH9M (T410 southbridge / PCH) ---- */
    { 0x8086, 0x3B09, "Intel ICH9M-E LPC Interface Controller" },
    { 0x8086, 0x3B0B, "Intel ICH9M LPC Interface Controller" },
    { 0x8086, 0x3B0D, "Intel ICH9ME LPC Interface" },
    { 0x8086, 0x3B0F, "Intel ICH9M LPC (mobile)" },
    { 0x8086, 0x3B22, "Intel 5 Series/3400 SMBus Controller" },
    { 0x8086, 0x3B30, "Intel 5 Series/3400 USB UHCI #1" },
    { 0x8086, 0x3B31, "Intel 5 Series/3400 USB UHCI #2" },
    { 0x8086, 0x3B32, "Intel 5 Series/3400 USB UHCI #3" },
    { 0x8086, 0x3B33, "Intel 5 Series/3400 USB UHCI #4" },
    { 0x8086, 0x3B34, "Intel 5 Series/3400 USB2 EHCI #1" },
    { 0x8086, 0x3B36, "Intel 5 Series/3400 USB2 EHCI #2" },
    { 0x8086, 0x3B42, "Intel 5 Series/3400 SATA AHCI" },
    { 0x8086, 0x3B56, "Intel 5 Series/3400 HD Audio" },
    { 0x8086, 0x3B64, "Intel 5 Series/3400 Thermal Subsystem" },
    { 0x8086, 0x3B3C, "Intel 5 Series/3400 MEI Controller #1" },
    { 0x8086, 0x3B3E, "Intel 5 Series/3400 MEI Controller #2" },
    { 0x8086, 0x3B44, "Intel 5 Series/3400 SATA RAID" },
    { 0x8086, 0x3B46, "Intel 5 Series/3400 SATA IDE" },
    { 0x8086, 0x3B4C, "Intel 5 Series/3400 PCI Express Root Port 1" },
    { 0x8086, 0x3B4E, "Intel 5 Series/3400 PCI Express Root Port 2" },
    { 0x8086, 0x3B50, "Intel 5 Series/3400 PCI Express Root Port 3" },
    { 0x8086, 0x3B52, "Intel 5 Series/3400 PCI Express Root Port 4" },
    { 0x8086, 0x3B54, "Intel 5 Series/3400 PCI Express Root Port 5" },
    { 0x8086, 0x3B48, "Intel 5 Series/3400 PCI Express Root Port 6" },

    /* ---- Intel Ethernet (T410 + QEMU) ---- */
    { 0x8086, 0x10EA, "Intel 82577LM Gigabit Ethernet" },
    { 0x8086, 0x10EB, "Intel 82577LC Gigabit Ethernet" },
    { 0x8086, 0x100E, "Intel 82540EM Gigabit Ethernet (QEMU)" },
    { 0x8086, 0x100F, "Intel 82545EM Gigabit Ethernet" },
    { 0x8086, 0x10D3, "Intel 82574L Gigabit Ethernet" },
    { 0x8086, 0x10EF, "Intel 82578DM Gigabit Ethernet" },
    { 0x8086, 0x10F0, "Intel 82578DC Gigabit Ethernet" },
    { 0x8086, 0x10F5, "Intel 82567LM Gigabit Ethernet" },
    { 0x8086, 0x1502, "Intel 82579LM Gigabit Ethernet" },
    { 0x8086, 0x1503, "Intel 82579V Gigabit Ethernet" },
    { 0x8086, 0x153A, "Intel I217-LM Gigabit Ethernet" },
    { 0x8086, 0x15A0, "Intel I218-LM Gigabit Ethernet" },

    /* ---- NVIDIA NVS 3100M (T410 discrete GPU) ---- */
    { 0x10DE, 0x0A6C, "NVIDIA NVS 3100M (GT218)" },
    { 0x10DE, 0x0A28, "NVIDIA NVS 3100M (alt)" },

    /* ---- Ricoh card reader (T410 ExpressCard/SD slot) ---- */
    { 0x1180, 0x0592, "Ricoh R5C592 SD/MMC Card Reader" },
    { 0x1180, 0x0852, "Ricoh R5U8xx xD/SD Card Reader" },
    { 0x1180, 0x0843, "Ricoh R5C843 FireWire (IEEE 1394)" },

    /* ---- QEMU virtual devices ---- */
    { 0x8086, 0x1237, "Intel 440FX Host Bridge (QEMU)" },
    { 0x8086, 0x7000, "Intel PIIX3 ISA Bridge (QEMU)" },
    { 0x8086, 0x7010, "Intel PIIX3 IDE (QEMU)" },
    { 0x8086, 0x7020, "Intel PIIX3 USB UHCI (QEMU)" },
    { 0x8086, 0x7113, "Intel PIIX4 ACPI/Power Mgmt (QEMU)" },
    { 0x8086, 0x2922, "Intel ICH9 AHCI (QEMU)" },
    { 0x8086, 0x2668, "Intel ICH6 HD Audio (QEMU)" },
    { 0x8086, 0x29C0, "Intel Q35 Host Bridge (QEMU)" },
    { 0x8086, 0x2918, "Intel ICH9 LPC Interface (QEMU)" },
    { 0x1234, 0x1111, "QEMU stdvga (Bochs VBE)" },
    { 0x1AF4, 0x1000, "VirtIO Network" },
    { 0x1AF4, 0x1001, "VirtIO Block" },
    { 0x1AF4, 0x1002, "VirtIO Balloon" },
    { 0x1AF4, 0x1003, "VirtIO Console" },
    { 0x1AF4, 0x1009, "VirtIO 9P (filesystem)" },
    { 0x1AF4, 0x1041, "VirtIO Network (modern)" },
    { 0x1AF4, 0x1042, "VirtIO Block (modern)" },
    { 0x1AF4, 0x1043, "VirtIO Console (modern)" },
    { 0x1AF4, 0x1044, "VirtIO RNG (modern)" },
    { 0x1AF4, 0x1045, "VirtIO Balloon (modern)" },
    { 0x1AF4, 0x1050, "VirtIO GPU" },
    { 0x1AF4, 0x1052, "VirtIO Input" },
    { 0x10EC, 0x8139, "Realtek RTL8139 Fast Ethernet" },

    /* ---- Intel Wireless (T410 option) ---- */
    { 0x8086, 0x0085, "Intel Centrino Advanced-N 6205" },
    { 0x8086, 0x4232, "Intel WiFi Link 5100 AGN" },
    { 0x8086, 0x4237, "Intel WiFi Link 5100" },
    { 0x8086, 0x0082, "Intel Centrino Advanced-N 6205 [Taylor Peak]" },
    { 0x8086, 0x422B, "Intel Centrino Ultimate-N 6300" },

    /* Sentinel */
    { 0, 0, NULL }
};

/*
 * PCI class-code to human-readable name. Used as a fallback when the exact
 * vendor:device pair is not in pci_known_devices[].
 */
static const char* pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
    case 0x00:
        switch (subclass) {
        case 0x00: return "Non-VGA unclassified";
        case 0x01: return "VGA-compatible unclassified";
        default:   return "Unclassified";
        }
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI Bus Controller";
        case 0x01: return "IDE Controller";
        case 0x02: return "Floppy Controller";
        case 0x03: return "IPI Bus Controller";
        case 0x04: return "RAID Controller";
        case 0x05: return "ATA Controller";
        case 0x06: return "SATA Controller";
        case 0x07: return "Serial Attached SCSI";
        case 0x08: return "NVMe Controller";
        default:   return "Mass Storage Controller";
        }
    case 0x02:
        switch (subclass) {
        case 0x00: return "Ethernet Controller";
        case 0x01: return "Token Ring Controller";
        case 0x02: return "FDDI Controller";
        case 0x03: return "ATM Controller";
        case 0x04: return "ISDN Controller";
        case 0x80: return "Other Network Controller";
        default:   return "Network Controller";
        }
    case 0x03:
        switch (subclass) {
        case 0x00: return "VGA-compatible Controller";
        case 0x01: return "XGA Controller";
        case 0x02: return "3D Controller";
        default:   return "Display Controller";
        }
    case 0x04:
        switch (subclass) {
        case 0x00: return "Multimedia Video";
        case 0x01: return "Multimedia Audio";
        case 0x02: return "Computer Telephony";
        case 0x03: return "HD Audio Controller";
        default:   return "Multimedia Controller";
        }
    case 0x05:
        switch (subclass) {
        case 0x00: return "RAM Controller";
        case 0x01: return "Flash Controller";
        default:   return "Memory Controller";
        }
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x02: return "EISA Bridge";
        case 0x03: return "MCA Bridge";
        case 0x04: return "PCI-to-PCI Bridge";
        case 0x05: return "PCMCIA Bridge";
        case 0x06: return "NuBus Bridge";
        case 0x07: return "CardBus Bridge";
        case 0x08: return "RACEway Bridge";
        case 0x09: return "PCI-to-PCI Bridge (semi-transparent)";
        case 0x80: return "Other Bridge";
        default:   return "Bridge Device";
        }
    case 0x07:
        switch (subclass) {
        case 0x00: return "Serial Controller";
        case 0x01: return "Parallel Controller";
        case 0x03: return "Modem";
        case 0x80: return "Other Communication";
        default:   return "Communication Controller";
        }
    case 0x08:
        switch (subclass) {
        case 0x00: return "PIC";
        case 0x01: return "DMA Controller";
        case 0x02: return "Timer";
        case 0x03: return "RTC Controller";
        case 0x04: return "PCI Hot-Plug";
        case 0x05: return "SD Host Controller";
        case 0x80: return "Other System Peripheral";
        default:   return "System Peripheral";
        }
    case 0x09:
        switch (subclass) {
        case 0x00: return "Keyboard Controller";
        case 0x01: return "Digitizer/Pen";
        case 0x02: return "Mouse Controller";
        case 0x03: return "Scanner Controller";
        case 0x04: return "Gameport Controller";
        default:   return "Input Device";
        }
    case 0x0A: return "Docking Station";
    case 0x0B:
        switch (subclass) {
        case 0x00: return "386 Processor";
        case 0x01: return "486 Processor";
        case 0x02: return "Pentium Processor";
        case 0x10: return "Alpha Processor";
        case 0x20: return "PowerPC Processor";
        case 0x30: return "MIPS Processor";
        case 0x40: return "Co-Processor";
        default:   return "Processor";
        }
    case 0x0C:
        switch (subclass) {
        case 0x00: return "IEEE 1394 (FireWire)";
        case 0x01: return "ACCESS Bus";
        case 0x02: return "SSA";
        case 0x03: return "USB Controller";
        case 0x04: return "Fibre Channel";
        case 0x05: return "SMBus";
        case 0x06: return "InfiniBand";
        case 0x07: return "IPMI Interface";
        default:   return "Serial Bus Controller";
        }
    case 0x0D:
        switch (subclass) {
        case 0x00: return "iRDA Controller";
        case 0x01: return "Consumer IR";
        case 0x10: return "RF Controller";
        case 0x11: return "Bluetooth Controller";
        case 0x12: return "Broadband Controller";
        case 0x20: return "Ethernet 802.1a (5 GHz)";
        case 0x21: return "Ethernet 802.1b (2.4 GHz)";
        default:   return "Wireless Controller";
        }
    case 0x0E: return "Intelligent Controller (I20)";
    case 0x0F:
        switch (subclass) {
        case 0x01: return "Satellite TV Controller";
        case 0x02: return "Satellite Audio Controller";
        case 0x03: return "Satellite Voice Controller";
        case 0x04: return "Satellite Data Controller";
        default:   return "Satellite Controller";
        }
    case 0x10: return "Encryption Controller";
    case 0x11:
        switch (subclass) {
        case 0x00: return "DPIO Module";
        case 0x01: return "Performance Counter";
        case 0x10: return "Communication Synchroniser";
        case 0x20: return "Signal Processing Management";
        default:   return "Signal Processing Controller";
        }
    case 0xFF: return "Vendor-specific";
    default:   return "Unknown";
    }
}

/*
 * Look up a human-readable device name for a vendor:device pair.
 * Returns the known name, or NULL if not in the database.
 */
static const char* pci_lookup_device_name(uint16_t vendor_id,
                                          uint16_t device_id) {
    for (uint32_t i = 0; pci_known_devices[i].name != NULL; i++) {
        if (pci_known_devices[i].vendor_id == vendor_id &&
            pci_known_devices[i].device_id == device_id) {
            return pci_known_devices[i].name;
        }
    }
    return NULL;
}

/*
 * Return the driver status for a given device: does the kernel have a driver
 * for it, and is it expected to work? This maps PCI class/vendor:device to
 * the driver that would claim it. Returns a short status string.
 */
static const char* pci_device_driver_status(const pci_device_t* dev) {
    /* ---- Ethernet ---- */
    if (dev->class_code == 0x02 && dev->subclass == 0x00) {
        if (dev->vendor_id == 0x8086) {
            switch (dev->device_id) {
            case 0x100E: case 0x100F: case 0x10D3:
                return "e1000 (verified)";
            case 0x10EA: case 0x10EB: case 0x10EF: case 0x10F0:
            case 0x10F5: case 0x1502: case 0x1503: case 0x153A: case 0x15A0:
                return "e1000 (PCH gated)";
            default:
                return "e1000 (generic fallback)";
            }
        }
        if (dev->vendor_id == 0x10EC && dev->device_id == 0x8139)
            return "rtl8139";
        return "no driver";
    }

    /* ---- Display / GPU ---- */
    if (dev->class_code == 0x03) {
        if (dev->vendor_id == 0x10DE)
            return "nvidia (read-only detect)";
        if (dev->vendor_id == 0x8086)
            return "no driver (Intel iGPU)";
        if (dev->vendor_id == 0x1234 && dev->device_id == 0x1111)
            return "stdvga (firmware FB)";
        return "no driver";
    }

    /* ---- SATA / AHCI ---- */
    if (dev->class_code == 0x01 && dev->subclass == 0x06 && dev->prog_if == 0x01)
        return "ahci";

    /* ---- IDE ---- */
    if (dev->class_code == 0x01 && dev->subclass == 0x01)
        return "no driver (IDE)";

    /* ---- NVMe ---- */
    if (dev->class_code == 0x01 && dev->subclass == 0x08 && dev->prog_if == 0x02)
        return "nvme";

    /* ---- HD Audio ---- */
    if (dev->class_code == 0x04 && dev->subclass == 0x03)
        return "hda";

    /* ---- USB ---- */
    if (dev->class_code == 0x0C && dev->subclass == 0x03) {
        switch (dev->prog_if) {
        case 0x00: return "uhci (USB 1.1)";
        case 0x10: return "no driver (OHCI)";
        case 0x20: return "no driver (EHCI)";
        case 0x30: return "no driver (xHCI)";
        case 0xFE: return "no driver (USB device)";
        default:   return "no driver (USB unknown)";
        }
    }

    /* ---- SMBus ---- */
    if (dev->class_code == 0x0C && dev->subclass == 0x05)
        return "no driver (SMBus)";

    /* ---- Bridges ---- */
    if (dev->class_code == 0x06)
        return "bridge (no driver needed)";

    /* ---- Communication / MEI ---- */
    if (dev->class_code == 0x07 && dev->subclass == 0x80)
        return "no driver (Intel MEI)";

    /* ---- Wireless ---- */
    if (dev->class_code == 0x02 && dev->subclass == 0x80)
        return "no driver (wireless)";

    /* ---- System peripherals / thermal ---- */
    if (dev->class_code == 0x08)
        return "no driver (system peripheral)";

    /* ---- FireWire ---- */
    if (dev->class_code == 0x0C && dev->subclass == 0x00)
        return "no driver (IEEE 1394)";

    return "no driver";
}

/*
 * pci_list() -- dump all enumerated PCI devices to kprintf with human-readable
 * names and driver status. Designed as a T410 diagnostic tool but works on any
 * machine (QEMU included).
 *
 * Output format per device:
 *   [PCI]   00:02.0  8086:0046  Intel HD Graphics (Ironlake)        [no driver (Intel iGPU)]
 *
 * If a vendor:device pair is not in the known-device database, the PCI class
 * name is shown instead, so unknown devices are never silently invisible.
 */
void pci_list(void) {
    kprintf("\n[PCI] ============ PCI Device Map (%u devices) ============\n",
            g_pci_device_count);
    kprintf("[PCI]  BDF         VEN:DEV  Name                                      Driver\n");
    kprintf("[PCI]  ----------  -------  ----------------------------------------  --------\n");

    for (uint32_t i = 0; i < g_pci_device_count; i++) {
        const pci_device_t* dev = &g_pci_devices[i];
        const char* name = pci_lookup_device_name(dev->vendor_id, dev->device_id);
        const char* driver = pci_device_driver_status(dev);

        if (name) {
            kprintf("[PCI]  %02x:%02x.%x  %04x:%04x  %-40s  [%s]\n",
                    dev->bus, dev->device, dev->function,
                    dev->vendor_id, dev->device_id,
                    name, driver);
        } else {
            /* Fallback: show class name for unknown vendor:device. */
            const char* cls_name = pci_class_name(dev->class_code,
                                                   dev->subclass);
            kprintf("[PCI]  %02x:%02x.%x  %04x:%04x  %-40s  [%s]\n",
                    dev->bus, dev->device, dev->function,
                    dev->vendor_id, dev->device_id,
                    cls_name, driver);
        }
    }

    kprintf("[PCI] =======================================================\n\n");
}

/* ------------------------------------------------------------------ */
/* SYS_PCI_LIST (92) -- export enumerated PCI devices to userspace.   */
/* ------------------------------------------------------------------ */

/*
 * sys_pci_list -- copy PCI device entries to a userspace buffer.
 *
 * arg1 = user pointer to array of pci_info_t
 * arg2 = max entries the buffer can hold
 * Returns: number of entries copied (>=0), or -errno.
 *
 * Follows the SYS_ROUTE_TABLE pattern: kernel fills a stack-local buffer of
 * pci_info_t structs from the enumerated device table, then copy_to_user's
 * the whole block.
 */
int64_t sys_pci_list(uint64_t out_ptr, uint64_t max_entries, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (out_ptr == 0 || max_entries == 0) return EINVAL;

    uint32_t count = pci_get_device_count();
    if (count == 0) return 0;

    /* Cap to the user buffer size and a sane stack limit. Clamp in 64-bit BEFORE
     * the uint32 cast: a value like 0x100000001 would otherwise truncate to 1 and
     * silently copy fewer entries than the caller requested. */
    uint32_t cap = (max_entries > PCI_MAX_DEVICES) ? PCI_MAX_DEVICES : (uint32_t)max_entries;
    if (cap > count)           cap = count;

    pci_info_t kbuf[PCI_MAX_DEVICES];

    for (uint32_t i = 0; i < cap; i++) {
        const pci_device_t *dev = pci_get_device_by_index(i);
        if (!dev) break;

        kbuf[i].vendor_id      = dev->vendor_id;
        kbuf[i].device_id      = dev->device_id;
        kbuf[i].bus            = dev->bus;
        kbuf[i].device         = dev->device;
        kbuf[i].function       = dev->function;
        kbuf[i].class_code     = dev->class_code;
        kbuf[i].subclass       = dev->subclass;
        kbuf[i].prog_if        = dev->prog_if;
        kbuf[i].revision_id    = dev->revision_id;
        kbuf[i].interrupt_line = dev->interrupt_line;
        kbuf[i].interrupt_pin  = dev->interrupt_pin;
        kbuf[i]._pad[0]        = 0;
        kbuf[i]._pad[1]        = 0;
        kbuf[i]._pad[2]        = 0;
    }

    size_t copy_bytes = (size_t)cap * sizeof(pci_info_t);
    if (copy_to_user((void*)out_ptr, kbuf, copy_bytes) != COPY_SUCCESS)
        return EFAULT;

    return (int64_t)cap;
}
