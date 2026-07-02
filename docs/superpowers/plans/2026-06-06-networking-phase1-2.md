# Networking Phase 1+2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hardcoded network globals with a `netif_t` interface abstraction, add syscalls to configure interfaces at runtime, wire DHCP lease application, and build CLI diagnostic tools.

**Architecture:** A `netif_t` struct wraps per-interface state (IP, MAC, gateway, driver callbacks). The existing `net.c` globals are replaced by a registered `eth0` interface. New syscalls (`SYS_NET_CONFIG`, `SYS_ROUTE_TABLE`, `SYS_ARP_TABLE`) expose kernel network state to userspace. The DHCP client calls `SYS_NET_CONFIG` to apply leases. CLI tools query these syscalls.

**Tech Stack:** C (freestanding kernel + freestanding userspace), x86-64 syscall ABI, QEMU e1000 NIC

---

### Task 1: Create `netif_t` header

**Files:**
- Create: `kernel/include/netif.h`

- [ ] **Step 1: Write the netif header**

```c
#ifndef NETIF_H
#define NETIF_H

#include "types.h"

#define NETIF_NAME_MAX  8
#define NETIF_MAX       4

typedef struct netif {
    char     name[NETIF_NAME_MAX];
    uint8_t  mac[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    bool     up;
    bool     dhcp_active;

    int  (*tx)(struct netif* nif, const void* frame, uint16_t len);
    int  (*rx_poll)(struct netif* nif, void* buf, uint16_t buf_len);
    int  (*get_mac)(struct netif* nif, uint8_t out[6]);

    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} netif_t;

int      netif_register(netif_t* nif);
netif_t* netif_get(const char* name);
netif_t* netif_get_default(void);
int      netif_count(void);
netif_t* netif_get_by_index(int idx);
int      netif_set_ip(netif_t* nif, uint32_t ip, uint32_t mask);
int      netif_set_gateway(netif_t* nif, uint32_t gw);
int      netif_set_dns(netif_t* nif, uint32_t dns);
int      netif_up(netif_t* nif);
int      netif_down(netif_t* nif);

/* Shared struct for SYS_NET_INFO (kernel↔userspace). */
typedef struct {
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    char     ifname[NETIF_NAME_MAX];
    uint8_t  up;
    uint8_t  dhcp;
    uint8_t  _reserved[2];
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} net_info_ext_t;

/* Shared struct for SYS_NET_CONFIG (userspace→kernel). */
typedef struct {
    char     ifname[NETIF_NAME_MAX];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint32_t flags;
} net_config_req_t;

/* Shared struct for SYS_ROUTE_TABLE. */
typedef struct {
    uint32_t dest;
    uint32_t mask;
    uint32_t gateway;
    uint32_t iface_ip;
    uint8_t  valid;
    uint8_t  _pad[3];
} route_info_t;

/* Shared struct for SYS_ARP_TABLE. */
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
    uint8_t  _pad;
} arp_info_t;

#endif /* NETIF_H */
```

- [ ] **Step 2: Commit**

```bash
git add kernel/include/netif.h
git commit -m "net: add netif_t interface abstraction header"
```

---

### Task 2: Implement `netif.c` interface registry

**Files:**
- Create: `kernel/net/netif.c`

- [ ] **Step 1: Write the netif registry implementation**

```c
#include "../include/netif.h"
#include "../include/string.h"
#include "../include/kernel.h"

static netif_t g_netifs[NETIF_MAX];
static int     g_netif_count = 0;

int netif_register(netif_t* nif) {
    if (!nif || g_netif_count >= NETIF_MAX) return -1;
    if (nif->name[0] == '\0') return -1;
    /* Reject duplicate names. */
    for (int i = 0; i < g_netif_count; i++) {
        if (strcmp(g_netifs[i].name, nif->name) == 0) return -1;
    }
    g_netifs[g_netif_count] = *nif;
    g_netif_count++;
    kprintf("[NETIF] registered '%s' mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            nif->name,
            nif->mac[0], nif->mac[1], nif->mac[2],
            nif->mac[3], nif->mac[4], nif->mac[5]);
    return 0;
}

netif_t* netif_get(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_netif_count; i++) {
        if (strcmp(g_netifs[i].name, name) == 0)
            return &g_netifs[i];
    }
    return NULL;
}

netif_t* netif_get_default(void) {
    for (int i = 0; i < g_netif_count; i++) {
        if (g_netifs[i].up) return &g_netifs[i];
    }
    return (g_netif_count > 0) ? &g_netifs[0] : NULL;
}

int netif_count(void) { return g_netif_count; }

netif_t* netif_get_by_index(int idx) {
    if (idx < 0 || idx >= g_netif_count) return NULL;
    return &g_netifs[idx];
}

int netif_set_ip(netif_t* nif, uint32_t ip, uint32_t mask) {
    if (!nif) return -1;
    nif->ip = ip;
    nif->netmask = mask;
    return 0;
}

int netif_set_gateway(netif_t* nif, uint32_t gw) {
    if (!nif) return -1;
    nif->gateway = gw;
    return 0;
}

int netif_set_dns(netif_t* nif, uint32_t dns) {
    if (!nif) return -1;
    nif->dns = dns;
    return 0;
}

int netif_up(netif_t* nif) {
    if (!nif) return -1;
    nif->up = true;
    return 0;
}

int netif_down(netif_t* nif) {
    if (!nif) return -1;
    nif->up = false;
    return 0;
}
```

- [ ] **Step 2: Add `netif.c` to the build**

In `scripts/quick_build.sh`, add after the `kernel/net/route.c` compile line:
```bash
compile kernel/net/netif.c
```

- [ ] **Step 3: Verify it compiles**

Run: `bash scripts/quick_build.sh` in WSL
Expected: 84 compiled, 0 failed

- [ ] **Step 4: Commit**

```bash
git add kernel/net/netif.c scripts/quick_build.sh
git commit -m "net: implement netif interface registry"
```

---

### Task 3: Refactor `net.c` to use `netif_t`

**Files:**
- Modify: `kernel/net/net.c`

- [ ] **Step 1: Add e1000 driver callbacks**

At the top of `net.c`, after the includes, add static wrapper functions that adapt the existing e1000/rtl8139 calls to the `netif_t` callback signature:

```c
#include "../include/netif.h"

static int eth0_tx(netif_t* nif, const void* frame, uint16_t len) {
    (void)nif;
    return (g_nic == NIC_RTL8139) ? rtl8139_tx(frame, len) : e1000_tx(frame, len);
}

static int eth0_rx_poll(netif_t* nif, void* buf, uint16_t buf_len) {
    (void)nif;
    return (g_nic == NIC_RTL8139) ? rtl8139_rx_poll(buf, buf_len) : e1000_rx_poll(buf, buf_len);
}

static int eth0_get_mac(netif_t* nif, uint8_t out[6]) {
    (void)nif;
    return (g_nic == NIC_E1000) ? e1000_get_mac(out) : rtl8139_get_mac(out);
}
```

- [ ] **Step 2: Register eth0 in `net_init()`**

After the existing MAC read and before the ARP settle loop, register the interface:

```c
/* Register eth0 as a netif. */
{
    netif_t eth0 = {0};
    strncpy(eth0.name, "eth0", NETIF_NAME_MAX - 1);
    memcpy(eth0.mac, net.mac, ETH_ALEN);
    eth0.ip      = net.ip;
    eth0.netmask = 0xFFFFFF00u;  /* 255.255.255.0 */
    eth0.gateway = net.gateway;
    eth0.dns     = NET_QEMU_DNS;
    eth0.up      = true;
    eth0.tx      = eth0_tx;
    eth0.rx_poll = eth0_rx_poll;
    eth0.get_mac = eth0_get_mac;
    netif_register(&eth0);
}
```

- [ ] **Step 3: Route `net_send`/`net_recv`/`net_get_ip`/`net_up` through netif**

Replace the bodies of these functions to read from `netif_get_default()` while keeping backward compat:

```c
int net_send(const void* frame, uint16_t len) {
    netif_t* nif = netif_get_default();
    if (!nif || !nif->up || !nif->tx) return -1;
    int r = nif->tx(nif, frame, len);
    if (r > 0) { nif->tx_packets++; nif->tx_bytes += r; }
    return r;
}

int net_recv(void* buf, uint16_t buf_len) {
    netif_t* nif = netif_get_default();
    if (!nif || !nif->up || !nif->rx_poll) return -1;
    int n = nif->rx_poll(nif, buf, buf_len);
    if (n > 0) {
        nif->rx_packets++; nif->rx_bytes += n;
        net_input((const uint8_t*)buf, (uint16_t)n);
    }
    return n;
}

uint32_t net_get_ip(void) {
    netif_t* nif = netif_get_default();
    return nif ? nif->ip : 0;
}

bool net_up(void) {
    netif_t* nif = netif_get_default();
    return nif && nif->up;
}
```

Keep the `net.up`, `net.ip`, `net.gateway`, `net.mac` globals synced for now (backward compat with ARP/ICMP code that reads them directly). Add a helper:

```c
static void netif_sync_globals(void) {
    netif_t* nif = netif_get_default();
    if (nif) {
        net.ip      = nif->ip;
        net.gateway  = nif->gateway;
        memcpy(net.mac, nif->mac, ETH_ALEN);
        net.up       = nif->up;
    }
}
```

Call `netif_sync_globals()` at the end of `net_init()` and whenever `netif_set_ip`/`netif_set_gateway` are called.

- [ ] **Step 4: Build and verify**

Run: `bash scripts/quick_build.sh` in WSL
Expected: 84 compiled, 0 failed, link OK. Boot behavior unchanged.

- [ ] **Step 5: Commit**

```bash
git add kernel/net/net.c
git commit -m "net: route net_send/recv/get_ip through netif abstraction"
```

---

### Task 4: Add `SYS_NET_CONFIG` syscall

**Files:**
- Modify: `kernel/include/syscall.h`
- Modify: `kernel/net/netsyscall.c`
- Modify: `kernel/core/syscall/syscall.c`

- [ ] **Step 1: Add syscall number**

In `kernel/include/syscall.h`, after `SYS_RECOVERY_OVERLAY 88`:
```c
#define SYS_NET_CONFIG   89  // apply IP/mask/gw/dns to a named interface
#define SYS_ROUTE_TABLE  90  // copy route table entries to user buffer
#define SYS_ARP_TABLE    91  // copy ARP cache entries to user buffer
```

- [ ] **Step 2: Implement `sys_net_config` handler**

In `kernel/net/netsyscall.c`, add at the end:

```c
#include "../include/netif.h"

int64_t sys_net_config(uint64_t req_ptr, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (req_ptr == 0) return EINVAL;

    net_config_req_t req;
    if (copy_from_user(&req, (const void*)req_ptr, sizeof(req)) != COPY_SUCCESS)
        return EFAULT;

    req.ifname[NETIF_NAME_MAX - 1] = '\0';
    netif_t* nif = netif_get(req.ifname);
    if (!nif) return EINVAL;

    if (req.ip)      netif_set_ip(nif, req.ip, req.netmask ? req.netmask : nif->netmask);
    if (req.netmask && !req.ip) netif_set_ip(nif, nif->ip, req.netmask);
    if (req.gateway) netif_set_gateway(nif, req.gateway);
    if (req.dns)     netif_set_dns(nif, req.dns);

    /* Sync legacy globals so ARP/ICMP/routing still work. */
    extern void netif_sync_globals(void);
    netif_sync_globals();

    /* Update route table with new gateway. */
    if (req.gateway) {
        extern void route_init(void);
        route_init();
    }

    kprintf("[NET] config %s: ip=%08x mask=%08x gw=%08x dns=%08x\n",
            nif->name, nif->ip, nif->netmask, nif->gateway, nif->dns);
    return 0;
}
```

- [ ] **Step 3: Update `sys_net_info` to use netif**

Replace the existing `sys_net_info` body:

```c
int64_t sys_net_info(uint64_t out_info, uint64_t ifindex, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (out_info == 0) return EINVAL;

    netif_t* nif;
    if (ifindex == 0xFFFFFFFF || ifindex == 0) {
        nif = netif_get_default();
    } else {
        nif = netif_get_by_index((int)ifindex);
    }
    if (!nif) return EINVAL;

    net_info_ext_t info = {0};
    memcpy(info.mac, nif->mac, 6);
    info.ip         = nif->ip;
    info.netmask    = nif->netmask;
    info.gateway    = nif->gateway;
    info.dns        = nif->dns;
    strncpy(info.ifname, nif->name, NETIF_NAME_MAX - 1);
    info.up         = nif->up ? 1 : 0;
    info.dhcp       = nif->dhcp_active ? 1 : 0;
    info.tx_packets = nif->tx_packets;
    info.rx_packets = nif->rx_packets;
    info.tx_bytes   = nif->tx_bytes;
    info.rx_bytes   = nif->rx_bytes;

    if (copy_to_user((void*)out_info, &info, sizeof(info)) != COPY_SUCCESS)
        return EFAULT;
    return 0;
}
```

- [ ] **Step 4: Add route table and ARP table syscalls**

```c
int64_t sys_route_table(uint64_t out_buf, uint64_t max_entries, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (out_buf == 0 || max_entries == 0) return EINVAL;

    extern int route_get_table(route_info_t* out, int max);
    route_info_t entries[16];
    int count = route_get_table(entries, 16);
    if (count < 0) count = 0;
    if ((uint64_t)count > max_entries) count = (int)max_entries;

    if (count > 0) {
        if (copy_to_user((void*)out_buf, entries, count * sizeof(route_info_t)) != COPY_SUCCESS)
            return EFAULT;
    }
    return (int64_t)count;
}

int64_t sys_arp_table(uint64_t out_buf, uint64_t max_entries, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (out_buf == 0 || max_entries == 0) return EINVAL;

    extern int net_get_arp_table(arp_info_t* out, int max);
    arp_info_t entries[16];
    int count = net_get_arp_table(entries, 16);
    if (count < 0) count = 0;
    if ((uint64_t)count > max_entries) count = (int)max_entries;

    if (count > 0) {
        if (copy_to_user((void*)out_buf, entries, count * sizeof(arp_info_t)) != COPY_SUCCESS)
            return EFAULT;
    }
    return (int64_t)count;
}
```

- [ ] **Step 5: Wire syscalls in dispatcher**

In `kernel/core/syscall/syscall.c`, in `syscall_init()`:
```c
syscall_table[SYS_NET_CONFIG]  = (syscall_handler_t)sys_net_config;
syscall_table[SYS_ROUTE_TABLE] = (syscall_handler_t)sys_route_table;
syscall_table[SYS_ARP_TABLE]   = (syscall_handler_t)sys_arp_table;
```

Add extern declarations for the three new handlers near the existing `sys_net_send` extern.

- [ ] **Step 6: Build and verify**

Run: `bash scripts/quick_build.sh` in WSL
Expected: 84 compiled, 0 failed

- [ ] **Step 7: Commit**

```bash
git add kernel/include/syscall.h kernel/net/netsyscall.c kernel/core/syscall/syscall.c
git commit -m "net: add SYS_NET_CONFIG, SYS_ROUTE_TABLE, SYS_ARP_TABLE syscalls"
```

---

### Task 5: Add `route_get_table` and `net_get_arp_table` kernel helpers

**Files:**
- Modify: `kernel/net/route.c`
- Modify: `kernel/net/net.c`

- [ ] **Step 1: Add `route_get_table()` to route.c**

```c
int route_get_table(route_info_t* out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < ROUTE_MAX && n < max; i++) {
        if (!g_rtable.routes[i].valid) continue;
        out[n].dest     = g_rtable.routes[i].dest;
        out[n].mask     = g_rtable.routes[i].mask;
        out[n].gateway  = g_rtable.routes[i].gateway;
        out[n].iface_ip = g_rtable.routes[i].iface;
        out[n].valid    = 1;
        out[n]._pad[0] = out[n]._pad[1] = out[n]._pad[2] = 0;
        n++;
    }
    return n;
}
```

Add `#include "../include/netif.h"` at the top of route.c for the `route_info_t` type.

- [ ] **Step 2: Add `net_get_arp_table()` to net.c**

```c
int net_get_arp_table(arp_info_t* out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < ARP_CACHE_SIZE && n < max; i++) {
        if (!net.arp[i].valid) continue;
        out[n].ip    = net.arp[i].ip;
        memcpy(out[n].mac, net.arp[i].mac, ETH_ALEN);
        out[n].valid = 1;
        out[n]._pad  = 0;
        n++;
    }
    return n;
}
```

- [ ] **Step 3: Build and verify**

Run: `bash scripts/quick_build.sh` in WSL

- [ ] **Step 4: Commit**

```bash
git add kernel/net/route.c kernel/net/net.c
git commit -m "net: add route_get_table and net_get_arp_table kernel helpers"
```

---

### Task 6: Update DHCP client to apply leases

**Files:**
- Modify: `userspace/apps/dhcpc/dhcpc.c`

- [ ] **Step 1: Add SYS_NET_CONFIG call after successful lease**

After the `dhcp_acquire()` success path (line 161), before the print block, add:

```c
/* Apply the lease to the kernel interface. */
{
    typedef struct {
        char     ifname[8];
        unsigned int ip, netmask, gateway, dns, flags;
    } net_cfg_t;

    net_cfg_t cfg;
    /* Zero the struct. */
    unsigned char *cp = (unsigned char *)&cfg;
    for (unsigned long ci = 0; ci < sizeof(cfg); ci++) cp[ci] = 0;

    cfg.ifname[0] = 'e'; cfg.ifname[1] = 't'; cfg.ifname[2] = 'h'; cfg.ifname[3] = '0';
    cfg.ip      = lease.ip;
    cfg.netmask = lease.netmask;
    cfg.gateway = lease.gateway;
    cfg.dns     = lease.dns;

    #define SYS_NET_CONFIG 89
    long cfgr = sc(SYS_NET_CONFIG, (long)&cfg, 0, 0, 0, 0, 0);
    if (cfgr == 0) {
        print("dhcpc: lease applied to eth0\n");
    } else {
        print("dhcpc: WARNING: failed to apply lease\n");
    }
}
```

- [ ] **Step 2: Build and verify**

Run: `bash scripts/quick_build.sh` in WSL

- [ ] **Step 3: Commit**

```bash
git add userspace/apps/dhcpc/dhcpc.c
git commit -m "dhcpc: apply DHCP lease via SYS_NET_CONFIG"
```

---

### Task 7: Build `netif` CLI tool

**Files:**
- Create: `userspace/apps/netif/netif.c`

- [ ] **Step 1: Write the netif tool**

Freestanding app that calls `SYS_NET_INFO` for each interface index (0..3) and prints results.

```c
/* netif.c -- list network interfaces (freestanding, ring 3). */

#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_NET_INFO  59

static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

typedef struct {
    unsigned char  mac[6];
    unsigned char  _pad[2];
    unsigned int   ip, netmask, gateway, dns;
    char           ifname[8];
    unsigned char  up, dhcp, _res[2];
    unsigned long  tx_packets, rx_packets, tx_bytes, rx_bytes;
} net_info_ext_t;

static void print(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0, 0);
}

static void print_dec(unsigned long v) {
    char buf[20]; int i = 0;
    if (v == 0) { print("0"); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    char out[20];
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    out[i] = '\0';
    print(out);
}

static void print_ip(unsigned int ip) {
    print_dec((ip >> 24) & 0xFF); print(".");
    print_dec((ip >> 16) & 0xFF); print(".");
    print_dec((ip >>  8) & 0xFF); print(".");
    print_dec(ip & 0xFF);
}

static void print_mac(const unsigned char *m) {
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char c[3] = { hex[m[i] >> 4], hex[m[i] & 0xF], '\0' };
        print(c);
        if (i < 5) print(":");
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    print("Interface  MAC                IP              Gateway         Status\n");
    print("---------- -----------------  --------------- --------------- ------\n");

    for (int i = 0; i < 4; i++) {
        net_info_ext_t info;
        unsigned char *p = (unsigned char*)&info;
        for (unsigned long j = 0; j < sizeof(info); j++) p[j] = 0;

        long r = sc(SYS_NET_INFO, (long)&info, (long)i, 0, 0, 0, 0);
        if (r != 0) continue;

        print(info.ifname);
        print("       ");
        print_mac(info.mac);
        print("  ");
        print_ip(info.ip);
        print("  ");
        print_ip(info.gateway);
        print("  ");
        print(info.up ? "UP" : "DOWN");
        print("\n");

        print("  TX: ");
        print_dec(info.tx_packets);
        print(" pkts, ");
        print_dec(info.tx_bytes);
        print(" bytes  RX: ");
        print_dec(info.rx_packets);
        print(" pkts, ");
        print_dec(info.rx_bytes);
        print(" bytes\n");
    }
    return 0;
}
```

- [ ] **Step 2: Add to build system and initrd**

Add the compile+link commands to the build script following the pattern of existing apps (dhcpc, ping, etc.).

- [ ] **Step 3: Commit**

```bash
git add userspace/apps/netif/netif.c
git commit -m "net: add netif CLI tool for listing interfaces"
```

---

### Task 8: Build `tcpconnect` CLI tool

**Files:**
- Create: `userspace/apps/tcpconnect/tcpconnect.c`

- [ ] **Step 1: Write the tcpconnect tool**

Freestanding app that takes a host IP and port, creates a TCP socket, connects, and reports success/failure. Uses DNS if the host isn't a dotted-quad.

```c
/* tcpconnect.c -- test TCP connectivity (freestanding, ring 3). */

#define SYS_EXIT    0
#define SYS_WRITE   3
#define SYS_SOCKET  51
#define SYS_CONNECT 52
#define SYS_CLOSE_SK 55

static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

static void print(const char *s) {
    unsigned long n = 0; while (s[n]) n++;
    sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0, 0);
}

static void print_dec(unsigned int v) {
    char buf[12]; int i = 0;
    if (!v) { print("0"); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { char c[2] = { buf[--i], '\0' }; print(c); }
}

static unsigned int parse_ip(const char *s) {
    unsigned int parts[4] = {0};
    int p = 0;
    for (int i = 0; s[i] && p < 4; i++) {
        if (s[i] == '.') { p++; continue; }
        if (s[i] >= '0' && s[i] <= '9')
            parts[p] = parts[p] * 10 + (s[i] - '0');
    }
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

static unsigned int parse_port(const char *s) {
    unsigned int v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i] - '0');
    }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        print("usage: tcpconnect <ip> <port>\n");
        return 1;
    }

    unsigned int ip   = parse_ip(argv[1]);
    unsigned int port = parse_port(argv[2]);

    if (ip == 0 || port == 0 || port > 65535) {
        print("tcpconnect: invalid ip or port\n");
        return 1;
    }

    /* SOCK_STREAM = 1 */
    long fd = sc(SYS_SOCKET, 1, 0, 0, 0, 0, 0);
    if (fd < 0) {
        print("tcpconnect: socket failed\n");
        return 1;
    }

    long r = sc(SYS_CONNECT, fd, (long)ip, (long)port, 0, 0, 0);
    if (r == 0) {
        print("connected to ");
        print_dec((ip >> 24) & 0xFF); print(".");
        print_dec((ip >> 16) & 0xFF); print(".");
        print_dec((ip >>  8) & 0xFF); print(".");
        print_dec(ip & 0xFF);
        print(":"); print_dec(port); print("\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0, 0);
        return 0;
    } else {
        print("tcpconnect: connection failed (");
        print_dec((unsigned int)(-r));
        print(")\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0, 0);
        return 1;
    }
}
```

- [ ] **Step 2: Add to build and commit**

```bash
git add userspace/apps/tcpconnect/tcpconnect.c
git commit -m "net: add tcpconnect CLI tool for TCP connectivity testing"
```

---

### Task 9: Build `route` CLI tool

**Files:**
- Create: `userspace/apps/route/route.c`

- [ ] **Step 1: Write the route tool**

Freestanding app that calls `SYS_ROUTE_TABLE` and prints the routing table.

```c
/* route.c -- show routing table (freestanding, ring 3). */

#define SYS_EXIT        0
#define SYS_WRITE       3
#define SYS_ROUTE_TABLE 90

static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

typedef struct {
    unsigned int dest, mask, gateway, iface_ip;
    unsigned char valid, _pad[3];
} route_info_t;

static void print(const char *s) {
    unsigned long n = 0; while (s[n]) n++;
    sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0, 0);
}

static void print_ip(unsigned int ip) {
    char buf[16]; int pos = 0;
    for (int b = 3; b >= 0; b--) {
        unsigned int octet = (ip >> (b * 8)) & 0xFF;
        if (octet >= 100) buf[pos++] = '0' + octet / 100;
        if (octet >= 10)  buf[pos++] = '0' + (octet / 10) % 10;
        buf[pos++] = '0' + octet % 10;
        if (b > 0) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    print(buf);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    route_info_t entries[16];
    unsigned char *p = (unsigned char*)entries;
    for (unsigned long i = 0; i < sizeof(entries); i++) p[i] = 0;

    long count = sc(SYS_ROUTE_TABLE, (long)entries, 16, 0, 0, 0, 0);
    if (count <= 0) {
        print("routing table empty\n");
        return 0;
    }

    print("Destination     Netmask         Gateway         Interface\n");
    print("--------------- --------------- --------------- ---------------\n");

    for (long i = 0; i < count; i++) {
        print_ip(entries[i].dest);    print("  ");
        print_ip(entries[i].mask);    print("  ");
        print_ip(entries[i].gateway); print("  ");
        print_ip(entries[i].iface_ip); print("\n");
    }
    return 0;
}
```

- [ ] **Step 2: Add to build and commit**

```bash
git add userspace/apps/route/route.c
git commit -m "net: add route CLI tool for showing routing table"
```

---

### Task 10: Verify end-to-end and final build

**Files:**
- Modify: `scripts/quick_build.sh` (add all new source files to compile list)

- [ ] **Step 1: Add all new files to build script**

In `scripts/quick_build.sh`, add compile lines for:
- `kernel/net/netif.c`
- All new userspace apps (netif, tcpconnect, route) — these get compiled, linked with crt0.o, and added to the initrd

- [ ] **Step 2: Full build**

Run: `bash scripts/quick_build.sh` in WSL
Expected: All compiled, 0 failed, link OK

- [ ] **Step 3: QEMU smoke test plan**

Boot with: `qemu-system-x86_64 -cdrom build/os.iso -device e1000,netdev=net0 -netdev user,id=net0`

Test sequence:
1. `netif` → shows eth0 with 10.0.2.15, gateway 10.0.2.2, UP
2. `dhcpc run` → prints lease, says "lease applied to eth0"
3. `netif` → shows eth0 with DHCP-assigned IP (likely still 10.0.2.15 for QEMU)
4. `ping 10.0.2.2` → replies
5. `dig google.com` → returns IP
6. `tcpconnect 93.184.216.34 80` → "connected" (example.com's IP)
7. `httpget http://example.com/` → prints HTML
8. `route` → shows default route via 10.0.2.2
9. `arp` → shows gateway MAC

- [ ] **Step 4: Final commit**

```bash
git add -A
git commit -m "net: Phase 1+2 complete — netif abstraction, DHCP lease application, CLI diagnostics"
```
