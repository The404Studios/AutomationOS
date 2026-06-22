/*
 * wifidiag.c -- WiFi bring-up diagnostics store (for the GUI, no serial needed).
 * ============================================================================
 * The Intel radio bring-up has no emulator, so on the real T410 it must be
 * iterated by reading WHERE it stopped. Rather than require a serial cable, the
 * active wifi backend (the simulated wifisim OR the real iwlwifi driver) updates
 * this single snapshot as detect -> APM -> ALIVE -> NVM -> register -> scan
 * proceeds (and on any failure records the step + a message). SYS_WLAN_DIAG
 * copies the snapshot to the Network Manager, which renders it on-screen.
 *
 * Single writer (the bring-up path) + single reader (the syscall), uniprocessor
 * here -- no lock needed (same discipline as the rest of the wifi control plane).
 *
 * Scope: kernel/net/wifidiag.c
 */
#include "../include/types.h"
#include "../include/string.h"   /* memset / memcpy */
#include "../include/wifi.h"
#include "../include/uapi/wlan.h"

static uapi_wlan_diag_t g_diag;   /* zero-initialized in .bss */

static void diag_cpstr(char* dst, int cap, const char* s) {
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; i++) dst[i] = s[i];
    dst[i] = '\0';
}

void wifi_diag_reset(void) {
    memset(&g_diag, 0, sizeof(g_diag));
    g_diag.last_scan_bss = -1;   /* "no scan yet" */
}

void wifi_diag_card(const char* name, int family, int present) {
    g_diag.present = present ? 1 : 0;
    g_diag.family  = (uint8_t)family;
    diag_cpstr(g_diag.card, (int)sizeof(g_diag.card), name);
}

void wifi_diag_rfkill(int killed) { g_diag.rf_kill = killed ? 1 : 0; }

void wifi_diag_stage(int stage) { g_diag.stage = (uint8_t)stage; }

void wifi_diag_mac(const uint8_t* mac6, int n_channels) {
    if (mac6) for (int i = 0; i < 6; i++) g_diag.mac[i] = mac6[i];
    g_diag.n_channels = (uint16_t)n_channels;
    g_diag.nvm_ok = 1;
}

void wifi_diag_alive(int alive) { g_diag.alive = alive ? 1 : 0; }

void wifi_diag_scan(int bss_count) { g_diag.last_scan_bss = (int16_t)bss_count; }

void wifi_diag_msg(const char* s) { diag_cpstr(g_diag.msg, (int)sizeof(g_diag.msg), s); }

void wifi_diag_get(void* out) {
    if (out) memcpy(out, &g_diag, sizeof(g_diag));
}
