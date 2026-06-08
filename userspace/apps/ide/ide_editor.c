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
#include "ide_complete.h"              /* symbol/snippet completion engine */
#include "ide_library.h"              /* category tags for the popup chips */
#include "ide_build.h"                 /* ide_build_line_severity for error highlighting */
#include "../../lib/keymap/keymap.h"   /* shared US-QWERTY: caps-lock + shift + symbols */

/* ---- tunables (mirror ide_codeview.c where it matters) ---- */
#define ED_HEADER_H   (ROW_H + 2)
/* Auto-size gutter: 3 digits for <1000 lines, 4 for <10000, 5 otherwise.
 * Adds 1 char of leading padding for the selection indicator. */
static inline int ed_gutter_ch(int total_lines) {
    if (total_lines < 100)   return 3;
    if (total_lines < 1000)  return 3;
    if (total_lines < 10000) return 4;
    return 5;
}
#define ED_GUTTER_CH  5   /* max width for ed_itoa_pad fallback */
#define ED_LINECAP    1024
/* Tab width + blink period are now Settings knobs (ide_gfx.c runtime vars);
 * the macros alias the live, clamped values so all call sites stay unchanged. */
#define ED_TABW       (g_tab_width < 1 ? 1 : g_tab_width)
#define ED_SBAR_W     (GFX_FW / 2 < 4 ? 4 : GFX_FW / 2)  /* scale with font */
#define ED_BLINK_MS   (g_blink_ms < 1 ? 1 : g_blink_ms)  /* caret on/off half-period */

/* ---- tiny local helpers ---- */
static inline int ed_min(int a, int b) { return a < b ? a : b; }
static inline int ed_max(int a, int b) { return a > b ? a : b; }

/* ---- forward declarations (used by autocomplete + multi-cursor before defn) ---- */
static int  ed_line_start(const char* src, int len, int ln);
static int  ed_line_len(const char* src, int len, int ln);
static int  ed_caret_off(struct Ide* a);
static int  ed_insert_byte(struct Ide* a, int off, char ch);
static void ed_delete_byte(struct Ide* a, int off);
static void ed_caret_from_off(struct Ide* a, int off);
static void ed_clamp_caret(struct Ide* a);
static void undo_begin_step(void);   /* start a fresh undo step (structural ops) */
static void undo_clear(void);        /* drop all undo/redo history               */

/* ===========================================================================
 * Autocomplete dictionary + helpers.
 *
 * A static table of C keywords, type names, preprocessor directives, and
 * common stdlib identifiers. When the user types 2+ identifier characters,
 * we filter this list by prefix and show up to AC_MAX_MATCHES in a popup
 * below the caret. Arrow keys navigate, Tab/Enter accepts, Escape dismisses.
 * ==========================================================================*/
/* The candidate engine (symbols + keywords + library snippets) now lives in
 * ide_complete.c (complete_refresh / complete_accept). ac_extract_prefix below
 * still extracts the typed word; ed_ac_refresh() is the editor-side wrapper that
 * gates completion out of strings/comments, then calls complete_refresh(). */

/* Extract the word being typed at the caret (backwards from caret_col on the
 * current line). Populates ac_prefix and ac_prefix_len. */
static void ac_extract_prefix(struct Ide* a) {
    Editor* e = &a->editor;
    int ls = ed_line_start(a->src, a->src_len, e->caret_line);
    int col = e->caret_col;
    /* Walk backwards from the caret to find the start of the identifier.
     * Also accept '#' as the first character for preprocessor directives. */
    int start = col;
    while (start > 0) {
        char c = a->src[ls + start - 1];
        int is_ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
        if (is_ident) { start--; continue; }
        /* Allow '#' only at the very beginning of the word */
        if (c == '#' && start == col - 1) { start--; continue; }
        if (c == '#' && start > 0) { /* '#' mid-word only if it leads the word */
            int all_ident = 1;
            for (int k = start; k < col; k++) {
                char d = a->src[ls + k];
                int ok = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                         (d >= '0' && d <= '9') || d == '_';
                if (!ok) { all_ident = 0; break; }
            }
            if (all_ident) { start--; }   /* include the '#'; never underflow */
            break;
        }
        break;
    }
    if (start < 0) start = 0;
    int plen = col - start;
    if (plen < 0) plen = 0;
    if (plen >= AC_PREFIX_CAP) plen = AC_PREFIX_CAP - 1;
    for (int i = 0; i < plen; i++)
        e->ac_prefix[i] = a->src[ls + start + i];
    e->ac_prefix[plen] = 0;
    e->ac_prefix_len = plen;
}

/* Is the caret inside a string or comment on its line? (Suppress completion.) */
static int ed_in_string_or_comment(struct Ide* a) {
    Editor* e = &a->editor;
    int ls = ed_line_start(a->src, a->src_len, e->caret_line);
    int ll = ed_line_len(a->src, a->src_len, e->caret_line);
    if (ll <= 0) return 0;
    if (ll > ED_LINECAP) ll = ED_LINECAP;
    static unsigned char cls[ED_LINECAP];
    lex_classify_line(a->src + ls, ll, cls);
    int col = e->caret_col;
    if (col > ll) col = ll;
    if (col <= 0) return 0;
    unsigned char c = cls[col - 1];
    return (c == LEXCLS_STRING || c == LEXCLS_COMMENT);
}

/* Editor-side autocomplete refresh: extract the typed prefix, suppress inside
 * strings/comments, otherwise rebuild the candidate list (ide_complete.c). */
static void ed_ac_refresh(struct Ide* a) {
    a->editor.ac_navigated = 0;   /* typing re-opens/narrows: re-arm Enter=newline */
    if (!g_autocomplete) { a->editor.ac_active = 0; a->editor.ac_count = 0; return; }
    ac_extract_prefix(a);
    if (ed_in_string_or_comment(a)) { a->editor.ac_active = 0; a->editor.ac_count = 0; return; }
    complete_refresh(a);
}

/* While a snippet's tab-stops are live, keep their byte offsets correct as the
 * user edits a field: shift every stop at/after `at` by `delta` (+1 insert,
 * -1 delete). Lets you type into one field and Tab to the next. */
static void ed_snippet_shift(Editor* e, int at, int delta) {
    if (!e->snippet_active) return;
    for (int k = 0; k < e->ts_count; k++)
        if (e->ts_off[k] >= at) e->ts_off[k] += delta;
}

/* Accent color for an autocomplete candidate's category chip. */
static unsigned int ac_kind_color(int kind) {
    switch (kind) {
    case CK_PARAM:   return TH_BLUE;
    case CK_FUNC:    return TH_YELLOW;
    case CK_GLOBAL:  return TH_GREEN;
    case CK_TYPE:    return TH_CYAN;
    case CK_MACRO:   return TH_PURPLE;
    case CK_SNIPPET: return TH_ORANGE;
    default:         return TH_TEXT_FAINT;
    }
}

/* Insert a snippet body at the caret, expanding ${N:..}/$N/$0 tab-stops, then
 * enter tab-stop navigation (Tab cycles them; see EDK_TAB). Continuation lines
 * are auto-indented to the caret's starting column. */
void ide_editor_insert_snippet(struct Ide* a, const char* body) {
    Editor* e = &a->editor;
    undo_begin_step();                /* snippet expansion = one undo step */
    int off = ed_caret_off(a);
    int base_col = e->caret_col;
    int ord[ED_TS_MAX], offs[ED_TS_MAX]; int nts = 0;
    int i = 0;
    while (body[i]) {
        char c = body[i];
        if (c == '$') {
            char n2 = body[i + 1];
            int num = -1, consumed = 0, placeholder = 0, pstart = 0, pend = 0;
            if (n2 == '{') {
                int j = i + 2, have = 0; num = 0;
                while (body[j] >= '0' && body[j] <= '9') { num = num * 10 + (body[j] - '0'); j++; have = 1; }
                if (have && body[j] == ':') { j++; placeholder = 1; pstart = j;
                    while (body[j] && body[j] != '}') j++; pend = j;
                    if (body[j] == '}') j++; consumed = j - i; }
                else if (have && body[j] == '}') { j++; consumed = j - i; }
                else num = -1;
            } else if (n2 >= '0' && n2 <= '9') { num = n2 - '0'; consumed = 2; }
            if (num >= 0) {
                int dup = 0;
                for (int k = 0; k < nts; k++) if (ord[k] == num) { dup = 1; break; }
                if (!dup && nts < ED_TS_MAX) { ord[nts] = num; offs[nts] = off; nts++; }
                if (placeholder)
                    for (int p = pstart; p < pend; p++) { if (ed_insert_byte(a, off, body[p])) off++; }
                i += consumed; continue;
            }
            if (ed_insert_byte(a, off, '$')) off++;
            i++; continue;
        }
        if (c == '\n') {
            if (ed_insert_byte(a, off, '\n')) off++;
            for (int s = 0; s < base_col; s++) { if (ed_insert_byte(a, off, ' ')) off++; }
            i++; continue;
        }
        if (ed_insert_byte(a, off, c)) off++;
        i++;
    }
    /* order tab-stops ascending, with $0 (order 0) placed last. */
    for (int p = 0; p < nts; p++)
        for (int q = p + 1; q < nts; q++) {
            int op = ord[p] == 0 ? 1000000 : ord[p];
            int oq = ord[q] == 0 ? 1000000 : ord[q];
            if (oq < op) { int t = ord[p]; ord[p]=ord[q]; ord[q]=t; t=offs[p]; offs[p]=offs[q]; offs[q]=t; }
        }
    for (int p = 0; p < nts; p++) e->ts_off[p] = offs[p];
    e->ts_count = nts;
    if (nts > 0) { e->snippet_active = 1; e->ts_current = 0; ed_caret_from_off(a, e->ts_off[0]); }
    else { e->snippet_active = 0; ed_caret_from_off(a, off); }
    e->want_col = e->caret_col;
    e->dirty = 1;
    e->ac_active = 0; e->ac_count = 0;

    /* LIVE blueprint sync: re-parse so the inserted complex registers as
     * Semantic-Lego-Map nodes immediately and auto-wires to siblings that share
     * its globals/calls (mirrors the Ctrl+S save path). */
    model_parse(&a->model, a->src, a->src_len, a->cur_file);
    if (a->model.nfuncs > 0) {
        if (a->focus_func >= a->model.nfuncs) a->focus_func = 0;
        a->model.focus = a->focus_func;
    } else {
        a->model.focus = -1;
    }
    model_analyze(&a->model);
}

/* Apply a completion at the caret: a plain symbol suffix, or (is_snippet) the
 * deletion of the typed trigger followed by snippet expansion. */
void ide_editor_apply_completion(struct Ide* a, const char* text, int is_snippet, int prefix_len) {
    Editor* e = &a->editor;
    undo_begin_step();                 /* accept = one atomic undo step */
    if (is_snippet) {
        int off = ed_caret_off(a);
        for (int i = 0; i < prefix_len && off > 0; i++) { ed_delete_byte(a, off - 1); off--; }
        ed_caret_from_off(a, off);
        ide_editor_insert_snippet(a, text);
    } else {
        /* Delete the typed prefix, then insert the FULL candidate, so the result
         * carries the candidate's casing (typing "PR" + accept "printf" -> the
         * old code appended "intf" giving "PRintf"; now it yields "printf"). */
        int off = ed_caret_off(a);
        for (int i = 0; i < prefix_len && off > 0; i++) { ed_delete_byte(a, off - 1); off--; }
        ed_caret_from_off(a, off);
        for (int i = 0; text[i]; i++) { if (!ed_insert_byte(a, off, text[i])) break; off++; }
        ed_caret_from_off(a, off);
        e->want_col = e->caret_col;
        e->ac_active = 0; e->ac_count = 0;
    }
}

static inline int ed_gutter_w_n(int total_lines) { return ed_gutter_ch(total_lines) * GFX_FW + 2 * PAD; }
static inline int ed_gutter_w(void) { return ED_GUTTER_CH * GFX_FW + 2 * PAD; } /* legacy fallback */

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

/* ===========================================================================
 * Undo / redo -- a bounded ring of single-byte ops grouped into "steps".
 *
 * EVERY buffer mutation funnels through ed_insert_byte/ed_delete_byte, so we
 * record there. A burst of typing coalesces into one step (same kind +
 * contiguous + not a word boundary); structural ops (selection-delete, paste,
 * snippet insert, dup/delete-line, replace-all) call undo_begin_step() so each
 * is its own atomic step. undo/redo replay a whole group through the same
 * primitives with undo_suspend set (so the replay isn't itself recorded).
 * Storage: 512 ops * 8 bytes = ~4KB static .bss; no malloc.
 * ==========================================================================*/
#define UNDO_OPS  512
typedef struct { unsigned int off; unsigned char kind; unsigned char grp; char ch; } UndoOp; /* kind:0=insert 1=delete */
static UndoOp        undo_ops[UNDO_OPS];
static int           undo_n;        /* live (undoable) op count                */
static int           undo_redo;     /* redoable ops sitting at [undo_n ..)      */
static unsigned char undo_grp;      /* current group id                        */
static int           undo_suspend;  /* >0 while applying undo/redo             */
static int           undo_lastkind = -1;  /* for coalescing (-1 = none)        */
static int           undo_lastoff;

static int undo_is_break(char c) {
    return c == ' ' || c == '\n' || c == '\t' ||
           (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}
static void undo_push(unsigned int off, unsigned char kind, char ch, int newgrp) {
    if (undo_suspend) return;
    undo_redo = 0;                       /* any new edit invalidates redo */
    if (newgrp) undo_grp++;
    if (undo_n >= UNDO_OPS) {             /* drop oldest op */
        for (int i = 1; i < UNDO_OPS; i++) undo_ops[i - 1] = undo_ops[i];
        undo_n--;
    }
    undo_ops[undo_n].off = off; undo_ops[undo_n].kind = kind;
    undo_ops[undo_n].ch = ch;  undo_ops[undo_n].grp = undo_grp;
    undo_n++;
    undo_lastkind = kind; undo_lastoff = (int)off;
}
static void undo_record_insert(unsigned int off, char ch) {
    int newgrp = (undo_lastkind != 0) || ((int)off != undo_lastoff + 1) || undo_is_break(ch);
    undo_push(off, 0, ch, newgrp);
}
static void undo_record_delete(unsigned int off, char ch) {
    int newgrp = (undo_lastkind != 1) ||
                 ((int)off != undo_lastoff && (int)off != undo_lastoff - 1);
    undo_push(off, 1, ch, newgrp);
}
/* Force the next recorded op to begin a fresh undo step. */
static void undo_begin_step(void) { undo_grp++; undo_lastkind = -1; }
/* Clear all history (called on file open / reset). */
static void undo_clear(void) { undo_n = 0; undo_redo = 0; undo_grp = 0; undo_lastkind = -1; }

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
    undo_record_insert((unsigned int)off, ch);
    return 1;
}

/* Delete one byte at offset `off`, shifting the tail down. */
static void ed_delete_byte(struct Ide* a, int off) {
    if (off < 0 || off >= a->src_len) return;
    undo_record_delete((unsigned int)off, a->src[off]);
    for (int i = off; i < a->src_len - 1; i++) a->src[i] = a->src[i + 1];
    a->src_len--;
    a->editor.dirty = 1;
}

/* ===========================================================================
 * Selection + clipboard.
 *   - Shift + a caret move EXTENDS the selection; any unshifted move drops it.
 *   - Ctrl+A select-all, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste.
 *   - Typing / Backspace / Delete / Enter / Tab over a selection REPLACES it.
 * sel_anchor_off is the FIXED end (a byte offset); the caret is the moving end.
 * A selection is active when sel_anchor_off >= 0 and differs from the caret.
 * The clipboard is app-local (no system clipboard exists yet).
 * ==========================================================================*/
#define ED_CLIP_CAP  8192
static char ed_clip[ED_CLIP_CAP];
static int  ed_clip_len = 0;

/* Set caret (line,col) from a byte offset. */
static void ed_caret_from_off(struct Ide* a, int off) {
    Editor* e = &a->editor;
    if (off < 0) off = 0;
    if (off > a->src_len) off = a->src_len;
    int line = 0, col = 0;
    for (int i = 0; i < off; i++) {
        if (a->src[i] == '\n') { line++; col = 0; }
        else col++;
    }
    e->caret_line = line; e->caret_col = col; e->want_col = col;
}

/* Ordered active-selection range [lo,hi) in byte offsets. Returns 1 if a
 * non-empty selection exists, else clears the anchor and returns 0. */
static int ed_sel_range(struct Ide* a, int* lo, int* hi) {
    Editor* e = &a->editor;
    if (e->sel_anchor_off < 0) return 0;
    int anc = e->sel_anchor_off;
    if (anc < 0) anc = 0; if (anc > a->src_len) anc = a->src_len;
    int car = ed_caret_off(a);
    int l = anc < car ? anc : car;
    int h = anc < car ? car : anc;
    if (l < 0) l = 0; if (h > a->src_len) h = a->src_len;
    if (h <= l) return 0;
    *lo = l; *hi = h;
    return 1;
}

/* Before a caret MOVE: Shift anchors the selection (once) at the current caret;
 * no Shift drops any selection. Call BEFORE performing the move. */
static void ed_sel_track(struct Ide* a, int shift) {
    Editor* e = &a->editor;
    if (shift) { if (e->sel_anchor_off < 0) e->sel_anchor_off = ed_caret_off(a); }
    else        e->sel_anchor_off = -1;
}

/* Delete the active selection (if any); caret ends at its start, anchor cleared.
 * Returns 1 if something was deleted. */
static int ed_delete_sel(struct Ide* a) {
    int lo, hi;
    if (!ed_sel_range(a, &lo, &hi)) { a->editor.sel_anchor_off = -1; return 0; }
    int n = hi - lo;
    undo_begin_step();                /* selection delete = one undo step */
    for (int i = 0; i < n; i++) ed_delete_byte(a, lo);   /* remove at lo n times */
    ed_caret_from_off(a, lo);
    a->editor.sel_anchor_off = -1;
    return 1;
}

/* Copy the active selection (or, with no selection, the whole current line incl.
 * its trailing newline) into the app-local clipboard. */
static void ed_copy(struct Ide* a) {
    int lo, hi;
    if (!ed_sel_range(a, &lo, &hi)) {
        lo = ed_line_start(a->src, a->src_len, a->editor.caret_line);
        hi = lo + ed_line_len(a->src, a->src_len, a->editor.caret_line);
        if (hi < a->src_len && a->src[hi] == '\n') hi++;   /* whole-line copy */
    }
    int n = hi - lo;
    if (n > ED_CLIP_CAP) n = ED_CLIP_CAP;
    for (int i = 0; i < n; i++) ed_clip[i] = a->src[lo + i];
    ed_clip_len = n;
}

/* Insert the clipboard at the caret, replacing any active selection. */
static void ed_paste(struct Ide* a) {
    undo_begin_step();                /* paste = its own undo step(s) */
    ed_delete_sel(a);
    int off = ed_caret_off(a);
    int i;
    for (i = 0; i < ed_clip_len; i++)
        if (!ed_insert_byte(a, off + i, ed_clip[i])) break;
    ed_caret_from_off(a, off + i);
}

/* Undo the newest step (all ops sharing the newest group id), replaying each
 * inverted through the primitives with recording suspended. */
void ide_editor_undo(struct Ide* a) {
    Editor* e = &a->editor;
    if (undo_n == 0) return;
    e->snippet_active = 0; e->mc_count = 0;   /* stale offsets after a replay */
    unsigned char g = undo_ops[undo_n - 1].grp;
    int caret_off = -1;
    undo_suspend++;
    while (undo_n > 0 && undo_ops[undo_n - 1].grp == g) {
        UndoOp* op = &undo_ops[undo_n - 1];
        if (op->kind == 0) { ed_delete_byte(a, (int)op->off); caret_off = (int)op->off; }
        else               { ed_insert_byte(a, (int)op->off, op->ch); caret_off = (int)op->off; }
        undo_n--; undo_redo++;
    }
    undo_suspend--;
    if (caret_off >= 0) ed_caret_from_off(a, caret_off);
    e->sel_anchor_off = -1; ed_clamp_caret(a); e->dirty = 1; undo_lastkind = -1;
}

/* Redo the next step (the group sitting just past the undo frontier). */
void ide_editor_redo(struct Ide* a) {
    Editor* e = &a->editor;
    if (undo_redo == 0) return;
    e->snippet_active = 0; e->mc_count = 0;
    unsigned char g = undo_ops[undo_n].grp;
    int caret_off = -1;
    undo_suspend++;
    while (undo_redo > 0 && undo_ops[undo_n].grp == g) {
        UndoOp* op = &undo_ops[undo_n];
        if (op->kind == 0) { ed_insert_byte(a, (int)op->off, op->ch); caret_off = (int)op->off + 1; }
        else               { ed_delete_byte(a, (int)op->off); caret_off = (int)op->off; }
        undo_n++; undo_redo--;
    }
    undo_suspend--;
    if (caret_off >= 0) ed_caret_from_off(a, caret_off);
    e->sel_anchor_off = -1; ed_clamp_caret(a); e->dirty = 1; undo_lastkind = -1;
}

/* ---- Public accessors for the LEGO code view (ide_codeview.c) ----
 * These let the read-only-turned-editable code panel share the single
 * a->editor caret model without duplicating the line-walk logic. */
int ide_editor_caret_off(struct Ide* a) { return ed_caret_off(a); }
int ide_editor_sel_range(struct Ide* a, int* lo, int* hi) { return ed_sel_range(a, lo, hi); }
int ide_editor_line_start(struct Ide* a, int ln) {
    return ed_line_start(a->src, a->src_len, ln);
}
int ide_editor_line_len(struct Ide* a, int ln) {
    return ed_line_len(a->src, a->src_len, ln);
}

/* case-sensitive substring search forward in a->src from byte offset `from`;
 * returns the match start offset or -1. */
static int ed_src_find(struct Ide* a, const char* needle, int from) {
    int nl = 0; while (needle[nl]) nl++;
    if (nl == 0) return -1;
    if (from < 0) from = 0;
    for (int i = from; i + nl <= a->src_len; i++) {
        int k = 0;
        while (k < nl && a->src[i + k] == needle[k]) k++;
        if (k == nl) return i;
    }
    return -1;
}

/* Find `needle` and SELECT it (the selection highlight shows the match), scrolling
 * it into view. The search base is the current match start (sel anchor) if a match
 * is selected, else the caret. `advance`=0 refines IN PLACE (incremental typing);
 * `advance`=1 skips the current match (Find Next). Wraps to the top. Returns 1/0. */
int ide_editor_find(struct Ide* a, const char* needle, int advance) {
    int nl = 0; while (needle[nl]) nl++;
    if (nl == 0) return 0;
    int base = (a->editor.sel_anchor_off >= 0) ? a->editor.sel_anchor_off : ed_caret_off(a);
    int m = ed_src_find(a, needle, base + (advance ? 1 : 0));
    if (m < 0) m = ed_src_find(a, needle, 0);   /* wrap to top */
    if (m < 0) return 0;
    a->editor.sel_anchor_off = m;       /* highlight the match via selection */
    ed_caret_from_off(a, m + nl);
    return 1;
}

/* Replace ALL occurrences of `needle` with `repl` in a->src. Returns the count
 * replaced. Bounded; clears any selection and parks the caret at the top. */
int ide_editor_replace_all(struct Ide* a, const char* needle, const char* repl) {
    int nl = 0; while (needle[nl]) nl++;
    if (nl == 0) return 0;
    int rl = 0; while (repl[rl]) rl++;
    undo_begin_step();                /* replace-all = one undo step */
    int count = 0, from = 0;
    while (count < 100000) {
        int m = ed_src_find(a, needle, from);
        if (m < 0) break;
        /* Stop cleanly if this replacement would not FIT (buffer full): otherwise
         * a partial insert leaves the needle in place and `from` never advances,
         * spinning the loop to its 100000 cap (a multi-second freeze). Checking
         * the net growth (rl-nl) up front guarantees every delete+insert below
         * fully succeeds, so `from = m + rl` always makes forward progress. */
        if (rl > nl && a->src_len + (rl - nl) > IDE_SRC_CAP) break;
        for (int i = 0; i < nl; i++) ed_delete_byte(a, m);          /* remove needle */
        for (int i = 0; i < rl; i++) ed_insert_byte(a, m + i, repl[i]);  /* room guaranteed */
        from = m + rl;
        count++;
    }
    a->editor.sel_anchor_off = -1;
    ed_caret_from_off(a, 0);
    ed_clamp_caret(a);
    return count;
}

/* ---- public clipboard / selection ops (Ctrl+A/C/X/V via ide.c chord handler) ---- */
void ide_editor_select_all(struct Ide* a) {
    a->editor.sel_anchor_off = 0;
    ed_caret_from_off(a, a->src_len);
}
void ide_editor_copy(struct Ide* a) { ed_copy(a); }
void ide_editor_cut(struct Ide* a) {
    ed_copy(a);
    if (!ed_delete_sel(a)) ide_editor_delete_line(a);
    ed_clamp_caret(a);
}
void ide_editor_paste(struct Ide* a) {
    ed_paste(a);
    ed_clamp_caret(a);
}

/* ---- public reset ---- */
void ide_editor_reset(struct Ide* a) {
    Editor* e = &a->editor;
    undo_clear();                     /* fresh file -> fresh undo history */
    e->caret_line = 0;
    e->caret_col = 0;
    e->want_col = 0;
    e->top_line = 0;
    e->left_col = 0;
    e->dirty = 0;
    e->blink_ms = 0;
    e->sel_anchor_off = -1;
    e->ac_active = 0;
    e->ac_sel = 0;
    e->ac_prefix_len = 0;
    e->ac_count = 0;
    e->mc_count = 0;              /* clear multi-cursors on file open */
    /* word_wrap and minimap are preserved across file opens (user prefs) */
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

/* ---- Ctrl+W: toggle word wrap ---- */
void ide_editor_toggle_wrap(struct Ide* a) {
    a->editor.word_wrap = !a->editor.word_wrap;
    /* Reset horizontal scroll when entering wrap mode (no horiz. scroll needed). */
    if (a->editor.word_wrap) a->editor.left_col = 0;
}

/* ---- Ctrl+M: toggle minimap ---- */
void ide_editor_toggle_minimap(struct Ide* a) {
    a->editor.minimap = !a->editor.minimap;
}

/* ---- Ctrl+D multi-cursor: find next occurrence of selection ---- */
void ide_editor_multi_cursor_add(struct Ide* a) {
    Editor* e = &a->editor;
    int slo, shi;
    if (!ed_sel_range(a, &slo, &shi)) {
        /* No selection: fall back to duplicate-line. */
        ide_editor_duplicate_line(a);
        return;
    }
    if (e->mc_count >= ED_MULTI_CURSOR_MAX) return;

    int sel_len = shi - slo;
    if (sel_len <= 0 || sel_len > 128) return;

    /* Search for the next occurrence after the last cursor position. */
    int search_from = shi;
    if (e->mc_count > 0) {
        /* Start after the last extra cursor. */
        int last = ed_line_start(a->src, a->src_len, e->mc_line[e->mc_count - 1])
                   + e->mc_col[e->mc_count - 1];
        if (last > search_from) search_from = last;
    }

    /* Linear search for the pattern (the selected text). */
    int found = -1;
    for (int i = search_from; i + sel_len <= a->src_len; i++) {
        int match = 1;
        for (int j = 0; j < sel_len; j++) {
            if (a->src[i + j] != a->src[slo + j]) { match = 0; break; }
        }
        if (match) { found = i; break; }
    }
    /* Wrap around. */
    if (found < 0) {
        for (int i = 0; i + sel_len <= search_from && i + sel_len <= a->src_len; i++) {
            int match = 1;
            for (int j = 0; j < sel_len; j++) {
                if (a->src[i + j] != a->src[slo + j]) { match = 0; break; }
            }
            if (match) { found = i; break; }
        }
    }
    if (found < 0) return;

    /* Convert found offset to (line, col). */
    int line = 0, col = 0;
    for (int i = 0; i < found; i++) {
        if (a->src[i] == '\n') { line++; col = 0; }
        else col++;
    }
    e->mc_line[e->mc_count] = line;
    e->mc_col[e->mc_count] = col;
    e->mc_count++;
}

/* ---- Ctrl+D: duplicate current line ---- */
void ide_editor_duplicate_line(struct Ide* a) {
    Editor* e = &a->editor;
    undo_begin_step();                /* duplicate-line = one undo step */
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
    undo_begin_step();                /* delete-line = one undo step */
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

    int total_lines = ed_line_count(a->src, a->src_len);
    int gutter_w = ed_gutter_w_n(total_lines);
    int minimap_w = e->minimap ? 60 : 0;   /* must match ED_MINIMAP_W in render */
    int code_w = body.w - gutter_w - 2 * PAD - ED_SBAR_W - minimap_w;
    if (code_w < GFX_FW) code_w = GFX_FW;
    int vis_cols = code_w / GFX_FW;
    if (vis_cols < 1) vis_cols = 1;

    if (e->caret_line < e->top_line) e->top_line = e->caret_line;
    if (e->caret_line >= e->top_line + vis_rows)
        e->top_line = e->caret_line - vis_rows + 1;
    if (e->top_line < 0) e->top_line = 0;

    /* Word wrap mode: no horizontal scrolling (lines wrap at the edge). */
    if (e->word_wrap) {
        e->left_col = 0;
    } else {
        if (e->caret_col < e->left_col) e->left_col = e->caret_col;
        if (e->caret_col >= e->left_col + vis_cols)
            e->left_col = e->caret_col - vis_cols + 1;
        if (e->left_col < 0) e->left_col = 0;
    }
}

/* ---- key handling ---- */
/* evdev keycodes we react to (matches kernel/include/input.h) */
#define EDK_BACKSPACE 14
#define EDK_TAB       15
#define EDK_ENTER     28
#define EDK_S         31
#define EDK_A         30      /* select-all */
#define EDK_X         45      /* cut        */
#define EDK_C         46      /* copy       */
#define EDK_V         47      /* paste      */
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

    /* Ctrl chords are routed by the IDE's central handler (handle_ctrl_chord in
     * ide.c) -- this entry point is called with ctrl already stripped (ctrl==0),
     * so Ctrl+S / Ctrl+A/C/X/V are handled THERE (via the ide_editor_* ops), not
     * here. We keep the guards defensively in case a caller ever forwards ctrl. */
    if (ctrl) { e->ac_active = 0; /* dismiss autocomplete on any Ctrl chord */
        if (keycode == EDK_S) { ide_editor_save(a); return 1; }
        return 0;
    }

    /* Any key other than Ctrl+D (which is handled in ide.c) clears multi-cursors.
     * The extra cursors are a transient visual state; typing collapses back to a
     * single primary cursor. */
    e->mc_count = 0;

    /* --- Autocomplete popup intercepts (when the popup is active) ---
     * Up/Down navigate the popup (and "engage" it); Tab always accepts the
     * highlighted item; Esc dismisses. Enter accepts ONLY if the user has
     * navigated the popup with the arrows -- otherwise Enter falls through to
     * insert a real newline (which also dismisses the popup). This is the fix
     * for the #1 "autocomplete is bugged" complaint: a bare Enter while the
     * popup auto-showed must make a newline, not inject a suggestion.
     * All other keys fall through to normal editing + a prefix re-scan. */
    if (e->ac_active) {
        if (keycode == EDK_UP) {
            if (e->ac_sel > 0) e->ac_sel--;
            else e->ac_sel = e->ac_count - 1;
            e->ac_navigated = 1;
            return 1;
        }
        if (keycode == EDK_DOWN) {
            if (e->ac_sel < e->ac_count - 1) e->ac_sel++;
            else e->ac_sel = 0;
            e->ac_navigated = 1;
            return 1;
        }
        if (keycode == EDK_TAB) {
            complete_accept(a);
            return 1;
        }
        if (keycode == EDK_ENTER && e->ac_navigated) {
            complete_accept(a);
            return 1;
        }
        if (keycode == 1 /* KEY_ESC */) {
            e->ac_active = 0;
            return 1;
        }
        /* Other keys (Enter-without-nav, printable, backspace, Left/Right):
         * fall through to normal editing; the prefix is re-scanned afterward. */
    }

    ed_clamp_caret(a);

    /* Selection: Shift + a caret move extends; any unshifted move clears. Runs
     * BEFORE the move so the anchor captures the pre-move caret offset. */
    switch (keycode) {
    case EDK_LEFT: case EDK_RIGHT: case EDK_UP: case EDK_DOWN:
    case EDK_HOME: case EDK_END: case EDK_PAGEUP: case EDK_PAGEDOWN:
        ed_sel_track(a, shift);
        break;
    default:
        /* a non-navigation key with no active selection edit: drop stale anchor
         * only for keys that are neither edits nor moves is handled below. */
        break;
    }

    switch (keycode) {
    case EDK_LEFT: {
        e->ac_active = 0;   /* dismiss autocomplete on cursor move */
        if (e->caret_col > 0) e->caret_col--;
        else if (e->caret_line > 0) {
            e->caret_line--;
            e->caret_col = ed_line_len(a->src, a->src_len, e->caret_line);
        }
        e->want_col = e->caret_col;
        return 1;
    }
    case EDK_RIGHT: {
        e->ac_active = 0;
        int ll = ed_line_len(a->src, a->src_len, e->caret_line);
        if (e->caret_col < ll) e->caret_col++;
        else if (e->caret_line < ed_line_count(a->src, a->src_len) - 1) {
            e->caret_line++; e->caret_col = 0;
        }
        e->want_col = e->caret_col;
        return 1;
    }
    case EDK_UP:
        e->ac_active = 0;
        if (e->caret_line > 0) {
            e->caret_line--;
            e->caret_col = e->want_col;
            ed_clamp_caret(a);
        }
        return 1;
    case EDK_DOWN:
        e->ac_active = 0;
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
        if (ed_delete_sel(a)) { e->want_col = e->caret_col; ed_ac_refresh(a); return 1; }
        int off = ed_caret_off(a);
        if (off > 0) {
            int prev_is_nl = (a->src[off - 1] == '\n');
            if (prev_is_nl) {
                int pl = e->caret_line - 1;
                int pll = ed_line_len(a->src, a->src_len, pl);
                ed_delete_byte(a, off - 1);
                ed_snippet_shift(e, off - 1, -1);
                e->caret_line = pl;
                e->caret_col = pll;
            } else {
                ed_delete_byte(a, off - 1);
                ed_snippet_shift(e, off - 1, -1);
                e->caret_col--;
            }
            e->want_col = e->caret_col;
        }
        ed_ac_refresh(a);
        return 1;
    }
    case EDK_DELETE: {
        if (ed_delete_sel(a)) { e->want_col = e->caret_col; ed_ac_refresh(a); return 1; }
        int off = ed_caret_off(a);
        if (off < a->src_len) { ed_delete_byte(a, off); ed_snippet_shift(e, off, -1); }
        ed_ac_refresh(a);
        return 1;
    }
    case EDK_ENTER: {
        e->ac_active = 0;
        e->snippet_active = 0;   /* Enter ends snippet tab-stop navigation */
        ed_delete_sel(a);                 /* Enter over a selection replaces it */
        int off = ed_caret_off(a);
        if (!ed_insert_byte(a, off, '\n')) return 1;

        e->caret_line++;
        e->caret_col = 0;
        e->want_col = 0;

        /* Auto-indent: preserve previous line's leading whitespace (Settings knob) */
        if (g_auto_indent && e->caret_line > 0) {
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
        /* Snippet tab-stop navigation takes priority over indentation: jump to
         * the next ${N}/$0 stop, or end snippet mode after the last one. */
        if (e->snippet_active && e->ts_count > 0) {
            if (e->ts_current + 1 < e->ts_count) {
                e->ts_current++;
                ed_caret_from_off(a, e->ts_off[e->ts_current]);
                e->want_col = e->caret_col;
            } else {
                e->snippet_active = 0;
            }
            return 1;
        }
        /* insert ED_TABW spaces (soft tabs) */
        ed_delete_sel(a);                 /* Tab over a selection replaces it */
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
        ed_delete_sel(a);                 /* typing over a selection replaces it */
        int off = ed_caret_off(a);
        if (ed_insert_byte(a, off, kch)) {
            ed_snippet_shift(e, off, +1); /* keep snippet tab-stops aligned       */
            e->caret_col++;
            e->want_col = e->caret_col;
        }
        /* Re-scan the word at the caret and update autocomplete matches. */
        ed_ac_refresh(a);
        return 1;
    }
    return 0;
}

/* ---- click: place caret ---- */
int ide_editor_click(struct Ide* a, Rect r, int mx, int my, int shift) {
    Editor* e = &a->editor;
    e->focused = 1;
    a->codeview_focus = 0;   /* clicking the editor releases LEGO code-view focus */
    e->ac_active = 0;   /* dismiss autocomplete on click */

    Rect body = ed_body(r);
    if (body.h <= 0) return 1;
    if (my < body.y) return 1;

    /* Shift+click EXTENDS the selection: anchor at the current caret (once),
     * then let the caret move below while the anchor stays put. */
    if (shift) { if (e->sel_anchor_off < 0) e->sel_anchor_off = ed_caret_off(a); }
    else         e->sel_anchor_off = -1;   /* a plain click drops any selection */

    int line_h = GFX_FH;
    int nlines = ed_line_count(a->src, a->src_len);
    int gutter_w = g_line_numbers ? ed_gutter_w_n(nlines) : 0;   /* mirror render */
    int code_x = body.x + gutter_w + PAD;

    int row = (my - body.y) / line_h;
    int ln = e->top_line + row;
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

/* class value -> text colour. Distinct hues for every syntactic class:
 *   keywords (if/while/return)     -> purple
 *   types (int/char/struct)        -> teal/cyan
 *   string/char literals           -> orange/brown
 *   comments                       -> gray/dim
 *   numbers                        -> green
 *   preprocessor (#include)        -> magenta
 *   function calls (id before '(') -> yellow
 *   operators / identifiers        -> white/default */
static uint32_t ed_class_color(unsigned char c) {
    switch (c) {
        case LEXCLS_KEYWORD: return TH_PURPLE;
        case LEXCLS_TYPE:    return TH_CYAN;
        case LEXCLS_STRING:  return TH_ORANGE;
        case LEXCLS_COMMENT: return TH_TEXT_FAINT;
        case LEXCLS_NUMBER:  return TH_GREEN;
        case LEXCLS_PREPROC: return TH_MAGENTA;
        case LEXCLS_CALL:    return TH_YELLOW;
        default:             return TH_TEXT;
    }
}

/* ===========================================================================
 * Bracket matching.
 *
 * Given an offset in the source where the character is one of { } ( ) [ ],
 * find the matching bracket by scanning forward (for openers) or backward
 * (for closers), counting nesting depth. Returns the byte offset of the
 * match, or -1 if not found within a sane limit.
 * ==========================================================================*/
static int ed_bracket_match(const char* src, int len, int off) {
    if (off < 0 || off >= len) return -1;
    char ch = src[off];
    char open, close;
    int dir;
    switch (ch) {
    case '(': open = '('; close = ')'; dir =  1; break;
    case ')': open = ')'; close = '('; dir = -1; break;
    case '{': open = '{'; close = '}'; dir =  1; break;
    case '}': open = '}'; close = '{'; dir = -1; break;
    case '[': open = '['; close = ']'; dir =  1; break;
    case ']': open = ']'; close = '['; dir = -1; break;
    default: return -1;
    }
    int depth = 0;
    int limit = 50000; /* don't scan more than 50k chars */
    for (int i = off; i >= 0 && i < len && limit > 0; i += dir, limit--) {
        if (src[i] == open) depth++;
        else if (src[i] == close) depth--;
        if (depth == 0) return i;
    }
    return -1;
}

/* Is a character a bracket we highlight? */
static int ed_is_bracket(char c) {
    return c == '(' || c == ')' || c == '{' || c == '}' ||
           c == '[' || c == ']';
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

    /* header: filename + dirty marker + Ln/Col + wrap/minimap indicators */
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

        /* right-aligned Ln, Col + wrap indicator */
        char lc[64]; int p = 0;
        char nb[16]; int nn;
        if (e->word_wrap) {
            const char* wl = "WRAP ";
            for (int i = 0; wl[i]; i++) lc[p++] = wl[i];
        }
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

    /* ---- Minimap geometry: a narrow column on the right edge ---- */
    #define ED_MINIMAP_W   60    /* minimap width in pixels               */
    #define ED_MINIMAP_CH   1    /* pixels per char in minimap (1px dot)  */
    #define ED_MINIMAP_LH   2    /* pixels per line in minimap            */
    int minimap_w = (e->minimap) ? ED_MINIMAP_W : 0;

    int line_h = GFX_FH;
    int total_lines = ed_line_count(a->src, a->src_len);
    int gch = ed_gutter_ch(total_lines);   /* auto-sized gutter digit count */
    int gutter_w = g_line_numbers ? ed_gutter_w_n(total_lines) : 0;   /* Settings knob */
    int code_x = body.x + gutter_w + PAD;
    int code_clip_w = body.x + body.w - PAD - ED_SBAR_W - minimap_w - code_x;
    if (code_clip_w < 0) code_clip_w = 0;

    /* keep caret visible before drawing */
    ed_clamp_caret(a);
    ed_scroll_to_caret(a, body);

    /* gutter background + divider (only when line numbers are shown) */
    if (g_line_numbers) {
        gfx_fill(cv, body.x, body.y, gutter_w, body.h, TH_PANEL);
        gfx_vline(cv, body.x + gutter_w, body.y, body.h, TH_BORDER);
    }

    int vis = (body.h + line_h - 1) / line_h;

    /* ---- Bracket matching: precompute the match offset for the caret ---- */
    int caret_off = ed_caret_off(a);
    int bracket_match_off = -1;     /* byte offset of matching bracket, or -1 */
    if (e->focused && caret_off < a->src_len && ed_is_bracket(a->src[caret_off])) {
        bracket_match_off = ed_bracket_match(a->src, a->src_len, caret_off);
    }
    /* Also try one char to the left (cursor between brackets). */
    int bracket_left_off = -1;
    if (e->focused && bracket_match_off < 0 && caret_off > 0 &&
        ed_is_bracket(a->src[caret_off - 1])) {
        bracket_left_off = caret_off - 1;
        bracket_match_off = ed_bracket_match(a->src, a->src_len, caret_off - 1);
    }
    /* We now have: bracket_left_off OR caret_off as the "source" bracket, and
     * bracket_match_off as the "target" bracket. Both need highlighting. */
    int bk_src_off = (bracket_left_off >= 0) ? bracket_left_off : caret_off;

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
            gfx_blend(cv, body.x, ry, body.w - ED_SBAR_W - minimap_w, line_h,
                      (0x18u << 24) | (TH_TEXT & 0x00FFFFFFu));

        /* Build-error/warning highlight: tint the background of lines that
         * have diagnostics so the user can see at a glance where the errors
         * are. Red for errors, yellow/orange for warnings. */
        {
            int sev = ide_build_line_severity(ln);
            if (sev == 1)        /* error: red tint */
                gfx_blend(cv, body.x + gutter_w, ry,
                          body.w - gutter_w - ED_SBAR_W - minimap_w, line_h,
                          0x28E2574Au);
            else if (sev == 2)   /* warning: orange tint */
                gfx_blend(cv, body.x + gutter_w, ry,
                          body.w - gutter_w - ED_SBAR_W - minimap_w, line_h,
                          0x20E6C24Au);
        }

        /* line number -- errors get a red number, warnings get orange */
        if (g_line_numbers) {
            int sev = ide_build_line_severity(ln);
            uint32_t lncol = is_caret_line ? TH_TEXT_DIM : TH_TEXT_FAINT;
            if (sev == 1)      lncol = TH_RED;
            else if (sev == 2) lncol = TH_ORANGE;
            ed_itoa_pad(ln + 1, gch, numbuf, (int)sizeof(numbuf));
            gfx_text_clip(cv, body.x + PAD, ry, numbuf,
                          lncol, body.x + PAD, gch * GFX_FW);
        }

        /* ---- Indent guides: draw subtle vertical lines at every ED_TABW columns
         * within the leading whitespace of the line. This helps visually track
         * indentation levels in nested code blocks. ---- */
        {
            /* Count leading whitespace chars. */
            int indent_cols = 0;
            for (int i = 0; i < llen; i++) {
                char c = src[lstart + i];
                if (c == ' ') indent_cols++;
                else if (c == '\t') indent_cols += ED_TABW;
                else break;
            }
            /* Draw a thin dotted vertical line at each tab-stop column within the
             * indent region. Skip col 0 (the gutter border is there). */
            int code_right = code_x + code_clip_w;
            for (int tc = ED_TABW; tc < indent_cols; tc += ED_TABW) {
                int vis_col = tc - e->left_col;
                if (vis_col < 0) continue;
                int gx = code_x + vis_col * GFX_FW;
                if (gx >= code_right) break;
                /* Subtle stippled line: draw every other pixel row. */
                for (int py = ry; py < ry + line_h; py += 2) {
                    if (py >= body.y && py < body.y + body.h)
                        gfx_fill(cv, gx, py, 1, 1, TH_BORDER);
                }
            }
        }

        /* selection highlight on this line (drawn under the code text). */
        {
            int slo, shi;
            if (e->focused && ed_sel_range(a, &slo, &shi)) {
                int line_end = lstart + llen;
                if (shi > lstart && slo <= line_end) {
                    int c0 = (slo > lstart ? slo : lstart) - lstart;
                    int c1 = (shi > line_end) ? (llen + 1) : (shi - lstart);
                    int v0 = c0 - e->left_col; if (v0 < 0) v0 = 0;
                    int v1 = c1 - e->left_col; if (v1 < 0) v1 = 0;
                    if (v1 > v0) {
                        int hx = code_x + v0 * GFX_FW;
                        int hw = (v1 - v0) * GFX_FW;
                        int code_right2 = code_x + code_clip_w;
                        if (hx < code_right2) {
                            if (hx + hw > code_right2) hw = code_right2 - hx;
                            gfx_blend(cv, hx, ry, hw, line_h,
                                      (0x66u << 24) | (TH_BLUE & 0x00FFFFFFu));
                        }
                    }
                }
            }
        }

        /* ---- Bracket highlight: if the matching bracket is on this line,
         * draw a highlight box behind it. Also highlight the source bracket. ---- */
        if (e->focused && bracket_match_off >= 0) {
            /* Check if source bracket is on this line. */
            if (bk_src_off >= lstart && bk_src_off < lstart + llen) {
                int bcol = bk_src_off - lstart;
                int bvis = bcol - e->left_col;
                if (bvis >= 0) {
                    int bpx = code_x + bvis * GFX_FW;
                    if (bpx < code_x + code_clip_w)
                        gfx_blend(cv, bpx, ry, GFX_FW, line_h,
                                  (0x50u << 24) | (TH_YELLOW & 0x00FFFFFFu));
                }
            }
            /* Check if matching bracket is on this line. */
            if (bracket_match_off >= lstart && bracket_match_off < lstart + llen) {
                int bcol = bracket_match_off - lstart;
                int bvis = bcol - e->left_col;
                if (bvis >= 0) {
                    int bpx = code_x + bvis * GFX_FW;
                    if (bpx < code_x + code_clip_w)
                        gfx_blend(cv, bpx, ry, GFX_FW, line_h,
                                  (0x50u << 24) | (TH_YELLOW & 0x00FFFFFFu));
                }
            }
        }

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
                if (cch == '\t') cch = ' ';
                if (cch >= 32 && cch < 127)
                    gfx_text_clip(cv, cx, ry, src + lstart + i,
                                  ed_class_color(cls[i]), code_x, code_clip_w);
            }
        }

        /* Primary caret on this line. */
        if (is_caret_line && e->focused) {
            int vis_col = e->caret_col - e->left_col;
            if (vis_col >= 0) {
                int cx = code_x + vis_col * GFX_FW;
                if (cx < code_x + code_clip_w)
                    gfx_fill(cv, cx, ry, 2, line_h, TH_BLUE);
            }
        }

        /* Multi-cursor extra carets on this line. */
        if (e->mc_count > 0 && e->focused) {
            for (int mc = 0; mc < e->mc_count; mc++) {
                if (e->mc_line[mc] == ln) {
                    int vis_col = e->mc_col[mc] - e->left_col;
                    if (vis_col >= 0) {
                        int cx = code_x + vis_col * GFX_FW;
                        if (cx < code_x + code_clip_w)
                            gfx_fill(cv, cx, ry, 2, line_h, TH_CYAN);
                    }
                }
            }
        }

        if (lend < slen) pos = lend + 1; else pos = slen;
    }

    /* ---- Minimap: tiny overview of the entire file on the right ---- */
    if (e->minimap && minimap_w > 0) {
        int mm_x = body.x + body.w - minimap_w - ED_SBAR_W;
        int mm_y = body.y;
        int mm_h = body.h;

        /* Background. */
        gfx_fill(cv, mm_x, mm_y, minimap_w, mm_h, TH_PANEL);
        gfx_vline(cv, mm_x, mm_y, mm_h, TH_BORDER);

        /* Draw lines as tiny coloured dots. Each source line occupies
         * ED_MINIMAP_LH vertical pixels. Horizontal: each char = 1px. */
        int mm_vis_lines = mm_h / ED_MINIMAP_LH;
        if (mm_vis_lines < 1) mm_vis_lines = 1;

        /* Compute the minimap scroll so the viewport region is centered. */
        int mm_top = top - mm_vis_lines / 4;
        if (mm_top < 0) mm_top = 0;
        if (mm_top + mm_vis_lines > total_lines)
            mm_top = total_lines - mm_vis_lines;
        if (mm_top < 0) mm_top = 0;

        /* Draw the viewport indicator (which lines are visible). */
        {
            int vp_y0 = mm_y + (top - mm_top) * ED_MINIMAP_LH;
            int vp_y1 = mm_y + (top - mm_top + vis) * ED_MINIMAP_LH;
            if (vp_y0 < mm_y) vp_y0 = mm_y;
            if (vp_y1 > mm_y + mm_h) vp_y1 = mm_y + mm_h;
            if (vp_y1 > vp_y0)
                gfx_blend(cv, mm_x + 1, vp_y0, minimap_w - 2, vp_y1 - vp_y0,
                          (0x30u << 24) | (TH_BLUE & 0x00FFFFFFu));
        }

        /* Draw each minimap line. */
        int mm_pos = ed_line_start(src, slen, mm_top);
        for (int mrow = 0; mrow < mm_vis_lines; mrow++) {
            int mln = mm_top + mrow;
            if (mln >= total_lines) break;
            int mry = mm_y + mrow * ED_MINIMAP_LH;
            if (mry >= mm_y + mm_h) break;

            int mls = mm_pos;
            int mle = mls;
            while (mle < slen && src[mle] != '\n') mle++;
            int mll = mle - mls;

            /* Draw chars as 1px-wide dots, max minimap_w-4 chars. */
            int max_chars = minimap_w - 4;
            if (max_chars < 0) max_chars = 0;
            int mc = mll; if (mc > max_chars) mc = max_chars;
            for (int ci = 0; ci < mc; ci++) {
                char c = src[mls + ci];
                if (c > ' ' && c < 127) {
                    /* Use a dim version of the text colour. */
                    uint32_t col = (c == '/' || c == '*') ? TH_TEXT_FAINT : TH_TEXT_DIM;
                    gfx_fill(cv, mm_x + 2 + ci, mry, 1, ED_MINIMAP_LH > 1 ? ED_MINIMAP_LH - 1 : 1, col);
                }
            }

            if (mle < slen) mm_pos = mle + 1; else mm_pos = slen;
        }
    }

    /* vertical scrollbar */
    {
        int sbar_w = ED_SBAR_W;
        if (total_lines > 0) {
            int track_x = body.x + body.w - sbar_w;
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
                gfx_blend(cv, track_x, body.y, sbar_w, track_h, 0x30000000u);
                gfx_fill(cv, track_x, thumb_y, sbar_w, thumb_h, TH_BORDER_LT);
            }
        }

        /* horizontal scroll indicator: a thin bar at the bottom when content
         * extends beyond the viewport horizontally. */
        if (e->left_col > 0 && !e->word_wrap) {
            int max_col = 0;
            {
                int lp2 = ed_line_start(src, slen, top);
                for (int vr = 0; vr < vis && top + vr < total_lines; vr++) {
                    int ll = ed_line_len(src, slen, top + vr);
                    if (ll > max_col) max_col = ll;
                    while (lp2 < slen && src[lp2] != '\n') lp2++;
                    if (lp2 < slen) lp2++;
                }
            }
            int vis_cols = code_clip_w / GFX_FW;
            if (vis_cols < 1) vis_cols = 1;
            if (max_col > vis_cols) {
                int hsbar_h = sbar_w < 3 ? 3 : sbar_w;
                int track_y = body.y + body.h - hsbar_h;
                int track_w = body.w - gutter_w - PAD - sbar_w - minimap_w;
                if (track_w < 10) track_w = 10;
                int thumb_w = (vis_cols * track_w) / max_col;
                if (thumb_w < 8) thumb_w = 8;
                if (thumb_w > track_w) thumb_w = track_w;
                int max_hoff = max_col - vis_cols;
                int lc2 = e->left_col; if (lc2 > max_hoff) lc2 = max_hoff;
                int thumb_x = body.x + gutter_w + PAD +
                    (max_hoff > 0 ? (lc2 * (track_w - thumb_w)) / max_hoff : 0);
                gfx_blend(cv, body.x + gutter_w + PAD, track_y, track_w, hsbar_h, 0x30000000u);
                gfx_fill(cv, thumb_x, track_y, thumb_w, hsbar_h, TH_BORDER_LT);
            }
        }
    }
    /* ---- Autocomplete popup (drawn over everything else in the editor) ---- */
    if (e->ac_active && e->ac_count > 0 && e->focused) {
        int ac_row_h = GFX_FH + 4;
        int ac_rows  = e->ac_count;

        /* Find the widest match entry (in pixels) for popup width. */
        int max_word_w = 0;
        for (int i = 0; i < ac_rows; i++) {
            int ww = gfx_textw(e->ac_matches[i]);
            if (ww > max_word_w) max_word_w = ww;
        }
        int chip_w = GFX_FW + 6;                       /* category chip column */
        int ac_w = max_word_w + chip_w + 2 * PAD + 4;
        if (ac_w < 14 * GFX_FW) ac_w = 14 * GFX_FW;    /* minimum width */
        int ac_h = ac_rows * ac_row_h + 4;             /* 2px top + 2px bottom pad */

        /* Preview pane (snippet body) for the selected candidate, if any. */
        const char* preview = (e->ac_sel >= 0 && e->ac_sel < ac_rows)
                              ? complete_preview(e->ac_sel) : 0;
        int pv_w = preview ? 30 * GFX_FW : 0;

        /* Position: below the caret, one row down. */
        int caret_vis_row = e->caret_line - e->top_line;
        int caret_vis_col = e->caret_col - e->left_col;
        int ac_x = code_x + caret_vis_col * GFX_FW;
        int ac_y = body.y + (caret_vis_row + 1) * line_h;

        int right_lim = body.x + body.w - ED_SBAR_W - minimap_w;
        /* Horizontal clamp (list + preview must fit). */
        if (ac_x + ac_w + pv_w > right_lim) ac_x = right_lim - ac_w - pv_w;
        if (ac_x < body.x) { ac_x = body.x; if (ac_w + pv_w > right_lim - body.x) pv_w = 0; }
        /* Vertical: prefer below; flip above only if there is more room there;
         * then clamp height to the chosen side so rows are never cut off-screen
         * (the old code could push a tall popup off the top). */
        int below_y = ac_y;
        int space_below = (body.y + body.h) - below_y;
        int above_y = body.y + caret_vis_row * line_h - ac_h;
        int space_above = (body.y + caret_vis_row * line_h) - body.y;
        if (ac_h <= space_below) { /* fits below */ }
        else if (space_above > space_below) {
            ac_y = above_y;
            if (ac_y < body.y) { ac_y = body.y; if (ac_h > space_above) ac_h = space_above; }
        } else {
            if (ac_h > space_below) ac_h = space_below;
        }

        /* Background + border. */
        gfx_fill  (cv, ac_x, ac_y, ac_w, ac_h, TH_PANEL);
        gfx_stroke(cv, ac_x, ac_y, ac_w, ac_h, TH_BORDER_LT);

        /* Rows: category chip + label; selected row highlighted. */
        for (int i = 0; i < ac_rows; i++) {
            int ry2 = ac_y + 2 + i * ac_row_h;
            if (ry2 + ac_row_h > ac_y + ac_h) break;   /* clip to popup height */
            if (i == e->ac_sel)
                gfx_fill(cv, ac_x + 1, ry2, ac_w - 2, ac_row_h, TH_SELECT);
            int ty = ry2 + (ac_row_h - GFX_FH) / 2;
            /* chip */
            gfx_round(cv, ac_x + PAD, ry2 + (ac_row_h - 8) / 2, 8, 8, 2,
                      ac_kind_color(complete_kind(i)));
            /* label */
            gfx_text_clip(cv, ac_x + PAD + chip_w, ty, e->ac_matches[i],
                          i == e->ac_sel ? TH_TEXT : TH_TEXT_DIM,
                          ac_x + 1, ac_w - 2 - chip_w);
        }

        /* Preview pane: the snippet body, line by line. */
        if (preview && pv_w > 0) {
            int px = ac_x + ac_w;
            gfx_fill  (cv, px, ac_y, pv_w, ac_h, TH_PANEL2);
            gfx_stroke(cv, px, ac_y, pv_w, ac_h, TH_BORDER_LT);
            int ly = ac_y + 3;
            int i = 0;
            while (preview[i] && ly + GFX_FH <= ac_y + ac_h) {
                char ln[64]; int n = 0;
                while (preview[i] && preview[i] != '\n' && n < 63) ln[n++] = preview[i++];
                ln[n] = 0;
                if (preview[i] == '\n') i++;
                gfx_text_clip(cv, px + 4, ly, ln, TH_TEXT_DIM, px + 4, pv_w - 8);
                ly += GFX_FH;
            }
        }
    }

    (void)ed_min; (void)ed_max;
}
