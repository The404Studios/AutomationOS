#ifndef AMIX_H
#define AMIX_H
/*
 * amix.h -- AUDIO-MIXER core API (kernel/drivers/audio/audio_mix.c).
 * Shared so the playback path (audio_tone.c) can drive the mixer into the HDA
 * DMA buffer, proving concurrent streams reach real audio hardware.
 */
#include "types.h"

#define AMIX_MAX_STREAMS 8

typedef struct {
    const int16_t* buf;    /* interleaved stereo s16 (frame f -> buf[f*2+ch]) */
    uint32_t       frames; /* frames available in buf                          */
    uint16_t       gain_q8;/* per-stream gain, 256 = unity                     */
    uint8_t        active; /* 0 = skip                                         */
} amix_stream_t;

/* Mix nstreams stereo-s16 streams into out (frames*2 samples) with per-stream
 * then master Q8 gain, int64 accumulate, clamp to int16. Returns frames. */
int amix_mix_period(const amix_stream_t* streams, int nstreams,
                    uint16_t master_q8, int16_t* out, uint32_t frames);

#endif /* AMIX_H */
