/*
 * iwl-pci.c -- Intel iwlwifi: card identification + SAFE probe (IWL-IDENT).
 * ========================================================================
 * Brick 1 of the real Intel WiFi driver. Detects the T410's iwlwifi card over
 * the candidate PCI IDs (iwl-devices.h), enables MMIO + bus-master, maps BAR0,
 * and reads CSR_HW_REV -- a single, side-effect-free MMIO read. Then it STOPS.
 *
 * It deliberately does NOT power up the APM, load firmware, or touch the uCode
 * rings (those are IWL-TRANS / IWL-LOAD / IWL-OPS, the hardware tail that has no
 * QEMU coverage and needs the physical T410 across many flash->boot cycles).
 *
 * Gated -DIWLWIFI (DEFAULT OFF), mirroring the e1000 PCH pattern (LAW: firmware-
 * driven init that can hang un-validated hardware stays opt-in). In QEMU there
 * is no iwlwifi card, so iwl_init() prints "no Intel WiFi card found" and
 * returns cleanly -- that graceful absence IS the QEMU acceptance test. On a
 * real T410 it prints the card name + its HW revision.
 *
 * Once IWL-OPS can scan, this driver registers a netif + wifi_ops behind the
 * SAME seam as wifisim -- swapping the sim for the real radio touches nothing
 * above the seam (the SYS_WLAN_* syscalls, the supplicant, the GUI).
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-pci.c
 */
#include "types.h"
#include "pci.h"
#include "kernel.h"          /* kprintf */
#include "iwl-devices.h"

/* CSR registers (the few this safe probe touches). */
#define IWL_CSR_HW_REV    0x028   /* hardware revision -- read-only, no side effects */

static inline uint32_t iwl_read32(volatile uint8_t* bar, uint32_t off) {
    return *(volatile uint32_t*)(bar + off);
}

/*
 * iwl_init -- IWL-IDENT entry. Called once at boot under #ifdef IWLWIFI from
 * kernel.c. Detect + safe-probe only; never blocks, never powers the radio.
 */
void iwl_init(void) {
    pci_device_t* dev = (pci_device_t*)0;
    const char* name = (const char*)0;
    const char* fw   = (const char*)0;

    for (int i = 0; i < IWL_NDEVICES; i++) {
        dev = pci_find_device(IWL_VENDOR_INTEL, iwl_devices[i].device);
        if (dev) { name = iwl_devices[i].name; fw = iwl_devices[i].fw_family; break; }
    }

    if (!dev) {
        /* QEMU (no iwlwifi card) lands here -- the graceful-absence acceptance. */
        kprintf("IWL: no Intel WiFi card found (none of the T410 candidates present)\n");
        return;
    }

    kprintf("IWL: found %s [%04x:%04x] at %02x:%02x.%x (fw family iwlwifi-%s)\n",
            name, dev->vendor_id, dev->device_id,
            dev->bus, dev->device, dev->function, fw);

    /* Enable MMIO + bus-master; map BAR0 (the CSR + PRPH window). */
    pci_enable_memory_space(dev);
    pci_enable_bus_master(dev);
    uint64_t bar0 = pci_get_bar(dev, 0);
    if (!bar0) {
        kprintf("IWL: BAR0 not mapped -- aborting safe probe (no MMIO touch)\n");
        return;
    }
    volatile uint8_t* mmio = (volatile uint8_t*)(bar0 & ~0xFULL);

    /* SAFE probe ONLY: a single read of CSR_HW_REV. No APM power-up, no reset,
     * no firmware -- those are the deferred hardware bring-up bricks. */
    uint32_t hw_rev = iwl_read32(mmio, IWL_CSR_HW_REV);
    kprintf("IWL: CSR_HW_REV=0x%08x (safe probe only)\n", hw_rev);
    kprintf("IWL: IDENT ok -- APM/firmware/RF bring-up is the T410 hardware tail "
            "(IWL-TRANS/LOAD/OPS), deferred + un-QEMU-testable\n");
}
