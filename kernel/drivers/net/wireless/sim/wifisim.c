/*
 * wifisim.c -- simulated WiFi backend (the swap-seam reference implementation).
 * ============================================================================
 * Registers a "wlan0" netif whose wifi_ops return a fixed set of fake APs and
 * simulate auth/assoc + a connect, so the ENTIRE stack above the seam (the
 * SYS_WLAN_* syscalls, the Network Manager GUI, the WPA supplicant, DHCP) runs
 * end-to-end in QEMU at zero cost -- no real radio. The real iwlwifi driver
 * implements the SAME wifi_ops contract; swapping it in touches nothing above
 * the seam.
 *
 * M1 (this brick): control-plane only -- scan + a simulated connect/status. The
 * data path (tx/rx_poll/get_mac) stays NULL; once associated, real IP traffic
 * falls through to the wired NIC under slirp (M2 wires the supplicant + DHCP).
 *
 * Gated -DWIFI_SIM. Scope: kernel/drivers/net/wireless/sim/wifisim.c
 */
#include "netif.h"
#include "wifi.h"
#include "kernel.h"   /* kprintf */
#include "string.h"   /* memset / memcpy */

/* The canned AP list the sim "sees" (a realistic mix of OPEN/WPA2/WPA3). */
static const struct {
    const char* ssid;
    uint8_t     sec;        /* wlan_sec_t            */
    uint16_t    channel;
    int16_t     signal;     /* dBm                   */
    uint8_t     bssid_last; /* last octet of a fake BSSID */
} g_sim_aps[] = {
    { "AutomationOS-Open", WLAN_SEC_OPEN, 1,  -40, 0x01 },
    { "HomeNet",           WLAN_SEC_WPA2, 6,  -55, 0x02 },
    { "Guest5G",           WLAN_SEC_WPA2, 36, -70, 0x03 },
    { "SecureMesh",        WLAN_SEC_WPA3, 11, -60, 0x04 },
};
#define SIM_NAP ((int)(sizeof(g_sim_aps) / sizeof(g_sim_aps[0])))

/* sim connection state */
static wlan_state_t g_sim_state    = WLAN_DOWN;
static int16_t      g_sim_rssi     = 0;
static uint8_t      g_sim_ssid[WLAN_SSID_MAX];
static uint8_t      g_sim_ssid_len = 0;

static int sim_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

/* ---- wifi_ops implementation -------------------------------------------- */

static int sim_scan_start(struct netif* nif) {
    (void)nif; g_sim_state = WLAN_SCANNING; return 0;
}

static int sim_scan_results(struct netif* nif, wlan_bss_t* out, int max) {
    (void)nif;
    int n = SIM_NAP; if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].bssid[0] = 0x02; out[i].bssid[1] = 0x00; out[i].bssid[2] = 0xCA;
        out[i].bssid[3] = 0xFE; out[i].bssid[4] = 0x00;
        out[i].bssid[5] = g_sim_aps[i].bssid_last;
        int sl = sim_strlen(g_sim_aps[i].ssid); if (sl > 32) sl = 32;
        memcpy(out[i].ssid, g_sim_aps[i].ssid, (unsigned)sl);
        out[i].ssid_len   = (uint8_t)sl;
        out[i].security   = g_sim_aps[i].sec;
        out[i].channel    = g_sim_aps[i].channel;
        out[i].signal     = g_sim_aps[i].signal;
        out[i].capability = 0;
    }
    if (g_sim_state == WLAN_SCANNING) g_sim_state = WLAN_DOWN;   /* scan done */
    kprintf("WIFISIM: scan -> %d APs\n", n);
    return n;
}

static int sim_connect(struct netif* nif, const wlan_bss_t* bss,
                       const char* passphrase) {
    (void)passphrase;
    if (!bss) return -1;
    int sl = bss->ssid_len; if (sl > 32) sl = 32;
    memcpy(g_sim_ssid, bss->ssid, (unsigned)sl); g_sim_ssid_len = (uint8_t)sl;
    g_sim_rssi = -55;
    /* OPEN associates straight to CONNECTED; WPA2/WPA3 reach ASSOCIATED and wait
     * for the supplicant's 4-way handshake (which lands via set_key in M2). */
    g_sim_state = (bss->security == WLAN_SEC_OPEN) ? WLAN_CONNECTED
                                                   : WLAN_ASSOCIATED;
    nif->up = true;   /* bring wlan0 up so the IP stack/DHCP can run on it */
    kprintf("WIFISIM: connect ssid_len=%d sec=%d -> state=%d\n",
            sl, (int)bss->security, (int)g_sim_state);
    return 0;
}

static int sim_disconnect(struct netif* nif) {
    g_sim_state = WLAN_DOWN; if (nif) nif->up = false; return 0;
}

static int sim_set_key(struct netif* nif, int idx, int pairwise,
                       const uint8_t* key, int klen) {
    (void)nif; (void)idx; (void)key;
    /* Installing the pairwise key (PTK) means the 4-way handshake completed. */
    if (pairwise && klen > 0) {
        g_sim_state = WLAN_CONNECTED;
        kprintf("WIFISIM: PTK installed (len=%d) -> CONNECTED\n", klen);
    }
    return 0;
}

static int sim_get_status(struct netif* nif, wlan_state_t* st, int16_t* rssi) {
    (void)nif;
    if (st)   *st   = g_sim_state;
    if (rssi) *rssi = g_sim_rssi;
    return 0;
}

static int sim_tx_mgmt(struct netif* nif, const void* f, uint16_t len) {
    (void)nif; (void)f; (void)len; return 0;   /* M1: sim short-circuits MLME */
}
static int sim_rx_poll_mgmt(struct netif* nif, void* b, uint16_t cap) {
    (void)nif; (void)b; (void)cap; return 0;
}

static wifi_ops_t g_sim_ops = {
    .scan_start   = sim_scan_start,
    .scan_results = sim_scan_results,
    .connect      = sim_connect,
    .disconnect   = sim_disconnect,
    .set_key      = sim_set_key,
    .get_status   = sim_get_status,
    .tx_mgmt      = sim_tx_mgmt,
    .rx_poll_mgmt = sim_rx_poll_mgmt,
};

/* Registered once at boot under -DWIFI_SIM (kernel.c, after wifi_seam_selftest). */
void wifisim_init(void) {
    netif_t w;
    memset(&w, 0, sizeof(w));
    memcpy(w.name, "wlan0", 6);
    w.mac[0] = 0x02; w.mac[1] = 0x00; w.mac[2] = 0xCA;
    w.mac[3] = 0xFE; w.mac[4] = 0x00; w.mac[5] = 0x10;
    w.up   = false;          /* down until a connect brings it up */
    w.wifi = &g_sim_ops;
    if (netif_register(&w) == 0)
        kprintf("WIFISIM: registered wlan0 (%d APs, sim backend)\n", SIM_NAP);
    else
        kprintf("WIFISIM: register FAILED\n");
}
