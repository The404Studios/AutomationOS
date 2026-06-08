/*
 * netif.h -- Network interface abstraction
 * ==========================================
 *
 * Defines the netif_t structure representing a logical network interface
 * and the related structs used by the SYS_NET_INFO, SYS_NET_CONFIG,
 * SYS_ROUTE_TABLE, and SYS_ARP_TABLE syscalls.
 *
 * All IP addresses are in HOST byte order internally (same as net.c).
 */
#ifndef NETIF_H
#define NETIF_H

#include "types.h"
#include "net.h"   /* ETH_ALEN */
#include "uapi/net.h" /* canonical UAPI struct definitions */

#define NETIF_NAME_MAX  16
#define NETIF_MAX        4

/* ------------------------------------------------------------------ */
/* Network interface descriptor                                        */
/* ------------------------------------------------------------------ */
typedef struct netif {
    char     name[NETIF_NAME_MAX];       /* e.g. "eth0"                  */
    uint8_t  mac[ETH_ALEN];
    uint32_t ip;                         /* host byte order              */
    uint32_t netmask;                    /* host byte order              */
    uint32_t gateway;                    /* host byte order              */
    uint32_t dns;                        /* host byte order              */
    bool     up;
    bool     dhcp_active;

    /* Driver callbacks. */
    int  (*tx)(const void* frame, uint16_t len);
    int  (*rx_poll)(void* buf, uint16_t buf_len);
    int  (*get_mac)(uint8_t out[ETH_ALEN]);

    /* Traffic counters. */
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} netif_t;

/* ------------------------------------------------------------------ */
/* Syscall payload structs — canonical definitions in uapi/net.h      */
/* ------------------------------------------------------------------ */

/* Legacy aliases: new code should use the uapi_* names directly. */
typedef uapi_net_info_t    net_info_ext_t;
typedef uapi_net_config_t  net_config_req_t;
typedef uapi_route_info_t  route_info_t;
typedef uapi_arp_info_t    arp_info_t;

/* NET_CONFIG_FLAG_* are now defined in uapi/net.h. */

/* ------------------------------------------------------------------ */
/* Interface registry API (kernel/net/netif.c)                         */
/* ------------------------------------------------------------------ */
int      netif_register(const netif_t* nif);
netif_t* netif_get(const char* name);
netif_t* netif_get_default(void);       /* first UP interface           */
netif_t* netif_get_by_index(int idx);
int      netif_count(void);

void     netif_set_ip(netif_t* nif, uint32_t ip);
void     netif_set_gateway(netif_t* nif, uint32_t gw);
void     netif_set_dns(netif_t* nif, uint32_t dns);
void     netif_up(netif_t* nif);
void     netif_down(netif_t* nif);

/* Sync default netif fields -> legacy net struct globals. */
void     netif_sync_globals(void);

/* Kernel helpers for syscalls. */
int      net_get_arp_table(arp_info_t* out, int max);
int      route_get_table(route_info_t* out, int max);

/* New syscall prototypes. */
int64_t sys_net_config(uint64_t req_ptr, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_route_table(uint64_t out_ptr, uint64_t max_entries, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_arp_table(uint64_t out_ptr, uint64_t max_entries, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* NETIF_H */
