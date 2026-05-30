/*
 * ath9k - Atheros 802.11n Wireless Driver
 * Main driver file
 */

#include "ath9k.h"
#include "../../../../../include/kernel.h"
#include "../../../../../include/mem.h"
#include "../../../../../include/x86_64.h"

// Global driver context (simplified for single device)
static ath9k_softc_t* global_sc = NULL;

// Forward declarations
static int ath9k_start(ieee80211_hw_t* hw);
static void ath9k_stop(ieee80211_hw_t* hw);
static int ath9k_add_interface(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
static void ath9k_remove_interface(ieee80211_hw_t* hw, ieee80211_vif_t* vif);
static int ath9k_config(ieee80211_hw_t* hw, uint32_t changed);
static void ath9k_configure_filter(ieee80211_hw_t* hw, uint32_t changed_flags,
                                    uint32_t* total_flags, uint64_t multicast);
static void ath9k_tx(ieee80211_hw_t* hw, sk_buff_t* skb);

// Driver operations
static ieee80211_ops_t ath9k_ops = {
    .start = ath9k_start,
    .stop = ath9k_stop,
    .add_interface = ath9k_add_interface,
    .remove_interface = ath9k_remove_interface,
    .config = ath9k_config,
    .configure_filter = ath9k_configure_filter,
    .tx = ath9k_tx,
};

// Supported 2.4 GHz channels
static ieee80211_channel_t ath9k_2ghz_chantable[] = {
    { 2412, 1, 0, 20, 0 },
    { 2417, 2, 0, 20, 0 },
    { 2422, 3, 0, 20, 0 },
    { 2427, 4, 0, 20, 0 },
    { 2432, 5, 0, 20, 0 },
    { 2437, 6, 0, 20, 0 },
    { 2442, 7, 0, 20, 0 },
    { 2447, 8, 0, 20, 0 },
    { 2452, 9, 0, 20, 0 },
    { 2457, 10, 0, 20, 0 },
    { 2462, 11, 0, 20, 0 },
    { 2467, 12, 0, 20, 0 },
    { 2472, 13, 0, 20, 0 },
    { 2484, 14, 0, 20, 0 },
};

// Supported bitrates (802.11b/g)
static ieee80211_rate_t ath9k_rates[] = {
    { 10, 0, 0 },   // 1 Mbps
    { 20, 1, 0 },   // 2 Mbps
    { 55, 2, 0 },   // 5.5 Mbps
    { 110, 3, 0 },  // 11 Mbps
    { 60, 4, 0 },   // 6 Mbps
    { 90, 5, 0 },   // 9 Mbps
    { 120, 6, 0 },  // 12 Mbps
    { 180, 7, 0 },  // 18 Mbps
    { 240, 8, 0 },  // 24 Mbps
    { 360, 9, 0 },  // 36 Mbps
    { 480, 10, 0 }, // 48 Mbps
    { 540, 11, 0 }, // 54 Mbps
};

// Supported band (2.4 GHz)
static ieee80211_supported_band_t ath9k_band_2ghz = {
    .channels = ath9k_2ghz_chantable,
    .bitrates = ath9k_rates,
    .n_channels = sizeof(ath9k_2ghz_chantable) / sizeof(ieee80211_channel_t),
    .n_bitrates = sizeof(ath9k_rates) / sizeof(ieee80211_rate_t),
    .ht_cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_20 |
              IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_TX_STBC,
};

/**
 * ath9k_start - Start the hardware
 */
static int ath9k_start(ieee80211_hw_t* hw) {
    ath9k_softc_t* sc = hw->priv;
    ath9k_hw_t* ah = sc->ah;
    int ret;

    kprintf("ath9k: Starting hardware\n");

    // Reset hardware
    ret = ath9k_hw_reset(ah);
    if (ret < 0) {
        kprintf("ath9k: Failed to reset hardware\n");
        return ret;
    }

    // Initialize PHY
    ret = ath9k_hw_phy_init(ah);
    if (ret < 0) {
        kprintf("ath9k: Failed to initialize PHY\n");
        return ret;
    }

    // Initialize TX
    ret = ath9k_tx_init(sc);
    if (ret < 0) {
        kprintf("ath9k: Failed to initialize TX\n");
        return ret;
    }

    // Initialize RX
    ret = ath9k_rx_init(sc);
    if (ret < 0) {
        kprintf("ath9k: Failed to initialize RX\n");
        ath9k_tx_deinit(sc);
        return ret;
    }

    // Enable interrupts
    uint32_t imr = AR_ISR_RXOK | AR_ISR_RXERR | AR_ISR_RXORN |
                   AR_ISR_TXOK | AR_ISR_TXERR | AR_ISR_TXURN |
                   AR_ISR_BMISS | AR_ISR_SWBA;
    ath9k_hw_reg_write(ah, AR_IER, 1);  // Global interrupt enable
    ath9k_hw_reg_write(ah, AR_IMR, imr);

    kprintf("ath9k: Hardware started successfully\n");
    return 0;
}

/**
 * ath9k_stop - Stop the hardware
 */
static void ath9k_stop(ieee80211_hw_t* hw) {
    ath9k_softc_t* sc = hw->priv;
    ath9k_hw_t* ah = sc->ah;

    kprintf("ath9k: Stopping hardware\n");

    // Disable interrupts
    ath9k_hw_reg_write(ah, AR_IER, 0);
    ath9k_hw_reg_write(ah, AR_IMR, 0);

    // Stop TX/RX
    ath9k_tx_deinit(sc);
    ath9k_rx_deinit(sc);

    kprintf("ath9k: Hardware stopped\n");
}

/**
 * ath9k_add_interface - Add virtual interface
 */
static int ath9k_add_interface(ieee80211_hw_t* hw, ieee80211_vif_t* vif) {
    kprintf("ath9k: Adding interface type %d\n", vif->type);
    return 0;
}

/**
 * ath9k_remove_interface - Remove virtual interface
 */
static void ath9k_remove_interface(ieee80211_hw_t* hw, ieee80211_vif_t* vif) {
    kprintf("ath9k: Removing interface\n");
}

/**
 * ath9k_config - Configure hardware
 */
static int ath9k_config(ieee80211_hw_t* hw, uint32_t changed) {
    ath9k_softc_t* sc = hw->priv;
    ath9k_hw_t* ah = sc->ah;

    if (changed & 0x01) {  // Channel changed
        uint32_t freq = 2437;  // Channel 6 (2437 MHz) - default
        kprintf("ath9k: Setting channel to %d MHz\n", freq);
        return ath9k_hw_set_channel(ah, freq);
    }

    return 0;
}

/**
 * ath9k_configure_filter - Configure RX filter
 */
static void ath9k_configure_filter(ieee80211_hw_t* hw, uint32_t changed_flags,
                                    uint32_t* total_flags, uint64_t multicast) {
    kprintf("ath9k: Configuring RX filter\n");
    // TODO: Configure hardware RX filter
}

/**
 * ath9k_tx - Transmit frame
 */
static void ath9k_tx(ieee80211_hw_t* hw, sk_buff_t* skb) {
    ath9k_softc_t* sc = hw->priv;

    // Queue frame to TX queue 0 (BE - Best Effort)
    int ret = ath9k_tx_queue_frame(sc, 0, skb);
    if (ret < 0) {
        kprintf("ath9k: Failed to queue TX frame\n");
        // Free SKB
    }
}

/**
 * ath9k_probe - Probe and initialize device
 */
static int ath9k_probe(pci_device_t* pci_dev) {
    ath9k_softc_t* sc;
    ath9k_hw_t* ah;
    ieee80211_hw_t* hw;
    int ret;

    kprintf("ath9k: Probing Atheros device %04x:%04x\n",
            pci_dev->vendor_id, pci_dev->device_id);

    // Allocate mac80211 hardware structure
    hw = ieee80211_alloc_hw(sizeof(ath9k_softc_t), &ath9k_ops);
    if (!hw) {
        kprintf("ath9k: Failed to allocate hw\n");
        return -1;
    }

    // Get driver private data
    sc = hw->priv;
    sc->hw = hw;
    sc->pci_dev = pci_dev;

    // Allocate hardware structure
    ah = kmalloc(sizeof(ath9k_hw_t));
    if (!ah) {
        kprintf("ath9k: Failed to allocate hardware structure\n");
        ieee80211_free_hw(hw);
        return -1;
    }

    sc->ah = ah;

    // Initialize hardware
    ret = ath9k_hw_init(ah, pci_dev);
    if (ret < 0) {
        kprintf("ath9k: Failed to initialize hardware\n");
        kfree(ah);
        ieee80211_free_hw(hw);
        return ret;
    }

    // Setup hardware flags
    hw->flags = IEEE80211_HW_SIGNAL_DBM |
                IEEE80211_HW_AMPDU_AGGREGATION |
                IEEE80211_HW_SUPPORTS_PS;
    hw->queues = 4;  // BE, BK, VI, VO
    hw->max_rates = 4;
    hw->max_rate_tries = 11;

    // Setup supported bands
    hw->wiphy_bands[0] = &ath9k_band_2ghz;  // 2.4 GHz
    hw->wiphy_bands[1] = NULL;              // 5 GHz (not implemented yet)

    // Register with mac80211
    ret = ieee80211_register_hw(hw);
    if (ret < 0) {
        kprintf("ath9k: Failed to register hardware\n");
        ath9k_hw_deinit(ah);
        kfree(ah);
        ieee80211_free_hw(hw);
        return ret;
    }

    // Save global context
    global_sc = sc;

    kprintf("ath9k: Device initialized successfully\n");
    kprintf("ath9k: MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
            ah->mac_addr[0], ah->mac_addr[1], ah->mac_addr[2],
            ah->mac_addr[3], ah->mac_addr[4], ah->mac_addr[5]);

    return 0;
}

/**
 * ath9k_remove - Remove device
 */
static void ath9k_remove(pci_device_t* pci_dev) {
    ath9k_softc_t* sc = global_sc;

    if (!sc) {
        return;
    }

    kprintf("ath9k: Removing device\n");

    // Unregister from mac80211
    ieee80211_unregister_hw(sc->hw);

    // Deinitialize hardware
    ath9k_hw_deinit(sc->ah);
    kfree(sc->ah);

    // Free mac80211 structure
    ieee80211_free_hw(sc->hw);

    global_sc = NULL;
}

/**
 * ath9k_init - Initialize driver
 */
int ath9k_init(void) {
    pci_device_t* pci_dev;

    kprintf("ath9k: Atheros 802.11n wireless driver\n");

    // Search for Atheros wireless devices
    // Try common device IDs
    uint16_t device_ids[] = {
        ATH_DEVICE_AR9280,
        ATH_DEVICE_AR9285,
        ATH_DEVICE_AR9287,
        ATH_DEVICE_AR9380,
        ATH_DEVICE_AR9485,
        ATH_DEVICE_AR9462,
        ATH_DEVICE_AR9565,
        0
    };

    for (int i = 0; device_ids[i] != 0; i++) {
        pci_dev = pci_find_device(ATH_VENDOR_ID, device_ids[i]);
        if (pci_dev) {
            kprintf("ath9k: Found Atheros device %04x\n", device_ids[i]);
            return ath9k_probe(pci_dev);
        }
    }

    kprintf("ath9k: No compatible Atheros wireless device found\n");
    return -1;
}

/**
 * ath9k_exit - Exit driver
 */
void ath9k_exit(void) {
    if (global_sc) {
        ath9k_remove(global_sc->pci_dev);
    }
    kprintf("ath9k: Driver unloaded\n");
}

/**
 * ath9k_get_global_softc - Get global driver context (for IRQ handler)
 */
ath9k_softc_t* ath9k_get_global_softc(void) {
    return global_sc;
}
