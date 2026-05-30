/*
 * ath9k - PHY/RF Management
 */

#include "ath9k.h"
#include "../../../../../include/kernel.h"

// PHY Register Offsets (simplified)
#define AR_PHY_BASE             0x9800
#define AR_PHY_TEST             (AR_PHY_BASE + 0x0000)
#define AR_PHY_TURBO            (AR_PHY_BASE + 0x0004)
#define AR_PHY_RF_CTL2          (AR_PHY_BASE + 0x0008)
#define AR_PHY_RF_CTL3          (AR_PHY_BASE + 0x000C)
#define AR_PHY_RF_CTL4          (AR_PHY_BASE + 0x0010)
#define AR_PHY_SETTLING         (AR_PHY_BASE + 0x0044)
#define AR_PHY_RXGAIN           (AR_PHY_BASE + 0x0048)
#define AR_PHY_DESIRED_SZ       (AR_PHY_BASE + 0x0050)
#define AR_PHY_FIND_SIG         (AR_PHY_BASE + 0x0058)
#define AR_PHY_AGC_CTL1         (AR_PHY_BASE + 0x005C)
#define AR_PHY_AGC_CONTROL      (AR_PHY_BASE + 0x0000)
#define AR_PHY_CCA              (AR_PHY_BASE + 0x0064)
#define AR_PHY_SFCORR_LOW       (AR_PHY_BASE + 0x0068)
#define AR_PHY_SFCORR           (AR_PHY_BASE + 0x006C)
#define AR_PHY_SLEEP_CTR_CONTROL (AR_PHY_BASE + 0x0070)
#define AR_PHY_SLEEP_SCAL       (AR_PHY_BASE + 0x0074)
#define AR_PHY_PLL_CTL          (AR_PHY_BASE + 0x0078)
#define AR_PHY_BIN_MASK_1       (AR_PHY_BASE + 0x0080)
#define AR_PHY_BIN_MASK_2       (AR_PHY_BASE + 0x0084)
#define AR_PHY_BIN_MASK_3       (AR_PHY_BASE + 0x0088)

// PHY Test Register Bits
#define AR_PHY_TEST_RFSILENT_BB 0x00002000
#define AR_PHY_TEST_RFSILENT_FORCE 0x00004000

// PHY Turbo Register Bits
#define AR_PHY_FC_TURBO_MODE    0x00000001
#define AR_PHY_FC_TURBO_SHORT   0x00000002
#define AR_PHY_FC_DYN2040_EN    0x00000004
#define AR_PHY_FC_DYN2040_PRI_ONLY 0x00000008
#define AR_PHY_FC_DYN2040_PRI_CH 0x00000010
#define AR_PHY_FC_DYN2040_EXT_CH 0x00000020
#define AR_PHY_FC_HT_EN         0x00000040
#define AR_PHY_FC_SHORT_GI_40   0x00000080
#define AR_PHY_FC_WALSH         0x00000100
#define AR_PHY_FC_SINGLE_HT_LTF1 0x00000200

/**
 * ath9k_hw_phy_init - Initialize PHY
 */
int ath9k_hw_phy_init(ath9k_hw_t* ah) {
    kprintf("ath9k_phy: Initializing PHY\n");

    // Disable RF Silent
    uint32_t val = ath9k_hw_reg_read(ah, AR_PHY_TEST);
    val &= ~(AR_PHY_TEST_RFSILENT_BB | AR_PHY_TEST_RFSILENT_FORCE);
    ath9k_hw_reg_write(ah, AR_PHY_TEST, val);

    // Setup basic PHY parameters
    ath9k_hw_reg_write(ah, AR_PHY_SETTLING, 0x1372161C);
    ath9k_hw_reg_write(ah, AR_PHY_DESIRED_SZ, 0x00000000);
    ath9k_hw_reg_write(ah, AR_PHY_AGC_CTL1, 0x1E1F2022);
    ath9k_hw_reg_write(ah, AR_PHY_CCA, 0x0007FFFF);
    ath9k_hw_reg_write(ah, AR_PHY_SFCORR_LOW, 0x1B1F1F1E);
    ath9k_hw_reg_write(ah, AR_PHY_SFCORR, 0x0B222220);

    // Enable HT (High Throughput - 802.11n)
    val = ath9k_hw_reg_read(ah, AR_PHY_TURBO);
    val |= AR_PHY_FC_HT_EN;
    val &= ~AR_PHY_FC_TURBO_MODE;  // Disable turbo mode
    ath9k_hw_reg_write(ah, AR_PHY_TURBO, val);

    // Setup AGC (Automatic Gain Control)
    ath9k_hw_reg_write(ah, AR_PHY_AGC_CONTROL, 0x00000000);

    // Setup RX gain
    ath9k_hw_reg_write(ah, AR_PHY_RXGAIN, 0x0000000E);

    // Initialize RF
    kprintf("ath9k_phy: Initializing RF\n");
    // TODO: RF initialization sequence (vendor-specific)

    // Calibrate
    ath9k_hw_calibrate(ah);

    ah->phy_enabled = true;

    kprintf("ath9k_phy: PHY initialized successfully\n");
    return 0;
}

/**
 * ath9k_phy_set_channel - Set PHY channel
 */
int ath9k_phy_set_channel(ath9k_hw_t* ah, uint32_t freq) {
    uint32_t channel_flags = 0;

    kprintf("ath9k_phy: Setting channel to %d MHz\n", freq);

    // Determine channel type (20/40 MHz)
    if (ah->curchan_flags & 0x01) {
        // 40 MHz channel
        channel_flags = AR_PHY_FC_DYN2040_EN | AR_PHY_FC_SHORT_GI_40;
    } else {
        // 20 MHz channel
        channel_flags = 0;
    }

    // Update turbo register
    uint32_t val = ath9k_hw_reg_read(ah, AR_PHY_TURBO);
    val &= ~(AR_PHY_FC_DYN2040_EN | AR_PHY_FC_SHORT_GI_40);
    val |= channel_flags;
    ath9k_hw_reg_write(ah, AR_PHY_TURBO, val);

    // Program RF synthesizer
    // TODO: Frequency-specific RF programming

    // Calibrate for new channel
    ath9k_hw_calibrate(ah);

    return 0;
}

/**
 * ath9k_phy_enable - Enable PHY
 */
void ath9k_phy_enable(ath9k_hw_t* ah) {
    if (ah->phy_enabled) {
        return;
    }

    kprintf("ath9k_phy: Enabling PHY\n");

    // Enable PHY test register
    uint32_t val = ath9k_hw_reg_read(ah, AR_PHY_TEST);
    val &= ~(AR_PHY_TEST_RFSILENT_BB | AR_PHY_TEST_RFSILENT_FORCE);
    ath9k_hw_reg_write(ah, AR_PHY_TEST, val);

    ah->phy_enabled = true;
}

/**
 * ath9k_phy_disable - Disable PHY
 */
void ath9k_phy_disable(ath9k_hw_t* ah) {
    if (!ah->phy_enabled) {
        return;
    }

    kprintf("ath9k_phy: Disabling PHY\n");

    // Force RF silent
    uint32_t val = ath9k_hw_reg_read(ah, AR_PHY_TEST);
    val |= AR_PHY_TEST_RFSILENT_BB | AR_PHY_TEST_RFSILENT_FORCE;
    ath9k_hw_reg_write(ah, AR_PHY_TEST, val);

    ah->phy_enabled = false;
}
