/*
 * iwl-devices.h -- candidate Intel iwlwifi PCI device IDs for the ThinkPad T410.
 * =============================================================================
 * The T410 (Calpella/Arrandale platform, 2010) ships an Intel half-mini PCIe
 * WiFi card from the iwlwifi families. Lenovo's FRU whitelist means it is almost
 * always one of these. IWL-IDENT scans for any of them; the matched name tells
 * us which firmware family (1000 / 5000 / 6000) the later bricks must load.
 *
 * NOTE: only IWL-IDENT (detect + safe probe) uses this today. The firmware load,
 * RF init, and association are the hardware tail and need the physical T410.
 */
#ifndef IWL_DEVICES_H
#define IWL_DEVICES_H

#include "types.h"

#define IWL_VENDOR_INTEL  0x8086

typedef struct {
    uint16_t    device;
    const char* name;
    const char* fw_family;   /* the iwlwifi-<family>-*.ucode the driver will need */
} iwl_device_t;

/* Candidate T410 WiFi cards (device id, friendly name, firmware family). */
static const iwl_device_t iwl_devices[] = {
    { 0x4239, "Intel Centrino Advanced-N 6200",  "6000" },
    { 0x4238, "Intel Centrino Ultimate-N 6300",  "6000" },
    { 0x422B, "Intel Centrino Ultimate-N 6300",  "6000" },
    { 0x0085, "Intel Centrino Advanced-N 6205",  "6000g2a" },
    { 0x0083, "Intel WiFi Link 1000 BGN",        "1000" },
    { 0x0084, "Intel WiFi Link 1000 BGN",        "1000" },
    { 0x4232, "Intel WiFi Link 5100 AGN",        "5000" },
    { 0x4237, "Intel WiFi Link 5100 AGN",        "5000" },
    { 0x4235, "Intel Ultimate-N 5300 AGN",       "5000" },
    { 0x4236, "Intel Ultimate-N 5300 AGN",       "5000" },
};
#define IWL_NDEVICES ((int)(sizeof(iwl_devices) / sizeof(iwl_devices[0])))

#endif /* IWL_DEVICES_H */
