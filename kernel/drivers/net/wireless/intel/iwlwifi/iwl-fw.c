/*
 * iwl-fw.c -- Intel iwlwifi MODERN TLV uCode firmware parser (IWL-FW).
 * ===================================================================
 * Brick 2 of the real Intel WiFi driver. Parses the on-disk TLV .ucode
 * container (layout in iwl-fw-file.h) into an iwl_fw result: the version plus
 * the INST/DATA/INIT/INIT_DATA sub-image sizes the later loader bricks
 * (IWL-TRANS / IWL-LOAD) will copy into the radio's SRAM/DRAM rings.
 *
 * The real firmware (iwlwifi-<family>-*.ucode) is *trusted* hardware vendor
 * data, but a corrupt or truncated initrd file must NOT cause an out-of-bounds
 * read. So iwl_fw_parse() is written hostile-input-safe: every field read and
 * every stream advance is bounds-checked against `len` before it happens, and a
 * malformed blob is rejected with -1 instead of walking off the end.
 *
 * QEMU has no iwlwifi card and we ship no firmware yet, so the QEMU proof is
 * iwl_fw_selftest(): it parses an EMBEDDED synthetic .ucode (built byte-exact in
 * this file) and asserts the recovered sizes match what it embedded, then feeds
 * a truncated blob and asserts the parser rejects it without crashing. The real
 * .ucode drops in unchanged on the T410 via iwl_fw_load_from_initrd().
 *
 * Gated -DIWLWIFI (DEFAULT OFF), same as IWL-IDENT.
 *
 * Scope: kernel/drivers/net/wireless/intel/iwlwifi/iwl-fw.c
 */
#include "types.h"
#include "kernel.h"          /* kprintf */
#include "initrd.h"          /* initrd_get_file */
#include "iwl-fw-file.h"

/*
 * Little-endian uint32 read from an unaligned byte cursor. The header/TLV
 * structs are LE-on-disk; rather than overlay a struct (which assumes both LE
 * host AND no trap on a possibly-unaligned mapped buffer) we read fields byte
 * by byte. x86 is LE so this is also the natural order.
 */
static inline uint32_t iwl_get_le32(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Fixed header size on disk: 4+4+64+4+4+8. */
#define IWL_FW_HDR_SIZE   88u
/* Each TLV record header (type + length) before its payload. */
#define IWL_TLV_HDR_SIZE  8u

/*
 * iwl_fw_parse -- validate the header, then walk the TLV stream recording the
 * sub-image sizes. Returns 0 on success, -1 on any malformed/overrunning input.
 *
 * Bounds discipline (the whole point of this brick):
 *   - len must cover the fixed header.
 *   - header zero==0 && magic==MAGIC.
 *   - at each TLV: the 8-byte TLV header must fit in the remaining bytes BEFORE
 *     reading type/length; then the declared payload `length` must also fit;
 *     then the 4-byte-aligned advance must not overflow or exceed len.
 * No read is ever issued past `blob + len`.
 */
int iwl_fw_parse(const uint8_t* blob, uint32_t len, struct iwl_fw* out) {
    if (!blob || !out)
        return -1;

    /* Zero the result up front so a partial/failed parse never leaks stale data. */
    out->ver = 0;
    out->inst_size = 0;
    out->data_size = 0;
    out->init_size = 0;
    out->init_data_size = 0;
    out->num_tlvs = 0;

    /* Header must fit. */
    if (len < IWL_FW_HDR_SIZE)
        return -1;

    /* zero field (offset 0) must be 0 -- this is what separates TLV from v1. */
    if (iwl_get_le32(blob + 0) != 0)
        return -1;

    /* magic (offset 4). */
    if (iwl_get_le32(blob + 4) != IWL_TLV_UCODE_MAGIC)
        return -1;

    /* ver (offset 4+4+64 = 72). human_readable[64] is at offset 8. */
    out->ver = iwl_get_le32(blob + 72);

    /* Walk the TLV stream starting right after the fixed header. `off` is the
     * byte offset of the NEXT TLV header within blob. */
    uint32_t off = IWL_FW_HDR_SIZE;

    while (off < len) {
        /* Need at least the 8-byte TLV header to read type+length.
         * (off <= len already from the loop guard; check the header span.) */
        if (len - off < IWL_TLV_HDR_SIZE)
            return -1;

        uint32_t type   = iwl_get_le32(blob + off);
        uint32_t length = iwl_get_le32(blob + off + 4);

        /* The payload must fit in the bytes remaining after this TLV header.
         * Compute remaining as (len - off - hdr) without overflow: off+hdr <= len
         * is guaranteed by the check above. */
        uint32_t avail = len - off - IWL_TLV_HDR_SIZE;
        if (length > avail)
            return -1;

        const uint8_t* payload = blob + off + IWL_TLV_HDR_SIZE;
        (void)payload;   /* later bricks copy this; IWL-FW only records sizes */

        switch (type) {
        case IWL_UCODE_TLV_INST:       out->inst_size      = length; break;
        case IWL_UCODE_TLV_DATA:       out->data_size      = length; break;
        case IWL_UCODE_TLV_INIT:       out->init_size      = length; break;
        case IWL_UCODE_TLV_INIT_DATA:  out->init_data_size = length; break;
        default:
            /* API_FLAGS / CAPABILITIES / anything else: counted, not captured. */
            break;
        }

        out->num_tlvs++;

        /* Advance past this record's header + payload, 4-byte aligned. Guard the
         * additions against uint32 overflow at every step, then re-check len. */
        uint32_t consumed = IWL_TLV_HDR_SIZE;          /* <= len-off, safe */
        if (consumed > UINT32_MAX - length)
            return -1;
        consumed += length;                            /* hdr + payload */

        /* 4-byte align the advance (TLVs are padded to a 4-byte boundary). */
        uint32_t aligned = (consumed + 3u) & ~3u;
        if (aligned < consumed)                        /* alignment overflow */
            return -1;

        if (aligned > len - off)                       /* would step past blob */
            return -1;

        off += aligned;
    }

    /* A well-formed stream lands exactly on len. */
    if (off != len)
        return -1;

    return 0;
}

/*
 * iwl_fw_load_from_initrd -- the real-hardware path. Read the firmware file from
 * the initrd and parse it. No firmware ships today (the QEMU build has none), so
 * the absent-file case prints a clean operator hint and returns -1.
 */
int iwl_fw_load_from_initrd(const char* path, struct iwl_fw* out) {
    if (!path || !out)
        return -1;

    uint64_t size = 0;
    void* data = initrd_get_file(path, &size);
    if (!data) {
        kprintf("IWL-FW: no firmware %s in initrd "
                "(provide iwlwifi-<fam>-*.ucode for the T410)\n", path);
        return -1;
    }

    if (size > (uint64_t)UINT32_MAX) {
        kprintf("IWL-FW: firmware %s too large (%u bytes)\n", path, (uint32_t)size);
        return -1;
    }

    int rc = iwl_fw_parse((const uint8_t*)data, (uint32_t)size, out);
    if (rc != 0) {
        kprintf("IWL-FW: %s is malformed (not a valid TLV .ucode)\n", path);
        return -1;
    }

    kprintf("IWL-FW: loaded %s: inst=%u data=%u init=%u init_data=%u ver=%u tlvs=%d\n",
            path, out->inst_size, out->data_size, out->init_size,
            out->init_data_size, out->ver, out->num_tlvs);
    return 0;
}

/* ========================================================================== *
 *  Self-test: parse an EMBEDDED synthetic .ucode (the QEMU proof).
 * ========================================================================== *
 *
 * The blob is hand-assembled below as a static byte array so it round-trips
 * through iwl_fw_parse() with no external file. Layout (all uint32 fields are
 * little-endian; TLV payloads are 4-byte multiples so no padding is needed):
 *
 *   offset  bytes  field
 *   ------  -----  -----------------------------------------------------------
 *   0       4      zero            = 0
 *   4       4      magic           = 0x0a4c5749
 *   8       64     human_readable  = "iwlwifi-synth-selftest" + NUL pad
 *   72      4      ver             = 0x00010203
 *   76      4      build           = 0
 *   80      8      ignore          = 0
 *   88      8      TLV INST hdr    type=1, length=0x100
 *   96      256    INST payload    filler 0xA1
 *   352     8      TLV DATA hdr    type=2, length=0x80
 *   360     128    DATA payload    filler 0xD2
 *   488     8      TLV INIT hdr    type=3, length=0x40
 *   496     64     INIT payload    filler 0x13
 *   560     8      TLV INIT_DATA   type=4, length=0x20
 *   568     32     INIT_DATA pl    filler 0x14
 *   total = 600 bytes, ends exactly on len.
 *
 * Expected parse: inst=256, data=128, init=64, init_data=32, ver=0x10203,
 * num_tlvs=4.
 */

#define IWL_ST_INST_LEN       0x100u   /* 256 */
#define IWL_ST_DATA_LEN       0x80u    /* 128 */
#define IWL_ST_INIT_LEN       0x40u    /* 64  */
#define IWL_ST_INIT_DATA_LEN  0x20u    /* 32  */
#define IWL_ST_VER            0x00010203u

/* LE-byte expansion helpers for the static initializer. */
#define LE32(v)  ((v) & 0xff), (((v) >> 8) & 0xff), (((v) >> 16) & 0xff), (((v) >> 24) & 0xff)

/* 64-entry human_readable: "iwlwifi-synth-selftest" then NUL pad to 64. The
 * string is 22 chars; we spell it out and pad with a 42-byte zero run so the
 * total is exactly 64. */
#define IWL_ST_HR \
    'i','w','l','w','i','f','i','-','s','y','n','t','h','-', \
    's','e','l','f','t','e','s','t', \
    /* 22 chars above; 42 zero bytes of pad below = 64 total */ \
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, 0,0

/* Filler payloads: repeat a byte N times. We use explicit runs sized to match. */
#define R8(b)    (b),(b),(b),(b),(b),(b),(b),(b)
#define R16(b)   R8(b),R8(b)
#define R32(b)   R16(b),R16(b)
#define R64(b)   R32(b),R32(b)
#define R128(b)  R64(b),R64(b)
#define R256(b)  R128(b),R128(b)

static const uint8_t iwl_synth_ucode[] = {
    /* --- fixed header (88 bytes) --- */
    LE32(0u),                    /* zero  */
    LE32(IWL_TLV_UCODE_MAGIC),   /* magic */
    IWL_ST_HR,                   /* human_readable[64] */
    LE32(IWL_ST_VER),            /* ver   */
    LE32(0u),                    /* build */
    LE32(0u), LE32(0u),          /* ignore (8 bytes) */

    /* --- TLV: INST, length 0x100 --- */
    LE32(IWL_UCODE_TLV_INST), LE32(IWL_ST_INST_LEN),
    R256(0xA1),

    /* --- TLV: DATA, length 0x80 --- */
    LE32(IWL_UCODE_TLV_DATA), LE32(IWL_ST_DATA_LEN),
    R128(0xD2),

    /* --- TLV: INIT, length 0x40 --- */
    LE32(IWL_UCODE_TLV_INIT), LE32(IWL_ST_INIT_LEN),
    R64(0x13),

    /* --- TLV: INIT_DATA, length 0x20 --- */
    LE32(IWL_UCODE_TLV_INIT_DATA), LE32(IWL_ST_INIT_DATA_LEN),
    R32(0x14),
};

int iwl_fw_selftest(void) {
    struct iwl_fw fw;
    int ok = 1;

    /* ---- positive test: the synthetic blob must parse to the exact sizes. ---- */
    int rc = iwl_fw_parse(iwl_synth_ucode, (uint32_t)sizeof(iwl_synth_ucode), &fw);
    if (rc != 0) {
        kprintf("IWL-FW: selftest parse FAILED (rc=%d)\n", rc);
        ok = 0;
    } else {
        kprintf("IWL-FW: parsed inst=%u data=%u init=%u ver=0x%x\n",
                fw.inst_size, fw.data_size, fw.init_size, fw.ver);
        if (fw.inst_size      != IWL_ST_INST_LEN)      ok = 0;
        if (fw.data_size      != IWL_ST_DATA_LEN)      ok = 0;
        if (fw.init_size      != IWL_ST_INIT_LEN)      ok = 0;
        if (fw.init_data_size != IWL_ST_INIT_DATA_LEN) ok = 0;
        if (fw.ver            != IWL_ST_VER)           ok = 0;
        if (fw.num_tlvs       != 4)                    ok = 0;
    }

    /* ---- negative test 1: truncated blob (header says a TLV the bytes don't
     * cover). Truncate the synthetic blob mid-INST-payload; the parser must
     * reject it with -1 and must NOT read past the truncated length. ---- */
    uint32_t trunc_len = IWL_FW_HDR_SIZE + IWL_TLV_HDR_SIZE + 16; /* 16 of 256 INST */
    struct iwl_fw fw_bad;
    int rc_trunc = iwl_fw_parse(iwl_synth_ucode, trunc_len, &fw_bad);
    if (rc_trunc != -1) {
        kprintf("IWL-FW: negative(truncated) FAILED -- expected -1 got %d\n", rc_trunc);
        ok = 0;
    }

    /* ---- negative test 2: a blob shorter than even the fixed header. ---- */
    int rc_short = iwl_fw_parse(iwl_synth_ucode, 10, &fw_bad);
    if (rc_short != -1) {
        kprintf("IWL-FW: negative(short) FAILED -- expected -1 got %d\n", rc_short);
        ok = 0;
    }

    /* ---- negative test 3: corrupt magic (flip the zero field to non-zero so it
     * looks like legacy v1, which the TLV parser must reject). ---- */
    static uint8_t corrupt[IWL_FW_HDR_SIZE + IWL_TLV_HDR_SIZE];
    for (uint32_t i = 0; i < sizeof(corrupt); i++)
        corrupt[i] = iwl_synth_ucode[i];
    corrupt[0] = 0x01;   /* zero field -> non-zero */
    int rc_magic = iwl_fw_parse(corrupt, (uint32_t)sizeof(corrupt), &fw_bad);
    if (rc_magic != -1) {
        kprintf("IWL-FW: negative(zero!=0) FAILED -- expected -1 got %d\n", rc_magic);
        ok = 0;
    }

    /* ---- negative test 4: oversized TLV length (declares far more payload than
     * the blob holds); the `length > avail` bound must reject without OOB-read. */
    static uint8_t oversized[IWL_FW_HDR_SIZE + IWL_TLV_HDR_SIZE];
    for (uint32_t i = 0; i < IWL_FW_HDR_SIZE; i++) oversized[i] = iwl_synth_ucode[i];
    oversized[IWL_FW_HDR_SIZE + 0] = 0x01;  /* type = INST            */
    oversized[IWL_FW_HDR_SIZE + 1] = 0x00;
    oversized[IWL_FW_HDR_SIZE + 2] = 0x00;
    oversized[IWL_FW_HDR_SIZE + 3] = 0x00;
    oversized[IWL_FW_HDR_SIZE + 4] = 0xF0;  /* length = 0xFFFFFFF0    */
    oversized[IWL_FW_HDR_SIZE + 5] = 0xFF;
    oversized[IWL_FW_HDR_SIZE + 6] = 0xFF;
    oversized[IWL_FW_HDR_SIZE + 7] = 0xFF;
    int rc_over = iwl_fw_parse(oversized, (uint32_t)sizeof(oversized), &fw_bad);
    if (rc_over != -1) {
        kprintf("IWL-FW: negative(oversized) FAILED -- expected -1 got %d\n", rc_over);
        ok = 0;
    }

    /* ---- negative test 5: a TLV whose 4-byte-aligned advance overruns the blob
     * (payload length 5 with no room for its alignment padding); the alignment
     * guard must reject. ---- */
    static uint8_t misaligned[IWL_FW_HDR_SIZE + IWL_TLV_HDR_SIZE + 5];
    for (uint32_t i = 0; i < IWL_FW_HDR_SIZE; i++) misaligned[i] = iwl_synth_ucode[i];
    misaligned[IWL_FW_HDR_SIZE + 0] = 0x01;  /* type = INST           */
    misaligned[IWL_FW_HDR_SIZE + 1] = 0x00;
    misaligned[IWL_FW_HDR_SIZE + 2] = 0x00;
    misaligned[IWL_FW_HDR_SIZE + 3] = 0x00;
    misaligned[IWL_FW_HDR_SIZE + 4] = 0x05;  /* length = 5 (unaligned) */
    misaligned[IWL_FW_HDR_SIZE + 5] = 0x00;
    misaligned[IWL_FW_HDR_SIZE + 6] = 0x00;
    misaligned[IWL_FW_HDR_SIZE + 7] = 0x00;
    int rc_mis = iwl_fw_parse(misaligned, (uint32_t)sizeof(misaligned), &fw_bad);
    if (rc_mis != -1) {
        kprintf("IWL-FW: negative(misaligned) FAILED -- expected -1 got %d\n", rc_mis);
        ok = 0;
    }

    if (ok) {
        kprintf("IWL-FW: PASS\n");
        return 0;
    }
    kprintf("IWL-FW: FAIL\n");
    return -1;
}
