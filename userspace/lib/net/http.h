/*
 * http.h -- Minimal freestanding HTTP/1.1 GET client (userspace, ring 3).
 * =========================================================================
 *
 * A tiny, dependency-free HTTP/1.1 GET helper for AutomationOS userspace.
 * NO libc, NO stdio, NO malloc, NO standard headers -- it resolves the host
 * via dns_resolve(), opens a connection through the unified netconn layer
 * (plain TCP for http:// or TLS for https://), sends a fixed GET request,
 * drains the response with the poll-mode RX loop, and copies the body out.
 *
 * Capabilities (all fully bounded -- nothing here can hang the OS):
 *   - HTTP/1.1 GET with Host + Connection header (keep-alive or close).
 *   - HTTPS via the netconn TLS layer (include HTTP_F_TLS flag or use
 *     https_get()).  Peer authentication succeeds only when the CA store in
 *     the TLS context is populated; when the store is empty the connection is
 *     ENCRYPTED-BUT-UNAUTHENTICATED (nc->trusted == 0).  Callers or the UA
 *     layer should warn the user in that case.
 *   - Redirect following (301/302/303/307/308) up to a small cap, parsing the
 *     Location header as either an absolute URL or a relative "/path" against
 *     the current host/port.  Both http:// and https:// redirect targets are
 *     now followed; the TLS flag is updated per the target scheme on each hop.
 *   - Transfer-Encoding: chunked decoding (hex sizes, concat data, 0 = end).
 *   - Content-Length honoured as `long` for an exact body read; large bodies
 *     do not truncate.
 *   - Content-Encoding: gzip / deflate inflate via the shared DEFLATE codec
 *     (inflate_decompress); on any decode failure it gracefully returns the
 *     raw (compressed) body rather than hanging or erroring.
 *   - Transfer-Encoding: identity treated the same as no encoding.
 *   - Header-name matching is case-insensitive throughout.  Extra whitespace
 *     after ':' is tolerated.
 *
 * -------------------------------------------------------------------------
 * KEEP-ALIVE CONNECTION CACHE
 * -------------------------------------------------------------------------
 *
 * When an HTTP/1.1 server does NOT send "Connection: close", the underlying
 * TCP (or TLS) connection is left open after the response body is fully read
 * and is stored in a 1-slot cache (g_ka_*) keyed by host + port + is_tls.
 *
 * On the next call to http_get / https_get / http_get_ex / http_get_range
 * for the same host:port:is_tls combination, the cached connection is reused
 * instead of opening a new one, saving a TCP round-trip (and a TLS handshake
 * for HTTPS).
 *
 * Cache eviction policy:
 *   - Only ONE connection is cached at any time (LRU-of-1).  A request to a
 *     different host:port:is_tls closes the cached connection and opens a
 *     fresh one.
 *   - Idle timeout: if the cached connection has been idle for >=
 *     HTTP_KA_IDLE_MS milliseconds (30 000 ms by default), it is closed and
 *     a fresh connection is opened.  Idle time is tracked with the
 *     SYS_GET_TICKS_MS=40 syscall.
 *   - If the server sends "Connection: close", the connection is always
 *     closed immediately after the response body is consumed.
 *   - Redirect hops always use the cache for their intermediate GETs, so
 *     a redirect chain to the same host benefits from keep-alive too.
 *
 * Thread safety: none.  The AutomationOS userspace is single-threaded; the
 * cache is not protected by a lock.
 *
 * -------------------------------------------------------------------------
 * RANGE REQUESTS
 * -------------------------------------------------------------------------
 *
 * http_get_range() adds a "Range: bytes=<offset>-<offset+length-1>" header
 * to the request.  A 206 Partial Content response is treated normally (the
 * partial body is returned).  A 200 response is accepted as a full-body
 * fallback (body is returned as-is, possibly longer than `length`).  Any
 * other status is returned via *out_status and the body bytes received are
 * still copied to out_body.
 *
 * The Range header follows RFC 7233 section 2.1 byte-range-spec:
 *   Range: bytes=<first-byte-pos>-<last-byte-pos>
 * where last-byte-pos = offset + length - 1.
 *
 * Build (flags passed DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/net/http.c -o http.o
 *   objdump -d http.o | grep fs:0x28   # MUST be empty (no stack canary)
 *
 * http.c depends on dns.o, deflate.o, and tlsconn.o (link all together).
 */

#ifndef AUTOMATIONOS_HTTP_H
#define AUTOMATIONOS_HTTP_H

/* Optional flags for http_get_ex() and http_get_range(). */
#define HTTP_F_NO_REDIRECT   0x0001  /* do not follow 3xx redirects           */
#define HTTP_F_NO_DECODE     0x0002  /* do not inflate gzip/deflate bodies    */
#define HTTP_F_TLS           0x0004  /* use TLS (HTTPS) for this request      */

/* Default maximum number of redirects http_get() / https_get() will follow. */
#define HTTP_MAX_REDIRECTS   5

/* Keep-alive idle timeout in milliseconds (30 seconds). */
#define HTTP_KA_IDLE_MS      30000

/*
 * Default TOTAL fetch timeout in milliseconds (~8 seconds).  Every public entry
 * point (http_get / https_get / http_get_ex / http_get_range / https_get_range)
 * enforces this single wall-clock budget across DNS resolution, the TCP/TLS
 * handshake, AND every redirect hop's transfer, using the SYS_GET_TICKS_MS=40
 * monotonic counter.  A slow or unreachable host therefore can no longer block
 * the caller indefinitely: on expiry the connection is closed and HTTP_ERR_TIMEO
 * (-110) is returned so the caller (browser2, wget, ...) falls back gracefully.
 * Use http_get_timeout_ex() to override the budget for a single call.
 */
#define HTTP_DEFAULT_TIMEOUT_MS  8000

/*
 * HTTP error / status return codes (negative).  Body-byte counts are >= 0;
 * anything < 0 is one of these.  HTTP_ERR_TIMEO is returned when the total
 * fetch budget (HTTP_DEFAULT_TIMEOUT_MS or the http_get_timeout_ex override)
 * elapses before a usable response is received.
 */
#define HTTP_ERR_TIMEO_CODE  (-110)  /* total fetch deadline exceeded          */

/*
 * http_get -- minimal HTTP/1.1 GET (plain TCP, port 80 convention).
 *
 *   host       : hostname OR dotted quad ("example.com" / "10.0.2.2")
 *   port       : TCP port (usually 80)
 *   path       : request path, e.g. "/" or "/index.html"
 *   out_body   : caller buffer that receives the response BODY
 *   out_cap    : capacity of out_body in bytes (body is truncated to fit)
 *   out_status : if non-null, receives the numeric HTTP status (e.g. 200)
 *
 * Resolves the host (via dns_resolve), opens a plain TCP connection (or
 * reuses a cached keep-alive connection to the same host:port), sends
 *   "GET <path> HTTP/1.1\r\nHost: <host>\r\n[Connection: ...]\r\n\r\n",
 * reads the whole response, strips the status line + headers, decodes the body
 * (chunked / gzip / deflate as advertised), follows up to HTTP_MAX_REDIRECTS
 * redirects (switching to TLS automatically on an https:// Location), and
 * copies the resulting body into out_body (up to out_cap bytes).
 *
 * Returns the number of body bytes copied (>= 0) on success, or a negative
 * value on error (DNS failure, connect failure, timeout, bad arguments).
 * Fully bounded -- it will not hang.
 *
 * Signature and default behaviour are unchanged from earlier versions.
 */
long http_get(const char *host, unsigned short port, const char *path,
              char *out_body, unsigned long out_cap, int *out_status);

/*
 * https_get -- minimal HTTPS/1.1 GET (TLS, default port 443).
 *
 * Identical contract to http_get but routes through the TLS-capable netconn
 * layer (HTTP_F_TLS is set internally).  Peer authentication succeeds only
 * when the CA store embedded in the TLS context is populated; otherwise the
 * connection is encrypted-but-unauthenticated (nc->trusted == 0).
 *
 *   host       : hostname for SNI + DNS resolution
 *   port       : TCP port (pass 443 for the HTTPS default)
 *   path       : request path
 *   out_body   : caller buffer for the decoded response body
 *   out_cap    : capacity of out_body in bytes
 *   out_status : if non-null, receives the numeric HTTP status
 *
 * Returns body bytes copied (>= 0) on success, or a negative HTTP_ERR_* code.
 */
long https_get(const char *host, unsigned short port, const char *path,
               char *out_body, unsigned long out_cap, int *out_status);

/*
 * http_get_ex -- like http_get but with explicit control over redirect depth,
 * body decoding, and TLS selection.
 *
 *   max_redirects : maximum 3xx redirects to follow (0 = follow none).
 *   flags         : bitwise OR of HTTP_F_* (0 for default behaviour).
 *                   HTTP_F_TLS  -- open the first connection over TLS.
 *                   HTTP_F_NO_REDIRECT -- ignore all 3xx responses.
 *                   HTTP_F_NO_DECODE   -- skip gzip/deflate inflation.
 *
 * When HTTP_F_TLS is set the initial connection uses TLS; subsequent redirect
 * hops switch scheme automatically based on the Location URL's scheme (http://
 * -> plain, https:// -> TLS).
 *
 * http_get(host, port, path, out_body, out_cap, out_status) is exactly
 *   http_get_ex(host, port, path, out_body, out_cap, out_status,
 *               HTTP_MAX_REDIRECTS, 0).
 *
 * https_get(host, port, path, out_body, out_cap, out_status) is exactly
 *   http_get_ex(host, port, path, out_body, out_cap, out_status,
 *               HTTP_MAX_REDIRECTS, HTTP_F_TLS).
 */
long http_get_ex(const char *host, unsigned short port, const char *path,
                 char *out_body, unsigned long out_cap, int *out_status,
                 int max_redirects, unsigned int flags);

/*
 * http_get_timeout_ex -- like http_get_ex but with an explicit TOTAL wall-clock
 * timeout (in milliseconds) for the whole call (DNS + connect/handshake + every
 * redirect hop).  This is the only way to change the budget; all the other
 * entry points use HTTP_DEFAULT_TIMEOUT_MS.
 *
 *   timeout_ms : total budget in ms.  0 selects HTTP_DEFAULT_TIMEOUT_MS.
 *
 * On expiry the in-flight connection is closed and HTTP_ERR_TIMEO (-110) is
 * returned.  All other behaviour (redirects, decoding, keep-alive, flags) is
 * identical to http_get_ex.  Adding this function does not change any existing
 * signature: existing callers continue to use the built-in default.
 */
long http_get_timeout_ex(const char *host, unsigned short port, const char *path,
                         char *out_body, unsigned long out_cap, int *out_status,
                         int max_redirects, unsigned int flags,
                         unsigned long timeout_ms);

/*
 * http_get_range -- HTTP range request (partial content download).
 *
 * Issues a GET request with a "Range: bytes=<offset>-<offset+length-1>"
 * header, allowing partial download of a resource.
 *
 *   host       : hostname OR dotted quad
 *   port       : TCP port (80 for HTTP, 443 for HTTPS)
 *   path       : request path
 *   offset     : first byte position (0-based, inclusive)
 *   length     : number of bytes requested
 *   out_body   : caller buffer that receives the (partial) response body
 *   out_cap    : capacity of out_body in bytes
 *   out_status : if non-null, receives the numeric HTTP status
 *                (206 = partial content delivered as requested;
 *                 200 = server ignored Range and returned full body;
 *                 other = error, body bytes still copied if any)
 *
 * The Range header sent is:
 *   Range: bytes=<offset>-<last>
 * where last = offset + length - 1.
 *
 * Returns the number of body bytes copied (>= 0) on success, or a negative
 * HTTP_ERR_* code on connection/protocol error.  Keep-alive is honoured for
 * range requests exactly as for normal GETs.
 *
 * For HTTPS, pass port=443 and set HTTP_F_TLS in flags, or use https_get_range.
 * The `flags` parameter accepts the same HTTP_F_* bits as http_get_ex().
 */
long http_get_range(const char *host, unsigned short port, const char *path,
                    unsigned long offset, unsigned long length,
                    char *out_body, unsigned long out_cap, int *out_status);

/*
 * https_get_range -- HTTPS variant of http_get_range.
 *
 * Identical to http_get_range but routes through TLS (HTTP_F_TLS set).
 */
long https_get_range(const char *host, unsigned short port, const char *path,
                     unsigned long offset, unsigned long length,
                     char *out_body, unsigned long out_cap, int *out_status);

/*
 * http_request -- general HTTP/HTTPS request with an arbitrary method, an
 * optional request body, and optional extra request headers.
 *
 *   method        : "GET", "POST", ... (NULL/"" => "GET").
 *   extra_headers : additional CRLF-terminated header lines, or NULL. Use this
 *                   for "Authorization: Bearer <tok>\r\n", "Cookie: ...\r\n", etc.
 *   content_type  : Content-Type for the body (used only when body != NULL),
 *                   e.g. "application/x-www-form-urlencoded".
 *   body,body_len : request body bytes (NULL/0 for none). Content-Length is added.
 *   flags         : HTTP_F_* bits (HTTP_F_TLS for HTTPS).
 *
 * A GET follows redirects (HTTP_MAX_REDIRECTS); a non-GET does NOT (OAuth token
 * endpoints answer directly). Returns body bytes (>= 0) or an HTTP_ERR_* code.
 */
long http_request(const char *host, unsigned short port, const char *path,
                  const char *method, const char *extra_headers,
                  const char *content_type, const unsigned char *body,
                  long body_len, unsigned int flags,
                  char *out_body, unsigned long out_cap, int *out_status);

/*
 * http_post / https_post -- POST a request body over plain TCP / TLS.
 * Convenience wrappers over http_request (HTTP_F_TLS set for https_post).
 * extra_headers may be NULL. Returns body bytes (>= 0) or an HTTP_ERR_* code.
 */
long http_post(const char *host, unsigned short port, const char *path,
               const char *content_type, const unsigned char *body, long body_len,
               const char *extra_headers,
               char *out_body, unsigned long out_cap, int *out_status);
long https_post(const char *host, unsigned short port, const char *path,
                const char *content_type, const unsigned char *body, long body_len,
                const char *extra_headers,
                char *out_body, unsigned long out_cap, int *out_status);

/*
 * http_selftest -- offline parser / keep-alive / range-header self-test.
 *
 * Builds a synthetic HTTP response in a stack buffer, exercises the internal
 * response parser, and verifies:
 *   1. Status code is correctly parsed.
 *   2. Content-Length (as long) is correctly parsed.
 *   3. "Connection: keep-alive" detection.
 *   4. "Connection: close" detection.
 *   5. Transfer-Encoding: identity is treated as no special encoding.
 *   6. Range header string construction (offset/length arithmetic).
 *   7. Body bytes are correctly isolated from the header block.
 *
 * Does NOT require a network connection, a running kernel, or any file I/O.
 * Prints nothing.  Returns 0 on pass, -1 on any check failure.
 *
 * Integrators may call this from cryptotest, libtest, or any early-init path.
 */
int http_selftest(void);

#endif /* AUTOMATIONOS_HTTP_H */
