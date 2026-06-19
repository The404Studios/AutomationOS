/*
 * iwl-scan.h -- DVM REPLY_SCAN_CMD + beacon harvest into wlan_bss_t[].
 * ===================================================================
 * iwl_scan() builds a REPLY_SCAN_CMD (0x80) over the NVM channel list, sends it,
 * then drains the RX ring: beacons / probe-responses arrive as REPLY_RX_MPDU_CMD
 * frames which we parse (SSID + BSSID + channel + signal + RSN security), and
 * SCAN_COMPLETE_NOTIFICATION (0x84) ends the harvest. Mirrors Linux dvm/scan.c
 * (iwlagn_request_scan) + dvm/rx.c (iwlagn_rx_reply_rx -> beacon to mac80211).
 *
 *   ===================  HELD FOR HARDWARE  ===================
 *   Never runs at boot; correct-by-review only. Bounded, markers, abort-clean.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-scan.h
 */
#ifndef IWL_SCAN_H
#define IWL_SCAN_H

#include "types.h"
#include "iwl-trans.h"
#include "iwl-nvm.h"
#include "wifi.h"   /* wlan_bss_t */

/*
 * iwl_scan -- run one scan and fill up to `max` results into `out`. `chans` is
 * the NVM channel list (from iwl_read_nvm). Returns the number of BSSes found
 * (>= 0), or -1 on a send/timeout failure (abort-clean).
 */
int iwl_scan(struct iwl_trans* trans, const iwl_nvm_data_t* chans,
             wlan_bss_t* out, int max);

#endif /* IWL_SCAN_H */
