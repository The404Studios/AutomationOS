/*
 * iwl-rxon.h -- DVM RXON radio-configuration command (REPLY_RXON 0x10).
 * ====================================================================
 * RXON is the command that turns the radio's MAC receiver ON: it sets the
 * channel, band, RX filters and antenna chain. The DVM firmware receives NO
 * 802.11 frame -- not even broadcast beacons during a scan -- until a valid
 * RXON has been committed. This brick builds + sends a BASELINE (un-associated)
 * RXON so a passive scan can actually hear beacons. The per-BSS association
 * RXON (+ REPLY_ADD_STA, key install) is Phase 2.
 *
 *   ===================  HELD FOR HARDWARE  ===================
 *   Reached only via iwl_wifi_bringup()/iwl_ops_scan_start() on the physical
 *   T410. Correct-by-review vs Linux dvm/rxon.c iwl_commit_rxon +
 *   dvm/main.c iwl_connection_init_rx_config. Bounded, marker-printing.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-rxon.h
 */
#ifndef IWL_RXON_H
#define IWL_RXON_H

#include "types.h"
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"

/*
 * iwl_build_rxon_baseline -- fill `out` with a baseline (un-associated) station
 * RXON for `channel` (2.4GHz): node_addr = our MAC, bssid = broadcast, dev_type
 * = ESS, band = 2.4GHz, filters accept group/bcast frames, ASSOC NOT set.
 * Pure logic (no MMIO) so the self-test harness can exercise it. Returns the
 * byte length written (== sizeof(struct iwl_rxon_cmd)).
 */
uint16_t iwl_build_rxon_baseline(struct iwl_rxon_cmd* out,
                                 const uint8_t* mac, uint8_t channel);

/*
 * iwl_rxon_baseline -- build + send the baseline RXON (REPLY_RXON 0x10) then
 * REPLY_RXON_TIMING (0x14) so the receiver is live before a scan. MUST run after
 * ALIVE + SCD bring-up. Returns 0 on a clean send, -1 on failure (abort-clean).
 */
int iwl_rxon_baseline(struct iwl_trans* trans, uint8_t channel);

/* Pure-logic KAT for the RXON builder (runs in QEMU, no radio). 0=PASS. */
int iwl_rxon_selftest(void);

#endif /* IWL_RXON_H */
