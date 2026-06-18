/*
 * wifi.h -- WiFi HAL seam (the swap contract).
 * ============================================
 * A `wifi_ops` control struct hangs off `netif_t`. The GUI + supplicant talk
 * only to SYS_WLAN_* syscalls; those call only `wifi_ops`; the MLME drives the
 * radio via tx_mgmt/rx_poll_mgmt. The simulated backend (wifisim) and the real
 * iwlwifi driver BOTH implement this identical contract -- swapping one for the
 * other touches ONLY the driver, nothing above the seam.
 *
 * Kernel-internal types. The userspace ABI mirror lives in uapi/wlan.h and must
 * be kept in lockstep with the enums/sizes here.
 */
#ifndef WIFI_H
#define WIFI_H

#include "types.h"
#include "net.h"   /* ETH_ALEN */

struct netif;       /* forward decl -- netif.h refers to wifi_ops only by ptr */

/* link/auth state surfaced to userspace (mirror: uapi_wlan_status_t.state) */
typedef enum {
    WLAN_DOWN = 0,
    WLAN_SCANNING,
    WLAN_AUTHENTICATING,
    WLAN_ASSOCIATING,
    WLAN_ASSOCIATED,
    WLAN_4WAY,
    WLAN_CONNECTED,
    WLAN_FAILED
} wlan_state_t;

/* security class (mirror: uapi_wlan_bss_t.security / connect.security) */
typedef enum {
    WLAN_SEC_OPEN = 0,
    WLAN_SEC_WPA2 = 1,
    WLAN_SEC_WPA3 = 2
} wlan_sec_t;

#define WLAN_SSID_MAX 32

/* one scan result row (kernel-internal; UAPI mirror = uapi_wlan_bss_t) */
typedef struct wlan_bss {
    uint8_t  bssid[ETH_ALEN];
    uint8_t  ssid[WLAN_SSID_MAX];
    uint8_t  ssid_len;
    uint8_t  security;           /* wlan_sec_t */
    uint16_t channel;
    int16_t  signal;             /* dBm */
    uint16_t capability;
} wlan_bss_t;

/*
 * wifi_ops -- the control-plane operations a wifi driver/backend implements.
 * The MLME (mac80211) provides scan/connect/disconnect/get_status by default;
 * a concrete driver supplies ONLY the radio seam (tx_mgmt/rx_poll_mgmt) +
 * set_key. The mock backend and the real iwlwifi differ in exactly those.
 */
typedef struct wifi_ops {
    int (*scan_start)(struct netif* nif);                       /* async start */
    int (*scan_results)(struct netif* nif, wlan_bss_t* out, int max);
    int (*connect)(struct netif* nif, const wlan_bss_t* bss,
                   const char* passphrase);                     /* auth+assoc  */
    int (*disconnect)(struct netif* nif);
    int (*set_key)(struct netif* nif, int key_idx, int pairwise,
                   const uint8_t* key, int key_len);            /* install PTK/GTK */
    int (*get_status)(struct netif* nif, wlan_state_t* st, int16_t* rssi);
    /* RADIO SEAM -- the only part the mock and iwlwifi differ in: */
    int (*tx_mgmt)(struct netif* nif, const void* frame, uint16_t len);
    int (*rx_poll_mgmt)(struct netif* nif, void* buf, uint16_t cap);
} wifi_ops_t;

/* First registered netif with a non-NULL wifi_ops (kernel/net/netif.c). */
struct netif* netif_get_wifi_default(void);

/* WIFI-SEAM boot self-test: asserts the wifi field exists + holds ops, that
 * wired interfaces keep wifi==NULL, and that the lookup resolves. Non-polluting
 * (registers nothing). Prints "WIFISEAM: PASS ...". */
void wifi_seam_selftest(void);

#endif /* WIFI_H */
