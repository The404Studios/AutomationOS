/*
 * ui.h -- M4 retained-mode UI toolkit (freestanding, ring 3).
 * ===========================================================
 *
 * A tiny retained-mode widget toolkit layered on top of the M3
 * "Wayland-lite" windowing stack (userspace/lib/wl) and the 8x16
 * bitmap font (userspace/lib/font). GUI apps describe a tree of
 * widgets (panels, labels, buttons); the toolkit owns the event
 * loop, layout, rendering and click dispatch.
 *
 * Usage sketch:
 *   #include "userspace/lib/ui/ui.h"
 *
 *   static void on_quit(void* ud) { (void)ud; ... }
 *
 *   void _start(void) {
 *       ui_app_t*    app  = ui_app_create("Demo", 480, 320);
 *       ui_widget_t* root = ui_app_root(app);
 *       ui_widget_t* pnl  = ui_panel(root, 16, 16, 200, 120, 0xFF2C2C2E);
 *       ui_label(pnl, 12, 12, "Hello, world", 0xFFFFFFFF);
 *       ui_button(pnl, 12, 48, 120, 32, "Quit", on_quit, 0);
 *       ui_app_run(app);   // never returns
 *   }
 *
 * No libc: pure inline syscalls + tiny freestanding helpers. Build the
 * toolkit alongside the app TU and link with wl_client.o + bitfont.o:
 *   gcc ... -c userspace/lib/ui/ui.c -o ui.o
 *   ld  ... app.o ui.o wl_client.o bitfont.o -o app
 *
 * Coordinate model: all coordinates are window-local pixels. Widget x/y
 * are relative to the PARENT widget's top-left; absolute rectangles are
 * computed at construction time (the tree is laid out as it is built).
 */

#ifndef UI_H
#define UI_H

/* Opaque handles -- callers never touch the internals directly. */
typedef struct ui_app    ui_app_t;
typedef struct ui_widget ui_widget_t;

/*
 * Create the application: connects to the compositor (wl_connect),
 * creates a w x h window titled `title`, and allocates a root panel that
 * covers the whole window (bg = 0xFF1C1C1E, the Aether Dark window bg).
 * Returns NULL on failure (connect/create error, or out of static slots).
 */
ui_app_t* ui_app_create(const char* title, int w, int h);

/* The root panel widget covering the whole window. */
ui_widget_t* ui_app_root(ui_app_t* app);

/*
 * Add a filled panel (container) as a child of `parent`.
 *   x, y -- top-left relative to parent's top-left, in pixels.
 *   w, h -- size in pixels.
 *   bg   -- ARGB32 fill color.
 * Returns the new widget, or NULL if `parent` is full / invalid.
 */
ui_widget_t* ui_panel(ui_widget_t* parent, int x, int y, int w, int h,
                      unsigned int bg);

/*
 * Add a text label as a child of `parent`. The label has no fill; its
 * size is derived from the text. `text` is copied into a fixed buffer
 * (truncated to 63 chars).
 *   x, y  -- top-left relative to parent, in pixels.
 *   color -- ARGB32 text color.
 */
ui_widget_t* ui_label(ui_widget_t* parent, int x, int y, const char* text,
                      unsigned int color);

/*
 * Add a clickable button as a child of `parent`.
 *   x, y     -- top-left relative to parent, in pixels.
 *   w, h     -- size in pixels.
 *   text     -- centered caption (copied, truncated to 63 chars).
 *   on_click -- callback invoked on a left-button press inside the rect
 *               (may be NULL).
 *   ud       -- opaque user data passed to on_click.
 */
ui_widget_t* ui_button(ui_widget_t* parent, int x, int y, int w, int h,
                       const char* text, void (*on_click)(void* ud), void* ud);

/* Replace a widget's text (copied, truncated to 63 chars). */
void ui_label_set_text(ui_widget_t* w, const char* text);

/* Change a widget's background fill color (panels, buttons, etc.). */
void ui_widget_set_bg(ui_widget_t* w, unsigned int bg);

/* Change a widget's foreground (text) color. */
void ui_widget_set_fg(ui_widget_t* w, unsigned int fg);

/*
 * Register a per-frame tick callback.  Before each frame is rendered,
 * ui_app_run() will call tick(ud) so the application can update labels
 * (e.g. a live clock).  Pass tick=NULL to clear a previously set callback.
 * ud is an opaque pointer forwarded to the callback unchanged.
 * Must be called before ui_app_run(); safe to call multiple times to swap
 * the callback.
 */
void ui_app_set_tick(ui_app_t* app, void (*tick)(void* ud), void* ud);

/*
 * Run the event loop. Drains input, dispatches clicks, renders the whole
 * tree and commits a frame each iteration, paced lightly. Never returns.
 */
void ui_app_run(ui_app_t* app);

/*
 * Recursively free a widget and all its descendants, returning their pool
 * slots for reuse. The widget MUST already have been detached from its
 * parent's children[] array (or be a content panel you are about to replace).
 * Does NOT detach from any parent -- caller is responsible for that.
 */
void ui_widget_free_tree(ui_widget_t* w);

/*
 * Remove a direct child from a parent's children[] array (shifts later
 * children down). Does NOT free the child -- call ui_widget_free_tree()
 * after if desired. Returns 0 on success, -1 if not found.
 */
int ui_widget_detach(ui_widget_t* parent, ui_widget_t* child);

/* =========================================================================
 * NEW WIDGETS (additive, v2)
 * =========================================================================
 */

/*
 * Checkbox widget.
 *   x, y      -- top-left relative to parent, in pixels.
 *   label     -- text drawn to the right of the check box.
 *   initial   -- initial checked state: 0 = unchecked, 1 = checked.
 *   on_toggle -- called whenever the state changes; receives new state (0/1)
 *                and user data.  May be NULL.
 *   ud        -- opaque user data forwarded to on_toggle.
 * Width is fixed at 16 + 4 + strlen(label)*8; height is 16.
 */
ui_widget_t* ui_checkbox(ui_widget_t* parent, int x, int y,
                         const char* label, int initial,
                         void (*on_toggle)(int state, void* ud), void* ud);

/* Read the current checked state of a checkbox (0 or 1). */
int ui_checkbox_checked(ui_widget_t* w);

/* Set the checked state programmatically (does NOT fire on_toggle). */
void ui_checkbox_set(ui_widget_t* w, int state);

/*
 * Horizontal slider widget.
 *   x, y      -- top-left relative to parent, in pixels.
 *   w         -- total widget width in pixels (minimum ~20).
 *   min, max  -- inclusive integer value range.
 *   initial   -- starting value (clamped to [min,max]).
 *   on_change -- called when the value changes; receives new value and ud.
 *                May be NULL.
 *   ud        -- opaque user data forwarded to on_change.
 * Height is fixed at 20px.
 */
ui_widget_t* ui_slider(ui_widget_t* parent, int x, int y, int w,
                       int min, int max, int initial,
                       void (*on_change)(int value, void* ud), void* ud);

/* Read the current integer value of a slider. */
int ui_slider_value(ui_widget_t* w);

/* Set the value programmatically (clamped; does NOT fire on_change). */
void ui_slider_set(ui_widget_t* w, int value);

/*
 * Single-line text input box.
 *   x, y   -- top-left relative to parent, in pixels.
 *   w      -- widget width in pixels.
 *   maxlen -- maximum number of characters (buffer is maxlen+1 bytes).
 *             Clamped to UI_TEXTBOX_MAXBUF-1.
 * Height is fixed at 20px.
 * Click to focus. Focused box receives key events:
 *   printable keys   -> appended (up to maxlen)
 *   KEY_BACKSPACE    -> delete last char
 * Only one textbox is focused at a time (global focus managed by the app).
 */
ui_widget_t* ui_textbox(ui_widget_t* parent, int x, int y, int w, int maxlen);

/* Returns a pointer to the NUL-terminated text buffer (read-only). */
const char* ui_textbox_text(ui_widget_t* w);

/* Set textbox content programmatically (truncated to maxlen). */
void ui_textbox_set_text(ui_widget_t* w, const char* text);

/* Maximum text buffer for a textbox (hard limit). */
#define UI_TEXTBOX_MAXBUF  128

/*
 * Progress bar.
 *   x, y  -- top-left relative to parent, in pixels.
 *   w, h  -- size in pixels.
 * Initial percent is 0.
 */
ui_widget_t* ui_progress(ui_widget_t* parent, int x, int y, int w, int h);

/* Set the progress percentage (0-100, clamped). */
void ui_progress_set(ui_widget_t* w, int pct);

/*
 * Colored icon tile (image_rect): a solid-colored rectangle with an optional
 * 1-char glyph centred inside, useful as a simple icon placeholder.
 *   x, y   -- top-left relative to parent, in pixels.
 *   sz     -- tile size in pixels (both width and height).
 *   color  -- ARGB32 tile fill color.
 *   glyph  -- a single ASCII character drawn centered, or '\0' for none.
 *   fg     -- foreground color for the glyph.
 */
ui_widget_t* ui_image_rect(ui_widget_t* parent, int x, int y, int sz,
                           unsigned int color, char glyph, unsigned int fg);

/*
 * Scroll-view container.
 *   x, y, w, h -- position and size of the *visible* viewport, in pixels.
 *   bg         -- viewport background color.
 *   content_h  -- total height of the scrollable content area.
 * Children added to the returned widget are placed relative to the scroll
 * origin and clipped to the viewport.  Scroll offset can be adjusted with
 * ui_scroll_set_offset(); the scroll thumb is drawn automatically.
 */
ui_widget_t* ui_scroll(ui_widget_t* parent, int x, int y, int w, int h,
                       unsigned int bg, int content_h);

/* Read the current vertical scroll offset (pixels from content top). */
int ui_scroll_offset(ui_widget_t* w);

/* Set the vertical scroll offset (clamped to [0, content_h - h]). */
void ui_scroll_set_offset(ui_widget_t* w, int offset);

#endif /* UI_H */
