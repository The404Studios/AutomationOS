/*
 * ide_editor.c -- editable text editor for the IDE (VS-Code-lite).
 *
 * Operates directly on a->src (a flat char buffer of IDE_SRC_CAP bytes) with
 * a->src_len bytes of live content. Lines are '\n'-delimited; the buffer is NOT
 * NUL-terminated by contract (we always track src_len). The caret is stored as
 * a (line, col) pair and converted to a byte offset on demand.
 *
 * Editing keeps a->src_len consistent and sets editor.dirty. After any edit
 * that changes structure we DO NOT re-run the semantic model (that happens
 * lazily on save / build / workspace switch) so typing stays responsive.
 *
 * Rendering: a left line-number gutter, syntax-highlighted code via the shared
 * lexer (lex_classify_line), a current-line highlight, and a blinking block
 * caret. Everything clips to the supplied Rect.
 *
 * Freestanding: no libc / malloc / stdio. Scratch is static or stack-bounded.
 */
#include "ide.h"
#include "ide_theme.h"
#include "ide_lex.h"
#include "ide_editor.h"
#include "../../lib/keymap/keymap.h"   /* shared US-QWERTY: caps-lock + shift + symbols */

/* ---- tunables (mirror ide_codeview.c where it matters) ---- */
#define ED_HEADER_H   (ROW_H + 2)
#define ED_GUTTER_CH  5
#define ED_LINECAP    1024
#define ED_TABW       4
#define ED_SBAR_W     3
#define ED_BLINK_MS   500            /* caret on/off half-period */

/* ---- tiny local helpers ---- */
static inline int ed_min(int a, int b) { return a < b ? a : b; }
static inline int ed_max(int a, int b) { return a > b ? a : b; }

static inline int ed_gutter_w(void) { return ED_GUTTER_CH * GFX_FW + 2 * PAD; }

static inline Rect ed_body(Rect r) {
    Rect b;
    b.x = r.x; b.y = r.y + ED_HEADER_H;
    b.w = r.w; b.h = r.h - ED_HEADER_H;
    if (b.h < 0) b.h = 0;
    return b;
}

/* Count newline-delimited lines (a non-empty buffer with no trailing '\n'
 * still has its final partial line counted; an empty buffer is 1 line). */
int ed_line_count(const char* src, int len) {   /* non-static: ide.c's go-to-line uses it */
    int n = 1, i;
    if (len <= 0) return 1;
    for (i = 0; i < len; i++) if (src[i] == '\n') n++;
    return n;
}

/* Byte offset at which line `ln` (0-based) starts. Clamped to [0,len]. */
static int ed_line_start(const char* src, int len, int ln) {
    int i = 0, cur = 0;
    if (ln <= 0) return 0;
    for (i = 0; i < len; i++) {
        if (src[i] == '\n') {
            cur++;
            if (cur == ln) return i + 1;
        }
    }
    return len;   /* past last line: clamp to EOF */
}

/* Length (excluding '\n') of line `ln`. */
static int ed_line_len(const char* src, int len, int ln) {
    int s = ed_line_start(src, len, ln);
    int e = s;
    while (e < len && src[e] != '\n') e++;
    return e - s;
}

/* (line,col) -> byte offset, both clamped to valid ranges. */
static int ed_caret_off(struct Ide* a) {
    int s = ed_line_start(a->src, a->src_len, a->editor.caret_line);
    int ll = ed_line_len(a->src, a->src_len, a->editor.caret_line);
    int col = a->editor.caret_col;
    if (col > ll) col = ll;
    if (col < 0) col = 0;
    return s + col;
}

/* Clamp the caret line/col into the current buffer. */
static void ed_clamp_caret(struct Ide* a) {
    Editor* e = &a->editor;
    int nlines = ed_line_count(a->src, a->src_len);
    if (e->caret_line < 0) e->caret_line = 0;
    if (e->caret_line >= nlines) e->caret_line = nlines - 1;
    int ll = ed_line_len(a->src, a->src_len, e->caret_line);
    if (e->caret_col < 0) e->caret_col = 0;
    if (e->caret_col > ll) e->caret_col = ll;
}

/* Insert one byte at offset `off`, shifting the tail up. Returns 1 on success
 * (room available), 0 if the buffer is full. */
static int ed_insert_byte(struct Ide* a, int off, char ch) {
    if (a->src_len + 1 > IDE_SRC_CAP) return 0;
    if (off < 0) off = 0;
    if (off > a->src_len) off = a->src_len;
    for (int i = a->src_len; i > off; i--) a->src[i] = a->src[i - 1];
    a->src[off] = ch;
    a->src_len++;
    a->editor.dirty = 1;
    return 1;
}

/* Delete one byte at offset `off`, shifting the tail down. */
static void ed_delete_byte(struct Ide* a, int off) {
    if (off < 0 || off >= a->src_len) return;
    for (int i = off; i < a->src_len - 1; i++) a->src[i] = a->src[i + 1];
    a->src_len--;
    a->editor.dirty = 1;
}

/* ---- public reset ---- */
void ide_editor_reset(struct Ide* a) {
    Editor* e = &a->editor;
    e->caret_line = 0;
    e->caret_col = 0;
    e->want_col = 0;
    e->top_line = 0;
    e->left_col = 0;
    e->dirty = 0;
    e->blink_ms = 0;
    e->sel_anchor_off = -1;
    /* focus is preserved across file opens so the user keeps typing */
}

int ide_editor_dirty(struct Ide* a) { return a->editor.dirty; }

/* ---- save ---- */
int ide_editor_save(struct Ide* a) {
    if (!a->cur_file[0]) return -1;
    int r = ide_write_file(a->cur_file, a->src, a->src_len);
    if (r == 0) {
        a->editor.dirty = 0;
        /* Re-run analysis so the LEGO workspace + status bar reflect edits. */
        model_parse(&a->model, a->src, a->src_len, a->cur_file);
        if (a->model.nfuncs > 0) {
            if (a->focus_func >= a->model.nfuncs) a->focus_func = 0;
            a->model.focus = a->focus_func;
        } else {
            a->model.focus = -1;
        }
        model_analyze(&a->model);
    }
    return r;
}

/* ---- blink ---- */
void ide_editor_tick(struct Ide* a, int dt_ms) {
    a->editor.blink_ms += dt_ms;
    if (a->editor.blink_ms >= 2 * ED_BLINK_MS) a->editor.blink_ms = 0;
}

/* ---- Ctrl+D: duplicate current line ---- */
void ide_editor_duplicate_line(struct Ide* a) {
    Editor* e = &a->editor;
    int line = e->caret_line;
    int line_start = ed_line_start(a->src, a->src_len, line);
    int line_len = ed_line_len(a->src, a->src_len, line);

    /* Check if there's room to duplicate (line + newline + line) */
    if (a->src_len + line_len + 1 > IDE_SRC_CAP) return;

    /* Insert newline at end of current line, then duplicate line content */
    int eol = line_start + line_len;
    if (!ed_insert_byte(a, eol, '\n')) return;

    /* Copy each character from current line to next line */
    for (int i = 0; i < line_len; i++) {
        char ch = a->src[line_start + i];
        if (!ed_insert_byte(a, eol + 1 + i, ch)) break;
    }

    /* Move caret to duplicated line, same column */
    e->caret_line++;
    ed_clamp_caret(a);
}

/* ---- Ctrl+Shift+K: delete current line ---- */
void ide_editor_delete_line(struct Ide* a) {
    Editor* e = &a->editor;
    int line = e->caret_line;
    int line_start = ed_line_start(a->src, a->src_len, line);
    int line_len = ed_line_len(a->src, a->src_len, line);

    /* Delete all chars in line */
    for (int i = 0; i < line_len; i++) {
        ed_delete_byte(a, line_start);
    }

    /* Delete the newline if present (unless it's the last line) */
    if (line_start < a->src_len && a->src[line_start] == '\n') {
        ed_delete_byte(a, line_start);
    }

    /* Keep caret on same line (now contains next line's content) */
    e->caret_col = 0;
    e->want_col = 0;
    ed_clamp_caret(a);
}

/* Ensure the caret is visible: adjust top_line / left_col so the caret cell is
 * within the body's visible rows/cols. */
static void ed_scroll_to_caret(struct Ide* a, Rect body) {
    Editor* e = &a->editor;
    int line_h = GFX_FH;
    int vis_rows = body.h / line_h;
    if (vis_rows < 1) vis_rows = 1;

    int gutter_w = ed_gutter_w();
    int code_w = body.w - gutter_w - 2 * PAD - ED_SBAR_W;
    if (code_w < GFX_FW) code_w = GFX_FW;
    int vis_cols = code_w / GFX_FW;
    if (vis_cols < 1) vis_cols = 1;

    if (e->caret_line < e->top_line) e->top_line = e->caret_line;
    if (e->caret_line >= e->top_line + vis_rows)
        e->top_line = e->caret_line - vis_rows + 1;
    if (e->top_line < 0) e->top_line = 0;

    if (e->caret_col < e->left_col) e->left_col = e->caret_col;
    if (e->caret_col >= e->left_col + vis_cols)
        e->left_col = e->caret_col - vis_cols + 1;
    if (e->left_col < 0) e->left_col = 0;
}

/* ---- key handling ---- */
/* evdev keycodes we react to (matches kernel/include/input.h) */
#define EDK_BACKSPACE 14
#define EDK_TAB       15
#define EDK_ENTER     28
#define EDK_S         31
#define EDK_UP        103
#define EDK_DOWN      108
#define EDK_LEFT      105
#define EDK_RIGHT     106
#define EDK_HOME      102
#define EDK_END       107
#define EDK_DELETE    111
#define EDK_PAGEUP    104
#define EDK_PAGEDOWN  109

/* Per-editor keyboard layout state. The compositor/IDE forward shift & ctrl as
 * separate parameters (ide.c swallows the shift KEY events itself), but NOT the
 * caps-lock toggle -- a CapsLock key-DOWN (evdev 58) falls through to us. We
 * therefore mirror the caller's shift/ctrl into this state every call and let
 * keymap_resolve() own the caps-lock toggle + the full US symbol map, so the
 * editor now gets uppercase via caps-lock as well as Shift+symbol glyphs. */
static keymap_state_t ed_km;

int ide_editor_key(struct Ide* a, int keycode, char ch, int shift, int ctrl) {
    Editor* e = &a->editor;
    (void)ch;   /* the upstream char was shift-only (no caps); we re-resolve below */

    /* Mirror the caller-tracked modifiers (authoritative; ide.c eats the shift
     * KEY events, so we can't see their up/down edges ourselves). */
    ed_km.shift_l = (uint8_t)(shift ? 1 : 0);
    ed_km.shift_r = 0;
    ed_km.ctrl    = (uint8_t)(ctrl ? 1 : 0);

    /* Resolve the printable glyph for THIS key-DOWN through the shared keymap.
     * This also folds the CapsLock toggle into ed_km (keycode 58) and returns 0
     * for arrows/Enter/Backspace/Tab/etc., so the special-key switch below is
     * unaffected. We only use kch on the printable path. */
    char kch = keymap_resolve((uint8_t)keycode, 1, &ed_km);

    /* Ctrl+S: save (handled even if a printable 's' resolved). */
    if (ctrl && keycode == EDK_S) {
        ide_editor_save(a);
        return 1;
    }
    /* Other Ctrl-chords are not editor edits -- let the caller route them. */
    if (ctrl) return 0;

    ed_clamp_caret(a);

    switch (keycode) {
    case EDK_LEFT: {
        if (e->caret_col > 0) e->caret_col--;
        else if (e->caret_line > 0) {
            e->caret_line--;
            e->caret_col = ed_line_len(a->src, a->src_len, e->caret_line);
        }
        e->want_col = e->caret_col;
        return 1;
    }
    case EDK_RIGHT: {
        int ll = ed_line_len(a->src, a->src_len, e->caret_line);
        if (e->caret_col < ll) e->caret_col++;
        else if (e->caret_line < ed_line_count(a->src, a->src_len) - 1) {
            e->caret_line++; e->caret_col = 0;
        }
        e->want_col = e->caret_col;
        return 1;
    }
    case EDK_UP:
        if (e->caret_line > 0) {
            e->caret_line--;
            e->caret_col = e->want_col;
            ed_clamp_caret(a);
        }
        return 1;
    case EDK_DOWN:
        if (e->caret_line < ed_line_count(a->src, a->src_len) - 1) {
            e->caret_line++;
            e->caret_col = e->want_col;
            ed_clamp_caret(a);
        }
        return 1;
    case EDK_HOME: {
        /* Ctrl+Home: jump to start of file */
        if (ctrl) {
            e->caret_line = 0;
            e->caret_col = 0;
            e->want_col = 0;
            return 1;
        }

        /* Smart Home: toggle between first non-whitespace and column 0 */
        int line_start = ed_line_start(a->src, a->src_len, e->caret_line);
        int line_len = ed_line_len(a->src, a->src_len, e->caret_line);

        /* Find first non-whitespace column */
        int first_nonws = 0;
        for (int i = 0; i < line_len; i++) {
            char ch = a->src[line_start + i];
            if (ch != ' ' && ch != '\t') {
                first_nonws = i;
                break;
            }
        }

        /* Toggle: if at column 0, go to first non-ws; otherwise go to 0 */
        if (e->caret_col == 0 && first_nonws > 0) {
            e->caret_col = first_nonws;
        } else {
            e->caret_col = 0;
        }
        e->want_col = e->caret_col;
        return 1;
    }
    case EDK_END: {
        /* Ctrl+End: jump to end of file */
        if (ctrl) {
            int last_line = ed_line_count(a->src, a->src_len) - 1;
            if (last_line < 0) last_line = 0;
            e->caret_line = last_line;
            e->caret_col = ed_line_len(a->src, a->src_len, last_line);
            e->want_col = e->caret_col;
            return 1;
        }

        /* Normal End: go to end of current line */
        e->caret_col = ed_line_len(a->src, a->src_len, e->caret_line);
        e->want_col = e->caret_col;
        return 1;
    }
    case EDK_PAGEUP:
        e->caret_line -= 12; if (e->caret_line < 0) e->caret_line = 0;
        e->caret_col = e->want_col; ed_clamp_caret(a);
        return 1;
    case EDK_PAGEDOWN: {
        int last = ed_line_count(a->src, a->src_len) - 1;
        e->caret_line += 12; if (e->caret_line > last) e->caret_line = last;
        e->caret_col = e->want_col; ed_clamp_caret(a);
        return 1;
    }
    case EDK_BACKSPACE: {
        int off = ed_caret_off(a);
        if (off > 0) {
            int prev_is_nl = (a->src[off - 1] == '\n');
            if (prev_is_nl) {
                int pl = e->caret_line - 1;
                int pll = ed_line_len(a->src, a->src_len, pl);
                ed_delete_byte(a, off - 1);
                e->caret_line = pl;
                e->caret_col = pll;
            } else {
                ed_delete_byte(a, off - 1);
                e->caret_col--;
            }
            e->want_col = e->caret_col;
        }
        return 1;
    }
    case EDK_DELETE: {
        int off = ed_caret_off(a);
        if (off < a->src_len) ed_delete_byte(a, off);
        return 1;
    }
    case EDK_ENTER: {
        int off = ed_caret_off(a);
        if (!ed_insert_byte(a, off, '\n')) return 1;

        e->caret_line++;
        e->caret_col = 0;
        e->want_col = 0;

        /* Auto-indent: preserve previous line's leading whitespace */
        if (e->caret_line > 0) {
            int prev_line = e->caret_line - 1;
            int prev_start = ed_line_start(a->src, a->src_len, prev_line);
            int prev_len = ed_line_len(a->src, a->src_len, prev_line);

            /* Count leading spaces/tabs on previous line */
            int indent = 0;
            for (int i = 0; i < prev_len && indent < 64; i++) {
                char ch = a->src[prev_start + i];
                if (ch == ' ' || ch == '\t') indent++;
                else break;
            }

            /* Insert same indentation on new line */
            int new_off = ed_caret_off(a);
            for (int i = 0; i < indent; i++) {
                char ch = a->src[prev_start + i];
                if (!ed_insert_byte(a, new_off + i, ch)) break;
                e->caret_col++;
            }
            e->want_col = e->caret_col;
        }
        return 1;
    }
    case EDK_TAB: {
        /* insert ED_TABW spaces (soft tabs) */
        int off = ed_caret_off(a);
        for (int i = 0; i < ED_TABW; i++) {
            if (!ed_insert_byte(a, off + i, ' ')) break;
            e->caret_col++;
        }
        e->want_col = e->caret_col;
        return 1;
    }
    default: break;
    }

    /* printable character (keymap-resolved: honours caps-lock + Shift+symbol) */
    if (kch >= 32 && kch < 127) {
        int off = ed_caret_off(a);
        if (ed_insert_byte(a, off, kch)) {
            e->caret_col++;
            e->want_col = e->caret_col;
        }
        return 1;
    }
    return 0;
}

/* ---- click: place caret ---- */
int ide_editor_click(struct Ide* a, Rect r, int mx, int my) {
    Editor* e = &a->editor;
    e->focused = 1;

    Rect body = ed_body(r);
    if (body.h <= 0) return 1;
    if (my < body.y) return 1;

    int line_h = GFX_FH;
    int gutter_w = ed_gutter_w();
    int code_x = body.x + gutter_w + PAD;

    int row = (my - body.y) / line_h;
    int ln = e->top_line + row;
    int nlines = ed_line_count(a->src, a->src_len);
    if (ln < 0) ln = 0;
    if (ln >= nlines) ln = nlines - 1;
    e->caret_line = ln;

    int col = (mx - code_x) / GFX_FW + e->left_col;
    if (col < 0) col = 0;
    int ll = ed_line_len(a->src, a->src_len, ln);
    if (col > ll) col = ll;
    e->caret_col = col;
    e->want_col = col;
    return 1;
}

/* class value -> text colour (same mapping as the code-view) */
static uint32_t ed_class_color(unsigned char c) {
    switch (c) {
        case LEXCLS_KEYWORD: return TH_PURPLE;
        case LEXCLS_TYPE:    return TH_CYAN;
        case LEXCLS_STRING:  return TH_GREEN;
        case LEXCLS_COMMENT: return TH_TEXT_FAINT;
        case LEXCLS_NUMBER:  return TH_ORANGE;
        case LEXCLS_PREPROC: return TH_MAGENTA;
        case LEXCLS_CALL:    return TH_BLUE;
        default:             return TH_TEXT;
    }
}

/* Right-aligned line number into out. */
static void ed_itoa_pad(int v, int w, char* out, int cap) {
    char tmp[16];
    int n = 0, k = 0, p = 0;
    if (w > cap - 1) w = cap - 1;
    if (v < 0) v = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (k = 0; k < w - n; k++) out[p++] = ' ';
    for (k = n - 1; k >= 0; k--) out[p++] = tmp[k];
    out[p] = '\0';
}

void ide_editor_render(struct Ide* a, Canvas* cv, Rect r) {
    static unsigned char cls[ED_LINECAP];
    static char numbuf[16];
    Editor* e = &a->editor;

    if (!cv || r.w <= 0 || r.h <= 0) return;

    /* background + header */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL2);
    gfx_fill(cv, r.x, r.y, r.w, ED_HEADER_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + ED_HEADER_H - 1, r.w, TH_BORDER);

    /* header: filename + dirty marker + Ln/Col */
    {
        const char* file = a->cur_file[0] ? a->cur_file : "(no file)";
        int hx = r.x + PAD;
        int hy = r.y + (ED_HEADER_H - GFX_FH) / 2;
        int hclip = r.w - 2 * PAD;
        if (hclip < 0) hclip = 0;
        gfx_text_clip(cv, hx, hy, file, e->focused ? TH_TEXT : TH_TEXT_DIM,
                      hx, hclip);
        int fx = hx + gfx_textw(file) + GFX_FW;
        if (e->dirty)
            gfx_text_clip(cv, fx, hy, "[+]", TH_ORANGE, hx, hclip);

        /* right-aligned Ln, Col */
        char lc[40]; int p = 0;
        char nb[16]; int nn;
        const char* lbl = "Ln ";
        for (int i = 0; lbl[i]; i++) lc[p++] = lbl[i];
        nn = ide_itoa(e->caret_line + 1, nb); for (int i = 0; i < nn; i++) lc[p++] = nb[i];
        lc[p++] = ','; lc[p++] = ' ';
        lc[p++] = 'C'; lc[p++] = 'o'; lc[p++] = 'l'; lc[p++] = ' ';
        nn = ide_itoa(e->caret_col + 1, nb); for (int i = 0; i < nn; i++) lc[p++] = nb[i];
        lc[p] = 0;
        int lcw = gfx_textw(lc);
        int lcx = r.x + r.w - PAD - lcw;
        if (lcx > hx) gfx_text_clip(cv, lcx, hy, lc, TH_TEXT_DIM, hx, hclip);
    }

    Rect body = ed_body(r);
    if (body.h <= 0) return;

    int line_h = GFX_FH;
    int gutter_w = ed_gutter_w();
    int code_x = body.x + gutter_w + PAD;
    int code_clip_w = body.x + body.w - PAD - ED_SBAR_W - code_x;
    if (code_clip_w < 0) code_clip_w = 0;

    /* keep caret visible before drawing */
    ed_clamp_caret(a);
    ed_scroll_to_caret(a, body);

    /* gutter background + divider */
    gfx_fill(cv, body.x, body.y, gutter_w, body.h, TH_PANEL);
    gfx_vline(cv, body.x + gutter_w, body.y, body.h, TH_BORDER);

    int total_lines = ed_line_count(a->src, a->src_len);
    int vis = (body.h + line_h - 1) / line_h;

    /* walk visible lines */
    int top = e->top_line;
    if (top < 0) top = 0;
    int pos = ed_line_start(a->src, a->src_len, top);
    int slen = a->src_len;
    const char* src = a->src;

    for (int row = 0; row < vis; row++) {
        int ln = top + row;
        if (ln >= total_lines) break;
        int ry = body.y + row * line_h;
        if (ry >= body.y + body.h) break;

        int lstart = pos;
        int lend = lstart;
        while (lend < slen && src[lend] != '\n') lend++;
        int llen = lend - lstart;
        if (llen < 0) llen = 0;

        int is_caret_line = (ln == e->caret_line);

        /* current-line highlight across the whole row */
        if (is_caret_line && e->focused)
            gfx_blend(cv, body.x, ry, body.w - ED_SBAR_W, line_h,
                      (0x40u << 24) | (TH_SELECT & 0x00FFFFFFu));

        /* line number */
        ed_itoa_pad(ln + 1, ED_GUTTER_CH, numbuf, (int)sizeof(numbuf));
        gfx_text_clip(cv, body.x + PAD, ry, numbuf,
                      is_caret_line ? TH_TEXT_DIM : TH_TEXT_FAINT,
                      body.x + PAD, ED_GUTTER_CH * GFX_FW);

        /* classify + draw, honoring horizontal scroll (left_col) */
        int n = llen; if (n > ED_LINECAP) n = ED_LINECAP;
        if (n > 0) {
            lex_classify_line(src + lstart, n, cls);
            int code_right = code_x + code_clip_w;
            for (int i = e->left_col; i < n; i++) {
                int vis_col = i - e->left_col;
                int cx = code_x + vis_col * GFX_FW;
                if (cx >= code_right) break;
                char cch = src[lstart + i];
                if (cch == '\t') cch = ' ';   /* render tab as space cell */
                if (cch >= 32 && cch < 127)
                    gfx_text_clip(cv, cx, ry, src + lstart + i,
                                  ed_class_color(cls[i]), code_x, code_clip_w);
            }
        }

        /* caret on this line */
        if (is_caret_line && e->focused && e->blink_ms < ED_BLINK_MS) {
            int vis_col = e->caret_col - e->left_col;
            if (vis_col >= 0) {
                int cx = code_x + vis_col * GFX_FW;
                if (cx < code_x + code_clip_w)
                    gfx_fill(cv, cx, ry, 2, line_h, TH_BLUE);
            }
        }

        if (lend < slen) pos = lend + 1; else pos = slen;
    }

    /* vertical scrollbar */
    if (total_lines > 0) {
        int track_x = body.x + body.w - ED_SBAR_W;
        int track_h = body.h;
        int vrows = body.h / line_h; if (vrows < 1) vrows = 1;
        if (total_lines > vrows) {
            int thumb_h = (vrows * track_h) / total_lines;
            int max_off = total_lines - vrows;
            if (thumb_h < 8) thumb_h = 8;
            if (thumb_h > track_h) thumb_h = track_h;
            int s = top; if (s > max_off) s = max_off;
            int thumb_y = body.y + (max_off > 0
                          ? (s * (track_h - thumb_h)) / max_off : 0);
            if (thumb_y < body.y) thumb_y = body.y;
            if (thumb_y + thumb_h > body.y + track_h)
                thumb_y = body.y + track_h - thumb_h;
            gfx_blend(cv, track_x, body.y, ED_SBAR_W, track_h, 0x30000000u);
            gfx_fill(cv, track_x, thumb_y, ED_SBAR_W, thumb_h, TH_BORDER_LT);
        }
    }
    (void)ed_min; (void)ed_max;
}
