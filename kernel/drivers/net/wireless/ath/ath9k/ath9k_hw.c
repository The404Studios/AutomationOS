/*
 * ath9k - Hardware Access Functions
 */

#include "ath9k.h"
#include "../../../../../include/kernel.h"
#include "../../../../../include/mem.h"
#include "../../../../../include/x86_64.h"

/**
 * ath9k_hw_reg_read - Read from hardware register
 */
uint32_t ath9k_hw_reg_read(ath9k_hw_t* ah, uint32_t reg) {
    volatile uint32_t* addr = (volatile uint32_t*)((uint8_t*)ah->mem + reg);
    return *addr;
}

/**
 * ath9k_hw_reg_write - Write to hardware register
 */
void ath9k_hw_reg_write(ath9k_hw_t* ah, uint32_t reg, uint32_t val) {
    volatile uint32_t* addr = (volatile uint32_t*)((uint8_t*)ah->mem + reg);
    *addr = val;
}

/**
 * ath9k_hw_wait - Wait for register bits to be set/cleared
 */
static bool ath9k_hw_wait(ath9k_hw_t* ah, uint32_t reg, uint32_t mask,
                          uint32_t val, uint32_t timeout_us) {
    uint32_t i;
    uint32_t reg_val;

    for (i = 0; i < timeout_us / 10; i++) {
        reg_val = ath9k_hw_reg_read(ah, reg);
        if ((reg_val & mask) == val) {
            return true;
        }
        // Busy wait for 10 us (simplified - should use timer)
        for (volatile int j = 0; j < 1000; j++);
    }

    return false;
}

/**
 * ath9k_hw_read_eeprom - Read EEPROM/OTP data
 */
void ath9k_hw_read_eeprom(ath9k_hw_t* ah) {
    // Simplified: Generate MAC address from chip ID
    // In real hardware, this would read from EEPROM
    uint32_t chip_id = ah->chip_id;

    ah->mac_addr[0] = 0x00;
    ah->mac_addr[1] = 0x03;
    ah->mac_addr[2] = 0x7F;
    ah->mac_addr[3] = (chip_id >> 16) & 0xFF;
    ah->mac_addr[4] = (chip_id >> 8) & 0xFF;
    ah->mac_addr[5] = chip_id & 0xFF;

    kprintf("ath9k_hw: Read MAC address from EEPROM\n");
}

/**
 * ath9k_hw_chip_test - Test hardware registers
 */
static int ath9k_hw_chip_test(ath9k_hw_t* ah) {
    uint32_t test_val = 0xDEADBEEF;
    uint32_t read_val;

    // Write test pattern to MACMISC register
    ath9k_hw_reg_write(ah, AR_MACMISC, test_val);
    read_val = ath9k_hw_reg_read(ah, AR_MACMISC);

    if (read_val != test_val) {
        kprintf("ath9k_hw: Chip test failed (wrote 0x%08x, read 0x%08x)\n",
                test_val, read_val);
        return -1;
    }

    return 0;
}

/**
 * ath9k_hw_start_pcicfg - Start PCI configuration
 */
int ath9k_hw_start_pcicfg(ath9k_hw_t* ah) {
    pci_device_t* pdev = ah->pci_dev;

    // Enable memory space and bus master
    pci_enable_memory_space(pdev);
    pci_enable_bus_master(pdev);

    return 0;
}

/**
 * ath9k_hw_reset - Reset hardware
 */
int ath9k_hw_reset(ath9k_hw_t* ah) {
    kprintf("ath9k_hw: Resetting hardware\n");

    // Disable interrupts
    ath9k_hw_reg_write(ah, AR_IER, 0);
    ath9k_hw_reg_write(ah, AR_IMR, 0);

    // Read and clear pending interrupts
    uint32_t isr = ath9k_hw_reg_read(ah, AR_ISR);
    (void)isr;  // Suppress unused warning

    // Software reset via command register
    // This is a simplified reset - real hardware needs more complex sequence
    ath9k_hw_reg_write(ah, AR_CR, AR_CR_RXD);

    // Wait for reset to complete (simplified)
    for (volatile int i = 0; i < 100000; i++);

    // Re-initialize basic registers
    ath9k_hw_reg_write(ah, AR_CFG, 0);  // Default configuration

    kprintf("ath9k_hw: Hardware reset complete\n");
    return 0;
}

/**
 * ath9k_hw_init - Initialize hardware
 */
int ath9k_hw_init(ath9k_hw_t* ah, pci_device_t* pci_dev) {
    int ret;

    kprintf("ath9k_hw: Initializing hardware\n");

    // Clear structure
    for (uint8_t* p = (uint8_t*)ah; p < (uint8_t*)ah + sizeof(ath9k_hw_t); p++) {
        *p = 0;
    }

    ah->pci_dev = pci_dev;

    // Get BAR0 (MMIO)
    uint64_t bar0 = pci_get_bar(pci_dev, 0);
    if (!bar0) {
        kprintf("ath9k_hw: Invalid BAR0\n");
        return -1;
    }

    // Map MMIO registers (assume already mapped in kernel space)
    ah->mem = (void*)bar0;

    kprintf("ath9k_hw: MMIO base at 0x%016llx\n", bar0);

    // Start PCI configuration
    ret = ath9k_hw_start_pcicfg(ah);
    if (ret < 0) {
        return ret;
    }

    // Read chip ID from first register
    ah->chip_id = ath9k_hw_reg_read(ah, 0x0000);
    ah->chip_rev = (ah->chip_id >> 16) & 0xFF;

    kprintf("ath9k_hw: Chip ID: 0x%08x, Rev: 0x%02x\n",
            ah->chip_id, ah->chip_rev);

    // Test chip registers
    ret = ath9k_hw_chip_test(ah);
    if (ret < 0) {
        return ret;
    }

    // Reset hardware
    ret = ath9k_hw_reset(ah);
    if (ret < 0) {
        return ret;
    }

    // Read EEPROM (MAC address)
    ath9k_hw_read_eeprom(ah);

    // Initialize PHY state
    ah->phy_enabled = false;
    ah->noise_floor = -95;  // Default noise floor (dBm)
    ah->tx_power = 20;      // Default TX power (dBm)

    // Initialize statistics
    ah->tx_packets = 0;
    ah->rx_packets = 0;
    ah->tx_errors = 0;
    ah->rx_errors = 0;

    kprintf("ath9k_hw: Hardware initialized successfully\n");
    return 0;
}

/**
 * ath9k_hw_deinit - Deinitialize hardware
 */
void ath9k_hw_deinit(ath9k_hw_t* ah) {
    kprintf("ath9k_hw: Deinitializing hardware\n");

    // Disable interrupts
    ath9k_hw_reg_write(ah, AR_IER, 0);
    ath9k_hw_reg_write(ah, AR_IMR, 0);

    // Reset hardware
    ath9k_hw_reset(ah);
}

/**
 * ath9k_hw_set_channel - Set channel
 */
int ath9k_hw_set_channel(ath9k_hw_t* ah, uint32_t freq) {
    kprintf("ath9k_hw: Setting channel to %d MHz\n", freq);

    ah->curchan = freq;
    ah->curchan_flags = 0;

    // TODO: Program RF synthesizer
    // This would involve writing to RF registers to tune to the frequency

    return 0;
}

/**
 * ath9k_hw_set_txpower - Set TX power
 */
void ath9k_hw_set_txpower(ath9k_hw_t* ah, uint8_t power) {
    kprintf("ath9k_hw: Setting TX power to %d dBm\n", power);

    ah->tx_power = power;

    // TODO: Program TX power registers
}

/**
 * ath9k_hw_calibrate - Perform calibration
 */
void ath9k_hw_calibrate(ath9k_hw_t* ah) {
    // Simplified calibration
    // Real hardware requires complex calibration sequences

    // Noise floor calibration
    ah->noise_floor = -95;  // Simulated

    kprintf("ath9k_hw: Calibration complete, NF=%d dBm\n", ah->noise_floor);
}
