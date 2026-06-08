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
