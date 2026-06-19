/*
 * iwl-fw-load.h -- DVM uCode load -> ALIVE -> calibration -> runtime ALIVE.
 * ========================================================================
 * iwl_load_ucode() drives the full DVM firmware bring-up:
 *   1. load the INIT ucode (DMA INST/DATA into instruction/data SRAM via the FH
 *      service channel), release the ARC (CSR_RESET=0), wait bounded for the
 *      INIT ALIVE notification on the RX ring.
 *   2. start calibrations (CALIBRATION_CFG_CMD), wait bounded for
 *      CALIBRATION_COMPLETE_NOTIFICATION, capture the CALIB_RES payloads.
 *   3. load the RUNTIME ucode (INST/DATA), release the ARC, wait for the
 *      RUNTIME ALIVE.
 * Mirrors Linux dvm/ucode.c iwl_run_init_ucode + iwl_load_ucode_wait_alive and
 * pcie/trans.c iwl_pcie_load_given_ucode / iwl_pcie_load_section.
 *
 *   ===================  HELD FOR HARDWARE  ===================
 *   Never runs at boot; correct-by-review only. Bounded, markers, abort-clean.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-fw-load.h
 */
#ifndef IWL_FW_LOAD_H
#define IWL_FW_LOAD_H

#include "types.h"
#include "iwl-trans.h"

/* One firmware sub-image: a pointer into the (trusted, initrd-resident) .ucode
 * blob plus its byte length and the destination SRAM address the FH DMA writes
 * it to. dest is the device-internal SRAM offset (instruction vs data SRAM). */
typedef struct iwl_fw_section {
    const uint8_t* data;     /* payload bytes in the parsed .ucode blob */
    uint32_t       len;      /* byte length */
    uint32_t       dest;     /* device SRAM destination address */
} iwl_fw_section_t;

/* The DVM firmware images the loader needs: INIT (run once for calibration) and
 * RUNTIME (the operational ucode). Each has an INST (instruction SRAM) and DATA
 * (data SRAM) section. Captured by iwl_fw_capture_sections() from the TLV blob. */
typedef struct iwl_fw_images {
    iwl_fw_section_t init_inst;
    iwl_fw_section_t init_data;
    iwl_fw_section_t inst;        /* runtime instruction */
    iwl_fw_section_t data;        /* runtime data */
    int valid;                    /* 1 once at least runtime INST/DATA captured */
} iwl_fw_images_t;

/*
 * iwl_fw_capture_sections -- re-walk the TLV .ucode blob (same bounds discipline
 * as iwl-fw.c) capturing PAYLOAD POINTERS (not just sizes) for INST/DATA/INIT/
 * INIT_DATA, and assign each the correct device SRAM destination. Returns 0 on
 * success, -1 on malformed input. blob/len are the bytes from
 * iwl_fw_load_from_initrd's underlying file (or the synthetic selftest blob).
 */
int iwl_fw_capture_sections(const uint8_t* blob, uint32_t len,
                            iwl_fw_images_t* out);

/*
 * iwl_load_ucode -- the held DVM firmware bring-up. trans must already be
 * brought up (rings allocated, APM powered) by iwl_trans_bringup(). Returns 0 if
 * the RUNTIME ucode reached ALIVE, -1 on any bounded failure (abort-clean).
 */
int iwl_load_ucode(struct iwl_trans* trans, const iwl_fw_images_t* fw);

/* DVM device SRAM destinations (dvm uses fixed instruction/data SRAM windows).
 * HARDWARE-VALIDATION: the exact RTC instruction/data SRAM base differs slightly
 * per family; these are the common DVM (5000/6000) values and are flagged. */
#define IWL_RTC_INST_LOWER_BOUND   0x000000   /* instruction SRAM base -- HW-VALIDATE */
#define IWL_RTC_DATA_LOWER_BOUND   0x800000   /* data SRAM base        -- HW-VALIDATE */

#endif /* IWL_FW_LOAD_H */
