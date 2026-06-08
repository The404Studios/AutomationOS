/*
 * synth.c -- Musical keyboard synthesizer app (freestanding, ring 3).
 * ====================================================================
 *
 * Draws a one-octave piano keyboard (C4 to C5) in a 520x220 window.
 * Computer keyboard keys A S D F G H J K  map to white keys C D E F G A B C5.
 * Keys W E T Y U  map to black keys C# D# F# G# A#.
 * Mouse click hit-tests the drawn key rects and plays the note.
 *
 * Audio: SYS_BEEP(freq_hz, ms) = syscall 45.  If it returns < 0 (not wired),
 * the key is still highlighted and the note is printed to serial.
 *
 * Build (flags DIRECTLY on cmdline -- never via shell vars):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/synth/synth.c       -o /tmp/syn.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c       -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c        -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld /tmp/syn.o /tmp/wlc.o /tmp/bf.o -o /tmp/syn.elf
 *   objdump -d /tmp/syn.elf | grep fs:0x28    # must be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers (must match kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40
#define SYS_BEEP         45   /* HDA tone: (freq_hz, ms); kernel/include/syscall.h */

typedef unsigned int   u32;
typedef int            i32;

/* Three-argument inline syscall (rdi, rsi, rdx). */
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
 * Minimal serial helpers (no libc).
 * --------------------------------------------------------------------- */
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

/* Print a decimal integer to serial. */
static void serial_num(long n)
{
    char b[24];
    int i = 0;
    if (n < 0) { serial("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1); }
}

/* -----------------------------------------------------------------------
 * Note / frequency table.
 *
 * Equal temperament, C4 = 262 Hz (rounded from 261.63 Hz).
 * Semitone offsets from C4: C=0, C#=1, D=2, D#=3, E=4, F=5, F#=6,
 *                           G=7, G#=8, A=9, A#=10, B=11, C5=12.
 * freq[i] = round(261.63 * 2^(i/12)), pre-computed in integer Hz.
 * --------------------------------------------------------------------- */
#define NUM_SEMITONES 13   /* C4 through C5 inclusive */

static const int note_freq[NUM_SEMITONES] = {
    262,  /* C4  semitone  0 */
    277,  /* C#4 semitone  1 */
    294,  /* D4  semitone  2 */
    311,  /* D#4 semitone  3 */
    330,  /* E4  semitone  4 */
    349,  /* F4  semitone  5 */
    370,  /* F#4 semitone  6 */
    392,  /* G4  semitone  7 */
    415,  /* G#4 semitone  8 */
    440,  /* A4  semitone  9 */
    466,  /* A#4 semitone 10 */
    494,  /* B4  semitone 11 */
    523,  /* C5  semitone 12 */
};

static const char *note_name[NUM_SEMITONES] = {
    "C4", "C#4", "D4", "D#4", "E4", "F4",
    "F#4", "G4", "G#4", "A4", "A#4", "B4", "C5"
};

/* Short label shown on the key (fits in one 8-px glyph column). */
static const char *note_label[NUM_SEMITONES] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B", "C"
};

/* -----------------------------------------------------------------------
 * Key layout -- white keys and black keys.
 *
 * Window: 520 x 220 px.
 * 8 white keys, each 60 px wide x 160 px tall, starting at x=10, y=30.
 * Black keys: 36 px wide x 100 px tall, centred over the gap.
 * --------------------------------------------------------------------- */
#define WIN_W  520
#define WIN_H  220

/* White key positions (C D E F G A B C5) -> semitones 0,2,4,5,7,9,11,12 */
#define NUM_WHITE 8
static const int white_semi[NUM_WHITE] = { 0, 2, 4, 5, 7, 9, 11, 12 };

/* Black key positions (C# D# F# G# A#) -> semitones 1,3,6,8,10 */
#define NUM_BLACK 5
static const int black_semi[NUM_BLACK] = { 1, 3, 6, 8, 10 };

/* White key pixel rect origin */
#define WK_X0    10
#define WK_Y0    30
#define WK_W     60
#define WK_H    160
#define WK_GAP    2    /* gap between white keys */

/* Black key pixel dimensions */
#define BK_W     36
#define BK_H    100

/* Compute left edge of the i-th white key (0-based). */
static i32 white_key_x(int i)
{
    return WK_X0 + i * (WK_W + WK_GAP);
}

/*
 * Black key x positions (relative to window).
 * Gaps between white keys are at offsets:
 *   C/C#: after key 0  -> x = white_key_x(0) + WK_W - BK_W/2
 *   D/D#: after key 1  -> white_key_x(1) + WK_W - BK_W/2
 *   F/F#: after key 3
 *   G/G#: after key 4
 *   A/A#: after key 5
 * Index into that array:
 */
static const int black_after_white[NUM_BLACK] = { 0, 1, 3, 4, 5 };

static i32 black_key_x(int i)
{
    return white_key_x(black_after_white[i]) + WK_W - BK_W / 2;
}

/* -----------------------------------------------------------------------
 * Key highlight state.
 * highlight_until[semitone] stores the tick-ms at which to stop highlight.
 * --------------------------------------------------------------------- */
#define HIGHLIGHT_MS 180

static long highlight_until[NUM_SEMITONES];   /* zero-init is fine (bss) */

/* -----------------------------------------------------------------------
 * Drawing helpers.
 * --------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w;  if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h;  if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void draw_rect_outline(u32 *buf, u32 bw, u32 bh, u32 spx,
                               i32 x, i32 y, i32 w, i32 h, u32 color)
{
    fill_rect(buf, bw, bh, spx, x,       y,       w, 1, color);
    fill_rect(buf, bw, bh, spx, x,       y+h-1,   w, 1, color);
    fill_rect(buf, bw, bh, spx, x,       y,       1, h, color);
    fill_rect(buf, bw, bh, spx, x+w-1,   y,       1, h, color);
}

/* -----------------------------------------------------------------------
 * Keyboard color palette.
 * --------------------------------------------------------------------- */
#define COL_BG          0xFF1A1A2Eu  /* dark navy background           */
#define COL_TITLE       0xFFE0E0FFu  /* title text                     */
#define COL_WHITE_KEY   0xFFEEEEEEu  /* white key normal               */
#define COL_WHITE_HL    0xFF88DDFFu  /* white key highlighted          */
#define COL_WHITE_EDGE  0xFF444444u  /* white key border               */
#define COL_BLACK_KEY   0xFF222222u  /* black key normal               */
#define COL_BLACK_HL    0xFF005599u  /* black key highlighted          */
#define COL_BLACK_EDGE  0xFF000000u  /* black key border               */
#define COL_LABEL_W     0xFF222222u  /* label on white key             */
#define COL_LABEL_B     0xFFCCCCCCu  /* label on black key             */
#define COL_HINT        0xFF888888u  /* keyboard shortcut hint text    */

/* -----------------------------------------------------------------------
 * Render one complete frame.
 * --------------------------------------------------------------------- */
static void render(wl_window *win, long now_ms)
{
    u32 *buf   = win->pixels;
    u32  bw    = win->w;
    u32  bh    = win->h;
    u32  spx   = win->stride / 4u;

    /* Background */
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, (i32)bh, COL_BG);

    /* Title */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     10, 6, "SYNTH  --  one-octave piano  (C4-C5)",
                     COL_TITLE);

    /* --- White keys --- */
    for (int i = 0; i < NUM_WHITE; i++) {
        int  semi = white_semi[i];
        i32  kx   = white_key_x(i);
        int  hl   = (highlight_until[semi] > now_ms);
        u32  fill = hl ? COL_WHITE_HL : COL_WHITE_KEY;

        fill_rect(buf, bw, bh, spx, kx, WK_Y0, WK_W, WK_H, fill);
        draw_rect_outline(buf, bw, bh, spx, kx, WK_Y0, WK_W, WK_H, COL_WHITE_EDGE);

        /* Note label, centred horizontally near the bottom */
        int lx = kx + (WK_W - (int)k_strlen(note_label[semi]) * FONT_W) / 2;
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         lx, WK_Y0 + WK_H - FONT_H - 4,
                         note_label[semi], COL_LABEL_W);
    }

    /* --- Black keys (drawn on top) --- */
    for (int i = 0; i < NUM_BLACK; i++) {
        int  semi = black_semi[i];
        i32  kx   = black_key_x(i);
        int  hl   = (highlight_until[semi] > now_ms);
        u32  fill = hl ? COL_BLACK_HL : COL_BLACK_KEY;

        fill_rect(buf, bw, bh, spx, kx, WK_Y0, BK_W, BK_H, fill);
        draw_rect_outline(buf, bw, bh, spx, kx, WK_Y0, BK_W, BK_H, COL_BLACK_EDGE);

        /* Short label */
        int lx = kx + (BK_W - (int)k_strlen(note_label[semi]) * FONT_W) / 2;
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         lx, WK_Y0 + BK_H - FONT_H - 2,
                         note_label[semi], COL_LABEL_B);
    }

    /* Key-binding hint line below the keyboard */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     WK_X0, WK_Y0 + WK_H + 6,
                     "Keys: A S D F G H J K  |  Sharps: W E T Y U",
                     COL_HINT);
}

/* -----------------------------------------------------------------------
 * Play a note: print to serial, set highlight, call SYS_BEEP.
 * --------------------------------------------------------------------- */
static void play_note(int semi, long now_ms)
{
    serial("[SYNTH] note ");
    serial(note_name[semi]);
    serial(" ");
    serial_num((long)note_freq[semi]);
    serial("Hz\n");

    highlight_until[semi] = now_ms + HIGHLIGHT_MS;

    long rc = sc(SYS_BEEP, (long)note_freq[semi], 200, 0);
    if (rc < 0) {
        /* SYS_BEEP not wired yet -- visual-only mode, note still logged */
        serial("[SYNTH] SYS_BEEP not wired (audio unavailable), visual only\n");
    }
}

/* -----------------------------------------------------------------------
 * Keyboard scancode -> semitone mapping.
 * Include kernel keycodes.
 * --------------------------------------------------------------------- */

/* KEY_* values from kernel/include/input.h */
#define K_A  30
#define K_S  31
#define K_D  32
#define K_F  33
#define K_G  34
#define K_H  35
#define K_J  36
#define K_K  37
#define K_W  17
#define K_E  18
#define K_T  20
#define K_Y  21
#define K_U  22

static int keycode_to_semitone(int kc)
{
    switch (kc) {
    /* White keys: C D E F G A B C5 */
    case K_A: return  0;  /* C4  */
    case K_S: return  2;  /* D4  */
    case K_D: return  4;  /* E4  */
    case K_F: return  5;  /* F4  */
    case K_G: return  7;  /* G4  */
    case K_H: return  9;  /* A4  */
    case K_J: return 11;  /* B4  */
    case K_K: return 12;  /* C5  */
    /* Black keys: C# D# F# G# A# */
    case K_W: return  1;  /* C#4 */
    case K_E: return  3;  /* D#4 */
    case K_T: return  6;  /* F#4 */
    case K_Y: return  8;  /* G#4 */
    case K_U: return 10;  /* A#4 */
    default:  return -1;
    }
}

/* -----------------------------------------------------------------------
 * Mouse hit-test: returns semitone or -1.
 * Check black keys first (they sit on top).
 * --------------------------------------------------------------------- */
static int hit_test(int mx, int my)
{
    /* Black keys (higher visual priority -- drawn on top) */
    for (int i = 0; i < NUM_BLACK; i++) {
        i32 kx = black_key_x(i);
        if (mx >= kx && mx < kx + BK_W &&
            my >= WK_Y0 && my < WK_Y0 + BK_H)
            return black_semi[i];
    }
    /* White keys */
    for (int i = 0; i < NUM_WHITE; i++) {
        i32 kx = white_key_x(i);
        if (mx >= kx && mx < kx + WK_W &&
            my >= WK_Y0 && my < WK_Y0 + WK_H)
            return white_semi[i];
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial("[SYNTH] starting\n");

    if (wl_connect() != 0) {
        serial("[SYNTH] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Synth");
    if (!win) {
        serial("[SYNTH] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    serial("[SYNTH] window ready -- piano keys C4-C5\n");

    /* Track mouse button state to detect button-down transitions. */
    int prev_buttons = 0;

    for (;;) {
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);

        /* Drain all pending events. */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                /* a = keycode, b = pressed (1) / released (0) */
                if (b == 1) {
                    int semi = keycode_to_semitone(a);
                    if (semi >= 0)
                        play_note(semi, now);
                }
            } else if (kind == WL_EVENT_POINTER) {
                /* a=x, b=y, c=buttons bitmask */
                int cur_btn = c & 1;            /* left button */
                int prev_btn = prev_buttons & 1;
                if (cur_btn && !prev_btn) {
                    /* Button just pressed */
                    int semi = hit_test(a, b);
                    if (semi >= 0)
                        play_note(semi, now);
                }
                prev_buttons = c;
            }
        }

        render(win, now);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0);
    }
}
