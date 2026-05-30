/*
 * dns.h -- Minimal freestanding DNS resolver (userspace, ring 3).
 * ===============================================================
 *
 * A tiny, dependency-free DNS A-record resolver for AutomationOS userspace.
 * NO libc, NO stdio, NO malloc, NO standard headers -- everything is built
 * on inline syscalls (SYS_SOCKET / SYS_SENDTO / SYS_SOCK_POLL / SYS_RECVFROM
 * / SYS_CLOSE_SK) plus a handful of static helpers and fixed-size buffers.
 *
 * In the QEMU user-net (slirp) environment the DNS server lives at
 * 10.0.2.3:53 and answers standard DNS-over-UDP queries; slirp NATs them out
 * to the host's real resolver.
 *
 * Build (flags passed DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dns.c -o dns.o
 *   objdump -d dns.o | grep fs:0x28   # MUST be empty (no stack canary)
 */

#ifndef AUTOMATIONOS_DNS_H
#define AUTOMATIONOS_DNS_H

/*
 * dns_resolve -- resolve `hostname` to an IPv4 address in HOST byte order
 * (e.g. 0x0A000202 for 10.0.2.2).
 *
 *   - If `hostname` is already a dotted quad "a.b.c.d", it is parsed directly
 *     with NO network query.
 *   - Otherwise a DNS A-record query (UDP) is sent to the configured resolver
 *     (default 10.0.2.3:53; override with dns_set_server()).  The reply is
 *     scanned across ALL answer records: CNAME records (TYPE=5) are skipped
 *     and the first A/IN record (the address of the canonical name, normally
 *     present in the same packet) is returned. Name compression (0xC0) is
 *     handled wherever names are skipped.  Non-NOERROR rcodes (NXDOMAIN etc.)
 *     are detected in the header and cause an immediate DNS_ERR_PARSE return.
 *   - Successful network lookups are stored in a small in-memory cache so a
 *     repeat resolve of the same name returns immediately with no query. The
 *     send is retried a few times if no reply arrives within the poll bound.
 *
 * On success returns 0 and writes the address to *out_ip.
 * On failure / timeout returns a negative value. The implementation is fully
 * bounded and will not hang.
 */
int dns_resolve(const char *hostname, unsigned int *out_ip);

/*
 * dns_resolve_all -- like dns_resolve but returns every A record found in the
 * response (up to `max`).
 *
 *   - `host`  : hostname or dotted quad (same rules as dns_resolve).
 *   - `ips`   : caller-provided array, filled with up to `max` addresses in
 *               HOST byte order.
 *   - `max`   : capacity of `ips` (must be >= 1).
 *   - `count` : optional; if non-NULL receives the number of addresses written.
 *
 * Behaves identically to dns_resolve for the cache, CNAME-chain following,
 * retry and compression handling. A dotted-quad or a cache hit yields exactly
 * one address. On success returns 0 (at least one address written); on
 * failure / timeout returns a negative value and writes 0 to *count.
 */
int dns_resolve_all(const char *host, unsigned int *ips, int max, int *count);

/*
 * dns_reverse -- perform a PTR query for the IPv4 address `ip` (host byte
 * order) and write the resulting hostname into `out_name` (at most
 * `out_cap - 1` bytes plus a NUL terminator).
 *
 * The query QNAME is `d.c.b.a.in-addr.arpa` where a.b.c.d is the dotted-quad
 * form of `ip`.
 *
 *   - `out_name` must point to a caller-supplied buffer of at least `out_cap`
 *     bytes.
 *   - On success writes the PTR name (NUL-terminated) and returns the length
 *     of the name (>= 1, not counting the NUL).
 *   - On failure (timeout, NXDOMAIN, truncation, bad argument) returns a
 *     negative DNS_ERR_* value; `out_name` is untouched.
 *   - Best-effort: the first PTR record in the answer section is returned.
 *
 * Note: PTR records are stored in the cache with the synthetic arpa name as
 * the key; the returned string is the RDATA name decoded from the wire.
 * No live PTR result is added to the A-record cache.
 */
int dns_reverse(unsigned int ip, char *out_name, int out_cap);

/*
 * dns_set_server -- override the resolver IP address (host byte order).
 *
 * The new server is used for all subsequent queries.  Pass the DHCP-obtained
 * resolver here when running on a real LAN instead of QEMU slirp.
 *
 * Thread-safety: not thread-safe (single-threaded kernel userspace context).
 *
 * Example:
 *   dns_set_server(0x08080808u);  // use 8.8.8.8
 */
void dns_set_server(unsigned int ip_host_order);

/*
 * dns_get_server -- return the currently configured resolver IP (host order).
 *
 * Returns 0x0A000203 (10.0.2.3) until dns_set_server() has been called.
 */
unsigned int dns_get_server(void);

/*
 * dns_clear_cache -- flush every entry from the in-memory resolver cache.
 *
 * All subsequent lookups will issue fresh network queries.  Useful as a
 * diagnostic or after a network reconfiguration event (e.g. DHCP renew).
 */
void dns_clear_cache(void);

/*
 * struct dns_stats -- counters filled by dns_stats().
 *
 * Fields:
 *   queries    -- total number of DNS network queries sent (each retry
 *                 within a single dns_resolve call counts as one query).
 *   cache_hits -- number of lookups satisfied from the in-memory cache
 *                 (includes TTL-fresh hits; TTL-expired entries are NOT
 *                 counted as cache hits).
 *   retries    -- number of per-attempt retries (i.e. every additional
 *                 send beyond the first for a given dns_resolve call).
 *   timeouts   -- number of complete dns_resolve / dns_resolve_all calls
 *                 that returned DNS_ERR_TIMEO or DNS_ERR_TRUNC because no
 *                 valid reply arrived within the bounded retry window.
 */
struct dns_stats {
    unsigned int queries;
    unsigned int cache_hits;
    unsigned int retries;
    unsigned int timeouts;
};

/*
 * dns_stats -- copy the current counter values into the caller-supplied
 * struct.  `s` must be non-NULL.  The counters are never reset
 * automatically; call dns_clear_cache() for cache state, and note that
 * the counters themselves have no separate reset API (they are diagnostic
 * only and wrap naturally at UINT_MAX).
 */
void dns_stats(struct dns_stats *s);

/*
 * dns_selftest -- run built-in sanity checks (no live network required).
 *
 * Tests:
 *   1. Dotted-quad fast path: "10.0.2.2" -> 0x0A000202, no query issued.
 *   2. dns_set_server / dns_get_server round-trip.
 *   3. A hand-crafted DNS response that contains an AAAA (TYPE=28) record
 *      BEFORE the A record: verifies the AAAA is skipped and the correct
 *      IPv4 address is extracted.
 *   4. rcode != 0 (NXDOMAIN) response is rejected (returns DNS_ERR_PARSE).
 *   5. dns_resolve_all dotted-quad: returns exactly 1 address and count=1.
 *   6. Cache hit increments the cache_hits counter.
 *   7. TTL eviction: entry inserted with TTL=0 is treated as expired and
 *      NOT returned from the cache (requires DNS_CACHE_TTL_MS_TEST == 0,
 *      set only when -DDNS_TEST_TTL_ZERO is defined -- self-test documents
 *      this path).
 *   8. dns_clear_cache empties the cache (subsequent lookup misses).
 *
 * Returns 0 on pass, -1 on any failure.
 */
int dns_selftest(void);

#endif /* AUTOMATIONOS_DNS_H */
