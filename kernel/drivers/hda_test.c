#include "../include/hda.h"
#include "../include/types.h"
#include "../include/x86_64.h"

// Simple sine wave generation for testing

/**
 * Generate a sine wave sample (16-bit)
 * Uses integer approximation of sine
 */
static int16_t sine_wave_sample(uint32_t sample_num, uint32_t frequency, uint32_t sample_rate) {
    // Simple sine approximation using integer math
    // phase = (sample_num * frequency * 65536) / sample_rate
    uint64_t phase = ((uint64_t)sample_num * frequency * 65536ULL) / sample_rate;
    phase = phase % 65536;  // Wrap to 0-65535

    // Convert phase to angle (0-360 degrees scaled to 0-65535)
    // Use simple triangle wave approximation for sine
    int32_t value;

    if (phase < 16384) {
        // 0 to 90 degrees: rising
        value = (phase * 32767) / 16384;
    } else if (phase < 32768) {
        // 90 to 180 degrees: falling
        value = 32767 - ((phase - 16384) * 32767) / 16384;
    } else if (phase < 49152) {
        // 180 to 270 degrees: falling negative
        value = -((phase - 32768) * 32767) / 16384;
    } else {
        // 270 to 360 degrees: rising to zero
        value = -32767 + ((phase - 49152) * 32767) / 16384;
    }

    return (int16_t)value;
}

/**
 * Test audio playback with a 440 Hz (A4) sine wave
 */
void hda_test_playback(void) {
    serial_write("HDA: Starting playback test\n", 29);

    // Get HDA controller
    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl) {
        serial_write("HDA: No controller found\n", 26);
        return;
    }

    // Check for codecs
    if (ctrl->num_codecs == 0) {
        serial_write("HDA: No codecs available\n", 26);
        return;
    }

    hda_codec_t* codec = ctrl->codecs[0];

    // Allocate output stream
    hda_stream_t* stream = hda_stream_alloc(ctrl, true);  // true = output
    if (!stream) {
        serial_write("HDA: Failed to allocate stream\n", 32);
        return;
    }

    // Setup stream: 48 kHz, 16-bit, stereo
    if (hda_stream_setup(ctrl, stream, 48000, 16, 2) != 0) {
        serial_write("HDA: Failed to setup stream\n", 29);
        hda_stream_free(stream);
        return;
    }

    // Generate 1 second of 440 Hz sine wave
    uint32_t sample_rate = 48000;
    uint32_t duration_samples = sample_rate;  // 1 second
    uint32_t frequency = 440;  // A4 note

    serial_write("HDA: Generating 440 Hz sine wave\n", 34);

    // Allocate temporary buffer for audio data
    uint32_t buffer_size = duration_samples * 2 * 2;  // 2 channels * 2 bytes per sample
    int16_t* audio_buffer = (int16_t*)kmalloc(buffer_size);
    if (!audio_buffer) {
        serial_write("HDA: Failed to allocate audio buffer\n", 38);
        hda_stream_free(stream);
        return;
    }

    // Generate sine wave (stereo)
    for (uint32_t i = 0; i < duration_samples; i++) {
        int16_t sample = sine_wave_sample(i, frequency, sample_rate);

        // Apply envelope to avoid clicks (fade in/out)
        if (i < sample_rate / 20) {
            // Fade in (50ms)
            sample = (sample * i) / (sample_rate / 20);
        } else if (i > duration_samples - (sample_rate / 20)) {
            // Fade out (50ms)
            sample = (sample * (duration_samples - i)) / (sample_rate / 20);
        }

        audio_buffer[i * 2] = sample;      // Left channel
        audio_buffer[i * 2 + 1] = sample;  // Right channel
    }

    // Write audio data to stream buffer
    hda_stream_write(stream, audio_buffer, buffer_size);

    // Free temporary buffer
    kfree(audio_buffer);

    // Set volume to 70%
    hda_set_volume(codec, ctrl, 70);

    // Start playback
    if (hda_stream_start(ctrl, stream) != 0) {
        serial_write("HDA: Failed to start stream\n", 29);
        hda_stream_free(stream);
        return;
    }

    serial_write("HDA: Playing 440 Hz tone for 1 second...\n", 42);

    // Wait for playback to complete (approximately)
    // In a real implementation, we'd wait for buffer completion interrupt
    for (uint32_t i = 0; i < 1000; i++) {
        hlt();  // Halt until next interrupt
    }

    // Stop playback
    hda_stream_stop(ctrl, stream);

    serial_write("HDA: Playback test complete\n", 29);

    // Free stream
    hda_stream_free(stream);
}

/**
 * Test multi-tone playback (chord)
 */
void hda_test_chord(void) {
    serial_write("HDA: Starting chord test\n", 26);

    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl || ctrl->num_codecs == 0) {
        return;
    }

    hda_codec_t* codec = ctrl->codecs[0];
    hda_stream_t* stream = hda_stream_alloc(ctrl, true);
    if (!stream) {
        return;
    }

    if (hda_stream_setup(ctrl, stream, 48000, 16, 2) != 0) {
        hda_stream_free(stream);
        return;
    }

    // Generate A major chord (A4=440, C#5=554, E5=659 Hz)
    uint32_t sample_rate = 48000;
    uint32_t duration_samples = sample_rate * 2;  // 2 seconds
    uint32_t buffer_size = duration_samples * 2 * 2;

    int16_t* audio_buffer = (int16_t*)kmalloc(buffer_size);
    if (!audio_buffer) {
        hda_stream_free(stream);
        return;
    }

    serial_write("HDA: Generating A major chord\n", 31);

    for (uint32_t i = 0; i < duration_samples; i++) {
        // Mix three frequencies
        int32_t sample = 0;
        sample += sine_wave_sample(i, 440, sample_rate) / 3;  // A4
        sample += sine_wave_sample(i, 554, sample_rate) / 3;  // C#5
        sample += sine_wave_sample(i, 659, sample_rate) / 3;  // E5

        // Envelope
        if (i < sample_rate / 20) {
            sample = (sample * i) / (sample_rate / 20);
        } else if (i > duration_samples - (sample_rate / 20)) {
            sample = (sample * (duration_samples - i)) / (sample_rate / 20);
        }

        audio_buffer[i * 2] = (int16_t)sample;
        audio_buffer[i * 2 + 1] = (int16_t)sample;
    }

    hda_stream_write(stream, audio_buffer, buffer_size);
    kfree(audio_buffer);

    hda_set_volume(codec, ctrl, 70);
    hda_stream_start(ctrl, stream);

    serial_write("HDA: Playing A major chord for 2 seconds...\n", 45);

    // Wait for playback
    for (uint32_t i = 0; i < 2000; i++) {
        hlt();
    }

    hda_stream_stop(ctrl, stream);
    hda_stream_free(stream);

    serial_write("HDA: Chord test complete\n", 26);
}

/**
 * Test volume control
 */
void hda_test_volume(void) {
    serial_write("HDA: Starting volume test\n", 27);

    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl || ctrl->num_codecs == 0) {
        return;
    }

    hda_codec_t* codec = ctrl->codecs[0];
    hda_stream_t* stream = hda_stream_alloc(ctrl, true);
    if (!stream) {
        return;
    }

    if (hda_stream_setup(ctrl, stream, 48000, 16, 2) != 0) {
        hda_stream_free(stream);
        return;
    }

    // Generate continuous tone
    uint32_t sample_rate = 48000;
    uint32_t buffer_size = stream->buffer_size;
    uint32_t num_samples = buffer_size / 4;  // 16-bit stereo

    int16_t* audio_buffer = (int16_t*)kmalloc(buffer_size);
    if (!audio_buffer) {
        hda_stream_free(stream);
        return;
    }

    // Generate 1 kHz tone
    for (uint32_t i = 0; i < num_samples; i++) {
        int16_t sample = sine_wave_sample(i, 1000, sample_rate);
        audio_buffer[i * 2] = sample;
        audio_buffer[i * 2 + 1] = sample;
    }

    hda_stream_write(stream, audio_buffer, buffer_size);
    kfree(audio_buffer);

    hda_stream_start(ctrl, stream);

    // Test different volume levels
    uint8_t volumes[] = {10, 30, 50, 70, 90, 100, 70, 50, 30, 10};

    for (uint8_t i = 0; i < 10; i++) {
        serial_write("HDA: Volume = ", 14);
        serial_putchar('0' + (volumes[i] / 10));
        serial_putchar('0' + (volumes[i] % 10));
        serial_write("%\n", 2);

        hda_set_volume(codec, ctrl, volumes[i]);

        // Wait 500ms
        for (uint32_t j = 0; j < 500; j++) {
            hlt();
        }
    }

    hda_stream_stop(ctrl, stream);
    hda_stream_free(stream);

    serial_write("HDA: Volume test complete\n", 27);
}

/**
 * Test mute functionality
 */
void hda_test_mute(void) {
    serial_write("HDA: Starting mute test\n", 25);

    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl || ctrl->num_codecs == 0) {
        return;
    }

    hda_codec_t* codec = ctrl->codecs[0];
    hda_stream_t* stream = hda_stream_alloc(ctrl, true);
    if (!stream) {
        return;
    }

    if (hda_stream_setup(ctrl, stream, 48000, 16, 2) != 0) {
        hda_stream_free(stream);
        return;
    }

    // Generate continuous tone
    uint32_t sample_rate = 48000;
    uint32_t buffer_size = stream->buffer_size;
    uint32_t num_samples = buffer_size / 4;

    int16_t* audio_buffer = (int16_t*)kmalloc(buffer_size);
    if (!audio_buffer) {
        hda_stream_free(stream);
        return;
    }

    for (uint32_t i = 0; i < num_samples; i++) {
        int16_t sample = sine_wave_sample(i, 880, sample_rate);  // A5
        audio_buffer[i * 2] = sample;
        audio_buffer[i * 2 + 1] = sample;
    }

    hda_stream_write(stream, audio_buffer, buffer_size);
    kfree(audio_buffer);

    hda_set_volume(codec, ctrl, 70);
    hda_stream_start(ctrl, stream);

    // Alternate mute/unmute
    for (uint8_t i = 0; i < 6; i++) {
        bool mute = (i % 2 == 0);
        serial_write(mute ? "HDA: MUTED\n" : "HDA: UNMUTED\n", mute ? 11 : 13);

        hda_set_mute(codec, ctrl, mute);

        // Wait 500ms
        for (uint32_t j = 0; j < 500; j++) {
            hlt();
        }
    }

    hda_stream_stop(ctrl, stream);
    hda_stream_free(stream);

    serial_write("HDA: Mute test complete\n", 25);
}

/**
 * Run all HDA tests
 */
void hda_run_tests(void) {
    serial_write("\n========== HDA AUDIO TESTS ==========\n\n", 41);

    // Initialize HDA if not already done
    if (!hda_find_controller()) {
        hda_init();
    }

    if (!hda_find_controller()) {
        serial_write("HDA: Cannot run tests - no controller\n", 39);
        return;
    }

    // Run tests
    hda_test_playback();
    serial_write("\n", 1);

    hda_test_chord();
    serial_write("\n", 1);

    hda_test_volume();
    serial_write("\n", 1);

    hda_test_mute();

    serial_write("\n========== ALL TESTS COMPLETE ==========\n\n", 44);
}
