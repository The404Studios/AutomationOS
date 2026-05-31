/*
 * ide_explorer.c -- PROJECT EXPLORER panel (top-left) for the Semantic LEGO
 * Map IDE. Renders the flat tree in a->entries[0..nentries) with indent,
 * folder/file glyphs, selection highlight and vertical scrolling. Click maps
 * a panel-relative y back to a row index, selects it, and opens files.
 *
 * Freestanding: no libc, no malloc, no stdio. All helpers are file-static and
 * prefixed expl_. Drawing stays inside the supplied Rect r.
 */
#include "ide.h"
#include "ide_theme.h"

/* Header bar height: the title row + a hairline divider underneath it. */
#define EXPL_HEADER_H  (ROW_H + 2)

/* Pixels reserved before a row's text for the leading glyph (folder/file). */
#define EXPL_GLYPH_W   (GFX_FW + 4)

/* Pixels per tree depth level (one indent "stop"). */
#define EXPL_LEVEL_W   (GFX_FW + 4)

/* Indent in pixels for one tree depth level. */
static inline int expl_indent(int depth) {
    return depth * EXPL_LEVEL_W;
}

/* String length (freestanding: no libc). */
static int expl_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Last path component of `path` into `out` (cap chars incl. NUL). Strips a
 * single trailing slash. Falls back to the whole string if no separator. */
static void expl_basename(const char* path, char* out, int cap) {
    int len = expl_strlen(path);
    if (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) len--;
    int start = 0;
    for (int i = 0; i < len; i++)
        if (path[i] == '/' || path[i] == '\\') start = i + 1;
    int j = 0;
    for (int i = start; i < len && j < cap - 1; i++) out[j++] = path[i];
    out[j] = 0;
}

/* Pick a name's accent colour: .c cyan-ish, .h dim, everything else primary. */
static uint32_t expl_file_color(const char* name, int selected) {
    int len = expl_strlen(name);
    if (len >= 2 && name[len - 2] == '.') {
        char c = name[len - 1];
        if (c == 'c' || c == 'C') return TH_CYAN;
        if (c == 'h' || c == 'H') return selected ? TH_TEXT : TH_TEXT_DIM;
    }
    return selected ? TH_TEXT : TH_TEXT_DIM;
}

/* Draw a tiny folder glyph (8x10-ish) inside the GFX_FW cell at (gx,gy). */
static void expl_draw_folder(Canvas* cv, int gx, int gy, uint32_t col) {
    int top = gy + 4;          /* leave a little headroom        */
    /* little tab */
    gfx_fill(cv, gx, top, 3, 2, col);
    /* body */
    gfx_fill(cv, gx, top + 2, GFX_FW, 7, col);
}

/* Draw a tiny file glyph (page with a dog-eared corner) at (gx,gy). */
static void expl_draw_file(Canvas* cv, int gx, int gy, uint32_t col) {
    int top = gy + 3;
    int w = GFX_FW - 2;
    if (w < 4) w = 4;
    gfx_stroke(cv, gx, top, w, 11, col);
    /* dog-ear: a notch at the top-right corner */
    gfx_fill(cv, gx + w - 3, top, 3, 3, TH_PANEL);
    gfx_line(cv, gx + w - 3, top, gx + w - 1, top + 2, col);
}

/* Body rect (panel minus header). Rows are laid out and clipped here. */
static inline Rect expl_body(Rect r) {
    Rect b;
    b.x = r.x;
    b.y = r.y + EXPL_HEADER_H;
    b.w = r.w;
    b.h = r.h - EXPL_HEADER_H;
    if (b.h < 0) b.h = 0;
    return b;
}

/* How many rows fit (fully or partially) in the body. */
static inline int expl_visible_rows(Rect body) {
    return (body.h + ROW_H - 1) / ROW_H;
}

void panel_explorer(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    /* Panel background. */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);

    /* Focus indicator: draw a thin cyan border when explorer is focused */
    if (a->explorer_focused && a->ws == WS_EDITOR) {
        gfx_stroke(cv, r.x, r.y, r.w, r.h, TH_CYAN);
        gfx_stroke(cv, r.x + 1, r.y + 1, r.w - 2, r.h - 2, TH_CYAN);
    }

    /* Header bar + title, with the project root basename appended. */
    gfx_fill(cv, r.x, r.y, r.w, EXPL_HEADER_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + EXPL_HEADER_H - 1, r.w, TH_BORDER);

    int hx = r.x + PAD;
    int hy = r.y + (EXPL_HEADER_H - GFX_FH) / 2;
    int hclip_w = r.w - 2 * PAD;
    if (hclip_w < 0) hclip_w = 0;
    gfx_text_clip(cv, hx, hy, "EXPLORER", TH_TEXT_DIM, hx, hclip_w);

    char base[64];
    expl_basename(a->root, base, (int)sizeof base);
    if (base[0]) {
        int bx = hx + (int)(sizeof "EXPLORER" - 1) * GFX_FW + GFX_FW;
        int brem = (r.x + r.w - PAD) - bx;
        if (brem > 0)
            gfx_text_clip(cv, bx, hy, base, TH_TEXT, bx, brem);
    }

    Rect body = expl_body(r);
    if (body.h <= 0) return;

    int scroll = a->explorer_scroll;
    if (scroll < 0) scroll = 0;
    int max_scroll = a->nentries - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;

    /* Reserve a sliver on the right for the scrollbar when content overflows. */
    int vis = expl_visible_rows(body);
    int overflow = (a->nentries > vis);
    int sb_w = overflow ? 3 : 0;

    int clip_x = body.x + PAD;
    int clip_w = body.w - 2 * PAD - sb_w;
    if (clip_w < 0) clip_w = 0;

    /* Is the cursor inside the body? Compute the hovered row (if any). */
    int hover_row = -1;
    if (rect_hit(body, a->mouse_x, a->mouse_y))
        hover_row = (a->mouse_y - body.y) / ROW_H;

    for (int row = 0; row < vis; row++) {
        int idx = scroll + row;
        if (idx >= a->nentries) break;

        EntryRow* e = &a->entries[idx];
        int ry = body.y + row * ROW_H;

        /* Clip the final (partial) row to the body bottom. */
        int row_h = ROW_H;
        if (ry + row_h > body.y + body.h) row_h = body.y + body.h - ry;
        if (row_h <= 0) break;

        int selected = (idx == a->sel_entry);
        if (selected)
            gfx_fill(cv, body.x, ry, body.w - sb_w, row_h, TH_SELECT);
        else if (row == hover_row)
            gfx_fill(cv, body.x, ry, body.w - sb_w, row_h, TH_HOVER);

        /* Faint vertical guide line at each ancestor indent stop, for a tree
         * feel. Skip depth 0 (no guide needed at the root column). */
        for (int d = 1; d <= e->depth; d++) {
            int gx = body.x + PAD + (d - 1) * EXPL_LEVEL_W + EXPL_LEVEL_W / 2;
            if (gx < clip_x + clip_w)
                gfx_vline(cv, gx, ry, row_h, TH_BORDER);
        }

        int tx = body.x + PAD + expl_indent(e->depth);
        int ty = ry + (ROW_H - GFX_FH) / 2;
        int nx = tx + EXPL_GLYPH_W;             /* text starts after glyph  */

        if (e->is_dir) {
            /* Folder glyph: yellow when expanded, dim when collapsed. */
            uint32_t fcol = e->collapsed ? TH_TEXT_DIM : TH_YELLOW;
            if (tx + GFX_FW <= clip_x + clip_w)
                expl_draw_folder(cv, tx, ry + (ROW_H - GFX_FH) / 2, fcol);
            gfx_text_clip(cv, nx, ty, e->name, TH_TEXT, clip_x, clip_w);
        } else {
            /* File glyph + suffix-aware name colour. */
            uint32_t col = expl_file_color(e->name, selected);
            if (tx + GFX_FW <= clip_x + clip_w)
                expl_draw_file(cv, tx, ry + (ROW_H - GFX_FH) / 2, TH_TEXT_FAINT);

            /* Check if this is the currently open file and it's dirty */
            int is_open = ide_streq(e->path, a->cur_file);
            int is_dirty = is_open && ide_editor_dirty(a);

            /* Show asterisk for dirty files */
            if (is_dirty) {
                /* Draw orange asterisk before filename */
                gfx_text_clip(cv, nx, ty, "*", TH_ORANGE, clip_x, clip_w);
                gfx_text_clip(cv, nx + GFX_FW, ty, e->name, col, clip_x, clip_w);
            } else {
                gfx_text_clip(cv, nx, ty, e->name, col, clip_x, clip_w);
            }
        }
    }

    /* Scrollbar thumb on the right edge when content overflows the body. */
    if (overflow) {
        int track_x = body.x + body.w - sb_w;
        gfx_fill(cv, track_x, body.y, sb_w, body.h, TH_PANEL2);

        int thumb_h = body.h * vis / a->nentries;
        if (thumb_h < 12) thumb_h = 12;
        if (thumb_h > body.h) thumb_h = body.h;

        int range = body.h - thumb_h;
        int thumb_y = body.y;
        if (max_scroll > 0)
            thumb_y += range * scroll / max_scroll;

        gfx_fill(cv, track_x, thumb_y, sb_w, thumb_h, TH_BORDER_LT);
    }
}

int panel_explorer_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;

    Rect body = expl_body(r);
    if (body.h <= 0) return 1;            /* inside panel, just no row */

    /* A click on the header consumes the event but selects nothing. */
    if (my < body.y) return 1;

    int row = (my - body.y) / ROW_H;
    if (row < 0) return 1;

    int scroll = a->explorer_scroll;
    if (scroll < 0) scroll = 0;
    int max_scroll = a->nentries - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;

    int idx = scroll + row;
    if (idx < 0 || idx >= a->nentries) return 1;

    a->sel_entry = idx;
    if (a->entries[idx].is_dir) {
        /* Clicking a folder toggles collapse/expand. */
        ide_toggle_collapsed(a, a->entries[idx].path);
        rebuild_visible_entries(a);
    } else {
        ide_open_file(a, a->entries[idx].path);
    }

    return 1;
}
