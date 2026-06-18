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

/*
 * NO-FLOAT / NO-XMM GATE (v3): at -O2 the GCC tree/SLP vectorizer turns the
 * toolkit per-pixel fills and struct/array zeroing into INTEGER SSE
 * (pxor, movdqa, pshufd, movaps over xmm). That is NOT floating point -- there
 * is no movss, movsd, cvt or x87 anywhere, and the freestanding ABI is
 * unaffected. We disable ONLY the auto-vectorizer for this TU so objdump shows
 * zero xmm; all integer arithmetic + every existing widget render is
 * byte-identical (this changes instruction selection, never logic).
 *
 * IMPORTANT: we must NOT also let -O2 loop-distribute-patterns turn the
 * toolkit hand-rolled ui_strlen/ui_memset loops into calls to libc strlen/
 * memset (this freestanding lib links no libc). With the vectorizer ON, GCC
 * keeps those loops inline; with it OFF it would synthesize the libc calls, so
 * we ALSO pin -fno-tree-loop-distribute-patterns to keep them inline. Together
 * these two pragmas give: 0 xmm, 0 undefined libc refs, identical behavior.
 */
#pragma GCC optimize("no-tree-vectorize")
#pragma GCC optimize("no-tree-loop-distribute-patterns")

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
#define UI_PCT     100   /* UI-CRISP-0: INTEGER native scale (no fractional blur) */
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

/* =========================================================================
 * INTEGER (Q8) EASING + SINE  -- copied from compositor_m8.c.
 * Input/output are Q8 in [0,256]. NO FLOAT (the #1 correctness gate): all
 * cubics use a single 64-bit multiply chain, the same as the compositor.
 * =========================================================================
 */
static inline i32 clamp256(i32 t) { return t < 0 ? 0 : (t > 256 ? 256 : t); }

/* ease_out_cubic(t) = 1 - (1-t)^3   (Q8 in, Q8 out) */
static i32 ease_out_cubic(i32 t) {
    t = clamp256(t);
    i32 u  = 256 - t;                                   /* (1-t) in Q8 */
    i32 u3 = (i32)(((long long)u * u * u) >> 16);       /* u^3 -> Q8   */
    return clamp256(256 - u3);
}

/* ease_in_cubic(t) = t^3   (Q8 in, Q8 out) -- kept for parity / future use.
 * __attribute__((unused)): part of the spec-requested easing set copied from the
 * compositor; not every curve is wired to a widget yet, so silence -Wall. */
__attribute__((unused))
static i32 ease_in_cubic(i32 t) {
    t = clamp256(t);
    return clamp256((i32)(((long long)t * t * t) >> 16));
}

/* ease_in_out_cubic: <0.5 -> 4t^3, else 1 - (-2t+2)^3 / 2  (Q8). */
__attribute__((unused))
static i32 ease_in_out_cubic(i32 t) {
    t = clamp256(t);
    if (t < 128) {
        i32 t3 = (i32)(((long long)t * t * t) >> 16);
        return clamp256(t3 << 2);
    } else {
        i32 u  = 2 * (256 - t);
        i32 u3 = (i32)(((long long)u * u * u) >> 16);
        return clamp256(256 - (u3 >> 1));
    }
}

/* Fixed-point sine: sin_q(deg) -> Q8 signed (-256..256). Quarter-wave table
 * (0..90 deg, 5-deg step) + quadrant symmetry + linear interp. NO LIBM. */
static const i32 UI_SINQ_TBL[19] = {  /* sin(0..90 by 5 deg) * 256 */
      0,  22,  44,  66,  88, 109, 128, 147, 165, 181,
    196, 209, 221, 231, 240, 247, 252, 255, 256
};
static i32 sin_q(i32 deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    int sign = 1;
    if (deg >= 180) { deg -= 180; sign = -1; }
    if (deg > 90) deg = 180 - deg;
    i32 i    = deg / 5;
    i32 frac = deg - i * 5;                  /* 0..4 */
    i32 a    = UI_SINQ_TBL[i];
    i32 b    = UI_SINQ_TBL[i + 1];
    i32 v    = a + (b - a) * frac / 5;
    return sign * v;
}
static i32 cos_q(i32 deg) { return sin_q(deg + 90); }

/* =========================================================================
 * PUBLIC ANIMATION LAYER (ui_anim_t)
 * =========================================================================
 */
int ui_now_ms(void) {
    return (int)sc(SYS_GET_TICKS_MS, 0, 0, 0);
}

void ui_anim_start(ui_anim_t* a, int from, int to, int dur_ms) {
    if (!a) return;
    a->start_ms = ui_now_ms();
    a->dur_ms   = dur_ms < 0 ? 0 : dur_ms;
    a->from     = from;
    a->to       = to;
    a->active   = 1;
}

int ui_anim_value(ui_anim_t* a, int now_ms) {
    if (!a) return 0;
    if (!a->active || a->dur_ms <= 0) return a->to;
    int elapsed = now_ms - a->start_ms;
    if (elapsed <= 0) return a->from;
    if (elapsed >= a->dur_ms) { a->active = 0; return a->to; }
    /* t in Q8 = elapsed/dur * 256, eased, then lerp from->to. */
    i32 t      = (i32)(((long long)elapsed << 8) / a->dur_ms);
    i32 e      = ease_out_cubic(t);                       /* 0..256 Q8 */
    int span   = a->to - a->from;
    int v      = a->from + (int)(((long long)span * e) >> 8);
    return v;
}

int ui_anim_done(ui_anim_t* a, int now_ms) {
    if (!a) return 1;
    if (!a->active) return 1;
    if (a->dur_ms <= 0) return 1;
    return (now_ms - a->start_ms) >= a->dur_ms;
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

/* v3 animated-widget palette */
#define COL_TOGGLE_OFF  0xFF3A3A3Cu   /* toggle track when off  */
#define COL_TOGGLE_ON   0xFF30D158u   /* toggle track when on   */
#define COL_TOGGLE_KNOB 0xFFFFFFFFu   /* toggle knob            */
#define COL_SPINNER     0xFF0A84FFu   /* spinner arc accent     */
#define COL_BARS_ON     0xFF0A84FFu   /* active signal bar      */
#define COL_BARS_OFF    0xFF38383Au   /* inactive signal bar    */
#define COL_ROW_SEL     0xFF0A84FFu   /* list-row selected accent */

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
    UI_SCROLL,
    /* v3 animated additions (appended -- existing values are unchanged) */
    UI_TOGGLE,
    UI_SPINNER,
    UI_SIGNAL_BARS,
    UI_LIST_ROW
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

    /* DOCK-DND: opt-in drag source (ui_widget_set_draggable). When set, this
     * widget's click is DEFERRED to release and a press+move fires on_drag once.
     * Non-draggable widgets are unchanged (click fires on press). */
    int          draggable;
    void       (*on_drag)(void* ud);

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

    /* ---- v3 animated widgets (additive) ---- */

    /* UI_TOGGLE */
    int          tg_on;                      /* 0 / 1                     */
    ui_anim_t    tg_anim;                    /* knob slide (0..256 Q8)    */
    void       (*tg_on_change)(int state, void* ud);

    /* UI_SIGNAL_BARS */
    int          sg_strength;                /* 0..4 active bars          */
    ui_anim_t    sg_anim;                    /* grow-in (0..256 Q8)       */

    /* UI_LIST_ROW */
    int          lr_selected;                /* 0 / 1                     */
    ui_anim_t    lr_hover;                   /* hover tint (0..256 Q8)    */
    int          lr_hover_prev;              /* last hover state for edge */

    /* UI_TEXTBOX -- extra: password mask (additive to the textbox state) */
    int          tb_mask;                    /* 1 => render dots          */
};

struct ui_app {
    wl_window   *win;
    ui_widget_t *root;

    /* Tracked cursor + left-button edge detection. */
    i32          cur_x, cur_y;
    int          btn_prev;   /* previous left-button state (0/1)          */

    /* DOCK-DND: drag tracking for draggable widgets. */
    i32          press_x, press_y;
    ui_widget_t *drag_armed;   /* draggable widget pressed (click deferred) */
    int          dragging;     /* on_drag already fired for this gesture    */

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
 * NEW ANIMATED WIDGET CONSTRUCTORS (v3, additive)
 * =========================================================================
 */

/* --- Textbox password mask (additive flag on UI_TEXTBOX) --- */

void ui_textbox_set_mask(ui_widget_t* w, int mask) {
    if (!w || w->kind != UI_TEXTBOX) return;
    w->tb_mask = mask ? 1 : 0;
}

/* --- Toggle (iOS-style switch) --- */

#define UI_TOGGLE_W  44
#define UI_TOGGLE_H  24

ui_widget_t* ui_toggle(ui_widget_t* parent, int x, int y, int on,
                       void (*on_change)(int state, void* ud), void* ud) {
    if (!parent) return 0;
    ui_widget_t* tg = widget_alloc();
    if (!tg) return 0;
    tg->kind         = UI_TOGGLE;
    tg->bg           = COL_TOGGLE_OFF;
    tg->fg           = COL_TOGGLE_KNOB;
    tg->tg_on        = on ? 1 : 0;
    tg->tg_on_change = on_change;
    tg->ud           = ud;
    /* Knob position is a Q8 value 0 (off/left) .. 256 (on/right). */
    ui_anim_start(&tg->tg_anim, tg->tg_on ? 256 : 0, tg->tg_on ? 256 : 0, 0);
    if (attach_child(parent, tg, x, y, UI_TOGGLE_W, UI_TOGGLE_H) != 0) {
        tg->used = 0;
        return 0;
    }
    return tg;
}

int ui_toggle_on(ui_widget_t* w) {
    if (!w || w->kind != UI_TOGGLE) return 0;
    return w->tg_on;
}

void ui_toggle_set(ui_widget_t* w, int on) {
    if (!w || w->kind != UI_TOGGLE) return;
    int nv = on ? 1 : 0;
    if (nv == w->tg_on) return;
    /* animate the knob from its CURRENT eased position to the new end. */
    int cur = ui_anim_value(&w->tg_anim, ui_now_ms());
    w->tg_on = nv;
    ui_anim_start(&w->tg_anim, cur, nv ? 256 : 0, 180);
}

/* --- Spinner (rotating arc, continuous) --- */

ui_widget_t* ui_spinner(ui_widget_t* parent, int x, int y, int size) {
    if (!parent) return 0;
    if (size < 8) size = 8;
    ui_widget_t* sp = widget_alloc();
    if (!sp) return 0;
    sp->kind = UI_SPINNER;
    sp->bg   = 0;                 /* no fill -- draws on whatever is under */
    sp->fg   = COL_SPINNER;
    if (attach_child(parent, sp, x, y, size, size) != 0) {
        sp->used = 0;
        return 0;
    }
    return sp;
}

/* --- Signal bars (4-bar WiFi strength meter) --- */

#define UI_BARS_W  24
#define UI_BARS_H  16

ui_widget_t* ui_signal_bars(ui_widget_t* parent, int x, int y, int strength) {
    if (!parent) return 0;
    ui_widget_t* sg = widget_alloc();
    if (!sg) return 0;
    sg->kind        = UI_SIGNAL_BARS;
    sg->bg          = 0;
    sg->fg          = COL_BARS_ON;
    sg->sg_strength = ui_clamp(strength, 0, 4);
    ui_anim_start(&sg->sg_anim, 0, 256, 320);   /* grow-in on create */
    if (attach_child(parent, sg, x, y, UI_BARS_W, UI_BARS_H) != 0) {
        sg->used = 0;
        return 0;
    }
    return sg;
}

void ui_signal_bars_set(ui_widget_t* w, int strength) {
    if (!w || w->kind != UI_SIGNAL_BARS) return;
    w->sg_strength = ui_clamp(strength, 0, 4);
    ui_anim_start(&w->sg_anim, 0, 256, 320);    /* re-trigger grow-in */
}

/* --- List row (selectable, animated hover tint) --- */

ui_widget_t* ui_list_row(ui_widget_t* parent, int x, int y, int w, int h,
                         const char* text, void (*on_click)(void* ud), void* ud) {
    if (!parent) return 0;
    ui_widget_t* lr = widget_alloc();
    if (!lr) return 0;
    lr->kind     = UI_LIST_ROW;
    lr->bg       = COL_SURFACE;
    lr->fg       = COL_TEXT;
    lr->on_click = on_click;
    lr->ud       = ud;
    lr->lr_selected  = 0;
    lr->lr_hover_prev = 0;
    ui_strlcpy(lr->text, text, UI_TEXT_CAP);
    ui_anim_start(&lr->lr_hover, 0, 0, 0);      /* settled at 0 (no tint) */
    if (attach_child(parent, lr, x, y, w, h) != 0) {
        lr->used = 0;
        return 0;
    }
    return lr;
}

int ui_list_row_selected(ui_widget_t* w) {
    if (!w || w->kind != UI_LIST_ROW) return 0;
    return w->lr_selected;
}

void ui_list_row_set_selected(ui_widget_t* w, int sel) {
    if (!w || w->kind != UI_LIST_ROW) return;
    w->lr_selected = sel ? 1 : 0;
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

/* =========================================================================
 * v3 ALPHA-AWARE DRAWING PRIMITIVES (additive; mirror compositor_m8.c).
 * Integer-only Q8 alpha-over. These do NOT change the existing fill_rect /
 * fill_round_rect / stroke_rect or any existing widget render.
 * =========================================================================
 */

/* Alpha-over a single src pixel onto dst using src's 0..255 A channel.
 * Mirrors compositor blend_pixel(); returns an opaque (0xFF) result. */
static inline u32 ui_blend_pixel(u32 src, u32 dst) {
    u32 a = (src >> 24) & 0xFFu;
    if (a == 0xFFu) return 0xFF000000u | (src & 0x00FFFFFFu);
    if (a == 0u)    return dst;
    u32 sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu, sb = src & 0xFFu;
    u32 dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
    u32 ia = 255u - a;
    u32 r = (sr * a + dr * ia) / 255u;
    u32 g = (sg * a + dg * ia) / 255u;
    u32 b = (sb * a + db * ia) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Alpha-over an opaque-RGB src onto dst with an explicit Q8 alpha (0..256).
 * Mirrors compositor blend_pixel_a256() exactly. */
static inline u32 ui_blend_a256(u32 src, u32 dst, u32 a256) {
    if (a256 >= 256u) return 0xFF000000u | (src & 0x00FFFFFFu);
    if (a256 == 0u)   return dst;
    u32 sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu, sb = src & 0xFFu;
    u32 dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
    u32 ia = 256u - a256;
    u32 r = (sr * a256 + dr * ia) >> 8;
    u32 g = (sg * a256 + dg * ia) >> 8;
    u32 b = (sb * a256 + db * ia) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Per-pixel alpha-over fill: blends `argb` (using its A channel) over the
 * existing buffer contents. Clipped like fill_rect. (The file-scope
 * no-tree-vectorize pragma keeps this hot per-pixel loop scalar / xmm-free.) */
static void blend_rect(u32* buf, i32 bw, i32 bh, i32 stride_px,
                       i32 x, i32 y, i32 w, i32 h, u32 argb) {
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w;
    i32 y2 = y + h;
    if (x2 > bw) x2 = bw;
    if (y2 > bh) y2 = bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32* row = buf + (u32)yy * (u32)stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = ui_blend_pixel(argb, row[xx]);
    }
}

/* Vertical gradient: per-row integer lerp of R/G/B from top_argb to bottom_argb.
 * Opaque output (alpha forced to 0xFF). Clipped like fill_rect. */
static void fill_grad_v(u32* buf, i32 bw, i32 bh, i32 stride_px,
                        i32 x, i32 y, i32 w, i32 h,
                        u32 top_argb, u32 bottom_argb) {
    if (w <= 0 || h <= 0) return;
    i32 tr = (i32)((top_argb >> 16) & 0xFFu);
    i32 tg = (i32)((top_argb >> 8)  & 0xFFu);
    i32 tb = (i32)(top_argb & 0xFFu);
    i32 br = (i32)((bottom_argb >> 16) & 0xFFu);
    i32 bg = (i32)((bottom_argb >> 8)  & 0xFFu);
    i32 bb = (i32)(bottom_argb & 0xFFu);
    i32 den = h > 1 ? h - 1 : 1;
    for (i32 ry = 0; ry < h; ry++) {
        /* signed deltas so a descending channel doesn't wrap */
        i32 r = tr + (br - tr) * ry / den;
        i32 g = tg + (bg - tg) * ry / den;
        i32 b = tb + (bb - tb) * ry / den;
        u32 row_col = 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        fill_rect(buf, bw, bh, stride_px, x, y + ry, w, 1, row_col);
    }
}

/* True rounded-rect fill: interior is opaque `argb`; the 1px corner boundary is
 * blended ~50% so the edge reads smooth on ANY backdrop (no window-bg notch
 * hack). For each corner box, dx*dx+dy*dy is compared against r*r:
 *   inside  (d2 <= rr-band) -> opaque;  boundary -> 50% blend;  outside -> skip.
 * Keeps the original fill_round_rect() untouched. */
static void fill_round_rect_r(u32* buf, i32 bw, i32 bh, i32 stride_px,
                              i32 x, i32 y, i32 w, i32 h, i32 radius, u32 argb) {
    if (w <= 0 || h <= 0) return;
    i32 r = radius;
    if (r < 1) { fill_rect(buf, bw, bh, stride_px, x, y, w, h, argb); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1)     { fill_rect(buf, bw, bh, stride_px, x, y, w, h, argb); return; }

    u32 edge = (argb & 0x00FFFFFFu) | 0x80000000u;   /* same RGB, ~50% alpha */
    i32 rr   = r * r;

    /* Middle band (full width, between the two corner rows): plain opaque. */
    fill_rect(buf, bw, bh, stride_px, x, y + r, w, h - 2 * r, argb);

    /* Top + bottom corner rows: opaque center span + rounded ends per row. */
    for (i32 ry = 0; ry < r; ry++) {
        /* distance of this row's pixel from the corner's circle center.
         * Corner circle center is at (x+r, y+r) [top-left], dy = r-1-ry. */
        i32 dy  = (r - 1) - ry;
        i32 dy2 = dy * dy;

        /* For each column within the corner box, decide opaque/edge/skip. */
        /* TOP row band at buffer y = y+ry ; BOTTOM at y = y+h-1-ry. */
        i32 top_y = y + ry;
        i32 bot_y = y + h - 1 - ry;

        /* Opaque center span (between the two corner boxes) for these rows. */
        fill_rect(buf, bw, bh, stride_px, x + r, top_y, w - 2 * r, 1, argb);
        fill_rect(buf, bw, bh, stride_px, x + r, bot_y, w - 2 * r, 1, argb);

        for (i32 cx = 0; cx < r; cx++) {
            i32 dx  = (r - 1) - cx;       /* distance into the left corner */
            i32 d2  = dx * dx + dy2;
            u32 col;
            if (d2 <= rr - r)       col = argb;     /* well inside  -> opaque */
            else if (d2 <= rr + r)  col = edge;     /* on boundary  -> 50%    */
            else                    continue;       /* outside      -> skip   */
            /* left + right mirrors, top + bottom mirrors (4 corners) */
            i32 lx = x + cx;
            i32 rx = x + w - 1 - cx;
            if (col == argb) {
                fill_rect (buf, bw, bh, stride_px, lx, top_y, 1, 1, col);
                fill_rect (buf, bw, bh, stride_px, rx, top_y, 1, 1, col);
                fill_rect (buf, bw, bh, stride_px, lx, bot_y, 1, 1, col);
                fill_rect (buf, bw, bh, stride_px, rx, bot_y, 1, 1, col);
            } else {
                blend_rect(buf, bw, bh, stride_px, lx, top_y, 1, 1, col);
                blend_rect(buf, bw, bh, stride_px, rx, top_y, 1, 1, col);
                blend_rect(buf, bw, bh, stride_px, lx, bot_y, 1, 1, col);
                blend_rect(buf, bw, bh, stride_px, rx, bot_y, 1, 1, col);
            }
        }
    }
}

/* Soft drop shadow: a few decreasing-alpha black blend passes, each offset
 * further down-right, to fake a 2-4px blurred shadow under a card. Draw this
 * BEFORE the card so the card paints on top. radius widens the offsets. */
static void draw_shadow(u32* buf, i32 bw, i32 bh, i32 stride_px,
                        i32 x, i32 y, i32 w, i32 h, i32 radius) {
    if (w <= 0 || h <= 0) return;
    i32 spread = radius < 2 ? 2 : (radius > 4 ? 4 : radius);
    /* Decreasing-alpha black, growing offset: outer passes are faintest. */
    static const u32 ALPHA[4] = { 0x10u, 0x18u, 0x28u, 0x40u };
    for (i32 p = spread; p >= 1; p--) {
        u32 a   = ALPHA[p - 1];
        u32 col = (a << 24);                 /* black with this alpha */
        i32 off = p;                          /* down-right offset      */
        i32 grow = p;                         /* slight outward grow    */
        blend_rect(buf, bw, bh, stride_px,
                   x - grow + off, y - grow + off,
                   w + 2 * grow, h + 2 * grow, col);
    }
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
            const char* shown = w->tb_buf;
            /* tb_mask (additive): render a dot per char (passphrase field). The
             * non-masked path is byte-identical to before -- only when the flag
             * is set do we build a transient '*' string of the same length. */
            char masked[UI_TEXTBOX_MAXBUF];
            if (w->tb_mask) {
                unsigned long n = ui_strlen(w->tb_buf);
                if (n >= UI_TEXTBOX_MAXBUF) n = UI_TEXTBOX_MAXBUF - 1;
                for (unsigned long mi = 0; mi < n; mi++) masked[mi] = '*';
                masked[n] = '\0';
                shown = masked;
            }
            font2_draw_cell_clip(buf, sp, bw, bh, cx, cx + cw,
                                 cx, ty, shown, g_ui_cw, g_ui_ch, w->fg);
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

    /* ---- v3 animated widget drawing ---- */

    case UI_TOGGLE: {
        int now = ui_now_ms();
        i32 knob_q = ui_anim_value(&w->tg_anim, now);   /* 0..256 Q8 */
        /* Track: lerp between off + on color by knob position (smooth fill). */
        u32 track = ui_blend_a256(COL_TOGGLE_ON, COL_TOGGLE_OFF, (u32)knob_q);
        i32 rad = w->ah / 2;
        fill_round_rect_r(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, rad, track);
        /* Subtle top-down sheen on the track (slightly lighter at top). */
        {
            u32 sheen_top = (track & 0x00FFFFFFu) | 0x22000000u;  /* +light  */
            u32 sheen_bot = (track & 0x00FFFFFFu) | 0x00000000u;  /* nothing */
            (void)sheen_bot;
            blend_rect(buf, bw, bh, sp, w->ax + rad, w->ay,
                       w->aw - 2 * rad, w->ah / 3, sheen_top);
        }

        /* Knob: a circle that slides left<->right with the Q8 position, with a
         * soft drop shadow beneath it for depth. */
        i32 pad      = UI_S(2);
        i32 ksz      = w->ah - pad * 2;
        i32 travel   = w->aw - pad * 2 - ksz;
        i32 kx       = w->ax + pad + (travel * knob_q) / 256;
        i32 ky       = w->ay + pad;
        draw_shadow(buf, bw, bh, sp, kx, ky, ksz, ksz, 2);
        fill_round_rect_r(buf, bw, bh, sp, kx, ky, ksz, ksz, ksz / 2, COL_TOGGLE_KNOB);
        break;
    }

    case UI_SPINNER: {
        /* Continuous rotating arc: a ring of dots whose alpha trails the head.
         * Phase advances with the millisecond clock (no app tick needed). */
        int now   = ui_now_ms();
        i32 cx     = w->ax + w->aw / 2;
        i32 cy     = w->ay + w->ah / 2;
        i32 radius = (w->aw < w->ah ? w->aw : w->ah) / 2 - UI_S(2);
        if (radius < 2) radius = 2;
        i32 head  = (now / 4) % 360;          /* rotate ~90 deg / s */
        i32 dot   = UI_S(2);
        if (dot < 1) dot = 1;
        /* 12 dots around the circle; brightness fades behind the head. */
        for (int k = 0; k < 12; k++) {
            i32 ang  = head - k * 30;
            i32 px   = cx + (radius * cos_q(ang)) / 256;
            i32 py   = cy + (radius * sin_q(ang)) / 256;
            u32 a256 = (u32)(256 - k * 20);   /* trailing fade */
            if ((i32)a256 < 32) a256 = 32;
            u32 col  = (w->fg & 0x00FFFFFFu) | ((a256 > 255 ? 255u : a256) << 24);
            blend_rect(buf, bw, bh, sp, px - dot / 2, py - dot / 2, dot, dot, col);
        }
        break;
    }

    case UI_SIGNAL_BARS: {
        int now    = ui_now_ms();
        i32 grow   = ui_anim_value(&w->sg_anim, now);   /* 0..256 Q8 */
        i32 nbars  = 4;
        i32 gap    = UI_S(2);
        i32 total_gap = gap * (nbars - 1);
        i32 bw_each = (w->aw - total_gap) / nbars;
        if (bw_each < 1) bw_each = 1;
        for (i32 k = 0; k < nbars; k++) {
            /* Each bar's full height steps up; animated height eases in. */
            i32 full_h = (w->ah * (k + 1)) / nbars;
            i32 cur_h  = (full_h * grow) / 256;
            if (cur_h < 1) cur_h = 1;
            i32 bx = w->ax + k * (bw_each + gap);
            i32 by = w->ay + w->ah - cur_h;
            u32 col = (k < w->sg_strength) ? COL_BARS_ON : COL_BARS_OFF;
            fill_round_rect_r(buf, bw, bh, sp, bx, by, bw_each, cur_h, UI_S(1), col);
        }
        break;
    }

    case UI_LIST_ROW: {
        int now = ui_now_ms();
        /* Animate the hover tint toward target on hover-state edges. */
        if (hovered != w->lr_hover_prev) {
            int cur = ui_anim_value(&w->lr_hover, now);
            ui_anim_start(&w->lr_hover, cur, hovered ? 256 : 0, 140);
            w->lr_hover_prev = hovered;
        }
        i32 tint = ui_anim_value(&w->lr_hover, now);    /* 0..256 Q8 */

        /* Base fill: a subtle top->bottom gradient (slightly darker at the
         * bottom) so stacked rows read as distinct cards. Then blend a hover
         * tint, then the selected accent. */
        {
            u32 g_top = w->bg;
            u32 r0 = (w->bg >> 16) & 0xFFu, g0 = (w->bg >> 8) & 0xFFu, b0 = w->bg & 0xFFu;
            r0 = r0 > 8 ? r0 - 8 : 0; g0 = g0 > 8 ? g0 - 8 : 0; b0 = b0 > 8 ? b0 - 8 : 0;
            u32 g_bot = 0xFF000000u | (r0 << 16) | (g0 << 8) | b0;
            fill_grad_v(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, g_top, g_bot);
        }
        if (tint > 0) {
            u32 tcol = (COL_HOVER & 0x00FFFFFFu) | ((u32)tint << 24);
            blend_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, tcol);
        }
        if (w->lr_selected) {
            /* thin accent bar on the left edge + a faint accent wash */
            fill_rect(buf, bw, bh, sp, w->ax, w->ay, UI_S(3), w->ah, COL_ROW_SEL);
            u32 wash = (COL_ROW_SEL & 0x00FFFFFFu) | 0x30000000u;
            blend_rect(buf, bw, bh, sp, w->ax, w->ay, w->aw, w->ah, wash);
        }

        /* Caption (vertically centered, small left inset). */
        if (w->text[0]) {
            i32 ty = w->ay + (w->ah - g_ui_ch) / 2;
            i32 tx = w->ax + UI_S(8);
            ui_text(buf, sp, bw, bh, tx, ty, w->text, w->fg);
        }
        break;
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

    case UI_TOGGLE: {
        /* Flip state, animate the knob, fire on_change. */
        int cur = ui_anim_value(&w->tg_anim, ui_now_ms());
        w->tg_on ^= 1;
        ui_anim_start(&w->tg_anim, cur, w->tg_on ? 256 : 0, 180);
        if (w->tg_on_change) w->tg_on_change(w->tg_on, w->ud);
        return 1;
    }

    case UI_LIST_ROW:
        w->lr_selected = 1;
        if (w->on_click) w->on_click(w->ud);
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
                /* UI-TOOLKIT-FIX-0: only dispatch to a child actually visible in
                 * the viewport [w->ay, w->ay+w->ah) -- matching the render-path
                 * clip (see UI_SCROLL render). Otherwise a click lands on an item
                 * scrolled out of view. */
                int visible = (ch->ay + ch->ah > w->ay) && (ch->ay < w->ay + w->ah);
                int hit = visible ? dispatch_click(app, ch, px, py) : 0;
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

/* Find the topmost DRAGGABLE widget whose rect contains (px,py), or NULL. */
static ui_widget_t* draggable_at(ui_widget_t* w, i32 px, i32 py) {
    for (int i = w->nchildren - 1; i >= 0; i--) {
        ui_widget_t* hit = draggable_at(w->children[i], px, py);
        if (hit) return hit;
    }
    if (w->draggable && px >= w->ax && px < w->ax + w->aw &&
        py >= w->ay && py < w->ay + w->ah)
        return w;
    return (ui_widget_t*)0;
}

/* Mark a widget as a drag source: its click is deferred to release and a
 * press+move fires on_drag(w->ud) once. Used for "drag a tile to the dock". */
void ui_widget_set_draggable(ui_widget_t* w, void (*on_drag)(void* ud)) {
    if (!w) return;
    w->draggable = 1;
    w->on_drag   = on_drag;
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
                    /* 0->1 press edge. If a DRAGGABLE widget is under the cursor,
                     * arm it and DEFER its click to release; otherwise dispatch
                     * the click immediately (unchanged for non-draggable). */
                    ui_widget_t* dw = draggable_at(app->root, app->cur_x, app->cur_y);
                    if (dw) {
                        app->drag_armed = dw;
                        app->press_x = app->cur_x;
                        app->press_y = app->cur_y;
                        app->dragging = 0;
                    } else {
                        dispatch_click(app, app->root, app->cur_x, app->cur_y);
                    }
                }
                if (!btn && app->btn_prev) {
                    /* 1->0 release edge. A deferred draggable click fires now iff
                     * it was NOT promoted to a drag. */
                    if (app->drag_armed) {
                        if (!app->dragging)
                            dispatch_click(app, app->root, app->press_x, app->press_y);
                        app->drag_armed = (ui_widget_t*)0;
                        app->dragging = 0;
                    }
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

        /* (1b) While LMB held, update drag state. Promote an armed draggable
         * widget to a drag once the cursor moves past a small threshold, and
         * fire its on_drag exactly once (the start menu uses this to hand a tile
         * off to the dock). */
        if (app->btn_prev) {
            if (app->drag_armed && !app->dragging) {
                i32 dx = app->cur_x - app->press_x, dy = app->cur_y - app->press_y;
                if (dx * dx + dy * dy > 36) {
                    app->dragging = 1;
                    if (app->drag_armed->on_drag)
                        app->drag_armed->on_drag(app->drag_armed->ud);
                }
            }
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
