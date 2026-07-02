# Networking Phase 1+2: Stable Wired Internet + CLI Diagnostics

**Status:** Design spec
**Date:** 2026-06-06
**Goal:** Make QEMU wired internet reliable and verifiable with CLI tools, establishing the foundation for TLS, WiFi, and AP mode.

## Context

The kernel has a working e1000 driver, TCP/IP/UDP/ARP/ICMP stack, and socket syscalls. However:
- IP address is hardcoded to 10.0.2.15 (QEMU user-net default)
- No way to change IP/gateway/DNS at runtime (DHCP client exists but can't apply leases)
- Network stack is hardcoded to a single e1000 global — no interface abstraction
- CLI tools exist (ping, dig, httpget, arp, dhcpc, nc, traceroute, wget) but some may not work end-to-end
- No `netif list`, `route show`, or `tcpconnect` commands

This spec delivers a testable "internet works" milestone before any WiFi or TLS work begins.

## Architecture

### Network Interface Abstraction (`netif_t`)

Replace the hardcoded `static struct { ... } net;` in `net.c` with a registered interface model.

```c
// kernel/include/netif.h

#define NETIF_NAME_MAX  8
#define NETIF_MAX       4      // eth0, wlan0, lo, ap0

typedef struct netif {
    char     name[NETIF_NAME_MAX];  // "eth0", "wlan0"
    uint8_t  mac[6];
    uint32_t ip;        // host byte order
    uint32_t netmask;   // host byte order
    uint32_t gateway;   // host byte order
    uint32_t dns;       // host byte order
    bool     up;
    bool     dhcp_active;

    // Driver callbacks
    int  (*tx)(struct netif* nif, const void* frame, uint16_t len);
    int  (*rx_poll)(struct netif* nif, void* buf, uint16_t buf_len);
    int  (*get_mac)(struct netif* nif, uint8_t out[6]);

    // Internal
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} netif_t;

// Registration
int      netif_register(netif_t* nif);
netif_t* netif_get(const char* name);       // "eth0"
netif_t* netif_get_default(void);           // first UP interface
int      netif_count(void);
netif_t* netif_get_by_index(int idx);

// Configuration (called by DHCP, CLI, etc.)
int      netif_set_ip(netif_t* nif, uint32_t ip, uint32_t mask);
int      netif_set_gateway(netif_t* nif, uint32_t gw);
int      netif_set_dns(netif_t* nif, uint32_t dns);
int      netif_up(netif_t* nif);
int      netif_down(netif_t* nif);
```

**Migration path:** `net_init()` creates an `eth0` netif backed by e1000 callbacks. All existing `net_send()`/`net_recv()` calls route through `netif_get_default()`. The hardcoded `net.ip`/`net.gateway` globals are replaced by `eth0->ip`/`eth0->gateway`. Zero behavior change for existing code.

### Syscall: SYS_NET_CONFIG (new)

Allows userspace to set IP/gateway/DNS on an interface. Used by `dhcpc` after lease acquisition.

```c
#define SYS_NET_CONFIG  70   // next free syscall number

// Userspace request structure
typedef struct {
    char     ifname[8];      // "eth0"
    uint32_t ip;             // 0 = don't change
    uint32_t netmask;        // 0 = don't change
    uint32_t gateway;        // 0 = don't change
    uint32_t dns;            // 0 = don't change
    uint32_t flags;          // reserved, must be 0
} net_config_req_t;

// Kernel handler
int64_t sys_net_config(uint64_t req_ptr, ...);
```

The handler copies the request via `copy_from_user`, looks up the interface by name, and applies non-zero fields. Returns 0 on success, EFAULT/EINVAL on error.

### Syscall: SYS_NET_INFO (update existing)

Already exists (syscall 59). Update to support querying by interface name instead of returning only the global state. Add interface list enumeration.

```c
// Extended net_info_t
typedef struct {
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    char     ifname[8];
    uint8_t  up;
    uint8_t  dhcp;
    uint8_t  _reserved[2];
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} net_info_t;
```

### DHCP Integration

The DHCP client (`userspace/apps/dhcpc/dhcpc.c`) already implements the full DISCOVER→OFFER→REQUEST→ACK handshake via UDP sockets. Currently it has no way to apply the lease.

**Change:** After `dhcp_acquire()` returns a `dhcp_lease_t`, call `SYS_NET_CONFIG` to apply it:
```c
net_config_req_t cfg = {0};
strncpy(cfg.ifname, "eth0", 7);
cfg.ip      = lease.ip;
cfg.netmask = lease.netmask;
cfg.gateway = lease.gateway;
cfg.dns     = lease.dns;
syscall(SYS_NET_CONFIG, (uint64_t)&cfg, 0, 0, 0, 0, 0);
```

### Route Table

Already exists (`kernel/net/route.c`). The current implementation has a fixed 32-entry table with LPM lookup. No changes needed for Phase 1 — routes are managed internally. Phase 2 adds a `route` CLI tool to inspect it.

### ARP Cache

Already exists in `net.c` with a small static cache. Phase 2's `arp` CLI tool will expose it via `SYS_NET_INFO` extensions or a new `SYS_ARP_TABLE` syscall.

## Phase 1 Deliverables

| # | Deliverable | Files |
|---|-------------|-------|
| 1 | `netif_t` abstraction + `eth0` registration | `kernel/include/netif.h` (new), `kernel/net/netif.c` (new), `kernel/net/net.c` (refactor) |
| 2 | `SYS_NET_CONFIG` syscall | `kernel/net/netsyscall.c`, `kernel/include/syscall.h`, `kernel/core/syscall/syscall.c` |
| 3 | `SYS_NET_INFO` extension for interface query | `kernel/net/netsyscall.c` |
| 4 | DHCP lease application in `dhcpc` | `userspace/apps/dhcpc/dhcpc.c` |
| 5 | `net_send`/`net_recv` route through `netif_get_default()` | `kernel/net/net.c` |
| 6 | Smoke test: boot → `dhcpc eth0` → `ping 10.0.2.2` → `httpget http://example.com/` | Manual QEMU test |

## Phase 2 Deliverables: CLI Diagnostics

Each tool should be a standalone binary in `userspace/apps/` or `userspace/bin/`.

| Command | What it does | Exists? | Work needed |
|---------|-------------|---------|-------------|
| `netif list` | Show all interfaces, IPs, MACs, link state, packet counts | No | New app, calls `SYS_NET_INFO` per interface |
| `route show` | Show routing table entries | No | New app, needs `SYS_ROUTE_TABLE` syscall |
| `arp show` | Show ARP cache entries | Partial (`userspace/apps/arp/`) | May need `SYS_ARP_TABLE` syscall |
| `dhcp eth0` | Run DHCP, apply lease, print result | Yes (`dhcpc`) | Add `SYS_NET_CONFIG` call |
| `ping <host>` | ICMP echo | Yes | Verify it works end-to-end |
| `dns <host>` | DNS A-record lookup | Yes (`dig`) | Verify, maybe rename/alias |
| `tcpconnect <host> <port>` | Raw TCP connect test, print success/fail | No | New app using `SYS_SOCKET`+`SYS_CONNECT` |
| `httpget <url>` | HTTP GET, print response | Yes | Verify end-to-end |

### New syscalls for Phase 2

```c
#define SYS_ROUTE_TABLE  71   // copy route entries to user buffer
#define SYS_ARP_TABLE    72   // copy ARP cache entries to user buffer
```

Each returns an array of fixed-size entries + count. Simple `copy_to_user` of kernel-side tables.

## Testing Plan

All tests run under QEMU with `-device e1000,netdev=net0 -netdev user,id=net0`.

1. **Boot with DHCP instead of hardcoded IP:**
   - `dhcpc eth0` prints assigned IP
   - `netif list` shows eth0 with the DHCP IP, not 10.0.2.15
   - Keep 10.0.2.15 as fallback if DHCP fails (QEMU user-net always assigns this)

2. **Connectivity chain:**
   - `ping 10.0.2.2` → replies
   - `dns google.com` → returns IP
   - `tcpconnect example.com 80` → "connected"
   - `httpget http://example.com/` → prints HTML

3. **Diagnostics:**
   - `netif list` → shows eth0 with MAC, IP, packet counts
   - `route show` → shows default route via 10.0.2.2
   - `arp show` → shows gateway MAC

## What This Does NOT Include

- WiFi (Phase 4)
- TLS/HTTPS (Phase 3)
- AP mode / NAT (Phases 7-9)
- Multiple simultaneous interfaces (only one active at a time for now)
- IPv6
- VLAN tagging
- Firewall rules

## Migration Risk

Low. The `netif_t` abstraction wraps the existing globals without changing behavior. The e1000 driver, TCP/IP stack, and socket layer are untouched. The only new kernel code is:
- `netif.c` (~150 lines): interface registry
- `netsyscall.c` additions (~100 lines): SYS_NET_CONFIG, SYS_ROUTE_TABLE, SYS_ARP_TABLE
- `net.c` refactor (~50 lines changed): replace `net.*` globals with `netif_get_default()->*`
- `dhcpc.c` update (~10 lines): call SYS_NET_CONFIG after lease

Total: ~300 lines new kernel code, ~50 lines refactored, ~100 lines new userspace.
