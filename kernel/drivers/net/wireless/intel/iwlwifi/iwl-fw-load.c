/*
 * iwl-fw-load.c -- DVM uCode load -> ALIVE -> calibration -> runtime ALIVE.
 * ========================================================================
 * The firmware-load brick: it copies the parsed .ucode INST/DATA sections into
 * the radio's instruction/data SRAM over the FH service-channel DMA, releases
 * the embedded ARC processor, and waits (BOUNDED) for the ALIVE notification --
 * first for the INIT ucode (which runs calibrations), then for the RUNTIME
 * ucode. Once the runtime ALIVE arrives the radio can take host commands and a
 * REPLY_SCAN_CMD will return real networks.
 *
 *   ===================  HELD FOR HARDWARE -- READ THIS  ===================
 *   Nothing here runs at boot. Reached only via iwl_wifi_bringup() (iwl-ops.c)
 *   on the physical T410. No QEMU emulates the card -> correct-by-review vs
 *   Linux iwlwifi DVM:
 *     pcie/trans.c -- iwl_pcie_load_given_ucode, iwl_pcie_load_cpu_sections,
 *                     iwl_pcie_load_section, iwl_pcie_load_firmware_chunk_fh
 *                     (the FH service-channel DMA register programming), and the
 *                     final "write 0 to CSR_RESET" that releases the CPU.
 *     dvm/ucode.c  -- iwl_run_init_ucode (INIT load -> ALIVE -> CALIBRATION_CFG
 *                     -> CALIBRATION_COMPLETE), iwl_load_ucode_wait_alive,
 *                     iwl_alive_fn (is_valid == UCODE_VALID_OK).
 *
 *   Safety laws (mirroring iwl-trans.c):
 *     - every DMA-done / ALIVE / CALIB wait is ITERATION-CAPPED (never tick-based)
 *     - a kprintf MARKER precedes every risky MMIO / firmware step
 *     - any timeout returns -1 (abort-clean); never panic.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-fw-load.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf */
#include "mem.h"             /* pmm_alloc_page */
#include "iwl-trans.h"
#include "iwl-dvm-commands.h"
#include "iwl-hostcmd.h"
#include "iwl-fw-file.h"     /* iwl_get_le32 equivalents / TLV constants */
#include "iwl-fw-load.h"

/* Fixed TLV layout sizes (must match iwl-fw.c). */
#define FWL_HDR_SIZE   88u
#define FWL_TLV_HDR    8u

static inline uint32_t fwl_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ====================================================================== *
 *  iwl_fw_capture_sections -- re-walk the TLV stream capturing payload POINTERS
 *  (iwl-fw.c only records sizes). Same strict bounds discipline: no read past
 *  blob+len; reject any overrun. Assigns SRAM destinations per section type.
 * ====================================================================== */
int iwl_fw_capture_sections(const uint8_t* blob, uint32_t len,
                            iwl_fw_images_t* out) {
    if (!blob || !out) return -1;

    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;

    if (len < FWL_HDR_SIZE) return -1;
    if (fwl_le32(blob + 0) != 0) return -1;                 /* zero field */
    if (fwl_le32(blob + 4) != IWL_TLV_UCODE_MAGIC) return -1;

    uint32_t off = FWL_HDR_SIZE;
    while (off < len) {
        if (len - off < FWL_TLV_HDR) return -1;
        uint32_t type   = fwl_le32(blob + off);
        uint32_t length = fwl_le32(blob + off + 4);
        uint32_t avail  = len - off - FWL_TLV_HDR;
        if (length > avail) return -1;

        const uint8_t* payload = blob + off + FWL_TLV_HDR;

        switch (type) {
        case IWL_UCODE_TLV_INST:
            out->inst.data = payload; out->inst.len = length;
            out->inst.dest = IWL_RTC_INST_LOWER_BOUND; break;
        case IWL_UCODE_TLV_DATA:
            out->data.data = payload; out->data.len = length;
            out->data.dest = IWL_RTC_DATA_LOWER_BOUND; break;
        case IWL_UCODE_TLV_INIT:
            out->init_inst.data = payload; out->init_inst.len = length;
            out->init_inst.dest = IWL_RTC_INST_LOWER_BOUND; break;
        case IWL_UCODE_TLV_INIT_DATA:
            out->init_data.data = payload; out->init_data.len = length;
            out->init_data.dest = IWL_RTC_DATA_LOWER_BOUND; break;
        default: break;
        }

        uint32_t consumed = FWL_TLV_HDR;
        if (consumed > UINT32_MAX - length) return -1;
        consumed += length;
        uint32_t aligned = (consumed + 3u) & ~3u;
        if (aligned < consumed) return -1;
        if (aligned > len - off) return -1;
        off += aligned;
    }
    if (off != len) return -1;

    out->valid = (out->inst.len > 0 && out->data.len > 0) ? 1 : 0;
    return out->valid ? 0 : -1;
}

/* ====================================================================== *
 *  iwl_load_section -- DMA one firmware section into device SRAM over the FH
 *  service channel (channel 9). Chunked at FH_MEM_TB_MAX_LENGTH. Mirrors Linux
 *  pcie/trans.c iwl_pcie_load_section + iwl_pcie_load_firmware_chunk_fh:
 *    pause svc-chnl DMA, set SRAM dest, set DMA source addr lo/hi+len, set buf
 *    status valid, enable the DMA channel, poll CSR_FH_INT_STATUS for the
 *    svc-chnl completion bit (bounded), ack it.
 *  Each chunk is staged in an identity-mapped DRAM page (phys == virt) so the
 *  device DMAs FROM host DRAM INTO its SRAM.
 * ====================================================================== */
static void* g_dma_chunk     = (void*)0;
static uint64_t g_dma_chunk_dma = 0;

/* ====================================================================== *
 *  Calibration-result store (L-C3b). During the INIT phase the firmware streams
 *  CALIBRATION_RES_NOTIFICATION(0x66) packets, each a struct iwl_calib_hdr +
 *  trailing data. Linux iwl_calib_set records them (one per op_code) and
 *  iwl_send_calib_results replays each to the RUNTIME uCode as a
 *  REPLY_PHY_CALIBRATION_CMD(0xb0). We keep a small bounded store (the DVM init
 *  calibrations are a handful: a per-op_code slot is plenty). Each stored result
 *  is the raw notification payload (hdr + data), replayed verbatim.
 * ====================================================================== */
#define IWL_CALIB_MAX_RESULTS   8                  /* bounded: DVM has ~4-6 */
#define IWL_CALIB_MAX_LEN       IWL_RESP_MAX       /* a result fits one notif */

typedef struct {
    uint8_t  op_code;                              /* iwl_calib_hdr.op_code */
    uint16_t len;                                  /* bytes in buf (hdr + data) */
    uint8_t  buf[IWL_CALIB_MAX_LEN];               /* raw payload to replay */
} iwl_calib_result_t;

static iwl_calib_result_t g_calib_results[IWL_CALIB_MAX_RESULTS];
static int                g_calib_n = 0;

/* iwl_calib_record -- store/replace a calibration result keyed by op_code
 * (Linux iwl_calib_set: replace an existing op_code, else append). */
static void iwl_calib_record(const uint8_t* payload, uint16_t len) {
    if (len < (uint16_t)sizeof(struct iwl_calib_hdr)) return;
    if (len > IWL_CALIB_MAX_LEN) len = IWL_CALIB_MAX_LEN;
    uint8_t op = ((const struct iwl_calib_hdr*)payload)->op_code;

    int slot = -1;
    for (int i = 0; i < g_calib_n; i++)
        if (g_calib_results[i].op_code == op) { slot = i; break; }
    if (slot < 0) {
        if (g_calib_n >= IWL_CALIB_MAX_RESULTS) {
            kprintf("IWLLOAD: calib store full -- dropping op_code 0x%02x\n", op);
            return;
        }
        slot = g_calib_n++;
    }
    g_calib_results[slot].op_code = op;
    g_calib_results[slot].len     = len;
    for (uint16_t i = 0; i < len; i++) g_calib_results[slot].buf[i] = payload[i];
    kprintf("IWLLOAD: recorded calib result op_code=0x%02x len=%u (slot %d)\n",
            op, len, slot);
}

static int iwl_load_chunk_ensure(void) {
    if (g_dma_chunk) return 0;
    void* p = pmm_alloc_page();          /* one page = 4096B, <= FH_MEM_TB_MAX_LENGTH */
    if (!p) { kprintf("IWLLOAD: DMA chunk alloc FAILED -- abort\n"); return -1; }
    g_dma_chunk     = p;
    g_dma_chunk_dma = (uint64_t)(uintptr_t)p;
    return 0;
}

static int iwl_load_section(struct iwl_trans* trans, const iwl_fw_section_t* sec) {
    if (!sec->data || sec->len == 0) return 0;   /* nothing to load */
    if (iwl_load_chunk_ensure() != 0) return -1;

    kprintf("IWLLOAD: load section dest=0x%06x len=%u (FH svc-chnl)...\n",
            sec->dest, sec->len);

    uint32_t done = 0;
    while (done < sec->len) {
        uint32_t this_len = sec->len - done;
        if (this_len > PAGE_SIZE) this_len = PAGE_SIZE;   /* per-page chunk */

        /* Stage this chunk into the DMA page. */
        volatile uint8_t* dst = (volatile uint8_t*)g_dma_chunk;
        for (uint32_t i = 0; i < this_len; i++) dst[i] = sec->data[done + i];
        iwl_desc_wmb();

        uint32_t dest_addr = sec->dest + done;

        if (iwl_grab_nic_access(trans) != 0) {
            kprintf("IWLLOAD: section could not grab NIC access -- abort\n");
            return -1;
        }

        /* (1) pause the service-channel DMA. */
        kprintf("IWLLOAD: pause svc-chnl DMA (FH_TCSR_CHNL_TX_CONFIG_REG)...\n");
        iwl_write32(trans, FH_TCSR_CHNL_TX_CONFIG_REG_FW(FH_SRVC_CHNL),
                    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);

        /* (2) destination SRAM address. Linux iwl_pcie_load_firmware_chunk_fh
         * writes dst_addr UNSHIFTED to FH_SRVC_CHNL_SRAM_ADDR_REG. (Review fix
         * L-C1: the old `>> 4` divided the destination by 16, scattering the
         * uCode across the wrong SRAM addresses.) */
        kprintf("IWLLOAD: set SRAM dest 0x%06x (FH_SRVC_CHNL_SRAM_ADDR_REG)...\n",
                dest_addr);
        iwl_write32(trans, FH_SRVC_CHNL_SRAM_ADDR_REG(FH_SRVC_CHNL),
                    dest_addr);   /* unshifted (Linux) */

        /* (3) DMA source low 32 bits. */
        kprintf("IWLLOAD: set DMA src lo (FH_TFDIB_CTRL0_REG)...\n");
        iwl_write32(trans, FH_TFDIB_CTRL0_REG(FH_SRVC_CHNL),
                    (uint32_t)(g_dma_chunk_dma & FH_MEM_TFDIB_DRAM_ADDR_LSB_MSK));

        /* (4) DMA source high bits + byte count. */
        kprintf("IWLLOAD: set DMA src hi+len (FH_TFDIB_CTRL1_REG)...\n");
        iwl_write32(trans, FH_TFDIB_CTRL1_REG(FH_SRVC_CHNL),
                    ((uint32_t)((g_dma_chunk_dma >> 32) & 0xF)
                       << FH_MEM_TFDIB_REG1_ADDR_BITSHIFT) | this_len);

        /* (5) mark the buffer status valid. */
        kprintf("IWLLOAD: set buf status valid (FH_TCSR_CHNL_TX_BUF_STS_REG)...\n");
        iwl_write32(trans, FH_TCSR_CHNL_TX_BUF_STS_REG(FH_SRVC_CHNL),
                    (1u << FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM) |
                    (1u << FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX) |
                    FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID);

        /* (6) enable the DMA channel -> the copy runs. */
        kprintf("IWLLOAD: enable svc-chnl DMA (FH_TCSR_CHNL_TX_CONFIG_REG)...\n");
        iwl_desc_wmb();
        iwl_write32(trans, FH_TCSR_CHNL_TX_CONFIG_REG_FW(FH_SRVC_CHNL),
                    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE_FW |
                    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
                    FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);

        /* (7) poll CSR_FH_INT_STATUS for the TX-channel uCode-write-complete bits
         * (bounded). Linux signals completion on CSR_FH_INT_TX_MASK = BIT(1)|BIT(0)
         * and acks with that same mask (write-1-to-clear). (Review fix L-C2.) */
        kprintf("IWLLOAD: poll DMA done (CSR_FH_INT_STATUS TX_MASK=0x%x)...\n",
                CSR_FH_INT_TX_MASK);
        int done_ok = -1;
        for (int it = 0; it < IWL_TRANS_POLL_MAX; it++) {
            uint32_t st = iwl_read32(trans, CSR_FH_INT_STATUS_FW);
            if (st & CSR_FH_INT_TX_MASK) {
                /* ack the completion bits (write-1-to-clear). */
                iwl_write32(trans, CSR_FH_INT_STATUS_FW, CSR_FH_INT_TX_MASK);
                done_ok = 0;
                break;
            }
            iwl_udelay_approx(64);
        }
        iwl_release_nic_access(trans);

        if (done_ok != 0) {
            kprintf("IWLLOAD: section DMA TIMEOUT at off %u -- abort\n", done);
            return -1;
        }
        done += this_len;
    }

    kprintf("IWLLOAD: section dest=0x%06x loaded (%u bytes)\n", sec->dest, sec->len);
    return 0;
}

/* ====================================================================== *
 *  iwl_load_image_and_alive -- load one ucode image (INST+DATA), release the
 *  embedded CPU (CSR_RESET=0), and wait bounded for its ALIVE notification.
 *  Linux: iwl_pcie_load_given_ucode (load sections, then write 0 to CSR_RESET)
 *  + iwl_load_ucode_wait_alive (wait REPLY_ALIVE, is_valid == UCODE_VALID_OK).
 *  `want_subtype_init`: 1 => expect the INIT alive (ver_subtype == 9); 0 =>
 *  runtime. Returns 0 on a valid ALIVE, -1 otherwise.
 * ====================================================================== */
static int iwl_load_image_and_alive(struct iwl_trans* trans,
                                    const iwl_fw_section_t* inst,
                                    const iwl_fw_section_t* data,
                                    int want_subtype_init) {
    if (iwl_load_section(trans, inst) != 0) return -1;
    if (iwl_load_section(trans, data) != 0) return -1;

    /* Release the ARC: write 0 to CSR_RESET (pcie/trans.c). The CPU jumps to its
     * loaded instruction SRAM and begins executing the ucode. */
    if (iwl_grab_nic_access(trans) != 0) {
        kprintf("IWLLOAD: could not grab NIC access to release CPU -- abort\n");
        return -1;
    }
    kprintf("IWLLOAD: release embedded CPU (CSR_RESET=0) -- ucode starts...\n");
    iwl_desc_wmb();
    iwl_write32(trans, CSR_RESET, IWL_CSR_RESET_RUN);
    iwl_release_nic_access(trans);

    /* Wait bounded for the ALIVE notification on the RX ring. */
    kprintf("IWLLOAD: wait %s ALIVE (REPLY_ALIVE 0x01, bounded)...\n",
            want_subtype_init ? "INIT" : "RUNTIME");
    iwl_rx_notif_t notif;
    if (iwl_rx_wait_notif(trans, REPLY_ALIVE, &notif) != 0) {
        kprintf("IWLLOAD: no ALIVE -- abort\n");
        return -1;
    }

    /* Validate the ALIVE payload (is_valid == UCODE_VALID_OK). */
    if (notif.len < sizeof(struct iwl_alive_resp)) {
        kprintf("IWLLOAD: ALIVE payload short (%u) -- abort\n", notif.len);
        return -1;
    }
    const struct iwl_alive_resp* a = (const struct iwl_alive_resp*)notif.data;
    if (a->is_valid != UCODE_VALID_OK) {
        kprintf("IWLLOAD: ALIVE is_valid=0x%x (not OK) -- abort\n", a->is_valid);
        return -1;
    }

    /* L-M1: the INIT uCode reports ver_subtype == INITIALIZE_SUBTYPE(9); the
     * runtime uCode reports a different subtype. Linux iwl_alive_fn records the
     * subtype and the INIT path checks it. A mismatch means we loaded the wrong
     * image (or the ALIVE is stale) -- abort rather than calibrate a runtime. */
    if (want_subtype_init && a->ver_subtype != INITIALIZE_SUBTYPE) {
        kprintf("IWLLOAD: INIT ALIVE ver_subtype=%u (expected %u) -- abort\n",
                a->ver_subtype, INITIALIZE_SUBTYPE);
        return -1;
    }
    /* Symmetric guard (audit #9): a RUNTIME ALIVE must NOT report the INIT
     * subtype. If it does, the runtime image failed to take over (we are still
     * running the INIT uCode or the ALIVE is stale) -- proceeding would calibrate
     * + scan against a dead runtime and silently return zero SSIDs. Abort. */
    if (!want_subtype_init && a->ver_subtype == INITIALIZE_SUBTYPE) {
        kprintf("IWLLOAD: RUNTIME ALIVE still reports INIT subtype %u -- "
                "runtime uCode did not start, abort\n", a->ver_subtype);
        return -1;
    }

    kprintf("IWLLOAD: %s ALIVE OK ucode=%u.%u subtype=%u\n",
            want_subtype_init ? "INIT" : "RUNTIME",
            a->ucode_major, a->ucode_minor, a->ver_subtype);

    /* L-M2: Linux msleep(5) after a valid ALIVE to let RF-kill / the radio
     * settle before the first host command. The PIT may be frozen here, so use a
     * bounded busy settle instead of a tick wait (never wait on ticks). */
    kprintf("IWLLOAD: post-ALIVE settle (bounded ~5ms)...\n");
    iwl_udelay_approx(100000);

    /* Bring up the TX scheduler for the command queue. A freshly-started uCode
     * resets the scheduler, so this must run after EVERY ALIVE (INIT + RUNTIME)
     * before the first host command. iwl_scd_cmd_queue_init is one-shot per
     * trans, so clear the guard before each load. (Part of item H-C2.) */
    trans->scd_ready = 0;
    if (iwl_scd_cmd_queue_init(trans) != 0) {
        kprintf("IWLLOAD: SCD cmd-queue bring-up FAILED -- abort\n");
        return -1;
    }

    return 0;
}

/* ====================================================================== *
 *  iwl_send_calib_results -- replay every captured INIT calibration result to
 *  the RUNTIME uCode as a REPLY_PHY_CALIBRATION_CMD(0xb0) host command. Mirrors
 *  Linux dvm/calib.c iwl_send_calib_results: each stored result (iwl_calib_hdr +
 *  data) is sent verbatim as the command payload. Call AFTER the runtime ALIVE.
 *  (Item L-C3b.)
 * ====================================================================== */
static int iwl_send_calib_results(struct iwl_trans* trans) {
    kprintf("IWLLOAD: replay %d calib result(s) to runtime (REPLY_PHY_CALIBRATION_CMD 0xb0)\n",
            g_calib_n);
    for (int i = 0; i < g_calib_n; i++) {
        kprintf("IWLLOAD: replay calib op_code=0x%02x len=%u\n",
                g_calib_results[i].op_code, g_calib_results[i].len);
        if (iwl_send_cmd(trans, REPLY_PHY_CALIBRATION_CMD,
                         g_calib_results[i].buf, g_calib_results[i].len,
                         0, (iwl_rx_notif_t*)0) != 0) {
            kprintf("IWLLOAD: calib replay (op 0x%02x) FAILED -- abort\n",
                    g_calib_results[i].op_code);
            return -1;
        }
    }
    return 0;
}

/* ====================================================================== *
 *  iwl_run_init_and_calib -- load the INIT ucode, reach ALIVE, configure the
 *  calibrations (real struct iwl_calib_cfg_cmd: request ALL init calibrations
 *  + the SEND_COMPLETE notify), then drain the RX ring CAPTURING each
 *  CALIBRATION_RES_NOTIFICATION(0x66) until CALIBRATION_COMPLETE(0x67). The
 *  captured results are replayed to the runtime uCode later (iwl_send_calib_
 *  results). Linux dvm/ucode.c iwl_run_init_ucode + iwl_send_calib_cfg +
 *  iwlagn_wait_calib. (Item L-C3.)
 * ====================================================================== */
static int iwl_run_init_and_calib(struct iwl_trans* trans,
                                  const iwl_fw_images_t* fw) {
    /* If there is no separate INIT image, DVM skips the init/calib phase. */
    if (fw->init_inst.len == 0) {
        kprintf("IWLLOAD: no INIT ucode -- skipping calibration phase\n");
        return 0;
    }

    g_calib_n = 0;   /* fresh capture for this bring-up */

    if (iwl_load_image_and_alive(trans, &fw->init_inst, &fw->init_data, 1) != 0)
        return -1;

    /* Build the real CALIBRATION_CFG_CMD (L-C3a): request ALL init calibrations
     * (once.is_enable/start/send_res = IWL_CALIB_INIT_CFG_ALL) and ask for the
     * SEND_COMPLETE notify so 0x67 actually arrives. Linux iwl_send_calib_cfg. */
    kprintf("IWLLOAD: send CALIBRATION_CFG_CMD (0x65, ALL + SEND_COMPLETE)...\n");
    struct iwl_calib_cfg_cmd cfg;
    for (uint32_t i = 0; i < sizeof(cfg); i++) ((uint8_t*)&cfg)[i] = 0;
    cfg.ucd_calib_cfg.once.is_enable = IWL_CALIB_INIT_CFG_ALL;
    cfg.ucd_calib_cfg.once.start     = IWL_CALIB_INIT_CFG_ALL;
    cfg.ucd_calib_cfg.once.send_res  = IWL_CALIB_INIT_CFG_ALL;
    cfg.ucd_calib_cfg.flags          = IWL_CALIB_CFG_FLAG_SEND_COMPLETE_NTFY_MSK;
    if (iwl_send_cmd(trans, CALIBRATION_CFG_CMD, &cfg, sizeof(cfg),
                     0, (iwl_rx_notif_t*)0) != 0) {
        kprintf("IWLLOAD: CALIBRATION_CFG_CMD enqueue failed -- abort\n");
        return -1;
    }

    /* Drain the RX ring CAPTURING each CALIBRATION_RES_NOTIFICATION(0x66) until
     * CALIBRATION_COMPLETE(0x67). Bounded; abort-clean on timeout. (L-C3b) */
    kprintf("IWLLOAD: harvest CALIB_RES (0x66) until COMPLETE (0x67, bounded)...\n");
    int got_complete = -1;
    iwl_rx_notif_t n;
    for (int it = 0; it < IWL_CMD_WAIT_MAX; it++) {
        int got = iwl_rx_poll_one(trans, &n);
        if (got < 0) { kprintf("IWLLOAD: RX inconsistency in calib -- abort\n"); return -1; }
        if (got == 0) { iwl_udelay_approx(64); continue; }
        if (n.cmd == CALIBRATION_RES_NOTIFICATION) {
            iwl_calib_record(n.data, n.len);
            continue;
        }
        if (n.cmd == CALIBRATION_COMPLETE_NOTIFICATION) { got_complete = 0; break; }
        /* other notifications during calib: ignore, keep draining */
    }
    if (got_complete != 0) {
        kprintf("IWLLOAD: calibration did not complete -- abort\n");
        return -1;
    }
    kprintf("IWLLOAD: calibration complete -- captured %d result(s) to replay\n",
            g_calib_n);
    return 0;
}

/* ====================================================================== *
 *  iwl_load_ucode -- THE held firmware bring-up entry. INIT -> calib -> RUNTIME.
 * ====================================================================== */
int iwl_load_ucode(struct iwl_trans* trans, const iwl_fw_images_t* fw) {
    if (!trans || !trans->mmio || !fw) {
        kprintf("IWLLOAD: bad args -- abort\n");
        return -1;
    }
    if (!fw->valid || fw->inst.len == 0 || fw->data.len == 0) {
        kprintf("IWLLOAD: runtime INST/DATA missing -- abort (parse .ucode first)\n");
        return -1;
    }

    kprintf("IWLLOAD: firmware bring-up START (real T410 only)\n");

    /* Phase 1: INIT ucode + calibrations (skipped if no INIT image). */
    if (iwl_run_init_and_calib(trans, fw) != 0) {
        kprintf("IWLLOAD: INIT/calibration phase FAILED\n");
        return -1;
    }

    /* Phase 2: RUNTIME ucode -> ALIVE. After this the radio takes host commands. */
    kprintf("IWLLOAD: loading RUNTIME ucode...\n");
    if (iwl_load_image_and_alive(trans, &fw->inst, &fw->data, 0) != 0) {
        kprintf("IWLLOAD: RUNTIME load/ALIVE FAILED\n");
        return -1;
    }

    /* L-C3b: replay the INIT calibration results to the now-running RUNTIME uCode
     * (REPLY_PHY_CALIBRATION_CMD). The runtime uCode needs these to operate the
     * radio correctly; without the replay it runs uncalibrated. Linux
     * iwl_send_calib_results runs here. If nothing was captured (no INIT image)
     * this is a no-op. */
    if (iwl_send_calib_results(trans) != 0) {
        kprintf("IWLLOAD: calib replay to runtime FAILED -- abort\n");
        return -1;
    }

    kprintf("IWLLOAD: firmware ALIVE -- radio ready for NVM read + scan\n");
    return 0;
}
