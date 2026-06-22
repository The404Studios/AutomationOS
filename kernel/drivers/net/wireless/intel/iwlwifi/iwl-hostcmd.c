/*
 * iwl-hostcmd.c -- the DVM host-command + RX-notification spine.
 * =============================================================
 * This is the load-bearing primitive every higher brick (fw-load, nvm, scan,
 * ops) uses to talk to the firmware once it is alive: build a command into a
 * DRAM staging buffer, point a cmd-queue TFD at it, ring the write-pointer
 * doorbell, then drain the RX ring (bounded) for the matching notification.
 *
 *   ===================  HELD FOR HARDWARE -- READ THIS  ===================
 *   Nothing here runs at boot. Reached only via iwl_wifi_bringup() (iwl-ops.c)
 *   on the physical T410. No QEMU emulates the card, so this is correct-by-
 *   review against Linux iwlwifi DVM:
 *     pcie/tx.c   -- iwl_pcie_enqueue_hcmd / iwl_pcie_txq_build_tfd /
 *                    iwl_pcie_tfd_set_tb / iwl_pcie_txq_inc_wr_ptr (doorbell)
 *     pcie/rx.c   -- iwl_pcie_rx_handle (closed_rb_num consume loop)
 *     iwl-trans.h -- struct iwl_rx_packet wire format
 *
 *   Safety laws (mirroring iwl-trans.c):
 *     - every poll/drain is ITERATION-CAPPED (never tick-based, never bare while)
 *     - a kprintf MARKER precedes every risky MMIO/doorbell step
 *     - any timeout returns -1 (abort-clean); never panic.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-hostcmd.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf */
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"
#include "iwl-hostcmd.h"

/* The cmd-queue is the queue DVM firmware services host commands on:
 * IWL_CMD_QUEUE_ID == 4 (IWL_DEFAULT_CMD_QUEUE_NUM), defined in the SHARED
 * iwl-dvm-commands.h so the transport (FH_MEM_CBBC_QUEUE base slot + the reset
 * doorbell) and this brick (doorbell + sequence) agree. (Review fix H-C1: the
 * old queue 0 was wrong -- DVM never services commands there.) */

/* A small DRAM staging buffer for the outgoing command bytes. Identity-mapped
 * (phys == virt) like every other DMA page here. Allocated lazily on first use
 * so nothing touches memory at boot. */
static void*    g_cmd_stage      = (void*)0;
static uint64_t g_cmd_stage_dma  = 0;

#include "mem.h"             /* pmm_alloc_page */

static int iwl_cmd_stage_ensure(void) {
    if (g_cmd_stage) return 0;
    void* p = pmm_alloc_page();
    if (!p) {
        kprintf("IWLCMD: cmd stage alloc FAILED -- abort\n");
        return -1;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) ((volatile uint8_t*)p)[i] = 0;
    g_cmd_stage     = p;
    g_cmd_stage_dma = (uint64_t)(uintptr_t)p;   /* phys == virt */
    return 0;
}

/* Little-endian stores into the staging buffer (x86 is LE; explicit for clarity
 * and to keep the wire format obvious to a reviewer). */
static inline void put_le16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

/* ====================================================================== *
 *  RX-ring consume. The device DMAs received packets into the per-RBD pages and
 *  publishes the count of closed RBDs in the shared RB-status block's first
 *  __le16 (closed_rb_num, iwl-fh.h struct iwl_rb_status). We read that, and for
 *  each newly-closed RBD decode the iwl_rx_packet at the head of its page.
 *  (Linux pcie/rx.c iwl_pcie_rx_handle: r = closed_rb_num & 0x0FFF; loop i..r.)
 * ====================================================================== */
static uint16_t iwl_rx_closed_count(struct iwl_trans* trans) {
    /* closed_rb_num is the first field of the RB-status block, low 12 bits. */
    volatile uint16_t* rbs = (volatile uint16_t*)trans->rb_status;
    return (uint16_t)(rbs[0] & 0x0FFF);
}

/* Decode the iwl_rx_packet living at the head of one RBD's RX page into `out`.
 * Returns 0 on a sane packet, -1 if the declared length is implausible. */
static int iwl_rx_decode_page(void* page, iwl_rx_notif_t* out) {
    const struct iwl_rx_packet* pkt = (const struct iwl_rx_packet*)page;

    /* len_n_flags low 14 bits = frame length INCLUDING the 4-byte header
     * (iwl-trans.h iwl_rx_packet_len). Clamp hard -- this is hostile-ish HW data. */
    uint32_t frame_len = pkt->len_n_flags & FH_RSCSR_FRAME_SIZE_MSK;
    if (frame_len < sizeof(struct iwl_cmd_header))
        return -1;

    uint32_t payload = frame_len - (uint32_t)sizeof(struct iwl_cmd_header);
    if (payload > IWL_RESP_MAX) payload = IWL_RESP_MAX;   /* clamp, never overrun */

    out->cmd = pkt->hdr.cmd;
    out->len = (uint16_t)payload;
    for (uint32_t i = 0; i < payload; i++)
        out->data[i] = pkt->data[i];
    return 0;
}

/*
 * iwl_rx_poll_one -- single non-blocking RX step. If the device has closed a new
 * RBD past our read cursor, decode that one packet and advance. Returns 1 on a
 * decoded packet, 0 if empty, -1 on inconsistency.
 */
int iwl_rx_poll_one(struct iwl_trans* trans, iwl_rx_notif_t* out) {
    if (!trans || !out) return -1;

    uint16_t closed = iwl_rx_closed_count(trans);
    uint32_t r = (uint32_t)closed & (RX_QUEUE_SIZE - 1);   /* device write index */
    uint32_t i = trans->rx_read & (RX_QUEUE_SIZE - 1);     /* our read index     */

    if (i == r)
        return 0;   /* ring empty: nothing new since last read */

    void* page = trans->rx_bufs[i];
    if (!page) return -1;

    int rc = iwl_rx_decode_page(page, out);

    /* RE-POST the just-consumed RBD before advancing, then publish the PRODUCER
     * index. Linux pcie/rx.c iwl_pcie_rxsq_restock re-writes the RBD ring slot
     * with the buffer's DMA pointer (>> 8) so the device can DMA into that page
     * again, and only rings the write-pointer doorbell when the producer index
     * crosses an 8-descriptor boundary (if (write_actual != (write & ~0x7))).
     * (Review fix H-M1: the RBD was never re-posted -> RX stalled after one wrap;
     *  H-M2: the doorbell must publish the producer index, gated on the boundary,
     *  not the raw consumer cursor.) */
    ((volatile uint32_t*)trans->rx_bd)[i] =
        (uint32_t)(((uint64_t)(uintptr_t)trans->rx_bufs[i]) >> 8);
    iwl_desc_wmb();

    uint32_t prev_write = trans->rx_read;
    trans->rx_read = (trans->rx_read + 1) & (RX_QUEUE_SIZE - 1);

    /* Only ring the doorbell on an 8-boundary crossing, with the 8-aligned
     * producer index (Linux iwl_pcie_rxq_inc_wr_ptr writes round_down(write,8)). */
    if ((trans->rx_read & ~0x7u) != (prev_write & ~0x7u)) {
        iwl_desc_wmb();
        iwl_write32(trans, FH_RSCSR_CHNL0_WPTR, trans->rx_read & ~0x7u);
    }

    return (rc == 0) ? 1 : -1;
}

/*
 * iwl_rx_wait_notif -- bounded drain waiting for a specific notification id.
 * Used for firmware-initiated notifications (ALIVE, CALIB_COMPLETE, SCAN_*).
 */
int iwl_rx_wait_notif(struct iwl_trans* trans, uint8_t want_cmd,
                      iwl_rx_notif_t* resp) {
    if (!trans) return -1;
    iwl_rx_notif_t scratch;
    iwl_rx_notif_t* slot = resp ? resp : &scratch;

    kprintf("IWLCMD: wait notif cmd=0x%02x (bounded)...\n", want_cmd);
    for (int it = 0; it < IWL_CMD_WAIT_MAX; it++) {
        int got = iwl_rx_poll_one(trans, slot);
        if (got == 1) {
            if (slot->cmd == want_cmd) {
                kprintf("IWLCMD: got notif cmd=0x%02x len=%u\n", slot->cmd, slot->len);
                return 0;
            }
            /* A different notification (e.g. a SCAN_RESULTS while we wait for
             * SCAN_COMPLETE). Keep draining within the same budget. */
            continue;
        }
        if (got < 0)
            return -1;            /* ring inconsistency -- abort clean */
        iwl_udelay_approx(64);    /* empty: settle, bounded total */
    }
    kprintf("IWLCMD: wait notif cmd=0x%02x TIMEOUT -- abort\n", want_cmd);
    return -1;
}

/* ====================================================================== *
 *  iwl_scd_cmd_queue_init -- minimal DVM TX-scheduler bring-up for the cmd queue.
 *  THE HARD PREREQUISITE (item H-C2): a host command cannot reach a RUNNING
 *  uCode until the scheduler has the cmd queue's byte-count table and the queue
 *  is activated + FIFO-mapped. Faithful to Linux pcie/tx.c iwl_pcie_tx_start +
 *  the dvm command-queue enable (iwl_trans_pcie_txq_enable cmd-queue path):
 *    1. read scd_base_addr from SCD_SRAM_BASE_ADDR (firmware reports it post-ALIVE),
 *    2. clear the SCD context/translation SRAM,
 *    3. write SCD_DRAM_BASE_ADDR = (scd_bc_tbls.dma >> 10),
 *    4. SCD_CHAINEXT_EN = 0; per-queue chain/aggr cleared,
 *    5. for the cmd queue: CBBC base already set by trans; reset SCD_QUEUE_RDPTR
 *       + HBUS_TARG_WRPTR, clear its SCD context word, set SCD_QUEUE_STATUS_BITS
 *       (active | fifo<<TXF | 1<<WSL | MSK),
 *    6. SCD_TXFACT = IWL_MASK(0,7) to activate the TX DMA/FIFO channels,
 *    7. seed the byte-count entry for the (empty) cmd slot.
 *  Every PRPH/MEM write is under NIC access, marker-printed, bounded; abort-clean.
 *
 *  HARDWARE-VALIDATE: the absolute SCD_BASE family-offset is NOT assumed -- we
 *  read scd_base_addr from the device (SCD_SRAM_BASE_ADDR). The per-register SCD
 *  offsets are the upstream gen1 values (iwl-prph.h) and are cited; pin the
 *  family base if a future part disagrees.
 * ====================================================================== */

/* Device-internal MEM write via HBUS_TARG_MEM_* (Linux iwl_trans_pcie_write_mem
 * inner store). `addr` is a device MEM address (scd_base + offset). Caller holds
 * NIC access. Writes `n` zero dwords starting at addr (the only use here). */
static void iwl_write_mem_zero(struct iwl_trans* trans, uint32_t addr, uint32_t n) {
    iwl_write32(trans, HBUS_TARG_MEM_WADDR, addr);
    for (uint32_t i = 0; i < n; i++)
        iwl_write32(trans, HBUS_TARG_MEM_WDAT, 0);   /* WADDR auto-increments */
}

static void iwl_write_mem32(struct iwl_trans* trans, uint32_t addr, uint32_t val) {
    iwl_write32(trans, HBUS_TARG_MEM_WADDR, addr);
    iwl_write32(trans, HBUS_TARG_MEM_WDAT, val);
}

static int iwl_scd_bc_tbl_ensure(struct iwl_trans* trans) {
    if (trans->scd_bc_tbl) return 0;
    /* One struct iwlagn_scd_bc_tbl is 320 * 2 = 640 bytes -- well under a page.
     * The cmd queue needs only its own table; one page is plenty + 16-aligned. */
    void* p = pmm_alloc_page();
    if (!p) { kprintf("IWLCMD: SCD bc-table alloc FAILED -- abort\n"); return -1; }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) ((volatile uint8_t*)p)[i] = 0;
    trans->scd_bc_tbl     = p;
    trans->scd_bc_tbl_dma = (uint64_t)(uintptr_t)p;   /* phys == virt */
    return 0;
}

int iwl_scd_cmd_queue_init(struct iwl_trans* trans) {
    if (!trans || !trans->mmio) return -1;
    if (trans->scd_ready) return 0;   /* one-shot */

    kprintf("IWLCMD: SCD cmd-queue bring-up START (q=%d fifo=%d)...\n",
            IWL_CMD_QUEUE_ID, IWL_TX_FIFO_CMD);

    if (iwl_scd_bc_tbl_ensure(trans) != 0) return -1;

    if (iwl_grab_nic_access(trans) != 0) {
        kprintf("IWLCMD: SCD bring-up could not grab NIC access -- abort\n");
        return -1;
    }

    /* (1) The firmware reports the SCD SRAM base only once it is running. */
    kprintf("IWLCMD: read scd_base_addr (PRPH SCD_SRAM_BASE_ADDR)...\n");
    trans->scd_base_addr = iwl_read_prph(trans, SCD_SRAM_BASE_ADDR);
    kprintf("IWLCMD: scd_base_addr=0x%08x\n", trans->scd_base_addr);

    /* (2) Reset the SCD context + translation-table SRAM for the cmd queue.
     * Linux clears from SCD_CONTEXT_MEM_LOWER_BOUND up to the translation table
     * for num_of_queues; we clear the region that covers the cmd queue (queues
     * 0..IWL_CMD_QUEUE_ID) -- bounded, sufficient for the single-queue model. */
    uint32_t nq = (uint32_t)IWL_CMD_QUEUE_ID + 1;
    uint32_t clear_dwords =
        (SCD_TRANS_TBL_OFFSET_QUEUE(nq) - SCD_CONTEXT_MEM_LOWER_BOUND) / 4u;
    kprintf("IWLCMD: clear SCD context SRAM (%u dwords)...\n", clear_dwords);
    iwl_write_mem_zero(trans, trans->scd_base_addr + SCD_CONTEXT_MEM_LOWER_BOUND,
                       clear_dwords);

    /* (3) Point the scheduler at the byte-count table (device wants dma >> 10). */
    kprintf("IWLCMD: program SCD byte-count table (PRPH SCD_DRAM_BASE_ADDR)...\n");
    iwl_desc_wmb();
    iwl_write_prph(trans, SCD_DRAM_BASE_ADDR,
                   (uint32_t)(trans->scd_bc_tbl_dma >> 10));

    /* (4) No chaining, no aggregation for the cmd queue (it is a plain AC/CMD
     * queue). Linux: SCD_CHAINEXT_EN = 0; per-queue SCD_QUEUECHAIN_SEL/AGGR_SEL
     * bits clear. */
    kprintf("IWLCMD: disable SCD chain/aggregation...\n");
    iwl_write_prph(trans, SCD_CHAINEXT_EN, 0);
    iwl_write_prph(trans, SCD_QUEUECHAIN_SEL, 0);
    iwl_write_prph(trans, SCD_AGGR_SEL, 0);

    /* (5) Bring up the command queue itself. The CBBC ring base was already
     * programmed by the transport (iwl_alloc_cmd_ring -> FH_MEM_CBBC_QUEUE[4]).
     * Reset its read/write pointers, clear its SCD context word, and activate it
     * against TX FIFO IWL_TX_FIFO_CMD. */
    kprintf("IWLCMD: reset cmd-queue SCD ptrs (RDPTR + HBUS_TARG_WRPTR)...\n");
    iwl_write_prph(trans, SCD_QUEUE_RDPTR(IWL_CMD_QUEUE_ID), 0);
    iwl_write32(trans, HBUS_TARG_WRPTR, (0u & 0xff) | (IWL_CMD_QUEUE_ID << 8));

    kprintf("IWLCMD: clear cmd-queue SCD context word...\n");
    iwl_write_mem32(trans,
        trans->scd_base_addr + SCD_CONTEXT_QUEUE_OFFSET(IWL_CMD_QUEUE_ID), 0);

    kprintf("IWLCMD: activate cmd queue (SCD_QUEUE_STATUS_BITS)...\n");
    iwl_write_prph(trans, SCD_QUEUE_STATUS_BITS(IWL_CMD_QUEUE_ID),
                   (1u << SCD_QUEUE_STTS_REG_POS_ACTIVE) |
                   ((uint32_t)IWL_TX_FIFO_CMD << SCD_QUEUE_STTS_REG_POS_TXF) |
                   (1u << SCD_QUEUE_STTS_REG_POS_WSL) |
                   SCD_QUEUE_STTS_REG_MSK);

    /* (6) Activate all TX DMA/FIFO channels (Linux iwl_pcie_txq_set_sched). */
    kprintf("IWLCMD: enable TX DMA/FIFO channels (SCD_TXFACT)...\n");
    iwl_write_prph(trans, SCD_TXFACT, SCD_TXFACT_ALL_QUEUES);

    /* (6b) Enable the FH TX DMA channels themselves. SCD_TXFACT activates the
     * SCHEDULER, but the per-channel FH TCSR DMA-enable + credit bits gate the
     * actual data path -- Linux iwl_pcie_tx_start loops channels 0..7 writing
     * FH_TCSR_CHNL_TX_CONFIG_REG with (DMA_CHNL_ENABLE | DMA_CREDIT_ENABLE) and
     * then sets the SCD auto-retry chicken bit. Without this the scheduler signals
     * a TFD is ready but no descriptor actually moves -> host commands (incl.
     * REPLY_SCAN_CMD) never reach the running uCode -> no scan results. (Audit #2.) */
    kprintf("IWLCMD: enable FH TX DMA channels 0..%d (FH_TCSR_CHNL_TX_CONFIG_REG)...\n",
            FH_TCSR_CHNL_NUM - 1);
    for (int ch = 0; ch < FH_TCSR_CHNL_NUM; ch++) {
        iwl_write32(trans, FH_TCSR_CHNL_TX_CONFIG_REG(ch),
                    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE |
                    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE);
    }
    kprintf("IWLCMD: set FH SCD auto-retry chicken bit...\n");
    iwl_set_bit(trans, FH_TX_CHICKEN_BITS_REG,
                FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN);

    iwl_release_nic_access(trans);

    /* (7) Seed the byte-count entry for the current (empty) cmd slot. The entry
     * is (len + CRC + delimiter) & 0xFFF | (sta_id << 12); for the empty queue we
     * seed slot 0 with the minimal delimiter+crc length (sta_id 0). The enqueue
     * path updates the live slot's entry per command below. */
    struct iwlagn_scd_bc_tbl* bc = (struct iwlagn_scd_bc_tbl*)trans->scd_bc_tbl;
    bc->tfd_offset[0] =
        (uint16_t)((IWL_TX_CRC_SIZE + IWL_TX_DELIMITER_SIZE) & 0xFFF);
    iwl_desc_wmb();

    trans->scd_ready = 1;
    kprintf("IWLCMD: SCD cmd-queue bring-up OK\n");
    return 0;
}

/* ====================================================================== *
 *  Command enqueue. Build [iwl_cmd_header | payload] into the staging page,
 *  point cmd-queue TFD[wr] at it (single TB), flush, and ring the doorbell.
 *  (Linux pcie/tx.c iwl_pcie_enqueue_hcmd + iwl_pcie_txq_build_tfd +
 *   iwl_pcie_tfd_set_tb + iwl_pcie_txq_inc_wr_ptr.)
 * ====================================================================== */
static int iwl_cmd_enqueue(struct iwl_trans* trans, uint8_t cmd_id,
                           const void* data, uint16_t len) {
    if (len > IWL_CMD_MAX_PAYLOAD) {
        kprintf("IWLCMD: payload %u too big -- abort\n", len);
        return -1;
    }
    if (iwl_cmd_stage_ensure() != 0)
        return -1;

    /* Build the command bytes: 4-byte header then payload. */
    uint8_t* buf = (uint8_t*)g_cmd_stage;
    buf[0] = cmd_id;          /* iwl_cmd_header.cmd      */
    buf[1] = 0;               /* flags / group_id (DVM = 0) */
    /* sequence (Linux iwl-trans.h SEQ packing): the QUEUE is bits 12:8 and the
     * INDEX is bits 7:0 -> seq = SEQ_TO_QUEUE(q) | SEQ_TO_INDEX(i) =
     * ((q & 0x1f) << 8) | (i & 0xff). (Review fix H-L6: queue/index were
     * swapped, so the device echoed a bogus sequence.) */
    put_le16(&buf[2], (uint16_t)(((IWL_CMD_QUEUE_ID & 0x1f) << 8) |
                                 (trans->cmd_wr_ptr & 0xff)));
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < len; i++) buf[4 + i] = src ? src[i] : 0;
    uint16_t total = (uint16_t)(4 + len);

    /* Point cmd-queue TFD[wr] at the staged bytes (single TB). */
    struct iwl_tfd* ring = (struct iwl_tfd*)trans->cmd_ring;
    uint32_t wr = trans->cmd_wr_ptr & (TFD_QUEUE_SIZE_MAX - 1);
    struct iwl_tfd* tfd = &ring[wr];

    /* Clear then set the single TB (Linux iwl_pcie_txq_build_tfd clears first). */
    for (uint32_t i = 0; i < sizeof(*tfd); i++) ((volatile uint8_t*)tfd)[i] = 0;
    tfd->num_tbs   = 1;
    tfd->tbs[0].lo = (uint32_t)(g_cmd_stage_dma & 0xFFFFFFFF);
    tfd->tbs[0].hi_n_len = iwl_tfd_hi_n_len(g_cmd_stage_dma, total);

    /* Update the scheduler byte-count table entry for THIS slot before the
     * doorbell. Linux iwl_pcie_txq_update_byte_cnt_tbl:
     *   bc_ent = (len + CRC + DELIMITER) & 0xFFF | (sta_id << 12).
     * sta_id is 0 for host commands. Without this the SCD will not dispatch the
     * TFD to a running uCode. (Part of item H-C2.) */
    if (trans->scd_bc_tbl) {
        struct iwlagn_scd_bc_tbl* bc =
            (struct iwlagn_scd_bc_tbl*)trans->scd_bc_tbl;
        uint16_t bc_len =
            (uint16_t)((total + IWL_TX_CRC_SIZE + IWL_TX_DELIMITER_SIZE) & 0xFFF);
        bc->tfd_offset[wr] = bc_len;
        /* DVM duplicates the first 64 entries (TFD_QUEUE_SIZE_BC_DUP) so the
         * scheduler's wrap-ahead read is consistent (Linux mirrors low slots). */
        if (wr < TFD_QUEUE_SIZE_BC_DUP)
            bc->tfd_offset[TFD_QUEUE_SIZE_MAX + wr] = bc_len;
    }

    /* Flush the TFD + byte-count table stores to DRAM before the device can be
     * told (via the doorbell) to read them. On x86 the sfence before the doorbell
     * below already orders these WB stores ahead of the UC doorbell write, but
     * make the descriptor-visible-before-doorbell ordering explicit here too,
     * matching Linux iwl_pcie_txq_build_tfd + update_byte_cnt + wmb. (Audit #8.) */
    iwl_desc_wmb();

    /* Hand the descriptor to the device: flush, then ring the write-pointer
     * doorbell with (write_ptr | (queue_id << 8)). NIC access held around it. */
    if (iwl_grab_nic_access(trans) != 0) {
        kprintf("IWLCMD: enqueue could not grab NIC access -- abort\n");
        return -1;
    }
    uint32_t next = (trans->cmd_wr_ptr + 1) & (TFD_QUEUE_SIZE_MAX - 1);
    kprintf("IWLCMD: ring cmd doorbell cmd=0x%02x wr=%u (HBUS_TARG_WRPTR)...\n",
            cmd_id, next);
    iwl_desc_wmb();
    iwl_write32(trans, HBUS_TARG_WRPTR,
                (next & 0xff) | ((IWL_CMD_QUEUE_ID & 0xff) << 8));
    iwl_release_nic_access(trans);

    trans->cmd_wr_ptr = next;
    return 0;
}

int iwl_send_cmd(struct iwl_trans* trans, uint8_t cmd_id,
                 const void* data, uint16_t len,
                 uint8_t want_cmd, iwl_rx_notif_t* resp) {
    if (!trans || !trans->mmio) return -1;

    kprintf("IWLCMD: send cmd=0x%02x len=%u want=0x%02x\n", cmd_id, len, want_cmd);
    if (iwl_cmd_enqueue(trans, cmd_id, data, len) != 0) {
        kprintf("IWLCMD: enqueue cmd=0x%02x FAILED -- abort\n", cmd_id);
        return -1;
    }

    if (want_cmd == 0)
        return 0;   /* async / fire-and-forget */

    return iwl_rx_wait_notif(trans, want_cmd, resp);
}
