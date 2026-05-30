/*
 * ath9k - Interrupt Handling
 */

#include "ath9k.h"
#include "../../../../../include/kernel.h"
#include "../../../../../include/x86_64.h"

// Forward declaration
extern ath9k_softc_t* ath9k_get_global_softc(void);

/**
 * ath9k_irq_handler - IRQ handler
 */
void ath9k_irq_handler(void) {
    ath9k_softc_t* sc = ath9k_get_global_softc();
    ath9k_hw_t* ah;
    uint32_t isr, pisr;

    if (!sc) {
        return;
    }

    ah = sc->ah;

    // Read interrupt status
    isr = ath9k_hw_reg_read(ah, AR_ISR);
    pisr = ath9k_hw_reg_read(ah, AR_PISR);

    // Clear interrupts (write 1 to clear)
    ath9k_hw_reg_write(ah, AR_ISR, isr);
    ath9k_hw_reg_write(ah, AR_PISR, pisr);

    sc->irq_count++;

    // Handle RX interrupts
    if (isr & (AR_ISR_RXOK | AR_ISR_RXDESC | AR_ISR_RXEOL | AR_ISR_RXORN)) {
        sc->rx_irq_count++;
        ath9k_rx_tasklet(sc);
    }

    // Handle TX interrupts
    if (isr & (AR_ISR_TXOK | AR_ISR_TXDESC | AR_ISR_TXEOL | AR_ISR_TXURN)) {
        sc->tx_irq_count++;
        ath9k_tx_tasklet(sc);
    }

    // Handle beacon interrupts
    if (isr & AR_ISR_SWBA) {
        // Software beacon alert (AP mode)
        // TODO: Send beacon
    }

    // Handle beacon miss
    if (isr & AR_ISR_BMISS) {
        kprintf("ath9k: Beacon miss\n");
    }

    // Handle errors
    if (isr & AR_ISR_RXERR) {
        kprintf("ath9k: RX error\n");
    }

    if (isr & AR_ISR_TXERR) {
        kprintf("ath9k: TX error\n");
    }

    if (isr & AR_ISR_RXORN) {
        kprintf("ath9k: RX overrun\n");
    }

    if (isr & AR_ISR_TXURN) {
        kprintf("ath9k: TX underrun\n");
    }
}

/**
 * ath9k_tasklet_schedule - Schedule tasklets
 */
void ath9k_tasklet_schedule(ath9k_softc_t* sc) {
    // In a real OS, this would schedule deferred work
    // For now, we just call the tasklets directly
    ath9k_rx_tasklet(sc);
    ath9k_tx_tasklet(sc);
}
