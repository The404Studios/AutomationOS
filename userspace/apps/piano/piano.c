/*
 * piano.c -- Two-octave piano keyboard app (freestanding, ring 3).
 * =================================================================
 *
 * Window: ~560 x 260 px, title "Piano".
 * Draws two octaves of white/black piano keys (C4-B5, 14 white + 10 black).
 * Computer keys map to notes via the prompt layout:
 *   a w s e d f t g y h u j k  ->  C C# D D# E F F# G G# A A# B C  (C4-C5)
 *
 * Audio via SYS_BEEP = 45.  freq = 440 * 2^((n-69)/12) for MIDI note n,
 * computed with a fixed-point semitone-ratio lookup table (no libm).
 * If SYS_BEEP returns negative, the app continues as a silent visual piano.
 *
 * "Demo" button plays C D E F G (C major scale) with 300 ms gaps.
 *
 * Build (flags DIRECTLY on cmdline -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/piano/piano.c -o /tmp/pn.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld /tmp/pn.o /tmp/wlc.o /tmp/bf.o -o /tmp/pn.elf
 *   objdump -d /tmp/pn.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [PIANO] starting
 *   [PIANO] beep avail=Y   (or =N if SYS_BEEP not wired)
 *   [PIANO] note <name> freq <hz>
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline helper (3-arg form).
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_BEEP         45   /* (freq_hz, duration_ms, 0); -ENOTSUP if unwired */

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
 * Minimal freestanding helpers (no libc).
 * --------------------------------------------------------------------- */
typedef unsigned int   u32;
typedef int            i32;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/* Print a decimal integer >= 0 to serial. */
static void serial_num(long n)
{
    char b[24];
    int  i = 0;
    if (n < 0) { serial("-"); n = -n; }
    if (n == 0) { char z = '0'; sc(SYS_WRITE, 1, (long)&z, 1); return; }
    do { b[i++] = (char)('0' + (int)(n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1); }
}

/* -----------------------------------------------------------------------
 * Frequency computation -- fixed-point, no libm.
 *
 * Equal temperament: each semitone = 2^(1/12) above the previous.
 * We pre-compute the 12 ratios as integer millihertz multiplied by 1000
 * (i.e., ratio * 1000), then build frequencies from A4=440 Hz.
 *
 * MIDI note n:  freq_hz = 440 * 2^((n-69)/12)
 *
 * Strategy:
 *   - Express n-69 = 12*octaves + semi  (octaves can be negative)
 *   - base_mHz = semitone_ratio_m1000[semi] * 440   (unit: mHz * 1000)
 *   - Double or halve base_mHz for each octave.
 *   - Round to Hz.
 *
 * semitone_ratio_m1000[k] = round(1000 * 2^(k/12)) for k=0..11:
 *   0:1000  1:1059  2:1122  3:1189  4:1260  5:1335
 *   6:1414  7:1498  8:1587  9:1682  10:1782  11:1888
 * --------------------------------------------------------------------- */
static const int semitone_ratio_m1000[12] = {
    1000, 1059, 1122, 1189, 1260, 1335,
    1414, 1498, 1587, 1682, 1782, 1888
};

/* Compute frequency in Hz for MIDI note n (integer arithmetic). */
static int midi_to_freq(int midi_note)
{
    /* delta from A4 (MIDI 69) */
    int delta = midi_note - 69;

    /* Normalise to range [0..11] semitones + octave offset from A4 */
    int semi = delta % 12;
    int octave = delta / 12;
    if (semi < 0) { semi += 12; octave -= 1; }

    /* base = 440 * ratio / 1000  (keep extra precision by working in 10ths) */
    long base_hz10 = (long)440 * semitone_ratio_m1000[semi]; /* = Hz * 1000 */

    /* Apply octave shifts */
    if (octave >= 0) {
        int i;
        for (i = 0; i < octave; i++) base_hz10 *= 2;
    } else {
        int i;
        for (i = 0; i < -octave; i++) base_hz10 /= 2;
    }

    /* Convert from milliHz to Hz, rounding */
    return (int)((base_hz10 + 500) / 1000);
}

/* -----------------------------------------------------------------------
 * Note table.
 *
 * We display two octaves: C4 (MIDI 60) through B5 (MIDI 83) = 24 semitones.
 * Index 0 = C4, index 23 = B5.  Index 24 is C6 sentinel for the right edge.
 * --------------------------------------------------------------------- */
#define NUM_NOTES     24    /* C4..B5 */

static const char *note_names[25] = {  /* 24 + sentinel C6 */
    "C4","C#4","D4","D#4","E4","F4","F#4","G4","G#4","A4","A#4","B4",
    "C5","C#5","D5","D#5","E5","F5","F#5","G5","G#5","A5","A#5","B5",
    "C6"
};

/* Short key labels for white keys (used on the drawn key). */
static const char *note_label[25] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B",
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B",
    "C"
};

/* MIDI base note for index 0 (C4). */
#define MIDI_BASE 60

/* -----------------------------------------------------------------------
 * Piano key layout.
 *
 * 2 octaves = 14 white keys (C D E F G A B  x2).
 * White key: 36 px wide, 150 px tall.  1 px gap between keys.
 * Black key: 22 px wide, 95 px tall, centred between adjacent white keys.
 *
 * Keys start at (KEYS_X0, KEYS_Y0).
 * --------------------------------------------------------------------- */
#define WIN_W    560
#define WIN_H    260

#define KEYS_X0  10
#define KEYS_Y0  60    /* below title/info bar */

#define WK_W     36
#define WK_H    150
#define WK_GAP    1

#define BK_W     22
#define BK_H     95

/* White-key semitone offsets within one octave (7 per octave). */
static const int white_off[7] = { 0, 2, 4, 5, 7, 9, 11 };
/* Black-key semitone offsets within one octave (5 per octave). */
static const int black_off[5] = { 1, 3, 6, 8, 10 };
/* Which white key index is to the left of each black key. */
static const int black_after_w[5] = { 0, 1, 3, 4, 5 };

/* Total number of white / black keys across 2 octaves. */
#define NUM_WHITE 14
#define NUM_BLACK 10

/*
 * White key i: semitone index = (i / 7) * 12 + white_off[i % 7]
 * Black key i: semitone index = (i / 5) * 12 + black_off[i % 5]
 */
static int white_semi(int i) {
    return (i / 7) * 12 + white_off[i % 7];
}
static int black_semi(int i) {
    return (i / 5) * 12 + black_off[i % 5];
}
static i32 white_key_x(int i) {
    return KEYS_X0 + i * (WK_W + WK_GAP);
}
static i32 black_key_x(int i) {
    int octave = i / 5;
    int b      = i % 5;
    /* white-key index for the left neighbour */
    int w_left = octave * 7 + black_after_w[b];
    return white_key_x(w_left) + WK_W - BK_W / 2;
}

/* -----------------------------------------------------------------------
 * Demo button geometry.
 * --------------------------------------------------------------------- */
#define DEMO_X   (KEYS_X0 + NUM_WHITE * (WK_W + WK_GAP) + 6)
#define DEMO_Y   (KEYS_Y0 + (WK_H / 2) - 12)
#define DEMO_W   52
#define DEMO_H   24

/* -----------------------------------------------------------------------
 * Highlight state.  highlight_until[note_index] = tick-ms until fade.
 * --------------------------------------------------------------------- */
#define HIGHLIGHT_MS  220
static long highlight_until[NUM_NOTES];   /* zero-init (BSS) */

/* -----------------------------------------------------------------------
 * Keycode -> semitone/note-index map.
 *
 * The prompt mapping: a w s e d f t g y h u j k = C C# D D# E F F# G G# A A# B C
 * Maps to note indices 0-12 (C4..C5).  C5 top maps to index 12.
 *
 * PS/2 scancodes (set 1 make codes, as used in synth.c):
 *   a=30  w=17  s=31  e=18  d=32  f=33  t=20  g=34  y=21  h=35  u=22  j=36  k=37
 * --------------------------------------------------------------------- */
#define K_A 30
#define K_W 17
#define K_S 31
#define K_E 18
#define K_D 32
#define K_F 33
#define K_T 20
#define K_G 34
#define K_Y 21
#define K_H 35
#define K_U 22
#define K_J 36
#define K_K 37

static int keycode_to_note(int kc)
{
    /* Returns note index into [0..NUM_NOTES), or -1. */
    switch (kc) {
    case K_A: return  0;  /* C4   */
    case K_W: return  1;  /* C#4  */
    case K_S: return  2;  /* D4   */
    case K_E: return  3;  /* D#4  */
    case K_D: return  4;  /* E4   */
    case K_F: return  5;  /* F4   */
    case K_T: return  6;  /* F#4  */
    case K_G: return  7;  /* G4   */
    case K_Y: return  8;  /* G#4  */
    case K_H: return  9;  /* A4   */
    case K_U: return 10;  /* A#4  */
    case K_J: return 11;  /* B4   */
    case K_K: return 12;  /* C5   */
    default:  return -1;
    }
}

/* -----------------------------------------------------------------------
 * Mouse hit-test.  Black keys have priority (drawn on top).
 * Returns note index in [0..NUM_NOTES), or -1.
 * --------------------------------------------------------------------- */
static int hit_test(int mx, int my)
{
    /* Black keys first */
    int i;
    for (i = 0; i < NUM_BLACK; i++) {
        i32 kx = black_key_x(i);
        if (mx >= kx && mx < kx + BK_W &&
            my >= KEYS_Y0 && my < KEYS_Y0 + BK_H)
            return black_semi(i);
    }
    /* White keys */
    for (i = 0; i < NUM_WHITE; i++) {
        i32 kx = white_key_x(i);
        if (mx >= kx && mx < kx + WK_W &&
            my >= KEYS_Y0 && my < KEYS_Y0 + WK_H)
            return white_semi(i);
    }
    return -1;
}

static int hit_demo(int mx, int my)
{
    return (mx >= DEMO_X && mx < DEMO_X + DEMO_W &&
            my >= DEMO_Y && my < DEMO_Y + DEMO_H);
}

/* -----------------------------------------------------------------------
 * Drawing helpers.
 * --------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void rect_outline(u32 *buf, u32 bw, u32 bh, u32 spx,
                         i32 x, i32 y, i32 w, i32 h, u32 color)
{
    fill_rect(buf, bw, bh, spx, x,     y,     w,  1, color);
    fill_rect(buf, bw, bh, spx, x,     y+h-1, w,  1, color);
    fill_rect(buf, bw, bh, spx, x,     y,     1,  h, color);
    fill_rect(buf, bw, bh, spx, x+w-1, y,     1,  h, color);
}

/* Draw a "glossy" white key: light face + subtle gradient at top + border. */
static void draw_white_key(u32 *buf, u32 bw, u32 bh, u32 spx,
                           i32 kx, int hl)
{
    u32 face  = hl ? 0xFF88DDFF : 0xFFEEEEEE;
    u32 shine = hl ? 0xFFAAEEFF : 0xFFFFFFFF;  /* lighter top strip */
    u32 shade = hl ? 0xFF55AACC : 0xFFCCCCCC;  /* slightly darker bottom */

    /* main face */
    fill_rect(buf, bw, bh, spx, kx+1, KEYS_Y0+1, WK_W-2, WK_H-2, face);
    /* top "gloss" strip (3 px) */
    fill_rect(buf, bw, bh, spx, kx+1, KEYS_Y0+1, WK_W-2, 3, shine);
    /* bottom shadow strip (6 px) */
    fill_rect(buf, bw, bh, spx, kx+1, KEYS_Y0+WK_H-7, WK_W-2, 6, shade);
    /* border */
    rect_outline(buf, bw, bh, spx, kx, KEYS_Y0, WK_W, WK_H, 0xFF444444);
}

/* Draw a "glossy" black key. */
static void draw_black_key(u32 *buf, u32 bw, u32 bh, u32 spx,
                           i32 kx, int hl)
{
    u32 face  = hl ? 0xFF005599 : 0xFF222222;
    u32 shine = hl ? 0xFF0077BB : 0xFF3A3A3A;
    u32 edge  = 0xFF000000;

    fill_rect(buf, bw, bh, spx, kx, KEYS_Y0, BK_W, BK_H, face);
    /* left gloss edge (3 px) */
    fill_rect(buf, bw, bh, spx, kx+2, KEYS_Y0+2, 3, BK_H-10, shine);
    rect_outline(buf, bw, bh, spx, kx, KEYS_Y0, BK_W, BK_H, edge);
}

/* -----------------------------------------------------------------------
 * Current note display state (shown in title bar).
 * --------------------------------------------------------------------- */
static const char *current_note_name = "---";
static int         current_note_freq = 0;

/* -----------------------------------------------------------------------
 * Probe beep availability at startup by calling SYS_BEEP(0,0,0).
 * Returns 1 if wired, 0 if not.
 * --------------------------------------------------------------------- */
static int beep_avail = 0;

static int probe_beep(void)
{
    long r = sc(SYS_BEEP, 0, 0, 0);
    /* A return of -ENOTSUP or any clearly negative value = not wired. */
    return (r >= 0) ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Play a note: set highlight, serial-print, call SYS_BEEP if available.
 * note_idx in [0..NUM_NOTES).
 * --------------------------------------------------------------------- */
static void play_note(int note_idx, long now_ms)
{
    if (note_idx < 0 || note_idx >= NUM_NOTES) return;

    int freq = midi_to_freq(MIDI_BASE + note_idx);

    serial("[PIANO] note ");
    serial(note_names[note_idx]);
    serial(" freq ");
    serial_num((long)freq);
    serial("\n");

    current_note_name = note_names[note_idx];
    current_note_freq = freq;
    highlight_until[note_idx] = now_ms + HIGHLIGHT_MS;

    if (beep_avail)
        sc(SYS_BEEP, (long)freq, 180, 0);
}

/* -----------------------------------------------------------------------
 * Demo melody: C D E F G (note indices 0 2 4 5 7 = C4 D4 E4 F4 G4).
 * Busy-wait between notes using SYS_GET_TICKS_MS to avoid sleeping in loop.
 * --------------------------------------------------------------------- */
static void play_demo(void)
{
    static const int melody[] = { 0, 2, 4, 5, 7 };
    static const int n_notes  = 5;
    int i;
    for (i = 0; i < n_notes; i++) {
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        play_note(melody[i], now);
        /* Wait ~300 ms between notes. */
        long start = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        long until = start + 300;
        long t;
        do {
            sc(SYS_YIELD, 0, 0, 0);
            t = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        } while (t < until);
    }
}

/* -----------------------------------------------------------------------
 * Render one full frame.
 * --------------------------------------------------------------------- */
static void render(wl_window *win, long now_ms)
{
    u32 *buf = win->pixels;
    u32  bw  = win->w;
    u32  bh  = win->h;
    u32  spx = win->stride / 4u;

    /* --- Background --- */
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, (i32)bh, 0xFF1A1A2E);

    /* --- Title / info bar --- */
    /* Dark panel */
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, KEYS_Y0 - 2, 0xFF14142A);
    fill_rect(buf, bw, bh, spx, 0, KEYS_Y0-2, (i32)bw, 2, 0xFF333366);

    /* App name */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     8, 6, "Piano", 0xFFE0E0FF);

    /* Current note display */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     8, 26, "Note: ", 0xFF9999CC);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     56, 26, current_note_name, 0xFFFFDD44);

    /* Frequency display */
    if (current_note_freq > 0) {
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         130, 26, "Hz: ", 0xFF9999CC);
        /* format frequency into a small static buffer */
        char fbuf[12];
        int  fi  = 0;
        int  tmp = current_note_freq;
        if (tmp == 0) {
            fbuf[fi++] = '0';
        } else {
            char rev[10];
            int  ri = 0;
            while (tmp > 0) { rev[ri++] = (char)('0' + tmp % 10); tmp /= 10; }
            while (ri > 0)  { fbuf[fi++] = rev[--ri]; }
        }
        fbuf[fi] = '\0';
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         162, 26, fbuf, 0xFFFFDD44);
    }

    /* Key hint line */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     8, 44, "Keys: a w s e d f t g y h u j k", 0xFF666699);

    /* --- White keys (drawn first) --- */
    int i;
    for (i = 0; i < NUM_WHITE; i++) {
        int  semi = white_semi(i);
        int  hl   = (semi < NUM_NOTES && highlight_until[semi] > now_ms);
        draw_white_key(buf, bw, bh, spx, white_key_x(i), hl);

        /* Note label at the bottom of the white key */
        const char *lbl = note_label[semi];
        int lbl_w = (int)k_strlen(lbl) * FONT_W;
        int lx    = white_key_x(i) + (WK_W - lbl_w) / 2;
        int ly    = KEYS_Y0 + WK_H - FONT_H - 4;
        u32 lcol  = hl ? 0xFF003366 : 0xFF444444;
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         lx, ly, lbl, lcol);
    }

    /* --- Black keys (drawn on top) --- */
    for (i = 0; i < NUM_BLACK; i++) {
        int semi = black_semi(i);
        int hl   = (semi < NUM_NOTES && highlight_until[semi] > now_ms);
        draw_black_key(buf, bw, bh, spx, black_key_x(i), hl);

        /* Short label at bottom of black key */
        const char *lbl = note_label[semi];
        /* For black keys just show the sharp symbol character '#' would
           exceed font width — just omit label to keep it tidy */
        (void)lbl;   /* label omitted on black keys for cleanliness */
    }

    /* --- Demo button --- */
    fill_rect(buf, bw, bh, spx, DEMO_X, DEMO_Y, DEMO_W, DEMO_H, 0xFF2A4A7A);
    rect_outline(buf, bw, bh, spx, DEMO_X, DEMO_Y, DEMO_W, DEMO_H, 0xFF5577BB);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     DEMO_X + 6, DEMO_Y + (DEMO_H - FONT_H) / 2,
                     "Demo", 0xFFCCDDFF);

    /* Beep status indicator (small) */
    const char *bav = beep_avail ? "SND:ON" : "SND:--";
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     DEMO_X, DEMO_Y + DEMO_H + 6, bav,
                     beep_avail ? 0xFF44BB44 : 0xFF886644);
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial("[PIANO] starting\n");

    /* Probe SYS_BEEP availability */
    beep_avail = probe_beep();
    if (beep_avail) {
        serial("[PIANO] beep avail=Y\n");
    } else {
        serial("[PIANO] beep avail=N (visual-only mode)\n");
    }

    if (wl_connect() != 0) {
        serial("[PIANO] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Piano");
    if (!win) {
        serial("[PIANO] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    serial("[PIANO] window ready\n");

    int prev_buttons = 0;

    for (;;) {
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);

        /* Drain all pending events. */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                /* a = keycode, b = 1 pressed / 0 released */
                if (b == 1) {
                    int ni = keycode_to_note(a);
                    if (ni >= 0)
                        play_note(ni, now);
                }
            } else if (kind == WL_EVENT_POINTER) {
                /* a=x, b=y, c=buttons bitmask; bit 0 = left button */
                int cur_btn  = c & 1;
                int prev_btn = prev_buttons & 1;
                if (cur_btn && !prev_btn) {
                    /* Left button just pressed */
                    if (hit_demo(a, b)) {
                        /* Play demo melody (blocking with yields). */
                        play_demo();
                        /* Refresh now after the demo loop. */
                        now = sc(SYS_GET_TICKS_MS, 0, 0, 0);
                    } else {
                        int ni = hit_test(a, b);
                        if (ni >= 0)
                            play_note(ni, now);
                    }
                }
                prev_buttons = c;
            }
        }

        render(win, now);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0);
    }
}
