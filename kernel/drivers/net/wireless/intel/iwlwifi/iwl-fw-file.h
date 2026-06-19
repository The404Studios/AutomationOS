/*
 * iwl-fw-file.h -- Intel iwlwifi MODERN TLV uCode firmware format (IWL-FW).
 * ==========================================================================
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
#define IWL_UCODE_TLV_API_FLAGS             14
#define IWL_UCODE_TLV_ENABLED_CAPABILITIES  18

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
