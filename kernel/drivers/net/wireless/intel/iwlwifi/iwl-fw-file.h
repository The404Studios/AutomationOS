/*
 * iwl-fw-file.h -- Intel iwlwifi legacy and TLV uCode formats (IWL-FW).
 * =======================================================================
 * Brick 2 of the real Intel WiFi driver. Mirrors the on-disk layout of the
 * Linux iwlwifi "v2"/TLV firmware container (see Linux
 * drivers/net/wireless/intel/iwlwifi/fw/file.h). A real .ucode is a fixed
 * header followed by a stream of type/length/value (TLV) records. The parser
 * (iwl-fw.c) walks that stream with strict bounds checks and records the
 * sub-image sizes the later loader bricks (IWL-TRANS/IWL-LOAD) will need.
 *
 * This header defines ONLY the on-the-wire structs + the constants the parser
 * touches. No code, no driver state -- IWL-IDENT already owns the device table.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-fw-file.h
 */
#ifndef IWL_FW_FILE_H
#define IWL_FW_FILE_H

#include "types.h"

/*
 * The TLV firmware magic. A legacy v1 .ucode begins with a non-zero version
 * field; the TLV format reuses that first word as a guaranteed-zero marker and
 * carries the real magic in the second word. So: zero==0 && magic==MAGIC
 * uniquely identifies the modern TLV container.
 */
#define IWL_TLV_UCODE_MAGIC   0x0a4c5749u   /* "IWL\n" little-endian */

/* Legacy DVM firmware starts with a packed version word followed by five image
 * sizes. API 1/2 use the 24-byte v1 header; API 3+ insert a build word and use
 * the 28-byte v2 header. Payload order is runtime INST, runtime DATA, INIT INST,
 * INIT DATA, then optional bootstrap bytes. iwlwifi-6000-4.ucode, used by common
 * T410 Intel 6200/6300 cards, is this legacy v2 format rather than TLV. */
#define IWL_UCODE_API(ver)              (((ver) >> 8) & 0xffu)
#define IWL_LEGACY_V1_HDR_SIZE          24u
#define IWL_LEGACY_V2_HDR_SIZE          28u

struct iwl_legacy_fw_layout {
    uint32_t ver;
    uint32_t header_size;
    uint32_t inst_size;
    uint32_t data_size;
    uint32_t init_size;
    uint32_t init_data_size;
    uint32_t boot_size;
};

static inline uint32_t iwl_fw_get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Decode and fully bounds-check a legacy firmware container. Requiring the
 * section sizes to consume the file exactly prevents a wrong header variant or
 * corrupt size table from producing plausible pointers into unrelated bytes. */
static inline int iwl_legacy_fw_decode(const uint8_t* blob, uint32_t len,
                                       struct iwl_legacy_fw_layout* out) {
    if (!blob || !out || len < 4) return -1;

    uint32_t ver = iwl_fw_get_le32(blob);
    if (ver == 0) return -1;

    uint32_t header_size = IWL_UCODE_API(ver) <= 2
                         ? IWL_LEGACY_V1_HDR_SIZE : IWL_LEGACY_V2_HDR_SIZE;
    if (len < header_size) return -1;

    uint32_t size_off = header_size == IWL_LEGACY_V1_HDR_SIZE ? 4u : 8u;
    out->ver            = ver;
    out->header_size    = header_size;
    out->inst_size      = iwl_fw_get_le32(blob + size_off + 0);
    out->data_size      = iwl_fw_get_le32(blob + size_off + 4);
    out->init_size      = iwl_fw_get_le32(blob + size_off + 8);
    out->init_data_size = iwl_fw_get_le32(blob + size_off + 12);
    out->boot_size      = iwl_fw_get_le32(blob + size_off + 16);

    if (out->inst_size == 0 || out->data_size == 0) return -1;

    uint64_t total = (uint64_t)header_size + out->inst_size + out->data_size +
                     out->init_size + out->init_data_size + out->boot_size;
    return total == (uint64_t)len ? 0 : -1;
}

/*
 * struct iwl_tlv_ucode_header -- the fixed 88-byte header at the start of a
 * modern .ucode. Packed little-endian on disk; the T410 (x86) is LE so a plain
 * struct overlay matches. Field order/sizes mirror Linux fw/file.h exactly:
 *
 *   zero            (4)   MUST be 0 -- distinguishes TLV from legacy v1.
 *   magic           (4)   == IWL_TLV_UCODE_MAGIC.
 *   human_readable  (64)  NUL-padded build string (e.g. "iwlwifi-6000-6.ucode").
 *   ver             (4)   firmware version.
 *   build           (4)   build number.
 *   ignore          (8)   reserved (was init/inst size in older layouts).
 *
 * Total = 4+4+64+4+4+8 = 88 bytes. The TLV stream begins immediately after.
 */
struct iwl_tlv_ucode_header {
    uint32_t zero;
    uint32_t magic;
    uint8_t  human_readable[64];
    uint32_t ver;
    uint32_t build;
    uint64_t ignore;
};

/*
 * struct iwl_ucode_tlv -- one TLV record header. `length` is the payload byte
 * count (NOT including this 8-byte header); each record is padded so the NEXT
 * record starts on a 4-byte boundary.
 */
struct iwl_ucode_tlv {
    uint32_t type;
    uint32_t length;
    uint8_t  data[];
};

/* TLV types (subset the parser recognizes; values per Linux fw/file.h). */
#define IWL_UCODE_TLV_INST                  1
#define IWL_UCODE_TLV_DATA                  2
#define IWL_UCODE_TLV_INIT                  3
#define IWL_UCODE_TLV_INIT_DATA             4
#define IWL_UCODE_TLV_FLAGS                 18  /* was misnamed API_FLAGS=14 (review fix) */
#define IWL_UCODE_TLV_ENABLED_CAPABILITIES  30  /* was 18 (collided with FLAGS) */

/*
 * struct iwl_fw -- the parsed result. The loader bricks read these sizes to
 * size + populate the uCode SRAM/DRAM rings. Sizes are in bytes.
 */
struct iwl_fw {
    uint32_t ver;             /* header ver field */
    uint32_t inst_size;       /* IWL_UCODE_TLV_INST payload bytes (runtime) */
    uint32_t data_size;       /* IWL_UCODE_TLV_DATA payload bytes (runtime) */
    uint32_t init_size;       /* IWL_UCODE_TLV_INIT payload bytes */
    uint32_t init_data_size;  /* IWL_UCODE_TLV_INIT_DATA payload bytes */
    int      num_tlvs;        /* total TLV records walked */
};

#endif /* IWL_FW_FILE_H */
