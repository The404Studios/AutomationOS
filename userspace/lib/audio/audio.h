/*
 * audio.h -- Freestanding userspace audio library (ring 3, no libc).
 * ==================================================================
 *
 * Provides tone/note/melody/SFX helpers built on SYS_BEEP (45).
 * If the kernel HDA driver is not wired yet, SYS_BEEP returns a negative
 * value (e.g. -ENOSYS/-ENOTSUP).  audio_available() detects this once
 * and caches the result; all audio_* calls become no-ops in that case --
 * no crash, no spin, just graceful silence.
 *
 * Frequency math: 440 * 2^((midi-69)/12) via a 12-entry fixed-point
 * semitone-ratio table (integer only, no libm, no floats).
 *
 * Usage:
 *   #include "../../lib/audio/audio.h"
 *   Link: audio.o alongside app
 *
 * Build (freestanding):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/audio/audio.c -o /tmp/audio.o
 *
 * No fs:0x28 canary -- compile with -fno-stack-protector.
 */

#ifndef AUDIO_H
#define AUDIO_H

/* Minimal integer types -- do not pull in stdint.h (not freestanding-safe
 * without libc headers).  Mirrors the convention used in paint.c. */
typedef unsigned int        audio_u32;
typedef unsigned short      audio_u16;
typedef int                 audio_i32;

/* -----------------------------------------------------------------------
 * Initialisation / probe
 * --------------------------------------------------------------------- */

/*
 * audio_available -- probe SYS_BEEP once (sc(45,0,0,0)).
 * Returns 1 if the syscall succeeded (>= 0), 0 if unavailable.
 * Result is cached; subsequent calls are free.
 */
int audio_available(void);

/* -----------------------------------------------------------------------
 * Core playback
 * --------------------------------------------------------------------- */

/*
 * audio_tone -- play a tone at freq_hz for duration_ms milliseconds.
 * No-op if audio_available() == 0.
 * The call blocks for the duration (kernel side drives the speaker/HDA).
 */
void audio_tone(audio_u32 freq_hz, audio_u32 duration_ms);

/*
 * audio_note -- play MIDI note `midi` for duration_ms milliseconds.
 * freq = 440 * 2^((midi-69)/12)  computed with integer fixed-point math.
 * MIDI 60 = C4 (261 Hz), 69 = A4 (440 Hz), 72 = C5 (523 Hz).
 * No-op if audio_available() == 0.
 */
void audio_note(audio_i32 midi, audio_u32 duration_ms);

/*
 * audio_play_melody -- play a blocking sequence of notes.
 * @midi_notes  array of MIDI note numbers (use 0 for a rest)
 * @durs_ms     parallel array of durations in milliseconds
 * @count       number of notes
 * Uses SYS_GET_TICKS_MS + SYS_YIELD for inter-note gaps so the
 * compositor can still process events between notes.
 * No-op if audio_available() == 0.
 */
void audio_play_melody(const int *midi_notes,
                       const audio_u16 *durs_ms,
                       int count);

/* -----------------------------------------------------------------------
 * Built-in SFX helpers -- short note sequences handy for games/UI.
 * All are no-ops when audio is unavailable.
 * --------------------------------------------------------------------- */

/* Single short click (UI feedback). */
void audio_sfx_click(void);

/* Ascending two-note coin pickup. */
void audio_sfx_coin(void);

/* Rising three-note jump sound. */
void audio_sfx_jump(void);

/* Descending game-over fanfare. */
void audio_sfx_gameover(void);

/* -----------------------------------------------------------------------
 * Utility: MIDI note -> Hz (always available, does not require audio hw).
 * Returns 1 for note 0 (rest placeholder) so callers can pass 0 safely.
 * --------------------------------------------------------------------- */
int audio_midi_to_freq(int midi_note);

#endif /* AUDIO_H */
