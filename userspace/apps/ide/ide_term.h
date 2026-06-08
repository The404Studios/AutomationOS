/*
 * ide_term.h -- the IDE's integrated terminal panel (VS-Code "bottom panel").
 *
 * A self-contained character-grid terminal with a line editor and a built-in
 * command interpreter that talks to the kernel via the same syscall ABI the
 * standalone shell uses (SYS_OPENDIR/READDIR, SYS_OPEN/READ, SYS_MKDIR,
 * SYS_SPAWN, ...). It runs IN-PROCESS as a panel -- it does not fork a child
 * /bin/sh (that would open a separate window). Commands implemented mirror the
 * core of userspace/apps/terminal/terminal_m3.c: help, echo, clear, pwd, cd,
 * ls, cat, mkdir, plus IDE-aware `build` and `run` that drive the on-device
 * toolchain on the file currently open in the editor.
 *
 * The grid is sized to its Rect every frame, so it lays out responsively when
 * the IDE window is maximized.
 *
 * Freestanding: no libc / malloc / stdio. All state is static / embedded.
 */
#ifndef IDE_TERM_H
#define IDE_TERM_H

#include "ide_gfx.h"

struct Ide;
struct Rect;

/* Terminal grid dimensions. Generous caps so a maximized window has room; the
 * live cols/rows are derived from the panel Rect each frame and never exceed
 * these. 200x60 cells = ~1600x960 px of text. */
#define IT_MAXCOLS 200
#define IT_MAXROWS 60
#define IT_LINEMAX 256
#define IT_CWDMAX  256
#define IT_HIST_MAX 16      /* command history ring buffer size */
#define IT_HIST_ENT 192     /* max chars per history entry */

/* Scrollback: 100 lines of up to IT_MAXCOLS chars each. When a line scrolls
 * off the top of the visible grid it is saved into the ring buffer so the
 * user can scroll back through terminal history with the mouse wheel or
 * PageUp/PageDown. */
#define IT_SCROLLBACK 100

typedef struct {
    char  grid[IT_MAXROWS][IT_MAXCOLS]; /* visible scrollback cells       */
    int   cols, rows;                   /* live grid size (<= caps)        */
    int   cur_row, cur_col;             /* output cursor                   */
    char  line[IT_LINEMAX];             /* current input line              */
    int   line_len;
    int   line_cur;                     /* insert cursor index in line[]   */
    int   input_col0, input_row;        /* grid origin of the input line   */

    /* Tab autocomplete popup (commands for the first word, file paths after). */
    #define IT_COMP_MAX 12
    #define IT_COMP_W   56
    int   comp_active;                  /* 1 while the popup is shown       */
    int   comp_count;                   /* number of candidates             */
    int   comp_sel;                     /* highlighted candidate (Tab cycles)*/
    int   comp_wstart;                  /* line[] index where the word began */
    char  comp[IT_COMP_MAX][IT_COMP_W]; /* candidate completions            */
    char  cwd[IT_CWDMAX];               /* current working directory       */
    int   inited;                       /* 1 after first ide_term_init     */
    int   blink_ms;
    /* command history ring buffer (16 last commands) */
    char  hist[IT_HIST_MAX][IT_HIST_ENT];
    int   hist_count;                   /* total pushed (wraps at max)     */
    int   hist_nav;                     /* current position (-1 = none)    */

    /* scrollback ring buffer (100 lines) */
    char  sb[IT_SCROLLBACK][IT_MAXCOLS]; /* saved lines                   */
    int   sb_count;                      /* total lines pushed (wraps)    */
    int   sb_cols[IT_SCROLLBACK];        /* cols when each line was saved */
    int   scroll_off;                    /* 0 = live; >0 = viewing N lines
                                          * back from the bottom          */
} IdeTerm;

/* One-time init: clear the grid, set cwd to the project root, print a banner
 * and the first prompt. Safe to call repeatedly (no-op after first). */
void ide_term_init(IdeTerm* t, const char* cwd);

/* Render the terminal into Rect r (sizes the grid to r). */
void ide_term_render(struct Ide* a, Canvas* cv, struct Rect r);

/* Feed one resolved character (ch != 0) or special keycode to the terminal.
 * Enter executes the line; Backspace edits it. Returns 1 (consumed). */
int  ide_term_key(struct Ide* a, int keycode, char ch, int shift, int ctrl);

/* Advance caret blink by dt_ms. */
void ide_term_tick(IdeTerm* t, int dt_ms);

/* Scroll the terminal scrollback by `delta` lines (>0 = back in time,
 * <0 = toward live). Clamps to [0, sb_count]. */
void ide_term_scroll(IdeTerm* t, int delta);

/* Append a string to the terminal output (for build output, etc.). */
void ide_term_puts(IdeTerm* t, const char* s);

#endif /* IDE_TERM_H */
