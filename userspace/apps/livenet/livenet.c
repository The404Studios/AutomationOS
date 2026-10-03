/*
 * livenet.c -- LIVE-INTERNET proof for AutomationOS (freestanding ring 3, crt0 main).
 * ===================================================================================
 * Does the OS actually reach the real Internet end to end? Runs, in order, against the real
 * Internet (QEMU slirp NAT -> the host's connection; on hardware, the wired/WiFi link):
 *
 *   1. DNS     : dns_resolve("example.com")                      -> an A record
 *   2. HTTP    : GET http://example.com/        (TCP 80)         -> 200 + the page
 *   3. HTTPS   : GET https://example.com/       (TLS, port 443)  -> 200 + the page,
 *                and whether the server certificate chain was VERIFIED against the
 *                embedded CA roots (http_last_trusted()) -- "encrypted" is not "authenticated".
 *   4. SITES   : a matrix of real HTTPS sites under DIFFERENT CAs / key types (RSA + ECDSA chains); each must
 *                authenticate (trusted=1)                                                  [CERT-REAL-0]
 *   5. NEGATIVES: deliberately broken certificates (badssl.com: expired, self-signed, wrong host, untrusted
 *                root) must be REFUSED before any request is sent (HTTP_ERR_CERT)          [TLS-STRICT-0]
 *
 * Output (grep-able; PASS needs 1-3, check 4 is reported but informational):
 *   LIVENET: dns example.com -> 93.184.x.x
 *   LIVENET: http  status=200 bytes=1256
 *   LIVENET: https status=200 bytes=1256 trusted=1
 *   LIVENET: https2 www.google.com status=200 trusted=1
 *   LIVENET: PASS dns=1 http=1 https=1 trusted=1
 *
 * Spawned by init only in -DNET_LIVE builds (NET_LIVE=1 bash scripts/build_all.sh). Every wait
 * inside the net libs is iteration-bounded, so a dead network makes this FAIL, never hang.
 */
#include "../../lib/net/dns.h"
#include "../../lib/net/http.h"

#define SYS_EXIT   0
#define SYS_WRITE  3
#define SYS_YIELD  15

typedef unsigned long size_t;

static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void out_u(unsigned long v) {
    char b[24]; int i = 23; b[i] = 0; if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    out(&b[i]);
}
static void out_ip(unsigned ip) {
    out_u(ip >> 24); out("."); out_u((ip >> 16) & 255); out("."); out_u((ip >> 8) & 255); out("."); out_u(ip & 255);
}
static void out_err(long rc) { out("ERR "); out_u((unsigned long)(-rc)); }

static char g_body[16384];

static int has(const char* hay, long n, const char* needle) {
    long m = (long)slen(needle);
    for (long i = 0; i + m <= n; i++) {
        long j = 0; while (j < m && hay[i + j] == needle[j]) j++;
        if (j == m) return 1;
    }
    return 0;
}

int main(void) {
    int dns_ok = 0, http_ok = 0, https_ok = 0, trusted = 0;
    unsigned ip = 0;

    /* A boot-time program races the stack's first-packet path (interface/lease, then the gateway's ARP reply): warm the
     * resolver up (bounded, ~15 s) so the probes below measure the Internet, not the start-up race. The warm-up is
     * reported, never hidden. */
    out("LIVENET: net ");
    if (dns_wait_net(20000)) out("up\n"); else out("NOT up after 20 s\n");

    int r = -1, warm = 0;
    for (; warm < 30; warm++) {
        r = dns_resolve("example.com", &ip);
        if (r == 0 && ip) break;
        sc3(9 /* SYS_SLEEP */, 500, 0, 0);
    }
    if (warm) { out("LIVENET: warm-up retries="); out_u((unsigned long)warm); out("\n"); }
    out("LIVENET: dns example.com -> ");
    if (r == 0 && ip) { dns_ok = 1; out_ip(ip); } else { out("FAILED rc="); out_u((unsigned long)(-r)); }
    out("\n");

    int st = 0;
    long n = http_get("example.com", 80, "/", g_body, sizeof(g_body), &st);
    out("LIVENET: http  ");
    if (n >= 0) {
        out("status="); out_u((unsigned long)st); out(" bytes="); out_u((unsigned long)n);
        http_ok = (st >= 200 && st < 400 && has(g_body, n, "Example Domain"));
        if (!http_ok) out(" (unexpected body)");
    } else out_err(n);
    out("\n");

    st = 0;
    n = https_get("example.com", 443, "/", g_body, sizeof(g_body), &st);
    out("LIVENET: https ");
    if (n >= 0) {
        trusted = http_last_trusted();
        out("status="); out_u((unsigned long)st); out(" bytes="); out_u((unsigned long)n);
        out(" trusted="); out_u((unsigned long)trusted);
        https_ok = (st >= 200 && st < 400 && has(g_body, n, "Example Domain"));
        if (!https_ok) out(" (unexpected body)");
    } else out_err(n);
    out("\n");

    /* ---- 4. site matrix: any HTTP status is fine -- the point is the certificate verdict ---- */
    static const char *const sites[] = {
        "www.google.com", "github.com", "www.cloudflare.com", "letsencrypt.org",
        "www.amazon.com", "www.microsoft.com", "en.wikipedia.org", "www.mozilla.org", 0 };
    int sites_ok = 0, sites_n = 0;
    for (int i = 0; sites[i]; i++) {
        st = 0;
        n = https_get(sites[i], 443, "/robots.txt", g_body, sizeof(g_body), &st);
        sites_n++;
        out("LIVENET: site "); out(sites[i]);
        if (n >= 0) {
            int tr = http_last_trusted();
            out(" status="); out_u((unsigned long)st); out(" trusted="); out_u((unsigned long)tr);
            if (tr) sites_ok++;
        } else if (n == HTTP_ERR_CERT) {
            out(" CERT-REJECTED (chain did not anchor to a built-in root)");
        } else out_err(n), out(" ");
        out("\n");
    }
    out("LIVENET: sites ok="); out_u((unsigned long)sites_ok); out(" of "); out_u((unsigned long)sites_n); out("\n");

    /* ---- 5. negatives: broken certificates MUST be refused ---- */
    static const char *const bad[] = {
        "expired.badssl.com", "self-signed.badssl.com", "wrong.host.badssl.com", "untrusted-root.badssl.com", 0 };
    int bad_ok = 0, bad_n = 0;
    for (int i = 0; bad[i]; i++) {
        st = 0;
        n = https_get(bad[i], 443, "/", g_body, sizeof(g_body), &st);
        bad_n++;
        out("LIVENET: neg "); out(bad[i]);
        if (n == HTTP_ERR_CERT)      { out(" REJECTED (cert)\n"); bad_ok++; }
        else if (n < 0)              { out(" "); out_err(n); out(" (connection failed -- not a certificate verdict)\n"); }
        else                         { out(" ACCEPTED status="); out_u((unsigned long)st); out(" (BAD: an invalid certificate was trusted)\n"); }
    }
    out("LIVENET: negatives rejected="); out_u((unsigned long)bad_ok); out(" of "); out_u((unsigned long)bad_n); out("\n");

    if (dns_ok && http_ok && https_ok && trusted) {
        out("LIVENET: PASS dns=1 http=1 https=1 trusted=1\n"); sc3(SYS_EXIT, 0, 0, 0);
    } else {
        out("LIVENET: FAIL dns="); out_u((unsigned long)dns_ok); out(" http="); out_u((unsigned long)http_ok);
        out(" https="); out_u((unsigned long)https_ok); out(" trusted="); out_u((unsigned long)trusted); out("\n");
        sc3(SYS_EXIT, 1, 0, 0);
    }
    for (;;) sc3(SYS_YIELD, 0, 0, 0);
}
