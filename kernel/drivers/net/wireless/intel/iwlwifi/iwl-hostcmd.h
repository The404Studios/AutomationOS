/*
 * iwl-hostcmd.h -- the DVM host-command + RX-notification spine.
 * =============================================================
 * iwl_send_cmd() places a command in the cmd-queue TFD, rings the doorbell
 * (HBUS_TARG_WRPTR), and waits (BOUNDED) for the response/notification on the
 * RX ring. iwl_rx_wait_notif() is the bounded RX-ring drain used by the
 * firmware-load (ALIVE/CALIB) and scan (SCAN_COMPLETE/beacons) bricks.
 *
 *   ===================  HELD FOR HARDWARE  ===================
 *   Never runs at boot. Correct-by-review vs Linux iwlwifi DVM
 *   (pcie/tx.c host-cmd enqueue, pcie/rx.c RX consume, iwl-trans.h wire format).
 *   Bounded loops, markers, abort-clean.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-hostcmd.h
 */
#ifndef IWL_HOSTCMD_H
#define IWL_HOSTCMD_H

#include "types.h"
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"

/* Max payload we hand the firmware in one command (one cmd-queue page is the
 * DRAM staging area; commands are small). */
#define IWL_CMD_MAX_PAYLOAD   512

/* Max bytes of a received notification payload we copy back to the caller. */
#define IWL_RESP_MAX          512

/* A received notification, decoded from one RBD's RX page. */
typedef struct iwl_rx_notif {
    uint8_t  cmd;                       /* iwl_cmd_header.cmd (REPLY_x/SCAN_x)  */
    uint16_t len;                       /* payload bytes copied into data[]     */
    uint8_t  data[IWL_RESP_MAX];        /* notification payload (after the hdr) */
} iwl_rx_notif_t;

/*
 * iwl_send_cmd -- enqueue a DVM host command and (unless async) wait bounded for
 * a notification whose cmd id == want_cmd. Returns 0 on success (resp filled if
 * non-NULL + want_cmd seen), -1 on enqueue failure or timeout (abort-clean).
 *
 *   cmd_id   : REPLY_* command id placed in the cmd header.
 *   data/len : command payload (<= IWL_CMD_MAX_PAYLOAD), copied after the header.
 *   want_cmd : the notification cmd id to wait for (0 = fire-and-forget/async).
 *   resp     : optional out; receives the first matching notification.
 */
int iwl_send_cmd(struct iwl_trans* trans, uint8_t cmd_id,
                 const void* data, uint16_t len,
                 uint8_t want_cmd, iwl_rx_notif_t* resp);

/*
 * iwl_rx_wait_notif -- bounded drain of the RX ring waiting for a notification
 * with cmd id == want_cmd. Used by fw-load (ALIVE/CALIB) and scan (SCAN_COMPLETE)
 * to wait on firmware-initiated notifications that are NOT command responses.
 * Returns 0 + fills resp on match, -1 on timeout.
 */
int iwl_rx_wait_notif(struct iwl_trans* trans, uint8_t want_cmd,
                      iwl_rx_notif_t* resp);

/*
 * iwl_rx_poll_one -- consume AT MOST one freshly-DMA'd RX packet (non-blocking
 * single step). Returns 1 if a notification was decoded into `out`, 0 if the
 * ring is currently empty, -1 on a ring inconsistency. The scan parser calls
 * this in a bounded loop to harvest beacons.
 */
int iwl_rx_poll_one(struct iwl_trans* trans, iwl_rx_notif_t* out);

/*
 * iwl_scd_cmd_queue_init -- the minimal DVM TX-scheduler (SCD) bring-up for the
 * COMMAND queue. A host command cannot reach a RUNNING uCode until the scheduler
 * knows the cmd queue's byte-count table and the queue is activated + mapped to
 * its TX FIFO. Mirrors Linux pcie/tx.c iwl_pcie_tx_start (+ the dvm cmd queue
 * enable): alloc the scd byte-count table, write SCD_DRAM_BASE_ADDR, clear the
 * SCD context SRAM, disable chain/aggr, reset the queue ptrs, activate queue
 * IWL_CMD_QUEUE_ID against IWL_TX_FIFO_CMD, and enable the TX channels.
 * Idempotent (safe to call once before the first host command). Returns 0 on
 * success, -1 on any bounded failure (abort-clean). MUST be called with the
 * uCode running (after ALIVE) -- the firmware reports scd_base_addr only then.
 */
int iwl_scd_cmd_queue_init(struct iwl_trans* trans);

/* Bounded wait budget for a host-command response / firmware notification.
 * Translated from Linux's millisecond timeouts into a HARD iteration cap
 * (the PIT may be frozen when the trigger runs -- never wait on ticks). */
#define IWL_CMD_WAIT_MAX   200000   /* per command/notification wait */

#endif /* IWL_HOSTCMD_H */
