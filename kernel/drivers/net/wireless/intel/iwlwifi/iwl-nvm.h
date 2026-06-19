/*
 * iwl-nvm.h -- read the MAC address + channel list from EEPROM/OTP (DVM).
 * ======================================================================
 * iwl_read_nvm() reads the radio's non-volatile memory: the 6-byte MAC address
 * and the supported channel list. DVM 5000/5100/5300 carry an EEPROM; 6005/
 * 6000g2 carry OTP (one-time-programmable) read through the same CSR_EEPROM_REG
 * front-end after an OTP-access enable. Mirrors Linux iwl-eeprom-read.c
 * (iwl_read_eeprom / iwl_init_otp_access / iwl_read_otp_word).
 *
 *   ===================  HELD FOR HARDWARE  ===================
 *   Never runs at boot; correct-by-review only. Bounded, markers, abort-clean.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-nvm.h
 */
#ifndef IWL_NVM_H
#define IWL_NVM_H

#include "types.h"
#include "iwl-trans.h"

#define IWL_NVM_MAX_CHANNELS  64

typedef struct iwl_nvm_channel {
    uint16_t number;     /* channel number (1..14 for 2.4GHz; 36.. for 5GHz) */
    uint8_t  band;       /* 0 = 2.4GHz, 1 = 5GHz */
} iwl_nvm_channel_t;

typedef struct iwl_nvm_data {
    uint8_t           mac[6];
    iwl_nvm_channel_t channels[IWL_NVM_MAX_CHANNELS];
    int               n_channels;
} iwl_nvm_data_t;

/*
 * iwl_read_nvm -- read MAC + channels from EEPROM or OTP. Auto-detects which via
 * CSR_OTP_GP_REG.DEVICE_SELECT (and the family's known EEPROM-only cards). Fills
 * `out`, and copies the MAC into trans->mac (publishable into the netif).
 * Returns 0 on success, -1 on any bounded failure.
 */
int iwl_read_nvm(struct iwl_trans* trans, iwl_nvm_data_t* out);

#endif /* IWL_NVM_H */
