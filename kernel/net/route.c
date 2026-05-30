/*
 * route.c -- Simple IPv4 routing table with longest prefix match.
 * ================================================================
 *
 * Implements a basic routing table for IPv4 with up to ROUTE_MAX entries.
 * Supports adding routes (destination network, netmask, gateway, interface),
 * longest prefix match lookup, and a default route (0.0.0.0/0).
 *
 * All IPs are in HOST byte order internally.
 *
 * Scope: kernel/net/route.c (new).
 */

#include "../include/route.h"
#include "../include/net.h"
#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/string.h"

/* ------------------------------------------------------------------ */
/* Routing table                                                       */
/* ------------------------------------------------------------------ */
#define ROUTE_MAX 16

typedef struct {
    bool     valid;
    uint32_t dest;         /* destination network (host order)      */
    uint32_t mask;         /* netmask (host order)                  */
    uint32_t gateway;      /* gateway IP (host order), 0 = on-link  */
    uint32_t iface;        /* interface IP (host order)             */
} route_entry_t;

static struct {
    route_entry_t routes[ROUTE_MAX];
    int           count;
} g_rtable;

/* ------------------------------------------------------------------ */
/* Routing table API                                                   */
/* ------------------------------------------------------------------ */

void route_init(void) {
    memset(&g_rtable, 0, sizeof(g_rtable));

    /* Add default on-link route for QEMU user-net 10.0.2.0/24. */
    uint32_t local_ip = net_get_ip();
    if (local_ip != 0) {
        route_add(local_ip & 0xFFFFFF00u, 0xFFFFFF00u, 0, local_ip);
    }

    /* Add default route via gateway. */
    route_add(0, 0, NET_QEMU_GATEWAY, local_ip);
}

int route_add(uint32_t dest, uint32_t mask, uint32_t gateway, uint32_t iface) {
    /* Find existing entry or free slot. */
    int slot = -1;

    /* Check if route already exists (update it). */
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (g_rtable.routes[i].valid &&
            g_rtable.routes[i].dest == (dest & mask) &&
            g_rtable.routes[i].mask == mask) {
            slot = i;
            break;
        }
    }

    /* Find free slot if not updating. */
    if (slot == -1) {
        for (int i = 0; i < ROUTE_MAX; i++) {
            if (!g_rtable.routes[i].valid) {
                slot = i;
                break;
            }
        }
    }

    if (slot == -1) {
        /* Table full -- evict default route or slot 0. */
        for (int i = 0; i < ROUTE_MAX; i++) {
            if (g_rtable.routes[i].dest == 0 && g_rtable.routes[i].mask == 0) {
                slot = i;
                break;
            }
        }
        if (slot == -1) slot = 0;
    }

    g_rtable.routes[slot].valid   = true;
    g_rtable.routes[slot].dest    = dest & mask;  /* normalize */
    g_rtable.routes[slot].mask    = mask;
    g_rtable.routes[slot].gateway = gateway;
    g_rtable.routes[slot].iface   = iface;

    /* Update count. */
    g_rtable.count = 0;
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (g_rtable.routes[i].valid) g_rtable.count++;
    }

    return 0;
}

int route_del(uint32_t dest, uint32_t mask) {
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (g_rtable.routes[i].valid &&
            g_rtable.routes[i].dest == (dest & mask) &&
            g_rtable.routes[i].mask == mask) {
            g_rtable.routes[i].valid = false;
            g_rtable.count--;
            return 0;
        }
    }
    return -1;
}

/*
 * Longest prefix match: find the most specific route for dst_ip.
 * Returns 0 on success, -1 if no route found.
 * On success: *out_gateway is the next hop (0 = on-link, send directly).
 *             *out_iface is the local interface IP to use.
 */
int route_lookup(uint32_t dst_ip, uint32_t* out_gateway, uint32_t* out_iface) {
    route_entry_t* best = NULL;
    uint32_t best_prefix_len = 0;

    for (int i = 0; i < ROUTE_MAX; i++) {
        if (!g_rtable.routes[i].valid) continue;

        uint32_t mask = g_rtable.routes[i].mask;
        if ((dst_ip & mask) == g_rtable.routes[i].dest) {
            /* Count prefix length (number of 1 bits in mask). */
            uint32_t len = 0;
            for (uint32_t m = mask; m; m >>= 1) len += (m & 1);

            if (len > best_prefix_len || best == NULL) {
                best = &g_rtable.routes[i];
                best_prefix_len = len;
            }
        }
    }

    if (best == NULL) return -1;

    if (out_gateway) *out_gateway = best->gateway;
    if (out_iface)   *out_iface   = best->iface;
    return 0;
}

void route_print(void) {
    kprintf("[ROUTE] Routing table (%d entries):\n", g_rtable.count);
    kprintf("  Destination     Netmask         Gateway         Interface\n");

    for (int i = 0; i < ROUTE_MAX; i++) {
        if (!g_rtable.routes[i].valid) continue;

        route_entry_t* r = &g_rtable.routes[i];
        kprintf("  %u.%u.%u.%u",
                (r->dest >> 24) & 0xFF, (r->dest >> 16) & 0xFF,
                (r->dest >> 8) & 0xFF, r->dest & 0xFF);
        kprintf("     %u.%u.%u.%u",
                (r->mask >> 24) & 0xFF, (r->mask >> 16) & 0xFF,
                (r->mask >> 8) & 0xFF, r->mask & 0xFF);

        if (r->gateway == 0) {
            kprintf("     on-link         ");
        } else {
            kprintf("     %u.%u.%u.%u",
                    (r->gateway >> 24) & 0xFF, (r->gateway >> 16) & 0xFF,
                    (r->gateway >> 8) & 0xFF, r->gateway & 0xFF);
        }

        kprintf("     %u.%u.%u.%u\n",
                (r->iface >> 24) & 0xFF, (r->iface >> 16) & 0xFF,
                (r->iface >> 8) & 0xFF, r->iface & 0xFF);
    }
}
