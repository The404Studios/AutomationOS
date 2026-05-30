/*
 * mac80211 - Generic 802.11 MAC Layer
 * Simplified implementation for ath9k driver
 */

#include "../../../../include/mac80211.h"
#include "../../../../include/kernel.h"
#include "../../../../include/mem.h"

/**
 * ieee80211_alloc_hw - Allocate hardware structure
 */
ieee80211_hw_t* ieee80211_alloc_hw(size_t priv_data_len, ieee80211_ops_t* ops) {
    ieee80211_hw_t* hw;
    size_t total_size = sizeof(ieee80211_hw_t) + priv_data_len;

    hw = kmalloc(total_size);
    if (!hw) {
        kprintf("mac80211: Failed to allocate hw structure\n");
        return NULL;
    }

    // Clear structure
    for (uint8_t* p = (uint8_t*)hw; p < (uint8_t*)hw + total_size; p++) {
        *p = 0;
    }

    hw->ops = ops;
    hw->priv = (void*)((uint8_t*)hw + sizeof(ieee80211_hw_t));

    kprintf("mac80211: Allocated hw structure (priv_size=%llu)\n", priv_data_len);
    return hw;
}

/**
 * ieee80211_free_hw - Free hardware structure
 */
void ieee80211_free_hw(ieee80211_hw_t* hw) {
    if (!hw) {
        return;
    }

    kprintf("mac80211: Freeing hw structure\n");
    kfree(hw);
}

/**
 * ieee80211_register_hw - Register hardware with mac80211
 */
int ieee80211_register_hw(ieee80211_hw_t* hw) {
    if (!hw || !hw->ops) {
        kprintf("mac80211: Invalid hw or ops\n");
        return -1;
    }

    kprintf("mac80211: Registering hardware\n");
    kprintf("mac80211: Flags: 0x%08x\n", hw->flags);
    kprintf("mac80211: Queues: %d\n", hw->queues);
    kprintf("mac80211: Max rates: %d\n", hw->max_rates);

    // Register bands
    if (hw->wiphy_bands[0]) {
        kprintf("mac80211: 2.4 GHz band: %d channels, %d rates\n",
                hw->wiphy_bands[0]->n_channels,
                hw->wiphy_bands[0]->n_bitrates);
    }

    if (hw->wiphy_bands[1]) {
        kprintf("mac80211: 5 GHz band: %d channels, %d rates\n",
                hw->wiphy_bands[1]->n_channels,
                hw->wiphy_bands[1]->n_bitrates);
    }

    kprintf("mac80211: Hardware registered successfully\n");
    return 0;
}

/**
 * ieee80211_unregister_hw - Unregister hardware
 */
void ieee80211_unregister_hw(ieee80211_hw_t* hw) {
    if (!hw) {
        return;
    }

    kprintf("mac80211: Unregistering hardware\n");

    // Stop hardware if running
    if (hw->ops && hw->ops->stop) {
        hw->ops->stop(hw);
    }
}

/**
 * ieee80211_rx - Receive frame from driver
 */
void ieee80211_rx(ieee80211_hw_t* hw, sk_buff_t* skb) {
    ieee80211_rx_status_t* status;
    ieee80211_hdr_t* hdr;

    if (!hw || !skb) {
        return;
    }

    status = (ieee80211_rx_status_t*)skb->cb;
    hdr = (ieee80211_hdr_t*)skb->data;

    // Basic frame processing
    uint16_t fc = hdr->frame_control;
    uint16_t ftype = fc & 0x0C;
    uint16_t stype = fc & 0xF0;

    kprintf("mac80211: RX frame (len=%d, freq=%d MHz, signal=%d dBm, type=0x%02x)\n",
            skb->len, status->freq, status->signal, ftype | stype);

    // TODO: Full frame processing
    // - Decrypt if encrypted
    // - Defragment if fragmented
    // - Aggregate if part of A-MPDU
    // - Pass to network stack
}

/**
 * ieee80211_tx_status - Report TX completion
 */
void ieee80211_tx_status(ieee80211_hw_t* hw, sk_buff_t* skb) {
    ieee80211_tx_info_t* info;

    if (!hw || !skb) {
        return;
    }

    info = (ieee80211_tx_info_t*)skb->cb;

    kprintf("mac80211: TX complete (len=%d, flags=0x%08x)\n",
            skb->len, info->flags);

    // TODO: Update statistics, notify upper layers
}

/**
 * ieee80211_beacon_get - Get beacon frame
 */
sk_buff_t* ieee80211_beacon_get(ieee80211_hw_t* hw, ieee80211_vif_t* vif) {
    // TODO: Generate beacon frame
    kprintf("mac80211: Beacon requested\n");
    return NULL;
}

/**
 * ieee80211_scan_completed - Report scan completion
 */
void ieee80211_scan_completed(ieee80211_hw_t* hw, bool aborted) {
    kprintf("mac80211: Scan completed (aborted=%d)\n", aborted);
    // TODO: Notify cfg80211
}

/**
 * ieee80211_wake_queues - Wake all TX queues
 */
void ieee80211_wake_queues(ieee80211_hw_t* hw) {
    if (!hw) {
        return;
    }

    kprintf("mac80211: Waking TX queues\n");
    // TODO: Wake queues
}

/**
 * ieee80211_stop_queues - Stop all TX queues
 */
void ieee80211_stop_queues(ieee80211_hw_t* hw) {
    if (!hw) {
        return;
    }

    kprintf("mac80211: Stopping TX queues\n");
    // TODO: Stop queues
}
