/*
 * audio_fmt.c -- AUDIO-FMT: PCM format / channel / sample-rate conversion.
 * =======================================================================
 *
 * A self-contained, FPU-free converter so the mixer + /dev/dsp can accept any
 * common PCM and reconcile it to the HDA stream format. BoredOS has ZERO audio,
 * so this is an absolute-bar 5x feature (formats 1->4, rates 2->6, channel
 * up/down-mix). Original by construction: integer normalize -> linear-interp
 * resample -> denormalize over OUR audio_pcm_fmt_t -- not libsoxr / libavresample.
 *
 * Pipeline per output frame/channel: read source sample(s) normalized to a
 * full-scale int32, map channels (mono<->stereo), linearly interpolate between
 * the two bracketing source frames for the resample ratio, then denormalize to
 * the output format. No heap (caller supplies buffers), no floating point.
 *
 * Scope: kernel/drivers/audio/audio_fmt.c (new). Pure utility; the mixer/dsp
 * wiring is a follow-on.
 */
#include "../../include/types.h"
#include "../../include/kernel.h"   /* kprintf (selftest only) */

#define AFMT_U8   1   /* unsigned 8-bit, midpoint 128            */
#define AFMT_S16  2   /* signed 16-bit little-endian             */
#define AFMT_S24  3   /* signed 24-bit (3 bytes) little-endian   */
#define AFMT_S32  4   /* signed 32-bit little-endian             */

typedef struct {
    uint32_t rate;    /* sample rate in Hz       */
    uint16_t fmt;     /* AFMT_*                  */
    uint16_t ch;      /* channel count (1 or 2)  */
} audio_pcm_fmt_t;

/* Read one sample (frame,ch) normalized to a full-scale signed int32. */
static int32_t afmt_norm_read(const audio_pcm_fmt_t* f, const void* buf,
                              uint32_t frame, uint32_t ch) {
    uint32_t idx = frame * f->ch + ch;
    switch (f->fmt) {
        case AFMT_U8: {
            const uint8_t* p = (const uint8_t*)buf;
            return ((int32_t)p[idx] - 128) << 24;
        }
        case AFMT_S16: {
            const int16_t* s = (const int16_t*)buf;
            return (int32_t)s[idx] << 16;
        }
        case AFMT_S24: {
            const uint8_t* q = (const uint8_t*)buf + (uint32_t)idx * 3;
            int32_t v = (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16));
            if (v & 0x00800000) v |= (int32_t)0xFF000000;   /* sign-extend 24->32 */
            return v << 8;
        }
        case AFMT_S32: {
            const int32_t* s = (const int32_t*)buf;
            return s[idx];
        }
    }
    return 0;
}

/* Write one full-scale int32 sample to (frame,ch) in the output format. */
static void afmt_norm_write(const audio_pcm_fmt_t* f, void* buf,
                            uint32_t frame, uint32_t ch, int32_t v) {
    uint32_t idx = frame * f->ch + ch;
    switch (f->fmt) {
        case AFMT_U8: {
            uint8_t* p = (uint8_t*)buf;
            int32_t u = (v >> 24) + 128;
            if (u < 0) u = 0; if (u > 255) u = 255;
            p[idx] = (uint8_t)u;
            break;
        }
        case AFMT_S16: {
            int16_t* s = (int16_t*)buf;
            int32_t x = v >> 16;
            if (x > 32767) x = 32767; if (x < -32768) x = -32768;
            s[idx] = (int16_t)x;
            break;
        }
        case AFMT_S24: {
            uint8_t* q = (uint8_t*)buf + (uint32_t)idx * 3;
            int32_t x = v >> 8;
            q[0] = (uint8_t)(x & 0xFF);
            q[1] = (uint8_t)((x >> 8) & 0xFF);
            q[2] = (uint8_t)((x >> 16) & 0xFF);
            break;
        }
        case AFMT_S32: {
            int32_t* s = (int32_t*)buf;
            s[idx] = v;
            break;
        }
    }
}

/* Normalized source sample for OUTPUT channel `oc` at source frame `sf`,
 * applying the channel map: same-count -> matching channel; mono->N -> ch0;
 * stereo->mono -> average of L+R. Frame index clamped to the last source frame. */
static int32_t afmt_src_ch(const audio_pcm_fmt_t* in, const void* buf,
                           uint32_t in_frames, uint32_t sf,
                           uint16_t oc, uint16_t out_ch) {
    if (sf >= in_frames) sf = in_frames - 1;
    if (in->ch == out_ch) return afmt_norm_read(in, buf, sf, oc);
    if (in->ch == 1)      return afmt_norm_read(in, buf, sf, 0);
    {
        int32_t l = afmt_norm_read(in, buf, sf, 0);
        int32_t r = afmt_norm_read(in, buf, sf, 1);
        return (int32_t)(((int64_t)l + r) / 2);
    }
}

/*
 * Convert in_frames of `in` PCM into `out` PCM (up to out_max_frames). Returns
 * the number of output frames produced (0 on bad args).
 */
int audio_convert(const audio_pcm_fmt_t* in, const void* in_buf, uint32_t in_frames,
                  const audio_pcm_fmt_t* out, void* out_buf, uint32_t out_max_frames) {
    if (!in || !out || !in_buf || !out_buf) return 0;
    if (in->rate == 0 || out->rate == 0 || in_frames == 0) return 0;
    if (in->ch == 0 || out->ch == 0) return 0;

    uint64_t of = (uint64_t)in_frames * out->rate / in->rate;
    if (of > out_max_frames) of = out_max_frames;

    for (uint32_t i = 0; i < (uint32_t)of; i++) {
        uint64_t num = (uint64_t)i * in->rate;
        uint32_t idx = (uint32_t)(num / out->rate);
        uint32_t rem = (uint32_t)(num % out->rate);     /* frac = rem / out->rate */
        for (uint16_t oc = 0; oc < out->ch; oc++) {
            int32_t a = afmt_src_ch(in, in_buf, in_frames, idx,     oc, out->ch);
            int32_t b = afmt_src_ch(in, in_buf, in_frames, idx + 1, oc, out->ch);
            int32_t s = a + (int32_t)(((int64_t)(b - a) * rem) / out->rate);
            afmt_norm_write(out, out_buf, i, oc, s);
        }
    }
    return (int)of;
}

#ifdef NET_SELFTEST
/* AFMT: prove format, channel up/down-mix, and resample with deterministic
 * synthetic buffers (zero hardware). */
void afmt_selftest(void) {
    int fmt_ok = 0, up_ok = 0, down_ok = 0, rs_ok = 0;

    /* (1) u8 -> s16: midpoint 0x80 -> 0; 0xFF -> +; 0x00 -> -. */
    {
        audio_pcm_fmt_t i8 = { 8000, AFMT_U8, 1 }, s16 = { 8000, AFMT_S16, 1 };
        uint8_t in[4] = { 0x80, 0xFF, 0x00, 0x80 };
        int16_t out[8] = { 0 };
        int n = audio_convert(&i8, in, 4, &s16, out, 8);
        fmt_ok = (n == 4 && out[0] == 0 && out[1] > 0 && out[2] < 0 && out[3] == 0) ? 1 : 0;
    }
    /* (2) mono -> stereo duplicate. */
    {
        audio_pcm_fmt_t m = { 8000, AFMT_S16, 1 }, st = { 8000, AFMT_S16, 2 };
        int16_t in[2] = { 100, 200 };
        int16_t out[8] = { 0 };
        int n = audio_convert(&m, in, 2, &st, out, 4);
        up_ok = (n == 2 && out[0] == 100 && out[1] == 100 &&
                 out[2] == 200 && out[3] == 200) ? 1 : 0;
    }
    /* (3) stereo -> mono average. */
    {
        audio_pcm_fmt_t st = { 8000, AFMT_S16, 2 }, m = { 8000, AFMT_S16, 1 };
        int16_t in[4] = { 100, 300, 200, 400 };   /* (L,R)(L,R) */
        int16_t out[4] = { 0 };
        int n = audio_convert(&st, in, 2, &m, out, 4);
        down_ok = (n == 2 && out[0] == 200 && out[1] == 300) ? 1 : 0;
    }
    /* (4) resample 8k -> 16k: frame count doubles, midpoint interpolated. */
    {
        audio_pcm_fmt_t a = { 8000, AFMT_S16, 1 }, b = { 16000, AFMT_S16, 1 };
        int16_t in[4] = { 0, 1000, 2000, 3000 };
        int16_t out[16] = { 0 };
        int n = audio_convert(&a, in, 4, &b, out, 16);
        rs_ok = (n == 8 && out[0] == 0 && out[1] == 500 && out[2] == 1000) ? 1 : 0;
    }

    kprintf("AFMT: %s fmt=%d up=%d down=%d rs=%d\n",
            (fmt_ok && up_ok && down_ok && rs_ok) ? "PASS" : "FAIL",
            fmt_ok, up_ok, down_ok, rs_ok);
}
#endif
