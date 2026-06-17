/*
 * http.c -- Minimal freestanding HTTP/1.1 GET client (userspace, ring 3).
 * =========================================================================
 *
 * Implements http_get() / http_get_ex() / https_get() / http_get_range() /
 * https_get_range() / http_selftest() over the unified netconn connection
 * abstraction (tlsconn.h).  For plain HTTP, netconn_open() opens a raw TCP
 * socket; for HTTPS it additionally completes a TLS handshake.  The receive
 * path loops on netconn_read() + SYS_YIELD between dry attempts until the
 * peer closes, the caller's buffer fills, or a fixed iteration bound is hit.
 * No loop is unbounded, so a stalled server cannot hang the OS.
 *
 * BOUNDED TOTAL TIMEOUT: in addition to the per-loop iteration caps, the WHOLE
 * call (DNS + TCP/TLS connect + every redirect hop's transfer) is bounded by a
 * single wall-clock deadline (default HTTP_TIMEOUT_MS, ~8 s) measured against
 * the SYS_GET_TICKS_MS=40 monotonic counter.  The deadline is computed once in
 * http_do_get and enforced in the send loop, the receive loop, AND inside the
 * netconn read/write loops (via netconn_set_deadline).  On expiry the
 * connection is closed and HTTP_ERR_TIMEO (-110) is returned, so a slow or
 * unreachable host cannot block the caller indefinitely -- callers such as
 * browser2 fall back to their built-in error page.  http_get_timeout_ex()
 * overrides the budget per call without changing any existing signature.
 *
 * No libc: request building and response parsing use the small static helpers
 * below; every scratch buffer is a fixed-size static or local array.
 *
 * Capabilities beyond a bare GET (see http.h for the full contract):
 *   1. REDIRECT FOLLOWING -- 301/302/303/307/308 + Location: are followed up
 *      to a small cap.  Location may be absolute "http://host[:port]/path",
 *      "https://host[:port]/path", or relative "/path" (resolved against the
 *      current host/port).  The TLS flag is updated per the target scheme on
 *      each hop, so an http -> https redirect is followed over TLS.
 *   2. CHUNKED TRANSFER-ENCODING -- "Transfer-Encoding: chunked" bodies are
 *      decoded (hex chunk sizes, concatenated data, terminating 0-chunk).
 *   3. CONTENT-LENGTH -- honoured as `long` for an exact body read; large
 *      bodies no longer truncate.  Connection-close drain is the fallback.
 *   4. GZIP / DEFLATE CONTENT-ENCODING -- "Content-Encoding: gzip" or
 *      "deflate" bodies are inflated via the shared codec.  On ANY decode
 *      failure we fall back to returning the raw (compressed) body.
 *   5. KEEP-ALIVE -- when the server does NOT send "Connection: close" on an
 *      HTTP/1.1 response, the TCP (or TLS) connection is cached in a 1-slot
 *      file-scope cache (g_ka_*) keyed by host + port + is_tls.  Subsequent
 *      requests to the same host:port:is_tls reuse the connection.  The cache
 *      is evicted on host mismatch or after HTTP_KA_IDLE_MS idle milliseconds
 *      (tracked via SYS_GET_TICKS_MS = syscall 40).
 *   6. RANGE REQUESTS -- http_get_range() / https_get_range() add a
 *      "Range: bytes=<first>-<last>" header; 206 and 200 responses are both
 *      accepted.
 *   All header-name comparisons are case-insensitive; all loops are bounded.
 *   Transfer-Encoding: identity is treated as no special encoding.
 *   Extra whitespace after ':' in header values is tolerated.
 *
 * Build (flags passed DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/http.c -o http.o
 *
 * Links against dns.o, deflate.o, and tlsconn.o.
 *
 * Exact codec API consumed for gzip/deflate (from ../deflate/deflate.h):
 *   long inflate_decompress(const unsigned char *in, long in_len,
 *                           unsigned char *out, long out_cap);
 *
 * netconn API consumed (from tlsconn.h):
 *   typedef struct { int fd; int is_tls; int trusted; tls_conn_t tls; } netconn;
 *   int  netconn_open (netconn *nc, const char *host, unsigned short port, int use_tls);
 *   long netconn_write(netconn *nc, const void *buf, unsigned long len);
 *   long netconn_read (netconn *nc, void *buf, unsigned long cap);
 *   void netconn_close(netconn *nc);
 */

#include "http.h"
#include "tlsconn.h"
#include "dns.h"
#include "../deflate/deflate.h"

/* ---- syscall numbers ---------------------------------------------------- */
#define SYS_YIELD           15
#define SYS_GET_TICKS_MS    40   /* returns monotonic millisecond counter     */

/* ---- error codes -------------------------------------------------------- */
#define HTTP_OK            0
#define HTTP_ERR_INVAL   (-22)   /* bad argument                            */
#define HTTP_ERR_DNS     (-1)    /* host resolution failed                  */
#define HTTP_ERR_SOCK    (-2)    /* could not create socket                 */
#define HTTP_ERR_CONNECT (-3)    /* TCP connect (or TLS handshake) failed   */
#define HTTP_ERR_SEND    (-4)    /* send failed                             */
#define HTTP_ERR_TIMEO   (-110)  /* no response within the bound            */

/* EAGAIN as returned by netconn_read (would-block, keep polling). */
#define EAGAIN_NEG       (-11)

/* Bounds (see header notes: ~200000 iterations with a yield each). */
#define HTTP_POLL_MAX    200000

/*
 * BOUNDED TOTAL FETCH TIMEOUT
 * ---------------------------
 * A single http_get / https_get call may touch DNS resolution, a TCP (and for
 * HTTPS a TLS) handshake, and a multi-round receive loop.  Any of those can
 * stall on a slow or unreachable peer.  To guarantee the caller (browser,
 * browser2, wget, apidemo, js_fetch) regains control, we impose a wall-clock
 * deadline measured against the SYS_GET_TICKS_MS monotonic counter.
 *
 * The deadline spans the WHOLE call (all redirect hops share one budget): it is
 * computed once at http_do_get() entry as now + HTTP_TIMEOUT_MS and enforced in
 * three places -- (1) the send loop, (2) the receive loop, both here in
 * http_fetch_raw, and (3) inside tlsconn's read/write loops via
 * netconn_set_deadline().  On expiry the connection is closed and
 * HTTP_ERR_TIMEO is returned so callers fall back to their error page.
 *
 * HTTP_TIMEOUT_MS is the default budget (~8 s).  http_get_timeout_ex() lets a
 * caller override it without disturbing any existing signature.
 */
#define HTTP_TIMEOUT_MS  8000UL    /* default total fetch budget (~8 seconds) */

/* Maximum total raw response bytes we accumulate per request (status line +
 * headers + raw body).  The raw body is then post-processed (de-chunked and/or
 * inflated) into the caller buffer.  Sized to comfortably exceed typical pages
 * the browser/wget fetch while staying a fixed, bounded allocation. */
#define HTTP_RAW_MAX     262144  /* 256 KiB */

/* Generous header-block ceiling: once we have this much without finding the
 * "\r\n\r\n" terminator we give up parsing headers. */
#define HTTP_HDR_MAX     8192

/* Per-host/path component caps for redirect URL parsing. */
#define HTTP_HOST_MAX    256
#define HTTP_PATH_MAX    1024

/* ---- raw syscall helpers ------------------------------------------------ */

static long sc_yield(void)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"((long)SYS_YIELD),
                   "D"((long)0), "S"((long)0), "d"((long)0)
                 : "rcx", "r11", "memory");
    return r;
}

/* Returns a monotonic millisecond timestamp (wraps after ~49 days). */
static unsigned long sc_ticks_ms(void)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"((long)SYS_GET_TICKS_MS),
                   "D"((long)0), "S"((long)0), "d"((long)0)
                 : "rcx", "r11", "memory");
    return (unsigned long)r;
}

/*
 * deadline_reached -- PURE wrap-safe comparison: 1 if `now` is at/after the
 * absolute `deadline`, else 0 (deadline 0 == disabled).  Split out so the
 * offline self-test can exercise the wrap arithmetic without a syscall.
 *
 * The subtraction is unsigned so it stays correct across the ~49-day tick wrap:
 * (now - deadline) only has its top bit clear once `now` has reached/passed
 * `deadline` (within a half-range window, which 8 s vs a 49-day period easily
 * satisfies).
 */
static int deadline_reached(unsigned long now, unsigned long deadline)
{
    if (deadline == 0)
        return 0;
    return (unsigned long)(now - deadline) < (1UL << 63);
}

/*
 * deadline_passed -- 1 if the monotonic clock has reached the absolute
 * `deadline` timestamp, else 0.  A `deadline` of 0 means "no deadline".
 */
static int deadline_passed(unsigned long deadline)
{
    if (deadline == 0)
        return 0;
    return deadline_reached(sc_ticks_ms(), deadline);
}

/* ---- file-scope static connection objects --------------------------------
 *
 * g_nc     : the active connection used for the current in-flight request.
 *            Once the response body is fully consumed, if the server allows
 *            keep-alive, g_nc is moved into the keep-alive cache (g_ka_nc).
 *
 * g_ka_nc  : the ONE cached keep-alive connection.
 * g_ka_host: the host name it is connected to (NUL-terminated).
 * g_ka_port: the TCP port.
 * g_ka_tls : 0 = plain TCP, 1 = TLS.
 * g_ka_idle: sc_ticks_ms() value at the time the connection was cached.
 *
 * netconn embeds tls_conn_t which can be ~40 KB.  Declaring them static keeps
 * them out of the stack (which is fixed-size in a freestanding env).
 * Only one fetch is in flight at a time (single-threaded userspace). */

static netconn g_nc;    /* active connection for the current request         */
static netconn g_ka_nc; /* cached keep-alive connection (fd == -1 if empty)  */
static char    g_ka_host[HTTP_HOST_MAX]; /* host of cached connection         */
static unsigned short g_ka_port;         /* port of cached connection         */
static int     g_ka_tls;                 /* TLS flag of cached connection     */
static unsigned long  g_ka_idle;         /* timestamp when it was cached      */
static int     g_ka_valid;               /* 1 if g_ka_nc is live              */

/* ---- tiny freestanding helpers ----------------------------------------- */

static void h_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
}

static int h_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int h_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ASCII lower-case of one byte. */
static int h_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

/* Append the NUL-terminated string `s` to buf at *pos, capped at cap.
 * Updates *pos.  Does not NUL-terminate (caller does if needed). */
static void buf_puts(char *buf, int *pos, int cap, const char *s)
{
    int p = *pos;
    while (*s && p < cap) buf[p++] = *s++;
    *pos = p;
}

/* Format an unsigned long as decimal into buf at *pos. */
static void buf_putulong(char *buf, int *pos, int cap, unsigned long v)
{
    char tmp[24];
    int  n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        unsigned long t = v;
        while (t && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + t % 10); t /= 10; }
        /* reverse */
        for (int a = 0, b = n - 1; a < b; a++, b--) {
            char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c;
        }
    }
    tmp[n] = '\0';
    buf_puts(buf, pos, cap, tmp);
}

/*
 * Find the "\r\n\r\n" header/body separator in the first `len` bytes of buf.
 * Returns the offset of the byte AFTER the separator (i.e. body start), or
 * -1 if the separator is not present in the inspected range.
 */
static int find_body_start(const char *buf, int len)
{
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

/*
 * Parse the numeric status code out of an HTTP status line that begins the
 * buffer ("HTTP/1.x <code> <reason>\r\n").  Returns the code, or -1 if it
 * cannot be parsed from the first `len` bytes.
 */
static int parse_status(const char *buf, int len)
{
    int i = 0;
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') i++;
    while (i < len && buf[i] == ' ') i++;
    int code = 0, digits = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9' && digits < 3) {
        code = code * 10 + (buf[i] - '0');
        i++; digits++;
    }
    if (digits == 0) return -1;
    return code;
}

/*
 * Case-insensitively test whether the header line that starts at hdr[off]
 * begins with `name` (which must include the trailing ':' e.g. "location:").
 * On match returns the offset of the first value byte AFTER the colon and any
 * leading spaces/tabs; otherwise returns -1.
 */
static int header_match(const char *hdr, int off, int end, const char *name)
{
    int i = off;
    int k = 0;
    while (name[k]) {
        if (i >= end) return -1;
        if (h_tolower((unsigned char)hdr[i]) != h_tolower((unsigned char)name[k]))
            return -1;
        i++; k++;
    }
    /* skip optional whitespace after the colon */
    while (i < end && (hdr[i] == ' ' || hdr[i] == '\t')) i++;
    return i;
}

/* Copy the header value starting at hdr[val] up to (not including) CR/LF into
 * dst[0..dcap), NUL-terminating.  Trailing whitespace is trimmed.  Returns the
 * number of value bytes stored. */
static int header_value(const char *hdr, int val, int end, char *dst, int dcap)
{
    int n = 0;
    while (val < end && hdr[val] != '\r' && hdr[val] != '\n') {
        if (n < dcap - 1) dst[n++] = hdr[val];
        val++;
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) n--;
    if (dcap > 0) dst[n] = '\0';
    return n;
}

/* Case-insensitive substring presence test (used for "chunked" / "gzip"). */
static int ci_contains(const char *hay, const char *needle)
{
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] &&
               h_tolower((unsigned char)hay[i + j]) ==
               h_tolower((unsigned char)needle[j])) {
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

/* Parse an unsigned decimal from a NUL-terminated string as `long`.  Stops
 * at the first non-digit.  Returns the value (bounded by the digit loop at
 * 20 digits, which is safe for any 64-bit value). */
static long parse_long(const char *s)
{
    long v = 0;
    int  guard = 0;
    while (*s >= '0' && *s <= '9' && guard < 20) {
        v = v * 10 + (long)(*s - '0');
        s++; guard++;
    }
    return v;
}

/* ---- response metadata ------------------------------------------------- */

struct http_meta {
    int           status;        /* parsed status code, or -1               */
    int           chunked;       /* Transfer-Encoding: chunked              */
    int           has_clen;      /* Content-Length present                  */
    long          clen;          /* Content-Length value (as long)          */
    int           enc;           /* 0 none, 1 gzip, 2 deflate               */
    char          location[HTTP_PATH_MAX + HTTP_HOST_MAX]; /* Location value */
    int           has_location;
    int           keep_alive;    /* 1 = connection may be reused            */
};

/*
 * Walk the header block hdr[0..hdr_end) (where hdr_end == body_off, i.e. just
 * past the terminating CRLFCRLF) and fill *m.  Each header line is matched
 * case-insensitively by name.
 *
 * HTTP/1.1 default: keep-alive UNLESS "Connection: close" is sent.
 * HTTP/1.0 default: close     UNLESS "Connection: keep-alive" is sent.
 */
static void parse_headers(const char *hdr, int hdr_end, struct http_meta *m)
{
    m->status       = parse_status(hdr, hdr_end);
    m->chunked      = 0;
    m->has_clen     = 0;
    m->clen         = 0;
    m->enc          = 0;
    m->has_location = 0;
    m->location[0]  = '\0';
    m->keep_alive   = 0;   /* default: decide after checking version + header */

    /* Detect HTTP version from the status line to set keep-alive default. */
    int is_11 = 0;
    /* Status line starts at offset 0: "HTTP/1.1 ..." */
    if (hdr_end >= 8 &&
        hdr[0] == 'H' && hdr[1] == 'T' && hdr[2] == 'T' && hdr[3] == 'P' &&
        hdr[4] == '/' && hdr[5] == '1' && hdr[6] == '.' && hdr[7] == '1') {
        is_11 = 1;
    }
    /* HTTP/1.1 default = keep-alive; HTTP/1.0 default = close */
    m->keep_alive = is_11 ? 1 : 0;

    /* advance past the status line */
    int i = 0;
    while (i < hdr_end && hdr[i] != '\n') i++;
    if (i < hdr_end) i++;   /* now at first header line */

    int guard = 0;
    while (i < hdr_end && guard < 512) {
        guard++;
        /* end of headers? (an empty line) */
        if (hdr[i] == '\r' || hdr[i] == '\n') break;

        int v;
        char val[HTTP_PATH_MAX + HTTP_HOST_MAX];

        if ((v = header_match(hdr, i, hdr_end, "content-length:")) >= 0) {
            header_value(hdr, v, hdr_end, val, (int)sizeof(val));
            m->clen     = parse_long(val);
            m->has_clen = 1;
        } else if ((v = header_match(hdr, i, hdr_end, "transfer-encoding:")) >= 0) {
            header_value(hdr, v, hdr_end, val, (int)sizeof(val));
            /* "identity" means no special encoding -- treat as plain */
            if (ci_contains(val, "chunked")) m->chunked = 1;
            /* "identity" explicitly: leave chunked = 0 (already 0) */
        } else if ((v = header_match(hdr, i, hdr_end, "content-encoding:")) >= 0) {
            header_value(hdr, v, hdr_end, val, (int)sizeof(val));
            if (ci_contains(val, "gzip"))         m->enc = 1;
            else if (ci_contains(val, "deflate"))  m->enc = 2;
        } else if ((v = header_match(hdr, i, hdr_end, "location:")) >= 0) {
            header_value(hdr, v, hdr_end, m->location, (int)sizeof(m->location));
            if (m->location[0]) m->has_location = 1;
        } else if ((v = header_match(hdr, i, hdr_end, "connection:")) >= 0) {
            header_value(hdr, v, hdr_end, val, (int)sizeof(val));
            if (ci_contains(val, "close"))
                m->keep_alive = 0;
            else if (ci_contains(val, "keep-alive"))
                m->keep_alive = 1;
        }

        /* skip to next line */
        while (i < hdr_end && hdr[i] != '\n') i++;
        if (i < hdr_end) i++;
    }
}

/* ---- chunked transfer decoder ------------------------------------------ */

/* Decode a "Transfer-Encoding: chunked" body in src[0..src_len) into
 * dst[0..dcap).  Returns the number of decoded bytes (>= 0), bounded by dcap.
 * Fully bounded; tolerates a truncated final chunk.
 *
 * IN-PLACE SAFE: dst may alias src (dst == src).  De-chunking is strictly
 * non-expanding and the write cursor `out` never overtakes the read cursor `i`
 * (each iteration advances `i` past the size line + CRLF before copying, and
 * copies cp <= take bytes), so the forward memcpy always has dst < src. */
static long dechunk(const unsigned char *src, long src_len,
                    unsigned char *dst, long dcap)
{
    long i = 0;
    long out = 0;
    int  guard = 0;

    while (i < src_len && guard < 1000000) {
        guard++;

        /* parse hex chunk size (until ';' extension or CRLF) */
        unsigned long sz = 0;
        int saw = 0;
        while (i < src_len) {
            int c = src[i];
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = (sz << 4) | (unsigned long)d;
            i++; saw = 1;
        }
        if (!saw) break;                 /* malformed -- stop gracefully */

        /* skip chunk extensions up to CRLF */
        while (i < src_len && src[i] != '\n') i++;
        if (i < src_len) i++;            /* consume the '\n' */

        if (sz == 0) break;              /* terminating chunk */

        /* copy chunk data (bounded by both src and dst) */
        long avail = src_len - i;
        long take  = (long)sz < avail ? (long)sz : avail;
        if (take < 0) take = 0;
        long room  = dcap - out;
        long cp    = take < room ? take : room;
        if (cp > 0) {
            h_memcpy(dst + out, src + i, (unsigned long)cp);
            out += cp;
        }
        i += take;

        if (out >= dcap) break;          /* destination full */

        /* skip the trailing CRLF after the chunk data */
        if (i < src_len && src[i] == '\r') i++;
        if (i < src_len && src[i] == '\n') i++;
    }

    return out;
}

/* ---- keep-alive cache helpers ------------------------------------------ */

/* Drop any cached keep-alive connection (close fd if still open). */
static void ka_evict(void)
{
    if (g_ka_valid) {
        netconn_close(&g_ka_nc);
        g_ka_valid    = 0;
        g_ka_host[0]  = '\0';
        g_ka_port     = 0;
        g_ka_tls      = 0;
        g_ka_idle     = 0;
    }
}

/*
 * Attempt to retrieve a reusable connection from the keep-alive cache for
 * host:port:is_tls.  On success copies the cached netconn into *nc and
 * invalidates the cache entry, returning 1.  On failure (no match, expired,
 * or empty) returns 0 and the caller must open a fresh connection.
 */
static int ka_get(netconn *nc, const char *host, unsigned short port, int is_tls)
{
    if (!g_ka_valid) return 0;

    /* host / port / tls mismatch => evict and fall through */
    if (g_ka_port != port || g_ka_tls != is_tls ||
        h_strcmp(g_ka_host, host) != 0) {
        ka_evict();
        return 0;
    }

    /* idle timeout check */
    unsigned long now   = sc_ticks_ms();
    unsigned long delta = now - g_ka_idle; /* wraps correctly for unsigned */
    if (delta >= HTTP_KA_IDLE_MS) {
        ka_evict();
        return 0;
    }

    /* hand the connection to the caller */
    h_memcpy(nc, &g_ka_nc, sizeof(netconn));
    g_ka_valid = 0;   /* cache is now empty; caller owns the fd */
    return 1;
}

/*
 * Stash the open connection *nc into the keep-alive cache for host:port:tls.
 * Any pre-existing cached connection (to a different host, or the same host
 * from a previous request) is closed first.
 */
static void ka_put(netconn *nc, const char *host, unsigned short port, int is_tls)
{
    ka_evict();   /* drop previous entry if any */

    h_memcpy(&g_ka_nc, nc, sizeof(netconn));

    int hn = 0;
    while (host[hn] && hn < HTTP_HOST_MAX - 1) { g_ka_host[hn] = host[hn]; hn++; }
    g_ka_host[hn] = '\0';

    g_ka_port  = port;
    g_ka_tls   = is_tls;
    g_ka_idle  = sc_ticks_ms();
    g_ka_valid = 1;
}

/* ---- one HTTP request/response round-trip ------------------------------ */

/*
 * http_fetch_raw -- open (or reuse a cached) connection to host:port, send a
 * complete HTTP request, receive the full raw response into raw[], and parse
 * the response metadata.
 *
 *   host, port, use_tls -- target (after any redirect-chain resolution)
 *   req_extra  -- optional extra CRLF-terminated header lines to inject
 *                 between the Host header and the final blank line.
 *                 May be NULL or "" (treated as empty).
 *   raw        -- output buffer for the raw response (status + hdrs + body)
 *   raw_cap    -- capacity of raw[]
 *   p_body_off -- receives the body-start offset within raw[], or -1
 *   meta       -- receives parsed response metadata
 *   deadline   -- absolute SYS_GET_TICKS_MS timestamp past which the send and
 *                 receive loops abort (0 = no deadline).  Shared across the
 *                 whole http_do_get call so every hop draws on one budget.
 *
 * Returns the number of raw bytes captured (>= 0) on success, or an
 * HTTP_ERR_* code (< 0) on failure (HTTP_ERR_TIMEO on deadline expiry).
 *
 * After this function returns the file-scope g_nc holds the open connection
 * UNLESS an error (< 0) was returned, in which case g_nc has already been
 * closed.  On success the caller is responsible for either caching it (ka_put)
 * or closing it (netconn_close) based on meta->keep_alive.
 */
static long http_fetch_raw(const char *host, unsigned short port,
                           const char *path, int use_tls,
                           const char *req_extra,
                           const char *method,
                           const unsigned char *body, long body_len,
                           unsigned char *raw, long raw_cap,
                           int *p_body_off, struct http_meta *meta,
                           unsigned long deadline)
{
    *p_body_off = -1;

    /* Bound DNS + connect (+ TLS handshake) inside the netconn layer with the
     * same wall-clock budget, then bail immediately if it already elapsed. */
    netconn_set_deadline(deadline);

    /* ---- try to reuse a cached connection ---- */
    int reused = ka_get(&g_nc, host, port, use_tls);
    if (!reused) {
        if (netconn_open(&g_nc, host, port, use_tls) != 0) {
            netconn_set_deadline(0);
            return HTTP_ERR_CONNECT;
        }
    }

    /* Connect/handshake may have consumed the whole budget. */
    if (deadline_passed(deadline)) {
        netconn_close(&g_nc);
        netconn_set_deadline(0);
        return HTTP_ERR_TIMEO;
    }

    /* ---- build the request ---- */
    /*
     * We send HTTP/1.1 to negotiate keep-alive by default.  We advertise
     * "Connection: keep-alive" explicitly so that both 1.0 and 1.1 servers
     * understand the preference.  If the caller supplies extra headers
     * (e.g. a Range header) they are injected here.
     */
    /* Header block (request line + Host/Accept-Encoding/Connection + caller's
     * req_extra such as a long Authorization: Bearer token). The request BODY is
     * sent separately below, so this only needs to hold headers. 4 KiB leaves
     * room for ~2 KiB bearer tokens. */
    char req[4096];
    int  rp = 0;
    buf_puts(req, &rp, (int)sizeof(req), (method && method[0]) ? method : "GET");
    buf_puts(req, &rp, (int)sizeof(req), " ");
    buf_puts(req, &rp, (int)sizeof(req), path);
    buf_puts(req, &rp, (int)sizeof(req), " HTTP/1.1\r\nHost: ");
    buf_puts(req, &rp, (int)sizeof(req), host);
    buf_puts(req, &rp, (int)sizeof(req),
             "\r\nAccept-Encoding: gzip, deflate\r\n"
             "Connection: keep-alive\r\n");
    if (req_extra && req_extra[0]) {
        buf_puts(req, &rp, (int)sizeof(req), req_extra);
    }
    /* POST/PUT body: advertise its length so the server reads exactly it; the
     * body bytes are sent right after the header block (see below). */
    if (body && body_len > 0) {
        buf_puts(req, &rp, (int)sizeof(req), "Content-Length: ");
        buf_putulong(req, &rp, (int)sizeof(req), (unsigned long)body_len);
        buf_puts(req, &rp, (int)sizeof(req), "\r\n");
    }
    buf_puts(req, &rp, (int)sizeof(req), "\r\n");
    int reqlen = rp;

    /* ---- send the whole request ---- */
    {
        int off = 0, guard = 0;
        while (off < reqlen) {
            long s = netconn_write(&g_nc, req + off,
                                   (unsigned long)(reqlen - off));
            if (s > 0) {
                off += (int)s;
                guard = 0;
            } else if (s == EAGAIN_NEG) {
                /* Abort on deadline so a stalled send cannot hang the caller. */
                if (deadline_passed(deadline)) {
                    netconn_close(&g_nc);
                    netconn_set_deadline(0);
                    return HTTP_ERR_TIMEO;
                }
                sc_yield();
                if (++guard > HTTP_POLL_MAX) {
                    netconn_close(&g_nc);
                    netconn_set_deadline(0);
                    return HTTP_ERR_SEND;
                }
            } else {
                netconn_close(&g_nc);
                netconn_set_deadline(0);
                return HTTP_ERR_SEND;
            }
        }
    }

    /* ---- send the request body (POST/PUT) right after the header block ---- */
    if (body && body_len > 0) {
        long boff = 0; int bguard = 0;
        while (boff < body_len) {
            long s = netconn_write(&g_nc, body + boff,
                                   (unsigned long)(body_len - boff));
            if (s > 0) {
                boff += (int)s;
                bguard = 0;
            } else if (s == EAGAIN_NEG) {
                if (deadline_passed(deadline)) {
                    netconn_close(&g_nc);
                    netconn_set_deadline(0);
                    return HTTP_ERR_TIMEO;
                }
                sc_yield();
                if (++bguard > HTTP_POLL_MAX) {
                    netconn_close(&g_nc);
                    netconn_set_deadline(0);
                    return HTTP_ERR_SEND;
                }
            } else {
                netconn_close(&g_nc);
                netconn_set_deadline(0);
                return HTTP_ERR_SEND;
            }
        }
    }

    /* ---- receive loop: stream straight into raw[], stop on close, on full
     *      buffer, or once we have all the bytes Content-Length promised. ---- */
    char chunk[1024];
    long raw_len  = 0;
    int  body_off = -1;
    int  have_meta = 0;

    int timed_out = 0;
    for (int it = 0; it < HTTP_POLL_MAX; it++) {
        /* Wall-clock deadline check (covers the TLS path too, whose internal
         * netconn_read bound is independent of our budget). */
        if (deadline_passed(deadline)) { timed_out = 1; break; }

        long n = netconn_read(&g_nc, chunk, (unsigned long)sizeof(chunk));

        if (n == 0)          break;        /* peer closed + drained */
        if (n == EAGAIN_NEG) { sc_yield(); continue; }
        if (n == NC_ERR_TIMEDOUT) { timed_out = 1; break; } /* deadline in netconn */
        if (n < 0)           break;        /* hard error -> stop, parse what we have */

        long room = raw_cap - raw_len;
        long take = n < room ? n : room;
        if (take > 0) {
            h_memcpy(raw + raw_len, chunk, (unsigned long)take);
            raw_len += take;
        }

        /* locate header terminator once (bounded scan window) */
        if (body_off < 0) {
            int scan = raw_len < HTTP_HDR_MAX ? (int)raw_len : HTTP_HDR_MAX;
            int bs = find_body_start((const char *)raw, scan);
            if (bs >= 0) {
                body_off = bs;
                parse_headers((const char *)raw, body_off, meta);
                have_meta = 1;
            } else if (raw_len >= HTTP_HDR_MAX) {
                break;                     /* headers too large -- bail */
            }
        }

        /* Content-Length fast stop: once the full body is in, stop reading
         * without waiting for the peer to close.  (Skip when chunked.) */
        if (have_meta && meta->has_clen && !meta->chunked && body_off >= 0) {
            long got_body = raw_len - body_off;
            if (got_body >= meta->clen) break;
        }

        if (raw_len >= raw_cap) break;     /* raw buffer full */
    }

    /* The netconn-layer deadline only applies to this round-trip; clear it so a
     * later reuse of g_nc (or another caller of netconn_read) is not surprised. */
    netconn_set_deadline(0);

    /* Deadline hit with NOTHING usable received: clean, distinct timeout so the
     * caller falls back to its error page.  If we DID receive bytes we fall
     * through and best-effort parse them (a partial page is better than none). */
    if (timed_out && raw_len == 0) {
        netconn_close(&g_nc);
        return HTTP_ERR_TIMEO;
    }

    /* If we never found the header separator, best-effort parse whatever we got. */
    if (!have_meta) {
        if (raw_len > 0) {
            int scan = raw_len < HTTP_HDR_MAX ? (int)raw_len : HTTP_HDR_MAX;
            meta->status       = parse_status((const char *)raw, scan);
            meta->chunked      = 0;
            meta->has_clen     = 0;
            meta->clen         = 0;
            meta->enc          = 0;
            meta->has_location = 0;
            meta->location[0]  = '\0';
            meta->keep_alive   = 0;
        } else {
            netconn_close(&g_nc);
            return HTTP_ERR_TIMEO;
        }
    }

    *p_body_off = body_off;
    return raw_len;
}

/* ---- URL (Location) parsing -------------------------------------------- */

/*
 * Resolve `loc` (a Location header value) relative to the current cur_host /
 * cur_port into new_host / new_port / new_path, and record the target scheme.
 *
 * Handled forms:
 *   "https://host[:port]/path"  -> returns 1, *new_tls = 1, default port 443
 *   "http://host[:port]/path"   -> returns 0, *new_tls = 0, default port 80
 *   "/path"                     -> returns 0, reuse current host/port/tls
 *   "relative"                  -> returns 0, coerced to root-relative
 *
 * Returns:
 *    0  -- followable (http or scheme-inherited)
 *    1  -- followable over TLS (https)
 *   -1  -- parse failure (do not follow)
 *
 * All copies are bounded.  *new_tls is set on every non-error return.
 */
static int resolve_location(const char *loc,
                            const char *cur_host, unsigned short cur_port,
                            int cur_tls,
                            char *new_host, unsigned short *new_port,
                            char *new_path, int *new_tls)
{
    const char *p = loc;

    /* "https://" -> followable over TLS */
    if ((h_tolower(p[0]) == 'h') && (h_tolower(p[1]) == 't') &&
        (h_tolower(p[2]) == 't') && (h_tolower(p[3]) == 'p') &&
        (h_tolower(p[4]) == 's') && p[5] == ':' && p[6] == '/' && p[7] == '/') {

        p += 8;   /* skip "https://" */

        int hn = 0;
        while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' &&
               hn < HTTP_HOST_MAX - 1) {
            new_host[hn++] = *p++;
        }
        new_host[hn] = '\0';
        if (hn == 0) return -1;

        unsigned short port = 443;
        if (*p == ':') {
            p++;
            unsigned int v = 0; int g = 0;
            while (*p >= '0' && *p <= '9' && g < 6) {
                v = v * 10 + (unsigned int)(*p - '0'); p++; g++;
            }
            if (v == 0 || v > 65535) port = 443; else port = (unsigned short)v;
        }
        *new_port = port;

        int pn = 0;
        if (*p == '/' || *p == '?') {
            while (*p && *p != '#' && pn < HTTP_PATH_MAX - 1) new_path[pn++] = *p++;
        }
        if (pn == 0) { new_path[pn++] = '/'; }
        new_path[pn] = '\0';

        *new_tls = 1;
        return 1;
    }

    /* "http://" -> absolute, plain TCP */
    if ((h_tolower(p[0]) == 'h') && (h_tolower(p[1]) == 't') &&
        (h_tolower(p[2]) == 't') && (h_tolower(p[3]) == 'p') &&
        p[4] == ':' && p[5] == '/' && p[6] == '/') {

        p += 7;   /* skip "http://" */

        int hn = 0;
        while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' &&
               hn < HTTP_HOST_MAX - 1) {
            new_host[hn++] = *p++;
        }
        new_host[hn] = '\0';
        if (hn == 0) return -1;

        unsigned short port = 80;
        if (*p == ':') {
            p++;
            unsigned int v = 0; int g = 0;
            while (*p >= '0' && *p <= '9' && g < 6) {
                v = v * 10 + (unsigned int)(*p - '0'); p++; g++;
            }
            if (v == 0 || v > 65535) port = 80; else port = (unsigned short)v;
        }
        *new_port = port;

        int pn = 0;
        if (*p == '/' || *p == '?') {
            while (*p && *p != '#' && pn < HTTP_PATH_MAX - 1) new_path[pn++] = *p++;
        }
        if (pn == 0) { new_path[pn++] = '/'; }
        new_path[pn] = '\0';

        *new_tls = 0;
        return 0;
    }

    /* relative -- reuse current host, port, and TLS flag. */
    {
        int hn = 0;
        while (cur_host[hn] && hn < HTTP_HOST_MAX - 1) { new_host[hn] = cur_host[hn]; hn++; }
        new_host[hn] = '\0';
        *new_port = cur_port;
        *new_tls  = cur_tls;

        int pn = 0;
        if (loc[0] != '/') new_path[pn++] = '/';   /* coerce to root-relative */
        const char *q = loc;
        while (*q && *q != '#' && pn < HTTP_PATH_MAX - 1) new_path[pn++] = *q++;
        if (pn == 0) new_path[pn++] = '/';
        new_path[pn] = '\0';
        return 0;
    }
}

/* ---- body post-processing (dechunk + inflate) -------------------------- */

/*
 * Take the raw body raw[body_off .. raw_len) plus parsed *meta and produce the
 * final body in out_body[0..out_cap).  Order: de-chunk (if chunked) IN PLACE
 * over the raw body region, then inflate (if gzip/deflate) into out_body --
 * otherwise straight copy.  On any inflate failure falls back to the
 * (possibly de-chunked) bytes.  Returns the number of bytes written.
 *
 * `raw` is mutable because the in-place de-chunk rewrites the body region.
 */
static long finalize_body(unsigned char *raw, long raw_len, int body_off,
                          const struct http_meta *meta,
                          char *out_body, unsigned long out_cap,
                          unsigned int flags)
{
    if (out_cap == 0 || body_off < 0 || body_off > raw_len) return 0;

    unsigned char *body = raw + body_off;
    long body_len = raw_len - body_off;
    if (body_len < 0) body_len = 0;

    /* If Content-Length is known and smaller than what we captured, trust it. */
    if (meta->has_clen && !meta->chunked &&
        meta->clen >= 0 && meta->clen < body_len) {
        body_len = meta->clen;
    }

    const unsigned char *src = body;
    long src_len = body_len;

    if (meta->chunked) {
        /* In-place de-chunk: dst == src is safe (see dechunk contract). */
        long dl = dechunk(body, body_len, body, body_len);
        src     = body;
        src_len = dl;
    }

    int want_decode = !(flags & HTTP_F_NO_DECODE) && meta->enc != 0;

    if (want_decode) {
        const unsigned char *zin = src;
        long zin_len = src_len;

        if (meta->enc == 1) {
            /* gzip container: 10-byte header + 8-byte trailer around DEFLATE.
             * Validate magic + skip optional extra fields conservatively. */
            if (zin_len >= 18 && zin[0] == 0x1f && zin[1] == 0x8b && zin[2] == 8) {
                unsigned int flg = zin[3];
                long hoff = 10;
                /* FEXTRA */
                if ((flg & 0x04) && hoff + 2 <= zin_len) {
                    unsigned int xlen = (unsigned int)zin[hoff] |
                                        ((unsigned int)zin[hoff + 1] << 8);
                    hoff += 2 + (long)xlen;
                }
                /* FNAME */
                if ((flg & 0x08)) { while (hoff < zin_len && zin[hoff]) hoff++; if (hoff < zin_len) hoff++; }
                /* FCOMMENT */
                if ((flg & 0x10)) { while (hoff < zin_len && zin[hoff]) hoff++; if (hoff < zin_len) hoff++; }
                /* FHCRC */
                if ((flg & 0x02)) hoff += 2;

                long deflate_len = zin_len - hoff - 8;   /* drop CRC32+ISIZE */
                if (hoff > 0 && hoff < zin_len && deflate_len > 0) {
                    long r = inflate_decompress(zin + hoff, deflate_len,
                                                (unsigned char *)out_body,
                                                (long)out_cap);
                    if (r >= 0) return r;     /* inflated OK */
                }
            }
            /* fall through -> graceful raw fallback below */
        } else if (meta->enc == 2) {
            /* "deflate" -- per RFC this is a zlib stream, but many servers send
             * raw DEFLATE.  Try raw first; if that fails, retry after skipping a
             * 2-byte zlib header. */
            long r = inflate_decompress(zin, zin_len,
                                        (unsigned char *)out_body, (long)out_cap);
            if (r >= 0) return r;
            if (zin_len > 2) {
                r = inflate_decompress(zin + 2, zin_len - 2,
                                       (unsigned char *)out_body, (long)out_cap);
                if (r >= 0) return r;
            }
            /* fall through -> graceful raw fallback below */
        }
        /* decode failed: fall back to the compressed (or de-chunked) bytes */
    }

    /* plain copy of src (de-chunked or original), bounded by out_cap */
    long cp = src_len < (long)out_cap ? src_len : (long)out_cap;
    if (cp < 0) cp = 0;
    h_memcpy(out_body, src, (unsigned long)cp);
    return cp;
}

/* ---- internal fetch with optional extra headers ------------------------- */

/*
 * Core GET implementation shared by http_get_ex and http_get_range.
 *
 *   extra_hdr  -- additional HTTP header lines to insert (already CRLF-
 *                 terminated, as a single string).  NULL or "" = none.
 *   timeout_ms -- total wall-clock budget for the WHOLE call (DNS + connect +
 *                 every redirect hop's transfer).  0 selects HTTP_TIMEOUT_MS.
 */
static long http_do_get(const char *host, unsigned short port, const char *path,
                        char *out_body, unsigned long out_cap, int *out_status,
                        int max_redirects, unsigned int flags,
                        const char *extra_hdr, unsigned long timeout_ms,
                        const char *method,
                        const unsigned char *body, long body_len)
{
    if (out_status) *out_status = 0;
    if (!host || !path || !out_body) return HTTP_ERR_INVAL;

    /* Compute the absolute deadline ONCE: all redirect hops share this budget,
     * so a redirect chain to slow hosts cannot multiply the wait.  A timeout of
     * 0 means "use the default"; the resulting `deadline` is always non-zero so
     * deadline_passed() / netconn_set_deadline() treat it as active. */
    if (timeout_ms == 0) timeout_ms = HTTP_TIMEOUT_MS;
    unsigned long deadline = sc_ticks_ms() + timeout_ms;
    if (deadline == 0) deadline = 1;   /* 0 is the "disabled" sentinel; avoid it */

    /* working copies of the current target (mutated as we follow redirects) */
    char cur_host[HTTP_HOST_MAX];
    char cur_path[HTTP_PATH_MAX];
    unsigned short cur_port = port;
    int  cur_tls  = (flags & HTTP_F_TLS) ? 1 : 0;
    {
        int i = 0;
        while (host[i] && i < HTTP_HOST_MAX - 1) { cur_host[i] = host[i]; i++; }
        cur_host[i] = '\0';
        int j = 0;
        while (path[j] && j < HTTP_PATH_MAX - 1) { cur_path[j] = path[j]; j++; }
        cur_path[j] = '\0';
    }

    /* one big static scratch for the raw response (avoids huge stack frames) */
    static unsigned char raw[HTTP_RAW_MAX];

    int hops = 0;
    if (max_redirects < 0) max_redirects = 0;
    if (flags & HTTP_F_NO_REDIRECT) max_redirects = 0;

    for (;;) {
        struct http_meta meta;
        int  body_off = -1;

        /* On redirect hops after the first, do not send the Range header --
         * range requests follow redirects to the final URL and start fresh. */
        const char *hdr_to_send = (hops == 0) ? extra_hdr : (const char *)0;
        /* Method + body apply to the FIRST hop only; a redirect is re-issued as
         * a bodyless GET (the safe 302->GET default; the OAuth POSTs do not
         * redirect). */
        const char          *m_send  = (hops == 0) ? method : (const char *)0;
        const unsigned char *b_send  = (hops == 0) ? body : (const unsigned char *)0;
        long                 bl_send = (hops == 0) ? body_len : 0;

        long raw_len = http_fetch_raw(cur_host, cur_port, cur_path, cur_tls,
                                      hdr_to_send, m_send, b_send, bl_send,
                                      raw, (long)sizeof(raw),
                                      &body_off, &meta, deadline);
        if (raw_len < 0) return raw_len;        /* propagate HTTP_ERR_* */

        int status = meta.status;

        /* ---- keep-alive cache management ---- */
        if (meta.keep_alive) {
            /* Stash the open g_nc into the cache for next time. */
            ka_put(&g_nc, cur_host, cur_port, cur_tls);
        } else {
            netconn_close(&g_nc);
        }

        /* ---- redirect handling ---- */
        int is_redirect = (status == 301 || status == 302 || status == 303 ||
                           status == 307 || status == 308);

        if (is_redirect && meta.has_location && hops < max_redirects) {
            char nh[HTTP_HOST_MAX];
            char np[HTTP_PATH_MAX];
            unsigned short nport = cur_tls ? 443 : 80;
            int ntls = cur_tls;

            int rr = resolve_location(meta.location,
                                      cur_host, cur_port, cur_tls,
                                      nh, &nport, np, &ntls);

            if (rr == 0 || rr == 1) {
                /* followable (plain or TLS) -- adopt the new target and loop */
                int i = 0; while (nh[i]) { cur_host[i] = nh[i]; i++; } cur_host[i] = '\0';
                int j = 0; while (np[j]) { cur_path[j] = np[j]; j++; } cur_path[j] = '\0';
                cur_port = nport;
                cur_tls  = ntls;
                hops++;
                continue;
            }
            /* rr < 0: unparseable Location -- fall through and return this body */
        }

        /* ---- terminal response: decode + copy body out ---- */
        long n = finalize_body(raw, raw_len, body_off, &meta,
                               out_body, out_cap, flags);

        if (out_status) *out_status = (status < 0) ? 0 : status;
        return n;
    }
}

/* ---- public API --------------------------------------------------------- */

long http_get_ex(const char *host, unsigned short port, const char *path,
                 char *out_body, unsigned long out_cap, int *out_status,
                 int max_redirects, unsigned int flags)
{
    return http_do_get(host, port, path, out_body, out_cap, out_status,
                       max_redirects, flags, (const char *)0,
                       0 /* default timeout */,
                       "GET", (const unsigned char *)0, 0);
}

long http_get_timeout_ex(const char *host, unsigned short port, const char *path,
                         char *out_body, unsigned long out_cap, int *out_status,
                         int max_redirects, unsigned int flags,
                         unsigned long timeout_ms)
{
    return http_do_get(host, port, path, out_body, out_cap, out_status,
                       max_redirects, flags, (const char *)0, timeout_ms,
                       "GET", (const unsigned char *)0, 0);
}

long http_get(const char *host, unsigned short port, const char *path,
              char *out_body, unsigned long out_cap, int *out_status)
{
    return http_get_ex(host, port, path, out_body, out_cap, out_status,
                       HTTP_MAX_REDIRECTS, 0);
}

long https_get(const char *host, unsigned short port, const char *path,
               char *out_body, unsigned long out_cap, int *out_status)
{
    return http_get_ex(host, port, path, out_body, out_cap, out_status,
                       HTTP_MAX_REDIRECTS, HTTP_F_TLS);
}

long http_get_range(const char *host, unsigned short port, const char *path,
                    unsigned long offset, unsigned long length,
                    char *out_body, unsigned long out_cap, int *out_status)
{
    if (!host || !path || !out_body) return HTTP_ERR_INVAL;

    /*
     * Build "Range: bytes=<offset>-<last>\r\n" into a small local buffer.
     * last = offset + length - 1.
     *
     * Request line format:  Range: bytes=<first-byte-pos>-<last-byte-pos>
     * (RFC 7233 §2.1 byte-range-spec)
     */
    char range_hdr[64];
    int  rp = 0;
    buf_puts(range_hdr, &rp, (int)sizeof(range_hdr), "Range: bytes=");
    buf_putulong(range_hdr, &rp, (int)sizeof(range_hdr), offset);
    buf_puts(range_hdr, &rp, (int)sizeof(range_hdr), "-");
    buf_putulong(range_hdr, &rp, (int)sizeof(range_hdr),
                 (length > 0) ? offset + length - 1 : offset);
    buf_puts(range_hdr, &rp, (int)sizeof(range_hdr), "\r\n");
    if (rp >= (int)sizeof(range_hdr) - 1) return HTTP_ERR_INVAL;
    range_hdr[rp] = '\0';

    return http_do_get(host, port, path, out_body, out_cap, out_status,
                       HTTP_MAX_REDIRECTS, 0 /* plain HTTP */, range_hdr,
                       0 /* default timeout */,
                       "GET", (const unsigned char *)0, 0);
}

long https_get_range(const char *host, unsigned short port, const char *path,
                     unsigned long offset, unsigned long length,
                     char *out_body, unsigned long out_cap, int *out_status)
{
    if (!host || !path || !out_body) return HTTP_ERR_INVAL;

    char range_hdr[64];
    int  rp = 0;
    buf_puts(range_hdr, &rp, (int)sizeof(range_hdr), "Range: bytes=");
    buf_putulong(range_hdr, &rp, (int)sizeof(range_hdr), offset);
    buf_puts(range_hdr, &rp, (int)sizeof(range_hdr), "-");
    buf_putulong(range_hdr, &rp, (int)sizeof(range_hdr),
                 (length > 0) ? offset + length - 1 : offset);
    buf_puts(range_hdr, &rp, (int)sizeof(range_hdr), "\r\n");
    if (rp >= (int)sizeof(range_hdr) - 1) return HTTP_ERR_INVAL;
    range_hdr[rp] = '\0';

    return http_do_get(host, port, path, out_body, out_cap, out_status,
                       HTTP_MAX_REDIRECTS, HTTP_F_TLS, range_hdr,
                       0 /* default timeout */,
                       "GET", (const unsigned char *)0, 0);
}

/* ---- HTTP/HTTPS request with method + body + extra headers (POST, etc.) -- */

/*
 * http_request -- general request: arbitrary method, optional request body, and
 * optional extra request headers (already CRLF-terminated, e.g.
 * "Authorization: Bearer X\r\n").  For a body, content_type is sent as the
 * Content-Type header and Content-Length is added automatically.  flags takes
 * the HTTP_F_* bits (HTTP_F_TLS for HTTPS).  Redirects are NOT followed for a
 * non-GET request (OAuth endpoints return their JSON directly); pass method
 * "GET" to get the normal redirect-following behaviour.  Returns body bytes
 * (>= 0) or an HTTP_ERR_* code.  Fully bounded.
 */
long http_request(const char *host, unsigned short port, const char *path,
                  const char *method, const char *extra_headers,
                  const char *content_type, const unsigned char *body,
                  long body_len, unsigned int flags,
                  char *out_body, unsigned long out_cap, int *out_status)
{
    if (!host || !path || !out_body) return HTTP_ERR_INVAL;

    /* Assemble the extra-header block: Content-Type (when a body is present)
     * followed by any caller-supplied header lines.  Both are already / will be
     * CRLF-terminated. */
    char hdr[2048];
    int  hp = 0;
    if (body && body_len > 0 && content_type && content_type[0]) {
        buf_puts(hdr, &hp, (int)sizeof(hdr), "Content-Type: ");
        buf_puts(hdr, &hp, (int)sizeof(hdr), content_type);
        buf_puts(hdr, &hp, (int)sizeof(hdr), "\r\n");
    }
    if (extra_headers && extra_headers[0]) {
        buf_puts(hdr, &hp, (int)sizeof(hdr), extra_headers);
    }
    if (hp >= (int)sizeof(hdr) - 1) return HTTP_ERR_INVAL;
    hdr[hp] = '\0';

    int is_get = !method || !method[0] ||
                 (method[0] == 'G' && method[1] == 'E' && method[2] == 'T' && method[3] == 0);
    int max_redir = is_get ? HTTP_MAX_REDIRECTS : 0;

    return http_do_get(host, port, path, out_body, out_cap, out_status,
                       max_redir, flags, (hp > 0) ? hdr : (const char *)0,
                       0 /* default timeout */,
                       (method && method[0]) ? method : "GET", body, body_len);
}

/* http_post / https_post -- POST a body (plain TCP / TLS).  extra_headers may be
 * NULL.  Convenience wrappers over http_request. */
long http_post(const char *host, unsigned short port, const char *path,
               const char *content_type, const unsigned char *body, long body_len,
               const char *extra_headers,
               char *out_body, unsigned long out_cap, int *out_status)
{
    return http_request(host, port, path, "POST", extra_headers, content_type,
                        body, body_len, 0, out_body, out_cap, out_status);
}

long https_post(const char *host, unsigned short port, const char *path,
                const char *content_type, const unsigned char *body, long body_len,
                const char *extra_headers,
                char *out_body, unsigned long out_cap, int *out_status)
{
    return http_request(host, port, path, "POST", extra_headers, content_type,
                        body, body_len, HTTP_F_TLS, out_body, out_cap, out_status);
}

/* ---- offline self-test ------------------------------------------------- */

/*
 * http_selftest -- offline parser / keep-alive / range self-test.
 *
 * Synthesises HTTP responses in local buffers (no network, no syscalls beyond
 * the parse helpers) and verifies correctness of:
 *   1. Status-code parsing.
 *   2. Content-Length parsing as long (not int) -- uses a value > INT_MAX.
 *   3. Connection: keep-alive detection.
 *   4. Connection: close detection (overrides HTTP/1.1 default).
 *   5. Transfer-Encoding: identity treated as no special encoding.
 *   6. Range header string construction (buf_putulong arithmetic).
 *   7. Body isolation: bytes after \r\n\r\n are the body.
 *
 * Returns 0 on pass, -1 on any check failure.
 */
int http_selftest(void)
{
    /* ---- helper: parse_headers on a synthetic response ---- */

    /* Test 1: HTTP/1.1 200 with Content-Length + Connection: keep-alive */
    {
        const char *resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 13\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "Hello, world!";

        int body_off = find_body_start(resp, h_strlen(resp));
        if (body_off < 0) return -1;   /* no header separator found */

        struct http_meta m;
        parse_headers(resp, body_off, &m);

        if (m.status    != 200)  return -1;
        if (!m.has_clen)         return -1;
        if (m.clen      != 13)   return -1;
        if (!m.keep_alive)       return -1;
        if (m.chunked)           return -1;
        if (m.enc       != 0)    return -1;

        /* Body bytes */
        int body_len = h_strlen(resp) - body_off;
        if (body_len != 13) return -1;
    }

    /* Test 2: HTTP/1.1 with Connection: close */
    {
        const char *resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "\r\n"
            "data";

        int body_off = find_body_start(resp, h_strlen(resp));
        if (body_off < 0) return -1;

        struct http_meta m;
        parse_headers(resp, body_off, &m);

        if (m.status != 200)  return -1;
        if (m.keep_alive)     return -1;   /* must be 0 due to Connection: close */
        if (m.clen   != 4)    return -1;
    }

    /* Test 3: HTTP/1.0 response (default = close) without Connection header */
    {
        const char *resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Length: 2\r\n"
            "\r\n"
            "ok";

        int body_off = find_body_start(resp, h_strlen(resp));
        if (body_off < 0) return -1;

        struct http_meta m;
        parse_headers(resp, body_off, &m);

        if (m.status != 200) return -1;
        if (m.keep_alive)    return -1;   /* HTTP/1.0 default = close */
    }

    /* Test 4: Content-Length as a value larger than INT_MAX (4294967296 == 2^32) */
    {
        const char *resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 4294967296\r\n"
            "Connection: close\r\n"
            "\r\n";

        int body_off = find_body_start(resp, h_strlen(resp));
        if (body_off < 0) return -1;

        struct http_meta m;
        parse_headers(resp, body_off, &m);

        if (!m.has_clen)             return -1;
        if (m.clen != 4294967296L)   return -1;
    }

    /* Test 5: Transfer-Encoding: identity is NOT chunked */
    {
        const char *resp =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: identity\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";

        int body_off = find_body_start(resp, h_strlen(resp));
        if (body_off < 0) return -1;

        struct http_meta m;
        parse_headers(resp, body_off, &m);

        if (m.chunked)   return -1;   /* identity must NOT set chunked */
        if (m.clen != 5) return -1;
    }

    /* Test 6: 206 Partial Content */
    {
        const char *resp =
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Range: bytes 100-199/1000\r\n"
            "Content-Length: 100\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        int body_off = find_body_start(resp, h_strlen(resp));
        if (body_off < 0) return -1;

        struct http_meta m;
        parse_headers(resp, body_off, &m);

        if (m.status    != 206) return -1;
        if (m.clen      != 100) return -1;
        if (!m.keep_alive)      return -1;
    }

    /* Test 7: Range header string construction */
    {
        char   buf[64];
        int    rp = 0;
        buf_puts   (buf, &rp, (int)sizeof(buf), "Range: bytes=");
        buf_putulong(buf, &rp, (int)sizeof(buf), 1024UL);
        buf_puts   (buf, &rp, (int)sizeof(buf), "-");
        buf_putulong(buf, &rp, (int)sizeof(buf), 1024UL + 512UL - 1UL);
        buf_puts   (buf, &rp, (int)sizeof(buf), "\r\n");
        buf[rp] = '\0';

        /* Expected: "Range: bytes=1024-1535\r\n" */
        const char *expected = "Range: bytes=1024-1535\r\n";
        int ok = 1;
        for (int i = 0; expected[i] || buf[i]; i++) {
            if (buf[i] != expected[i]) { ok = 0; break; }
        }
        if (!ok) return -1;
    }

    /* Test 8: header_match is case-insensitive */
    {
        const char *hdr = "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n";
        int body_off = find_body_start(hdr, h_strlen(hdr));
        if (body_off < 0) return -1;

        /* find the "content-length:" header line offset (after status line) */
        int line_off = 0;
        while (hdr[line_off] != '\n') line_off++;
        line_off++;  /* skip the \n */

        /* verify both cases match */
        int v1 = header_match(hdr, line_off, body_off, "content-length:");
        int v2 = header_match(hdr, line_off, body_off, "CONTENT-LENGTH:");
        if (v1 < 0 || v2 < 0) return -1;
        if (v1 != v2)          return -1;
    }

    /* Test 9: bounded-fetch deadline arithmetic (the timeout mechanism).
     * Validates the wrap-safe comparison that bounds the recv/send loops, all
     * offline (no network, no clock dependence -- `now` is supplied directly). */
    {
        /* disabled sentinel: a 0 deadline never trips, regardless of `now` */
        if (deadline_reached(0,          0)) return -1;
        if (deadline_reached(0xFFFFFFFFUL, 0)) return -1;

        /* not yet reached: now strictly before deadline */
        if (deadline_reached(1000, 9000)) return -1;   /* 7 s of budget left   */

        /* exactly reached and just past: must trip */
        if (!deadline_reached(9000, 9000)) return -1;  /* now == deadline      */
        if (!deadline_reached(9001, 9000)) return -1;  /* 1 ms past            */

        /* wrap case: deadline computed near the 49-day tick wrap.  now has
         * wrapped past 0 but is logically AFTER the deadline -> must trip. */
        unsigned long near_wrap = (unsigned long)-50;  /* 50 ms before wrap    */
        unsigned long after     = 30;                  /* wrapped 30 ms past 0 */
        if (!deadline_reached(after, near_wrap)) return -1;

        /* wrap case, not yet reached: now just before a pre-wrap deadline. */
        if (deadline_reached((unsigned long)-100, (unsigned long)-50)) return -1;

        /* end-to-end shape: a deadline 8000 ms in the "future" relative to a
         * synthetic clock is not yet reached, but one in the past is. */
        unsigned long base = 100000UL;
        unsigned long dl   = base + HTTP_TIMEOUT_MS;    /* future deadline     */
        if (deadline_reached(base,          dl)) return -1;  /* fresh: alive    */
        if (!deadline_reached(dl,           dl)) return -1;  /* at budget end   */
        if (!deadline_reached(dl + 1,       dl)) return -1;  /* over budget     */
    }

    return 0;   /* all checks passed */
}
