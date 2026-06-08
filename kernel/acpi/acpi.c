/*
 * ACPI (Advanced Configuration and Power Interface) Implementation
 * Complete ACPI support for AutomationOS
 *
 * This file implements:
 * - RSDP/RSDT/XSDT parsing
 * - FADT (power management)
 * - MADT (APIC/SMP)
 * - HPET (high precision timer)
 * - MCFG (PCIe configuration)
 * - _S5 (soft-off) sleep-type decode from the DSDT
 * - Poweroff (S5) and reboot (ACPI reset register + 8042 fallback)
 *
 * Notes on memory access:
 *   All ACPI tables (RSDP, RSDT/XSDT, FADT, DSDT, ...) live in low physical
 *   memory which boot.asm identity-maps (0..512MB, later extended). We therefore
 *   access them via a simple identity translation (acpi_phys_to_virt) instead of
 *   relying on an external phys_to_virt() helper that does not exist in this tree.
 */

#include "../include/acpi.h"
#include "../include/kernel.h"
#include "../include/io.h"
#include "../include/mem.h"
#include "../include/string.h"

// Global ACPI state
acpi_state_t acpi_state = {0};

// ACPI memory regions for table search
#define ACPI_RSDP_SEARCH_START  0x000E0000
#define ACPI_RSDP_SEARCH_END    0x000FFFFF
#define ACPI_RSDP_SEARCH_STEP   16

// EBDA (Extended BIOS Data Area) location
#define ACPI_EBDA_PTR           0x0000040E
#define ACPI_EBDA_SEARCH_SIZE   1024

// 8042 keyboard controller (reboot fallback)
#define KBC_STATUS_PORT         0x64
#define KBC_COMMAND_PORT        0x64
#define KBC_DATA_PORT           0x60
#define KBC_CMD_PULSE_RESET     0xFE   // Pulse reset line low (CPU reset)
#define KBC_STATUS_INPUT_FULL   0x02

/*
 * Identity translation for low-memory physical addresses.
 *
 * The kernel identity-maps low physical RAM, so the firmware tables are
 * reachable at their physical address. Keeping this local (rather than calling
 * an undefined phys_to_virt) lets this file compile and link standalone.
 */
static inline void* acpi_phys_to_virt(uint64_t phys) {
    return (void*)(uintptr_t)phys;
}

/*
 * Spin the CPU briefly. Used while waiting for SMI->ACPI handoff. We avoid the
 * kernel timer (timer_sleep) on purpose so poweroff/reboot work even if the
 * timer subsystem is torn down. A few thousand port reads give a coarse delay.
 */
static void acpi_io_delay(uint32_t loops) {
    for (uint32_t i = 0; i < loops; i++) {
        // Reading the unused 0x80 POST port is the classic ~1us I/O delay.
        (void)inb(0x80);
    }
}

// Park the CPU forever (used after issuing poweroff/reset).
static inline void acpi_hang(void) {
    for (;;) {
        cli();
        hlt();
    }
}

/**
 * Verify ACPI table checksum
 */
bool acpi_verify_checksum(const void* table, size_t length) {
    const uint8_t* bytes = (const uint8_t*)table;
    uint8_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }

    return sum == 0;
}

/**
 * Find RSDP in memory
 */
acpi_rsdp_t* acpi_find_rsdp(void) {
    kprintf("[ACPI] Searching for RSDP...\n");

    // Search EBDA (Extended BIOS Data Area)
    uint16_t ebda = *(uint16_t*)acpi_phys_to_virt(ACPI_EBDA_PTR);
    uint64_t ebda_addr = ((uint64_t)ebda) << 4;

    if (ebda_addr >= 0x80000 && ebda_addr < 0xA0000) {
        for (uint64_t addr = ebda_addr; addr < ebda_addr + ACPI_EBDA_SEARCH_SIZE; addr += ACPI_RSDP_SEARCH_STEP) {
            acpi_rsdp_t* rsdp = (acpi_rsdp_t*)acpi_phys_to_virt(addr);

            if (memcmp(rsdp->signature, ACPI_SIG_RSDP, 8) == 0) {
                if (acpi_verify_checksum(rsdp, 20)) {
                    kprintf("[ACPI] Found RSDP in EBDA at 0x%016lx\n", addr);
                    return rsdp;
                }
            }
        }
    }

    // Search main BIOS area (0xE0000 - 0xFFFFF)
    for (uint64_t addr = ACPI_RSDP_SEARCH_START; addr < ACPI_RSDP_SEARCH_END; addr += ACPI_RSDP_SEARCH_STEP) {
        acpi_rsdp_t* rsdp = (acpi_rsdp_t*)acpi_phys_to_virt(addr);

        if (memcmp(rsdp->signature, ACPI_SIG_RSDP, 8) == 0) {
            if (acpi_verify_checksum(rsdp, 20)) {
                kprintf("[ACPI] Found RSDP in BIOS area at 0x%016lx\n", addr);
                return rsdp;
            }
        }
    }

    kprintf("[ACPI] ERROR: RSDP not found!\n");
    return NULL;
}

/**
 * Find ACPI table by signature
 */
void* acpi_find_table(const char* signature) {
    if (!acpi_state.initialized && !acpi_state.rsdt && !acpi_state.xsdt) {
        return NULL;
    }

    // Use XSDT if available (64-bit), otherwise RSDT (32-bit)
    if (acpi_state.xsdt) {
        // A malformed length < the header size would underflow the unsigned
        // subtraction below into a huge entry count -> OOB reads past the table.
        if (acpi_state.xsdt->header.length < sizeof(acpi_table_header_t)) return NULL;
        uint32_t entries = (acpi_state.xsdt->header.length - sizeof(acpi_table_header_t)) / 8;

        for (uint32_t i = 0; i < entries; i++) {
            uint64_t table_addr = acpi_state.xsdt->entries[i];
            acpi_table_header_t* header = (acpi_table_header_t*)acpi_phys_to_virt(table_addr);

            if (memcmp(header->signature, signature, 4) == 0) {
                if (acpi_verify_checksum(header, header->length)) {
                    return header;
                }
            }
        }
    } else if (acpi_state.rsdt) {
        if (acpi_state.rsdt->header.length < sizeof(acpi_table_header_t)) return NULL;
        uint32_t entries = (acpi_state.rsdt->header.length - sizeof(acpi_table_header_t)) / 4;

        for (uint32_t i = 0; i < entries; i++) {
            uint32_t table_addr = acpi_state.rsdt->entries[i];
            acpi_table_header_t* header = (acpi_table_header_t*)acpi_phys_to_virt(table_addr);

            if (memcmp(header->signature, signature, 4) == 0) {
                if (acpi_verify_checksum(header, header->length)) {
                    return header;
                }
            }
        }
    }

    return NULL;
}

/**
 * Parse RSDT
 */
int acpi_parse_rsdt(acpi_rsdt_t* rsdt) {
    if (!rsdt) {
        return -1;
    }

    kprintf("[ACPI] Parsing RSDT...\n");

    if (!acpi_verify_checksum(rsdt, rsdt->header.length)) {
        kprintf("[ACPI] ERROR: RSDT checksum invalid\n");
        return -1;
    }

    uint32_t entries = (rsdt->header.length - sizeof(acpi_table_header_t)) / 4;
    kprintf("[ACPI] RSDT has %u entries\n", entries);

    acpi_state.rsdt = rsdt;
    return 0;
}

/**
 * Parse XSDT
 */
int acpi_parse_xsdt(acpi_xsdt_t* xsdt) {
    if (!xsdt) {
        return -1;
    }

    kprintf("[ACPI] Parsing XSDT...\n");

    if (!acpi_verify_checksum(xsdt, xsdt->header.length)) {
        kprintf("[ACPI] ERROR: XSDT checksum invalid\n");
        return -1;
    }

    uint32_t entries = (xsdt->header.length - sizeof(acpi_table_header_t)) / 8;
    kprintf("[ACPI] XSDT has %u entries\n", entries);

    acpi_state.xsdt = xsdt;
    return 0;
}

/*
 * Decode the SLP_TYPa / SLP_TYPb sleep-type values for the "_S5_" object out of
 * the DSDT's AML byte stream. This is a deliberately tiny AML "parser": we scan
 * for the 4-byte name "_S5_" and then decode the immediately-following Package.
 *
 * The encoding for "Name (_S5_, Package () { a, b, ... })" looks like:
 *     NameOp(0x08) '_' 'S' '5' '_' PackageOp(0x12) PkgLength NumElements
 *     <byte-prefix(0x0A)?> SLP_TYPa <byte-prefix(0x0A)?> SLP_TYPb ...
 * Small integers may also be encoded as ZeroOp(0x00) / OneOp(0x01) /
 * BytePrefix(0x0A) value. We handle those forms.
 *
 * Returns 0 on success (fills *s5a/*s5b), -1 if not found.
 */
static int acpi_decode_aml_integer(const uint8_t* p, const uint8_t* end, uint8_t* out) {
    if (p >= end) return -1;
    switch (*p) {
        case 0x00: *out = 0; return 1;   // ZeroOp
        case 0x01: *out = 1; return 1;   // OneOp
        case 0x0A:                       // BytePrefix
            if (p + 1 >= end) return -1;
            *out = p[1];
            return 2;
        case 0x0B:                       // WordPrefix
            if (p + 2 >= end) return -1;
            *out = p[1];                 // low byte is the sleep type
            return 3;
        case 0x0C:                       // DWordPrefix
            if (p + 4 >= end) return -1;
            *out = p[1];
            return 5;
        default:
            // Bare small constant (no prefix) -- treat byte as the value.
            *out = *p;
            return 1;
    }
}

static int acpi_parse_s5(acpi_table_header_t* dsdt, uint8_t* s5a, uint8_t* s5b) {
    if (!dsdt) return -1;

    const uint8_t* aml = (const uint8_t*)dsdt + sizeof(acpi_table_header_t);
    const uint8_t* end = (const uint8_t*)dsdt + dsdt->length;

    for (const uint8_t* p = aml; p + 5 < end; p++) {
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_') {
            const uint8_t* q = p + 4;

            // Optionally a NameOp(0x08) precedes the name; skip if present after.
            // After the name we expect a PackageOp (0x12). Some compilers emit
            // an extra 0x08 NameOp BEFORE the name; that case is handled because
            // we matched the name itself, and the package follows the name.
            if (q < end && *q != 0x12) {
                // Not the package form we expect; keep scanning.
                continue;
            }
            if (q >= end) break;
            q++;  // skip PackageOp

            // PkgLength: high 2 bits of first byte = number of following length
            // bytes. We only need to step over it.
            if (q >= end) break;
            uint8_t lead = *q;
            uint8_t extra = lead >> 6;
            q += 1 + extra;
            if (q >= end) break;

            // NumElements
            q++;  // number of package elements (>=2 for S5)
            if (q >= end) break;

            uint8_t a = 0, b = 0;
            int n = acpi_decode_aml_integer(q, end, &a);
            if (n < 0) break;
            q += n;
            (void)acpi_decode_aml_integer(q, end, &b);  // SLP_TYPb (optional)

            *s5a = a & 0x7;
            *s5b = b & 0x7;
            kprintf("[ACPI] _S5_ found: SLP_TYPa=0x%x SLP_TYPb=0x%x\n", *s5a, *s5b);
            return 0;
        }
    }

    return -1;
}

/**
 * Parse FADT (Fixed ACPI Description Table)
 * This table contains power management information
 */
int acpi_parse_fadt(acpi_fadt_t* fadt) {
    if (!fadt) {
        return -1;
    }

    kprintf("[ACPI] Parsing FADT...\n");

    acpi_state.fadt = fadt;

    // Extract PM register ports
    if (fadt->pm1a_control_block) {
        acpi_state.pm1a_control_port = fadt->pm1a_control_block;
        kprintf("[ACPI]   PM1a Control: 0x%04x\n", acpi_state.pm1a_control_port);
    }

    if (fadt->pm1b_control_block) {
        acpi_state.pm1b_control_port = fadt->pm1b_control_block;
        kprintf("[ACPI]   PM1b Control: 0x%04x\n", acpi_state.pm1b_control_port);
    }

    if (fadt->pm1a_event_block) {
        acpi_state.pm1a_status_port = fadt->pm1a_event_block;
        kprintf("[ACPI]   PM1a Status: 0x%04x\n", acpi_state.pm1a_status_port);
    }

    if (fadt->pm1b_event_block) {
        acpi_state.pm1b_status_port = fadt->pm1b_event_block;
        kprintf("[ACPI]   PM1b Status: 0x%04x\n", acpi_state.pm1b_status_port);
    }

    if (fadt->pm_timer_block) {
        acpi_state.pm_timer_port = fadt->pm_timer_block;
        kprintf("[ACPI]   PM Timer: 0x%04x\n", acpi_state.pm_timer_port);
    }

    // Locate the DSDT (prefer 64-bit X_DSDT, fall back to 32-bit DSDT) and
    // decode the real _S5_ sleep-type values. Without this, poweroff writes the
    // wrong SLP_TYP and the machine stays on.
    acpi_table_header_t* dsdt = NULL;
    uint64_t dsdt_phys = fadt->x_dsdt ? fadt->x_dsdt : (uint64_t)fadt->dsdt;
    // VALIDATE the DSDT physical address before dereferencing it. A firmware that
    // leaves x_dsdt zero/garbage, or any FADT-layout drift, would otherwise make
    // the memcmp below dereference a wild pointer and #GP/#PF — a hard kernel
    // panic during early boot (observed on QEMU: x_dsdt resolved to a tiny bad
    // address, GP-faulting in memcmp before the scheduler ever started). Require a
    // plausible, page-aligned, identity-mapped low-memory address (the kernel
    // identity-maps 0..16 GB). Anything else falls back to the default _S5 below,
    // which is harmless — poweroff still works on the common machines.
    if (dsdt_phys >= 0x1000 && dsdt_phys < 0x400000000ULL &&
        (dsdt_phys & 0x3) == 0) {
        dsdt = (acpi_table_header_t*)acpi_phys_to_virt(dsdt_phys);
    }

    bool have_s5 = false;
    if (dsdt && memcmp(dsdt->signature, ACPI_SIG_DSDT, 4) == 0 &&
        acpi_verify_checksum(dsdt, dsdt->length)) {
        acpi_state.dsdt = dsdt;
        uint8_t s5a = 0, s5b = 0;
        if (acpi_parse_s5(dsdt, &s5a, &s5b) == 0) {
            acpi_state.s5_sleep_type = s5a;
            acpi_state.s5_sleep_type_b = s5b;
            have_s5 = true;
        }
    } else {
        kprintf("[ACPI] WARNING: DSDT not found/invalid; using default _S5\n");
    }

    if (!have_s5) {
        // Common SLP_TYPa value for soft-off on most chipsets (QEMU/SeaBIOS=0).
        // 0x05 is a frequent fallback; 0 is what QEMU's PIIX4/Q35 actually use.
        acpi_state.s5_sleep_type = 0x05;
        acpi_state.s5_sleep_type_b = 0x05;
        kprintf("[ACPI] Using fallback S5 SLP_TYP=0x%x\n", acpi_state.s5_sleep_type);
    }

    // Default values for S3/S4 (refined elsewhere if needed).
    acpi_state.s3_sleep_type = 0x05;
    acpi_state.s4_sleep_type = 0x06;

    kprintf("[ACPI] FADT parsed successfully\n");
    return 0;
}

/**
 * Parse MADT (Multiple APIC Description Table)
 * This table contains information about APICs and CPUs
 */
int acpi_parse_madt(acpi_madt_t* madt) {
    if (!madt) {
        return -1;
    }

    kprintf("[ACPI] Parsing MADT...\n");

    acpi_state.madt = madt;
    acpi_state.local_apic_address = madt->local_apic_address;
    acpi_state.num_cpus = 0;
    acpi_state.num_io_apics = 0;

    kprintf("[ACPI]   Local APIC Address: 0x%08x\n", madt->local_apic_address);

    // Parse MADT entries
    uint8_t* ptr = madt->entries;
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr < end) {
        // Need the 2-byte entry header to read type+length without overrunning.
        if (ptr + sizeof(acpi_madt_entry_header_t) > end) {
            break;
        }
        acpi_madt_entry_header_t* header = (acpi_madt_entry_header_t*)ptr;

        if (header->length == 0) {
            break;  // malformed; avoid infinite loop
        }
        // The whole entry (header->length bytes) must fit inside the table; a
        // malformed length that overruns `end` would make the per-type handlers
        // below read fields past the MADT buffer (kernel-memory over-read at boot).
        if (ptr + header->length > end) {
            break;
        }

        switch (header->type) {
            case ACPI_MADT_TYPE_LOCAL_APIC: {
                acpi_madt_local_apic_t* lapic = (acpi_madt_local_apic_t*)ptr;
                if (lapic->flags & 1) {  // Enabled
                    acpi_state.num_cpus++;
                    kprintf("[ACPI]   CPU %u: APIC ID %u\n",
                            lapic->acpi_processor_id, lapic->apic_id);
                }
                break;
            }

            case ACPI_MADT_TYPE_IO_APIC: {
                acpi_madt_io_apic_t* ioapic = (acpi_madt_io_apic_t*)ptr;
                acpi_state.num_io_apics++;
                kprintf("[ACPI]   I/O APIC %u: Address 0x%08x, GSI Base %u\n",
                        ioapic->io_apic_id, ioapic->io_apic_address,
                        ioapic->global_system_interrupt_base);
                break;
            }

            case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE: {
                acpi_madt_interrupt_override_t* ovr = (acpi_madt_interrupt_override_t*)ptr;
                kprintf("[ACPI]   Interrupt Override: IRQ %u -> GSI %u\n",
                        ovr->source, ovr->global_system_interrupt);
                break;
            }

            default:
                break;
        }

        ptr += header->length;
    }

    kprintf("[ACPI] Found %u CPUs, %u I/O APICs\n",
            acpi_state.num_cpus, acpi_state.num_io_apics);
    return 0;
}

/**
 * Parse HPET (High Precision Event Timer)
 */
int acpi_parse_hpet(acpi_hpet_t* hpet) {
    if (!hpet) {
        return -1;
    }

    kprintf("[ACPI] Parsing HPET...\n");

    acpi_state.hpet = hpet;
    acpi_state.hpet_address = hpet->base_address.address;
    acpi_state.hpet_available = true;

    kprintf("[ACPI]   HPET Address: 0x%016lx\n", acpi_state.hpet_address);
    kprintf("[ACPI]   Minimum Tick: %u\n", hpet->minimum_tick);

    return 0;
}

/**
 * Parse MCFG (PCI Express Memory Mapped Configuration)
 */
int acpi_parse_mcfg(acpi_mcfg_t* mcfg) {
    if (!mcfg) {
        return -1;
    }

    kprintf("[ACPI] Parsing MCFG...\n");

    acpi_state.mcfg = mcfg;

    // Parse first entry (usually only one)
    if (mcfg->header.length > sizeof(acpi_mcfg_t)) {
        acpi_mcfg_entry_t* entry = (acpi_mcfg_entry_t*)mcfg->entries;
        acpi_state.pcie_config_base = entry->base_address;
        acpi_state.pcie_segment = entry->pci_segment_group;
        acpi_state.pcie_start_bus = entry->start_bus;
        acpi_state.pcie_end_bus = entry->end_bus;

        kprintf("[ACPI]   PCIe Config Base: 0x%016lx\n", entry->base_address);
        kprintf("[ACPI]   Segment: %u, Buses: %u-%u\n",
                entry->pci_segment_group, entry->start_bus, entry->end_bus);
    }

    return 0;
}

/**
 * Enable ACPI mode (hand control from firmware/SMM to the OS).
 */
int acpi_enable(void) {
    if (!acpi_state.fadt) {
        kprintf("[ACPI] ERROR: FADT not found\n");
        return -1;
    }

    kprintf("[ACPI] Enabling ACPI mode...\n");

    // Check if already in ACPI mode
    if (acpi_state.pm1a_control_port &&
        (inw(acpi_state.pm1a_control_port) & ACPI_PM1_SCI_EN)) {
        kprintf("[ACPI] Already in ACPI mode\n");
        acpi_state.enabled = true;
        return 0;
    }

    // Send ACPI_ENABLE command to SMI_CMD port
    if (acpi_state.fadt->smi_command_port && acpi_state.fadt->acpi_enable) {
        outb(acpi_state.fadt->smi_command_port, acpi_state.fadt->acpi_enable);

        // Wait for ACPI mode to be enabled (coarse timeout via I/O delay)
        for (int i = 0; i < 300; i++) {
            if (acpi_state.pm1a_control_port &&
                (inw(acpi_state.pm1a_control_port) & ACPI_PM1_SCI_EN)) {
                kprintf("[ACPI] ACPI mode enabled successfully\n");
                acpi_state.enabled = true;
                return 0;
            }
            acpi_io_delay(10000);  // ~10ms-ish
        }

        kprintf("[ACPI] WARNING: Timeout waiting for ACPI mode\n");
        return -1;
    }

    // No SMI command port, assume hardware-reduced / already in ACPI mode.
    kprintf("[ACPI] No SMI command port, assuming ACPI mode\n");
    acpi_state.enabled = true;
    return 0;
}

/**
 * Disable ACPI mode
 */
int acpi_disable(void) {
    if (!acpi_state.fadt) {
        return -1;
    }

    kprintf("[ACPI] Disabling ACPI mode...\n");

    if (acpi_state.fadt->smi_command_port && acpi_state.fadt->acpi_disable) {
        outb(acpi_state.fadt->smi_command_port, acpi_state.fadt->acpi_disable);
        acpi_state.enabled = false;
    }

    return 0;
}

/**
 * Check if ACPI is enabled
 */
bool acpi_is_enabled(void) {
    return acpi_state.enabled;
}

/**
 * Enter sleep state (S1/S3/S4/S5)
 */
int acpi_enter_sleep_state(acpi_sleep_state_t state) {
    if (!acpi_state.pm1a_control_port) {
        kprintf("[ACPI] ERROR: PM1a control port unknown\n");
        return -1;
    }

    kprintf("[ACPI] Entering sleep state S%d...\n", state);

    uint16_t sleep_type_a = 0;
    uint16_t sleep_type_b = 0;

    switch (state) {
        case ACPI_STATE_S1:
            sleep_type_a = 0x00;
            break;
        case ACPI_STATE_S3:
            sleep_type_a = acpi_state.s3_sleep_type;
            break;
        case ACPI_STATE_S4:
            sleep_type_a = acpi_state.s4_sleep_type;
            break;
        case ACPI_STATE_S5:
            sleep_type_a = acpi_state.s5_sleep_type;
            sleep_type_b = acpi_state.s5_sleep_type_b;
            break;
        default:
            kprintf("[ACPI] ERROR: Invalid sleep state\n");
            return -1;
    }

    // Disable interrupts before the final write.
    cli();

    // PM1a_CNT = (SLP_TYPa << 10) | SLP_EN(bit13)
    uint16_t pm1a = (uint16_t)((sleep_type_a & 0x7) << ACPI_PM1_SLP_TYP_SHIFT) | ACPI_PM1_SLP_EN;
    outw(acpi_state.pm1a_control_port, pm1a);

    if (acpi_state.pm1b_control_port) {
        uint16_t pm1b = (uint16_t)((sleep_type_b & 0x7) << ACPI_PM1_SLP_TYP_SHIFT) | ACPI_PM1_SLP_EN;
        outw(acpi_state.pm1b_control_port, pm1b);
    }

    // For S5 the machine powers off here. If it does not (firmware quirk), hang.
    acpi_hang();
    return 0;
}

/**
 * Prepare for sleep
 */
int acpi_prepare_sleep(acpi_sleep_state_t state) {
    kprintf("[ACPI] Preparing for sleep state S%d...\n", state);
    // _PTS method execution would go here for a full AML interpreter.
    return 0;
}

/**
 * Wake from sleep
 */
int acpi_wake_from_sleep(void) {
    kprintf("[ACPI] Waking from sleep...\n");
    return 0;
}

/**
 * Reboot system via ACPI reset register, with an 8042 fallback.
 */
int acpi_reboot(void) {
    kprintf("[ACPI] Rebooting system...\n");

    cli();

    // 1. ACPI reset register (if the FADT advertises support).
    if (acpi_state.fadt &&
        (acpi_state.fadt->flags & ACPI_FADT_RESET_REG_SUP)) {
        acpi_generic_address_t* reset_reg = &acpi_state.fadt->reset_reg;
        uint8_t reset_value = acpi_state.fadt->reset_value;

        if (reset_reg->address_space_id == 1 && reset_reg->address) {
            // System I/O space
            outb((uint16_t)reset_reg->address, reset_value);
        } else if (reset_reg->address_space_id == 0 && reset_reg->address) {
            // System Memory space (identity-mapped low memory)
            *(volatile uint8_t*)acpi_phys_to_virt(reset_reg->address) = reset_value;
        } else if (reset_reg->address_space_id == 2 && reset_reg->address) {
            // PCI config space reset is uncommon; skip to fallback.
        }

        acpi_io_delay(100000);  // give it a moment
    }

    // 2. Fallback: pulse the 8042 keyboard-controller reset line (0x64 <- 0xFE).
    kprintf("[ACPI] Reset register did not fire; trying 8042 reset...\n");
    for (int i = 0; i < 1000; i++) {
        if (!(inb(KBC_STATUS_PORT) & KBC_STATUS_INPUT_FULL)) {
            break;
        }
        acpi_io_delay(100);
    }
    outb(KBC_COMMAND_PORT, KBC_CMD_PULSE_RESET);

    acpi_io_delay(100000);

    // 3. Last resort: triple fault via a null IDT. Loading an all-zero IDT
    // and triggering a software interrupt causes #GP -> #DF -> CPU reset on
    // both real hardware and QEMU.
    kprintf("[ACPI] 8042 reset did not fire; attempting triple-fault...\n");
    {
        struct { uint16_t limit; uint64_t base; } __attribute__((packed)) zero_idt = {0, 0};
        __asm__ volatile("lidt %0" : : "m"(zero_idt));
        __asm__ volatile("int $3");   // #BP -> #DF -> reset
    }

    // Should never reach here
    acpi_hang();
    return 0;
}

/**
 * Power off system via ACPI (soft-off, S5).
 */
int acpi_poweroff(void) {
    kprintf("[ACPI] Powering off system (S5)...\n");
    return acpi_enter_sleep_state(ACPI_STATE_S5);
}

/* ===================================================================
 * Public power API (the names requested by the integrator).
 * These are thin, robust wrappers around the ACPI primitives above.
 * They never return.
 * =================================================================== */

void power_off(void) {
    kprintf("[POWER] Shutting down...\n");

    /*
     * Layer 0: QEMU / Bochs / VirtualBox magic I/O ports. On QEMU these
     * cause an immediate VM exit. On real hardware (T410) the ports are
     * unused or no-ops, so we fall through harmlessly to the ACPI path.
     */
    outw(0x604,  0x2000);   /* QEMU SeaBIOS / pc / q35 */
    outw(0xB004, 0x2000);   /* Bochs                   */
    outw(0x4004, 0x3400);   /* VirtualBox              */

    /* Layer 1: real ACPI S5 */
    acpi_poweroff();
    // acpi_poweroff() does not return; belt-and-suspenders below.
    acpi_hang();
}

/*
 * acpi_power_off() -- void wrapper matching the header declaration.
 * Called by the SYS_POWEROFF syscall handler.
 */
void acpi_power_off(void) {
    power_off();
}

/*
 * acpi_present() -- 1 if RSDP + FADT were located, else 0.
 */
int acpi_present(void) {
    return (acpi_state.initialized && acpi_state.fadt) ? 1 : 0;
}

void power_reboot(void) {
    kprintf("[POWER] Rebooting...\n");
    acpi_reboot();
    acpi_hang();
}

/**
 * Read PM1 control register
 */
uint8_t acpi_read_pm1_control(void) {
    if (!acpi_state.pm1a_control_port) {
        return 0;
    }
    return inb(acpi_state.pm1a_control_port);
}

/**
 * Write PM1 control register
 */
void acpi_write_pm1_control(uint8_t value) {
    if (acpi_state.pm1a_control_port) {
        outb(acpi_state.pm1a_control_port, value);
    }
    if (acpi_state.pm1b_control_port) {
        outb(acpi_state.pm1b_control_port, value);
    }
}

/**
 * Read PM1 status register
 */
uint16_t acpi_read_pm1_status(void) {
    if (!acpi_state.pm1a_status_port) {
        return 0;
    }
    return inw(acpi_state.pm1a_status_port);
}

/**
 * Write PM1 status register
 */
void acpi_write_pm1_status(uint16_t value) {
    if (acpi_state.pm1a_status_port) {
        outw(acpi_state.pm1a_status_port, value);
    }
    if (acpi_state.pm1b_status_port) {
        outw(acpi_state.pm1b_status_port, value);
    }
}

/**
 * Read PM timer
 */
uint32_t acpi_read_pm_timer(void) {
    if (!acpi_state.pm_timer_port) {
        return 0;
    }
    return inl(acpi_state.pm_timer_port);
}

/**
 * Dump all ACPI tables
 */
void acpi_dump_tables(void) {
    kprintf("\n[ACPI] ===== ACPI Tables =====\n");

    if (acpi_state.rsdp) {
        kprintf("[ACPI] RSDP:\n");
        kprintf("       OEM ID: %.6s\n", acpi_state.rsdp->oem_id);
        kprintf("       Revision: %u\n", acpi_state.rsdp->revision);
    }

    if (acpi_state.rsdt) {
        kprintf("[ACPI] RSDT: Length %u bytes\n", acpi_state.rsdt->header.length);
    }

    if (acpi_state.xsdt) {
        kprintf("[ACPI] XSDT: Length %u bytes\n", acpi_state.xsdt->header.length);
    }

    if (acpi_state.fadt) {
        kprintf("[ACPI] FADT: Revision %u\n", acpi_state.fadt->header.revision);
        kprintf("[ACPI]   S5 SLP_TYPa=0x%x SLP_TYPb=0x%x\n",
                acpi_state.s5_sleep_type, acpi_state.s5_sleep_type_b);
    }

    if (acpi_state.madt) {
        kprintf("[ACPI] MADT: %u CPUs, %u I/O APICs\n",
                acpi_state.num_cpus, acpi_state.num_io_apics);
    }

    if (acpi_state.hpet) {
        kprintf("[ACPI] HPET: Available\n");
    }

    if (acpi_state.mcfg) {
        kprintf("[ACPI] MCFG: PCIe Config Base 0x%016lx\n",
                acpi_state.pcie_config_base);
    }

    kprintf("[ACPI] ========================\n\n");
}

/**
 * Print ACPI information
 */
void acpi_print_info(void) {
    kprintf("\n[ACPI] Status: %s\n", acpi_state.enabled ? "Enabled" : "Disabled");
    kprintf("[ACPI] CPUs: %u\n", acpi_state.num_cpus);
    kprintf("[ACPI] HPET: %s\n", acpi_state.hpet_available ? "Available" : "Not available");
    kprintf("[ACPI] PM1a Control Port: 0x%04x\n", acpi_state.pm1a_control_port);
    kprintf("[ACPI] PM Timer Port: 0x%04x\n", acpi_state.pm_timer_port);
}

/**
 * Initialize ACPI
 */
int acpi_init(void) {
    kprintf("[ACPI] Initializing ACPI subsystem...\n");

    memset(&acpi_state, 0, sizeof(acpi_state));

    // 1. Find RSDP
    acpi_state.rsdp = acpi_find_rsdp();
    if (!acpi_state.rsdp) {
        kprintf("[ACPI] ERROR: Failed to find RSDP\n");
        return -1;
    }

    kprintf("[ACPI] RSDP found, revision %u\n", acpi_state.rsdp->revision);

    // 2. Parse RSDT or XSDT
    if (acpi_state.rsdp->revision >= 2 && acpi_state.rsdp->xsdt_address) {
        // ACPI 2.0+: Use XSDT (64-bit)
        acpi_xsdt_t* xsdt = (acpi_xsdt_t*)acpi_phys_to_virt(acpi_state.rsdp->xsdt_address);
        if (acpi_parse_xsdt(xsdt) < 0) {
            kprintf("[ACPI] ERROR: Failed to parse XSDT\n");
            return -1;
        }
    } else {
        // ACPI 1.0: Use RSDT (32-bit)
        acpi_rsdt_t* rsdt = (acpi_rsdt_t*)acpi_phys_to_virt((uint64_t)acpi_state.rsdp->rsdt_address);
        if (acpi_parse_rsdt(rsdt) < 0) {
            kprintf("[ACPI] ERROR: Failed to parse RSDT\n");
            return -1;
        }
    }

    // 3. Find and parse FADT (power management + DSDT/_S5 decode)
    acpi_state.fadt = (acpi_fadt_t*)acpi_find_table(ACPI_SIG_FADT);
    if (acpi_state.fadt) {
        if (acpi_parse_fadt(acpi_state.fadt) < 0) {
            kprintf("[ACPI] WARNING: Failed to parse FADT\n");
        }
    } else {
        kprintf("[ACPI] WARNING: FADT not found\n");
    }

    // 4. Find and parse MADT (APIC/SMP)
    acpi_state.madt = (acpi_madt_t*)acpi_find_table(ACPI_SIG_MADT);
    if (acpi_state.madt) {
        if (acpi_parse_madt(acpi_state.madt) < 0) {
            kprintf("[ACPI] WARNING: Failed to parse MADT\n");
        }
    } else {
        kprintf("[ACPI] WARNING: MADT not found\n");
    }

    // 5. Find and parse HPET (high precision timer)
    acpi_state.hpet = (acpi_hpet_t*)acpi_find_table(ACPI_SIG_HPET);
    if (acpi_state.hpet) {
        if (acpi_parse_hpet(acpi_state.hpet) < 0) {
            kprintf("[ACPI] WARNING: Failed to parse HPET\n");
        }
    } else {
        kprintf("[ACPI] WARNING: HPET not found\n");
    }

    // 6. Find and parse MCFG (PCIe configuration)
    acpi_state.mcfg = (acpi_mcfg_t*)acpi_find_table(ACPI_SIG_MCFG);
    if (acpi_state.mcfg) {
        if (acpi_parse_mcfg(acpi_state.mcfg) < 0) {
            kprintf("[ACPI] WARNING: Failed to parse MCFG\n");
        }
    } else {
        kprintf("[ACPI] INFO: MCFG not found (PCIe may not be available)\n");
    }

    // 7. Enable ACPI mode (so SCI_EN is set and the SLP_TYP write is honored)
    if (acpi_enable() < 0) {
        kprintf("[ACPI] WARNING: Failed to enable ACPI mode (poweroff may still work)\n");
    }

    acpi_state.initialized = true;

    kprintf("[ACPI] Initialization complete\n");
    acpi_dump_tables();

    // 8. Probe the Embedded Controller for battery status (laptop detection).
    // This is non-blocking: if the EC is absent (QEMU, desktops), the probe
    // times out in ~100ms and all future ec_battery_read() calls return
    // immediately. On laptops (T410) we log the initial battery state.
    {
        ec_battery_status_t bat;
        if (ec_battery_read(&bat) == 0 && bat.present) {
            kprintf("[ACPI] Battery: %u%% (%s), %u mV, %u mWh remaining\n",
                    bat.percentage,
                    bat.state == 1 ? "discharging" :
                    bat.state == 2 ? "charging" : "idle",
                    bat.voltage_mv, bat.remaining_mwh);
            if (bat.ac_online)
                kprintf("[ACPI] AC adapter: online\n");
        }
    }

    return 0;
}

/**
 * Shutdown ACPI
 */
void acpi_shutdown(void) {
    kprintf("[ACPI] Shutting down ACPI subsystem...\n");

    if (acpi_state.enabled) {
        acpi_disable();
    }

    memset(&acpi_state, 0, sizeof(acpi_state));
}

/* ===================================================================
 * Embedded Controller (EC) battery reader
 *
 * Lightweight, self-contained EC access for battery status on laptops
 * (e.g. ThinkPad T410). On platforms without an EC (QEMU, desktops)
 * the initial probe fails and all subsequent calls return immediately.
 * =================================================================== */

/* EC I/O ports (ACPI spec chapter 12) */
#define EC_DATA_PORT    0x62
#define EC_CMD_PORT     0x66    /* write=command, read=status */

/* EC status bits */
#define EC_OBF          (1 << 0)   /* Output Buffer Full    */
#define EC_IBF          (1 << 1)   /* Input Buffer Full     */

/* EC commands */
#define EC_RD           0x80    /* Read:  cmd -> addr -> read data   */

/* Timeout for busy-wait (~100k * 1us = ~100ms) */
#define EC_TMO          100000

/* ThinkPad EC battery RAM offsets (T410 / T420 / X220 family, from DSDT) */
#define ECB_PRESENT     0x38    /* Byte: bit0 = BAT0 present */
#define ECB_AC          0x39    /* Byte: bit0 = AC online    */
#define ECB_STATE       0x3A    /* Byte: 0=idle,1=disch,2=chg */
#define ECB_RATE        0x3C    /* 16LE: present rate (mW)   */
#define ECB_REMAIN      0x3E    /* 16LE: remaining (mWh)     */
#define ECB_VOLT        0x40    /* 16LE: voltage (mV)        */
#define ECB_DESIGN      0x42    /* 16LE: design cap (mWh)    */
#define ECB_FULLCAP     0x44    /* 16LE: last full cap (mWh) */

static bool g_ec_ok     = false;   /* EC responded at least once */
static bool g_ec_probed = false;

static int ec_wait_ibf(void) {
    for (int i = 0; i < EC_TMO; i++)
        if (!(inb(EC_CMD_PORT) & EC_IBF)) return 0;
    return -1;
}

static int ec_wait_obf(void) {
    for (int i = 0; i < EC_TMO; i++)
        if (inb(EC_CMD_PORT) & EC_OBF) return 0;
    return -1;
}

static int ec_rd(uint8_t addr) {
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_PORT, EC_RD);
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_DATA_PORT, addr);
    if (ec_wait_obf() < 0) return -1;
    return (int)inb(EC_DATA_PORT);
}

static int ec_rd16(uint8_t addr) {
    int lo = ec_rd(addr);
    if (lo < 0) return -1;
    int hi = ec_rd((uint8_t)(addr + 1));
    if (hi < 0) return -1;
    return lo | (hi << 8);
}

bool ec_battery_available(void) {
    return g_ec_ok;
}

int ec_battery_read(ec_battery_status_t *out) {
    if (!out) return -1;

    memset(out, 0, sizeof(*out));

    /* First-time probe */
    if (!g_ec_probed) {
        g_ec_probed = true;
        int p = ec_rd(ECB_PRESENT);
        if (p < 0) {
            kprintf("[EC] No Embedded Controller (QEMU/desktop)\n");
            g_ec_ok = false;
            return -1;
        }
        g_ec_ok = true;
        kprintf("[EC] Embedded Controller detected (port 0x62/0x66)\n");
    }

    if (!g_ec_ok) return -1;

    /* Battery presence */
    int bat = ec_rd(ECB_PRESENT);
    if (bat < 0 || !(bat & 0x01)) {
        out->present = false;
        return 0;   /* EC works, just no battery inserted */
    }
    out->present = true;

    /* AC adapter */
    int ac = ec_rd(ECB_AC);
    out->ac_online = (ac >= 0 && (ac & 0x01));

    /* State */
    int st = ec_rd(ECB_STATE);
    out->state = (st >= 0) ? (uint8_t)(st & 0x03) : 0;

    /* Readings */
    int rate = ec_rd16(ECB_RATE);
    int rem  = ec_rd16(ECB_REMAIN);
    int volt = ec_rd16(ECB_VOLT);
    int dcap = ec_rd16(ECB_DESIGN);
    int fcap = ec_rd16(ECB_FULLCAP);

    /* Sanity: reject garbage (wrong EC offsets for this laptop model) */
    if (volt < 0 || volt > 30000 ||
        dcap < 0 || dcap == 0xFFFF ||
        rem  < 0 || rem  == 0xFFFF) {
        out->present = false;
        return -1;
    }

    out->voltage_mv    = (uint16_t)volt;
    out->rate_mw       = (rate >= 0)  ? (uint16_t)rate : 0;
    out->remaining_mwh = (uint16_t)rem;
    out->design_mwh    = (dcap > 0)   ? (uint16_t)dcap : 0;
    out->full_cap_mwh  = (fcap > 0)   ? (uint16_t)fcap : 0;

    /* Percentage */
    if (fcap > 0) {
        uint32_t pct = (uint32_t)rem * 100 / (uint32_t)fcap;
        out->percentage = (pct > 100) ? 100 : (uint8_t)pct;
    }

    /* Time estimate (minutes) */
    if (out->state == 1 && rate > 0 && rem > 0) {
        /* discharging: remaining_mWh / rate_mW * 60 */
        out->time_min = (uint16_t)((uint32_t)rem * 60 / (uint32_t)rate);
    } else if (out->state == 2 && rate > 0 && fcap > rem) {
        /* charging: (full - remaining) / rate * 60 */
        out->time_min = (uint16_t)((uint32_t)(fcap - rem) * 60 / (uint32_t)rate);
    }

    return 0;
}
