/*
 * tracker.c -- Music Tracker / Step Sequencer (freestanding, ring 3).
 * ====================================================================
 *
 * A 16-step, 16-row step sequencer with a dark grid UI.
 * Rows represent pitches from a diatonic scale (two octaves, C major).
 * Columns represent 16 sequencer steps.
 *
 * Features:
 *   - Click cells to toggle notes on/off.
 *   - Play/Stop transport control.
 *   - BPM control (+/- buttons, 40–240 BPM).
 *   - Moving playhead column highlighted in real time (non-blocking).
 *   - Triggered cells flash when the playhead hits them.
 *   - Three preset patterns to load (empty, default, jungle).
 *   - "Clear" button clears the grid.
 *   - "Audio: ON / --" status from audio_available().
 *   - Graceful when audio hardware is absent (UI still animates).
 *
 * Build (WSL Arch, no -fstack-protector):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/tracker/tracker.c -o /tmp/tracker.o
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
 *       /tmp/tracker.o /tmp/audio.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/tracker.elf
 *   objdump -d /tmp/tracker.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [TRACKER] starting
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/audio/audio.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

static inline long sc6(long n, long a1, long a2, long a3,
                        long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

static inline unsigned long get_ticks_ms(void)
{
    return (unsigned long)sc6(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Minimal freestanding helpers.
 * --------------------------------------------------------------------- */
typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long u64;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc6(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

/* Integer-to-decimal string; returns pointer into a static buffer. */
static char _itoa_buf[16];
static const char *itoa_d(i32 v)
{
    int neg = (v < 0);
    if (neg) v = -v;
    char *p = _itoa_buf + 15;
    *p = '\0';
    do {
        *--p = '0' + (v % 10);
        v /= 10;
    } while (v);
    if (neg) *--p = '-';
    return p;
}

/* -----------------------------------------------------------------------
 * Window / layout constants.
 * --------------------------------------------------------------------- */
#define WIN_W  720
#define WIN_H  460

/* ---- Colour palette (dark theme) ---- */
#define C_BG          0xFF1A1A1Au  /* window background           */
#define C_TOOLBAR_BG  0xFF222222u  /* top toolbar                 */
#define C_TOOLBAR_SEP 0xFF444444u
#define C_PANEL_BG    0xFF1E1E1Eu  /* side/bottom panels          */
#define C_GRID_BG     0xFF252525u  /* empty cell background       */
#define C_GRID_LINE   0xFF333333u  /* grid lines                  */
#define C_GRID_LINE_4 0xFF3D3D3Du  /* heavier every-4-step line   */
#define C_NOTE_A      0xFF2E86C1u  /* active note – blue          */
#define C_NOTE_B      0xFF3498DBu  /* active note – lighter face  */
#define C_HEAD_BG     0xFF1A3A1Au  /* playhead column tint        */
#define C_HEAD_LINE   0xFF44FF44u  /* playhead column left edge   */
#define C_FLASH       0xFFFFFF44u  /* cell flash when triggered   */
#define C_BTN_FACE    0xFF333333u
#define C_BTN_HOT     0xFF404040u
#define C_BTN_PLAY    0xFF1E7A1Eu
#define C_BTN_STOP    0xFF7A1E1Eu
#define C_BTN_BORDER  0xFF666666u
#define C_TEXT        0xFFEEEEEEu
#define C_TEXT_DIM    0xFF888888u
#define C_ROW_LABEL   0xFF2A2A2Au  /* row label bg                */
#define C_ROW_LBL_C   0xFFCCCCCCu  /* row label text              */
#define C_ROW_EVEN    0xFF252525u
#define C_ROW_ODD     0xFF222222u

/* ---- Sequencer dimensions ---- */
#define NUM_STEPS    16
#define NUM_ROWS     16

/* ---- Grid geometry ---- */
#define TOOLBAR_H    44   /* top toolbar height                  */

#define ROW_LBL_W    38   /* left-side note labels               */
#define GRID_X0      (ROW_LBL_W)          /* grid left edge       */
#define GRID_Y0      (TOOLBAR_H + 2)      /* grid top edge        */

#define CELL_W       ((WIN_W - ROW_LBL_W - 2) / NUM_STEPS)  /* ~42px */
#define CELL_H       24

#define GRID_W       (NUM_STEPS * CELL_W)
#define GRID_H       (NUM_ROWS  * CELL_H)

/* ---- Transport/control panel (below grid) ---- */
#define CTRL_Y       (GRID_Y0 + GRID_H + 4)
#define CTRL_H       (WIN_H - CTRL_Y - 2)

/* Button geometry in the control panel */
#define BTN_H        26
#define BTN_Y        (CTRL_Y + (CTRL_H - BTN_H) / 2)

/* Play/Stop */
#define BTN_PLAY_X   (ROW_LBL_W)
#define BTN_PLAY_W   56

/* BPM label + -/+ */
#define BTN_BPM_LBL_X (BTN_PLAY_X + BTN_PLAY_W + 8)
#define BTN_BPM_DEC_X (BTN_BPM_LBL_X + 56)
#define BTN_BPM_INC_X (BTN_BPM_DEC_X + 26)
#define BTN_STEP_W    24

/* Preset buttons */
#define BTN_PRE0_X   (BTN_BPM_INC_X + BTN_STEP_W + 14)
#define BTN_PRE_W    46
#define BTN_PRE_GAP  4

/* Clear */
#define BTN_CLR_X    (BTN_PRE0_X + 3*(BTN_PRE_W + BTN_PRE_GAP) + 10)
#define BTN_CLR_W    46

/* -----------------------------------------------------------------------
 * MIDI note layout: two octaves of C major starting C3 (MIDI 48).
 * Row 0 is the highest pitch (top of grid), row 15 is lowest (bottom).
 * --------------------------------------------------------------------- */
/* C major scale degrees: C D E F G A B (7 notes/octave) */
/* Two octaves = 14 notes; we add two more for a full 16: */
static const int ROW_MIDI[NUM_ROWS] = {
    /* row 0 = highest */
    84, /* C6 */
    83, /* B5 */
    81, /* A5 */
    79, /* G5 */
    77, /* F5 */
    76, /* E5 */
    74, /* D5 */
    72, /* C5 */
    71, /* B4 */
    69, /* A4 */
    67, /* G4 */
    65, /* F4 */
    64, /* E4 */
    62, /* D4 */
    60, /* C4 */
    57, /* A3 */
};

static const char * const ROW_LABEL[NUM_ROWS] = {
    "C6","B5","A5","G5","F5","E5","D5","C5",
    "B4","A4","G4","F4","E4","D4","C4","A3",
};

/* -----------------------------------------------------------------------
 * Sequencer state.
 * --------------------------------------------------------------------- */
static int  grid[NUM_ROWS][NUM_STEPS];  /* 1 = note on                    */
static int  flash[NUM_ROWS][NUM_STEPS]; /* flash counter (frames)          */

static int  playing    = 0;
static int  cur_step   = 0;
static int  bpm        = 120;

/* Timestamp (ms) when the current step started. */
static u64  step_start_ms = 0;

/* When was the last audio_note() fired for the current step? */
static int  step_fired = 0;   /* 0 = not yet fired for this step */

#define BPM_MIN  40
#define BPM_MAX  240

/* Compute step duration in ms from BPM (one 16th note = beat/4). */
static u64 step_ms(void)
{
    /* 60000 ms / bpm = ms per beat; /4 for 16th note */
    return (u64)(60000 / (bpm * 4));
}

/* -----------------------------------------------------------------------
 * Three built-in preset patterns.
 * --------------------------------------------------------------------- */

/* Preset 0: all empty (already the reset state) */

/* Preset 1: simple 4-on-the-floor + melody */
static const int PRESET1[NUM_ROWS][NUM_STEPS] = {
    /* C6 */ {0,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,1,0},
    /* B5 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* A5 */ {0,0,0,1, 0,0,0,0, 0,0,0,1, 0,0,0,0},
    /* G5 */ {0,0,0,0, 0,1,0,0, 0,0,0,0, 0,1,0,0},
    /* F5 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* E5 */ {0,1,0,0, 0,0,0,1, 0,1,0,0, 0,0,0,1},
    /* D5 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* C5 */ {1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0},
    /* B4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* A4 */ {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0},
    /* G4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* F4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* E4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* D4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* C4 */ {1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0},
    /* A3 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
};

/* Preset 2: pentatonic arpeggio feel */
static const int PRESET2[NUM_ROWS][NUM_STEPS] = {
    /* C6 */ {0,0,0,0, 0,0,0,1, 0,0,0,0, 0,0,0,1},
    /* B5 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* A5 */ {0,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,1,0},
    /* G5 */ {0,0,0,0, 0,1,0,0, 0,0,0,0, 0,1,0,0},
    /* F5 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* E5 */ {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0},
    /* D5 */ {0,0,0,1, 0,0,0,0, 0,0,0,1, 0,0,0,0},
    /* C5 */ {0,0,1,0, 0,0,0,0, 0,0,1,0, 0,0,0,0},
    /* B4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* A4 */ {0,1,0,0, 0,0,0,0, 0,1,0,0, 0,0,0,0},
    /* G4 */ {1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0},
    /* F4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* E4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* D4 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    /* C4 */ {1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0},
    /* A3 */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
};

static void load_preset(int p)
{
    for (int r = 0; r < NUM_ROWS; r++)
        for (int s = 0; s < NUM_STEPS; s++) {
            if (p == 1) grid[r][s] = PRESET1[r][s];
            else if (p == 2) grid[r][s] = PRESET2[r][s];
            else grid[r][s] = 0;
        }
}

static void clear_grid(void)
{
    for (int r = 0; r < NUM_ROWS; r++)
        for (int s = 0; s < NUM_STEPS; s++) {
            grid[r][s] = 0;
            flash[r][s] = 0;
        }
}

/* -----------------------------------------------------------------------
 * Drawing primitives.
 * --------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = col;
    }
}

static void hline(u32 *buf, u32 bw, u32 bh, u32 spx,
                  i32 x, i32 y, i32 len, u32 col)
{
    fill_rect(buf, bw, bh, spx, x, y, len, 1, col);
}

static void vline(u32 *buf, u32 bw, u32 bh, u32 spx,
                  i32 x, i32 y, i32 len, u32 col)
{
    fill_rect(buf, bw, bh, spx, x, y, 1, len, col);
}

static void draw_border(u32 *buf, u32 bw, u32 bh, u32 spx,
                        i32 x, i32 y, i32 w, i32 h, u32 col)
{
    hline(buf, bw, bh, spx, x,         y,         w, col);
    hline(buf, bw, bh, spx, x,         y + h - 1, w, col);
    vline(buf, bw, bh, spx, x,         y,         h, col);
    vline(buf, bw, bh, spx, x + w - 1, y,         h, col);
}

/* Draw a button with optional fill color override. */
static void draw_button(u32 *buf, u32 bw, u32 bh, u32 spx,
                        i32 x, i32 y, i32 w, i32 h,
                        u32 face, const char *label, u32 text_col)
{
    fill_rect(buf, bw, bh, spx, x, y, w, h, face);
    draw_border(buf, bw, bh, spx, x, y, w, h, C_BTN_BORDER);
    /* Center label */
    int tw = font_text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - FONT_H) / 2;
    font_draw_string(buf, (int)spx, (int)bw, (int)bh, tx, ty, label, text_col);
}

/* -----------------------------------------------------------------------
 * Hit-test helpers.
 * --------------------------------------------------------------------- */
static int hit_cell(i32 mx, i32 my, int *out_row, int *out_step)
{
    i32 gx = mx - GRID_X0;
    i32 gy = my - GRID_Y0;
    if (gx < 0 || gx >= GRID_W) return 0;
    if (gy < 0 || gy >= GRID_H) return 0;
    *out_step = (int)(gx / CELL_W);
    *out_row  = (int)(gy / CELL_H);
    if (*out_step >= NUM_STEPS || *out_row >= NUM_ROWS) return 0;
    return 1;
}

static int hit_btn(i32 mx, i32 my, i32 bx, i32 by, i32 bw, i32 bh)
{
    return (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
}

/* -----------------------------------------------------------------------
 * Non-blocking playhead tick.
 * Called every frame; advances cur_step when the step duration has elapsed.
 * Fires audio_note for active cells at the start of each step.
 * --------------------------------------------------------------------- */
static void tick_playhead(void)
{
    if (!playing) return;

    u64 now = get_ticks_ms();
    u64 sms = step_ms();

    /* If the step duration has elapsed, advance to the next step. */
    if (now - step_start_ms >= sms) {
        cur_step = (cur_step + 1) % NUM_STEPS;
        step_start_ms = now;
        step_fired = 0;
    }

    /* Fire audio once per step (at step start). */
    if (!step_fired) {
        step_fired = 1;
        /* Play all active notes in this column. */
        u32 dur = (u32)(sms > 10 ? sms - 10 : sms); /* slight staccato */
        for (int r = 0; r < NUM_ROWS; r++) {
            if (grid[r][cur_step]) {
                audio_note(ROW_MIDI[r], dur);
                flash[r][cur_step] = 6; /* ~6 frames of flash */
            }
        }
    }

    /* Decay flash counters. */
    for (int r = 0; r < NUM_ROWS; r++)
        for (int s = 0; s < NUM_STEPS; s++)
            if (flash[r][s] > 0) flash[r][s]--;
}

/* -----------------------------------------------------------------------
 * Render: toolbar.
 * --------------------------------------------------------------------- */
static void draw_toolbar(u32 *buf, u32 bw, u32 bh, u32 spx, int audio_on)
{
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, TOOLBAR_H, C_TOOLBAR_BG);
    hline(buf, bw, bh, spx, 0, TOOLBAR_H - 1, (i32)bw, C_TOOLBAR_SEP);

    /* Title */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     6, (TOOLBAR_H - FONT_H) / 2,
                     "MUSIC TRACKER", C_TEXT);

    /* Audio status */
    const char *ast = audio_on ? "Audio: ON" : "Audio: --";
    u32 acol = audio_on ? 0xFF44FF44u : C_TEXT_DIM;
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     WIN_W - font_text_width(ast) - 8,
                     (TOOLBAR_H - FONT_H) / 2,
                     ast, acol);
}

/* -----------------------------------------------------------------------
 * Render: step grid.
 * --------------------------------------------------------------------- */
static void draw_grid(u32 *buf, u32 bw, u32 bh, u32 spx)
{
    /* Background */
    fill_rect(buf, bw, bh, spx,
              0, GRID_Y0, (i32)bw, GRID_H, C_PANEL_BG);

    for (int r = 0; r < NUM_ROWS; r++) {
        i32 cy = GRID_Y0 + r * CELL_H;
        u32 row_bg = (r & 1) ? C_ROW_ODD : C_ROW_EVEN;

        /* Row label */
        fill_rect(buf, bw, bh, spx, 0, cy, ROW_LBL_W, CELL_H, C_ROW_LABEL);
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         2, cy + (CELL_H - FONT_H) / 2,
                         ROW_LABEL[r], C_ROW_LBL_C);
        /* Separator after label */
        vline(buf, bw, bh, spx, ROW_LBL_W - 1, cy, CELL_H, C_GRID_LINE_4);

        for (int s = 0; s < NUM_STEPS; s++) {
            i32 cx = GRID_X0 + s * CELL_W;

            /* Cell background: playhead column gets a tint */
            u32 bg = (playing && s == cur_step) ? C_HEAD_BG : row_bg;
            fill_rect(buf, bw, bh, spx, cx, cy, CELL_W, CELL_H, bg);

            /* Cell content */
            if (grid[r][s]) {
                u32 note_col;
                if (flash[r][s] > 0)
                    note_col = C_FLASH;
                else
                    note_col = C_NOTE_A;

                /* Draw note block with a 2px inset margin */
                fill_rect(buf, bw, bh, spx,
                          cx + 2, cy + 2, CELL_W - 4, CELL_H - 4,
                          note_col);
                /* Lighter top strip for 3D feel */
                fill_rect(buf, bw, bh, spx,
                          cx + 2, cy + 2, CELL_W - 4, 3,
                          C_NOTE_B);
            }

            /* Grid lines */
            u32 gl = ((s % 4) == 0) ? C_GRID_LINE_4 : C_GRID_LINE;
            vline(buf, bw, bh, spx, cx, cy, CELL_H, gl);
        }

        /* Horizontal row separator */
        hline(buf, bw, bh, spx, GRID_X0, cy, GRID_W, C_GRID_LINE);

        /* Playhead left edge accent */
        if (playing) {
            i32 px = GRID_X0 + cur_step * CELL_W;
            vline(buf, bw, bh, spx, px, cy, CELL_H, C_HEAD_LINE);
        }
    }

    /* Right edge and bottom border */
    vline(buf, bw, bh, spx, GRID_X0 + GRID_W, GRID_Y0, GRID_H, C_GRID_LINE_4);
    hline(buf, bw, bh, spx, 0, GRID_Y0 + GRID_H, (i32)bw, C_TOOLBAR_SEP);
}

/* -----------------------------------------------------------------------
 * Render: transport / control bar.
 * --------------------------------------------------------------------- */
static void draw_controls(u32 *buf, u32 bw, u32 bh, u32 spx)
{
    /* Background */
    fill_rect(buf, bw, bh, spx, 0, CTRL_Y, (i32)bw, CTRL_H, C_PANEL_BG);

    /* Play/Stop button */
    u32 face_ps = playing ? C_BTN_STOP : C_BTN_PLAY;
    const char *lbl_ps = playing ? "STOP" : "PLAY";
    draw_button(buf, bw, bh, spx,
                BTN_PLAY_X, BTN_Y, BTN_PLAY_W, BTN_H,
                face_ps, lbl_ps, C_TEXT);

    /* BPM label */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     BTN_BPM_LBL_X, BTN_Y + (BTN_H - FONT_H) / 2,
                     "BPM:", C_TEXT_DIM);
    /* BPM value */
    const char *bpm_s = itoa_d(bpm);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     BTN_BPM_LBL_X + 36, BTN_Y + (BTN_H - FONT_H) / 2,
                     bpm_s, C_TEXT);

    /* BPM dec button */
    draw_button(buf, bw, bh, spx,
                BTN_BPM_DEC_X, BTN_Y, BTN_STEP_W, BTN_H,
                C_BTN_FACE, "-", C_TEXT);

    /* BPM inc button */
    draw_button(buf, bw, bh, spx,
                BTN_BPM_INC_X, BTN_Y, BTN_STEP_W, BTN_H,
                C_BTN_FACE, "+", C_TEXT);

    /* Preset buttons */
    static const char * const pre_labels[3] = {"P1","P2","P3"};
    for (int i = 0; i < 3; i++) {
        draw_button(buf, bw, bh, spx,
                    BTN_PRE0_X + i * (BTN_PRE_W + BTN_PRE_GAP),
                    BTN_Y, BTN_PRE_W, BTN_H,
                    C_BTN_FACE, pre_labels[i], C_TEXT);
    }

    /* Step counter while playing */
    if (playing) {
        char step_str[8];
        /* "S:XX" */
        step_str[0] = 'S'; step_str[1] = ':';
        step_str[2] = '0' + cur_step / 10;
        step_str[3] = '0' + cur_step % 10;
        step_str[4] = '\0';
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         BTN_CLR_X + BTN_CLR_W + 8,
                         BTN_Y + (BTN_H - FONT_H) / 2,
                         step_str, C_HEAD_LINE);
    }

    /* Clear button */
    draw_button(buf, bw, bh, spx,
                BTN_CLR_X, BTN_Y, BTN_CLR_W, BTN_H,
                C_BTN_FACE, "CLR", 0xFFFF6644u);
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    print("[TRACKER] starting\n");

    int audio_on = audio_available();

    if (wl_connect() != 0) {
        print("[TRACKER] wl_connect FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Music Tracker");
    if (!win) {
        print("[TRACKER] wl_create_window FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[TRACKER] window created\n");

    u32 spx = win->stride / 4u;

    /* ---- Initial state ---- */
    clear_grid();

    /* ---- Frame loop ---- */
    int prev_btn = 0;  /* was left button down last frame? */

    for (;;) {
        /* ---- Non-blocking playhead advance ---- */
        tick_playhead();

        /* ---- Event handling ---- */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                i32 mx = (i32)ea;
                i32 my = (i32)eb;
                int btn = (ec & 1);

                /* Only act on press transitions (leading edge). */
                if (btn && !prev_btn) {
                    /* --- Grid cell toggle --- */
                    int row, step;
                    if (hit_cell(mx, my, &row, &step)) {
                        grid[row][step] ^= 1;
                        flash[row][step] = 0;
                    }

                    /* --- Play/Stop --- */
                    else if (hit_btn(mx, my,
                                     BTN_PLAY_X, BTN_Y,
                                     BTN_PLAY_W, BTN_H)) {
                        if (!playing) {
                            playing = 1;
                            cur_step = 0;
                            step_start_ms = get_ticks_ms();
                            step_fired = 0;
                            print("[TRACKER] play\n");
                        } else {
                            playing = 0;
                            print("[TRACKER] stop\n");
                        }
                    }

                    /* --- BPM decrease --- */
                    else if (hit_btn(mx, my,
                                     BTN_BPM_DEC_X, BTN_Y,
                                     BTN_STEP_W, BTN_H)) {
                        if (bpm > BPM_MIN) bpm -= 5;
                        if (bpm < BPM_MIN) bpm = BPM_MIN;
                    }

                    /* --- BPM increase --- */
                    else if (hit_btn(mx, my,
                                     BTN_BPM_INC_X, BTN_Y,
                                     BTN_STEP_W, BTN_H)) {
                        if (bpm < BPM_MAX) bpm += 5;
                        if (bpm > BPM_MAX) bpm = BPM_MAX;
                    }

                    /* --- Preset 1 --- */
                    else if (hit_btn(mx, my,
                                     BTN_PRE0_X + 0*(BTN_PRE_W+BTN_PRE_GAP),
                                     BTN_Y, BTN_PRE_W, BTN_H)) {
                        load_preset(0);
                        print("[TRACKER] preset 0 (empty)\n");
                    }

                    /* --- Preset 2 --- */
                    else if (hit_btn(mx, my,
                                     BTN_PRE0_X + 1*(BTN_PRE_W+BTN_PRE_GAP),
                                     BTN_Y, BTN_PRE_W, BTN_H)) {
                        load_preset(1);
                        print("[TRACKER] preset 1\n");
                    }

                    /* --- Preset 3 --- */
                    else if (hit_btn(mx, my,
                                     BTN_PRE0_X + 2*(BTN_PRE_W+BTN_PRE_GAP),
                                     BTN_Y, BTN_PRE_W, BTN_H)) {
                        load_preset(2);
                        print("[TRACKER] preset 2\n");
                    }

                    /* --- Clear --- */
                    else if (hit_btn(mx, my,
                                     BTN_CLR_X, BTN_Y,
                                     BTN_CLR_W, BTN_H)) {
                        clear_grid();
                        print("[TRACKER] clear\n");
                    }
                }

                prev_btn = btn;
            }
            /* Key events not used; silently ignored. */
        }

        /* ---- Render ---- */
        fill_rect(win->pixels, win->w, win->h, spx,
                  0, 0, WIN_W, WIN_H, C_BG);

        draw_toolbar(win->pixels, win->w, win->h, spx, audio_on);
        draw_grid   (win->pixels, win->w, win->h, spx);
        draw_controls(win->pixels, win->w, win->h, spx);

        wl_commit(win);

        sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
