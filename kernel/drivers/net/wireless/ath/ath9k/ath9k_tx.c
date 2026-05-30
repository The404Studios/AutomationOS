/*
 * ath9k - TX (Transmit) Handling
 */

#include "ath9k.h"
#include "../../../../../include/kernel.h"
#include "../../../../../include/mem.h"

/**
 * ath9k_tx_init - Initialize TX queues
 */
int ath9k_tx_init(ath9k_softc_t* sc) {
    ath9k_hw_t* ah = sc->ah;
    int ret;

    kprintf("ath9k_tx: Initializing TX queues\n");

    // Allocate TX queues (4 queues: BE, BK, VI, VO)
    for (int i = 0; i < 4; i++) {
        ath9k_tx_queue_t* txq = kmalloc(sizeof(ath9k_tx_queue_t));
        if (!txq) {
            kprintf("ath9k_tx: Failed to allocate TX queue %d\n", i);
            ret = -1;
            goto fail;
        }

        // Clear structure
        for (uint8_t* p = (uint8_t*)txq; p < (uint8_t*)txq + sizeof(ath9k_tx_queue_t); p++) {
            *p = 0;
        }

        txq->qnum = i;
        txq->size = ATH_TXBUF;
        txq->head = 0;
        txq->tail = 0;

        // Allocate descriptor ring
        txq->desc_ring = pmm_alloc_page();
        if (!txq->desc_ring) {
            kprintf("ath9k_tx: Failed to allocate descriptor ring for queue %d\n", i);
            kfree(txq);
            ret = -1;
            goto fail;
        }

        // Get physical address (simplified - assume identity mapping)
        txq->desc_ring_phys = (uint64_t)txq->desc_ring;

        // Clear descriptors
        for (uint8_t* p = (uint8_t*)txq->desc_ring;
             p < (uint8_t*)txq->desc_ring + PAGE_SIZE; p++) {
            *p = 0;
        }

        // Allocate buffer pointers
        txq->buffers = kmalloc(sizeof(void*) * ATH_TXBUF);
        if (!txq->buffers) {
            kprintf("ath9k_tx: Failed to allocate buffer array for queue %d\n", i);
            pmm_free_page(txq->desc_ring);
            kfree(txq);
            ret = -1;
            goto fail;
        }

        // Clear buffer pointers
        for (int j = 0; j < ATH_TXBUF; j++) {
            txq->buffers[j] = NULL;
        }

        ah->tx_queues[i] = txq;
        kprintf("ath9k_tx: Queue %d initialized (ring at 0x%016llx)\n",
                i, txq->desc_ring_phys);
    }

    // Enable TX
    uint32_t txcfg = ath9k_hw_reg_read(ah, AR_TXCFG);
    txcfg |= 0x00000001;  // Enable TX DMA
    ath9k_hw_reg_write(ah, AR_TXCFG, txcfg);

    kprintf("ath9k_tx: TX initialized successfully\n");
    return 0;

fail:
    // Cleanup on failure
    for (int i = 0; i < 4; i++) {
        if (ah->tx_queues[i]) {
            if (ah->tx_queues[i]->buffers) {
                kfree(ah->tx_queues[i]->buffers);
            }
            if (ah->tx_queues[i]->desc_ring) {
                pmm_free_page(ah->tx_queues[i]->desc_ring);
            }
            kfree(ah->tx_queues[i]);
            ah->tx_queues[i] = NULL;
        }
    }
    return ret;
}

/**
 * ath9k_tx_deinit - Deinitialize TX queues
 */
void ath9k_tx_deinit(ath9k_softc_t* sc) {
    ath9k_hw_t* ah = sc->ah;

    kprintf("ath9k_tx: Deinitializing TX queues\n");

    // Disable TX
    uint32_t txcfg = ath9k_hw_reg_read(ah, AR_TXCFG);
    txcfg &= ~0x00000001;  // Disable TX DMA
    ath9k_hw_reg_write(ah, AR_TXCFG, txcfg);

    // Free TX queues
    for (int i = 0; i < 4; i++) {
        if (ah->tx_queues[i]) {
            // Free buffers
            if (ah->tx_queues[i]->buffers) {
                for (int j = 0; j < ATH_TXBUF; j++) {
                    if (ah->tx_queues[i]->buffers[j]) {
                        kfree(ah->tx_queues[i]->buffers[j]);
                    }
                }
                kfree(ah->tx_queues[i]->buffers);
            }

            // Free descriptor ring
            if (ah->tx_queues[i]->desc_ring) {
                pmm_free_page(ah->tx_queues[i]->desc_ring);
            }

            kfree(ah->tx_queues[i]);
            ah->tx_queues[i] = NULL;
        }
    }
}

/**
 * ath9k_tx_queue_frame - Queue frame for transmission
 */
int ath9k_tx_queue_frame(ath9k_softc_t* sc, uint8_t qnum, sk_buff_t* skb) {
    ath9k_hw_t* ah = sc->ah;
    ath9k_tx_queue_t* txq;
    ath9k_desc_t* desc;
    uint16_t tail;

    if (qnum >= 4) {
        kprintf("ath9k_tx: Invalid queue number %d\n", qnum);
        return -1;
    }

    txq = ah->tx_queues[qnum];
    if (!txq) {
        kprintf("ath9k_tx: Queue %d not initialized\n", qnum);
        return -1;
    }

    // Check if queue is full
    uint16_t next_tail = (txq->tail + 1) % txq->size;
    if (next_tail == txq->head) {
        kprintf("ath9k_tx: Queue %d full\n", qnum);
        return -1;
    }

    tail = txq->tail;

    // Get descriptor
    desc = &txq->desc_ring[tail];

    // Allocate TX buffer (copy SKB data)
    void* buf = kmalloc(skb->len);
    if (!buf) {
        kprintf("ath9k_tx: Failed to allocate TX buffer\n");
        return -1;
    }

    // Copy data
    for (uint16_t i = 0; i < skb->len; i++) {
        ((uint8_t*)buf)[i] = skb->data[i];
    }

    // Setup descriptor
    desc->ds_data = (uint32_t)(uint64_t)buf;  // Physical address (simplified)
    desc->ds_ctl0 = skb->len;  // Frame length
    desc->ds_ctl1 = ATH9K_TXDESC_INTREQ;  // Request interrupt on completion
    desc->ds_hw0 = 0;
    desc->ds_hw1 = 0;
    desc->ds_hw2 = 0;
    desc->ds_hw3 = 0;

    // Link to next descriptor
    uint16_t next_desc = (tail + 1) % txq->size;
    desc->ds_link = (uint32_t)(txq->desc_ring_phys + next_desc * sizeof(ath9k_desc_t));

    // Save buffer pointer
    txq->buffers[tail] = buf;

    // Update tail
    txq->tail = next_tail;

    // Ring doorbell (notify hardware)
    // TODO: Write to TX queue tail pointer register

    kprintf("ath9k_tx: Queued frame (len=%d) to queue %d\n", skb->len, qnum);

    return 0;
}

/**
 * ath9k_tx_tasklet - TX completion tasklet
 */
void ath9k_tx_tasklet(ath9k_softc_t* sc) {
    ath9k_hw_t* ah = sc->ah;

    // Process all TX queues
    for (int i = 0; i < 4; i++) {
        ath9k_tx_queue_t* txq = ah->tx_queues[i];
        if (!txq) {
            continue;
        }

        // Process completed descriptors
        while (txq->head != txq->tail) {
            ath9k_desc_t* desc = &txq->desc_ring[txq->head];

            // Check if descriptor is complete
            if (!(desc->ds_hw0 & ATH9K_TXDESC_INTREQ)) {
                break;  // Not complete yet
            }

            // Free buffer
            if (txq->buffers[txq->head]) {
                kfree(txq->buffers[txq->head]);
                txq->buffers[txq->head] = NULL;
            }

            // Clear descriptor
            desc->ds_hw0 = 0;
            desc->ds_hw1 = 0;

            // Advance head
            txq->head = (txq->head + 1) % txq->size;

            ah->tx_packets++;
        }
    }
}
