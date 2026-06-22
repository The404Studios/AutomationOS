#ifndef UAPI_WLAN_H
#define UAPI_WLAN_H

/*
 * UAPI: Kernel/Userspace ABI for the SYS_WLAN_* control plane
 * (SCAN 113, CONNECT 114, STATUS 115, DISCONNECT 116, SET_KEY 117).
 *
 * Same discipline as uapi/net.h: any struct the kernel copies to/from
 * userspace lives HERE; both sides #include this file; every struct carries an
 * ABI_SIZE constant + a _Static_assert so drift is a compile error.
 *
 * The values below MUST stay in lockstep with the enums in <wifi.h>.
 */

#ifdef __KERNEL__
#include "../types.h"
#else
#ifndef _UAPI_STDINT_DEFINED
#define _UAPI_STDINT_DEFINED
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
#endif
#endif

/* security classes (match wlan_sec_t in wifi.h) */
#define UAPI_WLAN_SEC_OPEN  0
#define UAPI_WLAN_SEC_WPA2  1
#define UAPI_WLAN_SEC_WPA3  2

/* link states (match wlan_state_t in wifi.h) */
#define UAPI_WLAN_ST_DOWN            0
#define UAPI_WLAN_ST_SCANNING        1
#define UAPI_WLAN_ST_AUTHENTICATING  2
#define UAPI_WLAN_ST_ASSOCIATING     3
#define UAPI_WLAN_ST_ASSOCIATED      4
#define UAPI_WLAN_ST_4WAY            5
#define UAPI_WLAN_ST_CONNECTED       6
#define UAPI_WLAN_ST_FAILED          7

/* -------------------------------------------------------------------
 * SYS_WLAN_SCAN (113): kernel fills an array of these for userspace
 * ------------------------------------------------------------------- */
#define WLAN_BSS_ABI_SIZE  48

typedef struct {
    uint8_t  bssid[6];          /* AP MAC                               */
    uint8_t  ssid[32];          /* not NUL-terminated; use ssid_len     */
    uint8_t  ssid_len;          /* 0..32                                */
    uint8_t  security;          /* UAPI_WLAN_SEC_*                      */
    uint16_t channel;
    int16_t  signal;            /* dBm (negative)                       */
    uint16_t capability;
    uint8_t  _pad[2];
} uapi_wlan_bss_t;

_Static_assert(sizeof(uapi_wlan_bss_t) == WLAN_BSS_ABI_SIZE,
               "uapi_wlan_bss_t ABI size drift");

/* -------------------------------------------------------------------
 * SYS_WLAN_CONNECT (114): join an SSID
 * ------------------------------------------------------------------- */
#define WLAN_CONNECT_ABI_SIZE  104

typedef struct {
    uint8_t  ssid[32];
    uint8_t  ssid_len;
    uint8_t  security;          /* UAPI_WLAN_SEC_*                      */
    uint8_t  bssid[6];          /* optional pin; all-zero = any BSSID   */
    char     passphrase[64];    /* empty for OPEN                        */
} uapi_wlan_connect_t;

_Static_assert(sizeof(uapi_wlan_connect_t) == WLAN_CONNECT_ABI_SIZE,
               "uapi_wlan_connect_t ABI size drift");

/* -------------------------------------------------------------------
 * SYS_WLAN_STATUS (115): current association state
 * ------------------------------------------------------------------- */
#define WLAN_STATUS_ABI_SIZE  44

typedef struct {
    uint8_t  state;             /* UAPI_WLAN_ST_*                       */
    int8_t   rssi;              /* dBm                                  */
    uint8_t  bssid[6];
    uint8_t  ssid[32];
    uint8_t  ssid_len;
    uint8_t  _pad[3];
} uapi_wlan_status_t;

_Static_assert(sizeof(uapi_wlan_status_t) == WLAN_STATUS_ABI_SIZE,
               "uapi_wlan_status_t ABI size drift");

/* -------------------------------------------------------------------
 * SYS_WLAN_SET_KEY (117): supplicant installs PTK (pairwise) / GTK (group)
 * ------------------------------------------------------------------- */
#define WLAN_SETKEY_ABI_SIZE  44

typedef struct {
    uint32_t key_idx;
    uint32_t pairwise;          /* 1 = pairwise (PTK), 0 = group (GTK)  */
    uint32_t key_len;           /* 16 (CCMP/GCMP-128) or 32             */
    uint8_t  key[32];
} uapi_wlan_setkey_t;

_Static_assert(sizeof(uapi_wlan_setkey_t) == WLAN_SETKEY_ABI_SIZE,
               "uapi_wlan_setkey_t ABI size drift");

/* SYS_WLAN_DISCONNECT (116) takes no payload. */

/* -------------------------------------------------------------------
 * SYS_WLAN_DIAG (118): radio bring-up diagnostics for the GUI. Lets the
 * Network Manager show WHERE bring-up stopped on real hardware WITHOUT a
 * serial cable -- the kernel IWL* markers distilled into a snapshot. The
 * active wifi backend (wifisim OR the real iwlwifi driver) maintains it.
 * ------------------------------------------------------------------- */
/* bring-up stage reached (monotonic until a failure resets to FAILED) */
#define UAPI_WLAN_STAGE_NONE        0   /* nothing attempted              */
#define UAPI_WLAN_STAGE_NOCARD      1   /* no Intel WiFi card present      */
#define UAPI_WLAN_STAGE_DETECTED    2   /* card detected + BAR0 mapped     */
#define UAPI_WLAN_STAGE_TRANS_OK    3   /* APM + DMA rings up              */
#define UAPI_WLAN_STAGE_ALIVE       4   /* firmware reached runtime ALIVE  */
#define UAPI_WLAN_STAGE_NVM_OK      5   /* MAC + channels read from NVM    */
#define UAPI_WLAN_STAGE_REGISTERED  6   /* wlan0 live behind the seam      */
#define UAPI_WLAN_STAGE_SCANNED     7   /* a scan has completed            */
#define UAPI_WLAN_STAGE_FAILED      8   /* bring-up aborted (see msg)      */

/* family (mirror of enum iwl_family in iwl-trans.h -- same values) */
#define UAPI_WLAN_FAM_UNKNOWN  0
#define UAPI_WLAN_FAM_1000     1
#define UAPI_WLAN_FAM_5000     2
#define UAPI_WLAN_FAM_6000     3
#define UAPI_WLAN_FAM_6000G2   4

#define WLAN_DIAG_ABI_SIZE  120

typedef struct {
    uint8_t  present;          /* 1 if a wifi backend is active            */
    uint8_t  family;           /* UAPI_WLAN_FAM_*                          */
    uint8_t  rf_kill;          /* 1 if HW RF-kill asserted (switch off)    */
    uint8_t  alive;            /* 1 if firmware reached runtime ALIVE      */
    uint8_t  nvm_ok;           /* 1 if MAC/channels read OK                */
    uint8_t  stage;            /* UAPI_WLAN_STAGE_*                        */
    uint8_t  mac[6];           /* radio MAC (once NVM read)                */
    uint16_t n_channels;       /* channels enumerated                      */
    int16_t  last_scan_bss;    /* last scan result count (-1 = none/error) */
    char     card[40];         /* card friendly name                       */
    char     msg[64];          /* last status / failure-step line          */
} uapi_wlan_diag_t;

_Static_assert(sizeof(uapi_wlan_diag_t) == WLAN_DIAG_ABI_SIZE,
               "uapi_wlan_diag_t ABI size drift");

#endif /* UAPI_WLAN_H */
