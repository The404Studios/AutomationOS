/*
 * ath9k - RX (Receive) Handling
 */

#include "ath9k.h"
#include "../../../../../include/kernel.h"
#include "../../../../../include/mem.h"

/**
 * ath9k_rx_init - Initialize RX queue
 */
int ath9k_rx_init(ath9k_softc_t* sc) {
    ath9k_hw_t* ah = sc->ah;
    ath9k_rx_queue_t* rxq;

    kprintf("ath9k_rx: Initializing RX queue\n");

    // Allocate RX queue
    rxq = kmalloc(sizeof(ath9k_rx_queue_t));
    if (!rxq) {
        kprintf("ath9k_rx: Failed to allocate RX queue\n");
        return -1;
    }

    // Clear structure
    for (uint8_t* p = (uint8_t*)rxq; p < (uint8_t*)rxq + sizeof(ath9k_rx_queue_t); p++) {
        *p = 0;
    }

    rxq->size = ATH_RXBUF;
    rxq->head = 0;

    // Allocate descriptor ring
    rxq->desc_ring = pmm_alloc_page();
    if (!rxq->desc_ring) {
        kprintf("ath9k_rx: Failed to allocate descriptor ring\n");
        kfree(rxq);
        return -1;
    }

    // Get physical address (simplified - assume identity mapping)
    rxq->desc_ring_phys = (uint64_t)rxq->desc_ring;

    // Clear descriptors
    for (uint8_t* p = (uint8_t*)rxq->desc_ring;
         p < (uint8_t*)rxq->desc_ring + PAGE_SIZE; p++) {
        *p = 0;
    }

    // Allocate buffer pointers
    rxq->buffers = kmalloc(sizeof(void*) * ATH_RXBUF);
    if (!rxq->buffers) {
        kprintf("ath9k_rx: Failed to allocate buffer array\n");
        pmm_free_page(rxq->desc_ring);
        kfree(rxq);
        return -1;
    }

    // Allocate and setup RX buffers
    for (int i = 0; i < ATH_RXBUF; i++) {
        void* buf = kmalloc(ATH_RX_BUF_SIZE);
        if (!buf) {
            kprintf("ath9k_rx: Failed to allocate RX buffer %d\n", i);
            // Cleanup
            for (int j = 0; j < i; j++) {
                kfree(rxq->buffers[j]);
            }
            kfree(rxq->buffers);
            pmm_free_page(rxq->desc_ring);
            kfree(rxq);
            return -1;
        }

        rxq->buffers[i] = buf;

        // Setup descriptor
        ath9k_desc_t* desc = &rxq->desc_ring[i];
        desc->ds_data = (uint32_t)(uint64_t)buf;  // Physical address (simplified)
        desc->ds_ctl0 = ATH_RX_BUF_SIZE;  // Buffer size
        desc->ds_ctl1 = 0;
        desc->ds_hw0 = 0;
        desc->ds_hw1 = 0;
        desc->ds_hw2 = 0;
        desc->ds_hw3 = 0;

        // Link to next descriptor
        uint16_t next_desc = (i + 1) % ATH_RXBUF;
        desc->ds_link = (uint32_t)(rxq->desc_ring_phys + next_desc * sizeof(ath9k_desc_t));
    }

    ah->rx_queue = rxq;

    // Program RX descriptor pointer
    ath9k_hw_reg_write(ah, AR_RXDP, (uint32_t)rxq->desc_ring_phys);

    // Enable RX
    uint32_t rxcfg = ath9k_hw_reg_read(ah, AR_RXCFG);
    rxcfg |= 0x00000001;  // Enable RX DMA
    ath9k_hw_reg_write(ah, AR_RXCFG, rxcfg);

    // Start RX
    ath9k_hw_reg_write(ah, AR_CR, AR_CR_RXE);

    kprintf("ath9k_rx: RX initialized successfully (ring at 0x%016llx)\n",
            rxq->desc_ring_phys);
    return 0;
}

/**
 * ath9k_rx_deinit - Deinitialize RX queue
 */
void ath9k_rx_deinit(ath9k_softc_t* sc) {
    ath9k_hw_t* ah = sc->ah;
    ath9k_rx_queue_t* rxq = ah->rx_queue;

    if (!rxq) {
        return;
    }

    kprintf("ath9k_rx: Deinitializing RX queue\n");

    // Disable RX
    ath9k_hw_reg_write(ah, AR_CR, AR_CR_RXD);

    uint32_t rxcfg = ath9k_hw_reg_read(ah, AR_RXCFG);
    rxcfg &= ~0x00000001;  // Disable RX DMA
    ath9k_hw_reg_write(ah, AR_RXCFG, rxcfg);

    // Free buffers
    if (rxq->buffers) {
        for (int i = 0; i < ATH_RXBUF; i++) {
            if (rxq->buffers[i]) {
                kfree(rxq->buffers[i]);
            }
        }
        kfree(rxq->buffers);
    }

    // Free descriptor ring
    if (rxq->desc_ring) {
        pmm_free_page(rxq->desc_ring);
    }

    kfree(rxq);
    ah->rx_queue = NULL;
}

/**
 * ath9k_rx_process_frame - Process received frame
 */
static void ath9k_rx_process_frame(ath9k_softc_t* sc, ath9k_desc_t* desc,
                                    void* buf, uint16_t len) {
    ieee80211_hw_t* hw = sc->hw;
    ath9k_hw_t* ah = sc->ah;

    // Create SKB (simplified)
    sk_buff_t skb;
    skb.data = buf;
    skb.len = len;
    skb.data_len = len;

    // Setup RX status
    ieee80211_rx_status_t rx_status;
    rx_status.freq = ah->curchan;
    rx_status.rate_idx = 0;  // TODO: Extract from descriptor
    rx_status.signal = -50;  // TODO: Extract from descriptor
    rx_status.band = 0;      // 2.4 GHz
    rx_status.flags = 0;

    skb.cb = &rx_status;

    // Pass to mac80211
    ieee80211_rx(hw, &skb);

    ah->rx_packets++;
}

/**
 * ath9k_rx_tasklet - RX tasklet
 */
void ath9k_rx_tasklet(ath9k_softc_t* sc) {
    ath9k_hw_t* ah = sc->ah;
    ath9k_rx_queue_t* rxq = ah->rx_queue;

    if (!rxq) {
        return;
    }

    // Process received frames
    int processed = 0;
    while (processed < 64) {  // Limit to 64 frames per tasklet
        ath9k_desc_t* desc = &rxq->desc_ring[rxq->head];

        // Check if descriptor is complete
        if (!(desc->ds_hw0 & ATH9K_RXDESC_DONE)) {
            break;  // No more frames
        }

        // Check for errors
        if (desc->ds_hw0 & (ATH9K_RXDESC_ERR_CRC | ATH9K_RXDESC_ERR_PHY |
                            ATH9K_RXDESC_ERR_MIC)) {
            kprintf("ath9k_rx: Frame error (status=0x%08x)\n", desc->ds_hw0);
            ah->rx_errors++;
            goto next;
        }

        // Get frame length
        uint16_t len = desc->ds_hw1 & 0xFFFF;
        if (len == 0 || len > ATH_RX_BUF_SIZE) {
            kprintf("ath9k_rx: Invalid frame length %d\n", len);
            goto next;
        }

        // Process frame
        ath9k_rx_process_frame(sc, desc, rxq->buffers[rxq->head], len);

next:
        // Clear descriptor done bit
        desc->ds_hw0 &= ~ATH9K_RXDESC_DONE;

        // Advance head
        rxq->head = (rxq->head + 1) % rxq->size;
        processed++;
    }

    if (processed > 0) {
        kprintf("ath9k_rx: Processed %d frames\n", processed);
    }
}
