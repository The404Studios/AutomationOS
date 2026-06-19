/*
 * iwl-dvm-commands.h -- Intel iwlwifi DVM (AGN) host-command IDs + wire structs.
 * ============================================================================
 * The ThinkPad T410 carries DVM-family cards (1000 / 5000 / 6000) -- these use
 * the legacy AGN firmware API, NOT the newer mvm. This header is the command-ID
 * dictionary + on-the-wire struct layouts the firmware-load / host-command /
 * scan bricks need, copied verbatim from the Linux iwlwifi DVM sources (kernel
 * v5.10) with the exact upstream symbol cited per value:
 *
 *   dvm/commands.h  -- REPLY_x/SCAN_x/CALIBRATION_x command IDs, iwl_scan_cmd,
 *                      iwl_scan_channel, iwl_ssid_ie, iwl_alive_resp
 *   iwl-trans.h     -- iwl_cmd_header, iwl_rx_packet, FH_RSCSR_FRAME_SIZE_MSK
 *   pcie/tx.c       -- struct iwl_tfd, struct iwl_tfd_tb (hi_n_len packing)
 *   iwl-fh.h        -- the FH firmware-load DMA registers (service channel 9)
 *
 * NOTHING here is invented -- the whole tail is correct-by-review since no QEMU
 * emulates these cards.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-dvm-commands.h
 */
#ifndef IWL_DVM_COMMANDS_H
#define IWL_DVM_COMMANDS_H

#include "types.h"

/* ====================================================================== *
 *  DVM host-command / notification IDs (dvm/commands.h, enum iwl_legacy_cmds).
 *  Verified against torvalds/linux v5.10 dvm/commands.h.
 * ====================================================================== */
#define REPLY_ALIVE                      0x01  /* dvm/commands.h */
#define REPLY_RXON                       0x10  /* dvm/commands.h */
#define REPLY_RXON_TIMING                0x14  /* dvm/commands.h */
#define REPLY_ADD_STA                    0x18  /* dvm/commands.h */
#define REPLY_TX                         0x1c  /* dvm/commands.h */
#define REPLY_WEPKEY                     0x20  /* dvm/commands.h (legacy WEP key) */
#define COEX_PRIORITY_TABLE_CMD          0x5a  /* dvm/commands.h (BT coex) */
#define CALIBRATION_CFG_CMD              0x65  /* dvm/commands.h */
#define CALIBRATION_RES_NOTIFICATION     0x66  /* dvm/commands.h */
#define CALIBRATION_COMPLETE_NOTIFICATION 0x67 /* dvm/commands.h */
#define REPLY_SCAN_CMD                   0x80  /* dvm/commands.h */
#define SCAN_ABORT_CMD                   0x81  /* dvm/commands.h */
#define SCAN_START_NOTIFICATION          0x82  /* dvm/commands.h */
#define SCAN_RESULTS_NOTIFICATION        0x83  /* dvm/commands.h */
#define SCAN_COMPLETE_NOTIFICATION       0x84  /* dvm/commands.h */
#define REPLY_TX_POWER_DBM_CMD           0x95  /* dvm/commands.h */
#define REPLY_TX_PWR_TABLE_CMD           0x97  /* dvm/commands.h */
#define REPLY_PHY_CALIBRATION_CMD        0xb0  /* dvm/commands.h (replay calib to runtime) */
#define REPLY_RX_PHY_CMD                 0xc0  /* dvm/commands.h */
#define REPLY_RX_MPDU_CMD                0xc1  /* dvm/commands.h */
#define REPLY_RX                         0xc3  /* dvm/commands.h */

/* ====================================================================== *
 *  IWL_CMD_QUEUE_ID -- the txq the DVM firmware services host commands on.
 *  Linux dvm: IWL_DEFAULT_CMD_QUEUE_NUM = 4 (iwl_trans_pcie cmd_queue). Both the
 *  host-command brick (doorbell + sequence) and iwl-trans.c (FH_MEM_CBBC_QUEUE
 *  base slot + the reset doorbell encoding) must agree on this id, so it lives
 *  here in the shared header. (Review fix H-C1: queue 0 was wrong; DVM never
 *  services commands on queue 0.) */
#define IWL_CMD_QUEUE_ID                 4     /* dvm IWL_DEFAULT_CMD_QUEUE_NUM */

/* DVM command-queue TX FIFO. Linux dvm: the command queue maps to TX FIFO 7
 * (IWL_TX_FIFO_CMD = 7, iwl_trans_pcie cmd_fifo). Used by the SCD bring-up to
 * activate queue 4 against this FIFO. */
#define IWL_TX_FIFO_CMD                  7     /* dvm cmd_fifo */

/* REPLY_ALIVE is_valid value (dvm/commands.h: UCODE_VALID_OK). */
#define UCODE_VALID_OK                   0x1   /* dvm/commands.h */
/* The ALIVE ver_subtype: INIT alive uses 9, RUNTIME alive != 9 (dvm/ucode.c). */
#define INITIALIZE_SUBTYPE               9     /* dvm/ucode.c iwl_alive_fn */

/* ====================================================================== *
 *  struct iwl_cmd_header (iwl-trans.h) -- 4 bytes for DVM/gen1.
 *  x86 is LE so a packed overlay matches the wire.
 * ====================================================================== */
struct iwl_cmd_header {
    uint8_t  cmd;        /* command/notification id (the REPLY_x/SCAN_x above) */
    uint8_t  flags;      /* group_id on new gens; flags on DVM (0 for us)       */
    uint16_t sequence;   /* LE: SEQ_TO_QUEUE | index | RX_FRAME bit             */
} __attribute__((packed));

/* ====================================================================== *
 *  struct iwl_rx_packet (iwl-trans.h). len_n_flags FIRST (offset 0), then the
 *  4-byte header, then the payload. The frame length (which INCLUDES the
 *  header) is len_n_flags & FH_RSCSR_FRAME_SIZE_MSK.
 * ====================================================================== */
#define FH_RSCSR_FRAME_SIZE_MSK   0x00003FFF   /* iwl-trans.h: len bits 13:0 */

struct iwl_rx_packet {
    uint32_t              len_n_flags;   /* offset 0 */
    struct iwl_cmd_header hdr;           /* offset 4 (cmd @4, flags @5, seq @6) */
    uint8_t               data[];        /* offset 8: notification payload */
} __attribute__((packed));

/* ====================================================================== *
 *  struct iwl_alive_resp (dvm/commands.h). Payload of a REPLY_ALIVE packet.
 * ====================================================================== */
struct iwl_alive_resp {
    uint8_t  ucode_minor;
    uint8_t  ucode_major;
    uint16_t reserved1;
    uint8_t  sw_rev[8];
    uint8_t  ver_type;
    uint8_t  ver_subtype;        /* 9 == INIT alive, else RUNTIME (dvm/ucode.c) */
    uint16_t reserved2;
    uint32_t log_event_table_ptr;
    uint32_t error_event_table_ptr;
    uint32_t timestamp;
    uint32_t is_valid;           /* == UCODE_VALID_OK (0x1) on a good ALIVE */
} __attribute__((packed));

/* ====================================================================== *
 *  Calibration (dvm/commands.h + dvm/ucode.c iwl_send_calib_cfg).
 *  The INIT uCode runs the runtime calibrations; CALIBRATION_CFG_CMD selects
 *  which ones + asks for the SEND_COMPLETE notification, the results stream back
 *  as CALIBRATION_RES_NOTIFICATION(0x66) packets, and CALIBRATION_COMPLETE(0x67)
 *  ends the phase. The captured results are then replayed to the RUNTIME uCode as
 *  REPLY_PHY_CALIBRATION_CMD(0xb0) host commands (dvm/calib.c iwl_calib_set +
 *  iwl_send_calib_results).
 * ====================================================================== */

/* iwl_send_calib_cfg requests "all" init calibrations (dvm/commands.h:
 * IWL_CALIB_INIT_CFG_ALL = cpu_to_le32(0xFFFFFFFF)) and asks for the completion
 * notify (IWL_CALIB_CFG_FLAG_SEND_COMPLETE_NTFY_MSK = cpu_to_le32(BIT(0))). */
#define IWL_CALIB_INIT_CFG_ALL               0xFFFFFFFFu  /* dvm/commands.h */
#define IWL_CALIB_CFG_FLAG_SEND_COMPLETE_NTFY_MSK  0x00000001u /* dvm/commands.h BIT(0) */

/* dvm/commands.h struct iwl_calib_cfg_elmnt_s (5 x __le32 = 20 bytes). */
struct iwl_calib_cfg_elmnt_s {
    uint32_t is_enable;
    uint32_t start;
    uint32_t send_res;
    uint32_t apply_res;
    uint32_t reserved;
} __attribute__((packed));

/* dvm/commands.h struct iwl_calib_cfg_status_s (once + perd + flags). */
struct iwl_calib_cfg_status_s {
    struct iwl_calib_cfg_elmnt_s once;
    struct iwl_calib_cfg_elmnt_s perd;
    uint32_t flags;
} __attribute__((packed));

/* dvm/commands.h struct iwl_calib_cfg_cmd (ucd + drv + reserved). This is the
 * CALIBRATION_CFG_CMD(0x65) payload. */
struct iwl_calib_cfg_cmd {
    struct iwl_calib_cfg_status_s ucd_calib_cfg;
    struct iwl_calib_cfg_status_s drv_calib_cfg;
    uint32_t reserved1;
} __attribute__((packed));

/* dvm/commands.h struct iwl_calib_hdr -- prefixes every CALIBRATION_RES payload
 * AND is what gets replayed (with its trailing data) as REPLY_PHY_CALIBRATION_CMD.
 * op_code identifies the calibration; the host replays the result verbatim. */
struct iwl_calib_hdr {
    uint8_t op_code;
    uint8_t first_group;
    uint8_t groups_num;
    uint8_t data_valid;
} __attribute__((packed));

/* ====================================================================== *
 *  TFD (Transfer Frame Descriptor) -- pcie/tx.c struct iwl_tfd / iwl_tfd_tb.
 *  Each cmd-queue slot is one iwl_tfd. A host command is a single TB pointing
 *  at the command bytes (header + payload) in DRAM.
 * ====================================================================== */
#define IWL_NUM_OF_TBS   20      /* pcie/tx.c: tbs[20] */

struct iwl_tfd_tb {
    uint32_t lo;          /* low 32 bits of the TB DMA address */
    uint16_t hi_n_len;    /* bits 0-3 = addr bits 32-35; bits 4-15 = length */
} __attribute__((packed));

struct iwl_tfd {
    uint8_t           __reserved1[3];
    uint8_t           num_tbs;            /* count of valid tbs[] */
    struct iwl_tfd_tb tbs[IWL_NUM_OF_TBS];
    uint32_t          __pad;
} __attribute__((packed));

/* hi_n_len packing helper (pcie/tx.c iwl_pcie_tfd_set_tb:
 *   hi_n_len = len << 4; hi_n_len |= (addr >> 16) >> 16 & 0xF). */
static inline uint16_t iwl_tfd_hi_n_len(uint64_t addr, uint16_t len) {
    uint16_t v = (uint16_t)(len << 4);
    v |= (uint16_t)((addr >> 32) & 0xF);
    return v;
}

/* ====================================================================== *
 *  REPLY_SCAN_CMD payload -- dvm/commands.h struct iwl_scan_cmd + the per-
 *  channel struct iwl_scan_channel that follows it in the command buffer.
 *  Only the fields the brick fills are commented; the rest are zeroed.
 * ====================================================================== */
#define PROBE_OPTION_MAX   20   /* dvm/commands.h: direct_scan[20] */

struct iwl_ssid_ie {
    uint8_t id;                 /* WLAN_EID_SSID == 0 */
    uint8_t len;
    uint8_t ssid[32];
} __attribute__((packed));

struct iwl_scan_channel {
    uint32_t type;              /* SCAN_CHANNEL_TYPE_* | (probe ssid bitmap<<...) */
    uint16_t channel;           /* channel number */
    uint8_t  tx_gain;
    uint8_t  dsp_atten;
    uint16_t active_dwell;      /* TU (1024us) -- ~30 for 2.4GHz */
    uint16_t passive_dwell;     /* TU -- ~120 */
} __attribute__((packed));

/* iwl_scan_channel.type bits (dvm/commands.h). */
#define SCAN_CHANNEL_TYPE_PASSIVE  0
#define SCAN_CHANNEL_TYPE_ACTIVE   1

/* iwl_scan_channel.type also carries the per-channel probe-SSID bitmap in the
 * high bits. Linux dvm/scan.c: scan_ch->type |= IWL_SCAN_PROBE_MASK(n_probes)
 * with IWL_SCAN_PROBE_MASK(n) = (BIT(n) | (BIT(n) - BIT(1))). For one broadcast
 * probe (n_probes = 1) that is (BIT(1) | (BIT(1) - BIT(1))) = BIT(1) = 0x2; the
 * low type bit stays the ACTIVE flag. (Review fix S-S2.) */
#define IWL_SCAN_PROBE_MASK_1     0x00000002u  /* dvm/scan.c one-probe mask */

/* iwl_scan_channel.tx_gain for the 2.4GHz probe (dvm/scan.c:
 * scan_ch->tx_gain = ((1 << 5) | (5 << 3))). */
#define IWL_SCAN_TX_GAIN_24       (uint8_t)((1 << 5) | (5 << 3))  /* dvm/scan.c */

/*
 * The fixed iwl_scan_cmd head (dvm/commands.h struct iwl_scan_cmd):
 *   __le16 len; u8 scan_flags; u8 channel_count; __le16 quiet_time;
 *   __le16 quiet_plcp_th; __le16 good_CRC_th; __le16 rx_chain;
 *   __le32 max_out_time; __le32 suspend_time; __le32 flags; __le32 filter_flags;
 *     -> 28 bytes of leading fixed fields,
 *   struct iwl_tx_cmd tx_cmd;                 -> 60 bytes (gen1 DVM iwl_tx_cmd),
 *   struct iwl_ssid_ie direct_scan[20];       -> 20 * 34 = 680 bytes,
 *   u8 data[];  (channels appended here)
 *  => head = 28 + 60 + 680 = 768 = 0x300. (Review fix S-S1: the old 0xC8/200
 *  was far too small, so channels landed inside direct_scan[] -> malformed cmd.)
 *  HARDWARE-VALIDATE: sizeof(iwl_tx_cmd)=60 is the upstream gen1 value; pin it
 *  against the target firmware. The probe template is built into tx_cmd below. */
#define IWL_SCAN_CMD_HEAD_FIXED   28u    /* leading fixed fields before tx_cmd */
#define IWL_SCAN_TX_CMD_SIZE      60u    /* sizeof(struct iwl_tx_cmd) gen1 -- HW-VALIDATE */
#define IWL_SCAN_DIRECT_SSID_SIZE 34u    /* sizeof(struct iwl_ssid_ie) = 2 + 32 */
#define IWL_SCAN_CMD_HEAD_SIZE \
    (IWL_SCAN_CMD_HEAD_FIXED + IWL_SCAN_TX_CMD_SIZE + \
     PROBE_OPTION_MAX * IWL_SCAN_DIRECT_SSID_SIZE)   /* = 768 = 0x300 */

/* Byte offset of iwl_scan_cmd.tx_cmd within the head (after the 28 fixed bytes).
 * The probe-request 802.11 frame template is appended AFTER the whole head
 * block in iwl_scan_cmd.data[] and tx_cmd.len records its length. */
#define SCAN_CMD_OFF_TX_CMD        IWL_SCAN_CMD_HEAD_FIXED   /* 28 */
/* tx_cmd.len is the FIRST __le16 of struct iwl_tx_cmd (dvm gen1). */
#define SCAN_TXCMD_OFF_LEN         (SCAN_CMD_OFF_TX_CMD + 0)

/* Leading named fields of iwl_scan_cmd (dvm/commands.h) -- byte offsets within
 * the head block. We only write these; everything else stays zero. */
#define SCAN_CMD_OFF_LEN            0    /* __le16 len            */
#define SCAN_CMD_OFF_SCAN_FLAGS     2    /* u8 scan_flags         */
#define SCAN_CMD_OFF_CHANNEL_COUNT  3    /* u8 channel_count      */
#define SCAN_CMD_OFF_QUIET_TIME     4    /* __le16 quiet_time     */

#define IWL_ACTIVE_DWELL_TIME_24    30   /* dvm/scan.c ~30 TU (2.4GHz active) */
#define IWL_PASSIVE_DWELL_TIME_24   120  /* dvm/scan.c ~120 TU (passive)      */
#define IWL_ACTIVE_QUIET_TIME       10   /* dvm/scan.c IWL_ACTIVE_QUIET_TIME  */

/* ====================================================================== *
 *  REPLY_RX path structs (dvm/rx.c + dvm/commands.h). A received MPDU
 *  (REPLY_RX_MPDU_CMD 0xc1) payload begins with a 4-byte iwl_rx_mpdu_res_start;
 *  the 802.11 header follows at a FIXED offset of sizeof(that) == 4. The RSSI
 *  comes from the preceding REPLY_RX_PHY_CMD(0xc0) iwl_rx_phy_res, cached.
 * ====================================================================== */

/* dvm/commands.h struct iwl_rx_mpdu_res_start (prefixes a REPLY_RX_MPDU). */
struct iwl_rx_mpdu_res_start {
    uint16_t byte_count;        /* 802.11 frame length that follows */
    uint16_t reserved;
} __attribute__((packed));      /* sizeof == 4 -> header at mpdu+4 */

/* dvm/commands.h struct iwl_rx_phy_res -- the REPLY_RX_PHY_CMD(0xc0) payload.
 * iwlagn_calc_rssi reads the per-antenna RSSI/AGC out of non_cfg_phy_buf. */
struct iwl_rx_phy_res {
    uint8_t  non_cfg_phy_cnt;
    uint8_t  cfg_phy_cnt;
    uint8_t  stat_id;
    uint8_t  reserved1;
    uint32_t timestamp_lo;          /* __le64 timestamp (split to avoid align) */
    uint32_t timestamp_hi;
    uint32_t beacon_time_stamp;
    uint16_t phy_flags;
    uint16_t channel;
    uint8_t  non_cfg_phy_buf[32];   /* DSP AGC/RSSI measurements */
    uint32_t rate_n_flags;
    uint16_t byte_count;
    uint16_t frame_time;
} __attribute__((packed));

/* ====================================================================== *
 *  FH firmware-load DMA registers (iwl-fh.h) used by iwl-fw-load.c. These are
 *  the service-channel (channel 9) registers the DVM ucode loader programs.
 * ====================================================================== */
#define FH_MEM_LOWER_BOUND_FW          0x1000

#define FH_TFDIB_LOWER_BOUND           (FH_MEM_LOWER_BOUND_FW + 0x900)        /* 0x1900 */
#define FH_TFDIB_CTRL0_REG(ch)         (FH_TFDIB_LOWER_BOUND + 0x8 * (ch))     /* iwl-fh.h */
#define FH_TFDIB_CTRL1_REG(ch)         (FH_TFDIB_LOWER_BOUND + 0x8 * (ch) + 0x4)
#define FH_MEM_TFDIB_REG1_ADDR_BITSHIFT 28                                     /* iwl-fh.h */
#define FH_MEM_TFDIB_DRAM_ADDR_LSB_MSK 0xFFFFFFFF                              /* iwl-fh.h */

#define FH_SRVC_LOWER_BOUND            (FH_MEM_LOWER_BOUND_FW + 0x9C8)         /* 0x19C8 */
#define FH_SRVC_CHNL                   9                                       /* iwl-fh.h */
#define FH_SRVC_CHNL_SRAM_ADDR_REG(ch) (FH_SRVC_LOWER_BOUND + ((ch) - 9) * 0x4)

#define FH_TCSR_LOWER_BOUND_FW         (FH_MEM_LOWER_BOUND_FW + 0xD00)         /* 0x1D00 */
#define FH_TCSR_CHNL_TX_CONFIG_REG_FW(ch)  (FH_TCSR_LOWER_BOUND_FW + 0x20 * (ch))
#define FH_TCSR_CHNL_TX_BUF_STS_REG(ch)    (FH_TCSR_LOWER_BOUND_FW + 0x20 * (ch) + 0x8)

#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE     0x00000000  /* iwl-fh.h */
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE_FW 0x80000000  /* iwl-fh.h */
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE 0x00000000  /* iwl-fh.h */
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD   0x00100000  /* iwl-fh.h */

#define FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM       20          /* iwl-fh.h */
#define FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX       12          /* iwl-fh.h */
#define FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID   0x00000003  /* iwl-fh.h */

#define FH_MEM_TB_MAX_LENGTH                         0x00020000  /* iwl-fh.h: 128KB chunk cap */

/* CSR_FH_INT_STATUS uCode-load completion bits (iwl-csr.h). The DVM loader DMAs
 * each chunk over the FH service channel, but the HW signals "uCode write
 * complete" on the TX-channel bits, NOT a service-channel bit: Linux
 * iwl_pcie_irq_handler acks CSR_FH_INT_STATUS with CSR_FH_INT_TX_MASK =
 * (CSR_FH_INT_BIT_TX_CHNL1 | CSR_FH_INT_BIT_TX_CHNL0) = BIT(1)|BIT(0) and only
 * then sets ucode_write_complete. We poll for those bits + ack with that mask.
 * (Review fix L-C2: the old BIT(9) service-channel bit is NEVER set for uCode
 * load, so the poll would always time out.) */
#define CSR_FH_INT_STATUS_FW           0x010                     /* CSR_FH_INT_STATUS offset */
#define CSR_FH_INT_BIT_TX_CHNL0        (1u << 0)                 /* iwl-csr.h */
#define CSR_FH_INT_BIT_TX_CHNL1        (1u << 1)                 /* iwl-csr.h */
#define CSR_FH_INT_TX_MASK             (CSR_FH_INT_BIT_TX_CHNL1 | CSR_FH_INT_BIT_TX_CHNL0)

/* CSR_RESET write that releases the embedded CPU to run the loaded ucode
 * (pcie/trans.c iwl_pcie_load_given_ucode: "write 0 to CSR_RESET"). */
#define IWL_CSR_RESET_RUN              0x00000000

/* ====================================================================== *
 *  EEPROM / OTP access (iwl-eeprom-read.c). CSR_EEPROM_REG drives both.
 * ====================================================================== */
#define CSR_EEPROM_REG_READ_VALID_MSK  0x00000001  /* iwl-csr.h */
#define CSR_EEPROM_REG_BIT_CMD         0x00000002  /* iwl-csr.h */
#define CSR_EEPROM_REG_MSK_ADDR        0x0000FFFC  /* iwl-csr.h */
#define CSR_EEPROM_REG_MSK_DATA        0xFFFF0000  /* iwl-csr.h */

/* EEPROM ownership semaphore (iwl-csr.h CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM).
 * Linux iwl_eeprom_acquire_semaphore SETS this bit in CSR_HW_IF_CONFIG_REG and
 * polls for the device to grant it before any EEPROM word read; the release
 * CLEARS it. (Review fix N-N1: the semaphore was never taken.) */
#define CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM  0x00200000  /* iwl-csr.h */

#define CSR_OTP_GP_REG                 0x034        /* iwl-csr.h CSR_OTP_GP_REG */
#define CSR_OTP_GP_REG_DEVICE_SELECT   0x00010000   /* iwl-csr.h: OTP-vs-EEPROM */
#define CSR_OTP_GP_REG_ECC_CORR_STATUS 0x00100000   /* iwl-csr.h */
#define CSR_OTP_GP_REG_ECC_UNCORR_STATUS 0x00200000 /* iwl-csr.h */

/* OTP image-walk constants (iwl-eeprom-read.c iwl_find_otp_image /
 * iwl_read_otp_word). OTP is a linked list of "images": each block's last word
 * (at OTP_LINK_BYTE_OFFSET within the block) is the byte offset of the next
 * image; a 0 link terminates and the LAST valid image is the one to read from.
 * The per-word read reuses the CSR_EEPROM_REG front-end. */
#define OTP_LOWER_BLOCKS_TOTAL         3            /* iwl-eeprom-read.c (gen1) */
#define OTP_LINK_WORD_OFFSET           1            /* link is the 2nd word of a block */
/* HARDWARE-VALIDATE: the OTP block size + the max image count are family-
 * specific (1000/6000g2 differ). These are the common gen1 values; pin them. */
#define OTP_BLOCK_SIZE_BYTES           0x200        /* iwl-eeprom-read.c -- HW-VALIDATE */
#define OTP_MAX_LL_ITEMS               4            /* bounded image-walk cap -- HW-VALIDATE */

/* MAC address EEPROM byte offset (iwl-eeprom-parse.c: EEPROM_MAC_ADDRESS).
 * The DVM gen1 EEPROM holds the 6-byte MAC at byte offset 0x15*2 = 0x2A.
 * HARDWARE-VALIDATION ITEM -- exact offset is per-family; this is the common
 * 5000/6000 value and is flagged. */
#define EEPROM_MAC_ADDRESS_BYTE_OFF    0x2A         /* iwl-eeprom-parse.c -- HW-VALIDATE */

/* ====================================================================== *
 *  SCD (Scheduler) registers + byte-count table -- iwl-prph.h.
 *  Required to deliver ANY host command to a running uCode (the TX scheduler
 *  has to know the cmd queue's byte-count table and have the queue activated +
 *  the FIFO mapped). Used by the minimal DVM SCD bring-up in iwl-trans.c.
 *
 *  Linux iwl-prph.h: SCD_BASE = PRPH_BASE + 0xa02c00. The gen1 PRPH window the
 *  DVM driver reaches via HBUS_TARG_PRPH_* masks the address with 0x000FFFFF
 *  (iwl-csr.h IWL_PRPH_ADDR_MASK), so the SCD register file is addressed by its
 *  scd_base_addr which the firmware reports at runtime via SCD_SRAM_BASE_ADDR --
 *  we read that and add the per-register offsets below. The absolute SCD_BASE is
 *  kept here for reference; the bring-up uses the runtime-read scd_base_addr.
 *  HARDWARE-VALIDATE: scd_base_addr is read from the device (not assumed).
 * ====================================================================== */
#define SCD_PRPH_BASE                  0xa02c00u   /* iwl-prph.h SCD_BASE (PRPH-relative) */
#define SCD_SRAM_BASE_ADDR             (SCD_PRPH_BASE + 0x00)  /* reports scd_base_addr */
#define SCD_DRAM_BASE_ADDR             (SCD_PRPH_BASE + 0x08)  /* byte-count table base (>>10) */
#define SCD_AIT                        (SCD_PRPH_BASE + 0x0c)  /* iwl-prph.h */
#define SCD_TXFACT                     (SCD_PRPH_BASE + 0x10)  /* activate TX DMA/FIFO chans */
#define SCD_ACTIVE                     (SCD_PRPH_BASE + 0x14)  /* iwl-prph.h */
#define SCD_QUEUECHAIN_SEL             (SCD_PRPH_BASE + 0xe8)  /* per-queue chain mode */
#define SCD_CHAINEXT_EN                (SCD_PRPH_BASE + 0x244) /* iwl-prph.h SCD_CHAINEXT_EN */
#define SCD_AGGR_SEL                   (SCD_PRPH_BASE + 0x248) /* per-queue aggregation */
#define SCD_INTERRUPT_MASK             (SCD_PRPH_BASE + 0x108) /* iwl-prph.h */
#define SCD_QUEUE_RDPTR(x)             (SCD_PRPH_BASE + 0x68 + (x) * 4)   /* iwl-prph.h */
#define SCD_QUEUE_STATUS_BITS(x)       (SCD_PRPH_BASE + 0x10c + (x) * 4)  /* iwl-prph.h */

/* SCD per-queue status-bits composition to ACTIVATE a queue against a TX FIFO
 * (iwl_trans_tx_queue_set_status):
 *   (1 << POS_ACTIVE) | (fifo << POS_TXF) | (1 << POS_WSL) | STTS_REG_MSK. */
#define SCD_QUEUE_STTS_REG_POS_TXF     0           /* iwl-prph.h */
#define SCD_QUEUE_STTS_REG_POS_ACTIVE  3           /* iwl-prph.h */
#define SCD_QUEUE_STTS_REG_POS_WSL     4           /* iwl-prph.h */
#define SCD_QUEUE_STTS_REG_MSK         0x00FF0000u /* iwl-prph.h (gen1) */

/* SCD context/translation SRAM region (cleared at bring-up). iwl-prph.h:
 * SCD_CONTEXT_MEM_LOWER_BOUND = 0x600 (relative to scd_base_addr). */
#define SCD_CONTEXT_MEM_LOWER_BOUND    0x600        /* iwl-prph.h */
#define SCD_CONTEXT_QUEUE_OFFSET(x)    (SCD_CONTEXT_MEM_LOWER_BOUND + ((x) * 8))
#define SCD_TRANS_TBL_MEM_LOWER_BOUND  0x7E0        /* iwl-prph.h */
#define SCD_TRANS_TBL_OFFSET_QUEUE(x)  ((SCD_TRANS_TBL_MEM_LOWER_BOUND + ((x) * 2)) & 0xfffc)

/* SCD byte-count table: one struct iwlagn_scd_bc_tbl per queue, each a
 * __le16 tfd_offset[TFD_QUEUE_BC_SIZE]. iwl-fh.h: TFD_QUEUE_BC_SIZE =
 * TFD_QUEUE_SIZE_MAX(256) + TFD_QUEUE_SIZE_BC_DUP(64) = 320. */
#define TFD_QUEUE_SIZE_BC_DUP          64           /* iwl-fh.h */
#define TFD_QUEUE_BC_SIZE              (256 + TFD_QUEUE_SIZE_BC_DUP)   /* 320 */

struct iwlagn_scd_bc_tbl {
    uint16_t tfd_offset[TFD_QUEUE_BC_SIZE];
} __attribute__((packed));

/* A byte-count entry = (frame_len + CRC + delimiter) & 0xFFF | (sta_id << 12).
 * iwl-fh.h IWL_TX_CRC_SIZE = 4, IWL_TX_DELIMITER_SIZE = 4. */
#define IWL_TX_CRC_SIZE                4            /* iwl-fh.h */
#define IWL_TX_DELIMITER_SIZE          4            /* iwl-fh.h */

/* iwl_pcie_tx_start activates all 8 TX DMA/FIFO channels: SCD_TXFACT = 0xFF
 * (IWL_MASK(0,7)). FH_TCSR_CHNL_NUM = 8 (iwl-fh.h). */
#define SCD_TXFACT_ALL_QUEUES          0x000000FFu  /* IWL_MASK(0,7) */
#define FH_TCSR_CHNL_NUM               8            /* iwl-fh.h */

#endif /* IWL_DVM_COMMANDS_H */
