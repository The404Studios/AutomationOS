/*
 * gsignin.c -- "Sign in with Google" via OAuth 2.0 Device Flow (RFC 8628).
 * =========================================================================
 *
 * The realistic way a from-scratch OS logs into a real Google account: NOT by
 * rendering accounts.google.com (a heavy JS SPA behind bot-detection that no
 * from-scratch browser can drive), but by the SAME device-authorization grant
 * smart TVs, the gcloud CLI and game consoles use:
 *
 *   1. POST <device endpoint>   -> { user_code, device_code, verification_url, interval }
 *   2. show the user the short code + URL; they approve on a phone/laptop.
 *   3. poll POST <token endpoint> -> authorization_pending ... then { access_token }
 *   4. GET  <userinfo>  (Bearer)  -> { email, name }   == the signed-in account.
 *
 * No password is ever typed into our OS; the OS only exchanges codes/tokens.
 *
 * Two modes, ONE code path (the new http_post / https_post in lib/net/http):
 *   - MOCK  (default, ZERO cost): plain HTTP to the host mock at 10.0.2.2:8434
 *     (scripts/oauth_mock.py over the QEMU slirp seam). Proves the whole flow
 *     end to end with no Google project, no key, no phone (auto-approves).
 *   - LIVE  (real Google account): HTTPS to oauth2.googleapis.com +
 *     www.googleapis.com, with a "TV and Limited Input device" OAuth client.
 *     Selected when /etc/gsignin.conf exists or client_id+secret are on argv.
 *     The client_id/secret are NEVER compiled in -- they come from the config
 *     file or argv at runtime.
 *
 * Usage:
 *   gsignin                       # MOCK unless /etc/gsignin.conf exists
 *   gsignin --mock                # force the mock (zero cost)
 *   gsignin <client_id> <secret>  # force LIVE Google with these creds
 *
 * Freestanding ring 3, crt0+main. Links json.o + the HTTPS object bundle.
 * On any network failure it SKIPs cleanly (exit 0) so the default boot stays
 * green. Prints "GSIGNIN: PASS signed_in_as <email>" on success.
 */

#include "../../lib/net/http.h"
#include "../../lib/json/json.h"

/* ---- syscalls ----------------------------------------------------------- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_SLEEP         9      /* milliseconds (handlers.c) */
#define SYS_GET_TICKS_MS 40
#define O_RDONLY          0

static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static int starts(const char *s, const char *p) {
    while (*p) { if (*s != *p) return 0; s++; p++; }
    return 1;
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s), 0, 0); }
static void msleep(long ms) { sc(SYS_SLEEP, ms, 0, 0, 0, 0); }

/* append src to dst[*dp] within cap (NUL-terminated). */
static void apnd(char *dst, int *dp, int cap, const char *src) {
    int p = *dp;
    while (src && *src && p < cap - 1) dst[p++] = *src++;
    dst[p] = 0;
    *dp = p;
}

/* ---- config ------------------------------------------------------------- */
typedef struct {
    int            tls;        /* 0 = mock (http), 1 = live (https)       */
    const char    *dev_host;
    unsigned short dev_port;
    const char    *ui_host;
    unsigned short ui_port;
    char           client_id[256];
    char           client_secret[256];
} cfg_t;

/* Copy the value of "key=" out of a small config buffer into out[cap]. */
static int conf_val(const char *buf, int len, const char *key, char *out, int cap) {
    int kl = (int)slen(key);
    for (int i = 0; i < len; i++) {
        /* match key at line start */
        int j = 0;
        while (j < kl && i + j < len && buf[i + j] == key[j]) j++;
        if (j == kl && i + kl < len && buf[i + kl] == '=') {
            int s = i + kl + 1, o = 0;
            while (s < len && buf[s] != '\n' && buf[s] != '\r' && o < cap - 1)
                out[o++] = buf[s++];
            out[o] = 0;
            return o;
        }
        /* skip to next line */
        while (i < len && buf[i] != '\n') i++;
    }
    return 0;
}

/* Load /etc/gsignin.conf (client_id=, client_secret=). Returns 1 if both found. */
static int load_conf(cfg_t *c) {
    long fd = sc(SYS_OPEN, (long)"/etc/gsignin.conf", O_RDONLY, 0, 0, 0);
    if (fd < 0) return 0;
    static char buf[1024];
    long n = sc(SYS_READ, fd, (long)buf, (long)sizeof(buf) - 1, 0, 0);
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    if (n <= 0) return 0;
    buf[n] = 0;
    int a = conf_val(buf, (int)n, "client_id", c->client_id, sizeof(c->client_id));
    int b = conf_val(buf, (int)n, "client_secret", c->client_secret, sizeof(c->client_secret));
    return (a > 0 && b > 0);
}

/* ---- JSON helpers ------------------------------------------------------- */
static char       g_resp[64 * 1024];
static json_node  g_pool[1024];

/* Extract a decoded string field of the just-parsed object into out[cap]. */
static int jget(json_doc *d, int obj, const char *key, char *out, int cap) {
    int idx = json_object_get(d, obj, key);
    if (idx < 0) { out[0] = 0; return -1; }
    int r = json_unescape(d, idx, out, (unsigned long)cap);
    if (r < 0) { out[0] = 0; return -1; }
    return r;
}

/* POST a form body to host:port/path; returns parsed root index or -1, and
 * fills *pstatus + the json_doc (which borrows g_resp -- keep it stable). */
static int post_form(cfg_t *c, const char *host, unsigned short port,
                     const char *path, const char *body,
                     json_doc *doc, int *pstatus) {
    /* ONE call, chosen by scheme. Doing plain http_post FIRST and only then
     * https_post would leak the client_secret + device_code in CLEARTEXT to :443
     * (and double every poll's latency) on the LIVE path -- so branch up front. */
    long n = c->tls
        ? https_post(host, port, path, "application/x-www-form-urlencoded",
                     (const unsigned char *)body, (long)slen(body),
                     (const char *)0, g_resp, sizeof(g_resp) - 1, pstatus)
        : http_post(host, port, path, "application/x-www-form-urlencoded",
                    (const unsigned char *)body, (long)slen(body),
                    (const char *)0, g_resp, sizeof(g_resp) - 1, pstatus);
    if (n < 0) return -1;
    g_resp[n] = 0;
    return json_parse(doc, g_pool, (int)(sizeof(g_pool) / sizeof(g_pool[0])),
                      g_resp, (unsigned long)n);
}

int main(int argc, char **argv) {
    cfg_t c;
    c.client_id[0] = 0; c.client_secret[0] = 0;

    int force_mock = 0, have_creds = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] == '-' && argv[i][1] == '-' &&
            argv[i][2] == 'm') { force_mock = 1; }
    }
    /* argv creds: gsignin <client_id> <secret> */
    if (argc >= 3 && argv[1] && argv[1][0] && argv[1][0] != '-' && argv[2] && argv[2][0]) {
        int p = 0; apnd(c.client_id, &p, sizeof(c.client_id), argv[1]);
        p = 0;     apnd(c.client_secret, &p, sizeof(c.client_secret), argv[2]);
        have_creds = 1;
    }
    if (!have_creds && !force_mock) have_creds = load_conf(&c);

    if (have_creds && !force_mock) {
        c.tls = 1;
        c.dev_host = "oauth2.googleapis.com"; c.dev_port = 443;
        c.ui_host  = "www.googleapis.com";    c.ui_port  = 443;
        out("GSIGNIN: mode=LIVE (real Google account)\n");
    } else {
        c.tls = 0;
        c.dev_host = "10.0.2.2"; c.dev_port = 8434;
        c.ui_host  = "10.0.2.2"; c.ui_port  = 8434;
        int p = 0; apnd(c.client_id, &p, sizeof(c.client_id), "mock-client");
        p = 0;     apnd(c.client_secret, &p, sizeof(c.client_secret), "mock-secret");
        out("GSIGNIN: mode=MOCK (host oauth_mock.py @ 10.0.2.2:8434, zero cost)\n");
    }

    /* ---- 1. device/code ------------------------------------------------- */
    static char body[1024];
    int bp = 0;
    apnd(body, &bp, sizeof(body), "client_id=");
    apnd(body, &bp, sizeof(body), c.client_id);
    apnd(body, &bp, sizeof(body), "&scope=openid%20email%20profile");

    json_doc doc; int status = 0; int root = -1;
    /* Retry the first request: at boot the DHCP lease / link may not be up yet. */
    for (int attempt = 0; attempt < 20; attempt++) {
        root = post_form(&c, c.dev_host, c.dev_port, "/device/code", body, &doc, &status);
        if (root >= 0) break;
        if (attempt == 0) out("GSIGNIN: waiting for network...\n");
        msleep(1000);
    }
    if (root < 0) {
        out("GSIGNIN: SKIP (device endpoint unreachable -- run scripts/oauth_mock.py "
            "and boot with -netdev user -device e1000)\n");
        return 0;
    }

    static char device_code[512], user_code[128], verify[256];
    char interval_s[32]; int interval = 5, expires = 1800;
    jget(&doc, root, "device_code", device_code, sizeof(device_code));
    jget(&doc, root, "user_code",   user_code,   sizeof(user_code));
    if (jget(&doc, root, "verification_url", verify, sizeof(verify)) < 0)
        jget(&doc, root, "verification_uri", verify, sizeof(verify));
    int ii = json_object_get(&doc, root, "interval");
    if (ii >= 0) interval = (int)g_pool[ii].inum;
    int ei = json_object_get(&doc, root, "expires_in");
    if (ei >= 0) expires = (int)g_pool[ei].inum;
    if (interval < 1) interval = 1;
    if (!device_code[0] || !user_code[0]) {
        out("GSIGNIN: SKIP (device endpoint gave no code)\n");
        return 0;
    }
    (void)interval_s;

    out("GSIGNIN: ===========================================\n");
    out("GSIGNIN:  To sign in, open:  "); out(verify); out("\n");
    out("GSIGNIN:  and enter code:    "); out(user_code); out("\n");
    out("GSIGNIN: ===========================================\n");
    out("GSIGNIN: waiting for approval...\n");

    /* ---- 2. poll the token endpoint ------------------------------------- */
    long t0 = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    static char access_token[4096];
    access_token[0] = 0;
    for (int poll = 0; poll < 600; poll++) {
        long elapsed = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0) - t0;
        if (elapsed > (long)expires * 1000) {
            out("GSIGNIN: FAIL (code expired before approval)\n");
            return 0;
        }
        msleep((long)interval * 1000);

        bp = 0;
        apnd(body, &bp, sizeof(body), "client_id=");
        apnd(body, &bp, sizeof(body), c.client_id);
        apnd(body, &bp, sizeof(body), "&client_secret=");
        apnd(body, &bp, sizeof(body), c.client_secret);
        apnd(body, &bp, sizeof(body), "&device_code=");
        apnd(body, &bp, sizeof(body), device_code);
        apnd(body, &bp, sizeof(body),
             "&grant_type=urn:ietf:params:oauth:grant-type:device_code");

        json_doc td; int tstatus = 0;
        int troot = post_form(&c, c.dev_host, c.dev_port, "/token", body, &td, &tstatus);
        if (troot < 0) { out("GSIGNIN: (poll: no response, retrying)\n"); continue; }

        if (jget(&td, troot, "access_token", access_token, sizeof(access_token)) > 0)
            break;   /* approved! */

        char err[64];
        if (jget(&td, troot, "error", err, sizeof(err)) > 0) {
            /* authorization_pending -> keep polling; slow_down -> back off;
             * anything else (access_denied/expired_token/...) is terminal. */
            if (starts(err, "authorization_pending")) {
                /* user has not approved yet -- keep waiting, no message spam */
            } else if (starts(err, "slow_down")) {
                interval += 5;
            } else {
                out("GSIGNIN: FAIL error="); out(err); out("\n");
                return 0;
            }
        }
    }
    if (!access_token[0]) { out("GSIGNIN: FAIL (no token after polling)\n"); return 0; }
    out("GSIGNIN: approved -- got access token.\n");

    /* ---- 3. userinfo (Bearer) ------------------------------------------- */
    static char authhdr[4200];
    int ap = 0;
    apnd(authhdr, &ap, sizeof(authhdr), "Authorization: Bearer ");
    apnd(authhdr, &ap, sizeof(authhdr), access_token);
    apnd(authhdr, &ap, sizeof(authhdr), "\r\n");

    int uistatus = 0;
    long un = http_request(c.ui_host, c.ui_port, "/oauth2/v3/userinfo",
                           "GET", authhdr, (const char *)0,
                           (const unsigned char *)0, 0,
                           c.tls ? HTTP_F_TLS : 0,
                           g_resp, sizeof(g_resp) - 1, &uistatus);
    if (un < 0) { out("GSIGNIN: FAIL (userinfo unreachable)\n"); return 0; }
    g_resp[un] = 0;

    json_doc ud;
    int uroot = json_parse(&ud, g_pool, (int)(sizeof(g_pool) / sizeof(g_pool[0])),
                           g_resp, (unsigned long)un);
    static char email[256], name[256];
    email[0] = name[0] = 0;
    if (uroot >= 0) {
        jget(&ud, uroot, "email", email, sizeof(email));
        jget(&ud, uroot, "name",  name,  sizeof(name));
    }
    if (!email[0]) { out("GSIGNIN: FAIL (userinfo had no email)\n"); return 0; }

    out("GSIGNIN: ===========================================\n");
    out("GSIGNIN: Signed in as "); out(email);
    if (name[0]) { out(" ("); out(name); out(")"); }
    out("\n");
    out("GSIGNIN: PASS signed_in_as "); out(email); out("\n");
    return 0;
}
