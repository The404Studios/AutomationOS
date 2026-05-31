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
} Editor;

/* The Ide pointer is opaque here; ide.h includes this header *after* Rect and
 * Ide are declared, so the .c file gets the real types. We declare the API in
 * terms of the full names which resolve once ide.h is included. */

/* Reset caret/scroll/dirty after a new file is loaded into a->src. */
void ide_editor_reset(struct Ide* a);

/* Render the editor panel (gutter + code + caret) into Rect r. */
void ide_editor_render(struct Ide* a, Canvas* cv, struct Rect r);

/* Handle a left-click inside the editor: place the caret at the clicked cell
 * and take focus. Returns 1 (consumed). r is the same rect render used. */
int  ide_editor_click(struct Ide* a, struct Rect r, int mx, int my);

/* Feed one key press to the editor. `ch` is the resolved ASCII (0 if none),
 * `keycode` is the raw evdev code (for arrows/enter/backspace/tab), `shift`
 * and `ctrl` are modifier states. Returns 1 if the key was consumed.
 * Ctrl+S triggers a save here and is reported consumed. */
int  ide_editor_key(struct Ide* a, int keycode, char ch, int shift, int ctrl);

/* Save a->src to a->cur_file; clears dirty on success. Returns 0 on success. */
int  ide_editor_save(struct Ide* a);

/* True if the open buffer has unsaved edits. */
int  ide_editor_dirty(struct Ide* a);

/* Advance the caret blink animation by dt_ms (call once per frame). */
void ide_editor_tick(struct Ide* a, int dt_ms);

/* Duplicate the current line (Ctrl+D). */
void ide_editor_duplicate_line(struct Ide* a);

/* Delete the current line (Ctrl+Shift+K). */
void ide_editor_delete_line(struct Ide* a);

#endif /* IDE_EDITOR_H */
