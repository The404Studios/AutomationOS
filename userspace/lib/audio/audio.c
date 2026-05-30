/*
 * audio.c -- Freestanding userspace audio library implementation.
 * ================================================================
 *
 * Implements the API declared in audio.h.
 *
 * Design principles:
 *  - Pure freestanding: no libc, no headers outside this TU.
 *  - Inline syscall helper sc() for SYS_BEEP, SYS_GET_TICKS_MS, SYS_YIELD.
 *  - audio_available() probes once (sc(45,0,0,0)) and caches the result.
 *    All public functions check the cache before doing anything so the
 *    caller never needs to guard calls.
 *  - Frequency math: integer fixed-point via a 12-entry semitone-ratio
 *    table (values * 1000), identical to the piano.c implementation.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/audio/audio.c -o /tmp/audio.o
 *
 * objdump -d /tmp/audio.o | grep fs:0x28   # MUST be empty
 */

#include "audio.h"

/* -----------------------------------------------------------------------
 * Syscall numbers.
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_BEEP         45   /* sc(45, freq_hz, duration_ms, 0) */

/* -----------------------------------------------------------------------
 * Inline syscall helper (3-argument form; matches piano.c convention).
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Cached availability flag.
 *   0  = not yet probed
 *   1  = available
 *  -1  = unavailable (SYS_BEEP returned negative)
 * --------------------------------------------------------------------- */
static int g_audio_state = 0;   /* 0 = unprobed */

/* -----------------------------------------------------------------------
 * Frequency table: semitone ratios * 1000, k = 0..11.
 * semitone_ratio_m1000[k] = round(1000 * 2^(k/12))
 *   0:1000  1:1059  2:1122  3:1189  4:1260  5:1335
 *   6:1414  7:1498  8:1587  9:1682 10:1782 11:1888
 * Identical to the table in piano.c.
 * --------------------------------------------------------------------- */
static const int g_semi_ratio[12] = {
    1000, 1059, 1122, 1189, 1260, 1335,
    1414, 1498, 1587, 1682, 1782, 1888
};

/* -----------------------------------------------------------------------
 * audio_midi_to_freq -- integer MIDI-note to Hz conversion.
 * MIDI note 0 used as a rest placeholder: returns 1 (avoids div-by-zero
 * in callers that pass the result directly to audio_tone).
 * --------------------------------------------------------------------- */
int audio_midi_to_freq(int midi_note)
{
    if (midi_note <= 0) return 1;   /* rest / silence placeholder */

    /* delta from A4 (MIDI 69, 440 Hz) */
    int delta = midi_note - 69;

    /* normalise to semi in [0..11] + whole octave offset */
    int semi   = delta % 12;
    int octave = delta / 12;
    if (semi < 0) { semi += 12; octave -= 1; }

    /* base_hz1000 = 440 * ratio  (unit: milliHz * 1000 → Hz * 1000) */
    long base_hz1000 = (long)440 * g_semi_ratio[semi];

    /* apply octave doubling / halving */
    if (octave >= 0) {
        int i;
        for (i = 0; i < octave; i++) base_hz1000 *= 2;
    } else {
        int i;
        for (i = 0; i < -octave; i++) base_hz1000 /= 2;
    }

    /* convert from mHz*1000 to Hz, rounding to nearest */
    return (int)((base_hz1000 + 500) / 1000);
}

/* -----------------------------------------------------------------------
 * audio_available -- probe once, cache result.
 * --------------------------------------------------------------------- */
int audio_available(void)
{
    if (g_audio_state == 0) {
        /* Probe: sc(45, 0, 0, 0) -- a zero-Hz zero-ms tone is harmless. */
        long ret = sc(SYS_BEEP, 0, 0, 0);
        g_audio_state = (ret >= 0) ? 1 : -1;
    }
    return (g_audio_state == 1) ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * audio_tone -- play freq_hz for duration_ms milliseconds.
 * --------------------------------------------------------------------- */
void audio_tone(audio_u32 freq_hz, audio_u32 duration_ms)
{
    if (!audio_available()) return;
    if (duration_ms == 0)   return;
    sc(SYS_BEEP, (long)freq_hz, (long)duration_ms, 0);
}

/* -----------------------------------------------------------------------
 * audio_note -- play MIDI note for duration_ms milliseconds.
 * --------------------------------------------------------------------- */
void audio_note(audio_i32 midi, audio_u32 duration_ms)
{
    if (!audio_available()) return;
    if (midi <= 0) {
        /* Rest: just yield for the duration using ticks. */
        long start = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        long until = start + (long)duration_ms;
        long t;
        do {
            sc(SYS_YIELD, 0, 0, 0);
            t = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        } while (t < until);
        return;
    }
    int freq = audio_midi_to_freq(midi);
    sc(SYS_BEEP, (long)freq, (long)duration_ms, 0);
}

/* -----------------------------------------------------------------------
 * audio_play_melody -- blocking note sequence with yield between notes.
 * Uses SYS_GET_TICKS_MS to ensure correct gaps even if SYS_BEEP returns
 * before the tone is done (or completes synchronously on the kernel side).
 * --------------------------------------------------------------------- */
void audio_play_melody(const int *midi_notes,
                       const audio_u16 *durs_ms,
                       int count)
{
    if (!audio_available()) return;
    int i;
    for (i = 0; i < count; i++) {
        int  midi = midi_notes[i];
        long dur  = (long)durs_ms[i];

        /* Fire the tone (or rest). */
        if (midi > 0) {
            int freq = audio_midi_to_freq(midi);
            sc(SYS_BEEP, (long)freq, dur, 0);
        }

        /* Wait out the full duration using ticks so we don't drift. */
        long start = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        long until = start + dur;
        long t;
        do {
            sc(SYS_YIELD, 0, 0, 0);
            t = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        } while (t < until);
    }
}

/* -----------------------------------------------------------------------
 * SFX helpers.
 * All use audio_tone() or audio_note() so they inherit graceful degradation.
 * --------------------------------------------------------------------- */

/* audio_sfx_click -- single short 1 kHz tick (UI button feedback). */
void audio_sfx_click(void)
{
    audio_tone(1000, 30);
}

/* audio_sfx_coin -- two rising notes: E5 (659 Hz) then B5 (988 Hz). */
void audio_sfx_coin(void)
{
    static const int    notes[2] = { 76, 83 };   /* E5, B5 */
    static const audio_u16 durs[2]  = {  80, 120 };
    audio_play_melody(notes, durs, 2);
}

/* audio_sfx_jump -- three rising notes: C4 E4 G4. */
void audio_sfx_jump(void)
{
    static const int    notes[3] = { 60, 64, 67 };   /* C4 E4 G4 */
    static const audio_u16 durs[3]  = {  80, 60, 80 };
    audio_play_melody(notes, durs, 3);
}

/*
 * audio_sfx_gameover -- descending fanfare: G4 E4 C4 A3 (minor feel).
 * Longer durations for dramatic effect.
 */
void audio_sfx_gameover(void)
{
    static const int    notes[4] = { 67, 64, 60, 57 };  /* G4 E4 C4 A3 */
    static const audio_u16 durs[4]  = { 200, 200, 200, 400 };
    audio_play_melody(notes, durs, 4);
}
