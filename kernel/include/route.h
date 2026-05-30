#ifndef ROUTE_H
#define ROUTE_H

#include "types.h"

/*
 * Simple IPv4 routing table.
 * ===========================
 *
 * Provides longest prefix match routing for IPv4 packets.
 * All IP addresses are in HOST byte order.
 */

/* Initialize the routing table with default routes. */
void route_init(void);

/*
 * Add a route to the table.
 *   dest:    destination network (host order)
 *   mask:    netmask (host order)
 *   gateway: next-hop IP (host order), 0 = on-link (send directly)
 *   iface:   local interface IP (host order)
 *
 * Example:
 *   route_add(0x0A000200, 0xFFFFFF00, 0, 0x0A00020F);  // 10.0.2.0/24 on-link
 *   route_add(0, 0, 0x0A000202, 0x0A00020F);           // default via 10.0.2.2
 */
int route_add(uint32_t dest, uint32_t mask, uint32_t gateway, uint32_t iface);

/*
 * Delete a route from the table.
 * Returns 0 on success, -1 if not found.
 */
int route_del(uint32_t dest, uint32_t mask);

/*
 * Longest prefix match lookup for dst_ip (host order).
 * On success (returns 0):
 *   *out_gateway is set to the next hop (0 = on-link, send to dst_ip directly).
 *   *out_iface is set to the local interface IP to use as source.
 * On failure (returns -1): no route found.
 */
int route_lookup(uint32_t dst_ip, uint32_t* out_gateway, uint32_t* out_iface);

/*
 * Print the routing table to the kernel console (for debugging).
 */
void route_print(void);

#endif /* ROUTE_H */
