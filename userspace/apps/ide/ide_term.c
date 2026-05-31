/*
 * ide_term.c -- the IDE's integrated terminal panel.
 *
 * A character-grid terminal with an in-process command interpreter. It reuses
 * the command model of userspace/apps/terminal/terminal_m3.c (a line buffer,
 * Backspace editing, Enter to execute, a scrolling grid) but runs as a PANEL
 * inside the IDE rather than owning a whole window, and adds IDE-aware `build`
 * and `run` commands that drive the on-device toolchain on the file open in the
 * editor.
 *
 * Commands: help, echo, clear, pwd, cd, ls, cat, mkdir, touch, build, run.
 * Everything talks to the kernel through the shared ide_sc() syscall ABI.
 *
 * Freestanding: no libc / malloc / stdio. The grid + line buffer live in the
 * embedded IdeTerm struct; one static path scratch (KPATH-sized) is used for
 * syscalls that copy a fixed number of bytes out of the user pointer.
 */
#include "ide.h"
#include "ide_theme.h"
#include "ide_term.h"
#include "ide_build.h"

/* ---- syscalls (subset; numbers match kernel/include/syscall.h) ---- */
#define ITS_READ      2
#define ITS_OPEN      4
#define ITS_CLOSE     5
#define ITS_SPAWN     16
#define ITS_OPENDIR   30
#define ITS_READDIR   31
#define ITS_CLOSEDIR  32
#define ITS_MKDIR     67

#define ITO_RDONLY 0x0000
#define ITO_WRONLY 0x0001
#define ITO_CREAT  0x0040

/* kernel copies up to MAX_PATH_LEN (4096) out of a path pointer; give it room */
#define ITKPATH 4096

/* dirent layout mirrors IdeDirent / kernel struct dirent */
#define ITDT_DIR 4

/* ---- evdev keycodes ---- */
#define ITK_BACKSPACE 14
#define ITK_ENTER     28
#define ITK_UP        103  /* evdev KEY_UP   -- command-history recall (prev) */
#define ITK_DOWN      108  /* evdev KEY_DOWN -- command-history recall (next) */

/* ---- terminal colors ---- */
#define IT_BG     TH_PANEL2
#define IT_FG     0xFFD8E0E8u
#define IT_PROMPT TH_GREEN
#define IT_CURSOR TH_GREEN
#define IT_HDR_H  (ROW_H + 2)
#define IT_BLINK  500

/* static path scratch for syscalls (16-aligned, kernel-copy-safe) */
static char it_pathbuf[ITKPATH] __attribute__((aligned(16)));

/* ---- tiny string helpers (local; the shared ide_str* live in ide_sys.c) ---- */
static int it_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static const char* it_skip_sp(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
/* first whitespace token of *p into out[cap]; advance *p past it; return len */
static int it_token(const char** p, char* out, int cap) {
    const char* s = it_skip_sp(*p);
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && n < cap - 1) out[n++] = *s++;
    out[n] = 0;
    *p = s;
    return n;
}

/* ---- grid primitives ---- */
static void it_clear(IdeTerm* t) {
    for (int r = 0; r < IT_MAXROWS; r++)
        for (int c = 0; c < IT_MAXCOLS; c++) t->grid[r][c] = ' ';
    t->cur_row = 0;
    t->cur_col = 0;
}

static void it_scroll_up(IdeTerm* t) {
    for (int r = 1; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++) t->grid[r - 1][c] = t->grid[r][c];
    for (int c = 0; c < t->cols; c++) t->grid[t->rows - 1][c] = ' ';
}

static void it_newline(IdeTerm* t) {
    t->cur_col = 0;
    t->cur_row++;
    if (t->cur_row >= t->rows) { it_scroll_up(t); t->cur_row = t->rows - 1; }
}

static void it_putc(IdeTerm* t, char ch) {
    if (ch == '\n') { it_newline(t); return; }
    if (ch == '\r') return;
    if (ch == '\t') { do { it_putc(t, ' '); } while (t->cur_col % 4 != 0); return; }
    if (t->cols <= 0) return;
    if (t->cur_col >= t->cols) it_newline(t);
    t->grid[t->cur_row][t->cur_col] = ch;
    t->cur_col++;
    if (t->cur_col >= t->cols) it_newline(t);
}

static void it_puts(IdeTerm* t, const char* s) { for (; s && *s; s++) it_putc(t, *s); }

static void it_backspace(IdeTerm* t) {
    if (t->cur_col > 0) { t->cur_col--; t->grid[t->cur_row][t->cur_col] = ' '; }
}

static void it_prompt(IdeTerm* t) {
    it_puts(t, "ide:");
    it_puts(t, t->cwd);
    it_puts(t, "$ ");
}

/* ---- path resolution (cwd-aware, no normalization of .. beyond simple join) */
static const char* it_resolve(IdeTerm* t, const char* arg) {
    for (int i = 0; i < ITKPATH; i++) it_pathbuf[i] = 0;
    arg = it_skip_sp(arg ? arg : "");
    int n = 0;
    if (arg[0] == '/') {
        while (arg[n] && n < ITKPATH - 1) { it_pathbuf[n] = arg[n]; n++; }
    } else if (arg[0] == 0) {
        int c = 0; while (t->cwd[c] && n < ITKPATH - 1) it_pathbuf[n++] = t->cwd[c++];
    } else {
        int c = 0; while (t->cwd[c] && n < ITKPATH - 1) it_pathbuf[n++] = t->cwd[c++];
        if (n == 0 || it_pathbuf[n - 1] != '/') { if (n < ITKPATH - 1) it_pathbuf[n++] = '/'; }
        int b = 0; while (arg[b] && n < ITKPATH - 1) it_pathbuf[n++] = arg[b++];
    }
    it_pathbuf[n] = 0;
    return it_pathbuf;
}

/* ---- commands ---- */
static void it_cmd_help(IdeTerm* t) {
    it_puts(t, "integrated terminal -- commands:\n");
    it_puts(t, "  help            this list\n");
    it_puts(t, "  echo <text>     print text\n");
    it_puts(t, "  clear           clear the terminal\n");
    it_puts(t, "  pwd             print working directory\n");
    it_puts(t, "  cd [dir]        change directory\n");
    it_puts(t, "  ls [path]       list a directory\n");
    it_puts(t, "  cat <file>      print a file\n");
    it_puts(t, "  mkdir <dir>     create a directory\n");
    it_puts(t, "  touch <file>    create an empty file\n");
    it_puts(t, "  build           compile the open file (Ctrl+B)\n");
    it_puts(t, "  run             spawn the last build\n");
}

static void it_cmd_ls(IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    it_token(&p, tok, sizeof(tok));
    const char* path = it_resolve(t, tok);
    long fd = ide_sc(ITS_OPENDIR, (long)path, 0, 0, 0, 0, 0);
    if (fd < 0) { it_puts(t, "ls: cannot open '"); it_puts(t, path); it_puts(t, "'\n"); return; }
    IdeDirent de;
    int count = 0;
    for (;;) {
        long r = ide_sc(ITS_READDIR, fd, (long)&de, 0, 0, 0, 0);
        if (r != 0) break;
        de.name[255] = 0;
        if (de.name[0] == 0) continue;
        it_puts(t, de.name);
        if (de.type == ITDT_DIR) it_putc(t, '/');
        it_putc(t, '\n');
        count++;
    }
    ide_sc(ITS_CLOSEDIR, fd, 0, 0, 0, 0, 0);
    if (count == 0) it_puts(t, "(empty)\n");
}

static void it_cmd_cat(struct Ide* a, IdeTerm* t, const char* args) {
    (void)a;
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "cat: missing file\n"); return; }
    const char* path = it_resolve(t, tok);
    long fd = ide_sc(ITS_OPEN, (long)path, ITO_RDONLY, 0, 0, 0, 0);
    if (fd < 0) { it_puts(t, "cat: cannot open '"); it_puts(t, path); it_puts(t, "'\n"); return; }
    char buf[256];
    for (;;) {
        long nrd = ide_sc(ITS_READ, fd, (long)buf, (long)sizeof(buf), 0, 0, 0);
        if (nrd <= 0) break;
        for (long i = 0; i < nrd; i++) it_putc(t, buf[i]);
    }
    ide_sc(ITS_CLOSE, fd, 0, 0, 0, 0, 0);
    if (t->cur_col != 0) it_putc(t, '\n');
}

static void it_cmd_cd(IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    int have = it_token(&p, tok, sizeof(tok));
    if (!have || it_eq(tok, "~")) { t->cwd[0] = '/'; t->cwd[1] = 0; return; }
    const char* path = it_resolve(t, tok);
    long fd = ide_sc(ITS_OPENDIR, (long)path, 0, 0, 0, 0, 0);
    if (fd >= 0) {
        ide_sc(ITS_CLOSEDIR, fd, 0, 0, 0, 0, 0);
        ide_strlcpy(t->cwd, path, IT_CWDMAX);
    } else {
        it_puts(t, "cd: "); it_puts(t, tok); it_puts(t, ": no such directory\n");
    }
}

static void it_cmd_mkdir(IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "mkdir: missing operand\n"); return; }
    const char* path = it_resolve(t, tok);
    long r = ide_sc(ITS_MKDIR, (long)path, 0755, 0, 0, 0, 0);
    if (r < 0) { it_puts(t, "mkdir: cannot create '"); it_puts(t, path); it_puts(t, "'\n"); }
}

static void it_cmd_touch(IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "touch: missing file\n"); return; }
    const char* path = it_resolve(t, tok);
    long fd = ide_sc(ITS_OPEN, (long)path, ITO_WRONLY | ITO_CREAT, 0644, 0, 0, 0);
    if (fd >= 0) ide_sc(ITS_CLOSE, fd, 0, 0, 0, 0, 0);
    else { it_puts(t, "touch: cannot create '"); it_puts(t, path); it_puts(t, "'\n"); }
}

/* build/run drive the same on-device toolchain the B-key uses, then echo the
 * cached result summary so the terminal shows progress too. */
static void it_cmd_build(struct Ide* a, IdeTerm* t) {
    if (!a->cur_file[0]) { it_puts(t, "build: no file open in editor\n"); return; }
    it_puts(t, "building "); it_puts(t, a->cur_file); it_puts(t, " ...\n");
    ide_do_build(a);
    a->btab = BTAB_BUILD;   /* surface full diagnostics in the BUILD tab */
    it_puts(t, "  (see BUILD tab for full output)\n");
}

static void it_cmd_run(struct Ide* a, IdeTerm* t) {
    ide_do_run(a);
    a->btab = BTAB_BUILD;
    it_puts(t, "run: dispatched (see BUILD tab)\n");
}

/* ---- command history ring buffer ---- */
static void it_hist_push(IdeTerm* t, const char* line) {
    if (!line || !line[0]) return;  /* don't save empty commands */
    int slot = t->hist_count % IT_HIST_MAX;
    ide_strlcpy(t->hist[slot], line, IT_HIST_ENT);
    t->hist_count++;
}

static const char* it_hist_get(IdeTerm* t, int offset) {
    if (offset < 0 || offset >= t->hist_count || offset >= IT_HIST_MAX) return 0;
    int idx = (t->hist_count - 1 - offset) % IT_HIST_MAX;
    return t->hist[idx];
}

static void it_execute(struct Ide* a, IdeTerm* t) {
    char cmd[IT_LINEMAX];
    const char* p = t->line;
    int have = it_token(&p, cmd, sizeof(cmd));
    const char* args = p;   /* remainder after the command word */

    /* Push to history before executing (so up-arrow recalls even if cmd fails) */
    if (have && t->line[0]) it_hist_push(t, t->line);

    it_putc(t, '\n');

    if (!have) { it_prompt(t); t->line_len = 0; t->line[0] = 0; t->hist_nav = -1; return; }

    if      (it_eq(cmd, "help"))  it_cmd_help(t);
    else if (it_eq(cmd, "clear")) { it_clear(t); }
    else if (it_eq(cmd, "echo"))  { it_puts(t, it_skip_sp(args)); it_putc(t, '\n'); }
    else if (it_eq(cmd, "pwd"))   { it_puts(t, t->cwd); it_putc(t, '\n'); }
    else if (it_eq(cmd, "cd"))    it_cmd_cd(t, args);
    else if (it_eq(cmd, "ls"))    it_cmd_ls(t, args);
    else if (it_eq(cmd, "cat"))   it_cmd_cat(a, t, args);
    else if (it_eq(cmd, "mkdir")) it_cmd_mkdir(t, args);
    else if (it_eq(cmd, "touch")) it_cmd_touch(t, args);
    else if (it_eq(cmd, "build")) it_cmd_build(a, t);
    else if (it_eq(cmd, "run"))   it_cmd_run(a, t);
    else {
        it_puts(t, cmd);
        it_puts(t, ": command not found (try 'help')\n");
    }

    it_prompt(t);
    t->line_len = 0;
    t->line[0] = 0;
    t->hist_nav = -1;   /* reset history navigation */
}

/* ---- public API ---- */
void ide_term_init(IdeTerm* t, const char* cwd) {
    if (t->inited) return;
    t->cols = 80; t->rows = 24;
    it_clear(t);
    t->line_len = 0; t->line[0] = 0;
    t->blink_ms = 0;
    t->hist_count = 0;
    t->hist_nav = -1;
    if (cwd && cwd[0]) ide_strlcpy(t->cwd, cwd, IT_CWDMAX);
    else { t->cwd[0] = '/'; t->cwd[1] = 0; }
    it_puts(t, "AutomationOS integrated terminal. Type 'help'.\n");
    it_prompt(t);
    t->inited = 1;
}

void ide_term_tick(IdeTerm* t, int dt_ms) {
    t->blink_ms += dt_ms;
    if (t->blink_ms >= 2 * IT_BLINK) t->blink_ms = 0;
}

int ide_term_key(struct Ide* a, int keycode, char ch, int shift, int ctrl) {
    IdeTerm* t = &a->term;
    (void)shift;
    if (ctrl) return 0;   /* Ctrl-chords routed by caller */

    if (keycode == ITK_ENTER) { it_execute(a, t); return 1; }
    if (keycode == ITK_BACKSPACE) {
        if (t->line_len > 0) { t->line_len--; t->line[t->line_len] = 0; it_backspace(t); }
        return 1;
    }

    /* Arrow key history navigation (up/down) */
    if (keycode == ITK_UP) {   /* recall previous command */
        int available = t->hist_count < IT_HIST_MAX ? t->hist_count : IT_HIST_MAX;
        if (available > 0) {
            int next_nav = t->hist_nav + 1;
            if (next_nav < available) {
                const char* prev = it_hist_get(t, next_nav);
                if (prev) {
                    /* clear current line visually */
                    for (int i = 0; i < t->line_len; i++) it_backspace(t);
                    /* load history entry */
                    ide_strlcpy(t->line, prev, IT_LINEMAX);
                    t->line_len = ide_strlen(t->line);
                    /* display it */
                    it_puts(t, t->line);
                    t->hist_nav = next_nav;
                }
            }
        }
        return 1;
    }
    if (keycode == ITK_DOWN) {  /* recall next command (toward present) */
        if (t->hist_nav > 0) {
            int next_nav = t->hist_nav - 1;
            const char* next = it_hist_get(t, next_nav);
            if (next) {
                /* clear current line */
                for (int i = 0; i < t->line_len; i++) it_backspace(t);
                /* load history entry */
                ide_strlcpy(t->line, next, IT_LINEMAX);
                t->line_len = ide_strlen(t->line);
                it_puts(t, t->line);
                t->hist_nav = next_nav;
            }
        } else if (t->hist_nav == 0) {
            /* back to empty line */
            for (int i = 0; i < t->line_len; i++) it_backspace(t);
            t->line[0] = 0;
            t->line_len = 0;
            t->hist_nav = -1;
        }
        return 1;
    }

    if (ch >= 32 && ch < 127) {
        if (t->line_len < IT_LINEMAX - 1) {
            t->line[t->line_len++] = ch;
            t->line[t->line_len] = 0;
            it_putc(t, ch);
        }
        return 1;
    }
    return 0;
}

void ide_term_render(struct Ide* a, Canvas* cv, Rect r) {
    IdeTerm* t = &a->term;
    if (!cv || r.w <= 0 || r.h <= 0) return;

    /* size the grid to the panel body (responsive when maximized) */
    Rect body;
    body.x = r.x; body.y = r.y;
    body.w = r.w; body.h = r.h;

    int cols = (body.w - 2 * PAD) / GFX_FW;
    int rows = (body.h - 2) / GFX_FH;
    if (cols < 1) cols = 1;
    if (cols > IT_MAXCOLS) cols = IT_MAXCOLS;
    if (rows < 1) rows = 1;
    if (rows > IT_MAXROWS) rows = IT_MAXROWS;
    /* If the grid grew, just adopt the new size; if it shrank, clamp cursor. */
    t->cols = cols;
    t->rows = rows;
    if (t->cur_row >= rows) t->cur_row = rows - 1;
    if (t->cur_col >= cols) t->cur_col = cols - 1;

    gfx_fill(cv, body.x, body.y, body.w, body.h, IT_BG);

    int ox = body.x + PAD;
    int oy = body.y + 1;
    for (int rr = 0; rr < rows; rr++) {
        int py = oy + rr * GFX_FH;
        if (py + GFX_FH > body.y + body.h) break;
        for (int cc = 0; cc < cols; cc++) {
            char chr = t->grid[rr][cc];
            if (chr == ' ' || chr == 0) continue;
            int px = ox + cc * GFX_FW;
            gfx_text_clip(cv, px, py, &chr, IT_FG, ox, cols * GFX_FW);
        }
    }

    /* caret block when focused + blink on */
    if (a->term_focus && t->blink_ms < IT_BLINK) {
        int px = ox + t->cur_col * GFX_FW;
        int py = oy + t->cur_row * GFX_FH;
        if (py + GFX_FH <= body.y + body.h)
            gfx_fill(cv, px, py, GFX_FW, GFX_FH, IT_CURSOR);
        /* re-draw the char under the caret in the bg color for contrast */
        char chr = t->grid[t->cur_row][t->cur_col];
        if (chr != ' ' && chr != 0)
            gfx_text_clip(cv, px, py, &chr, IT_BG, ox, cols * GFX_FW);
    }
}
