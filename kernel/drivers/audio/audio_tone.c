/**
 * Audio Tone Playback - minimal kernel audio API
 * ==============================================
 *
 * Provides the minimal kernel-facing audio entry points requested by the
 * integrator:
 *
 *     int audio_play_tone(uint32_t freq_hz, uint32_t ms);
 *     int audio_play_pcm(const int16_t* buf, size_t frames, uint32_t rate);
 *
 * These sit directly on top of the existing HDA controller / codec / stream
 * driver (kernel/drivers/hda.c, hda_stream.c). The HDA controller must already
 * be brought up via hda_init() (which PCI-scans, resets the controller, sets up
 * CORB/RIRB, enumerates the codec and configures a DAC->pin output path).
 *
 * Playback model
 * --------------
 * The HDA DMA engine plays the BDL cyclically. For a fixed-duration tone we:
 *   1. fill the (identity-mapped) DMA buffer with one or more periods of a
 *      48 kHz / 16-bit / stereo sine wave at the requested frequency,
 *   2. start the stream (DMA loops over the buffer),
 *   3. busy/HLT-sleep for the requested number of milliseconds,
 *   4. stop the stream.
 *
 * This is intentionally simple and synchronous: it blocks the caller for `ms`
 * milliseconds. It is fine for a startup chime or a userspace SYS_BEEP.
 */

#include "../../include/hda.h"
#include "../../include/audio.h"
#include "../../include/mem.h"
#include "../../include/x86_64.h"
#include "../../include/drivers.h"   // serial_*, timer_*, timer_sleep
#include "../../include/types.h"

/* hda_msleep lives in hda.c; reuse it for consistent timing. */
extern void hda_msleep(uint32_t ms);

/* A reusable output stream for tone/PCM playback. Allocated lazily on first
 * use and kept around (HDA streams are a scarce hardware resource). */
static hda_stream_t* g_tone_stream = NULL;

/* Fixed-point sine table: 256 entries, one full period, range [-32767,32767].
 * Generated with a 5th-order minimax-ish polynomial at build-relevant precision
 * is overkill here; instead we synthesise the table at init with a small CORDIC-
 * free Taylor/Bhaskara approximation. To keep this freestanding and table-free
 * at file scope, we compute samples on the fly with an integer sine helper. */

/*
 * Integer sine approximation, Bhaskara I's formula.
 * Input:  angle in "brads" 0..65535 representing 0..2*pi.
 * Output: value in [-32767, 32767].
 *
 * Bhaskara I (for 0..180 deg, x in degrees):
 *   sin(x) ~= 16x(180 - x) / (40500 - x(180 - x))
 * We scale to brad input and mirror for the negative half.
 */
static int32_t isin(uint32_t brad) {
    brad &= 0xFFFF;
    int neg = 0;
    if (brad >= 32768) {       /* second half of the circle -> negative */
        brad -= 32768;
        neg = 1;
    }
    /* Map 0..32767 (half period) to degrees 0..180 (scaled by 256). */
    /* deg256 = brad * 180 / 32768 * 256 == brad * 180 / 128 */
    uint32_t deg = (brad * 180u) / 32768u;   /* 0..180 */
    uint32_t t = deg * (180u - deg);         /* x(180-x), 0..8100 */
    /* Bhaskara: 4*t / (40500 - t) gives 0..1, scale to 32767. */
    int32_t num = (int32_t)(4u * t) * 32767;
    int32_t den = (int32_t)(40500u - t);
    int32_t v = den ? (num / den) : 0;
    if (v > 32767) v = 32767;
    if (neg) v = -v;
    return v;
}

/*
 * Ensure we have a configured 48 kHz / 16-bit / stereo output stream.
 * Returns the controller via *out_ctrl and the codec via *out_codec.
 */
static int tone_prepare_stream(hda_controller_t** out_ctrl,
                               hda_codec_t** out_codec) {
    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl) {
        serial_write("AUDIO: tone: no HDA controller (call hda_init first)\n", 53);
        return -1;
    }
    if (ctrl->num_codecs == 0) {
        serial_write("AUDIO: tone: no codec available\n", 33);
        return -1;
    }
    hda_codec_t* codec = ctrl->codecs[0];

    if (!g_tone_stream) {
        g_tone_stream = hda_stream_alloc(ctrl, true /* output */);
        if (!g_tone_stream) {
            serial_write("AUDIO: tone: failed to allocate stream\n", 40);
            return -1;
        }
    }

    /* (Re)configure format. hda_stream_setup also (re)programs the BDL and the
     * stream-descriptor registers and is safe to call repeatedly. */
    if (hda_stream_setup(ctrl, g_tone_stream, 48000, 16, 2) != 0) {
        serial_write("AUDIO: tone: stream setup failed\n", 34);
        return -1;
    }

    *out_ctrl = ctrl;
    *out_codec = codec;
    return 0;
}

/*
 * Fill the stream's DMA buffer with a continuous sine tone.
 * 48 kHz, 16-bit signed, 2 channels (interleaved L,R).
 * The buffer is played cyclically by the DMA engine, so we fill the WHOLE
 * buffer with whole-ish periods to minimise the discontinuity at wrap.
 */
static void tone_fill_buffer(hda_stream_t* stream, uint32_t freq_hz) {
    if (!stream || !stream->buffer_virt) return;

    int16_t* buf = (int16_t*)stream->buffer_virt;
    uint32_t total_samples = stream->buffer_size / 2;     /* int16 units    */
    uint32_t frames = total_samples / 2;                  /* L+R pairs      */
    const uint32_t rate = 48000;

    if (freq_hz == 0) freq_hz = 440;

    /* phase increment per frame in brads (0..65535 == one period) */
    /* inc = freq * 65536 / rate */
    uint32_t inc = (uint32_t)(((uint64_t)freq_hz * 65536ULL) / rate);
    uint32_t phase = 0;

    /* short fade in/out (~5ms) to avoid an audible click at start/stop */
    uint32_t fade = rate / 200;            /* 5 ms worth of frames */
    if (fade > frames / 4) fade = frames / 4;

    for (uint32_t f = 0; f < frames; f++) {
        int32_t s = isin(phase);
        phase += inc;

        /* fade envelope */
        if (f < fade) {
            s = (s * (int32_t)f) / (int32_t)fade;
        } else if (f >= frames - fade) {
            s = (s * (int32_t)(frames - f)) / (int32_t)fade;
        }

        /* ~50% amplitude to be gentle */
        s = s / 2;

        buf[f * 2 + 0] = (int16_t)s;   /* left  */
        buf[f * 2 + 1] = (int16_t)s;   /* right */
    }
}

/**
 * audio_beep - primary public API for SYS_BEEP=45.
 *
 * Play a sine tone at `freq_hz` Hz for `duration_ms` milliseconds.
 * Synchronous; safe to call from syscall context.  Returns 0 on success,
 * negative if no HDA device is present (never hangs/panics on absent HW).
 */
int audio_beep(uint32_t freq_hz, uint32_t duration_ms) {
    return audio_play_tone(freq_hz, duration_ms);
}

/**
 * audio_play_tone - play a sine tone of `freq_hz` for `ms` milliseconds.
 *
 * Synchronous: blocks the caller for the duration. Returns 0 on success,
 * negative on error.
 */
int audio_play_tone(uint32_t freq_hz, uint32_t ms) {
    hda_controller_t* ctrl = NULL;
    hda_codec_t* codec = NULL;

    if (tone_prepare_stream(&ctrl, &codec) != 0) {
        return -1;
    }

    /* Reset write position and fill the whole DMA buffer with the tone. */
    g_tone_stream->position = 0;
    tone_fill_buffer(g_tone_stream, freq_hz);

    /* Ensure the codec output is at a sensible volume and unmuted. */
    hda_set_volume(codec, ctrl, 80);
    hda_set_mute(codec, ctrl, false);

    serial_write("AUDIO: playing tone\n", 20);

    if (hda_stream_start(ctrl, g_tone_stream) != 0) {
        serial_write("AUDIO: tone: stream start failed\n", 34);
        return -1;
    }

    /* Block for the requested duration while DMA loops the buffer. */
    if (ms == 0) ms = 200;
    hda_msleep(ms);

    hda_stream_stop(ctrl, g_tone_stream);

    serial_write("AUDIO: tone done\n", 17);
    return 0;
}

/**
 * audio_play_pcm - play raw interleaved 16-bit stereo PCM.
 *
 * @buf:    pointer to interleaved L,R int16 samples
 * @frames: number of stereo frames (one frame == one L + one R sample)
 * @rate:   sample rate in Hz (44100 or 48000)
 *
 * Copies up to one DMA-buffer worth of audio and plays it once. For audio
 * longer than the DMA buffer, call repeatedly (or use the streaming
 * audio_write() path in audio_core.c). Returns frames queued, or negative.
 */
int audio_play_pcm(const int16_t* buf, size_t frames, uint32_t rate) {
    hda_controller_t* ctrl = NULL;
    hda_codec_t* codec = NULL;

    if (!buf || frames == 0) return -1;
    if (rate != 44100 && rate != 48000) rate = 48000;

    ctrl = hda_find_controller();
    if (!ctrl || ctrl->num_codecs == 0) return -1;
    codec = ctrl->codecs[0];

    if (!g_tone_stream) {
        g_tone_stream = hda_stream_alloc(ctrl, true);
        if (!g_tone_stream) return -1;
    }
    if (hda_stream_setup(ctrl, g_tone_stream, rate, 16, 2) != 0) {
        return -1;
    }

    /* Clamp to the DMA buffer capacity. */
    size_t max_frames = g_tone_stream->buffer_size / 4;  /* 16-bit stereo */
    size_t play_frames = (frames < max_frames) ? frames : max_frames;
    uint32_t bytes = (uint32_t)(play_frames * 4);

    g_tone_stream->position = 0;
    /* Zero the whole buffer first so the cyclic tail is silence. */
    for (uint32_t i = 0; i < g_tone_stream->buffer_size; i++) {
        ((uint8_t*)g_tone_stream->buffer_virt)[i] = 0;
    }
    hda_stream_write(g_tone_stream, buf, bytes);

    hda_set_volume(codec, ctrl, 80);
    hda_set_mute(codec, ctrl, false);

    if (hda_stream_start(ctrl, g_tone_stream) != 0) return -1;

    /* Block for the duration of the queued audio. */
    uint32_t ms = (uint32_t)((play_frames * 1000ULL) / rate);
    if (ms == 0) ms = 1;
    hda_msleep(ms);

    hda_stream_stop(ctrl, g_tone_stream);
    return (int)play_frames;
}

/**
 * audio_startup_beep - convenience: bring up HDA (if needed) and emit a short
 * boot chime. Safe to call from kernel init after PCI is initialised.
 *
 * Returns 0 if a tone was played, negative if no audio hardware was found.
 */
int audio_startup_beep(void) {
    if (!hda_find_controller()) {
        hda_init();    /* PCI-scan, reset, CORB/RIRB, enumerate codec */
    }
    if (!hda_find_controller()) {
        serial_write("AUDIO: startup beep skipped (no HDA)\n", 38);
        return -1;
    }
    /* Two quick rising notes: A5 then E6. */
    audio_play_tone(880, 150);
    audio_play_tone(1318, 150);
    return 0;
}
