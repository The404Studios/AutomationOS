/*
 * browser.c -- Minimal windowed HTTP/HTTPS web browser (freestanding, ring 3).
 * =============================================================================
 *
 * A GUI web browser for AutomationOS, built directly on the M3 Wayland-lite
 * client library + the 8x16 bitmap font (the same wl-direct pattern used by
 * editor.c and nettool.c -- NO libc, NO stdio, NO malloc, NO standard
 * headers, only inline syscalls + fixed/static buffers).
 *
 * Window layout (720x520):
 *   +--------------------------------------------------------------+
 *   | [Tab1] [Tab2] [Tab3]  ...  [+]                              |  tab bar
 *   +--------------------------------------------------------------+
 *   | [ address bar ............................. ]   [ Go ] [Back]|  chrome
 *   +--------------------------------------------------------------+
 *   | rendered page text (word-wrapped), scrollable                |
 *   | ...                                                          |
 *   | --- Links ---                                                |
 *   | [1] link text   ->  https://...                              |
 *   | ...                                                          |
 *   +--------------------------------------------------------------+
 *   | [https] / [http insecure]  status line: URL / bytes / hints  |
 *   +--------------------------------------------------------------+
 *
 * Networking (HTTP + HTTPS via TLS):
 *   #include "../../lib/net/http.h"  -> http_get(host, port, path, ...)
 *                                    -> https_get(host, port, path, ...)  TLS
 *   #include "../../lib/net/dns.h"   -> dns_resolve(hostname, &ip)
 * (http.c / dns.c / net.c / tls.c are built separately and linked in by the
 *  integrator; this TU only consumes their headers.)
 *
 * Navigation model:
 *   - Type a URL in the address bar and press Enter (or click "Go").
 *   - http://HOST[:PORT]/PATH   ; default port 80  ; bare host -> "/".
 *   - https://HOST[:PORT]/PATH  ; default port 443 ; TLS-encrypted fetch.
 *   - Links found in the page are numbered [1],[2],... and listed at the
 *     bottom.  To follow link N, type "N" or "link:N" in the address bar
 *     and press Enter.  Relative links inherit the current page's scheme.
 *   - "back" (or the Back button) returns to the previous URL (small ring).
 *
 * TABS:
 *   - Up to MAX_TABS (6) tabs; each has its own URL, buffers, scroll, history.
 *   - Ctrl+T = new tab;  Ctrl+W = close tab;  click a tab label = switch.
 *   - Tab bar shows a '+' button on the right to open a new tab.
 *
 * DOWNLOADS:
 *   - If the URL path ends in a known extension (.zip .tar .gz .pdf .iso
 *     .png .jpg .jpeg .bin .img) the fetched body is saved to
 *     /tmp/<basename> automatically (no JavaScript / no DOM needed).
 *   - Status bar shows a brief "Saved: /tmp/..." confirmation.
 *
 * BOOKMARKS (persistent):
 *   - Path defined by BM_PATH (default /tmp/bookmarks.txt).
 *   - Loaded at startup; saved immediately on add (b) or remove (none yet).
 *   - 'b' = bookmark current URL, 'B' = show bookmark list.
 *
 * Build (flags DIRECT on the command line -- NEVER via a shell variable; NO
 * stack canary -> objdump must show no fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/browser/browser.c -o browser.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       browser.o http.o https.o dns.o net.o tls.o wl_client.o bitfont.o \
 *       -o build/browser
 *   objdump -d build/browser | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/net/http.h"
#include "../../lib/net/dns.h"

/*
 * https_get -- TLS-encrypted HTTP GET (provided by tls.o / https.o, linked in
 * by the integrator; default port 443).
 */
extern long https_get(const char *host, unsigned short port, const char *path,
                      char *out_body, unsigned long out_cap, int *out_status);

/* ---- syscall numbers ---- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40
#define SYS_MKDIR         67

/* ---- open() flags / mode ---- */
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_CREAT   0x040
#define O_TRUNC   0x200
#define BM_MODE   0x1B6    /* 0666 */

/* ---- fixed-width types ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef signed int         i32;
typedef signed long        i64;

/* ---- raw 6-arg inline syscall ---- */
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
static u32 k_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void serial_puts(const char *m)
{
    sc(SYS_WRITE, 1, (i64)m, (i64)k_strlen(m), 0, 0, 0);
}
static void serial_num(i64 n)
{
    char b[24]; int i = 0;
    if (n < 0) { sc(SYS_WRITE, 1, (i64)"-", 1, 0, 0, 0); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (i64)&ch, 1, 0, 0, 0); }
}

/* ---- small freestanding helpers ---- */
static void k_memset(void *dst, u8 v, u64 n)
{
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = v;
}
static int  k_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int k_strncmp_ci(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (u8)ca - (u8)cb;
        if (!ca) return 0;
    }
    return 0;
}
/* copy src into dst[cap] (NUL-terminated); returns chars written */
static int k_strlcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}
/* append src to dst[cap] (NUL-terminated); returns total length */
static int k_strlcat(char *dst, const char *src, int cap)
{
    int i = 0;
    while (i < cap - 1 && dst[i]) i++;
    int j = 0;
    while (i < cap - 1 && src[j]) dst[i++] = src[j++];
    dst[i] = '\0';
    return i;
}
static int k_isdigit(char c) { return c >= '0' && c <= '9'; }
static long k_atoi(const char *s)
{
    long v = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (k_isdigit(*s)) { v = v * 10 + (*s - '0'); s++; }
    return v;
}
/* unsigned long -> decimal string in out[cap]; returns length (NUL-terminated) */
static int k_utoa(unsigned long v, char *out, int cap)
{
    char tmp[24]; int di = 0;
    do { tmp[di++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v > 0 && di < 24);
    int n = 0;
    while (di > 0 && n < cap - 1) out[n++] = tmp[--di];
    out[n] = 0;
    return n;
}

/* =========================================================================
 * Geometry / colours
 * ========================================================================= */
#define WIN_W      720              /* initial window width  (resizable)     */
#define WIN_H      520              /* initial window height (resizable)     */

#define TAB_BAR_H  20               /* tab strip at very top                 */
#define CHROME_H   28               /* address-bar toolbar height            */
#define TOOLBAR_H  (TAB_BAR_H + CHROME_H)  /* total chrome                  */
#define STATUS_H   18               /* bottom status bar height              */
#define CONTENT_X  6
#define CONTENT_Y  (TOOLBAR_H + 4)

/* ---- Width/height-derived geometry is RUNTIME state so the page reflows on
 * compositor resize (Maximize / snap). g_win_w / g_win_h cache the current
 * win->w / win->h; recompute_geometry() rederives everything below from them.
 * These identifiers are deliberately the same names the old macros used, so
 * every existing use site is unchanged. recompute_geometry(WIN_W, WIN_H) is
 * called once at startup so the values are valid before the first draw. */
static int g_win_w = WIN_W;         /* current window width  (pixels)        */
static int g_win_h = WIN_H;         /* current window height (pixels)        */
static int CONTENT_W;               /* content area width    (pixels)        */
static int CONTENT_BOT;             /* y where content ends / status begins  */
static int CONTENT_H;               /* content area height   (pixels)        */
static int VIS_ROWS;                /* visible text rows in the content area */
static int CONTENT_COLS;            /* max chars per rendered line (wrap)    */

/* address bar geometry (relative to TAB_BAR_H baseline) */
#define ADDR_X     6
#define ADDR_Y     (TAB_BAR_H + 4)
#define GO_W       40
#define GO_H       20
static int ADDR_W;                  /* address-bar width (stretches w/ window)*/
#define ADDR_H     20
static int BACK_X;                  /* Back button x (anchored to right edge) */
static int GO_X;                    /* Go button x (right of the address bar) */

/* tab bar geometry */
#define TAB_MAX_W  110              /* max label width in pixels             */
#define TAB_PLUS_W 20               /* '+' new-tab button width              */

/*
 * recompute_geometry -- rederive all width/height-dependent layout metrics
 * from a window size. Mirrors the original compile-time expressions exactly,
 * so passing (WIN_W, WIN_H) reproduces the default layout byte-for-byte.
 * Clamped so a tiny window can never produce a negative/zero content extent
 * (every drawn pixel is additionally clipped to win->w/h by fill_rect /
 * font_draw_*, so this is belt-and-suspenders against a degenerate size).
 */
static void recompute_geometry(int win_w, int win_h)
{
    if (win_w < 120) win_w = 120;   /* keep chrome buttons from colliding   */
    if (win_h < (TOOLBAR_H + STATUS_H + FONT_H))
        win_h = TOOLBAR_H + STATUS_H + FONT_H;
    g_win_w = win_w;
    g_win_h = win_h;

    CONTENT_W   = win_w - 2 * CONTENT_X;
    if (CONTENT_W < FONT_W) CONTENT_W = FONT_W;
    CONTENT_BOT = win_h - STATUS_H;
    CONTENT_H   = CONTENT_BOT - CONTENT_Y;
    if (CONTENT_H < FONT_H) CONTENT_H = FONT_H;
    VIS_ROWS    = CONTENT_H / FONT_H;
    if (VIS_ROWS < 1) VIS_ROWS = 1;
    CONTENT_COLS = CONTENT_W / FONT_W;
    if (CONTENT_COLS < 1) CONTENT_COLS = 1;

    ADDR_W = win_w - ADDR_X - GO_W - 12 - 56;
    if (ADDR_W < FONT_W) ADDR_W = FONT_W;
    BACK_X = win_w - 52;
    GO_X   = ADDR_X + ADDR_W + 6;
}

#define COL_BG          0xFF101418u
#define COL_CHROME      0xFF1B2230u
#define COL_TAB_ACTIVE  0xFF243040u
#define COL_TAB_INACTIVE 0xFF141820u
#define COL_TAB_BORDER  0xFF2A3340u
#define COL_ADDR_BG     0xFF0B0E12u
#define COL_ADDR_BG_F   0xFF13202Eu
#define COL_ADDR_BORDER 0xFF2A3340u
#define COL_BTN         0xFF2D6CDFu
#define COL_BTN_TXT     0xFFFFFFFFu
#define COL_TEXT        0xFFD6DCE4u
#define COL_DIM         0xFF8A93A0u
#define COL_LINK        0xFF5AB0FFu
#define COL_HEADING     0xFFFFD479u
#define COL_STATUS_BG   0xFF15324Fu
#define COL_STATUS_TXT  0xFFCBE4FFu
#define COL_ERR         0xFFFF6B6Bu
#define COL_CURSOR      0xFFD6DCE4u
#define COL_PRE         0xFF9FE0B0u
#define COL_PRE_BG      0xFF0C1A14u
#define COL_TABLE       0xFFC8D0DBu
#define COL_BULLET      0xFFFFB454u
#define COL_FIND_BG     0xFF7A5A12u
#define COL_HINT        0xFF7E8794u
#define COL_OVERLAY_BG  0xFF0A0D11u
#define COL_OVERLAY_BD  0xFF2D6CDFu
#define COL_SEC_HTTPS   0xFF4DC97Au
#define COL_SEC_HTTP    0xFFFF8C42u
#define COL_DL_OK       0xFF4DC97Au   /* download success indicator (green)  */

/* =========================================================================
 * Keycodes (evdev/Linux)
 * ========================================================================= */
#define KEY_ESC         1
#define KEY_BACKSPACE  14
#define KEY_ENTER      28
#define KEY_F1         59
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTCTRL   29
#define KEY_RIGHTCTRL  97
#define KEY_SPACE      57
#define KEY_HOME      102
#define KEY_UP        103
#define KEY_PAGEUP    104
#define KEY_LEFT      105
#define KEY_RIGHT     106
#define KEY_END       107
#define KEY_DOWN      108
#define KEY_PAGEDOWN  109

/* keycodes for t and w (used with Ctrl) */
#define KEY_T          20
#define KEY_W          17

static char keycode_to_ascii(int kc, int shift)
{
    switch (kc) {
        case 2:  return shift ? '!' : '1';
        case 3:  return shift ? '@' : '2';
        case 4:  return shift ? '#' : '3';
        case 5:  return shift ? '$' : '4';
        case 6:  return shift ? '%' : '5';
        case 7:  return shift ? '^' : '6';
        case 8:  return shift ? '&' : '7';
        case 9:  return shift ? '*' : '8';
        case 10: return shift ? '(' : '9';
        case 11: return shift ? ')' : '0';
        case 12: return shift ? '_' : '-';
        case 13: return shift ? '+' : '=';
        case 16: return shift ? 'Q' : 'q';
        case 17: return shift ? 'W' : 'w';
        case 18: return shift ? 'E' : 'e';
        case 19: return shift ? 'R' : 'r';
        case 20: return shift ? 'T' : 't';
        case 21: return shift ? 'Y' : 'y';
        case 22: return shift ? 'U' : 'u';
        case 23: return shift ? 'I' : 'i';
        case 24: return shift ? 'O' : 'o';
        case 25: return shift ? 'P' : 'p';
        case 26: return shift ? '{' : '[';
        case 27: return shift ? '}' : ']';
        case 30: return shift ? 'A' : 'a';
        case 31: return shift ? 'S' : 's';
        case 32: return shift ? 'D' : 'd';
        case 33: return shift ? 'F' : 'f';
        case 34: return shift ? 'G' : 'g';
        case 35: return shift ? 'H' : 'h';
        case 36: return shift ? 'J' : 'j';
        case 37: return shift ? 'K' : 'k';
        case 38: return shift ? 'L' : 'l';
        case 39: return shift ? ':' : ';';
        case 40: return shift ? '"' : '\'';
        case 41: return shift ? '~' : '`';
        case 43: return shift ? '|' : '\\';
        case 44: return shift ? 'Z' : 'z';
        case 45: return shift ? 'X' : 'x';
        case 46: return shift ? 'C' : 'c';
        case 47: return shift ? 'V' : 'v';
        case 48: return shift ? 'B' : 'b';
        case 49: return shift ? 'N' : 'n';
        case 50: return shift ? 'M' : 'm';
        case 51: return shift ? '<' : ',';
        case 52: return shift ? '>' : '.';
        case 53: return shift ? '?' : '/';
        case KEY_SPACE: return ' ';
        default: return 0;
    }
}

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */
static void fill_rect(u32 *pix, u32 stride_px, u32 bw, u32 bh,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = pix + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}
static void draw_border(u32 *pix, u32 stride_px, u32 bw, u32 bh,
                        i32 x, i32 y, i32 w, i32 h, u32 color)
{
    fill_rect(pix, stride_px, bw, bh, x, y, w, 1, color);
    fill_rect(pix, stride_px, bw, bh, x, y + h - 1, w, 1, color);
    fill_rect(pix, stride_px, bw, bh, x, y, 1, h, color);
    fill_rect(pix, stride_px, bw, bh, x + w - 1, y, 1, h, color);
}

/* =========================================================================
 * Buffers (all fixed-size static)
 * ========================================================================= */
#define HTML_CAP      262144          /* raw HTTP body buffer per tab        */
#define RENDER_CAP    131072          /* stripped/rendered text bytes per tab */
#define MAX_RLINES    4096            /* rendered display lines per tab       */
#define MAX_LINKS     256
#define URL_CAP       512
#define HOST_CAP      256
#define PATH_CAP      512
#define LINK_TXT_CAP  96
#define LINK_HREF_CAP 512

/* ---- per-tab structural markers ---- */
#define MK_HEADING  '\x10'
#define MK_LISTITEM '\x11'
#define MK_PRE      '\x12'
#define MK_TABLE    '\x13'

/* line kinds */
#define LK_TEXT     0
#define LK_LINKROW  1
#define LK_LHEADER  2
#define LK_HEADING  3
#define LK_LISTITEM 4
#define LK_PRE      5
#define LK_TABLE    6

typedef struct {
    int  off;
    int  len;
    int  kind;
    int  link_idx;
    int  attr;
} rline_t;

typedef struct {
    char text[LINK_TXT_CAP];
    char href[LINK_HREF_CAP];
} link_t;

/* =========================================================================
 * Tab data model
 * ========================================================================= */
#define MAX_TABS   6
#define HIST_MAX   16

typedef struct {
    /* page buffers */
    char      html[HTML_CAP];
    char      render[RENDER_CAP];
    int       render_len;
    rline_t   lines[MAX_RLINES];
    int       nlines;
    link_t    links[MAX_LINKS];
    int       nlinks;
    /* current location */
    char      cur_url[URL_CAP];
    char      cur_host[HOST_CAP];
    char      cur_path[PATH_CAP];
    int       cur_port;
    int       cur_https;
    int       page_tls;
    /* address bar */
    char      addr[URL_CAP];
    int       addr_len;
    /* scroll */
    int       scroll;
    /* per-tab status line text (so tab switching restores the right text) */
    char      status[160];
    /* per-tab history ring */
    char      hist[HIST_MAX][URL_CAP];
    int       hist_top;
    /* alive flag */
    int       alive;
} tab_t;

/* 6 tabs * (262144 + 131072 + ...) ≈ 2.4 MB BSS -- acceptable */
static tab_t g_tabs[MAX_TABS];
static int   g_ntabs;       /* number of open tabs (1..MAX_TABS)   */
static int   g_atab;        /* active tab index (0-based)          */

/* Convenience macros: operate on the active tab */
#define T          (&g_tabs[g_atab])
#define g_html     (T->html)
#define g_render   (T->render)
#define g_render_len (T->render_len)
#define g_nlines   (T->nlines)
#define g_lines    (T->lines)
#define g_links    (T->links)
#define g_nlinks   (T->nlinks)
#define g_cur_url  (T->cur_url)
#define g_cur_host (T->cur_host)
#define g_cur_path (T->cur_path)
#define g_cur_port (T->cur_port)
#define g_cur_https (T->cur_https)
#define g_page_tls (T->page_tls)
#define g_addr     (T->addr)
#define g_addr_len (T->addr_len)
#define g_scroll   (T->scroll)
#define g_hist     (T->hist)
#define g_hist_top (T->hist_top)

/* address-bar focus is shared (only one bar on screen) */
static int  g_addr_focus = 1;

/* status bar text is per-tab (lives in tab_t.status); aliased via the T macro */
#define g_status   (T->status)

/* ---- bookmarks (persisted to /tmp/bookmarks.txt) ---- */
#define BM_MAX        64
#define BM_PATH       "/tmp/bookmarks.txt"   /* swap this path to persist elsewhere */
#define BM_DIR        "/tmp"
#define BM_FILE_CAP   (BM_MAX * URL_CAP)
static char g_bm[BM_MAX][URL_CAP];
static int  g_nbm;
static char g_bm_io[BM_FILE_CAP];

/* ---- find-in-page ---- */
static int  g_find_active;
static char g_find_q[96];
static int  g_find_len;
static int  g_find_hits;
static int  g_find_line;

/* ---- transient mode flags ---- */
static int  g_show_help;

/* ---- ctrl key state ---- */
static int  g_ctrl;

/* ---- download status ---- */
static char g_dl_status[160];   /* last download result (shown in status bar) */
static int  g_dl_ticks;         /* countdown for how long to show it (frames)  */

/* =========================================================================
 * HTML entity decode
 * ========================================================================= */
static int decode_entity(const char **pp, char *out)
{
    const char *p = *pp;
    const char *q = p + 1;
    if (*q == '#') {
        q++;
        long val = 0;
        if (*q == 'x' || *q == 'X') {
            q++;
            while (*q && *q != ';') {
                char c = *q;
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                val = val * 16 + d;
                q++;
            }
        } else {
            while (k_isdigit(*q)) { val = val * 10 + (*q - '0'); q++; }
        }
        if (*q == ';') q++;
        if (val >= 0x20 && val <= 0x7E) { out[0] = (char)val; }
        else if (val == 0xA0) { out[0] = ' '; }
        else if (val == 0x2019 || val == 0x2018) { out[0] = '\''; }
        else if (val == 0x201C || val == 0x201D) { out[0] = '"'; }
        else if (val == 0x2013 || val == 0x2014) { out[0] = '-'; }
        else { out[0] = '?'; }
        *pp = q;
        return 1;
    }
    static const struct { const char *name; char ch; } ents[] = {
        { "amp;",   '&'  },
        { "lt;",    '<'  },
        { "gt;",    '>'  },
        { "quot;",  '"'  },
        { "apos;",  '\'' },
        { "nbsp;",  ' '  },
        { "#39;",   '\'' },
        { "copy;",  'c'  },
        { "reg;",   'r'  },
        { "mdash;", '-'  },
        { "ndash;", '-'  },
        { "hellip;",'.'  },
    };
    for (unsigned i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        int nlen = (int)k_strlen(ents[i].name);
        int ok = 1;
        for (int j = 0; j < nlen; j++) {
            if (q[j] != ents[i].name[j]) { ok = 0; break; }
        }
        if (ok) {
            out[0] = ents[i].ch;
            *pp = q + nlen;
            return 1;
        }
    }
    out[0] = '&';
    *pp = p + 1;
    return 1;
}

/* append one char to g_render */
static void render_putc(char c)
{
    if (g_render_len < RENDER_CAP - 1) g_render[g_render_len++] = c;
}
static void render_puts(const char *s)
{
    while (*s && g_render_len < RENDER_CAP - 1) g_render[g_render_len++] = *s++;
}

static int tag_is(const char *p, const char *name)
{
    int n = (int)k_strlen(name);
    if (k_strncmp_ci(p, name, n) != 0) return 0;
    char after = p[n];
    return after == ' ' || after == '>' || after == '/' ||
           after == '\t' || after == '\n' || after == '\r' || after == 0;
}

static int extract_href(const char *p, char *out, int cap)
{
    out[0] = 0;
    while (*p && *p != '>') {
        if ((p[0] == 'h' || p[0] == 'H') &&
            k_strncmp_ci(p, "href", 4) == 0) {
            const char *q = p + 4;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '=') { p++; continue; }
            q++;
            while (*q == ' ' || *q == '\t') q++;
            char quote = 0;
            if (*q == '"' || *q == '\'') { quote = *q; q++; }
            int n = 0;
            while (*q && *q != '>' && n < cap - 1) {
                if (quote && *q == quote) break;
                if (!quote && (*q == ' ' || *q == '\t')) break;
                out[n++] = *q++;
            }
            out[n] = 0;
            return n > 0;
        }
        p++;
    }
    return 0;
}

/* =========================================================================
 * HTML -> g_render (tag stripping + entity decode + whitespace collapse).
 * ========================================================================= */
static void anchor_putc(int in_anchor, int cur_link, char c)
{
    if (in_anchor && cur_link >= 0) {
        int L = (int)k_strlen(g_links[cur_link].text);
        if (L < LINK_TXT_CAP - 1) {
            g_links[cur_link].text[L]   = c;
            g_links[cur_link].text[L+1] = 0;
        }
    }
}

static void strip_html(const char *html, int html_len)
{
    g_render_len = 0;
    g_nlinks = 0;

    int last_was_space = 1;
    int last_was_nl = 1;
    int line_started = 0;
    int in_anchor = 0;
    int cur_link = -1;
    int in_pre = 0;
    int list_depth = 0;
    int in_cell = 0;
    int in_row = 0;
    int pending_h = 0;

    const char *p   = html;
    const char *end = html + html_len;

    #define EMIT_SPACE() do { \
        if (!last_was_space) { render_putc(' '); last_was_space = 1; } \
    } while (0)
    #define EMIT_NL() do { \
        if (!last_was_nl) { render_putc('\n'); last_was_nl = 1; last_was_space = 1; \
                            line_started = 0; } \
    } while (0)
    #define EMIT_HARD_NL() do { \
        render_putc('\n'); last_was_nl = 1; last_was_space = 1; line_started = 0; \
    } while (0)
    #define START_MARKER(m) do { \
        EMIT_NL(); render_putc((char)(m)); last_was_nl = 0; last_was_space = 1; \
        line_started = 1; \
    } while (0)

    while (p < end && *p) {
        if (*p == '<') {
            const char *t = p + 1;

            if (t[0] == '!' && t[1] == '-' && t[2] == '-') {
                p = t + 3;
                while (p < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++;
                if (p < end) p += 3;
                continue;
            }
            /* <![CDATA[...]]>  -- emit the inner text verbatim (RSS/Atom feeds
             * wrap titles/descriptions in CDATA so HTML inside isn't escaped).
             * Bounded by the closing ]]> sequence. Lookahead is bounded so an
             * unterminated CDATA at EOF cannot read past `end`. */
            if (t + 8 <= end &&
                t[0] == '!' && t[1] == '[' &&
                (t[2] == 'C' || t[2] == 'c') &&
                (t[3] == 'D' || t[3] == 'd') &&
                (t[4] == 'A' || t[4] == 'a') &&
                (t[5] == 'T' || t[5] == 't') &&
                (t[6] == 'A' || t[6] == 'a') && t[7] == '[') {
                p = t + 8;
                while (p + 2 < end && !(p[0] == ']' && p[1] == ']' && p[2] == '>')) {
                    char c = *p++;
                    if (c == '\r') continue;
                    if (c == '\n' || c == '\t') {
                        if (!last_was_space) { render_putc(' '); last_was_space = 1; }
                    } else if (c >= 0x20 && c <= 0x7E) {
                        render_putc(c);
                        last_was_space = 0; last_was_nl = 0; line_started = 1;
                        anchor_putc(in_anchor, cur_link, c);
                    }
                }
                /* skip past ]]> if present; otherwise we're at EOF */
                if (p + 2 < end && p[0] == ']' && p[1] == ']' && p[2] == '>')
                    p += 3;
                else
                    p = end;
                continue;
            }
            if (t[0] == '!') {
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }
            /* XML processing instruction <? ... ?> (RSS/Atom feeds start with
             * <?xml version="1.0"?>) -- swallow it whole. Bounded lookahead. */
            if (t[0] == '?') {
                p = t + 1;
                while (p + 1 < end && !(p[0] == '?' && p[1] == '>')) p++;
                if (p + 1 < end && p[0] == '?' && p[1] == '>') p += 2;
                else p = end;
                continue;
            }

            int closing = 0;
            if (t[0] == '/') { closing = 1; t++; }

            if (!closing && (tag_is(t, "script") || tag_is(t, "style"))) {
                int is_script = tag_is(t, "script");
                const char *closetag = is_script ? "/script" : "/style";
                int clen = (int)k_strlen(closetag);
                p = t;
                while (p < end) {
                    if (*p == '<' && k_strncmp_ci(p + 1, closetag, clen) == 0) {
                        while (p < end && *p != '>') p++;
                        if (p < end) p++;
                        break;
                    }
                    p++;
                }
                continue;
            }

            if (tag_is(t, "pre")) {
                if (!closing) { EMIT_NL(); in_pre = 1; START_MARKER(MK_PRE); }
                else          { in_pre = 0; EMIT_HARD_NL(); }
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            if ((t[0] == 'h' || t[0] == 'H') && t[1] >= '1' && t[1] <= '6' &&
                (t[2] == '>' || t[2] == ' ' || t[2] == '/' || t[2] == '\t' ||
                 t[2] == '\n' || t[2] == '\r')) {
                if (!closing) {
                    pending_h = t[1] - '0';
                    START_MARKER(MK_HEADING);
                    render_putc((char)('0' + pending_h));
                } else {
                    pending_h = 0;
                    EMIT_NL();
                }
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            if (tag_is(t, "ul") || tag_is(t, "ol")) {
                if (!closing) { if (list_depth < 8) list_depth++; }
                else          { if (list_depth > 0) list_depth--; }
                EMIT_NL();
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }
            if (!closing && tag_is(t, "li")) {
                int d = list_depth > 0 ? list_depth : 1;
                if (d > 9) d = 9;
                START_MARKER(MK_LISTITEM);
                render_putc((char)('0' + d));
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            if (tag_is(t, "tr")) {
                if (!closing) { in_row = 1; in_cell = 0; START_MARKER(MK_TABLE); }
                else          { in_row = 0; in_cell = 0; EMIT_HARD_NL(); }
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }
            if (tag_is(t, "td") || tag_is(t, "th")) {
                if (!closing) {
                    if (in_row && in_cell) { render_putc('\t'); last_was_space = 1; }
                    in_cell = 1;
                }
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            if (tag_is(t, "br")    || tag_is(t, "p")   || tag_is(t, "div")  ||
                tag_is(t, "table") || tag_is(t, "section") ||
                tag_is(t, "header")|| tag_is(t, "footer")|| tag_is(t, "article") ||
                tag_is(t, "blockquote") || tag_is(t, "hr")) {
                EMIT_NL();
            }

            /* ---- RSS/Atom feed handling ----
             * <item>/<entry>     -> blank-line separator between feed items
             * <title>            -> emit as a heading (level 3)
             * <description>      -> blank-line before, plain text after
             * <link href="...">  -> Atom self-closing link, treat like <a href>
             * <link>URL</link>   -> RSS plain-text URL, capture as link
             */
            if (tag_is(t, "item") || tag_is(t, "entry")) {
                EMIT_NL();
                if (!closing) {
                    /* visible separator between feed items */
                    EMIT_NL();
                    render_puts("------------------------------------------");
                    EMIT_NL();
                }
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }
            if (tag_is(t, "title")) {
                if (!closing) {
                    START_MARKER(MK_HEADING);
                    render_putc('3');     /* h3-ish: feed item title */
                } else {
                    EMIT_NL();
                }
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }
            if (tag_is(t, "description") || tag_is(t, "summary") ||
                tag_is(t, "content") || tag_is(t, "subtitle")) {
                EMIT_NL();
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }
            if (tag_is(t, "link")) {
                /* Atom: <link href="..." rel="..." /> -- self-closing, has href */
                char lhref[LINK_HREF_CAP];
                int has_href = extract_href(t + 1, lhref, LINK_HREF_CAP);
                if (!closing && has_href && lhref[0] && g_nlinks < MAX_LINKS) {
                    /* register as a regular link; text = the href itself */
                    cur_link = g_nlinks;
                    k_strlcpy(g_links[cur_link].href, lhref, LINK_HREF_CAP);
                    k_strlcpy(g_links[cur_link].text, lhref, LINK_TXT_CAP);
                    g_nlinks++;
                    /* emit "[N] href" inline so it's clickable from the list */
                    char mk[16];
                    int mn = 0; mk[mn++] = ' '; mk[mn++] = '[';
                    int num = cur_link + 1;
                    char dg[8]; int di = 0;
                    do { dg[di++] = (char)('0' + num % 10); num /= 10; } while (num > 0);
                    while (di > 0) mk[mn++] = dg[--di];
                    mk[mn++] = ']'; mk[mn] = 0;
                    render_puts(mk);
                    last_was_space = 0; last_was_nl = 0; line_started = 1;
                    cur_link = -1;       /* not an open anchor; just registered */
                    in_anchor = 0;
                    while (p < end && *p != '>') p++;
                    if (p < end) p++;
                    continue;
                }
                /* RSS: <link>https://...</link> -- treat the text content as
                 * an anchor whose href will be filled from the inner text. */
                if (!closing && !has_href && g_nlinks < MAX_LINKS) {
                    cur_link = g_nlinks;
                    g_links[cur_link].href[0] = 0;
                    g_links[cur_link].text[0] = 0;
                    in_anchor = 1;
                    g_nlinks++;
                    while (p < end && *p != '>') p++;
                    if (p < end) p++;
                    continue;
                }
                if (closing && in_anchor && cur_link >= 0) {
                    /* The text we captured into .text IS the href for RSS */
                    if (!g_links[cur_link].href[0]) {
                        k_strlcpy(g_links[cur_link].href,
                                  g_links[cur_link].text, LINK_HREF_CAP);
                    }
                    /* emit "[N]" reference */
                    char mk[12];
                    int mn = 0;
                    mk[mn++] = '[';
                    int num = cur_link + 1;
                    char dg[8]; int di = 0;
                    do { dg[di++] = (char)('0' + num % 10); num /= 10; } while (num > 0);
                    while (di > 0) mk[mn++] = dg[--di];
                    mk[mn++] = ']'; mk[mn] = 0;
                    render_puts(mk);
                    last_was_space = 0; last_was_nl = 0; line_started = 1;
                    in_anchor = 0;
                    cur_link = -1;
                }
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            if (!closing && tag_is(t, "a")) {
                if (g_nlinks < MAX_LINKS) {
                    char href[LINK_HREF_CAP];
                    if (extract_href(t + 1, href, LINK_HREF_CAP) && href[0]) {
                        if (k_strncmp_ci(href, "javascript:", 11) != 0 &&
                            href[0] != '#') {
                            cur_link = g_nlinks;
                            k_strlcpy(g_links[cur_link].href, href, LINK_HREF_CAP);
                            g_links[cur_link].text[0] = 0;
                            in_anchor = 1;
                            g_nlinks++;
                        }
                    }
                }
            }
            if (closing && tag_is(t, "a")) {
                if (in_anchor && cur_link >= 0) {
                    char mk[12];
                    int mn = 0;
                    mk[mn++] = '[';
                    int num = cur_link + 1;
                    char dg[8]; int di = 0;
                    do { dg[di++] = (char)('0' + num % 10); num /= 10; } while (num > 0);
                    while (di > 0) mk[mn++] = dg[--di];
                    mk[mn++] = ']'; mk[mn] = 0;
                    render_puts(mk);
                    last_was_space = 0; last_was_nl = 0; line_started = 1;
                }
                in_anchor = 0;
                cur_link = -1;
            }

            while (p < end && *p != '>') p++;
            if (p < end) p++;
            continue;
        }

        if (*p == '&') {
            char dec[4];
            const char *pp = p;
            int n = decode_entity(&pp, dec);
            p = pp;
            for (int i = 0; i < n; i++) {
                char c = dec[i];
                if (c == ' ') { EMIT_SPACE(); }
                else { render_putc(c); last_was_space = 0; last_was_nl = 0;
                       line_started = 1;
                       anchor_putc(in_anchor, cur_link, c);
                }
            }
            continue;
        }

        if (in_pre) {
            char c = *p++;
            if (c == '\r') continue;
            if (c == '\n') { EMIT_HARD_NL(); render_putc(MK_PRE); last_was_nl = 0;
                             last_was_space = 1; continue; }
            if (c == '\t') { render_putc(' '); render_putc(' '); continue; }
            if (c >= 0x20 && c <= 0x7E) {
                render_putc(c); last_was_space = 0; last_was_nl = 0; line_started = 1;
                anchor_putc(in_anchor, cur_link, c);
            }
            continue;
        }

        char c = *p++;
        if (c == ' ' || c == '\t') {
            EMIT_SPACE();
        } else if (c == '\n' || c == '\r') {
            EMIT_SPACE();
        } else if (c >= 0x20 && c <= 0x7E) {
            render_putc(c);
            last_was_space = 0; last_was_nl = 0; line_started = 1;
            anchor_putc(in_anchor, cur_link, c);
        }
    }

    g_render[g_render_len] = 0;
    (void)line_started; (void)pending_h; (void)in_cell;
    #undef EMIT_SPACE
    #undef EMIT_NL
    #undef EMIT_HARD_NL
    #undef START_MARKER
}

/* =========================================================================
 * Word-wrap g_render into g_lines[]
 * ========================================================================= */
static void add_line(int off, int len, int kind, int link_idx, int attr)
{
    if (g_nlines >= MAX_RLINES) return;
    g_lines[g_nlines].off      = off;
    g_lines[g_nlines].len      = len;
    g_lines[g_nlines].kind     = kind;
    g_lines[g_nlines].link_idx = link_idx;
    g_lines[g_nlines].attr     = attr;
    g_nlines++;
}

static void layout_render(void)
{
    g_nlines = 0;
    g_find_hits = 0;
    g_find_line = -1;
    int width = CONTENT_COLS;
    if (width < 8) width = 8;

    int i = 0;
    while (i < g_render_len) {
        int line_start = i;
        int line_end = i;
        while (line_end < g_render_len && g_render[line_end] != '\n') line_end++;

        int kind = LK_TEXT;
        int attr = 0;
        int wrap = 1;
        if (line_start < line_end) {
            char m = g_render[line_start];
            if (m == MK_HEADING && line_start + 1 < line_end) {
                kind = LK_HEADING;
                attr = g_render[line_start + 1] - '0';
                if (attr < 1) attr = 1; if (attr > 6) attr = 6;
                line_start += 2;
            } else if (m == MK_LISTITEM && line_start + 1 < line_end) {
                kind = LK_LISTITEM;
                attr = g_render[line_start + 1] - '0';
                if (attr < 1) attr = 1; if (attr > 9) attr = 9;
                line_start += 2;
            } else if (m == MK_PRE) {
                kind = LK_PRE; wrap = 0;
                line_start += 1;
            } else if (m == MK_TABLE) {
                kind = LK_TABLE; wrap = 0;
                line_start += 1;
            }
        }

        if (line_end <= line_start) {
            add_line(line_start, 0, kind, -1, attr);
        } else if (!wrap) {
            add_line(line_start, line_end - line_start, kind, -1, attr);
        } else {
            int avail = width;
            if (kind == LK_LISTITEM) avail = width - (2 + 2 * attr);
            if (avail < 8) avail = 8;
            int seg = line_start;
            int first = 1;
            while (seg < line_end) {
                int remain = line_end - seg;
                int w = first ? avail : width;
                if (remain <= w) {
                    add_line(seg, remain, kind, -1, first ? attr : 0);
                    seg = line_end;
                } else {
                    int brk = seg + w;
                    int sp = brk;
                    while (sp > seg && g_render[sp] != ' ') sp--;
                    if (sp <= seg) sp = brk;
                    add_line(seg, sp - seg, first ? kind : LK_TEXT, -1,
                             first ? attr : 0);
                    seg = sp;
                    while (seg < line_end && g_render[seg] == ' ') seg++;
                }
                first = 0;
            }
        }
        i = line_end;
        if (i < g_render_len && g_render[i] == '\n') i++;
    }

    if (g_nlinks > 0) {
        add_line(0, 0, LK_TEXT, -1, 0);
        add_line(-1, 0, LK_LHEADER, -1, 0);
        for (int k = 0; k < g_nlinks; k++) {
            add_line(0, 0, LK_LINKROW, k, 0);
        }
    }
}

/* =========================================================================
 * URL parsing / resolution
 * ========================================================================= */
static int parse_url(const char *url, char *host, int host_cap,
                     int *port, char *path, int path_cap, int *is_https)
{
    host[0] = 0; path[0] = 0; *port = 80; *is_https = 0;
    const char *p = url;

    if (k_strncmp_ci(p, "http://", 7) == 0) {
        p += 7;
    } else if (k_strncmp_ci(p, "https://", 8) == 0) {
        p += 8; *is_https = 1; *port = 443;
    }

    int hn = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':' &&
           hn < host_cap - 1) {
        host[hn++] = *p++;
    }
    host[hn] = 0;
    if (hn == 0) return 0;

    if (*p == ':') {
        p++;
        long pv = 0;
        while (k_isdigit(*p)) { pv = pv * 10 + (*p - '0'); p++; }
        if (pv > 0 && pv <= 65535) *port = (int)pv;
    }

    if (*p == '/' || *p == '?' || *p == '#') {
        if (*p == '/') {
            k_strlcpy(path, p, path_cap);
        } else {
            path[0] = '/';
            k_strlcpy(path + 1, p, path_cap - 1);
        }
    } else {
        k_strlcpy(path, "/", path_cap);
    }
    for (int j = 0; path[j]; j++) {
        if (path[j] == '#') { path[j] = 0; break; }
    }
    if (path[0] == 0) k_strlcpy(path, "/", path_cap);
    return 1;
}

static void resolve_url(const char *href, char *out, int cap)
{
    out[0] = 0;
    if (k_strncmp_ci(href, "http://", 7) == 0 ||
        k_strncmp_ci(href, "https://", 8) == 0) {
        k_strlcpy(out, href, cap);
        return;
    }
    if (href[0] == '/' && href[1] == '/') {
        k_strlcpy(out, g_cur_https ? "https:" : "http:", cap);
        k_strlcat(out, href, cap);
        return;
    }
    k_strlcpy(out, g_cur_https ? "https://" : "http://", cap);
    k_strlcat(out, g_cur_host, cap);
    if ((g_cur_https && g_cur_port != 443) ||
        (!g_cur_https && g_cur_port != 80)) {
        char pb[8]; int n = 0; int pv = g_cur_port;
        char dg[8]; int di = 0;
        do { dg[di++] = (char)('0' + pv % 10); pv /= 10; } while (pv > 0);
        pb[n++] = ':';
        while (di > 0) pb[n++] = dg[--di];
        pb[n] = 0;
        k_strlcat(out, pb, cap);
    }

    if (href[0] == '/') {
        k_strlcat(out, href, cap);
    } else {
        char base[PATH_CAP];
        k_strlcpy(base, g_cur_path, PATH_CAP);
        int slash = -1;
        for (int j = 0; base[j]; j++) if (base[j] == '/') slash = j;
        if (slash < 0) { base[0] = '/'; base[1] = 0; }
        else base[slash + 1] = 0;
        k_strlcat(out, base, cap);
        k_strlcat(out, href, cap);
    }
}

/* =========================================================================
 * Download helper
 * ========================================================================= */

/* Return 1 if the path has a downloadable file extension */
static int path_is_download(const char *path)
{
    /* find the last '/' then check extension of the basename */
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
    /* find last '.' in the basename */
    const char *dot = (void*)0;
    for (const char *q = base; *q; q++) if (*q == '.') dot = q;
    if (!dot) return 0;
    static const char *exts[] = {
        ".zip", ".tar", ".gz", ".bz2", ".xz", ".pdf",
        ".iso", ".img", ".bin",
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp",
        (void*)0
    };
    for (int i = 0; exts[i]; i++) {
        int n = (int)k_strlen(exts[i]);
        if (k_strncmp_ci(dot, exts[i], n) == 0 && (dot[n] == 0 || dot[n] == '?'))
            return 1;
    }
    return 0;
}

/* Save body[0..blen) to /tmp/<basename-of-path>. Writes g_dl_status. */
static void download_save(const char *path, const char *body, int blen)
{
    /* extract basename */
    const char *base = path;
    for (const char *q = path; *q && *q != '?'; q++) if (*q == '/') base = q + 1;

    /* strip query string from base */
    char bname[128];
    int bi = 0;
    while (*base && *base != '?' && bi < 127) bname[bi++] = *base++;
    bname[bi] = 0;
    if (bi == 0) { k_strlcpy(bname, "download", sizeof(bname)); }

    /* build /tmp/<basename> */
    char dpath[160];
    k_strlcpy(dpath, "/tmp/", sizeof(dpath));
    k_strlcat(dpath, bname, sizeof(dpath));

    sc(SYS_MKDIR, (i64)"/tmp", BM_MODE, 0, 0, 0, 0);
    i64 fd = sc(SYS_OPEN, (i64)dpath,
                O_WRONLY | O_CREAT | O_TRUNC, BM_MODE, 0, 0, 0);
    if (fd < 0) {
        k_strlcpy(g_dl_status, "Download FAILED (open): ", sizeof(g_dl_status));
        k_strlcat(g_dl_status, dpath, sizeof(g_dl_status));
        g_dl_ticks = 180;
        return;
    }

    int off = 0;
    for (int guard = 0; guard < 4096 && off < blen; guard++) {
        i64 w = sc(SYS_WRITE, fd, (i64)(body + off), (i64)(blen - off), 0, 0, 0);
        if (w <= 0) break;
        off += (int)w;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

    /* report */
    k_strlcpy(g_dl_status, "Saved: ", sizeof(g_dl_status));
    k_strlcat(g_dl_status, dpath, sizeof(g_dl_status));
    k_strlcat(g_dl_status, " (", sizeof(g_dl_status));
    {
        char nb[24]; k_utoa((unsigned long)off, nb, sizeof(nb));
        k_strlcat(g_dl_status, nb, sizeof(g_dl_status));
    }
    k_strlcat(g_dl_status, " bytes)", sizeof(g_dl_status));
    g_dl_ticks = 300;   /* show for ~300 frames */

    serial_puts("[BROWSER] downloaded ");
    serial_puts(dpath);
    serial_puts(" bytes=");
    serial_num(off);
    serial_puts("\n");
}

/* =========================================================================
 * Page builders
 * ========================================================================= */
static void set_simple_page(const char *title, const char *body)
{
    g_render_len = 0;
    g_nlinks = 0;
    render_puts(body);
    g_render[g_render_len] = 0;
    layout_render();
    g_scroll = 0;
    k_strlcpy(g_status, title, sizeof(g_status));
    g_page_tls = 0;
}

static const char *WELCOME_PAGE =
    "AutomationOS Web Browser\n"
    "========================\n"
    "\n"
    "A tiny HTTP + HTTPS browser for a from-scratch operating system.\n"
    "\n"
    "HOW TO USE\n"
    "  - Click the address bar (or it is focused at launch) and type a URL.\n"
    "  - Press Enter or click [Go] to navigate.\n"
    "  - Examples:\n"
    "      http://example.com\n"
    "      https://example.com\n"
    "      http://example.com:80/index.html\n"
    "      https://example.com:443/index.html\n"
    "      example.com            (http:// and / are assumed)\n"
    "\n"
    "TABS\n"
    "  - Ctrl+T     open a new tab (up to 6)\n"
    "  - Ctrl+W     close the current tab\n"
    "  - Click a tab label in the tab bar to switch\n"
    "  - Each tab keeps its own URL, page, scroll position, and history.\n"
    "  - Status bar shows active tab number: e.g. [T2/4]\n"
    "\n"
    "FOLLOWING LINKS\n"
    "  - Links in a page are numbered [1], [2], ... and listed at the bottom.\n"
    "  - To follow link N, type its number (e.g. 3) or 'link:3' in the\n"
    "    address bar and press Enter.\n"
    "\n"
    "NAVIGATION\n"
    "  - Type 'back' (or click [Back]) to return to the previous page.\n"
    "  - Scroll: Up/Down arrows, PageUp/PageDown, Home/End, or mouse wheel.\n"
    "\n"
    "DOWNLOADS\n"
    "  - Navigating to a URL ending in .zip .tar .gz .pdf .iso .png .jpg\n"
    "    etc. automatically saves the body to /tmp/<filename>.\n"
    "  - The status bar shows 'Saved: /tmp/...' on completion.\n"
    "\n"
    "HTTPS / SECURITY\n"
    "  - Both http:// and https:// are supported.\n"
    "  - HTTPS pages are fetched over an encrypted TLS connection.\n"
    "  - The status bar shows the security level:\n"
    "      [https] (encrypted)  -- TLS connection (green badge)\n"
    "      [http insecure]      -- plaintext connection (orange badge)\n"
    "\n"
    "BOOKMARKS\n"
    "  - b  bookmark the current URL (saved to /tmp/bookmarks.txt)\n"
    "  - B  show saved bookmarks as a navigable link list\n"
    "  - Bookmarks are loaded at startup and saved on each add.\n"
    "\n"
    "OTHER KEYS (page focused)\n"
    "  - /        find-in-page (Enter=next match, Esc=cancel)\n"
    "  - n        jump to next find match\n"
    "  - F1 / ?   toggle help overlay\n"
    "\n"
    "Type a URL above to begin.\n";

static void show_welcome(void)
{
    set_simple_page("Welcome  |  http + https supported", WELCOME_PAGE);
    k_strlcpy(g_cur_url, "about:welcome", URL_CAP);
    g_cur_host[0] = 0;
    k_strlcpy(g_cur_path, "/", PATH_CAP);
    g_cur_port = 80; g_cur_https = 0;
    g_page_tls = 0;
}

/* =========================================================================
 * History (per-tab)
 * ========================================================================= */
static void hist_push(const char *url)
{
    if (!url || !url[0]) return;
    if (g_hist_top > 0 && k_streq(g_hist[(g_hist_top - 1) % HIST_MAX], url))
        return;
    k_strlcpy(g_hist[g_hist_top % HIST_MAX], url, URL_CAP);
    g_hist_top++;
}

/* forward decl */
static void clamp_scroll(void);

/* =========================================================================
 * Bookmarks (persisted to BM_PATH, one URL per line)
 * ========================================================================= */
static void bm_parse(int len)
{
    g_nbm = 0;
    int i = 0;
    while (i < len && g_nbm < BM_MAX) {
        int s = i;
        while (i < len && g_bm_io[i] != '\n' && g_bm_io[i] != '\r') i++;
        int e = i;
        while (i < len && (g_bm_io[i] == '\n' || g_bm_io[i] == '\r')) i++;
        if (e > s) {
            int n = e - s; if (n > URL_CAP - 1) n = URL_CAP - 1;
            for (int j = 0; j < n; j++) g_bm[g_nbm][j] = g_bm_io[s + j];
            g_bm[g_nbm][n] = 0;
            g_nbm++;
        }
    }
}

static void bm_load(void)
{
    g_nbm = 0;
    i64 fd = sc(SYS_OPEN, (i64)BM_PATH, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return;
    int total = 0;
    for (int guard = 0; guard < BM_MAX + 4 && total < BM_FILE_CAP; guard++) {
        i64 r = sc(SYS_READ, fd, (i64)(g_bm_io + total),
                   (i64)(BM_FILE_CAP - total), 0, 0, 0);
        if (r <= 0) break;
        total += (int)r;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    bm_parse(total);
}

static void bm_save(void)
{
    sc(SYS_MKDIR, (i64)BM_DIR, BM_MODE, 0, 0, 0, 0);
    i64 fd = sc(SYS_OPEN, (i64)BM_PATH, O_WRONLY | O_CREAT | O_TRUNC, BM_MODE, 0, 0, 0);
    if (fd < 0) {
        k_strlcpy(g_status, "Bookmark save failed: cannot write " BM_PATH, sizeof(g_status));
        return;
    }
    int len = 0;
    for (int k = 0; k < g_nbm && len < BM_FILE_CAP - 2; k++) {
        const char *u = g_bm[k];
        for (int j = 0; u[j] && len < BM_FILE_CAP - 2; j++) g_bm_io[len++] = u[j];
        g_bm_io[len++] = '\n';
    }
    int off = 0;
    for (int guard = 0; guard < BM_MAX + 4 && off < len; guard++) {
        i64 w = sc(SYS_WRITE, fd, (i64)(g_bm_io + off), (i64)(len - off), 0, 0, 0);
        if (w <= 0) break;
        off += (int)w;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

static void bm_add_current(void)
{
    if (!g_cur_url[0] || k_streq(g_cur_url, "about:welcome") ||
        k_streq(g_cur_url, "about:bookmarks")) {
        k_strlcpy(g_status, "Nothing to bookmark", sizeof(g_status));
        return;
    }
    for (int k = 0; k < g_nbm; k++) {
        if (k_streq(g_bm[k], g_cur_url)) {
            k_strlcpy(g_status, "Already bookmarked", sizeof(g_status));
            return;
        }
    }
    if (g_nbm >= BM_MAX) {
        k_strlcpy(g_status, "Bookmarks full", sizeof(g_status));
        return;
    }
    k_strlcpy(g_bm[g_nbm], g_cur_url, URL_CAP);
    g_nbm++;
    bm_save();   /* persist immediately */
    k_strlcpy(g_status, "Bookmarked: ", sizeof(g_status));
    k_strlcat(g_status, g_cur_url, sizeof(g_status));
}

static void show_bookmarks(void)
{
    g_render_len = 0;
    g_nlinks = 0;
    render_puts("Bookmarks\n");
    render_puts("=========\n\n");
    if (g_nbm == 0) {
        render_puts("No bookmarks yet.\n\nPress 'b' on any page to save it here.\n");
    } else {
        render_puts("Saved pages (pick a numbered link below):\n");
    }
    g_render[g_render_len] = 0;

    for (int k = 0; k < g_nbm && k < MAX_LINKS; k++) {
        k_strlcpy(g_links[k].href, g_bm[k], LINK_HREF_CAP);
        k_strlcpy(g_links[k].text, g_bm[k], LINK_TXT_CAP);
        g_nlinks++;
    }

    layout_render();
    g_scroll = 0;
    k_strlcpy(g_cur_url, "about:bookmarks", URL_CAP);
    g_cur_host[0] = 0;
    k_strlcpy(g_cur_path, "/", PATH_CAP);
    g_cur_port = 80; g_cur_https = 0;
    g_page_tls = 0;

    k_strlcpy(g_status, "Bookmarks (", sizeof(g_status));
    {
        char nb[16]; k_utoa((unsigned long)g_nbm, nb, sizeof(nb));
        k_strlcat(g_status, nb, sizeof(g_status));
    }
    k_strlcat(g_status, " saved)", sizeof(g_status));
}

/* =========================================================================
 * Tab management
 * ========================================================================= */
static void tab_init(int idx)
{
    tab_t *t = &g_tabs[idx];
    k_memset(t->html,     0, HTML_CAP);
    k_memset(t->render,   0, RENDER_CAP);
    k_memset(t->lines,    0, sizeof(t->lines));
    k_memset(t->links,    0, sizeof(t->links));
    k_memset(t->hist,     0, sizeof(t->hist));
    t->render_len = 0;
    t->nlines     = 0;
    t->nlinks     = 0;
    t->scroll     = 0;
    t->hist_top   = 0;
    t->cur_port   = 80;
    t->cur_https  = 0;
    t->page_tls   = 0;
    t->addr_len   = 0;
    t->cur_url[0] = 0;
    t->cur_host[0]= 0;
    t->cur_path[0]= 0;
    t->addr[0]    = 0;
    t->status[0]  = 0;
    t->alive      = 1;
}

/* Open a new tab (if not at MAX_TABS). Sets it as active. */
static void tab_open_new(void)
{
    if (g_ntabs >= MAX_TABS) {
        k_strlcpy(g_status, "Max tabs open (6)", sizeof(g_status));
        return;
    }
    int idx = g_ntabs;
    tab_init(idx);
    g_ntabs++;
    g_atab = idx;
    g_addr_focus = 1;
    /* show welcome page on the new tab */
    show_welcome();
    hist_push("about:welcome");
    k_strlcpy(g_addr, "http://", URL_CAP);
    g_addr_len = (int)k_strlen(g_addr);
}

/* Close the active tab. Switches to the previous tab. */
static void tab_close(void)
{
    if (g_ntabs <= 1) {
        k_strlcpy(g_status, "Cannot close last tab", sizeof(g_status));
        return;
    }
    /* shift tabs left */
    for (int i = g_atab; i < g_ntabs - 1; i++) {
        /* swap by copy (tab_t is large; copy field-by-field to avoid stack blowup) */
        g_tabs[i] = g_tabs[i + 1];
    }
    g_ntabs--;
    if (g_atab >= g_ntabs) g_atab = g_ntabs - 1;
    /* status already on the newly-active tab via T macro */
    k_strlcpy(g_status, "Tab closed", sizeof(g_status));
}

/* =========================================================================
 * Find-in-page
 * ========================================================================= */
static int line_has_query(int li)
{
    rline_t *L = &g_lines[li];
    if (L->off < 0 || L->len <= 0 || g_find_len == 0) return 0;
    int qn = g_find_len;
    for (int s = 0; s + qn <= L->len; s++) {
        int ok = 1;
        for (int j = 0; j < qn; j++) {
            char a = g_render[L->off + s + j];
            char b = g_find_q[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static void find_next(int from_line)
{
    g_find_hits = 0;
    g_find_line = -1;
    if (g_find_len == 0) { k_strlcpy(g_status, "Find: (empty)", sizeof(g_status)); return; }

    int first = -1, next = -1;
    for (int li = 0; li < g_nlines; li++) {
        if (line_has_query(li)) {
            g_find_hits++;
            if (first < 0) first = li;
            if (next < 0 && li > from_line) next = li;
        }
    }
    if (g_find_hits == 0) {
        k_strlcpy(g_status, "Not found: ", sizeof(g_status));
        k_strlcat(g_status, g_find_q, sizeof(g_status));
        return;
    }
    g_find_line = (next >= 0) ? next : first;
    g_scroll = g_find_line - 1;
    clamp_scroll();

    k_strlcpy(g_status, "Found '", sizeof(g_status));
    k_strlcat(g_status, g_find_q, sizeof(g_status));
    k_strlcat(g_status, "' (", sizeof(g_status));
    {
        char nb[16]; k_utoa((unsigned long)g_find_hits, nb, sizeof(nb));
        k_strlcat(g_status, nb, sizeof(g_status));
    }
    k_strlcat(g_status, " hits)", sizeof(g_status));
}

/* =========================================================================
 * Navigation: fetch + render a URL
 * ========================================================================= */
static void navigate(const char *url, int record_history);

static void go_back(void)
{
    if (g_hist_top < 2) {
        k_strlcpy(g_status, "No previous page", sizeof(g_status));
        return;
    }
    g_hist_top--;
    char prev[URL_CAP];
    k_strlcpy(prev, g_hist[(g_hist_top - 1) % HIST_MAX], URL_CAP);
    g_hist_top--;
    navigate(prev, 1);
}

static void navigate(const char *url, int record_history)
{
    char host[HOST_CAP];
    char path[PATH_CAP];
    int  port, https;

    serial_puts("[BROWSER] navigate ");
    serial_puts(url);
    serial_puts("\n");

    if (!parse_url(url, host, HOST_CAP, &port, path, PATH_CAP, &https)) {
        set_simple_page("Bad URL",
            "Could not parse that URL.\n\n"
            "Use the form http://host[:port]/path\n"
            "Example: http://example.com/\n");
        return;
    }

    /* normalise the full URL */
    char full[URL_CAP];
    k_strlcpy(full, https ? "https://" : "http://", URL_CAP);
    k_strlcat(full, host, URL_CAP);
    if ((https && port != 443) || (!https && port != 80)) {
        char pb[8]; int n = 0; int pv = port;
        char dg[8]; int di = 0;
        do { dg[di++] = (char)('0' + pv % 10); pv /= 10; } while (pv > 0);
        pb[n++] = ':'; while (di > 0) pb[n++] = dg[--di]; pb[n] = 0;
        k_strlcat(full, pb, URL_CAP);
    }
    k_strlcat(full, path, URL_CAP);

    k_strlcpy(g_cur_url, full, URL_CAP);
    k_strlcpy(g_cur_host, host, HOST_CAP);
    k_strlcpy(g_cur_path, path, PATH_CAP);
    g_cur_port  = port;
    g_cur_https = https;
    k_strlcpy(g_addr, full, URL_CAP);
    g_addr_len = (int)k_strlen(g_addr);

    if (record_history) hist_push(full);

    g_page_tls = 0;

    k_strlcpy(g_status, https ? "[https] Loading " : "[http] Loading ",
              sizeof(g_status));
    k_strlcat(g_status, host, sizeof(g_status));
    k_strlcat(g_status, " ...", sizeof(g_status));

    /* perform GET */
    int http_status = 0;
    k_memset(g_html, 0, 1);
    long blen;
    if (https) {
        serial_puts("[BROWSER] https_get ");
        serial_puts(host);
        serial_puts(path);
        serial_puts("\n");
        blen = https_get(host, (unsigned short)port, path,
                         g_html, (unsigned long)HTML_CAP, &http_status);
        if (blen >= 0) g_page_tls = 1;
    } else {
        blen = http_get(host, (unsigned short)port, path,
                        g_html, (unsigned long)HTML_CAP, &http_status);
    }

    if (blen < 0) {
        g_page_tls = 0;
        if (https) {
            set_simple_page("HTTPS load failed",
                "Could not load the page over HTTPS.\n\n"
                "Possible reasons:\n"
                "  - TLS handshake failed (server certificate rejected, or\n"
                "    CA store not populated -- peer unauthenticated).\n"
                "  - Networking not enabled yet (driver not wired).\n"
                "  - Host could not be resolved (DNS failure).\n"
                "  - Connection refused or timed out.\n\n"
                "Check the URL and try again, or type 'back'.\n");
        } else {
            set_simple_page("Load failed",
                "Could not load the page.\n\n"
                "Possible reasons:\n"
                "  - Networking not enabled yet (driver not wired).\n"
                "  - Host could not be resolved (DNS failure).\n"
                "  - Connection refused or timed out.\n"
                "  - The server returned no body.\n\n"
                "Check the URL and try again, or type 'back'.\n");
        }
        k_strlcpy(g_status, "Load failed: ", sizeof(g_status));
        k_strlcat(g_status, host, sizeof(g_status));
        serial_puts("[BROWSER] ");
        serial_puts(https ? "https_get" : "http_get");
        serial_puts(" failed rc=");
        serial_num(blen);
        serial_puts(" status=");
        serial_num(http_status);
        serial_puts("\n");
        return;
    }

    if (blen >= HTML_CAP) blen = HTML_CAP - 1;
    g_html[blen] = 0;

    /* ---- DOWNLOAD: save body if URL path has a binary/media extension ---- */
    if (path_is_download(path)) {
        download_save(path, g_html, (int)blen);
        /* still render/show what we got (may be HTML or raw bytes) */
    }

    /* render */
    strip_html(g_html, (int)blen);
    layout_render();
    g_scroll = 0;

    /* build status line */
    if (g_page_tls) {
        k_strlcpy(g_status, "[https] (encrypted)  HTTP ", sizeof(g_status));
    } else {
        k_strlcpy(g_status, "[http insecure]  HTTP ", sizeof(g_status));
    }
    {
        char nb[16]; int n = 0; int v = http_status; if (v < 0) v = 0;
        char dg[8]; int di = 0;
        do { dg[di++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (di > 0) nb[n++] = dg[--di]; nb[n] = 0;
        k_strlcat(g_status, nb, sizeof(g_status));
    }
    k_strlcat(g_status, "  ", sizeof(g_status));
    {
        char nb[24]; int n = 0; long v = blen;
        char dg[24]; int di = 0;
        do { dg[di++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (di > 0) nb[n++] = dg[--di]; nb[n] = 0;
        k_strlcat(g_status, nb, sizeof(g_status));
    }
    k_strlcat(g_status, " bytes  ", sizeof(g_status));
    k_strlcat(g_status, host, sizeof(g_status));
    if (g_nlinks > 0) {
        k_strlcat(g_status, "  (", sizeof(g_status));
        char nb[16]; int n = 0; int v = g_nlinks;
        char dg[8]; int di = 0;
        do { dg[di++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (di > 0) nb[n++] = dg[--di]; nb[n] = 0;
        k_strlcat(g_status, nb, sizeof(g_status));
        k_strlcat(g_status, " links)", sizeof(g_status));
    }

    serial_puts("[BROWSER] loaded ");
    serial_puts(g_page_tls ? "(tls) " : "(plain) ");
    serial_puts("status=");
    serial_num(http_status);
    serial_puts(" bytes=");
    serial_num(blen);
    serial_puts(" links=");
    serial_num(g_nlinks);
    serial_puts("\n");
}

/* =========================================================================
 * Address-bar command dispatch
 * ========================================================================= */
static void follow_link(int n)
{
    if (n < 1 || n > g_nlinks) {
        k_strlcpy(g_status, "No such link", sizeof(g_status));
        return;
    }
    char abs[URL_CAP];
    resolve_url(g_links[n - 1].href, abs, URL_CAP);
    if (!abs[0]) {
        k_strlcpy(g_status, "Could not resolve link", sizeof(g_status));
        return;
    }
    navigate(abs, 1);
}

static void address_submit(void)
{
    char *s = g_addr;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return;

    if (k_streq(s, "back")) { go_back(); return; }
    if (k_streq(s, "home")) {
        show_welcome();
        hist_push("about:welcome");
        k_strlcpy(g_addr, "about:welcome", URL_CAP);
        g_addr_len = (int)k_strlen(g_addr);
        return;
    }

    if (k_strncmp_ci(s, "link:", 5) == 0) {
        follow_link((int)k_atoi(s + 5));
        return;
    }

    {
        int all_digits = 1;
        for (char *q = s; *q; q++) {
            if (!k_isdigit(*q)) { all_digits = 0; break; }
        }
        if (all_digits) { follow_link((int)k_atoi(s)); return; }
    }

    navigate(s, 1);
}

/* =========================================================================
 * Rendering the window
 * ========================================================================= */
static void clamp_scroll(void)
{
    int max = g_nlines - VIS_ROWS;
    if (max < 0) max = 0;
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0) g_scroll = 0;
}

static void scroll_by(int delta)
{
    g_scroll += delta;
    clamp_scroll();
}

static int draw_render_span(u32 *pix, u32 stride_px, u32 bw, u32 bh,
                            int x, int y, int off, int len, u32 color, int bold)
{
    int cx = x;
    if (off < 0 || off >= g_render_len) return 0;
    if (len > g_render_len - off) len = g_render_len - off;
    for (int i = 0; i < len; i++) {
        char c = g_render[off + i];
        if (c < 0x20 || c > 0x7E) c = ' ';
        font_draw_char(pix, (int)stride_px, (int)bw, (int)bh, cx, y, c, color);
        if (bold)
            font_draw_char(pix, (int)stride_px, (int)bw, (int)bh, cx + 1, y, c, color);
        cx += FONT_W;
    }
    return cx - x;
}

static void draw_underline(u32 *pix, u32 stride_px, u32 bw, u32 bh,
                           int x, int y, int cols, u32 color)
{
    fill_rect(pix, stride_px, bw, bh, x, y + FONT_H - 2, cols * FONT_W, 1, color);
}

static void draw_table_span(u32 *pix, u32 stride_px, u32 bw, u32 bh,
                            int x, int y, int off, int len, u32 color)
{
    if (off < 0 || off >= g_render_len) return;
    if (len > g_render_len - off) len = g_render_len - off;
    int col = 0;
    for (int i = 0; i < len; i++) {
        char c = g_render[off + i];
        if (c == '\t') {
            int next = (col / 8 + 1) * 8;
            while (col < next && col < CONTENT_COLS) col++;
            continue;
        }
        if (c < 0x20 || c > 0x7E) c = ' ';
        if (col >= CONTENT_COLS) break;
        font_draw_char(pix, (int)stride_px, (int)bw, (int)bh,
                       x + col * FONT_W, y, c, color);
        col++;
    }
}

/* ---- tab bar ---- */
static void render_tab_bar(u32 *pix, u32 stride_px, u32 bw, u32 bh)
{
    /* background */
    fill_rect(pix, stride_px, bw, bh, 0, 0, (i32)bw, TAB_BAR_H, COL_CHROME);

    /* compute tab width: share space evenly, but cap at TAB_MAX_W */
    /* reserve space for the '+' button on the right */
    int usable = (i32)bw - TAB_PLUS_W - 2;
    int tw = (g_ntabs > 0) ? usable / g_ntabs : usable;
    if (tw > TAB_MAX_W) tw = TAB_MAX_W;
    if (tw < 20) tw = 20;

    for (int i = 0; i < g_ntabs; i++) {
        int tx = i * tw;
        int is_active = (i == g_atab);
        u32 bg = is_active ? COL_TAB_ACTIVE : COL_TAB_INACTIVE;
        fill_rect(pix, stride_px, bw, bh, tx, 0, tw, TAB_BAR_H, bg);
        /* right border */
        fill_rect(pix, stride_px, bw, bh, tx + tw - 1, 0, 1, TAB_BAR_H, COL_TAB_BORDER);
        /* active tab bottom highlight (no border, blends into chrome) */
        if (is_active)
            fill_rect(pix, stride_px, bw, bh, tx, TAB_BAR_H - 1, tw - 1, 1, COL_TAB_ACTIVE);

        /* label: "T1 <short-hostname>" or "T1 new tab" */
        char label[32];
        label[0] = 'T';
        char dg[8]; int di = 0;
        int num = i + 1;
        do { dg[di++] = (char)('0' + num % 10); num /= 10; } while (num > 0);
        int li = 1;
        while (di > 0) label[li++] = dg[--di];
        label[li++] = ' ';
        /* tab title = first 10 chars of hostname (or "new tab") */
        const char *ttl = g_tabs[i].cur_host[0] ? g_tabs[i].cur_host : "new tab";
        for (int j = 0; ttl[j] && li < 30; j++) label[li++] = ttl[j];
        label[li] = 0;

        /* clip label to tab width */
        int max_lchars = (tw - 4) / FONT_W;
        if (max_lchars < 1) max_lchars = 1;
        u32 lcol = is_active ? COL_TEXT : COL_DIM;
        int drawn = 0;
        for (int j = 0; label[j] && drawn < max_lchars; j++, drawn++) {
            char c = label[j];
            if (c < 0x20 || c > 0x7E) c = ' ';
            font_draw_char(pix, (int)stride_px, (int)bw, (int)bh,
                           tx + 2 + drawn * FONT_W, 2, c, lcol);
        }
    }

    /* '+' button */
    int px = (i32)bw - TAB_PLUS_W;
    fill_rect(pix, stride_px, bw, bh, px, 0, TAB_PLUS_W, TAB_BAR_H, COL_TAB_INACTIVE);
    draw_border(pix, stride_px, bw, bh, px, 0, TAB_PLUS_W, TAB_BAR_H, COL_TAB_BORDER);
    font_draw_char(pix, (int)stride_px, (int)bw, (int)bh,
                   px + (TAB_PLUS_W - FONT_W) / 2, 2, '+', COL_DIM);
}

static void render_window(wl_window *win, u64 ticks)
{
    u32 stride_px = win->stride / 4u;
    u32 bw = win->w, bh = win->h;
    u32 *pix = win->pixels;

    /* background */
    fill_rect(pix, stride_px, bw, bh, 0, 0, (i32)bw, (i32)bh, COL_BG);

    /* ---- tab bar ---- */
    render_tab_bar(pix, stride_px, bw, bh);

    /* ---- chrome / toolbar ---- */
    fill_rect(pix, stride_px, bw, bh, 0, TAB_BAR_H, (i32)bw, CHROME_H, COL_CHROME);

    /* address bar box */
    u32 addr_bg = g_addr_focus ? COL_ADDR_BG_F : COL_ADDR_BG;
    fill_rect(pix, stride_px, bw, bh, ADDR_X, ADDR_Y, ADDR_W, ADDR_H, addr_bg);
    draw_border(pix, stride_px, bw, bh, ADDR_X, ADDR_Y, ADDR_W, ADDR_H,
                COL_ADDR_BORDER);

    /* address text */
    {
        int maxchars = (ADDR_W - 8) / FONT_W;
        int start = 0;
        if (g_addr_len > maxchars) start = g_addr_len - maxchars;
        int tx = ADDR_X + 4;
        for (int i = start; i < g_addr_len; i++) {
            char c = g_addr[i];
            if (c < 0x20 || c > 0x7E) c = ' ';
            font_draw_char(pix, (int)stride_px, (int)bw, (int)bh,
                           tx, ADDR_Y + 2, c, COL_TEXT);
            tx += FONT_W;
        }
        if (g_addr_focus && ((ticks / 500) & 1) == 0) {
            fill_rect(pix, stride_px, bw, bh, tx, ADDR_Y + 2, 2, FONT_H,
                      COL_CURSOR);
        }
    }

    /* Go button */
    fill_rect(pix, stride_px, bw, bh, GO_X, ADDR_Y, GO_W, GO_H, COL_BTN);
    font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                     GO_X + (GO_W - 2 * FONT_W) / 2, ADDR_Y + 2, "Go",
                     COL_BTN_TXT);

    /* Back button */
    fill_rect(pix, stride_px, bw, bh, BACK_X, ADDR_Y, 48, GO_H, COL_CHROME);
    draw_border(pix, stride_px, bw, bh, BACK_X, ADDR_Y, 48, GO_H,
                COL_ADDR_BORDER);
    font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                     BACK_X + 4, ADDR_Y + 2, "Back", COL_DIM);

    /* ---- content area ---- */
    for (int vi = 0; vi < VIS_ROWS; vi++) {
        int li = g_scroll + vi;
        if (li >= g_nlines) break;
        int y = CONTENT_Y + vi * FONT_H;
        rline_t *L = &g_lines[li];

        if (g_find_hits > 0 && li == g_find_line) {
            fill_rect(pix, stride_px, bw, bh, CONTENT_X - 2, y - 1,
                      CONTENT_W + 4, FONT_H, COL_FIND_BG);
        }

        if (L->kind == LK_LHEADER) {
            int adv = font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                                       CONTENT_X, y, "--- Links ---", COL_HEADING);
            (void)adv;
        } else if (L->kind == LK_LINKROW) {
            if (L->link_idx < 0 || L->link_idx >= g_nlinks) { /* skip invalid link_idx */ }
            else {
            link_t *lk = &g_links[L->link_idx];
            char head[24];
            int hn = 0;
            head[hn++] = '[';
            int num = L->link_idx + 1;
            char dg[8]; int di = 0;
            do { dg[di++] = (char)('0' + num % 10); num /= 10; } while (num > 0);
            while (di > 0) head[hn++] = dg[--di];
            head[hn++] = ']'; head[hn++] = ' '; head[hn] = 0;

            int x = CONTENT_X;
            x += font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                                  x, y, head, COL_LINK);
            const char *txt = lk->text[0] ? lk->text : lk->href;
            int avail_cols = (CONTENT_W - (x - CONTENT_X)) / FONT_W;
            int drawn = 0;
            for (int i = 0; txt[i] && i < avail_cols; i++) {
                char c = txt[i];
                if (c < 0x20 || c > 0x7E) c = ' ';
                font_draw_char(pix, (int)stride_px, (int)bw, (int)bh,
                               x, y, c, COL_LINK);
                x += FONT_W; drawn++;
            }
            int total_cols = (int)(k_strlen(head)) + drawn;
            draw_underline(pix, stride_px, bw, bh, CONTENT_X, y, total_cols, COL_LINK);
            }
        } else if (L->kind == LK_HEADING) {
            int adv = draw_render_span(pix, stride_px, bw, bh,
                                       CONTENT_X, y, L->off, L->len,
                                       COL_HEADING, 1);
            if (L->attr <= 2)
                draw_underline(pix, stride_px, bw, bh, CONTENT_X, y,
                               adv / FONT_W, COL_HEADING);
        } else if (L->kind == LK_LISTITEM) {
            int indent = (2 + 2 * (L->attr > 0 ? L->attr : 1)) * FONT_W;
            int bx = CONTENT_X + indent - 2 * FONT_W;
            font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                             bx, y, "*", COL_BULLET);
            draw_render_span(pix, stride_px, bw, bh,
                             CONTENT_X + indent, y, L->off, L->len, COL_TEXT, 0);
        } else if (L->kind == LK_PRE) {
            if (!(g_find_hits > 0 && li == g_find_line))
                fill_rect(pix, stride_px, bw, bh, CONTENT_X - 2, y - 1,
                          CONTENT_W + 4, FONT_H, COL_PRE_BG);
            int n = L->len; if (n > CONTENT_COLS) n = CONTENT_COLS;
            draw_render_span(pix, stride_px, bw, bh,
                             CONTENT_X, y, L->off, n, COL_PRE, 0);
        } else if (L->kind == LK_TABLE) {
            draw_table_span(pix, stride_px, bw, bh,
                            CONTENT_X, y, L->off, L->len, COL_TABLE);
        } else {
            draw_render_span(pix, stride_px, bw, bh,
                             CONTENT_X, y, L->off, L->len, COL_TEXT, 0);
        }
    }

    /* scroll indicator */
    if (g_nlines > VIS_ROWS) {
        int track_x = (i32)bw - 4;
        int track_y = CONTENT_Y;
        int track_h = CONTENT_H;
        fill_rect(pix, stride_px, bw, bh, track_x, track_y, 3, track_h,
                  COL_CHROME);
        int thumb_h = track_h * VIS_ROWS / g_nlines;
        if (thumb_h < 12) thumb_h = 12;
        int max_scroll = g_nlines - VIS_ROWS;
        int thumb_y = track_y;
        if (max_scroll > 0)
            thumb_y += (track_h - thumb_h) * g_scroll / max_scroll;
        fill_rect(pix, stride_px, bw, bh, track_x, thumb_y, 3, thumb_h,
                  COL_DIM);
    }

    /* ---- status bar ---- */
    i32 sb_y = (i32)bh - STATUS_H;
    fill_rect(pix, stride_px, bw, bh, 0, sb_y, (i32)bw, STATUS_H, COL_STATUS_BG);

    if (g_find_active) {
        fill_rect(pix, stride_px, bw, bh, 0, sb_y, (i32)bw, STATUS_H, COL_STATUS_BG);
        int fx = 4;
        fx += font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                               fx, sb_y + 1, "Find: ", COL_HEADING);
        for (int i = 0; i < g_find_len; i++) {
            char c = g_find_q[i]; if (c < 0x20 || c > 0x7E) c = ' ';
            font_draw_char(pix, (int)stride_px, (int)bw, (int)bh,
                           fx, sb_y + 1, c, COL_STATUS_TXT);
            fx += FONT_W;
        }
        if (((ticks / 500) & 1) == 0)
            fill_rect(pix, stride_px, bw, bh, fx, sb_y + 1, 2, FONT_H - 2, COL_CURSOR);
        font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                         (i32)bw - 200, sb_y + 1,
                         "[Enter] next  [Esc] cancel", COL_HINT);
    } else if (g_dl_ticks > 0) {
        /* download status overlay on the status bar */
        int sx = 4;
        sx += font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                               sx, sb_y + 1, g_dl_status, COL_DL_OK);
        (void)sx;
        g_dl_ticks--;
    } else {
        /*
         * Normal status: security badge + status text.
         */
        int sx = 4;
        if (g_page_tls) {
            int lx = sx, ly = sb_y + 2;
            fill_rect(pix, stride_px, bw, bh, lx + 1, ly,     5, 1, COL_SEC_HTTPS);
            fill_rect(pix, stride_px, bw, bh, lx,     ly + 1, 1, 2, COL_SEC_HTTPS);
            fill_rect(pix, stride_px, bw, bh, lx + 6, ly + 1, 1, 2, COL_SEC_HTTPS);
            fill_rect(pix, stride_px, bw, bh, lx,     ly + 3, 7, 5, COL_SEC_HTTPS);
            fill_rect(pix, stride_px, bw, bh, lx + 3, ly + 4, 1, 2, COL_STATUS_BG);
            sx += 10;

            const char *badge = "[https] (encrypted)  ";
            sx += font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                                   sx, sb_y + 1, badge, COL_SEC_HTTPS);
            int badge_len = (int)k_strlen(badge);
            const char *rest = g_status;
            int slen = (int)k_strlen(g_status);
            if (slen >= badge_len) rest = g_status + badge_len;
            font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                             sx, sb_y + 1, rest, COL_STATUS_TXT);
        } else {
            const char *http_badge = "[http insecure]  ";
            int hblen = (int)k_strlen(http_badge);
            int has_badge = 1;
            if ((int)k_strlen(g_status) < hblen) {
                has_badge = 0;
            } else {
                for (int ci = 0; ci < hblen; ci++) {
                    if (g_status[ci] != http_badge[ci]) { has_badge = 0; break; }
                }
            }
            if (has_badge) {
                int lx = sx, ly = sb_y + 2;
                fill_rect(pix, stride_px, bw, bh, lx + 1, ly,     3, 1, COL_SEC_HTTP);
                fill_rect(pix, stride_px, bw, bh, lx,     ly + 1, 1, 2, COL_SEC_HTTP);
                fill_rect(pix, stride_px, bw, bh, lx,     ly + 3, 7, 5, COL_SEC_HTTP);
                fill_rect(pix, stride_px, bw, bh, lx + 3, ly + 4, 1, 2, COL_STATUS_BG);
                sx += 10;

                sx += font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                                       sx, sb_y + 1, http_badge, COL_SEC_HTTP);
                font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                                 sx, sb_y + 1, g_status + hblen, COL_STATUS_TXT);
            } else {
                font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                                 sx, sb_y + 1, g_status, COL_STATUS_TXT);
            }
        }

        /* tab indicator "[T2/4]" + scroll position on the right */
        char pos[48]; int pn = 0;
        pos[pn++] = '['; pos[pn++] = 'T';
        pn += k_utoa((unsigned long)(g_atab + 1), pos + pn, (int)sizeof(pos) - pn);
        pos[pn++] = '/';
        pn += k_utoa((unsigned long)g_ntabs, pos + pn, (int)sizeof(pos) - pn);
        pos[pn++] = ']'; pos[pn++] = ' ';
        pn += k_utoa((unsigned long)(g_nlines ? g_scroll + 1 : 0),
                     pos + pn, (int)sizeof(pos) - pn);
        pos[pn++] = '/';
        pn += k_utoa((unsigned long)g_nlines, pos + pn, (int)sizeof(pos) - pn);
        pos[pn] = 0;
        int pw = font_text_width(pos);
        font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                         (i32)bw - pw - 6, sb_y + 1, pos, COL_HINT);
    }

    /* ---- keys hint line ---- */
    if (!g_show_help) {
        i32 hint_y = sb_y - FONT_H;
        fill_rect(pix, stride_px, bw, bh, 0, hint_y, (i32)bw, FONT_H, COL_BG);
        font_draw_string(pix, (int)stride_px, (int)bw, (int)bh, 4, hint_y,
                         "[/] find  [b] bmark  [B] list  [^T] new tab  [^W] close tab  [F1] help",
                         COL_HINT);
    }

    /* ---- help overlay ---- */
    if (g_show_help) {
        int ox = 40, oy = TOOLBAR_H + 8;
        int ow = (i32)bw - 80, oh = (i32)bh - TOOLBAR_H - STATUS_H - 16;
        fill_rect(pix, stride_px, bw, bh, ox, oy, ow, oh, COL_OVERLAY_BG);
        draw_border(pix, stride_px, bw, bh, ox, oy, ow, oh, COL_OVERLAY_BD);
        static const char *help[] = {
            "AutomationOS Browser -- Keys & Help",
            "",
            "TABS",
            "  Ctrl+T              open new tab (up to 6)",
            "  Ctrl+W              close current tab",
            "  click tab label     switch to that tab",
            "  click [+]           open new tab",
            "",
            "ADDRESS BAR (when focused)",
            "  type a URL, Enter / [Go] to navigate",
            "  a bare number or 'link:N' follows link N",
            "  'back' or 'home' as commands",
            "",
            "PAGE (click page text to focus it)",
            "  Up/Down, PgUp/PgDn, Home/End  scroll",
            "  Backspace                      go back",
            "  click a [N] link row           follow it",
            "",
            "FEATURES",
            "  /        find-in-page (Enter=next, Esc=cancel)",
            "  n        jump to next find match",
            "  b        bookmark the current URL",
            "  B        show saved bookmarks (as links)",
            "  F1 or ?  toggle this help",
            "",
            "DOWNLOADS",
            "  URLs ending in .zip .tar .gz .pdf .iso .png .jpg etc.",
            "  are saved automatically to /tmp/<filename>.",
            "",
            "SECURITY",
            "  [https](green)=encrypted, [http insecure](orange)=plain.",
            "Press F1 or ? to close.",
        };
        int ty = oy + 6;
        for (unsigned i = 0; i < sizeof(help)/sizeof(help[0]); i++) {
            u32 col = (i == 0) ? COL_HEADING : COL_TEXT;
            font_draw_string(pix, (int)stride_px, (int)bw, (int)bh,
                             ox + 10, ty, help[i], col);
            ty += FONT_H;
            if (ty + FONT_H > oy + oh - 4) break;  /* don't overflow overlay */
        }
    }
}

/* =========================================================================
 * Input handling
 * ========================================================================= */
static int g_shift;

static void on_key(int kc, int pressed)
{
    if (kc == KEY_LEFTSHIFT  || kc == KEY_RIGHTSHIFT) { g_shift = pressed; return; }
    if (kc == KEY_LEFTCTRL   || kc == KEY_RIGHTCTRL)  { g_ctrl  = pressed; return; }
    if (!pressed) return;

    /* ---- Ctrl+T / Ctrl+W: tab management (highest priority) ---- */
    if (g_ctrl) {
        if (kc == KEY_T) { tab_open_new(); return; }
        if (kc == KEY_W) { tab_close();    return; }
    }

    /* ---- find prompt ---- */
    if (g_find_active) {
        if (kc == KEY_ESC) {
            g_find_active = 0;
            g_find_hits = 0; g_find_line = -1;
            k_strlcpy(g_status, "Find cancelled", sizeof(g_status));
            return;
        }
        if (kc == KEY_ENTER) {
            g_find_q[g_find_len] = 0;
            find_next(g_find_hits > 0 ? g_find_line : -1);
            g_find_active = 0;
            return;
        }
        if (kc == KEY_BACKSPACE) {
            if (g_find_len > 0) g_find_q[--g_find_len] = 0;
            return;
        }
        char ch = keycode_to_ascii(kc, g_shift);
        if (ch && g_find_len < (int)sizeof(g_find_q) - 1) {
            g_find_q[g_find_len++] = ch;
            g_find_q[g_find_len] = 0;
        }
        return;
    }

    /* ---- global toggles ---- */
    if (kc == KEY_F1) { g_show_help = !g_show_help; return; }

    if (g_addr_focus) {
        if (kc == KEY_ENTER) {
            g_addr[g_addr_len] = 0;
            address_submit();
            return;
        }
        if (kc == KEY_BACKSPACE) {
            if (g_addr_len > 0) g_addr[--g_addr_len] = 0;
            return;
        }
        if (kc == KEY_PAGEUP)   { scroll_by(-(VIS_ROWS - 1)); return; }
        if (kc == KEY_PAGEDOWN) { scroll_by(+(VIS_ROWS - 1)); return; }
        char ch = keycode_to_ascii(kc, g_shift);
        if (ch && g_addr_len < URL_CAP - 1) {
            g_addr[g_addr_len++] = ch;
            g_addr[g_addr_len] = 0;
        }
        return;
    }

    /* ---- content-focused ---- */
    char ch = keycode_to_ascii(kc, g_shift);
    if (ch == '/' || ch == '?') {
        if (ch == '?') { g_show_help = !g_show_help; return; }
        g_find_active = 1;
        g_find_len = 0; g_find_q[0] = 0;
        g_find_hits = 0; g_find_line = -1;
        k_strlcpy(g_status, "Find:", sizeof(g_status));
        return;
    }
    if (ch == 'n') { find_next(g_find_line); return; }
    if (ch == 'b') { bm_add_current(); return; }
    if (ch == 'B') { show_bookmarks(); g_addr_focus = 0; return; }

    switch (kc) {
        case KEY_UP:       scroll_by(-1); break;
        case KEY_DOWN:     scroll_by(+1); break;
        case KEY_PAGEUP:   scroll_by(-(VIS_ROWS - 1)); break;
        case KEY_PAGEDOWN: scroll_by(+(VIS_ROWS - 1)); break;
        case KEY_HOME:     g_scroll = 0; break;
        case KEY_END:      g_scroll = g_nlines; clamp_scroll(); break;
        case KEY_BACKSPACE: go_back(); break;
        default: break;
    }
}

static void on_pointer(int x, int y, int buttons, int *prev_btn)
{
    int cur = buttons;

    if (buttons & 4) {
        scroll_by(y * 3 > 0 ? 3 : (y < 0 ? -3 : 0));
    }

    if (cur & 1) {
        if (!(*prev_btn & 1)) {
            /* tab bar click */
            if (y >= 0 && y < TAB_BAR_H) {
                /* '+' button (use the LIVE width so the hit-test tracks the
                 * render side, which uses bw == win->w, after a resize). */
                int px = g_win_w - TAB_PLUS_W;
                if (x >= px) {
                    tab_open_new();
                } else {
                    /* tab label click */
                    int usable = g_win_w - TAB_PLUS_W - 2;
                    int tw = (g_ntabs > 0) ? usable / g_ntabs : usable;
                    if (tw > TAB_MAX_W) tw = TAB_MAX_W;
                    if (tw < 20) tw = 20;
                    int idx = x / tw;
                    if (idx >= 0 && idx < g_ntabs) {
                        g_atab = idx;
                        /* address bar shows the new tab's URL */
                        g_addr_focus = 0;
                    }
                }
            } else if (y >= ADDR_Y && y < ADDR_Y + ADDR_H) {
                if (x >= ADDR_X && x < ADDR_X + ADDR_W) {
                    g_addr_focus = 1;
                } else if (x >= GO_X && x < GO_X + GO_W) {
                    g_addr[g_addr_len] = 0;
                    address_submit();
                } else if (x >= BACK_X && x < BACK_X + 48) {
                    go_back();
                }
            } else if (y >= CONTENT_Y && y < CONTENT_BOT) {
                int vi = (y - CONTENT_Y) / FONT_H;
                int li = g_scroll + vi;
                if (li >= 0 && li < g_nlines && g_lines[li].kind == LK_LINKROW &&
                    g_lines[li].link_idx >= 0 && g_lines[li].link_idx < g_nlinks) {
                    follow_link(g_lines[li].link_idx + 1);
                } else {
                    g_addr_focus = 0;
                }
            }
        }
    }
    *prev_btn = cur;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
void _start(void)
{
    serial_puts("[BROWSER] starting\n");

    /* seed the runtime layout geometry from the initial window size BEFORE any
     * layout_render() runs (show_welcome / navigate word-wrap to CONTENT_COLS). */
    recompute_geometry(WIN_W, WIN_H);

    /* zero global state */
    k_memset(g_tabs,    0, sizeof(g_tabs));
    k_memset(g_bm,      0, sizeof(g_bm));
    k_memset(g_bm_io,   0, sizeof(g_bm_io));
    k_memset(g_find_q,  0, sizeof(g_find_q));
    k_memset(g_dl_status, 0, sizeof(g_dl_status));

    g_nbm        = 0;
    g_find_active = 0;
    g_find_len    = 0;
    g_find_hits   = 0;
    g_find_line   = -1;
    g_show_help   = 0;
    g_shift       = 0;
    g_ctrl        = 0;
    g_addr_focus  = 1;
    g_dl_ticks    = 0;

    /* initialise first tab (must happen BEFORE any g_status / g_addr access,
     * because those macros dereference g_tabs[g_atab]) */
    g_ntabs = 1;
    g_atab  = 0;
    tab_init(0);

    /* load persisted bookmarks */
    bm_load();

    /* welcome page */
    show_welcome();
    hist_push("about:welcome");
    k_strlcpy(g_addr, "http://", URL_CAP);
    g_addr_len = (int)k_strlen(g_addr);

    if (wl_connect() != 0) {
        serial_puts("[BROWSER] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Browser");
    if (!win) {
        serial_puts("[BROWSER] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    serial_puts("[BROWSER] window created\n");
    serial_puts("[BROWSER] ready\n");

    int prev_btn = 0;

    for (;;) {
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                on_key(a, b);
            } else if (kind == WL_EVENT_POINTER) {
                on_pointer(a, b, c, &prev_btn);
            } else if (kind == WL_EVENT_RESIZE) {
                /* The library has ALREADY reallocated the buffer and updated
                 * win->{w,h,stride,pixels}; we only rederive layout. Reflow the
                 * content (word-wrap width + visible rows) to the new size and
                 * re-wrap every tab so a later tab switch isn't stale. */
                recompute_geometry((int)win->w, (int)win->h);
                int save_atab = g_atab;
                for (int ti = 0; ti < g_ntabs; ti++) {
                    if (!g_tabs[ti].alive) continue;
                    g_atab = ti;
                    layout_render();       /* re-wraps T->render to CONTENT_COLS */
                    clamp_scroll();        /* keep scroll within new VIS_ROWS    */
                }
                g_atab = save_atab;
            }
        }

        u64 ticks = (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        render_window(win, ticks);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
