/*
 * dnstest.c -- DNS-LEASE-0 proof: the resolver must follow the DHCP-learned DNS server.
 * =====================================================================================
 * Freestanding ring 3, linked with crt0 + userspace/lib/net/{dns,dhcp}.c.
 *
 * dns.c used to query a hard-wired 10.0.2.3 (QEMU slirp's DNS) no matter what DHCP handed out,
 * and nothing ever called dns_set_server() (dhcpc's own comment even assumes "the tools each
 * call dns_set_server themselves" -- none do). On any real LAN -- the T410's wired port after
 * `nicup` + `dhcpc`, or WiFi -- every name lookup therefore went to a dead address.
 *
 * This probe does exactly what the T410 does: run a REAL DHCP exchange, apply the lease through
 * SYS_NET_CONFIG (what dhcpc does), then ask the resolver which server it will use:
 *
 *   DNSTEST: dhcp lease ip=10.0.2.15 dns=10.0.2.4
 *   DNSTEST: lease=10.0.2.4 using=10.0.2.4
 *   DNSTEST: PASS (resolver follows the DHCP lease)
 *   DNSTEST: resolve example.com -> 93.184.x.x          (informational; needs the host's Internet)
 *
 * The verdict prints BEFORE the lookup, so it never depends on a timeout. It is only meaningful
 * when the lease differs from the old hard-wired value, so build_test/dns_lease_check.sh boots QEMU
 * with `-netdev user,dns=10.0.2.4`. Spawned by init only in -DDNS_TEST builds.
 */
#include "../../lib/net/dns.h"
#include "../../lib/net/dhcp.h"

#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_SLEEP         9     /* kernel/include/syscall.h (the uapi header's 10 is STALE) */
#define SYS_NET_CONFIG    89

typedef unsigned long size_t;

static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void out_u(unsigned v) {
    char b[12]; int i = 11; b[i] = 0; if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    out(&b[i]);
}
static void out_ip(unsigned ip) {
    out_u(ip >> 24); out("."); out_u((ip >> 16) & 255); out("."); out_u((ip >> 8) & 255); out("."); out_u(ip & 255);
}

int main(void) {
    sc3(SYS_SLEEP, 2500, 0, 0);                       /* let boot-time autodhcp/net settle first */

    dhcp_lease_t lease;
    { unsigned char* p = (unsigned char*)&lease; for (size_t i = 0; i < sizeof(lease); i++) p[i] = 0; }
    int drc = dhcp_acquire(&lease);
    if (drc != DHCP_OK) {
        out("DNSTEST: FAIL (no DHCP lease -- cannot judge the resolver) dhcp_rc="); out_u((unsigned)(-drc));
        /* firewall counters: did the kernel packet filter eat the DHCP exchange? (uapi_fw_req_t layout) */
        struct { unsigned op; int index; unsigned arg, pad; unsigned char rule[32]; unsigned long long hits;
                 unsigned en, rules, ct; unsigned char pi, po, ap, pd;
                 unsigned long long in_acc, in_drop, out_acc, out_drop, ct_hits, ct_new, ct_ev, bad; } q;
        { unsigned char* b = (unsigned char*)&q; for (size_t i = 0; i < sizeof(q); i++) b[i] = 0; }
        if (sc3(136, (long)&q, 0, 0) == 0) {
            out(" fw: out_accept="); out_u((unsigned)q.out_acc); out(" out_drop="); out_u((unsigned)q.out_drop);
            out(" in_accept="); out_u((unsigned)q.in_acc); out(" in_drop="); out_u((unsigned)q.in_drop);
        }
        out("\n"); sc3(SYS_EXIT, 2, 0, 0);
    }
    out("DNSTEST: dhcp lease ip="); out_ip(lease.ip); out(" dns="); out_ip(lease.dns); out("\n");

    /* Apply it the way dhcpc does (uapi_net_config_t: ifname[16], ip, mask, gw, dns, flags = 36 bytes). */
    struct { char ifname[16]; unsigned ip, netmask, gateway, dns, flags; } cfg;
    { unsigned char* p = (unsigned char*)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.ifname[0] = 'e'; cfg.ifname[1] = 't'; cfg.ifname[2] = 'h'; cfg.ifname[3] = '0';
    cfg.ip = lease.ip; cfg.netmask = lease.netmask; cfg.gateway = lease.gateway; cfg.dns = lease.dns;
    if (sc3(SYS_NET_CONFIG, (long)&cfg, 0, 0) != 0) { out("DNSTEST: FAIL (could not apply the lease)\n"); sc3(SYS_EXIT, 2, 0, 0); }

    unsigned using_ = dns_get_server();               /* the server the resolver WILL use */
    out("DNSTEST: lease="); out_ip(lease.dns); out(" using="); out_ip(using_); out("\n");
    int pass = (lease.dns != 0 && using_ == lease.dns);
    out(pass ? "DNSTEST: PASS (resolver follows the DHCP lease)\n" : "DNSTEST: FAIL (resolver ignores the DHCP lease)\n");

    unsigned ip = 0;
    int r = dns_resolve("example.com", &ip);          /* informational: real lookup through that server */
    out("DNSTEST: resolve example.com -> ");
    if (r == 0) out_ip(ip); else { out("FAILED rc="); out_u((unsigned)(-r)); }
    out("\n");

    sc3(SYS_EXIT, pass ? 0 : 1, 0, 0);
    for (;;) sc3(SYS_YIELD, 0, 0, 0);
}
