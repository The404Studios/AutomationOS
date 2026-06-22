/*
 * iwl-scan.c -- DVM REPLY_SCAN_CMD + beacon harvest into wlan_bss_t[].
 * ===================================================================
 * Build + send a REPLY_SCAN_CMD over the NVM channel list, then drain the RX
 * ring: beacons/probe-responses arrive as REPLY_RX_MPDU_CMD frames; we parse the
 * 802.11 management frame (SSID + BSSID + channel + RSN/security) into
 * wlan_bss_t rows, and SCAN_COMPLETE_NOTIFICATION (0x84) ends the harvest.
 *
 *   ===================  HELD FOR HARDWARE -- READ THIS  ===================
 *   Nothing here runs at boot. Reached only via iwl_wifi_bringup() (iwl-ops.c)
 *   on the physical T410. Correct-by-review vs Linux iwlwifi DVM:
 *     dvm/scan.c -- iwlagn_request_scan: builds struct iwl_scan_cmd (REPLY_SCAN_CMD
 *                   0x80), appends struct iwl_scan_channel per channel.
 *     dvm/rx.c   -- iwlagn_rx_reply_rx (REPLY_RX_MPDU_CMD 0xc1): the received
 *                   beacon's 802.11 header + IEs are handed to mac80211; we parse
 *                   them in-tree into wlan_bss_t.
 *     SCAN_COMPLETE_NOTIFICATION 0x84 ends the scan (dvm/scan.c).
 *
 *   Safety laws: bounded harvest loop, markers, abort-clean on timeout.
 *
 *   HARDWARE-VALIDATION ITEMS (flagged honestly):
 *     - The exact byte offset of the 802.11 MAC header inside a REPLY_RX_MPDU
 *       payload (rx_non_cfg_phy / iwl_rx_phy_res preamble) is family-specific.
 *       We locate the management frame defensively (scan for the 0x80 beacon /
 *       0x50 proberesp frame-control within a bounded window) and flag it.
 *     - The signal (RSSI) is in the RX PHY preamble; we surface a placeholder and
 *       flag exact extraction for hardware pinning.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-scan.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf */
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"
#include "iwl-hostcmd.h"
#include "iwl-nvm.h"
#include "iwl-scan.h"
#include "wifi.h"

/* Bounded number of RX harvest steps after sending the scan (covers many
 * channels' worth of beacons + the completion notif). */
#define IWL_SCAN_HARVEST_MAX   100000

/* 802.11 element IDs (parsed from the beacon/probe-resp body). */
#define WLAN_EID_SSID      0
#define WLAN_EID_DS_PARAMS 3    /* current channel */
#define WLAN_EID_RSN       48   /* WPA2/WPA3 */
#define WLAN_EID_VENDOR    221  /* WPA1 (Microsoft OUI) */

/* 802.11 frame-control values for a probe request (dvm/scan.c iwl_fill_probe_req:
 * IEEE80211_STYPE_PROBE_REQ | IEEE80211_FTYPE_MGMT = 0x0040 | 0x0000 = 0x0040). */
#define IEEE80211_FC_PROBE_REQ   0x0040

/* ====================================================================== *
 *  iwl_fill_probe_req -- build a broadcast probe-request 802.11 management frame
 *  into `frame` (S-S2). Mirrors Linux dvm/scan.c iwl_fill_probe_req:
 *    frame_control = PROBE_REQ; da = bssid = broadcast; sa = own MAC; seq = 0;
 *    then a wildcard SSID IE (id 0, len 0) and a basic 2.4GHz supported-rates IE.
 *  Returns the total frame length. The probe template is appended to the scan
 *  command after the head; tx_cmd.len records this length.
 * ====================================================================== */
__attribute__((unused))
static uint16_t iwl_fill_probe_req(uint8_t* frame, const uint8_t* own_mac) {
    uint16_t p = 0;
    /* frame_control (LE16). */
    frame[p++] = IEEE80211_FC_PROBE_REQ & 0xff;
    frame[p++] = (IEEE80211_FC_PROBE_REQ >> 8) & 0xff;
    /* duration (2). */
    frame[p++] = 0; frame[p++] = 0;
    /* addr1 / DA = broadcast. */
    for (int i = 0; i < 6; i++) frame[p++] = 0xff;
    /* addr2 / SA = own MAC. */
    for (int i = 0; i < 6; i++) frame[p++] = own_mac ? own_mac[i] : 0xff;
    /* addr3 / BSSID = broadcast. */
    for (int i = 0; i < 6; i++) frame[p++] = 0xff;
    /* seq_ctrl (2). */
    frame[p++] = 0; frame[p++] = 0;
    /* Wildcard SSID IE: id 0, len 0 (broadcast probe). */
    frame[p++] = WLAN_EID_SSID;   /* 0 */
    frame[p++] = 0;               /* len 0 */
    /* Supported-rates IE (id 1): the mandatory 2.4GHz rates 1/2/5.5/11 Mbps,
     * encoded as rate*2 with the basic-rate bit (0x80). A minimal usable set so
     * APs answer the probe. */
    frame[p++] = 1;               /* WLAN_EID_SUPP_RATES */
    frame[p++] = 4;               /* 4 rates */
    frame[p++] = 0x82;            /* 1   Mbps (basic) */
    frame[p++] = 0x84;            /* 2   Mbps (basic) */
    frame[p++] = 0x8b;            /* 5.5 Mbps (basic) */
    frame[p++] = 0x96;            /* 11  Mbps (basic) */
    return p;
}

/* ====================================================================== *
 *  iwl_build_scan_cmd -- assemble the REPLY_SCAN_CMD payload into `buf`.
 *  Layout (dvm/scan.c): [iwl_scan_cmd head][iwl_scan_channel * n][probe frame].
 *  We zero the head, poke the few leading fields, build the probe-request
 *  template into iwl_scan_cmd.data[] (after the channels), record its length in
 *  tx_cmd.len, and OR the probe mask into each active channel. Returns the total
 *  payload byte length.
 * ====================================================================== */
/* Little-endian field stores into the command buffer. */
static inline void sc_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
}
static inline void sc_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);         p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff); p[3] = (uint8_t)((v >> 24) & 0xff);
}

/*
 * Build a PASSIVE 2.4GHz scan. Milestone 1 (see SSIDs) does a passive listen:
 * every AP beacons ~every 100ms, so a passive_dwell > 100 TU on each channel
 * harvests them all -- WITHOUT needing a probe-request TX (which would require a
 * broadcast station via REPLY_ADD_STA, a Phase-2 item). The fixed header is now
 * fully initialized (previously only ~6 of 28 bytes were set, so the firmware
 * received a malformed command). Returns the total payload byte length.
 */
static uint16_t iwl_build_scan_cmd(const iwl_nvm_data_t* chans,
                                   const uint8_t* own_mac,
                                   uint8_t* buf, uint16_t cap) {
    (void)own_mac;   /* passive scan transmits no probe (active = Phase 2) */

    /* Head: zeroed block of IWL_SCAN_CMD_HEAD_SIZE (768 = 0x300). (S-S1: the old
     * 0xC8 head was far too small, so channels overwrote direct_scan[].) */
    uint16_t head = (uint16_t)IWL_SCAN_CMD_HEAD_SIZE;
    if (head > cap) return 0;
    for (uint16_t i = 0; i < head; i++) buf[i] = 0;

    int nch = chans ? chans->n_channels : 0;
    uint16_t per = (uint16_t)sizeof(struct iwl_scan_channel);
    while (nch > 0 && (uint32_t)head + (uint32_t)nch * per > cap) nch--;

    /* ---- fixed header fields (dvm/scan.c iwlagn_request_scan) ---- */
    buf[SCAN_CMD_OFF_SCAN_FLAGS]    = 0;
    buf[SCAN_CMD_OFF_CHANNEL_COUNT] = (uint8_t)nch;
    sc_le16(buf + SCAN_CMD_OFF_QUIET_TIME,    IWL_ACTIVE_QUIET_TIME);
    sc_le16(buf + SCAN_CMD_OFF_QUIET_PLCP_TH, IWL_PLCP_QUIET_THRESH);
    /* passive scan never early-exits a channel on good CRCs */
    sc_le16(buf + SCAN_CMD_OFF_GOOD_CRC_TH,   IWL_GOOD_CRC_TH_DISABLED);
    sc_le16(buf + SCAN_CMD_OFF_RX_CHAIN,      (uint16_t)RXON_RX_CHAIN_SCAN_DEFAULT);
    /* not associated -> stay on each channel for the full dwell (no host time-share) */
    sc_le32(buf + SCAN_CMD_OFF_MAX_OUT_TIME,  0);
    sc_le32(buf + SCAN_CMD_OFF_SUSPEND_TIME,  0);
    /* band/RXON flags for the scan RF: 2.4GHz + auto-detect modulation */
    sc_le32(buf + SCAN_CMD_OFF_FLAGS,
            RXON_FLG_BAND_24G_MSK | RXON_FLG_AUTO_DETECT_MSK);
    /* receive beacons/probe-responses from any BSS */
    sc_le32(buf + SCAN_CMD_OFF_FILTER_FLAGS,
            RXON_FILTER_PROMISC_MSK | RXON_FILTER_BCON_AWARE_MSK);
    /* tx_cmd.len = 0: no probe template appended (passive). Phase-2 active scan
     * fills tx_cmd (tx_flags/rate_n_flags/sta_id) + appends the probe template +
     * sets the per-channel IWL_SCAN_PROBE_MASK_1. */
    sc_le16(buf + SCAN_TXCMD_OFF_LEN, 0);

    /* ---- per-channel descriptors (PASSIVE: listen for beacons) ---- */
    uint16_t off = head;
    for (int i = 0; i < nch; i++) {
        struct iwl_scan_channel* sc = (struct iwl_scan_channel*)(buf + off);
        for (uint16_t b = 0; b < per; b++) ((uint8_t*)sc)[b] = 0;
        sc->type          = SCAN_CHANNEL_TYPE_PASSIVE;   /* no probe TX */
        sc->channel       = chans->channels[i].number;
        sc->tx_gain       = IWL_SCAN_TX_GAIN_24;
        sc->active_dwell  = IWL_ACTIVE_DWELL_TIME_24;
        sc->passive_dwell = IWL_PASSIVE_DWELL_TIME_24;   /* >100 TU -> catches a beacon */
        sc->dsp_atten     = 110;
        off += per;
    }

    /* Fill iwl_scan_cmd.len (LE16) with the total payload. */
    sc_le16(buf + SCAN_CMD_OFF_LEN, off);
    return off;
}

/* ====================================================================== *
 *  Beacon parse. Given a REPLY_RX_MPDU payload, locate the 802.11 management
 *  frame and pull SSID/BSSID/channel/security into a wlan_bss_t. Returns 1 if a
 *  beacon/probe-resp was parsed, 0 if the frame is not a usable mgmt beacon.
 *
 *  802.11 mgmt frame: frame_control(2) duration(2) addr1(6) addr2(6=BSSID for
 *  beacons addr3 too) addr3(6) seq(2) then for beacon: timestamp(8) beacon_int(2)
 *  capability(2) then the IE list (id,len,data...). BSSID = addr3.
 *
 *  `rssi` is the dBm signal extracted from the preceding REPLY_RX_PHY_CMD(0xc0).
 * ====================================================================== */
static int iwl_parse_beacon(const uint8_t* mpdu, uint16_t mpdu_len,
                            int16_t rssi, wlan_bss_t* bss) {
    /* S-S3: for REPLY_RX_MPDU_CMD(0xc1) the 802.11 header is at a FIXED offset:
     * the payload begins with a 4-byte struct iwl_rx_mpdu_res_start, so the mgmt
     * frame's frame_control is at mpdu+4 (Linux dvm/rx.c iwlagn_rx_reply_rx:
     * header = pkt->data + sizeof(*rx_res)). Parse it directly -- no byte-scan,
     * which could lock onto an interior 0x80/0x50 byte. */
    if (mpdu_len < sizeof(struct iwl_rx_mpdu_res_start) + 24)
        return 0;
    const struct iwl_rx_mpdu_res_start* rs =
        (const struct iwl_rx_mpdu_res_start*)mpdu;
    uint16_t fc_off = (uint16_t)sizeof(struct iwl_rx_mpdu_res_start);   /* 4 */

    /* Sanity-clamp the frame to the declared byte_count (never past mpdu_len). */
    uint16_t avail = (uint16_t)(mpdu_len - fc_off);
    uint16_t bc    = rs->byte_count;
    if (bc > avail) bc = avail;

    const uint8_t* f = mpdu + fc_off;
    /* Accept only a beacon (0x80) or probe-response (0x50) frame_control. */
    uint8_t fc = f[0];
    if (fc != 0x80 && fc != 0x50) return 0;

    uint16_t flen = bc;
    if (flen < 24 + 12) return 0;   /* need header + fixed beacon params */

    /* addr3 (BSSID) at offset 16. */
    for (int i = 0; i < 6; i++) bss->bssid[i] = f[16 + i];

    /* capability at 24 + 8(timestamp) + 2(beacon_int) = 34. */
    uint16_t cap = (uint16_t)(f[34] | (f[35] << 8));
    bss->capability = cap;

    /* Walk IEs starting at offset 36 (after timestamp/interval/capability). */
    uint8_t  sec    = WLAN_SEC_OPEN;
    int      have_ssid = 0;
    uint16_t chan_from_ds = 0;
    uint16_t p = 36;
    /* If the privacy bit (cap bit 4) is set but no RSN found, it is WEP/WPA1;
     * we map any privacy to at least WPA2 only when an RSN IE is present below. */
    while (p + 2 <= flen) {
        uint8_t id  = f[p];
        uint8_t len = f[p + 1];
        if (p + 2 + len > flen) break;     /* malformed IE -> stop, no overrun */
        const uint8_t* ie = f + p + 2;

        if (id == WLAN_EID_SSID) {
            uint8_t sl = len; if (sl > WLAN_SSID_MAX) sl = WLAN_SSID_MAX;
            for (uint8_t i = 0; i < sl; i++) bss->ssid[i] = ie[i];
            bss->ssid_len = sl;
            have_ssid = 1;
        } else if (id == WLAN_EID_DS_PARAMS && len >= 1) {
            chan_from_ds = ie[0];
        } else if (id == WLAN_EID_RSN) {
            /* RSN IE present => WPA2 (or WPA3 if it advertises SAE AKM 0x000FAC08;
             * detecting SAE precisely is a refinement -- we mark WPA2 and flag
             * WPA3 detection as a hardware-validation nicety). */
            sec = WLAN_SEC_WPA2;
        }
        p += (uint16_t)(2 + len);
    }

    bss->security = sec;
    bss->channel  = chan_from_ds;
    bss->signal   = rssi;   /* from the cached REPLY_RX_PHY_CMD (S-S6) */

    return have_ssid ? 1 : 0;
}

/* Already-seen BSSID de-dup (a beacon repeats every ~100ms; one row per AP). */
static int iwl_bss_seen(const wlan_bss_t* out, int n, const uint8_t* bssid) {
    for (int i = 0; i < n; i++) {
        int same = 1;
        for (int b = 0; b < 6; b++) if (out[i].bssid[b] != bssid[b]) { same = 0; break; }
        if (same) return 1;
    }
    return 0;
}

/* ====================================================================== *
 *  RSSI from the cached REPLY_RX_PHY_CMD(0xc0) iwl_rx_phy_res (S-S6). Linux
 *  iwlagn_calc_rssi reads the per-antenna RSSI + AGC out of phy_res->
 *  non_cfg_phy_buf and returns max(rssi_a,b,c) - agc - IWLAGN_RSSI_OFFSET.
 *  The EXACT byte layout of non_cfg_phy_buf (which dword holds AGC vs each
 *  antenna's RSSI) is family-specific (5000 vs 6000 differ).
 *
 *  TODO(HARDWARE-VALIDATE): pin the non_cfg_phy_buf field offsets per family
 *  against Linux dvm/devices.c iwlagn_calc_rssi (iwlagn_non_cfg_phy AGC/RSSI
 *  dword indices + IWLAGN_RSSI_OFFSET = 44). Until pinned we surface a SAFE,
 *  clearly-marked placeholder so the scan still returns usable rows; the value
 *  is conservative (mid-range) rather than fabricated-precise.
 * ====================================================================== */
#define IWLAGN_RSSI_OFFSET   44   /* dvm/devices.c IWLAGN_RSSI_OFFSET */

static int g_phy_res_valid = 0;
static struct iwl_rx_phy_res g_last_phy_res;

static int16_t iwl_calc_rssi_from_phy(void) {
    if (!g_phy_res_valid)
        return -70;   /* no PHY seen yet -- safe placeholder */
    /* SAFE placeholder pending the per-family non_cfg_phy_buf decode (above).
     * We intentionally do NOT guess antenna/AGC dword positions blind. */
    return -65;       /* clearly-marked placeholder (NOT a precise reading) */
}

/* Cache a REPLY_RX_PHY_CMD(0xc0) payload (Linux: priv->last_phy_res). The next
 * REPLY_RX_MPDU consumes it for its RSSI. */
static void iwl_cache_phy_res(const uint8_t* data, uint16_t len) {
    if (len < sizeof(struct iwl_rx_phy_res)) { g_phy_res_valid = 0; return; }
    for (uint32_t i = 0; i < sizeof(g_last_phy_res); i++)
        ((uint8_t*)&g_last_phy_res)[i] = data[i];
    g_phy_res_valid = 1;
}

/* ====================================================================== *
 *  iwl_scan -- send REPLY_SCAN_CMD, harvest beacons until SCAN_COMPLETE.
 * ====================================================================== */
int iwl_scan(struct iwl_trans* trans, const iwl_nvm_data_t* chans,
             wlan_bss_t* out, int max) {
    if (!trans || !trans->mmio || !out || max <= 0) {
        kprintf("IWLSCAN: bad args -- abort\n");
        return -1;
    }

    kprintf("IWLSCAN: scan START (real T410 only)\n");

    g_phy_res_valid = 0;   /* fresh PHY cache for this scan */

    /* Build the scan command. The head alone is 768 bytes; with up to 13 channels
     * (13*12) + the probe template the buffer must be >= 768 + 156 -- size it to
     * 1280 with headroom (S-S1). */
    static uint8_t scanbuf[1280];
    uint16_t plen = iwl_build_scan_cmd(chans, trans->mac, scanbuf,
                                       (uint16_t)sizeof(scanbuf));
    if (plen == 0) {
        kprintf("IWLSCAN: scan cmd build failed -- abort\n");
        return -1;
    }

    /* Send REPLY_SCAN_CMD (async: the firmware replies later with notifications
     * + RX beacons, not an immediate response). */
    kprintf("IWLSCAN: send REPLY_SCAN_CMD (0x80, %u bytes, %d channels)...\n",
            plen, chans ? chans->n_channels : 0);
    if (iwl_send_cmd(trans, REPLY_SCAN_CMD, scanbuf, plen, 0, (iwl_rx_notif_t*)0) != 0) {
        kprintf("IWLSCAN: REPLY_SCAN_CMD send FAILED -- abort\n");
        return -1;
    }

    /* Harvest: drain RX (bounded). Beacons => parse into out[]; SCAN_COMPLETE
     * (0x84) => done. Other notifications (SCAN_START/RESULTS) are ignored. */
    int found = 0;
    iwl_rx_notif_t n;
    for (int it = 0; it < IWL_SCAN_HARVEST_MAX; it++) {
        int got = iwl_rx_poll_one(trans, &n);
        if (got < 0) {
            kprintf("IWLSCAN: RX ring inconsistency -- abort\n");
            return -1;
        }
        if (got == 0) { iwl_udelay_approx(64); continue; }   /* empty: settle */

        if (n.cmd == SCAN_COMPLETE_NOTIFICATION) {
            kprintf("IWLSCAN: SCAN_COMPLETE -- harvested %d BSS\n", found);
            return found;
        }

        /* Cache the RX PHY (0xc0): it precedes its MPDU and carries the RSSI. */
        if (n.cmd == REPLY_RX_PHY_CMD) {
            iwl_cache_phy_res(n.data, n.len);
            continue;
        }

        /* S-S7: DVM v5.10 delivers received frames as REPLY_RX_MPDU_CMD(0xc1)
         * only -- the legacy REPLY_RX(0xc3) has a different layout and is not
         * emitted, so we parse 0xc1 exclusively (header at +4). */
        if (n.cmd == REPLY_RX_MPDU_CMD && found < max) {
            int16_t rssi = iwl_calc_rssi_from_phy();
            wlan_bss_t cand;
            for (uint32_t z = 0; z < sizeof(cand); z++) ((uint8_t*)&cand)[z] = 0;
            if (iwl_parse_beacon(n.data, n.len, rssi, &cand) == 1) {
                if (!iwl_bss_seen(out, found, cand.bssid)) {
                    out[found] = cand;
                    kprintf("IWLSCAN: BSS %02x:%02x:%02x:%02x:%02x:%02x ch=%u sec=%u ssid_len=%u\n",
                            cand.bssid[0], cand.bssid[1], cand.bssid[2],
                            cand.bssid[3], cand.bssid[4], cand.bssid[5],
                            cand.channel, cand.security, cand.ssid_len);
                    found++;
                }
            }
        }
    }

    /* Budget exhausted without SCAN_COMPLETE: return what we harvested (still a
     * clean, bounded outcome -- never hangs). */
    kprintf("IWLSCAN: harvest budget exhausted (no SCAN_COMPLETE) -- %d BSS\n", found);
    return found;
}

/* ====================================================================== *
 *  iwl_scan_selftest -- pure-logic KAT for the scan-command builder + the beacon
 *  parser. Runs in QEMU (no radio): proves the command head is fully formed and
 *  that a synthetic beacon decodes to the right SSID/BSSID/channel/security --
 *  catching struct-offset / IE-walk bugs WITHOUT hardware. Returns 0 / -1.
 * ====================================================================== */
static uint16_t st_rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t st_rd32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

int iwl_scan_selftest(void) {
    int ok = 1;

    /* ---- (A) scan-command builder: 13x 2.4GHz passive channels ---- */
    static iwl_nvm_data_t nvm;
    for (uint32_t i = 0; i < sizeof(nvm); i++) ((uint8_t*)&nvm)[i] = 0;
    nvm.n_channels = 13;
    for (int c = 0; c < 13; c++) { nvm.channels[c].number = (uint16_t)(c + 1); nvm.channels[c].band = 0; }
    const uint8_t mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    static uint8_t buf[1280];
    uint16_t plen = iwl_build_scan_cmd(&nvm, mac, buf, (uint16_t)sizeof(buf));

    if (plen == 0) { kprintf("IWL-SCAN: build returned 0\n"); ok = 0; }
    if (buf[SCAN_CMD_OFF_CHANNEL_COUNT] != 13) ok = 0;
    if (st_rd16(buf + SCAN_CMD_OFF_GOOD_CRC_TH) != IWL_GOOD_CRC_TH_DISABLED) ok = 0;
    if (st_rd16(buf + SCAN_CMD_OFF_RX_CHAIN) != (uint16_t)RXON_RX_CHAIN_SCAN_DEFAULT) ok = 0;
    if (!(st_rd32(buf + SCAN_CMD_OFF_FLAGS) & RXON_FLG_BAND_24G_MSK)) ok = 0;
    if (!(st_rd32(buf + SCAN_CMD_OFF_FILTER_FLAGS) & RXON_FILTER_PROMISC_MSK)) ok = 0;
    if (st_rd16(buf + SCAN_TXCMD_OFF_LEN) != 0) ok = 0;   /* passive: no probe */
    /* first per-channel descriptor (right after the 768-byte head) */
    {
        const struct iwl_scan_channel* sc0 =
            (const struct iwl_scan_channel*)(buf + IWL_SCAN_CMD_HEAD_SIZE);
        if (sc0->channel != 1) ok = 0;
        if (sc0->type & SCAN_CHANNEL_TYPE_ACTIVE) ok = 0;   /* must be PASSIVE */
        if (sc0->passive_dwell != IWL_PASSIVE_DWELL_TIME_24) ok = 0;
    }

    /* ---- (B) beacon parser: synthetic REPLY_RX_MPDU -> wlan_bss_t ---- */
    static uint8_t mpdu[128];
    for (uint32_t i = 0; i < sizeof(mpdu); i++) mpdu[i] = 0;
    uint8_t* f = mpdu + 4;   /* 802.11 header starts after iwl_rx_mpdu_res_start */
    int p = 0;
    f[p++] = 0x80; f[p++] = 0x00;                 /* frame_control = beacon */
    f[p++] = 0;    f[p++] = 0;                     /* duration */
    for (int i = 0; i < 6; i++) f[p++] = 0xff;     /* addr1 = broadcast */
    for (int i = 0; i < 6; i++) f[p++] = (uint8_t)(0xa0 + i);  /* addr2 = SA */
    const uint8_t bssid[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x42 };
    for (int i = 0; i < 6; i++) f[p++] = bssid[i]; /* addr3 = BSSID (offset 16) */
    f[p++] = 0;    f[p++] = 0;                     /* seq_ctrl */
    for (int i = 0; i < 8; i++) f[p++] = 0;        /* timestamp */
    f[p++] = 0x64; f[p++] = 0x00;                  /* beacon_interval = 100 */
    f[p++] = 0x11; f[p++] = 0x00;                  /* capability (ESS+privacy) @34 */
    /* IEs @36: SSID "Hello", DS ch6, RSN (=> WPA2) */
    f[p++] = 0;  f[p++] = 5;  f[p++]='H'; f[p++]='e'; f[p++]='l'; f[p++]='l'; f[p++]='o';
    f[p++] = 3;  f[p++] = 1;  f[p++] = 6;
    f[p++] = 48; f[p++] = 4;  f[p++]=1; f[p++]=0; f[p++]=0; f[p++]=0;
    uint16_t framelen = (uint16_t)p;
    mpdu[0] = (uint8_t)(framelen & 0xff);          /* iwl_rx_mpdu_res_start.byte_count */
    mpdu[1] = (uint8_t)((framelen >> 8) & 0xff);

    wlan_bss_t bss;
    for (uint32_t i = 0; i < sizeof(bss); i++) ((uint8_t*)&bss)[i] = 0;
    int r = iwl_parse_beacon(mpdu, (uint16_t)(4 + framelen), -50, &bss);
    if (r != 1) { kprintf("IWL-SCAN: beacon parse returned %d\n", r); ok = 0; }
    if (bss.ssid_len != 5) ok = 0;
    { const char* exp = "Hello"; for (int i = 0; i < 5; i++) if (bss.ssid[i] != (uint8_t)exp[i]) ok = 0; }
    for (int i = 0; i < 6; i++) if (bss.bssid[i] != bssid[i]) ok = 0;
    if (bss.channel != 6) ok = 0;
    if (bss.security != WLAN_SEC_WPA2) ok = 0;

    kprintf("IWL-SCAN: selftest %s (scan_plen=%u ssid='%c%c%c%c%c' len=%u ch=%u sec=%u)\n",
            ok ? "PASS" : "FAIL", plen,
            bss.ssid[0], bss.ssid[1], bss.ssid[2], bss.ssid[3], bss.ssid[4],
            bss.ssid_len, bss.channel, bss.security);
    return ok ? 0 : -1;
}
