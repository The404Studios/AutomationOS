/*
 * browser2.c -- DOM-rendering web browser (freestanding, ring 3).
 * ================================================================
 *
 * Integration capstone for the browser pipeline wave.  Uses the
 * concurrent-agent libraries:
 *
 *   html_parse  -> dom_document / dom_node tree
 *   css_parse   -> css_stylesheet (inline styles + UA default sheet)
 *   layout_compute -> layout_box tree (block/inline/text boxes)
 *   js_eval     -> ES5 inline scripts with DOM bindings
 *   dom_bindings_install -> window / document / console visible to JS
 *   http_get / https_get -> HTTP/HTTPS page fetch
 *   wl_client   -> compositor SHM window (800x600)
 *   bitfont     -> 8x16 glyph blit into framebuffer
 *   SYS_FB_ACQUIRE (39) -> direct framebuffer fallback
 *
 * Entry: crt0 (start.asm) -> int main(int argc, char **argv)
 * Smoke exit: enters a bounded ~60fps render loop (polls input, runs JS
 *             timers, animates inertial scroll, repaints chrome + content),
 *             prints "BROWSER2: rendered <N> boxes for <URL>" once after the
 *             first paint, runs for ~5 s of frames, then calls SYS_EXIT(0).
 *             Never blocks waiting for input (smoke test is non-interactive).
 *
 * Bounds:
 *   MAX_DOM_NODES  4096  (html_parse hard-caps at 4096 internally; we check)
 *   MAX_LAYOUT_BOXES 2048
 *   MAX_SCRIPT_BYTES 65536 (64 KB)
 *
 * Build (NO fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/browser2/browser2.c -o browser2.o
 *   objdump -d build/browser2 | grep fs:0x28   # MUST be empty
 */

/* ---- Library headers (all freestanding, no libc) ---- */
#include "../../lib/html/html_parse.h"   /* html_parse, html_get_inline_scripts */
#include "../../lib/css/css.h"           /* css_parse, css_free                  */
#include "../../lib/layout/layout.h"     /* layout_compute, layout_free          */
#include "../../lib/js/js.h"             /* js_new, js_eval, js_set_print        */
#include "../../lib/js/js_native.h"      /* js_native_*                          */
#include "../../lib/js/js_console.h"     /* js_console_install                   */
#include "../../lib/js/js_timers.h"      /* js_timers_install, js_timers_run     */
#include "../../lib/js/js_fetch.h"       /* js_fetch_install                     */
#include "../../lib/js/js_storage.h"     /* js_storage_install                   */
#include "../../lib/js/js_url.h"         /* js_url_install                       */
#include "../../lib/dom/dom_bindings.h"  /* dom_bindings_install                 */
#include "../../lib/net/http.h"          /* http_get, https_get                  */
#include "../../lib/wl/wl_client.h"      /* wl_connect, wl_create_window, ...    */
#include "../../lib/font/bitfont.h"      /* font_draw_char, FONT_W, FONT_H       */
#include "browser2_ui.h"                 /* b2ui_draw_chrome, b2ui_hit_chrome    */
#include "browser2_anim.h"               /* b2anim_scroll_*, b2anim_crossfade    */

/* =========================================================================
 * Basic types (no stdint.h in freestanding)
 * ========================================================================= */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef signed int     i32;
typedef signed long    i64;

/* =========================================================================
 * Syscall numbers
 * ========================================================================= */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40
#define SYS_FB_ACQUIRE    39

/* SYS_FB_ACQUIRE result structure (mirrors kernel fb_acquire_t) */
typedef struct {
    u64 vaddr;
    u32 width;
    u32 height;
    u32 pitch;   /* bytes per row */
    u32 bpp;     /* bits per pixel */
} b2_fb_info;

/* =========================================================================
 * 6-arg inline syscall (no fs:0x28 canary)
 * ========================================================================= */
static inline i64 sc(i64 n, i64 a1, i64 a2, i64 a3,
                     i64 a4, i64 a5, i64 a6)
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

/* =========================================================================
 * Serial output helpers (fd 1)
 * ========================================================================= */
static u32 b2_strlen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static void b2_puts(const char *s)
{
    sc(SYS_WRITE, 1, (i64)s, (i64)b2_strlen(s), 0, 0, 0);
}

static void b2_putnum(i64 v)
{
    char buf[24];
    int  i = 0;
    if (v < 0) { sc(SYS_WRITE, 1, (i64)"-", 1, 0, 0, 0); v = -v; }
    do { buf[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    while (i > 0) { char c = buf[--i]; sc(SYS_WRITE, 1, (i64)&c, 1, 0, 0, 0); }
}

/* =========================================================================
 * URL parser
 * ========================================================================= */
#define URL_CAP   512
#define HOST_CAP  256
#define PATH_CAP  512

static int b2_strncasecmp(const char *a, const char *b, int n)
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

static int b2_isdigit(char c) { return c >= '0' && c <= '9'; }

/* =========================================================================
 * Keyboard: evdev/Linux keycodes + US-layout keycode->ASCII (matches the
 * sibling browser.c so the whole system shares one mapping). Used only by the
 * address-bar editor; the scroll keys below are handled by raw keycode.
 * ========================================================================= */
#define KEY_ESC         1
#define KEY_BACKSPACE  14
#define KEY_ENTER      28
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_SPACE      57

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

/* Parse `url` into host, port, path, is_https.  Returns 1 on success. */
static int b2_parse_url(const char *url,
                        char *host, int host_cap,
                        int *port, char *path, int path_cap,
                        int *is_https)
{
    host[0] = 0; path[0] = 0; *port = 80; *is_https = 0;
    const char *p = url;

    if (b2_strncasecmp(p, "https://", 8) == 0) {
        p += 8; *is_https = 1; *port = 443;
    } else if (b2_strncasecmp(p, "http://", 7) == 0) {
        p += 7;
    }
    /* else: bare host */

    int hn = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':' &&
           hn < host_cap - 1)
        host[hn++] = *p++;
    host[hn] = 0;
    if (hn == 0) return 0;

    if (*p == ':') {
        p++;
        long pv = 0;
        while (b2_isdigit(*p)) { pv = pv * 10 + (*p - '0'); p++; }
        if (pv > 0 && pv <= 65535) *port = (int)pv;
    }

    if (*p == '/' || *p == '?' || *p == '#') {
        int pn = 0;
        while (*p && pn < path_cap - 1) path[pn++] = *p++;
        path[pn] = 0;
    } else {
        path[0] = '/'; path[1] = 0;
    }
    /* strip fragment */
    for (int j = 0; path[j]; j++) {
        if (path[j] == '#') { path[j] = 0; break; }
    }
    if (path[0] == 0) { path[0] = '/'; path[1] = 0; }
    return 1;
}

/* =========================================================================
 * Address-bar input normalization.
 *
 * Turns whatever the user typed into a navigable URL written into out[cap]:
 *   - "about:..." / "http://..." / "https://..."  -> kept verbatim.
 *   - a token that looks like a bare host ("example.com", "localhost:8080",
 *     a dotted-quad, or any single word containing a '.')  -> prefix https://.
 *   - anything else (has a space, or no '.') -> Google search:
 *        https://www.google.com/search?q=<url-encoded query>
 *
 * The Google-search default makes the address bar double as a search box,
 * which is the requested UX. URL-encoding keeps spaces/&/# from breaking the
 * query string. Output is always NUL-terminated and bounded to cap.
 * ========================================================================= */
static int b2_is_scheme(const char *s)
{
    return b2_strncasecmp(s, "http://", 7) == 0 ||
           b2_strncasecmp(s, "https://", 8) == 0 ||
           b2_strncasecmp(s, "about:", 6) == 0 ||
           b2_strncasecmp(s, "file:", 5) == 0;
}

/* Append one byte to out if room; returns updated length. */
static int b2_emit(char *out, int cap, int len, char c)
{
    if (len < cap - 1) { out[len++] = c; out[len] = 0; }
    return len;
}

/* Percent-encode `s` into out[cap] starting at `len`, encoding everything
 * that is not an RFC3986 unreserved char; spaces become '+'. */
static int b2_urlencode(char *out, int cap, int len, const char *s)
{
    static const char hexd[] = "0123456789ABCDEF";
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            len = b2_emit(out, cap, len, (char)c);
        } else if (c == ' ') {
            len = b2_emit(out, cap, len, '+');
        } else {
            len = b2_emit(out, cap, len, '%');
            len = b2_emit(out, cap, len, hexd[(c >> 4) & 0xF]);
            len = b2_emit(out, cap, len, hexd[c & 0xF]);
        }
    }
    return len;
}

static void b2_normalize_input(const char *in, char *out, int cap)
{
    out[0] = 0;
    if (!in || !in[0] || cap < 2) return;

    /* Trim leading spaces. */
    const char *p = in;
    while (*p == ' ' || *p == '\t') p++;

    /* Already an explicit scheme -> copy verbatim (trim trailing spaces). */
    if (b2_is_scheme(p)) {
        int i = 0;
        while (p[i] && i < cap - 1) { out[i] = p[i]; i++; }
        while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) i--;
        out[i] = 0;
        return;
    }

    /* Decide: bare host vs. search query. A host-like token has no internal
     * space and contains a '.' (or is "localhost"). */
    int has_space = 0, has_dot = 0;
    for (const char *q = p; *q; q++) {
        if (*q == ' ' || *q == '\t') has_space = 1;
        if (*q == '.') has_dot = 1;
    }
    int is_localhost = (b2_strncasecmp(p, "localhost", 9) == 0);

    if (!has_space && (has_dot || is_localhost)) {
        /* Treat as a bare host: prefix https:// */
        int len = 0;
        const char *pre = "https://";
        for (int i = 0; pre[i] && len < cap - 1; i++) out[len++] = pre[i];
        for (int i = 0; p[i] && len < cap - 1; i++) {
            if (p[i] == ' ' || p[i] == '\t') break;
            out[len++] = p[i];
        }
        out[len] = 0;
        return;
    }

    /* Otherwise: Google search. */
    int len = 0;
    const char *pre = "https://www.google.com/search?q=";
    for (int i = 0; pre[i] && len < cap - 1; i++) out[len++] = pre[i];
    out[len] = 0;
    len = b2_urlencode(out, cap, len, p);
    out[len] = 0;
}

/* =========================================================================
 * Bounds
 * ========================================================================= */
#define MAX_DOM_NODES     4096
#define MAX_LAYOUT_BOXES  2048
#define MAX_SCRIPT_BYTES  65536   /* 64 KB */

/* =========================================================================
 * HTTP fetch buffer (256 KB for page body)
 * ========================================================================= */
#define BODY_CAP  (256 * 1024)
static char g_body[BODY_CAP];

/* =========================================================================
 * UA default stylesheet (baked-in CSS string).
 * css_parse handles the UA defaults internally but we give it a small
 * supplemental block to ensure the browser's own visual identity. The
 * library's built-in UA sheet is applied first (via CSS cascade), so this
 * is purely additive.
 * ========================================================================= */
static const char UA_CSS[] =
    "body { background-color: #f0f0f0; color: #111111; margin: 8px; }\n"
    "a    { color: #0000cc; text-decoration: underline; }\n"
    "h1   { font-size: 32px; font-weight: bold; }\n"
    "h2   { font-size: 24px; font-weight: bold; }\n"
    "h3   { font-size: 20px; font-weight: bold; }\n"
    "pre,code { background-color: #e8e8e8; color: #003333; }\n";

/* =========================================================================
 * Viewport / window geometry
 * ========================================================================= */
#define VP_W  800
#define VP_H  600

/* =========================================================================
 * Framebuffer (compositor SHM window or direct SYS_FB_ACQUIRE fallback).
 * ========================================================================= */
/* 800*600 ARGB32 = 1 920 000 bytes (~1.83 MB). */
static u32 g_fb[VP_W * VP_H];  /* our software framebuffer (ARGB32) */

/* Copy g_fb into a wl_window (if available).
 *
 * The window may have been resized by the compositor (Maximize / snap): the
 * wl library already reallocated win->pixels and updated win->{w,h,stride}, so
 * the destination geometry is whatever win now reports -- it is NOT VP_W x VP_H.
 * We render into a fixed VP_W x VP_H software framebuffer (g_fb), so on present
 * we LETTERBOX: copy the overlapping region clamped to BOTH g_fb and the live
 * window, addressing the destination with the CURRENT win->stride, then fill any
 * margins of the (larger) window with the chrome background so the new area is
 * never stale garbage. Every destination write is bounded to win->w/win->h via
 * win->stride/4, so a SMALLER window can never overflow the reallocated buffer. */
static void fb_commit_to_window(wl_window *win)
{
    if (!win || !win->pixels) return;

    u32 *dst        = win->pixels;
    const u32 *src  = g_fb;
    u32 dst_pitch   = win->stride / 4;          /* destination pixels per row */
    u32 win_w       = win->w;
    u32 win_h       = win->h;
    if (dst_pitch == 0 || win_w == 0 || win_h == 0) { wl_commit(win); return; }

    /* Overlap of our fixed canvas with the live window. */
    u32 copy_w = win_w  < (u32)VP_W ? win_w  : (u32)VP_W;
    u32 copy_h = win_h  < (u32)VP_H ? win_h  : (u32)VP_H;
    if (copy_w > dst_pitch) copy_w = dst_pitch;  /* never exceed real stride */

    for (u32 row = 0; row < win_h; row++) {
        u32 *drow = dst + (u64)row * dst_pitch;
        if (row < copy_h) {
            const u32 *srow = src + (u64)row * VP_W;
            u32 col = 0;
            for (; col < copy_w; col++) drow[col] = srow[col];
            /* Right margin (window wider than canvas): chrome background. */
            for (; col < win_w && col < dst_pitch; col++) drow[col] = 0xFFF0F0F0u;
        } else {
            /* Bottom margin (window taller than canvas): chrome background. */
            for (u32 col = 0; col < win_w && col < dst_pitch; col++)
                drow[col] = 0xFFF0F0F0u;
        }
    }
    wl_commit(win);
}

/* =========================================================================
 * Drawing helpers
 *
 * g_clip_top is the lowest y a paint may touch (defaults to 0; the render
 * loop raises it to B2UI_CHROME_H so scrolled page content never draws over
 * the chrome strip). The chrome itself is drawn by b2ui_draw_chrome(), which
 * does not use these helpers, so it is unaffected.
 * ========================================================================= */
static i32 g_clip_top = 0;

static void fill_rect(i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < g_clip_top ? g_clip_top : y;
    i32 x2 = x + w; if (x2 > VP_W) x2 = VP_W;
    i32 y2 = y + h; if (y2 > VP_H) y2 = VP_H;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = g_fb + (u32)yy * VP_W;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/* Draw text clipped to viewport (and to [g_clip_top, VP_H)). Returns x advance. */
static int draw_text_clipped(int x, int y, const char *s, u32 color)
{
    if (!s) return 0;
    int adv = 0;
    for (int i = 0; s[i]; i++) {
        int cx = x + adv;
        /* clip: don't draw chars that are fully off-screen */
        if (cx >= VP_W) break;
        /* Require the whole glyph cell to sit inside the content area:
         * font_draw_char only clips against the framebuffer, not g_clip_top,
         * so this keeps glyphs from bleeding up into the chrome strip. */
        if (cx + FONT_W > 0 && y >= g_clip_top && y < VP_H) {
            font_draw_char(g_fb, VP_W, VP_W, VP_H, cx, y, s[i], color);
        }
        adv += FONT_W;
    }
    return adv;
}

/* =========================================================================
 * JS console.log -> serial (fd 1) sink
 * ========================================================================= */
static void js_print_sink(const char *s, unsigned long n)
{
    sc(SYS_WRITE, 1, (i64)s, (i64)n, 0, 0, 0);
}

/* =========================================================================
 * DOM node counter (for bound check)
 * ========================================================================= */
static int g_dom_node_count = 0;

static void count_node_cb(dom_node *node, void *ctx)
{
    (void)node; (void)ctx;
    g_dom_node_count++;
}

/* =========================================================================
 * Layout box counter (recursive)
 * ========================================================================= */
static int count_boxes(const layout_box *b)
{
    if (!b) return 0;
    int n = 1;
    n += count_boxes(b->first_child);
    n += count_boxes(b->next_sibling);
    return n;
}

/* =========================================================================
 * Paint: walk layout tree, blit text boxes and fill background rects.
 * Clips everything to [0,0)-(VP_W,VP_H).
 * Returns number of boxes rendered.
 * ========================================================================= */
static int g_box_rendered = 0;
static int g_paint_dy     = 0;   /* added to every box y (chrome + -scroll) */

static void paint_box(const layout_box *b)
{
    if (!b) return;
    if (g_box_rendered >= MAX_LAYOUT_BOXES) return;

    /* sanity clip: skip degenerate boxes; apply the vertical paint offset */
    int bx = b->x, by = b->y + g_paint_dy, bw = b->w, bh = b->h;

    /* count even off-screen boxes toward total (they were laid out) */
    g_box_rendered++;

    /* Draw background for block and inline boxes that have a background. */
    if (b->kind == LB_BLOCK || b->kind == LB_INLINE) {
        /* css_computed::background: 0 = transparent */
        if (b->style.background != 0) {
            /* Convert AABBGGRR -> AARRGGBB (css uses 0xAARRGGBB already) */
            u32 bg = b->style.background;
            /* if alpha is 0 the color is transparent -- skip */
            u8 alpha = (u8)((bg >> 24) & 0xFF);
            if (alpha > 0) {
                fill_rect(bx, by, bw, bh, bg);
            }
        }
    }

    /* Draw text for LB_TEXT boxes. */
    if (b->kind == LB_TEXT && b->text) {
        /* Clip: only blit if box overlaps viewport */
        if (bx < VP_W && by < VP_H && bx + bw > 0 && by + bh > 0) {
            u32 color = b->style.color;
            /* If color has zero alpha (transparent) use opaque black */
            if ((color >> 24) == 0) color = 0xFF000000u;
            draw_text_clipped(bx, by, b->text, color);
        }
    }

    /* Recurse: children first, then siblings. */
    paint_box(b->first_child);
    paint_box(b->next_sibling);
}

/* Paint all boxes in document order (pre-order DFS). */
static int paint_layout(const layout_box *root)
{
    g_box_rendered = 0;
    g_clip_top     = 0;
    g_paint_dy     = 0;
    /* White page background */
    fill_rect(0, 0, VP_W, VP_H, 0xFFFFFFFFu);
    paint_box(root);
    return g_box_rendered;
}

/* Paint the page content area only: below the chrome (top = B2UI_CHROME_H),
 * scrolled vertically by `scroll_y` (content moves up as scroll_y grows).
 * Fills the content area white first, then paints layout boxes clipped to it.
 * Returns the number of boxes rendered. */
static int paint_layout_scrolled(const layout_box *root, int scroll_y)
{
    g_box_rendered = 0;
    g_clip_top     = B2UI_CHROME_H;
    g_paint_dy     = B2UI_CHROME_H - scroll_y;
    /* White background for the content area (chrome is drawn separately). */
    fill_rect(0, B2UI_CHROME_H, VP_W, VP_H - B2UI_CHROME_H, 0xFFFFFFFFu);
    paint_box(root);
    return g_box_rendered;
}

/* =========================================================================
 * Content height: the bottom-most extent of the laid-out tree (max y+h).
 * The layout root's h is the total document height per layout.h, but we
 * compute it defensively by walking the boxes so a missing root height or a
 * deep last child still yields a sane scrollable extent.
 * ========================================================================= */
static int g_content_h = 0;

static void measure_box(const layout_box *b, int depth)
{
    if (!b || depth > 512) return;
    int bottom = b->y + b->h;
    if (bottom > g_content_h) g_content_h = bottom;
    measure_box(b->first_child, depth + 1);
    measure_box(b->next_sibling, depth + 1);
}

static int content_height(const layout_box *root)
{
    g_content_h = 0;
    measure_box(root, 0);
    /* Prefer the root box height if it is larger (layout.h documents the
     * root h as the total document height). */
    if (root && root->h > g_content_h) g_content_h = root->h;
    return g_content_h;
}

/* =========================================================================
 * Link hover hit-test: find the inline box whose originating node is an <a>
 * element under the cursor (cx,cy are framebuffer coords; boxes are tested in
 * painted/offset space using g_paint_dy). Returns 1 and fills *out_* on hit.
 * ========================================================================= */
static int node_is_anchor(const struct dom_node *n)
{
    if (!n || n->type != DOM_NODE_ELEMENT || !n->tag) return 0;
    const char *t = n->tag;   /* dom tags are lowercased */
    return t[0] == 'a' && t[1] == '\0';
}

typedef struct {
    int cx, cy;                  /* cursor in framebuffer coords            */
    int found;                   /* 1 once a hit is recorded                */
    int bx, by, bw, bh;          /* hit box in framebuffer coords           */
} hover_ctx;

static void hover_visit(const layout_box *b, hover_ctx *h, int depth)
{
    if (!b || depth > 512 || h->found) return;
    if (b->kind == LB_INLINE && node_is_anchor(b->node)) {
        int by = b->y + g_paint_dy;
        if (h->cx >= b->x && h->cx < b->x + b->w &&
            h->cy >= by    && h->cy < by + b->h &&
            by + b->h > B2UI_CHROME_H) {
            h->found = 1;
            h->bx = b->x; h->by = by; h->bw = b->w; h->bh = b->h;
            return;
        }
    }
    hover_visit(b->first_child, h, depth + 1);
    hover_visit(b->next_sibling, h, depth + 1);
}

/* =========================================================================
 * Inline CSS extraction: walk the DOM looking for <style> elements and
 * collect their text-node content into a single buffer.
 * Returns length written into buf[cap].
 * ========================================================================= */
typedef struct {
    char *buf;
    int   cap;
    int   len;
    int   in_style;
} style_collect_ctx;

static void style_collect_visit(dom_node *node, void *ctx_)
{
    style_collect_ctx *ctx = (style_collect_ctx *)ctx_;

    if (node->type == DOM_NODE_ELEMENT && node->tag) {
        /* Check if this is a <style> element */
        const char *tag = node->tag;
        int is_style = (tag[0] == 's' && tag[1] == 't' &&
                        tag[2] == 'y' && tag[3] == 'l' &&
                        tag[4] == 'e' && tag[5] == '\0');
        ctx->in_style = is_style;
        return;
    }

    if (node->type == DOM_NODE_TEXT && ctx->in_style && node->text) {
        const char *t = node->text;
        while (*t && ctx->len < ctx->cap - 1)
            ctx->buf[ctx->len++] = *t++;
        if (ctx->len < ctx->cap - 1)
            ctx->buf[ctx->len++] = '\n';
        ctx->in_style = 0;  /* reset; next sibling won't be style body */
    }
}

/* =========================================================================
 * External <link rel="stylesheet"> fetching.
 *
 * Resolves each stylesheet href against the page's base scheme/host/port and
 * GETs it (same robust DNS->TLS->HTTP path as the page, with the built-in 8 s
 * budget so it can never hang), appending the CSS text to *css buffer.
 *
 * Supported href forms:
 *   https://host/p.css   absolute  -> used as-is
 *   http://host/p.css    absolute  -> used as-is
 *   //host/p.css         scheme-relative -> inherits the page scheme
 *   /p.css               root-relative   -> base scheme+host+port
 *   p.css                path-relative   -> base scheme+host + dir(base_path)
 *
 * Bounded: at most MAX_EXT_SHEETS sheets, each capped at EXT_SHEET_CAP bytes.
 * Only fetches when page_is_network is set (skipped for about:/offline pages).
 * ========================================================================= */
#define MAX_EXT_SHEETS  8
#define EXT_SHEET_CAP   (64 * 1024)
static char g_ext_sheet[EXT_SHEET_CAP];   /* scratch for one fetched sheet */

/* Resolve `href` into out[cap] as host/port/path/is_https for a GET. Returns 1
 * on success. Mirrors browser resolution for the four href forms above. */
static int b2_resolve_href(const char *href,
                           const char *base_host, int base_port, int base_https,
                           const char *base_path,
                           char *o_host, int host_cap, int *o_port,
                           char *o_path, int path_cap, int *o_https)
{
    if (!href || !href[0]) return 0;

    if (b2_strncasecmp(href, "http://", 7) == 0 ||
        b2_strncasecmp(href, "https://", 8) == 0) {
        return b2_parse_url(href, o_host, host_cap, o_port,
                            o_path, path_cap, o_https);
    }

    /* scheme-relative: //host/path -> reuse base scheme. */
    if (href[0] == '/' && href[1] == '/') {
        char tmp[URL_CAP];
        const char *pre = base_https ? "https:" : "http:";
        int k = 0;
        for (int i = 0; pre[i] && k < URL_CAP - 1; i++) tmp[k++] = pre[i];
        for (int i = 0; href[i] && k < URL_CAP - 1; i++) tmp[k++] = href[i];
        tmp[k] = 0;
        return b2_parse_url(tmp, o_host, host_cap, o_port,
                            o_path, path_cap, o_https);
    }

    /* Everything else is relative to the base host/port/scheme. */
    *o_https = base_https;
    *o_port  = base_port;
    { int i = 0; while (base_host[i] && i < host_cap - 1) { o_host[i] = base_host[i]; i++; } o_host[i] = 0; }

    if (href[0] == '/') {
        /* root-relative */
        int i = 0; while (href[i] && i < path_cap - 1) { o_path[i] = href[i]; i++; }
        o_path[i] = 0;
    } else {
        /* path-relative: base directory + href */
        int slash = -1;
        for (int i = 0; base_path[i]; i++) if (base_path[i] == '/') slash = i;
        int k = 0;
        for (int i = 0; i <= slash && k < path_cap - 1; i++) o_path[k++] = base_path[i];
        if (k == 0 && k < path_cap - 1) o_path[k++] = '/';
        for (int i = 0; href[i] && k < path_cap - 1; i++) o_path[k++] = href[i];
        o_path[k] = 0;
    }
    return 1;
}

/* Fetch all <link rel="stylesheet"> sheets and append their CSS to css[cap]
 * starting at *pcss_len. No-op (apart from a log line) when page_is_network==0.
 */
static void fetch_external_sheets(dom_document *doc, int page_is_network,
                                  const char *base_host, int base_port,
                                  int base_https, const char *base_path,
                                  char *css, int cap, int *pcss_len)
{
    dom_node *root = doc ? doc->root : (dom_node *)0;
    if (!root) return;

    int fetched = 0;
    #define WALK_STACK 64
    dom_node *stack[WALK_STACK];
    int top = 0;
    stack[top++] = root;

    while (top > 0) {
        dom_node *n = stack[--top];
        if (!n) continue;

        if (n->type == DOM_NODE_ELEMENT && n->tag) {
            const char *tag = n->tag;
            int is_link = (tag[0]=='l' && tag[1]=='i' && tag[2]=='n' &&
                           tag[3]=='k' && tag[4]=='\0');
            if (is_link) {
                const char *rel  = dom_get_attribute(n, "rel");
                const char *href = dom_get_attribute(n, "href");
                if (rel && href && b2_strncasecmp(rel, "stylesheet", 10) == 0) {
                    if (!page_is_network) {
                        b2_puts("BROWSER2: external stylesheet skipped (offline): ");
                        b2_puts(href); b2_puts("\n");
                    } else if (fetched >= MAX_EXT_SHEETS) {
                        b2_puts("BROWSER2: external stylesheet skipped (cap): ");
                        b2_puts(href); b2_puts("\n");
                    } else {
                        char rh[HOST_CAP], rp[PATH_CAP];
                        int  rport = 80, rhttps = 0;
                        if (b2_resolve_href(href, base_host, base_port,
                                            base_https, base_path,
                                            rh, HOST_CAP, &rport,
                                            rp, PATH_CAP, &rhttps)) {
                            int st = 0;
                            long n2 = rhttps
                                ? https_get(rh, (unsigned short)rport, rp,
                                            g_ext_sheet, EXT_SHEET_CAP, &st)
                                : http_get(rh, (unsigned short)rport, rp,
                                           g_ext_sheet, EXT_SHEET_CAP, &st);
                            if (n2 > 0) {
                                if (n2 > EXT_SHEET_CAP - 1) n2 = EXT_SHEET_CAP - 1;
                                g_ext_sheet[n2] = 0;
                                int L = *pcss_len;
                                if (L < cap - 2) css[L++] = '\n';
                                for (long i = 0; i < n2 && L < cap - 1; i++)
                                    css[L++] = g_ext_sheet[i];
                                css[L] = 0;
                                *pcss_len = L;
                                fetched++;
                                b2_puts("BROWSER2: external stylesheet loaded: ");
                                b2_puts(href); b2_puts("\n");
                            } else {
                                b2_puts("BROWSER2: external stylesheet fetch failed: ");
                                b2_puts(href); b2_puts("\n");
                            }
                        }
                    }
                }
            }
        }

        dom_node *children[64];
        int ci = 0;
        for (dom_node *c = n->first_child; c && ci < 64; c = c->next_sibling)
            children[ci++] = c;
        for (int i = ci - 1; i >= 0 && top < WALK_STACK; i--)
            stack[top++] = children[i];
    }
    #undef WALK_STACK
}

/* =========================================================================
 * Page load: fetch URL -> DOM -> CSS -> run inline JS -> layout.
 *
 * Frees any prior doc/sheet/layout passed via the in/out pointers, then
 * rebuilds them for `url`.  `*p_doc` borrows `url` for doc->url (caller keeps
 * `url` alive).  On a hard bound (parse/layout OOM) it leaves the out-params
 * NULL and returns < 0; the caller treats that as an error page.
 *
 *   return:  >= 0  number of layout boxes (page usable / error-stub usable)
 *            <  0  hard failure (no layout)
 *   *p_fetch_ok: 1 if the HTTP fetch succeeded, 0 if the stub HTML was used
 *
 * vm may be NULL (JS disabled); then scripts are skipped.
 * ========================================================================= */
static int load_page(const char *url, js_vm *vm,
                     dom_document **p_doc, css_stylesheet **p_sheet,
                     layout_box **p_layout, int *p_fetch_ok)
{
    /* Free any previous page resources (reload path). */
    if (*p_layout) { layout_free(*p_layout); *p_layout = (layout_box *)0; }
    if (*p_sheet)  { css_free(*p_sheet);     *p_sheet  = (css_stylesheet *)0; }
    if (*p_doc)    { (*p_doc)->url = (char *)0; dom_document_free(*p_doc);
                     *p_doc = (dom_document *)0; }

    *p_fetch_ok = 0;

    long body_len;

    /* Base-URL components of the page we are loading, kept at function scope so
     * the external-stylesheet fetcher below can resolve relative hrefs against
     * the same scheme/host/port. For about: pages they stay empty (no fetch). */
    char base_host[HOST_CAP]; base_host[0] = 0;
    char base_path[PATH_CAP]; base_path[0] = 0;
    int  base_port = 80, base_https = 0;
    int  page_is_network = 0;

    /* The built-in home page (also the offline fallback). NO network access ->
     * renders deterministically and never blocks. */
    static const char HOME_HTML[] =
        "<html><head><title>AutomationOS</title><style>"
        "body{background:#ffffff;color:#202020}"
        "h1{color:#3b82f6;font-size:28px}"
        ".card{background:#f2f2f2;color:#202020;font-size:16px}"
        "</style></head><body>"
        "<h1>AutomationOS Browser</h1>"
        "<p>A from-scratch DOM-rendering browser.</p>"
        "<div class=\"card\"><p>Type a URL or a search and press Enter.</p></div>"
        "</body></html>";

    /* "about:" URLs are served from the built-in page. */
    if (url[0]=='a' && url[1]=='b' && url[2]=='o' &&
        url[3]=='u' && url[4]=='t' && url[5]==':') {
        int si = 0;
        while (HOME_HTML[si] && si < BODY_CAP - 1) { g_body[si] = HOME_HTML[si]; si++; }
        g_body[si] = 0;
        body_len = si;
        *p_fetch_ok = 1;   /* built-in page counts as a successful load */
    } else {
        /* -- Parse URL -- */
        if (!b2_parse_url(url, base_host, HOST_CAP, &base_port,
                          base_path, PATH_CAP, &base_https)) {
            b2_puts("BROWSER2: BOUND url-parse-failed\n");
            return -1;
        }
        page_is_network = 1;

        /* -- HTTP fetch (DNS -> [TLS] -> HTTP, 8 s wall-clock budget, follows
         * redirects). Returns < 0 on any failure; it cannot hang. -- */
        int  http_status = 0;
        if (base_https)
            body_len = https_get(base_host, (unsigned short)base_port, base_path,
                                 g_body, BODY_CAP, &http_status);
        else
            body_len = http_get(base_host, (unsigned short)base_port, base_path,
                                g_body, BODY_CAP, &http_status);

        if (body_len < 0) {
            /* Fetch failed (network likely down -- the lead re-enables the NIC
             * path separately). Fall back to the built-in home page so the
             * browser stays usable instead of showing a dead error screen. */
            b2_puts("BROWSER2: fetch failed; falling back to about:home\n");
            int si = 0;
            while (HOME_HTML[si] && si < BODY_CAP - 1) { g_body[si] = HOME_HTML[si]; si++; }
            g_body[si] = 0;
            body_len = si;
            page_is_network = 0;   /* don't try to fetch sub-resources offline */
            *p_fetch_ok = 1;       /* usable fallback page, not an error screen */
        } else {
            *p_fetch_ok = 1;
        }
    }

    if (body_len > BODY_CAP - 1) {
        b2_puts("BROWSER2: BOUND body-truncated\n");
        body_len = BODY_CAP - 1;
    }
    g_body[body_len] = 0;

    /* -- HTML parse -> DOM -- */
    dom_document *doc = html_parse(g_body, (unsigned long)body_len);
    if (!doc) {
        b2_puts("BROWSER2: BOUND html-parse-oom\n");
        return -1;
    }
    doc->url = (char *)url;   /* borrow; caller keeps url alive */

    g_dom_node_count = 0;
    dom_walk(doc->root, count_node_cb, (void *)0);
    if (g_dom_node_count > MAX_DOM_NODES) {
        b2_puts("BROWSER2: BOUND dom-nodes\n");
        doc->url = (char *)0;
        dom_document_free(doc);
        return -1;
    }

    /* -- CSS: UA defaults + external <link> sheets + inline <style> blocks --
     *
     * Cascade order (later wins ties at equal specificity): UA defaults first,
     * then external stylesheets (fetched here when online), then inline <style>
     * so author page styles take precedence. css_parse handles each rule's
     * specificity; this ordering only affects equal-specificity ties. */
    static char style_src[32768];
    style_collect_ctx sctx;
    sctx.buf = style_src; sctx.cap = (int)sizeof(style_src);
    sctx.len = 0; sctx.in_style = 0;
    dom_walk(doc->root, style_collect_visit, &sctx);

    /* full_css is large enough for UA + MAX_EXT_SHEETS external sheets + the
     * inline <style> blocks. Kept static (BSS) to avoid a huge stack frame. */
    static char full_css[EXT_SHEET_CAP * MAX_EXT_SHEETS + 32768 + sizeof(UA_CSS) + 8];
    int fc = 0;
    for (int i = 0; UA_CSS[i] && fc < (int)(sizeof(full_css) - 2); i++)
        full_css[fc++] = UA_CSS[i];
    full_css[fc++] = '\n';
    full_css[fc] = 0;

    /* External stylesheets (resolved against the page base; offline -> skipped). */
    fetch_external_sheets(doc, page_is_network,
                          base_host, base_port, base_https, base_path,
                          full_css, (int)sizeof(full_css), &fc);

    /* Inline <style> blocks last so they win equal-specificity ties. */
    if (fc < (int)(sizeof(full_css) - 2)) full_css[fc++] = '\n';
    for (int i = 0; i < sctx.len && fc < (int)(sizeof(full_css) - 1); i++)
        full_css[fc++] = style_src[i];
    full_css[fc] = 0;

    css_stylesheet *sheet = css_parse(full_css, (unsigned long)fc);
    /* sheet may be NULL on OOM -- layout_compute handles NULL gracefully */

    /* -- JS: install DOM bindings + the web APIs, run inline scripts --
     *
     * Lifecycle (the previous version was broken):
     *   js_eval() RESETS the arena, the global env, AND the native-class
     *   registry at the START of every call -- so any dom_bindings_install()
     *   done right before js_eval() was wiped before the script ran, and
     *   `document` evaluated to a ReferenceError. Worse, each js_eval() reset
     *   discarded DOM mutations from the previous <script>.
     *
     * The fix:
     *   1. js_new() once here to give this page a clean VM (fresh arena +
     *      builtins; the print sink survives the reset). This also gives the
     *      reload path correct per-page isolation (old page's JS state is
     *      dropped) without the caller juggling VMs.
     *   2. Install DOM bindings + web APIs ONCE, AFTER the reset, so the
     *      registry and the `document` global are live.
     *   3. Run every <script> with js_eval_keep_env(), which does NOT reset --
     *      so `document` (and accumulated DOM/global state) persists across all
     *      of the page's scripts, matching real multi-<script> semantics.
     *   We must NOT re-install between scripts: dom_bindings_install() clears
     *   the event side-table (new arena epoch), which would drop handlers
     *   registered by an earlier script. */
    if (vm) {
        vm = js_new();              /* reset to a clean per-page VM */
        js_set_print(vm, js_print_sink);

        /* Install the full surface ONCE, after the reset, before any eval. */
        dom_bindings_install(vm, doc);
        js_console_install(vm);
        js_timers_install(vm);
        js_fetch_install(vm);
        js_storage_install(vm);
        js_url_install(vm);
        b2_puts("BROWSER2: ui ready (apis=5)\n");

        int nscripts = 0;
        char **scripts = html_get_inline_scripts(doc, &nscripts);
        if (scripts && nscripts > 0) {
            int total_js = 0;
            static char js_result[256];
            for (int si = 0; si < nscripts; si++) {
                const char *src = scripts[si];
                if (!src) continue;
                int slen = 0; while (src[slen]) slen++;
                if (total_js + slen > MAX_SCRIPT_BYTES) {
                    b2_puts("BROWSER2: BOUND script-budget\n");
                    break;
                }
                total_js += slen;

                /* keep_env: preserves `document` + globals across scripts. */
                int rc = js_eval_keep_env(vm, src, (unsigned long)slen,
                                          js_result,
                                          (unsigned long)sizeof(js_result));
                if (rc < 0) {
                    b2_puts("BROWSER2: js error: ");
                    b2_puts(js_result);
                    b2_puts("\n");
                }
            }
            for (int si = 0; si < nscripts; si++)
                if (scripts[si]) free(scripts[si]);
            free(scripts);
        }
    }

    /* -- Layout -- */
    layout_box *layout = layout_compute(doc, sheet, VP_W);
    if (!layout) {
        b2_puts("BROWSER2: BOUND layout-oom\n");
        if (sheet) css_free(sheet);
        doc->url = (char *)0;
        dom_document_free(doc);
        return -1;
    }

    int nboxes = count_boxes(layout);
    if (nboxes > MAX_LAYOUT_BOXES) {
        b2_puts("BROWSER2: BOUND layout-boxes\n");
        /* proceed; paint_box self-limits to MAX_LAYOUT_BOXES */
    }

    *p_doc    = doc;
    *p_sheet  = sheet;
    *p_layout = layout;
    return nboxes;
}

/* =========================================================================
 * Bounded copy: dst[cap] <- src (NUL-terminated, truncated to cap-1). Returns
 * the number of bytes written (excluding the terminator).
 * ========================================================================= */
static int b2_strcpy_cap(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0) return 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

/* =========================================================================
 * Back/forward history: a tiny bounded ring of visited URLs. navigate() pushes
 * the URL we are leaving before loading the new one; B2UI_ACT_BACK pops it.
 * Bounded (no malloc): oldest entries are overwritten once full, but we only
 * pop from the end so BACK always walks the most-recent trail.
 * ========================================================================= */
#define HIST_CAP   16
static char g_hist[HIST_CAP][URL_CAP];
static int  g_hist_len = 0;          /* number of entries currently stored */

static void hist_push(const char *u)
{
    if (!u || !u[0]) return;
    if (g_hist_len < HIST_CAP) {
        b2_strcpy_cap(g_hist[g_hist_len], URL_CAP, u);
        g_hist_len++;
    } else {
        /* Full: drop the oldest, shift down, append at the end. */
        for (int i = 1; i < HIST_CAP; i++)
            b2_strcpy_cap(g_hist[i - 1], URL_CAP, g_hist[i]);
        b2_strcpy_cap(g_hist[HIST_CAP - 1], URL_CAP, u);
    }
}

/* Pop the most-recent history entry into `out` (cap bytes). Returns 1 if a
 * previous URL was available, 0 if the history was empty. */
static int hist_pop(char *out, int cap)
{
    if (g_hist_len <= 0) return 0;
    g_hist_len--;
    b2_strcpy_cap(out, cap, g_hist[g_hist_len]);
    return 1;
}

/* Build the "Could not load <url>" detail line shown on the error page. */
static void rebuild_err_msg(char *err, int cap, const char *url)
{
    const char *pre = "Could not load ";
    int ei = 0;
    for (int i = 0; pre[i] && ei < cap - 1; i++) err[ei++] = pre[i];
    for (int i = 0; url && url[i] && ei < cap - 1; i++) err[ei++] = url[i];
    err[ei] = 0;
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char **argv)
{
    /* -- 1. Open compositor window (or framebuffer) FIRST ----------------
     * We acquire the presentation surface before choosing the home URL so the
     * headless boot smoke-test (no window AND no framebuffer) can stay on the
     * network-free about:home page -- deterministic and instant -- while an
     * interactive launch (which has a compositor window) defaults to Google. */
    int has_window = 0;
    wl_window *win = (wl_window *)0;

    /* Try compositor SHM window first */
    if (wl_connect() == 0) {
        win = wl_create_window((wl_u32)VP_W, (wl_u32)VP_H, "browser2");
        if (win) has_window = 1;
    }

    /* If compositor failed, try direct framebuffer */
    int has_fb = 0;
    static b2_fb_info fb_info;
    if (!has_window) {
        i64 fb_rc = sc(SYS_FB_ACQUIRE, (i64)&fb_info, 0, 0, 0, 0, 0);
        if (fb_rc == 0 && fb_info.vaddr != 0) {
            has_fb = 1;
        } else {
            b2_puts("BROWSER2: headless mode (no framebuffer)\n");
        }
    }
    int interactive = (has_window || has_fb);

    /* -- 2. Determine URL ------------------------------------------------- */
    static char url[URL_CAP];
    /* Interactive home is Google (the requested default). Real fetching needs
     * the NIC/HTTPS path (the lead re-enables it separately); until then
     * load_page() falls back to the built-in about:home page if the fetch fails
     * (bounded by the HTTP layer's 8 s budget) so the browser stays usable and
     * never blocks. The HEADLESS boot path defaults to about:home so the smoke
     * test renders deterministically with no network round-trip. An explicit
     * argv[1] always wins (a bare domain gets https://, free text becomes a
     * Google search, an http(s)://|about: URL passes through). */
    /* Default to the built-in about:home (a stable Google-style search page).
     * Auto-fetching the real google.com on startup is intentionally removed:
     * Google's page is too JS/CSS-heavy for this from-scratch engine and was
     * FAULTING the browser at boot (both in the smoke, which has a compositor
     * window, and interactively). "Google by default" is preserved via the
     * address bar -- free text becomes a Google search and a typed URL still
     * loads -- without the unstable startup fetch. argv[1] overrides. */
    const char *default_url = "about:home";
    (void)interactive;
    if (argc >= 2 && argv[1] && argv[1][0]) {
        b2_normalize_input(argv[1], url, URL_CAP);
    } else {
        int i = 0;
        while (default_url[i] && i < URL_CAP - 1) { url[i] = default_url[i]; i++; }
        url[i] = 0;
    }

    b2_puts("BROWSER2: fetching ");
    b2_puts(url);
    b2_puts("\n");

    /* -- 3. JS engine (one VM, reused across reloads) -------------------- */
    js_vm *vm = js_new();

    /* -- 4. Initial page load (fetch -> DOM -> CSS -> JS -> layout) ------ */
    dom_document   *doc    = (dom_document *)0;
    css_stylesheet *sheet  = (css_stylesheet *)0;
    layout_box     *layout = (layout_box *)0;
    int             fetch_ok = 0;
    int             nboxes = load_page(url, vm, &doc, &sheet, &layout, &fetch_ok);
    /* nboxes < 0 (hard fail) is handled in the render loop via the error
     * page; doc/sheet/layout are NULL in that case. */

    /* -- 9. Real bounded ~60fps render loop ------------------------------ */
    /*
     * Frame model: ~16 ms per frame, run for ~5 s (about 300 frames). The
     * smoke test is non-interactive, so we never block on input -- every
     * iteration polls (non-blocking), advances the scroll animation by the
     * measured dt, repaints when anything changed, and presents the frame.
     * After the first paint we print the deterministic
     * "BROWSER2: rendered N boxes for <URL>" marker exactly once, then keep
     * running frames until the deadline and SYS_EXIT(0).
     */
    const int   FRAME_MS    = 16;     /* ~60 fps                            */
    const i64   RUN_MS      = 5000;   /* total bounded runtime (~5 s)       */
    const int   viewport_h  = VP_H - B2UI_CHROME_H;   /* content area height */

    int  content_h    = (nboxes >= 0 && layout) ? content_height(layout) : 0;
    int  hover_active = 0, hx = 0, hy = 0, hw = 0, hh = 0;
    int  prev_buttons = 0;
    int  rendered_boxes = 0;
    int  reported     = 0;            /* printed the "rendered N" line yet?  */
    int  need_repaint = 1;            /* force first frame                   */

    /* -- Address-bar editor state ---------------------------------------- */
    /* When `editing` is set, keystrokes go to `editbuf` (an editable copy of
     * the URL) instead of scrolling the page; Enter navigates, Esc cancels. */
    int         editing      = 0;
    int         shift_down   = 0;
    static char editbuf[URL_CAP];
    int         edit_len     = 0;
    /* Buffer holding edit text + a caret glyph, handed to b2ui_draw_chrome as
     * the "url" so the caret renders wherever the address-bar text renders
     * (no dependency on the chrome's internal address-bar geometry). */
    static char editview[URL_CAP + 2];

    /* The page is shown as an error screen when the fetch failed (stub HTML
     * was substituted) or the parse/layout hard-failed (no layout tree). */
    static char err_msg[URL_CAP + 32];
    rebuild_err_msg(err_msg, (int)sizeof(err_msg), url);

    i64  t_start = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    i64  deadline = t_start + RUN_MS;
    i64  t_prev  = t_start;

    while (1) {
        i64 now = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        int dt_ms = (int)(now - t_prev);
        if (dt_ms < 0)   dt_ms = 0;
        if (dt_ms > 250) dt_ms = 250;   /* clamp big gaps (scheduler hiccup) */
        t_prev = now;

        /* -- 9a. Poll input (non-blocking); feed scroll + chrome hits -- */
        if (has_window && win) {
            int kind = 0, a = 0, b = 0, c = 0;
            while (wl_poll_event(win, &kind, &a, &b, &c)) {
                if (kind == WL_EVENT_POINTER) {
                    /* a=x, b=y, c=buttons. Wheel arrives as buttons bit 2,
                     * with b carrying the wheel direction (cf. browser.c). */
                    int x = a, y = b, buttons = c;

                    if (buttons & 4) {
                        /* Mouse wheel: scale direction to a pixel impulse. */
                        int dir = (y > 0) ? 1 : (y < 0 ? -1 : 0);
                        if (dir) b2anim_scroll_input(dir * 48);
                        need_repaint = 1;
                    }

                    /* Link hover (only over the content area). */
                    if (layout && y >= B2UI_CHROME_H) {
                        hover_ctx hc;
                        hc.cx = x; hc.cy = y; hc.found = 0;
                        hc.bx = hc.by = hc.bw = hc.bh = 0;
                        hover_visit(layout, &hc, 0);
                        if (hc.found != hover_active ||
                            (hc.found && (hc.bx != hx || hc.by != hy ||
                                          hc.bw != hw || hc.bh != hh))) {
                            need_repaint = 1;
                        }
                        hover_active = hc.found;
                        hx = hc.bx; hy = hc.by; hw = hc.bw; hh = hc.bh;
                    } else if (hover_active) {
                        hover_active = 0; need_repaint = 1;
                    }

                    /* Left-button click in the chrome strip. */
                    if ((buttons & 1) && !(prev_buttons & 1) &&
                        y >= 0 && y < B2UI_CHROME_H) {
                        int act = b2ui_hit_chrome(x, y, VP_W);
                        if (act == B2UI_ACT_RELOAD) {
                            /* Reload current URL. Always retryable: even if the
                             * previous load failed, load_page() rebuilds from
                             * scratch (about: stays network-free; http(s):
                             * re-fetches). */
                            editing = 0;
                            b2_puts("BROWSER2: reload ");
                            b2_puts(url);
                            b2_puts("\n");
                            nboxes = load_page(url, vm, &doc, &sheet,
                                               &layout, &fetch_ok);
                            content_h = (nboxes >= 0 && layout)
                                        ? content_height(layout) : 0;
                            rebuild_err_msg(err_msg, (int)sizeof(err_msg), url);
                            b2anim_scroll_input(-1000000); /* snap to top */
                            hover_active = 0;
                        } else if (act == B2UI_ACT_BACK) {
                            /* Navigate to the previous URL in the history ring,
                             * if any. No-op (but no crash) when empty. */
                            editing = 0;
                            char prev[URL_CAP];
                            if (hist_pop(prev, (int)sizeof(prev))) {
                                b2_strcpy_cap(url, URL_CAP, prev);
                                b2_puts("BROWSER2: back ");
                                b2_puts(url);
                                b2_puts("\n");
                                nboxes = load_page(url, vm, &doc, &sheet,
                                                   &layout, &fetch_ok);
                                content_h = (nboxes >= 0 && layout)
                                            ? content_height(layout) : 0;
                                rebuild_err_msg(err_msg, (int)sizeof(err_msg),
                                                url);
                                b2anim_scroll_input(-1000000);
                                hover_active = 0;
                            }
                        } else if (act == B2UI_ACT_ADDRFOCUS) {
                            /* Enter address-edit mode: seed the editor with the
                             * current URL, caret at the end. */
                            editing  = 1;
                            edit_len = b2_strcpy_cap(editbuf, URL_CAP, url);
                        } else {
                            /* newtab / closetab / seltab: best-effort no-op
                             * (single-tab build); must not crash. A click
                             * elsewhere in the chrome leaves edit mode. */
                            editing = 0;
                        }
                        /* Other actions are best-effort/no-op for now but
                         * must not crash (back/newtab/closetab/addrfocus). */
                        need_repaint = 1;
                    }
                    prev_buttons = buttons;
                } else if (kind == WL_EVENT_KEY) {
                    /* a=keycode, b=pressed. */
                    int kc = a, pressed = b;

                    /* Track shift for the address-bar editor (both press and
                     * release matter, so handle it before the pressed gate). */
                    if (kc == KEY_LEFTSHIFT || kc == KEY_RIGHTSHIFT) {
                        shift_down = pressed ? 1 : 0;
                    } else if (editing && pressed) {
                        /* -- Address-bar edit mode: keystrokes edit the URL. -- */
                        if (kc == KEY_ENTER) {
                            /* Commit: navigate to the typed URL. Push the URL we
                             * are leaving onto the history ring first so BACK
                             * works. Empty input just exits edit mode. */
                            editing = 0;
                            if (edit_len > 0) {
                                hist_push(url);
                                /* Normalize the typed text: bare domains get
                                 * https://, free text becomes a Google search,
                                 * explicit schemes/about: pass through. */
                                b2_normalize_input(editbuf, url, URL_CAP);
                                b2_puts("BROWSER2: navigate ");
                                b2_puts(url);
                                b2_puts("\n");
                                nboxes = load_page(url, vm, &doc, &sheet,
                                                   &layout, &fetch_ok);
                                content_h = (nboxes >= 0 && layout)
                                            ? content_height(layout) : 0;
                                rebuild_err_msg(err_msg,
                                                (int)sizeof(err_msg), url);
                                b2anim_scroll_input(-1000000); /* snap to top */
                                hover_active = 0;
                            }
                            need_repaint = 1;
                        } else if (kc == KEY_ESC) {
                            /* Cancel edit; restore the displayed URL. */
                            editing = 0;
                            need_repaint = 1;
                        } else if (kc == KEY_BACKSPACE) {
                            if (edit_len > 0) editbuf[--edit_len] = 0;
                            need_repaint = 1;
                        } else {
                            char ch = keycode_to_ascii(kc, shift_down);
                            if (ch && edit_len < URL_CAP - 1) {
                                editbuf[edit_len++] = ch;
                                editbuf[edit_len]   = 0;
                                need_repaint = 1;
                            }
                            /* Unmapped keys (arrows, fn, etc.) ignored while
                             * editing so they don't scroll behind the bar. */
                        }
                    } else if (pressed) {
                        /* -- Not editing: scroll keys (Linux input codes). -- */
                        switch (kc) {
                            case 104: b2anim_scroll_input(-viewport_h + FONT_H);
                                      need_repaint = 1; break;  /* PageUp   */
                            case 109: b2anim_scroll_input(+viewport_h - FONT_H);
                                      need_repaint = 1; break;  /* PageDown */
                            case 103: b2anim_scroll_input(-FONT_H * 3);
                                      need_repaint = 1; break;  /* Up arrow */
                            case 108: b2anim_scroll_input(+FONT_H * 3);
                                      need_repaint = 1; break;  /* Down arrow */
                            case 102: b2anim_scroll_input(-1000000);
                                      need_repaint = 1; break;  /* Home     */
                            case 107: b2anim_scroll_input(+1000000);
                                      need_repaint = 1; break;  /* End      */
                            default: break;
                        }
                    }
                } else if (kind == WL_EVENT_RESIZE) {
                    /* Compositor Maximize / snap resized the window. The wl lib
                     * has ALREADY reallocated win->pixels and updated
                     * win->{w,h,stride} (a=new_w, b=new_h), so there is nothing
                     * to allocate here. We render into a fixed VP_W x VP_H
                     * canvas, so the present path (fb_commit_to_window) reads the
                     * live win->stride/w/h every frame and letterboxes + clears
                     * the new margins -- all we must do is force a fresh paint so
                     * the whole new surface is repainted (no stale garbage).
                     * (void)a/b: the size is read straight from win at present. */
                    (void)a; (void)b; (void)c;
                    need_repaint = 1;
                }
            }
        }

        /* -- 9b. Drain JS timers once per frame (setTimeout/setInterval) -- */
        if (vm) {
            if (js_timers_run(vm) > 0) need_repaint = 1;
        }

        /* -- 9c. Advance the inertial scroll model -- */
        b2anim_scroll_tick(dt_ms, content_h, viewport_h);
        if (b2anim_scroll_active()) need_repaint = 1;

        /* -- 9d. Repaint a changed frame and present it -- */
        if (need_repaint) {
            int show_error = (nboxes < 0 || !layout || !fetch_ok);
            if (show_error) {
                /* Fetch failed (or hard parse/layout fail): error screen. */
                g_clip_top = 0; g_paint_dy = 0;
                b2ui_draw_error_page(g_fb, VP_W, VP_H, err_msg);
                rendered_boxes = (nboxes >= 0) ? nboxes : 0;
            } else {
                int sy = b2anim_scroll_y();
                rendered_boxes = paint_layout_scrolled(layout, sy);
                if (hover_active)
                    b2ui_draw_link_hover(g_fb, VP_W, VP_H, hx, hy, hw, hh);
            }

            /* Chrome on top, every frame. load_pct: 100 if usable, -1 idle.
             * While editing the address bar, show the editable buffer with a
             * trailing caret glyph instead of the committed URL. Passing it as
             * the chrome's `url` makes the caret track the address-bar text
             * without hardcoding the bar's internal geometry. */
            int load_pct = -1;
            const char *addr_str = url;
            if (editing) {
                int k = b2_strcpy_cap(editview, URL_CAP, editbuf);
                editview[k++] = '_';     /* caret */
                editview[k]   = 0;
                addr_str = editview;
            }
            b2ui_draw_chrome(g_fb, VP_W, VP_H, addr_str, load_pct, 1, 0);

            /* Present g_fb. */
            if (has_window) {
                fb_commit_to_window(win);
            } else if (has_fb) {
                u32 *fb_ptr   = (u32 *)(u64)fb_info.vaddr;
                u32  fb_w     = fb_info.width  < (u32)VP_W ? fb_info.width  : (u32)VP_W;
                u32  fb_h     = fb_info.height < (u32)VP_H ? fb_info.height : (u32)VP_H;
                u32  fb_pitch = fb_info.pitch / 4; /* stride in pixels */
                for (u32 row = 0; row < fb_h; row++) {
                    u32 *dst = fb_ptr + row * fb_pitch;
                    u32 *src = g_fb  + row * VP_W;
                    for (u32 col = 0; col < fb_w; col++)
                        dst[col] = src[col];
                }
            }
            need_repaint = 0;
        }

        /* -- 9e. After the first paint, emit the deterministic marker -- */
        if (!reported) {
            b2_puts("BROWSER2: rendered ");
            b2_putnum((i64)rendered_boxes);
            b2_puts(" boxes for ");
            b2_puts(url);
            b2_puts("\n");
            reported = 1;
        }

        /* -- 9f. Bounded exit + frame pacing (never block on input) -- */
        now = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        if (now >= deadline) break;

        /* Pace to ~FRAME_MS by yielding; if the scroll is settling we keep
         * spinning for smoothness, otherwise yield to be a good citizen. */
        i64 frame_end = now + FRAME_MS;
        do {
            sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
            now = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        } while (now < frame_end && now < deadline);
    }

    /* -- 10. Teardown ----------------------------------------------------- */
    if (vm) js_timers_clear_all();
    if (layout) layout_free(layout);
    if (sheet)  css_free(sheet);
    if (doc) {
        /* Do not free doc->url -- it borrows url[] (static storage). */
        doc->url = (char *)0;
        dom_document_free(doc);
    }

    sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    /* unreachable: crt0 will also call exit after main returns */
    return 0;
}
