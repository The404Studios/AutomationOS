/*
 * dhcp.h -- Freestanding DHCP client (userspace, ring 3) for AutomationOS.
 * =======================================================================
 *
 * A tiny, dependency-free DHCP (RFC 2131 / BOOTP) client that performs a full
 * DISCOVER -> OFFER -> REQUEST -> ACK exchange over UDP and returns the lease
 * it was granted (offered IP, netmask, router/gateway, DNS, server id, lease
 * duration).  Its purpose is to let the OS obtain an address on a *real*
 * network instead of relying on the hardcoded QEMU/slirp static IP 10.0.2.15.
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.  Everything is built on
 * inline syscalls plus a handful of static helpers and fixed-size buffers, the
 * same way userspace/lib/net/dns.c is.
 *
 * --- Transport notes (how the OFFER/ACK reach us) ------------------------
 * DHCP normally is client UDP port 68 -> server UDP port 67 (broadcast), and
 * the server replies to the client's port 68.  This kernel DOES expose a
 * SYS_BIND syscall (76), so dhcp.c binds the socket to the DHCP client port
 * (68) before sending; the implementation then:
 *   - opens a SOCK_DGRAM socket and SYS_BIND()s it to client port 68,
 *   - SENDTO the limited-broadcast address 255.255.255.255:67, and
 *   - sets the BOOTP `flags` BROADCAST bit (0x8000) so the server broadcasts
 *     its reply rather than unicasting to an (unassigned) client IP.
 * The kernel UDP layer demultiplexes the inbound datagram to the socket whose
 * local port matches, so RECVFROM on our socket sees the OFFER/ACK.
 *
 * A raw-frame fallback (hand-built Ethernet+IP+UDP via SYS_NET_SEND/RECV) is
 * provided behind #define DHCP_USE_RAW_FALLBACK, but the socket path is the
 * default and preferred route.
 *
 * --- Integrator note (OUT OF SCOPE here) ---------------------------------
 * This library only *obtains* a lease.  Actually applying it -- i.e. replacing
 * the static net.ip / net.gateway / net.netmask / DNS inside the kernel with
 * the values in dhcp_lease_t -- requires a NEW kernel hook (e.g. a
 * SYS_NET_SET_CONFIG syscall or a sys_net_info write path).  No such hook
 * exists yet; wiring it is a separate, kernel-side task.  Do not expect the
 * machine's address to change just from calling dhcp_acquire().
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dhcp.c -o dhcp.o
 *   objdump -d dhcp.o | grep fs:0x28   # MUST be empty (no stack canary)
 */

#ifndef AUTOMATIONOS_DHCP_H
#define AUTOMATIONOS_DHCP_H

/*
 * dhcp_lease_t -- the configuration granted by the DHCP server.
 *
 * ALL address fields are in HOST byte order (u32 = 0xAABBCCDD for A.B.C.D),
 * matching net.h / dns.h conventions.  A field is 0 if the server did not
 * supply that option.
 */
typedef struct {
    unsigned int ip;          /* our offered/assigned IPv4 (yiaddr)        */
    unsigned int netmask;     /* subnet mask         (option 1)            */
    unsigned int gateway;     /* default router      (option 3, first)     */
    unsigned int dns;         /* DNS server          (option 6, first)     */
    unsigned int server;      /* DHCP server id      (option 54)           */
    unsigned int lease_secs;  /* lease duration sec  (option 51)           */
} dhcp_lease_t;

/*
 * dhcp_acquire -- run a full DHCP handshake and fill *out with the lease.
 *
 * Performs DISCOVER -> (wait OFFER) -> REQUEST -> (wait ACK).  The whole thing
 * is bounded: each receive wait is a fixed poll/yield loop and the DISCOVER is
 * retried a small, fixed number of times.  It can never hang the OS.
 *
 * Returns 0 on success (and *out is fully populated, IPs in host order), or a
 * negative DHCP_E* code on timeout / NAK / malformed reply / socket failure.
 */
int dhcp_acquire(dhcp_lease_t* out);

/*
 * dhcp_selftest -- offline structural self test (NO live DHCP server needed).
 *
 *   1. Builds a DHCPDISCOVER packet into a buffer and verifies:
 *        - op == 1 (BOOTREQUEST), htype == 1, hlen == 6
 *        - the 4-byte magic cookie 0x63825363 is present at the right offset
 *        - DHCP option 53 (message type) is present and == 1 (DISCOVER)
 *   2. Parses a hardcoded sample DHCPACK byte array and verifies the option
 *      parser extracts the expected yiaddr, router (option 3), DNS (option 6),
 *      server id (option 54) and lease time (option 51).
 *
 * Returns 0 if every check passes, a negative DHCP_E* / -100.. code otherwise.
 */
int dhcp_selftest(void);

/* ---- error / status codes (negative on failure) ------------------------- */
#define DHCP_OK          0
#define DHCP_E_INVAL   (-22)   /* bad argument                              */
#define DHCP_E_SOCK     (-1)   /* could not create socket                   */
#define DHCP_E_SEND     (-2)   /* sendto failed                            */
#define DHCP_E_TIMEO  (-110)   /* no usable reply within the bound          */
#define DHCP_E_NAK      (-3)   /* server sent DHCPNAK                       */
#define DHCP_E_PARSE    (-4)   /* malformed / unexpected reply              */
#define DHCP_E_NOMAC    (-5)   /* could not read our MAC (SYS_NET_INFO)     */

#endif /* AUTOMATIONOS_DHCP_H */
