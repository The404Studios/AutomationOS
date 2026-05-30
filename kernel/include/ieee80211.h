#ifndef IEEE80211_H
#define IEEE80211_H

#include "types.h"

// IEEE 802.11 Frame Types
#define IEEE80211_FTYPE_MGMT    0x00
#define IEEE80211_FTYPE_CTL     0x04
#define IEEE80211_FTYPE_DATA    0x08

// IEEE 802.11 Management Subtypes
#define IEEE80211_STYPE_ASSOC_REQ    0x00
#define IEEE80211_STYPE_ASSOC_RESP   0x10
#define IEEE80211_STYPE_REASSOC_REQ  0x20
#define IEEE80211_STYPE_REASSOC_RESP 0x30
#define IEEE80211_STYPE_PROBE_REQ    0x40
#define IEEE80211_STYPE_PROBE_RESP   0x50
#define IEEE80211_STYPE_BEACON       0x80
#define IEEE80211_STYPE_ATIM         0x90
#define IEEE80211_STYPE_DISASSOC     0xA0
#define IEEE80211_STYPE_AUTH         0xB0
#define IEEE80211_STYPE_DEAUTH       0xC0
#define IEEE80211_STYPE_ACTION       0xD0

// IEEE 802.11 Data Subtypes
#define IEEE80211_STYPE_DATA         0x00
#define IEEE80211_STYPE_QOS_DATA     0x80

// IEEE 802.11 Channels
#define IEEE80211_CHAN_2GHZ_MIN      1
#define IEEE80211_CHAN_2GHZ_MAX      14
#define IEEE80211_CHAN_5GHZ_MIN      36
#define IEEE80211_CHAN_5GHZ_MAX      165

// IEEE 802.11n HT Capabilities
#define IEEE80211_HT_CAP_LDPC               0x0001
#define IEEE80211_HT_CAP_SUP_WIDTH_20_40    0x0002
#define IEEE80211_HT_CAP_SM_PS              0x000C
#define IEEE80211_HT_CAP_GRN_FLD            0x0010
#define IEEE80211_HT_CAP_SGI_20             0x0020
#define IEEE80211_HT_CAP_SGI_40             0x0040
#define IEEE80211_HT_CAP_TX_STBC            0x0080
#define IEEE80211_HT_CAP_RX_STBC            0x0300
#define IEEE80211_HT_CAP_DELAY_BA           0x0400
#define IEEE80211_HT_CAP_MAX_AMSDU          0x0800

// Encryption Types
#define IEEE80211_ENCRYPT_NONE      0
#define IEEE80211_ENCRYPT_WEP       1
#define IEEE80211_ENCRYPT_TKIP      2
#define IEEE80211_ENCRYPT_CCMP      3

// IEEE 802.11 Frame Header
typedef struct {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t addr1[6];  // Destination
    uint8_t addr2[6];  // Source
    uint8_t addr3[6];  // BSSID
    uint16_t seq_ctrl;
} PACKED ieee80211_hdr_t;

// IEEE 802.11 Channel
typedef struct {
    uint32_t freq;          // Frequency in MHz
    uint16_t channel;       // Channel number
    uint16_t flags;         // Channel flags
    uint8_t max_power;      // Max TX power (dBm)
    uint8_t max_antenna_gain;
} ieee80211_channel_t;

// IEEE 802.11 Rate
typedef struct {
    uint16_t bitrate;       // Bitrate in 100 kbps
    uint8_t hw_value;       // Hardware rate index
    uint8_t flags;
} ieee80211_rate_t;

// IEEE 802.11 Supported Band
typedef struct {
    ieee80211_channel_t* channels;
    ieee80211_rate_t* bitrates;
    uint16_t n_channels;
    uint16_t n_bitrates;
    uint16_t ht_cap;        // HT capabilities
} ieee80211_supported_band_t;

// IEEE 802.11 BSS (Basic Service Set)
typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint16_t channel;
    int8_t signal;          // Signal strength (dBm)
    uint16_t capability;
    uint16_t beacon_interval;
    uint64_t timestamp;
} ieee80211_bss_t;

// IEEE 802.11 Station Info
typedef struct {
    uint8_t addr[6];
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    int8_t signal;
    uint8_t tx_rate;
} ieee80211_sta_info_t;

#endif
