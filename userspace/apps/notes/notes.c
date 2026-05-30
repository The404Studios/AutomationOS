/*
 * notes.c -- Notes app (freestanding, ring 3).
 * =============================================
 *
 * 640x440 window.  Left sidebar (~180px) lists saved note files found under
 * /tmp/notes/.  Right panel is a multi-line text editor for the current note.
 * Status bar at the bottom shows current note name + Ctrl-S / F2 / N hints.
 *
 * Syscalls used:
 *   SYS_READ      = 2   (read file content)
 *   SYS_WRITE     = 3   (serial print + file write)
 *   SYS_OPEN      = 4   (open/create files;  O_RDONLY=0, O_WRONLY=1,
 *                          O_CREAT=0x40, O_TRUNC=0x200)
 *   SYS_CLOSE     = 5
 *   SYS_YIELD     = 15
 *   SYS_OPENDIR   = 30
 *   SYS_READDIR   = 31
 *   SYS_CLOSEDIR  = 32
 *   SYS_GET_TICKS_MS = 40
 *
 * Controls:
 *   Typing     -- insert printable character
 *   Backspace  -- delete char before cursor
 *   Enter      -- insert newline
 *   Arrow keys -- move cursor
 *   F2         -- save current note
 *   F5         -- new note  (KEY_F5 = 63)
 *   Click sidebar entry -- load that note
 *   Click [New] button  -- new note
 *   Click [Save] button -- save note
 *
 * Build (exact flags, never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/notes/notes.c -o /tmp/notes.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/notes.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/notes.elf
 *   objdump -d /tmp/notes.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* ---- syscall numbers ---- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_YIELD         15
#define SYS_OPENDIR       30
#define SYS_READDIR       31
#define SYS_CLOSEDIR      32
#define SYS_GET_TICKS_MS  40

/* ---- open() flags (Linux x86-64 values) ---- */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0x40    /* 0100 octal */
#define O_TRUNC     0x200   /* 0400 octal */

/* ---- types ---- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;
typedef long           i64;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---- inline syscall ---- */
static inline i64 sc(i64 n, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6)
{
    i64 r;
    register i64 r10 asm("r10") = a4;
    register i64 r8  asm("r8")  = a5;
    register i64 r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- serial diagnostics ---- */
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (i64)m, (i64)k_strlen(m), 0, 0, 0);
}
static void serial_num(i64 n)
{
    char b[24]; int i = 0;
    if (n < 0) { serial_print("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (i64)&ch, 1, 0, 0, 0); }
}

/* ---- string helpers ---- */
static void k_memset(void *dst, u8 v, u64 n)
{
    u8 *d = (u8*)dst;
    for (u64 i = 0; i < n; i++) d[i] = v;
}

static void k_memmove(void *dst, const void *src, u64 n)
{
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    if (d < s) {
        for (u64 i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (u64 i = n; i > 0; i--) d[i-1] = s[i-1];
    }
}

static void k_strlcpy(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void k_strlcat(char *dst, const char *src, int n)
{
    int len = 0;
    while (dst[len]) len++;
    int i = 0;
    while (len + i < n - 1 && src[i]) { dst[len + i] = src[i]; i++; }
    dst[len + i] = '\0';
}

static int k_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void int_to_str(i64 n, char *buf)
{
    char tmp[24]; int i = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = 0; return; }
    if (n < 0) { *buf++ = '-'; n = -n; }
    do { tmp[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

/* ---- dirent layout (matches kernel/include/vfs.h) ---- */
#define DT_REG 8
#define DT_DIR 4
struct dirent {
    u64  d_ino;
    i64  d_off;
    u16  d_reclen;
    u8   d_type;
    char d_name[256];
};

/* ---- drawing helpers ---- */
static void fill_rect(u32 *pixels, u32 stride_px, u32 bw, u32 bh,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = pixels + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/* Draw a single-pixel border rect (no fill) */
static void draw_border(u32 *pixels, u32 stride_px, u32 bw, u32 bh,
                        i32 x, i32 y, i32 w, i32 h, u32 color)
{
    fill_rect(pixels, stride_px, bw, bh, x,     y,     w, 1, color);  /* top    */
    fill_rect(pixels, stride_px, bw, bh, x,     y+h-1, w, 1, color);  /* bottom */
    fill_rect(pixels, stride_px, bw, bh, x,     y,     1, h, color);  /* left   */
    fill_rect(pixels, stride_px, bw, bh, x+w-1, y,     1, h, color);  /* right  */
}

/* ---- keycodes ---- */
#define KEY_ESC         1
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_LEFTSHIFT   42
#define KEY_RIGHTSHIFT  54
#define KEY_SPACE       57
#define KEY_F2          60
#define KEY_F5          63
#define KEY_UP          103
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_DOWN        108

/* US-layout keycode -> ASCII (no shift) */
static const char keymap_low[256] = {
    /* 0  */ 0,   0,   '1', '2', '3', '4', '5', '6',   /* 0-7   */
    /* 8  */ '7', '8', '9', '0', '-', '=', 0,   '\t',  /* 8-15  */
    /* 16 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   /* 16-23 */
    /* 24 */ 'o', 'p', '[', ']', 0,   0,   'a', 's',   /* 24-31 */
    /* 32 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   /* 32-39 */
    /* 40 */ '\'','`', 0,  '\\','z', 'x', 'c', 'v',    /* 40-47 */
    /* 48 */ 'b', 'n', 'm', ',', '.', '/', 0,   '*',   /* 48-55 */
    /* 56 */ 0,   ' ', 0,                               /* 56-58 */
};

/* US-layout keycode -> ASCII (with shift) */
static const char keymap_hi[256] = {
    /* 0  */ 0,   0,   '!', '@', '#', '$', '%', '^',
    /* 8  */ '&', '*', '(', ')', '_', '+', 0,   '\t',
    /* 16 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 24 */ 'O', 'P', '{', '}', 0,   0,   'A', 'S',
    /* 32 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 40 */ '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    /* 48 */ 'B', 'N', 'M', '<', '>', '?', 0,   '*',
    /* 56 */ 0,   ' ', 0,
};

/* ---- window geometry ---- */
#define WIN_W       640
#define WIN_H       440

#define SIDEBAR_W   180
#define DIVIDER_W   2

#define EDITOR_X    (SIDEBAR_W + DIVIDER_W)
#define EDITOR_W    (WIN_W - EDITOR_X)

#define TITLEBAR_H  32
#define STATUS_H    20
#define EDITOR_H    (WIN_H - TITLEBAR_H - STATUS_H)

/* Editor text area (relative to window top-left) */
#define EDIT_LEFT   (EDITOR_X + 4)
#define EDIT_TOP    (TITLEBAR_H + 2)
#define EDIT_RIGHT  (WIN_W - 4)
#define EDIT_BOTTOM (WIN_H - STATUS_H)

/* Visible text columns/rows in the editor */
#define EDIT_COLS   ((EDIT_RIGHT - EDIT_LEFT) / FONT_W)
#define EDIT_ROWS   ((EDIT_BOTTOM - EDIT_TOP) / FONT_H)

/* Sidebar list top */
#define SIDEBAR_LIST_TOP  (TITLEBAR_H + 4)
/* Height for each sidebar row */
#define SIDEBAR_ROW_H  20
/* Max visible sidebar entries */
#define SIDEBAR_VISIBLE  ((WIN_H - TITLEBAR_H - STATUS_H - 4) / SIDEBAR_ROW_H)

/* ---- colors ---- */
#define COL_BG           0xFF1A1A2Eu  /* deep navy */
#define COL_SIDEBAR_BG   0xFF16213Eu  /* slightly lighter navy */
#define COL_SIDEBAR_SEL  0xFF0F3460u  /* selected item */
#define COL_SIDEBAR_HOV  0xFF1B3A5Cu  /* hovered item */
#define COL_EDITOR_BG    0xFF1E1E2Eu  /* editor background */
#define COL_TITLEBAR_BG  0xFF0D1B2Au  /* title bar */
#define COL_STATUS_BG    0xFF0D1B2Au  /* status bar */
#define COL_DIVIDER      0xFF3A3A5Cu  /* sidebar/editor divider */
#define COL_TEXT         0xFFCDD6F4u  /* main text (catppuccin blue) */
#define COL_TEXT_DIM     0xFF6C7086u  /* dimmed text */
#define COL_CURSOR       0xFF89B4FAu  /* cursor highlight */
#define COL_CURSOR_TXT   0xFF1E1E2Eu  /* text under cursor */
#define COL_HEADER_TXT   0xFFA6E3A1u  /* green header accent */
#define COL_BUTTON_BG    0xFF313244u  /* button background */
#define COL_BUTTON_TXT   0xFFCDD6F4u  /* button text */
#define COL_BUTTON_HOV   0xFF45475Au  /* button hover */
#define COL_DIRTY        0xFFEBA0ACu  /* red-pink unsaved indicator */
#define COL_SELECTED_TXT 0xFFF38BA8u  /* selected note name (pink) */
#define COL_BORDER       0xFF45475Au  /* panel borders */
#define COL_NEW_BG       0xFF40A02Bu  /* green "New" button */
#define COL_SAVE_BG      0xFF1E66F5u  /* blue "Save" button */
#define COL_SAVE_HOV     0xFF2577FFu  /* save hover */

/* ---- notes storage ---- */
#define NOTES_DIR        "/tmp/notes"
#define MAX_NOTES        32
#define NOTE_NAME_LEN    64
#define NOTE_PATH_LEN    128

/* Each note entry in the sidebar */
typedef struct {
    char name[NOTE_NAME_LEN];   /* filename only (e.g. "note1.txt") */
    char path[NOTE_PATH_LEN];   /* full path (e.g. "/tmp/notes/note1.txt") */
} note_entry_t;

/* ---- editor state ---- */
#define MAX_LINES    512
#define MAX_LINE_LEN 256

/* All static so they live in .bss (zero-initialized). */
static note_entry_t g_notes[MAX_NOTES];
static int          g_note_count;
static int          g_selected_note;   /* -1 = none */
static int          g_sidebar_scroll;

/* Text editor buffer */
static char g_lines[MAX_LINES][MAX_LINE_LEN];
static int  g_line_len[MAX_LINES];
static int  g_num_lines;
static int  g_cur_row;
static int  g_cur_col;
static int  g_scroll_row;
static int  g_dirty;

/* Current open file name (just the filename) */
static char g_cur_name[NOTE_NAME_LEN];

/* Next auto-generated note number */
static int  g_next_note_num;

/* Modifier state */
static int g_shift;
static int g_ctrl;

/* Mouse state */
static int g_mouse_x;
static int g_mouse_y;
static int g_mouse_btn;   /* non-zero if pressed */

/* ---- path string buffer (kernel copy_from_user safety) ---- */
/*
 * All paths passed to kernel syscalls are routed through this zeroed
 * 256-byte buffer to avoid faulting on short strings near a page end.
 */
static char g_path_buf[256];

static void safe_path(const char *src)
{
    k_memset(g_path_buf, 0, sizeof(g_path_buf));
    k_strlcpy(g_path_buf, src, sizeof(g_path_buf));
}

/* ---- editor helpers ---- */
static void editor_clear(void)
{
    k_memset(g_lines,    0, sizeof(g_lines));
    k_memset(g_line_len, 0, sizeof(g_line_len));
    g_num_lines  = 1;
    g_cur_row    = 0;
    g_cur_col    = 0;
    g_scroll_row = 0;
    g_dirty      = 0;
}

static void ensure_visible(void)
{
    if (g_cur_row < g_scroll_row)
        g_scroll_row = g_cur_row;
    else if (g_cur_row >= g_scroll_row + EDIT_ROWS)
        g_scroll_row = g_cur_row - EDIT_ROWS + 1;
}

static void insert_char(char c)
{
    int row = g_cur_row;
    int col = g_cur_col;
    int len = g_line_len[row];
    if (col > len) col = len;
    if (len >= MAX_LINE_LEN - 1) return;

    k_memmove(&g_lines[row][col + 1], &g_lines[row][col], (u64)(len - col));
    g_lines[row][col] = c;
    g_line_len[row]++;
    g_lines[row][g_line_len[row]] = 0;
    g_cur_col = col + 1;
    g_dirty = 1;
}

static void do_enter(void)
{
    if (g_num_lines >= MAX_LINES) return;
    int row = g_cur_row;
    int col = g_cur_col;
    int len = g_line_len[row];
    if (col > len) col = len;

    /* Shift lines down */
    for (int i = g_num_lines - 1; i > row; i--) {
        k_memmove(g_lines[i + 1], g_lines[i], (u64)(g_line_len[i] + 1));
        g_line_len[i + 1] = g_line_len[i];
    }
    g_num_lines++;

    int tail = len - col;
    k_memmove(g_lines[row + 1], &g_lines[row][col], (u64)tail);
    g_line_len[row + 1] = tail;
    g_lines[row + 1][tail] = 0;

    g_line_len[row] = col;
    g_lines[row][col] = 0;

    g_cur_row = row + 1;
    g_cur_col = 0;
    g_dirty = 1;
}

static void do_backspace(void)
{
    int row = g_cur_row;
    int col = g_cur_col;

    if (col > 0) {
        int len = g_line_len[row];
        k_memmove(&g_lines[row][col - 1], &g_lines[row][col], (u64)(len - col + 1));
        g_line_len[row]--;
        g_cur_col = col - 1;
    } else if (row > 0) {
        int prev = row - 1;
        int prev_len = g_line_len[prev];
        int cur_len  = g_line_len[row];

        if (prev_len + cur_len < MAX_LINE_LEN - 1) {
            k_memmove(&g_lines[prev][prev_len], g_lines[row], (u64)(cur_len + 1));
            g_line_len[prev] = prev_len + cur_len;
            for (int i = row; i < g_num_lines - 1; i++) {
                k_memmove(g_lines[i], g_lines[i + 1], (u64)(g_line_len[i + 1] + 1));
                g_line_len[i] = g_line_len[i + 1];
            }
            g_num_lines--;
            g_cur_row = prev;
            g_cur_col = prev_len;
        }
    }
    g_dirty = 1;
}

/* ---- file I/O ---- */

/* mkdir-like: open the dir; if it fails we can't help */
static void ensure_notes_dir(void)
{
    /* Try to opendir; if it fails we need to create it.
     * The ramfs mkdir is done via SYS_OPEN with O_CREAT on a directory name
     * on some kernels, but simplest: just try opendir and ignore failures.
     * The actual notes files are stored under /tmp/notes/.
     * We'll try opening the directory; if it works, great.  If not, we
     * fall back to using /tmp directly. */
    safe_path(NOTES_DIR);
    i64 dfd = sc(SYS_OPENDIR, (i64)g_path_buf, 0, 0, 0, 0, 0);
    if (dfd >= 0) {
        sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);
    }
    /* We won't fail hard -- scan_notes handles missing dir gracefully. */
}

/* Scan /tmp/notes/ for *.txt files and populate g_notes[]. */
static void scan_notes(void)
{
    g_note_count = 0;

    safe_path(NOTES_DIR);
    i64 dfd = sc(SYS_OPENDIR, (i64)g_path_buf, 0, 0, 0, 0, 0);
    if (dfd < 0) {
        /* Directory doesn't exist yet; no notes. */
        serial_print("[NOTES] /tmp/notes not found, starting empty\n");
        return;
    }

    struct dirent ent;
    int max_num = 0;

    while (g_note_count < MAX_NOTES) {
        i64 r = sc(SYS_READDIR, dfd, (i64)&ent, 0, 0, 0, 0);
        if (r != 0) break;

        /* Skip . and .. */
        if (ent.d_name[0] == '.' &&
            (ent.d_name[1] == '\0' ||
             (ent.d_name[1] == '.' && ent.d_name[2] == '\0')))
            continue;

        /* Only list .txt files */
        int nlen = (int)k_strlen(ent.d_name);
        if (nlen < 4) continue;
        if (ent.d_name[nlen-4] != '.' ||
            ent.d_name[nlen-3] != 't' ||
            ent.d_name[nlen-2] != 'x' ||
            ent.d_name[nlen-1] != 't') continue;

        note_entry_t *ne = &g_notes[g_note_count];
        k_strlcpy(ne->name, ent.d_name, NOTE_NAME_LEN);

        /* Build full path */
        k_strlcpy(ne->path, NOTES_DIR "/", NOTE_PATH_LEN);
        k_strlcat(ne->path, ent.d_name, NOTE_PATH_LEN);

        /* Track highest note number for auto-numbering.
         * Format: "note<N>.txt"  */
        if (ent.d_name[0] == 'n' && ent.d_name[1] == 'o' &&
            ent.d_name[2] == 't' && ent.d_name[3] == 'e') {
            int num = 0;
            for (int i = 4; ent.d_name[i] >= '0' && ent.d_name[i] <= '9'; i++) {
                num = num * 10 + (ent.d_name[i] - '0');
            }
            if (num > max_num) max_num = num;
        }

        g_note_count++;
    }

    sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);

    g_next_note_num = max_num + 1;

    serial_print("[NOTES] found ");
    serial_num((i64)g_note_count);
    serial_print(" note(s) in /tmp/notes\n");
}

/* Load the note at g_notes[idx] into the editor buffer. */
static void load_note(int idx)
{
    if (idx < 0 || idx >= g_note_count) return;

    editor_clear();
    g_selected_note = idx;
    k_strlcpy(g_cur_name, g_notes[idx].name, NOTE_NAME_LEN);

    safe_path(g_notes[idx].path);
    i64 fd = sc(SYS_OPEN, (i64)g_path_buf, O_RDONLY, 0644, 0, 0, 0);
    if (fd < 0) {
        serial_print("[NOTES] load: open failed: ");
        serial_print(g_notes[idx].path);
        serial_print("\n");
        return;
    }

    /* Read file content into a temporary flat buffer, then split into lines. */
    static char load_buf[MAX_LINES * MAX_LINE_LEN];
    k_memset(load_buf, 0, sizeof(load_buf));

    i64 total = 0;
    i64 chunk;
    while (total < (i64)(sizeof(load_buf) - 1)) {
        chunk = sc(SYS_READ, fd, (i64)(load_buf + total),
                   (i64)(sizeof(load_buf) - 1 - (u64)total), 0, 0, 0);
        if (chunk <= 0) break;
        total += chunk;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    load_buf[total] = '\0';

    /* Parse flat buffer into lines array. */
    g_num_lines = 0;
    int ci = 0;
    while (ci <= (int)total && g_num_lines < MAX_LINES) {
        /* find end of line */
        int start = ci;
        while (ci < (int)total && load_buf[ci] != '\n') ci++;
        int end = ci;
        if (ci < (int)total) ci++; /* skip '\n' */

        int len = end - start;
        if (len > MAX_LINE_LEN - 1) len = MAX_LINE_LEN - 1;
        k_memmove(g_lines[g_num_lines], load_buf + start, (u64)len);
        g_lines[g_num_lines][len] = 0;
        g_line_len[g_num_lines] = len;
        g_num_lines++;

        if (ci >= (int)total) break;
    }
    if (g_num_lines == 0) g_num_lines = 1;

    g_dirty = 0;
    g_cur_row = 0;
    g_cur_col = 0;
    g_scroll_row = 0;

    serial_print("[NOTES] loaded ");
    serial_print(g_notes[idx].path);
    serial_print("\n");
}

/* Save current editor buffer to /tmp/notes/<name>. */
static void save_note(void)
{
    /* If no name, generate one */
    if (g_cur_name[0] == '\0') {
        char numstr[16];
        int_to_str((i64)g_next_note_num, numstr);
        k_strlcpy(g_cur_name, "note", NOTE_NAME_LEN);
        k_strlcat(g_cur_name, numstr, NOTE_NAME_LEN);
        k_strlcat(g_cur_name, ".txt", NOTE_NAME_LEN);
        g_next_note_num++;
    }

    /* Build full path */
    char fpath[NOTE_PATH_LEN];
    k_strlcpy(fpath, NOTES_DIR "/", NOTE_PATH_LEN);
    k_strlcat(fpath, g_cur_name, NOTE_PATH_LEN);

    safe_path(fpath);
    i64 fd = sc(SYS_OPEN, (i64)g_path_buf,
                O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) {
        serial_print("[NOTES] save: open failed: ");
        serial_print(fpath);
        serial_print("\n");
        return;
    }

    for (int i = 0; i < g_num_lines; i++) {
        if (g_line_len[i] > 0)
            sc(SYS_WRITE, fd, (i64)g_lines[i], (i64)g_line_len[i], 0, 0, 0);
        sc(SYS_WRITE, fd, (i64)"\n", 1, 0, 0, 0);
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    g_dirty = 0;

    serial_print("[NOTES] saved ");
    serial_print(fpath);
    serial_print("\n");

    /* Refresh sidebar (rescan) */
    scan_notes();

    /* Update selected index to point at the just-saved note. */
    g_selected_note = -1;
    for (int i = 0; i < g_note_count; i++) {
        if (k_strcmp(g_notes[i].name, g_cur_name) == 0) {
            g_selected_note = i;
            break;
        }
    }
}

/* Start a new, empty note. */
static void new_note(void)
{
    editor_clear();
    g_cur_name[0] = '\0';
    g_selected_note = -1;
    g_dirty = 0;
    serial_print("[NOTES] new note\n");
}

/* ---- hit-test helpers ---- */
static int pt_in_rect(int px, int py, int rx, int ry, int rw, int rh)
{
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

/* ---- rendering ---- */

/* Draw a button with label; returns 1 if mouse is hovering. */
static int draw_button(u32 *pix, u32 spx, u32 bw, u32 bh,
                       i32 bx, i32 by, i32 bw2, i32 bh2,
                       const char *label, u32 bg_col, u32 hover_col, u32 txt_col)
{
    int hover = pt_in_rect(g_mouse_x, g_mouse_y, bx, by, bw2, bh2);
    u32 bg = hover ? hover_col : bg_col;
    fill_rect(pix, spx, bw, bh, bx, by, bw2, bh2, bg);
    draw_border(pix, spx, bw, bh, bx, by, bw2, bh2, COL_BORDER);

    /* Center text */
    int tw = (int)k_strlen(label) * FONT_W;
    int tx = bx + (bw2 - tw) / 2;
    int ty = by + (bh2 - FONT_H) / 2;
    font_draw_string(pix, (int)spx, (int)bw, (int)bh, tx, ty, label, txt_col);
    return hover;
}

static void render(wl_window *win, u64 ticks)
{
    u32 spx = win->stride / 4u;
    u32 bw  = win->w;
    u32 bh  = win->h;
    u32 *pix = win->pixels;

    /* ---------- Background ---------- */
    fill_rect(pix, spx, bw, bh, 0, 0, WIN_W, WIN_H, COL_BG);

    /* ---------- Title bar ---------- */
    fill_rect(pix, spx, bw, bh, 0, 0, WIN_W, TITLEBAR_H, COL_TITLEBAR_BG);
    draw_border(pix, spx, bw, bh, 0, 0, WIN_W, TITLEBAR_H, COL_DIVIDER);

    /* Title text */
    font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                     8, (TITLEBAR_H - FONT_H) / 2,
                     "Notes", COL_HEADER_TXT);

    /* [New] button in title bar */
    draw_button(pix, spx, bw, bh,
                WIN_W - 130, (TITLEBAR_H - 22) / 2, 56, 22,
                "New", COL_NEW_BG, 0xFF50B03Bu, COL_BUTTON_TXT);

    /* [Save] button in title bar */
    draw_button(pix, spx, bw, bh,
                WIN_W - 68, (TITLEBAR_H - 22) / 2, 56, 22,
                "Save", COL_SAVE_BG, COL_SAVE_HOV, COL_BUTTON_TXT);

    /* Dirty indicator in title bar next to "Notes" */
    if (g_dirty) {
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         8 + 5 * FONT_W + 8, (TITLEBAR_H - FONT_H) / 2,
                         "[+]", COL_DIRTY);
    }

    /* ---------- Sidebar ---------- */
    fill_rect(pix, spx, bw, bh, 0, TITLEBAR_H, SIDEBAR_W, EDITOR_H, COL_SIDEBAR_BG);

    /* Sidebar header */
    fill_rect(pix, spx, bw, bh, 0, TITLEBAR_H, SIDEBAR_W, 22, COL_TITLEBAR_BG);
    font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                     4, TITLEBAR_H + 3, "Notes", COL_TEXT_DIM);

    /* List entries */
    for (int i = 0; i < SIDEBAR_VISIBLE; i++) {
        int idx = g_sidebar_scroll + i;
        if (idx >= g_note_count) break;

        i32 ry = TITLEBAR_H + 22 + i * SIDEBAR_ROW_H;
        int hover = pt_in_rect(g_mouse_x, g_mouse_y, 0, ry, SIDEBAR_W, SIDEBAR_ROW_H);

        u32 row_bg;
        u32 txt_color;
        if (idx == g_selected_note) {
            row_bg   = COL_SIDEBAR_SEL;
            txt_color = COL_SELECTED_TXT;
        } else if (hover) {
            row_bg   = COL_SIDEBAR_HOV;
            txt_color = COL_TEXT;
        } else {
            row_bg   = COL_SIDEBAR_BG;
            txt_color = COL_TEXT_DIM;
        }

        fill_rect(pix, spx, bw, bh, 0, ry, SIDEBAR_W, SIDEBAR_ROW_H, row_bg);

        /* Truncate name to fit sidebar */
        char display[24];
        k_strlcpy(display, g_notes[idx].name, sizeof(display));
        /* Remove .txt suffix for cleaner display */
        int dlen = (int)k_strlen(display);
        if (dlen >= 4 &&
            display[dlen-4] == '.' && display[dlen-3] == 't' &&
            display[dlen-2] == 'x' && display[dlen-1] == 't') {
            display[dlen-4] = '\0';
        }
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         6, ry + (SIDEBAR_ROW_H - FONT_H) / 2,
                         display, txt_color);
    }

    /* "no notes" hint */
    if (g_note_count == 0) {
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         4, TITLEBAR_H + 28, "No notes yet.", COL_TEXT_DIM);
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         4, TITLEBAR_H + 44, "Press F5 or", COL_TEXT_DIM);
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         4, TITLEBAR_H + 60, "click [New].", COL_TEXT_DIM);
    }

    /* ---------- Divider ---------- */
    fill_rect(pix, spx, bw, bh, SIDEBAR_W, TITLEBAR_H, DIVIDER_W, EDITOR_H, COL_DIVIDER);

    /* ---------- Editor background ---------- */
    fill_rect(pix, spx, bw, bh, EDITOR_X, TITLEBAR_H, EDITOR_W, EDITOR_H, COL_EDITOR_BG);

    /* File name header inside editor area */
    {
        char hdr[NOTE_NAME_LEN + 16];
        if (g_cur_name[0] != '\0') {
            k_strlcpy(hdr, g_cur_name, sizeof(hdr));
        } else {
            k_strlcpy(hdr, "(new note)", sizeof(hdr));
        }
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         EDITOR_X + 4, TITLEBAR_H + 2, hdr, COL_TEXT_DIM);
        /* horizontal rule under the name */
        fill_rect(pix, spx, bw, bh, EDITOR_X, TITLEBAR_H + FONT_H + 4,
                  EDITOR_W, 1, COL_DIVIDER);
    }

    /* Cursor blink period 600ms */
    int blink_on = ((ticks / 500) & 1) == 0;

    /* Editor text lines */
    i32 text_top = TITLEBAR_H + FONT_H + 6;
    for (int vi = 0; vi < EDIT_ROWS; vi++) {
        int row = g_scroll_row + vi;
        if (row >= g_num_lines) break;

        i32 py = text_top + vi * FONT_H;

        /* Cursor on this row? */
        if (row == g_cur_row) {
            i32 cx = EDIT_LEFT + g_cur_col * FONT_W;
            if (blink_on) {
                fill_rect(pix, spx, bw, bh, cx, py, FONT_W, FONT_H, COL_CURSOR);
            }
        }

        /* Draw each character */
        for (int ci = 0; ci < g_line_len[row]; ci++) {
            i32 cx = EDIT_LEFT + ci * FONT_W;
            char c = g_lines[row][ci];
            u32 fg;
            if (row == g_cur_row && ci == g_cur_col && blink_on) {
                fg = COL_CURSOR_TXT;
            } else {
                fg = COL_TEXT;
            }
            font_draw_char(pix, (int)spx, (int)bw, (int)bh, cx, py, c, fg);
        }
    }

    /* ---------- Status bar ---------- */
    fill_rect(pix, spx, bw, bh, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);
    fill_rect(pix, spx, bw, bh, 0, WIN_H - STATUS_H, WIN_W, 1, COL_DIVIDER);

    {
        char sb[128];
        k_strlcpy(sb, " Ln ", sizeof(sb));
        char ns[16];
        int_to_str((i64)(g_cur_row + 1), ns);
        k_strlcat(sb, ns, sizeof(sb));
        k_strlcat(sb, " Col ", sizeof(sb));
        int_to_str((i64)(g_cur_col + 1), ns);
        k_strlcat(sb, ns, sizeof(sb));
        k_strlcat(sb, "  |  F2=Save  F5=New", sizeof(sb));
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         0, WIN_H - STATUS_H + 2, sb, COL_TEXT_DIM);
    }
}

/* ---- input handling ---- */

static void handle_key(int keycode, int pressed)
{
    /* Track modifier keys */
    if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
        g_shift = pressed;
        return;
    }
    if (keycode == KEY_LEFTCTRL) {
        g_ctrl = pressed;
        return;
    }

    if (!pressed) return;

    switch (keycode) {
    case KEY_ENTER:
        do_enter();
        break;

    case KEY_BACKSPACE:
        do_backspace();
        break;

    case KEY_LEFT:
        if (g_cur_col > 0) {
            g_cur_col--;
        } else if (g_cur_row > 0) {
            g_cur_row--;
            g_cur_col = g_line_len[g_cur_row];
        }
        break;

    case KEY_RIGHT:
        if (g_cur_col < g_line_len[g_cur_row]) {
            g_cur_col++;
        } else if (g_cur_row < g_num_lines - 1) {
            g_cur_row++;
            g_cur_col = 0;
        }
        break;

    case KEY_UP:
        if (g_cur_row > 0) {
            g_cur_row--;
            if (g_cur_col > g_line_len[g_cur_row])
                g_cur_col = g_line_len[g_cur_row];
        }
        break;

    case KEY_DOWN:
        if (g_cur_row < g_num_lines - 1) {
            g_cur_row++;
            if (g_cur_col > g_line_len[g_cur_row])
                g_cur_col = g_line_len[g_cur_row];
        }
        break;

    case KEY_F2:
        save_note();
        break;

    case KEY_F5:
        new_note();
        break;

    default: {
        if (keycode >= 0 && keycode < 256) {
            const char *km = g_shift ? keymap_hi : keymap_low;
            char c = km[keycode];
            if (c >= 0x20 && c <= 0x7E) {
                insert_char(c);
            }
        }
        break;
    }
    }

    ensure_visible();
}

/* Handle a mouse click. */
static void handle_click(int mx, int my)
{
    /* [New] button */
    {
        i32 bx = WIN_W - 130;
        i32 by = (TITLEBAR_H - 22) / 2;
        if (pt_in_rect(mx, my, bx, by, 56, 22)) {
            new_note();
            return;
        }
    }

    /* [Save] button */
    {
        i32 bx = WIN_W - 68;
        i32 by = (TITLEBAR_H - 22) / 2;
        if (pt_in_rect(mx, my, bx, by, 56, 22)) {
            save_note();
            return;
        }
    }

    /* Sidebar note entries */
    if (mx < SIDEBAR_W && my >= TITLEBAR_H + 22 && my < WIN_H - STATUS_H) {
        int row = (my - (TITLEBAR_H + 22)) / SIDEBAR_ROW_H;
        int idx = g_sidebar_scroll + row;
        if (idx >= 0 && idx < g_note_count) {
            load_note(idx);
        }
        return;
    }

    /* Click in editor area: place cursor */
    i32 text_top = TITLEBAR_H + FONT_H + 6;
    if (mx >= EDITOR_X && my >= text_top && my < WIN_H - STATUS_H) {
        int vi = (my - text_top) / FONT_H;
        int row = g_scroll_row + vi;
        if (row >= g_num_lines) row = g_num_lines - 1;
        if (row < 0) row = 0;
        int col = (mx - EDIT_LEFT) / FONT_W;
        if (col < 0) col = 0;
        if (col > g_line_len[row]) col = g_line_len[row];
        g_cur_row = row;
        g_cur_col = col;
    }
}

/* ================================================================
 * _start
 * ================================================================ */
void _start(void)
{
    serial_print("[NOTES] starting\n");

    /* Zero-init all static buffers */
    k_memset(g_notes,    0, sizeof(g_notes));
    k_memset(g_lines,    0, sizeof(g_lines));
    k_memset(g_line_len, 0, sizeof(g_line_len));

    g_note_count     = 0;
    g_selected_note  = -1;
    g_sidebar_scroll = 0;
    g_num_lines      = 1;
    g_cur_row        = 0;
    g_cur_col        = 0;
    g_scroll_row     = 0;
    g_dirty          = 0;
    g_next_note_num  = 1;
    g_shift          = 0;
    g_ctrl           = 0;
    g_mouse_x        = 0;
    g_mouse_y        = 0;
    g_mouse_btn      = 0;
    g_cur_name[0]    = '\0';

    /* Ensure /tmp/notes directory exists (best-effort) */
    ensure_notes_dir();

    /* Scan for existing notes */
    scan_notes();

    /* Connect to compositor */
    if (wl_connect() != 0) {
        serial_print("[NOTES] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Notes");
    if (!win) {
        serial_print("[NOTES] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    serial_print("[NOTES] window ");
    serial_num((i64)win->win_id);
    serial_print(" created\n");

    /* If there are existing notes, load the first one. */
    if (g_note_count > 0) {
        load_note(0);
    }

    int prev_btn = 0;

    for (;;) {
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                handle_key(a, b);
            } else if (kind == WL_EVENT_POINTER) {
                g_mouse_x = a;
                g_mouse_y = b;
                int cur_btn = c;

                /* Detect button press (transition 0->1) */
                if (cur_btn && !prev_btn) {
                    handle_click(a, b);
                }
                prev_btn = cur_btn;
            }
        }

        u64 ticks = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        render(win, ticks);
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
