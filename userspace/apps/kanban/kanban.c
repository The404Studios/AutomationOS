/*
 * kanban.c -- Sticky-note Kanban board (freestanding, ring 3).
 * =============================================================
 *
 * 840x520 window.  Three columns: "To Do", "Doing", "Done".
 * Each column holds sticky-note CARDS in soft pastel colors.
 *
 * Features:
 *   + button in column header -- create a new card (enter text, press Enter)
 *   Click card   -- select it; type to edit; Backspace to delete chars
 *   X button     -- delete card
 *   Arrow keys (Left/Right) when card selected -- move card between columns
 *   Drag card    -- drag-and-drop to another column
 *   Ctrl+C       -- copy selected card text to clipboard
 *   Ctrl+V       -- paste clipboard into new card in selected column
 *
 * Persistence:
 *   On every change the board is saved to /tmp/kanban.dat.
 *   Format: one card per line  "COL\tTEXT\n"  (COL = 0/1/2).
 *   Loaded on startup.
 *
 * Syscalls used:
 *   SYS_READ      = 2
 *   SYS_WRITE     = 3
 *   SYS_OPEN      = 4
 *   SYS_CLOSE     = 5
 *   SYS_YIELD     = 15
 *   SYS_GETTIME   = 42
 *   SYS_CLIP_SET  = 63
 *   SYS_CLIP_GET  = 64
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/kanban/kanban.c -o /tmp/kanban.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/kanban.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/kanban.elf
 *   objdump -d /tmp/kanban.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* ---- syscall numbers ---- */
#define SYS_READ      2
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_YIELD     15
#define SYS_GETTIME   42
#define SYS_CLIP_SET  63
#define SYS_CLIP_GET  64

/* ---- open() flags ---- */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0x40
#define O_TRUNC     0x200

/* ---- fixed-width types ---- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;
typedef long           i64;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ================================================================
 * Inline syscall
 * ================================================================ */
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

/* ================================================================
 * String / memory helpers (no libc)
 * ================================================================ */
static u64 k_strlen(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void k_memset(void *dst, u8 v, u64 n)
{
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = v;
}

static void k_memmove(void *dst, const void *src, u64 n)
{
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d < s) {
        for (u64 i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (u64 i = n; i > 0; i--) d[i - 1] = s[i - 1];
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
    if (n < 0)  { *buf++ = '-'; n = -n; }
    do { tmp[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

/* ---- serial diagnostics ---- */
static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (i64)m, (i64)k_strlen(m), 0, 0, 0);
}

/* ================================================================
 * Drawing helpers
 * ================================================================ */
static void fill_rect(u32 *pix, u32 spx, u32 bw, u32 bh,
                      i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = pix + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = col;
    }
}

static void draw_border(u32 *pix, u32 spx, u32 bw, u32 bh,
                        i32 x, i32 y, i32 w, i32 h, u32 col)
{
    fill_rect(pix, spx, bw, bh, x,     y,     w, 1, col);
    fill_rect(pix, spx, bw, bh, x,     y+h-1, w, 1, col);
    fill_rect(pix, spx, bw, bh, x,     y,     1, h, col);
    fill_rect(pix, spx, bw, bh, x+w-1, y,     1, h, col);
}

/* Draw a filled rounded rect (approximated with corner pixels cleared) */
static void fill_rounded(u32 *pix, u32 spx, u32 bw, u32 bh,
                         i32 x, i32 y, i32 w, i32 h, u32 col, i32 r)
{
    if (r < 1) r = 1;
    fill_rect(pix, spx, bw, bh, x+r, y,   w-2*r, h,   col);
    fill_rect(pix, spx, bw, bh, x,   y+r, r,     h-2*r, col);
    fill_rect(pix, spx, bw, bh, x+w-r, y+r, r,   h-2*r, col);
    /* corner fills */
    for (i32 cy = 0; cy < r; cy++) {
        for (i32 cx = 0; cx < r; cx++) {
            /* distance from corner center */
            i32 dx = r - 1 - cx;
            i32 dy = r - 1 - cy;
            if (dx*dx + dy*dy <= (r-1)*(r-1)+r) {
                /* top-left */
                fill_rect(pix, spx, bw, bh, x+cx, y+cy, 1, 1, col);
                /* top-right */
                fill_rect(pix, spx, bw, bh, x+w-1-cx, y+cy, 1, 1, col);
                /* bottom-left */
                fill_rect(pix, spx, bw, bh, x+cx, y+h-1-cy, 1, 1, col);
                /* bottom-right */
                fill_rect(pix, spx, bw, bh, x+w-1-cx, y+h-1-cy, 1, 1, col);
            }
        }
    }
}

/* Simple shadow: draw 2px offset dark translucent rect */
static void draw_shadow(u32 *pix, u32 spx, u32 bw, u32 bh,
                        i32 x, i32 y, i32 w, i32 h)
{
    /* Shadow is a semi-transparent dark strip offset by 2,2 */
    u32 shadow = 0x44000000u;
    i32 sx = x + 3;
    i32 sy = y + 3;
    i32 x1 = sx < 0 ? 0 : sx;
    i32 y1 = sy < 0 ? 0 : sy;
    i32 x2 = sx + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = sy + h; if (y2 > (i32)bh) y2 = (i32)bh;
    (void)shadow;
    /* Darken existing pixels */
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = pix + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++) {
            u32 p = row[xx];
            u32 nr = ((p >> 16) & 0xFF);
            u32 ng = ((p >>  8) & 0xFF);
            u32 nb = ( p        & 0xFF);
            nr = nr * 3 / 4;
            ng = ng * 3 / 4;
            nb = nb * 3 / 4;
            row[xx] = (p & 0xFF000000u) | (nr << 16) | (ng << 8) | nb;
        }
    }
}

/* ================================================================
 * Key codes (same mapping as notes.c)
 * ================================================================ */
#define KEY_ESC         1
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_LEFTSHIFT   42
#define KEY_RIGHTSHIFT  54
#define KEY_SPACE       57
#define KEY_UP          103
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_DOWN        108
#define KEY_DELETE      111

static const char keymap_low[256] = {
    /* 0  */ 0,   0,   '1', '2', '3', '4', '5', '6',
    /* 8  */ '7', '8', '9', '0', '-', '=', 0,   '\t',
    /* 16 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 24 */ 'o', 'p', '[', ']', 0,   0,   'a', 's',
    /* 32 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 40 */ '\'','`', 0,  '\\','z', 'x', 'c', 'v',
    /* 48 */ 'b', 'n', 'm', ',', '.', '/', 0,   '*',
    /* 56 */ 0,   ' ', 0,
};

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

/* ================================================================
 * Window / layout constants
 * ================================================================ */
#define WIN_W        840
#define WIN_H        520

#define TITLEBAR_H   36
#define STATUS_H     22
#define BOARD_TOP    TITLEBAR_H
#define BOARD_H      (WIN_H - TITLEBAR_H - STATUS_H)

/* 3 columns; small gaps between */
#define NUM_COLS     3
#define COL_GAP      8
#define COL_W        ((WIN_W - (NUM_COLS + 1) * COL_GAP) / NUM_COLS)
#define COL_HDR_H    38

/* Card geometry */
#define CARD_W       (COL_W - 12)
#define CARD_H       56
#define CARD_GAP     8
#define CARD_PAD_X   8
#define CARD_PAD_Y   6
#define CARD_TEXT_ROWS  2    /* lines of text visible per card */

/* Max cards per column */
#define MAX_CARDS_PER_COL  32
/* Max text per card */
#define CARD_TEXT_LEN      128

/* ================================================================
 * Colors -- pastel sticky-note palette
 * ================================================================ */
#define COL_BG            0xFF1E1E2Eu  /* dark navy background */
#define COL_TITLEBAR      0xFF181825u
#define COL_STATUS_BG     0xFF181825u
#define COL_TEXT_MAIN     0xFFCDD6F4u
#define COL_TEXT_DIM      0xFF6C7086u
#define COL_DIVIDER       0xFF313244u
#define COL_HEADER_TXT    0xFFA6E3A1u
#define COL_BORDER        0xFF45475Au

/* Column header backgrounds */
static const u32 COL_HDR_BG[3] = {
    0xFF313244u,   /* To Do   -- slate */
    0xFF2D3B55u,   /* Doing   -- blue-slate */
    0xFF1E3A2Fu,   /* Done    -- dark green */
};

/* Column body backgrounds (slightly lighter than window) */
static const u32 COL_BODY_BG[3] = {
    0xFF262637u,
    0xFF232E45u,
    0xFF1A2E24u,
};

/* Sticky card pastel fill colors (rotated per card index) */
static const u32 CARD_COLORS[6] = {
    0xFFF5E6A3u,   /* pastel yellow  */
    0xFFB5E3C8u,   /* pastel mint    */
    0xFFB5CBF5u,   /* pastel blue    */
    0xFFF5B5C8u,   /* pastel pink    */
    0xFFE8C8F5u,   /* pastel lavender*/
    0xFFFFD8B0u,   /* pastel peach   */
};
#define NUM_CARD_COLORS  6

#define CARD_TEXT_COL    0xFF1E1E2Eu   /* dark text on pastel */
#define CARD_SEL_BORDER  0xFF89B4FAu   /* blue selection ring  */
#define CARD_DRAG_ALPHA  0xCC000000u   /* dragged card overlay */

/* Add-button color */
#define COL_ADD_BTN      0xFF40A02Bu
#define COL_ADD_HOV      0xFF50B03Bu
#define COL_ADD_TXT      0xFFFFFFFFu

/* Column label colors */
static const u32 COL_LABEL[3] = {
    0xFFCBA6F7u,   /* mauve  */
    0xFF89DCEB u,  /* sky    */
    0xFFA6E3A1u,   /* green  */
};

/* ================================================================
 * Data model
 * ================================================================ */
typedef struct {
    char text[CARD_TEXT_LEN];
    int  text_len;
    int  col;       /* 0=ToDo 1=Doing 2=Done */
} card_t;

#define MAX_TOTAL_CARDS  (MAX_CARDS_PER_COL * NUM_COLS)

static card_t g_cards[MAX_TOTAL_CARDS];
static int    g_card_count;

/* Per-column ordered index arrays */
static int g_col_cards[NUM_COLS][MAX_CARDS_PER_COL];
static int g_col_count[NUM_COLS];

/* Selection & editing */
static int g_sel_card;       /* -1 = none selected */
static int g_editing;        /* 1 when a card is in active edit mode (typing) */
static int g_cursor_pos;     /* cursor in the selected card text */

/* New-card entry mode */
static int g_adding_col;     /* -1 = not adding; 0-2 = column being added to */
static char g_add_buf[CARD_TEXT_LEN];
static int  g_add_buf_len;

/* Drag state */
static int g_drag_card;      /* -1 = no drag */
static int g_drag_ox;        /* offset from card origin when drag started */
static int g_drag_oy;
static int g_drag_x;         /* current drag position */
static int g_drag_y;

/* Modifier keys */
static int g_shift;
static int g_ctrl;

/* Mouse state */
static int g_mouse_x;
static int g_mouse_y;
static int g_mouse_btn;
static int g_prev_btn;

/* Path buffer (safe copy for kernel) */
static char g_path_buf[256];

static void safe_path(const char *src)
{
    k_memset(g_path_buf, 0, sizeof(g_path_buf));
    k_strlcpy(g_path_buf, src, sizeof(g_path_buf));
}

/* Clipboard buffer */
static char g_clip_buf[CARD_TEXT_LEN];

/* ================================================================
 * Column-index helpers
 * ================================================================ */
static void rebuild_col_index(void)
{
    for (int c = 0; c < NUM_COLS; c++) g_col_count[c] = 0;
    for (int i = 0; i < g_card_count; i++) {
        int c = g_cards[i].col;
        if (c < 0 || c >= NUM_COLS) continue;
        if (g_col_count[c] < MAX_CARDS_PER_COL)
            g_col_cards[c][g_col_count[c]++] = i;
    }
}

/* ================================================================
 * Persistence
 * ================================================================ */
#define KANBAN_FILE  "/tmp/kanban.dat"

static void save_board(void)
{
    safe_path(KANBAN_FILE);
    i64 fd = sc(SYS_OPEN, (i64)g_path_buf,
                O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) {
        serial_print("[KANBAN] save: open failed\n");
        return;
    }

    for (int i = 0; i < g_card_count; i++) {
        char col_ch[2];
        col_ch[0] = (char)('0' + g_cards[i].col);
        col_ch[1] = '\t';
        sc(SYS_WRITE, fd, (i64)col_ch,            2, 0, 0, 0);
        sc(SYS_WRITE, fd, (i64)g_cards[i].text,
           (i64)g_cards[i].text_len,               0, 0, 0);
        sc(SYS_WRITE, fd, (i64)"\n",               1, 0, 0, 0);
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    serial_print("[KANBAN] saved\n");
}

static void load_board(void)
{
    safe_path(KANBAN_FILE);
    i64 fd = sc(SYS_OPEN, (i64)g_path_buf, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) {
        serial_print("[KANBAN] no saved board\n");
        return;
    }

    static char load_buf[MAX_TOTAL_CARDS * (CARD_TEXT_LEN + 4)];
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

    /* Parse: "COL\tTEXT\n" */
    g_card_count = 0;
    int pos = 0;
    while (pos < (int)total && g_card_count < MAX_TOTAL_CARDS) {
        /* Read column digit */
        if (load_buf[pos] < '0' || load_buf[pos] > '2') { pos++; continue; }
        int col = load_buf[pos] - '0';
        pos++;
        if (pos >= (int)total || load_buf[pos] != '\t') continue;
        pos++; /* skip tab */

        /* Read text until newline */
        int start = pos;
        while (pos < (int)total && load_buf[pos] != '\n') pos++;
        int len = pos - start;
        if (len > CARD_TEXT_LEN - 1) len = CARD_TEXT_LEN - 1;
        if (pos < (int)total) pos++; /* skip newline */

        card_t *card = &g_cards[g_card_count];
        k_memmove(card->text, load_buf + start, (u64)len);
        card->text[len] = '\0';
        card->text_len  = len;
        card->col       = col;
        g_card_count++;
    }

    serial_print("[KANBAN] loaded ");
    char nb[16]; int_to_str((i64)g_card_count, nb);
    serial_print(nb);
    serial_print(" cards\n");
    rebuild_col_index();
}

/* ================================================================
 * Card management
 * ================================================================ */
static int add_card(int col, const char *text, int text_len)
{
    if (g_card_count >= MAX_TOTAL_CARDS) return -1;
    if (g_col_count[col] >= MAX_CARDS_PER_COL) return -1;

    int idx = g_card_count++;
    card_t *c = &g_cards[idx];
    if (text_len > CARD_TEXT_LEN - 1) text_len = CARD_TEXT_LEN - 1;
    k_memmove(c->text, text, (u64)text_len);
    c->text[text_len] = '\0';
    c->text_len = text_len;
    c->col = col;
    rebuild_col_index();
    save_board();
    return idx;
}

static void delete_card(int idx)
{
    if (idx < 0 || idx >= g_card_count) return;
    /* Shift all cards down */
    for (int i = idx; i < g_card_count - 1; i++)
        g_cards[i] = g_cards[i + 1];
    g_card_count--;

    /* Fix selection */
    if (g_sel_card == idx) {
        g_sel_card   = -1;
        g_editing    = 0;
        g_cursor_pos = 0;
    } else if (g_sel_card > idx) {
        g_sel_card--;
    }

    rebuild_col_index();
    save_board();
}

static void move_card_col(int idx, int new_col)
{
    if (idx < 0 || idx >= g_card_count) return;
    if (new_col < 0 || new_col >= NUM_COLS) return;
    if (g_col_count[new_col] >= MAX_CARDS_PER_COL) return;
    g_cards[idx].col = new_col;
    rebuild_col_index();
    save_board();
}

/* ================================================================
 * Layout helpers -- column X position and card rectangle
 * ================================================================ */
static i32 col_x(int c)
{
    return COL_GAP + c * (COL_W + COL_GAP);
}

/* Y of card slot 'slot' inside column c (absolute window coords) */
static i32 card_y(int slot)
{
    return BOARD_TOP + COL_HDR_H + COL_GAP + slot * (CARD_H + CARD_GAP);
}

/* Hit-test helpers */
static int pt_in(int px, int py, int rx, int ry, int rw, int rh)
{
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

/* Which column does screen-x fall in? -1 = none */
static int x_to_col(int x)
{
    for (int c = 0; c < NUM_COLS; c++) {
        if (x >= col_x(c) && x < col_x(c) + COL_W)
            return c;
    }
    return -1;
}

/* ================================================================
 * Rendering
 * ================================================================ */

/* Draw card text (wraps after ~(CARD_W - 2*CARD_PAD_X) pixels) */
static void draw_card_text(u32 *pix, u32 spx, u32 bw, u32 bh,
                           i32 cx, i32 cy, const char *text, int text_len,
                           u32 fg, int show_cursor, int cursor_pos)
{
    /* Max chars per line */
    int max_ch = (CARD_W - 2 * CARD_PAD_X) / FONT_W;
    if (max_ch < 1) max_ch = 1;

    int row = 0;
    int pos = 0;
    while (pos <= text_len && row < CARD_TEXT_ROWS) {
        /* Compute segment */
        int seg_end = pos + max_ch;
        if (seg_end > text_len) seg_end = text_len;

        i32 ty = cy + CARD_PAD_Y + row * (FONT_H + 2);

        /* Draw text segment */
        for (int i = pos; i < seg_end; i++) {
            i32 tx = cx + CARD_PAD_X + (i - pos) * FONT_W;

            /* Cursor block */
            if (show_cursor && i == cursor_pos) {
                fill_rect(pix, spx, bw, bh, tx, ty, FONT_W, FONT_H, 0xFF89B4FAu);
                font_draw_char(pix, (int)spx, (int)bw, (int)bh,
                               tx, ty, text[i], 0xFF1E1E2Eu);
            } else {
                font_draw_char(pix, (int)spx, (int)bw, (int)bh,
                               tx, ty, text[i], fg);
            }
        }

        /* Cursor at end of segment */
        if (show_cursor && cursor_pos == seg_end && row < CARD_TEXT_ROWS - 1) {
            i32 tx = cx + CARD_PAD_X + (seg_end - pos) * FONT_W;
            fill_rect(pix, spx, bw, bh, tx, ty, 2, FONT_H, 0xFF89B4FAu);
        } else if (show_cursor && cursor_pos >= seg_end && seg_end == text_len) {
            i32 tx = cx + CARD_PAD_X + (seg_end - pos) * FONT_W;
            fill_rect(pix, spx, bw, bh, tx, ty, 2, FONT_H, 0xFF89B4FAu);
        }

        if (seg_end >= text_len) break;
        pos = seg_end;
        row++;
    }
}

static void render(wl_window *win, u64 ticks)
{
    u32  spx = win->stride / 4u;
    u32  bw  = win->w;
    u32  bh  = win->h;
    u32 *pix = win->pixels;

    int blink_on = ((ticks / 500) & 1) == 0;

    /* ---- Background ---- */
    fill_rect(pix, spx, bw, bh, 0, 0, WIN_W, WIN_H, COL_BG);

    /* ---- Title bar ---- */
    fill_rect(pix, spx, bw, bh, 0, 0, WIN_W, TITLEBAR_H, COL_TITLEBAR);
    fill_rect(pix, spx, bw, bh, 0, TITLEBAR_H - 1, WIN_W, 1, COL_DIVIDER);

    font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                     10, (TITLEBAR_H - FONT_H) / 2,
                     "Kanban", COL_HEADER_TXT);

    /* Hint text in title bar */
    font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                     WIN_W - 320, (TITLEBAR_H - FONT_H) / 2,
                     "Arr=Move  Del=Delete  Ctrl+C/V=Clipboard",
                     COL_TEXT_DIM);

    /* ---- Columns ---- */
    static const char *col_names[3] = { "To Do", "Doing", "Done" };

    for (int c = 0; c < NUM_COLS; c++) {
        i32 cx = col_x(c);

        /* Column body */
        fill_rounded(pix, spx, bw, bh,
                     cx, BOARD_TOP, COL_W, BOARD_H,
                     COL_BODY_BG[c], 6);

        /* Column header */
        fill_rounded(pix, spx, bw, bh,
                     cx, BOARD_TOP, COL_W, COL_HDR_H,
                     COL_HDR_BG[c], 6);
        /* Square off bottom of header */
        fill_rect(pix, spx, bw, bh,
                  cx, BOARD_TOP + COL_HDR_H / 2,
                  COL_W, COL_HDR_H - COL_HDR_H / 2,
                  COL_HDR_BG[c]);

        /* Column label */
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         cx + 10, BOARD_TOP + (COL_HDR_H - FONT_H) / 2,
                         col_names[c], COL_LABEL[c]);

        /* Card count badge */
        {
            char cnt[8];
            int_to_str((i64)g_col_count[c], cnt);
            i32 bx = cx + COL_W - 30;
            i32 by = BOARD_TOP + (COL_HDR_H - FONT_H) / 2;
            font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                             bx, by, cnt, COL_TEXT_DIM);
        }

        /* "+" add button */
        {
            i32 bx = cx + COL_W - 22;
            i32 by = BOARD_TOP + (COL_HDR_H - 18) / 2 + 1;
            int hover = pt_in(g_mouse_x, g_mouse_y, bx, by, 18, 18);
            fill_rounded(pix, spx, bw, bh, bx, by, 18, 18,
                         hover ? COL_ADD_HOV : COL_ADD_BTN, 4);
            font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                             bx + 5, by + 1, "+", COL_ADD_TXT);
        }

        /* ---- Cards ---- */
        for (int s = 0; s < g_col_count[c]; s++) {
            int ci   = g_col_cards[c][s];
            card_t *card = &g_cards[ci];

            i32 cardx = cx + 6;
            i32 cardy = card_y(s);

            /* Skip if dragging this card */
            if (ci == g_drag_card) continue;

            /* Shadow */
            draw_shadow(pix, spx, bw, bh, cardx, cardy, CARD_W, CARD_H);

            /* Card fill */
            u32 card_col = CARD_COLORS[ci % NUM_CARD_COLORS];
            fill_rounded(pix, spx, bw, bh,
                         cardx, cardy, CARD_W, CARD_H,
                         card_col, 6);

            /* Selection ring */
            if (ci == g_sel_card) {
                draw_border(pix, spx, bw, bh,
                            cardx - 1, cardy - 1,
                            CARD_W + 2, CARD_H + 2,
                            CARD_SEL_BORDER);
                draw_border(pix, spx, bw, bh,
                            cardx - 2, cardy - 2,
                            CARD_W + 4, CARD_H + 4,
                            CARD_SEL_BORDER);
            }

            /* Card text */
            int show_cur = (ci == g_sel_card && g_editing && blink_on);
            draw_card_text(pix, spx, bw, bh,
                           cardx, cardy,
                           card->text, card->text_len,
                           CARD_TEXT_COL,
                           show_cur, g_cursor_pos);

            /* "x" delete button (top-right corner) */
            {
                i32 xx = cardx + CARD_W - 14;
                i32 xy = cardy + 3;
                int hover = pt_in(g_mouse_x, g_mouse_y, xx, xy, 11, 11);
                u32 xbg = hover ? 0xFFEBA0ACu : 0x88EBA0ACu;
                fill_rounded(pix, spx, bw, bh, xx, xy, 11, 11, xbg, 3);
                font_draw_char(pix, (int)spx, (int)bw, (int)bh,
                               xx + 2, xy - 2, 'x', CARD_TEXT_COL);
            }
        }

        /* ---- New-card entry widget ---- */
        if (g_adding_col == c) {
            i32 cardx = cx + 6;
            i32 cardy = card_y(g_col_count[c]);

            draw_shadow(pix, spx, bw, bh, cardx, cardy, CARD_W, CARD_H);
            fill_rounded(pix, spx, bw, bh,
                         cardx, cardy, CARD_W, CARD_H,
                         0xFFFFF5CCu, 6);
            draw_border(pix, spx, bw, bh, cardx, cardy, CARD_W, CARD_H,
                        CARD_SEL_BORDER);

            /* Prompt */
            font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                             cardx + CARD_PAD_X, cardy + CARD_PAD_Y,
                             "New card:", 0xFF888888u);

            /* Entry text */
            int max_ch = (CARD_W - 2 * CARD_PAD_X) / FONT_W;
            int disp_start = 0;
            if (g_add_buf_len > max_ch) disp_start = g_add_buf_len - max_ch;

            i32 ty = cardy + CARD_PAD_Y + FONT_H + 4;
            for (int i = disp_start; i < g_add_buf_len; i++) {
                i32 tx = cardx + CARD_PAD_X + (i - disp_start) * FONT_W;
                font_draw_char(pix, (int)spx, (int)bw, (int)bh,
                               tx, ty, g_add_buf[i], CARD_TEXT_COL);
            }
            /* Cursor */
            if (blink_on) {
                i32 tx = cardx + CARD_PAD_X + (g_add_buf_len - disp_start) * FONT_W;
                fill_rect(pix, spx, bw, bh, tx, ty, 2, FONT_H, 0xFF89B4FAu);
            }
        }
    }

    /* ---- Dragged card (floating) ---- */
    if (g_drag_card >= 0) {
        card_t *card = &g_cards[g_drag_card];
        i32 dx = g_drag_x - g_drag_ox;
        i32 dy = g_drag_y - g_drag_oy;

        draw_shadow(pix, spx, bw, bh, dx, dy, CARD_W, CARD_H);
        u32 card_col = CARD_COLORS[g_drag_card % NUM_CARD_COLORS];
        /* Lighten slightly for dragging */
        fill_rounded(pix, spx, bw, bh, dx, dy, CARD_W, CARD_H, card_col, 6);
        draw_border(pix, spx, bw, bh, dx - 1, dy - 1,
                    CARD_W + 2, CARD_H + 2, CARD_SEL_BORDER);

        draw_card_text(pix, spx, bw, bh,
                       dx, dy,
                       card->text, card->text_len,
                       CARD_TEXT_COL, 0, 0);

        /* Column drop target highlight */
        int hover_col = x_to_col(g_drag_x);
        if (hover_col >= 0) {
            draw_border(pix, spx, bw, bh,
                        col_x(hover_col), BOARD_TOP,
                        COL_W, BOARD_H, CARD_SEL_BORDER);
        }
    }

    /* ---- Status bar ---- */
    fill_rect(pix, spx, bw, bh, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);
    fill_rect(pix, spx, bw, bh, 0, WIN_H - STATUS_H, WIN_W, 1, COL_DIVIDER);

    {
        char sb[128];
        int_to_str((i64)g_card_count, sb);
        char msg[128];
        k_strlcpy(msg, " ", sizeof(msg));
        k_strlcat(msg, sb, sizeof(msg));
        k_strlcat(msg, " card(s) total  |  Click + to add  |  Click card to edit  |  X to delete", sizeof(msg));
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         0, WIN_H - STATUS_H + 3, msg, COL_TEXT_DIM);
    }
}

/* ================================================================
 * Input handling
 * ================================================================ */

/* Insert char into selected card at cursor position */
static void card_insert_char(int idx, char c)
{
    if (idx < 0 || idx >= g_card_count) return;
    card_t *card = &g_cards[idx];
    if (card->text_len >= CARD_TEXT_LEN - 1) return;
    if (g_cursor_pos > card->text_len) g_cursor_pos = card->text_len;

    k_memmove(&card->text[g_cursor_pos + 1],
              &card->text[g_cursor_pos],
              (u64)(card->text_len - g_cursor_pos));
    card->text[g_cursor_pos] = c;
    card->text_len++;
    card->text[card->text_len] = '\0';
    g_cursor_pos++;
    save_board();
}

static void card_backspace(int idx)
{
    if (idx < 0 || idx >= g_card_count) return;
    card_t *card = &g_cards[idx];
    if (g_cursor_pos <= 0) return;

    k_memmove(&card->text[g_cursor_pos - 1],
              &card->text[g_cursor_pos],
              (u64)(card->text_len - g_cursor_pos + 1));
    card->text_len--;
    g_cursor_pos--;
    save_board();
}

static void handle_key(int keycode, int pressed)
{
    /* Track modifiers */
    if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
        g_shift = pressed; return;
    }
    if (keycode == KEY_LEFTCTRL) {
        g_ctrl = pressed; return;
    }
    if (!pressed) return;

    /* ---- Adding a new card ---- */
    if (g_adding_col >= 0) {
        switch (keycode) {
        case KEY_ESC:
            g_adding_col   = -1;
            g_add_buf_len  = 0;
            g_add_buf[0]   = '\0';
            return;

        case KEY_ENTER:
            if (g_add_buf_len > 0) {
                int ni = add_card(g_adding_col, g_add_buf, g_add_buf_len);
                g_sel_card   = ni;
                g_editing    = 0;
                g_cursor_pos = 0;
            }
            g_adding_col  = -1;
            g_add_buf_len = 0;
            g_add_buf[0]  = '\0';
            return;

        case KEY_BACKSPACE:
            if (g_add_buf_len > 0) {
                g_add_buf_len--;
                g_add_buf[g_add_buf_len] = '\0';
            }
            return;

        default: {
            if (keycode >= 0 && keycode < 256 && g_add_buf_len < CARD_TEXT_LEN - 1) {
                const char *km = g_shift ? keymap_hi : keymap_low;
                char c = km[keycode];
                if (c >= 0x20 && c <= 0x7E) {
                    g_add_buf[g_add_buf_len++] = c;
                    g_add_buf[g_add_buf_len]   = '\0';
                }
            }
            return;
        }
        }
    }

    /* ---- Card selected ---- */
    if (g_sel_card >= 0) {
        /* Ctrl+C: copy text */
        if (g_ctrl && keycode == 46 /* 'c' */) {
            card_t *card = &g_cards[g_sel_card];
            k_strlcpy(g_clip_buf, card->text, sizeof(g_clip_buf));
            safe_path(g_clip_buf);
            sc(SYS_CLIP_SET, (i64)g_path_buf, (i64)k_strlen(g_path_buf), 0, 0, 0, 0);
            serial_print("[KANBAN] copied to clipboard\n");
            return;
        }
        /* Ctrl+V: paste into new card in same column */
        if (g_ctrl && keycode == 47 /* 'v' */) {
            k_memset(g_path_buf, 0, sizeof(g_path_buf));
            i64 r = sc(SYS_CLIP_GET, (i64)g_path_buf, sizeof(g_path_buf) - 1, 0, 0, 0, 0);
            if (r > 0) {
                int col = g_cards[g_sel_card].col;
                int ni = add_card(col, g_path_buf, (int)r);
                g_sel_card   = ni;
                g_editing    = 1;
                g_cursor_pos = (int)r;
            }
            return;
        }

        switch (keycode) {
        case KEY_ESC:
            g_sel_card   = -1;
            g_editing    = 0;
            g_cursor_pos = 0;
            return;

        case KEY_ENTER:
            g_editing = !g_editing;
            if (g_editing)
                g_cursor_pos = g_cards[g_sel_card].text_len;
            return;

        case KEY_DELETE:
            delete_card(g_sel_card);
            return;

        case KEY_BACKSPACE:
            if (g_editing) {
                card_backspace(g_sel_card);
            }
            return;

        case KEY_LEFT:
            if (g_editing) {
                if (g_cursor_pos > 0) g_cursor_pos--;
            } else {
                /* Move card left */
                int new_col = g_cards[g_sel_card].col - 1;
                if (new_col >= 0) {
                    move_card_col(g_sel_card, new_col);
                }
            }
            return;

        case KEY_RIGHT:
            if (g_editing) {
                if (g_cursor_pos < g_cards[g_sel_card].text_len)
                    g_cursor_pos++;
            } else {
                /* Move card right */
                int new_col = g_cards[g_sel_card].col + 1;
                if (new_col < NUM_COLS) {
                    move_card_col(g_sel_card, new_col);
                }
            }
            return;

        case KEY_UP:
            if (g_editing && g_cursor_pos > 0) g_cursor_pos--;
            return;

        case KEY_DOWN:
            if (g_editing &&
                g_cursor_pos < g_cards[g_sel_card].text_len)
                g_cursor_pos++;
            return;

        default: {
            if (g_editing && keycode >= 0 && keycode < 256) {
                const char *km = g_shift ? keymap_hi : keymap_low;
                char c = km[keycode];
                if (c >= 0x20 && c <= 0x7E) {
                    card_insert_char(g_sel_card, c);
                }
            }
            return;
        }
        }
    }
}

/* Hit-test a card in a column, returns global card index or -1 */
static int hit_card(int mx, int my, int *hit_x_btn)
{
    *hit_x_btn = 0;
    for (int c = 0; c < NUM_COLS; c++) {
        i32 cx = col_x(c);
        for (int s = 0; s < g_col_count[c]; s++) {
            int ci    = g_col_cards[c][s];
            i32 cardx = cx + 6;
            i32 cardy = card_y(s);
            if (pt_in(mx, my, cardx, cardy, CARD_W, CARD_H)) {
                /* Check "x" button */
                i32 xx = cardx + CARD_W - 14;
                i32 xy = cardy + 3;
                if (pt_in(mx, my, xx, xy, 11, 11))
                    *hit_x_btn = 1;
                return ci;
            }
        }
    }
    return -1;
}

static void handle_mouse_press(int mx, int my)
{
    /* ---- Check column "+" buttons ---- */
    for (int c = 0; c < NUM_COLS; c++) {
        i32 bx = col_x(c) + COL_W - 22;
        i32 by = BOARD_TOP + (COL_HDR_H - 18) / 2 + 1;
        if (pt_in(mx, my, bx, by, 18, 18)) {
            g_adding_col  = c;
            g_add_buf_len = 0;
            g_add_buf[0]  = '\0';
            g_sel_card    = -1;
            g_editing     = 0;
            return;
        }
    }

    /* ---- Cancel new-card entry if clicking elsewhere ---- */
    if (g_adding_col >= 0) {
        g_adding_col  = -1;
        g_add_buf_len = 0;
        g_add_buf[0]  = '\0';
    }

    /* ---- Hit test cards ---- */
    int hit_x = 0;
    int ci = hit_card(mx, my, &hit_x);

    if (ci >= 0) {
        if (hit_x) {
            /* Delete card */
            delete_card(ci);
            return;
        }

        if (ci == g_sel_card) {
            /* Second click on selected card: enter edit mode */
            g_editing    = 1;
            g_cursor_pos = g_cards[ci].text_len;
        } else {
            g_sel_card   = ci;
            g_editing    = 0;
            g_cursor_pos = 0;
        }

        /* Start drag */
        /* Find card's screen position */
        int c = g_cards[ci].col;
        i32 cardx = col_x(c) + 6;
        /* Find slot */
        int slot = 0;
        for (int s = 0; s < g_col_count[c]; s++) {
            if (g_col_cards[c][s] == ci) { slot = s; break; }
        }
        i32 cardy = card_y(slot);
        g_drag_card = ci;
        g_drag_ox   = mx - cardx;
        g_drag_oy   = my - cardy;
        g_drag_x    = mx;
        g_drag_y    = my;
    } else {
        /* Click on empty area -- deselect */
        g_sel_card  = -1;
        g_editing   = 0;
        g_drag_card = -1;
    }
}

static void handle_mouse_release(int mx, int my)
{
    if (g_drag_card >= 0) {
        /* Drop card into whatever column the mouse is over */
        int target_col = x_to_col(mx);
        if (target_col >= 0 && target_col != g_cards[g_drag_card].col) {
            move_card_col(g_drag_card, target_col);
            g_sel_card = g_drag_card;
        }
        g_drag_card = -1;
    }
}

/* ================================================================
 * Entry point
 * ================================================================ */
void _start(void)
{
    serial_print("[KANBAN] starting\n");

    /* Zero all statics */
    k_memset(g_cards,     0, sizeof(g_cards));
    k_memset(g_col_cards, 0, sizeof(g_col_cards));
    k_memset(g_col_count, 0, sizeof(g_col_count));
    k_memset(g_add_buf,   0, sizeof(g_add_buf));
    k_memset(g_clip_buf,  0, sizeof(g_clip_buf));
    k_memset(g_path_buf,  0, sizeof(g_path_buf));

    g_card_count = 0;
    g_sel_card   = -1;
    g_editing    = 0;
    g_cursor_pos = 0;
    g_adding_col = -1;
    g_add_buf_len = 0;
    g_drag_card  = -1;
    g_shift = 0;
    g_ctrl  = 0;
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_btn = 0;
    g_prev_btn  = 0;

    /* Load persisted board */
    load_board();

    /* Connect to compositor */
    if (wl_connect() != 0) {
        serial_print("[KANBAN] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Kanban");
    if (!win) {
        serial_print("[KANBAN] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    serial_print("[KANBAN] window created\n");

    for (;;) {
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                handle_key(a, b);
            } else if (kind == WL_EVENT_POINTER) {
                g_mouse_x = a;
                g_mouse_y = b;
                int cur_btn = c;

                /* Update drag position */
                if (g_drag_card >= 0 && cur_btn) {
                    g_drag_x = a;
                    g_drag_y = b;
                }

                /* Press transition */
                if (cur_btn && !g_prev_btn) {
                    handle_mouse_press(a, b);
                }
                /* Release transition */
                if (!cur_btn && g_prev_btn) {
                    handle_mouse_release(a, b);
                }

                g_prev_btn = cur_btn;
            }
        }

        u64 ticks = (u64)sc(SYS_GETTIME, 0, 0, 0, 0, 0, 0);
        render(win, ticks);
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
