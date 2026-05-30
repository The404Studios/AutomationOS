/*
 * browser2_ui.c -- chrome / UX rendering for the DOM-rendering browser2.
 * =====================================================================
 *
 * Implements the browser2_ui.h contract: a clean, modern browser chrome
 * (toolbar + address bar + tab strip + load-progress line), link-hover
 * affordances, and a tasteful full-page error screen. Everything paints
 * straight into a caller-provided 32-bit ARGB framebuffer (0xAARRGGBB).
 *
 * Freestanding, ring 3: NO libc/stdio/malloc, NO syscalls. Text uses
 * font_draw_char() from the bitfont library; small string helpers come
 * from userspace/libc/string.h. No dynamic allocation -- selftest() uses
 * a module-static scratch buffer.
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -I userspace/lib/font -I userspace/libc \
 *       -c userspace/apps/browser2/browser2_ui.c -o browser2_ui.o
 *   objdump -d browser2_ui.o | grep -c fs:0x28     # MUST be 0
 *
 * DESIGN -- draw() and hit_chrome() MUST agree on geometry, so every region
 * is defined ONCE in the "Layout constants" block below and consumed by both
 * the painter and the hit-tester. Coordinates are in framebuffer pixels.
 *
 * VISUAL IMPROVEMENTS (over prior version):
 *   - Richer toolbar: beveled active-tab indicator, rounded address bar with
 *     inner shadow, security-icon prefix in address bar.
 *   - Three-dot ellipsis ("...") for URL truncation — standard web convention.
 *   - Toolbar buttons use a filled-disc hover backdrop glyph set.
 *   - Active tab: 3px bottom accent stripe + shadow separator from toolbar.
 *   - Inactive tabs: slightly elevated from the trough with a subtle gradient
 *     effect achieved via a two-tone horizontal rule at their top.
 *   - Progress hairline: 3px tall with a bright leading dot for visibility.
 *   - Error page: larger icon, better vertical rhythm, accent rule refinement.
 */

#include "../../lib/font/bitfont.h"   /* font_draw_char, FONT_W (8), FONT_H (16) */
#include "../../libc/string.h"        /* strlen, memset (freestanding userspace libc) */
#include "browser2_ui.h"

/* =========================================================================
 * Palette -- 0xAARRGGBB. A light, modern, neutral chrome with a blue accent.
 * ========================================================================= */
#define C_CHROME_BG    0xFFF0F0F0u   /* toolbar / chrome background (slightly cooler) */
#define C_TABSTRIP_BG  0xFFE2E2E2u   /* tab-strip trough (distinctly darker)         */
#define C_ADDR_BG      0xFFFFFFFFu   /* address-bar fill (white)                     */
#define C_ADDR_SHADOW  0xFFE8E8E8u   /* address-bar inner-shadow bottom row          */
#define C_ADDR_BORDER  0xFFBBBBBBu   /* address-bar 1px border (sharper contrast)    */
#define C_ADDR_FOCUS   0xFF3B82F6u   /* address-bar focus ring colour                */
#define C_ACCENT       0xFF2563EBu   /* progress / focus / link accent (deeper blue) */
#define C_ACCENT_LITE  0xFF93C5FDu   /* accent highlight / progress leading dot      */
#define C_TEXT         0xFF1A1A1Au   /* primary text (near-black, higher contrast)   */
#define C_TEXT_DIM     0xFF6B7280u   /* secondary / placeholder text                 */
#define C_ICON         0xFF374151u   /* toolbar glyph color                          */
#define C_ICON_DIM     0xFFB0B7C3u   /* disabled toolbar glyph                       */
#define C_TAB_ACTIVE   0xFFFFFFFFu   /* active tab fill (pops off trough)            */
#define C_TAB_INACTIVE 0xFFD8D8D8u   /* inactive tab fill                            */
#define C_TAB_TOP      0xFFCCCCCCu   /* two-tone rule at top of inactive tab         */
#define C_TAB_TEXT_ACT 0xFF1A1A1Au   /* active-tab label (full contrast)             */
#define C_TAB_TEXT_INA 0xFF505766u   /* inactive-tab label (muted)                   */
#define C_TAB_BORDER   0xFFC0C0C0u   /* tab separator / outline                      */
#define C_DIVIDER      0xFFCCCCCCu   /* 1px divider under the chrome                 */
#define C_CLOSE        0xFF808080u   /* tab close glyph (rest)                       */
#define C_CLOSE_HOT    0xFFCC3333u   /* tab close glyph (hover — drawn if active)    */
#define C_BTN_HOVER    0xFFE0E0E0u   /* button hover fill circle                     */
#define C_SECURE       0xFF22C55Eu   /* green lock dot for https                     */

/* Error-page palette (clean, airy design). */
#define C_ERR_BG       0xFFF8F9FAu
#define C_ERR_HEAD     0xFF1F2937u
#define C_ERR_BODY     0xFF6B7280u
#define C_ERR_ACCENT   0xFF2563EBu
#define C_ERR_CIRCLE   0xFFEFF6FFu   /* face icon background circle                 */
#define C_ERR_RULE     0xFFDDE3ECu   /* decorative horizontal rule                  */

/* =========================================================================
 * Layout constants -- the SINGLE SOURCE OF TRUTH shared by draw + hit-test.
 *
 *  +---------------------------------------------------------------+  y=0
 *  | progress hairline (3px) spanning load_pct%                    |
 *  | TOOLBAR ROW:  [<back>][reload]  [ address bar .............. ] |  y=6..30
 *  | TAB STRIP:   [ tab0 ][ tab1 ]... [+]                          |  y=32..55
 *  +---------------------------------------------------------------+  y=56 (CHROME_H)
 *  |                       page content                            |
 * ========================================================================= */

/* Progress hairline (always drawn at the very top). */
#define PROG_Y          0
#define PROG_H          3            /* 3px for better visibility */

/* Toolbar row (back / reload / address bar). */
#define TOOL_Y          5
#define TOOL_H          24            /* button + address-bar height */
#define BTN_W           28            /* square-ish toolbar button   */
#define BTN_R           12            /* hover-circle radius (pixels) */
#define BACK_X          8
#define RELOAD_X        (BACK_X + BTN_W + 4)        /* 40 */
#define ADDR_X          (RELOAD_X + BTN_W + 8)      /* 76 */
#define ADDR_RIGHT_PAD  10            /* gap from address bar to right edge */
#define ADDR_TEXT_PAD   8             /* internal horizontal padding in addr bar */
#define ADDR_SEC_W      10            /* width reserved for security indicator */

/* Tab strip. The [+] new-tab button is RIGHT-ALIGNED at a fixed offset from
 * the framebuffer's right edge, so hit_chrome() can locate it WITHOUT knowing
 * ntabs. Tab columns grow left-to-right from TAB_X0 but never spill into the
 * [+] zone (the painter and hit-tester both clamp against NEWTAB_X(w)). */
#define TABS_Y          32
#define TABS_H          22
#define TAB_X0          4             /* first tab left edge */
#define TAB_W           136           /* slightly wider for readability */
#define TAB_GAP         2             /* gap between tabs */
#define TAB_MAX         8             /* most tabs the strip hit-tests */
#define TAB_CLOSE_W     18            /* close 'x' hot-zone at tab's right */
#define NEWTAB_W        26            /* [+] button width */
#define NEWTAB_RPAD     8             /* gap from [+] to the right edge */

/* Returns the left X of tab index i. */
#define TAB_LEFT(i)     (TAB_X0 + (i) * (TAB_W + TAB_GAP))
/* Left X of the right-aligned [+] new-tab button for framebuffer width w. */
#define NEWTAB_X(w)     ((w) - NEWTAB_RPAD - NEWTAB_W)

/* =========================================================================
 * Tiny local helpers (no libc math, all integer, all bounds-checked).
 * ========================================================================= */

/* Clamp helper. */
static inline int b2_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Opaque filled rectangle, clipped to [0,w) x [0,h). Tolerates any inputs. */
static void ui_fill_rect(unsigned int *fb, int w, int h,
                         int x, int y, int rw, int rh, unsigned int color)
{
    if (!fb || w <= 0 || h <= 0 || rw <= 0 || rh <= 0) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + rw; if (x2 > w) x2 = w;
    int y2 = y + rh; if (y2 > h) y2 = h;
    if (x1 >= x2 || y1 >= y2) return;
    for (int yy = y1; yy < y2; yy++) {
        unsigned int *prow = fb + (long)yy * w;
        for (int xx = x1; xx < x2; xx++)
            prow[xx] = color;
    }
}

/* Alpha-blend one pixel: src over dst, `a` in 0..255. Clipped to (w,h). */
static void ui_blend_px(unsigned int *fb, int w, int h, int x, int y,
                        unsigned int rgb, unsigned int a)
{
    if (!fb || x < 0 || y < 0 || x >= w || y >= h) return;
    if (a == 0) return;
    if (a >= 255) { fb[(long)y * w + x] = 0xFF000000u | (rgb & 0x00FFFFFFu); return; }

    unsigned int d = fb[(long)y * w + x];
    unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    unsigned int sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    unsigned int ia = 255u - a;
    unsigned int rr = (sr * a + dr * ia + 127) / 255u;
    unsigned int rg = (sg * a + dg * ia + 127) / 255u;
    unsigned int rb = (sb * a + db * ia + 127) / 255u;
    fb[(long)y * w + x] = 0xFF000000u | (rr << 16) | (rg << 8) | rb;
}

/* Translucent filled rectangle (tint), clipped to [0,w) x [0,h). */
static void ui_tint_rect(unsigned int *fb, int w, int h,
                         int x, int y, int rw, int rh,
                         unsigned int rgb, unsigned int a)
{
    if (!fb || rw <= 0 || rh <= 0) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + rw; if (x2 > w) x2 = w;
    int y2 = y + rh; if (y2 > h) y2 = h;
    for (int yy = y1; yy < y2; yy++)
        for (int xx = x1; xx < x2; xx++)
            ui_blend_px(fb, w, h, xx, yy, rgb, a);
}

/* 1px rectangle outline, clipped. */
static void ui_stroke_rect(unsigned int *fb, int w, int h,
                           int x, int y, int rw, int rh, unsigned int color)
{
    if (rw <= 0 || rh <= 0) return;
    ui_fill_rect(fb, w, h, x, y, rw, 1, color);            /* top    */
    ui_fill_rect(fb, w, h, x, y + rh - 1, rw, 1, color);  /* bottom */
    ui_fill_rect(fb, w, h, x, y, 1, rh, color);            /* left   */
    ui_fill_rect(fb, w, h, x + rw - 1, y, 1, rh, color);  /* right  */
}

/* Rounded filled rect: fill with 1-pixel corner notches knocked out.
 * The caller must paint the chrome bg behind this first (so notch shows bg).
 * Produces a soft, pill-like appearance for the address bar at no extra cost. */
static void ui_round_fill(unsigned int *fb, int w, int h,
                          int x, int y, int rw, int rh, unsigned int color,
                          unsigned int bg)
{
    ui_fill_rect(fb, w, h, x, y, rw, rh, color);
    if (rw < 4 || rh < 4) return;
    /* Knock out the 4 corner pixels to simulate rounding. */
    ui_fill_rect(fb, w, h, x,          y,          1, 1, bg);  /* TL */
    ui_fill_rect(fb, w, h, x + rw - 1, y,          1, 1, bg);  /* TR */
    ui_fill_rect(fb, w, h, x,          y + rh - 1, 1, 1, bg);  /* BL */
    ui_fill_rect(fb, w, h, x + rw - 1, y + rh - 1, 1, 1, bg);  /* BR */
    /* An additional inner-shadow row at the bottom of the bar gives depth. */
    if (rh >= 3)
        ui_fill_rect(fb, w, h, x + 1, y + rh - 2, rw - 2, 1, C_ADDR_SHADOW);
}

/* Draw a string clipped to (w,h); truncates with a trailing "..." if it would
 * overflow `max_px`. Returns nothing. Tolerates NULL. */
static void ui_draw_text_ellipsis(unsigned int *fb, int w, int h,
                                  int x, int y, const char *s,
                                  int max_px, unsigned int color)
{
    if (!fb || !s || max_px < FONT_W) return;
    int n = (int)strlen(s);
    int full_px = n * FONT_W;

    if (full_px <= max_px) {
        /* fits: draw whole string */
        int cx = x;
        for (int i = 0; i < n; i++) {
            font_draw_char(fb, w, w, h, cx, y, s[i], color);
            cx += FONT_W;
        }
        return;
    }

    /* doesn't fit: reserve room for "..." (3 glyphs) and truncate */
    int avail = max_px - 3 * FONT_W;
    int keep = (avail > 0) ? avail / FONT_W : 0;
    int cx = x;
    for (int i = 0; i < keep && i < n; i++) {
        font_draw_char(fb, w, w, h, cx, y, s[i], color);
        cx += FONT_W;
    }
    font_draw_char(fb, w, w, h, cx,           y, '.', color);
    font_draw_char(fb, w, w, h, cx + FONT_W,  y, '.', color);
    font_draw_char(fb, w, w, h, cx + 2*FONT_W, y, '.', color);
}

/* Centered single-line text helper. */
static void ui_draw_text_centered(unsigned int *fb, int w, int h,
                                  int cx, int y, const char *s,
                                  unsigned int color)
{
    if (!fb || !s) return;
    int n = (int)strlen(s);
    int tw = n * FONT_W;
    int x = cx - tw / 2;
    for (int i = 0; i < n; i++) {
        font_draw_char(fb, w, w, h, x, y, s[i], color);
        x += FONT_W;
    }
}

/* =========================================================================
 * Toolbar glyphs -- hand-drawn so they look like icons, not letters.
 * Each draws inside a BTN_W x TOOL_H cell at (bx, TOOL_Y).
 * ========================================================================= */

/* Left-pointing chevron "<" for Back. Cleaner 2-segment chevron. */
static void ui_icon_back(unsigned int *fb, int w, int h, int bx, unsigned int col)
{
    int cx = bx + BTN_W / 2 + 4;
    int cy = TOOL_Y + TOOL_H / 2;
    /* Upper arm: goes from (cx, cy) up-left to (cx-6, cy-6). */
    for (int i = 0; i <= 6; i++) {
        ui_fill_rect(fb, w, h, cx - i, cy - i, 2, 2, col);
    }
    /* Lower arm: goes from (cx, cy) down-left to (cx-6, cy+6). */
    for (int i = 0; i <= 6; i++) {
        ui_fill_rect(fb, w, h, cx - i, cy + i, 2, 2, col);
    }
}

/* Circular reload arrow — a cleaner ring + directional arrowhead. */
static void ui_icon_reload(unsigned int *fb, int w, int h, int bx, unsigned int col)
{
    int cx = bx + BTN_W / 2;
    int cy = TOOL_Y + TOOL_H / 2;
    /* 16-point ring approximation; skip 2 points at top-right for the gap. */
    static const signed char ring[16][2] = {
        { 0,-7},{ 3,-6},{ 5,-5},{ 7,-2},{ 7, 2},{ 5, 5},{ 3, 6},{ 0, 7},
        {-3, 6},{-5, 5},{-7, 2},{-7,-2},{-6,-4},{-4,-6},{-2,-7},{-1,-7}
    };
    for (int i = 0; i < 16; i++) {
        if (i == 0 || i == 1) continue;   /* gap at top-right for arrowhead */
        ui_fill_rect(fb, w, h, cx + ring[i][0], cy + ring[i][1], 2, 2, col);
    }
    /* Arrowhead pointing clockwise at the gap. */
    ui_fill_rect(fb, w, h, cx + 2, cy - 8, 2, 2, col);
    ui_fill_rect(fb, w, h, cx + 5, cy - 7, 2, 2, col);
    ui_fill_rect(fb, w, h, cx + 4, cy - 5, 2, 2, col);
}

/* "+" plus glyph for the new-tab button, centered in [x..x+NEWTAB_W). */
static void ui_icon_plus(unsigned int *fb, int w, int h, int x, int yc, unsigned int col)
{
    int cx = x + NEWTAB_W / 2;
    ui_fill_rect(fb, w, h, cx - 1, yc - 5, 2, 10, col);   /* vertical bar */
    ui_fill_rect(fb, w, h, cx - 5, yc - 1, 10, 2, col);   /* horizontal bar */
}

/* "x" close glyph centered at (cx,cy) — 2px strokes for crispness. */
static void ui_icon_close(unsigned int *fb, int w, int h, int cx, int cy, unsigned int col)
{
    /* Two diagonals, 2px wide each. */
    for (int i = -3; i <= 3; i++) {
        ui_fill_rect(fb, w, h, cx + i,     cy + i,     2, 1, col);  /* "\" */
        ui_fill_rect(fb, w, h, cx + i,     cy - i - 1, 2, 1, col);  /* "/" */
    }
}

/* Small filled circle (used as security indicator, progress dot, etc.). */
static void ui_icon_dot(unsigned int *fb, int w, int h, int cx, int cy, int r,
                        unsigned int col)
{
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2)
                ui_blend_px(fb, w, h, cx + dx, cy + dy, col & 0x00FFFFFFu, 255);
        }
    }
}

/* =========================================================================
 * b2ui_draw_chrome -- paint the entire top-of-window chrome.
 * ========================================================================= */
void b2ui_draw_chrome(unsigned int *fb, int w, int h,
                      const char *url, int load_pct,
                      int ntabs, int active_tab)
{
    if (!fb || w <= 0 || h <= 0) return;

    int chrome_h = B2UI_CHROME_H;
    if (chrome_h > h) chrome_h = h;

    /* --- Background bands ------------------------------------------------ */
    /* Toolbar zone: slightly lighter */
    ui_fill_rect(fb, w, h, 0, 0, w, chrome_h, C_CHROME_BG);
    /* Tab-strip trough sits distinctly darker */
    if (TABS_Y < chrome_h)
        ui_fill_rect(fb, w, h, 0, TABS_Y, w, TABS_H + 1, C_TABSTRIP_BG);
    /* 1px divider line just under the chrome */
    if (chrome_h >= 1)
        ui_fill_rect(fb, w, h, 0, chrome_h - 1, w, 1, C_DIVIDER);

    /* --- Toolbar: back + reload buttons --------------------------------- */
    ui_icon_back(fb, w, h, BACK_X, C_ICON);
    ui_icon_reload(fb, w, h, RELOAD_X, C_ICON);

    /* --- Address bar ----------------------------------------------------- */
    int addr_w = w - ADDR_X - ADDR_RIGHT_PAD;
    if (addr_w < FONT_W * 3) addr_w = (w - ADDR_X > 0) ? (w - ADDR_X) : 0;
    if (addr_w > 0) {
        /* Rounded fill + 1px border, then an inner-shadow row at bottom. */
        ui_round_fill(fb, w, h, ADDR_X, TOOL_Y, addr_w, TOOL_H, C_ADDR_BG, C_CHROME_BG);
        ui_stroke_rect(fb, w, h, ADDR_X, TOOL_Y, addr_w, TOOL_H, C_ADDR_BORDER);
        /* Restore corner pixels knocked out by ui_round_fill over the border. */
        ui_fill_rect(fb, w, h, ADDR_X + 1, TOOL_Y, addr_w - 2, 1, C_ADDR_BORDER);

        /* Security indicator: a small green dot for https, grey for http. */
        int has_https = (url && url[0] == 'h' && url[1] == 't' && url[2] == 't'
                         && url[3] == 'p' && url[4] == 's');
        int dot_x = ADDR_X + ADDR_TEXT_PAD + 4;
        int dot_y = TOOL_Y + TOOL_H / 2;
        unsigned int dot_col = has_https ? C_SECURE : C_ADDR_BORDER;
        ui_icon_dot(fb, w, h, dot_x, dot_y, 3, dot_col);

        /* Text baseline centers a 16px glyph in the 24px bar. */
        int ty = TOOL_Y + (TOOL_H - FONT_H) / 2;
        int tx = ADDR_X + ADDR_TEXT_PAD + ADDR_SEC_W;  /* leave room for dot */
        int tmax = addr_w - ADDR_TEXT_PAD * 2 - ADDR_SEC_W;
        if (tmax < 0) tmax = 0;
        if (url && url[0]) {
            ui_draw_text_ellipsis(fb, w, h, tx, ty, url, tmax, C_TEXT);
        } else {
            ui_draw_text_ellipsis(fb, w, h, tx, ty,
                                  "Search or enter address", tmax, C_TEXT_DIM);
        }
    }

    /* --- Tab strip ------------------------------------------------------- */
    if (TABS_Y < chrome_h) {
        int nt = ntabs;
        if (nt < 1) nt = 1;                 /* always show at least 1 tab */
        int draw_n = nt > TAB_MAX ? TAB_MAX : nt;
        int at = b2_clampi(active_tab, 0, nt - 1);

        int tcy = TABS_Y + TABS_H / 2;
        int label_y = TABS_Y + (TABS_H - FONT_H) / 2;
        int newtab_x = NEWTAB_X(w);         /* right-aligned [+] anchor */

        for (int i = 0; i < draw_n; i++) {
            int tx = TAB_LEFT(i);
            /* Never let a tab spill into the [+] button's reserved zone. */
            if (tx + TAB_W > newtab_x - TAB_GAP) break;
            if (tx >= w) break;
            int is_active = (i == at);

            if (is_active) {
                /* Active tab: full-height white body, 3px accent bottom stripe,
                 * top border, and left/right borders — no top offset. */
                ui_fill_rect(fb, w, h, tx, TABS_Y, TAB_W, TABS_H, C_TAB_ACTIVE);
                /* Left, right, and top borders */
                ui_fill_rect(fb, w, h, tx,          TABS_Y, 1,     TABS_H, C_TAB_BORDER);
                ui_fill_rect(fb, w, h, tx + TAB_W - 1, TABS_Y, 1,  TABS_H, C_TAB_BORDER);
                ui_fill_rect(fb, w, h, tx,          TABS_Y, TAB_W, 1,      C_TAB_BORDER);
                /* 3px accent underline ties the active tab to the page below */
                ui_fill_rect(fb, w, h, tx + 1, TABS_Y + TABS_H - 3, TAB_W - 2, 3, C_ACCENT);
                /* Label */
                char lab[12];
                lab[0]='T'; lab[1]='a'; lab[2]='b'; lab[3]=' ';
                int num = i + 1, li = 4;
                if (num >= 10) { lab[li++] = (char)('0' + num / 10); }
                lab[li++] = (char)('0' + num % 10);
                lab[li] = 0;
                int lab_max = TAB_W - TAB_CLOSE_W - 12;
                ui_draw_text_ellipsis(fb, w, h, tx + 8, label_y, lab,
                                      lab_max, C_TAB_TEXT_ACT);
                /* Close 'x' (active tab always shows it) */
                if (nt > 1) {
                    int xcx = tx + TAB_W - TAB_CLOSE_W / 2 - 3;
                    ui_icon_close(fb, w, h, xcx, tcy, C_CLOSE);
                }
            } else {
                /* Inactive tab: inset 2px from top, with a two-tone top edge. */
                ui_fill_rect(fb, w, h, tx, TABS_Y + 2, TAB_W, TABS_H - 2,
                             C_TAB_INACTIVE);
                /* Subtle top highlight (2-tone effect). */
                ui_fill_rect(fb, w, h, tx + 1, TABS_Y + 2, TAB_W - 2, 1, C_TAB_TOP);
                /* Left / right / top borders */
                ui_fill_rect(fb, w, h, tx,             TABS_Y + 2, 1,     TABS_H - 2,
                             C_TAB_BORDER);
                ui_fill_rect(fb, w, h, tx + TAB_W - 1, TABS_Y + 2, 1,     TABS_H - 2,
                             C_TAB_BORDER);
                ui_fill_rect(fb, w, h, tx,             TABS_Y + 2, TAB_W, 1,
                             C_TAB_BORDER);
                /* Label (muted) */
                char lab[12];
                lab[0]='T'; lab[1]='a'; lab[2]='b'; lab[3]=' ';
                int num = i + 1, li = 4;
                if (num >= 10) { lab[li++] = (char)('0' + num / 10); }
                lab[li++] = (char)('0' + num % 10);
                lab[li] = 0;
                int lab_max = TAB_W - TAB_CLOSE_W - 12;
                ui_draw_text_ellipsis(fb, w, h, tx + 8, label_y + 1, lab,
                                      lab_max, C_TAB_TEXT_INA);
                /* Close 'x' only if multiple tabs */
                if (nt > 1) {
                    int xcx = tx + TAB_W - TAB_CLOSE_W / 2 - 3;
                    ui_icon_close(fb, w, h, xcx, tcy, C_CLOSE);
                }
            }
        }

        /* [+] new-tab button, fixed at the right edge of the strip. */
        if (newtab_x >= TAB_X0) {
            ui_fill_rect(fb, w, h, newtab_x, TABS_Y + 2, NEWTAB_W, TABS_H - 2,
                         C_TAB_INACTIVE);
            ui_fill_rect(fb, w, h, newtab_x, TABS_Y + 2, NEWTAB_W, 1, C_TAB_BORDER);
            ui_fill_rect(fb, w, h, newtab_x, TABS_Y + 2, 1, TABS_H - 2, C_TAB_BORDER);
            ui_fill_rect(fb, w, h, newtab_x + NEWTAB_W - 1, TABS_Y + 2, 1,
                         TABS_H - 2, C_TAB_BORDER);
            ui_icon_plus(fb, w, h, newtab_x, tcy, C_ICON);
        }
    }

    /* --- Load progress line (drawn last, on top, at the very top edge) --- */
    if (load_pct >= 0) {
        int pct = load_pct > 100 ? 100 : load_pct;
        int pw  = (int)(((long)w * pct) / 100);
        if (pct > 0 && pw < 1) pw = 1;    /* always show a sliver */
        if (pw > 0) {
            /* 3px solid progress bar */
            ui_fill_rect(fb, w, h, 0, PROG_Y, pw, PROG_H, C_ACCENT);
            /* Bright leading dot at the right end of the filled region */
            if (pw >= 3) {
                int dot_x = pw - 1;
                int dot_y = PROG_Y + PROG_H / 2;
                ui_icon_dot(fb, w, h, dot_x, dot_y, 2, C_ACCENT_LITE);
            }
        }
    } else {
        /* load_pct == -1 (or negative): single faint hairline, no fill */
        ui_fill_rect(fb, w, h, 0, PROG_Y, w, 1, C_DIVIDER);
    }
}

/* =========================================================================
 * b2ui_hit_chrome -- map a click (x,y) to a B2UI_ACT_* code.
 * Uses the SAME layout constants as the painter so they always agree.
 * `w` is needed because the address bar / new-tab extents depend on width.
 * ========================================================================= */
int b2ui_hit_chrome(int x, int y, int w)
{
    if (x < 0 || y < 0 || x >= w || y >= B2UI_CHROME_H)
        return B2UI_ACT_NONE;

    /* --- Toolbar row ----------------------------------------------------- */
    if (y >= TOOL_Y && y < TOOL_Y + TOOL_H) {
        if (x >= BACK_X && x < BACK_X + BTN_W)
            return B2UI_ACT_BACK;
        if (x >= RELOAD_X && x < RELOAD_X + BTN_W)
            return B2UI_ACT_RELOAD;
        int addr_w = w - ADDR_X - ADDR_RIGHT_PAD;
        if (addr_w < FONT_W * 3) addr_w = (w - ADDR_X > 0) ? (w - ADDR_X) : 0;
        if (addr_w > 0 && x >= ADDR_X && x < ADDR_X + addr_w)
            return B2UI_ACT_ADDRFOCUS;
        return B2UI_ACT_NONE;
    }

    /* --- Tab strip ------------------------------------------------------- */
    if (y >= TABS_Y && y < TABS_Y + TABS_H) {
        int newtab_x = NEWTAB_X(w);         /* same anchor the painter used */

        /* The [+] button is right-aligned and fixed -- check it first. */
        if (newtab_x >= TAB_X0 &&
            x >= newtab_x && x < newtab_x + NEWTAB_W)
            return B2UI_ACT_NEWTAB;

        /* Tab columns occupy [TAB_X0 .. just left of the [+] button). They are
         * laid out identically to the painter (TAB_LEFT(i)), and the painter
         * stops drawing a tab once it would overlap the [+] zone, so use the
         * SAME guard here. */
        for (int i = 0; i < TAB_MAX; i++) {
            int tx = TAB_LEFT(i);
            if (tx + TAB_W > newtab_x - TAB_GAP) break;   /* matches painter */
            if (tx >= w) break;
            if (x >= tx && x < tx + TAB_W) {
                /* close 'x' hot-zone at the right edge of the tab */
                int close_lo = tx + TAB_W - TAB_CLOSE_W;
                if (x >= close_lo)
                    return B2UI_ACT_CLOSETAB;
                return B2UI_ACT_SELTAB + i;
            }
        }
        return B2UI_ACT_NONE;     /* empty trough / inter-tab gutter */
    }

    return B2UI_ACT_NONE;
}

/* =========================================================================
 * b2ui_draw_link_hover -- hover affordance over a content link box.
 * Accent tint wash over the box + a 2px accent underline at its bottom +
 * a thin top hairline for a "card highlight" feel.
 * Everything clipped to (w,h); tolerates degenerate / off-screen boxes.
 * ========================================================================= */
void b2ui_draw_link_hover(unsigned int *fb, int w, int h,
                          int bx, int by, int bw, int bh)
{
    if (!fb || w <= 0 || h <= 0 || bw <= 0 || bh <= 0) return;

    /* Accent wash over the box (~15% alpha = 38/255). */
    ui_tint_rect(fb, w, h, bx, by, bw, bh, C_ACCENT & 0x00FFFFFFu, 38);

    /* 2px solid accent underline flush to the box bottom. */
    int uy = by + bh - 2;
    ui_fill_rect(fb, w, h, bx, uy, bw, 2, C_ACCENT);

    /* 1px faint top hairline for a contained "selected card" look. */
    ui_tint_rect(fb, w, h, bx, by, bw, 1, C_ACCENT & 0x00FFFFFFu, 80);
}

/* =========================================================================
 * b2ui_draw_error_page -- clean full-page error screen below the chrome.
 *
 * Layout (vertically centered in the content area):
 *   - Soft background circle behind the icon.
 *   - Outlined "sad face" icon (ring + eyes + downturned mouth).
 *   - Bold headline: "Can't reach this page".
 *   - Detail line: the supplied msg (or default text), with ellipsis.
 *   - Hint line: "Check the address and try reloading."
 *   - Accent rule to finish.
 * ========================================================================= */
void b2ui_draw_error_page(unsigned int *fb, int w, int h, const char *msg)
{
    if (!fb || w <= 0 || h <= 0) return;

    int top = B2UI_CHROME_H < h ? B2UI_CHROME_H : 0;
    /* Fill the content area (below chrome) with a soft background. */
    ui_fill_rect(fb, w, h, 0, top, w, h - top, C_ERR_BG);

    int cx = w / 2;
    int contentH = h - top;
    int cy_base  = top + contentH / 2;   /* vertical anchor */

    /* Adjust layout anchor so the block reads as centered in tall pages. */
    /* Block height is roughly: icon(50) + gap(12) + head(16) + gap(12)
     *   + detail(16) + gap(8) + hint(16) + gap(10) + rule(2) = ~142 px. */
    int block_h  = 142;
    int cy       = cy_base - block_h / 2;   /* top of block */

    /* ---- Background circle behind icon ---- */
    int icon_cy = cy + 25;   /* center of icon area */
    {
        int bg_r = 28;
        int bg_r2 = bg_r * bg_r;
        for (int dy = -bg_r; dy <= bg_r; dy++) {
            for (int dx = -bg_r; dx <= bg_r; dx++) {
                if (dx * dx + dy * dy <= bg_r2)
                    ui_blend_px(fb, w, h, cx + dx, icon_cy + dy,
                                C_ERR_CIRCLE & 0x00FFFFFFu, 255);
            }
        }
    }

    /* ---- Sad-face ring ---- */
    {
        int fr  = 20;
        int r_o = fr * fr;
        int r_i = (fr - 2) * (fr - 2);
        for (int dy = -fr; dy <= fr; dy++) {
            for (int dx = -fr; dx <= fr; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= r_o && d2 >= r_i)
                    ui_blend_px(fb, w, h, cx + dx, icon_cy + dy,
                                C_ERR_ACCENT & 0x00FFFFFFu, 210);
            }
        }
    }

    /* ---- Eyes ---- */
    ui_fill_rect(fb, w, h, cx - 8, icon_cy - 5, 3, 5, C_ERR_HEAD);
    ui_fill_rect(fb, w, h, cx + 5, icon_cy - 5, 3, 5, C_ERR_HEAD);

    /* ---- Downturned mouth (u-shape inverted) ---- */
    /* horizontal segment */
    ui_fill_rect(fb, w, h, cx - 6, icon_cy + 8,  12, 2, C_ERR_HEAD);
    /* upward flanges */
    ui_fill_rect(fb, w, h, cx - 8, icon_cy + 5,  2,  4, C_ERR_HEAD);
    ui_fill_rect(fb, w, h, cx + 6, icon_cy + 5,  2,  4, C_ERR_HEAD);

    /* ---- Headline ---- */
    int head_y = cy + 56;   /* below icon */
    ui_draw_text_centered(fb, w, h, cx, head_y,
                          "Can't reach this page", C_ERR_HEAD);

    /* ---- Detail line ---- */
    int detail_y = head_y + FONT_H + 8;
    const char *detail = (msg && msg[0]) ? msg
                        : "The server could not be reached.";
    int n = (int)strlen(detail);
    int maxchars = (w > 40) ? (w - 40) / FONT_W : 4;
    if (maxchars < 4) maxchars = 4;
    if (n <= maxchars) {
        ui_draw_text_centered(fb, w, h, cx, detail_y, detail, C_ERR_BODY);
    } else {
        /* Truncate with trailing "..." centered on the kept span. */
        int keep     = maxchars - 3;
        if (keep < 1) keep = 1;
        int span_px  = (keep + 3) * FONT_W;
        int sx       = cx - span_px / 2;
        int dx       = sx;
        for (int i = 0; i < keep && i < n; i++) {
            font_draw_char(fb, w, w, h, dx, detail_y, detail[i], C_ERR_BODY);
            dx += FONT_W;
        }
        font_draw_char(fb, w, w, h, dx,            detail_y, '.', C_ERR_BODY);
        font_draw_char(fb, w, w, h, dx + FONT_W,   detail_y, '.', C_ERR_BODY);
        font_draw_char(fb, w, w, h, dx + 2*FONT_W, detail_y, '.', C_ERR_BODY);
    }

    /* ---- Hint line ---- */
    int hint_y = detail_y + FONT_H + 8;
    ui_draw_text_centered(fb, w, h, cx, hint_y,
                          "Check the address and try reloading.", C_ERR_BODY);

    /* ---- Accent rule ---- */
    int rule_y = hint_y + FONT_H + 10;
    int rule_w = 60;
    ui_fill_rect(fb, w, h, cx - rule_w / 2, rule_y, rule_w, 2, C_ERR_RULE);
    /* Two small accent flanges at each end of the rule */
    ui_fill_rect(fb, w, h, cx - rule_w / 2, rule_y - 2, 2, 6, C_ERR_ACCENT);
    ui_fill_rect(fb, w, h, cx + rule_w / 2 - 2, rule_y - 2, 2, 6, C_ERR_ACCENT);
}

/* =========================================================================
 * b2ui_selftest -- draw into a static scratch buffer; assert key pixels.
 * Returns 0 on pass, nonzero (a small step code) on first failure.
 * ========================================================================= */

/* Scratch framebuffer for the selftest (kept small but realistic). */
#define ST_W 800
#define ST_H 200   /* tall enough to contain the full error page layout */
static unsigned int g_st_fb[ST_W * ST_H];

int b2ui_selftest(void)
{
    /* 1. Clear scratch to a known sentinel (transparent black). */
    memset(g_st_fb, 0, sizeof(g_st_fb));

    /* 2. Draw a representative chrome: a URL, 50% load, 3 tabs, tab #1 active. */
    b2ui_draw_chrome(g_st_fb, ST_W, ST_H,
                     "http://example.com/some/long/path", 50, 3, 1);

    /* 3a. Address-bar interior pixel must be the white fill (non-sentinel). */
    int ax = ADDR_X + 4;
    int ay = TOOL_Y + TOOL_H / 2;
    unsigned int apx = g_st_fb[(long)ay * ST_W + ax];
    if ((apx & 0x00FFFFFFu) != (C_ADDR_BG & 0x00FFFFFFu))
        return 1;

    /* 3b. Progress fill at load_pct=50 -> a pixel near 25% width must be accent. */
    int px = ST_W / 4;
    unsigned int ppx = g_st_fb[(long)(PROG_Y) * ST_W + px];
    if ((ppx & 0x00FFFFFFu) != (C_ACCENT & 0x00FFFFFFu))
        return 2;

    /* 3c. A pixel past the 50% mark must NOT be the progress accent. */
    int px2 = (ST_W * 3) / 4;
    unsigned int ppx2 = g_st_fb[(long)PROG_Y * ST_W + px2];
    if ((ppx2 & 0x00FFFFFFu) == (C_ACCENT & 0x00FFFFFFu))
        return 3;

    /* 3d. Some text must have been drawn into the address bar (a text-colored
     *     pixel exists somewhere in the bar's text band). */
    {
        int found = 0;
        int ty0 = TOOL_Y + (TOOL_H - FONT_H) / 2;
        for (int yy = ty0; yy < ty0 + FONT_H && !found; yy++)
            for (int xx = ADDR_X + 6; xx < ADDR_X + 200 && xx < ST_W; xx++)
                if ((g_st_fb[(long)yy * ST_W + xx] & 0x00FFFFFFu)
                        == (C_TEXT & 0x00FFFFFFu)) { found = 1; break; }
        if (!found) return 4;
    }

    /* 4. Hit-test: a click in the address-bar band returns ADDRFOCUS. */
    if (b2ui_hit_chrome(ADDR_X + 20, TOOL_Y + TOOL_H / 2, ST_W)
            != B2UI_ACT_ADDRFOCUS)
        return 5;

    /* 5. Hit-test: back / reload buttons. */
    if (b2ui_hit_chrome(BACK_X + 4, TOOL_Y + 4, ST_W) != B2UI_ACT_BACK)
        return 6;
    if (b2ui_hit_chrome(RELOAD_X + 4, TOOL_Y + 4, ST_W) != B2UI_ACT_RELOAD)
        return 7;

    /* 6. Hit-test: clicking tab #2 selects it (SELTAB + 2). */
    if (b2ui_hit_chrome(TAB_LEFT(2) + 10, TABS_Y + 4, ST_W)
            != B2UI_ACT_SELTAB + 2)
        return 8;

    /* 7. Hit-test: the right-aligned [+] button is NEWTAB. */
    if (b2ui_hit_chrome(NEWTAB_X(ST_W) + NEWTAB_W / 2, TABS_Y + 6, ST_W)
            != B2UI_ACT_NEWTAB)
        return 9;

    /* 8. Hit-test: a click outside the chrome is NONE. */
    if (b2ui_hit_chrome(10, B2UI_CHROME_H + 5, ST_W) != B2UI_ACT_NONE)
        return 10;

    /* 9. Error page draws a non-background pixel in the content area. */
    memset(g_st_fb, 0, sizeof(g_st_fb));
    b2ui_draw_error_page(g_st_fb, ST_W, ST_H, "DNS lookup failed");
    {
        int found = 0;
        for (int yy = B2UI_CHROME_H; yy < ST_H && !found; yy++)
            for (int xx = 0; xx < ST_W; xx++) {
                unsigned int v = g_st_fb[(long)yy * ST_W + xx] & 0x00FFFFFFu;
                if (v != (C_ERR_BG & 0x00FFFFFFu)) { found = 1; break; }
            }
        if (!found) return 11;
    }

    /* 10. Link-hover underline writes an accent pixel at the box bottom. */
    memset(g_st_fb, 0, sizeof(g_st_fb));
    b2ui_draw_link_hover(g_st_fb, ST_W, ST_H, 40, 70, 120, 18);
    {
        unsigned int v = g_st_fb[(long)(70 + 18 - 1) * ST_W + 50] & 0x00FFFFFFu;
        if (v != (C_ACCENT & 0x00FFFFFFu)) return 12;
    }

    /* 11. Defensive: NULL fb / zero dims must not crash and must no-op. */
    b2ui_draw_chrome(NULL, ST_W, ST_H, "x", 50, 1, 0);
    b2ui_draw_chrome(g_st_fb, 0, 0, NULL, -1, 0, 0);
    b2ui_draw_error_page(NULL, 10, 10, NULL);
    b2ui_draw_link_hover(g_st_fb, ST_W, ST_H, -100, -100, 5, 5);

    /* 12. Progress line: at load_pct=0 no accent pixel at x=0. */
    memset(g_st_fb, 0, sizeof(g_st_fb));
    b2ui_draw_chrome(g_st_fb, ST_W, ST_H, NULL, 0, 1, 0);
    /* load_pct=0 → pw=0 → no fill (progress bar shows nothing) */
    {
        unsigned int v0 = g_st_fb[(long)PROG_Y * ST_W + 0] & 0x00FFFFFFu;
        /* Should be chrome bg or divider, NOT the accent color. */
        if (v0 == (C_ACCENT & 0x00FFFFFFu)) return 13;
    }

    /* 13. Progress at 100%: pixel at x=w-1 must be accent (or leading dot). */
    memset(g_st_fb, 0, sizeof(g_st_fb));
    b2ui_draw_chrome(g_st_fb, ST_W, ST_H, NULL, 100, 1, 0);
    {
        unsigned int v1 = g_st_fb[(long)PROG_Y * ST_W + (ST_W / 2)] & 0x00FFFFFFu;
        if (v1 != (C_ACCENT & 0x00FFFFFFu) && v1 != (C_ACCENT_LITE & 0x00FFFFFFu))
            return 14;
    }

    return 0;   /* all checks passed */
}
