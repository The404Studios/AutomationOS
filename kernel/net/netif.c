/*
 * netif.c -- Network interface registry
 * =======================================
 *
 * Manages a small static array of netif_t descriptors. net.c registers
 * "eth0" at boot; future drivers (virtio-net, loopback) add their own.
 *
 * Scope: kernel/net/netif.c (new).
 */

#include "../include/netif.h"
#include "../include/net.h"
#include "../include/kernel.h"
#include "../include/string.h"
#include "../include/wifi.h"   /* WIFI-SEAM: wifi_ops + netif_get_wifi_default */
#include "../include/uapi/wlan.h" /* WIFI-SEAM: freeze + ABI-check the SYS_WLAN_* structs */

/* ------------------------------------------------------------------ */
/* Static registry                                                     */
/* ------------------------------------------------------------------ */
static netif_t  g_netifs[NETIF_MAX];
static int      g_netif_count;

int netif_register(const netif_t* nif) {
    if (!nif || g_netif_count >= NETIF_MAX) return -1;

    /* Reject duplicates. */
    for (int i = 0; i < g_netif_count; i++) {
        if (strcmp(g_netifs[i].name, nif->name) == 0) return -1;
    }

    memcpy(&g_netifs[g_netif_count], nif, sizeof(netif_t));
    kprintf("[NETIF] registered %s\n", g_netifs[g_netif_count].name);
    g_netif_count++;
    return 0;
}

netif_t* netif_get(const char* name) {
    for (int i = 0; i < g_netif_count; i++) {
        if (strcmp(g_netifs[i].name, name) == 0)
            return &g_netifs[i];
    }
    return (netif_t*)0;
}

netif_t* netif_get_default(void) {
    for (int i = 0; i < g_netif_count; i++) {
        if (g_netifs[i].up) return &g_netifs[i];
    }
    return (netif_t*)0;
}

netif_t* netif_get_by_index(int idx) {
    if (idx < 0 || idx >= g_netif_count) return (netif_t*)0;
    return &g_netifs[idx];
}

/* AUDIT FIX (K1 prerequisite): look up the interface that owns an IP so ip_tx can
 * route to the right netif->tx instead of a hard-coded global NIC. Returns 0 on hit. */
int netif_get_by_ip(uint32_t ip, netif_t** out) {
    if (!out) return -1;
    for (int i = 0; i < g_netif_count; i++) {
        if (g_netifs[i].ip == ip) { *out = &g_netifs[i]; return 0; }
    }
    return -1;
}

int netif_count(void) {
    return g_netif_count;
}

void netif_set_ip(netif_t* nif, uint32_t ip) {
    if (nif) nif->ip = ip;
}

void netif_set_gateway(netif_t* nif, uint32_t gw) {
    if (nif) nif->gateway = gw;
}

void netif_set_dns(netif_t* nif, uint32_t dns) {
    if (nif) nif->dns = dns;
}

void netif_up(netif_t* nif) {
    if (nif) nif->up = true;
}

void netif_down(netif_t* nif) {
    if (nif) nif->up = false;
}

/*
 * netif_sync_globals -- push the default (first UP) interface's IP/gateway
 * into the legacy net.c globals so that net_get_ip(), ARP source-address,
 * and ip_tx() all reflect the current netif configuration (e.g. after DHCP).
 */
void netif_sync_globals(void) {
    netif_t* nif = netif_get_default();
    if (!nif) return;

    net_set_ip(nif->ip);
    net_set_gateway(nif->gateway);
}

/* ------------------------------------------------------------------ */
/* WIFI-SEAM: the wifi HAL lookup + boot self-test                     */
/* ------------------------------------------------------------------ */

/*
 * netif_get_wifi_default -- first registered interface carrying wifi_ops.
 * Returns NULL until a wifi driver/backend (wifisim or iwlwifi) registers a
 * wlan netif. The GUI/supplicant/SYS_WLAN_* path use this to find the radio.
 */
struct netif* netif_get_wifi_default(void) {
    for (int i = 0; i < g_netif_count; i++) {
        if (g_netifs[i].wifi) return &g_netifs[i];
    }
    return (struct netif*)0;
}

/*
 * wifi_seam_selftest -- prove the seam compiled + wired, WITHOUT polluting the
 * registry. Asserts: (1) the netif_t.wifi field exists and can hold ops;
 * (2) wired interfaces (eth0) carry wifi==NULL; (3) the lookup resolves (NULL
 * now, since no wifi backend is registered yet -- WIFI-SIM makes it non-NULL).
 */
void wifi_seam_selftest(void) {
    static wifi_ops_t dummy;            /* zeroed; members are never called    */
    netif_t probe;                      /* stack-local -- NOT registered        */
    memset(&probe, 0, sizeof(probe));
    probe.wifi = &dummy;

    int field_ok   = (probe.wifi == &dummy);
    netif_t* def   = netif_get_default();
    int wired_null = (!def || def->wifi == (struct wifi_ops*)0);
    int lookup_ok  = (netif_get_wifi_default() == (struct netif*)0);

    if (field_ok && wired_null && lookup_ok)
        kprintf("WIFISEAM: PASS field=ok wired_wifi=null wifi_default=none\n");
    else
        kprintf("WIFISEAM: FAIL field=%d wired_null=%d lookup=%d\n",
                field_ok, wired_null, lookup_ok);
}
