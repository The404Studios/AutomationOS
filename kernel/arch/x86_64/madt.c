/*
 * madt.c -- Standalone, READ-ONLY ACPI MADT CPU enumerator (SMP brick 0).
 *
 * Purpose: make the kernel AWARE of how many CPUs the firmware reports, without
 * changing any behavior. The system stays SINGLE-CORE -- this only emits a log
 * line ("SMP: detected N cpus"). No APIC writes, no SIPI, no AP bring-up.
 *
 * This file is deliberately self-contained: it reuses only the STRUCT layouts
 * from kernel/include/acpi.h (structs emit no linker symbols) and defines a
 * single new public symbol, madt_count_cpus(). It does NOT include or depend on
 * kernel/acpi/acpi.c or kernel/drivers/acpi/acpi.c, and it shares no symbol with
 * the real build. All helpers are static.
 *
 * Memory access model (mirrors kernel/acpi/acpi.c exactly):
 *   ACPI firmware tables live in LOW physical memory. boot.asm identity-maps
 *   0..512MB at boot (and VMM later extends the identity map to all RAM), so a
 *   physical address is reachable simply by casting it to a pointer. We use the
 *   same trivial identity translation acpi.c uses (acpi_phys_to_virt). Using any
 *   other scheme (e.g. a higher-half offset) would fault, because this tree has
 *   no phys_to_virt() helper and the tables are mapped 1:1.
 */

#include "../../include/acpi.h"   /* struct layouts only (no symbols) */
#include "../../include/kernel.h" /* kprintf */
#include "../../include/string.h" /* memcmp */
#include "../../include/types.h"

/* RSDP search windows (same constants acpi.c uses). */
#define MADT_RSDP_SEARCH_START 0x000E0000u
#define MADT_RSDP_SEARCH_END   0x000FFFFFu
#define MADT_RSDP_SEARCH_STEP  16u
#define MADT_EBDA_PTR          0x0000040Eu /* word: EBDA segment >> 4 */
#define MADT_EBDA_SEARCH_SIZE  1024u

/*
 * Identity translation for low-memory physical addresses. Low physical RAM is
 * identity-mapped by boot.asm, so the firmware tables are reachable at their
 * physical address. This matches acpi.c's acpi_phys_to_virt() byte-for-byte.
 */
static inline void *madt_phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)phys;
}

/* Sum of `length` bytes must be 0 for a valid ACPI structure. */
static int madt_checksum_ok(const void *table, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

/* Scan EBDA + the BIOS ROM area for the "RSD PTR " signature; validate the
 * 20-byte ACPI 1.0 checksum. Returns NULL if not found. */
static acpi_rsdp_t *madt_find_rsdp(void)
{
    /* 1. Extended BIOS Data Area (segment stored as a word at 0x40E). */
    uint16_t ebda_seg = *(volatile uint16_t *)madt_phys_to_virt(MADT_EBDA_PTR);
    uint64_t ebda_addr = ((uint64_t)ebda_seg) << 4;

    if (ebda_addr >= 0x80000 && ebda_addr < 0xA0000) {
        for (uint64_t addr = ebda_addr;
             addr < ebda_addr + MADT_EBDA_SEARCH_SIZE;
             addr += MADT_RSDP_SEARCH_STEP) {
            acpi_rsdp_t *rsdp = (acpi_rsdp_t *)madt_phys_to_virt(addr);
            if (memcmp(rsdp->signature, ACPI_SIG_RSDP, 8) == 0 &&
                madt_checksum_ok(rsdp, 20)) {
                return rsdp;
            }
        }
    }

    /* 2. Main BIOS area 0xE0000 - 0xFFFFF. */
    for (uint64_t addr = MADT_RSDP_SEARCH_START;
         addr < MADT_RSDP_SEARCH_END;
         addr += MADT_RSDP_SEARCH_STEP) {
        acpi_rsdp_t *rsdp = (acpi_rsdp_t *)madt_phys_to_virt(addr);
        if (memcmp(rsdp->signature, ACPI_SIG_RSDP, 8) == 0 &&
            madt_checksum_ok(rsdp, 20)) {
            return rsdp;
        }
    }

    return NULL;
}

/* Locate the MADT ("APIC") via the XSDT (ACPI >= 2.0, 64-bit pointers) or the
 * RSDT (ACPI 1.0, 32-bit pointers). Returns NULL if not present/invalid. */
static acpi_madt_t *madt_find(acpi_rsdp_t *rsdp)
{
    if (!rsdp) {
        return NULL;
    }

    /* Prefer the XSDT on ACPI 2.0+. */
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        acpi_xsdt_t *xsdt =
            (acpi_xsdt_t *)madt_phys_to_virt(rsdp->xsdt_address);
        if (memcmp(xsdt->header.signature, ACPI_SIG_XSDT, 4) != 0 ||
            xsdt->header.length < sizeof(acpi_table_header_t) ||
            !madt_checksum_ok(xsdt, xsdt->header.length)) {
            return NULL;
        }
        uint32_t entries =
            (xsdt->header.length - sizeof(acpi_table_header_t)) / 8;
        for (uint32_t i = 0; i < entries; i++) {
            acpi_table_header_t *hdr =
                (acpi_table_header_t *)madt_phys_to_virt(xsdt->entries[i]);
            if (memcmp(hdr->signature, ACPI_SIG_MADT, 4) == 0 &&
                hdr->length >= sizeof(acpi_madt_t) &&
                madt_checksum_ok(hdr, hdr->length)) {
                return (acpi_madt_t *)hdr;
            }
        }
        return NULL;
    }

    /* ACPI 1.0: use the RSDT. */
    if (rsdp->rsdt_address) {
        acpi_rsdt_t *rsdt =
            (acpi_rsdt_t *)madt_phys_to_virt((uint64_t)rsdp->rsdt_address);
        if (memcmp(rsdt->header.signature, ACPI_SIG_RSDT, 4) != 0 ||
            rsdt->header.length < sizeof(acpi_table_header_t) ||
            !madt_checksum_ok(rsdt, rsdt->header.length)) {
            return NULL;
        }
        uint32_t entries =
            (rsdt->header.length - sizeof(acpi_table_header_t)) / 4;
        for (uint32_t i = 0; i < entries; i++) {
            acpi_table_header_t *hdr =
                (acpi_table_header_t *)madt_phys_to_virt(
                    (uint64_t)rsdt->entries[i]);
            if (memcmp(hdr->signature, ACPI_SIG_MADT, 4) == 0 &&
                hdr->length >= sizeof(acpi_madt_t) &&
                madt_checksum_ok(hdr, hdr->length)) {
                return (acpi_madt_t *)hdr;
            }
        }
    }

    return NULL;
}

/*
 * madt_count_cpus -- count ENABLED processors reported by the ACPI MADT.
 *
 * Walks type-0 (Local APIC) entries and counts those with flags bit0 set. The
 * walk is bounded by the MADT's declared length and guards against a zero-length
 * entry (which would otherwise loop forever). On ANY missing/malformed table we
 * fall back to 1 (assume just the bootstrap processor) rather than faulting --
 * this routine must never crash the boot.
 *
 * READ-ONLY: it only reads identity-mapped ACPI memory. No MMIO/APIC writes.
 */
int madt_count_cpus(void)
{
    acpi_rsdp_t *rsdp = madt_find_rsdp();
    if (!rsdp) {
        return 1; /* no ACPI -> assume single BSP */
    }

    acpi_madt_t *madt = madt_find(rsdp);
    if (!madt) {
        return 1; /* no MADT -> assume single BSP */
    }

    int count = 0;
    const uint8_t *ptr = madt->entries;
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (ptr + sizeof(acpi_madt_entry_header_t) <= end) {
        const acpi_madt_entry_header_t *eh =
            (const acpi_madt_entry_header_t *)ptr;

        if (eh->length == 0) {
            break; /* malformed; avoid an infinite loop */
        }
        if (ptr + eh->length > end) {
            break; /* entry would run past the table; stop */
        }

        if (eh->type == ACPI_MADT_TYPE_LOCAL_APIC &&
            eh->length >= sizeof(acpi_madt_local_apic_t)) {
            const acpi_madt_local_apic_t *lapic =
                (const acpi_madt_local_apic_t *)ptr;
            if (lapic->flags & 1u) { /* bit0 = processor enabled */
                count++;
            }
        }

        ptr += eh->length;
    }

    /* If the MADT had no enabled LAPIC entries (shouldn't happen on real HW),
     * still report at least the bootstrap processor. */
    return count > 0 ? count : 1;
}
