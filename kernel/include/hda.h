#ifndef HDA_H
#define HDA_H

#include "types.h"
#include "pci.h"

// Intel HDA PCI Class/Subclass/ProgIF
#define PCI_CLASS_MULTIMEDIA      0x04
#define PCI_SUBCLASS_AUDIO        0x03
#define PCI_PROG_IF_HDA           0x00

// HDA Register Offsets (Memory-Mapped I/O)
#define HDA_REG_GCAP              0x00    // Global Capabilities
#define HDA_REG_VMIN              0x02    // Minor Version
#define HDA_REG_VMAJ              0x03    // Major Version
#define HDA_REG_OUTPAY            0x04    // Output Payload Capability
#define HDA_REG_INPAY             0x06    // Input Payload Capability
#define HDA_REG_GCTL              0x08    // Global Control
#define HDA_REG_WAKEEN            0x0C    // Wake Enable
#define HDA_REG_STATESTS          0x0E    // State Change Status
#define HDA_REG_GSTS              0x10    // Global Status
#define HDA_REG_OUTSTRMPAY        0x18    // Output Stream Payload Capability
#define HDA_REG_INSTRMPAY         0x1A    // Input Stream Payload Capability
#define HDA_REG_INTCTL            0x20    // Interrupt Control
#define HDA_REG_INTSTS            0x24    // Interrupt Status
#define HDA_REG_WALCLK            0x30    // Wall Clock Counter
#define HDA_REG_SSYNC             0x38    // Stream Synchronization
#define HDA_REG_CORBLBASE         0x40    // CORB Lower Base Address
#define HDA_REG_CORBUBASE         0x44    // CORB Upper Base Address
#define HDA_REG_CORBWP            0x48    // CORB Write Pointer
#define HDA_REG_CORBRP            0x4A    // CORB Read Pointer
#define HDA_REG_CORBCTL           0x4C    // CORB Control
#define HDA_REG_CORBSTS           0x4D    // CORB Status
#define HDA_REG_CORBSIZE          0x4E    // CORB Size
#define HDA_REG_RIRBLBASE         0x50    // RIRB Lower Base Address
#define HDA_REG_RIRBUBASE         0x54    // RIRB Upper Base Address
#define HDA_REG_RIRBWP            0x58    // RIRB Write Pointer
#define HDA_REG_RINTCNT           0x5A    // Response Interrupt Count
#define HDA_REG_RIRBCTL           0x5C    // RIRB Control
#define HDA_REG_RIRBSTS           0x5D    // RIRB Status
#define HDA_REG_RIRBSIZE          0x5E    // RIRB Size
// Immediate Command Interface (HDA spec §3.4.3) -- a CORB/RIRB-free path for
// single codec verbs. More reliable than CORB/RIRB DMA under QEMU's intel-hda.
#define HDA_REG_IC                0x60    // Immediate Command (write the 32-bit verb)
#define HDA_REG_IR                0x64    // Immediate Response (read the 32-bit result)
#define HDA_REG_ICS               0x68    // Immediate Command Status (16-bit)
#define HDA_ICS_ICB               0x0001  // Immediate Command Busy (write 1 to issue)
#define HDA_ICS_IRV               0x0002  // Immediate Result Valid (RW1C)
#define HDA_REG_DPLBASE           0x70    // DMA Position Lower Base Address
#define HDA_REG_DPUBASE           0x74    // DMA Position Upper Base Address

// Stream Descriptor Registers (base + n * 0x20, n = stream number)
#define HDA_SD_BASE               0x80
#define HDA_SD_CTL                0x00    // Stream Control
#define HDA_SD_STS                0x03    // Stream Status
#define HDA_SD_LPIB               0x04    // Link Position in Buffer
#define HDA_SD_CBL                0x08    // Cyclic Buffer Length
#define HDA_SD_LVI                0x0C    // Last Valid Index
#define HDA_SD_FIFOW              0x0E    // FIFO Watermark
#define HDA_SD_FIFOS              0x10    // FIFO Size
#define HDA_SD_FMT                0x12    // Stream Format
#define HDA_SD_BDPL               0x18    // BDL Pointer Lower
#define HDA_SD_BDPU               0x1C    // BDL Pointer Upper

// Global Control Register Bits
#define HDA_GCTL_CRST             0x00000001  // Controller Reset

// CORB/RIRB Control Bits
#define HDA_CORBCTL_RUN           0x02
#define HDA_RIRBCTL_DMA_EN        0x02
#define HDA_RIRBCTL_INT_EN        0x01

// Stream Control Register Bits
#define HDA_SD_CTL_STREAM_RUN     0x00000002
#define HDA_SD_CTL_INT_EN         0x00000004
#define HDA_SD_CTL_FIFO_ERR_INT   0x00000008
#define HDA_SD_CTL_DESC_ERR_INT   0x00000010
#define HDA_SD_CTL_STRIPE_MASK    0x00030000
#define HDA_SD_CTL_TP             0x40000000
#define HDA_SD_CTL_STRIPE(x)      (((x) & 0x3) << 16)

// Stream Status Register Bits
#define HDA_SD_STS_FIFO_READY     0x20
#define HDA_SD_STS_BCIS           0x04  // Buffer Completion Interrupt Status
#define HDA_SD_STS_FIFOE          0x08  // FIFO Error
#define HDA_SD_STS_DESE           0x10  // Descriptor Error

// Verb Commands (HD Audio Specification)
// 12-bit verbs: encoded as (verb<<8)|param in the CORB DW, param is 8 bits.
#define HDA_VERB_GET_PARAMETER           0xF00
#define HDA_VERB_GET_CONN_SELECT         0xF01
#define HDA_VERB_SET_CONN_SELECT         0x701
#define HDA_VERB_GET_CONN_LIST           0xF02
#define HDA_VERB_GET_PIN_CTRL            0xF07
#define HDA_VERB_SET_PIN_CTRL            0x707
#define HDA_VERB_GET_PIN_SENSE           0xF09
#define HDA_VERB_SET_EAPD_ENABLE         0x70C
#define HDA_VERB_GET_CONFIG_DEFAULT      0xF1C
#define HDA_VERB_SET_POWER_STATE         0x705
#define HDA_VERB_SET_STREAM_CHANNEL      0x706  /* 12-bit verb: bits[7:4]=stream_tag, bits[3:0]=channel */
// 4-bit verbs: encoded via hda_send_verb4(); payload is 16 bits.
// These MUST NOT be passed to hda_send_command() -- the 8-bit param field
// would truncate the 16-bit payload.
#define HDA_VERB4_SET_CONVERTER_FORMAT   0x2    /* 4-bit: SET_CONVERTER_FORMAT, payload=format_word */
#define HDA_VERB4_SET_AMP_GAIN_MUTE      0x3    /* 4-bit: SET_AMP_GAIN_MUTE,      payload=amp_word  */
// Legacy 12-bit aliases kept for backward compatibility (WRONG for amp use):
#define HDA_VERB_GET_AMP_GAIN_MUTE       0xB00
#define HDA_VERB_SET_AMP_GAIN_MUTE       0x300  /* DO NOT USE - use HDA_VERB4_SET_AMP_GAIN_MUTE */

// Parameters
#define HDA_PARAM_VENDOR_ID              0x00
#define HDA_PARAM_REVISION_ID            0x02
#define HDA_PARAM_SUB_NODE_COUNT         0x04
#define HDA_PARAM_FUNC_GROUP_TYPE        0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP       0x09
#define HDA_PARAM_PCM_SIZE_RATE          0x0A
#define HDA_PARAM_STREAM_FORMATS         0x0B
#define HDA_PARAM_PIN_CAP                0x0C
#define HDA_PARAM_AMP_INPUT_CAP          0x0D
#define HDA_PARAM_AMP_OUTPUT_CAP         0x12
#define HDA_PARAM_CONN_LIST_LEN          0x0E
#define HDA_PARAM_POWER_STATE            0x0F
#define HDA_PARAM_GPIO_COUNT             0x11
#define HDA_PARAM_VOLUME_KNOB_CAP        0x13

// Audio Widget Types
#define HDA_WIDGET_AUDIO_OUTPUT          0x0
#define HDA_WIDGET_AUDIO_INPUT           0x1
#define HDA_WIDGET_AUDIO_MIXER           0x2
#define HDA_WIDGET_AUDIO_SELECTOR        0x3
#define HDA_WIDGET_PIN_COMPLEX           0x4
#define HDA_WIDGET_POWER                 0x5
#define HDA_WIDGET_VOLUME_KNOB           0x6
#define HDA_WIDGET_BEEP_GENERATOR        0x7
#define HDA_WIDGET_VENDOR_DEFINED        0xF

// Pin Widget Control Bits
#define HDA_PIN_CTL_VREF_EN              0x01
#define HDA_PIN_CTL_IN_EN                0x20
#define HDA_PIN_CTL_OUT_EN               0x40
#define HDA_PIN_CTL_HP_EN                0x80

// Stream Format Bits (16-bit format register)
// Bits [3:0] = Number of channels - 1
// Bits [6:4] = Sample base rate (0=48kHz, 1=44.1kHz)
// Bits [10:8] = Sample rate multiplier (0=1x, 1=2x, 2=3x, 3=4x)
// Bits [13:11] = Sample rate divisor (0=1, 1=/2, 2=/3, 3=/4, 4=/5, 5=/6, 6=/7, 7=/8)
// Bits [15:14] = Bits per sample (0=8bit, 1=16bit, 2=20bit, 3=24bit, 4=32bit)
#define HDA_FMT_CHAN(x)           ((x) & 0xF)
#define HDA_FMT_BASE_44KHZ        (1 << 14)
#define HDA_FMT_BASE_48KHZ        (0 << 14)
#define HDA_FMT_MULT(x)           (((x) & 0x7) << 11)
#define HDA_FMT_DIV(x)            (((x) & 0x7) << 8)
#define HDA_FMT_BITS_8            (0 << 4)
#define HDA_FMT_BITS_16           (1 << 4)
#define HDA_FMT_BITS_20           (2 << 4)
#define HDA_FMT_BITS_24           (3 << 4)
#define HDA_FMT_BITS_32           (4 << 4)

// Common format presets
#define HDA_FORMAT_48KHZ_16BIT_STEREO  (HDA_FMT_BASE_48KHZ | HDA_FMT_BITS_16 | HDA_FMT_CHAN(1))
#define HDA_FORMAT_44KHZ_16BIT_STEREO  (HDA_FMT_BASE_44KHZ | HDA_FMT_BITS_16 | HDA_FMT_CHAN(1))

// Buffer Descriptor List Entry
typedef struct {
    uint64_t address;          // Buffer physical address
    uint32_t length;           // Buffer length in bytes
    uint32_t ioc;              // Interrupt on Completion (bit 0)
} __attribute__((packed)) hda_bdl_entry_t;

// Maximum values
#define HDA_MAX_CODECS            15
#define HDA_MAX_STREAMS           16
#define HDA_MAX_WIDGETS           128
#define HDA_BDL_MAX_ENTRIES       256
#define HDA_CORB_ENTRIES          256
#define HDA_RIRB_ENTRIES          256

// DMA buffer sizes
#define HDA_DMA_BUFFER_SIZE       (16 * 4096)  // 64KB audio buffer

// CORB Entry (Command Output Ring Buffer)
typedef struct {
    uint32_t data;
} __attribute__((packed)) hda_corb_entry_t;

// RIRB Entry (Response Input Ring Buffer)
typedef struct {
    uint32_t response;
    uint32_t response_ex;
} __attribute__((packed)) hda_rirb_entry_t;

// HDA Widget Node
typedef struct {
    uint8_t nid;                          // Node ID
    uint8_t type;                         // Widget type
    uint32_t capabilities;                // Audio Widget Capabilities
    uint32_t pin_caps;                    // Pin Capabilities (for pin widgets)
    uint32_t config_default;              // Configuration Default
    uint8_t conn_list[HDA_MAX_WIDGETS];   // Connection list
    uint8_t conn_list_len;
    uint8_t selected_conn;                // Currently selected connection
} hda_widget_t;

// HDA Codec
typedef struct {
    uint8_t addr;                         // Codec address (0-14)
    uint32_t vendor_id;                   // Vendor ID
    uint32_t subsystem_id;                // Subsystem ID
    uint32_t revision_id;                 // Revision ID

    // Audio Function Group
    uint8_t afg_nid;                      // AFG Node ID
    uint8_t afg_start_nid;                // First widget NID
    uint8_t afg_num_nodes;                // Number of widgets

    // Widgets
    hda_widget_t widgets[HDA_MAX_WIDGETS];
    uint8_t num_widgets;

    // Output/Input paths
    uint8_t dac_nid;                      // DAC (Digital-to-Analog Converter) node
    uint8_t pin_nid;                      // Output pin node
    uint8_t adc_nid;                      // ADC (Analog-to-Digital Converter) node
    uint8_t input_pin_nid;                // Input pin node
} hda_codec_t;

// HDA Stream
typedef struct {
    uint8_t stream_num;                   // Stream number (1-15, 0 reserved)
    uint8_t stream_tag;                   // Stream tag
    bool is_output;                       // true = playback, false = capture
    bool running;                         // Stream running state

    // DMA buffers
    void* buffer_phys;                    // Physical address of audio buffer
    void* buffer_virt;                    // Virtual address of audio buffer
    uint32_t buffer_size;                 // Total buffer size

    // Buffer Descriptor List
    hda_bdl_entry_t* bdl_virt;           // Virtual address of BDL
    void* bdl_phys;                       // Physical address of BDL
    uint32_t bdl_entries;                 // Number of BDL entries

    // Format
    uint16_t format;                      // Stream format
    uint32_t sample_rate;                 // Sample rate (Hz)
    uint8_t bits_per_sample;              // Bits per sample
    uint8_t channels;                     // Number of channels

    // Stream position
    uint32_t position;                    // Current position in buffer (bytes)
#ifdef HDA_ENABLE
    /* AUDIO B1 gapless streaming: the DMA plays the 8-entry BDL cyclically and
     * raises one BCIS per chunk; on each BCIS the IRQ handler refills the
     * just-completed chunk via refill_cb, so playback continues past the initial
     * 64 KB (true gapless streaming, not a looped buffer). All HDA_ENABLE-gated
     * so the default (HDA-off) kernel is byte-identical. */
    uint32_t chunk_size;                  // bytes per BDL chunk (buffer_size/bdl_entries)
    uint32_t refill_next;                 // next chunk index on_bcis will refill
    volatile uint32_t refill_count;       // chunks refilled (proof counter)
    void (*refill_cb)(void* stream, uint32_t chunk_idx);  // NULL = not streaming
#endif
} hda_stream_t;

// HDA Controller
typedef struct {
    pci_device_t* pci_dev;                // PCI device
    void* mmio_base;                      // MMIO base address (BAR0)

    // Controller capabilities
    uint16_t gcap;                        // Global Capabilities
    uint8_t vmaj;                         // Major version
    uint8_t vmin;                         // Minor version
    uint8_t num_oss;                      // Number of output streams
    uint8_t num_iss;                      // Number of input streams
    uint8_t num_bss;                      // Number of bidirectional streams

    // CORB/RIRB (Command/Response buffers)
    hda_corb_entry_t* corb_virt;         // CORB virtual address
    void* corb_phys;                      // CORB physical address
    uint16_t corb_wp;                     // CORB write pointer

    hda_rirb_entry_t* rirb_virt;         // RIRB virtual address
    void* rirb_phys;                      // RIRB physical address
    uint16_t rirb_rp;                     // RIRB read pointer

    // Codecs
    hda_codec_t* codecs[HDA_MAX_CODECS];
    uint8_t num_codecs;

    // Streams
    hda_stream_t* streams[HDA_MAX_STREAMS];
    uint8_t num_streams;

    // IRQ
    uint8_t irq_line;
} hda_controller_t;

// HDA Functions
void hda_init(void);
void hda_msleep(uint32_t ms);   // millisecond busy-wait, shared by stream/tone code
hda_controller_t* hda_find_controller(void);
int hda_reset_controller(hda_controller_t* ctrl);
int hda_init_corb_rirb(hda_controller_t* ctrl);
int hda_enumerate_codecs(hda_controller_t* ctrl);

// Codec functions
uint32_t hda_send_command(hda_controller_t* ctrl, uint8_t codec_addr,
                          uint8_t nid, uint32_t verb, uint32_t param);
/* 4-bit verb sender (SET_AMP_GAIN_MUTE, SET_CONVERTER_FORMAT).
 * verb4 = 4-bit verb nibble (e.g. 0x3 or 0x2); payload16 = full 16-bit payload. */
uint32_t hda_send_verb4(hda_controller_t* ctrl, uint8_t codec_addr,
                        uint8_t nid, uint8_t verb4, uint16_t payload16);
int hda_codec_read_widgets(hda_codec_t* codec, hda_controller_t* ctrl);
int hda_codec_setup_output(hda_codec_t* codec, hda_controller_t* ctrl);
int hda_codec_setup_input(hda_codec_t* codec, hda_controller_t* ctrl);

// Stream functions
hda_stream_t* hda_stream_alloc(hda_controller_t* ctrl, bool is_output);
void hda_stream_free(hda_stream_t* stream);
int hda_stream_setup(hda_controller_t* ctrl, hda_stream_t* stream,
                     uint32_t sample_rate, uint8_t bits, uint8_t channels);
int hda_stream_start(hda_controller_t* ctrl, hda_stream_t* stream);
int hda_stream_stop(hda_controller_t* ctrl, hda_stream_t* stream);
int hda_stream_write(hda_stream_t* stream, const void* data, uint32_t size);
int hda_stream_read(hda_stream_t* stream, void* data, uint32_t size);

// Volume control
int hda_set_volume(hda_codec_t* codec, hda_controller_t* ctrl, uint8_t volume);
int hda_set_mute(hda_codec_t* codec, hda_controller_t* ctrl, bool mute);

// Interrupt handler
void hda_irq_handler(void);

/*
 * AUDIO-IRQ proof counter: incremented inside hda_irq_handler() every time a
 * stream's buffer-completion (BCIS) status bit is observed.  A non-zero value
 * proves the stream DMA engine advanced through at least one BDL entry and the
 * controller raised (and we serviced) a real interrupt.  Read by the
 * AUDIO-VERIFY marker in audio_tone.c.  volatile: written from IRQ context,
 * read from task context.
 */
extern volatile uint32_t g_hda_bcis;

/* Read the active stream's Link Position In Buffer (SD_LPIB).  Returns the
 * byte position the DMA engine has consumed; a delta across playback proves
 * real DMA movement even on builds where the BCIS interrupt does not fire.
 * Returns 0 if no tone stream is active. */
uint32_t hda_active_lpib(void);

// Register access helpers
static inline uint32_t hda_read32(hda_controller_t* ctrl, uint32_t offset) {
    return *((volatile uint32_t*)((uint8_t*)ctrl->mmio_base + offset));
}

static inline void hda_write32(hda_controller_t* ctrl, uint32_t offset, uint32_t value) {
    *((volatile uint32_t*)((uint8_t*)ctrl->mmio_base + offset)) = value;
}

static inline uint16_t hda_read16(hda_controller_t* ctrl, uint32_t offset) {
    return *((volatile uint16_t*)((uint8_t*)ctrl->mmio_base + offset));
}

static inline void hda_write16(hda_controller_t* ctrl, uint32_t offset, uint16_t value) {
    *((volatile uint16_t*)((uint8_t*)ctrl->mmio_base + offset)) = value;
}

static inline uint8_t hda_read8(hda_controller_t* ctrl, uint32_t offset) {
    return *((volatile uint8_t*)((uint8_t*)ctrl->mmio_base + offset));
}

static inline void hda_write8(hda_controller_t* ctrl, uint32_t offset, uint8_t value) {
    *((volatile uint8_t*)((uint8_t*)ctrl->mmio_base + offset)) = value;
}

// Stream register access
static inline uint32_t hda_sd_read32(hda_controller_t* ctrl, uint8_t stream, uint32_t offset) {
    return hda_read32(ctrl, HDA_SD_BASE + (stream * 0x20) + offset);
}

static inline void hda_sd_write32(hda_controller_t* ctrl, uint8_t stream, uint32_t offset, uint32_t value) {
    hda_write32(ctrl, HDA_SD_BASE + (stream * 0x20) + offset, value);
}

static inline uint16_t hda_sd_read16(hda_controller_t* ctrl, uint8_t stream, uint32_t offset) {
    return hda_read16(ctrl, HDA_SD_BASE + (stream * 0x20) + offset);
}

static inline void hda_sd_write16(hda_controller_t* ctrl, uint8_t stream, uint32_t offset, uint16_t value) {
    hda_write16(ctrl, HDA_SD_BASE + (stream * 0x20) + offset, value);
}

static inline uint8_t hda_sd_read8(hda_controller_t* ctrl, uint8_t stream, uint32_t offset) {
    return hda_read8(ctrl, HDA_SD_BASE + (stream * 0x20) + offset);
}

static inline void hda_sd_write8(hda_controller_t* ctrl, uint8_t stream, uint32_t offset, uint8_t value) {
    hda_write8(ctrl, HDA_SD_BASE + (stream * 0x20) + offset, value);
}

#endif
