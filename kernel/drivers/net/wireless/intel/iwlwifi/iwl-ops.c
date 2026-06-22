/*
 * iwl-ops.c -- the iwlwifi wifi_ops implementation + the held top-level entry.
 * ===========================================================================
 * This is where the real radio meets the rest of the OS. It implements the
 * wifi_ops control-plane seam (the SAME contract wifisim implements, so swapping
 * the sim for the real radio touches nothing above the seam: SYS_WLAN_*, the
 * supplicant, the Network Manager GUI), and it provides iwl_wifi_bringup() -- the
 * single, held entry that powers the radio, loads firmware to ALIVE, reads the
 * NVM, and registers a REAL "wlan0".
 *
 *   ===================  HELD FOR HARDWARE -- READ THIS  ===================
 *   iwl_wifi_bringup() is the ONLY entry and it is NOT called from any boot path
 *   in this tree. The future post-desktop trigger (a separate parent brick, on
 *   the physical T410) calls it. No QEMU emulates the card, so the whole tail is
 *   correct-by-review against Linux iwlwifi DVM:
 *     dvm/main.c   -- iwl_setup_deferred_work / iwlagn_mac_setup_register (the
 *                     bring-up order: trans -> ucode/alive -> nvm -> scan).
 *     dvm/rx.c     -- the REPLY_* notification dispatch this brick consumes via
 *                     iwl-hostcmd's RX drain.
 *     dvm/scan.c   -- REPLY_SCAN_CMD (iwl-scan.c).
 *     dvm/sta.c / dvm/rxon.c -- REPLY_ADD_STA / REPLY_RXON (connect, scaffolded).
 *
 *   Safety: every sub-step is the bounded, marker-printing, abort-clean brick it
 *   delegates to. iwl_wifi_bringup returns cleanly (registers nothing) on any
 *   failure; it never panics and never blocks unbounded.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-ops.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf */
#include "string.h"          /* memset / memcpy */
#include "pci.h"
#include "netif.h"
#include "wifi.h"
#include "iwl-devices.h"
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"
#include "iwl-hostcmd.h"
#include "iwl-fw-file.h"
#include "iwl-fw-load.h"
#include "iwl-nvm.h"
#include "iwl-scan.h"
#include "iwl-rxon.h"

/* The firmware parser entry (iwl-fw.c) + the section-capture helper. */
int iwl_fw_load_from_initrd(const char* path, struct iwl_fw* out);

/* ====================================================================== *
 *  Driver state (all file-static; nothing allocated until bring-up runs).
 * ====================================================================== */
static struct iwl_trans g_trans;          /* the one transport instance      */
static iwl_nvm_data_t   g_nvm;            /* MAC + channels                  */
static wlan_bss_t       g_scan[32];       /* last scan results               */
static int              g_scan_n   = 0;
static wlan_state_t     g_state    = WLAN_DOWN;
static int16_t          g_rssi     = 0;
static int              g_alive    = 0;   /* 1 once firmware ALIVE + NVM read */

/* The raw .ucode blob bytes (kept so iwl_fw_capture_sections can re-walk it).
 * We re-read from the initrd at bring-up; the pointer stays valid (initrd is
 * resident). */
static iwl_fw_images_t  g_fw_images;

/* ====================================================================== *
 *  wifi_ops -- control plane. These run AFTER bring-up; if the radio never came
 *  alive (g_alive == 0) they fail cleanly so the stack above the seam degrades
 *  gracefully (exactly like "no networks").
 * ====================================================================== */

static int iwl_ops_scan_start(struct netif* nif) {
    (void)nif;
    if (!g_alive) { kprintf("IWLOPS: scan_start but radio not alive\n"); return -1; }

    /* Live RF-kill re-check: the user can flip the physical wireless switch at any
     * time. If asserted, a scan is guaranteed empty -- report clearly + bail so the
     * GUI can tell the user WHY (rather than a mystery "no networks"). */
    if (iwl_is_rfkill(&g_trans)) {
        kprintf("IWLOPS: scan aborted -- HW RF-KILL is ON (flip the wireless switch)\n");
        g_state = WLAN_DOWN;
        return -1;
    }

    g_state = WLAN_SCANNING;

    /* THE #1 SCAN PREREQUISITE: configure the radio's MAC receiver via a baseline
     * RXON BEFORE scanning. Without a committed RXON the DVM firmware delivers no
     * received frames, so the scan would harvest zero beacons (no SSIDs). We use
     * the first NVM channel (or channel 1) as the baseline listen channel; the
     * scan command then hops channels itself. (Audit root cause #1.) */
    uint8_t base_ch = (g_nvm.n_channels > 0) ? (uint8_t)g_nvm.channels[0].number : 1;
    if (iwl_rxon_baseline(&g_trans, base_ch) != 0) {
        kprintf("IWLOPS: baseline RXON FAILED -- scan would be deaf, abort\n");
        g_state = WLAN_DOWN;
        return -1;
    }

    int n = iwl_scan(&g_trans, &g_nvm, g_scan,
                     (int)(sizeof(g_scan) / sizeof(g_scan[0])));
    if (n < 0) { g_state = WLAN_DOWN; g_scan_n = 0; return -1; }
    g_scan_n = n;
    g_state  = WLAN_DOWN;     /* scan finished */
    kprintf("IWLOPS: scan_start -> %d BSS\n", n);
    return 0;
}

static int iwl_ops_scan_results(struct netif* nif, wlan_bss_t* out, int max) {
    (void)nif;
    int n = g_scan_n; if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = g_scan[i];
    return n;
}

/*
 * connect -- BEST-EFFORT / SCAFFOLDED. The DVM association path is: build the
 * RXON (REPLY_RXON) for the target BSS, set timing (REPLY_RXON_TIMING), add the
 * AP as a station (REPLY_ADD_STA), then the host supplicant drives the WPA2
 * 4-way handshake and installs keys via set_key() below. Each of those host
 * commands needs hardware validation (the exact RXON/sta struct layouts are
 * large and family-sensitive), so we DO NOT fabricate them blind here -- we send
 * the minimal sequence with clearly-marked TODOs and report ASSOCIATING so the
 * supplicant can proceed. A real connect requires pinning these on the T410.
 */
static int iwl_ops_connect(struct netif* nif, const wlan_bss_t* bss,
                           const char* passphrase) {
    (void)passphrase;
    if (!g_alive || !bss) return -1;

    kprintf("IWLOPS: connect ssid_len=%u sec=%u (DVM assoc -- scaffolded)\n",
            bss->ssid_len, bss->security);

    /* TODO(HARDWARE): build + send REPLY_RXON for `bss` (channel, bssid, basic
     * rates, RXON flags). struct iwl_rxon_cmd is large + family-specific. */
    /* TODO(HARDWARE): REPLY_RXON_TIMING (beacon interval from the beacon). */
    /* TODO(HARDWARE): REPLY_ADD_STA to add the AP station + its tx queues. */

    if (nif) nif->up = true;
    g_state = (bss->security == WLAN_SEC_OPEN) ? WLAN_CONNECTED : WLAN_ASSOCIATED;
    g_rssi  = bss->signal;
    /* OPEN networks have no 4-way; the IP stack can run immediately. Secured
     * networks wait for the supplicant + set_key() to reach CONNECTED. */
    return 0;
}

static int iwl_ops_disconnect(struct netif* nif) {
    if (!g_alive) return -1;
    /* TODO(HARDWARE): REPLY_RXON with assoc cleared to drop the link. */
    g_state = WLAN_DOWN;
    if (nif) nif->up = false;
    kprintf("IWLOPS: disconnect\n");
    return 0;
}

/*
 * set_key -- install the PTK/GTK after the supplicant's 4-way handshake. The DVM
 * path is REPLY_ADD_STA carrying the key (or REPLY_WEPKEY for WEP). Layout is
 * family-specific, so this is scaffolded: on a successful pairwise key install
 * we advance state to CONNECTED (mirroring wifisim's contract) and mark the TODO
 * for the real REPLY_ADD_STA key command.
 */
static int iwl_ops_set_key(struct netif* nif, int idx, int pairwise,
                           const uint8_t* key, int klen) {
    (void)nif; (void)idx; (void)key;
    if (!g_alive) return -1;
    /* TODO(HARDWARE): REPLY_ADD_STA with the key material (PTK pairwise / GTK). */
    if (pairwise && klen > 0) {
        /* NOTE: WLAN_CONNECTED here is a SCAFFOLDED control-plane state, NOT a
         * verified RF link -- the RXON/ADD_STA above are TODO no-ops, so the radio
         * is not really associated. It is safe today only because the IP data path
         * does NOT flow through netif_t.tx (net.c net_send() hard-codes the wired
         * NIC via g_nic), so nothing is actually transmitted over the dead radio.
         * When the real RXON/ADD_STA land (hardware phase), net_send() must be made
         * wlan0-aware AT THE SAME TIME, or DHCP/traffic would be sent into a dead
         * link. Do not trust this CONNECTED as a real link-up signal until then. */
        g_state = WLAN_CONNECTED;
        kprintf("IWLOPS: set_key pairwise len=%d -> CONNECTED (scaffolded; key cmd TODO)\n", klen);
    }
    return 0;
}

static int iwl_ops_get_status(struct netif* nif, wlan_state_t* st, int16_t* rssi) {
    (void)nif;
    if (st)   *st   = g_state;
    if (rssi) *rssi = g_rssi;
    return 0;
}

/*
 * The radio data seam. Once associated, real IP frames would tx/rx via the
 * firmware's REPLY_TX + RX. Wiring the data queues is the post-scan hardware
 * tail; for now these are honest stubs (the control plane -- scan -- is the
 * load-bearing deliverable). tx_mgmt/rx_poll_mgmt are unused on DVM (the
 * firmware owns MLME), so they no-op.
 */
static int iwl_ops_tx_mgmt(struct netif* nif, const void* f, uint16_t len) {
    (void)nif; (void)f; (void)len; return 0;
}
static int iwl_ops_rx_poll_mgmt(struct netif* nif, void* b, uint16_t cap) {
    (void)nif; (void)b; (void)cap; return 0;
}

static wifi_ops_t g_iwl_ops = {
    .scan_start   = iwl_ops_scan_start,
    .scan_results = iwl_ops_scan_results,
    .connect      = iwl_ops_connect,
    .disconnect   = iwl_ops_disconnect,
    .set_key      = iwl_ops_set_key,
    .get_status   = iwl_ops_get_status,
    .tx_mgmt      = iwl_ops_tx_mgmt,
    .rx_poll_mgmt = iwl_ops_rx_poll_mgmt,
};

/* ====================================================================== *
 *  netif data-path shims. The wired netif_t carries tx/rx_poll/get_mac function
 *  pointers; once the radio is associated these would carry real IP traffic. We
 *  give get_mac the real NVM MAC (so DHCP/ARP use the radio's address) and stub
 *  tx/rx_poll until the DVM data queues are wired (post-scan hardware tail).
 * ====================================================================== */
static int iwl_netif_get_mac(uint8_t out[ETH_ALEN]) {
    for (int i = 0; i < ETH_ALEN; i++) out[i] = g_trans.mac[i];
    return 0;
}
static int iwl_netif_tx(const void* frame, uint16_t len) {
    (void)frame; (void)len;
    /* TODO(HARDWARE): REPLY_TX on the data queue. */
    return -1;
}
static int iwl_netif_rx_poll(void* buf, uint16_t cap) {
    (void)buf; (void)cap;
    /* TODO(HARDWARE): consume RX data frames (REPLY_RX) off the ring. */
    return 0;
}

/* ====================================================================== *
 *  iwl_wifi_bringup -- THE HELD TOP-LEVEL ENTRY.
 *  Called ONLY by the future post-desktop trigger on the real T410 (a separate
 *  parent brick), NEVER from boot. Chain:
 *     detect card + map BAR0 (re-using IWL-IDENT's logic)
 *       -> iwl_trans_bringup (APM + rings)
 *       -> parse .ucode from initrd + capture sections
 *       -> iwl_load_ucode (INIT->ALIVE->calib->RUNTIME->ALIVE)
 *       -> iwl_read_nvm (MAC + channels)
 *       -> register a REAL wlan0 with g_iwl_ops + the real MAC
 *  Returns 0 on a registered live wlan0, -1 on any clean failure.
 * ====================================================================== */
void iwl_wifi_bringup(void) {
    kprintf("IWLWIFI: bring-up START (real radio; T410 only)\n");

    memset(&g_trans, 0, sizeof(g_trans));
    memset(&g_nvm,   0, sizeof(g_nvm));
    g_scan_n = 0; g_state = WLAN_DOWN; g_alive = 0;

    /* ---- detect the card + map BAR0 (IWL-IDENT logic, inline) ---- */
    pci_device_t* dev = (pci_device_t*)0;
    const char* name = (const char*)0;
    const char* fwfam = (const char*)0;
    for (int i = 0; i < IWL_NDEVICES; i++) {
        dev = pci_find_device(IWL_VENDOR_INTEL, iwl_devices[i].device);
        if (dev) { name = iwl_devices[i].name; fwfam = iwl_devices[i].fw_family; break; }
    }
    if (!dev) {
        kprintf("IWLWIFI: no Intel WiFi card present -- nothing to bring up\n");
        return;
    }
    kprintf("IWLWIFI: card %s (fw family iwlwifi-%s)\n", name, fwfam);

    /* Classify the family from the fw_family string (iwl-devices.h). This drives
     * the 5000/1000-only ANA_PLL APM step and the NVM geometry. The strings are
     * "1000", "5000", "6000", "6000g2a". pll_cfg is true for 1000 + 5000 (Linux
     * iwl1000/iwl5000_base_params.pll_cfg), false for 6000. */
    g_trans.family  = IWL_FAM_6000;
    g_trans.pll_cfg = 0;
    if (fwfam) {
        if (fwfam[0] == '1') {                 /* "1000" */
            g_trans.family = IWL_FAM_1000;  g_trans.pll_cfg = 1;
        } else if (fwfam[0] == '5') {          /* "5000" */
            g_trans.family = IWL_FAM_5000;  g_trans.pll_cfg = 1;
        } else if (fwfam[4] == 'g') {          /* "6000g2a" */
            g_trans.family = IWL_FAM_6000G2; g_trans.pll_cfg = 0;
        } else {                                /* "6000" */
            g_trans.family = IWL_FAM_6000;  g_trans.pll_cfg = 0;
        }
    }
    kprintf("IWLWIFI: family=%d pll_cfg=%d\n", g_trans.family, g_trans.pll_cfg);

    pci_enable_memory_space(dev);
    pci_enable_bus_master(dev);
    uint64_t bar0 = pci_get_bar(dev, 0);
    if (!bar0) {
        kprintf("IWLWIFI: BAR0 not mapped -- abort\n");
        return;
    }
    g_trans.mmio = (volatile uint8_t*)(bar0 & ~0xFULL);
    g_trans.hw_rev = iwl_read32(&g_trans, CSR_HW_REV);
    kprintf("IWLWIFI: BAR0 mapped, CSR_HW_REV=0x%08x\n", g_trans.hw_rev);

    /* ---- transport: APM power-up + cmd/RX rings ---- */
    if (iwl_trans_bringup(&g_trans) != 0) {
        kprintf("IWLWIFI: transport bring-up FAILED -- abort\n");
        return;
    }

    /* ---- firmware: parse the .ucode from the initrd + capture sections ---- *
     * The firmware file name follows the family (iwlwifi-<fam>-N.ucode). We try
     * a family-named path; absent firmware fails cleanly (no radio, no crash). */
    struct iwl_fw fw_meta;
    /* Build the conventional path. We keep it simple: the operator drops the
     * matching ucode at /lib/firmware/iwlwifi.ucode (a stable alias). The real
     * driver tries versioned names; the alias keeps this brick firmware-agnostic.
     * HARDWARE-VALIDATION: the versioned name probe is a refinement. */
    const char* fw_path = "/lib/firmware/iwlwifi.ucode";
    if (iwl_fw_load_from_initrd(fw_path, &fw_meta) != 0) {
        kprintf("IWLWIFI: firmware %s not available -- abort "
                "(provide iwlwifi-%s-*.ucode)\n", fw_path, fwfam);
        return;
    }

    /* Re-walk the SAME file bytes to capture section pointers for the DMA load. */
    {
        uint64_t fsz = 0;
        extern void* initrd_get_file(const char* path, uint64_t* size);
        void* blob = initrd_get_file(fw_path, &fsz);
        if (!blob || fsz == 0 || fsz > (uint64_t)0xFFFFFFFFu ||
            iwl_fw_capture_sections((const uint8_t*)blob, (uint32_t)fsz,
                                    &g_fw_images) != 0) {
            kprintf("IWLWIFI: firmware section capture FAILED -- abort\n");
            return;
        }
    }

    /* ---- load firmware to ALIVE (INIT->calib->RUNTIME) ---- */
    if (iwl_load_ucode(&g_trans, &g_fw_images) != 0) {
        kprintf("IWLWIFI: ucode load/ALIVE FAILED -- abort\n");
        return;
    }

    /* ---- read NVM (MAC + channels) ---- */
    if (iwl_read_nvm(&g_trans, &g_nvm) != 0) {
        kprintf("IWLWIFI: NVM read FAILED -- abort\n");
        return;
    }

    g_alive = 1;   /* radio is alive + identified; control plane may run */

    /* ---- register the REAL wlan0 behind the wifi_ops seam ---- */
    netif_t w;
    memset(&w, 0, sizeof(w));
    memcpy(w.name, "wlan0", 6);
    memcpy(w.mac, g_trans.mac, ETH_ALEN);
    w.up      = false;                  /* down until a connect brings it up */
    w.wifi    = &g_iwl_ops;
    w.get_mac = iwl_netif_get_mac;
    w.tx      = iwl_netif_tx;
    w.rx_poll = iwl_netif_rx_poll;

    if (netif_register(&w) == 0) {
        kprintf("IWLWIFI: registered REAL wlan0 "
                "(%02x:%02x:%02x:%02x:%02x:%02x, %d channels) -- radio LIVE\n",
                g_trans.mac[0], g_trans.mac[1], g_trans.mac[2],
                g_trans.mac[3], g_trans.mac[4], g_trans.mac[5], g_nvm.n_channels);
    } else {
        /* Registration failed (name clash with wifisim, or registry full). Roll
         * g_alive back so a re-run of iwlup re-does the FULL bring-up (NVM/scan)
         * instead of operating on stale state behind a non-existent wlan0.
         * (Audit #9: g_alive was left set on this path.) */
        g_alive = 0;
        kprintf("IWLWIFI: wlan0 register FAILED "
                "(name clash? ensure wifisim is NOT compiled in) -- radio NOT live\n");
    }
}
