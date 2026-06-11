/*
 * ui.c -- M4 retained-mode UI toolkit implementation (freestanding).
 * ==================================================================
 *
 * Implements the API declared in ui.h on top of:
 *   - userspace/lib/wl/wl_client.h   (windowing: connect/create/commit/poll)
 *   - userspace/lib/font/bitfont.h   (8x16 bitmap text rendering)
 *
 * Build (EXACT -- flags passed DIRECTLY, never via an unquoted variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/ui/ui.c -o ui.o
 *
 * No libc, no _start: this is a library TU. Memory comes from static
 * fixed-size pools (no malloc/realloc). The widget tree is retained:
 * built once by the app, then rendered every frame by ui_app_run().
 */

#include "ui.h"
#include "../wl/wl_client.h"
#include "../font/bitfont.h"

/* Scalable text renderer (userspace/lib/font2), linked into every toolkit app. */
extern void font2_draw_cell_clip(unsigned int *px, int stride, int maxw, int maxh,
                                 int clip_x0, int clip_x1, int x, int y,
                                 const char *str, int cell_w, int cell_h,
                                 unsigned int argb);

/* GLOBAL UI SCALE for the toolkit (matches the compositor chrome default so the
 * whole desktop is consistent). Every widget's window size + layout coords are
 * scaled by UI_S at create/attach time, so positions, sizes and the cursor all
 * live in the SAME scaled pixel space (no input remap needed); text renders via
 * font2 at the scaled cell (g_ui_cw x g_ui_ch). Keeping the base FONT_W/FONT_H
 * for the widget-size MATH (then scaling once in attach_child) avoids double
 * scaling. 130% => 10x20, readable on a real panel. */
#define UI_PCT     130
#define UI_S(v)    (((v) * UI_PCT) / 100)
#define UI_CELL_W  (8  * UI_PCT / 100)
#define UI_CELL_H  (16 * UI_PCT / 100)
static const int g_ui_cw = UI_CELL_W;
static const int g_ui_ch = UI_CELL_H;

/* Drop-in scaled text: position is already in scaled space; render at the cell. */
static void ui_text(unsigned int *buf, int sp, int bw, int bh,
                    int x, int y, const char *s, unsigned int col) {
    font2_draw_cell_clip(buf, sp, bw, bh, 0, bw, x, y, s, g_ui_cw, g_ui_ch, col);
}

/* ---- syscall numbers (per task spec) ---- */
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

typedef unsigned int u32;
typedef int          i32;

/*
 * 3-argument inline syscall (args rdi/rsi/rdx). The toolkit only needs
 * WRITE (diagnostics), YIELD and GET_TICKS_MS, all of which take <= 3 args.
 */
static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny freestanding string/mem helpers (no libc) ---- */

static unsigned long ui_strlen(const char* s) {
    unsigned long n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Copy at most cap-1 bytes of src into dst and NUL-terminate (cap > 0). */
static void ui_strlcpy(char* dst, const char* src, unsigned long cap) {
    unsigned long i = 0;
    if (cap == 0) return;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void* ui_memset(void* p, int v, unsigned long n) {
    unsigned char* d = (unsigned char*)p;
    for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)v;
    return p;
}

static i32 ui_clamp(i32 v, i32 lo, i32 hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ---- serial diagnostics (fd 1) ---- */
static void ui_log(const char* m) {
    sc(SYS_WRITE, 1, (long)m, (long)ui_strlen(m));
}

/* ---- Aether Dark palette ---- */
#define COL_WINDOW  0xFF1C1C1Eu   /* root window background */
#define COL_SURFACE 0xFF2C2C2Eu   /* default button face    */
#define COL_HOVER   0xFF3A3A3Cu   /* hover tint             */
#define COL_TEXT    0xFFFFFFFFu   /* default text           */
#define COL_ACCENT  0xFF0A84FFu   /* pressed accent         */
#define COL_BORDER  0xFF38383Au   /* widget border          */

/* New palette entries for additional widgets */
#define COL_CHECK_BG    0xFF1C1C1Eu   /* checkbox unchecked bg  */
#define COL_CHECK_FILL  0xFF0A84FFu   /* checkbox checked fill  */
#define COL_SLIDER_TRK  0xFF38383Au   /* slider track           */
#define COL_SLIDER_KNOB 0xFF8E8E93u   /* slider knob            */
#define COL_TXTBOX_BG   0xFF1C1C1Eu   /* textbox background     */
#define COL_TXTBOX_FCS  0xFF0A84FFu   /* textbox focus ring     */
#define COL_PROG_BG     0xFF38383Au   /* progress track         */
#define COL_PROG_FILL   0xFF30D158u   /* progress fill (green)  */
#define COL_SCROLL_BAR  0xFF48484Au   /* scrollbar thumb        */

/* ---- widget model ---- */

#define UI_TEXT_CAP      64        /* fixed text buffer per widget          */
#define UI_MAX_CHILDREN  16        /* fixed child array per widget          */
#define UI_MAX_WIDGETS   512       /* total widgets across all apps (was 256, bumped for new types) */
#define UI_MAX_APPS      4         /* total apps                            */

/* Keycodes (Linux set-1 scancodes, matching the kernel PS/2 driver). */
#define UI_KEY_BACKSPACE  14
#define UI_KEY_LSHIFT     42
#define UI_KEY_RSHIFT     54
#define UI_KEY_CAPS       58

/* US-layout keycode -> ASCII (lowercase, no modifier). */
static const char g_keymap_lo[256] = {
    /* 0  */ 0,   0,   '1', '2', '3', '4', '5', '6',
    /* 8  */ '7', '8', '9', '0', '-', '=', 0,   '\t',
    /* 16 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 24 */ 'o', 'p', '[', ']', 0,   0,   'a', 's',
    /* 32 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 40 */ '\'','`', 0,  '\\','z', 'x', 'c', 'v',
    /* 48 */ 'b', 'n', 'm', ',', '.', '/', 0,   '*',
    /* 56 */ 0,   ' ', 0,
};

/* US-layout keycode -> ASCII (uppercase / shifted). */
static const char g_keymap_hi[256] = {
    /* 0  */ 0,   0,   '!', '@', '#', '$', '%', '^',
    /* 8  */ '&', '*', '(', ')', '_', '+', 0,   '\t',
    /* 16 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 24 */ 'O', 'P', '{', '}', 0,   0,   'A', 'S',
    /* 32 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 40 */ '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    /* 48 */ 'B', 'N', 'M', '<', '>', '?', 0,   '*',
    /* 56 */ 0,   ' ', 0,
};

typedef enum {
    UI_PANEL = 0,
    UI_LABEL,
    UI_BUTTON,
    /* v2 additions */
    UI_CHECKBOX,
    UI_SLIDER,
    UI_TEXTBOX,
    UI_PROGRESS,
    UI_IMAGE_RECT,
    UI_SCROLL
} ui_kind_t;

struct ui_widget {
    ui_kind_t   kind;

    /* Absolute window-local rectangle (computed at construction). */
    i32         ax, ay, aw, ah;

    unsigned int bg;       /* fill color (panel/button/etc.)               */
    unsigned int fg;       /* text color                                   */
    char         text[UI_TEXT_CAP];

    void       (*on_click)(void* ud);
    void        *ud;

    ui_widget_t *children[UI_MAX_CHILDREN];
    int          nchildren;

    int          used;     /* pool slot occupied                           */

    /* ---- per-kind extra state ---- */

    /* UI_CHECKBOX */
    int          checked;                    /* 0 / 1                     */
    void       (*on_toggle)(int state, void* ud);

    /* UI_SLIDER */
    i32          sl_min, sl_max, sl_val;
    int          sl_dragging;                /* 1 while LMB held on knob  */
    void       (*on_change)(int value, void* ud);

    /* UI_TEXTBOX */
    char         tb_buf[UI_TEXTBOX_MAXBUF];
    int          tb_maxlen;
    int          tb_focused;                 /* 1 = has key focus         */
    int          tb_caret_blink;             /* frame counter for blink   */

    /* UI_PROGRESS */
    int          pct;                        /* 0-100                     */

    /* UI_IMAGE_RECT */
    char         ir_glyph;                   /* single glyph, or '\0'     */
    unsigned int ir_color;                   /* tile fill                 */
    unsigned int ir_fg;                      /* glyph foreground          */

    /* UI_SCROLL */
    i32          sc_content_h;              /* total scrollable height    */
    i32          sc_offset;                 /* current Y offset (pixels)  */
    int          sc_dragging;               /* dragging scrollbar?        */
    i32          sc_drag_start_y;           /* cursor Y when drag started */
    i32          sc_drag_start_off;         /* offset when drag started   */
};

struct ui_app {
    wl_window   *win;
    ui_widget_t *root;

    /* Tracked cursor + left-button edge detection. */
    i32          cur_x, cur_y;
    int          btn_prev;   /* previous left-button state (0/1)          */

    /* Optional per-frame tick callback (NULL = disabled). */
    void       (*tick)(void* ud);
    void        *tick_ud;

    /* Keyboard modifier state (shift). */
    int          shift;

    /* Currently focused textbox (NULL = none). */
    ui_widget_t *focus;

    /* Frame counter (for caret blink). */
    int          frame;

    int          used;       /* pool slot occupied                        */
};

/* Static pools -- no dynamic allocation in a freestanding ring-3 lib. */
static struct ui_widget g_widgets[UI_MAX_WIDGETS];
static struct ui_app    g_apps[UI_MAX_APPS];

static ui_widget_t* widget_alloc(void) {
    for (int i = 0; i < UI_MAX_WIDGETS; i++) {
        if (!g_widgets[i].used) {
            ui_memset(&g_widgets[i], 0, sizeof(g_widgets[i]));
            g_widgets[i].used = 1;
            return &g_widgets[i];
        }
    }
    return 0;
}

static ui_app_t* app_alloc(void) {
    for (int i = 0; i < UI_MAX_APPS; i++) {
        if (!g_apps[i].used) {
            ui_memset(&g_apps[i], 0, sizeof(g_apps[i]));
            g_apps[i].used = 1;
            return &g_apps[i];
        }
    }
    return 0;
}

/*
 * Attach `child` to `parent`, computing the child's absolute rect from the
 * parent's absolute origin plus the supplied parent-relative x/y. Returns 0
 * on success, -1 if the parent's child array is full.
 */
static int attach_child(ui_widget_t* parent, ui_widget_t* child,
                        i32 rx, i32 ry, i32 w, i32 h) {
    if (parent->nchildren >= UI_MAX_CHILDREN) return -1;
    /* Scale the relative offset + size once, here, into the parent's (already
     * scaled) absolute space -- so the whole widget tree lays out at UI scale. */
    child->ax = parent->ax + UI_S(rx);
    child->ay = parent->ay + UI_S(ry);
    child->aw = UI_S(w);
    child->ah = UI_S(h);
    parent->children[parent->nchildren++] = child;
    return 0;
}

/* ---- widget free / detach ---- */

void ui_widget_free_tree(ui_widget_t* w) {
    if (!w) return;
    /* Free children first (depth-first). */
    for (int i = 0; i < w->nchildren; i++)
        ui_widget_free_tree(w->children[i]);
    w->nchildren = 0;
    w->used = 0;   /* return slot to the pool */
}

int ui_widget_detach(ui_widget_t* parent, ui_widget_t* child) {
    if (!parent || !child) return -1;
    for (int i = 0; i < parent->nchildren; i++) {
        if (parent->children[i] == child) {
            /* Shift later children down. */
            for (int j = i; j + 1 < parent->nchildren; j++)
                parent->children[j] = parent->children[j + 1];
            parent->nchildren--;
            return 0;
        }
    }
    return -1;
}

/* ---- public construction API ---- */

ui_app_t* ui_app_create(const char* title, int w, int h) {
    if (w <= 0 || h <= 0) return 0;

    if (wl_connect() != 0) {
        ui_log("[UI] wl_connect FAILED\n");
        return 0;
    }

    /* Create the window at the SCALED size so the scaled layout fits. */
    wl_window* win = wl_create_window((wl_u32)UI_S(w), (wl_u32)UI_S(h), title);
    if (!win) {
        ui_log("[UI] wl_create_window FAILED\n");
        return 0;
    }

    ui_app_t* app = app_alloc();
    if (!app) {
        ui_log("[UI] out of app slots\n");
        return 0;
    }

    ui_widget_t* root = widget_alloc();
    if (!root) {
        ui_log("[UI] out of widget slots (root)\n");
        app->used = 0;
        return 0;
    }

    root->kind = UI_PANEL;
    root->ax = 0;
    root->ay = 0;
    root->aw = (i32)win->w;
    root->ah = (i32)win->h;
    root->bg = COL_WINDOW;
    root->fg = COL_TEXT;

    app->win      = win;
    app->root     = root;
    app->cur_x    = -1;
    app->cur_y    = -1;
    app->btn_prev = 0;
    return app;
}

ui_widget_t* ui_app_root(ui_app_t* app) {
    return app ? app->root : 0;
}

ui_widget_t* ui_panel(ui_widget_t* parent, int x, int y, int w, int h,
                      unsigned int bg) {
    if (!parent) return 0;
    ui_widget_t* p = widget_alloc();
    if (!p) return 0;
    p->kind = UI_PANEL;
    p->bg   = bg;
    p->fg   = COL_TEXT;
    if (attach_child(parent, p, x, y, w, h) != 0) {
        p->used = 0;
        return 0;
    }
    return p;
}

ui_widget_t* ui_label(ui_widget_t* parent, int x, int y, const char* text,
                      unsigned int color) {
    if (!parent) return 0;
    ui_widget_t* l = widget_alloc();
    if (!l) return 0;
    l->kind = UI_LABEL;
    l->bg   = 0;            /* labels have no fill */
    l->fg   = color;
    ui_strlcpy(l->text, text, UI_TEXT_CAP);
    i32 tw = (i32)ui_strlen(l->text) * FONT_W;
    if (attach_child(parent, l, x, y, tw, FONT_H) != 0) {
        l->used = 0;
        return 0;
    }
    return l;
}

ui_widget_t* ui_button(ui_widget_t* parent, int x, int y, int w, int h,
                       const char* text, void (*on_click)(void* ud),
                       void* ud) {
    if (!parent) return 0;
    ui_widget_t* b = widget_alloc();
    if (!b) return 0;
    b->kind     = UI_BUTTON;
    b->bg       = COL_SURFACE;
    b->fg       = COL_TEXT;
    b->on_click = on_click;
    b->ud       = ud;
    ui_strlcpy(b->text, text, UI_TEXT_CAP);
    if (attach_child(parent, b, x, y, w, h) != 0) {
        b->used = 0;
        return 0;
    }
    return b;
}

void ui_label_set_text(ui_widget_t* w, const char* text) {
    if (!w) return;
    ui_strlcpy(w->text, text, UI_TEXT_CAP);
    if (w->kind == UI_LABEL)
        w->aw = (i32)ui_strlen(w->text) * g_ui_cw;  /* scaled width matches render */
}

void ui_widget_set_bg(ui_widget_t* w, unsigned int bg) {
    if (!w) return;
    w->bg = bg;
}

void ui_widget_set_fg(ui_widget_t* w, unsigned int fg) {
    if (!w) return;
    w->fg = fg;
}

void ui_app_set_tick(ui_app_t* app, void (*tick)(void* ud), void* ud) {
    if (!app) return;
    app->tick    = tick;
    app->tick_ud = ud;
}

/* =========================================================================
 * NEW WIDGET CONSTRUCTORS (v2)
 * =========================================================================
 */

/* --- Checkbox --- */

ui_widget_t* ui_checkbox(ui_widget_t* parent, int x, int y,
                         const char* label, int initial,
                         void (*on_toggle)(int state, void* ud), void* ud) {
    if (!parent) return 0;
    ui_widget_t* cb = widget_alloc();
    if (!cb) return 0;
    cb->kind      = UI_CHECKBOX;
    cb->bg        = COL_CHECK_BG;
    cb->fg        = COL_TEXT;
    cb->checked   = initial ? 1 : 0;
    cb->on_toggle = on_toggle;
    cb->ud        = ud;
    ui_strlcpy(cb->text, label ? label : "", UI_TEXT_CAP);
    /* Width = 16px box + 4px gap + text width; height = 16px */
    i32 tw = (i32)ui_strlen(cb->text) * FONT_W;
    i32 ww = 16 + 4 + tw;
    if (ww < 16) ww = 16;
    if (attach_child(parent, cb, x, y, ww, FONT_H) != 0) {
        cb->used = 0;
        return 0;
    }
    return cb;
}

int ui_checkbox_checked(ui_widget_t* w) {
    if (!w || w->kind != UI_CHECKBOX) return 0;
    return w->checked;
}

void ui_checkbox_set(ui_widget_t* w, int state) {
    if (!w || w->kind != UI_CHECKBOX) return;
    w->checked = state ? 1 : 0;
}

/* --- Slider --- */

ui_widget_t* ui_slider(ui_widget_t* parent, int x, int y, int w,
                       int min, int max, int initial,
                       void (*on_change)(int value, void* ud), void* ud) {
    if (!parent) return 0;
    if (max <= min) max = min + 1;
    ui_widget_t* sl = widget_alloc();
    if (!sl) return 0;
    sl->kind      = UI_SLIDER;
    sl->bg        = COL_SLIDER_TRK;
    sl->fg        = COL_TEXT;
    sl->sl_min    = min;
    sl->sl_max    = max;
    sl->sl_val    = ui_clamp(initial, min, max);
    sl->sl_dragging = 0;
    sl->on_change = on_change;
    sl->ud        = ud;
    if (attach_child(parent, sl, x, y, w, 20) != 0) {
        sl->used = 0;
        return 0;
    }
    return sl;
}

int ui_slider_value(ui_widget_t* w) {
    if (!w || w->kind != UI_SLIDER) return 0;
    return w->sl_val;
}

void ui_slider_set(ui_widget_t* w, int value) {
    if (!w || w->kind != UI_SLIDER) return;
    w->sl_val = ui_clamp(value, w->sl_min, w->sl_max);
}

/* --- Textbox --- */

ui_widget_t* ui_textbox(ui_widget_t* parent, int x, int y, int w, int maxlen) {
    if (!parent) return 0;
    ui_widget_t* tb = widget_alloc();
    if (!tb) return 0;
    tb->kind      = UI_TEXTBOX;
    tb->bg        = COL_TXTBOX_BG;
    tb->fg        = COL_TEXT;
    int cap = maxlen;
    if (cap < 1)  cap = 1;
    if (cap >= UI_TEXTBOX_MAXBUF) cap = UI_TEXTBOX_MAXBUF - 1;
    tb->tb_maxlen = cap;
    tb->tb_buf[0] = '\0';
    tb->tb_focused = 0;
    tb->tb_caret_blink = 0;
    if (attach_child(parent, tb, x, y, w, 20) != 0) {
        tb->used = 0;
        return 0;
    }
    return tb;
}

const char* ui_textbox_text(ui_widget_t* w) {
    if (!w || w->kind != UI_TEXTBOX) return "";
    return w->tb_buf;
}

void ui_textbox_set_text(ui_widget_t* w, const char* text) {
    if (!w || w->kind != UI_TEXTBOX) return;
    ui_strlcpy(w->tb_buf, text, (unsigned long)(w->tb_maxlen + 1));
}

/* --- Progress --- */

ui_widget_t* ui_progress(ui_widget_t* parent, int x, int y, int w, int h) {
    if (!parent) return 0;
    ui_widget_t* pr = widget_alloc();
    if (!pr) return 0;
    pr->kind = UI_PROGRESS;
    pr->bg   = COL_PROG_BG;
    pr->fg   = COL_PROG_FILL;
    pr->pct  = 0;
    if (attach_child(parent, pr, x, y, w, h) != 0) {
        pr->used = 0;
        return 0;
    }
    return pr;
}

void ui_progress_set(ui_widget_t* w, int pct) {
    if (!w || w->kind != UI_PROGRESS) return;
    w->pct = ui_clamp(pct, 0, 100);
}

/* --- Image rect (colored icon tile) --- */

ui_widget_t* ui_image_rect(ui_widget_t* parent, int x, int y, int sz,
                           unsigned int color, char glyph, unsigned int fg) {
    if (!parent) return 0;
    ui_widget_t* ir = widget_alloc();
    if (!ir) return 0;
    ir->kind     = UI_IMAGE_RECT;
    ir->bg       = color;
    ir->fg       = fg;
    ir->ir_color = color;
    ir->ir_fg    = fg;
    ir->ir_glyph = glyph;
    if (attach_child(parent, ir, x, y, sz, sz) != 0) {
        ir->used = 0;
        return 0;
    }
    return ir;
}

/* --- Scroll view --- */

ui_widget_t* ui_scroll(ui_widget_t* parent, int x, int y, int w, int h,
                       unsigned int bg, int content_h) {
    if (!parent) return 0;
    ui_widget_t* sc = widget_alloc();
    if (!sc) return 0;
    sc->kind         = UI_SCROLL;
    sc->bg           = bg;
    sc->fg           = COL_TEXT;
    sc->sc_content_h = content_h > h ? content_h : h;
    sc->sc_offset    = 0;
    sc->sc_dragging  = 0;
    if (attach_child(parent, sc, x, y, w, h) != 0) {
        sc->used = 0;
        return 0;
    }
    return sc;
}

int ui_scroll_offset(ui_widget_t* w) {
    if (!w || w->kind != UI_SCROLL) return 0;
    return w->sc_offset;
}

void ui_scroll_set_offset(ui_widget_t* w, int offset) {
    if (!w || w->kind != UI_SCROLL) return;
    i32 max_off = w->sc_content_h - w->ah;
    if (max_off < 0) max_off = 0;
    w->sc_offset = ui_clamp(offset, 0, max_off);
}

/* =========================================================================
 * RENDERING
 * =========================================================================
 */

/* Fill a clipped ARGB32 rectangle in the stride-addressed window buffer. */
static void fill_rect(u32* buf, i32 bw, i32 bh, i32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color) {
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w;
    i32 y2 = y + h;
    if (x2 > bw) x2 = bw;
    if (y2 > bh) y2 = bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32* row = buf + (u32)yy * (u32)stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/*
 * Clipped fill: same as fill_rect but additionally clipped to a clip rect.
 * Used for the scroll content area.
 */
static void fill_rect_clipped(u32* buf, i32 bw, i32 bh, i32 stride_px,
                              i32 cx, i32 cy, i32 cw, i32 ch,
                              i32 x, i32 y, i32 w, i32 h, u32 color) {
    /* clip to the clip rect first */
    if (x < cx) { w -= cx - x; x = cx; }
    if (y < cy) { h -= cy - y; y = cy; }
    i32 x2 = x + w; if (x2 > cx + cw) x2 = cx + cw;
    i32 y2 = y + h; if (y2 > cy + ch) y2 = cy + ch;
    w = x2 - x; h = y2 - y;
    if (w <= 0 || h <= 0) return;
    fill_rect(buf, bw, bh, stride_px, x, y, w, h, color);
}

/*
 * Filled rounded rectangle: a plain fill minus the four 2x2 corner pixels,
 * which gives buttons a soft "rounded-ish" look without per-pixel math.
 */
static void fill_round_rect(u32* buf, i32 bw, i32 bh, i32 stride_px,
                            i32 x, i32 y, i32 w, i32 h, u32 color) {
    if (w <= 0 || h <= 0) return;
    fill_rect(buf, bw, bh, stride_px, x, y, w, h, color);
    if (w >= 4 && h >= 4) {
        /* Knock the very corners back to whatever is underneath: we don't
         * have the backdrop, so just leave a 1px notch by re-clearing to
         * the window bg -- visually reads as a rounded corner on dark UI. */
        u32 notch = COL_WINDOW;
        fill_rect(buf, bw, bh, stride_px, x,         y,         1, 1, notch);
        fill_rect(buf, bw, bh, stride_px, x + w - 1, y,         1, 1, notch);
        fill_rect(buf, bw, bh, stride_px, x,         y + h - 1, 1, 1, notch);
        fill_rect(buf, bw, bh, stride_px, x + w - 1, y + h - 1, 1, 1, notch);
    }
}

/* 1px border outline. */
static void stroke_rect(u32* buf, i32 bw, i32 bh, i32 stride_px,
                        i32 x, i32 y, i32 w, i32 h, u32 color) {
    if (w <= 0 || h <= 0) return;
    fill_rect(buf, bw, bh, stride_px, x,         y,         w, 1, color);
    fill_rect(buf, bw, bh, stride_px, x,         y + h - 1, w, 1, color);
    fill_rect(buf, bw, bh, stride_px, x,         y,         1, h, color);
    fill_rect(buf, bw, bh, stride_px, x + w - 1, y,         1, h, color);
}

static int point_in(const ui_widget_t* w, i32 px, i32 py) {
    return px >= w->ax && px < w->ax + w->aw &&
           py >= w->ay && py < w->ay + w->ah;
}

/* Clipped font_draw_string: only draws characters whose X falls within [cx, cx+cw). */
static void font_draw_string_clipped(u32* buf, i32 stride_px, i32 bw, i32 bh,
                                     i32 cx, i32 cy, i32 cw, i32 ch,
                                     i32 x, i32 y, const char* s, u32 color) {
    /* Simple approach: call font_draw_char per character with clipping check. */
    for (unsigned long i = 0; s[i]; i++) {
        i32 gx = x + (i32)i * FONT_W;
        /* Skip glyphs entirely outside the clip rect */
        if (gx + FONT_W <= cx) continue;
        if (gx >= cx + cw)    break;
        if (y + FONT_H <= cy || y >= cy + ch) break;
        font_draw_char(buf, stride_px, bw, bh, gx, y, s[i], color);
    }
}

/*
 * Compute the knob center X for a slider given its current value.
 * Track runs from (ax+8) to (ax+aw-8), knob is 8px wide.
 */
static i32 slider_knob_cx(const ui_widget_t* sl) {
    i32 track_x0 = sl->ax + 8;
    i32 track_x1 = sl->ax + sl->aw - 8;
    i32 range = sl->sl_max - sl->sl_min;
    if (range <= 0) return track_x0;
    i32 pos = track_x0 + ((sl->sl_val - sl->sl_min) * (track_x1 - track_x0)) / range;
    return pos;
}

/* Scrollbar geometry helper: returns thumb Y and height for a scroll widget. */
static void scroll_thumb(const ui_widget_t* sc, i32* ty, i32* th) {
    /* Scrollbar is 8px wide on the right edge. */
    i32 bar_h = sc->ah;
    i32 content = sc->sc_content_h;
    if (content <= sc->ah) content = sc->ah + 1;
    /* Thumb height proportional to visible / total */
    i32 h = (bar_h * sc->ah) / content;
    if (h < 8) h = 8;
    if (h > bar_h) h = bar_h;
    /* Thumb position */
    i32 travel = bar_h - h;
    i32 max_off = sc->sc_content_h - sc->ah;
    if (max_off <= 0) max_off = 1;
    i32 y = (travel * sc->sc_offset) / max_off;
    *ty = sc->ay + y;
    *th = h;
}

/* Recursively draw a widget subtree (parents before children).
 * __attribute__((no_stack_protector)) suppresses Ubuntu GCC 13's injected
 * stack-clash canary on this large-frame function; the kernel never touches
 * fs:0x28 in ring 3 and there is no libc __stack_chk_fail to link against. */
__attribute__((no_stack_protector))
static void render_widget(ui_app_t* app, ui_widget_t* w) {
    wl_window* win = app->win;
    u32* buf = win->pixels;
    i32  bw  = (i32)win->w;
    i32  bh  = (i32)win->h;
    i32  sp  = (i32)(win->stride / 4u);

    int hovered = (app->cur_x >= 0) && point_in(w, app->cur_x, app->cur_y);

    switch (w->kind) {
    case UI_PANEL:
        fill_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, w->bg);
        break;

    case UI_LABEL: {
        /* Vertically center the 16px glyph cell within the label's box. */
        i32 ty = w->ay + (w->ah - g_ui_ch) / 2;
        ui_text(buf, sp, bw, bh, w->ax, ty, w->text, w->fg);
        break;
    }

    case UI_BUTTON: {
        int pressed = hovered && app->btn_prev;
        int has_caption = (w->text && w->text[0]);
        if (has_caption) {
            /* Normal labelled button: opaque face + border + centered caption. */
            u32 face = w->bg;
            if (pressed)      face = COL_ACCENT;
            else if (hovered) face = COL_HOVER;

            fill_round_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, face);
            stroke_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, COL_BORDER);

            /* Center the caption within the button rect (scaled text width). */
            i32 tw = (i32)ui_strlen(w->text) * g_ui_cw;
            i32 tx = w->ax + (w->aw - tw) / 2;
            i32 ty = w->ay + (w->ah - g_ui_ch) / 2;
            if (tx < w->ax) tx = w->ax;
            ui_text(buf, sp, bw, bh, tx, ty, w->text, w->fg);
        } else {
            /* Empty caption => invisible click hotspot (e.g. start-menu tiles
             * lay an icon + label down first, then a full-tile button on top to
             * catch the click). Filling here would paint over those siblings and
             * leave a blank box, so draw NO fill; give border-only hover/press
             * feedback so the region still reacts without becoming opaque. */
            if (pressed)      stroke_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, COL_ACCENT);
            else if (hovered) stroke_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, COL_BORDER);
        }
        break;
    }

    /* ---- v2 widget drawing ---- */

    case UI_CHECKBOX: {
        /* box scales with the UI cell so it matches the label height */
        i32 bx = UI_S(16);
        u32 box_color = hovered ? COL_HOVER : COL_SURFACE;
        fill_rect(buf, bw, bh, sp, w->ax, w->ay, bx, bx, box_color);
        stroke_rect(buf, bw, bh, sp, w->ax, w->ay, bx, bx, COL_BORDER);

        if (w->checked) {
            /* Filled inner square as the check glyph */
            fill_rect(buf, bw, bh, sp, w->ax + UI_S(3), w->ay + UI_S(3),
                      bx - UI_S(6), bx - UI_S(6), COL_CHECK_FILL);
        }

        /* Label text to the right */
        if (w->text[0]) {
            i32 ty = w->ay + (bx - g_ui_ch) / 2;
            ui_text(buf, sp, bw, bh, w->ax + bx + UI_S(4), ty, w->text, w->fg);
        }
        break;
    }

    case UI_SLIDER: {
        /* Track (horizontal bar, vertically centered), all metrics scaled */
        i32 track_y  = w->ay + UI_S(8);
        i32 track_h  = UI_S(4);
        i32 inset    = UI_S(8);
        fill_rect(buf, bw, bh, sp,
                  w->ax + inset, track_y, w->aw - inset * 2, track_h, COL_SLIDER_TRK);

        /* Filled portion from left to knob */
        i32 knob_cx = slider_knob_cx(w);
        i32 fill_w  = knob_cx - (w->ax + inset);
        if (fill_w > 0)
            fill_rect(buf, bw, bh, sp,
                      w->ax + inset, track_y, fill_w, track_h, COL_ACCENT);

        /* Knob approximated as a rounded rect, scaled */
        i32 ks = UI_S(12);
        u32 knob_col = (hovered || w->sl_dragging) ? COL_ACCENT : COL_SLIDER_KNOB;
        fill_round_rect(buf, bw, bh, sp,
                        knob_cx - ks / 2, w->ay + UI_S(4), ks, ks, knob_col);
        stroke_rect(buf, bw, bh, sp,
                    knob_cx - ks / 2, w->ay + UI_S(4), ks, ks, COL_BORDER);
        break;
    }

    case UI_TEXTBOX: {
        u32 border_col = w->tb_focused ? COL_TXTBOX_FCS : COL_BORDER;
        fill_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, w->bg);
        stroke_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, border_col);

        /* Text content, clipped to the box (scaled cell via ui_text clip window) */
        i32 ty = w->ay + (w->ah - g_ui_ch) / 2;
        i32 cx = w->ax + UI_S(4);
        i32 cw = w->aw - UI_S(8);
        if (cw > 0) {
            font2_draw_cell_clip(buf, sp, bw, bh, cx, cx + cw,
                                 cx, ty, w->tb_buf, g_ui_cw, g_ui_ch, w->fg);
        }

        /* Blinking caret when focused */
        if (w->tb_focused) {
            int blink = (app->frame >> 3) & 1;   /* blink every 8 frames */
            if (blink) {
                unsigned long tlen = ui_strlen(w->tb_buf);
                i32 caret_x = cx + (i32)tlen * g_ui_cw;
                /* Clamp caret inside box */
                i32 right_edge = w->ax + w->aw - UI_S(4);
                if (caret_x > right_edge) caret_x = right_edge;
                fill_rect(buf, bw, bh, sp,
                          caret_x, w->ay + UI_S(3), 1, w->ah - UI_S(6), COL_TEXT);
            }
        }
        break;
    }

    case UI_PROGRESS: {
        /* Background track */
        fill_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, w->bg);
        stroke_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, COL_BORDER);
        /* Filled portion */
        i32 fill_w = (w->aw * w->pct) / 100;
        if (fill_w > 0) {
            fill_rect(buf, bw, bh, sp,
                      w->ax, w->ay, fill_w, w->ah, w->fg);
        }
        break;
    }

    case UI_IMAGE_RECT: {
        fill_round_rect(buf, bw, bh, sp,
                        w->ax, w->ay, w->aw, w->ah, w->ir_color);
        stroke_rect(buf, bw, bh, sp,
                    w->ax, w->ay, w->aw, w->ah, COL_BORDER);
        if (w->ir_glyph) {
            /* Center the single glyph (scaled cell via font2) */
            i32 gx = w->ax + (w->aw - g_ui_cw) / 2;
            i32 gy = w->ay + (w->ah - g_ui_ch) / 2;
            char gs[2] = { (char)w->ir_glyph, 0 };
            font2_draw_cell_clip(buf, sp, bw, bh, 0, bw, gx, gy, gs,
                                 g_ui_cw, g_ui_ch, w->ir_fg);
        }
        break;
    }

    case UI_SCROLL: {
        /* Viewport background */
        fill_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, w->bg);

        /* Draw children shifted up by sc_offset, clipped to viewport.
         * We render children manually here (skipping the generic loop below)
         * so we can apply the clip. */
        i32 clip_x = w->ax;
        i32 clip_y = w->ay;
        i32 clip_w = w->aw - 10; /* leave 10px for scrollbar */
        i32 clip_h = w->ah;

        /* Temporarily translate each child's absolute coords by -sc_offset
         * and clip. Because children track their own ax/ay, we adjust and
         * restore. */
        for (int ci = 0; ci < w->nchildren; ci++) {
            ui_widget_t* ch = w->children[ci];
            i32 saved_ay = ch->ay;
            ch->ay -= w->sc_offset;

            /* Only render if child rect intersects viewport */
            if (ch->ay + ch->ah > clip_y && ch->ay < clip_y + clip_h) {
                render_widget(app, ch);
            }

            ch->ay = saved_ay;
        }

        /* Scrollbar track (right 10px strip) */
        i32 bar_x = w->ax + w->aw - 10;
        fill_rect(buf, bw, bh, sp, bar_x, w->ay, 10, w->ah, COL_SURFACE);

        /* Scrollbar thumb */
        i32 thumb_y, thumb_h;
        scroll_thumb(w, &thumb_y, &thumb_h);
        fill_rect(buf, bw, bh, sp, bar_x + 2, thumb_y, 6, thumb_h, COL_SCROLL_BAR);

        /* Viewport border */
        stroke_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, COL_BORDER);

        /* Suppress the generic children loop for UI_SCROLL (already handled). */
        return;
    }

    } /* end switch */

    for (int i = 0; i < w->nchildren; i++)
        render_widget(app, w->children[i]);
}

/* =========================================================================
 * CLICK / KEY DISPATCH
 * =========================================================================
 */

/*
 * Forward declaration for recursive focus clearing.
 */
static void clear_focus_tree(ui_widget_t* w);

static void clear_focus_tree(ui_widget_t* w) {
    if (w->kind == UI_TEXTBOX) w->tb_focused = 0;
    for (int i = 0; i < w->nchildren; i++)
        clear_focus_tree(w->children[i]);
}

/*
 * Walk the subtree front-to-back (children, which paint on top, are tested
 * before their parent) and fire the on_click of the first BUTTON whose rect
 * contains the point. Returns 1 if a click was consumed.
 *
 * For the new widgets, click dispatch also toggles checkboxes, focuses
 * textboxes, and initiates slider drags.
 */
static int dispatch_click(ui_app_t* app, ui_widget_t* w, i32 px, i32 py) {
    /* Children are drawn after (on top of) the parent, and later children
     * on top of earlier ones -> test in reverse to honor z-order. */
    for (int i = w->nchildren - 1; i >= 0; i--) {
        if (dispatch_click(app, w->children[i], px, py)) return 1;
    }

    if (!point_in(w, px, py)) return 0;

    switch (w->kind) {
    case UI_BUTTON:
        if (w->on_click) w->on_click(w->ud);
        return 1;

    case UI_CHECKBOX:
        w->checked ^= 1;
        if (w->on_toggle) w->on_toggle(w->checked, w->ud);
        return 1;

    case UI_TEXTBOX:
        /* Clear all focus first, then set this one. */
        clear_focus_tree(app->root);
        w->tb_focused = 1;
        app->focus = w;
        return 1;

    case UI_SLIDER:
        /* On click, move the knob to the click position and begin dragging. */
        {
            i32 track_x0 = w->ax + 8;
            i32 track_x1 = w->ax + w->aw - 8;
            i32 range = w->sl_max - w->sl_min;
            if (range > 0 && track_x1 > track_x0) {
                i32 new_val = w->sl_min +
                    ((px - track_x0) * range) / (track_x1 - track_x0);
                new_val = ui_clamp(new_val, w->sl_min, w->sl_max);
                if (new_val != w->sl_val) {
                    w->sl_val = new_val;
                    if (w->on_change) w->on_change(w->sl_val, w->ud);
                }
            }
            w->sl_dragging = 1;
        }
        return 1;

    case UI_SCROLL:
        {
            /* Hit the scrollbar thumb? */
            i32 bar_x = w->ax + w->aw - 10;
            if (px >= bar_x) {
                /* Click in scrollbar area -- set drag. */
                w->sc_dragging = 1;
                w->sc_drag_start_y   = py;
                w->sc_drag_start_off = w->sc_offset;
                return 1;
            }
            /* Otherwise pass to children (they are in content-local coords,
             * adjusted via the render loop; dispatch them with offset applied). */
            for (int i = w->nchildren - 1; i >= 0; i--) {
                ui_widget_t* ch = w->children[i];
                i32 saved_ay = ch->ay;
                ch->ay -= w->sc_offset;
                int hit = dispatch_click(app, ch, px, py);
                ch->ay = saved_ay;
                if (hit) return 1;
            }
        }
        return 0;

    default:
        return 0;
    }
}

/*
 * Handle dragging state for sliders and scroll thumbs. Called every frame
 * while the left button is held.
 */
__attribute__((no_stack_protector))
static void dispatch_drag(ui_app_t* app, ui_widget_t* w) {
    for (int i = 0; i < w->nchildren; i++)
        dispatch_drag(app, w->children[i]);

    switch (w->kind) {
    case UI_SLIDER:
        if (w->sl_dragging) {
            i32 track_x0 = w->ax + 8;
            i32 track_x1 = w->ax + w->aw - 8;
            i32 range = w->sl_max - w->sl_min;
            if (range > 0 && track_x1 > track_x0) {
                i32 new_val = w->sl_min +
                    ((app->cur_x - track_x0) * range) / (track_x1 - track_x0);
                new_val = ui_clamp(new_val, w->sl_min, w->sl_max);
                if (new_val != w->sl_val) {
                    w->sl_val = new_val;
                    if (w->on_change) w->on_change(w->sl_val, w->ud);
                }
            }
        }
        break;

    case UI_SCROLL:
        if (w->sc_dragging) {
            /* Map cursor delta to scroll offset delta. */
            i32 bar_h   = w->ah;
            i32 content = w->sc_content_h;
            i32 delta_y = app->cur_y - w->sc_drag_start_y;
            /* Thumb travel range */
            i32 thumb_h_dummy;
            i32 dummy_ty;
            scroll_thumb(w, &dummy_ty, &thumb_h_dummy);
            i32 travel  = bar_h - thumb_h_dummy;
            if (travel <= 0) travel = 1;
            i32 max_off = content - w->ah;
            if (max_off <= 0) { w->sc_dragging = 0; break; }
            i32 new_off = w->sc_drag_start_off +
                          (delta_y * max_off) / travel;
            w->sc_offset = ui_clamp(new_off, 0, max_off);
        }
        break;

    default:
        break;
    }
}

/* Release drag state when LMB released. */
static void dispatch_release(ui_widget_t* w) {
    for (int i = 0; i < w->nchildren; i++)
        dispatch_release(w->children[i]);

    if (w->kind == UI_SLIDER)  w->sl_dragging  = 0;
    if (w->kind == UI_SCROLL)  w->sc_dragging  = 0;
}

/*
 * Deliver a key press to the currently focused textbox.
 * keycode is the raw kernel scancode; pressed is 1 for down, 0 for up.
 */
static void dispatch_key(ui_app_t* app, int keycode, int pressed) {
    if (!pressed) {
        /* Track modifier release. */
        if (keycode == UI_KEY_LSHIFT || keycode == UI_KEY_RSHIFT)
            app->shift = 0;
        return;
    }

    /* Track modifier press. */
    if (keycode == UI_KEY_LSHIFT || keycode == UI_KEY_RSHIFT) {
        app->shift = 1;
        return;
    }

    /* No focused textbox -- nothing to do. */
    if (!app->focus || app->focus->kind != UI_TEXTBOX) return;
    ui_widget_t* tb = app->focus;

    if (keycode == UI_KEY_BACKSPACE) {
        unsigned long len = ui_strlen(tb->tb_buf);
        if (len > 0) tb->tb_buf[len - 1] = '\0';
        return;
    }

    /* Printable character */
    const char* km = app->shift ? g_keymap_hi : g_keymap_lo;
    if (keycode >= 0 && keycode < 256) {
        char c = km[keycode];
        if (c) {
            unsigned long len = ui_strlen(tb->tb_buf);
            if ((int)len < tb->tb_maxlen) {
                tb->tb_buf[len]     = c;
                tb->tb_buf[len + 1] = '\0';
            }
        }
    }
}

/* ---- event loop ---- */

__attribute__((no_stack_protector))
void ui_app_run(ui_app_t* app) {
    if (!app) { for (;;) sc(SYS_YIELD, 0, 0, 0); }

    wl_window* win = app->win;
    long last_frame = sc(SYS_GET_TICKS_MS, 0, 0, 0);

    for (;;) {
        /* (1) Drain input: track cursor, detect left-button press edge. */
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_POINTER) {
                app->cur_x = a;
                app->cur_y = b;
                int btn = (c & 1) ? 1 : 0;   /* bit0 = left button */
                if (btn && !app->btn_prev) {
                    /* 0->1 press edge: click dispatch from root. */
                    dispatch_click(app, app->root, app->cur_x, app->cur_y);
                }
                if (!btn && app->btn_prev) {
                    /* 1->0 release edge: clear drag state. */
                    dispatch_release(app->root);
                }
                app->btn_prev = btn;
            } else if (kind == WL_EVENT_KEY) {
                /* a=keycode, b=pressed */
                dispatch_key(app, a, b);
            } else if (kind == WL_EVENT_RESIZE) {
                /* Compositor resized our buffer (maximize / restore / snap).
                 * wl_poll_event already re-pointed win->{w,h,stride,pixels} to
                 * the new buffer; re-sync the root so its background fill covers
                 * the WHOLE new surface (a=new_w, b=new_h). Widgets keep their
                 * absolute (top-left-anchored) positions — no relayout needed for
                 * the fixed toolkit layouts, but the window now fills the screen
                 * instead of letterboxing stale content. */
                app->root->aw = (i32)win->w;
                app->root->ah = (i32)win->h;
            }
        }

        /* (1b) While LMB held, update drag state. */
        if (app->btn_prev) {
            dispatch_drag(app, app->root);
        }

        /* (2a) Invoke the per-frame tick hook (if registered). */
        if (app->tick) app->tick(app->tick_ud);

        /* (2b) Render the whole retained tree. */
        render_widget(app, app->root);

        /* (3) Present the frame. */
        wl_commit(win);

        /* (4) Increment frame counter (for caret blink). */
        app->frame++;

        /* (5) Yield + pace to ~25 fps (40ms budget). */
        sc(SYS_YIELD, 0, 0, 0);
        for (;;) {
            long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);
            if (now - last_frame >= 40) { last_frame = now; break; }
            sc(SYS_YIELD, 0, 0, 0);
        }
    }
}
