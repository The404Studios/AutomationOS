/*
 * soundtest.c -- Audio library demo app (freestanding, ring 3).
 * =============================================================
 *
 * Window: 480 x 300 px, title "Sound Test".
 *
 * Layout:
 *   - Header bar (y 0-28): title + "Audio: ON" / "Audio: --" indicator.
 *   - SFX row  (y 32-72):  four buttons: Click | Coin | Jump | GameOver.
 *   - Piano row (y 80-134): one octave of C4-B4 (12 semitones as keys).
 *   - Melody button (y 142-178): "Play Melody" -- plays a recognisable
 *     C-major jingle (do re mi fa sol la si do).
 *   - Status line (y 188+): last action text.
 *
 * Audio is handled entirely by libauido (audio.h / audio.c).
 * If SYS_BEEP is not wired the indicator reads "Audio: --" and all buttons
 * remain visible but produce no sound and no crash.
 *
 * Build (flags DIRECTLY on cmdline):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/soundtest/soundtest.c -o /tmp/st.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/audio/audio.c -o /tmp/audio.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/st.o /tmp/audio.o /tmp/wlc.o /tmp/bf.o -o /tmp/soundtest.elf
 *   objdump -d /tmp/soundtest.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [SOUND] starting
 *   [SOUND] beep avail=Y   (or =N)
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/audio/audio.h"

/* -----------------------------------------------------------------------
 * Syscall inline helper (3-arg form, identical to paint.c / piano.c).
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

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
 * Minimal freestanding helpers.
 * --------------------------------------------------------------------- */
typedef unsigned int  u32;
typedef int           i32;

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

/* itoa: convert non-negative integer to decimal string in buf, return ptr. */
static char *itoa_buf(int v, char *buf, int bufsz)
{
    int i = bufsz - 1;
    buf[i] = '\0';
    if (v <= 0) { buf[--i] = '0'; return buf + i; }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    }
    return buf + i;
}

/* -----------------------------------------------------------------------
 * Window constants.
 * --------------------------------------------------------------------- */
#define WIN_W   480
#define WIN_H   300

/* -----------------------------------------------------------------------
 * Drawing primitives (same pattern as paint.c).
 * --------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    i32 yy, xx;
    for (yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * spx;
        for (xx = x1; xx < x2; xx++)
            row[xx] = col;
    }
}

static void hline(u32 *buf, u32 bw, u32 bh, u32 spx,
                  i32 x, i32 y, i32 len, u32 col)
{
    fill_rect(buf, bw, bh, spx, x, y, len, 1, col);
}

static void rect_outline(u32 *buf, u32 bw, u32 bh, u32 spx,
                         i32 x, i32 y, i32 w, i32 h, u32 col)
{
    hline(buf, bw, bh, spx, x,     y,     w, col);
    hline(buf, bw, bh, spx, x,     y+h-1, w, col);
    fill_rect(buf, bw, bh, spx, x,     y, 1, h, col);
    fill_rect(buf, bw, bh, spx, x+w-1, y, 1, h, col);
}

/* -----------------------------------------------------------------------
 * Button helpers.
 * --------------------------------------------------------------------- */
#define BTN_H       36
#define BTN_RADIUS  0      /* flat style */

/* Colors */
#define C_BG        0xFF1C1C2E   /* dark navy background */
#define C_PANEL     0xFF23233A   /* slightly lighter panel */
#define C_BTN       0xFF2E4070   /* button face */
#define C_BTN_HOT   0xFF3A5490   /* hover / pressed */
#define C_BTN_OUT   0xFF4A6AAA   /* button outline */
#define C_HEADER    0xFF14142A   /* header bar */
#define C_SEP       0xFF333366   /* separator line */
#define C_TEXT      0xFFDDDDFF   /* normal text */
#define C_DIM       0xFF777799   /* dimmed text */
#define C_AVAIL     0xFF44BB44   /* green "ON" */
#define C_UNAVAIL   0xFF886644   /* amber "--" */
#define C_KEY_WHITE 0xFFEEEEEE   /* piano white key */
#define C_KEY_BLACK 0xFF222222   /* piano black key */
#define C_KEY_LIT   0xFF5599FF   /* highlighted piano key */
#define C_STATUS    0xFF998888   /* status line text */

/* -----------------------------------------------------------------------
 * Section Y positions.
 * --------------------------------------------------------------------- */
#define HDR_H       30           /* header bar height */
#define SFX_Y       38           /* SFX button row top */
#define PIANO_Y     90           /* piano keys top */
#define PIANO_H     50           /* piano key height */
#define MELODY_Y    152          /* "Play Melody" button top */
#define STATUS_Y    200          /* status text top */

/* -----------------------------------------------------------------------
 * SFX button descriptors.
 * --------------------------------------------------------------------- */
#define NUM_SFX 4
static const char * const sfx_labels[NUM_SFX] = {
    "Click", "Coin", "Jump", "Game Over"
};
/* X positions computed at draw time; widths: */
#define SFX_BTN_W   100
#define SFX_BTN_GAP 8
/* total row width = NUM_SFX * SFX_BTN_W + (NUM_SFX-1) * SFX_BTN_GAP = 424
 * left margin = (480 - 424) / 2 = 28 */
#define SFX_ROW_X0  28

static i32 sfx_btn_x(int i) {
    return SFX_ROW_X0 + i * (SFX_BTN_W + SFX_BTN_GAP);
}

static int hit_sfx(i32 mx, i32 my)
{
    if (my < SFX_Y || my >= SFX_Y + BTN_H) return -1;
    int i;
    for (i = 0; i < NUM_SFX; i++) {
        i32 bx = sfx_btn_x(i);
        if (mx >= bx && mx < bx + SFX_BTN_W) return i;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Piano keys: one octave, C4 (MIDI 60) through B4 (MIDI 71).
 * White keys: C D E F G A B  (indices 0-6, semitones 0 2 4 5 7 9 11)
 * Black keys: C# D# F# G# A# (indices 0-4, semitones 1 3 6 8 10)
 * --------------------------------------------------------------------- */
#define NUM_WHITE   7
#define NUM_BLACK   5
#define WK_W        44     /* white key width  */
#define WK_GAP      2      /* gap between white keys */
#define BK_W        26     /* black key width  */
#define BK_H        30     /* black key height */

/* total piano row width = NUM_WHITE * (WK_W+WK_GAP) - WK_GAP = 314 */
#define PIANO_X0    ((WIN_W - (NUM_WHITE * (WK_W + WK_GAP) - WK_GAP)) / 2)  /* = 80 */

static const int white_semi[NUM_WHITE] = { 0, 2, 4, 5, 7, 9, 11 };
/* Black key x offsets from the START of the corresponding white key group.
 * Pattern: after key 0 (C), after key 1 (D), after key 3 (F), after key 4 (G), after key 5 (A) */
static const int black_after_white[NUM_BLACK] = { 0, 1, 3, 4, 5 };
static const int black_semi[NUM_BLACK]  = { 1, 3, 6, 8, 10 };

static i32 white_key_x(int i) {
    return PIANO_X0 + i * (WK_W + WK_GAP);
}

static i32 black_key_x(int i)
{
    /* Black key is centered over the gap between two adjacent white keys. */
    int w = black_after_white[i];
    return white_key_x(w) + WK_W - BK_W / 2;
}

/* Hit-test piano keys.  Returns semitone offset (0-11) or -1. */
static int hit_piano(i32 mx, i32 my)
{
    if (my < PIANO_Y || my >= PIANO_Y + PIANO_H) return -1;
    int i;
    /* Black keys have priority (drawn on top). */
    if (my < PIANO_Y + BK_H) {
        for (i = 0; i < NUM_BLACK; i++) {
            i32 bx = black_key_x(i);
            if (mx >= bx && mx < bx + BK_W)
                return black_semi[i];
        }
    }
    /* White keys. */
    for (i = 0; i < NUM_WHITE; i++) {
        i32 kx = white_key_x(i);
        if (mx >= kx && mx < kx + WK_W)
            return white_semi[i];
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Melody button.
 * --------------------------------------------------------------------- */
#define MELODY_BTN_W  180
#define MELODY_BTN_H   34
#define MELODY_BTN_X  ((WIN_W - MELODY_BTN_W) / 2)   /* = 150 */

static int hit_melody(i32 mx, i32 my)
{
    return (mx >= MELODY_BTN_X && mx < MELODY_BTN_X + MELODY_BTN_W &&
            my >= MELODY_Y      && my < MELODY_Y + MELODY_BTN_H);
}

/* -----------------------------------------------------------------------
 * Built-in jingle: C major scale up and back down.
 * do re mi fa sol la si do si la sol fa mi re do
 * MIDI: 60 62 64 65 67 69 71 72 71 69 67 65 64 62 60
 * --------------------------------------------------------------------- */
#define MELODY_LEN 15
static const int melody_notes[MELODY_LEN] = {
    60, 62, 64, 65, 67, 69, 71, 72,
    71, 69, 67, 65, 64, 62, 60
};
static const audio_u16 melody_durs[MELODY_LEN] = {
    200, 200, 200, 200, 200, 200, 200, 300,
    200, 200, 200, 200, 200, 200, 400
};

/* -----------------------------------------------------------------------
 * Application state.
 * --------------------------------------------------------------------- */
static int g_avail      = 0;
static int g_piano_lit  = -1;     /* semitone currently highlighted (-1 = none) */
static long g_lit_until = 0;     /* ticks until highlight clears */

/* Status message (shown at bottom, updated on button press). */
static const char *g_status = "Press a button or piano key.";

/* -----------------------------------------------------------------------
 * Render helpers.
 * --------------------------------------------------------------------- */

/* Draw a button with centered label. `hot` = 1 means pressed color. */
static void draw_btn(u32 *buf, u32 bw, u32 bh, u32 spx,
                     i32 x, i32 y, i32 w, i32 h,
                     const char *label, int hot)
{
    u32 face = hot ? C_BTN_HOT : C_BTN;
    fill_rect(buf, bw, bh, spx, x, y, w, h, face);
    rect_outline(buf, bw, bh, spx, x, y, w, h, C_BTN_OUT);

    /* Center label. */
    int lw = font_text_width(label);
    int lx = x + (w - lw) / 2;
    int ly = y + (h - FONT_H) / 2;
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     lx, ly, label, C_TEXT);
}

/* Draw the full scene. */
static void render(wl_window *win, long now_ms)
{
    u32 *buf = win->pixels;
    u32  bw  = win->w;
    u32  bh  = win->h;
    u32  spx = win->stride / 4u;

    /* --- Background --- */
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, (i32)bh, C_BG);

    /* --- Header bar --- */
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, HDR_H, C_HEADER);
    hline(buf, bw, bh, spx, 0, HDR_H - 1, (i32)bw, C_SEP);

    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     8, 7, "Sound Test", C_TEXT);

    /* "Audio: ON" / "Audio: --" indicator */
    {
        const char *avs  = g_avail ? "Audio: ON" : "Audio: --";
        u32         avcol = g_avail ? C_AVAIL : C_UNAVAIL;
        int         avw  = font_text_width(avs);
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         (i32)bw - avw - 8, 7, avs, avcol);
    }

    /* --- Section label: SFX --- */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     SFX_ROW_X0, SFX_Y - 12, "SFX:", C_DIM);

    /* --- SFX buttons --- */
    {
        int i;
        for (i = 0; i < NUM_SFX; i++) {
            draw_btn(buf, bw, bh, spx,
                     sfx_btn_x(i), SFX_Y, SFX_BTN_W, BTN_H,
                     sfx_labels[i], 0);
        }
    }

    /* --- Section label: Piano --- */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     PIANO_X0, PIANO_Y - 12, "Piano (C4-B4):", C_DIM);

    /* --- White keys --- */
    {
        int i;
        /* Clear lit flag if time has passed. */
        if (g_piano_lit >= 0 && now_ms >= g_lit_until)
            g_piano_lit = -1;

        for (i = 0; i < NUM_WHITE; i++) {
            int  semi = white_semi[i];
            int  lit  = (semi == g_piano_lit);
            u32  col  = lit ? C_KEY_LIT : C_KEY_WHITE;
            i32  kx   = white_key_x(i);
            fill_rect(buf, bw, bh, spx, kx, PIANO_Y, WK_W, PIANO_H, col);
            rect_outline(buf, bw, bh, spx, kx, PIANO_Y, WK_W, PIANO_H, 0xFF888888u);

            /* Note name at bottom of key */
            static const char * const wnames[NUM_WHITE] = {
                "C","D","E","F","G","A","B"
            };
            int nx = kx + (WK_W - FONT_W) / 2;
            int ny = PIANO_Y + PIANO_H - FONT_H - 2;
            u32 ncol = lit ? 0xFF003366u : 0xFF444444u;
            font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                             nx, ny, wnames[i], ncol);
        }
    }

    /* --- Black keys (drawn on top of whites) --- */
    {
        int i;
        for (i = 0; i < NUM_BLACK; i++) {
            int  semi = black_semi[i];
            int  lit  = (semi == g_piano_lit);
            u32  col  = lit ? C_KEY_LIT : C_KEY_BLACK;
            i32  bx   = black_key_x(i);
            fill_rect(buf, bw, bh, spx, bx, PIANO_Y, BK_W, BK_H, col);
            rect_outline(buf, bw, bh, spx, bx, PIANO_Y, BK_W, BK_H, 0xFF555555u);
        }
    }

    /* --- Melody button --- */
    {
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         MELODY_BTN_X, MELODY_Y - 12, "Melody:", C_DIM);
        draw_btn(buf, bw, bh, spx,
                 MELODY_BTN_X, MELODY_Y, MELODY_BTN_W, MELODY_BTN_H,
                 "Play Melody", 0);
    }

    /* --- Status line --- */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     8, STATUS_Y, g_status, C_STATUS);

    /* --- Freq readout if piano key lit --- */
    if (g_piano_lit >= 0) {
        int midi  = 60 + g_piano_lit;   /* C4 = MIDI 60 */
        int freq  = audio_midi_to_freq(midi);
        char fbuf[32];
        char nbuf[8];
        /* Build "Key: NNN Hz" */
        const char *p  = "Key: ";
        char *nstr = itoa_buf(freq, nbuf, sizeof(nbuf));
        int   fi = 0;
        const char *s;
        for (s = p; *s; s++)    fbuf[fi++] = *s;
        for (s = nstr; *s; s++) fbuf[fi++] = *s;
        fbuf[fi++] = ' '; fbuf[fi++] = 'H'; fbuf[fi++] = 'z'; fbuf[fi] = '\0';
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         8, STATUS_Y + FONT_H + 4, fbuf, 0xFFFFDD44u);
    }
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial("[SOUND] starting\n");

    /* Probe audio availability (caches internally). */
    g_avail = audio_available();
    if (g_avail) {
        serial("[SOUND] beep avail=Y\n");
    } else {
        serial("[SOUND] beep avail=N\n");
    }

    /* Connect to compositor. */
    if (wl_connect() != 0) {
        serial("[SOUND] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Sound Test");
    if (!win) {
        serial("[SOUND] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    serial("[SOUND] window ready\n");

    int prev_buttons = 0;

    for (;;) {
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);

        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_POINTER) {
                int cur_btn  = c & 1;
                int prev_btn = prev_buttons & 1;

                /* Detect left-button press edge. */
                if (cur_btn && !prev_btn) {
                    i32 mx = (i32)a;
                    i32 my = (i32)b;

                    /* SFX buttons */
                    int si = hit_sfx(mx, my);
                    if (si >= 0) {
                        switch (si) {
                            case 0:
                                g_status = "Playing: Click";
                                audio_sfx_click();
                                break;
                            case 1:
                                g_status = "Playing: Coin";
                                audio_sfx_coin();
                                break;
                            case 2:
                                g_status = "Playing: Jump";
                                audio_sfx_jump();
                                break;
                            case 3:
                                g_status = "Playing: Game Over";
                                audio_sfx_gameover();
                                break;
                        }
                    }

                    /* Piano keys */
                    int semi = hit_piano(mx, my);
                    if (semi >= 0) {
                        g_piano_lit  = semi;
                        g_lit_until  = now + 400;
                        g_status     = "Piano key pressed.";
                        /* Play note: MIDI 60 = C4, offset by semitone. */
                        audio_note(60 + semi, 180);
                    }

                    /* Melody button */
                    if (hit_melody(mx, my)) {
                        g_status = "Playing: C Major Scale";
                        /* render the status before blocking */
                        render(win, now);
                        wl_commit(win);
                        audio_play_melody(melody_notes, melody_durs, MELODY_LEN);
                        g_status = "Melody done.";
                    }
                }
                prev_buttons = c;
            }
            /* Key events not used here. */
        }

        render(win, now);
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0);
    }
}
