/*
 * editor.c -- Multi-line text editor with clipboard, line numbers, status bar.
 * ===========================================================================
 *
 * Builds directly on wl_client + bitfont. No libc.
 * Window: 600x420.
 * Layout: [line-number gutter | text area] + status bar at bottom.
 *
 * New in this revision
 * --------------------
 *  - Line-number gutter (right-aligned, 4-digit, COL_LINE_NUM colour).
 *  - Status bar: "Ln R  Col C  [*]  edit.txt  ^S=Save ^N=New ^C=Copy ^X=Cut ^V=Paste"
 *  - Ctrl detection: keycode 29 = KEY_LEFTCTRL; press/release tracked in
 *    `ctrl_held`.  All Ctrl+key combos tested inside handle_key().
 *  - Clipboard (system): SYS_CLIP_SET=63 / SYS_CLIP_GET=64.
 *      Ctrl+C  – copy current line into clipboard
 *      Ctrl+X  – cut current line (copy then delete)
 *      Ctrl+V  – paste clipboard at cursor (inserts chars, then newline)
 *  - Ctrl+S  – save (also kept on F2)
 *  - Ctrl+N  – new / clear buffer
 *  - Smooth scrolling: scroll_row tracks so cursor stays visible.
 *  - All path/clipboard buffers are static and zeroed at startup.
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -Iuserspace/lib/wl -Iuserspace/lib/font -Ikernel/include \
 *       -c userspace/apps/editor/editor.c -o /tmp/ed.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld /tmp/ed.o /tmp/wlc.o /tmp/bf.o -o /tmp/ed.elf
 *   objdump -d /tmp/ed.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* ---- syscall numbers ---- */
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40
#define SYS_CLIP_SET      63   /* sc(63, buf, len, 0) -> copy to OS clipboard */
#define SYS_CLIP_GET      64   /* sc(64, buf, max, 0) -> bytes read;
                                  sc(64, 0,   0,  0) -> total length */

/* ---- O_CREAT | O_WRONLY flags ---- */
#define O_WRONLY    1
#define O_CREAT     0x40   /* 0100 octal, Linux x86-64 */
#define O_TRUNC     0x200  /* 0400 octal, Linux x86-64 */

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;
typedef long           i64;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---- inline syscall (3 args, convenience wrapper) ---- */
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

/* ---- serial diagnostics (fd 1) ---- */
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

/* ---- small memory helpers ---- */
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

static void k_memset(void *dst, u8 v, u64 n)
{
    u8 *d = (u8*)dst;
    for (u64 i = 0; i < n; i++) d[i] = v;
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

/* Right-align n into a field of `width` chars; pad with spaces */
static void int_to_str_padded(i64 n, char *buf, int width)
{
    char tmp[24];
    int_to_str(n, tmp);
    int len = (int)k_strlen(tmp);
    int pad = width - len;
    int i = 0;
    while (pad-- > 0) buf[i++] = ' ';
    for (int j = 0; tmp[j]; j++) buf[i++] = tmp[j];
    buf[i] = 0;
}

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

/* ---- keycodes ---- */
#define KEY_ESC         1
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29   /* Ctrl key — press/release tracked separately */
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_SPACE       57
#define KEY_F2          60
#define KEY_UP          103
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_DOWN        108

/*
 * US layout keycode -> ASCII map (lowercase, no shift).
 * Index is the keycode; 0 = not printable.
 */
static const char keymap[256] = {
    /*  0 */ 0,
    /*  1 */ 0,  /* ESC */
    /*  2 */ '1',
    /*  3 */ '2',
    /*  4 */ '3',
    /*  5 */ '4',
    /*  6 */ '5',
    /*  7 */ '6',
    /*  8 */ '7',
    /*  9 */ '8',
    /* 10 */ '9',
    /* 11 */ '0',
    /* 12 */ '-',
    /* 13 */ '=',
    /* 14 */ 0,  /* BACKSPACE */
    /* 15 */ '\t',
    /* 16 */ 'q',
    /* 17 */ 'w',
    /* 18 */ 'e',
    /* 19 */ 'r',
    /* 20 */ 't',
    /* 21 */ 'y',
    /* 22 */ 'u',
    /* 23 */ 'i',
    /* 24 */ 'o',
    /* 25 */ 'p',
    /* 26 */ '[',
    /* 27 */ ']',
    /* 28 */ 0,  /* ENTER */
    /* 29 */ 0,  /* LEFTCTRL */
    /* 30 */ 'a',
    /* 31 */ 's',
    /* 32 */ 'd',
    /* 33 */ 'f',
    /* 34 */ 'g',
    /* 35 */ 'h',
    /* 36 */ 'j',
    /* 37 */ 'k',
    /* 38 */ 'l',
    /* 39 */ ';',
    /* 40 */ '\'',
    /* 41 */ '`',
    /* 42 */ 0,  /* LEFTSHIFT */
    /* 43 */ '\\',
    /* 44 */ 'z',
    /* 45 */ 'x',
    /* 46 */ 'c',
    /* 47 */ 'v',
    /* 48 */ 'b',
    /* 49 */ 'n',
    /* 50 */ 'm',
    /* 51 */ ',',
    /* 52 */ '.',
    /* 53 */ '/',
    /* 54 */ 0,  /* RIGHTSHIFT */
    /* 55 */ '*',
    /* 56 */ 0,  /* LEFTALT */
    /* 57 */ ' ',
    /* rest: 0 */
};

/* ---- editor state ---- */
#define MAX_LINES    256
#define MAX_LINE_LEN 512
#define CLIP_MAX     (MAX_LINE_LEN + 2)   /* enough for one line + newline + NUL */

static char lines[MAX_LINES][MAX_LINE_LEN];
static int  line_len[MAX_LINES];
static int  num_lines  = 1;
static int  cur_row    = 0;
static int  cur_col    = 0;
static int  dirty      = 0;
static int  ctrl_held  = 0;   /* 1 while left-Ctrl is physically pressed */

/* Static clipboard staging buffer (zeroed at startup) */
static char clip_buf[CLIP_MAX];

/* ---- geometry ---- */
#define WIN_W       600
#define WIN_H       420

/* Line-number gutter: 4 digit chars + 1 space separator */
#define GUTTER_COLS  5                      /* characters wide */
#define GUTTER_W    (GUTTER_COLS * FONT_W)  /* pixels */

#define TEXT_X      (GUTTER_W + 4)          /* left edge of text area */
#define TEXT_Y      2                        /* top margin */
#define STATUS_H    18
#define TEXT_AREA_H (WIN_H - STATUS_H)
#define VISIBLE_ROWS (TEXT_AREA_H / FONT_H)

/* ---- colors ---- */
#define COL_BG          0xFF1E1E1Eu
#define COL_GUTTER_BG   0xFF252525u   /* very slightly lighter than main bg */
#define COL_STATUS_BG   0xFF007ACCu
#define COL_TEXT        0xFFD4D4D4u
#define COL_CURSOR      0xFFAEAFADu
#define COL_CURSOR_TXT  0xFF1E1E1Eu
#define COL_STATUS_TXT  0xFFFFFFFFu
#define COL_DIRTY       0xFFFFCC00u
#define COL_LINE_NUM    0xFF858585u
#define COL_LINE_NUM_CUR 0xFFCCCCCCu  /* current-line number highlighted */

/* ---- scroll ---- */
static int scroll_row = 0;

static void ensure_visible(void)
{
    if (cur_row < scroll_row)
        scroll_row = cur_row;
    else if (cur_row >= scroll_row + VISIBLE_ROWS)
        scroll_row = cur_row - VISIBLE_ROWS + 1;
}

/* ---- insert char ---- */
static void insert_char(char c)
{
    if (num_lines == 0) { num_lines = 1; line_len[0] = 0; }
    int row = cur_row;
    int col = cur_col;
    int len = line_len[row];

    if (col > len) col = len;
    if (len >= MAX_LINE_LEN - 1) return;

    k_memmove(&lines[row][col + 1], &lines[row][col], (u64)(len - col));
    lines[row][col] = c;
    line_len[row]++;
    lines[row][line_len[row]] = 0;
    cur_col = col + 1;
    dirty = 1;
}

/* ---- Enter: split line ---- */
static void do_enter(void)
{
    if (num_lines >= MAX_LINES) return;

    int row = cur_row;
    int col = cur_col;
    int len = line_len[row];
    if (col > len) col = len;

    for (int i = num_lines - 1; i > row; i--) {
        k_memmove(lines[i + 1], lines[i], (u64)(line_len[i] + 1));
        line_len[i + 1] = line_len[i];
    }
    num_lines++;

    int tail = len - col;
    k_memmove(lines[row + 1], &lines[row][col], (u64)tail);
    line_len[row + 1] = tail;
    lines[row + 1][tail] = 0;

    line_len[row] = col;
    lines[row][col] = 0;

    cur_row = row + 1;
    cur_col = 0;
    dirty = 1;
}

/* ---- Backspace ---- */
static void do_backspace(void)
{
    int row = cur_row;
    int col = cur_col;

    if (col > 0) {
        int len = line_len[row];
        k_memmove(&lines[row][col - 1], &lines[row][col], (u64)(len - col + 1));
        line_len[row]--;
        cur_col = col - 1;
    } else if (row > 0) {
        int prev     = row - 1;
        int prev_len = line_len[prev];
        int cur_len  = line_len[row];

        if (prev_len + cur_len < MAX_LINE_LEN - 1) {
            k_memmove(&lines[prev][prev_len], lines[row], (u64)(cur_len + 1));
            line_len[prev] = prev_len + cur_len;

            for (int i = row; i < num_lines - 1; i++) {
                k_memmove(lines[i], lines[i + 1], (u64)(line_len[i + 1] + 1));
                line_len[i] = line_len[i + 1];
            }
            num_lines--;
            cur_row = prev;
            cur_col = prev_len;
        }
    }
    dirty = 1;
}

/* ---- save ---- */
static void do_save(void)
{
    int fd = (int)sc(SYS_OPEN, (i64)"/tmp/edit.txt",
                     O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) {
        serial_print("[EDIT] save: open failed\n");
        return;
    }
    for (int i = 0; i < num_lines; i++) {
        if (line_len[i] > 0)
            sc(SYS_WRITE, fd, (i64)lines[i], (i64)line_len[i], 0, 0, 0);
        sc(SYS_WRITE, fd, (i64)"\n", 1, 0, 0, 0);
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    dirty = 0;
    serial_print("[EDIT] saved /tmp/edit.txt\n");
}

/* ---- new / clear buffer ---- */
static void do_new(void)
{
    k_memset(lines,    0, sizeof(lines));
    k_memset(line_len, 0, sizeof(line_len));
    num_lines  = 1;
    cur_row    = 0;
    cur_col    = 0;
    scroll_row = 0;
    dirty      = 0;
    serial_print("[EDIT] new buffer\n");
}

/* ---- clipboard: copy current line to OS clipboard ---- */
static void do_copy(void)
{
    int row = cur_row;
    int len = line_len[row];

    /* Build "text\n" in static staging buffer, then zero the rest */
    k_memset(clip_buf, 0, CLIP_MAX);
    if (len > 0)
        k_memmove(clip_buf, lines[row], (u64)len);
    clip_buf[len]     = '\n';
    clip_buf[len + 1] = 0;

    i64 result = sc(SYS_CLIP_SET, (i64)clip_buf, (i64)(len + 1), 0, 0, 0, 0);
    serial_print("[EDIT] clip_set row=");
    serial_num((i64)row);
    serial_print(" len=");
    serial_num((i64)(len + 1));
    serial_print(" rc=");
    serial_num(result);
    serial_print("\n");
}

/* ---- clipboard: paste from OS clipboard at cursor ---- */
static void do_paste(void)
{
    /* Query length first (sc(64,0,0,0) -> byte count) */
    i64 avail = sc(SYS_CLIP_GET, 0, 0, 0, 0, 0, 0);
    if (avail <= 0) {
        serial_print("[EDIT] paste: clipboard empty\n");
        return;
    }

    /* Clamp to buffer */
    i64 want = avail;
    if (want >= CLIP_MAX) want = CLIP_MAX - 1;

    k_memset(clip_buf, 0, CLIP_MAX);
    i64 got = sc(SYS_CLIP_GET, (i64)clip_buf, want, 0, 0, 0, 0);
    if (got <= 0) return;
    clip_buf[got] = 0;

    /* Insert each character; newlines become do_enter() */
    for (i64 i = 0; i < got; i++) {
        char c = clip_buf[i];
        if (c == '\n' || c == '\r') {
            do_enter();
        } else if (c >= 0x20 && c <= 0x7E) {
            insert_char(c);
        }
    }

    serial_print("[EDIT] paste: inserted ");
    serial_num(got);
    serial_print(" bytes\n");
}

/* ---- clipboard: cut = copy then delete current line ---- */
static void do_cut(void)
{
    do_copy();

    int row = cur_row;

    if (num_lines == 1) {
        /* Only line: just clear it */
        k_memset(lines[0], 0, MAX_LINE_LEN);
        line_len[0] = 0;
        cur_col = 0;
    } else {
        /* Remove the line, shift everything up */
        for (int i = row; i < num_lines - 1; i++) {
            k_memmove(lines[i], lines[i + 1], (u64)(line_len[i + 1] + 1));
            line_len[i] = line_len[i + 1];
        }
        num_lines--;
        if (cur_row >= num_lines) cur_row = num_lines - 1;
        if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
    }
    dirty = 1;

    serial_print("[EDIT] cut row=");
    serial_num((i64)row);
    serial_print("\n");
}

/* ---- status bar text ---- */
static void build_status(char *buf, u64 buflen)
{
    u64 n = 0;
    const char *p;
    char tmp[16];

    /* " Ln R  Col C " */
    p = " Ln ";
    for (int i = 0; p[i] && n < buflen-1; i++) buf[n++] = p[i];
    int_to_str((i64)(cur_row + 1), tmp);
    for (int i = 0; tmp[i] && n < buflen-1; i++) buf[n++] = tmp[i];

    p = "  Col ";
    for (int i = 0; p[i] && n < buflen-1; i++) buf[n++] = p[i];
    int_to_str((i64)(cur_col + 1), tmp);
    for (int i = 0; tmp[i] && n < buflen-1; i++) buf[n++] = tmp[i];

    /* dirty indicator */
    if (dirty) {
        p = " [*]";
        for (int i = 0; p[i] && n < buflen-1; i++) buf[n++] = p[i];
    }

    /* filename */
    p = "  edit.txt";
    for (int i = 0; p[i] && n < buflen-1; i++) buf[n++] = p[i];

    /* key hints */
    p = "  ^S=Save ^N=New ^C=Copy ^X=Cut ^V=Paste";
    for (int i = 0; p[i] && n < buflen-1; i++) buf[n++] = p[i];

    buf[n] = 0;
}

/* ---- render one frame ---- */
static void render(wl_window *win, u64 ticks)
{
    u32 stride_px = win->stride / 4u;
    u32 bw = win->w;
    u32 bh = win->h;
    u32 *pix = win->pixels;

    /* Clear full background */
    fill_rect(pix, stride_px, bw, bh, 0, 0, (i32)bw, (i32)bh, COL_BG);

    /* Gutter background */
    fill_rect(pix, stride_px, bw, bh, 0, 0, GUTTER_W, (i32)(bh - STATUS_H), COL_GUTTER_BG);

    /* Status bar */
    i32 sb_y = (i32)bh - STATUS_H;
    fill_rect(pix, stride_px, bw, bh, 0, sb_y, (i32)bw, STATUS_H, COL_STATUS_BG);

    /* Draw text lines */
    int vis_rows = VISIBLE_ROWS;
    for (int vi = 0; vi < vis_rows; vi++) {
        int row = scroll_row + vi;
        if (row >= num_lines) break;

        i32 py = TEXT_Y + vi * FONT_H;

        /* --- Line number in gutter --- */
        {
            char ln_buf[8];
            int_to_str_padded((i64)(row + 1), ln_buf, 4);
            u32 ln_col = (row == cur_row) ? COL_LINE_NUM_CUR : COL_LINE_NUM;
            font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                             2, py, ln_buf, ln_col);
        }

        /* --- Cursor block --- */
        if (row == cur_row) {
            i32 cx = TEXT_X + cur_col * FONT_W;
            int blink_on = ((ticks / 500) & 1) == 0;
            if (blink_on) {
                fill_rect(pix, stride_px, bw, bh,
                          cx, py, FONT_W, FONT_H, COL_CURSOR);
            }
        }

        /* --- Text glyphs --- */
        for (int ci = 0; ci < line_len[row]; ci++) {
            i32 cx = TEXT_X + ci * FONT_W;
            char c = lines[row][ci];
            u32 fg = (row == cur_row && ci == cur_col) ? COL_CURSOR_TXT : COL_TEXT;
            font_draw_char(pix, (int)stride_px, (int)bw, (int)bh, cx, py, c, fg);
        }
    }

    /* Status text */
    char status_buf[160];
    build_status(status_buf, 160);
    font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                     4, sb_y + 1, status_buf, COL_STATUS_TXT);
}

/* ---- handle a key-press event ---- */
static void handle_key(int keycode)
{
    /* ---- Ctrl combos ---- */
    if (ctrl_held) {
        switch (keycode) {
        case KEY_S:
            do_save();
            return;
        case KEY_N:
            do_new();
            return;
        case KEY_C:
            do_copy();
            return;
        case KEY_X:
            do_cut();
            return;
        case KEY_V:
            do_paste();
            ensure_visible();
            return;
        default:
            /* Absorb other Ctrl+key combinations silently */
            return;
        }
    }

    /* ---- normal keys ---- */
    switch (keycode) {

    case KEY_ENTER:
        do_enter();
        break;

    case KEY_BACKSPACE:
        do_backspace();
        break;

    case KEY_LEFT:
        if (cur_col > 0) {
            cur_col--;
        } else if (cur_row > 0) {
            cur_row--;
            cur_col = line_len[cur_row];
        }
        break;

    case KEY_RIGHT:
        if (cur_col < line_len[cur_row]) {
            cur_col++;
        } else if (cur_row < num_lines - 1) {
            cur_row++;
            cur_col = 0;
        }
        break;

    case KEY_UP:
        if (cur_row > 0) {
            cur_row--;
            if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
        }
        break;

    case KEY_DOWN:
        if (cur_row < num_lines - 1) {
            cur_row++;
            if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
        }
        break;

    case KEY_F2:
        do_save();
        break;

    default: {
        if (keycode >= 0 && keycode < 256) {
            char c = keymap[keycode];
            if (c >= 0x20 && c <= 0x7E) insert_char(c);
        }
        break;
    }
    } /* switch */

    ensure_visible();
}

/* ================================================================
 * _start
 * ================================================================ */
void _start(void)
{
    serial_print("[EDIT] starting\n");

    /* Zero all static state */
    k_memset(lines,    0, sizeof(lines));
    k_memset(line_len, 0, sizeof(line_len));
    k_memset(clip_buf, 0, sizeof(clip_buf));
    num_lines  = 1;
    cur_row    = 0;
    cur_col    = 0;
    scroll_row = 0;
    dirty      = 0;
    ctrl_held  = 0;

    if (wl_connect() != 0) {
        serial_print("[EDIT] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Editor");
    if (!win) {
        serial_print("[EDIT] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    serial_print("[EDIT] window ");
    serial_num((i64)win->win_id);
    serial_print(" created\n");

    for (;;) {
        /* Drain all pending input events */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                /* Track Ctrl press/release.
                 * b == 1 -> key pressed; b == 0 -> key released. */
                if (a == KEY_LEFTCTRL) {
                    ctrl_held = b;   /* 1 on press, 0 on release */
                } else if (b /* pressed */) {
                    handle_key(a);
                }
            }
            /* pointer events ignored */
        }

        /* Render */
        u64 ticks = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        render(win, ticks);
        wl_commit(win);

        /* Yield to scheduler */
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
