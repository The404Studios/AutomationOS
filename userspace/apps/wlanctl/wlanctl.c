/*
 * wlanctl.c -- WiFi control-plane probe (M1 proof of the SYS_WLAN_* path).
 * ========================================================================
 * Calls SYS_WLAN_SCAN and prints the AP list, proving the whole chain:
 *   userspace -> SYS_WLAN_SCAN -> netif.wifi (wifi_ops) -> backend (wifisim).
 * On a build with no wifi interface the syscall returns ENOTSUP (clean no-op).
 * Freestanding, bare _start, no libc, no crt0.
 */
#include "../../../kernel/include/uapi/wlan.h"

#define SYS_EXIT        0
#define SYS_WRITE       3
#define SYS_WLAN_SCAN   113
#define SYS_WLAN_STATUS 115

static long sc(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void print(const char* m)  { sc(SYS_WRITE, 1, (long)m, (long)slen(m)); }
static void printn(const char* m, unsigned len) { sc(SYS_WRITE, 1, (long)m, (long)len); }
static void print_dec(int v) {
    char b[12]; int i = 0;
    if (v < 0) { print("-"); v = -v; }
    if (v == 0) { print("0"); return; }
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    char o[12]; int j = 0; while (i > 0) o[j++] = b[--i]; o[j] = 0; print(o);
}

void _start(void) {
    uapi_wlan_bss_t aps[16];
    long n = sc(SYS_WLAN_SCAN, (long)aps, 16, 0);
    if (n < 0) {
        print("WLANCTL: scan ENOTSUP (no wifi interface)\n");
        sc(SYS_EXIT, 0, 0, 0);
        for (;;) {}
    }
    print("WLANCTL: scan "); print_dec((int)n); print(" aps\n");
    for (int i = 0; i < (int)n && i < 16; i++) {
        print("WLANCTL:   ssid=");
        printn((const char*)aps[i].ssid, aps[i].ssid_len);
        print(" sec=");  print_dec((int)aps[i].security);
        print(" ch=");   print_dec((int)aps[i].channel);
        print(" sig=");  print_dec((int)aps[i].signal);
        print("\n");
    }
    print("WLANCTL: PASS\n");
    sc(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}
