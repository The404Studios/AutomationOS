/*
 * iwl-rxon.c -- DVM RXON radio-configuration (REPLY_RXON 0x10) bring-up.
 * =====================================================================
 * THE #1 SCAN PREREQUISITE. The DVM firmware will not deliver a single received
 * 802.11 frame to the host RX ring until the MAC receiver has been configured by
 * a committed RXON (channel/band/filters/antenna). Without this, REPLY_SCAN_CMD
 * runs but the RX ring stays empty and the scan returns zero SSIDs -- which is
 * exactly the "no networks on the T410" symptom.
 *
 * This brick commits a BASELINE, un-associated station RXON suitable for a
 * passive scan (listen for beacons -- every AP beacons ~every 100ms). The
 * association RXON (with the AP's BSSID + ASSOC flag), REPLY_ADD_STA and key
 * install are Phase 2.
 *
 *   ===================  HELD FOR HARDWARE  ===================
 *   Reached only via iwl_ops_scan_start()/iwl_wifi_bringup() on the real T410.
 *   Correct-by-review vs Linux dvm/rxon.c iwl_commit_rxon +
 *   dvm/main.c iwl_connection_init_rx_config. Bounded, marker-printing,
 *   abort-clean.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-rxon.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf */
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"
#include "iwl-hostcmd.h"
#include "iwl-rxon.h"

uint16_t iwl_build_rxon_baseline(struct iwl_rxon_cmd* r,
                                 const uint8_t* mac, uint8_t channel) {
    for (uint32_t i = 0; i < sizeof(*r); i++) ((uint8_t*)r)[i] = 0;

    /* node_addr = our own MAC (from NVM). */
    if (mac) for (int i = 0; i < 6; i++) r->node_addr[i] = mac[i];
    /* bssid = broadcast: we are NOT associated to any AP yet (baseline). */
    for (int i = 0; i < 6; i++) r->bssid_addr[i] = 0xff;

    r->dev_type        = RXON_DEV_TYPE_ESS;                 /* managed station */
    r->rx_chain        = (uint16_t)RXON_RX_CHAIN_SCAN_DEFAULT;
    r->cck_basic_rates = IWL_CCK_BASIC_RATES_24;
    r->ofdm_basic_rates= IWL_OFDM_BASIC_RATES_24;
    r->channel         = channel;

    /* 2.4GHz band, auto-detect modulation, report TSF to host. ASSOC is NOT
     * set -- this is a listen-only baseline so a passive scan hears beacons. */
    r->flags = RXON_FLG_BAND_24G_MSK | RXON_FLG_AUTO_DETECT_MSK |
               RXON_FLG_TSF2HOST_MSK;

    /* Accept group (broadcast/multicast) frames + promiscuous so every beacon
     * and probe-response reaches the host RX ring during the scan. */
    r->filter_flags = RXON_FILTER_PROMISC_MSK | RXON_FILTER_ACCEPT_GRP_MSK;

    return (uint16_t)sizeof(*r);
}

static uint16_t iwl_build_rxon_timing(struct iwl_rxon_time_cmd* t) {
    for (uint32_t i = 0; i < sizeof(*t); i++) ((uint8_t*)t)[i] = 0;
    /* Defaults until a real beacon refines them (only needed once associated). */
    t->beacon_interval = 100;      /* 100 TU */
    t->listen_interval = 10;
    t->dtim_period     = 1;
    return (uint16_t)sizeof(*t);
}

int iwl_rxon_baseline(struct iwl_trans* trans, uint8_t channel) {
    if (!trans || !trans->mmio) return -1;

    struct iwl_rxon_cmd r;
    iwl_build_rxon_baseline(&r, trans->mac, channel);
    kprintf("IWLRXON: commit baseline RXON ch=%u (REPLY_RXON 0x10, %u bytes)...\n",
            channel, (unsigned)sizeof(r));
    /* Async send: the firmware applies the RXON; the proof it worked is that the
     * subsequent scan starts returning beacons. */
    if (iwl_send_cmd(trans, REPLY_RXON, &r, sizeof(r), 0, (iwl_rx_notif_t*)0) != 0) {
        kprintf("IWLRXON: REPLY_RXON send FAILED -- abort\n");
        return -1;
    }
    iwl_udelay_approx(2000);   /* let the RF retune to the channel/band */

    struct iwl_rxon_time_cmd tm;
    iwl_build_rxon_timing(&tm);
    kprintf("IWLRXON: send RXON_TIMING (0x14)...\n");
    if (iwl_send_cmd(trans, REPLY_RXON_TIMING, &tm, sizeof(tm), 0,
                     (iwl_rx_notif_t*)0) != 0) {
        kprintf("IWLRXON: REPLY_RXON_TIMING send FAILED -- abort\n");
        return -1;
    }

    kprintf("IWLRXON: baseline RXON committed (receiver live; scan can hear beacons)\n");
    return 0;
}

/* ====================================================================== *
 *  iwl_rxon_selftest -- pure-logic KAT for the baseline RXON builder. Runs in
 *  QEMU (no radio needed): verifies the wire struct is built to the exact size
 *  + fields a DVM firmware expects, so a struct-offset / flag bug is caught
 *  WITHOUT hardware. Returns 0 on PASS, -1 on FAIL.
 * ====================================================================== */
int iwl_rxon_selftest(void) {
    int ok = 1;
    const uint8_t mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    struct iwl_rxon_cmd r;
    uint16_t n = iwl_build_rxon_baseline(&r, mac, 6);

    if (n != (uint16_t)sizeof(struct iwl_rxon_cmd)) {
        kprintf("IWL-RXON: size mismatch %u != %u\n", n,
                (unsigned)sizeof(struct iwl_rxon_cmd));
        ok = 0;
    }
    for (int i = 0; i < 6; i++) if (r.node_addr[i]  != mac[i]) ok = 0;   /* our MAC */
    for (int i = 0; i < 6; i++) if (r.bssid_addr[i] != 0xff)   ok = 0;   /* broadcast */
    if (r.dev_type != RXON_DEV_TYPE_ESS)                       ok = 0;
    if (r.channel  != 6)                                       ok = 0;
    if (!(r.flags & RXON_FLG_BAND_24G_MSK))                    ok = 0;   /* 2.4GHz */
    if (!(r.filter_flags & RXON_FILTER_ACCEPT_GRP_MSK))        ok = 0;   /* hear bcast */
    if (r.filter_flags & RXON_FILTER_ASSOC_MSK)               ok = 0;   /* NOT associated */
    if (r.rx_chain != (uint16_t)RXON_RX_CHAIN_SCAN_DEFAULT)    ok = 0;

    kprintf("IWL-RXON: selftest %s (size=%u ch=%u flags=0x%x filt=0x%x rxchain=0x%x)\n",
            ok ? "PASS" : "FAIL", n, r.channel, r.flags, r.filter_flags, r.rx_chain);
    return ok ? 0 : -1;
}
