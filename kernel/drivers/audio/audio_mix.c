/*
 * audio_mix.c -- AUDIO-MIXER core: sum N client PCM streams into one period.
 * =======================================================================
 *
 * The heart of concurrent playback (BoredOS has ZERO audio -> absolute-bar 5x):
 * up to AMIX_MAX_STREAMS interleaved stereo-s16 streams are summed with a
 * per-stream Q8 gain and a master Q8 gain, accumulated in int64, then clamped
 * to int16 (no wraparound). Heap-free, FPU-free, deterministic.
 *
 * Original by construction: a plain int-sum/clamp over OUR amix_stream_t -- not
 * PulseAudio/PipeWire. Sources at other rates/formats are reconciled by the
 * AUDIO-FMT converter (audio_convert) BEFORE they reach the mixer; this stage
 * assumes the canonical stream format (stereo s16).
 *
 * This brick is the mixing CORE + KAT. The daemon that serves N clients over the
 * SYS_CH_* capability rail + SysV SHM (NOT a /tmp/*.sock -- the orphaned
 * audiomixerd.c socket is to be deleted per the charter) and the gapless HDA
 * on_bcis refill are the AUDIO-MIXER-daemon follow-on.
 *
 * Scope: kernel/drivers/audio/audio_mix.c (new).
 */
#include "../../include/types.h"
#include "../../include/kernel.h"   /* kprintf (selftest only) */

#define AMIX_MAX_STREAMS 8

typedef struct {
    const int16_t* buf;   /* interleaved stereo s16 (frame f -> buf[f*2 + ch]) */
    uint32_t       frames;/* frames available in buf                           */
    uint16_t       gain_q8;/* per-stream gain, 256 = unity                     */
    uint8_t        active; /* 0 = skip                                         */
} amix_stream_t;

/*
 * Mix `nstreams` stereo-s16 streams into `out` (frames * 2 samples), applying
 * per-stream gain then master gain, summing in int64, clamping to int16.
 * A stream shorter than `frames` contributes silence past its end. Returns the
 * number of frames written.
 */
int amix_mix_period(const amix_stream_t* streams, int nstreams,
                    uint16_t master_q8, int16_t* out, uint32_t frames) {
    if (!streams || !out || nstreams < 0) return 0;
    if (nstreams > AMIX_MAX_STREAMS) nstreams = AMIX_MAX_STREAMS;

    for (uint32_t f = 0; f < frames; f++) {
        for (int ch = 0; ch < 2; ch++) {
            int64_t acc = 0;
            for (int i = 0; i < nstreams; i++) {
                const amix_stream_t* s = &streams[i];
                if (!s->active || !s->buf || f >= s->frames) continue;
                int32_t samp = s->buf[f * 2 + ch];
                acc += ((int64_t)samp * (int64_t)s->gain_q8) >> 8;
            }
            acc = (acc * (int64_t)master_q8) >> 8;
            if (acc > 32767)  acc = 32767;
            if (acc < -32768) acc = -32768;
            out[f * 2 + ch] = (int16_t)acc;
        }
    }
    return (int)frames;
}

#ifdef NET_SELFTEST
/* AMIX: prove sum, clamp, per-stream gain, master gain, and 8-stream mixing on
 * deterministic synthetic buffers (zero hardware). */
void amix_selftest(void) {
    int sum_ok = 0, clamp_ok = 0, gain_ok = 0, master_ok = 0, multi_ok = 0;
    int16_t out[2];

    /* (1) sum: +1000 and -3000 at unity -> -2000. */
    {
        int16_t a[2] = { 1000, 1000 }, b[2] = { -3000, -3000 };
        amix_stream_t s[2] = { { a, 1, 256, 1 }, { b, 1, 256, 1 } };
        amix_mix_period(s, 2, 256, out, 1);
        sum_ok = (out[0] == -2000 && out[1] == -2000) ? 1 : 0;
    }
    /* (2) clamp: +32767 + +32767 -> +32767 (no wrap). */
    {
        int16_t a[2] = { 32767, 32767 }, b[2] = { 32767, 32767 };
        amix_stream_t s[2] = { { a, 1, 256, 1 }, { b, 1, 256, 1 } };
        amix_mix_period(s, 2, 256, out, 1);
        clamp_ok = (out[0] == 32767 && out[1] == 32767) ? 1 : 0;
    }
    /* (3) per-stream gain 0.5 (q8=128). */
    {
        int16_t a[2] = { 1000, 1000 };
        amix_stream_t s[1] = { { a, 1, 128, 1 } };
        amix_mix_period(s, 1, 256, out, 1);
        gain_ok = (out[0] == 500 && out[1] == 500) ? 1 : 0;
    }
    /* (4) master gain 0.5. */
    {
        int16_t a[2] = { 1000, 1000 };
        amix_stream_t s[1] = { { a, 1, 256, 1 } };
        amix_mix_period(s, 1, 128, out, 1);
        master_ok = (out[0] == 500 && out[1] == 500) ? 1 : 0;
    }
    /* (5) 8 streams of +100 at unity -> +800 (concurrent playback). */
    {
        int16_t a[AMIX_MAX_STREAMS][2];
        amix_stream_t s[AMIX_MAX_STREAMS];
        for (int i = 0; i < AMIX_MAX_STREAMS; i++) {
            a[i][0] = 100; a[i][1] = 100;
            s[i].buf = a[i]; s[i].frames = 1; s[i].gain_q8 = 256; s[i].active = 1;
        }
        amix_mix_period(s, AMIX_MAX_STREAMS, 256, out, 1);
        multi_ok = (out[0] == 800 && out[1] == 800) ? 1 : 0;
    }

    kprintf("AMIX: %s sum=%d clamp=%d gain=%d master=%d multi=%d\n",
            (sum_ok && clamp_ok && gain_ok && master_ok && multi_ok) ? "PASS" : "FAIL",
            sum_ok, clamp_ok, gain_ok, master_ok, multi_ok);
}
#endif
