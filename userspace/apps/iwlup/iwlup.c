/*
 * iwlup.c -- IWL-TRIGGER: fire the DEFERRED real Intel iwlwifi bring-up (ring 3).
 * ============================================================================
 *
 * The T410's real radio (Intel iwlwifi, 1000/5000/6000 = DVM firmware) defers
 * ALL of its firmware-driven bring-up out of boot: the kernel's IWL-IDENT brick
 * detects the card and reads its HW revision, then STOPS. Running this tool from
 * the desktop invokes the held bring-up ladder via SYS_NET_CONFIG's
 * WLAN_BRINGUP flag:
 *
 *     iwl_trans_bringup  (APM power-up + grab NIC access + rings)
 *  -> iwl_load_ucode     (DMA the uCode sections to SRAM, wait for INIT ALIVE,
 *                         calibration, runtime ALIVE)
 *  -> iwl_read_nvm       (MAC address + channel list)
 *  -> register wlan0 behind the wifi_ops seam  (the REAL radio, NOT wifisim)
 *
 * Every risky step prints a serial marker first and every poll is iteration-
 * bounded, so if the firmware never comes ALIVE the kernel ABORTS cleanly and
 * this tool can simply be re-run -- a stall costs a retry on a live machine with
 * serial, never the boot.
 *
 * Requires: a build with the real driver (IWLWIFI=1, which excludes WIFI_SIM)
 * AND the firmware blob staged in the initrd at /lib/firmware/iwlwifi-<fam>.ucode
 * (drop it in per docs/T410_IWLWIFI.md). On a build without the real radio, or in
 * QEMU (no iwlwifi card), this is a clean ENOTSUP no-op.
 *
 * After a successful bring-up: open the Network Manager (or run `wlanctl` to
 * scan), pick an SSID, then `wpasupp <ssid> <wpa2|wpa3> <passphrase>`, then
 * `dhcpc wlan0` for a lease.
 *
 * NO libc, NO stdio. Inline syscalls + fixed buffers only (house pattern).
 */

#include "../../../kernel/include/uapi/net.h"

#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_NET_CONFIG 89

static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    uapi_net_config_t req;
    char *p = (char *)&req;
    for (unsigned i = 0; i < sizeof(req); i++) p[i] = 0;
    req.ifname[0] = 'w'; req.ifname[1] = 'l';
    req.ifname[2] = 'a'; req.ifname[3] = 'n'; req.ifname[4] = '0';
    req.flags = NET_CONFIG_FLAG_WLAN_BRINGUP;

    print("iwlup: triggering deferred Intel iwlwifi bring-up...\n");
    print("iwlup: (watch serial for the IWL TRANS/LOAD/ALIVE/NVM ladder)\n");
    long r = sc(SYS_NET_CONFIG, (long)&req, 0, 0, 0, 0, 0);
    if (r == 0) {
        print("iwlup: OK -- bring-up ladder ran; wlan0 registered if the uCode came ALIVE.\n");
        print("iwlup: next: scan (wlanctl / Network Manager), wpasupp <ssid> <sec> <pass>, dhcpc wlan0.\n");
        return 0;
    }
    if (r == 95 || r == -95) {   /* ENOTSUP: no real radio in this build */
        print("iwlup: nothing to do (no real iwlwifi in this build -- WIFI_SIM or stock, or no card).\n");
        return 0;
    }
    print("iwlup: bring-up ABORTED (no ALIVE / firmware missing) -- safe to re-run; see serial.\n");
    return 1;
}
