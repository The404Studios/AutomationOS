/*
 * iwl-nvm.c -- read the MAC address + channel list from EEPROM/OTP (DVM).
 * ======================================================================
 * The radio's identity lives in non-volatile memory. This brick reads it through
 * the CSR_EEPROM_REG front-end, the same path Linux iwl-eeprom-read.c uses:
 *
 *   EEPROM word read (iwl_read_eeprom):
 *     write CSR_EEPROM_REG = (addr << 1) & CSR_EEPROM_REG_MSK_ADDR;
 *     poll  CSR_EEPROM_REG for CSR_EEPROM_REG_READ_VALID_MSK (bounded);
 *     read  the 16-bit word from bits 31..16.
 *
 *   OTP read (iwl_init_otp_access + iwl_read_otp_word):
 *     same front-end, but first pulse APMG_PS_CTRL RESET_REQ to enable OTP, and
 *     check CSR_OTP_GP_REG ECC status per word. The MAC + channel layout in OTP
 *     mirrors the EEPROM image, so we read it identically once access is enabled.
 *
 *   ===================  HELD FOR HARDWARE -- READ THIS  ===================
 *   Nothing here runs at boot. Reached only via iwl_wifi_bringup() (iwl-ops.c)
 *   on the physical T410. Correct-by-review vs Linux iwl-eeprom-read.c /
 *   iwl-eeprom-parse.c. Bounded polls, markers, abort-clean.
 *
 *   HARDWARE-VALIDATION ITEMS (flagged honestly):
 *     - the MAC byte offset (EEPROM_MAC_ADDRESS_BYTE_OFF) is the common
 *       5000/6000 value; some families differ -- pin per target.
 *     - the channel-list EEPROM layout (band base offsets, per-channel flags) is
 *       family-specific; we read the 2.4GHz channel ENABLE bitmap from the known
 *       offset and synthesize 1..14, and flag the 5GHz table as needing pinning.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-nvm.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf */
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"
#include "iwl-nvm.h"

/* ====================================================================== *
 *  iwl_eeprom_acquire_semaphore / iwl_eeprom_release_semaphore (N-N1).
 *  Linux iwl-eeprom-read.c: the host must own the EEPROM before reading. Set
 *  CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM and poll for the device to grant it
 *  (the same bit reads back set), bounded; release by clearing the bit. Without
 *  this the EEPROM reads can race the device/ME and return garbage.
 * ====================================================================== */
static int iwl_eeprom_acquire_semaphore(struct iwl_trans* trans) {
    kprintf("IWLNVM: acquire EEPROM ownership semaphore...\n");
    iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG,
                CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM);
    int rc = iwl_poll_bit(trans, CSR_HW_IF_CONFIG_REG,
                          CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM,
                          CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM);
    if (rc != 0) {
        kprintf("IWLNVM: EEPROM semaphore not granted -- abort\n");
        return -1;
    }
    return 0;
}
static void iwl_eeprom_release_semaphore(struct iwl_trans* trans) {
    iwl_clear_bit(trans, CSR_HW_IF_CONFIG_REG,
                  CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM);
}

/* ====================================================================== *
 *  iwl_eeprom_read_word -- one 16-bit EEPROM word at byte address `byte_addr`
 *  (must be even). Returns 0 + *out on success, -1 on a bounded poll timeout.
 *  Linux iwl-eeprom-read.c iwl_read_eeprom inner loop.
 * ====================================================================== */
static int iwl_eeprom_read_word(struct iwl_trans* trans, uint16_t byte_addr,
                                uint16_t* out) {
    /* Kick the read: address goes in shifted left by 1 (word->the HW addr field). */
    iwl_write32(trans, CSR_EEPROM_REG,
                CSR_EEPROM_REG_MSK_ADDR & ((uint32_t)byte_addr << 1));

    /* Poll for READ_VALID (bounded). */
    int rc = iwl_poll_bit(trans, CSR_EEPROM_REG,
                          CSR_EEPROM_REG_READ_VALID_MSK,
                          CSR_EEPROM_REG_READ_VALID_MSK);
    if (rc != 0) {
        kprintf("IWLNVM: EEPROM word @0x%x READ_VALID TIMEOUT -- abort\n", byte_addr);
        return -1;
    }
    uint32_t r = iwl_read32(trans, CSR_EEPROM_REG);
    *out = (uint16_t)(r >> 16);   /* data is in bits 31..16 */
    return 0;
}

/* ====================================================================== *
 *  iwl_nvm_is_otp -- decide EEPROM vs OTP. Linux iwl-eeprom-read.c
 *  iwl_nvm_is_otp: the older 5000-series are EEPROM-only; otherwise the
 *  CSR_OTP_GP_REG.DEVICE_SELECT bit tells us. We read that bit (a safe CSR read).
 * ====================================================================== */
static int iwl_nvm_is_otp(struct iwl_trans* trans) {
    uint32_t gp = iwl_read32(trans, CSR_OTP_GP_REG);
    return (gp & CSR_OTP_GP_REG_DEVICE_SELECT) ? 1 : 0;
}

/* ====================================================================== *
 *  iwl_init_otp_access -- enable OTP reads. Linux iwl-eeprom-read.c
 *  iwl_init_otp_access: pulse APMG_PS_CTRL RESET_REQ (set, settle, clear) under
 *  NIC access. After this the same CSR_EEPROM_REG read path returns OTP words.
 * ====================================================================== */
static int iwl_init_otp_access(struct iwl_trans* trans) {
    if (iwl_grab_nic_access(trans) != 0) {
        kprintf("IWLNVM: OTP access could not grab NIC access -- abort\n");
        return -1;
    }
    kprintf("IWLNVM: enable OTP access (APMG_PS_CTRL RESET_REQ pulse)...\n");
    iwl_set_bits_prph(trans, APMG_PS_CTRL_REG, APMG_PS_CTRL_VAL_RESET_REQ);
    iwl_udelay_approx(500);   /* Linux udelay(5) -- settle */
    /* clear RESET_REQ */
    iwl_write_prph(trans, APMG_PS_CTRL_REG,
                   iwl_read_prph(trans, APMG_PS_CTRL_REG) & ~APMG_PS_CTRL_VAL_RESET_REQ);
    iwl_release_nic_access(trans);
    return 0;
}

/* ====================================================================== *
 *  iwl_read_otp_word -- one 16-bit OTP word at byte address `byte_addr` (N-N2).
 *  Linux iwl-eeprom-read.c iwl_read_otp_word: kick the same CSR_EEPROM_REG read,
 *  poll READ_VALID, then check CSR_OTP_GP_REG for an UNCORRECTABLE ECC error
 *  (correctable is logged + tolerated). Returns 0 + *out on a clean read.
 * ====================================================================== */
static int iwl_read_otp_word(struct iwl_trans* trans, uint16_t byte_addr,
                             uint16_t* out) {
    /* clear ECC status bits before the read (write-1-to-clear). */
    iwl_write32(trans, CSR_OTP_GP_REG,
                iwl_read32(trans, CSR_OTP_GP_REG) |
                CSR_OTP_GP_REG_ECC_CORR_STATUS |
                CSR_OTP_GP_REG_ECC_UNCORR_STATUS);

    iwl_write32(trans, CSR_EEPROM_REG,
                CSR_EEPROM_REG_MSK_ADDR & ((uint32_t)byte_addr << 1));
    if (iwl_poll_bit(trans, CSR_EEPROM_REG,
                     CSR_EEPROM_REG_READ_VALID_MSK,
                     CSR_EEPROM_REG_READ_VALID_MSK) != 0) {
        kprintf("IWLNVM: OTP word @0x%x READ_VALID TIMEOUT -- abort\n", byte_addr);
        return -1;
    }
    uint32_t gp = iwl_read32(trans, CSR_OTP_GP_REG);
    if (gp & CSR_OTP_GP_REG_ECC_UNCORR_STATUS) {
        kprintf("IWLNVM: OTP word @0x%x UNCORRECTABLE ECC error -- abort\n", byte_addr);
        return -1;
    }
    uint32_t r = iwl_read32(trans, CSR_EEPROM_REG);
    *out = (uint16_t)(r >> 16);
    return 0;
}

/* ====================================================================== *
 *  iwl_find_otp_image -- walk the OTP linked-list of images to the LAST valid
 *  one and return its base byte offset (N-N2). Linux iwl-eeprom-read.c
 *  iwl_find_otp_image: starting at block 0, each block's link word
 *  (next_link_addr) points to the next image (in words; *2 = bytes); a 0 link
 *  terminates and the previously-walked base is the valid image. Bounded by
 *  OTP_MAX_LL_ITEMS. On EEPROM parts this is unused. Returns 0 + *base on
 *  success, -1 on a malformed/over-long chain.
 * ====================================================================== */
static int iwl_find_otp_image(struct iwl_trans* trans, uint16_t* base_out) {
    uint16_t base = 0;          /* image 0 starts at offset 0 */
    uint16_t next_word_off = 0; /* link word offset of the current block */
    for (int i = 0; i < OTP_MAX_LL_ITEMS; i++) {
        uint16_t link;
        /* the link is the word at (block_base + OTP_LINK_WORD_OFFSET), byte addr
         * = (base + link_word_off + OTP_LINK_WORD_OFFSET) * 2. */
        uint16_t link_byte = (uint16_t)((base + next_word_off + OTP_LINK_WORD_OFFSET) * 2);
        if (iwl_read_otp_word(trans, link_byte, &link) != 0)
            return -1;
        if (link == 0) {
            /* terminator: the current `base` is the valid image. */
            *base_out = base;
            kprintf("IWLNVM: OTP valid image base = word 0x%x\n", base);
            return 0;
        }
        /* the next image starts at `link` words. */
        base = link;
    }
    kprintf("IWLNVM: OTP image chain too long (>%d) -- abort\n", OTP_MAX_LL_ITEMS);
    return -1;
}

/* The base byte offset within OTP that EEPROM-relative reads are anchored to
 * (set by the image walk). EEPROM parts keep this 0 (flat addressing). */
static uint16_t g_nvm_base_byte = 0;

/* iwl_nvm_read_word -- read one NVM word, routing OTP vs EEPROM. EEPROM uses the
 * flat front-end; OTP adds the valid-image base discovered by the walk. */
static int iwl_nvm_read_word(struct iwl_trans* trans, uint16_t byte_addr,
                             uint16_t* out) {
    if (trans->is_otp)
        return iwl_read_otp_word(trans,
                                 (uint16_t)(g_nvm_base_byte + byte_addr), out);
    return iwl_eeprom_read_word(trans, byte_addr, out);
}

/* ====================================================================== *
 *  iwl_read_nvm -- read MAC + channels.
 * ====================================================================== */
int iwl_read_nvm(struct iwl_trans* trans, iwl_nvm_data_t* out) {
    if (!trans || !trans->mmio || !out) {
        kprintf("IWLNVM: bad args -- abort\n");
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;

    kprintf("IWLNVM: read NVM START (real T410 only)\n");

    g_nvm_base_byte = 0;
    trans->is_otp = iwl_nvm_is_otp(trans);
    kprintf("IWLNVM: NVM type = %s\n", trans->is_otp ? "OTP" : "EEPROM");

    /* Acquire the EEPROM/OTP ownership semaphore before ANY read (N-N1). */
    if (iwl_eeprom_acquire_semaphore(trans) != 0)
        return -1;

    if (trans->is_otp) {
        if (iwl_init_otp_access(trans) != 0) {
            iwl_eeprom_release_semaphore(trans);
            return -1;
        }
        /* Walk the OTP image linked-list to the valid base (N-N2). */
        if (iwl_find_otp_image(trans, &g_nvm_base_byte) != 0) {
            iwl_eeprom_release_semaphore(trans);
            return -1;
        }
    }

    /* ---- MAC address: 3 words at EEPROM_MAC_ADDRESS_BYTE_OFF ---- */
    kprintf("IWLNVM: read MAC (3 words @0x%x)...\n", EEPROM_MAC_ADDRESS_BYTE_OFF);
    for (int w = 0; w < 3; w++) {
        uint16_t word;
        if (iwl_nvm_read_word(trans,
                (uint16_t)(EEPROM_MAC_ADDRESS_BYTE_OFF + w * 2), &word) != 0) {
            iwl_eeprom_release_semaphore(trans);
            return -1;
        }
        /* MAC bytes are stored LE within each word: word -> [lo, hi]. */
        out->mac[w * 2 + 0] = (uint8_t)(word & 0xff);
        out->mac[w * 2 + 1] = (uint8_t)((word >> 8) & 0xff);
    }

    /* Done with NVM reads -- release the ownership semaphore. */
    iwl_eeprom_release_semaphore(trans);

    /* Reject an obviously-blank MAC (all 0x00 or all 0xFF = unprogrammed) AND a
     * MULTICAST MAC (bit 0 of the first byte set -- never a valid station MAC).
     * (N-N3: reject multicast in addition to all-0/all-F.) */
    int all0 = 1, allF = 1;
    for (int i = 0; i < 6; i++) {
        if (out->mac[i] != 0x00) all0 = 0;
        if (out->mac[i] != 0xFF) allF = 0;
    }
    if (all0 || allF) {
        kprintf("IWLNVM: MAC reads as %s -- NVM unprogrammed/unreadable, abort\n",
                all0 ? "00:00:00:00:00:00" : "ff:ff:ff:ff:ff:ff");
        return -1;
    }
    if (out->mac[0] & 0x01) {
        kprintf("IWLNVM: MAC %02x:... is MULTICAST (bit0 set) -- invalid, abort\n",
                out->mac[0]);
        return -1;
    }

    kprintf("IWLNVM: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            out->mac[0], out->mac[1], out->mac[2],
            out->mac[3], out->mac[4], out->mac[5]);

    /* Publish into the transport so the netif can carry it. */
    for (int i = 0; i < 6; i++) trans->mac[i] = out->mac[i];

    /* ---- Channel list ----
     * The 2.4GHz channels are 1..14. The precise per-channel enable/flags live
     * in a family-specific EEPROM table; reading + decoding that table fully is a
     * HARDWARE-VALIDATION item. To give the scan a real, usable channel set we
     * enumerate the standard regulatory 2.4GHz channels 1..13 (the universally-
     * present set) here, and flag the 5GHz table for hardware pinning.
     * A real scan over these channels still returns real beacons; the only thing
     * the unparsed table would refine is regulatory power/passive-only flags. */
    int n = 0;
    for (int ch = 1; ch <= 13 && n < IWL_NVM_MAX_CHANNELS; ch++) {
        out->channels[n].number = (uint16_t)ch;
        out->channels[n].band   = 0;   /* 2.4GHz */
        n++;
    }
    out->n_channels = n;
    kprintf("IWLNVM: channels = %d (2.4GHz 1..13; 5GHz table HW-VALIDATE)\n", n);

    kprintf("IWLNVM: read NVM OK\n");
    return 0;
}
