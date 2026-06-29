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
#include "../../include/amix.h"      // amix_mix_period (mixer -> HDA DMA proof)

/* hda_msleep lives in hda.c; reuse it for consistent timing. */
extern void hda_msleep(uint32_t ms);

/* A reusable output stream for tone/PCM playback. Allocated lazily on first
 * use and kept around (HDA streams are a scarce hardware resource). */
static hda_stream_t* g_tone_stream = NULL;

/*
 * hda_active_lpib - read the active tone stream's Link Position In Buffer.
 *
 * SD_LPIB (HDA spec §3.3.39) is the byte offset the stream DMA engine has
 * consumed within the cyclic buffer.  Reading it before and after playback and
 * comparing the two values proves the DMA engine moved -- the most direct,
 * interrupt-independent evidence that audio actually streamed out the link.
 * Returns 0 if no tone stream is currently allocated.
 */
uint32_t hda_active_lpib(void) {
    hda_controller_t* ctrl = hda_find_controller();
    if (!ctrl || !g_tone_stream) return 0;
    return hda_sd_read32(ctrl, g_tone_stream->stream_num, HDA_SD_LPIB);
}

/* Helper used by the AUDIO-VERIFY marker: print a small unsigned decimal to
 * the serial console (no printf in this freestanding TU). */
static void tone_serial_u32(uint32_t v) {
    char tmp[12];
    int i = 0;
    if (v == 0) {
        serial_putchar('0');
        return;
    }
    while (v && i < 11) {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        serial_putchar(tmp[--i]);
    }
}

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

    /* AUDIO-VERIFY: snapshot the DMA link position right after the stream is
     * running, before we sleep.  After the duration we read it again; the delta
     * is how many bytes the DMA engine consumed.  Either a non-zero BCIS count
     * (real interrupts serviced) OR a non-zero LPIB delta (real DMA movement)
     * proves audio actually played -- not just that the registers were poked. */
    uint32_t lpib_before = hda_sd_read32(ctrl, g_tone_stream->stream_num, HDA_SD_LPIB);
    uint32_t bcis_before = g_hda_bcis;

    /* Block for the requested duration while DMA loops the buffer. */
    if (ms == 0) ms = 200;
    hda_msleep(ms);

    /* Read LPIB while the stream is STILL running (stopping resets it). */
    uint32_t lpib_after = hda_sd_read32(ctrl, g_tone_stream->stream_num, HDA_SD_LPIB);
    uint32_t lpib_delta = lpib_after - lpib_before;  /* wraps cleanly mod 2^32 */
    uint32_t bcis_total = g_hda_bcis;
    (void)bcis_before;

    hda_stream_stop(ctrl, g_tone_stream);

    /* The proof marker the headless verify asserts on:
     *   "AUDIO: tone done bcis=<N> lpib_adv=<D>"
     * N>0 OR D>0 == real DMA playback happened. */
    serial_write("AUDIO: tone done bcis=", 22);
    tone_serial_u32(bcis_total);
    serial_write(" lpib_adv=", 10);
    tone_serial_u32(lpib_delta);
    serial_putchar('\n');
    return 0;
}

/* ====================================================================== *
 *  AUDIO-MIXER end-to-end proof: play TWO tones SUMMED by the real software
 *  mixer (amix_mix_period) through the HDA DMA engine -- proving concurrent
 *  streams reach real audio hardware, not just a RAM-buffer KAT.
 * ====================================================================== */
#define MIX_CHUNK 256
static int16_t g_mix_a[MIX_CHUNK * 2];   /* source A, interleaved stereo s16 */
static int16_t g_mix_b[MIX_CHUNK * 2];   /* source B                          */

static void tone_fill_buffer_mixed(hda_stream_t* stream, uint32_t f1, uint32_t f2) {
    if (!stream || !stream->buffer_virt) return;
    int16_t* dst = (int16_t*)stream->buffer_virt;
    uint32_t frames = (stream->buffer_size / 2) / 2;     /* L+R pairs */
    const uint32_t rate = 48000;
    if (f1 == 0) f1 = 440;
    if (f2 == 0) f2 = 660;
    uint32_t inc1 = (uint32_t)(((uint64_t)f1 * 65536ULL) / rate);
    uint32_t inc2 = (uint32_t)(((uint64_t)f2 * 65536ULL) / rate);
    uint32_t ph1 = 0, ph2 = 0;
    for (uint32_t off = 0; off < frames; off += MIX_CHUNK) {
        uint32_t n = frames - off; if (n > MIX_CHUNK) n = MIX_CHUNK;
        for (uint32_t i = 0; i < n; i++) {
            int32_t a = isin(ph1) / 3; ph1 += inc1;   /* ~1/3 each: headroom for the sum */
            int32_t b = isin(ph2) / 3; ph2 += inc2;
            g_mix_a[i*2+0] = (int16_t)a; g_mix_a[i*2+1] = (int16_t)a;
            g_mix_b[i*2+0] = (int16_t)b; g_mix_b[i*2+1] = (int16_t)b;
        }
        amix_stream_t st[2] = {
            { g_mix_a, n, 256, 1 },
            { g_mix_b, n, 256, 1 },
        };
        amix_mix_period(st, 2, 256, dst + (uint64_t)off * 2, n);
    }
}

/**
 * audio_play_mixed - play two tones summed by the AUDIO-MIXER through HDA DMA.
 * Proves the mixer output reaches real hardware via the bcis/lpib DMA marker.
 */
int audio_play_mixed(uint32_t f1, uint32_t f2, uint32_t ms) {
    hda_controller_t* ctrl = NULL;
    hda_codec_t* codec = NULL;
    if (tone_prepare_stream(&ctrl, &codec) != 0) return -1;

    g_tone_stream->position = 0;
    tone_fill_buffer_mixed(g_tone_stream, f1, f2);
    hda_set_volume(codec, ctrl, 80);
    hda_set_mute(codec, ctrl, false);

    serial_write("AUDIO: playing mixed\n", 21);
    if (hda_stream_start(ctrl, g_tone_stream) != 0) {
        serial_write("AUDIO: mixed: stream start failed\n", 34);
        return -1;
    }
    uint32_t lpib_before = hda_sd_read32(ctrl, g_tone_stream->stream_num, HDA_SD_LPIB);
    if (ms == 0) ms = 200;
    hda_msleep(ms);
    uint32_t lpib_after = hda_sd_read32(ctrl, g_tone_stream->stream_num, HDA_SD_LPIB);
    uint32_t lpib_delta = lpib_after - lpib_before;
    uint32_t bcis_total = g_hda_bcis;
    hda_stream_stop(ctrl, g_tone_stream);

    serial_write("AUDIO: mixed done bcis=", 23);
    tone_serial_u32(bcis_total);
    serial_write(" lpib_adv=", 10);
    tone_serial_u32(lpib_delta);
    serial_putchar('\n');
    return 0;
}

#ifdef HDA_ENABLE
/* ====================================================================== *
 *  AUDIO B1: on_bcis gapless refill -- the streaming keystone.
 *
 *  The DMA plays the 8x8KB BDL cyclically, raising one BCIS per chunk. The IRQ
 *  handler (hda_stream.c) calls refill_cb for the just-completed chunk; we fill
 *  it with the NEXT slice of a 48kHz/16-bit/stereo sine whose phase PERSISTS
 *  across chunks. So the played waveform is continuous far beyond the 64 KB
 *  buffer -- genuine gapless streaming, not a looped buffer. This is the B0->B1
 *  mechanism a userspace SYS_AUDIO_STREAM_WRITE will later feed from a ring.
 * ====================================================================== */
static volatile uint32_t g_stream_phase = 0;   /* brad phase (IRQ-written)      */
static uint32_t g_stream_inc = 0;              /* per-frame phase increment     */

/* Runs in the HDA IRQ on each BCIS. Bounded fill + sfence only -- no alloc,
 * no codec verbs, no locks. */
static void stream_refill_sine(void* sptr, uint32_t chunk_idx) {
    hda_stream_t* stream = (hda_stream_t*)sptr;
    if (!stream || !stream->buffer_virt || stream->chunk_size == 0) return;
    int16_t* dst = (int16_t*)((uint8_t*)stream->buffer_virt + chunk_idx * stream->chunk_size);
    uint32_t frames = stream->chunk_size / 4;        /* 16-bit stereo = 4 B/frame */
    uint32_t ph = g_stream_phase;
    for (uint32_t f = 0; f < frames; f++) {
        int32_t s = isin(ph) / 2;                    /* ~50% amplitude            */
        ph += g_stream_inc;
        dst[f*2+0] = (int16_t)s;
        dst[f*2+1] = (int16_t)s;
    }
    g_stream_phase = ph;
    asm volatile("sfence" ::: "memory");             /* visible before DMA laps it */
}

/**
 * audio_stream_selftest - prove on_bcis gapless refill.
 *
 * Streams a continuous sine via the per-chunk on_bcis refill for long enough to
 * cross multiple buffer cycles (>8 refills == >1 full 64 KB cycle, so the refill
 * fired repeatedly across a wrap). Marker:
 *   "AUDIO: stream done bcis=<delta> refills=<R> lpib_adv=<D>"
 * PASS = R>8 (gapless refill ran across a wrap) AND D>0 (DMA actually moved).
 */
int audio_stream_selftest(void) {
    hda_controller_t* ctrl = NULL;
    hda_codec_t* codec = NULL;
    if (tone_prepare_stream(&ctrl, &codec) != 0) return -1;

    /* 440 Hz continuous sine */
    g_stream_inc   = (uint32_t)(((uint64_t)440 * 65536ULL) / 48000ULL);
    g_stream_phase = 0;

    /* Prefill every chunk once so the first cycle plays real audio before any
     * chunk's first on_bcis refill. */
    g_tone_stream->position     = 0;
    for (uint32_t i = 0; i < g_tone_stream->bdl_entries; i++)
        stream_refill_sine(g_tone_stream, i);
    g_tone_stream->refill_next  = 0;
    g_tone_stream->refill_count = 0;

    hda_set_volume(codec, ctrl, 80);
    hda_set_mute(codec, ctrl, false);

    uint32_t bcis_before = g_hda_bcis;
    g_tone_stream->refill_cb = stream_refill_sine;   /* arm BEFORE start */
    serial_write("AUDIO: streaming (gapless on_bcis refill)\n", 42);
    if (hda_stream_start(ctrl, g_tone_stream) != 0) {
        g_tone_stream->refill_cb = 0;
        serial_write("AUDIO: stream: start failed\n", 28);
        return -1;
    }

    uint32_t lpib_before = hda_sd_read32(ctrl, g_tone_stream->stream_num, HDA_SD_LPIB);
    hda_msleep(600);                                  /* ~14 chunk completions */
    uint32_t lpib_after  = hda_sd_read32(ctrl, g_tone_stream->stream_num, HDA_SD_LPIB);
    uint32_t lpib_delta  = lpib_after - lpib_before;
    uint32_t bcis_delta  = g_hda_bcis - bcis_before;
    uint32_t refills     = g_tone_stream->refill_count;

    g_tone_stream->refill_cb = 0;                     /* disarm before stop */
    hda_stream_stop(ctrl, g_tone_stream);

    serial_write("AUDIO: stream done bcis=", 24);
    tone_serial_u32(bcis_delta);
    serial_write(" refills=", 9);
    tone_serial_u32(refills);
    serial_write(" lpib_adv=", 10);
    tone_serial_u32(lpib_delta);
    serial_putchar('\n');
    return 0;
}

/* ====================================================================== *
 *  AUDIO B1 part-2: SYS_AUDIO_STREAM_WRITE -- userspace PCM streaming.
 *
 *  A single-producer/single-consumer software ring feeds the SAME on_bcis hook
 *  as part-1, but from ring-3 PCM instead of a kernel sine generator:
 *    producer = audio_stream_write (task ctx, IF=0, owns ring_tail)
 *    consumer = stream_refill_ring (on_bcis IRQ ctx, owns ring_head)
 *  Both use the shared g_tone_stream (sequential with tone/mixed/sine-selftest,
 *  never concurrent). Race-safe on the single-core default (volatile aligned
 *  cursors, single-writer-each, sfence-before-publish; see the part-2 audit).
 * ====================================================================== */
#define HDA_RING_SIZE (16 * 4096)        /* 64KB == 8 chunks; pmm_alloc_pages(16) */

/* CONSUMER (IRQ ctx): pop chunk_size bytes ring->DMA chunk; zero-fill + count an
 * underrun if the ring has < a chunk. Pure: bounded copy + sfence, no alloc/verb/
 * lock/serial/yield (the on_bcis contract). */
static void stream_refill_ring(void* sptr, uint32_t chunk_idx) {
    hda_stream_t* s = (hda_stream_t*)sptr;
    if (!s || !s->buffer_virt || !s->ring || s->chunk_size == 0) return;
    uint8_t* dst = (uint8_t*)s->buffer_virt + chunk_idx * s->chunk_size;
    uint32_t head = s->ring_head;                 /* consumer owns        */
    uint32_t tail = s->ring_tail;                 /* snapshot producer    */
    uint32_t avail = (tail - head + HDA_RING_SIZE) % HDA_RING_SIZE;
    uint32_t cs = s->chunk_size;
    uint32_t pop = (avail < cs) ? avail : cs;
    for (uint32_t i = 0; i < pop; i++) {
        dst[i] = s->ring[head];
        head++; if (head >= HDA_RING_SIZE) head = 0;
    }
    for (uint32_t i = pop; i < cs; i++) dst[i] = 0;   /* underrun -> silence */
    if (pop < cs) s->underruns++;
    asm volatile("sfence" ::: "memory");          /* visible before DMA laps it */
    s->ring_head = head;                          /* publish (single store) */
}

/* PRODUCER (task ctx, IF=0): push PCM into the ring with back-pressure. Arms the
 * stream on first call (refill_cb set LAST, before start). NON-BLOCKING: returns
 * bytes accepted (may be < len when the ring is full); never waits. */
int audio_stream_write(const void* data, uint32_t len) {
    if (!data || len == 0) return 0;

    /* (Re)arm streaming if not already running the ring consumer. */
    if (!g_tone_stream || !g_tone_stream->running ||
        g_tone_stream->refill_cb != stream_refill_ring) {
        hda_controller_t* ctrl = NULL; hda_codec_t* codec = NULL;
        if (tone_prepare_stream(&ctrl, &codec) != 0) return -1;   /* alloc+setup */
        hda_stream_t* s = g_tone_stream;
        if (!s->ring) {
            s->ring = (uint8_t*)pmm_alloc_pages(16);              /* 64KB ring */
            if (!s->ring) return -1;
        }
        s->ring_head = s->ring_tail = 0; s->underruns = 0;
        s->refill_next = 0; s->refill_count = 0;
        /* zero the DMA buffer so the initial laps (before ring data propagates)
         * play silence, not stale bytes. */
        for (uint32_t i = 0; i < s->buffer_size; i++)
            ((uint8_t*)s->buffer_virt)[i] = 0;
        hda_set_volume(codec, ctrl, 80);
        hda_set_mute(codec, ctrl, false);
        s->refill_cb = stream_refill_ring;                        /* arm LAST */
        if (hda_stream_start(ctrl, s) != 0) { s->refill_cb = 0; return -1; }
    }

    hda_stream_t* s = g_tone_stream;
    const uint8_t* src = (const uint8_t*)data;
    uint32_t head = s->ring_head;                 /* snapshot consumer    */
    uint32_t tail = s->ring_tail;                 /* producer owns        */
    uint32_t count = (tail - head + HDA_RING_SIZE) % HDA_RING_SIZE;
    uint32_t freeb = (HDA_RING_SIZE - 1) - count; /* reserve 1 (full vs empty) */
    uint32_t n = (len < freeb) ? len : freeb;     /* BACK-PRESSURE: partial   */
    n &= ~3u;   /* AUDIO-PARTIAL-FIX: keep the accepted count a whole 4-byte stereo
                 * frame so ring_tail never lands mid-frame -- a non-frame-aligned
                 * partial would permanently shift L/R for the rest of the stream */
    for (uint32_t i = 0; i < n; i++) {
        s->ring[tail] = src[i];
        tail++; if (tail >= HDA_RING_SIZE) tail = 0;
    }
    asm volatile("sfence" ::: "memory");          /* payload visible before... */
    s->ring_tail = tail;                          /* ...publishing the cursor  */
    return (int)n;
}

/* Read the streaming underrun count (0 if not streaming) -- for diagnostics. */
uint32_t audio_stream_underruns(void) {
    return (g_tone_stream && g_tone_stream->refill_cb == stream_refill_ring)
           ? g_tone_stream->underruns : 0;
}
#endif /* HDA_ENABLE */

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
