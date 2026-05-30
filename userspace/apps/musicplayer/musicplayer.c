/*
 * musicplayer.c -- Chiptune music player with visualizer (freestanding, ring 3).
 * ==============================================================================
 *
 * Window: 600 x 400 px, title "Music Player".
 *
 * Built-in track library (4 tracks, hard-coded MIDI note arrays):
 *   0 "C Major Scale"   -- ascending/descending C major scale
 *   1 "Arpeggio Jingle" -- bright arpeggiated chord jingle
 *   2 "Twinkle Twinkle" -- classic nursery rhyme melody
 *   3 "Ode to Joy"      -- Beethoven's famous theme (theme from Symphony No.9)
 *
 * Transport controls: Prev / Play|Pause / Stop / Next buttons + progress bar.
 *
 * Non-blocking note scheduler:
 *   Each frame checks SYS_GET_TICKS_MS.  When (now - note_start_ms) >=
 *   current note duration, the scheduler fires audio_tone() for the NEXT
 *   note (which is essentially non-blocking -- the kernel schedules the
 *   beep asynchronously) and updates note_start_ms.  The UI loop is NEVER
 *   blocked waiting for audio.
 *
 * Visualizer:
 *   16 vertical bars across the canvas area.  Each bar's height derives
 *   from the current MIDI pitch (higher = taller) scaled into 0..VIS_MAX_H.
 *   On each note-advance, bars get individual "energy" values proportional
 *   to the note, with slight random-ish spread across bars.  Energy decays
 *   each frame so bars smoothly fall back to zero (gravity effect).
 *   The background pulses between two colours in sync with note onsets.
 *
 * Build (flags DIRECTLY on cmdline):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/musicplayer/musicplayer.c -o /tmp/mp.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/audio/audio.c -o /tmp/au.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/mp.o /tmp/wlc.o /tmp/bf.o /tmp/au.o -o /tmp/musicplayer.elf
 *   objdump -d /tmp/musicplayer.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [MUSIC] starting
 *   [MUSIC] play <track name>
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/audio/audio.h"

/* -------------------------------------------------------------------------
 * Syscall numbers and inline helpers.
 * ---------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

static inline long sc3(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static inline long get_ticks(void)
{
    return sc3(SYS_GET_TICKS_MS, 0, 0, 0);
}

static inline void sys_yield(void)
{
    sc3(SYS_YIELD, 0, 0, 0);
}

/* -------------------------------------------------------------------------
 * Minimal freestanding helpers.
 * ---------------------------------------------------------------------- */
typedef unsigned int  u32;
typedef int           i32;
typedef unsigned char u8;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial(const char *m)
{
    sc3(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/* Integer absolute value. */
static i32 iabs(i32 v) { return v < 0 ? -v : v; }

/* Integer clamp. */
static i32 iclamp(i32 v, i32 lo, i32 hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* -------------------------------------------------------------------------
 * Window / layout constants.
 * ---------------------------------------------------------------------- */
#define WIN_W   600
#define WIN_H   400

/* Header bar: title + audio indicator. */
#define HDR_H   30

/* Visualizer canvas sits just below header. */
#define VIS_Y   HDR_H
#define VIS_H   200

/* Info bar: track name + progress bar. */
#define INFO_Y  (VIS_Y + VIS_H)
#define INFO_H  46

/* Transport bar: Prev / Play|Pause / Stop / Next. */
#define TRANS_Y (INFO_Y + INFO_H)
#define TRANS_H 60

/* Track list panel below transport. */
#define LIST_Y  (TRANS_Y + TRANS_H)
#define LIST_H  (WIN_H - LIST_Y)    /* fills rest of window */

/* -------------------------------------------------------------------------
 * Colour palette.
 * ---------------------------------------------------------------------- */
#define C_BG_BASE      0xFF0D0D1Au   /* near-black navy  */
#define C_BG_PULSE_A   0xFF1A0D2Eu   /* deep purple      */
#define C_BG_PULSE_B   0xFF0D1A2Eu   /* deep blue        */
#define C_HDR_BG       0xFF16213Eu
#define C_INFO_BG      0xFF1A1A2Eu
#define C_TRANS_BG     0xFF12122Au
#define C_LIST_BG      0xFF0F0F1Cu
#define C_LIST_SEL     0xFF2D2D6Au
#define C_SEP          0xFF33335Au
#define C_TEXT         0xFFCCCCFFu
#define C_TEXT_DIM     0xFF6666AAu
#define C_TEXT_BRIGHT  0xFFFFFFFFu
#define C_BTN_FACE     0xFF2A2A5Au
#define C_BTN_HOT      0xFF3C3C7Au
#define C_BTN_BORDER   0xFF5555AAu
#define C_PROG_BG      0xFF1E1E3Eu
#define C_PROG_FG      0xFF6644FFu
#define C_PROG_HEAD    0xFFCC88FFu
#define C_BAR_BASE     0xFF4400CCu   /* visualizer bar base colour */
#define C_BAR_MID      0xFF8822FFu
#define C_BAR_TOP      0xFFFFAAFFu

/* -------------------------------------------------------------------------
 * Track data.
 * MIDI note 0 is used as a rest (audio_midi_to_freq handles it).
 * ---------------------------------------------------------------------- */

/* Track 0: C Major Scale (C4 up to C5, then back down) */
static const int t0_notes[] = {
    60,62,64,65,67,69,71,72,  /* C4 D4 E4 F4 G4 A4 B4 C5 */
    72,71,69,67,65,64,62,60   /* back down */
};
static const audio_u16 t0_durs[] = {
    220,220,220,220,220,220,220,440,
    220,220,220,220,220,220,220,440
};
#define T0_LEN 16

/* Track 1: Arpeggio Jingle (C major & A minor arpeggios) */
static const int t1_notes[] = {
    60,64,67,72, 67,64,60,  /* C major up and down */
    57,60,64,69, 64,60,57,  /* A minor up and down */
    62,65,69,74, 69,65,62,  /* D minor              */
    60,64,67,72,72,0,0,0    /* C major finish + rest */
};
static const audio_u16 t1_durs[] = {
    150,150,150,300, 150,150,300,
    150,150,150,300, 150,150,300,
    150,150,150,300, 150,150,300,
    150,150,150,300,600,100,100,100
};
#define T1_LEN 31

/* Track 2: Twinkle Twinkle Little Star */
static const int t2_notes[] = {
    60,60,67,67,69,69,67,0,   /* Twin-kle twin-kle lit-tle star  */
    65,65,64,64,62,62,60,0,   /* How I won-der what you are      */
    67,67,65,65,64,64,62,0,   /* Up a-bove the world so high     */
    67,67,65,65,64,64,62,0,   /* Like a dia-mond in the sky      */
    60,60,67,67,69,69,67,0,   /* Twin-kle twin-kle lit-tle star  */
    65,65,64,64,62,62,60,0    /* How I won-der what you are      */
};
static const audio_u16 t2_durs[] = {
    300,300,300,300,300,300,600,150,
    300,300,300,300,300,300,600,150,
    300,300,300,300,300,300,600,150,
    300,300,300,300,300,300,600,150,
    300,300,300,300,300,300,600,150,
    300,300,300,300,300,300,700,150
};
#define T2_LEN 48

/* Track 3: Ode to Joy (Beethoven 9th, main theme) */
static const int t3_notes[] = {
    64,64,65,67, 67,65,64,62, 60,60,62,64, 64,62,62,0,  /* line 1 */
    64,64,65,67, 67,65,64,62, 60,60,62,64, 62,60,60,0,  /* line 2 */
    62,62,64,60, 62,64,65,64, 60,62,64,65, 64,62,60,62, /* line 3 */
    60,62,64,60, 55,0,0,0,                               /* ending */
    64,64,65,67, 67,65,64,62, 60,60,62,64, 62,60,60,0   /* repeat  */
};
static const audio_u16 t3_durs[] = {
    300,300,300,300, 300,300,300,300, 300,300,300,300, 450,150,600,150,
    300,300,300,300, 300,300,300,300, 300,300,300,300, 450,150,600,150,
    300,300,300,300, 300,150,150,300, 300,150,150,300, 300,300,300,300,
    600,300,300,300,
    300,300,300,300, 300,300,300,300, 300,300,300,300, 450,150,700,150
};
#define T3_LEN 60

/* Track registry. */
#define NUM_TRACKS 4

typedef struct {
    const char    *name;
    const int     *notes;
    const audio_u16 *durs;
    int            len;
} Track;

static const Track tracks[NUM_TRACKS] = {
    { "C Major Scale",   t0_notes, t0_durs, T0_LEN },
    { "Arpeggio Jingle", t1_notes, t1_durs, T1_LEN },
    { "Twinkle Twinkle", t2_notes, t2_durs, T2_LEN },
    { "Ode to Joy",      t3_notes, t3_durs, T3_LEN },
};

/* -------------------------------------------------------------------------
 * Playback state.
 * ---------------------------------------------------------------------- */
typedef enum { PS_STOPPED, PS_PLAYING, PS_PAUSED } PlayState;

static int        cur_track    = 0;
static int        cur_note     = 0;     /* index into tracks[cur_track] */
static long       note_start   = 0;     /* ticks_ms when cur note began */
static long       pause_offset = 0;     /* ms elapsed in note before pause */
static PlayState  play_state   = PS_STOPPED;

/* Total elapsed ms in the track (for progress bar). */
static long       track_elapsed_ms = 0;

/* Compute total track duration in ms. */
static long track_total_ms(int t)
{
    long total = 0;
    for (int i = 0; i < tracks[t].len; i++)
        total += tracks[t].durs[i];
    return total;
}

/* Compute ms elapsed up to (not including) note index n in track t. */
static long ms_up_to_note(int t, int n)
{
    long total = 0;
    for (int i = 0; i < n && i < tracks[t].len; i++)
        total += tracks[t].durs[i];
    return total;
}

/* -------------------------------------------------------------------------
 * Visualizer state.
 * ---------------------------------------------------------------------- */
#define VIS_BARS      16
#define VIS_MAX_H     (VIS_H - 8)   /* maximum bar height (px) */
#define VIS_BAR_W     ((WIN_W - 20) / VIS_BARS)  /* bar width  */
#define VIS_BAR_GAP   2

/* Current bar heights (floating: stored as 0..VIS_MAX_H * 256 for decay). */
static i32 bar_energy[VIS_BARS];    /* fixed-point *256 */

/* Background pulse phase (0..511). */
static i32 pulse_phase = 0;

/* "Beat flash" counter: set on each note onset, counts down. */
static i32 beat_flash = 0;

/* Pseudo-random state for bar spread (simple LCG). */
static u32 rng_state = 0xDEADBEEFu;
static u32 prng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/*
 * Trigger the visualizer for a new note onset.
 * midi=0 is a rest -- bars decay to zero.
 */
static void vis_trigger(int midi_note)
{
    if (midi_note == 0) {
        /* Rest: let bars fall naturally, just set beat_flash. */
        beat_flash = 6;
        return;
    }
    /* Map MIDI to normalised energy 0..256.
     * MIDI 48 (C3) -> low, MIDI 84 (C6) -> high. */
    i32 norm = iclamp((midi_note - 48) * 256 / 36, 0, 256);

    beat_flash = 12;

    for (int i = 0; i < VIS_BARS; i++) {
        /* Spread: each bar gets base energy +/- random 25%. */
        u32 rnd = prng_next();
        i32 spread = (i32)(rnd & 0x3Fu) - 32;  /* -32..+31 */
        i32 energy = (norm + spread) * VIS_MAX_H;
        if (energy < 0)    energy = 0;
        if (energy > VIS_MAX_H * 256) energy = VIS_MAX_H * 256;
        bar_energy[i] = energy;
    }
}

/*
 * Decay bars each frame (called once per frame).
 * Decay is proportional so bars fall with a pleasing gravity curve.
 */
static void vis_decay(void)
{
    for (int i = 0; i < VIS_BARS; i++) {
        /* Subtract ~6% of VIS_MAX_H per frame (roughly 4 frames to fall). */
        bar_energy[i] -= (VIS_MAX_H * 256) / 16;
        if (bar_energy[i] < 0) bar_energy[i] = 0;
    }
    if (beat_flash > 0) beat_flash--;
    /* Advance pulse phase continuously. */
    pulse_phase = (pulse_phase + 3) & 0x1FF;
}

/* -------------------------------------------------------------------------
 * Drawing primitives.
 * ---------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > WIN_W) x2 = WIN_W;
    i32 y2 = y + h; if (y2 > WIN_H) y2 = WIN_H;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = color;
    }
}

static void draw_hline(u32 *buf, u32 stride_px,
                       i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, stride_px, x, y, len, 1, color);
}

static void draw_border(u32 *buf, u32 stride_px,
                        i32 x, i32 y, i32 w, i32 h, u32 color)
{
    draw_hline(buf, stride_px, x, y,         w, color);
    draw_hline(buf, stride_px, x, y + h - 1, w, color);
    fill_rect(buf, stride_px, x,         y, 1, h, color);
    fill_rect(buf, stride_px, x + w - 1, y, 1, h, color);
}

/*
 * Interpolate between two ARGB colours by factor t in [0..256].
 * Components blended linearly.
 */
static u32 lerp_color(u32 a, u32 b, i32 t)
{
    if (t <= 0)   return a;
    if (t >= 256) return b;
    u32 ra = (a >> 16) & 0xFF, ga = (a >> 8) & 0xFF, ba_ = a & 0xFF;
    u32 rb = (b >> 16) & 0xFF, gb = (b >> 8) & 0xFF, bb_ = b & 0xFF;
    u32 r  = ra + (u32)((i32)(rb - ra) * t / 256);
    u32 g  = ga + (u32)((i32)(gb - ga) * t / 256);
    u32 bl = ba_ + (u32)((i32)(bb_ - ba_) * t / 256);
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

/* Draw a vertical gradient fill in a rectangle (top -> bottom). */
static void fill_gradient_v(u32 *buf, u32 stride_px,
                             i32 x, i32 y, i32 w, i32 h,
                             u32 col_top, u32 col_bot)
{
    if (w <= 0 || h <= 0) return;
    for (i32 yy = 0; yy < h; yy++) {
        u32 c = lerp_color(col_top, col_bot, yy * 256 / h);
        fill_rect(buf, stride_px, x, y + yy, w, 1, c);
    }
}

/* -------------------------------------------------------------------------
 * UI button helper.
 * ---------------------------------------------------------------------- */
#define BTN_W  80
#define BTN_H  36

typedef struct {
    i32         x, y;
    const char *label;
    int         hot;   /* 1 if mouse is over */
} Button;

static void draw_button(u32 *buf, u32 stride_px, const Button *b)
{
    u32 face = b->hot ? C_BTN_HOT : C_BTN_FACE;
    fill_rect(buf, stride_px, b->x, b->y, BTN_W, BTN_H, face);
    draw_border(buf, stride_px, b->x, b->y, BTN_W, BTN_H, C_BTN_BORDER);
    /* Centre label. */
    int tw = font_text_width(b->label);
    int tx = b->x + (BTN_W - tw) / 2;
    int ty = b->y + (BTN_H - FONT_H) / 2;
    font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                     tx, ty, b->label, C_TEXT_BRIGHT);
}

static int hit_button(const Button *b, i32 mx, i32 my)
{
    return (mx >= b->x && mx < b->x + BTN_W &&
            my >= b->y && my < b->y + BTN_H);
}

/* -------------------------------------------------------------------------
 * Transport buttons layout.
 * ---------------------------------------------------------------------- */
#define BTN_GAP  12
#define BTN_PREV_X  ((WIN_W - (4 * BTN_W + 3 * BTN_GAP)) / 2)
#define BTN_PLAY_X  (BTN_PREV_X + BTN_W + BTN_GAP)
#define BTN_STOP_X  (BTN_PLAY_X + BTN_W + BTN_GAP)
#define BTN_NEXT_X  (BTN_STOP_X + BTN_W + BTN_GAP)
#define BTN_Y       (TRANS_Y + (TRANS_H - BTN_H) / 2)

/* -------------------------------------------------------------------------
 * Track list layout.
 * ---------------------------------------------------------------------- */
#define LIST_ITEM_H  ((LIST_H - 4) / NUM_TRACKS)   /* height per track row */
#define LIST_X       10
#define LIST_W       (WIN_W - 20)

/* -------------------------------------------------------------------------
 * Non-blocking note scheduler.
 * Called each frame; fires the next note when its predecessor's duration
 * has elapsed.  Returns 1 if a note was advanced (visualizer should update).
 * ---------------------------------------------------------------------- */
static int scheduler_tick(void)
{
    if (play_state != PS_PLAYING) return 0;

    const Track *tr = &tracks[cur_track];
    if (cur_note >= tr->len) {
        /* Track finished: stop. */
        play_state = PS_STOPPED;
        cur_note   = 0;
        track_elapsed_ms = 0;
        return 0;
    }

    long now = get_ticks();
    long elapsed = now - note_start;
    long dur     = tr->durs[cur_note];

    if (elapsed < dur) return 0;   /* still in current note */

    /* Advance to next note. */
    track_elapsed_ms += dur;
    cur_note++;

    if (cur_note >= tr->len) {
        /* Just finished the last note. */
        play_state = PS_STOPPED;
        cur_note   = 0;
        track_elapsed_ms = 0;
        return 1;
    }

    /* Fire the new note (non-blocking: kernel schedules beep async). */
    int midi = tr->notes[cur_note];
    if (midi != 0) {
        audio_note((audio_i32)midi, (audio_u32)tr->durs[cur_note]);
    }
    note_start = get_ticks();
    vis_trigger(midi);
    return 1;
}

/* Start playback from the current note position. */
static void start_play(void)
{
    const Track *tr = &tracks[cur_track];
    if (cur_note >= tr->len) cur_note = 0;
    play_state = PS_PLAYING;
    note_start = get_ticks();
    /* Fire the first note immediately. */
    int midi = tr->notes[cur_note];
    if (midi != 0) {
        audio_note((audio_i32)midi, (audio_u32)tr->durs[cur_note]);
    }
    vis_trigger(midi);
}

static void stop_play(void)
{
    play_state       = PS_STOPPED;
    cur_note         = 0;
    track_elapsed_ms = 0;
    pause_offset     = 0;
}

static void pause_play(void)
{
    if (play_state == PS_PLAYING) {
        long now = get_ticks();
        pause_offset = now - note_start;
        play_state   = PS_PAUSED;
    } else if (play_state == PS_PAUSED) {
        /* Resume. */
        note_start = get_ticks() - pause_offset;
        pause_offset = 0;
        play_state   = PS_PLAYING;
    }
}

/* -------------------------------------------------------------------------
 * Rendering helpers.
 * ---------------------------------------------------------------------- */

/* Draw the pulsing gradient background for the visualizer area. */
static void draw_vis_background(u32 *buf, u32 stride_px)
{
    /* Pulse between two background colours using sine-like triangle wave. */
    i32 t = pulse_phase;                      /* 0..511 */
    i32 tri;
    if (t < 256) tri = t;
    else         tri = 511 - t;              /* triangle: 0..255..0 */

    /* Beat flash: boost brightness briefly. */
    i32 flash_boost = beat_flash * 12;
    if (flash_boost > 64) flash_boost = 64;
    tri = iclamp(tri + flash_boost, 0, 255);

    u32 bg_col = lerp_color(C_BG_PULSE_A, C_BG_PULSE_B, tri);

    /* Draw gradient: bg_col at top of vis area fading to C_BG_BASE. */
    fill_gradient_v(buf, stride_px,
                    0, VIS_Y, WIN_W, VIS_H,
                    bg_col, C_BG_BASE);
}

/* Draw the 16 visualizer bars. */
static void draw_vis_bars(u32 *buf, u32 stride_px)
{
    int total_bar_area = VIS_BARS * VIS_BAR_W + (VIS_BARS - 1) * VIS_BAR_GAP;
    int bar_x0 = (WIN_W - total_bar_area) / 2;
    int vis_bottom = VIS_Y + VIS_H - 4;  /* 4px padding at bottom */

    for (int i = 0; i < VIS_BARS; i++) {
        int bh = bar_energy[i] / 256;  /* pixel height */
        if (bh < 2) bh = 2;           /* minimum stub so bars are visible */

        int bx = bar_x0 + i * (VIS_BAR_W + VIS_BAR_GAP);
        int by = vis_bottom - bh;

        /* Gradient: base at bottom, bright at top. */
        fill_gradient_v(buf, stride_px,
                        bx, by, VIS_BAR_W, bh,
                        C_BAR_TOP, C_BAR_BASE);

        /* Bright cap pixel at very top of the bar. */
        fill_rect(buf, stride_px, bx, by, VIS_BAR_W, 2, C_BAR_TOP);
    }
}

/* Draw header bar. */
static void draw_header(u32 *buf, u32 stride_px)
{
    fill_rect(buf, stride_px, 0, 0, WIN_W, HDR_H, C_HDR_BG);
    draw_hline(buf, stride_px, 0, HDR_H - 1, WIN_W, C_SEP);

    /* Title */
    font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                     8, (HDR_H - FONT_H) / 2,
                     "Music Player", C_TEXT_BRIGHT);

    /* Audio indicator */
    const char *aud_str = audio_available() ? "Audio: ON" : "Audio: --";
    int aw = font_text_width(aud_str);
    font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                     WIN_W - aw - 8, (HDR_H - FONT_H) / 2,
                     aud_str,
                     audio_available() ? 0xFF88FF88u : C_TEXT_DIM);
}

/* Draw info bar (track name + progress). */
static void draw_info(u32 *buf, u32 stride_px)
{
    fill_rect(buf, stride_px, 0, INFO_Y, WIN_W, INFO_H, C_INFO_BG);
    draw_hline(buf, stride_px, 0, INFO_Y, WIN_W, C_SEP);
    draw_hline(buf, stride_px, 0, INFO_Y + INFO_H - 1, WIN_W, C_SEP);

    /* Track name. */
    const char *name = tracks[cur_track].name;
    const char *state_str = (play_state == PS_PLAYING) ? " [Playing]"
                          : (play_state == PS_PAUSED)  ? " [Paused]"
                          :                              " [Stopped]";
    int ny = INFO_Y + 6;
    font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                     8, ny, name, C_TEXT_BRIGHT);
    int nx = 8 + font_text_width(name);
    font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                     nx, ny, state_str, C_TEXT_DIM);

    /* Progress bar. */
    int pb_x  = 8;
    int pb_y  = INFO_Y + INFO_H - 14;
    int pb_w  = WIN_W - 16;
    int pb_h  = 8;

    fill_rect(buf, stride_px, pb_x, pb_y, pb_w, pb_h, C_PROG_BG);
    draw_border(buf, stride_px, pb_x, pb_y, pb_w, pb_h, C_SEP);

    long total = track_total_ms(cur_track);
    long elapsed = track_elapsed_ms;
    if (play_state == PS_PLAYING || play_state == PS_PAUSED) {
        /* Add time within current note. */
        long note_ms;
        if (play_state == PS_PAUSED) {
            note_ms = pause_offset;
        } else {
            note_ms = get_ticks() - note_start;
        }
        long note_dur = tracks[cur_track].durs[cur_note < tracks[cur_track].len
                                                ? cur_note : 0];
        if (note_ms > note_dur) note_ms = note_dur;
        elapsed += note_ms;
    }
    if (total > 0) {
        int fill_w = (int)(elapsed * (long)(pb_w - 2) / total);
        if (fill_w < 0) fill_w = 0;
        if (fill_w > pb_w - 2) fill_w = pb_w - 2;
        if (fill_w > 0) {
            fill_rect(buf, stride_px,
                      pb_x + 1, pb_y + 1, fill_w, pb_h - 2, C_PROG_FG);
            /* Bright head. */
            fill_rect(buf, stride_px,
                      pb_x + fill_w, pb_y + 1, 2, pb_h - 2, C_PROG_HEAD);
        }
    }
}

/* Draw transport. */
static void draw_transport(u32 *buf, u32 stride_px,
                           const Button *btn_prev,
                           const Button *btn_play,
                           const Button *btn_stop,
                           const Button *btn_next)
{
    fill_rect(buf, stride_px, 0, TRANS_Y, WIN_W, TRANS_H, C_TRANS_BG);
    draw_hline(buf, stride_px, 0, TRANS_Y + TRANS_H - 1, WIN_W, C_SEP);

    draw_button(buf, stride_px, btn_prev);
    draw_button(buf, stride_px, btn_play);
    draw_button(buf, stride_px, btn_stop);
    draw_button(buf, stride_px, btn_next);
}

/* Draw track list. */
static void draw_tracklist(u32 *buf, u32 stride_px)
{
    fill_rect(buf, stride_px, 0, LIST_Y, WIN_W, LIST_H, C_LIST_BG);
    draw_hline(buf, stride_px, 0, LIST_Y, WIN_W, C_SEP);

    /* Column header. */
    font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                     LIST_X, LIST_Y + 3, "Tracks:", C_TEXT_DIM);

    for (int i = 0; i < NUM_TRACKS; i++) {
        int iy = LIST_Y + LIST_ITEM_H * i + (LIST_H - NUM_TRACKS * LIST_ITEM_H) / 2;
        if (iy < LIST_Y) iy = LIST_Y;

        /* Highlight selected track. */
        if (i == cur_track) {
            fill_rect(buf, stride_px,
                      LIST_X, iy, LIST_W, LIST_ITEM_H - 1, C_LIST_SEL);
        }

        /* Track number + name. */
        char num_buf[4];
        num_buf[0] = '1' + (char)i;
        num_buf[1] = '.';
        num_buf[2] = ' ';
        num_buf[3] = '\0';
        int tx = LIST_X + 4;
        int ty = iy + (LIST_ITEM_H - FONT_H) / 2;
        font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                         tx, ty, num_buf,
                         (i == cur_track) ? C_TEXT_BRIGHT : C_TEXT_DIM);
        font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                         tx + 3 * FONT_W, ty, tracks[i].name,
                         (i == cur_track) ? C_TEXT_BRIGHT : C_TEXT_DIM);

        /* Playing indicator. */
        if (i == cur_track && play_state == PS_PLAYING) {
            font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                             WIN_W - 24, ty, ">", 0xFF88FF88u);
        }
    }
}

/* -------------------------------------------------------------------------
 * Entry point.
 * ---------------------------------------------------------------------- */
void _start(void)
{
    serial("[MUSIC] starting\n");

    if (wl_connect() != 0) {
        serial("[MUSIC] wl_connect FAILED\n");
        for (;;) sys_yield();
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Music Player");
    if (!win) {
        serial("[MUSIC] wl_create_window FAILED\n");
        for (;;) sys_yield();
    }

    u32 stride_px = win->stride / 4u;

    /* Probe audio once (cached). */
    (void)audio_available();

    /* Button descriptors. */
    Button btn_prev = { BTN_PREV_X, BTN_Y, "Prev", 0 };
    Button btn_play = { BTN_PLAY_X, BTN_Y, "Play", 0 };
    Button btn_stop = { BTN_STOP_X, BTN_Y, "Stop", 0 };
    Button btn_next = { BTN_NEXT_X, BTN_Y, "Next", 0 };

    /* Initialise visualizer bars to zero. */
    for (int i = 0; i < VIS_BARS; i++) bar_energy[i] = 0;

    /* ---- Main frame loop. ---- */
    for (;;) {
        /* 1. Poll all pending input events. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea;
                i32 my = (i32)eb;
                int btn_down = (ec & 1);

                /* Update hover state. */
                btn_prev.hot = hit_button(&btn_prev, mx, my);
                btn_play.hot = hit_button(&btn_play, mx, my);
                btn_stop.hot = hit_button(&btn_stop, mx, my);
                btn_next.hot = hit_button(&btn_next, mx, my);

                if (btn_down) {
                    /* Transport buttons. */
                    if (hit_button(&btn_prev, mx, my)) {
                        stop_play();
                        cur_track = (cur_track + NUM_TRACKS - 1) % NUM_TRACKS;
                        audio_sfx_click();
                    } else if (hit_button(&btn_play, mx, my)) {
                        if (play_state == PS_STOPPED) {
                            serial("[MUSIC] play ");
                            serial(tracks[cur_track].name);
                            serial("\n");
                            start_play();
                            btn_play.label = "Pause";
                        } else if (play_state == PS_PLAYING) {
                            pause_play();
                            btn_play.label = "Play";
                        } else {
                            /* Paused -> resume. */
                            pause_play();
                            btn_play.label = "Pause";
                        }
                    } else if (hit_button(&btn_stop, mx, my)) {
                        stop_play();
                        btn_play.label = "Play";
                        audio_sfx_click();
                    } else if (hit_button(&btn_next, mx, my)) {
                        stop_play();
                        cur_track = (cur_track + 1) % NUM_TRACKS;
                        audio_sfx_click();
                    } else {
                        /* Check track list click. */
                        if (mx >= LIST_X && mx < LIST_X + LIST_W
                         && my >= LIST_Y  && my < LIST_Y + LIST_H)
                        {
                            int rel_y = my - LIST_Y
                                        - (LIST_H - NUM_TRACKS * LIST_ITEM_H) / 2;
                            if (rel_y >= 0) {
                                int clicked = rel_y / LIST_ITEM_H;
                                if (clicked >= 0 && clicked < NUM_TRACKS) {
                                    stop_play();
                                    cur_track = clicked;
                                    btn_play.label = "Play";
                                    audio_sfx_click();
                                }
                            }
                        }
                    }
                }
            }
            /* Key events not used; silently ignored. */
        }

        /* 2. Run non-blocking note scheduler. */
        if (scheduler_tick()) {
            /* Note advanced; update play button label if we stopped. */
            if (play_state == PS_STOPPED)
                btn_play.label = "Play";
        }

        /* 3. Decay visualizer bars every frame. */
        vis_decay();

        /* 4. Render. */
        draw_header(win->pixels, stride_px);
        draw_vis_background(win->pixels, stride_px);
        draw_vis_bars(win->pixels, stride_px);
        draw_info(win->pixels, stride_px);
        draw_transport(win->pixels, stride_px,
                       &btn_prev, &btn_play, &btn_stop, &btn_next);
        draw_tracklist(win->pixels, stride_px);

        wl_commit(win);
        sys_yield();
    }
}
