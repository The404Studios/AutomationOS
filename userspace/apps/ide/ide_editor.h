/*
 * ide_editor.h -- a real, editable text editor for the IDE (VS-Code-lite).
 *
 * The editor operates directly on the app's open buffer (a->src / a->src_len):
 * it owns a text cursor (line/col), edit operations (insert char, newline,
 * backspace, delete), caret navigation, vertical + horizontal scrolling, a
 * dirty flag, and Ctrl+S save (via ide_write_file). Rendering draws a
 * line-number gutter, per-character syntax highlighting (reusing the project
 * lexer's lex_classify_line) and a blinking block caret, all clipped to a Rect.
 *
 * This is the editable counterpart to ide_codeview.c (which is a read-only,
 * semantics-annotated view used by the LEGO map workspace). The editor never
 * allocates: all scratch is static or on the (bounded) caller stack.
 *
 * Freestanding: no libc / malloc / stdio.
 */
#ifndef IDE_EDITOR_H
#define IDE_EDITOR_H

#include "ide_gfx.h"

struct Ide;            /* forward decl: full definition in ide.h */
struct Rect;           /* forward decl: full definition in ide.h */

/* Editor caret + view state. One instance lives inside the Ide struct so it
 * survives file switches being reset explicitly by ide_editor_reset(). */
/* Maximum autocomplete matches shown in the popup. */
#define AC_MAX_MATCHES  8
#define AC_PREFIX_CAP  64
#define AC_WORD_CAP    48

/* Maximum extra cursors for multi-cursor editing (Ctrl+D). */
#define ED_MULTI_CURSOR_MAX  8

/* Maximum tab-stops in an inserted snippet/complex (${1:..}/$N/$0). */
#define ED_TS_MAX  16

typedef struct {
    int  caret_line;       /* 0-based line of the caret                  */
    int  caret_col;        /* 0-based column (in characters, tabs = 1)   */
    int  want_col;         /* preferred column across vertical moves     */
    int  top_line;         /* first visible line (vertical scroll)       */
    int  left_col;         /* first visible column (horizontal scroll)   */
    int  dirty;            /* 1 if buffer changed since last save        */
    int  focused;          /* 1 if the editor owns keyboard input        */
    int  blink_ms;         /* caret blink phase accumulator              */
    int  sel_anchor_off;   /* reserved for future selection (unused=−1)  */

    /* Word-wrap toggle (Ctrl+W). When enabled, long lines soft-wrap at the
     * window edge instead of scrolling horizontally. */
    int  word_wrap;        /* 1 = soft word wrap enabled                 */

    /* Minimap toggle (Ctrl+M). Draws a tiny overview of the file at the
     * right edge of the editor. */
    int  minimap;          /* 1 = minimap sidebar visible                */

    /* Multi-cursor state (Ctrl+D to add cursors at next occurrence). The
     * primary cursor is always editor.caret_line/col; extra cursors live here. */
    int  mc_count;         /* number of extra cursors (0 = single cursor)  */
    int  mc_line[ED_MULTI_CURSOR_MAX]; /* extra cursor lines               */
    int  mc_col[ED_MULTI_CURSOR_MAX];  /* extra cursor columns             */

    /* Autocomplete popup state. */
    int  ac_active;                          /* 1 if the popup is visible        */
    int  ac_sel;                             /* selected index in matches[]      */
    int  ac_navigated;                       /* 1 once the user pressed Up/Down in
                                              * the popup -- gates Enter=accept so
                                              * a bare Enter still makes a newline */
    char ac_prefix[AC_PREFIX_CAP];           /* current word prefix being typed  */
    int  ac_prefix_len;                      /* length of ac_prefix              */
    int  ac_count;                           /* number of current matches        */
    char ac_matches[AC_MAX_MATCHES][AC_WORD_CAP]; /* matched keyword strings    */

    /* Snippet/complex tab-stop state (active after inserting a library body). */
    int  snippet_active;                     /* 1 while Tab cycles tab-stops     */
    int  ts_off[ED_TS_MAX];                  /* byte offsets of each tab-stop    */
    int  ts_count;                           /* number of tab-stops              */
    int  ts_current;                         /* index of the current tab-stop    */
} Editor;

/* The Ide pointer is opaque here; ide.h includes this header *after* Rect and
 * Ide are declared, so the .c file gets the real types. We declare the API in
 * terms of the full names which resolve once ide.h is included. */

/* Reset caret/scroll/dirty after a new file is loaded into a->src. */
void ide_editor_reset(struct Ide* a);

/* Render the editor panel (gutter + code + caret) into Rect r. */
void ide_editor_render(struct Ide* a, Canvas* cv, struct Rect r);

/* Handle a left-click inside the editor: place the caret at the clicked cell
 * and take focus. With `shift`, EXTENDS the selection from the existing caret
 * instead of dropping it. Returns 1 (consumed). r is the same rect render used. */
int  ide_editor_click(struct Ide* a, struct Rect r, int mx, int my, int shift);

/* Feed one key press to the editor. `ch` is the resolved ASCII (0 if none),
 * `keycode` is the raw evdev code (for arrows/enter/backspace/tab), `shift`
 * and `ctrl` are modifier states. Returns 1 if the key was consumed.
 * Ctrl+S triggers a save here and is reported consumed. */
int  ide_editor_key(struct Ide* a, int keycode, char ch, int shift, int ctrl);

/* Save a->src to a->cur_file; clears dirty on success. Returns 0 on success. */
int  ide_editor_save(struct Ide* a);

/* Undo / redo the most recent edit step (Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z). */
void ide_editor_undo(struct Ide* a);
void ide_editor_redo(struct Ide* a);

/* Public accessors over the editor's caret/line model, so the LEGO code view
 * (ide_codeview.c) can share the single a->editor caret. All offsets are byte
 * offsets into a->src; line indices are 0-based. */
int  ide_editor_caret_off(struct Ide* a);            /* caret as a byte offset */
int  ide_editor_sel_range(struct Ide* a, int* lo, int* hi); /* 1 if non-empty  */
int  ide_editor_line_start(struct Ide* a, int ln);   /* byte offset of line ln */
int  ide_editor_line_len(struct Ide* a, int ln);     /* length of line ln      */

/* Apply a completion at the caret (called by the completion engine):
 *  - is_snippet=0: insert text[prefix_len..] (the un-typed suffix of a symbol).
 *  - is_snippet=1: delete the typed prefix, then expand `text` as a snippet
 *    body with ${N:..}/$N/$0 tab-stops and enter tab-stop navigation. */
void ide_editor_apply_completion(struct Ide* a, const char* text,
                                 int is_snippet, int prefix_len);

/* Insert a snippet body (with tab-stops) at the caret + start tab-stop nav. */
void ide_editor_insert_snippet(struct Ide* a, const char* body);

/* True if the open buffer has unsaved edits. */
int  ide_editor_dirty(struct Ide* a);

/* Advance the caret blink animation by dt_ms (call once per frame). */
void ide_editor_tick(struct Ide* a, int dt_ms);

/* Duplicate the current line (Ctrl+D with no selection / single cursor). */
void ide_editor_duplicate_line(struct Ide* a);

/* Delete the current line (Ctrl+Shift+K). */
void ide_editor_delete_line(struct Ide* a);

/* Toggle soft word wrap (Ctrl+W). */
void ide_editor_toggle_wrap(struct Ide* a);

/* Toggle the minimap sidebar (Ctrl+M). */
void ide_editor_toggle_minimap(struct Ide* a);

/* Ctrl+D with an active selection: select the next occurrence and place an
 * additional cursor there (multi-cursor). With no selection, falls back to
 * duplicate-line. */
void ide_editor_multi_cursor_add(struct Ide* a);

/* Clipboard / selection ops. These are driven by Ctrl+A/C/X/V routed through the
 * IDE's central chord handler (handle_ctrl_chord in ide.c), since the editor's
 * own key entry point is invoked with ctrl already stripped. Copy/cut with no
 * active selection act on the whole current line (VS Code style). */
void ide_editor_select_all(struct Ide* a);
void ide_editor_copy(struct Ide* a);
void ide_editor_cut(struct Ide* a);
void ide_editor_paste(struct Ide* a);

/* Find `needle` after the caret (wraps); selects+scrolls to the match. Driven by
 * Ctrl+F's prompt in ide.c. Returns 1 if found. */
int ide_editor_find(struct Ide* a, const char* needle, int dir);

/* Replace ALL occurrences of `needle` with `repl`; returns the count replaced.
 * Driven by Ctrl+H's replace prompt in ide.c. */
int ide_editor_replace_all(struct Ide* a, const char* needle, const char* repl);

#endif /* IDE_EDITOR_H */
