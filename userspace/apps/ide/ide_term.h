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

typedef struct {
    char  grid[IT_MAXROWS][IT_MAXCOLS]; /* visible scrollback cells       */
    int   cols, rows;                   /* live grid size (<= caps)        */
    int   cur_row, cur_col;             /* output cursor                   */
    char  line[IT_LINEMAX];             /* current input line              */
    int   line_len;
    char  cwd[IT_CWDMAX];               /* current working directory       */
    int   inited;                       /* 1 after first ide_term_init     */
    int   blink_ms;
    /* command history ring buffer (16 last commands) */
    char  hist[IT_HIST_MAX][IT_HIST_ENT];
    int   hist_count;                   /* total pushed (wraps at max)     */
    int   hist_nav;                     /* current position (-1 = none)    */
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

#endif /* IDE_TERM_H */
