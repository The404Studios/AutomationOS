/*
 * wpasupp.c -- WPA2 supplicant driver: connect end-to-end.
 * ========================================================
 *
 * Drives a full WiFi connect through the SYS_WLAN_* control plane, deriving
 * and installing real WPA2 keys with the freestanding 4-way-handshake crypto
 * in ../../lib/wpa:
 *
 *   1. SYS_WLAN_CONNECT  -- join the SSID (security 0=OPEN 1=WPA2 2=WPA3).
 *   2. poll SYS_WLAN_STATUS  -- OPEN expects CONNECTED directly; WPA2/WPA3
 *      reach ASSOCIATED, then the supplicant runs the handshake.
 *   3. WPA2: PMK = PBKDF2(passphrase, SSID); PTK = PRF(PMK, MACs, nonces);
 *      install TK (PTK[32..48]) via SYS_WLAN_SET_KEY (pairwise).
 *      (The sim does not emit real EAPOL frames, so fixed demo nonces / MACs
 *      stand in for the live ANonce/SNonce exchange -- the crypto is real, the
 *      transport is mocked, exactly mirroring the wifisim control plane.)
 *   4. poll SYS_WLAN_STATUS -> CONNECTED.
 *   5. spawn bin/dhcpc ["run","wlan0"] so the lease lands on wlan0.
 *
 * Self-test mode: `wpasupp` (no args) runs wpa_selftest() and prints PASS/FAIL.
 *
 * Freestanding ring 3, crt0+main (no _start here; crt0 calls main(argc,argv)
 * and feeds the return value to SYS_EXIT). No libc. Bounded loops only.
 */

#include "../../../kernel/include/uapi/wlan.h"
#include "../../lib/wpa/wpa.h"
#include "../../lib/crypto/pbkdf2.h"     /* wpa_pmk()  -- WPA2 PMK   */
#include "../../lib/crypto/sae.h"        /* sae_* dragonfly -- WPA3 PMK */

/* ---- syscall numbers ------------------------------------------------- */
#define SYS_WRITE           3
#define SYS_WAITPID         6
#define SYS_YIELD           15
#define SYS_WLAN_CONNECT    114
#define SYS_WLAN_STATUS     115
#define SYS_WLAN_SET_KEY    117
#define SYS_SPAWN_EX_ARGV   106

/* ---- syscall wrappers ------------------------------------------------ */
static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny print helpers (fd 1) -------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char *m)  { sc(SYS_WRITE, 1, (long)m, (long)slen(m)); }
static void outn(const char *m, unsigned len) { sc(SYS_WRITE, 1, (long)m, (long)len); }
static void outd(int v) {
    char b[12]; int i = 0; char o[12]; int j = 0;
    if (v < 0) { out("-"); v = -v; }
    if (v == 0) { out("0"); return; }
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) { o[j++] = b[--i]; }
    o[j] = 0; out(o);
}

/* ---- no-libc helpers ------------------------------------------------- */
static void zero(void *p, int n) { unsigned char *d = p; while (n-- > 0) *d++ = 0; }
static void copy(void *d, const void *s, int n) {
    unsigned char *dd = d; const unsigned char *ss = s; while (n-- > 0) *dd++ = *ss++;
}
static int parse_int(const char *s) {       /* small, non-negative-or-0 */
    int v = 0; if (!s) return 0;
    for (int i = 0; s[i]; i++) { if (s[i] < '0' || s[i] > '9') return v; v = v*10 + (s[i]-'0'); }
    return v;
}

/* ---- fixed demo identities (stand in for the live EAPOL exchange) ---- */
static const unsigned char DEMO_AA[6]  = { 0x02,0x00,0xca,0xfe,0x00,0x02 }; /* authenticator */
static const unsigned char DEMO_SPA[6] = { 0x02,0x00,0xca,0xfe,0x00,0x10 }; /* station       */

/* ====================================================================== *
 * SELF-TEST MODE
 * ====================================================================== */
static int run_selftest(void) {
    if (wpa_selftest() == 0) { out("WPASUPP SELFTEST: PASS\n"); return 0; }
    out("WPASUPP SELFTEST: FAIL\n");
    return 1;
}

/* ====================================================================== *
 * Poll SYS_WLAN_STATUS until state == want (or any terminal/failed state),
 * bounded. Returns the last observed state, or -1 on syscall error.
 * ====================================================================== */
static int wait_state(int want, int max_iters) {
    uapi_wlan_status_t st;
    for (int i = 0; i < max_iters; i++) {
        zero(&st, sizeof(st));
        long r = sc(SYS_WLAN_STATUS, (long)&st, 0, 0);
        if (r < 0) return -1;
        if (st.state == (unsigned char)want) return st.state;
        if (st.state == UAPI_WLAN_ST_FAILED) return st.state;
        sc(SYS_YIELD, 0, 0, 0);
    }
    /* timed out: report whatever we last saw */
    zero(&st, sizeof(st));
    if (sc(SYS_WLAN_STATUS, (long)&st, 0, 0) < 0) return -1;
    return st.state;
}

/* ====================================================================== *
 * Pack argv[1..] NUL-separated for SYS_SPAWN_EX_ARGV.
 * ====================================================================== */
static int argv_pack(char *buf, int cap, const char *const *args, int n) {
    int p = 0;
    for (int i = 0; i < n; i++) {
        const char *a = args[i];
        for (int j = 0; a[j] && p < cap - 1; j++) buf[p++] = a[j];
        if (p < cap) buf[p++] = 0;
    }
    return p;
}

/* ====================================================================== *
 *  main(argc, argv) -- crt0 supplies _start and feeds our return to EXIT.
 * ====================================================================== */
int main(int argc, char **argv) {
    /* No-arg invocation -> self-test. */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        return run_selftest();
    }

    /* Usage: wpasupp <ssid> <security 0|1|2> [passphrase] */
    if (argc < 3 || !argv[2] || !argv[2][0]) {
        out("WPASUPP: usage: wpasupp <ssid> <security 0|1|2> [passphrase]\n");
        return 2;
    }

    const char *ssid_s = argv[1];
    int security       = parse_int(argv[2]);
    const char *pass   = (argc >= 4 && argv[3]) ? argv[3] : "";

    int ssid_len = (int)slen(ssid_s);
    if (ssid_len < 1 || ssid_len > 32) {
        out("WPASUPP: bad ssid length\n");
        return 2;
    }
    if (security != UAPI_WLAN_SEC_OPEN &&
        security != UAPI_WLAN_SEC_WPA2 &&
        security != UAPI_WLAN_SEC_WPA3) {
        out("WPASUPP: bad security class\n");
        return 2;
    }
    if (security != UAPI_WLAN_SEC_OPEN && (!pass || !pass[0])) {
        out("WPASUPP: WPA needs a passphrase\n");
        return 2;
    }

    /* ---- (a) SYS_WLAN_CONNECT --------------------------------------- */
    uapi_wlan_connect_t conn;
    zero(&conn, sizeof(conn));
    copy(conn.ssid, ssid_s, ssid_len);
    conn.ssid_len = (uint8_t)ssid_len;
    conn.security = (uint8_t)security;
    /* bssid left all-zero = any BSSID */
    {
        int pl = (int)slen(pass);
        if (pl > 63) pl = 63;                 /* leave room for the NUL */
        copy(conn.passphrase, pass, pl);
        conn.passphrase[pl] = 0;
    }

    long r = sc(SYS_WLAN_CONNECT, (long)&conn, 0, 0);
    if (r < 0) {
        out("WPASUPP: connect failed (no wifi interface?)\n");
        return 1;
    }
    out("WPASUPP: connect ssid="); outn(ssid_s, slen(ssid_s));
    out(" sec="); outd(security); out("\n");

    /* ---- (b) poll for the expected association state ---------------- */
    if (security == UAPI_WLAN_SEC_OPEN) {
        int s = wait_state(UAPI_WLAN_ST_CONNECTED, 256);
        if (s != UAPI_WLAN_ST_CONNECTED) {
            out("WPASUPP: OPEN did not reach CONNECTED (state="); outd(s); out(")\n");
            return 1;
        }
        out("WPASUPP: CONNECTED ssid="); outn(ssid_s, slen(ssid_s)); out("\n");
    } else {
        int s = wait_state(UAPI_WLAN_ST_ASSOCIATED, 256);
        if (s != UAPI_WLAN_ST_ASSOCIATED &&
            s != UAPI_WLAN_ST_4WAY &&
            s != UAPI_WLAN_ST_CONNECTED) {
            out("WPASUPP: did not associate (state="); outd(s); out(")\n");
            return 1;
        }

        /* ---- (c) WPA2 4-way key derivation -------------------------- */
        unsigned char pmk[32];
        unsigned char ptk[48];
        unsigned char anonce[32], snonce[32];

        /* fixed demo nonces: ANonce = 0x01..0x20, SNonce = 0x21..0x40 */
        for (int i = 0; i < 32; i++) anonce[i] = (unsigned char)(0x01 + i);
        for (int i = 0; i < 32; i++) snonce[i] = (unsigned char)(0x21 + i);

        if (security == UAPI_WLAN_SEC_WPA3) {
            /* WPA3: PMK from SAE (dragonfly). The sim has no live AP to swap
             * auth frames with, so run BOTH SAE sides locally with deterministic
             * demo randoms -- a genuine SAE-derived PMK, just not over the air. */
            sae_state sta, ap;
            unsigned char ra[32], ma[32], rb[32], mb[32], kck[32];
            for (int i = 0; i < 32; i++) {
                ra[i] = (unsigned char)(0x11 + i); ma[i] = (unsigned char)(0x22 + i);
                rb[i] = (unsigned char)(0x33 + i); mb[i] = (unsigned char)(0x44 + i);
            }
            unsigned long pwlen = (unsigned long)slen(pass);
            if (sae_derive_pwe(&sta, (const unsigned char *)pass, pwlen, DEMO_SPA, DEMO_AA) != 0 ||
                sae_derive_pwe(&ap,  (const unsigned char *)pass, pwlen, DEMO_AA, DEMO_SPA) != 0 ||
                sae_build_commit(&sta, ra, ma) != 0 ||
                sae_build_commit(&ap,  rb, mb) != 0 ||
                sae_process_commit(&sta, ap.commit_scalar, ap.commit_element, kck, pmk) != 0) {
                out("WPASUPP: SAE PMK derivation failed\n");
                return 1;
            }
            zero(kck, sizeof(kck));
            out("WPASUPP: SAE PMK derived (WPA3 dragonfly)\n");
        } else {
            /* WPA2: PMK = PBKDF2(passphrase, SSID). */
            wpa_pmk(pass, (const uint8_t *)conn.ssid, ssid_len, pmk);
        }
        wpa_ptk(pmk, DEMO_AA, DEMO_SPA, anonce, snonce, ptk, 48);

        /* TK = PTK[32..48]; install it as the pairwise key. */
        uapi_wlan_setkey_t sk;
        zero(&sk, sizeof(sk));
        sk.key_idx  = 0;
        sk.pairwise = 1;
        sk.key_len  = 16;
        copy(sk.key, ptk + 32, 16);

        long kr = sc(SYS_WLAN_SET_KEY, (long)&sk, 0, 0);

        /* wipe the secret material from our stack once installed */
        zero(pmk, sizeof(pmk));
        zero(ptk, sizeof(ptk));
        zero(&sk, sizeof(sk));

        if (kr < 0) {
            out("WPASUPP: set_key failed\n");
            return 1;
        }
        out("WPASUPP: 4way complete, keys installed\n");

        /* ---- (d) poll for CONNECTED ------------------------------- */
        s = wait_state(UAPI_WLAN_ST_CONNECTED, 256);
        if (s != UAPI_WLAN_ST_CONNECTED) {
            out("WPASUPP: did not reach CONNECTED (state="); outd(s); out(")\n");
            return 1;
        }
        out("WPASUPP: CONNECTED ssid="); outn(ssid_s, slen(ssid_s)); out("\n");
    }

    /* ---- (e) spawn dhcpc on wlan0 ----------------------------------- */
    {
        const char *args[2] = { "run", "wlan0" };
        char av[64];
        int al = argv_pack(av, (int)sizeof(av), args, 2);
        /* dhcpc lives at bin/dhcpc (that is how init spawns it), not sbin/. */
        long pid = sc6(SYS_SPAWN_EX_ARGV, (long)"bin/dhcpc", (long)av, al, 0, 0, 0);
        if (pid < 0) {
            out("WPASUPP: dhcpc spawn failed\n");
            return 1;
        }
        out("WPASUPP: dhcp on wlan0\n");
    }

    return 0;
}
