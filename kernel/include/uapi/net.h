#ifndef UAPI_NET_H
#define UAPI_NET_H

/*
 * UAPI: Kernel/Userspace ABI contract for networking structs.
 *
 * RULE: If the kernel copies a struct to/from userspace, that struct
 * lives HERE. Not in random app-local typedefs. Both kernel and
 * userspace #include this file. No copy-paste. No drift.
 *
 * ANY change to these structs is an ABI break that must update:
 *   1. The ABI_SIZE constant
 *   2. The _Static_assert
 *   3. All consumers (grep for the type name)
 */

#ifdef __KERNEL__
#include "../types.h"
#else
/* Freestanding userspace — provide the needed integer types locally. */
#ifndef _UAPI_STDINT_DEFINED
#define _UAPI_STDINT_DEFINED
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
#endif
#endif

/* -------------------------------------------------------------------
 * SYS_NET_INFO (59): query per-interface network state
 * ------------------------------------------------------------------- */
#define NET_INFO_ABI_SIZE  80

typedef struct {
    char     ifname[16];        /* interface name ("eth0", "wlan0")    */
    uint8_t  mac[6];            /* hardware MAC address                */
    uint8_t  _pad[2];
    uint32_t ip;                /* host byte order                     */
    uint32_t netmask;           /* host byte order                     */
    uint32_t gateway;           /* host byte order                     */
    uint32_t dns;               /* host byte order                     */
    uint8_t  up;                /* 1 = link up                         */
    uint8_t  dhcp;              /* 1 = DHCP active                     */
    uint8_t  _reserved[6];      /* align to 8 for uint64_t counters    */
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} uapi_net_info_t;

_Static_assert(sizeof(uapi_net_info_t) == NET_INFO_ABI_SIZE,
               "uapi_net_info_t ABI size drift");

/* -------------------------------------------------------------------
 * SYS_NET_CONFIG (89): apply IP/mask/gw/dns to a named interface
 * ------------------------------------------------------------------- */
#define NET_CONFIG_ABI_SIZE  36

/* Flag bits for uapi_net_config_t.flags */
#define NET_CONFIG_FLAG_UP     (1u << 0)
#define NET_CONFIG_FLAG_DOWN   (1u << 1)
#define NET_CONFIG_FLAG_DHCP   (1u << 2)
/* E1000-PCH-0B: trigger a DEFERRED NIC bring-up (the T410 82577LM defers its
 * ME-shared-MDIO init out of boot; this completes it post-desktop). Works
 * while net is DOWN; clean ENOTSUP no-op when nothing was deferred. */
#define NET_CONFIG_FLAG_NIC_BRINGUP (1u << 3)

typedef struct {
    char     ifname[16];        /* target interface name               */
    uint32_t ip;                /* 0 = don't change                    */
    uint32_t netmask;           /* 0 = don't change                    */
    uint32_t gateway;           /* 0 = don't change                    */
    uint32_t dns;               /* 0 = don't change                    */
    uint32_t flags;             /* NET_CONFIG_FLAG_*                   */
} uapi_net_config_t;

_Static_assert(sizeof(uapi_net_config_t) == NET_CONFIG_ABI_SIZE,
               "uapi_net_config_t ABI size drift");

/* -------------------------------------------------------------------
 * SYS_ROUTE_TABLE (90): dump routing table entries
 * ------------------------------------------------------------------- */
#define ROUTE_INFO_ABI_SIZE  16

typedef struct {
    uint32_t dest;              /* destination network (host order)    */
    uint32_t mask;              /* netmask (host order)                */
    uint32_t gateway;           /* gateway IP (host order)             */
    uint32_t iface_ip;          /* interface IP (host order)           */
} uapi_route_info_t;

_Static_assert(sizeof(uapi_route_info_t) == ROUTE_INFO_ABI_SIZE,
               "uapi_route_info_t ABI size drift");

/* -------------------------------------------------------------------
 * SYS_ARP_TABLE (91): dump ARP cache entries
 * ------------------------------------------------------------------- */
#define ARP_INFO_ABI_SIZE  12

typedef struct {
    uint32_t ip;                /* host byte order                     */
    uint8_t  mac[6];
    uint8_t  valid;
    uint8_t  _pad;
} uapi_arp_info_t;

_Static_assert(sizeof(uapi_arp_info_t) == ARP_INFO_ABI_SIZE,
               "uapi_arp_info_t ABI size drift");

#endif /* UAPI_NET_H */
