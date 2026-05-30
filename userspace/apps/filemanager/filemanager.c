/*
 * filemanager.c -- "Files" GUI app, Windows 11 File Explorer style.
 * =================================================================
 *
 * A from-scratch, freestanding (ring 3, NO libc) file explorer that draws
 * EVERYTHING itself by blitting ARGB32 pixels into a compositor SHM window
 * (userspace/lib/wl) using the 8x16 bitmap font (userspace/lib/font). It does
 * NOT use the dark "ui" toolkit -- a custom light theme is required for the
 * Windows 11 look (left sidebar, breadcrumb + back/forward toolbar, an icon
 * grid with rounded selection highlight, inline rename, a preview pane).
 *
 * Window: 960x600, light theme.
 *   +----------------------------------------------------------------+
 *   | [<][>][^]  [ New folder ]            Files            (toolbar) |  TOOLBAR_H
 *   | > Home / sub / dir                                  (address)   |  ADDR_H
 *   +----------+-----------------------------------------------------+
 *   | Quick    |  [icon] name     [icon] name     [icon] name        |
 *   | access   |  [icon] name     [icon] name     ...                |  content
 *   | This PC  |                                                     |  (icon grid)
 *   | bin/     |                                                     |
 *   | etc/ ... |                          (preview pane on a file)   |
 *   +----------+-----------------------------------------------------+
 *   | N items | selected: name (size)                   (status bar) |  STATUS_H
 *   +----------------------------------------------------------------+
 *
 * Interaction (the bug the user hit was: rows weren't clickable & a new
 * folder never appeared). This rewrite makes hit-testing explicit and
 * RE-SCANS after every mutation:
 *   - single left click on an item  -> select it (rounded blue highlight)
 *   - double left click on an item  -> OPEN: folder navigates in; file opens
 *                                      a preview pane (text peek / info)
 *   - sidebar entry click           -> navigate to that location
 *   - [<] / [>]                     -> history back / forward
 *   - [^]                           -> parent directory
 *   - [ New folder ]                -> SYS_MKDIR a unique "New folder" then
 *                                      RE-SCAN so it shows immediately, with
 *                                      the new item selected + in rename mode
 *   - F2 (or click selected name)   -> inline rename (SYS_RENAME on Enter)
 *   - Backspace                     -> go up a directory (when not renaming)
 *
 * Real filesystem via VFS syscalls (AOS numbers, NOT Linux):
 *   SYS_WRITE 3, SYS_OPEN 4, SYS_READ 2, SYS_CLOSE 5, SYS_SPAWN 16,
 *   SYS_OPENDIR 30, SYS_READDIR 31, SYS_CLOSEDIR 32, SYS_STAT 33,
 *   SYS_RENAME 35, SYS_GET_TICKS_MS 40, SYS_YIELD 15, SYS_MKDIR 67.
 *
 * Build (NO fs:0x28 canary -- verified with objdump):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/filemanager/filemanager.c -o /tmp/filemanager.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/filemanager.o /tmp/wlc.o /tmp/bf.o -o /tmp/filemanager.elf
 *
 * Links: wl_client.o + bitfont.o   (same as build_ui_app minus ui.o; the
 * lead can keep `build_ui_app userspace/apps/filemanager/filemanager.c
 * filemanager` -- linking the extra ui.o is harmless, our code references
 * none of it. See the report for the one-line note.)
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* All local-buffer functions are annotated so the host GCC's hardening
 * heuristic cannot inject a %fs:0x28 stack canary (no libssp to link). */
#define NO_SSP __attribute__((no_stack_protector))

/* Some toolchains emit a canary in sibling TUs (wl_client.c / bitfont.c).
 * Provide a trivial resolver so the freestanding link always succeeds; on the
 * intended toolchain no canary is emitted and this symbol is simply unused. */
__attribute__((noreturn)) void __stack_chk_fail(void)
{
    for (;;) { }
}

/* ----------------------------------------------------------------------- */
/* Syscalls                                                                */
/* ----------------------------------------------------------------------- */
#define SYS_READ     2
#define SYS_WRITE    3
#define SYS_OPEN     4
#define SYS_CLOSE    5
#define SYS_YIELD    15
#define SYS_SPAWN    16
#define SYS_OPENDIR  30
#define SYS_READDIR  31
#define SYS_CLOSEDIR 32
#define SYS_STAT     33
#define SYS_RENAME   35
#define SYS_GET_TICKS_MS 40
#define SYS_MKDIR    67

#define O_RDONLY     0x0000

static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* 6-arg variant for SYS_SPAWN(path, args, ...) and the like. */
static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ----------------------------------------------------------------------- */
/* dirent / stat (must match kernel/include/vfs.h)                         */
/* ----------------------------------------------------------------------- */
#define DT_DIR   4
#define DT_REG   8
#define NAME_MAX 256

struct dirent {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX];
};

typedef struct {
    unsigned long long st_dev, st_ino;
    unsigned int       st_mode, st_nlink, st_uid, st_gid;
    unsigned long long st_rdev, st_size, st_blksize, st_blocks;
    unsigned long long st_atime, st_mtime, st_ctime;
} fm_stat_t;

/* ----------------------------------------------------------------------- */
/* Freestanding string helpers                                             */
/* ----------------------------------------------------------------------- */
static unsigned long s_len(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

static void s_cpy(char *dst, const char *src, int n)
{
    int i = 0;
    if (n <= 0) return;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void s_cat(char *dst, const char *src, int n)
{
    int len = 0; while (dst[len]) len++;
    int i = 0;
    while (len + i < n - 1 && src[i]) { dst[len + i] = src[i]; i++; }
    dst[len + i] = '\0';
}

static int s_eq(const char *a, const char *b) { int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == b[i]; }

static void s_zero(char *b, int n) { for (int i = 0; i < n; i++) b[i] = '\0'; }

static void serial(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)s_len(m)); }

NO_SSP static void fmt_u64(char *buf, unsigned long long v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char t[22]; int i = 0;
    while (v > 0 && i < 21) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    t[i] = '\0';
    int lo = 0, hi = i - 1;
    while (lo < hi) { char c = t[lo]; t[lo] = t[hi]; t[hi] = c; lo++; hi--; }
    s_cpy(buf, t, 22);
}

NO_SSP static void fmt_int(char *buf, int v)
{
    if (v < 0) { buf[0] = '-'; fmt_u64(buf + 1, (unsigned long long)(-v)); }
    else fmt_u64(buf, (unsigned long long)v);
}

NO_SSP static void fmt_size(char *buf, unsigned long long sz)
{
    char nb[22];
    if (sz >= 1024ULL * 1024ULL) { fmt_u64(nb, sz / (1024ULL * 1024ULL)); s_cpy(buf, nb, 20); s_cat(buf, " MB", 20); }
    else if (sz >= 1024ULL)      { fmt_u64(nb, sz / 1024ULL);            s_cpy(buf, nb, 20); s_cat(buf, " KB", 20); }
    else                         { fmt_u64(nb, sz);                       s_cpy(buf, nb, 20); s_cat(buf, " bytes", 20); }
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ends_with(const char *name, const char *ext)
{
    int nl = (int)s_len(name), el = (int)s_len(ext);
    if (el > nl) return 0;
    const char *t = name + nl - el;
    for (int i = 0; ext[i]; i++) if (lc(t[i]) != lc(ext[i])) return 0;
    return 1;
}

/* ----------------------------------------------------------------------- */
/* Path helpers                                                            */
/* ----------------------------------------------------------------------- */
#define PATHLEN 512
#define NAMELEN 96

NO_SSP static void path_join(char *out, int outsz, const char *dir, const char *name)
{
    s_cpy(out, dir, outsz);
    int l = (int)s_len(out);
    if (l == 0 || out[l - 1] != '/') s_cat(out, "/", outsz);
    s_cat(out, name, outsz);
}

static void path_parent(char *path)
{
    int len = (int)s_len(path);
    if (len <= 1) { path[0] = '/'; path[1] = '\0'; return; }
    int i = len - 1;
    if (path[i] == '/') i--;
    while (i > 0 && path[i] != '/') i--;
    if (i == 0) { path[0] = '/'; path[1] = '\0'; }
    else        { path[i] = '\0'; }
}

/* ----------------------------------------------------------------------- */
/* Window + theme                                                          */
/* ----------------------------------------------------------------------- */
#define WIN_W       960
#define WIN_H       600
#define TOOLBAR_H   48
#define ADDR_H      40
#define STATUS_H    28
#define SIDEBAR_W   190
#define HEADER_TOP  (TOOLBAR_H + ADDR_H)             /* 88 */
#define CONTENT_Y   HEADER_TOP
#define CONTENT_X   SIDEBAR_W

/* Windows 11 light palette (ARGB, opaque). */
#define COL_CHROME    0xFFF3F3F3u   /* toolbar / address strip            */
#define COL_SIDEBAR   0xFFEBEBEBu   /* sidebar background                 */
#define COL_CONTENT   0xFFFFFFFFu   /* main view background               */
#define COL_BORDER    0xFFE1E1E1u   /* hairline separators                */
#define COL_TEXT      0xFF1B1B1Bu   /* primary text                       */
#define COL_TEXTDIM   0xFF6A6A6Au   /* secondary text                     */
#define COL_ACCENT    0xFF0067C0u   /* Win11 blue                         */
#define COL_SELBG     0xFFCCE4F7u   /* selected item fill (light blue)    */
#define COL_SELBORDER 0xFF99CCEEu   /* selected item border               */
#define COL_HOVER     0xFFF0F0F0u   /* hover fill                         */
#define COL_BTN       0xFFFBFBFBu   /* toolbar button face                */
#define COL_BTNHOVER  0xFFEAEAEAu
#define COL_FOLDER    0xFFFFC75Bu   /* folder icon body (amber)           */
#define COL_FOLDER2   0xFFE8A83Cu   /* folder icon shade                  */
#define COL_FILE      0xFFFFFFFFu   /* file icon page                     */
#define COL_FILEEDGE  0xFFB9B9B9u   /* file icon outline                  */

/* ----------------------------------------------------------------------- */
/* Drawing primitives (ARGB32, stride in pixels)                           */
/* ----------------------------------------------------------------------- */
static wl_window *g_win;
static unsigned int *FB;          /* g_win->pixels                         */
static int FBW, FBH, FBS;         /* width, height, stride in PIXELS        */

static inline void px(int x, int y, unsigned int c)
{
    if (x < 0 || y < 0 || x >= FBW || y >= FBH) return;
    FB[y * FBS + x] = c;
}

NO_SSP static void fill(int x, int y, int w, int h, unsigned int c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FBW) w = FBW - x;
    if (y + h > FBH) h = FBH - y;
    for (int j = 0; j < h; j++) {
        unsigned int *row = FB + (long)(y + j) * FBS + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void hline(int x, int y, int w, unsigned int c) { fill(x, y, w, 1, c); }
static void vline(int x, int y, int h, unsigned int c) { fill(x, y, 1, h, c); }

static void rect_outline(int x, int y, int w, int h, unsigned int c)
{
    hline(x, y, w, c); hline(x, y + h - 1, w, c);
    vline(x, y, h, c); vline(x + w - 1, y, h, c);
}

/* Rounded filled rectangle (small radius, simple corner clipping). */
NO_SSP static void fill_round(int x, int y, int w, int h, int r, unsigned int c)
{
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int j = 0; j < h; j++) {
        int dy = (j < r) ? (r - 1 - j) : (j >= h - r ? j - (h - r) : -1);
        int inset = 0;
        if (dy >= 0) {
            /* distance from the rounded corner: clip a quarter-circle. */
            int k = r - 1 - dy;          /* 0..r-1 */
            /* x inset where x*x + (r-1-k)*k... approximate with a table-free
             * circle test: inset = r - floor(sqrt(r^2 - dy^2)). */
            int rr = r * r, dd = dy * dy;
            int sq = 0; while ((sq + 1) * (sq + 1) <= rr - dd) sq++;
            inset = r - sq;
            (void)k;
        }
        fill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

/* Rounded outline (used for the selection ring). */
NO_SSP static void round_outline(int x, int y, int w, int h, int r, unsigned int c)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    /* top & bottom straight runs */
    hline(x + r, y, w - 2 * r, c);
    hline(x + r, y + h - 1, w - 2 * r, c);
    vline(x, y + r, h - 2 * r, c);
    vline(x + w - 1, y + r, h - 2 * r, c);
    for (int i = 0; i < r; i++) {
        int rr = r * r, dd = (r - 1 - i) * (r - 1 - i);
        int sq = 0; while ((sq + 1) * (sq + 1) <= rr - dd) sq++;
        int off = r - sq;
        px(x + off, y + i, c);
        px(x + w - 1 - off, y + i, c);
        px(x + off, y + h - 1 - i, c);
        px(x + w - 1 - off, y + h - 1 - i, c);
    }
}

static void text(int x, int y, const char *s, unsigned int c)
{
    font_draw_string(FB, FBS, FBW, FBH, x, y, s, c);
}

/* Draw a string clipped to a pixel width (elide with ".." if too long). */
NO_SSP static void text_clip(int x, int y, const char *s, int maxw, unsigned int c)
{
    int maxch = maxw / FONT_W;
    if (maxch <= 0) return;
    int len = (int)s_len(s);
    if (len <= maxch) { text(x, y, s, c); return; }
    char buf[80];
    int keep = maxch - 2;
    if (keep < 1) keep = 1;
    if (keep > (int)sizeof(buf) - 3) keep = (int)sizeof(buf) - 3;
    int i;
    for (i = 0; i < keep; i++) buf[i] = s[i];
    buf[i++] = '.'; buf[i++] = '.'; buf[i] = '\0';
    text(x, y, buf, c);
}

/* Centered text within [x, x+w). */
static void text_center(int x, int y, int w, const char *s, unsigned int c)
{
    int tw = (int)s_len(s) * FONT_W;
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    text(tx, y, s, c);
}

/* ----------------------------------------------------------------------- */
/* Icons: a folder glyph and a document glyph (drawn from rects).          */
/* ----------------------------------------------------------------------- */
NO_SSP static void icon_folder(int x, int y)   /* 48x40 tile, centered art */
{
    int fx = x + 6, fy = y + 10, fw = 36, fh = 24;
    /* back tab */
    fill_round(fx, fy - 6, 16, 10, 2, COL_FOLDER2);
    /* body */
    fill_round(fx, fy, fw, fh, 3, COL_FOLDER);
    /* subtle front lip highlight */
    fill(fx + 2, fy + 2, fw - 4, 3, 0xFFFFD98Au);
}

NO_SSP static void icon_file(int x, int y, char kind)
{
    int fx = x + 12, fy = y + 6, fw = 24, fh = 30;
    fill_round(fx, fy, fw, fh, 3, COL_FILE);
    round_outline(fx, fy, fw, fh, 3, COL_FILEEDGE);
    /* folded corner */
    fill(fx + fw - 8, fy, 8, 8, COL_SIDEBAR);
    hline(fx + fw - 8, fy + 8, 8, COL_FILEEDGE);
    vline(fx + fw - 8, fy, 8, COL_FILEEDGE);
    /* type tint bar + glyph */
    unsigned int tint = 0xFF8A8A8Au;
    if (kind == 'C') tint = 0xFF3E78C8u;   /* code   */
    else if (kind == 'E') tint = 0xFF6FAE5Bu;   /* exe   */
    else if (kind == 'T') tint = 0xFF9A6FC8u;   /* text  */
    else if (kind == 'A') tint = 0xFFC87A3Eu;   /* asm   */
    fill(fx + 4, fy + fh - 9, fw - 8, 4, tint);
    char g[2] = { kind, 0 };
    if (kind != '-') text(fx + (fw - FONT_W) / 2, fy + 8, g, tint);
    /* a couple of "text lines" */
    fill(fx + 4, fy + 6, fw - 12, 2, 0xFFD8D8D8u);
    fill(fx + 4, fy + 12, fw - 8, 2, 0xFFE2E2E2u);
}

/* ----------------------------------------------------------------------- */
/* Model                                                                   */
/* ----------------------------------------------------------------------- */
#define MAXENT 256

typedef struct {
    char               name[NAMELEN];
    unsigned char      type;
    unsigned long long size;
} entry_t;

static char     g_cwd[PATHLEN];
static entry_t  g_ent[MAXENT];
static int      g_nent;
static int      g_sel = -1;          /* selected index, -1 none            */
static int      g_scroll;            /* first visible ROW (grid row)       */
static int      g_dirty = 1;         /* repaint requested                  */
static char     g_pathbuf[PATHLEN];  /* all syscall path args go here      */

/* History (back/forward) ring. */
#define HIST_MAX 64
static char g_hist[HIST_MAX][PATHLEN];
static int  g_hist_n;                /* number of entries                  */
static int  g_hist_i;               /* current index into g_hist          */

/* Double-click detection. */
static long g_last_click_ms;
static int  g_last_click_idx = -2;

/* Inline rename state. */
static int  g_renaming;              /* editing g_sel's name?              */
static char g_renbuf[NAMELEN];
static int  g_renlen;

/* Preview pane (shown when a file is opened). */
static int  g_preview;               /* preview active?                    */
static char g_prev_name[NAMELEN];
static unsigned long long g_prev_size;
static char g_prev_text[2048];       /* first bytes of the file            */
static int  g_prev_len;

/* Grid metrics. */
#define CELL_W   132
#define CELL_H   104
#define ICON_DY  10
#define PAD_X    18
#define PAD_Y    12

static int grid_cols(void)
{
    int avail = (FBW - CONTENT_X) - PAD_X;
    int c = avail / CELL_W;
    if (c < 1) c = 1;
    return c;
}

static int content_h_px(void) { return FBH - CONTENT_Y - STATUS_H; }
static int grid_rows_visible(void) { int r = content_h_px() / CELL_H; if (r < 1) r = 1; return r; }

/* ----------------------------------------------------------------------- */
/* Type glyph                                                              */
/* ----------------------------------------------------------------------- */
static char entry_glyph(const entry_t *e)
{
    if (e->type == DT_DIR) return 'D';
    if (ends_with(e->name, ".c") || ends_with(e->name, ".h")) return 'C';
    if (ends_with(e->name, ".asm") || ends_with(e->name, ".s")) return 'A';
    if (ends_with(e->name, ".elf") || ends_with(e->name, ".o")) return 'E';
    if (ends_with(e->name, ".md") || ends_with(e->name, ".txt")) return 'T';
    return '-';
}

/* ----------------------------------------------------------------------- */
/* Directory scan -> g_ent (dirs first, then files; both insertion order)  */
/* ----------------------------------------------------------------------- */
NO_SSP static void do_scan(const char *path)
{
    g_nent = 0;
    g_scroll = 0;

    s_zero(g_pathbuf, PATHLEN);
    s_cpy(g_pathbuf, path, PATHLEN);
    long fd = sc(SYS_OPENDIR, (long)g_pathbuf, 0, 0);
    if (fd < 0) {
        serial("[FILES] opendir failed: "); serial(g_pathbuf); serial("\n");
        return;
    }

    /* First pass: directories. Second pass would need rewind; instead we
     * collect everything then stable-partition dirs to the front. */
    struct dirent ent;
    while (g_nent < MAXENT) {
        long r = sc(SYS_READDIR, fd, (long)&ent, 0);
        if (r != 0) break;
        if (ent.d_name[0] == '.' &&
            (ent.d_name[1] == '\0' || (ent.d_name[1] == '.' && ent.d_name[2] == '\0')))
            continue;
        entry_t *e = &g_ent[g_nent];
        s_cpy(e->name, ent.d_name, NAMELEN);
        e->type = ent.d_type;
        e->size = 0;
        if (ent.d_type != DT_DIR) {
            s_zero(g_pathbuf, PATHLEN);
            path_join(g_pathbuf, PATHLEN, path, ent.d_name);
            fm_stat_t st;
            if (sc(SYS_STAT, (long)g_pathbuf, (long)&st, 0) == 0) e->size = st.st_size;
        }
        g_nent++;
    }
    sc(SYS_CLOSEDIR, fd, 0, 0);

    /* Stable partition: directories first. */
    for (int i = 0; i < g_nent; i++) {
        if (g_ent[i].type == DT_DIR) continue;
        /* find next dir after i */
        int j = i + 1;
        while (j < g_nent && g_ent[j].type != DT_DIR) j++;
        if (j < g_nent) {
            entry_t tmp = g_ent[j];
            for (int k = j; k > i; k--) g_ent[k] = g_ent[k - 1];
            g_ent[i] = tmp;
        } else break;
    }

    char nb[16]; fmt_int(nb, g_nent);
    serial("[FILES] "); serial(path); serial(" -> "); serial(nb); serial(" items\n");
}

/* ----------------------------------------------------------------------- */
/* Navigation                                                              */
/* ----------------------------------------------------------------------- */
NO_SSP static void load_dir(const char *path)
{
    s_cpy(g_cwd, path, PATHLEN);
    do_scan(g_cwd);
    g_sel = -1;
    g_renaming = 0;
    g_preview = 0;
    g_dirty = 1;
}

/* Navigate, pushing the destination onto the history ring (truncating any
 * forward history). */
NO_SSP static void navigate(const char *path)
{
    if (s_eq(path, g_cwd) && g_hist_n > 0) { load_dir(path); return; }
    if (g_hist_n == 0) {
        s_cpy(g_hist[0], path, PATHLEN);
        g_hist_n = 1; g_hist_i = 0;
    } else {
        if (g_hist_i < HIST_MAX - 1) g_hist_i++;
        else { for (int i = 1; i < HIST_MAX; i++) s_cpy(g_hist[i - 1], g_hist[i], PATHLEN); }
        s_cpy(g_hist[g_hist_i], path, PATHLEN);
        g_hist_n = g_hist_i + 1;
    }
    load_dir(path);
}

static void hist_back(void)
{
    if (g_hist_i > 0) { g_hist_i--; load_dir(g_hist[g_hist_i]); }
}
static void hist_forward(void)
{
    if (g_hist_i < g_hist_n - 1) { g_hist_i++; load_dir(g_hist[g_hist_i]); }
}
static void go_up(void)
{
    if (s_eq(g_cwd, "/")) return;
    char p[PATHLEN]; s_cpy(p, g_cwd, PATHLEN); path_parent(p);
    navigate(p);
}

/* ----------------------------------------------------------------------- */
/* Open a file -> populate the preview pane (peek first bytes), or launch  */
/* an .elf via SYS_SPAWN.                                                  */
/* ----------------------------------------------------------------------- */
NO_SSP static void open_file(entry_t *e)
{
    s_zero(g_pathbuf, PATHLEN);
    path_join(g_pathbuf, PATHLEN, g_cwd, e->name);

    /* Executables: spawn them. */
    if (ends_with(e->name, ".elf")) {
        serial("[FILES] spawn "); serial(g_pathbuf); serial("\n");
        sc6(SYS_SPAWN, (long)g_pathbuf, 0, 0, 0, 0, 0);
        return;
    }

    /* Everything else: text/info preview. */
    s_cpy(g_prev_name, e->name, NAMELEN);
    g_prev_size = e->size;
    g_prev_len = 0;
    g_prev_text[0] = '\0';

    long fd = sc(SYS_OPEN, (long)g_pathbuf, O_RDONLY, 0);
    if (fd >= 0) {
        long n = sc(SYS_READ, fd, (long)g_prev_text, (long)(sizeof(g_prev_text) - 1));
        if (n > 0) {
            g_prev_len = (int)n;
            g_prev_text[n] = '\0';
            /* sanitize: keep printable + newline/tab, cap line length later */
            for (int i = 0; i < g_prev_len; i++) {
                unsigned char ch = (unsigned char)g_prev_text[i];
                if (ch == '\t') g_prev_text[i] = ' ';
                else if (ch != '\n' && (ch < 0x20 || ch > 0x7E)) g_prev_text[i] = '.';
            }
        }
        sc(SYS_CLOSE, fd, 0, 0);
    }
    g_preview = 1;
    g_dirty = 1;
}

/* ----------------------------------------------------------------------- */
/* Create a new folder: pick a unique name, SYS_MKDIR, RE-SCAN, select+rename */
/* ----------------------------------------------------------------------- */
NO_SSP static int name_exists(const char *nm)
{
    for (int i = 0; i < g_nent; i++) if (s_eq(g_ent[i].name, nm)) return 1;
    return 0;
}

NO_SSP static void new_folder(void)
{
    char nm[NAMELEN];
    s_cpy(nm, "New folder", NAMELEN);
    if (name_exists(nm)) {
        for (int n = 2; n < 1000; n++) {
            char num[12]; fmt_int(num, n);
            s_cpy(nm, "New folder (", NAMELEN);
            s_cat(nm, num, NAMELEN);
            s_cat(nm, ")", NAMELEN);
            if (!name_exists(nm)) break;
        }
    }

    s_zero(g_pathbuf, PATHLEN);
    path_join(g_pathbuf, PATHLEN, g_cwd, nm);
    long r = sc(SYS_MKDIR, (long)g_pathbuf, 0755, 0);
    if (r != 0) { serial("[FILES] mkdir failed\n"); return; }

    /* CRITICAL FIX: re-scan so the new folder shows immediately. */
    do_scan(g_cwd);
    g_preview = 0;

    /* Select the newly created folder and drop into inline rename. */
    g_sel = -1;
    for (int i = 0; i < g_nent; i++) if (s_eq(g_ent[i].name, nm)) { g_sel = i; break; }
    if (g_sel >= 0) {
        g_renaming = 1;
        s_cpy(g_renbuf, nm, NAMELEN);
        g_renlen = (int)s_len(g_renbuf);
        /* make sure it is scrolled into view */
        int cols = grid_cols();
        int row = g_sel / cols;
        int vis = grid_rows_visible();
        if (row < g_scroll) g_scroll = row;
        if (row >= g_scroll + vis) g_scroll = row - vis + 1;
    }
    g_dirty = 1;
}

/* Commit an inline rename via SYS_RENAME, then re-scan. */
NO_SSP static void commit_rename(void)
{
    if (!g_renaming || g_sel < 0 || g_sel >= g_nent) { g_renaming = 0; return; }
    if (g_renlen == 0 || s_eq(g_renbuf, g_ent[g_sel].name)) { g_renaming = 0; g_dirty = 1; return; }

    char oldp[PATHLEN], newp[PATHLEN];
    s_zero(oldp, PATHLEN); path_join(oldp, PATHLEN, g_cwd, g_ent[g_sel].name);
    s_zero(newp, PATHLEN); path_join(newp, PATHLEN, g_cwd, g_renbuf);
    char savedname[NAMELEN]; s_cpy(savedname, g_renbuf, NAMELEN);

    long r = sc(SYS_RENAME, (long)oldp, (long)newp, 0);
    if (r == 0) {
        do_scan(g_cwd);
        g_sel = -1;
        for (int i = 0; i < g_nent; i++) if (s_eq(g_ent[i].name, savedname)) { g_sel = i; break; }
    }
    g_renaming = 0;
    g_dirty = 1;
}

/* ----------------------------------------------------------------------- */
/* Open / activate an item (double-click or Enter).                        */
/* ----------------------------------------------------------------------- */
NO_SSP static void activate(int idx)
{
    if (idx < 0 || idx >= g_nent) return;
    entry_t *e = &g_ent[idx];
    if (e->type == DT_DIR) {
        char p[PATHLEN];
        s_zero(p, PATHLEN);
        path_join(p, PATHLEN, g_cwd, e->name);
        navigate(p);
    } else {
        open_file(e);
    }
}

/* ----------------------------------------------------------------------- */
/* Sidebar: places + root folders. Built at scan-of-root time; we just     */
/* hard-list a few standard initrd dirs + dynamic? Keep it simple+static.  */
/* ----------------------------------------------------------------------- */
typedef struct { const char *label; const char *path; int header; } side_t;
static side_t g_side[] = {
    { "Quick access", 0, 1 },
    { "  Home",      "/home",    0 },
    { "  Desktop",   "/Desktop", 0 },
    { "  Documents", "/usr",     0 },
    { "This PC",      0, 1 },
    { "  System (/)", "/",      0 },
    { "  bin",       "/bin",    0 },
    { "  sbin",      "/sbin",   0 },
    { "  etc",       "/etc",    0 },
    { "  tmp",       "/tmp",    0 },
};
#define NSIDE ((int)(sizeof(g_side)/sizeof(g_side[0])))
#define SIDE_ROW_H 26
#define SIDE_Y0    (HEADER_TOP + 8)

/* Hit-test the sidebar; returns index or -1. */
static int side_hit(int mx, int my)
{
    if (mx < 0 || mx >= SIDEBAR_W) return -1;
    for (int i = 0; i < NSIDE; i++) {
        int ry = SIDE_Y0 + i * SIDE_ROW_H;
        if (g_side[i].header) continue;
        if (my >= ry && my < ry + SIDE_ROW_H) return i;
    }
    return -1;
}

/* ----------------------------------------------------------------------- */
/* Toolbar hit regions                                                     */
/* ----------------------------------------------------------------------- */
/* Button rectangles (x,y,w,h). */
#define BTN_BACK_X    10
#define BTN_FWD_X     46
#define BTN_UP_X      82
#define NAVBTN_Y      9
#define NAVBTN_W      30
#define NAVBTN_H      30
#define NEWF_X        130
#define NEWF_Y        9
#define NEWF_W        118
#define NEWF_H        30

static int in_rect(int mx, int my, int x, int y, int w, int h)
{ return mx >= x && mx < x + w && my >= y && my < y + h; }

/* ----------------------------------------------------------------------- */
/* Grid hit-test: pixel -> entry index, or -1.                             */
/* ----------------------------------------------------------------------- */
static int grid_hit(int mx, int my)
{
    if (mx < CONTENT_X || my < CONTENT_Y || my >= FBH - STATUS_H) return -1;
    if (g_preview && mx >= FBW - 300) return -1;     /* over preview pane */
    int cols = grid_cols();
    int relx = mx - (CONTENT_X + PAD_X);
    int rely = my - (CONTENT_Y + PAD_Y);
    if (relx < 0 || rely < 0) return -1;
    int col = relx / CELL_W;
    int row = rely / CELL_H;
    if (col >= cols) return -1;
    /* within-cell padding check (don't select the gaps) */
    int cx = relx - col * CELL_W;
    if (cx > CELL_W - 8) return -1;
    int idx = (g_scroll + row) * cols + col;
    if (idx < 0 || idx >= g_nent) return -1;
    return idx;
}

/* ----------------------------------------------------------------------- */
/* RENDER                                                                  */
/* ----------------------------------------------------------------------- */
static int g_hover_idx = -1;     /* hovered grid item                     */
static int g_hover_side = -1;
static int g_hover_btn = -1;     /* 0=back 1=fwd 2=up 3=newfolder         */

NO_SSP static void draw_toolbar(void)
{
    fill(0, 0, FBW, TOOLBAR_H, COL_CHROME);

    /* Nav buttons (back / forward / up) -- chevrons drawn from lines. */
    struct { int x; const char *g; int enabled; int hov; } nav[3] = {
        { BTN_BACK_X, "<", g_hist_i > 0,            g_hover_btn == 0 },
        { BTN_FWD_X,  ">", g_hist_i < g_hist_n - 1, g_hover_btn == 1 },
        { BTN_UP_X,   "^", !s_eq(g_cwd, "/"),       g_hover_btn == 2 },
    };
    for (int i = 0; i < 3; i++) {
        unsigned int face = nav[i].hov && nav[i].enabled ? COL_BTNHOVER : COL_CHROME;
        fill_round(nav[i].x, NAVBTN_Y, NAVBTN_W, NAVBTN_H, 6, face);
        unsigned int fg = nav[i].enabled ? COL_TEXT : 0xFFC0C0C0u;
        text_center(nav[i].x, NAVBTN_Y + 7, NAVBTN_W, nav[i].g, fg);
    }

    /* New folder button. */
    unsigned int nf = g_hover_btn == 3 ? COL_BTNHOVER : COL_BTN;
    fill_round(NEWF_X, NEWF_Y, NEWF_W, NEWF_H, 6, nf);
    round_outline(NEWF_X, NEWF_Y, NEWF_W, NEWF_H, 6, COL_BORDER);
    /* tiny folder glyph */
    fill_round(NEWF_X + 12, NEWF_Y + 11, 16, 11, 2, COL_FOLDER);
    fill(NEWF_X + 12, NEWF_Y + 9, 7, 3, COL_FOLDER2);
    text(NEWF_X + 36, NEWF_Y + 7, "New folder", COL_TEXT);

    /* App title (right). */
    text(FBW - 80, NAVBTN_Y + 7, "Files", COL_TEXTDIM);

    hline(0, TOOLBAR_H - 1, FBW, COL_BORDER);
}

/* Breadcrumb address bar: "> Home / a / b". */
NO_SSP static void draw_address(void)
{
    int y = TOOLBAR_H;
    fill(0, y, FBW, ADDR_H, COL_CHROME);

    int bx = 16, by = y + (ADDR_H - 20) / 2;
    fill_round(bx, by, FBW - 320, 24, 6, COL_CONTENT);
    round_outline(bx, by, FBW - 320, 24, 6, COL_BORDER);

    int tx = bx + 10, ty = by + 4;
    /* leading home chip */
    text(tx, ty, ">", COL_TEXTDIM);
    tx += FONT_W + 6;

    /* split g_cwd on '/' */
    const char *p = g_cwd;
    if (*p == '/') p++;
    char seg[NAMELEN];
    text(tx, ty, "Home", COL_TEXT);
    tx += 4 * FONT_W;
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < NAMELEN - 1) seg[i++] = *p++;
        seg[i] = '\0';
        if (*p == '/') p++;
        if (i == 0) continue;
        text(tx, ty, " / ", COL_TEXTDIM); tx += 3 * FONT_W;
        int segw = i * FONT_W;
        if (tx + segw > bx + (FBW - 320) - 12) { text(tx, ty, "..", COL_TEXTDIM); break; }
        text(tx, ty, seg, COL_TEXT); tx += segw;
    }

    hline(0, y + ADDR_H - 1, FBW, COL_BORDER);
}

NO_SSP static void draw_sidebar(void)
{
    fill(0, HEADER_TOP, SIDEBAR_W, FBH - HEADER_TOP - STATUS_H, COL_SIDEBAR);
    vline(SIDEBAR_W - 1, HEADER_TOP, FBH - HEADER_TOP - STATUS_H, COL_BORDER);

    for (int i = 0; i < NSIDE; i++) {
        int ry = SIDE_Y0 + i * SIDE_ROW_H;
        if (g_side[i].header) {
            text(10, ry + 6, g_side[i].label, COL_TEXTDIM);
            continue;
        }
        int active = g_side[i].path && s_eq(g_side[i].path, g_cwd);
        if (active) { fill_round(6, ry + 1, SIDEBAR_W - 12, SIDE_ROW_H - 2, 6, COL_SELBG); }
        else if (g_hover_side == i) { fill_round(6, ry + 1, SIDEBAR_W - 12, SIDE_ROW_H - 2, 6, COL_HOVER); }
        if (active) vline(8, ry + 5, SIDE_ROW_H - 10, COL_ACCENT);
        /* little folder dot */
        fill_round(16, ry + 8, 12, 9, 2, COL_FOLDER);
        text(34, ry + 5, g_side[i].label + 2, COL_TEXT);   /* skip the 2-space indent */
    }
}

NO_SSP static void draw_grid(void)
{
    int cw = (g_preview ? FBW - 300 : FBW) - CONTENT_X;
    fill(CONTENT_X, CONTENT_Y, cw, content_h_px(), COL_CONTENT);

    if (g_nent == 0) {
        text(CONTENT_X + PAD_X, CONTENT_Y + PAD_Y + 4, "This folder is empty.", COL_TEXTDIM);
    }

    int cols = grid_cols();
    if (g_preview) { int pc = (FBW - 300 - CONTENT_X - PAD_X) / CELL_W; if (pc < 1) pc = 1; cols = pc; }
    int vis = grid_rows_visible();

    for (int row = 0; row < vis; row++) {
        for (int col = 0; col < cols; col++) {
            int idx = (g_scroll + row) * cols + col;
            if (idx < 0 || idx >= g_nent) continue;
            entry_t *e = &g_ent[idx];
            int cx = CONTENT_X + PAD_X + col * CELL_W;
            int cy = CONTENT_Y + PAD_Y + row * CELL_H;

            int selected = (idx == g_sel);
            int hovered  = (idx == g_hover_idx);
            if (selected) {
                fill_round(cx, cy, CELL_W - 8, CELL_H - 6, 8, COL_SELBG);
                round_outline(cx, cy, CELL_W - 8, CELL_H - 6, 8, COL_SELBORDER);
            } else if (hovered) {
                fill_round(cx, cy, CELL_W - 8, CELL_H - 6, 8, COL_HOVER);
            }

            int ix = cx + ((CELL_W - 8) - 48) / 2;
            int iy = cy + ICON_DY;
            if (e->type == DT_DIR) icon_folder(ix, iy);
            else                    icon_file(ix, iy, entry_glyph(e));

            /* label (or inline rename editor) */
            int ly = cy + CELL_H - 30;
            if (selected && g_renaming) {
                int ew = CELL_W - 20;
                fill(cx + 6, ly - 2, ew, 18, COL_CONTENT);
                rect_outline(cx + 6, ly - 2, ew, 18, COL_ACCENT);
                char shown[NAMELEN + 1];
                s_cpy(shown, g_renbuf, NAMELEN);
                int maxc = (ew - 6) / FONT_W;
                int len = (int)s_len(shown);
                int off = (len > maxc) ? len - maxc : 0;
                text_clip(cx + 9, ly, shown + off, ew - 6, COL_TEXT);
                /* caret */
                int cxx = cx + 9 + (len - off) * FONT_W;
                vline(cxx, ly, FONT_H, COL_ACCENT);
            } else {
                /* center label, up to 2 short lines via simple truncation */
                int tw = (int)s_len(e->name) * FONT_W;
                int maxw = CELL_W - 16;
                if (tw <= maxw) {
                    text_center(cx, ly, CELL_W - 8, e->name, selected ? COL_TEXT : COL_TEXT);
                } else {
                    /* two-line wrap by char budget */
                    int per = maxw / FONT_W;
                    char l1[40], l2[40];
                    int n = (int)s_len(e->name);
                    int a = per < (int)sizeof(l1) - 1 ? per : (int)sizeof(l1) - 1;
                    int b = a + per;
                    int bi = 0;
                    for (int i = 0; i < a && i < n; i++) l1[i] = e->name[i];
                    l1[a < n ? a : n] = '\0';
                    for (int i = a; i < b && i < n && bi < (int)sizeof(l2) - 3; i++) l2[bi++] = e->name[i];
                    if (b < n) { l2[bi++] = '.'; l2[bi++] = '.'; }
                    l2[bi] = '\0';
                    text_center(cx, ly - 8, CELL_W - 8, l1, COL_TEXT);
                    text_center(cx, ly + 8, CELL_W - 8, l2, COL_TEXT);
                }
            }
        }
    }

    /* scrollbar (if content overflows) */
    int total_rows = (g_nent + cols - 1) / cols;
    if (total_rows > vis) {
        int trackx = (g_preview ? FBW - 300 : FBW) - 8;
        int tracky = CONTENT_Y + 4;
        int trackh = content_h_px() - 8;
        fill(trackx, tracky, 4, trackh, 0xFFE8E8E8u);
        int thumb_h = trackh * vis / total_rows;
        if (thumb_h < 24) thumb_h = 24;
        int max_scroll = total_rows - vis;
        int thumb_y = tracky + (max_scroll > 0 ? (trackh - thumb_h) * g_scroll / max_scroll : 0);
        fill_round(trackx, thumb_y, 4, thumb_h, 2, 0xFFBdBdBdu);
    }
}

NO_SSP static void draw_preview(void)
{
    if (!g_preview) return;
    int px0 = FBW - 300;
    fill(px0, CONTENT_Y, 300, content_h_px(), COL_SIDEBAR);
    vline(px0, CONTENT_Y, content_h_px(), COL_BORDER);

    int x = px0 + 18, y = CONTENT_Y + 16;
    /* big file icon */
    icon_file(x + 90, y, '-');
    y += 56;
    text_center(px0, y, 300, g_prev_name, COL_TEXT); y += 24;
    char sb[40]; fmt_size(sb, g_prev_size);
    text_center(px0, y, 300, sb, COL_TEXTDIM); y += 24;
    hline(px0 + 16, y, 268, COL_BORDER); y += 12;

    text(x, y, "Preview:", COL_TEXTDIM); y += 20;

    /* render first lines of the file, wrapped to the pane width */
    int maxc = (300 - 36) / FONT_W;
    int i = 0, lines = 0;
    char line[64];
    int li = 0;
    int maxlines = (CONTENT_Y + content_h_px() - y) / FONT_H - 1;
    while (i < g_prev_len && lines < maxlines) {
        char ch = g_prev_text[i++];
        if (ch == '\n' || li >= maxc || li >= (int)sizeof(line) - 1) {
            line[li] = '\0';
            text(x, y, line, COL_TEXT);
            y += FONT_H; lines++; li = 0;
            if (ch != '\n' && ch != '\0') { line[li++] = ch; }
            continue;
        }
        line[li++] = ch;
    }
    if (li > 0 && lines < maxlines) { line[li] = '\0'; text(x, y, line, COL_TEXT); }
}

NO_SSP static void draw_status(void)
{
    int y = FBH - STATUS_H;
    fill(0, y, FBW, STATUS_H, COL_CHROME);
    hline(0, y, FBW, COL_BORDER);

    char st[120]; char nb[16];
    fmt_int(nb, g_nent);
    s_cpy(st, nb, sizeof(st));
    s_cat(st, " items", sizeof(st));
    if (g_sel >= 0 && g_sel < g_nent) {
        entry_t *e = &g_ent[g_sel];
        s_cat(st, "    |    ", sizeof(st));
        s_cat(st, e->name, sizeof(st));
        if (e->type == DT_DIR) s_cat(st, "  (folder)", sizeof(st));
        else { char szb[24]; fmt_size(szb, e->size); s_cat(st, "  ", sizeof(st)); s_cat(st, szb, sizeof(st)); }
    }
    text(12, y + 6, st, COL_TEXTDIM);
}

NO_SSP static void render(void)
{
    fill(0, 0, FBW, FBH, COL_CONTENT);
    draw_grid();
    draw_preview();
    draw_sidebar();
    draw_toolbar();
    draw_address();
    draw_status();
    wl_commit(g_win);
}

/* ----------------------------------------------------------------------- */
/* Keyboard map (US, lowercase by default; shift handled below)            */
/* ----------------------------------------------------------------------- */
#define KEY_ESC        1
#define KEY_BACKSPACE 14
#define KEY_ENTER     28
#define KEY_LSHIFT    42
#define KEY_RSHIFT    54
#define KEY_F2        60
#define KEY_SPACE     57
#define KEY_DOT       52
#define KEY_MINUS     12

static char key_ascii(int kc, int shift)
{
    switch (kc) {
        case 2: return shift ? '!' : '1'; case 3: return shift ? '@' : '2';
        case 4: return shift ? '#' : '3'; case 5: return shift ? '$' : '4';
        case 6: return shift ? '%' : '5'; case 7: return shift ? '^' : '6';
        case 8: return shift ? '&' : '7'; case 9: return shift ? '*' : '8';
        case 10: return shift ? '(' : '9'; case 11: return shift ? ')' : '0';
        case 12: return shift ? '_' : '-'; case 13: return shift ? '+' : '=';
        case 16: return shift ? 'Q':'q'; case 17: return shift ? 'W':'w';
        case 18: return shift ? 'E':'e'; case 19: return shift ? 'R':'r';
        case 20: return shift ? 'T':'t'; case 21: return shift ? 'Y':'y';
        case 22: return shift ? 'U':'u'; case 23: return shift ? 'I':'i';
        case 24: return shift ? 'O':'o'; case 25: return shift ? 'P':'p';
        case 30: return shift ? 'A':'a'; case 31: return shift ? 'S':'s';
        case 32: return shift ? 'D':'d'; case 33: return shift ? 'F':'f';
        case 34: return shift ? 'G':'g'; case 35: return shift ? 'H':'h';
        case 36: return shift ? 'J':'j'; case 37: return shift ? 'K':'k';
        case 38: return shift ? 'L':'l'; case 44: return shift ? 'Z':'z';
        case 45: return shift ? 'X':'x'; case 46: return shift ? 'C':'c';
        case 47: return shift ? 'V':'v'; case 48: return shift ? 'B':'b';
        case 49: return shift ? 'N':'n'; case 50: return shift ? 'M':'m';
        case KEY_DOT: return shift ? '>' : '.';
        case KEY_SPACE: return ' ';
        default: return 0;
    }
}

/* ----------------------------------------------------------------------- */
/* Click handling                                                          */
/* ----------------------------------------------------------------------- */
static void ensure_visible(int idx)
{
    int cols = grid_cols();
    int row = idx / cols;
    int vis = grid_rows_visible();
    if (row < g_scroll) g_scroll = row;
    if (row >= g_scroll + vis) g_scroll = row - vis + 1;
}

NO_SSP static void on_click(int mx, int my)
{
    /* If renaming, a click elsewhere commits the rename first. */
    int grid_idx = grid_hit(mx, my);

    /* Toolbar buttons. */
    if (my < TOOLBAR_H) {
        if (g_renaming) commit_rename();
        if (in_rect(mx, my, BTN_BACK_X, NAVBTN_Y, NAVBTN_W, NAVBTN_H)) { hist_back(); return; }
        if (in_rect(mx, my, BTN_FWD_X, NAVBTN_Y, NAVBTN_W, NAVBTN_H)) { hist_forward(); return; }
        if (in_rect(mx, my, BTN_UP_X, NAVBTN_Y, NAVBTN_W, NAVBTN_H))  { go_up(); return; }
        if (in_rect(mx, my, NEWF_X, NEWF_Y, NEWF_W, NEWF_H))          { new_folder(); return; }
        return;
    }

    /* Sidebar. */
    int s = side_hit(mx, my);
    if (s >= 0 && g_side[s].path) {
        if (g_renaming) commit_rename();
        navigate(g_side[s].path);
        return;
    }

    /* Grid item. */
    if (grid_idx >= 0) {
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        int dbl = (grid_idx == g_last_click_idx) && (now - g_last_click_ms <= 400);

        if (g_renaming && grid_idx != g_sel) commit_rename();

        if (dbl) {
            /* double-click: open / navigate */
            g_last_click_idx = -2;     /* reset so a 3rd click isn't a dbl */
            activate(grid_idx);
            return;
        }

        /* single click: select. If it was already selected (and not just
         * created), a slow second click enters rename (Win11/Explorer feel). */
        if (grid_idx == g_sel && !g_renaming) {
            g_renaming = 1;
            s_cpy(g_renbuf, g_ent[grid_idx].name, NAMELEN);
            g_renlen = (int)s_len(g_renbuf);
        } else {
            g_sel = grid_idx;
        }
        g_last_click_idx = grid_idx;
        g_last_click_ms = now;
        g_dirty = 1;
        return;
    }

    /* Click in empty content area: deselect + commit any rename. */
    if (g_renaming) commit_rename();
    if (mx >= CONTENT_X && my >= CONTENT_Y && my < FBH - STATUS_H) {
        g_sel = -1; g_last_click_idx = -2; g_dirty = 1;
    }
}

NO_SSP static void on_key(int kc, int pressed, int *shift)
{
    if (kc == KEY_LSHIFT || kc == KEY_RSHIFT) { *shift = pressed; return; }
    if (!pressed) return;

    if (g_renaming) {
        if (kc == KEY_ENTER) { commit_rename(); return; }
        if (kc == KEY_ESC)   { g_renaming = 0; g_dirty = 1; return; }
        if (kc == KEY_BACKSPACE) { if (g_renlen > 0) g_renbuf[--g_renlen] = '\0'; g_dirty = 1; return; }
        char ch = key_ascii(kc, *shift);
        if (ch && g_renlen < NAMELEN - 1 && ch != '/') { g_renbuf[g_renlen++] = ch; g_renbuf[g_renlen] = '\0'; g_dirty = 1; }
        return;
    }

    /* Navigation / shortcuts. */
    switch (kc) {
        case KEY_BACKSPACE: go_up(); break;
        case KEY_ENTER: if (g_sel >= 0) activate(g_sel); break;
        case KEY_ESC: if (g_preview) { g_preview = 0; g_dirty = 1; } break;
        case KEY_F2:
            if (g_sel >= 0) { g_renaming = 1; s_cpy(g_renbuf, g_ent[g_sel].name, NAMELEN); g_renlen = (int)s_len(g_renbuf); g_dirty = 1; }
            break;
        case 105: /* left arrow */ if (g_sel > 0) { g_sel--; ensure_visible(g_sel); g_dirty = 1; } break;
        case 106: /* right arrow */ if (g_sel < g_nent - 1) { g_sel++; ensure_visible(g_sel); g_dirty = 1; } break;
        case 103: /* up arrow */ { int c = grid_cols(); if (g_sel - c >= 0) { g_sel -= c; ensure_visible(g_sel); g_dirty = 1; } } break;
        case 108: /* down arrow */ { int c = grid_cols(); if (g_sel + c < g_nent) { g_sel += c; ensure_visible(g_sel); g_dirty = 1; } } break;
        default: break;
    }
}

/* ----------------------------------------------------------------------- */
/* Entry point                                                             */
/* ----------------------------------------------------------------------- */
NO_SSP void _start(void)
{
    serial("[FILES] Windows 11 explorer starting\n");

    if (wl_connect() != 0) { serial("[FILES] wl_connect failed\n"); for (;;) sc(SYS_YIELD, 0, 0, 0); }
    g_win = wl_create_window(WIN_W, WIN_H, "Files");
    if (!g_win) { serial("[FILES] create_window failed\n"); for (;;) sc(SYS_YIELD, 0, 0, 0); }

    FB  = g_win->pixels;
    FBW = (int)g_win->w;
    FBH = (int)g_win->h;
    FBS = (int)(g_win->stride / 4);

    s_cpy(g_cwd, "/", PATHLEN);
    navigate("/");

    int prev_btn = 0;
    int shift = 0;
    long last_frame = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    int cur_x = 0, cur_y = 0;

    for (;;) {
        int kind, a, b, c;
        while (wl_poll_event(g_win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_POINTER) {
                cur_x = a; cur_y = b;
                int btn = (c & 1) ? 1 : 0;

                /* update hover state */
                int hi = grid_hit(cur_x, cur_y);
                int hs = side_hit(cur_x, cur_y);
                int hb = -1;
                if (cur_y < TOOLBAR_H) {
                    if (in_rect(cur_x, cur_y, BTN_BACK_X, NAVBTN_Y, NAVBTN_W, NAVBTN_H)) hb = 0;
                    else if (in_rect(cur_x, cur_y, BTN_FWD_X, NAVBTN_Y, NAVBTN_W, NAVBTN_H)) hb = 1;
                    else if (in_rect(cur_x, cur_y, BTN_UP_X, NAVBTN_Y, NAVBTN_W, NAVBTN_H)) hb = 2;
                    else if (in_rect(cur_x, cur_y, NEWF_X, NEWF_Y, NEWF_W, NEWF_H)) hb = 3;
                }
                if (hi != g_hover_idx || hs != g_hover_side || hb != g_hover_btn) {
                    g_hover_idx = hi; g_hover_side = hs; g_hover_btn = hb; g_dirty = 1;
                }

                if (btn && !prev_btn) on_click(cur_x, cur_y);   /* press edge */
                prev_btn = btn;
            } else if (kind == WL_EVENT_KEY) {
                on_key(a, b, &shift);
            }
        }

        if (g_dirty) { g_dirty = 0; render(); }

        /* pace ~30fps */
        sc(SYS_YIELD, 0, 0, 0);
        for (;;) {
            long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);
            if (now - last_frame >= 33) { last_frame = now; break; }
            sc(SYS_YIELD, 0, 0, 0);
        }
    }
}
