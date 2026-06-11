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
 * Commands: help, echo, clear, pwd, cd, ls, cat, mkdir, touch, rm, cp, mv,
 *           write, wc, head, ps, uptime, build, run, spawn.
 * Unknown commands fall through to /bin/<cmd> via SYS_SPAWN.
 *
 * Scrollback: the last IT_SCROLLBACK (100) lines that scroll off the top of
 * the visible grid are saved in a ring buffer. Mouse-wheel / PageUp / PageDown
 * scroll through the history; any new output snaps back to live view.
 *
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
#define ITS_WRITE     3
#define ITS_OPEN      4
#define ITS_CLOSE     5
#define ITS_WAITPID   6
#define ITS_YIELD     15
#define ITS_SPAWN     16
#define ITS_UNLINK    34
#define ITS_RENAME    35
#define ITS_OPENDIR   30
#define ITS_READDIR   31
#define ITS_CLOSEDIR  32
#define ITS_TICKS_MS  40
#define ITS_PROCLIST  44
#define ITS_MKDIR     67
#define ITS_SLEEP     9
#define ITS_KILL      26
#define ITS_GETTIME   42
#define ITS_SYSINFO   62

#define ITO_RDONLY 0x0000
#define ITO_WRONLY 0x0001
#define ITO_RDWR   0x0002
#define ITO_CREAT  0x0040
#define ITO_TRUNC  0x0200

/* kernel copies up to MAX_PATH_LEN (4096) out of a path pointer; give it room */
#define ITKPATH 4096

/* dirent layout mirrors IdeDirent / kernel struct dirent */
#define ITDT_DIR 4

/* ---- evdev keycodes ---- */
#define ITK_BACKSPACE 14
#define ITK_ENTER     28
#define ITK_UP        103  /* evdev KEY_UP   -- command-history recall (prev) */
#define ITK_DOWN      108  /* evdev KEY_DOWN -- command-history recall (next) */
#define ITK_PAGEUP    104  /* evdev KEY_PAGEUP   -- scroll back              */
#define ITK_PAGEDOWN  109  /* evdev KEY_PAGEDOWN -- scroll forward           */
#define ITK_HOME      102  /* evdev KEY_HOME  */
#define ITK_END       107  /* evdev KEY_END   */
#define ITK_LEFT      105  /* evdev KEY_LEFT  -- move input cursor left      */
#define ITK_RIGHT     106  /* evdev KEY_RIGHT -- move input cursor right     */
#define ITK_DELETE    111  /* evdev KEY_DELETE -- forward-delete in the line */
#define ITK_TAB       15   /* evdev KEY_TAB   -- insert spaces               */

/* ---- terminal colors ---- */
#define IT_BG     TH_PANEL2
#define IT_FG     0xFFD8E0E8u
#define IT_PROMPT TH_GREEN
#define IT_CURSOR TH_GREEN
#define IT_DIM    TH_TEXT_DIM
#define IT_ERR    TH_RED
#define IT_HDR_H  (ROW_H + 2)
#define IT_BLINK  500

/* static path scratch for syscalls (16-aligned, kernel-copy-safe) */
static char it_pathbuf[ITKPATH] __attribute__((aligned(16)));

/* second path scratch for two-path commands (cp, mv) */
static char it_pathbuf2[ITKPATH] __attribute__((aligned(16)));

/* padded spawn path and args buffers for SYS_SPAWN (kernel reads 127 bytes) */
static char it_spawn_path[128] __attribute__((aligned(16)));
static char it_spawn_args[256] __attribute__((aligned(16)));

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

static void it_put_num(IdeTerm* t, long v);  /* forward decl */

/* ---- scrollback ring buffer ---- */

/* Save the top row of the visible grid into the scrollback ring. */
static void it_sb_push(IdeTerm* t) {
    int slot = t->sb_count % IT_SCROLLBACK;
    int cols = t->cols > 0 ? t->cols : 80;
    if (cols > IT_MAXCOLS) cols = IT_MAXCOLS;
    for (int c = 0; c < cols; c++) t->sb[slot][c] = t->grid[0][c];
    for (int c = cols; c < IT_MAXCOLS; c++) t->sb[slot][c] = ' ';
    t->sb_cols[slot] = cols;
    t->sb_count++;
}

/* ---- grid primitives ---- */
static void it_clear(IdeTerm* t) {
    for (int r = 0; r < IT_MAXROWS; r++)
        for (int c = 0; c < IT_MAXCOLS; c++) t->grid[r][c] = ' ';
    t->cur_row = 0;
    t->cur_col = 0;
}

static void it_scroll_up(IdeTerm* t) {
    /* save the line scrolling off the top into the scrollback buffer */
    it_sb_push(t);
    for (int r = 1; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++) t->grid[r - 1][c] = t->grid[r][c];
    for (int c = 0; c < t->cols; c++) t->grid[t->rows - 1][c] = ' ';
}

static void it_newline(IdeTerm* t) {
    t->cur_col = 0;
    t->cur_row++;
    if (t->cur_row >= t->rows) { it_scroll_up(t); t->cur_row = t->rows - 1; }
    /* any new output snaps view back to live */
    t->scroll_off = 0;
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

/* Print a signed long in decimal. */
static void it_put_num(IdeTerm* t, long v) {
    if (v < 0) { it_putc(t, '-'); v = -v; }
    char buf[20]; int n = 0;
    if (v == 0) { it_putc(t, '0'); return; }
    while (v > 0 && n < 20) { buf[n++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) it_putc(t, buf[i]);
}

/* Print an unsigned long in decimal. */
static void it_put_unum(IdeTerm* t, unsigned long v) {
    char buf[20]; int n = 0;
    if (v == 0) { it_putc(t, '0'); return; }
    while (v > 0 && n < 20) { buf[n++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) it_putc(t, buf[i]);
}

static void it_backspace(IdeTerm* t) {
    if (t->cur_col > 0) { t->cur_col--; t->grid[t->cur_row][t->cur_col] = ' '; }
}

static void it_prompt(IdeTerm* t) {
    it_puts(t, "ide:");
    it_puts(t, t->cwd);
    it_puts(t, "$ ");
    /* Remember where the editable input line begins so mid-line edits can
     * repaint just this row + place the cursor by character index. */
    t->input_row  = t->cur_row;
    t->input_col0 = t->cur_col;
    t->line_cur   = 0;
}

/* Repaint the current input line on its grid row from t->line and place the
 * visual cursor at line_cur. Single-row: a command longer than the row width is
 * clipped visually (the buffer + execution stay correct). Used for EVERY input
 * edit so mid-line insert/delete/cursor-move render consistently. */
static void it_redraw_input(IdeTerm* t) {
    int row = t->input_row;
    if (row < 0 || row >= IT_MAXROWS) return;
    int c0 = t->input_col0;
    for (int c = c0; c < t->cols && c < IT_MAXCOLS; c++) t->grid[row][c] = ' ';
    for (int i = 0; i < t->line_len; i++) {
        int c = c0 + i;
        if (c >= t->cols || c >= IT_MAXCOLS) break;
        t->grid[row][c] = t->line[i];
    }
    t->cur_row = row;
    int cc = c0 + t->line_cur;
    if (cc >= t->cols) cc = t->cols - 1;
    if (cc < 0) cc = 0;
    t->cur_col = cc;
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

/* Same as it_resolve but into the second path buffer (for two-arg commands). */
static const char* it_resolve2(IdeTerm* t, const char* arg) {
    for (int i = 0; i < ITKPATH; i++) it_pathbuf2[i] = 0;
    arg = it_skip_sp(arg ? arg : "");
    int n = 0;
    if (arg[0] == '/') {
        while (arg[n] && n < ITKPATH - 1) { it_pathbuf2[n] = arg[n]; n++; }
    } else if (arg[0] == 0) {
        int c = 0; while (t->cwd[c] && n < ITKPATH - 1) it_pathbuf2[n++] = t->cwd[c++];
    } else {
        int c = 0; while (t->cwd[c] && n < ITKPATH - 1) it_pathbuf2[n++] = t->cwd[c++];
        if (n == 0 || it_pathbuf2[n - 1] != '/') { if (n < ITKPATH - 1) it_pathbuf2[n++] = '/'; }
        int b = 0; while (arg[b] && n < ITKPATH - 1) it_pathbuf2[n++] = arg[b++];
    }
    it_pathbuf2[n] = 0;
    return it_pathbuf2;
}

/* ---- commands ---- */
static void it_cmd_help(IdeTerm* t) {
    it_puts(t, "integrated terminal -- commands:\n");
    it_puts(t, " File system:\n");
    it_puts(t, "  pwd             print working directory\n");
    it_puts(t, "  cd [dir]        change directory\n");
    it_puts(t, "  ls [path]       list a directory\n");
    it_puts(t, "  cat <file>      print a file\n");
    it_puts(t, "  head <file>     first 10 lines of a file\n");
    it_puts(t, "  wc <file>       count lines/words/bytes\n");
    it_puts(t, "  mkdir <dir>     create a directory\n");
    it_puts(t, "  touch <file>    create an empty file\n");
    it_puts(t, "  rm <file>       remove a file\n");
    it_puts(t, "  cp <src> <dst>  copy a file\n");
    it_puts(t, "  mv <src> <dst>  rename / move a file\n");
    it_puts(t, "  write <f> <txt> write text to a file\n");
    it_puts(t, " IDE / build:\n");
    it_puts(t, "  build           compile the open file (^B)\n");
    it_puts(t, "  run [path]      spawn the last build or a path\n");
    it_puts(t, "  spawn <app>     spawn /sbin/<app>\n");
    it_puts(t, " System:\n");
    it_puts(t, "  ps              list processes\n");
    it_puts(t, "  uptime          show system uptime\n");
    it_puts(t, " General:\n");
    it_puts(t, "  echo <text>     print text\n");
    it_puts(t, "  clear           clear the terminal\n");
    it_puts(t, "  help            this list\n");
    it_puts(t, " <cmd> [args]     try /bin/<cmd> (external)\n");
    it_puts(t, " Ctrl+` or Ctrl+J to toggle terminal focus.\n");
    it_puts(t, " PageUp/PageDown or mouse wheel to scroll.\n");
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
        /* skip . and .. */
        if (de.name[0] == '.' && (de.name[1] == 0 || (de.name[1] == '.' && de.name[2] == 0)))
            continue;
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

/* head <file> -- print first 10 lines */
static void it_cmd_head(struct Ide* a, IdeTerm* t, const char* args) {
    (void)a;
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "head: missing file\n"); return; }
    const char* path = it_resolve(t, tok);
    long fd = ide_sc(ITS_OPEN, (long)path, ITO_RDONLY, 0, 0, 0, 0);
    if (fd < 0) { it_puts(t, "head: cannot open '"); it_puts(t, path); it_puts(t, "'\n"); return; }
    int lines = 0;
    char buf[256];
    for (;;) {
        long nrd = ide_sc(ITS_READ, fd, (long)buf, (long)sizeof(buf), 0, 0, 0);
        if (nrd <= 0) break;
        for (long i = 0; i < nrd; i++) {
            it_putc(t, buf[i]);
            if (buf[i] == '\n') { lines++; if (lines >= 10) goto done; }
        }
    }
done:
    ide_sc(ITS_CLOSE, fd, 0, 0, 0, 0, 0);
    if (t->cur_col != 0) it_putc(t, '\n');
}

/* wc <file> -- count lines/words/bytes */
static void it_cmd_wc(struct Ide* a, IdeTerm* t, const char* args) {
    (void)a;
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "wc: missing file\n"); return; }
    const char* path = it_resolve(t, tok);
    long fd = ide_sc(ITS_OPEN, (long)path, ITO_RDONLY, 0, 0, 0, 0);
    if (fd < 0) { it_puts(t, "wc: cannot open '"); it_puts(t, path); it_puts(t, "'\n"); return; }
    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[256];
    for (;;) {
        long nrd = ide_sc(ITS_READ, fd, (long)buf, (long)sizeof(buf), 0, 0, 0);
        if (nrd <= 0) break;
        for (long i = 0; i < nrd; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r') {
                in_word = 0;
            } else {
                if (!in_word) { words++; in_word = 1; }
            }
        }
    }
    ide_sc(ITS_CLOSE, fd, 0, 0, 0, 0, 0);
    it_put_num(t, lines); it_putc(t, ' ');
    it_put_num(t, words); it_putc(t, ' ');
    it_put_num(t, bytes); it_putc(t, ' ');
    it_puts(t, tok); it_putc(t, '\n');
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

/* rm <file> -- unlink a file */
static void it_cmd_rm(IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "rm: missing file\n"); return; }
    const char* path = it_resolve(t, tok);
    long r = ide_sc(ITS_UNLINK, (long)path, 0, 0, 0, 0, 0);
    if (r < 0) { it_puts(t, "rm: cannot remove '"); it_puts(t, path); it_puts(t, "'\n"); }
}

/* cp <src> <dst> -- copy a file */
/* static scratch for source path (avoid 4KB stack alloc in freestanding) */
static char it_cp_src[ITKPATH] __attribute__((aligned(16)));

static void it_cmd_cp(IdeTerm* t, const char* args) {
    char src_tok[IT_LINEMAX], dst_tok[IT_LINEMAX];
    const char* p = args;
    if (!it_token(&p, src_tok, sizeof(src_tok)) || !it_token(&p, dst_tok, sizeof(dst_tok))) {
        it_puts(t, "cp: usage: cp <src> <dst>\n"); return;
    }
    /* resolve source first, copy to static since resolve2 shares pathbuf */
    const char* src = it_resolve(t, src_tok);
    ide_strlcpy(it_cp_src, src, ITKPATH);
    const char* dst = it_resolve2(t, dst_tok);

    long sfd = ide_sc(ITS_OPEN, (long)it_cp_src, ITO_RDONLY, 0, 0, 0, 0);
    if (sfd < 0) { it_puts(t, "cp: cannot open '"); it_puts(t, src_tok); it_puts(t, "'\n"); return; }
    long dfd = ide_sc(ITS_OPEN, (long)dst, ITO_WRONLY | ITO_CREAT | ITO_TRUNC, 0644, 0, 0, 0);
    if (dfd < 0) { ide_sc(ITS_CLOSE, sfd, 0, 0, 0, 0, 0);
                   it_puts(t, "cp: cannot create '"); it_puts(t, dst_tok); it_puts(t, "'\n"); return; }
    char buf[512];
    for (;;) {
        long nrd = ide_sc(ITS_READ, sfd, (long)buf, (long)sizeof(buf), 0, 0, 0);
        if (nrd <= 0) break;
        ide_sc(ITS_WRITE, dfd, (long)buf, nrd, 0, 0, 0);
    }
    ide_sc(ITS_CLOSE, sfd, 0, 0, 0, 0, 0);
    ide_sc(ITS_CLOSE, dfd, 0, 0, 0, 0, 0);
}

/* mv <src> <dst> -- rename via SYS_RENAME */
/* static scratch for mv source path */
static char it_mv_src[ITKPATH] __attribute__((aligned(16)));

static void it_cmd_mv(IdeTerm* t, const char* args) {
    char src_tok[IT_LINEMAX], dst_tok[IT_LINEMAX];
    const char* p = args;
    if (!it_token(&p, src_tok, sizeof(src_tok)) || !it_token(&p, dst_tok, sizeof(dst_tok))) {
        it_puts(t, "mv: usage: mv <src> <dst>\n"); return;
    }
    const char* src = it_resolve(t, src_tok);
    ide_strlcpy(it_mv_src, src, ITKPATH);
    const char* dst = it_resolve2(t, dst_tok);

    long r = ide_sc(ITS_RENAME, (long)it_mv_src, (long)dst, 0, 0, 0, 0);
    if (r < 0) {
        it_puts(t, "mv: cannot rename '"); it_puts(t, src_tok);
        it_puts(t, "' to '"); it_puts(t, dst_tok); it_puts(t, "'\n");
    }
}

/* write <file> <text...> -- truncate and write text to a file */
static void it_cmd_write(IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "write: usage: write <file> <text...>\n"); return; }
    const char* text = it_skip_sp(p);
    if (!text[0]) { it_puts(t, "write: missing text\n"); return; }
    const char* path = it_resolve(t, tok);
    long fd = ide_sc(ITS_OPEN, (long)path, ITO_WRONLY | ITO_CREAT | ITO_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) { it_puts(t, "write: cannot open '"); it_puts(t, path); it_puts(t, "'\n"); return; }
    int len = ide_strlen(text);
    ide_sc(ITS_WRITE, fd, (long)text, (long)len, 0, 0, 0);
    /* write a trailing newline */
    char nl = '\n';
    ide_sc(ITS_WRITE, fd, (long)&nl, 1, 0, 0, 0);
    ide_sc(ITS_CLOSE, fd, 0, 0, 0, 0, 0);
    it_puts(t, "wrote "); it_put_num(t, len + 1); it_puts(t, " bytes\n");
}

/* ps -- list processes via SYS_PROCLIST (44). The kernel fills a flat array of
 * proc_info_t (64 bytes each): { u32 pid, parent_pid, state, flags;
 * char name[32]; u64 cpu_ticks, ctx_switches; }. Returns the count or <0. */
typedef struct {
    unsigned int pid;
    unsigned int parent_pid;
    unsigned int state;
    unsigned int flags;
    char         name[32];
    unsigned long cpu_ticks;
    unsigned long ctx_switches;
} ITProcInfo;   /* must match kernel proc_info_t (64 bytes) */

#define IT_PS_MAX 32
static ITProcInfo it_ps_buf[IT_PS_MAX];

static const char* it_state_str(unsigned int s) {
    switch (s) {
        case 0: return "READY";
        case 1: return "RUNNING";
        case 2: return "BLOCKED";
        case 3: return "ZOMBIE";
        default: return "?";
    }
}

static void it_cmd_ps(IdeTerm* t) {
    long n = ide_sc(ITS_PROCLIST, (long)it_ps_buf, IT_PS_MAX, 0, 0, 0, 0);
    if (n < 0) { it_puts(t, "ps: not available\n"); return; }
    it_puts(t, " PID  STATE    NAME\n");
    for (long i = 0; i < n && i < IT_PS_MAX; i++) {
        /* right-align PID in a 4-char field */
        int pid = (int)it_ps_buf[i].pid;
        if (pid < 10)   it_puts(t, "   ");
        else if (pid < 100)  it_puts(t, "  ");
        else if (pid < 1000) it_puts(t, " ");
        it_put_num(t, pid);
        it_puts(t, "  ");
        const char* st = it_state_str(it_ps_buf[i].state);
        it_puts(t, st);
        /* pad state to 8 chars */
        int slen = ide_strlen(st);
        for (int j = slen; j < 8; j++) it_putc(t, ' ');
        it_ps_buf[i].name[31] = 0;
        it_puts(t, it_ps_buf[i].name);
        it_putc(t, '\n');
    }
}

/* uptime -- show milliseconds since boot in a human-readable format */
static void it_cmd_uptime(IdeTerm* t) {
    long ms = ide_sc(ITS_TICKS_MS, 0, 0, 0, 0, 0, 0);
    if (ms < 0) { it_puts(t, "uptime: not available\n"); return; }
    long secs = ms / 1000;
    long mins = secs / 60;
    long hrs  = mins / 60;
    it_puts(t, "up ");
    if (hrs > 0) { it_put_num(t, hrs); it_puts(t, "h "); }
    it_put_num(t, mins % 60); it_puts(t, "m ");
    it_put_num(t, secs % 60); it_puts(t, "s (");
    it_put_unum(t, (unsigned long)ms); it_puts(t, " ms)\n");
}

/* build/run drive the same on-device toolchain the B-key uses, then echo the
 * cached result summary so the terminal shows progress too. */
static void it_cmd_build(struct Ide* a, IdeTerm* t) {
    if (!a->cur_file[0]) { it_puts(t, "build: no file open in editor\n"); return; }
    it_puts(t, "building "); it_puts(t, a->cur_file); it_puts(t, " ...\n");
    ide_do_build(a);
    a->btab = BTAB_BUILD;   /* surface full diagnostics in the BUILD tab */
    /* show build time */
    int bms = ide_build_time_ms();
    if (bms > 0) {
        it_puts(t, "  completed in ");
        it_put_num(t, bms);
        it_puts(t, " ms\n");
    }
    it_puts(t, "  (see BUILD tab for full output)\n");
}

/* run [path] -- spawn the last build OR an explicit path */
static void it_cmd_run(struct Ide* a, IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    int have = it_token(&p, tok, sizeof(tok));
    if (have && tok[0]) {
        /* explicit path: spawn it directly */
        const char* path = it_resolve(t, tok);
        for (int i = 0; i < (int)sizeof(it_spawn_path); i++) it_spawn_path[i] = 0;
        ide_strlcpy(it_spawn_path, path, (int)sizeof(it_spawn_path));
        long pid = ide_sc(ITS_SPAWN, (long)it_spawn_path, 0, 0, 0, 0, 0);
        if (pid <= 0) {
            it_puts(t, "run: cannot execute '"); it_puts(t, tok); it_puts(t, "'\n");
        } else {
            it_puts(t, "spawned pid "); it_put_num(t, pid); it_putc(t, '\n');
        }
        return;
    }
    /* no arg: run the last build */
    ide_do_run(a);
    it_puts(t, "run: dispatched (see BUILD tab)\n");
}

/* spawn <app> -- prepend /sbin/ and SYS_SPAWN */
static void it_cmd_spawn(IdeTerm* t, const char* args) {
    char tok[IT_LINEMAX]; const char* p = args;
    if (!it_token(&p, tok, sizeof(tok))) { it_puts(t, "spawn: usage: spawn <app>\n"); return; }
    for (int i = 0; i < (int)sizeof(it_spawn_path); i++) it_spawn_path[i] = 0;
    const char* pre = "sbin/";
    int pi = 0;
    while (pre[pi] && pi < (int)sizeof(it_spawn_path) - 1)
        { it_spawn_path[pi] = pre[pi]; pi++; }
    ide_strlcpy(it_spawn_path + pi, tok, (int)sizeof(it_spawn_path) - pi);

    long pid = ide_sc(ITS_SPAWN, (long)it_spawn_path, 0, 0, 0, 0, 0);
    if (pid <= 0) {
        it_puts(t, "spawn: cannot execute '"); it_puts(t, it_spawn_path); it_puts(t, "'\n");
    } else {
        it_puts(t, "spawned '"); it_puts(t, it_spawn_path);
        it_puts(t, "' as pid "); it_put_num(t, pid); it_putc(t, '\n');
    }
}

/* External command fallback: try /bin/<cmd> [args]. No pipe/redirect, so we
 * just spawn and report the PID. */
static int it_try_external(IdeTerm* t, const char* cmd, const char* args) {
    for (int i = 0; i < (int)sizeof(it_spawn_path); i++) it_spawn_path[i] = 0;
    const char* pre = "/bin/";
    int pi = 0;
    while (pre[pi] && pi < (int)sizeof(it_spawn_path) - 1)
        { it_spawn_path[pi] = pre[pi]; pi++; }
    ide_strlcpy(it_spawn_path + pi, cmd, (int)sizeof(it_spawn_path) - pi);

    for (int i = 0; i < (int)sizeof(it_spawn_args); i++) it_spawn_args[i] = 0;
    if (args) {
        const char* a = it_skip_sp(args);
        if (a[0]) ide_strlcpy(it_spawn_args, a, (int)sizeof(it_spawn_args));
    }

    long pid = ide_sc(ITS_SPAWN, (long)it_spawn_path, (long)it_spawn_args, 0, 0, 0, 0);
    if (pid <= 0) return 0;  /* not found */
    it_puts(t, "spawned '"); it_puts(t, it_spawn_path);
    it_puts(t, "' as pid "); it_put_num(t, pid); it_putc(t, '\n');
    return 1;
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

/* ============================================================================
 * Shell layer -- a command registry, variables, aliases, scripting (NAME=val,
 * $VAR, `;` chaining, `source <file>`, `repeat N <cmd>`, `# comments`) and a
 * wide command set: this is what turns the panel into a real automation CLI.
 * ==========================================================================*/

/* command categories + the registry that drives help, `which`, and Tab-complete */
enum { CC_SCRIPT, CC_FS, CC_SYS, CC_DEV };
typedef struct { const char* name; int cat; const char* help; } ItCmd;
static const ItCmd g_itcmds[] = {
    { "help",   CC_SCRIPT, "list commands, or `help <cmd>`" },
    { "clear",  CC_SCRIPT, "clear screen + scrollback" },
    { "history",CC_SCRIPT, "show command history" },
    { "echo",   CC_SCRIPT, "print text (expands $vars)" },
    { "set",    CC_SCRIPT, "set NAME VALUE  (no args: list)" },
    { "unset",  CC_SCRIPT, "unset NAME" },
    { "vars",   CC_SCRIPT, "list shell variables" },
    { "alias",  CC_SCRIPT, "alias NAME=CMD  (no args: list)" },
    { "unalias",CC_SCRIPT, "remove an alias" },
    { "source", CC_SCRIPT, "run a .tli script of commands" },
    { "repeat", CC_SCRIPT, "repeat N <cmd...>" },
    { "which",  CC_SCRIPT, "what is NAME" },
    { "sleep",  CC_SCRIPT, "sleep <ms>" },
    { "pwd",    CC_FS, "print working directory" },
    { "cd",     CC_FS, "change directory" },
    { "ls",     CC_FS, "list a directory" },
    { "cat",    CC_FS, "print a file" },
    { "head",   CC_FS, "first 10 lines" },
    { "wc",     CC_FS, "count lines/words/bytes" },
    { "grep",   CC_FS, "grep <pat> <file>" },
    { "mkdir",  CC_FS, "create a directory" },
    { "touch",  CC_FS, "create an empty file" },
    { "rm",     CC_FS, "remove a file" },
    { "cp",     CC_FS, "copy a file" },
    { "mv",     CC_FS, "rename / move a file" },
    { "write",  CC_FS, "write text to a file" },
    { "ps",     CC_SYS, "list processes" },
    { "kill",   CC_SYS, "kill <pid>" },
    { "uptime", CC_SYS, "system uptime" },
    { "free",   CC_SYS, "memory usage" },
    { "date",   CC_SYS, "show date/time" },
    { "whoami", CC_SYS, "current user" },
    { "uname",  CC_SYS, "OS name" },
    { "build",  CC_DEV, "compile the open file" },
    { "run",    CC_DEV, "run last build / a path" },
    { "spawn",  CC_DEV, "spawn /sbin/<app>" },
    { "edit",   CC_DEV, "open <file> in the editor" },
};
#define N_ITCMDS ((int)(sizeof(g_itcmds) / sizeof(g_itcmds[0])))

/* ---- variables + aliases (file-static; one terminal) ---- */
#define IT_NM     32
#define IT_VL     192
#define IT_NVARS  32
#define IT_NALIAS 24
static char it_vn[IT_NVARS][IT_NM], it_vv[IT_NVARS][IT_VL]; static int it_nv = 0;
static char it_an[IT_NALIAS][IT_NM], it_av[IT_NALIAS][IT_VL]; static int it_na = 0;

static int  it_find_var(const char* n){ for(int i=0;i<it_nv;i++) if(it_eq(it_vn[i],n)) return i; return -1; }
static const char* it_get_var(const char* n){ int i=it_find_var(n); return i<0?0:it_vv[i]; }
static void it_set_var(const char* n,const char* v){
    if(!n||!n[0]) return; int i=it_find_var(n);
    if(i<0){ if(it_nv>=IT_NVARS) return; i=it_nv++; ide_strlcpy(it_vn[i],n,IT_NM); }
    ide_strlcpy(it_vv[i], v?v:"", IT_VL);
}
static void it_unset_var(const char* n){
    int i=it_find_var(n); if(i<0) return;
    for(int j=i;j<it_nv-1;j++){ ide_strlcpy(it_vn[j],it_vn[j+1],IT_NM); ide_strlcpy(it_vv[j],it_vv[j+1],IT_VL); } it_nv--;
}
static int  it_find_alias(const char* n){ for(int i=0;i<it_na;i++) if(it_eq(it_an[i],n)) return i; return -1; }
static const char* it_get_alias(const char* n){ int i=it_find_alias(n); return i<0?0:it_av[i]; }
static void it_set_alias(const char* n,const char* v){
    if(!n||!n[0]) return; int i=it_find_alias(n);
    if(i<0){ if(it_na>=IT_NALIAS) return; i=it_na++; ide_strlcpy(it_an[i],n,IT_NM); }
    ide_strlcpy(it_av[i], v?v:"", IT_VL);
}

static int it_ident_ch(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
static int it_substr(const char* hay,const char* needle){
    if(!needle[0]) return 1;
    for(int i=0;hay[i];i++){ int j=0; while(needle[j]&&hay[i+j]==needle[j]) j++; if(!needle[j]) return 1; }
    return 0;
}
/* expand $NAME / ${NAME} into out[cap]. */
static void it_expand(const char* in,char* out,int cap){
    int o=0;
    for(int i=0; in[i] && o<cap-1; ){
        if(in[i]=='$'){
            int j=i+1, braced=0; if(in[j]=='{'){ braced=1; j++; }
            char nm[IT_NM]; int k=0;
            while(in[j]&&it_ident_ch(in[j])&&k<IT_NM-1) nm[k++]=in[j++];
            nm[k]=0; if(braced&&in[j]=='}') j++;
            if(k>0){ const char* v=it_get_var(nm); if(v) for(int m=0;v[m]&&o<cap-1;m++) out[o++]=v[m]; i=j; continue; }
        }
        out[o++]=in[i++];
    }
    out[o]=0;
}
static void it_puts_pad(IdeTerm* t,const char* s,int w){
    int n=0; for(;s[n];n++) it_putc(t,s[n]); for(;n<w;n++) it_putc(t,' ');
}
static void it_put2(IdeTerm* t,int v){ it_putc(t,'0'+(v/10)%10); it_putc(t,'0'+v%10); }

/* forward decls (mutual recursion: source/repeat re-enter the runner) */
static void it_run_one(struct Ide* a, IdeTerm* t, const char* cmdline, int depth);
static void it_run_line(struct Ide* a, IdeTerm* t, const char* line, int depth);

/* ---- new commands ---- */
static void it_cmd_help2(IdeTerm* t,const char* args){
    char tok[IT_NM]; const char* p=args;
    if(it_token(&p,tok,sizeof(tok))){
        for(int i=0;i<N_ITCMDS;i++) if(it_eq(g_itcmds[i].name,tok)){
            it_puts(t,"  "); it_puts(t,tok); it_puts(t," -- "); it_puts(t,g_itcmds[i].help); it_putc(t,'\n'); return; }
        it_puts(t,"help: no such command\n"); return;
    }
    static const char* CN[4]={ "Shell / script","File system","System","IDE / build" };
    for(int c=0;c<4;c++){
        it_puts(t,CN[c]); it_puts(t,":\n");
        for(int i=0;i<N_ITCMDS;i++) if(g_itcmds[i].cat==c){
            it_puts(t,"  "); it_puts_pad(t,g_itcmds[i].name,9); it_puts(t,g_itcmds[i].help); it_putc(t,'\n'); }
    }
    it_puts(t,"Automation:  NAME=val   $NAME   a ; b   alias x=cmd   source f.tli   repeat N cmd   # note\n");
    it_puts(t,"Tab completes commands + file paths.  Up/Down history.  Ctrl+`/Ctrl+J focus.\n");
}
static void it_cmd_set(IdeTerm* t,const char* args){
    char nm[IT_NM]; const char* p=args;
    if(!it_token(&p,nm,sizeof(nm))){ /* list */
        if(it_nv==0){ it_puts(t,"(no variables)\n"); return; }
        for(int i=0;i<it_nv;i++){ it_puts(t,it_vn[i]); it_putc(t,'='); it_puts(t,it_vv[i]); it_putc(t,'\n'); } return;
    }
    it_set_var(nm, it_skip_sp(p));
}
static void it_cmd_vars(IdeTerm* t){
    if(it_nv==0){ it_puts(t,"(no variables)\n"); return; }
    for(int i=0;i<it_nv;i++){ it_puts(t,it_vn[i]); it_putc(t,'='); it_puts(t,it_vv[i]); it_putc(t,'\n'); }
}
static void it_cmd_alias(IdeTerm* t,const char* args){
    const char* s=it_skip_sp(args);
    if(!s[0]){ if(it_na==0){ it_puts(t,"(no aliases)\n"); return; }
        for(int i=0;i<it_na;i++){ it_puts(t,"alias "); it_puts(t,it_an[i]); it_puts(t,"='"); it_puts(t,it_av[i]); it_puts(t,"'\n"); } return; }
    /* alias NAME=VALUE  or  alias NAME VALUE */
    char nm[IT_NM]; int k=0; int i=0;
    while(s[i]&&s[i]!='='&&s[i]!=' '&&s[i]!='\t'&&k<IT_NM-1) nm[k++]=s[i++];
    nm[k]=0;
    while(s[i]==' '||s[i]=='\t') i++;
    if(s[i]=='='){ i++; while(s[i]==' '||s[i]=='\t') i++; }
    /* strip surrounding quotes */
    const char* val=s+i; char vb[IT_VL]; int vo=0;
    char q=0; if(val[0]=='\''||val[0]=='"'){ q=val[0]; val++; }
    for(int m=0; val[m] && vo<IT_VL-1; m++){ if(q&&val[m]==q) break; vb[vo++]=val[m]; } vb[vo]=0;
    it_set_alias(nm, vb);
}
static void it_cmd_unalias(IdeTerm* t,const char* args){
    char tk[IT_NM]; const char* p=args; if(!it_token(&p,tk,sizeof(tk))) return;
    int i=it_find_alias(tk); if(i<0) return;
    for(int j=i;j<it_na-1;j++){ ide_strlcpy(it_an[j],it_an[j+1],IT_NM); ide_strlcpy(it_av[j],it_av[j+1],IT_VL); } it_na--;
}
static void it_cmd_history(IdeTerm* t){
    int n = t->hist_count < IT_HIST_MAX ? t->hist_count : IT_HIST_MAX;
    for(int off=n-1; off>=0; off--){ const char* h=it_hist_get(t,off); if(!h||!h[0]) continue; it_put_num(t,n-off); it_puts(t,"  "); it_puts(t,h); it_putc(t,'\n'); }
}
static void it_cmd_which(IdeTerm* t,const char* args){
    char tk[IT_NM]; const char* p=args;
    if(!it_token(&p,tk,sizeof(tk))){ it_puts(t,"which: usage: which <name>\n"); return; }
    const char* al=it_get_alias(tk);
    if(al){ it_puts(t,tk); it_puts(t,": alias -> "); it_puts(t,al); it_putc(t,'\n'); return; }
    for(int i=0;i<N_ITCMDS;i++) if(it_eq(g_itcmds[i].name,tk)){ it_puts(t,tk); it_puts(t,": builtin -- "); it_puts(t,g_itcmds[i].help); it_putc(t,'\n'); return; }
    it_puts(t,tk); it_puts(t,": external (/bin/"); it_puts(t,tk); it_puts(t,")\n");
}
static void it_cmd_date(IdeTerm* t){
    struct { unsigned short year; unsigned char month,day,hour,min,sec; } tm;
    tm.year=0; tm.month=tm.day=tm.hour=tm.min=tm.sec=0;
    long r=ide_sc(ITS_GETTIME,(long)&tm,0,0,0,0,0);
    if(r<0){ it_puts(t,"date: unavailable\n"); return; }
    it_put_num(t,tm.year); it_putc(t,'-'); it_put2(t,tm.month); it_putc(t,'-'); it_put2(t,tm.day);
    it_putc(t,' '); it_put2(t,tm.hour); it_putc(t,':'); it_put2(t,tm.min); it_putc(t,':'); it_put2(t,tm.sec); it_putc(t,'\n');
}
static void it_cmd_sleep(IdeTerm* t,const char* args){
    char tk[16]; const char* p=args; if(!it_token(&p,tk,sizeof(tk))){ it_puts(t,"sleep: usage: sleep <ms>\n"); return; }
    int ms=0; for(int i=0;tk[i]>='0'&&tk[i]<='9';i++) ms=ms*10+(tk[i]-'0');
    if(ms>0 && ms<=60000) ide_sc(ITS_SLEEP,ms,0,0,0,0,0);
}
static void it_cmd_kill(IdeTerm* t,const char* args){
    char tk[16]; const char* p=args; if(!it_token(&p,tk,sizeof(tk))){ it_puts(t,"kill: usage: kill <pid>\n"); return; }
    int pid=0; for(int i=0;tk[i]>='0'&&tk[i]<='9';i++) pid=pid*10+(tk[i]-'0');
    if(pid<=1){ it_puts(t,"kill: refusing pid<=1\n"); return; }
    long r=ide_sc(ITS_KILL,pid,9,0,0,0,0);
    if(r<0) it_puts(t,"kill: failed\n"); else { it_puts(t,"killed pid "); it_put_num(t,pid); it_putc(t,'\n'); }
}
static void it_cmd_free(IdeTerm* t){
    /* sysinfo: {total_kb,free_kb,procs,uptime_ms} layout is best-effort. */
    long info[6]; for(int i=0;i<6;i++) info[i]=0;
    long r=ide_sc(ITS_SYSINFO,(long)info,0,0,0,0,0);
    if(r<0){ it_puts(t,"free: unavailable\n"); return; }
    it_puts(t,"mem total "); it_put_num(t,info[0]); it_puts(t," KB, free "); it_put_num(t,info[1]); it_puts(t," KB\n");
}
static void it_cmd_grep(struct Ide* a,IdeTerm* t,const char* args){
    (void)a; char pat[80]; const char* p=args;
    if(!it_token(&p,pat,sizeof(pat))){ it_puts(t,"grep: usage: grep <pat> <file>\n"); return; }
    char ftok[ITKPATH]; if(!it_token(&p,ftok,sizeof(ftok))){ it_puts(t,"grep: missing file\n"); return; }
    const char* path=it_resolve(t,ftok);
    long fd=ide_sc(ITS_OPEN,(long)path,ITO_RDONLY,0,0,0,0);
    if(fd<0){ it_puts(t,"grep: cannot open '"); it_puts(t,ftok); it_puts(t,"'\n"); return; }
    static char rb[2048]; char line[256]; int ll=0, hits=0; long n;
    while((n=ide_sc(ITS_READ,fd,(long)rb,sizeof(rb),0,0,0))>0){
        for(int i=0;i<n;i++){ char c=rb[i];
            if(c=='\n'||ll>=255){ line[ll]=0; if(it_substr(line,pat)){ it_puts(t,line); it_putc(t,'\n'); hits++; } ll=0; if(c!='\n'&&c!='\r') line[ll++]=c; }
            else if(c!='\r') line[ll++]=c; }
    }
    if(ll>0){ line[ll]=0; if(it_substr(line,pat)){ it_puts(t,line); it_putc(t,'\n'); hits++; } }
    ide_sc(ITS_CLOSE,fd,0,0,0,0,0);
    if(hits==0) it_puts(t,"(no matches)\n");
}
static void it_cmd_edit(struct Ide* a,IdeTerm* t,const char* args){
    char tk[ITKPATH]; const char* p=args;
    if(!it_token(&p,tk,sizeof(tk))){ it_puts(t,"edit: usage: edit <file>\n"); return; }
    const char* path=it_resolve(t,tk);
    ide_open_file(a, path);
    a->editor.focused=1; a->term_focus=0;
    it_puts(t,"opened "); it_puts(t,path); it_puts(t," in the editor\n");
}
static void it_cmd_source(struct Ide* a,IdeTerm* t,const char* args,int depth){
    char tk[ITKPATH]; const char* p=args;
    if(!it_token(&p,tk,sizeof(tk))){ it_puts(t,"source: usage: source <file>\n"); return; }
    const char* path=it_resolve(t,tk);
    static char sb[8192];
    int n=ide_read_file(path, sb, (int)sizeof(sb)-1);
    if(n<0){ it_puts(t,"source: cannot read '"); it_puts(t,tk); it_puts(t,"'\n"); return; }
    if(n>(int)sizeof(sb)-1) n=(int)sizeof(sb)-1; sb[n]=0;
    /* run each line as a command line (chaining/vars/aliases honored) */
    char ln[IT_LINEMAX]; int lo=0;
    for(int i=0;i<=n;i++){
        char c = (i<n)? sb[i] : '\n';
        if(c=='\n'){ ln[lo<IT_LINEMAX?lo:IT_LINEMAX-1]=0; it_run_line(a,t,ln,depth+1); lo=0; }
        else if(c!='\r' && lo<IT_LINEMAX-1) ln[lo++]=c;
    }
}
static void it_cmd_repeat(struct Ide* a,IdeTerm* t,const char* args,int depth){
    char tk[16]; const char* p=args;
    if(!it_token(&p,tk,sizeof(tk))){ it_puts(t,"repeat: usage: repeat N <cmd...>\n"); return; }
    int N=0; for(int i=0;tk[i]>='0'&&tk[i]<='9';i++) N=N*10+(tk[i]-'0');
    if(N<=0||N>10000){ it_puts(t,"repeat: bad count\n"); return; }
    const char* body=it_skip_sp(p);
    if(!body[0]){ it_puts(t,"repeat: missing command\n"); return; }
    for(int i=0;i<N;i++) it_run_line(a,t,body,depth+1);
}

/* ---- the single-command dispatcher (post var-expansion) ---- */
static void it_run_one(struct Ide* a, IdeTerm* t, const char* cmdline, int depth){
    cmdline = it_skip_sp(cmdline);
    if(!cmdline[0] || cmdline[0]=='#') return;          /* blank / comment */

    /* bare assignment NAME=VALUE (no spaces in NAME) */
    {
        int eq=-1, ok=1;
        for(int i=0; cmdline[i] && cmdline[i]!=' ' && cmdline[i]!='\t'; i++){
            if(cmdline[i]=='='){ eq=i; break; }
            if(!it_ident_ch(cmdline[i])){ ok=0; break; }
        }
        if(ok && eq>0){
            char nm[IT_NM]; int k=0; for(int i=0;i<eq&&k<IT_NM-1;i++) nm[k++]=cmdline[i]; nm[k]=0;
            it_set_var(nm, cmdline+eq+1); return;
        }
    }

    char cmd[IT_LINEMAX]; const char* p=cmdline;
    it_token(&p, cmd, sizeof(cmd));
    const char* args=p;

    /* one-level alias expansion of the command word */
    const char* al=it_get_alias(cmd);
    if(al && depth<24){
        static char ax[IT_LINEMAX]; int o=0;
        for(int i=0; al[i]&&o<IT_LINEMAX-1; i++) ax[o++]=al[i];
        if(args[0]){ if(o<IT_LINEMAX-1) ax[o++]=' '; for(int i=0; args[i]&&o<IT_LINEMAX-1;i++) ax[o++]=args[i]; }
        ax[o]=0; p=ax; it_token(&p,cmd,sizeof(cmd)); args=p;
    }

    if      (it_eq(cmd,"help"))    it_cmd_help2(t,args);
    else if (it_eq(cmd,"clear"))   { it_clear(t); t->sb_count=0; t->scroll_off=0; }
    else if (it_eq(cmd,"echo"))    { it_puts(t, it_skip_sp(args)); it_putc(t,'\n'); }
    else if (it_eq(cmd,"set"))     it_cmd_set(t,args);
    else if (it_eq(cmd,"unset"))   { char tk[IT_NM]; const char* q=args; if(it_token(&q,tk,sizeof(tk))) it_unset_var(tk); }
    else if (it_eq(cmd,"vars")||it_eq(cmd,"env")) it_cmd_vars(t);
    else if (it_eq(cmd,"alias"))   it_cmd_alias(t,args);
    else if (it_eq(cmd,"unalias")) it_cmd_unalias(t,args);
    else if (it_eq(cmd,"source")||it_eq(cmd,".")) { if(depth<16) it_cmd_source(a,t,args,depth); else it_puts(t,"source: too deep\n"); }
    else if (it_eq(cmd,"repeat"))  { if(depth<16) it_cmd_repeat(a,t,args,depth); else it_puts(t,"repeat: too deep\n"); }
    else if (it_eq(cmd,"history")) it_cmd_history(t);
    else if (it_eq(cmd,"which")||it_eq(cmd,"type")) it_cmd_which(t,args);
    else if (it_eq(cmd,"date"))    it_cmd_date(t);
    else if (it_eq(cmd,"whoami"))  it_puts(t,"root\n");
    else if (it_eq(cmd,"uname"))   it_puts(t,"AutomationOS x86_64\n");
    else if (it_eq(cmd,"sleep"))   it_cmd_sleep(t,args);
    else if (it_eq(cmd,"kill"))    it_cmd_kill(t,args);
    else if (it_eq(cmd,"free")||it_eq(cmd,"mem")) it_cmd_free(t);
    else if (it_eq(cmd,"grep"))    it_cmd_grep(a,t,args);
    else if (it_eq(cmd,"edit"))    it_cmd_edit(a,t,args);
    else if (it_eq(cmd,"pwd"))     { it_puts(t,t->cwd); it_putc(t,'\n'); }
    else if (it_eq(cmd,"cd"))      it_cmd_cd(t,args);
    else if (it_eq(cmd,"ls"))      it_cmd_ls(t,args);
    else if (it_eq(cmd,"cat"))     it_cmd_cat(a,t,args);
    else if (it_eq(cmd,"head"))    it_cmd_head(a,t,args);
    else if (it_eq(cmd,"wc"))      it_cmd_wc(a,t,args);
    else if (it_eq(cmd,"mkdir"))   it_cmd_mkdir(t,args);
    else if (it_eq(cmd,"touch"))   it_cmd_touch(t,args);
    else if (it_eq(cmd,"rm"))      it_cmd_rm(t,args);
    else if (it_eq(cmd,"cp"))      it_cmd_cp(t,args);
    else if (it_eq(cmd,"mv"))      it_cmd_mv(t,args);
    else if (it_eq(cmd,"write"))   it_cmd_write(t,args);
    else if (it_eq(cmd,"ps"))      it_cmd_ps(t);
    else if (it_eq(cmd,"uptime"))  it_cmd_uptime(t);
    else if (it_eq(cmd,"build"))   it_cmd_build(a,t);
    else if (it_eq(cmd,"run"))     it_cmd_run(a,t,args);
    else if (it_eq(cmd,"spawn"))   it_cmd_spawn(t,args);
    else {
        if(!it_try_external(t,cmd,args)){ it_puts(t,cmd); it_puts(t,": command not found (try 'help')\n"); }
    }
}

/* ---- one input line: expand vars, split on ';', run each segment ---- */
static void it_run_line(struct Ide* a, IdeTerm* t, const char* line, int depth){
    if(depth>32) return;
    int i=0;
    while(line[i]){
        /* collect one ';'-delimited segment */
        char seg[IT_LINEMAX]; int s=0;
        while(line[i] && line[i]!=';' && s<IT_LINEMAX-1) seg[s++]=line[i++];
        seg[s]=0;
        if(line[i]==';') i++;
        char exp[IT_LINEMAX];
        it_expand(seg, exp, sizeof(exp));
        it_run_one(a, t, exp, depth);
    }
}

static void it_execute(struct Ide* a, IdeTerm* t) {
    /* Push to history before executing (so up-arrow recalls even if cmd fails) */
    if (t->line[0]) it_hist_push(t, t->line);
    it_putc(t, '\n');
    it_run_line(a, t, t->line, 0);
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
    t->line_cur = 0;
    t->comp_active = 0; t->comp_count = 0; t->comp_sel = 0; t->comp_wstart = 0;
    t->blink_ms = 0;
    t->hist_count = 0;
    t->hist_nav = -1;
    t->sb_count = 0;
    t->scroll_off = 0;
    for (int i = 0; i < IT_SCROLLBACK; i++) {
        for (int c = 0; c < IT_MAXCOLS; c++) t->sb[i][c] = ' ';
        t->sb_cols[i] = 80;
    }
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

void ide_term_scroll(IdeTerm* t, int delta) {
    int max_off = t->sb_count < IT_SCROLLBACK ? t->sb_count : IT_SCROLLBACK;
    t->scroll_off += delta;
    if (t->scroll_off < 0) t->scroll_off = 0;
    if (t->scroll_off > max_off) t->scroll_off = max_off;
}

void ide_term_puts(IdeTerm* t, const char* s) {
    it_puts(t, s);
}

/* ============================================================================
 * Tab autocomplete: command names for the first word, file paths after. A small
 * popup lists candidates; repeated Tab cycles through them.
 * ==========================================================================*/
static int it_word_bounds(IdeTerm* t){
    int w=t->line_cur;
    while(w>0 && t->line[w-1]!=' ' && t->line[w-1]!='\t') w--;
    return w;
}
static int it_first_word(IdeTerm* t,int wstart){
    for(int i=0;i<wstart;i++) if(t->line[i]!=' '&&t->line[i]!='\t') return 0;
    return 1;
}
static void it_comp_add(IdeTerm* t,const char* prefix,int plen,const char* cand){
    if(t->comp_count>=IT_COMP_MAX) return;
    for(int i=0;i<plen;i++) if(cand[i]!=prefix[i]) return;   /* cand must start with prefix */
    for(int i=0;i<t->comp_count;i++){ int j=0; while(t->comp[i][j]&&cand[j]&&t->comp[i][j]==cand[j]) j++;
        if(t->comp[i][j]==0&&cand[j]==0) return; }            /* dedup */
    int k=0; while(cand[k]&&k<IT_COMP_W-1){ t->comp[t->comp_count][k]=cand[k]; k++; }
    t->comp[t->comp_count][k]=0; t->comp_count++;
}
static void it_collect(IdeTerm* t){
    t->comp_count=0;
    int wstart=it_word_bounds(t);
    t->comp_wstart=wstart;
    int wlen=t->line_cur-wstart; if(wlen<0) wlen=0;
    char word[IT_COMP_W]; int wl=0;
    for(int i=0;i<wlen&&wl<IT_COMP_W-1;i++) word[wl++]=t->line[wstart+i]; word[wl]=0;

    if(it_first_word(t,wstart)){
        for(int i=0;i<N_ITCMDS;i++) it_comp_add(t,word,wl,g_itcmds[i].name);
        for(int i=0;i<it_na;i++)    it_comp_add(t,word,wl,it_an[i]);
        return;
    }
    /* path completion: split the word into dir + base */
    int slash=-1; for(int i=0;i<wl;i++) if(word[i]=='/') slash=i;
    char dirpart[ITKPATH]; int dl=0;
    if(slash>=0){ for(int i=0;i<=slash&&dl<ITKPATH-1;i++) dirpart[dl++]=word[i]; }
    dirpart[dl]=0;
    const char* rdir=it_resolve(t, dirpart[0]?dirpart:"");
    long fd=ide_sc(ITS_OPENDIR,(long)rdir,0,0,0,0,0);
    if(fd<0) return;
    IdeDirent de;
    for(;;){
        long r=ide_sc(ITS_READDIR,fd,(long)&de,0,0,0,0); if(r!=0) break;
        de.name[255]=0; if(de.name[0]==0) continue;
        if(de.name[0]=='.'&&(de.name[1]==0||(de.name[1]=='.'&&de.name[2]==0))) continue;
        char cand[IT_COMP_W]; int co=0;
        for(int i=0;dirpart[i]&&co<IT_COMP_W-1;i++) cand[co++]=dirpart[i];
        for(int i=0;de.name[i]&&co<IT_COMP_W-2;i++) cand[co++]=de.name[i];
        if(de.type==ITDT_DIR && co<IT_COMP_W-1) cand[co++]='/';
        cand[co]=0;
        it_comp_add(t, word, wl, cand);
    }
    ide_sc(ITS_CLOSEDIR,fd,0,0,0,0,0);
}
static void it_apply_comp(IdeTerm* t,int idx){
    if(idx<0||idx>=t->comp_count) return;
    const char* cand=t->comp[idx];
    int wstart=t->comp_wstart;
    if(wstart<0||wstart>t->line_len) return;
    char nl[IT_LINEMAX]; int o=0;
    for(int i=0;i<wstart&&o<IT_LINEMAX-1;i++) nl[o++]=t->line[i];
    for(int i=0;cand[i]&&o<IT_LINEMAX-1;i++) nl[o++]=cand[i];
    int ccur=o;
    for(int i=t->line_cur;i<t->line_len&&o<IT_LINEMAX-1;i++) nl[o++]=t->line[i];
    nl[o]=0;
    ide_strlcpy(t->line, nl, IT_LINEMAX);
    t->line_len=o; t->line_cur=ccur;
    it_redraw_input(t);
}
static void it_tab(IdeTerm* t){
    int wstart=it_word_bounds(t);
    if(t->comp_active && wstart==t->comp_wstart && t->comp_count>0){
        t->comp_sel=(t->comp_sel+1)%t->comp_count;   /* cycle */
        it_apply_comp(t,t->comp_sel);
        return;
    }
    it_collect(t);
    if(t->comp_count==0){ t->comp_active=0; return; }
    t->comp_sel=0;
    it_apply_comp(t,0);
    t->comp_active=(t->comp_count>1);   /* keep the popup only if ambiguous */
}

int ide_term_key(struct Ide* a, int keycode, char ch, int shift, int ctrl) {
    IdeTerm* t = &a->term;
    (void)shift;
    if (ctrl) return 0;   /* Ctrl-chords routed by caller */

    /* Tab autocomplete: Tab completes/cycles; Esc dismisses the popup; any other
     * key dismisses it (then proceeds with normal handling). */
    if (keycode == ITK_TAB) { it_tab(t); return 1; }
    if (keycode == 1 /* KEY_ESC */ && t->comp_active) { t->comp_active = 0; return 1; }
    t->comp_active = 0;

    /* Scrollback navigation */
    if (keycode == ITK_PAGEUP) {
        ide_term_scroll(t, t->rows > 1 ? t->rows - 1 : 1);
        return 1;
    }
    if (keycode == ITK_PAGEDOWN) {
        ide_term_scroll(t, -(t->rows > 1 ? t->rows - 1 : 1));
        return 1;
    }
    if (keycode == ITK_HOME && shift) {
        /* Shift+Home: scroll to top of scrollback */
        int max_off = t->sb_count < IT_SCROLLBACK ? t->sb_count : IT_SCROLLBACK;
        t->scroll_off = max_off;
        return 1;
    }
    if (keycode == ITK_END && shift) {
        /* Shift+End: snap to live */
        t->scroll_off = 0;
        return 1;
    }

    /* Any typing snaps to live view */
    t->scroll_off = 0;

    if (keycode == ITK_ENTER) { it_execute(a, t); return 1; }

    /* ---- in-line cursor editing (mid-line, not just append) ---- */
    if (keycode == ITK_LEFT) {
        if (t->line_cur > 0) t->line_cur--;
        t->cur_row = t->input_row; t->cur_col = t->input_col0 + t->line_cur;
        return 1;
    }
    if (keycode == ITK_RIGHT) {
        if (t->line_cur < t->line_len) t->line_cur++;
        t->cur_row = t->input_row; t->cur_col = t->input_col0 + t->line_cur;
        return 1;
    }
    if (keycode == ITK_HOME) {            /* (Shift+Home scrollback handled above) */
        t->line_cur = 0;
        t->cur_row = t->input_row; t->cur_col = t->input_col0;
        return 1;
    }
    if (keycode == ITK_END) {             /* (Shift+End handled above) */
        t->line_cur = t->line_len;
        t->cur_row = t->input_row; t->cur_col = t->input_col0 + t->line_cur;
        return 1;
    }
    if (keycode == ITK_BACKSPACE) {
        if (t->line_cur > 0) {
            for (int i = t->line_cur - 1; i < t->line_len - 1; i++) t->line[i] = t->line[i + 1];
            t->line_len--; t->line_cur--; t->line[t->line_len] = 0;
            it_redraw_input(t);
        }
        return 1;
    }
    if (keycode == ITK_DELETE) {
        if (t->line_cur < t->line_len) {
            for (int i = t->line_cur; i < t->line_len - 1; i++) t->line[i] = t->line[i + 1];
            t->line_len--; t->line[t->line_len] = 0;
            it_redraw_input(t);
        }
        return 1;
    }
    /* (Tab is handled at the top of this function for autocomplete.) */

    /* Arrow key history navigation (up/down) -- loads a recalled command and
     * parks the cursor at end of line, then repaints the whole input row. */
    if (keycode == ITK_UP) {   /* recall previous command */
        int available = t->hist_count < IT_HIST_MAX ? t->hist_count : IT_HIST_MAX;
        if (available > 0) {
            int next_nav = t->hist_nav + 1;
            if (next_nav < available) {
                const char* prev = it_hist_get(t, next_nav);
                if (prev) {
                    ide_strlcpy(t->line, prev, IT_LINEMAX);
                    t->line_len = ide_strlen(t->line);
                    t->line_cur = t->line_len;
                    it_redraw_input(t);
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
                ide_strlcpy(t->line, next, IT_LINEMAX);
                t->line_len = ide_strlen(t->line);
                t->line_cur = t->line_len;
                it_redraw_input(t);
                t->hist_nav = next_nav;
            }
        } else if (t->hist_nav == 0) {
            t->line[0] = 0; t->line_len = 0; t->line_cur = 0;
            it_redraw_input(t);
            t->hist_nav = -1;
        }
        return 1;
    }

    if (ch >= 32 && ch < 127) {           /* insert a printable char at the cursor */
        if (t->line_len < IT_LINEMAX - 1) {
            for (int i = t->line_len; i > t->line_cur; i--) t->line[i] = t->line[i - 1];
            t->line[t->line_cur] = ch;
            t->line_len++; t->line_cur++;
            t->line[t->line_len] = 0;
            it_redraw_input(t);
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

    if (t->scroll_off > 0) {
        /* ---- scrollback view: show historical lines from the ring buffer,
         * then the visible grid rows that are still on screen. ---- */
        int sb_avail = t->sb_count < IT_SCROLLBACK ? t->sb_count : IT_SCROLLBACK;
        /* We want to show `rows` lines total. The bottom of the view is
         * (live grid bottom - scroll_off) lines back. We construct the view
         * by pulling from the scrollback ring + the live grid. */

        /* Total logical lines = sb_avail (scrollback) + cur_row+1 (live grid rows with content) */
        int live_rows = t->cur_row + 1;
        int total_logical = sb_avail + live_rows;
        /* Bottom visible line index (0-based from the top of all logical lines) */
        int bottom_idx = total_logical - 1 - t->scroll_off;
        if (bottom_idx < 0) bottom_idx = 0;
        int top_idx = bottom_idx - rows + 1;
        if (top_idx < 0) top_idx = 0;

        for (int screen_row = 0; screen_row < rows; screen_row++) {
            int py = oy + screen_row * GFX_FH;
            if (py + GFX_FH > body.y + body.h) break;
            int logical = top_idx + screen_row;
            if (logical > bottom_idx) break;

            if (logical < sb_avail) {
                /* this line is in the scrollback ring */
                int ring_idx = (t->sb_count - sb_avail + logical) % IT_SCROLLBACK;
                for (int cc = 0; cc < cols; cc++) {
                    char chr[2] = { t->sb[ring_idx][cc], 0 };
                    if (chr[0] == ' ' || chr[0] == 0) continue;
                    int px = ox + cc * GFX_FW;
                    gfx_text_clip(cv, px, py, chr, IT_DIM, ox, cols * GFX_FW);
                }
            } else {
                /* this line is in the live grid */
                int grid_row = logical - sb_avail;
                if (grid_row >= 0 && grid_row < IT_MAXROWS) {
                    for (int cc = 0; cc < cols; cc++) {
                        char chr[2] = { t->grid[grid_row][cc], 0 };
                        if (chr[0] == ' ' || chr[0] == 0) continue;
                        int px = ox + cc * GFX_FW;
                        gfx_text_clip(cv, px, py, chr, IT_FG, ox, cols * GFX_FW);
                    }
                }
            }
        }

        /* scrollback indicator */
        char ind[32]; int ip = 0;
        ind[ip++] = '['; ind[ip++] = 's'; ind[ip++] = 'c'; ind[ip++] = 'r';
        ind[ip++] = 'o'; ind[ip++] = 'l'; ind[ip++] = 'l'; ind[ip++] = ':';
        ind[ip++] = ' ';
        { char nb[12]; int nn = ide_itoa(t->scroll_off, nb);
          for (int i = 0; i < nn; i++) ind[ip++] = nb[i]; }
        ind[ip++] = ']'; ind[ip] = 0;
        int iw = gfx_textw(ind);
        int ix = body.x + body.w - PAD - iw;
        int iy = body.y + 2;
        gfx_text_clip(cv, ix, iy, ind, TH_ORANGE, ox, body.w);
    } else {
        /* ---- live view: render the grid directly ---- */
        for (int rr = 0; rr < rows; rr++) {
            int py = oy + rr * GFX_FH;
            if (py + GFX_FH > body.y + body.h) break;
            for (int cc = 0; cc < cols; cc++) {
                char chr[2] = { t->grid[rr][cc], 0 };
                if (chr[0] == ' ' || chr[0] == 0) continue;
                int px = ox + cc * GFX_FW;
                gfx_text_clip(cv, px, py, chr, IT_FG, ox, cols * GFX_FW);
            }
        }

        /* caret block when focused + blink on + live view */
        if (a->term_focus && t->blink_ms < IT_BLINK) {
            int px = ox + t->cur_col * GFX_FW;
            int py = oy + t->cur_row * GFX_FH;
            if (py + GFX_FH <= body.y + body.h)
                gfx_fill(cv, px, py, GFX_FW, GFX_FH, IT_CURSOR);
            /* re-draw the char under the caret in the bg color for contrast */
            char chr[2] = { t->grid[t->cur_row][t->cur_col], 0 };
            if (chr[0] != ' ' && chr[0] != 0)
                gfx_text_clip(cv, px, py, chr, IT_BG, ox, cols * GFX_FW);
        }

        /* ---- Tab autocomplete popup overlay (drawn over the grid) ---- */
        if (a->term_focus && t->comp_active && t->comp_count > 0) {
            int n = t->comp_count;
            int wmax = 6;
            for (int i = 0; i < n; i++) { int w = 0; while (t->comp[i][w]) w++; if (w > wmax) wmax = w; }
            int bw = (wmax + 2) * GFX_FW;
            int bh = n * GFX_FH + 2;
            int wcol = t->input_col0 + t->comp_wstart;     /* grid col of the word start */
            int bx = ox + wcol * GFX_FW;
            if (bx + bw > body.x + body.w - PAD) bx = body.x + body.w - PAD - bw;
            if (bx < body.x + PAD) bx = body.x + PAD;
            int by = oy + t->input_row * GFX_FH - bh;        /* above the input row */
            if (by < body.y) by = body.y;                    /* clamp to panel top */
            if (by + bh > body.y + body.h) by = body.y;      /* short panel: anchor top */
            gfx_fill(cv, bx - 1, by - 1, bw + 2, bh + 2, TH_BORDER);
            gfx_fill(cv, bx, by, bw, bh, TH_PANEL);
            for (int i = 0; i < n; i++) {
                int ry = by + 1 + i * GFX_FH;
                if (ry + GFX_FH > body.y + body.h) break;    /* don't bleed into the tab strip */
                if (i == t->comp_sel) gfx_fill(cv, bx, ry, bw, GFX_FH, TH_SELECT);
                gfx_text_clip(cv, bx + GFX_FW, ry, t->comp[i],
                              i == t->comp_sel ? TH_TEXT : IT_DIM, bx, bw);
            }
        }
    }
}
