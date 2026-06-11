/*
 * Networking syscall helpers (SYS_NET_* surface).
 * ================================================
 *
 * Handlers for SYS_NET_SEND (68), SYS_NET_RECV (69), SYS_NET_INFO (59),
 * SYS_NET_CONFIG (89), SYS_ROUTE_TABLE (90), and SYS_ARP_TABLE (91).
 *
 * ABI:
 *   sys_net_send(user_buf, len)   -> bytes sent (>0) | -EINVAL | -EFAULT
 *   sys_net_recv(user_buf, len)   -> frame len (>0) | 0 (none) | -errno
 *   sys_net_info(user uapi_net_info_t)   -> 0 | -errno
 *   sys_net_config(user uapi_net_config_t) -> 0 | -errno
 *   sys_route_table(user buf, max_entries) -> count (>=0) | -errno
 *   sys_arp_table(user buf, max_entries)   -> count (>=0) | -errno
 *
 * Scope: kernel/net/netsyscall.c
 */

#include "../include/net.h"
#include "../include/netif.h"
#include "../include/uapi/net.h" /* canonical UAPI struct definitions */
#include "../include/route.h"    /* route_get_table() */
#include "../include/types.h"
#include "../include/mem.h"      /* copy_from_user / copy_to_user, COPY_*  */
#include "../include/errno.h"    /* canonical negative errno (EINVAL/EFAULT/ENOTSUP) */
#include "../include/string.h"   /* memset, memcpy, strcmp */

/* Bound user transfers to one Ethernet frame. */
#define NET_SYS_MAX_FRAME  ETH_MAX_FRAME

int64_t sys_net_send(uint64_t buf, uint64_t len, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!net_up()) return ENOTSUP;
    if (buf == 0 || len == 0 || len > NET_SYS_MAX_FRAME) return EINVAL;

    uint8_t kframe[NET_SYS_MAX_FRAME];
    if (copy_from_user(kframe, (const void*)buf, (size_t)len) != COPY_SUCCESS) {
        return EFAULT;
    }

    int sent = net_send(kframe, (uint16_t)len);
    return (sent < 0) ? (int64_t)EINVAL : (int64_t)sent;
}

int64_t sys_net_recv(uint64_t buf, uint64_t len, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!net_up()) return ENOTSUP;
    if (buf == 0 || len == 0) return EINVAL;

    uint16_t cap = (len > NET_SYS_MAX_FRAME) ? NET_SYS_MAX_FRAME : (uint16_t)len;

    uint8_t kframe[NET_SYS_MAX_FRAME];
    int n = net_recv(kframe, cap);
    if (n <= 0) {
        return (int64_t)n;   /* 0 = nothing pending, <0 = error */
    }

    if (copy_to_user((void*)buf, kframe, (size_t)n) != COPY_SUCCESS) {
        return EFAULT;
    }
    return (int64_t)n;
}

/*
 * sys_net_info -- fill a userspace uapi_net_info_t with the default
 * interface's state.  Struct layout defined in uapi/net.h (80 bytes).
 */
int64_t sys_net_info(uint64_t out_info, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!net_up()) return ENOTSUP;
    if (out_info == 0) return EINVAL;

    uapi_net_info_t info;
    memset(&info, 0, sizeof(info));

    /* Try the netif registry first (it has the richest data). */
    netif_t* nif = netif_get_default();
    if (nif) {
        memcpy(info.ifname, nif->name, NETIF_NAME_MAX);
        memcpy(info.mac, nif->mac, ETH_ALEN);
        info.ip           = nif->ip;
        info.netmask      = nif->netmask;
        info.gateway      = nif->gateway;
        info.dns          = nif->dns;
        info.up           = nif->up;
        info.dhcp         = nif->dhcp_active;
        info.tx_packets   = nif->tx_packets;
        info.rx_packets   = nif->rx_packets;
        info.tx_bytes     = nif->tx_bytes;
        info.rx_bytes     = nif->rx_bytes;
    } else {
        /* Fallback: use legacy net.c globals. */
        const char *name = "eth0";
        memcpy(info.ifname, name, 5);
        if (net_get_mac(info.mac) != 0) return ENOTSUP;
        info.ip      = net_get_ip();
        info.netmask = 0xFFFFFF00u;     /* 255.255.255.0 default   */
        info.gateway = NET_QEMU_GATEWAY;
        info.dns     = NET_QEMU_DNS;
        info.up      = 1;
    }

    if (copy_to_user((void*)out_info, &info, sizeof(info)) != COPY_SUCCESS) {
        return EFAULT;
    }
    return 0;
}

/*
 * sys_net_config -- apply a network configuration to a named interface.
 *
 * Userspace passes a uapi_net_config_t (from uapi/net.h):
 *   ifname[16], ip, netmask, gateway, dns, flags.
 * Fields set to 0 are left unchanged.  Also updates the legacy net.c globals
 * so that net_get_ip(), ARP, and ip_tx() source-IP all reflect the new config.
 */
int64_t sys_net_config(uint64_t req_ptr, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (req_ptr == 0) return EINVAL;

    uapi_net_config_t req;
    if (copy_from_user(&req, (const void*)req_ptr, sizeof(req)) != COPY_SUCCESS)
        return EFAULT;

    /* Ensure ifname is NUL-terminated. */
    req.ifname[NETIF_NAME_MAX - 1] = '\0';

    /* E1000-PCH-0B: the deferred-NIC trigger MUST work while net is DOWN --
     * that is its whole point (the T410's PCH NIC defers its risky bring-up
     * out of boot; nicup invokes it from the running desktop). On a machine
     * with nothing deferred (QEMU classic NIC already up, or no NIC) this is
     * a clean diagnostic no-op. */
    if (req.flags & NET_CONFIG_FLAG_NIC_BRINGUP) {
        extern int net_attach_late(void);
        int r = net_attach_late();
        if (r != 0) return (r == -2) ? ENOTSUP : EIO;
        /* fall through: the rest of the request (IP/flags) applies to the
         * freshly registered interface. */
    }

    if (!net_up()) return ENOTSUP;

    netif_t* nif = netif_get(req.ifname);
    if (!nif) {
        /* If no registered interface, try the default. */
        nif = netif_get_default();
    }

    if (nif) {
        if (req.ip)      netif_set_ip(nif, req.ip);
        if (req.netmask) nif->netmask = req.netmask;
        if (req.gateway) netif_set_gateway(nif, req.gateway);
        if (req.dns)     netif_set_dns(nif, req.dns);

        if (req.flags & NET_CONFIG_FLAG_UP)   netif_up(nif);
        if (req.flags & NET_CONFIG_FLAG_DOWN) netif_down(nif);
        if (req.flags & NET_CONFIG_FLAG_DHCP) nif->dhcp_active = true;
    }

    /* Sync to legacy net.c globals so net_get_ip(), ARP source-IP, and
     * ip_tx() all reflect the new configuration. */
    netif_sync_globals();

    return 0;
}

/*
 * sys_route_table -- copy the kernel routing table to a userspace buffer.
 *
 * arg1 = user pointer to array of route_info_t
 * arg2 = max entries the buffer can hold
 * Returns: number of entries copied (>=0), or -errno.
 */
int64_t sys_route_table(uint64_t out_ptr, uint64_t max_entries, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (out_ptr == 0 || max_entries == 0) return EINVAL;

    /* Cap to a sane stack-local limit. */
    int cap = (int)max_entries;
    if (cap > 64) cap = 64;

    route_info_t kbuf[64];
    int n = route_get_table(kbuf, cap);
    if (n <= 0) return 0;

    size_t copy_bytes = (size_t)n * sizeof(route_info_t);
    if (copy_to_user((void*)out_ptr, kbuf, copy_bytes) != COPY_SUCCESS)
        return EFAULT;

    return (int64_t)n;
}

/*
 * sys_arp_table -- copy the kernel ARP cache to a userspace buffer.
 *
 * arg1 = user pointer to array of arp_info_t
 * arg2 = max entries the buffer can hold
 * Returns: number of entries copied (>=0), or -errno.
 */
int64_t sys_arp_table(uint64_t out_ptr, uint64_t max_entries, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (out_ptr == 0 || max_entries == 0) return EINVAL;

    /* Cap to a sane stack-local limit. */
    int cap = (int)max_entries;
    if (cap > 64) cap = 64;

    arp_info_t kbuf[64];
    int n = net_get_arp_table(kbuf, cap);
    if (n <= 0) return 0;

    size_t copy_bytes = (size_t)n * sizeof(arp_info_t);
    if (copy_to_user((void*)out_ptr, kbuf, copy_bytes) != COPY_SUCCESS)
        return EFAULT;

    return (int64_t)n;
}
