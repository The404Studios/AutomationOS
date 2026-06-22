/*
 * wlansyscall.c -- WiFi control-plane syscalls (SYS_WLAN_*).
 * =========================================================
 * Thin handlers: find the wifi interface (netif_get_wifi_default), validate the
 * user struct, and call through netif_t.wifi (wifi_ops). The MLME / the
 * simulated backend / the real iwlwifi driver implement wifi_ops; these handlers
 * never touch a driver directly -- that is the swap seam.
 *
 *   SYS_WLAN_SCAN(113)       scan_start + scan_results -> user array; ret count
 *   SYS_WLAN_CONNECT(114)    connect(ssid, passphrase)
 *   SYS_WLAN_STATUS(115)     get_status -> user struct
 *   SYS_WLAN_DISCONNECT(116) disconnect
 *   SYS_WLAN_SET_KEY(117)    set_key (supplicant installs PTK/GTK)
 *
 * Returns: 0 / count (>=0) on success, or a negative errno (ENOTSUP when no
 * wifi interface is registered -- e.g. a wired-only or no-NIC build).
 *
 * Scope: kernel/net/wlansyscall.c
 */
#include "../include/netif.h"
#include "../include/wifi.h"
#include "../include/uapi/wlan.h"
#include "../include/types.h"
#include "../include/mem.h"      /* copy_from_user / copy_to_user / COPY_SUCCESS */
#include "../include/errno.h"    /* EINVAL / EFAULT / ENOTSUP / EIO (negative)   */
#include "../include/string.h"   /* memset / memcpy                              */

#define WLAN_SCAN_CAP 16         /* max scan rows marshalled per call            */

static netif_t* wlan_if(void) {
    return (netif_t*)netif_get_wifi_default();
}

/* SYS_WLAN_SCAN: trigger a scan + copy results (uapi_wlan_bss_t[]) to userspace. */
int64_t sys_wlan_scan(uint64_t out_ptr, uint64_t max_entries, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (out_ptr == 0 || max_entries == 0) return EINVAL;

    netif_t* nif = wlan_if();
    if (!nif || !nif->wifi || !nif->wifi->scan_results) return ENOTSUP;

    int cap = (int)max_entries;
    if (cap > WLAN_SCAN_CAP) cap = WLAN_SCAN_CAP;

    if (nif->wifi->scan_start) nif->wifi->scan_start(nif);

    wlan_bss_t kbss[WLAN_SCAN_CAP];
    int n = nif->wifi->scan_results(nif, kbss, cap);
    if (n <= 0) return 0;
    if (n > cap) n = cap;

    /* marshal kernel wlan_bss_t -> the UAPI mirror */
    uapi_wlan_bss_t ubss[WLAN_SCAN_CAP];
    memset(ubss, 0, sizeof(ubss));
    for (int i = 0; i < n; i++) {
        memcpy(ubss[i].bssid, kbss[i].bssid, 6);
        memcpy(ubss[i].ssid,  kbss[i].ssid, 32);
        ubss[i].ssid_len   = kbss[i].ssid_len;
        ubss[i].security   = kbss[i].security;
        ubss[i].channel    = kbss[i].channel;
        ubss[i].signal     = kbss[i].signal;
        ubss[i].capability = kbss[i].capability;
    }
    if (copy_to_user((void*)out_ptr, ubss,
                     (size_t)n * sizeof(uapi_wlan_bss_t)) != COPY_SUCCESS)
        return EFAULT;
    return n;
}

/* SYS_WLAN_CONNECT: join an SSID (with optional WPA passphrase). */
int64_t sys_wlan_connect(uint64_t req_ptr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (req_ptr == 0) return EINVAL;

    netif_t* nif = wlan_if();
    if (!nif || !nif->wifi || !nif->wifi->connect) return ENOTSUP;

    uapi_wlan_connect_t req;
    if (copy_from_user(&req, (const void*)req_ptr, sizeof(req)) != COPY_SUCCESS)
        return EFAULT;
    if (req.ssid_len > 32) req.ssid_len = 32;
    req.passphrase[sizeof(req.passphrase) - 1] = '\0';

    wlan_bss_t bss;
    memset(&bss, 0, sizeof(bss));
    memcpy(bss.ssid, req.ssid, 32);
    bss.ssid_len = req.ssid_len;
    bss.security = req.security;
    memcpy(bss.bssid, req.bssid, 6);

    int r = nif->wifi->connect(nif, &bss, req.passphrase);
    return (r == 0) ? 0 : EIO;
}

/* SYS_WLAN_STATUS: current association state + signal. */
int64_t sys_wlan_status(uint64_t out_ptr, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (out_ptr == 0) return EINVAL;

    netif_t* nif = wlan_if();
    if (!nif || !nif->wifi || !nif->wifi->get_status) return ENOTSUP;

    wlan_state_t st = WLAN_DOWN;
    int16_t rssi = 0;
    nif->wifi->get_status(nif, &st, &rssi);

    uapi_wlan_status_t out;
    memset(&out, 0, sizeof(out));
    out.state = (uint8_t)st;
    out.rssi  = (int8_t)rssi;
    if (copy_to_user((void*)out_ptr, &out, sizeof(out)) != COPY_SUCCESS)
        return EFAULT;
    return 0;
}

/* SYS_WLAN_DISCONNECT. */
int64_t sys_wlan_disconnect(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    netif_t* nif = wlan_if();
    if (!nif || !nif->wifi || !nif->wifi->disconnect) return ENOTSUP;
    int r = nif->wifi->disconnect(nif);
    return (r == 0) ? 0 : EIO;
}

/* SYS_WLAN_DIAG: copy the radio bring-up diagnostics snapshot to userspace so the
 * Network Manager can show WHERE bring-up stopped on real hardware (card, family,
 * RF-kill, stage, MAC, channels, last scan count, message) -- no serial needed. */
int64_t sys_wlan_diag(uint64_t out_ptr, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (out_ptr == 0) return EINVAL;
    uapi_wlan_diag_t d;
    memset(&d, 0, sizeof(d));
    wifi_diag_get(&d);
    if (copy_to_user((void*)out_ptr, &d, sizeof(d)) != COPY_SUCCESS)
        return EFAULT;
    return 0;
}

/* SYS_WLAN_SET_KEY: the supplicant installs a PTK (pairwise) or GTK (group). */
int64_t sys_wlan_set_key(uint64_t req_ptr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (req_ptr == 0) return EINVAL;

    netif_t* nif = wlan_if();
    if (!nif || !nif->wifi || !nif->wifi->set_key) return ENOTSUP;

    uapi_wlan_setkey_t req;
    if (copy_from_user(&req, (const void*)req_ptr, sizeof(req)) != COPY_SUCCESS)
        return EFAULT;
    if (req.key_len > 32) return EINVAL;

    int r = nif->wifi->set_key(nif, (int)req.key_idx, (int)req.pairwise,
                               req.key, (int)req.key_len);
    return (r == 0) ? 0 : EIO;
}
