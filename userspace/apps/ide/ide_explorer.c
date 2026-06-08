/*
 * ide_explorer.c -- PROJECT EXPLORER panel (top-left) for the Semantic LEGO
 * Map IDE. Renders the flat tree in a->entries[0..nentries) with indent,
 * folder/file glyphs, selection highlight and vertical scrolling. Click maps
 * a panel-relative y back to a row index, selects it, and opens files.
 *
 * VS-Code-style file tree: expand/collapse arrows, file-type prefixes,
 * current-file highlight, 2-char-per-level indentation, mouse-wheel scroll,
 * and auto-expand-to-current-file on open.
 *
 * Freestanding: no libc, no malloc, no stdio. All helpers are file-static and
 * prefixed expl_. Drawing stays inside the supplied Rect r.
 */
#include "ide.h"
#include "ide_theme.h"

/* Header bar height: the title row + a hairline divider underneath it. */
#define EXPL_HEADER_H  (ROW_H + 2)

/* Pixels reserved for the expand/collapse arrow on directory rows. */
#define EXPL_ARROW_W   (GFX_FW)

/* Pixels reserved for the file-type tag (e.g. "[C]"). */
#define EXPL_TAG_W     (3 * GFX_FW + 2)

/* Pixels per tree depth level: 2 characters of indent per nesting level,
 * matching the VS-Code convention of compact tree indentation. */
#define EXPL_LEVEL_W   (2 * GFX_FW)

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

/* Case-insensitive character compare (ASCII only). */
static inline int expl_tolower(char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
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

/* ---------------------------------------------------------------------------
 * File-type classification: returns a short tag string and accent colour.
 * Tags are terse ASCII that render in the bitmap font (no Unicode needed).
 * -------------------------------------------------------------------------*/
typedef enum {
    FTYPE_DIR,       /* directory            */
    FTYPE_C,         /* .c source            */
    FTYPE_H,         /* .h header            */
    FTYPE_MAKE,      /* Makefile / makefile   */
    FTYPE_TXT,       /* .txt text            */
    FTYPE_ELF,       /* .elf binary          */
    FTYPE_ASM,       /* .asm / .s assembly   */
    FTYPE_UNKNOWN    /* anything else        */
} FileType;

static FileType expl_classify(const char* name, int is_dir) {
    if (is_dir) return FTYPE_DIR;
    int len = expl_strlen(name);

    /* Check for Makefile (case insensitive first char) */
    if (len >= 8) {
        int is_makefile = 1;
        const char* mf = "makefile";
        for (int i = 0; i < 8; i++) {
            if (expl_tolower(name[i]) != mf[i]) { is_makefile = 0; break; }
        }
        if (is_makefile && (len == 8 || name[8] == '.')) return FTYPE_MAKE;
    }
    /* Extension-based classification */
    if (len >= 2 && name[len - 2] == '.') {
        char c = name[len - 1];
        if (c == 'c' || c == 'C') return FTYPE_C;
        if (c == 'h' || c == 'H') return FTYPE_H;
        if (c == 's' || c == 'S') return FTYPE_ASM;
        if (c == 'o')             return FTYPE_ELF;
    }
    if (len >= 4 && name[len - 4] == '.') {
        char e1 = expl_tolower(name[len - 3]);
        char e2 = expl_tolower(name[len - 2]);
        char e3 = expl_tolower(name[len - 1]);
        if (e1 == 'e' && e2 == 'l' && e3 == 'f') return FTYPE_ELF;
        if (e1 == 't' && e2 == 'x' && e3 == 't') return FTYPE_TXT;
        if (e1 == 'a' && e2 == 's' && e3 == 'm') return FTYPE_ASM;
    }
    return FTYPE_UNKNOWN;
}

/* Tag string for a file type. */
static const char* expl_tag(FileType ft) {
    switch (ft) {
        case FTYPE_C:       return "[C]";
        case FTYPE_H:       return "[H]";
        case FTYPE_MAKE:    return "[M]";
        case FTYPE_TXT:     return "[T]";
        case FTYPE_ELF:     return "[E]";
        case FTYPE_ASM:     return "[S]";
        case FTYPE_UNKNOWN: return "[?]";
        default:            return "";
    }
}

/* Accent colour for a file type (name text). */
static uint32_t expl_file_color(FileType ft, int selected, int is_open) {
    if (is_open) return TH_TEXT;      /* always bright for the active file */
    switch (ft) {
        case FTYPE_C:    return TH_CYAN;
        case FTYPE_H:    return selected ? TH_TEXT : TH_TEXT_DIM;
        case FTYPE_MAKE: return TH_YELLOW;
        case FTYPE_ASM:  return TH_PURPLE;
        case FTYPE_ELF:  return TH_GREEN;
        case FTYPE_TXT:  return selected ? TH_TEXT : TH_TEXT_DIM;
        default:         return selected ? TH_TEXT : TH_TEXT_DIM;
    }
}

/* Tag colour (the "[C]" prefix). Slightly dimmer than the name colour. */
static uint32_t expl_tag_color(FileType ft) {
    switch (ft) {
        case FTYPE_C:    return TH_CYAN;
        case FTYPE_H:    return TH_BLUE;
        case FTYPE_MAKE: return TH_YELLOW;
        case FTYPE_ASM:  return TH_PURPLE;
        case FTYPE_ELF:  return TH_GREEN;
        case FTYPE_TXT:  return TH_TEXT_DIM;
        default:         return TH_TEXT_FAINT;
    }
}

/* Current-file highlight: a subtle accent background, distinct from the
 * selection highlight (TH_SELECT) and hover (TH_HOVER). This uses a dark
 * tinted blue so the open file is always visually identifiable even when
 * the selection cursor is elsewhere. */
#define TH_OPEN_FILE   0xFF162440u

/* Draw a tiny expand/collapse arrow (triangle) for directory rows.
 * Collapsed = right-pointing triangle (>); expanded = downward (v). */
static void expl_draw_arrow(Canvas* cv, int ax, int ay, int collapsed,
                            uint32_t col) {
    int cx = ax + GFX_FW / 2;
    int cy = ay + GFX_FH / 2;
    int sz = GFX_FW / 3;
    if (sz < 2) sz = 2;

    if (collapsed) {
        /* Right-pointing triangle: three horizontal lines narrowing */
        for (int dy = -sz; dy <= sz; dy++) {
            int w = sz - (dy < 0 ? -dy : dy);
            if (w < 0) w = 0;
            gfx_hline(cv, cx - 1, cy + dy, w + 1, col);
        }
    } else {
        /* Down-pointing triangle: three vertical lines narrowing */
        for (int dx = -sz; dx <= sz; dx++) {
            int h = sz - (dx < 0 ? -dx : dx);
            if (h < 0) h = 0;
            gfx_vline(cv, cx + dx, cy - 1, h + 1, col);
        }
    }
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
    /* Better scroll clamping: ensure at least one row shows when possible. */
    int vis = expl_visible_rows(body);
    int max_scroll = a->nentries - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
    a->explorer_scroll = scroll;   /* write back clamped value */

    /* Reserve a sliver on the right for the scrollbar when content overflows. */
    int overflow = (a->nentries > vis);
    int sb_w = overflow ? 4 : 0;   /* slightly wider for easier grab */

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
        int is_open  = !e->is_dir && ide_streq(e->path, a->cur_file);
        int is_dirty = is_open && ide_editor_dirty(a);
        FileType ft  = expl_classify(e->name, e->is_dir);

        /* Row background: layered priority:
         *   1. selected row (keyboard/click selection)
         *   2. currently open file (persistent highlight)
         *   3. hovered row (mouse hover) */
        if (selected)
            gfx_fill(cv, body.x, ry, body.w - sb_w, row_h, TH_SELECT);
        else if (is_open)
            gfx_fill(cv, body.x, ry, body.w - sb_w, row_h, TH_OPEN_FILE);
        else if (row == hover_row)
            gfx_fill(cv, body.x, ry, body.w - sb_w, row_h, TH_HOVER);

        /* Faint vertical guide lines at each ancestor indent stop, giving the
         * tree structure a visual spine. Skip depth 0 (root needs no guide). */
        for (int d = 1; d <= e->depth; d++) {
            int gx = body.x + PAD + (d - 1) * EXPL_LEVEL_W + EXPL_LEVEL_W / 2;
            if (gx < clip_x + clip_w)
                gfx_vline(cv, gx, ry, row_h, TH_BORDER);
        }

        /* Layout:  [indent] [arrow(dir only)] [glyph] [tag] [name]
         *   - indent: depth * 2 chars
         *   - arrow:  1 char width (dirs only, expand/collapse indicator)
         *   - glyph:  1 char width (folder or file icon)
         *   - tag:    3 chars (e.g. "[C]") + 1 char gap  (files only)
         *   - name:   the entry basename
         */
        int tx = body.x + PAD + expl_indent(e->depth);
        int ty = ry + (ROW_H - GFX_FH) / 2;

        if (e->is_dir) {
            /* --- Directory row --- */
            /* Expand/collapse arrow */
            int arrow_x = tx;
            uint32_t arrow_col = e->collapsed ? TH_TEXT_FAINT : TH_TEXT_DIM;
            if (arrow_x + EXPL_ARROW_W <= clip_x + clip_w)
                expl_draw_arrow(cv, arrow_x, ty, e->collapsed, arrow_col);

            /* Folder glyph: yellow when expanded, dim when collapsed. */
            int glyph_x = arrow_x + EXPL_ARROW_W;
            uint32_t fcol = e->collapsed ? TH_TEXT_DIM : TH_YELLOW;
            if (glyph_x + GFX_FW <= clip_x + clip_w)
                expl_draw_folder(cv, glyph_x, ty, fcol);

            /* Directory name */
            int name_x = glyph_x + GFX_FW + 2;
            gfx_text_clip(cv, name_x, ty, e->name, TH_TEXT, clip_x, clip_w);
        } else {
            /* --- File row --- */
            /* File glyph */
            int glyph_x = tx;
            if (glyph_x + GFX_FW <= clip_x + clip_w)
                expl_draw_file(cv, glyph_x, ty, expl_tag_color(ft));

            /* File type tag */
            int tag_x = glyph_x + GFX_FW + 2;
            const char* tag = expl_tag(ft);
            if (tag[0])
                gfx_text_clip(cv, tag_x, ty, tag, expl_tag_color(ft), clip_x, clip_w);

            /* File name, offset past the tag */
            int name_x = tag_x + (tag[0] ? EXPL_TAG_W : 0);
            uint32_t ncol = expl_file_color(ft, selected, is_open);

            /* Show asterisk for dirty files */
            if (is_dirty) {
                gfx_text_clip(cv, name_x, ty, "*", TH_ORANGE, clip_x, clip_w);
                gfx_text_clip(cv, name_x + GFX_FW, ty, e->name, ncol, clip_x, clip_w);
            } else {
                gfx_text_clip(cv, name_x, ty, e->name, ncol, clip_x, clip_w);
            }

            /* Open-file indicator: a small bright bar on the left edge. */
            if (is_open) {
                gfx_fill(cv, body.x, ry, 2, row_h, TH_CYAN);
            }
        }
    }

    /* Scrollbar thumb on the right edge when content overflows the body. */
    if (overflow) {
        int track_x = body.x + body.w - sb_w;
        gfx_fill(cv, track_x, body.y, sb_w, body.h, TH_PANEL2);

        int thumb_h = body.h * vis / a->nentries;
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > body.h) thumb_h = body.h;

        int range = body.h - thumb_h;
        int thumb_y = body.y;
        if (max_scroll > 0)
            thumb_y += range * scroll / max_scroll;

        /* Rounded scrollbar thumb (VS Code style). */
        gfx_round(cv, track_x, thumb_y, sb_w, thumb_h, 2, TH_BORDER_LT);
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
