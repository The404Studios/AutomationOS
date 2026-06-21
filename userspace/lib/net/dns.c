/*
 * dns.c -- Minimal freestanding DNS A-record resolver (userspace, ring 3).
 * ========================================================================
 *
 * Implements dns_resolve() / dns_resolve_all() / dns_reverse() over the
 * AutomationOS socket syscalls.  The DNS stack is poll-mode: after sending a
 * query we must repeatedly pump the NIC via SYS_SOCK_POLL before SYS_RECVFROM
 * will see the reply.  Every wait loop is bounded by a fixed iteration cap
 * with a SYS_YIELD between attempts so an unresponsive server can never hang.
 *
 * Features:
 *   - A fixed-size in-memory cache (64 entries, LRU eviction with TTL).
 *     Each entry records a coarse insertion timestamp (SYS_GET_TICKS_MS,
 *     syscall 40) and is evicted on next lookup when older than
 *     DNS_CACHE_TTL_MS (default 60 000 ms).  Short TTL is intentional: the
 *     OS reboots fresh so aggressive caching of stale records is unhelpful.
 *   - The full answer section is scanned so CNAME (TYPE=5) records before the
 *     A record are followed within the same packet; DNS name compression
 *     (0xC0 pointers) is handled everywhere a name is skipped.
 *   - Multiple A records are supported (dns_resolve returns the first;
 *     dns_resolve_all returns up to `max`).
 *   - AAAA (TYPE=28), NS, MX, TXT and all other non-A RRs are correctly
 *     skipped by RDLENGTH; the rcode and opcode are validated before parsing.
 *   - The answer iteration is capped independently of the wire ancount to
 *     prevent pathological/crafted responses from running off the end.
 *   - If the TC (truncation) bit is set the current partial reply is used
 *     as-is if at least one A record was extracted (best-effort); otherwise
 *     the attempt is treated as a miss and the retry loop continues with a
 *     fresh query.
 *   - EDNS0: each query carries one OPT pseudo-RR in the Additional section
 *     advertising a 1232-byte UDP payload (recommended minimum for DNSSEC /
 *     real resolvers per RFC 6891 + DNS flag day 2020).  ARCOUNT is set to 1.
 *   - The query send is retried a few times (fresh transaction id each time)
 *     if no reply arrives within the per-attempt poll bound.
 *   - Configurable resolver: dns_set_server() / dns_get_server() let a
 *     DHCP client substitute the real LAN resolver for 10.0.2.3.
 *   - dns_reverse(): PTR lookup for `d.c.b.a.in-addr.arpa`, decodes the
 *     first PTR RDATA name back into a printable string.
 *   - dns_clear_cache(): flushes all cached entries (diagnostic).
 *   - dns_stats(): fills a caller-supplied struct dns_stats with cumulative
 *     counters for queries, cache_hits, retries, timeouts.
 *
 * No libc: all string / byte handling is done with the small static helpers
 * below, and every scratch buffer is a fixed-size local array.
 *
 * Build (flags passed DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/dns.c -o dns.o
 *
 * For the TTL-zero eviction self-test path compile with:
 *   -DDNS_TEST_TTL_ZERO
 * This forces DNS_CACHE_TTL_MS to 0 so every newly inserted entry is
 * immediately expired on the next lookup, exercising the eviction branch
 * without needing wall-clock time to pass.  DO NOT define this for
 * production builds.
 */

#include "dns.h"

/* ---- syscall numbers ---------------------------------------------------- */
#define SYS_YIELD           15
#define SYS_SOCKET          51
#define SYS_CONNECT         52
#define SYS_SEND            53
#define SYS_RECV            54
#define SYS_CLOSE_SK        55
#define SYS_SENDTO          56
#define SYS_RECVFROM        57
#define SYS_SOCK_POLL       58
#define SYS_GET_TICKS_MS    40  /* returns current time in milliseconds */

/* ---- socket types ------------------------------------------------------- */
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

/* ---- environment constants ---------------------------------------------- */
/* Default resolver: 10.0.2.3 (QEMU slirp DNS gateway), host byte order. */
#define DNS_DEFAULT_SERVER  0x0A000203u
#define DNS_PORT            53

/* ---- error codes -------------------------------------------------------- */
#define DNS_OK           0
#define DNS_ERR_INVAL  (-22)   /* bad argument                              */
#define DNS_ERR_SOCK   (-1)    /* could not create socket                   */
#define DNS_ERR_SEND   (-2)    /* sendto failed                             */
#define DNS_ERR_TIMEO  (-110)  /* no/invalid reply within the bound         */
#define DNS_ERR_PARSE  (-3)    /* malformed response / no A record          */
#define DNS_ERR_TRUNC  (-4)    /* TC bit set and no usable A record found   */

/* EAGAIN as returned by the kernel (would-block, keep polling). */
#define EAGAIN_NEG     (-11)

/* Bound for any receive wait loop (see header notes: ~200000 + yield).
 * This is the per-attempt cap; with retries the total stays bounded. */
#define DNS_POLL_MAX    200000

/* Number of times the query is (re-)sent before giving up. Each attempt uses
 * a fresh transaction id and its own bounded poll window. */
#define DNS_MAX_TRIES   3

/* Hard cap on answer-RR iteration regardless of the wire ancount field.
 * Protects against crafted/corrupted packets with a large ancount that would
 * drive the parser off the end of the buffer. */
#define DNS_MAX_ANSWERS 64

/* ---- in-memory resolver cache ------------------------------------------- */
#define DNS_CACHE_SIZE  64     /* bumped from 16: a full modern page may hit
                                  many third-party hosts; 64 avoids thrashing */
#define DNS_NAME_MAX    64     /* max stored name length incl. NUL */

/*
 * Cache TTL in milliseconds.  Default: 60 000 ms (60 s).  Short because the
 * OS reboots fresh; keeping stale entries across long runs is worse than the
 * cost of a fresh query.
 *
 * DNS_TEST_TTL_ZERO: compile-time override for the self-test TTL-eviction
 * path.  When defined, the TTL is forced to 0 so every entry expires
 * immediately on the very next lookup, making the eviction branch testable
 * without real wall-clock time passing.  PRODUCTION builds MUST NOT define
 * this symbol.
 */
#ifdef DNS_TEST_TTL_ZERO
#  define DNS_CACHE_TTL_MS  0u
#else
#  define DNS_CACHE_TTL_MS  60000u
#endif

/* ---- configurable resolver --------------------------------------------- */
static unsigned int g_dns_server = DNS_DEFAULT_SERVER;

void dns_set_server(unsigned int ip_host_order)
{
    g_dns_server = ip_host_order;
}

unsigned int dns_get_server(void)
{
    return g_dns_server;
}

/* ---- statistics counters ------------------------------------------------ */
static struct dns_stats g_stats;   /* zero-initialised by C spec (static) */

void dns_stats(struct dns_stats *s)
{
    if (!s) return;
    s->queries    = g_stats.queries;
    s->cache_hits = g_stats.cache_hits;
    s->retries    = g_stats.retries;
    s->timeouts   = g_stats.timeouts;
}

/* ---- raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8) ---------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* sock_addr_t filled by SYS_RECVFROM out_addr (ip + port in HOST order). */
typedef struct {
    unsigned int   ip;
    unsigned short port;
    unsigned short _pad;
} sock_addr_t;

/*
 * One cache slot.
 *
 *   `used`        : 1 if the slot holds a valid entry.
 *   `seq`         : insertion order counter; larger == more recently used.
 *                   Used to find the LRU victim when all slots are occupied.
 *   `inserted_ms` : coarse wall-clock timestamp at insertion time, obtained
 *                   via SYS_GET_TICKS_MS.  Used for TTL eviction: an entry
 *                   is considered stale when
 *                     (current_ms - inserted_ms) >= DNS_CACHE_TTL_MS.
 *   `name`        : NUL-terminated hostname, stored in lowercase so lookups
 *                   are case-insensitive per RFC 1035.
 *   `ip`          : resolved address in host byte order.
 */
typedef struct {
    char         name[DNS_NAME_MAX];
    unsigned int ip;            /* host byte order */
    unsigned int seq;           /* insertion order; larger == more recent */
    unsigned int inserted_ms;   /* SYS_GET_TICKS_MS at insert time */
    int          used;
} dns_cache_entry_t;

static dns_cache_entry_t g_cache[DNS_CACHE_SIZE];
static unsigned int      g_cache_seq;  /* monotonically increasing stamp */

/* ---- tiny freestanding helpers ----------------------------------------- */

static unsigned int d_strlen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

/* ASCII lowercase for case-insensitive name comparison (DNS is case-insens). */
static char d_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* Case-insensitive equality of NUL-terminated strings. */
static int d_name_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = d_lower(*a);
        char cb = d_lower(*b);
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

/* ---- cache --------------------------------------------------------------
 *
 * Lookup: linear scan over the fixed array for a used, non-expired slot
 * whose name matches `host` case-insensitively.  Expired slots are cleared
 * on encounter (TTL eviction) and treated as a miss.  On a hit the slot's
 * `seq` is bumped so it counts as most-recently-used.
 *
 * Insert: if the name already lives in a slot (including previously expired
 * ones that were cleared), refresh its ip / seq / inserted_ms.  Otherwise
 * fill the first free slot, or (if full) evict the slot with the smallest
 * `seq` -- the least-recently-used / oldest entry.
 *
 * Names longer than DNS_NAME_MAX-1 are not cached; resolution still works,
 * it just skips the cache for that name.
 *
 * All names are stored lowercased to ensure case-insensitive matching per
 * RFC 1035.
 */

/* Return current tick timestamp via SYS_GET_TICKS_MS.  Cast to unsigned int
 * to keep the arithmetic simple; wraps at ~49 days (uint32 ms). */
static unsigned int get_ticks_ms(void)
{
    long t = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    return (unsigned int)(t < 0 ? 0 : t);
}

/* Return 1 if entry `e` is expired relative to `now_ms`. */
static int cache_entry_expired(const dns_cache_entry_t *e, unsigned int now_ms)
{
    /* Unsigned subtraction handles the wrap-around case correctly as long as
     * the actual elapsed time is less than ~49 days, which is always true
     * since the OS reboots fresh. */
    return (now_ms - e->inserted_ms) >= DNS_CACHE_TTL_MS;
}

static int cache_lookup(const char *host, unsigned int *out_ip)
{
    unsigned int now = get_ticks_ms();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;

        /* TTL eviction: clear stale entries on encounter. */
        if (cache_entry_expired(&g_cache[i], now)) {
            g_cache[i].used = 0;
            continue;
        }

        if (d_name_eq(g_cache[i].name, host)) {
            g_cache[i].seq = ++g_cache_seq;     /* mark as recently used */
            *out_ip = g_cache[i].ip;
            g_stats.cache_hits++;
            return 1;
        }
    }
    return 0;
}

static void cache_insert(const char *host, unsigned int ip)
{
    unsigned int hlen = d_strlen(host);
    if (hlen == 0 || hlen >= DNS_NAME_MAX) return;   /* too long: skip cache */

    unsigned int now = get_ticks_ms();

    /* Pick a victim slot in priority order:
     *   1. an existing slot already holding this name (refresh in place),
     *   2. the first free (or expired) slot,
     *   3. the least-recently-used slot (smallest seq).            */
    int victim = -1;
    int free_slot = -1;
    int lru = 0;
    unsigned int oldest = 0xFFFFFFFFu;

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (g_cache[i].used && d_name_eq(g_cache[i].name, host)) {
            victim = i;                          /* refresh existing entry */
            break;
        }
        if (!g_cache[i].used) {
            if (free_slot < 0) free_slot = i;
        } else if (cache_entry_expired(&g_cache[i], now)) {
            /* Treat expired entries as free victims. */
            if (free_slot < 0) free_slot = i;
        } else if (g_cache[i].seq < oldest) {
            oldest = g_cache[i].seq;
            lru = i;
        }
    }

    if (victim < 0)
        victim = (free_slot >= 0) ? free_slot : lru;

    /* Copy the (bounded) name in, lowercased for case-insensitive matching. */
    unsigned int i = 0;
    for (; i < hlen && i < DNS_NAME_MAX - 1; i++)
        g_cache[victim].name[i] = d_lower(host[i]);
    g_cache[victim].name[i]        = '\0';
    g_cache[victim].ip             = ip;
    g_cache[victim].seq            = ++g_cache_seq;
    g_cache[victim].inserted_ms    = now;
    g_cache[victim].used           = 1;
}

void dns_clear_cache(void)
{
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        g_cache[i].used = 0;
        /* Zero name + ip for hygiene; seq / inserted_ms don't matter. */
        for (int j = 0; j < DNS_NAME_MAX; j++) g_cache[i].name[j] = 0;
        g_cache[i].ip          = 0;
        g_cache[i].seq         = 0;
        g_cache[i].inserted_ms = 0;
    }
    /* Note: g_cache_seq is intentionally NOT reset so eviction ordering
     * remains monotone across clear calls. */
}

static int d_is_digit(char c) { return c >= '0' && c <= '9'; }

/*
 * Try to parse `s` as a dotted-quad "a.b.c.d".  Accepts ONLY strings made of
 * digits and exactly three dots producing four octets in 0..255.  On success
 * writes the host-order address to *out and returns 1; otherwise returns 0
 * (and does not touch *out).
 */
static int parse_dotted_quad(const char *s, unsigned int *out)
{
    unsigned int ip = 0;
    int octets = 0;
    unsigned int cur = 0;
    int digits = 0;

    for (;; s++) {
        char c = *s;
        if (d_is_digit(c)) {
            cur = cur * 10u + (unsigned int)(c - '0');
            if (cur > 255u) return 0;          /* octet overflow */
            digits++;
            if (digits > 3) return 0;          /* too many digits */
        } else if (c == '.' || c == '\0') {
            if (!digits) return 0;             /* empty octet */
            ip = (ip << 8) | cur;
            cur = 0; digits = 0;
            octets++;
            if (c == '\0') break;
            if (octets > 3) return 0;          /* too many dots */
        } else {
            return 0;                          /* non dotted-quad char */
        }
    }

    if (octets != 4) return 0;
    *out = ip;
    return 1;
}

/*
 * Format the 32-bit host-order IP `ip` as the PTR query name
 * `d.c.b.a.in-addr.arpa` into `out` (capacity `cap`).
 * Returns the number of bytes written (excluding NUL), or 0 on overflow.
 *
 * The four octets are written least-significant first (i.e. `ip & 0xFF` is
 * the first component) per RFC 1035 section 3.5.
 */
static int format_arpa_name(unsigned int ip, char *out, int cap)
{
    unsigned char a = (unsigned char)(ip >> 24);
    unsigned char b = (unsigned char)(ip >> 16);
    unsigned char c = (unsigned char)(ip >>  8);
    unsigned char d = (unsigned char)(ip);

    /* We need at most 4*3 digits + 3 dots + ".in-addr.arpa" (13) + NUL = 30 */
    if (cap < 30) return 0;

    /* Write the four octet components in reverse order: d.c.b.a */
    int pos = 0;

    /* Helper: write one decimal octet at `out[pos]`, advance pos. */
#define WRITE_OCTET(oct) \
    do { \
        unsigned char _v = (oct); \
        if (_v >= 100) { out[pos++] = (char)('0' + _v / 100); _v %= 100; } \
        if (_v >= 10)  { out[pos++] = (char)('0' + _v / 10);  _v %= 10;  } \
        out[pos++] = (char)('0' + _v); \
    } while (0)

    WRITE_OCTET(d); out[pos++] = '.';
    WRITE_OCTET(c); out[pos++] = '.';
    WRITE_OCTET(b); out[pos++] = '.';
    WRITE_OCTET(a);

#undef WRITE_OCTET

    /* Append ".in-addr.arpa" */
    const char *suffix = ".in-addr.arpa";
    int si = 0;
    while (suffix[si]) {
        if (pos >= cap - 1) return 0;
        out[pos++] = suffix[si++];
    }
    out[pos] = '\0';
    return pos;
}

/*
 * Encode a hostname into DNS QNAME label format at `out`.
 * "www.example.com" -> 3 w w w 7 e x a m p l e 3 c o m 0
 * Returns number of bytes written, or 0 on error (label/name too long).
 * `cap` is the available space in out.
 */
static int encode_qname(const char *host, unsigned char *out, int cap)
{
    int pos = 0;
    const char *seg = host;

    for (;;) {
        /* measure the next label up to '.' or end */
        int len = 0;
        while (seg[len] && seg[len] != '.') len++;

        if (len == 0) {
            /* empty label: only valid as the very end (trailing dot or "") */
            break;
        }
        if (len > 63) return 0;                /* label too long */
        if (pos + 1 + len >= cap) return 0;    /* + room for terminating 0 */

        out[pos++] = (unsigned char)len;
        for (int i = 0; i < len; i++) out[pos++] = (unsigned char)seg[i];

        seg += len;
        if (*seg == '.') {
            seg++;
            if (*seg == '\0') break;           /* trailing dot terminates */
        } else {
            break;                             /* hit the terminating NUL */
        }
    }

    if (pos + 1 > cap) return 0;
    out[pos++] = 0;                            /* root label */
    return pos;
}

/*
 * Skip a DNS name starting at offset `off` within a `len`-byte message `msg`.
 * Handles label sequences AND compression pointers (0xC0 prefix).
 * Returns the offset of the byte FOLLOWING the name on success, or -1 on a
 * malformed name / out-of-bounds read.
 */
static int skip_name(const unsigned char *msg, int len, int off)
{
    int guard = 0;
    while (off >= 0 && off < len) {
        unsigned char b = msg[off];

        if ((b & 0xC0) == 0xC0) {
            /* compression pointer: 2 bytes, name ends here for this stream */
            if (off + 1 >= len) return -1;
            return off + 2;
        }
        if (b == 0) {
            return off + 1;                    /* root label -> end of name */
        }
        /* ordinary label of length b */
        off += 1 + b;

        if (++guard > 128) return -1;          /* runaway / loop guard */
    }
    return -1;
}

/* Read a big-endian 16-bit value at msg[off], or -1 if out of bounds. */
static int read_be16(const unsigned char *msg, int len, int off)
{
    if (off < 0 || off + 1 >= len) return -1;
    return ((int)msg[off] << 8) | (int)msg[off + 1];
}

/*
 * Decode a DNS wire-format name starting at `msg[off]` into the printable
 * string `out` (capacity `out_cap`).  Handles label sequences and 0xC0
 * compression pointers.  Labels are separated by '.'.  The root label
 * is NOT appended as a trailing dot.  Returns the number of characters
 * written (not counting the NUL), or -1 on error.
 *
 * Used by dns_reverse() to decode the RDATA of PTR records.
 */
static int decode_name(const unsigned char *msg, int mlen, int off,
                       char *out, int out_cap)
{
    int pos = 0;
    int jumps = 0;         /* compression pointer hop count */
    int first = 1;         /* suppress leading dot */

    while (off >= 0 && off < mlen) {
        unsigned char b = msg[off];

        if ((b & 0xC0) == 0xC0) {
            /* compression pointer */
            if (off + 1 >= mlen) return -1;
            int target = ((int)(b & 0x3F) << 8) | (int)msg[off + 1];
            if (target >= mlen)  return -1;
            off = target;
            if (++jumps > 10) return -1;       /* loop guard */
            continue;
        }

        if (b == 0) break;                     /* end of name */

        unsigned char llen = b;
        off++;

        if (!first) {
            if (pos + 1 >= out_cap) return -1;
            out[pos++] = '.';
        }
        first = 0;

        if (off + (int)llen > mlen) return -1;
        for (unsigned char k = 0; k < llen; k++) {
            if (pos + 1 >= out_cap) return -1;
            out[pos++] = (char)msg[off + k];
        }
        off += llen;

        if (++jumps > 256) return -1;          /* label count guard */
    }

    if (pos >= out_cap) return -1;
    out[pos] = '\0';
    return pos;
}

/*
 * Parse a DNS response that is `rlen` bytes long in `resp`, having been issued
 * with transaction id `id`.  Collects up to `max` A/IN addresses (host byte
 * order) into `ips` and returns how many were written (>=1 on success), or a
 * negative DNS_ERR_* on a malformed packet / no usable A record.
 *
 * Header validation (RFC 1035 section 4.1.1):
 *   - QR bit (bit 15) must be 1 (response).
 *   - OPCODE (bits 14-11) must be 0 (QUERY).
 *   - RCODE (bits 3-0 of the low flags byte) must be 0 (NOERROR).  Any other
 *     value (NXDOMAIN=3, SERVFAIL=2, REFUSED=5, etc.) causes an immediate
 *     DNS_ERR_PARSE so callers are not stuck polling on a definitive failure.
 *
 * TC (truncation) bit (bit 9 of the flags word, i.e. bit 1 of resp[2]):
 *   The function sets *tc_out to 1 if the TC bit is set in the reply.
 *
 * Record-skipping robustness:
 *   AAAA (TYPE=28), NS (2), MX (15), TXT (16), SOA (6), SRV (33) and every
 *   other non-A RR type is skipped by advancing `off` by the RR's RDLENGTH.
 *   The iteration is capped at DNS_MAX_ANSWERS regardless of wire ancount.
 */
static int parse_response(const unsigned char *resp, int rlen,
                          unsigned short id, unsigned int *ips, int max,
                          int *tc_out)
{
    if (tc_out) *tc_out = 0;
    if (rlen < 12) return DNS_ERR_TIMEO;       /* nothing usable arrived */

    /* ---- verify header ---- */

    int resp_id = read_be16(resp, rlen, 0);
    if (resp_id != (int)id) return DNS_ERR_PARSE;

    unsigned char flags_hi = resp[2];
    unsigned char flags_lo = resp[3];

    if ((flags_hi & 0x80) == 0) return DNS_ERR_PARSE;   /* QR must be 1 */
    if ((flags_hi & 0x78) != 0) return DNS_ERR_PARSE;   /* OPCODE must be 0 */

    if ((flags_hi & 0x02) && tc_out) *tc_out = 1;       /* TC bit */

    if ((flags_lo & 0x0F) != 0) return DNS_ERR_PARSE;   /* RCODE must be 0 */

    int qdcount = read_be16(resp, rlen, 4);
    int ancount = read_be16(resp, rlen, 6);
    if (qdcount < 0 || ancount <= 0) return DNS_ERR_PARSE;

    int iter_max = ancount;
    if (iter_max > DNS_MAX_ANSWERS) iter_max = DNS_MAX_ANSWERS;

    /* ---- skip the question section(s) ---- */
    int off = 12;
    for (int q = 0; q < qdcount; q++) {
        off = skip_name(resp, rlen, off);
        if (off < 0) return DNS_ERR_PARSE;
        off += 4;                              /* QTYPE(2) + QCLASS(2) */
        if (off > rlen) return DNS_ERR_PARSE;
    }

    /* ---- walk every answer record, collecting A/IN addresses ---- */
    int count = 0;
    for (int a = 0; a < iter_max && count < max; a++) {
        off = skip_name(resp, rlen, off);
        if (off < 0) return DNS_ERR_PARSE;

        int type     = read_be16(resp, rlen, off);
        int klass    = read_be16(resp, rlen, off + 2);
        int rdlength = read_be16(resp, rlen, off + 8);

        if (type < 0 || klass < 0 || rdlength < 0) return DNS_ERR_PARSE;

        int rdata = off + 10;
        if (rdata + rdlength > rlen) return DNS_ERR_PARSE;

        if (type == 1 /* A */ && klass == 1 /* IN */ && rdlength == 4) {
            ips[count++] =
                ((unsigned int)resp[rdata]     << 24) |
                ((unsigned int)resp[rdata + 1] << 16) |
                ((unsigned int)resp[rdata + 2] <<  8) |
                ((unsigned int)resp[rdata + 3]);
        }

        off = rdata + rdlength;
    }

    return (count > 0) ? count : DNS_ERR_PARSE;
}

/*
 * parse_ptr_response -- like parse_response but extracts the first PTR
 * (TYPE=12) RDATA and decodes it into `out_name` (capacity `out_cap`).
 * Returns the name length on success, or a negative DNS_ERR_* on failure.
 */
static int parse_ptr_response(const unsigned char *resp, int rlen,
                               unsigned short id,
                               char *out_name, int out_cap)
{
    if (rlen < 12)            return DNS_ERR_TIMEO;

    int resp_id = read_be16(resp, rlen, 0);
    if (resp_id != (int)id)   return DNS_ERR_PARSE;

    unsigned char flags_hi = resp[2];
    unsigned char flags_lo = resp[3];

    if ((flags_hi & 0x80) == 0) return DNS_ERR_PARSE;
    if ((flags_hi & 0x78) != 0) return DNS_ERR_PARSE;
    if ((flags_lo & 0x0F) != 0) return DNS_ERR_PARSE;

    int qdcount = read_be16(resp, rlen, 4);
    int ancount = read_be16(resp, rlen, 6);
    if (qdcount < 0 || ancount <= 0) return DNS_ERR_PARSE;

    int iter_max = ancount;
    if (iter_max > DNS_MAX_ANSWERS) iter_max = DNS_MAX_ANSWERS;

    int off = 12;
    for (int q = 0; q < qdcount; q++) {
        off = skip_name(resp, rlen, off);
        if (off < 0) return DNS_ERR_PARSE;
        off += 4;
        if (off > rlen) return DNS_ERR_PARSE;
    }

    for (int a = 0; a < iter_max; a++) {
        off = skip_name(resp, rlen, off);
        if (off < 0) return DNS_ERR_PARSE;

        int type     = read_be16(resp, rlen, off);
        int klass    = read_be16(resp, rlen, off + 2);
        int rdlength = read_be16(resp, rlen, off + 8);

        if (type < 0 || klass < 0 || rdlength < 0) return DNS_ERR_PARSE;

        int rdata = off + 10;
        if (rdata + rdlength > rlen) return DNS_ERR_PARSE;

        if (type == 12 /* PTR */ && klass == 1 /* IN */) {
            /* Decode the RDATA domain name. */
            int nlen = decode_name(resp, rlen, rdata, out_name, out_cap);
            return nlen;   /* >= 0 on success, -1 on error */
        }

        off = rdata + rdlength;
    }

    return DNS_ERR_PARSE;   /* no PTR record found */
}

/*
 * Build, send and await a single DNS query for `hostname` with the given
 * QTYPE, retrying up to DNS_MAX_TRIES times.  For QTYPE=1 (A) writes up to
 * `max` addresses to `ips` and returns the count.  For QTYPE=12 (PTR) writes
 * the decoded name to `ptr_out` / `ptr_cap` and returns the name length on
 * success.  Returns a negative DNS_ERR_* on failure.
 */
static int do_query_ex(const char *hostname, unsigned int hlen,
                       unsigned int *ips, int max,
                       char *ptr_out, int ptr_cap,
                       unsigned short qtype)
{
    long fd = sc(SYS_SOCKET, SOCK_DGRAM, 0, 0, 0, 0);
    if (fd < 0) return DNS_ERR_SOCK;

    int result = DNS_ERR_TIMEO;
    int saw_tc = 0;
    unsigned char query[512];
    unsigned char resp[512];
    sock_addr_t from;

    unsigned int seed = (unsigned int)sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);

    for (int attempt = 0; attempt < DNS_MAX_TRIES; attempt++) {
        if (attempt > 0) g_stats.retries++;

        unsigned int mix = seed ^ (0x9E3779B9u * (unsigned int)(attempt + 1));
        for (unsigned int i = 0; i < hlen; i++)
            mix = mix * 31u + (unsigned char)hostname[i];
        unsigned short id = (unsigned short)(mix ^ (mix >> 16) ^ 0xA53Cu);
        if (id == 0) id = (unsigned short)(0x1234 + attempt);
        seed = mix;

        /* ---- build the DNS query ---- */
        int qlen = 0;

        query[qlen++] = (unsigned char)(id >> 8);
        query[qlen++] = (unsigned char)(id & 0xFF);
        query[qlen++] = 0x01;   /* flags hi: RD=1 */
        query[qlen++] = 0x00;   /* flags lo       */
        query[qlen++] = 0x00; query[qlen++] = 0x01;   /* QDCOUNT = 1 */
        query[qlen++] = 0x00; query[qlen++] = 0x00;   /* ANCOUNT = 0 */
        query[qlen++] = 0x00; query[qlen++] = 0x00;   /* NSCOUNT = 0 */
        query[qlen++] = 0x00; query[qlen++] = 0x01;   /* ARCOUNT = 1 (OPT) */

        int nlen = encode_qname(hostname, query + qlen,
                                (int)sizeof(query) - qlen - 4 - 11);
        if (nlen <= 0) { result = DNS_ERR_INVAL; break; }
        qlen += nlen;

        /* QTYPE (caller-specified), QCLASS = IN */
        query[qlen++] = (unsigned char)(qtype >> 8);
        query[qlen++] = (unsigned char)(qtype & 0xFF);
        query[qlen++] = 0x00; query[qlen++] = 0x01;

        /* EDNS0 OPT pseudo-RR (RFC 6891), 11 bytes */
        if (qlen + 11 <= (int)sizeof(query)) {
            query[qlen++] = 0x00;               /* NAME: root */
            query[qlen++] = 0x00; query[qlen++] = 0x29;  /* TYPE: OPT   */
            query[qlen++] = 0x04; query[qlen++] = 0xD0;  /* CLASS: 1232 */
            query[qlen++] = 0x00; query[qlen++] = 0x00;  /* TTL hi      */
            query[qlen++] = 0x00; query[qlen++] = 0x00;  /* TTL lo      */
            query[qlen++] = 0x00; query[qlen++] = 0x00;  /* RDLENGTH=0  */
        } else {
            query[10] = 0x00; query[11] = 0x00;  /* ARCOUNT = 0 */
        }

        g_stats.queries++;

        long sent = sc(SYS_SENDTO, fd, (long)query, qlen,
                       (long)g_dns_server, DNS_PORT);
        if (sent < 0) { result = DNS_ERR_SEND; continue; }

        /* ---- bounded poll loop ---- */
        int rlen = -1;
        for (int it = 0; it < DNS_POLL_MAX; it++) {
            sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);

            long n = sc(SYS_RECVFROM, fd, (long)resp, (long)sizeof(resp),
                        (long)&from, 0);
            /* DNS source validation (anti cache-poisoning): only accept a
             * reply that came from the resolver we queried (IP + port 53).
             * from.ip/from.port are HOST order, matching g_dns_server. A
             * spoofed reply from any other source is ignored; the bounded
             * poll loop keeps waiting for the genuine answer. */
            if (n > 0 && from.ip == g_dns_server && from.port == DNS_PORT) {
                rlen = (int)n; break;
            }

            sc(SYS_YIELD, 0, 0, 0, 0, 0);
            (void)n;
        }

        if (rlen < 0) continue;

        if (qtype == 12 /* PTR */) {
            int plen = parse_ptr_response(resp, rlen, id, ptr_out, ptr_cap);
            if (plen >= 0) {
                result = plen;
                break;
            }
            result = plen;
            continue;
        }

        /* QTYPE == A */
        int tc = 0;
        int parsed = parse_response(resp, rlen, id, ips, max, &tc);

        if (tc) saw_tc = 1;

        if (parsed > 0) {
            result = parsed;
            break;
        }

        if (tc && parsed <= 0) {
            result = DNS_ERR_TRUNC;
            continue;
        }

        result = parsed;
    }

    if (result == DNS_ERR_TIMEO && saw_tc) result = DNS_ERR_TRUNC;

    if (result == DNS_ERR_TIMEO || result == DNS_ERR_TRUNC)
        g_stats.timeouts++;

    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    return result;
}

/*
 * do_query -- forward A query (backward-compatible wrapper around do_query_ex).
 */
static int do_query(const char *hostname, unsigned int hlen,
                    unsigned int *ips, int max)
{
    return do_query_ex(hostname, hlen, ips, max, (char *)0, 0, 1 /* A */);
}

/* ---- public API --------------------------------------------------------- */

int dns_resolve_all(const char *host, unsigned int *ips, int max, int *count)
{
    if (count) *count = 0;
    if (!host || !ips || max < 1) return DNS_ERR_INVAL;

    unsigned int parsed;
    if (parse_dotted_quad(host, &parsed)) {
        ips[0] = parsed;
        if (count) *count = 1;
        return DNS_OK;
    }

    unsigned int hlen = d_strlen(host);
    if (hlen == 0 || hlen > 253) return DNS_ERR_INVAL;

    unsigned int cached;
    if (cache_lookup(host, &cached)) {
        ips[0] = cached;
        if (count) *count = 1;
        return DNS_OK;
    }

    int n = do_query(host, hlen, ips, max);
    if (n > 0) {
        cache_insert(host, ips[0]);
        if (count) *count = n;
        return DNS_OK;
    }
    return n;
}

int dns_resolve(const char *hostname, unsigned int *out_ip)
{
    if (!hostname || !out_ip) return DNS_ERR_INVAL;

    unsigned int parsed;
    if (parse_dotted_quad(hostname, &parsed)) {
        *out_ip = parsed;
        return DNS_OK;
    }

    unsigned int hlen = d_strlen(hostname);
    if (hlen == 0 || hlen > 253) return DNS_ERR_INVAL;

    if (cache_lookup(hostname, out_ip))
        return DNS_OK;

    unsigned int ip;
    int n = do_query(hostname, hlen, &ip, 1);
    if (n > 0) {
        *out_ip = ip;
        cache_insert(hostname, ip);
        return DNS_OK;
    }
    return n;
}

int dns_reverse(unsigned int ip, char *out_name, int out_cap)
{
    if (!out_name || out_cap < 2) return DNS_ERR_INVAL;

    /* Build the "d.c.b.a.in-addr.arpa" query name as a C string. */
    char arpa[32];
    if (!format_arpa_name(ip, arpa, (int)sizeof(arpa)))
        return DNS_ERR_INVAL;

    unsigned int hlen = d_strlen(arpa);

    /* PTR query -- no A-record cache involvement. */
    int r = do_query_ex(arpa, hlen,
                        (unsigned int *)0, 0,
                        out_name, out_cap,
                        12 /* PTR */);
    return r;   /* >= 0 = name length; negative = error */
}

/* ---- self-test (no live network) --------------------------------------- */

/*
 * Build a minimal valid DNS response packet in `buf` (capacity `cap`) for
 * transaction id `id`, containing the RRs described in `rrs`.
 *
 * rr_types[i]  : 1 = A record, 28 = AAAA, 5 = CNAME (RDATA = name bytes)
 * For TYPE=1  : rdata must be 4 bytes (IPv4 address, network order)
 * For TYPE=28 : rdata must be 16 bytes (IPv6 address) -- we write zeros
 * For TYPE=5  : rdata = encoded label name (we write a minimal 1-byte root)
 *
 * Returns the packet length, or -1 if cap is too small.
 */
struct selftest_rr {
    unsigned short type;
    unsigned int   ipv4;   /* only meaningful for type==1 */
};

static int build_test_response(unsigned char *buf, int cap,
                               unsigned short id,
                               const struct selftest_rr *rrs, int nrrs)
{
    if (cap < 12 + 5) return -1;

    int pos = 0;

    /* Header */
    buf[pos++] = (unsigned char)(id >> 8);
    buf[pos++] = (unsigned char)(id & 0xFF);
    buf[pos++] = 0x81;  /* QR=1 OPCODE=0 AA=1 TC=0 RD=1 */
    buf[pos++] = 0x80;  /* RA=1 RCODE=0                  */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* QDCOUNT=1 */
    buf[pos++] = (unsigned char)(nrrs >> 8);
    buf[pos++] = (unsigned char)(nrrs & 0xFF);  /* ANCOUNT=nrrs */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* NSCOUNT=0 */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* ARCOUNT=0 */

    /* Question: name=root (0x00), QTYPE=A, QCLASS=IN */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* QTYPE=A   */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* QCLASS=IN */

    /* Answer RRs */
    for (int i = 0; i < nrrs; i++) {
        if (pos + 1 >= cap) return -1;
        buf[pos++] = 0x00;                  /* NAME: root label */

        if (pos + 8 > cap) return -1;
        buf[pos++] = (unsigned char)(rrs[i].type >> 8);
        buf[pos++] = (unsigned char)(rrs[i].type & 0xFF);
        buf[pos++] = 0x00; buf[pos++] = 0x01;  /* CLASS=IN */
        buf[pos++] = 0x00; buf[pos++] = 0x00;  /* TTL high */
        buf[pos++] = 0x00; buf[pos++] = 0x3C;  /* TTL low = 60 */

        if (rrs[i].type == 1) {
            if (pos + 6 > cap) return -1;
            buf[pos++] = 0x00; buf[pos++] = 0x04;
            buf[pos++] = (unsigned char)(rrs[i].ipv4 >> 24);
            buf[pos++] = (unsigned char)(rrs[i].ipv4 >> 16);
            buf[pos++] = (unsigned char)(rrs[i].ipv4 >>  8);
            buf[pos++] = (unsigned char)(rrs[i].ipv4 & 0xFF);
        } else if (rrs[i].type == 28) {
            if (pos + 18 > cap) return -1;
            buf[pos++] = 0x00; buf[pos++] = 0x10;
            for (int j = 0; j < 16; j++) buf[pos++] = 0x00;
        } else if (rrs[i].type == 5) {
            if (pos + 3 > cap) return -1;
            buf[pos++] = 0x00; buf[pos++] = 0x01;
            buf[pos++] = 0x00;  /* root label */
        } else {
            if (pos + 2 > cap) return -1;
            buf[pos++] = 0x00; buf[pos++] = 0x00;
        }
    }

    return pos;
}

/*
 * Build a PTR response packet for use in the self-test.
 * The RDATA name is encoded as the literal label sequence for `ptr_name`
 * (a plain C string already formatted as labels by encode_qname).
 * `id` is the transaction ID to stamp in the header.
 * Returns packet length or -1 on overflow.
 */
static int build_ptr_response(unsigned char *buf, int cap,
                               unsigned short id,
                               const char *ptr_name)
{
    if (cap < 12 + 5 + 1 + 10 + 2) return -1;

    int pos = 0;

    /* Header */
    buf[pos++] = (unsigned char)(id >> 8);
    buf[pos++] = (unsigned char)(id & 0xFF);
    buf[pos++] = 0x81;  /* QR=1 OPCODE=0 AA=1 TC=0 RD=1 */
    buf[pos++] = 0x80;  /* RA=1 RCODE=0 */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* QDCOUNT=1 */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* ANCOUNT=1 */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* NSCOUNT=0 */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* ARCOUNT=0 */

    /* Question: name=root (0x00), QTYPE=PTR(12), QCLASS=IN */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x0C;  /* QTYPE=PTR */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* QCLASS=IN */

    /* Answer RR: NAME=root, TYPE=PTR, CLASS=IN, TTL=60 */
    if (pos + 1 >= cap) return -1;
    buf[pos++] = 0x00;                      /* NAME: root */

    if (pos + 8 > cap) return -1;
    buf[pos++] = 0x00; buf[pos++] = 0x0C;  /* TYPE=PTR  */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* CLASS=IN  */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* TTL high  */
    buf[pos++] = 0x00; buf[pos++] = 0x3C;  /* TTL low   */

    /* Encode RDATA: the PTR target name as DNS labels. */
    /* Reserve 2 bytes for RDLENGTH, fill later. */
    if (pos + 2 > cap) return -1;
    int rdlen_pos = pos;
    pos += 2;

    int rdata_start = pos;
    int nlen = encode_qname(ptr_name, buf + pos, cap - pos);
    if (nlen <= 0) return -1;
    pos += nlen;

    int rdlength = pos - rdata_start;
    buf[rdlen_pos]     = (unsigned char)(rdlength >> 8);
    buf[rdlen_pos + 1] = (unsigned char)(rdlength & 0xFF);

    return pos;
}

/*
 * dns_selftest -- run built-in sanity checks with no live network.
 *
 * Returns 0 on pass, -1 on any failure.
 *
 * Test 1: Dotted-quad fast path via dns_resolve.
 * Test 2: dns_set_server / dns_get_server round-trip.
 * Test 3: AAAA before A in response (parse_response skips AAAA).
 * Test 4: Non-zero RCODE is rejected.
 * Test 5: dns_resolve_all dotted-quad returns count=1 and ips[0] correct.
 * Test 6: Cache hit increments cache_hits counter.
 * Test 7: TTL eviction -- if DNS_CACHE_TTL_MS == 0, inserted entry must NOT
 *         be returned as a hit (it expires immediately on the next lookup).
 *         When DNS_CACHE_TTL_MS > 0, this test is skipped (trivially passes).
 * Test 8: dns_clear_cache empties the cache; subsequent lookup is a miss.
 */
int dns_selftest(void)
{
    /* ---- Test 1: dotted-quad fast path ---- */
    {
        unsigned int ip = 0;
        int r = dns_resolve("10.0.2.2", &ip);
        if (r != DNS_OK)         return -1;
        if (ip != 0x0A000202u)   return -1;
    }

    /* ---- Test 2: server get/set round-trip ---- */
    {
        unsigned int orig = dns_get_server();
        dns_set_server(0x08080808u);
        if (dns_get_server() != 0x08080808u)  return -1;
        dns_set_server(orig);
        if (dns_get_server() != orig)          return -1;
    }

    /* ---- Test 3: AAAA (TYPE=28) before A (TYPE=1) ---- */
    {
        unsigned char pkt[256];
        unsigned short id = 0xBEEF;

        struct selftest_rr rrs[2];
        rrs[0].type = 28;
        rrs[0].ipv4 = 0;
        rrs[1].type = 1;
        rrs[1].ipv4 = 0x5DB8D822u;  /* 93.184.216.34 = example.com */

        int plen = build_test_response(pkt, (int)sizeof(pkt), id, rrs, 2);
        if (plen <= 0) return -1;

        unsigned int ips[4];
        int tc = 0;
        int n = parse_response(pkt, plen, id, ips, 4, &tc);
        if (n != 1)                  return -1;
        if (ips[0] != 0x5DB8D822u)   return -1;
        if (tc != 0)                 return -1;
    }

    /* ---- Test 4: RCODE != 0 (NXDOMAIN) is rejected ---- */
    {
        unsigned char pkt[64];
        unsigned short id = 0x1234;

        struct selftest_rr rrs[1];
        rrs[0].type = 1;
        rrs[0].ipv4 = 0x01020304u;

        int plen = build_test_response(pkt, (int)sizeof(pkt), id, rrs, 1);
        if (plen <= 0) return -1;

        pkt[3] = (pkt[3] & 0xF0) | 0x03;  /* patch RCODE=3 */

        unsigned int ips[4];
        int tc = 0;
        int n = parse_response(pkt, plen, id, ips, 4, &tc);
        if (n != DNS_ERR_PARSE) return -1;
    }

    /* ---- Test 5: dns_resolve_all dotted-quad ---- */
    {
        unsigned int ips[4];
        int cnt = 0;
        int r = dns_resolve_all("1.2.3.4", ips, 4, &cnt);
        if (r != DNS_OK)           return -1;
        if (cnt < 1)               return -1;
        if (ips[0] != 0x01020304u) return -1;
    }

    /* ---- Test 6: cache hit increments cache_hits counter ---- */
    /*
     * This test inserts an entry and verifies that a successful cache lookup
     * increments g_stats.cache_hits.
     *
     * When compiled with -DDNS_TEST_TTL_ZERO, DNS_CACHE_TTL_MS is 0 and
     * every entry expires immediately; no cache hit is possible.  In that
     * mode we instead verify that a lookup on a missing name does NOT
     * increment cache_hits (counter stays stable on a miss), which is the
     * complementary correctness property.
     */
    {
#ifdef DNS_TEST_TTL_ZERO
        /* TTL=0 mode: verify miss does NOT increment cache_hits. */
        dns_clear_cache();
        struct dns_stats stA; dns_stats(&stA);
        unsigned int ip6 = 0;
        cache_lookup("no-such-entry.internal", &ip6);
        struct dns_stats stB; dns_stats(&stB);
        if (stB.cache_hits != stA.cache_hits) return -1;
#else
        /* Normal mode: insert an entry, look it up, verify counter bumps. */
        dns_clear_cache();
        cache_insert("selftest.internal", 0xC0000201u);
        struct dns_stats st0; dns_stats(&st0);
        unsigned int ip = 0;
        int hit = cache_lookup("selftest.internal", &ip);
        if (!hit)                         return -1;
        if (ip != 0xC0000201u)            return -1;
        struct dns_stats st1; dns_stats(&st1);
        if (st1.cache_hits <= st0.cache_hits) return -1;
#endif
    }

    /* ---- Test 7: TTL eviction ---- */
    /*
     * When compiled with -DDNS_TEST_TTL_ZERO, DNS_CACHE_TTL_MS is 0 and
     * every entry expires on the very next lookup (current_ms - inserted_ms
     * >= 0 is always true).  We insert an entry, look it up, and verify
     * the lookup is a MISS (the entry was evicted as expired).
     *
     * Without -DDNS_TEST_TTL_ZERO, the TTL is 60 s and this sub-test is
     * trivially skipped (the inserted entry IS found, which is correct
     * behaviour for a non-zero TTL).
     */
    {
#ifdef DNS_TEST_TTL_ZERO
        dns_clear_cache();
        cache_insert("ttl-test.internal", 0xDEADBEEFu);

        unsigned int ip = 0;
        int hit = cache_lookup("ttl-test.internal", &ip);
        /* With TTL=0 the entry must have been evicted -- expect a miss. */
        if (hit != 0) return -1;
#endif
        /* Without DNS_TEST_TTL_ZERO: insert and verify the entry IS found
         * (i.e. normal TTL=60 s path). */
#ifndef DNS_TEST_TTL_ZERO
        dns_clear_cache();
        cache_insert("ttl-live.internal", 0xAABBCCDDu);
        unsigned int ip = 0;
        int hit = cache_lookup("ttl-live.internal", &ip);
        if (!hit)                      return -1;
        if (ip != 0xAABBCCDDu)         return -1;
#endif
    }

    /* ---- Test 8: dns_clear_cache empties the cache ---- */
    {
        cache_insert("clear-test.internal", 0x11223344u);
        dns_clear_cache();

        unsigned int ip = 0;
        int hit = cache_lookup("clear-test.internal", &ip);
        if (hit != 0) return -1;   /* must miss after clear */
    }

    /* ---- Test 9: PTR response parsing (parse_ptr_response, no network) -- */
    {
        unsigned char pkt[256];
        unsigned short id = 0xCAFE;

        /* Build a PTR response for "4.3.2.1.in-addr.arpa" -> "host.example" */
        int plen = build_ptr_response(pkt, (int)sizeof(pkt), id, "host.example");
        if (plen <= 0) return -1;

        char name[128];
        int nlen = parse_ptr_response(pkt, plen, id, name, (int)sizeof(name));
        if (nlen <= 0)                   return -1;
        /* Verify the decoded name starts with "host" */
        if (name[0] != 'h') return -1;
        if (name[1] != 'o') return -1;
        if (name[2] != 's') return -1;
        if (name[3] != 't') return -1;
    }

    return 0;  /* all tests passed */
}
