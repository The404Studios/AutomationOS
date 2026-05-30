#ifndef MAC80211_H
#define MAC80211_H

#include "types.h"
#include "ieee80211.h"

// mac80211 is the generic 802.11 MAC layer framework
// It provides the infrastructure for wireless drivers

// Forward declarations
typedef struct ieee80211_hw ieee80211_hw_t;
typedef struct ieee80211_vif ieee80211_vif_t;
typedef struct ieee80211_ops ieee80211_ops_t;

// Interface Types
typedef enum {
    IEEE80211_IF_TYPE_STATION,      // Infrastructure BSS station
    IEEE80211_IF_TYPE_AP,           // Access Point
    IEEE80211_IF_TYPE_ADHOC,        // Ad-hoc network
    IEEE80211_IF_TYPE_MONITOR,      // Monitor mode
    IEEE80211_IF_TYPE_MESH,         // Mesh network
} ieee80211_if_type_t;

// Hardware Flags
#define IEEE80211_HW_HAS_RATE_CONTROL           0x00000001
#define IEEE80211_HW_RX_INCLUDES_FCS            0x00000002
#define IEEE80211_HW_HOST_BROADCAST_PS_BUFFERING 0x00000004
#define IEEE80211_HW_SIGNAL_DBM                 0x00000008
#define IEEE80211_HW_AMPDU_AGGREGATION          0x00000010
#define IEEE80211_HW_SUPPORTS_PS                0x00000020
#define IEEE80211_HW_PS_NULLFUNC_STACK          0x00000040
#define IEEE80211_HW_SUPPORTS_DYNAMIC_PS        0x00000080

// TX Control Flags
#define IEEE80211_TX_CTL_REQ_TX_STATUS          0x00000001
#define IEEE80211_TX_CTL_ASSIGN_SEQ             0x00000002
#define IEEE80211_TX_CTL_NO_ACK                 0x00000004
#define IEEE80211_TX_CTL_AMPDU                  0x00000008
#define IEEE80211_TX_CTL_INJECTED               0x00000010

// RX Status Flags
#define IEEE80211_RX_RA_MATCH                   0x00000001
#define IEEE80211_RX_FRAGMENTED                 0x00000002
#define IEEE80211_RX_DECRYPTED                  0x00000004
#define IEEE80211_RX_MMIC_STRIPPED              0x00000008

// TX Info
typedef struct {
    uint32_t flags;
    uint8_t band;
    uint8_t hw_queue;
    uint16_t rate_index;
    void* driver_data[8];
} ieee80211_tx_info_t;

// RX Status
typedef struct {
    uint64_t mactime;
    uint32_t flags;
    uint16_t freq;
    uint8_t rate_idx;
    int8_t signal;
    uint8_t antenna;
    uint8_t band;
} ieee80211_rx_status_t;

// SKB (Socket Buffer) - simplified for now
typedef struct {
    uint8_t* data;
    uint16_t len;
    uint16_t data_len;
    void* cb;           // Control buffer (ieee80211_tx_info or ieee80211_rx_status)
} sk_buff_t;

// Virtual Interface
struct ieee80211_vif {
    ieee80211_if_type_t type;
    uint8_t addr[6];        // MAC address
    bool bss_conf_changed;
    void* driver_priv;
};

// Hardware Configuration
typedef struct {
    uint16_t channel;
    uint16_t channel_type;  // HT20, HT40+, HT40-
    uint8_t max_power;
} ieee80211_conf_t;

// Driver Operations (callbacks)
struct ieee80211_ops {
    int (*start)(ieee80211_hw_t* hw);
    void (*stop)(ieee80211_hw_t* hw);
    int (*add_interface)(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
    void (*remove_interface)(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
    int (*config)(ieee80211_hw_t* hw, uint32_t changed);
    void (*configure_filter)(ieee80211_hw_t* hw, uint32_t changed_flags,
                             uint32_t* total_flags, uint64_t multicast);
    int (*set_key)(ieee80211_hw_t* hw, uint32_t cmd, ieee80211_vif_t* vif,
                   uint8_t* key, uint8_t key_len);
    void (*bss_info_changed)(ieee80211_hw_t* hw, ieee80211_vif_t* vif,
                             uint32_t changed);
    int (*hw_scan)(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
    void (*sw_scan_start)(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
    void (*sw_scan_complete)(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
    int (*get_stats)(ieee80211_hw_t* hw, ieee80211_sta_info_t* stats);
    void (*tx)(ieee80211_hw_t* hw, sk_buff_t* skb);
};

// Hardware Structure
struct ieee80211_hw {
    ieee80211_conf_t conf;
    ieee80211_ops_t* ops;
    void* priv;             // Driver private data
    uint32_t flags;
    uint16_t extra_tx_headroom;
    uint16_t queues;
    uint16_t max_rates;
    uint16_t max_rate_tries;
    ieee80211_supported_band_t* wiphy_bands[2]; // 2.4 GHz, 5 GHz
};

// mac80211 API Functions
ieee80211_hw_t* ieee80211_alloc_hw(size_t priv_data_len, ieee80211_ops_t* ops);
int ieee80211_register_hw(ieee80211_hw_t* hw);
void ieee80211_unregister_hw(ieee80211_hw_t* hw);
void ieee80211_free_hw(ieee80211_hw_t* hw);

void ieee80211_rx(ieee80211_hw_t* hw, sk_buff_t* skb);
void ieee80211_tx_status(ieee80211_hw_t* hw, sk_buff_t* skb);

sk_buff_t* ieee80211_beacon_get(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
void ieee80211_scan_completed(ieee80211_hw_t* hw, bool aborted);
void ieee80211_wake_queues(ieee80211_hw_t* hw);
void ieee80211_stop_queues(ieee80211_hw_t* hw);

#endif
