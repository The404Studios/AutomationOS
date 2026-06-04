#include "../include/hda.h"
#include "../include/mem.h"
#include "../include/x86_64.h"
#include "../include/drivers.h"   /* serial_write, serial_putchar */

// External helpers
extern void hda_msleep(uint32_t ms);

/*
 * hda_send_verb4 - send a 4-bit verb with 16-bit payload (defined in hda.c).
 * Declared here as extern so hda_stream.c can call it without a header change.
 *
 * 4-bit verb encoding (HDA spec §7.3.3):
 *   bits[31:28]=codec_addr, bits[27:20]=NID, bits[19:16]=verb4, bits[15:0]=payload16
 */
extern uint32_t hda_send_verb4(hda_controller_t* ctrl, uint8_t codec_addr,
                                uint8_t nid, uint8_t verb4, uint16_t payload16);

/**
 * Allocate a stream descriptor
 */
hda_stream_t* hda_stream_alloc(hda_controller_t* ctrl, bool is_output) {
    // Find available stream
    uint8_t max_streams = is_output ? ctrl->num_oss : ctrl->num_iss;
    uint8_t stream_base = is_output ? 0 : ctrl->num_oss;

    for (uint8_t i = 0; i < max_streams; i++) {
        uint8_t stream_num = stream_base + i;
        if (ctrl->streams[stream_num] == NULL) {
            // Allocate stream structure
            hda_stream_t* stream = (hda_stream_t*)kmalloc(sizeof(hda_stream_t));
            if (!stream) {
                return NULL;
            }

            // Clear structure
            for (uint32_t j = 0; j < sizeof(hda_stream_t); j++) {
                ((uint8_t*)stream)[j] = 0;
            }

            stream->stream_num = stream_num;
            stream->stream_tag = stream_num + 1;  // Tags are 1-based
            stream->is_output = is_output;
            stream->running = false;

            ctrl->streams[stream_num] = stream;

            serial_write("HDA: Allocated stream ", 22);
            serial_putchar('0' + stream_num);
            serial_write(is_output ? " (output)\n" : " (input)\n", 10);

            return stream;
        }
    }

    serial_write("HDA: No available streams\n", 27);
    return NULL;
}

/**
 * Free a stream descriptor
 */
void hda_stream_free(hda_stream_t* stream) {
    if (!stream) {
        return;
    }

    // Free DMA buffer (allocated as a contiguous run with pmm_alloc_pages)
    if (stream->buffer_phys) {
        uint32_t num_pages = (stream->buffer_size + 4095) / 4096;
        pmm_free_pages(stream->buffer_phys, (size_t)num_pages);
    }

    // Free BDL
    if (stream->bdl_phys) {
        pmm_free_page(stream->bdl_phys);
    }

    kfree(stream);
}

/**
 * Setup stream format and DMA buffers
 */
int hda_stream_setup(hda_controller_t* ctrl, hda_stream_t* stream,
                     uint32_t sample_rate, uint8_t bits, uint8_t channels) {
    if (!stream) {
        return -1;
    }

    serial_write("HDA: Setting up stream ", 23);
    serial_putchar('0' + stream->stream_num);
    serial_putchar('\n');

    // Stop stream if running
    if (stream->running) {
        hda_stream_stop(ctrl, stream);
    }

    // Calculate format register value
    uint16_t format = 0;

    // Channels (0 = 1 channel, 1 = 2 channels, etc.)
    format |= HDA_FMT_CHAN(channels - 1);

    // Sample rate
    if (sample_rate == 44100) {
        format |= HDA_FMT_BASE_44KHZ;
    } else if (sample_rate == 48000) {
        format |= HDA_FMT_BASE_48KHZ;
    } else {
        serial_write("HDA: Unsupported sample rate\n", 30);
        return -1;
    }

    // Bits per sample
    if (bits == 8) {
        format |= HDA_FMT_BITS_8;
    } else if (bits == 16) {
        format |= HDA_FMT_BITS_16;
    } else if (bits == 20) {
        format |= HDA_FMT_BITS_20;
    } else if (bits == 24) {
        format |= HDA_FMT_BITS_24;
    } else if (bits == 32) {
        format |= HDA_FMT_BITS_32;
    } else {
        serial_write("HDA: Unsupported bit depth\n", 28);
        return -1;
    }

    stream->format = format;
    stream->sample_rate = sample_rate;
    stream->bits_per_sample = bits;
    stream->channels = channels;

    /*
     * Allocate a physically-contiguous DMA buffer.
     *
     * DMA/virt→phys assumption: the kernel runs with an identity map for all
     * physical RAM (phys == virt for kernel addresses).  pmm_alloc_pages(N)
     * returns a contiguous run of N physical 4 KB pages; the returned pointer
     * is simultaneously the physical base address and the accessible virtual
     * address.  No additional mapping step is needed.
     *
     * HDA_DMA_BUFFER_SIZE = 64 KB = 16 pages.  We use pmm_alloc_pages(16)
     * rather than pmm_alloc_page() which gives only ONE page (4 KB).
     * Allocating a single page and then addressing 64 KB past it would read/
     * write past the allocated region — undefined behaviour that typically
     * corrupts adjacent kernel structures or causes a page fault.
     */
    stream->buffer_size = HDA_DMA_BUFFER_SIZE;
    uint32_t num_pages = (stream->buffer_size + 4095) / 4096; /* = 16 */

    void* buffer_phys = pmm_alloc_pages((size_t)num_pages);
    if (!buffer_phys) {
        serial_write("HDA: Failed to allocate DMA buffer\n", 36);
        return -1;
    }

    stream->buffer_phys = buffer_phys;
    stream->buffer_virt = buffer_phys;  /* identity-mapped: phys == virt */

    // Clear buffer
    for (uint32_t i = 0; i < stream->buffer_size; i++) {
        ((uint8_t*)stream->buffer_virt)[i] = 0;
    }

    // Allocate BDL (Buffer Descriptor List)
    void* bdl_page = pmm_alloc_page();
    if (!bdl_page) {
        serial_write("HDA: Failed to allocate BDL\n", 29);
        pmm_free_page(buffer_phys);
        return -1;
    }

    stream->bdl_phys = bdl_page;
    stream->bdl_virt = (hda_bdl_entry_t*)bdl_page;

    // Setup BDL entries (split buffer into chunks for periodic interrupts)
    // Use 8 entries of 8KB each = 64KB total
    stream->bdl_entries = 8;
    uint32_t chunk_size = stream->buffer_size / stream->bdl_entries;

    for (uint32_t i = 0; i < stream->bdl_entries; i++) {
        stream->bdl_virt[i].address = (uint64_t)buffer_phys + (i * chunk_size);
        stream->bdl_virt[i].length = chunk_size;
        stream->bdl_virt[i].ioc = 1;  // Interrupt on completion for each entry
    }

    // Configure stream descriptor registers
    uint8_t sd = stream->stream_num;

    // Stop stream
    uint32_t ctl = hda_sd_read32(ctrl, sd, HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_STREAM_RUN;
    hda_sd_write32(ctrl, sd, HDA_SD_CTL, ctl);

    // Wait for stream to stop
    int timeout = 100;
    while (timeout > 0) {
        if (!(hda_sd_read32(ctrl, sd, HDA_SD_CTL) & HDA_SD_CTL_STREAM_RUN)) {
            break;
        }
        hda_msleep(1);
        timeout--;
    }

    // Reset stream
    ctl = hda_sd_read32(ctrl, sd, HDA_SD_CTL);
    ctl |= 0x00000001;  // Stream reset bit
    hda_sd_write32(ctrl, sd, HDA_SD_CTL, ctl);
    hda_msleep(1);

    // Wait for reset to complete
    timeout = 100;
    while (timeout > 0) {
        if (hda_sd_read32(ctrl, sd, HDA_SD_CTL) & 0x00000001) {
            break;
        }
        hda_msleep(1);
        timeout--;
    }

    // Clear reset
    ctl = hda_sd_read32(ctrl, sd, HDA_SD_CTL);
    ctl &= ~0x00000001;
    hda_sd_write32(ctrl, sd, HDA_SD_CTL, ctl);
    hda_msleep(1);

    // Set stream tag and channel
    ctl = hda_sd_read32(ctrl, sd, HDA_SD_CTL);
    ctl &= 0x0000FFFF;  // Clear upper bits
    ctl |= (stream->stream_tag << 20);  // Set stream tag
    hda_sd_write32(ctrl, sd, HDA_SD_CTL, ctl);

    /*
     * Memory barrier before programming BDL address registers.
     *
     * The stream DMA engine will begin fetching BDL entries as soon as the
     * stream RUN bit is set (below).  All BDL entry writes (address, length,
     * ioc) and the DMA buffer zeroing above must be visible in memory before
     * the controller reads the BDL base address registers.  The SFENCE closes
     * the WB→UC store-reorder window on x86, and the compiler barrier prevents
     * the compiler from sinking the BDL stores past the MMIO writes.
     */
    asm volatile("sfence" ::: "memory");

    // Set BDL base address
    uint64_t bdl_phys_addr = (uint64_t)stream->bdl_phys;
    hda_sd_write32(ctrl, sd, HDA_SD_BDPL, (uint32_t)bdl_phys_addr);
    hda_sd_write32(ctrl, sd, HDA_SD_BDPU, (uint32_t)(bdl_phys_addr >> 32));

    // Set cyclic buffer length (total size of all BDL entries)
    hda_sd_write32(ctrl, sd, HDA_SD_CBL, stream->buffer_size);

    // Set last valid index (number of BDL entries - 1)
    hda_sd_write16(ctrl, sd, HDA_SD_LVI, stream->bdl_entries - 1);

    // Set stream format
    hda_sd_write16(ctrl, sd, HDA_SD_FMT, stream->format);

    // Enable interrupts for stream
    ctl = hda_sd_read32(ctrl, sd, HDA_SD_CTL);
    ctl |= HDA_SD_CTL_INT_EN;
    hda_sd_write32(ctrl, sd, HDA_SD_CTL, ctl);

    serial_write("HDA: Stream setup complete (", 28);
    serial_putchar('0' + (sample_rate / 10000) % 10);
    serial_putchar('0' + (sample_rate / 1000) % 10);
    serial_write(" kHz, ", 6);
    serial_putchar('0' + (bits / 10) % 10);
    serial_putchar('0' + bits % 10);
    serial_write("-bit, ", 6);
    serial_putchar('0' + channels);
    serial_write(" ch)\n", 5);

    return 0;
}

/**
 * Start stream DMA
 */
int hda_stream_start(hda_controller_t* ctrl, hda_stream_t* stream) {
    if (!stream || stream->running) {
        return -1;
    }

    serial_write("HDA: Starting stream ", 21);
    serial_putchar('0' + stream->stream_num);
    serial_putchar('\n');

    uint8_t sd = stream->stream_num;

    // Clear status flags
    hda_sd_write8(ctrl, sd, HDA_SD_STS, HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);

    /*
     * Configure codec DAC BEFORE starting stream DMA.
     *
     * HDA spec §3.3.35: the converter format and stream/channel assignment
     * must be set while the stream is stopped.  If we set them after asserting
     * the RUN bit the DMA engine may have already started fetching samples
     * before the DAC knows the sample rate and channel layout, causing the
     * codec to output silence (or garbage) for the initial buffer period.
     *
     * Order:
     *   1. SET_CONVERTER_FORMAT (verb4 0x2): tell the DAC widget the exact
     *      PCM format.  Uses hda_send_verb4() because the 16-bit format word
     *      would be silently truncated to 8 bits by hda_send_command().
     *   2. SET_CONVERTER_STREAM_CHANNEL (verb 0x706): bind the DAC to this
     *      stream's tag and start at channel 0.
     *   3. Assert the stream RUN bit.
     */
    if (stream->is_output && ctrl->num_codecs > 0) {
        hda_codec_t* codec = ctrl->codecs[0];

        /*
         * SET_CONVERTER_FORMAT: 4-bit verb 0x2, 16-bit payload = format word.
         * Must use hda_send_verb4() — hda_send_command() encodes a 12-bit verb
         * with an 8-bit param, which truncates the 16-bit format payload and
         * causes the codec to receive a malformed verb, leaving the converter
         * in its reset-default format (usually 48 kHz 16-bit) regardless of
         * what was programmed into the stream descriptor format register.
         *
         * Format word layout (same bits as SDnFMT register):
         *   bits[3:0]  = channels - 1  (1 = stereo)
         *   bits[7:4]  = bits per sample selector (1 = 16-bit)
         *   bits[13:8] = sample-rate multiplier/divisor fields
         *   bit[14]    = base rate (0 = 48 kHz, 1 = 44.1 kHz)
         */
        hda_send_verb4(ctrl, codec->addr, codec->dac_nid,
                       HDA_VERB4_SET_CONVERTER_FORMAT,
                       (uint16_t)stream->format);

        /*
         * SET_CONVERTER_STREAM_CHANNEL: 12-bit verb 0x706, 8-bit payload.
         * Bits[7:4] = stream tag (must match stream_tag in SDnCTL bits[23:20]).
         * Bits[3:0] = first channel index in stream (0 = L, 1 = R for stereo).
         */
        hda_send_command(ctrl, codec->addr, codec->dac_nid,
                        HDA_VERB_SET_STREAM_CHANNEL,
                        (stream->stream_tag << 4) | 0);  /* channel 0 */
    }

    // Start stream DMA (after codec is configured)
    uint32_t ctl = hda_sd_read32(ctrl, sd, HDA_SD_CTL);
    ctl |= HDA_SD_CTL_STREAM_RUN;
    hda_sd_write32(ctrl, sd, HDA_SD_CTL, ctl);

    stream->running = true;
    stream->position = 0;

    serial_write("HDA: Stream started\n", 20);
    return 0;
}

/**
 * Stop stream DMA
 */
int hda_stream_stop(hda_controller_t* ctrl, hda_stream_t* stream) {
    if (!stream || !stream->running) {
        return -1;
    }

    serial_write("HDA: Stopping stream ", 21);
    serial_putchar('0' + stream->stream_num);
    serial_putchar('\n');

    uint8_t sd = stream->stream_num;

    // Stop stream
    uint32_t ctl = hda_sd_read32(ctrl, sd, HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_STREAM_RUN;
    hda_sd_write32(ctrl, sd, HDA_SD_CTL, ctl);

    // Wait for stream to stop
    int timeout = 100;
    while (timeout > 0) {
        if (!(hda_sd_read32(ctrl, sd, HDA_SD_CTL) & HDA_SD_CTL_STREAM_RUN)) {
            break;
        }
        hda_msleep(1);
        timeout--;
    }

    stream->running = false;

    serial_write("HDA: Stream stopped\n", 20);
    return 0;
}

/**
 * Write PCM data to stream buffer
 */
int hda_stream_write(hda_stream_t* stream, const void* data, uint32_t size) {
    if (!stream || !stream->buffer_virt) {
        return -1;
    }

    // Simple circular buffer write
    uint32_t written = 0;
    const uint8_t* src = (const uint8_t*)data;

    while (written < size) {
        if (stream->buffer_size == 0) break;                  // nothing to write into
        // Guard the unsigned subtraction below: if position somehow reached/passed
        // buffer_size on entry (stale/DMA-updated), (buffer_size - position) would
        // underflow to ~4GB of "space" -> writes far past the ring buffer.
        if (stream->position >= stream->buffer_size) stream->position = 0;
        uint32_t space = stream->buffer_size - stream->position;
        uint32_t to_write = (size - written < space) ? (size - written) : space;

        // Copy data
        for (uint32_t i = 0; i < to_write; i++) {
            ((uint8_t*)stream->buffer_virt)[stream->position + i] = src[written + i];
        }

        stream->position += to_write;
        written += to_write;

        // Wrap around
        if (stream->position >= stream->buffer_size) {
            stream->position = 0;
        }
    }

    return written;
}

/**
 * Read PCM data from stream buffer
 */
int hda_stream_read(hda_stream_t* stream, void* data, uint32_t size) {
    if (!stream || !stream->buffer_virt) {
        return -1;
    }

    // Simple circular buffer read
    uint32_t read = 0;
    uint8_t* dst = (uint8_t*)data;

    while (read < size) {
        uint32_t available = stream->buffer_size - stream->position;
        uint32_t to_read = (size - read < available) ? (size - read) : available;

        // Copy data
        for (uint32_t i = 0; i < to_read; i++) {
            dst[read + i] = ((uint8_t*)stream->buffer_virt)[stream->position + i];
        }

        stream->position += to_read;
        read += to_read;

        // Wrap around
        if (stream->position >= stream->buffer_size) {
            stream->position = 0;
        }
    }

    return read;
}

/**
 * Set output volume (0-100) on both the DAC and the pin widget output amps.
 *
 * Both amps must be unmuted with a sensible gain for audible output.  The pin
 * amp defaults to MUTED in QEMU's codec emulation, which is why naively only
 * updating the DAC amp yields silence.
 *
 * Uses the 4-bit verb SET_AMP_GAIN_MUTE (verb4=0x3) with a 16-bit payload:
 *   bit 15: set OUTPUT amp direction
 *   bit 13: apply to LEFT channel
 *   bit 12: apply to RIGHT channel
 *   bit 7:  MUTE (0 = unmuted)
 *   bits[6:0]: gain value (0x00 = min, 0x7F = max)
 */
int hda_set_volume(hda_codec_t* codec, hda_controller_t* ctrl, uint8_t volume) {
    if (!codec || volume > 100) {
        return -1;
    }

    /* Convert 0-100 percentage to 0-0x57 (87) — leave ~30% headroom
     * to avoid QEMU's software mixer clipping.  Use 0x7F for raw maximum. */
    uint8_t gain = (uint8_t)((volume * 0x57) / 100);

    /*
     * SET_AMP_GAIN_MUTE payload (16-bit):
     *   bit 15 = OUTPUT direction
     *   bit 13 = LEFT channel
     *   bit 12 = RIGHT channel
     *   bit 7  = MUTE (0 = unmuted)
     *   [6:0]  = gain
     */
    uint16_t amp_payload = (uint16_t)((1<<15) | (1<<13) | (1<<12) | gain);

    /* DAC (Audio Output widget) output amp */
    hda_send_verb4(ctrl, codec->addr, codec->dac_nid,
                   HDA_VERB4_SET_AMP_GAIN_MUTE, amp_payload);

    /* Pin widget output amp — this is what QEMU has muted by default! */
    if (codec->pin_nid) {
        hda_send_verb4(ctrl, codec->addr, codec->pin_nid,
                       HDA_VERB4_SET_AMP_GAIN_MUTE, amp_payload);
    }

    return 0;
}

/**
 * Mute/unmute output on both DAC and pin widget amps.
 */
int hda_set_mute(hda_codec_t* codec, hda_controller_t* ctrl, bool mute) {
    if (!codec) {
        return -1;
    }

    /*
     * For unmuted: gain = 0x57 (~68% — same as hda_set_volume default)
     * For muted:   bit 7 = 1 (MUTE bit), gain doesn't matter
     */
    uint16_t amp_payload = (uint16_t)((1<<15) | (1<<13) | (1<<12) | 0x57);
    if (mute) {
        amp_payload |= (1 << 7);  /* set MUTE bit */
    }

    /* DAC output amp */
    hda_send_verb4(ctrl, codec->addr, codec->dac_nid,
                   HDA_VERB4_SET_AMP_GAIN_MUTE, amp_payload);

    /* Pin widget output amp */
    if (codec->pin_nid) {
        hda_send_verb4(ctrl, codec->addr, codec->pin_nid,
                       HDA_VERB4_SET_AMP_GAIN_MUTE, amp_payload);
    }

    return 0;
}

/**
 * Interrupt handler for HDA
 */
void hda_irq_handler(void) {
    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl) {
        return;
    }

    // Read interrupt status
    uint32_t intsts = hda_read32(ctrl, HDA_REG_INTSTS);

    // Check for stream interrupts
    for (uint8_t i = 0; i < HDA_MAX_STREAMS; i++) {
        if (intsts & (1 << i)) {
            hda_stream_t* stream = ctrl->streams[i];
            if (stream && stream->running) {
                // Read and clear stream status
                uint8_t sts = hda_sd_read8(ctrl, i, HDA_SD_STS);
                hda_sd_write8(ctrl, i, HDA_SD_STS, sts);

                // Handle buffer completion interrupt
                if (sts & HDA_SD_STS_BCIS) {
                    // Buffer completed - could trigger a callback here
                }

                // Handle FIFO error
                if (sts & HDA_SD_STS_FIFOE) {
                    serial_write("HDA: FIFO error on stream ", 27);
                    serial_putchar('0' + i);
                    serial_putchar('\n');
                }

                // Handle descriptor error
                if (sts & HDA_SD_STS_DESE) {
                    serial_write("HDA: Descriptor error on stream ", 33);
                    serial_putchar('0' + i);
                    serial_putchar('\n');
                }
            }
        }
    }
}
