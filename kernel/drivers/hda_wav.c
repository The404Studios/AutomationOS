#include "../include/hda.h"
#include "../include/types.h"

/**
 * WAV File Format Support
 *
 * Simple WAV file parser and player for HDA driver testing.
 * Supports PCM WAV files (uncompressed audio).
 */

// WAV file header structures
typedef struct {
    char riff[4];           // "RIFF"
    uint32_t file_size;     // File size - 8
    char wave[4];           // "WAVE"
} __attribute__((packed)) wav_riff_header_t;

typedef struct {
    char fmt[4];            // "fmt "
    uint32_t chunk_size;    // Chunk size (16 for PCM)
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;  // 1 = mono, 2 = stereo
    uint32_t sample_rate;   // 44100, 48000, etc.
    uint32_t byte_rate;     // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align;   // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // 8, 16, 24, 32
} __attribute__((packed)) wav_fmt_chunk_t;

typedef struct {
    char data[4];           // "data"
    uint32_t data_size;     // Size of audio data
} __attribute__((packed)) wav_data_chunk_t;

/**
 * Parse WAV file header
 */
static int wav_parse_header(const void* wav_data, uint32_t wav_size,
                            uint32_t* sample_rate,
                            uint8_t* bits, uint8_t* channels,
                            const void** audio_data, uint32_t* audio_size) {
    const uint8_t* ptr = (const uint8_t*)wav_data;

    // Need at least the RIFF + fmt headers present before reading them; the
    // chunk-skip loop below bounds the rest against wav_size.
    if (wav_size < sizeof(wav_riff_header_t) + sizeof(wav_fmt_chunk_t)) {
        serial_write("WAV: too small\n", 15);
        return -1;
    }

    // Read RIFF header
    wav_riff_header_t* riff = (wav_riff_header_t*)ptr;
    if (riff->riff[0] != 'R' || riff->riff[1] != 'I' ||
        riff->riff[2] != 'F' || riff->riff[3] != 'F') {
        serial_write("WAV: Invalid RIFF header\n", 26);
        return -1;
    }

    if (riff->wave[0] != 'W' || riff->wave[1] != 'A' ||
        riff->wave[2] != 'V' || riff->wave[3] != 'E') {
        serial_write("WAV: Invalid WAVE header\n", 26);
        return -1;
    }

    ptr += sizeof(wav_riff_header_t);

    // Read fmt chunk
    wav_fmt_chunk_t* fmt = (wav_fmt_chunk_t*)ptr;
    if (fmt->fmt[0] != 'f' || fmt->fmt[1] != 'm' ||
        fmt->fmt[2] != 't' || fmt->fmt[3] != ' ') {
        serial_write("WAV: Invalid fmt chunk\n", 23);
        return -1;
    }

    if (fmt->audio_format != 1) {
        serial_write("WAV: Only PCM format supported\n", 32);
        return -1;
    }

    *sample_rate = fmt->sample_rate;
    *bits = fmt->bits_per_sample;
    *channels = fmt->num_channels;

    serial_write("WAV: Format - ", 14);
    serial_putchar('0' + (*sample_rate / 10000) % 10);
    serial_putchar('0' + (*sample_rate / 1000) % 10);
    serial_write(" kHz, ", 6);
    serial_putchar('0' + (*bits / 10) % 10);
    serial_putchar('0' + (*bits % 10));
    serial_write("-bit, ", 6);
    serial_putchar('0' + *channels);
    serial_write(" ch\n", 4);

    ptr += sizeof(wav_fmt_chunk_t);

    // Skip extra fmt data if present
    if (fmt->chunk_size > 16) {
        ptr += (fmt->chunk_size - 16);
    }

    // Find data chunk (may need to skip other chunks). Bound every access
    // against the real buffer size BEFORE dereferencing, and accumulate the
    // offset in 64-bit so a hostile chunk_size can't wrap the pointer past the
    // guard. (Previously: read-then-check against a fixed 1024 with wrappable
    // pointer math -> OOB read on a crafted/short file.)
    while (1) {
        uint64_t off = (uint64_t)(ptr - (const uint8_t*)wav_data);
        if (off + 8 > (uint64_t)wav_size) {
            serial_write("WAV: Data chunk not found\n", 27);
            return -1;
        }
        if (ptr[0] == 'd' && ptr[1] == 'a' && ptr[2] == 't' && ptr[3] == 'a') {
            break;
        }

        // Skip this chunk
        uint32_t chunk_size = *((const uint32_t*)(ptr + 4));
        ptr += 8 + (uint64_t)chunk_size;
    }

    // Read data chunk
    wav_data_chunk_t* data = (wav_data_chunk_t*)ptr;
    ptr += sizeof(wav_data_chunk_t);

    *audio_data = ptr;
    *audio_size = data->data_size;

    serial_write("WAV: Audio data size = ", 23);
    // Simple decimal output
    uint32_t size_kb = data->data_size / 1024;
    if (size_kb >= 1000) {
        serial_putchar('0' + (size_kb / 1000));
        size_kb %= 1000;
    }
    if (size_kb >= 100) {
        serial_putchar('0' + (size_kb / 100));
        size_kb %= 100;
    }
    if (size_kb >= 10) {
        serial_putchar('0' + (size_kb / 10));
        size_kb %= 10;
    }
    serial_putchar('0' + size_kb);
    serial_write(" KB\n", 4);

    return 0;
}

/**
 * Play a WAV file from memory
 */
int hda_play_wav(const void* wav_data, uint32_t wav_size) {
    serial_write("HDA: Playing WAV file\n", 22);

    // Get HDA controller
    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl) {
        serial_write("HDA: No controller found\n", 26);
        return -1;
    }

    if (ctrl->num_codecs == 0) {
        serial_write("HDA: No codecs available\n", 26);
        return -1;
    }

    hda_codec_t* codec = ctrl->codecs[0];

    // Parse WAV header
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t num_channels;
    const void* audio_data;
    uint32_t audio_size;

    if (wav_parse_header(wav_data, wav_size, &sample_rate, &bits_per_sample,
                         &num_channels, &audio_data, &audio_size) != 0) {
        return -1;
    }

    // Validate format
    if (bits_per_sample != 16) {
        serial_write("HDA: Only 16-bit audio supported\n", 34);
        return -1;
    }

    if (num_channels != 1 && num_channels != 2) {
        serial_write("HDA: Only mono/stereo supported\n", 33);
        return -1;
    }

    if (sample_rate != 44100 && sample_rate != 48000) {
        serial_write("HDA: Only 44.1/48 kHz supported\n", 33);
        return -1;
    }

    // Allocate output stream
    hda_stream_t* stream = hda_stream_alloc(ctrl, true);
    if (!stream) {
        serial_write("HDA: Failed to allocate stream\n", 32);
        return -1;
    }

    // Setup stream
    if (hda_stream_setup(ctrl, stream, sample_rate, bits_per_sample, num_channels) != 0) {
        serial_write("HDA: Failed to setup stream\n", 29);
        hda_stream_free(stream);
        return -1;
    }

    // Set volume
    hda_set_volume(codec, ctrl, 70);

    // Calculate playback duration
    uint32_t bytes_per_second = sample_rate * num_channels * (bits_per_sample / 8);
    uint32_t duration_ms = (audio_size * 1000) / bytes_per_second;

    serial_write("HDA: Duration = ", 16);
    serial_putchar('0' + (duration_ms / 1000) % 10);
    serial_putchar('.');
    serial_putchar('0' + (duration_ms / 100) % 10);
    serial_write(" seconds\n", 9);

    // Write audio data to stream buffer (in chunks if needed)
    uint32_t written = 0;
    while (written < audio_size) {
        uint32_t chunk_size = audio_size - written;
        if (chunk_size > stream->buffer_size) {
            chunk_size = stream->buffer_size;
        }

        hda_stream_write(stream, (uint8_t*)audio_data + written, chunk_size);
        written += chunk_size;

        // Start playback on first chunk
        if (written == chunk_size && !stream->running) {
            hda_stream_start(ctrl, stream);
            serial_write("HDA: Playback started\n", 22);
        }
    }

    // Wait for playback to complete
    serial_write("HDA: Waiting for playback to finish...\n", 40);

    // Simple busy wait (in real implementation, use interrupts)
    for (uint32_t i = 0; i < duration_ms; i++) {
        hlt();
    }

    // Stop playback
    hda_stream_stop(ctrl, stream);
    hda_stream_free(stream);

    serial_write("HDA: Playback complete\n", 24);
    return 0;
}

/**
 * Generate a simple WAV file in memory (for testing)
 */
void* hda_generate_test_wav(uint32_t* out_size) {
    // Generate 1 second of 440 Hz tone, 48 kHz, 16-bit, stereo

    uint32_t sample_rate = 48000;
    uint8_t bits_per_sample = 16;
    uint8_t num_channels = 2;
    uint32_t duration_samples = sample_rate;  // 1 second

    uint32_t audio_size = duration_samples * num_channels * (bits_per_sample / 8);
    uint32_t file_size = sizeof(wav_riff_header_t) + sizeof(wav_fmt_chunk_t) +
                         sizeof(wav_data_chunk_t) + audio_size;

    // Allocate buffer
    void* buffer = kmalloc(file_size);
    if (!buffer) {
        return NULL;
    }

    uint8_t* ptr = (uint8_t*)buffer;

    // Write RIFF header
    wav_riff_header_t* riff = (wav_riff_header_t*)ptr;
    riff->riff[0] = 'R'; riff->riff[1] = 'I';
    riff->riff[2] = 'F'; riff->riff[3] = 'F';
    riff->file_size = file_size - 8;
    riff->wave[0] = 'W'; riff->wave[1] = 'A';
    riff->wave[2] = 'V'; riff->wave[3] = 'E';
    ptr += sizeof(wav_riff_header_t);

    // Write fmt chunk
    wav_fmt_chunk_t* fmt = (wav_fmt_chunk_t*)ptr;
    fmt->fmt[0] = 'f'; fmt->fmt[1] = 'm';
    fmt->fmt[2] = 't'; fmt->fmt[3] = ' ';
    fmt->chunk_size = 16;
    fmt->audio_format = 1;  // PCM
    fmt->num_channels = num_channels;
    fmt->sample_rate = sample_rate;
    fmt->byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
    fmt->block_align = num_channels * (bits_per_sample / 8);
    fmt->bits_per_sample = bits_per_sample;
    ptr += sizeof(wav_fmt_chunk_t);

    // Write data chunk header
    wav_data_chunk_t* data = (wav_data_chunk_t*)ptr;
    data->data[0] = 'd'; data->data[1] = 'a';
    data->data[2] = 't'; data->data[3] = 'a';
    data->data_size = audio_size;
    ptr += sizeof(wav_data_chunk_t);

    // Generate sine wave audio data
    int16_t* samples = (int16_t*)ptr;
    for (uint32_t i = 0; i < duration_samples; i++) {
        // Simple sine approximation (same as in hda_test.c)
        uint64_t phase = ((uint64_t)i * 440 * 65536ULL) / sample_rate;
        phase = phase % 65536;

        int32_t value;
        if (phase < 16384) {
            value = (phase * 32767) / 16384;
        } else if (phase < 32768) {
            value = 32767 - ((phase - 16384) * 32767) / 16384;
        } else if (phase < 49152) {
            value = -((phase - 32768) * 32767) / 16384;
        } else {
            value = -32767 + ((phase - 49152) * 32767) / 16384;
        }

        // Apply fade in/out
        if (i < sample_rate / 20) {
            value = (value * i) / (sample_rate / 20);
        } else if (i > duration_samples - (sample_rate / 20)) {
            value = (value * (duration_samples - i)) / (sample_rate / 20);
        }

        samples[i * 2] = (int16_t)value;      // Left
        samples[i * 2 + 1] = (int16_t)value;  // Right
    }

    *out_size = file_size;
    return buffer;
}

/**
 * Test WAV playback
 */
void hda_test_wav_playback(void) {
    serial_write("HDA: Testing WAV playback\n", 27);

    // Generate test WAV file
    uint32_t wav_size;
    void* wav_data = hda_generate_test_wav(&wav_size);
    if (!wav_data) {
        serial_write("HDA: Failed to generate test WAV\n", 34);
        return;
    }

    serial_write("HDA: Generated test WAV file (", 31);
    serial_putchar('0' + (wav_size / 1000) % 10);
    serial_putchar('0' + (wav_size / 100) % 10);
    serial_putchar('0' + (wav_size / 10) % 10);
    serial_putchar('0' + (wav_size % 10));
    serial_write(" bytes)\n", 8);

    // Play WAV file
    if (hda_play_wav(wav_data, wav_size) != 0) {
        serial_write("HDA: WAV playback failed\n", 26);
    }

    // Free buffer
    kfree(wav_data);

    serial_write("HDA: WAV playback test complete\n", 33);
}

/**
 * Load and play WAV file from disk (requires filesystem)
 */
#if 0  // Disabled until filesystem is available
int hda_play_wav_file(const char* filename) {
    // Open file
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        serial_write("HDA: Failed to open WAV file\n", 30);
        return -1;
    }

    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    uint32_t file_size = st.st_size;

    // Allocate buffer
    void* buffer = kmalloc(file_size);
    if (!buffer) {
        close(fd);
        return -1;
    }

    // Read entire file
    if (read(fd, buffer, file_size) != (ssize_t)file_size) {
        kfree(buffer);
        close(fd);
        return -1;
    }

    close(fd);

    // Play WAV
    int result = hda_play_wav(buffer, file_size);

    kfree(buffer);
    return result;
}
#endif
